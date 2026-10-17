#ifndef ISAAC_GUEST_WINDOW_H
#define ISAAC_GUEST_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#include "guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve the 4 GiB window and create the guest heap.  0 on success. */
int guest_window_init(void);
/* Alternative: use [arena, arena+size) (read/write, any alignment) for the
 * image reserve at GUEST_IMAGE_BASE and the heap above it. */
int guest_window_init_arena(void *arena, size_t size);
int guest_window_contains(const void *p, size_t size);

/* Make [address, address+size) of the window usable (the PE image). */
void *guest_window_map_fixed(uint32_t address, uint32_t size);
void guest_window_unmap_fixed(void *p, uint32_t size);

/* Allocations the guest may see.  Results are inside the window, so their
 * low 32 bits are their guest address. */
void *guest_window_malloc(size_t size);
void guest_window_free(void *p);
void *guest_window_calloc(size_t n, size_t size);
void *guest_window_realloc(void *p, size_t size);
void *guest_window_memalign(size_t alignment, size_t size);
size_t guest_window_usable_size(const void *p);
size_t guest_window_heap_in_use(void);

#ifdef __cplusplus
}
#endif

#endif
