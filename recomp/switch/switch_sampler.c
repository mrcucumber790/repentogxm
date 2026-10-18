/* Sampling profiler for the game thread (diagnostic, Switch only).
 *
 * A thread on another core pauses the game thread about once a millisecond,
 * reads its PC, LR and frame pointer chain (svcSetThreadActivity +
 * svcGetThreadContext3 on a thread of our own process) and puts them in a
 * ring with a timestamp.  At each main loop iteration the SDL backend hands
 * over the frame's time window; the sampler thread appends the samples of
 * slow frames, and of every 8th ordinary frame, to
 * sdmc:/switch/isaac/samples.bin, which is symbolized offline against
 * isaac.elf.  All file writes happen on the sampler thread so the profiler
 * itself does not stall frames.  The file starts with the run-time address
 * of main so the PCs can be rebased. */
#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"

#define SAMPLER_PATH "sdmc:/switch/isaac/samples.bin"
#define SAMPLER_RING 4096u           /* power of two, ~4 s at 1 kHz */
#define SAMPLER_FRAMES 256u          /* power of two, pending frame windows */
#define SAMPLER_DEPTH 6u             /* return addresses from the fp chain */
#define SAMPLER_MAGIC 0x32504D53u    /* "SMP2" */
#define SAMPLER_STACK_SPAN (8u * 1024u * 1024u)

typedef struct {
    uint64_t tick;
    uint64_t pc;
    uint64_t lr;
    uint64_t stack[SAMPLER_DEPTH];
} sampler_entry;

typedef struct {
    uint64_t pc;
    uint64_t lr;
    uint32_t frame;      /* frame index */
    uint32_t frame_us;   /* that frame's length */
    uint64_t stack[SAMPLER_DEPTH];
} sampler_record;

typedef struct {
    uint64_t begin;
    uint64_t end;
    uint32_t frame;
    uint32_t frame_us;
} sampler_window;

int main(int argc, char **argv);

static Handle s_target;
static Thread s_thread;
static volatile int s_running;
static sampler_entry s_ring[SAMPLER_RING];
static uint32_t s_head;            /* sampler thread only */
static uint32_t s_tail;
static sampler_window s_windows[SAMPLER_FRAMES];
static volatile uint32_t s_win_head;   /* written by the game thread */
static uint32_t s_win_tail;            /* sampler thread */
static FILE *s_out;

/* Walk the AArch64 frame records {prev fp, return address} on the paused
 * thread's stack, which belongs to this process. */
static void sampler_unwind(const ThreadContext *ctx, uint64_t *stack)
{
    uint64_t sp = ctx->sp;
    uint64_t fp = ctx->fp;
    unsigned i;

    for (i = 0; i < SAMPLER_DEPTH; ++i)
        stack[i] = 0;
    for (i = 0; i < SAMPLER_DEPTH; ++i) {
        const uint64_t *record;

        if (fp < sp || fp >= sp + SAMPLER_STACK_SPAN || (fp & 7u))
            break;
        record = (const uint64_t *)(uintptr_t)fp;
        stack[i] = record[1];
        if (record[0] <= fp)
            break;
        fp = record[0];
    }
}

static void sampler_write_window(const sampler_window *w)
{
    for (; s_tail != s_head; ++s_tail) {
        const sampler_entry *e = &s_ring[s_tail & (SAMPLER_RING - 1u)];
        sampler_record r;

        if (e->tick < w->begin)
            continue;
        if (e->tick > w->end)
            break;
        r.pc = e->pc;
        r.lr = e->lr;
        r.frame = w->frame;
        r.frame_us = w->frame_us;
        memcpy(r.stack, e->stack, sizeof r.stack);
        fwrite(&r, sizeof r, 1, s_out);
    }
}

static void sampler_drain(void)
{
    uint32_t head = __atomic_load_n(&s_win_head, __ATOMIC_ACQUIRE);

    if (head - s_win_tail > SAMPLER_FRAMES)
        s_win_tail = head - SAMPLER_FRAMES;
    if (s_head - s_tail > SAMPLER_RING)
        s_tail = s_head - SAMPLER_RING;
    for (; s_win_tail != head; ++s_win_tail)
        sampler_write_window(&s_windows[s_win_tail & (SAMPLER_FRAMES - 1u)]);
}

static void sampler_main(void *arg)
{
    unsigned n = 0;

    (void)arg;
    while (s_running) {
        ThreadContext ctx;

        svcSleepThread(1000000ull);
        if (R_SUCCEEDED(svcSetThreadActivity(s_target,
                                             ThreadActivity_Paused))) {
            if (R_SUCCEEDED(svcGetThreadContext3(&ctx, s_target))) {
                sampler_entry *e = &s_ring[s_head & (SAMPLER_RING - 1u)];

                e->tick = armGetSystemTick();
                e->pc = ctx.pc.x;
                e->lr = ctx.lr;
                sampler_unwind(&ctx, e->stack);
                ++s_head;
            }
            svcSetThreadActivity(s_target, ThreadActivity_Runnable);
        }
        if ((++n & 31u) == 0u)
            sampler_drain();
        if ((n & 2047u) == 0u)
            fflush(s_out);
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

/* One main loop iteration spanned ticks [begin, end]; game thread. */
void switch_sampler_frame(uint64_t begin, uint64_t end, int slow)
{
    static uint32_t frame;
    sampler_window *w;
    uint32_t head;

    ++frame;
    if (!s_running || !(slow || (frame & 7u) == 0u))
        return;
    head = s_win_head;
    w = &s_windows[head & (SAMPLER_FRAMES - 1u)];
    w->begin = begin;
    w->end = end;
    w->frame = frame;
    w->frame_us = (uint32_t)(armTicksToNs(end - begin) / 1000u);
    __atomic_store_n(&s_win_head, head + 1u, __ATOMIC_RELEASE);
}
