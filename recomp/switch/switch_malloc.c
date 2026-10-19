/* newlib's allocator entry points, routed to the guest window heap
 * (guest_window.c).  newlib calls the reentrant spellings internally; every
 * symbol of its mallocr family that anything may reference is defined here,
 * so none of newlib's allocator objects is linked. */
#include <malloc.h>
#include <reent.h>
#include <string.h>

#include "guest_window.h"

void *_malloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return guest_window_malloc(size);
}

void _free_r(struct _reent *r, void *p)
{
    (void)r;
    guest_window_free(p);
}

void *_calloc_r(struct _reent *r, size_t n, size_t size)
{
    (void)r;
    return guest_window_calloc(n, size);
}

void *_realloc_r(struct _reent *r, void *p, size_t size)
{
    (void)r;
    return guest_window_realloc(p, size);
}

void *_memalign_r(struct _reent *r, size_t alignment, size_t size)
{
    (void)r;
    return guest_window_memalign(alignment, size);
}

void *_valloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return guest_window_memalign(4096u, size);
}

void *_pvalloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return guest_window_memalign(4096u, (size + 4095u) & ~(size_t)4095u);
}

size_t _malloc_usable_size_r(struct _reent *r, void *p)
{
    (void)r;
    return guest_window_usable_size(p);
}

int _malloc_trim_r(struct _reent *r, size_t pad)
{
    (void)r;
    (void)pad;
    return 0;
}

int _mallopt_r(struct _reent *r, int param, int value)
{
    (void)r;
    (void)param;
    (void)value;
    return 0;
}

void _malloc_stats_r(struct _reent *r)
{
    (void)r;
}

struct mallinfo _mallinfo_r(struct _reent *r)
{
    struct mallinfo info;

    (void)r;
    memset(&info, 0, sizeof info);
    info.uordblks = guest_window_heap_in_use();
    return info;
}
