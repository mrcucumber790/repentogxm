/* Host platform edge for the 64-bit Linux build: log, clocks and the fixed
 * PE image placement (recomp/vita/platform.c and fixed_image.c on Vita). */
#include "platform.h"
#include "fixed_image.h"
#include "vita_host_services.h"
#include "guest_window.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x98000000u
#endif

#define ISAAC_LOG_PATH ISAAC_VITA_NATIVE_DATA_ROOT "/first-arm-fault.log"

static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_start_us;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void log_write(const char *line, unsigned int size)
{
    FILE *f;
    pthread_mutex_lock(&s_log_lock);
    fwrite(line, 1, size, stderr);
    f = fopen(ISAAC_LOG_PATH, "ab");
    if (f) {
        fwrite(line, 1, size, f);
        fclose(f);
    }
    pthread_mutex_unlock(&s_log_lock);
}

void isaac_vita_log_sink_file(const char *line, unsigned int size)
{
    log_write(line, size);
}

int isaac_vita_get_win32_filetime(uint64_t *filetime)
{
    struct timespec ts;
    if (!filetime)
        return -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* 100 ns intervals since 1601-01-01. */
    *filetime = ((uint64_t)ts.tv_sec + UINT64_C(11644473600)) * 10000000u +
                (uint64_t)ts.tv_nsec / 100u;
    return 0;
}

uint32_t isaac_vita_get_thread_id(void)
{
    return (uint32_t)syscall(SYS_gettid);
}

uint32_t isaac_vita_get_process_id(void) { return (uint32_t)getpid(); }

uint64_t isaac_vita_get_process_time(void) { return now_us(); }

void isaac_vita_log_reset(void)
{
    FILE *f;
    s_start_us = now_us();
    mkdir(ISAAC_VITA_NATIVE_DATA_ROOT, 0777);
    f = fopen(ISAAC_LOG_PATH, "wb");
    if (f)
        fclose(f);
}

void isaac_vita_log(const char *format, ...)
{
    char body[384];
    char line[512];
    va_list args;
    int body_length, line_length;
    uint64_t t = now_us() - s_start_us;

    va_start(args, format);
    body_length = vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (body_length < 0)
        return;
    line_length = snprintf(line, sizeof(line), "[%u.%03u thr 0x%x] %s\n",
                           (unsigned)(t / 1000000u),
                           (unsigned)((t / 1000u) % 1000u),
                           (unsigned)syscall(SYS_gettid), body);
    if (line_length <= 0)
        return;
    if (line_length >= (int)sizeof(line))
        line_length = (int)sizeof(line) - 1;
    log_write(line, (unsigned int)line_length);
}

static void *s_image;
static uint32_t s_image_size;

void *isaac_vita_fixed_alloc(uint32_t address, uint32_t size)
{
    uint32_t mapped;
    if (s_image) {
        isaac_vita_log("fixed image allocation requested twice");
        return NULL;
    }
    if (address != GUEST_IMAGE_BASE) {
        isaac_vita_log("refusing guest base 0x%08x; build expects 0x%08x",
                       (unsigned)address, (unsigned)GUEST_IMAGE_BASE);
        return NULL;
    }
    mapped = (size + 0xfffu) & ~0xfffu;
    s_image = guest_window_map_fixed(address, mapped);
    if (!s_image) {
        isaac_vita_log("guest window: image map at 0x%08x size %u failed",
                       (unsigned)address, (unsigned)mapped);
        return NULL;
    }
    s_image_size = mapped;
    isaac_vita_log("fixed PE image mapped: guest=0x%08x host=%p size=%u",
                   (unsigned)address, s_image, (unsigned)mapped);
    return s_image;
}

void isaac_vita_fixed_free(void *address)
{
    if (!s_image || address != s_image)
        return;
    guest_window_unmap_fixed(s_image, s_image_size);
    s_image = NULL;
}
