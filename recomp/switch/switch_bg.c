/* Background flusher (Switch only).
 *
 * The port's log and the game's log.txt are fully buffered on Switch (an SD
 * card write stalls for tens of milliseconds).  This low-priority thread
 * writes them out every few seconds so the game thread never waits on the
 * card for them. */
#include <switch.h>

#include "host_vita_crt.h"
#include "platform.h"

#define BG_FLUSH_PERIOD_NS (3000000000ull)

static Thread s_thread;

static void bg_main(void *arg)
{
    (void)arg;
    for (;;) {
        svcSleepThread(BG_FLUSH_PERIOD_NS);
        isaac_vita_log_flush();
        isaac_vita_crt_flush_game_log();
    }
}

void switch_bg_start(void)
{
    /* Below the game thread (0x2C), on core 2. */
    if (R_FAILED(threadCreate(&s_thread, bg_main, NULL, NULL, 0x4000,
                              0x3B, 2)) ||
        R_FAILED(threadStart(&s_thread)))
        isaac_vita_log("isaac-switch: flusher thread failed to start");
}
