#ifndef ISAAC_VITA_PLATFORM_H
#define ISAAC_VITA_PLATFORM_H

/* Linux build: same spellings as recomp/vita/platform.h.  The process runs
 * with its data root as the current directory, where `ux0:/data`,
 * `ux0:data -> ux0:/data` and `app0:` exist, so these resolve unchanged. */
#define ISAAC_VITA_GUEST_DATA_PARENT  "ux0:data"
#define ISAAC_VITA_NATIVE_DATA_PARENT "ux0:/data"
#define ISAAC_VITA_DATA_ROOT \
    ISAAC_VITA_GUEST_DATA_PARENT "/isaacr001"
#define ISAAC_VITA_NATIVE_DATA_ROOT \
    ISAAC_VITA_NATIVE_DATA_PARENT "/isaacr001"

#ifdef __cplusplus
extern "C" {
#endif

void isaac_vita_log_reset(void);
void isaac_vita_log(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
