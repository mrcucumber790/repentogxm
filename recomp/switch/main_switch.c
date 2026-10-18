/* Entry point of the Nintendo Switch host (homebrew, libnx).
 *
 * Layout on the SD card (see switch_paths.c):
 *
 *   sdmc:/switch/isaac/isaac-ng.exe.unpacked.exe   the unpacked PE (app0:)
 *   sdmc:/switch/isaac/resources/                  the game's resources
 *   sdmc:/switch/isaac/mods/                       Lua mods
 *   sdmc:/switch/isaac/first-arm-fault.log         the port's log
 *
 * Saves and the game's own log.txt live under sdmc:/switch/isaac/Documents.
 * Guest memory is the libnx heap (guest_window.c, arena layout), so this
 * needs the full application memory: launch it as a title override (hold R
 * while starting a game) or as an installed NSP, not from the album. */
#include <switch.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "guest_window.h"
#include "host_vita_crt.h"
#include "platform.h"
#include "switch_paths.h"

#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x98000000u
#endif

int isaac_vita_run_first_fault(const char *pe_path);

extern char *fake_heap_start;
extern char *fake_heap_end;

/* The translated game recurses on the native stack: run it on a thread with
 * a stack at least as large as the Vita main thread's (4 MiB). */
#define GAME_STACK_SIZE (8u * 1024u * 1024u)

static const char *s_pe;
static int s_result;

void switch_sampler_start(void);
void switch_bg_start(void);

static void *game_thread(void *arg)
{
    (void)arg;
    /* Diagnostic: sample this thread's PC into samples.bin. */
    switch_sampler_start();
    s_result = isaac_vita_run_first_fault(s_pe);
    return NULL;
}

static void show_error(const char *message)
{
    PadState pad;

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    printf("The Binding of Isaac (recompiled)\n\n%s\n\n"
           "Expected files under sdmc:/switch/isaac/ (see README).\n"
           "Press + to exit.\n", message);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

int main(int argc, char **argv)
{
    const char *pe = "app0:/isaac-ng.exe.unpacked.exe";
    struct stat st;
    int result;

    (void)argc;
    (void)argv;
    if (switch_paths_init() != 0) {
        show_error("Could not register the ux0:/app0: path devices.");
        return 1;
    }
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/isaac", 0777);
    /* The game and the SDL backend report on stderr (libepoxy, GL setup);
     * on hardware nothing reads it, so keep it in a file next to the log. */
    if (freopen("sdmc:/switch/isaac/stderr.txt", "w", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    if (stat(pe, &st) != 0) {
        show_error("Missing sdmc:/switch/isaac/isaac-ng.exe.unpacked.exe");
        return 1;
    }
    if (guest_window_init() != 0 || isaac_vita_crt_window_init() != 0) {
        show_error("Not enough memory: start the game as a title override "
                   "(hold R while launching a game), not from the album.");
        return 1;
    }
    /* Faster CPU for the initial resource load; the SDL backend turns it
     * off once frames come steadily. */
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    isaac_vita_log_reset();
    switch_bg_start();
    isaac_vita_log("isaac-switch: port version %s", ISAAC_SWITCH_PORT_VERSION);
    isaac_vita_log("isaac-switch: arena=%p..%p guest base=0x%08x pe=%s",
                   (void *)fake_heap_start, (void *)fake_heap_end,
                   (unsigned)GUEST_IMAGE_BASE, pe);
    {
        pthread_t thread;
        pthread_attr_t attr;

        s_pe = pe;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, GAME_STACK_SIZE);
        if (pthread_create(&thread, &attr, game_thread, NULL) != 0) {
            pthread_attr_destroy(&attr);
            show_error("Could not start the game thread.");
            return 1;
        }
        pthread_attr_destroy(&attr);
        pthread_join(thread, NULL);
        result = s_result;
    }
    isaac_vita_log("first-fault boundary returned %d", result);
    return result;
}
