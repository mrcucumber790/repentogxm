/* Sampling profiler for the game thread (diagnostic, Switch only).
 *
 * A thread on another core pauses the game thread about once a millisecond,
 * reads its PC and LR (svcSetThreadActivity + svcGetThreadContext3 on a
 * thread of our own process) and puts them in a ring with a timestamp.  At
 * the end of each main loop iteration the SDL backend hands over the frame's
 * time window: samples of slow frames, and of every 8th ordinary frame, are
 * appended to sdmc:/switch/isaac/samples.bin, which is symbolized offline
 * against isaac.elf.  The file starts with the run-time address of main so
 * the PCs can be rebased. */
#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"

#define SAMPLER_PATH "sdmc:/switch/isaac/samples.bin"
#define SAMPLER_RING 4096u           /* power of two, ~4 s at 1 kHz */
#define SAMPLER_MAGIC 0x31504D53u    /* "SMP1" */

typedef struct {
    uint64_t tick;
    uint64_t pc;
    uint64_t lr;
} sampler_entry;

typedef struct {
    uint64_t pc;
    uint64_t lr;
    uint32_t frame;      /* frame index */
    uint32_t frame_us;   /* that frame's length */
} sampler_record;

int main(int argc, char **argv);

static Handle s_target;
static Thread s_thread;
static volatile int s_running;
static sampler_entry s_ring[SAMPLER_RING];
static volatile uint32_t s_head;   /* next slot to write (sampler thread) */
static uint32_t s_tail;            /* first slot not yet consumed */
static FILE *s_out;

static void sampler_main(void *arg)
{
    (void)arg;
    while (s_running) {
        ThreadContext ctx;
        svcSleepThread(1000000ull);
        if (R_FAILED(svcSetThreadActivity(s_target, ThreadActivity_Paused)))
            continue;
        if (R_SUCCEEDED(svcGetThreadContext3(&ctx, s_target))) {
            uint32_t slot = s_head & (SAMPLER_RING - 1u);
            s_ring[slot].tick = armGetSystemTick();
            s_ring[slot].pc = ctx.pc.x;
            s_ring[slot].lr = ctx.lr;
            __atomic_store_n(&s_head, s_head + 1u, __ATOMIC_RELEASE);
        }
        svcSetThreadActivity(s_target, ThreadActivity_Runnable);
    }
}

/* Called on the game thread itself. */
void switch_sampler_start(void)
{
    sampler_record header;

    s_out = fopen(SAMPLER_PATH, "wb");
    if (!s_out)
        return;
    setvbuf(s_out, NULL, _IOFBF, 64u * 1024u);
    memset(&header, 0, sizeof header);
    header.pc = (uint64_t)(uintptr_t)&main;
    header.frame = SAMPLER_MAGIC;
    header.frame_us = (uint32_t)armGetSystemTickFreq();
    fwrite(&header, sizeof header, 1, s_out);
    fflush(s_out);

    s_target = threadGetCurHandle();
    s_running = 1;
    /* Higher priority than the game (0x2C), on core 1. */
    if (R_FAILED(threadCreate(&s_thread, sampler_main, NULL, NULL, 0x4000,
                              0x20, 1)) ||
        R_FAILED(threadStart(&s_thread))) {
        s_running = 0;
        isaac_vita_log("isaac-switch: sampler thread failed to start");
        return;
    }
    isaac_vita_log("isaac-switch: sampler on, main=%p", (void *)&main);
}

/* One main loop iteration spanned ticks [begin, end]. */
void switch_sampler_frame(uint64_t begin, uint64_t end, int slow)
{
    static uint32_t frame;
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    uint32_t frame_us;
    int keep;

    ++frame;
    if (!s_running || !s_out)
        return;
    if (head - s_tail > SAMPLER_RING)
        s_tail = head - SAMPLER_RING;
    keep = slow || (frame & 7u) == 0u;
    frame_us = (uint32_t)armTicksToNs(end - begin) / 1000u;
    for (; s_tail != head; ++s_tail) {
        const sampler_entry *e = &s_ring[s_tail & (SAMPLER_RING - 1u)];
        sampler_record r;

        if (e->tick < begin || !keep)
            continue;
        if (e->tick > end)
            break;
        r.pc = e->pc;
        r.lr = e->lr;
        r.frame = frame;
        r.frame_us = frame_us;
        fwrite(&r, sizeof r, 1, s_out);
    }
}

void switch_sampler_flush(void)
{
    if (s_out)
        fflush(s_out);
}
