/* Entry point of the 64-bit Linux host.
 *
 *   isaac-linux64 <data-root> [unpacked-pe]
 *
 * <data-root> becomes the working directory.  The Vita layout is recreated
 * there (see platform.h): the game's resources go to
 * <data-root>/ux0:/data/isaacr001/resources, saves and the port log live next
 * to them.  The PE defaults to <data-root>/app0:/isaac-ng.exe.unpacked.exe. */
#include "platform.h"
#include "guest_window.h"
#include "host_vita_crt.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x98000000u
#endif

int isaac_vita_run_first_fault(const char *pe_path);

static int prepare_root(const char *root)
{
    struct stat st;
    if (chdir(root) != 0) {
        fprintf(stderr, "cannot enter data root %s: %s\n", root,
                strerror(errno));
        return -1;
    }
    mkdir("ux0:", 0777);
    mkdir("ux0:/data", 0777);
    mkdir("ux0:/data/isaacr001", 0777);
    mkdir("app0:", 0777);
    if (lstat("ux0:data", &st) != 0 && symlink("ux0:/data", "ux0:data") != 0) {
        fprintf(stderr, "cannot create ux0:data link: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *pe = "app0:/isaac-ng.exe.unpacked.exe";
    int result;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <data-root> [unpacked-pe]\n", argv[0]);
        return 2;
    }
    if (guest_window_init() != 0 || isaac_vita_crt_window_init() != 0)
        return 2;
    if (prepare_root(argv[1]) != 0)
        return 2;
    if (argc > 2)
        pe = argv[2];

    isaac_vita_log_reset();
    isaac_vita_log("isaac-linux64: window=%p guest base=0x%08x pe=%s",
                   (void *)g_guest_mem_base, (unsigned)GUEST_IMAGE_BASE, pe);
    result = isaac_vita_run_first_fault(pe);
    isaac_vita_log("first-fault boundary returned %d", result);
    return result;
}
