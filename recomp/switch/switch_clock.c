/* CPU clock (Switch only).
 *
 * The recompiled game is CPU bound wherever many entities are on screen
 * (Downpour/Dross rain and reflections, room loads).  The stock clock is
 * 1020 MHz; 1785 MHz is the rate the system itself uses in its boost mode,
 * set here through clkrst (pcv before 8.0.0) without the boost mode's GPU
 * downclock.  The original rate comes back at exit.  Create
 * sdmc:/switch/isaac/no-overclock to keep the stock clock. */
#include <switch.h>

#include <stdlib.h>
#include <sys/stat.h>

#include "platform.h"

#define CLOCK_CPU_HZ 1785000000u

static int s_state;          /* 0 untried, 1 active, -1 off/unavailable */
static int s_clkrst;
static ClkrstSession s_session;
static u32 s_original_hz;

static Result clock_get(u32 *hz)
{
    return s_clkrst ? clkrstGetClockRate(&s_session, hz)
                    : pcvGetClockRate(PcvModule_CpuBus, hz);
}

static Result clock_set(u32 hz)
{
    return s_clkrst ? clkrstSetClockRate(&s_session, hz)
                    : pcvSetClockRate(PcvModule_CpuBus, hz);
}

static void clock_restore(void)
{
    if (s_state != 1)
        return;
    if (s_original_hz)
        clock_set(s_original_hz);
    if (s_clkrst) {
        clkrstCloseSession(&s_session);
        clkrstExit();
    } else {
        pcvExit();
    }
    s_state = -1;
}

static int clock_open(void)
{
    struct stat st;

    if (stat("sdmc:/switch/isaac/no-overclock", &st) == 0) {
        isaac_vita_log("isaac-switch: no-overclock present, stock CPU clock");
        return 0;
    }
    if (hosversionAtLeast(8, 0, 0)) {
        if (R_FAILED(clkrstInitialize()))
            return 0;
        if (R_FAILED(clkrstOpenSession(&s_session, PcvModuleId_CpuBus, 3))) {
            clkrstExit();
            return 0;
        }
        s_clkrst = 1;
    } else if (R_FAILED(pcvInitialize())) {
        return 0;
    }
    if (R_FAILED(clock_get(&s_original_hz)))
        s_original_hz = 0;
    atexit(clock_restore);
    return 1;
}

/* Called when the game runs steadily (after the load-time boost mode is
 * dropped) and then now and then: docking or a performance mode change can
 * put the stock rate back. */
void switch_clock_apply(void)
{
    u32 hz = 0;

    if (s_state == 0) {
        s_state = clock_open() ? 1 : -1;
        if (s_state != 1) {
            isaac_vita_log("isaac-switch: CPU clock control unavailable");
            return;
        }
    }
    if (s_state != 1)
        return;
    if (R_SUCCEEDED(clock_get(&hz)) && hz >= CLOCK_CPU_HZ)
        return;
    if (R_FAILED(clock_set(CLOCK_CPU_HZ))) {
        isaac_vita_log("isaac-switch: CPU clock set failed (was %u Hz)",
                       (unsigned)hz);
        clock_restore();
        return;
    }
    clock_get(&hz);
    isaac_vita_log("isaac-switch: CPU clock %u MHz (stock %u MHz)",
                   (unsigned)(hz / 1000000u),
                   (unsigned)(s_original_hz / 1000000u));
}
