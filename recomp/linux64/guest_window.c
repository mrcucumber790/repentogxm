/* The 4 GiB guest window for 64-bit hosts (GUEST_MEM_WINDOW=1).
 *
 * Every guest address is an offset into this window.  The window base is
 * aligned to 4 GiB, so the low 32 bits of any host pointer inside the window
 * ARE its guest address: the shared runtime's existing
 * `(uint32_t)(uintptr_t)host_pointer` spellings stay correct as long as the
 * memory came from here.  The reverse direction (guest address -> host
 * pointer) must go through GUEST_HOST(); a missed spot dereferences a low
 * address outside the window and faults loudly instead of corrupting memory.
 *
 * Layout (guest addresses):
 *   0x00000000 - 0x0000ffff  never mapped (NULL guard)
 *   0x00010000 - 0x8fffffff  guest heap: one dlmalloc mspace
 *   0x98000000 - ...         the PE image (GUEST_IMAGE_BASE), mapped on demand
 *   everything else          reserved, inaccessible
 *
 * The guest heap also backs EVERY malloc of the process: this file defines
 * malloc/free/... for the whole executable (glibc, GL and audio libraries
 * included).  That mirrors the Vita, where the guest and every native library
 * share one 32-bit address space, and it makes any heap pointer the runtime
 * hands to the guest a valid guest address without touching the shared code.
 * What remains outside the window are host stacks and the executable's own
 * .data/.bss; those must never be handed to the guest. */
#include "guest_window.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__SWITCH__)
#include <sys/mman.h>
#endif
#include <unistd.h>

#define WINDOW_SIZE      (UINT64_C(1) << 32)
#define HEAP_GUEST_START UINT32_C(0x00010000)
#define HEAP_GUEST_END   UINT32_C(0x90000000)

/* dlmalloc, compiled as mspaces only, in this translation unit. */
#define ONLY_MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define MSPACES 1
#define USE_DL_PREFIX 1
#define DLMALLOC_EXPORT static
#define MALLOC_ALIGNMENT 16
#define FOOTERS 0
#define INSECURE 0
#define ABORT_ON_ASSERT_FAILURE 1
#if defined(__SWITCH__)
#define malloc_getpagesize ((size_t)4096U)
#endif
#include "third_party/dlmalloc.c"

/* Arena layout (guest_window_init_arena): one caller-provided read/write
 * block holds the image reserve at GUEST_IMAGE_BASE and the heap above it.
 * Guest addresses outside it are not backed.  Used where the host cannot
 * reserve address space cheaply (Switch: the libnx heap). */
#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x98000000u
#endif
#define ARENA_IMAGE_RESERVE UINT32_C(0x01000000)
#define ARENA_GUEST_END     UINT32_C(0xf0000000)

uintptr_t g_guest_mem_base;
static mspace s_heap;
static uint32_t s_fixed_start = HEAP_GUEST_END;
static uint32_t s_fixed_end = UINT32_C(0xffffffff);
static int s_arena;

static void window_die(const char *message)
{
    ssize_t ignored = write(2, message, strlen(message));
    (void)ignored;
    abort();
}

static inline void window_ready(void)
{
    if (__builtin_expect(s_heap == NULL, 0))
        (void)guest_window_init();
}

#if defined(__SWITCH__)
/* libnx hands the whole process heap to newlib; this file replaces newlib's
 * allocator, so the heap becomes the guest arena. */
extern char *fake_heap_start;
extern char *fake_heap_end;

int guest_window_init(void)
{
    if (g_guest_mem_base)
        return 0;
    return guest_window_init_arena(fake_heap_start,
                                   (size_t)(fake_heap_end - fake_heap_start));
}
#else
int guest_window_init(void)
{
    uintptr_t want;
    void *window = MAP_FAILED;
    void *heap;
    const char *arena_mb;

    if (g_guest_mem_base)
        return 0;
    /* Test hook for the Switch layout on Linux: an unaligned arena. */
    arena_mb = getenv("ISAAC_WINDOW_ARENA_MB");
    if (arena_mb && atoi(arena_mb) > 0) {
        size_t size = (size_t)atoi(arena_mb) << 20;
        void *arena = mmap(NULL, size + 0x1000u, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                           -1, 0);
        if (arena == MAP_FAILED) {
            window_die("guest window: cannot map arena\n");
            return -1;
        }
        /* Deliberately not 4 GiB aligned. */
        return guest_window_init_arena((char *)arena + 0x10, size);
    }

    /* Prefer a fixed 4 GiB-aligned spot; fall back to over-reserving and
     * trimming to an aligned 4 GiB. */
    for (want = UINT64_C(0x200000000); want <= UINT64_C(0x7f0000000000) &&
             window == MAP_FAILED; want += UINT64_C(0x1000000000)) {
        window = mmap((void *)want, WINDOW_SIZE, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                      MAP_FIXED_NOREPLACE, -1, 0);
        if (window != MAP_FAILED && window != (void *)want) {
            munmap(window, WINDOW_SIZE);
            window = MAP_FAILED;
        }
    }
    if (window == MAP_FAILED) {
        void *big = mmap(NULL, 2 * WINDOW_SIZE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        uintptr_t lo, aligned;
        if (big == MAP_FAILED) {
            window_die("guest window: cannot reserve 8 GiB\n");
            return -1;
        }
        lo = (uintptr_t)big;
        aligned = (lo + WINDOW_SIZE - 1u) & ~(uintptr_t)(WINDOW_SIZE - 1u);
        if (aligned > lo)
            munmap(big, aligned - lo);
        munmap((void *)(aligned + WINDOW_SIZE),
               lo + 2 * WINDOW_SIZE - (aligned + WINDOW_SIZE));
        window = (void *)aligned;
    }
    g_guest_mem_base = (uintptr_t)window;

    heap = (void *)(g_guest_mem_base + HEAP_GUEST_START);
    if (mprotect(heap, HEAP_GUEST_END - HEAP_GUEST_START,
                 PROT_READ | PROT_WRITE) != 0) {
        window_die("guest window: heap mprotect failed\n");
        return -1;
    }
    s_heap = create_mspace_with_base(heap, HEAP_GUEST_END - HEAP_GUEST_START,
                                     1);
    if (!s_heap) {
        window_die("guest window: mspace creation failed\n");
        return -1;
    }
    return 0;
}
#endif

int guest_window_init_arena(void *arena, size_t size)
{
    uintptr_t start = ((uintptr_t)arena + 0xfffu) & ~(uintptr_t)0xfffu;
    uint64_t usable;
    uint32_t heap_start = GUEST_IMAGE_BASE + ARENA_IMAGE_RESERVE;

    if (g_guest_mem_base)
        return 0;
    if (!arena || start - (uintptr_t)arena >= size ||
            start < (uintptr_t)GUEST_IMAGE_BASE) {
        window_die("guest window: unusable arena\n");
        return -1;
    }
    usable = size - (start - (uintptr_t)arena);
    if (usable > (uint64_t)(ARENA_GUEST_END - GUEST_IMAGE_BASE))
        usable = ARENA_GUEST_END - GUEST_IMAGE_BASE;
    if (usable < (uint64_t)ARENA_IMAGE_RESERVE + UINT64_C(0x04000000)) {
        window_die("guest window: arena too small\n");
        return -1;
    }
    g_guest_mem_base = start - GUEST_IMAGE_BASE;
    s_fixed_start = GUEST_IMAGE_BASE;
    s_fixed_end = heap_start;
    s_arena = 1;
    s_heap = create_mspace_with_base(GUEST_HOST(heap_start),
                                     (size_t)(usable - ARENA_IMAGE_RESERVE),
                                     1);
    if (!s_heap) {
        window_die("guest window: mspace creation failed\n");
        return -1;
    }
    return 0;
}

int guest_window_contains(const void *p, size_t size)
{
    uintptr_t a = (uintptr_t)p;
    return g_guest_mem_base && a >= g_guest_mem_base &&
           size <= WINDOW_SIZE && a - g_guest_mem_base <= WINDOW_SIZE - size;
}

void *guest_window_map_fixed(uint32_t address, uint32_t size)
{
    void *p = GUEST_HOST(address);
    if (!g_guest_mem_base || address < s_fixed_start ||
        (uint64_t)address + size > s_fixed_end)
        return NULL;
#if !defined(__SWITCH__)
    if (!s_arena && mprotect(p, size, PROT_READ | PROT_WRITE) != 0)
        return NULL;
#endif
    memset(p, 0, size);
    return p;
}

void guest_window_unmap_fixed(void *p, uint32_t size)
{
    if (s_arena)
        return;
#if !defined(__SWITCH__)
    if (p && guest_window_contains(p, size)) {
        madvise(p, size, MADV_DONTNEED);
        mprotect(p, size, PROT_NONE);
    }
#endif
}

void *guest_window_malloc(size_t size)
{
    window_ready();
    return mspace_malloc(s_heap, size);
}
void guest_window_free(void *p)
{
    if (p)
        mspace_free(s_heap, p);
}
void *guest_window_calloc(size_t n, size_t size)
{
    window_ready();
    return mspace_calloc(s_heap, n, size);
}
void *guest_window_realloc(void *p, size_t size)
{
    window_ready();
    return mspace_realloc(s_heap, p, size);
}
void *guest_window_memalign(size_t alignment, size_t size)
{
    window_ready();
    if (alignment <= MALLOC_ALIGNMENT)
        return mspace_malloc(s_heap, size);
    return mspace_memalign(s_heap, alignment, size);
}
size_t guest_window_usable_size(const void *p)
{
    return p ? mspace_usable_size(p) : 0u;
}
size_t guest_window_heap_in_use(void)
{
    struct mallinfo info;
    window_ready();
    info = mspace_mallinfo(s_heap);
    return (size_t)info.uordblks;
}

/* ---- process-wide allocator ---- */
void *malloc(size_t size) { return guest_window_malloc(size); }
void free(void *p) { guest_window_free(p); }
void *calloc(size_t n, size_t size) { return guest_window_calloc(n, size); }
void *realloc(void *p, size_t size) { return guest_window_realloc(p, size); }
void *memalign(size_t alignment, size_t size)
{
    return guest_window_memalign(alignment, size);
}
void *aligned_alloc(size_t alignment, size_t size)
{
    return guest_window_memalign(alignment, size);
}
int posix_memalign(void **out, size_t alignment, size_t size)
{
    void *p;
    if (alignment < sizeof(void *) || (alignment & (alignment - 1u)) != 0u)
        return EINVAL;
    p = guest_window_memalign(alignment, size);
    if (!p && size)
        return ENOMEM;
    *out = p;
    return 0;
}
void *valloc(size_t size) { return guest_window_memalign(4096u, size); }
void *pvalloc(size_t size)
{
    return guest_window_memalign(4096u, (size + 4095u) & ~(size_t)4095u);
}
size_t malloc_usable_size(void *p) { return guest_window_usable_size(p); }

