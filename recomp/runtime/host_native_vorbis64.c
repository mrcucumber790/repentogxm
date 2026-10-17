/* Native stb_vorbis mirrors for the KAGE OGG path on 64-bit hosts.
 * See host_native_vorbis64.h for the model, conventions and fallbacks. */
#include <pthread.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest.h"
#include "host_native_vorbis64.h"

void isaac_vita_log(const char *format, ...);
void *isaac_vita_guest_malloc(size_t size);
int isaac_vita_guest_free(void *pointer);

/* ------------------------------------------------------------------------
 * Frozen PE facts (8,650,240-byte Repentance PE). */
#define NVW_RVA_GET_SAMPLES        0x005bdf80U
#define NVW_RVA_SET_FILE_OFFSET    0x005b7740U
#define NVW_RVA_PUMP_FIRST_FRAME   0x005bb260U
#define NVW_RVA_OPEN_STREAM        0x005bd740U
#define NVW_RVA_OPEN_MEMORY        0x005bd8b0U
/* Return addresses of Decode's inlined seek_start (StreamSourceOgg). */
#define NVW_RET_DECODE_SEEK        0x005a2197U
#define NVW_RET_DECODE_PUMP        0x005a21b9U
/* Return markers pushed before the guest stream calls: the translated
 * decoder's own call sites (get8/getn read, set_file_offset seek/tell). */
#define NVW_SITE_READ              0x005b772cU
#define NVW_SITE_SEEK              0x005b77aeU
#define NVW_SITE_TELL              0x005b77b6U
/* KAGE stream vtable slots used by the game's stb_vorbis edit. */
#define NVW_VT_TELL                0x08U
#define NVW_VT_SEEK                0x0cU
#define NVW_VT_READ                0x14U
/* v1.04 32-bit state layout. */
#define NVW_ST_SAMPLE_RATE         0x00U
#define NVW_ST_CHANNELS            0x04U
#define NVW_ST_MARKER              0x10U   /* setup_temp_memory_required */
#define NVW_ST_STREAM              0x14U   /* FILE *f -> KAGE stream */
#define NVW_ST_F_START             0x18U
#define NVW_ST_STREAM_LEN          0x2cU
#define NVW_ST_PUSH_MODE           0x30U
#define NVW_ST_ALLOC_BUFFER        0x60U
#define NVW_ST_ALLOC_LEN           0x64U

#ifndef NVW_MIRRORS_MAX
#define NVW_MIRRORS_MAX            24U
#endif
#ifndef NVW_IO_BLOCK
#define NVW_IO_BLOCK               (16U * 1024U)
#endif
/* Native arena: the guest's own stb_vorbis_alloc size (0x4b000) does not
 * fit the 64-bit layout of the same setup, so take a multiple of it. */
#ifndef NVW_ARENA_MIN
#define NVW_ARENA_MIN              (640U * 1024U)
#endif
#ifndef NVW_ARENA_MAX
#define NVW_ARENA_MAX              (4U * 1024U * 1024U)
#endif

/* ------------------------------------------------------------------------
 * Decoder I/O seam: the vendored decoder's stdio calls go to the mirror's
 * block cache.  Memory-mode mirrors never reach them (USE_MEMORY). */
typedef struct nvw_io {
    uint32_t stream;       /* guest KAGE stream object */
    uint32_t pos;          /* logical position */
    uint32_t buf_pos;      /* stream position of buf[0] */
    uint32_t buf_len;
    int error;             /* a guest stream call failed */
    uint8_t buf[NVW_IO_BLOCK];
} nvw_io;

static int nvw_io_getc(FILE *token);
static size_t nvw_io_fread(void *buffer, size_t size, size_t count,
                           FILE *token);
static int nvw_io_fseek(FILE *token, long offset, int whence);
static long nvw_io_ftell(FILE *token);
static int nvw_io_fclose(FILE *token);
static FILE *nvw_io_fopen(const char *name, const char *mode);

#define fgetc(token) nvw_io_getc(token)
#define fread(buffer, size, count, token) nvw_io_fread(buffer, size, count, token)
#define fseek(token, offset, whence) nvw_io_fseek(token, offset, whence)
#define ftell(token) nvw_io_ftell(token)
#define fclose(token) nvw_io_fclose(token)
#define fopen(name, mode) nvw_io_fopen(name, mode)

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif
#define ISAAC_NV_ASSERT_HEADER "../../host_vita_native_vorbis_assert.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wempty-body"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#endif
#include "third_party/stb_vorbis/stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#undef fgetc
#undef fread
#undef fseek
#undef ftell
#undef fclose
#undef fopen
#undef C
#undef L
#undef R
#undef BUFFER_SIZE

/* ------------------------------------------------------------------------
 * State. */
enum { NVW_MODE_MEMORY = 1, NVW_MODE_STREAM = 2 };

typedef struct nvw_mirror {
    /* read by the free observer under s_spin */
    uint32_t active;
    uint32_t f;
    uint32_t alloc_lo;
    uint32_t alloc_len;
    uint32_t data_lo;
    uint32_t freed;
    /* decode lock */
    uint32_t marker;
    int mode;
    int verify;
    int broken;
    int skip_pump;
    stb_vorbis *v;
    void *arena;
    uint32_t arena_bytes;
    nvw_io *io;
    uint32_t post_open_pos;     /* guest stream position after the real open */
    uint64_t delivered;         /* frames since the last sync point */
} nvw_mirror;

typedef struct nvw_stats {
    uint32_t open_mem, open_stream;
    uint32_t mirrors_mem, mirrors_stream;
    uint32_t open_fail, layout_fail, pool_full, replaced;
    uint32_t calls_mem, calls_stream, calls_real;
    uint64_t frames_mem, frames_stream;
    uint64_t us_mem, us_stream;
    uint32_t us_max_mem, us_max_stream;
    uint32_t seek_start_native, seek_start_real, pump_skipped;
    uint32_t clean_fallback, broken_mid, broken_drop;
    uint32_t asserts, io_errors;
    uint32_t retire_deinit, retire_freed, retire_dead;
    uint32_t live, live_max;
    uint32_t arena_used_max, arena_retry;
    uint32_t threads, foreign_calls;
    uint32_t fallbacks;
    /* verify */
    uint32_t v_mirrors, v_calls, v_match, v_count_diff, v_sample_diff;
    uint64_t v_diff_samples;
    uint32_t v_maxabs;
    uint64_t v_us_native, v_us_real;
} nvw_stats;

static nvw_mirror s_mirrors[NVW_MIRRORS_MAX];
static nvw_stats s_st;
static uint32_t s_live;                 /* atomic: mirrors active */
static int s_spin;                      /* guards the observer fields */
static pthread_mutex_t s_lock;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static pthread_t s_first_thread;
static int s_first_thread_set;
static uint32_t s_marker_seq;
static uint32_t s_staging;              /* guest block for stream reads */
static CPU *s_io_cpu;                   /* set only around native calls */
static jmp_buf s_jmp;
static int s_jmp_armed;
static int s_verify_budget = -1;        /* -1: not read yet */
static int16_t *s_verify_buf;
static size_t s_verify_cap;

static void nvw_init_once(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    /* Only the debug self-test re-enters the lock (guest_call into the
     * wrapped entries); production paths never nest. */
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0)
        isaac_vita_log("KAGE NATIVE VORBIS: recursive mutex unavailable");
    pthread_mutex_init(&s_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void nvw_lock(void)
{
    pthread_once(&s_once, nvw_init_once);
    pthread_mutex_lock(&s_lock);
    if (!s_first_thread_set) {
        s_first_thread = pthread_self();
        s_first_thread_set = 1;
        s_st.threads = 1U;
    } else if (!pthread_equal(s_first_thread, pthread_self())) {
        ++s_st.foreign_calls;
    }
}

static void nvw_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

static void nvw_spin_lock(void)
{
    while (__atomic_exchange_n(&s_spin, 1, __ATOMIC_ACQUIRE))
        ;
}

static void nvw_spin_unlock(void)
{
    __atomic_store_n(&s_spin, 0, __ATOMIC_RELEASE);
}

static uint64_t nvw_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U;
}

static int nvw_pow2(uint32_t n)
{
    return n && (n & (n - 1U)) == 0U;
}

static void nvw_fallback(uint32_t f, const char *reason)
{
    uint32_t n = ++s_st.fallbacks;

    if (nvw_pow2(n))
        isaac_vita_log("KAGE NATIVE VORBIS FALLBACK: f=%08x reason=%s n=%u",
                       (unsigned)f, reason, (unsigned)n);
}

/* ISAAC_NATIVE_VORBIS=0 in the environment disables mirroring (A/B runs). */
static int nvw_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("ISAAC_NATIVE_VORBIS");
        enabled = !(e && e[0] == '0');
    }
    return enabled;
}

/* ISAAC_NATIVE_VORBIS_TEST_LOOP_FRAMES=<n> (debug): a stream mirror reports
 * end of stream after n frames, so StreamSourceOgg's loop restart (the
 * native seek_start path) runs within a short session. */
static uint64_t nvw_test_loop_frames(void)
{
    static int64_t frames = -1;

    if (frames < 0) {
        const char *e = getenv("ISAAC_NATIVE_VORBIS_TEST_LOOP_FRAMES");
        frames = e ? atoll(e) : 0;
        if (frames < 0)
            frames = 0;
    }
    return (uint64_t)frames;
}

static int s_force_verify;              /* self-test: verify the next open */

static int nvw_verify_take(void)
{
    if (s_force_verify) {
        s_force_verify = 0;
        ++s_st.v_mirrors;
        return 1;
    }
    if (s_verify_budget < 0) {
        const char *e = getenv("ISAAC_NATIVE_VORBIS_VERIFY");
#if defined(ISAAC_NATIVE_VORBIS_VERIFY)
        s_verify_budget = ISAAC_NATIVE_VORBIS_VERIFY;
#else
        s_verify_budget = 0;
#endif
        if (e && *e)
            s_verify_budget = atoi(e);
        if (s_verify_budget < 0)
            s_verify_budget = 0;
    }
    if (s_verify_budget == 0)
        return 0;
    --s_verify_budget;
    ++s_st.v_mirrors;
    return 1;
}

/* Decoder assertions unwind to the native call boundary (see the assert
 * header); outside one they are counted and ignored, as with NDEBUG. */
void isaac_nv_assert_fail(const char *expression, int line)
{
    (void)expression;
    (void)line;
    ++s_st.asserts;
    if (s_jmp_armed) {
        s_jmp_armed = 0;
        longjmp(s_jmp, 1);
    }
}

/* ------------------------------------------------------------------------
 * Guest stream calls: thiscall, arguments pushed right to left, callee
 * cleans, result in eax.  Caller-saved registers are clobbered. */
static uint32_t nvw_vcall(CPU *__restrict c, uint32_t object, uint32_t slot,
                          uint32_t site, const uint32_t *args, unsigned count,
                          int *ok)
{
    uint32_t vtable;
    uint32_t target;
    uint32_t saved_esp = c->esp;
    unsigned i;

    *ok = 0;
    if (!c || !object || (object & 3U) || c->fault)
        return 0U;
    vtable = ld32(object);
    if (!vtable || (vtable & 3U))
        return 0U;
    target = ld32(vtable + slot);
    if (!target)
        return 0U;
    for (i = count; i-- > 0U;)
        gpush(c, args[i]);
    gpush(c, site);
    c->ecx = object;
    guest_call(c, target);
    if (c->fault || c->esp != saved_esp) {
        c->esp = saved_esp;
        return 0U;
    }
    *ok = 1;
    return c->eax;
}

static int nvw_tell(CPU *__restrict c, uint32_t stream, uint32_t *pos)
{
    int ok;
    uint32_t v = nvw_vcall(c, stream, NVW_VT_TELL, NVW_SITE_TELL, NULL, 0U,
                           &ok);
    if (ok)
        *pos = v;
    return ok;
}

static int nvw_seek(CPU *__restrict c, uint32_t stream, uint32_t pos,
                    uint32_t whence)
{
    uint32_t args[2];
    int ok;

    args[0] = pos;
    args[1] = whence;
    (void)nvw_vcall(c, stream, NVW_VT_SEEK, NVW_SITE_SEEK, args, 2U, &ok);
    return ok;
}

/* Seek to pos (SEEK_SET) and confirm with tell. */
static int nvw_seek_to(CPU *__restrict c, uint32_t stream, uint32_t pos)
{
    uint32_t got = 0U;

    if (nvw_tell(c, stream, &got) && got == pos)
        return 1;
    if (!nvw_seek(c, stream, pos, 0U))
        return 0;
    return nvw_tell(c, stream, &got) && got == pos;
}

static int nvw_refill(nvw_io *io)
{
    CPU *c = s_io_cpu;
    uint32_t args[3];
    uint32_t got;
    int ok;

    if (!c || io->error)
        goto fail;
    if (!s_staging) {
        void *p = isaac_vita_guest_malloc(NVW_IO_BLOCK);
        if (!p)
            goto fail;
        s_staging = guest_addr(p);
    }
    if (!nvw_seek_to(c, io->stream, io->pos))
        goto fail;
    args[0] = s_staging;
    args[1] = 1U;
    args[2] = NVW_IO_BLOCK;
    got = nvw_vcall(c, io->stream, NVW_VT_READ, NVW_SITE_READ, args, 3U, &ok);
    if (!ok)
        goto fail;
    if (got > NVW_IO_BLOCK)
        got = NVW_IO_BLOCK;
    memcpy(io->buf, guest_ptr(s_staging), got);
    io->buf_pos = io->pos;
    io->buf_len = got;
    return got != 0U;
fail:
    if (!io->error) {
        io->error = 1;
        ++s_st.io_errors;
    }
    io->buf_len = 0U;
    return 0;
}

static int nvw_io_cached(const nvw_io *io)
{
    return io->pos >= io->buf_pos && io->pos - io->buf_pos < io->buf_len;
}

static int nvw_io_getc(FILE *token)
{
    nvw_io *io = (nvw_io *)token;

    if (!nvw_io_cached(io) && !nvw_refill(io))
        return EOF;
    return io->buf[io->pos++ - io->buf_pos];
}

static size_t nvw_io_fread(void *buffer, size_t size, size_t count,
                           FILE *token)
{
    nvw_io *io = (nvw_io *)token;
    uint8_t *out = (uint8_t *)buffer;
    size_t want = size * count;
    size_t done = 0U;

    if (!size)
        return 0U;
    while (done < want) {
        uint32_t avail;
        size_t take;

        if (!nvw_io_cached(io) && !nvw_refill(io))
            break;
        avail = io->buf_len - (io->pos - io->buf_pos);
        take = want - done < avail ? want - done : avail;
        memcpy(out + done, io->buf + (io->pos - io->buf_pos), take);
        io->pos += (uint32_t)take;
        done += take;
    }
    return done / size;
}

static int nvw_io_fseek(FILE *token, long offset, int whence)
{
    nvw_io *io = (nvw_io *)token;

    if (whence == SEEK_SET) {
        io->pos = (uint32_t)offset;
        return 0;
    }
    if (whence == SEEK_CUR) {
        io->pos += (uint32_t)offset;
        return 0;
    }
    /* SEEK_END: only on v1.04's set_file_offset failure path. */
    {
        uint32_t pos = 0U;
        CPU *c = s_io_cpu;

        if (!c || !nvw_seek(c, io->stream, (uint32_t)offset, 2U) ||
                !nvw_tell(c, io->stream, &pos)) {
            io->error = 1;
            return -1;
        }
        io->pos = pos;
        return 0;
    }
}

static long nvw_io_ftell(FILE *token)
{
    return (long)((nvw_io *)token)->pos;
}

static int nvw_io_fclose(FILE *token)
{
    (void)token;
    return 0;
}

static FILE *nvw_io_fopen(const char *name, const char *mode)
{
    (void)name;
    (void)mode;
    return NULL;
}

/* ------------------------------------------------------------------------
 * Mirror table. */
static void nvw_note_arena(nvw_mirror *m)
{
    stb_vorbis_info info = stb_vorbis_get_info(m->v);
    uint32_t used = info.setup_memory_required + info.temp_memory_required;

    if (used > s_st.arena_used_max)
        s_st.arena_used_max = used;
}

static void nvw_destroy(nvw_mirror *m)
{
    uint32_t was_active;

    nvw_spin_lock();
    was_active = m->active;
    m->active = 0U;
    m->freed = 0U;
    nvw_spin_unlock();
    if (was_active)
        __atomic_sub_fetch(&s_live, 1U, __ATOMIC_RELEASE);
    free(m->arena);
    free(m->io);
    memset(m, 0, sizeof *m);
}

/* Drop every mirror the free observer flagged.  (A mirror whose state lost
 * its marker is dropped at its next lookup; a flagged state's memory is not
 * read again.) */
static void nvw_sweep(void)
{
    uint32_t i;

    for (i = 0U; i < NVW_MIRRORS_MAX; ++i) {
        nvw_mirror *m = &s_mirrors[i];

        if (!m->active)
            continue;
        if (__atomic_load_n(&m->freed, __ATOMIC_ACQUIRE)) {
            ++s_st.retire_freed;
            nvw_destroy(m);
        }
    }
}

static nvw_mirror *nvw_lookup(uint32_t f)
{
    uint32_t i;

    if (!f || !__atomic_load_n(&s_live, __ATOMIC_ACQUIRE))
        return NULL;
    for (i = 0U; i < NVW_MIRRORS_MAX; ++i) {
        nvw_mirror *m = &s_mirrors[i];

        if (!m->active || m->f != f)
            continue;
        if (__atomic_load_n(&m->freed, __ATOMIC_ACQUIRE)) {
            ++s_st.retire_freed;
            nvw_destroy(m);
            return NULL;
        }
        if (ld32(f + NVW_ST_MARKER) != m->marker) {
            ++s_st.retire_dead;
            nvw_destroy(m);
            return NULL;
        }
        return m;
    }
    return NULL;
}

/* A new guest state at f: whatever mirror sat at that address is stale. */
static void nvw_drop_address(uint32_t f)
{
    uint32_t i;

    for (i = 0U; i < NVW_MIRRORS_MAX; ++i) {
        if (s_mirrors[i].active && s_mirrors[i].f == f) {
            ++s_st.replaced;
            nvw_destroy(&s_mirrors[i]);
        }
    }
}

static nvw_mirror *nvw_slot(void)
{
    uint32_t i;

    nvw_sweep();
    for (i = 0U; i < NVW_MIRRORS_MAX; ++i)
        if (!s_mirrors[i].active)
            return &s_mirrors[i];
    ++s_st.pool_full;
    return NULL;
}

static void nvw_publish(nvw_mirror *m, uint32_t f)
{
    uint32_t live;

    m->marker = 0x80000000U | (++s_marker_seq & 0x7fffffffU);
    st32(f + NVW_ST_MARKER, m->marker);
    m->f = f;
    nvw_spin_lock();
    __atomic_store_n(&m->active, 1U, __ATOMIC_RELEASE);
    nvw_spin_unlock();
    live = __atomic_add_fetch(&s_live, 1U, __ATOMIC_RELEASE);
    if (live > s_st.live_max)
        s_st.live_max = live;
}

/* Arena size for a guest state whose own stb_vorbis_alloc is guest_len. */
static uint32_t nvw_arena_bytes(uint32_t guest_len, int attempt)
{
    uint32_t bytes = guest_len * 2U;

    if (bytes < NVW_ARENA_MIN)
        bytes = NVW_ARENA_MIN;
    if (attempt)
        bytes *= 3U;
    if (bytes > NVW_ARENA_MAX)
        bytes = NVW_ARENA_MAX;
    return (bytes + 15U) & ~15U;
}

/* Runs the native open (memory when io is NULL) with assertion recovery.
 * Retries once with a larger arena on VORBIS_outofmem. */
static stb_vorbis *nvw_native_open(CPU *__restrict c, nvw_mirror *m,
                                   const uint8_t *data, int len,
                                   uint32_t stream_len)
{
    uint32_t guest_len = ld32(m->f + NVW_ST_ALLOC_LEN);
    int attempt;

    for (attempt = 0; attempt < 2; ++attempt) {
        stb_vorbis_alloc alloc;
        stb_vorbis *volatile v = NULL;
        int error = 0;

        m->arena_bytes = nvw_arena_bytes(guest_len, attempt);
        m->arena = malloc(m->arena_bytes);
        if (!m->arena)
            return NULL;
        alloc.alloc_buffer = (char *)m->arena;
        alloc.alloc_buffer_length_in_bytes = (int)m->arena_bytes;
        s_io_cpu = c;
        if (setjmp(s_jmp) == 0) {
            s_jmp_armed = 1;
            if (m->io) {
                m->io->pos = ld32(m->f + NVW_ST_F_START);
                m->io->buf_len = 0U;
                m->io->error = 0;
                v = stb_vorbis_open_file_section((FILE *)m->io, 0, &error,
                                                 &alloc, stream_len);
            } else {
                v = stb_vorbis_open_memory(data, len, &error, &alloc);
            }
            s_jmp_armed = 0;
        } else {
            v = NULL;
        }
        s_io_cpu = NULL;
        if (v && (!m->io || !m->io->error))
            return v;
        free(m->arena);
        m->arena = NULL;
        if (v || error != VORBIS_outofmem)
            return NULL;
        ++s_st.arena_retry;
    }
    return NULL;
}

static int nvw_layout_matches(const nvw_mirror *m)
{
    return (uint32_t)m->v->channels == ld32(m->f + NVW_ST_CHANNELS) &&
           m->v->sample_rate == ld32(m->f + NVW_ST_SAMPLE_RATE) &&
           m->v->channels > 0 && m->v->channels <= STB_VORBIS_MAX_CHANNELS;
}

/* ------------------------------------------------------------------------
 * Native calls with assertion recovery.  *ok = 0 when the decoder asserted
 * or a guest stream call failed. */
static int nvw_native_samples(CPU *__restrict c, nvw_mirror *m, int channels,
                              short *out, int num_shorts, int *ok)
{
    volatile int n = 0;

    s_io_cpu = c;
    if (m->io)
        m->io->error = 0;
    if (setjmp(s_jmp) == 0) {
        s_jmp_armed = 1;
        n = stb_vorbis_get_samples_short_interleaved(m->v, channels, out,
                                                     num_shorts);
        s_jmp_armed = 0;
        *ok = !(m->io && m->io->error);
    } else {
        n = 0;
        *ok = 0;
    }
    s_io_cpu = NULL;
    return n;
}

static int nvw_native_seek_start(CPU *__restrict c, nvw_mirror *m)
{
    int ok;

    s_io_cpu = c;
    if (m->io)
        m->io->error = 0;
    if (setjmp(s_jmp) == 0) {
        s_jmp_armed = 1;
        stb_vorbis_seek_start(m->v);
        s_jmp_armed = 0;
        ok = !(m->io && m->io->error) && m->v->error != VORBIS_invalid_api_mixing;
    } else {
        ok = 0;
    }
    s_io_cpu = NULL;
    return ok;
}

/* ------------------------------------------------------------------------
 * Wrapped guest entries. */
void __real_sub_005bdf80(CPU *__restrict c);
void __real_sub_005b7740(CPU *__restrict c);
void __real_sub_005bb260(CPU *__restrict c);
void __real_sub_005bd310(CPU *__restrict c);
void __real_sub_005bd740(CPU *__restrict c);
void __real_sub_005bd8b0(CPU *__restrict c);

static void nvw_finish_open(CPU *__restrict c, nvw_mirror *m, uint32_t f,
                            uint32_t eax, uint32_t ecx, uint32_t edx)
{
    (void)c;
    nvw_publish(m, f);
    if (m->mode == NVW_MODE_MEMORY)
        ++s_st.mirrors_mem;
    else
        ++s_st.mirrors_stream;
    c->eax = eax;
    c->ecx = ecx;
    c->edx = edx;
}

/* stb_vorbis_open_memory(data = ecx, len = edx, [esp+4] int *error,
 * [esp+8] stb_vorbis_alloc *) -> eax state or 0; caller cleans. */
void __wrap_sub_005bd8b0(CPU *__restrict c)
{
    uint32_t data = c->ecx;
    int32_t len = (int32_t)c->edx;
    uint32_t f;
    nvw_mirror *m;

    __real_sub_005bd8b0(c);
    f = c->eax;
    if (c->fault || !f || (f & 3U) || !nvw_enabled())
        return;
    nvw_lock();
    ++s_st.open_mem;
    nvw_drop_address(f);
    if (!data || len <= 0 || ld8(f + NVW_ST_PUSH_MODE)) {
        nvw_fallback(f, "open-args");
        nvw_unlock();
        return;
    }
    m = nvw_slot();
    if (!m) {
        nvw_fallback(f, "pool-full");
        nvw_unlock();
        return;
    }
    m->f = f;
    m->mode = NVW_MODE_MEMORY;
    m->v = nvw_native_open(c, m, (const uint8_t *)guest_ptr(data), len, 0U);
    if (!m->v) {
        ++s_st.open_fail;
        nvw_fallback(f, "native-open");
        nvw_destroy(m);
        nvw_unlock();
        return;
    }
    if (!nvw_layout_matches(m)) {
        ++s_st.layout_fail;
        nvw_fallback(f, "layout");
        nvw_destroy(m);
        nvw_unlock();
        return;
    }
    nvw_note_arena(m);
    m->data_lo = data;
    m->alloc_lo = ld32(f + NVW_ST_ALLOC_BUFFER);
    m->alloc_len = m->alloc_lo ? ld32(f + NVW_ST_ALLOC_LEN) : 0U;
    m->verify = nvw_verify_take();
    nvw_finish_open(c, m, f, f, c->ecx, c->edx);
    nvw_unlock();
}

/* ------------------------------------------------------------------------
 * Self-test (ISAAC_NATIVE_VORBIS_SELFTEST=<n>, debug only): for the first n
 * music streams, read the whole OGG through the guest stream into a guest
 * buffer and push it through the wrapped whole-sample path exactly as
 * sub_005a36a0 does (guest_call -> open_memory, get_samples, deinit), once
 * with the mirror verified against the translated decoder and once served
 * natively, and compare the two PCM CRCs.  Results land in the receipt. */
#ifndef NVW_SELFTEST_CALLS
#define NVW_SELFTEST_CALLS 48U
#endif
#define NVW_SELFTEST_FRAMES 4096U

typedef struct nvw_selftest_stats {
    uint32_t files, runs, frames_verified, frames_native, crc_equal, crc_diff,
             failed;
} nvw_selftest_stats;
static nvw_selftest_stats s_selftest;
static int s_selftest_budget = -1;

static uint32_t nvw_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    int k;

    crc = ~crc;
    for (i = 0U; i < n; ++i) {
        crc ^= p[i];
        for (k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

/* Direct guest call with the frozen custom convention: ecx/edx registers,
 * stack words pushed right to left, caller cleans. */
static uint32_t nvw_guest_fn(CPU *__restrict c, uint32_t rva, uint32_t ecx,
                             uint32_t edx, const uint32_t *args,
                             unsigned count, uint32_t site, int *ok)
{
    uint32_t saved_esp = c->esp;
    unsigned i;

    for (i = count; i-- > 0U;)
        gpush(c, args[i]);
    gpush(c, site);
    c->ecx = ecx;
    c->edx = edx;
    guest_call(c, (uint32_t)GUEST_IMAGE_BASE + rva);
    *ok = !c->fault && c->esp == saved_esp - 4U * count;
    c->esp = saved_esp;
    return c->eax;
}

/* One whole-sample pass; returns frames decoded, *crc over the PCM. */
static uint32_t nvw_selftest_pass(CPU *__restrict c, uint32_t data,
                                  uint32_t len, uint32_t alloc,
                                  uint32_t error, uint32_t pcm, int verify,
                                  uint32_t *crc, int *ok)
{
    uint32_t args[2];
    uint32_t f;
    uint32_t channels;
    uint32_t frames = 0U;
    unsigned k;

    *crc = 0U;
    s_force_verify = verify;
    args[0] = error;
    args[1] = alloc;
    f = nvw_guest_fn(c, NVW_RVA_OPEN_MEMORY, data, len, args, 2U, 0x5a3718U,
                     ok);
    s_force_verify = 0;
    if (!*ok || !f)
        return 0U;
    if (!nvw_lookup(f)) {
        *ok = 0;   /* not mirrored */
    }
    channels = ld32(f + NVW_ST_CHANNELS);
    for (k = 0U; *ok && k < NVW_SELFTEST_CALLS; ++k) {
        uint32_t n;

        args[0] = pcm;
        args[1] = NVW_SELFTEST_FRAMES * channels;
        n = nvw_guest_fn(c, NVW_RVA_GET_SAMPLES, f, channels, args, 2U,
                         0x5a37a3U, ok);
        if (!*ok || (int32_t)n <= 0)
            break;
        *crc = nvw_crc32(*crc, (const uint8_t *)guest_ptr(pcm),
                         (size_t)n * channels * 2U);
        frames += n;
    }
    {
        int ok2;
        (void)nvw_guest_fn(c, 0x005bd310U, f, 0U, NULL, 0U, 0x5a378dU, &ok2);
        if (!ok2)
            *ok = 0;
    }
    return frames;
}

static void nvw_selftest(CPU *__restrict c, nvw_mirror *stream_m)
{
    uint32_t stream = stream_m->io->stream;
    uint32_t start = ld32(stream_m->f + NVW_ST_F_START);
    uint32_t len = ld32(stream_m->f + NVW_ST_STREAM_LEN);
    uint32_t back = 0U;
    void *data_p = NULL, *alloc_p = NULL, *buf_p = NULL, *pcm_p = NULL;
    uint32_t data, got = 0U;
    uint32_t crc_v = 0U, crc_n = 0U, fr_v, fr_n;
    int ok = 0;

    if (s_selftest_budget < 0) {
        const char *e = getenv("ISAAC_NATIVE_VORBIS_SELFTEST");
        s_selftest_budget = e ? atoi(e) : 0;
    }
    if (s_selftest_budget <= 0 || len == 0U || len > 64U * 1024U * 1024U)
        return;
    --s_selftest_budget;
    ++s_selftest.files;
    if (!nvw_tell(c, stream, &back))
        goto out;
    data_p = isaac_vita_guest_malloc(len);
    alloc_p = isaac_vita_guest_malloc(16U);
    buf_p = isaac_vita_guest_malloc(0x4b000U);
    pcm_p = isaac_vita_guest_malloc(NVW_SELFTEST_FRAMES * 2U *
                                    STB_VORBIS_MAX_CHANNELS);
    if (!data_p || !alloc_p || !buf_p || !pcm_p)
        goto out;
    data = guest_addr(data_p);
    if (!nvw_seek_to(c, stream, start))
        goto out;
    while (got < len) {
        uint32_t args[3];
        uint32_t n;
        int rok;

        args[0] = data + got;
        args[1] = 1U;
        args[2] = len - got;
        n = nvw_vcall(c, stream, NVW_VT_READ, NVW_SITE_READ, args, 3U, &rok);
        if (!rok || n == 0U || n > len - got)
            break;
        got += n;
    }
    (void)nvw_seek_to(c, stream, back);
    if (got != len)
        goto out;
    /* stb_vorbis_alloc {buffer, length} followed by the int *error word */
    st32(guest_addr(alloc_p), guest_addr(buf_p));
    st32(guest_addr(alloc_p) + 4U, 0x4b000U);
    st32(guest_addr(alloc_p) + 8U, 0U);
    fr_v = nvw_selftest_pass(c, data, len, guest_addr(alloc_p),
                             guest_addr(alloc_p) + 8U, guest_addr(pcm_p), 1,
                             &crc_v, &ok);
    if (ok)
        fr_n = nvw_selftest_pass(c, data, len, guest_addr(alloc_p),
                                 guest_addr(alloc_p) + 8U, guest_addr(pcm_p),
                                 0, &crc_n, &ok);
    else
        fr_n = 0U;
    if (ok) {
        ++s_selftest.runs;
        s_selftest.frames_verified += fr_v;
        s_selftest.frames_native += fr_n;
        if (fr_v == fr_n && crc_v == crc_n)
            ++s_selftest.crc_equal;
        else
            ++s_selftest.crc_diff;
        isaac_vita_log("KAGE NATIVE VORBIS SELFTEST: bytes=%u frames=%u/%u "
                       "crc_translated=%08x crc_native=%08x",
                       (unsigned)len, (unsigned)fr_v, (unsigned)fr_n,
                       (unsigned)crc_v, (unsigned)crc_n);
    }
out:
    if (!ok)
        ++s_selftest.failed;
    if (pcm_p) isaac_vita_guest_free(pcm_p);
    if (buf_p) isaac_vita_guest_free(buf_p);
    if (alloc_p) isaac_vita_guest_free(alloc_p);
    if (data_p) isaac_vita_guest_free(data_p);
}

/* stb_vorbis_open_file (game edit: KAGE stream = ecx, [esp+4] int *error,
 * [esp+8] stb_vorbis_alloc *) -> eax state or 0; caller cleans. */
void __wrap_sub_005bd740(CPU *__restrict c)
{
    uint32_t f;
    uint32_t eax, ecx, edx;
    uint32_t stream;
    nvw_mirror *m;

    __real_sub_005bd740(c);
    f = c->eax;
    if (c->fault || !f || (f & 3U) || !nvw_enabled())
        return;
    eax = c->eax;
    ecx = c->ecx;
    edx = c->edx;
    nvw_lock();
    ++s_st.open_stream;
    nvw_drop_address(f);
    stream = ld32(f + NVW_ST_STREAM);
    if (!stream || ld8(f + NVW_ST_PUSH_MODE)) {
        nvw_fallback(f, "open-args");
        nvw_unlock();
        return;
    }
    m = nvw_slot();
    if (!m) {
        nvw_fallback(f, "pool-full");
        nvw_unlock();
        return;
    }
    m->f = f;
    m->mode = NVW_MODE_STREAM;
    m->io = (nvw_io *)calloc(1U, sizeof *m->io);
    if (!m->io || !nvw_tell(c, stream, &m->post_open_pos)) {
        nvw_fallback(f, m->io ? "open-tell" : "open-memory");
        nvw_destroy(m);
        c->eax = eax; c->ecx = ecx; c->edx = edx;
        nvw_unlock();
        return;
    }
    m->io->stream = stream;
    m->v = nvw_native_open(c, m, NULL, 0, ld32(f + NVW_ST_STREAM_LEN));
    if (!m->v || !nvw_layout_matches(m)) {
        if (m->v) {
            ++s_st.layout_fail;
            nvw_fallback(f, "layout");
        } else {
            ++s_st.open_fail;
            nvw_fallback(f, "native-open");
        }
        /* the translated state expects the stream where its open left it */
        if (!nvw_seek_to(c, stream, m->post_open_pos))
            nvw_fallback(f, "open-restore");
        nvw_destroy(m);
        c->eax = eax; c->ecx = ecx; c->edx = edx;
        nvw_unlock();
        return;
    }
    nvw_note_arena(m);
    m->alloc_lo = ld32(f + NVW_ST_ALLOC_BUFFER);
    m->alloc_len = m->alloc_lo ? ld32(f + NVW_ST_ALLOC_LEN) : 0U;
    m->verify = nvw_verify_take();
    if (m->verify)
        (void)nvw_seek_to(c, stream, m->post_open_pos);
    nvw_finish_open(c, m, f, eax, ecx, edx);
    nvw_selftest(c, m);
    c->eax = eax;
    c->ecx = ecx;
    c->edx = edx;
    nvw_unlock();
}

static void nvw_call_real_samples(CPU *__restrict c, uint32_t eax,
                                  uint32_t ecx, uint32_t edx)
{
    c->eax = eax;
    c->ecx = ecx;
    c->edx = edx;
    __real_sub_005bdf80(c);
}

static void nvw_verify_call(CPU *__restrict c, nvw_mirror *m, uint32_t eax,
                            uint32_t f, int channels, uint32_t buffer,
                            int num_shorts)
{
    uint32_t gpos = 0U;
    int have_gpos = 0;
    int ok = 0;
    int n_native;
    int n_real;
    uint64_t t0, t1, t2;
    size_t need = (size_t)num_shorts;

    if (need > s_verify_cap) {
        int16_t *p = (int16_t *)realloc(s_verify_buf, need * sizeof *p);
        if (!p) {
            nvw_fallback(f, "verify-memory");
            nvw_destroy(m);
            nvw_unlock();
            nvw_call_real_samples(c, eax, f, (uint32_t)channels);
            return;
        }
        s_verify_buf = p;
        s_verify_cap = need;
    }
    if (m->mode == NVW_MODE_STREAM)
        have_gpos = nvw_tell(c, m->io->stream, &gpos);
    t0 = nvw_now_us();
    n_native = nvw_native_samples(c, m, channels, s_verify_buf, num_shorts,
                                  &ok);
    t1 = nvw_now_us();
    if (have_gpos)
        (void)nvw_seek_to(c, m->io->stream, gpos);
    if (!ok) {
        ++s_st.broken_drop;
        nvw_fallback(f, "verify-native-broken");
        nvw_destroy(m);
        m = NULL;
    }
    nvw_unlock();
    t1 = t1 - t0;
    t0 = nvw_now_us();
    nvw_call_real_samples(c, eax, f, (uint32_t)channels);
    t2 = nvw_now_us();
    nvw_lock();
    s_st.v_us_native += t1;
    s_st.v_us_real += t2 - t0;
    if (!ok || c->fault) {
        nvw_unlock();
        return;
    }
    n_real = (int32_t)c->eax;
    ++s_st.v_calls;
    if (n_native != n_real) {
        ++s_st.v_count_diff;
        if (s_st.v_count_diff <= 4U)
            isaac_vita_log("KAGE NATIVE VORBIS VERIFY: f=%08x count native=%d "
                           "translated=%d req=%d", (unsigned)f, n_native,
                           n_real, num_shorts / channels);
    } else {
        const int16_t *a = s_verify_buf;
        const int16_t *b = (const int16_t *)guest_ptr(buffer);
        uint32_t i, diff = 0U, maxabs = 0U;

        for (i = 0U; i < (uint32_t)(n_real * channels); ++i) {
            if (a[i] != b[i]) {
                int32_t d = (int32_t)a[i] - (int32_t)b[i];
                if (d < 0)
                    d = -d;
                ++diff;
                if ((uint32_t)d > maxabs)
                    maxabs = (uint32_t)d;
            }
        }
        if (diff) {
            ++s_st.v_sample_diff;
            s_st.v_diff_samples += diff;
            if (maxabs > s_st.v_maxabs)
                s_st.v_maxabs = maxabs;
            if (s_st.v_sample_diff <= 4U)
                isaac_vita_log("KAGE NATIVE VORBIS VERIFY: f=%08x frames=%d "
                               "diff=%u maxabs=%u", (unsigned)f, n_real,
                               (unsigned)diff, (unsigned)maxabs);
        } else {
            ++s_st.v_match;
        }
    }
    nvw_unlock();
}

/* get_samples_short_interleaved(f = ecx, channels = edx, [esp+4] buffer,
 * [esp+8] num_shorts) -> eax frames; plain ret, caller cleans. */
void __wrap_sub_005bdf80(CPU *__restrict c)
{
    uint32_t entry_eax = c->eax;
    uint32_t f = c->ecx;
    int32_t channels = (int32_t)c->edx;
    uint32_t buffer;
    int32_t num_shorts;
    nvw_mirror *m;
    int ok = 0;
    int n;
    uint64_t t0, dt;

    if (!f || (f & 3U) || !__atomic_load_n(&s_live, __ATOMIC_ACQUIRE)) {
        __real_sub_005bdf80(c);
        return;
    }
    nvw_lock();
    m = nvw_lookup(f);
    if (!m) {
        ++s_st.calls_real;
        nvw_unlock();
        __real_sub_005bdf80(c);
        return;
    }
    buffer = ld32(guest_stack_address(c, c->esp + 4U, 4U,
                                      NVW_RVA_GET_SAMPLES));
    num_shorts = (int32_t)ld32(guest_stack_address(c, c->esp + 8U, 4U,
                                                   NVW_RVA_GET_SAMPLES));
    if (channels <= 0 || channels > STB_VORBIS_MAX_CHANNELS || !buffer ||
            (buffer & 1U) || num_shorts < channels) {
        /* keep the guest state authoritative only while nothing was
         * delivered; otherwise answer like the native decoder would */
        if (m->delivered == 0U && !m->broken) {
            nvw_fallback(f, "arguments");
            if (m->mode == NVW_MODE_STREAM)
                (void)nvw_seek_to(c, m->io->stream, m->post_open_pos);
            nvw_destroy(m);
            nvw_unlock();
            nvw_call_real_samples(c, entry_eax, f, (uint32_t)channels);
            return;
        }
        nvw_unlock();
        c->eax = 0U;
        (void)gpop(c);
        return;
    }
    if (m->verify) {
        nvw_verify_call(c, m, entry_eax, f, channels, buffer, num_shorts);
        return;
    }
    if (m->broken || (m->mode == NVW_MODE_STREAM && nvw_test_loop_frames() &&
                      m->delivered >= nvw_test_loop_frames())) {
        nvw_unlock();
        c->eax = 0U;
        (void)gpop(c);
        return;
    }
    t0 = nvw_now_us();
    n = nvw_native_samples(c, m, channels, (short *)guest_ptr(buffer),
                           num_shorts, &ok);
    dt = nvw_now_us() - t0;
    if (!ok) {
        if (m->delivered == 0U) {
            /* nothing left the mirror: the guest state is exactly where
             * its open put it; put the stream back there too */
            ++s_st.clean_fallback;
            nvw_fallback(f, "native-broken");
            if (m->mode == NVW_MODE_STREAM &&
                    !nvw_seek_to(c, m->io->stream, m->post_open_pos))
                nvw_fallback(f, "restore");
            nvw_destroy(m);
            nvw_unlock();
            nvw_call_real_samples(c, entry_eax, f, (uint32_t)channels);
            return;
        }
        ++s_st.broken_mid;
        nvw_fallback(f, "native-broken-mid");
        m->broken = 1;
        if (n < 0)
            n = 0;
    }
    m->delivered += (uint32_t)n;
    if (m->mode == NVW_MODE_MEMORY) {
        ++s_st.calls_mem;
        s_st.frames_mem += (uint32_t)n;
        s_st.us_mem += dt;
        if (dt > s_st.us_max_mem)
            s_st.us_max_mem = (uint32_t)dt;
    } else {
        ++s_st.calls_stream;
        s_st.frames_stream += (uint32_t)n;
        s_st.us_stream += dt;
        if (dt > s_st.us_max_stream)
            s_st.us_max_stream = (uint32_t)dt;
    }
    nvw_unlock();
    c->eax = (uint32_t)n;
    (void)gpop(c);
}

/* set_file_offset(f = ecx, loc = edx); plain ret.  From Decode's inlined
 * seek_start on a mirrored state: native seek_start, then no-op. */
void __wrap_sub_005b7740(CPU *__restrict c)
{
    uint32_t f = c->ecx;
    nvw_mirror *m;
    int ok;

    if (!__atomic_load_n(&s_live, __ATOMIC_ACQUIRE) ||
            ld32(c->esp) != NVW_RET_DECODE_SEEK) {
        __real_sub_005b7740(c);
        return;
    }
    nvw_lock();
    m = nvw_lookup(f);
    if (!m) {
        nvw_unlock();
        __real_sub_005b7740(c);
        return;
    }
    if (m->broken) {
        /* the translated seek_start resynchronises the guest state */
        ++s_st.seek_start_real;
        nvw_destroy(m);
        nvw_unlock();
        __real_sub_005b7740(c);
        return;
    }
    {
        uint32_t ecx = c->ecx, edx = c->edx;

        ok = nvw_native_seek_start(c, m);
        c->ecx = ecx;
        c->edx = edx;
    }
    if (!ok) {
        ++s_st.broken_drop;
        ++s_st.seek_start_real;
        nvw_fallback(f, "native-seek-start");
        nvw_destroy(m);
        nvw_unlock();
        __real_sub_005b7740(c);
        return;
    }
    ++s_st.seek_start_native;
    m->delivered = 0U;
    if (m->verify) {
        /* the guest decoder is authoritative: let it seek too */
        nvw_unlock();
        __real_sub_005b7740(c);
        return;
    }
    m->skip_pump = 1;
    nvw_unlock();
    c->eax = 1U;
    (void)gpop(c);
}

/* vorbis_pump_first_frame(f = ecx); plain ret. */
void __wrap_sub_005bb260(CPU *__restrict c)
{
    nvw_mirror *m;

    if (!__atomic_load_n(&s_live, __ATOMIC_ACQUIRE) ||
            ld32(c->esp) != NVW_RET_DECODE_PUMP) {
        __real_sub_005bb260(c);
        return;
    }
    nvw_lock();
    m = nvw_lookup(c->ecx);
    if (m && m->skip_pump) {
        m->skip_pump = 0;
        ++s_st.pump_skipped;
        nvw_unlock();
        c->eax = 1U;
        (void)gpop(c);
        return;
    }
    nvw_unlock();
    __real_sub_005bb260(c);
}

/* vorbis_deinit(f = ecx). */
void __wrap_sub_005bd310(CPU *__restrict c)
{
    if (__atomic_load_n(&s_live, __ATOMIC_ACQUIRE)) {
        uint32_t i;
        uint32_t f = c->ecx;

        nvw_lock();
        for (i = 0U; i < NVW_MIRRORS_MAX; ++i) {
            if (s_mirrors[i].active && s_mirrors[i].f == f) {
                ++s_st.retire_deinit;
                nvw_destroy(&s_mirrors[i]);
            }
        }
        nvw_unlock();
    }
    __real_sub_005bd310(c);
}

/* Guest heap free observer (weak in host_vita_heap.c), any thread: flag
 * mirrors whose alloc buffer, state or source data is being freed. */
void isaac_nv_guest_buffer_freed(void *pointer)
{
    uintptr_t offset;
    uint32_t a;
    uint32_t i;

    if (!pointer || !__atomic_load_n(&s_live, __ATOMIC_ACQUIRE))
        return;
    offset = (uintptr_t)pointer - GUEST_MEM_BASE;
    if (offset > (uintptr_t)UINT32_MAX)
        return;
    a = (uint32_t)offset;
    nvw_spin_lock();
    for (i = 0U; i < NVW_MIRRORS_MAX; ++i) {
        nvw_mirror *m = &s_mirrors[i];

        if (!m->active)
            continue;
        if (a == m->f || a == m->data_lo ||
                (m->alloc_lo && a - m->alloc_lo < m->alloc_len))
            __atomic_store_n(&m->freed, 1U, __ATOMIC_RELEASE);
    }
    nvw_spin_unlock();
}

void isaac_native_vorbis_report(const char *why)
{
    const nvw_stats *s = &s_st;
    const char *w = why ? why : "?";

    /* isaac_vita_log bodies are 384 bytes: three lines. */
    isaac_vita_log(
        "KAGE NATIVE VORBIS: why=%s open_mem=%u open_stream=%u "
        "mirrors_mem=%u mirrors_stream=%u calls_mem=%u calls_stream=%u "
        "calls_real=%u frames_mem=%llu frames_stream=%llu ms_mem=%llu "
        "ms_stream=%llu max_us_mem=%u max_us_stream=%u fallbacks=%u",
        w, (unsigned)s->open_mem, (unsigned)s->open_stream,
        (unsigned)s->mirrors_mem, (unsigned)s->mirrors_stream,
        (unsigned)s->calls_mem, (unsigned)s->calls_stream,
        (unsigned)s->calls_real, (unsigned long long)s->frames_mem,
        (unsigned long long)s->frames_stream,
        (unsigned long long)(s->us_mem / 1000U),
        (unsigned long long)(s->us_stream / 1000U),
        (unsigned)s->us_max_mem, (unsigned)s->us_max_stream,
        (unsigned)s->fallbacks);
    isaac_vita_log(
        "KAGE NATIVE VORBIS LIFE: why=%s seek_native=%u seek_real=%u "
        "pump_skip=%u open_fail=%u layout_fail=%u pool_full=%u clean_fb=%u "
        "broken_mid=%u broken_drop=%u asserts=%u io_err=%u retire_deinit=%u "
        "retire_freed=%u retire_dead=%u replaced=%u live=%u live_max=%u "
        "arena_max=%u arena_retry=%u foreign=%u",
        w, (unsigned)s->seek_start_native, (unsigned)s->seek_start_real,
        (unsigned)s->pump_skipped, (unsigned)s->open_fail,
        (unsigned)s->layout_fail, (unsigned)s->pool_full,
        (unsigned)s->clean_fallback, (unsigned)s->broken_mid,
        (unsigned)s->broken_drop, (unsigned)s->asserts,
        (unsigned)s->io_errors, (unsigned)s->retire_deinit,
        (unsigned)s->retire_freed, (unsigned)s->retire_dead,
        (unsigned)s->replaced,
        (unsigned)__atomic_load_n(&s_live, __ATOMIC_ACQUIRE),
        (unsigned)s->live_max, (unsigned)s->arena_used_max,
        (unsigned)s->arena_retry, (unsigned)s->foreign_calls);
    if (s->v_mirrors || s_selftest.files)
        isaac_vita_log(
            "KAGE NATIVE VORBIS VERIFY: why=%s mirrors=%u calls=%u match=%u "
            "count_diff=%u sample_diff=%u diff_samples=%llu maxabs=%u "
            "native_ms=%llu translated_ms=%llu selftest_files=%u runs=%u "
            "frames=%u/%u crc_equal=%u crc_diff=%u failed=%u",
            w, (unsigned)s->v_mirrors, (unsigned)s->v_calls,
            (unsigned)s->v_match, (unsigned)s->v_count_diff,
            (unsigned)s->v_sample_diff,
            (unsigned long long)s->v_diff_samples, (unsigned)s->v_maxabs,
            (unsigned long long)(s->v_us_native / 1000U),
            (unsigned long long)(s->v_us_real / 1000U),
            (unsigned)s_selftest.files, (unsigned)s_selftest.runs,
            (unsigned)s_selftest.frames_verified,
            (unsigned)s_selftest.frames_native,
            (unsigned)s_selftest.crc_equal, (unsigned)s_selftest.crc_diff,
            (unsigned)s_selftest.failed);
}
