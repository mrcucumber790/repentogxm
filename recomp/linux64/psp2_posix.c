/* POSIX implementations of the VitaSDK calls the shared runtime makes.
 *
 * Paths are used as given.  The host entry point chdir()s into the data root
 * and lays out `ux0:/data`, `ux0:data -> ux0:/data` and `app0:` there, so the
 * Vita spellings `ux0:data/...`, `ux0:/data/...` and `app0:/...` all resolve
 * as ordinary relative paths with no translation layer. */
#include "psp2_compat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "host_tid.h"
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <time.h>
#include <unistd.h>

#include "guest_window.h"

/* glibc spells st_atime/st_mtime/st_ctime as macros over st_*tim.tv_sec,
 * which would rewrite the SceIoStat fields of the same name. */
#undef st_atime
#undef st_mtime
#undef st_ctime

static int sce_errno(void)
{
    /* SCE_ERROR_ERRNO_* is 0x80010000 | errno. */
    return (int)(0x80010000u | (unsigned)(errno & 0xffff));
}

/* ------------------------------------------------------------------ io */

SceUID sceIoOpen(const char *file, int flags, SceMode mode)
{
    int f = 0, fd;
    switch (flags & SCE_O_RDWR) {
    case SCE_O_WRONLY: f = O_WRONLY; break;
    case SCE_O_RDWR: f = O_RDWR; break;
    default: f = O_RDONLY; break;
    }
    if (flags & SCE_O_APPEND) f |= O_APPEND;
    if (flags & SCE_O_CREAT) f |= O_CREAT;
    if (flags & SCE_O_TRUNC) f |= O_TRUNC;
    if (flags & SCE_O_EXCL) f |= O_EXCL;
    fd = open(file, f | O_CLOEXEC, mode ? (mode_t)mode : 0666);
    return fd < 0 ? sce_errno() : fd;
}

int sceIoClose(SceUID fd) { return close(fd) == 0 ? 0 : sce_errno(); }

SceSSize sceIoRead(SceUID fd, void *buf, SceSize nbyte)
{
    ssize_t r = read(fd, buf, nbyte);
    return r < 0 ? sce_errno() : r;
}

SceSSize sceIoWrite(SceUID fd, const void *buf, SceSize nbyte)
{
    ssize_t r = write(fd, buf, nbyte);
    return r < 0 ? sce_errno() : r;
}

#if defined(__SWITCH__)
/* newlib on libnx has no pread(): seek, read and restore the position under
 * one lock so concurrent positional reads cannot interleave. */
static pthread_mutex_t s_pread_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

int sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset)
{
#if defined(__SWITCH__)
    ssize_t r;
    off_t saved;
    pthread_mutex_lock(&s_pread_lock);
    saved = lseek(fd, 0, SEEK_CUR);
    if (saved < 0 || lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        int err = sce_errno();
        pthread_mutex_unlock(&s_pread_lock);
        return err;
    }
    r = read(fd, data, size);
    {
        int read_errno = errno;
        lseek(fd, saved, SEEK_SET);
        errno = read_errno;
    }
    if (r < 0) {
        int err = sce_errno();
        pthread_mutex_unlock(&s_pread_lock);
        return err;
    }
    pthread_mutex_unlock(&s_pread_lock);
    return (int)r;
#else
    ssize_t r = pread(fd, data, size, (off_t)offset);
    return r < 0 ? sce_errno() : (int)r;
#endif
}

SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    off_t r = lseek(fd, (off_t)offset, whence);
    return r < 0 ? sce_errno() : (SceOff)r;
}

long sceIoLseek32(SceUID fd, long offset, int whence)
{
    off_t r = lseek(fd, (off_t)offset, whence);
    return r < 0 ? sce_errno() : (long)r;
}

int sceIoRemove(const char *file)
{
    return unlink(file) == 0 ? 0 : sce_errno();
}

static void date_from_time(SceDateTime *out, time_t t, long nsec)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    out->year = (unsigned short)(tm.tm_year + 1900);
    out->month = (unsigned short)(tm.tm_mon + 1);
    out->day = (unsigned short)tm.tm_mday;
    out->hour = (unsigned short)tm.tm_hour;
    out->minute = (unsigned short)tm.tm_min;
    out->second = (unsigned short)tm.tm_sec;
    out->microsecond = (unsigned int)(nsec / 1000);
}

static void stat_from_posix(SceIoStat *out, const struct stat *st)
{
    memset(out, 0, sizeof *out);
    out->st_mode = (SceMode)((st->st_mode & 0777) |
                             (S_ISDIR(st->st_mode) ? SCE_S_IFDIR :
                              S_ISLNK(st->st_mode) ? SCE_S_IFLNK :
                                                     SCE_S_IFREG));
    out->st_size = (SceOff)st->st_size;
    date_from_time(&out->st_ctime, st->st_ctim.tv_sec, st->st_ctim.tv_nsec);
    date_from_time(&out->st_atime, st->st_atim.tv_sec, st->st_atim.tv_nsec);
    date_from_time(&out->st_mtime, st->st_mtim.tv_sec, st->st_mtim.tv_nsec);
}

int sceIoGetstat(const char *file, SceIoStat *out)
{
    struct stat st;
    if (stat(file, &st) != 0)
        return sce_errno();
    stat_from_posix(out, &st);
    return 0;
}

int sceIoGetstatByFd(SceUID fd, SceIoStat *out)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return sce_errno();
    stat_from_posix(out, &st);
    return 0;
}

/* Directory handles: small table, ids offset so they never look like fds. */
#define DIR_SLOTS 64
#define DIR_ID_BASE 0x40000000
static struct {
    DIR *dir;
    int dirfd_valid;
    char path[512];
} s_dirs[DIR_SLOTS];
static pthread_mutex_t s_dir_lock = PTHREAD_MUTEX_INITIALIZER;

SceUID sceIoDopen(const char *dirname)
{
    DIR *d = opendir(dirname);
    int i;
    if (!d)
        return sce_errno();
    pthread_mutex_lock(&s_dir_lock);
    for (i = 0; i < DIR_SLOTS; i++) {
        if (!s_dirs[i].dir) {
            s_dirs[i].dir = d;
            snprintf(s_dirs[i].path, sizeof s_dirs[i].path, "%s", dirname);
            pthread_mutex_unlock(&s_dir_lock);
            return DIR_ID_BASE + i;
        }
    }
    pthread_mutex_unlock(&s_dir_lock);
    closedir(d);
    return (int)0x80010018; /* EMFILE */
}

static DIR *dir_of(SceUID id)
{
    int i = id - DIR_ID_BASE;
    return (i >= 0 && i < DIR_SLOTS) ? s_dirs[i].dir : NULL;
}

int sceIoDread(SceUID fd, SceIoDirent *out)
{
    DIR *d = dir_of(fd);
    struct dirent *e;
    struct stat st;
    if (!d)
        return (int)0x80010009; /* EBADF */
    do {
        errno = 0;
        e = readdir(d);
        if (!e)
            return errno ? sce_errno() : 0;
    } while (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."));
    memset(out, 0, sizeof *out);
    snprintf(out->d_name, sizeof out->d_name, "%s", e->d_name);
#if defined(__SWITCH__)
    {
        /* newlib has no fstatat/dirfd. */
        char full[1024];
        snprintf(full, sizeof full, "%s/%s",
                 s_dirs[fd - DIR_ID_BASE].path, e->d_name);
        if (stat(full, &st) == 0)
            stat_from_posix(&out->d_stat, &st);
    }
#else
    if (fstatat(dirfd(d), e->d_name, &st, 0) == 0)
        stat_from_posix(&out->d_stat, &st);
#endif
    return 1;
}

int sceIoDclose(SceUID fd)
{
    DIR *d = dir_of(fd);
    if (!d)
        return (int)0x80010009;
    pthread_mutex_lock(&s_dir_lock);
    s_dirs[fd - DIR_ID_BASE].dir = NULL;
    pthread_mutex_unlock(&s_dir_lock);
    closedir(d);
    return 0;
}

/* ---------------------------------------------------------------- clib */

int sceClibPrintf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------ process/thread */

void isaac_vita_crt_async_write_shutdown(void) __attribute__((weak));

int sceKernelExitProcess(int res)
{
    /* Queued save images must reach the card before the process ends. */
    if (isaac_vita_crt_async_write_shutdown)
        isaac_vita_crt_async_write_shutdown();
    fflush(NULL);
    _exit(res);
}

SceUID sceKernelGetProcessId(void) { return (SceUID)getpid(); }

int sceKernelGetThreadId(void) { return (int)isaac_host_tid(); }

int sceKernelDelayThread(SceUInt delay)
{
    struct timespec ts;
    ts.tv_sec = delay / 1000000u;
    ts.tv_nsec = (long)(delay % 1000000u) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    return 0;
}

SceUInt64 sceKernelGetProcessTimeWide(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (SceUInt64)ts.tv_sec * 1000000u + (SceUInt64)ts.tv_nsec / 1000u;
}

/* Event flags: one mutex + condvar each. */
#define EVF_SLOTS 256
#define EVF_ID_BASE 0x50000000
typedef struct evf {
    int used;
    unsigned int bits;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} evf;
static evf s_evf[EVF_SLOTS];
static pthread_mutex_t s_evf_table = PTHREAD_MUTEX_INITIALIZER;

static evf *evf_of(int id)
{
    int i = id - EVF_ID_BASE;
    return (i >= 0 && i < EVF_SLOTS && s_evf[i].used) ? &s_evf[i] : NULL;
}

/* ---------------------------------------------- semaphores and threads */

#define SEMA_SLOTS 16
#define SEMA_ID_BASE 0x51000000
#define THREAD_SLOTS 16
#define THREAD_ID_BASE 0x52000000

typedef struct {
    int used;
    int count, max;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} sema;

typedef struct {
    int used, started;
    SceKernelThreadEntry entry;
    SceSize stack_size, arglen;
    void *argp;
    int status;
    pthread_t thread;
} kthread;

static pthread_mutex_t s_obj_table = PTHREAD_MUTEX_INITIALIZER;
static sema s_sema[SEMA_SLOTS];
static kthread s_thread[THREAD_SLOTS];

static sema *sema_of(SceUID id)
{
    int i = id - SEMA_ID_BASE;
    return (i >= 0 && i < SEMA_SLOTS && s_sema[i].used) ? &s_sema[i] : NULL;
}

static kthread *thread_of(SceUID id)
{
    int i = id - THREAD_ID_BASE;
    return (i >= 0 && i < THREAD_SLOTS && s_thread[i].used) ? &s_thread[i]
                                                           : NULL;
}

SceUID sceKernelCreateSema(const char *name, SceUInt attr, int init_val,
                           int max_val, void *opt)
{
    int i;
    (void)name; (void)attr; (void)opt;
    pthread_mutex_lock(&s_obj_table);
    for (i = 0; i < SEMA_SLOTS; i++) {
        if (!s_sema[i].used) {
            s_sema[i].used = 1;
            s_sema[i].count = init_val;
            s_sema[i].max = max_val;
            pthread_mutex_init(&s_sema[i].lock, NULL);
            pthread_cond_init(&s_sema[i].cond, NULL);
            pthread_mutex_unlock(&s_obj_table);
            return SEMA_ID_BASE + i;
        }
    }
    pthread_mutex_unlock(&s_obj_table);
    return (int)0x8002801a;
}

int sceKernelDeleteSema(SceUID id)
{
    sema *e = sema_of(id);
    if (!e)
        return (int)0x800281a0;
    pthread_mutex_lock(&s_obj_table);
    pthread_cond_destroy(&e->cond);
    pthread_mutex_destroy(&e->lock);
    e->used = 0;
    pthread_mutex_unlock(&s_obj_table);
    return 0;
}

int sceKernelSignalSema(SceUID id, int signal)
{
    sema *e = sema_of(id);
    if (!e)
        return (int)0x800281a0;
    pthread_mutex_lock(&e->lock);
    e->count += signal;
    if (e->max > 0 && e->count > e->max)
        e->count = e->max;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    return 0;
}

int sceKernelWaitSema(SceUID id, int signal, SceUInt *timeout)
{
    sema *e = sema_of(id);
    (void)timeout; /* the save writer waits without one */
    if (!e)
        return (int)0x800281a0;
    pthread_mutex_lock(&e->lock);
    while (e->count < signal)
        pthread_cond_wait(&e->cond, &e->lock);
    e->count -= signal;
    pthread_mutex_unlock(&e->lock);
    return 0;
}

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry,
                             int init_priority, SceSize stack_size,
                             SceUInt attr, int cpu_affinity_mask,
                             const void *opt)
{
    int i;
    (void)name; (void)init_priority; (void)attr; (void)cpu_affinity_mask;
    (void)opt;
    if (!entry)
        return (int)0x80020000;
    pthread_mutex_lock(&s_obj_table);
    for (i = 0; i < THREAD_SLOTS; i++) {
        if (!s_thread[i].used) {
            memset(&s_thread[i], 0, sizeof s_thread[i]);
            s_thread[i].used = 1;
            s_thread[i].entry = entry;
            s_thread[i].stack_size = stack_size;
            pthread_mutex_unlock(&s_obj_table);
            return THREAD_ID_BASE + i;
        }
    }
    pthread_mutex_unlock(&s_obj_table);
    return (int)0x8002801a;
}

static void *kthread_main(void *arg)
{
    kthread *t = arg;
    t->status = t->entry(t->arglen, t->argp);
    return NULL;
}

int sceKernelStartThread(SceUID id, SceSize arglen, void *argp)
{
    kthread *t = thread_of(id);
    pthread_attr_t attr;
    int rc;
    if (!t || t->started)
        return (int)0x800201a0;
    t->arglen = arglen;
    t->argp = argp;
    pthread_attr_init(&attr);
    if (t->stack_size)
        pthread_attr_setstacksize(&attr, t->stack_size < 64u * 1024u
                                             ? 64u * 1024u : t->stack_size);
    rc = pthread_create(&t->thread, &attr, kthread_main, t);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return (int)0x80020000;
    t->started = 1;
    return 0;
}

int sceKernelWaitThreadEnd(SceUID id, int *stat, SceUInt *timeout)
{
    kthread *t = thread_of(id);
    (void)timeout;
    if (!t || !t->started)
        return (int)0x800201a0;
    pthread_join(t->thread, NULL);
    t->started = 0;
    if (stat)
        *stat = t->status;
    return 0;
}

int sceKernelDeleteThread(SceUID id)
{
    kthread *t = thread_of(id);
    if (!t)
        return (int)0x800201a0;
    if (t->started)
        return (int)0x800201a0; /* still running */
    t->used = 0;
    return 0;
}

SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *opt)
{
    int i;
    (void)name; (void)attr; (void)opt;
    pthread_mutex_lock(&s_evf_table);
    for (i = 0; i < EVF_SLOTS; i++) {
        if (!s_evf[i].used) {
            s_evf[i].used = 1;
            s_evf[i].bits = (unsigned int)bits;
            pthread_mutex_init(&s_evf[i].lock, NULL);
            pthread_cond_init(&s_evf[i].cond, NULL);
            pthread_mutex_unlock(&s_evf_table);
            return EVF_ID_BASE + i;
        }
    }
    pthread_mutex_unlock(&s_evf_table);
    return (int)0x8002801a; /* no free event flag */
}

int sceKernelDeleteEventFlag(int id)
{
    evf *e = evf_of(id);
    if (!e)
        return (int)0x800281a0;
    pthread_mutex_lock(&s_evf_table);
    pthread_cond_destroy(&e->cond);
    pthread_mutex_destroy(&e->lock);
    e->used = 0;
    pthread_mutex_unlock(&s_evf_table);
    return 0;
}

int sceKernelSetEventFlag(SceUID id, unsigned int bits)
{
    evf *e = evf_of(id);
    if (!e)
        return (int)0x800281a0;
    pthread_mutex_lock(&e->lock);
    e->bits |= bits;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    return 0;
}

int sceKernelClearEventFlag(SceUID id, unsigned int bits)
{
    /* Vita semantics: the flag pattern is ANDed with `bits`. */
    evf *e = evf_of(id);
    if (!e)
        return (int)0x800281a0;
    pthread_mutex_lock(&e->lock);
    e->bits &= bits;
    pthread_mutex_unlock(&e->lock);
    return 0;
}

int sceKernelWaitEventFlag(int id, unsigned int bits, unsigned int wait,
                           unsigned int *out_bits, SceUInt *timeout)
{
    evf *e = evf_of(id);
    struct timespec deadline;
    int rc = 0;
    if (!e)
        return (int)0x800281a0;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += *timeout / 1000000u;
        deadline.tv_nsec += (long)(*timeout % 1000000u) * 1000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&e->lock);
    for (;;) {
        int ok = (wait & SCE_EVENT_WAITOR) ? (e->bits & bits) != 0
                                           : (e->bits & bits) == bits;
        if (ok)
            break;
        if (timeout) {
            if (pthread_cond_timedwait(&e->cond, &e->lock, &deadline) ==
                ETIMEDOUT) {
                rc = (int)0x80028005; /* wait timeout */
                break;
            }
        } else {
            pthread_cond_wait(&e->cond, &e->lock);
        }
    }
    if (out_bits)
        *out_bits = e->bits;
    if (rc == 0) {
        if (wait & SCE_EVENT_WAITCLEAR_PAT)
            e->bits &= ~bits;
        else if (wait & SCE_EVENT_WAITCLEAR)
            e->bits = 0;
    }
    pthread_mutex_unlock(&e->lock);
    return rc;
}

/* -------------------------------------------------------------- sysmem */

/* Memory blocks come from the guest window heap, so anything the runtime
 * carves out of one is guest-addressable, exactly as on the Vita. */
#define MEMBLOCK_SLOTS 128
#define MEMBLOCK_ID_BASE 0x60000000
static struct {
    void *base;
    SceSize size;
    SceKernelMemBlockType type;
} s_blocks[MEMBLOCK_SLOTS];
static pthread_mutex_t s_block_lock = PTHREAD_MUTEX_INITIALIZER;

SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size, SceKernelAllocMemBlockOpt *opt)
{
    int i;
    void *p;
    (void)name; (void)opt;
    p = guest_window_memalign(4096u, size);
    if (!p)
        return (int)0x80020002; /* no memory */
    memset(p, 0, size);
    pthread_mutex_lock(&s_block_lock);
    for (i = 0; i < MEMBLOCK_SLOTS; i++) {
        if (!s_blocks[i].base) {
            s_blocks[i].base = p;
            s_blocks[i].size = size;
            s_blocks[i].type = type;
            pthread_mutex_unlock(&s_block_lock);
            return MEMBLOCK_ID_BASE + i;
        }
    }
    pthread_mutex_unlock(&s_block_lock);
    guest_window_free(p);
    return (int)0x80020002;
}

int sceKernelFreeMemBlock(SceUID uid)
{
    int i = uid - MEMBLOCK_ID_BASE;
    void *p;
    if (i < 0 || i >= MEMBLOCK_SLOTS || !s_blocks[i].base)
        return (int)0x80020001;
    pthread_mutex_lock(&s_block_lock);
    p = s_blocks[i].base;
    s_blocks[i].base = NULL;
    pthread_mutex_unlock(&s_block_lock);
    guest_window_free(p);
    return 0;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    int i = uid - MEMBLOCK_ID_BASE;
    if (i < 0 || i >= MEMBLOCK_SLOTS || !s_blocks[i].base)
        return (int)0x80020001;
    *base = s_blocks[i].base;
    return 0;
}

int sceKernelGetMemBlockInfoByAddr(void *addr, SceKernelMemBlockInfo *info)
{
    int i;
    uintptr_t a = (uintptr_t)addr;
    for (i = 0; i < MEMBLOCK_SLOTS; i++) {
        uintptr_t b = (uintptr_t)s_blocks[i].base;
        if (b && a >= b && a - b < s_blocks[i].size) {
            memset(info, 0, sizeof *info);
            info->size = sizeof *info;
            info->mappedBase = s_blocks[i].base;
            info->mappedSize = s_blocks[i].size;
            info->access = SCE_KERNEL_MEMORY_ACCESS_R |
                           SCE_KERNEL_MEMORY_ACCESS_W;
            info->type = s_blocks[i].type;
            return 0;
        }
    }
    /* Anything else readable/writable in the window counts as USER_RW. */
    if (guest_window_contains(addr, 1)) {
        memset(info, 0, sizeof *info);
        info->size = sizeof *info;
        info->mappedBase = addr;
        info->mappedSize = 1;
        info->access = SCE_KERNEL_MEMORY_ACCESS_R | SCE_KERNEL_MEMORY_ACCESS_W;
        info->type = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
        return 0;
    }
    return (int)0x80020001;
}

/* ----------------------------------------------------------------- rtc */

/* Ticks are microseconds since 0001-01-01 00:00:00 UTC, as on the Vita. */
#define RTC_UNIX_EPOCH_TICK UINT64_C(62135596800000000)

unsigned int sceRtcGetTickResolution(void) { return 1000000u; }

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    /* Howard Hinnant's algorithm; days since 1970-01-01. */
    y -= m <= 2;
    {
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
        const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
        return era * 146097 + (int64_t)doe - 719468;
    }
}

int sceRtcGetTick(const SceDateTime *t, SceRtcTick *tick)
{
    int64_t days = days_from_civil(t->year, t->month, t->day);
    int64_t secs = days * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
    tick->tick = RTC_UNIX_EPOCH_TICK + (SceUInt64)(secs * 1000000) +
                 t->microsecond;
    return 0;
}

int sceRtcSetTick(SceDateTime *t, const SceRtcTick *tick)
{
    int64_t us = (int64_t)(tick->tick - RTC_UNIX_EPOCH_TICK);
    int64_t secs = us >= 0 ? us / 1000000 : -((-us + 999999) / 1000000);
    date_from_time(t, (time_t)secs, (long)((us - secs * 1000000) * 1000));
    return 0;
}

int sceRtcSetTime64_t(SceDateTime *t, SceUInt64 unix_seconds)
{
    date_from_time(t, (time_t)unix_seconds, 0);
    return 0;
}

int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local)
{
    time_t secs = (time_t)((utc->tick - RTC_UNIX_EPOCH_TICK) / 1000000u);
#if defined(__SWITCH__)
    TimeCalendarTime calendar;
    TimeCalendarAdditionalInfo info;
    int64_t offset = 0;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule((u64)secs, &calendar,
                                                 &info)))
        offset = info.offset;
    local->tick = utc->tick + (SceUInt64)(offset * 1000000);
#else
    struct tm tm;
    localtime_r(&secs, &tm);
    local->tick = utc->tick + (SceUInt64)((int64_t)tm.tm_gmtoff * 1000000);
#endif
    return 0;
}
