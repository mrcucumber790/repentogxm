/* Frame-time spike log (ISAAC_FRAME_SPIKE_LOG, off by default).
 *
 * Linked with -Wl,--wrap=SDL_GL_SwapWindow, so it times the interval between
 * two presents without touching the SDL backend.  Every frame longer than
 * ISAAC_FRAME_SPIKE_MS is logged with its length; every 600 frames one
 * summary line gives the frame count, the spike count and the worst frame.
 * Used to compare hitches (tear shots, room transitions) between builds. */
#include "platform.h"

#include <SDL.h>
#include <stdint.h>
#include <time.h>

#ifndef ISAAC_FRAME_SPIKE_MS
#define ISAAC_FRAME_SPIKE_MS 25
#endif

void __real_SDL_GL_SwapWindow(SDL_Window *window);

static uint64_t spike_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void __wrap_SDL_GL_SwapWindow(SDL_Window *window)
{
    static uint64_t last, window_start, worst, total_spike_us;
    static unsigned frames, spikes, frame_index;
    uint64_t now, dt;

    __real_SDL_GL_SwapWindow(window);
    now = spike_now_us();
    if (!last) {
        last = window_start = now;
        return;
    }
    dt = now - last;
    last = now;
    ++frame_index;
    ++frames;
    if (dt > worst)
        worst = dt;
    if (dt >= (uint64_t)ISAAC_FRAME_SPIKE_MS * 1000u) {
        ++spikes;
        total_spike_us += dt;
        isaac_vita_log("frame-spike: frame=%u %.1f ms", frame_index,
                       (double)dt / 1000.0);
    }
    if (frames == 600u) {
        isaac_vita_log("frame-spike: window frames=%u avg=%.2f ms spikes=%u "
                       "spike_total=%.1f ms worst=%.1f ms",
                       frames, (double)(now - window_start) / 1000.0 / frames,
                       spikes, (double)total_spike_us / 1000.0,
                       (double)worst / 1000.0);
        frames = spikes = 0u;
        worst = total_spike_us = 0u;
        window_start = now;
    }
}
