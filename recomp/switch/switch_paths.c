/* Vita-style device paths on the Switch SD card.
 *
 * The shared runtime, the game and native Lua all spell paths the Vita way
 * ("ux0:/data/isaacr001/...", "ux0:data/isaacr001/...", "app0:/...").  On
 * the Switch newlib treats "name:" as a device, so two devices named "ux0"
 * and "app0" are registered here.  They forward to the "sdmc" device after
 * rewriting the path:
 *
 *   ux0:/data/isaacr001/X, ux0:data/isaacr001/X  -> sdmc:/switch/isaac/X
 *   ux0:/data                                  -> sdmc:/switch/isaac
 *   ux0:/Y                                     -> sdmc:/switch/isaac/ux0/Y
 *   app0:/X                                    -> sdmc:/switch/isaac/X
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

#include "switch_paths.h"

#define ROOT "sdmc:/switch/isaac"

static const devoptab_t *s_sdmc;
static devoptab_t s_ux0;
static devoptab_t s_app0;

static int rewrite(struct _reent *r, const char *path, char *out, size_t size)
{
    const char *colon = strchr(path, ':');
    const char *rest;
    int n;

    if (!colon) {
        r->_errno = ENOENT;
        return -1;
    }
    rest = colon + 1;
    while (*rest == '/')
        ++rest;
    if (colon - path == 4 && strncmp(path, "app0", 4) == 0) {
        n = snprintf(out, size, ROOT "/%s", rest);
    } else if (strcmp(rest, "data") == 0 || strcmp(rest, "data/") == 0) {
        /* The game creates every component of its save path, starting with
         * "ux0:/data"; that must exist, or it never opens log.txt. */
        n = snprintf(out, size, ROOT);
    } else if (strncmp(rest, "data/isaacr001", 14) == 0 &&
               (rest[14] == '\0' || rest[14] == '/')) {
        rest += 14;
        while (*rest == '/')
            ++rest;
        n = snprintf(out, size, ROOT "/%s", rest);
    } else {
        n = snprintf(out, size, ROOT "/ux0/%s", rest);
    }
    if (n < 0 || (size_t)n >= size) {
        r->_errno = ENAMETOOLONG;
        return -1;
    }
    /* No trailing slash ("ROOT/" for the bare root). */
    while (n > 1 && out[n - 1] == '/' && out[n - 2] != ':')
        out[--n] = '\0';
    return 0;
}

#define PATH_BUF 1024
#define WRAP1(name, ret, call)                                            \
    static ret wrap_##name(struct _reent *r, const char *path)             \
    {                                                                     \
        char p[PATH_BUF];                                                 \
        if (rewrite(r, path, p, sizeof p) != 0)                           \
            return -1;                                                    \
        return call;                                                      \
    }

static int wrap_open(struct _reent *r, void *fs, const char *path, int flags,
                     int mode)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->open_r(r, fs, p, flags, mode);
}

static int wrap_stat(struct _reent *r, const char *path, struct stat *st)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->stat_r(r, p, st);
}

static int wrap_lstat(struct _reent *r, const char *path, struct stat *st)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->lstat_r ? s_sdmc->lstat_r(r, p, st)
                           : s_sdmc->stat_r(r, p, st);
}

WRAP1(unlink, int, s_sdmc->unlink_r(r, p))
WRAP1(chdir, int, s_sdmc->chdir_r(r, p))
WRAP1(rmdir, int, s_sdmc->rmdir_r(r, p))

static int wrap_mkdir(struct _reent *r, const char *path, int mode)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->mkdir_r(r, p, mode);
}

static int wrap_rename(struct _reent *r, const char *from, const char *to)
{
    char a[PATH_BUF];
    char b[PATH_BUF];
    if (rewrite(r, from, a, sizeof a) != 0 || rewrite(r, to, b, sizeof b) != 0)
        return -1;
    return s_sdmc->rename_r(r, a, b);
}

static DIR_ITER *wrap_diropen(struct _reent *r, DIR_ITER *dir,
                              const char *path)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return NULL;
    return s_sdmc->diropen_r(r, dir, p);
}

static int wrap_statvfs(struct _reent *r, const char *path,
                        struct statvfs *buf)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->statvfs_r(r, p, buf);
}

static int wrap_chmod(struct _reent *r, const char *path, mode_t mode)
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->chmod_r(r, p, mode);
}

static int wrap_utimes(struct _reent *r, const char *path,
                       const struct timeval times[2])
{
    char p[PATH_BUF];
    if (rewrite(r, path, p, sizeof p) != 0)
        return -1;
    return s_sdmc->utimes_r(r, p, times);
}

static void make_device(devoptab_t *d, const char *name)
{
    *d = *s_sdmc;
    d->name = name;
    d->open_r = wrap_open;
    d->stat_r = wrap_stat;
    d->lstat_r = wrap_lstat;
    d->unlink_r = s_sdmc->unlink_r ? wrap_unlink : NULL;
    d->chdir_r = s_sdmc->chdir_r ? wrap_chdir : NULL;
    d->rmdir_r = s_sdmc->rmdir_r ? wrap_rmdir : NULL;
    d->mkdir_r = s_sdmc->mkdir_r ? wrap_mkdir : NULL;
    d->rename_r = s_sdmc->rename_r ? wrap_rename : NULL;
    d->diropen_r = s_sdmc->diropen_r ? wrap_diropen : NULL;
    d->statvfs_r = s_sdmc->statvfs_r ? wrap_statvfs : NULL;
    d->chmod_r = s_sdmc->chmod_r ? wrap_chmod : NULL;
    d->utimes_r = s_sdmc->utimes_r ? wrap_utimes : NULL;
    d->link_r = NULL;
    d->symlink_r = NULL;
    d->readlink_r = NULL;
    d->pathconf_r = NULL;
}

int switch_paths_init(void)
{
    s_sdmc = GetDeviceOpTab("sdmc");
    if (!s_sdmc)
        return -1;
    make_device(&s_ux0, "ux0");
    make_device(&s_app0, "app0");
    if (AddDevice(&s_ux0) < 0 || AddDevice(&s_app0) < 0)
        return -1;
    return 0;
}
