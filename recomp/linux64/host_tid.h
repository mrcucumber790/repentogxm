#ifndef ISAAC_HOST_TID_H
#define ISAAC_HOST_TID_H

#include <stdint.h>

#if defined(__SWITCH__)
#include <switch.h>

static inline uint32_t isaac_host_tid(void)
{
    u64 id = 0;
    (void)svcGetThreadId(&id, CUR_THREAD_HANDLE);
    return (uint32_t)id;
}
#else
#include <sys/syscall.h>
#include <unistd.h>

static inline uint32_t isaac_host_tid(void)
{
    return (uint32_t)syscall(SYS_gettid);
}
#endif

#endif
