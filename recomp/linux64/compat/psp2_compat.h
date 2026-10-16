/* Linux/Switch stand-ins for the handful of VitaSDK declarations the shared
 * runtime uses.  Only what the runtime calls is declared; psp2_posix.c
 * implements it over POSIX.  Field names and constants follow vita-headers so
 * the shared sources compile unchanged. */
#ifndef ISAAC_PSP2_COMPAT_H
#define ISAAC_PSP2_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  SceUID;
typedef int64_t  SceOff;
typedef uint64_t SceUInt64;
typedef uint32_t SceUInt32;
typedef unsigned int SceUInt;
typedef int      SceMode;
typedef size_t   SceSize;
typedef ssize_t  SceSSize;
typedef int      SceKernelMemBlockType;
typedef void     SceKernelAllocMemBlockOpt;
typedef void     SceKernelEventFlagOptParam;

typedef struct SceDateTime {
    unsigned short year;
    unsigned short month;
    unsigned short day;
    unsigned short hour;
    unsigned short minute;
    unsigned short second;
    unsigned int microsecond;
} SceDateTime;

typedef struct SceRtcTick {
    SceUInt64 tick;
} SceRtcTick;

typedef struct SceIoStat {
    SceMode st_mode;
    unsigned int st_attr;
    SceOff st_size;
    SceDateTime st_ctime;
    SceDateTime st_atime;
    SceDateTime st_mtime;
    unsigned int st_private[6];
} SceIoStat;

typedef struct SceIoDirent {
    SceIoStat d_stat;
    char d_name[256];
    void *d_private;
    int dummy;
} SceIoDirent;

typedef struct SceKernelMemBlockInfo {
    SceSize size;
    void *mappedBase;
    SceSize mappedSize;
    int memoryType;
    SceUInt32 access;
    SceKernelMemBlockType type;
} SceKernelMemBlockInfo;

enum {
    SCE_O_RDONLY = 0x0001, SCE_O_WRONLY = 0x0002, SCE_O_RDWR = 0x0003,
    SCE_O_APPEND = 0x0100, SCE_O_CREAT = 0x0200, SCE_O_TRUNC = 0x0400,
    SCE_O_EXCL = 0x0800
};
#define SCE_S_IFMT  0xF000
#define SCE_S_IFLNK 0x4000
#define SCE_S_IFDIR 0x1000
#define SCE_S_IFREG 0x2000
#define SCE_S_IRUSR 000400
#define SCE_S_IWUSR 000200
#define SCE_S_IXUSR 000100
#define SCE_S_ISDIR(m) (((m) & SCE_S_IFMT) == SCE_S_IFDIR)
#define SCE_S_ISREG(m) (((m) & SCE_S_IFMT) == SCE_S_IFREG)
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060
#define SCE_KERNEL_MEMORY_ACCESS_W 0x02
#define SCE_KERNEL_MEMORY_ACCESS_R 0x04

#define SCE_EVENT_WAITAND 0
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR 2
#define SCE_EVENT_WAITCLEAR_PAT 4
#define SCE_EVENT_WAITMULTIPLE 0x00001000

#define SCE_ERROR_ERRNO_ENOENT 0x80010002
#define SCE_ERROR_ERRNO_ENODEV 0x80010013
#define SCE_KERNEL_ERROR_UNKNOWN_THREAD_ID   ((int)0x80028021)
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEX_ID    ((int)0x80028141)
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVF      ((int)0x80028143)
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UDF    ((int)0x80028144)
#define SCE_KERNEL_ERROR_MUTEX_FAILED_TO_OWN ((int)0x80028145)
#define SCE_KERNEL_ERROR_MUTEX_NOT_OWNED     ((int)0x80028146)

/* io */
SceUID sceIoOpen(const char *file, int flags, SceMode mode);
int sceIoClose(SceUID fd);
SceSSize sceIoRead(SceUID fd, void *buf, SceSize nbyte);
SceSSize sceIoWrite(SceUID fd, const void *buf, SceSize nbyte);
int sceIoPread(SceUID fd, void *data, SceSize size, SceOff offset);
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence);
long sceIoLseek32(SceUID fd, long offset, int whence);
int sceIoRemove(const char *file);
int sceIoGetstat(const char *file, SceIoStat *stat);
int sceIoGetstatByFd(SceUID fd, SceIoStat *stat);
SceUID sceIoDopen(const char *dirname);
int sceIoDread(SceUID fd, SceIoDirent *dir);
int sceIoDclose(SceUID fd);

/* clib */
int sceClibPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* process / thread */
int sceKernelExitProcess(int res);
SceUID sceKernelGetProcessId(void);
int sceKernelGetThreadId(void);
int sceKernelDelayThread(SceUInt delay);
SceUInt64 sceKernelGetProcessTimeWide(void);
SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *opt);
int sceKernelDeleteEventFlag(int evid);
int sceKernelSetEventFlag(SceUID evid, unsigned int bits);
int sceKernelClearEventFlag(SceUID evid, unsigned int bits);
int sceKernelWaitEventFlag(int evid, unsigned int bits, unsigned int wait,
                           unsigned int *outBits, SceUInt *timeout);

/* sysmem */
SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size, SceKernelAllocMemBlockOpt *opt);
int sceKernelFreeMemBlock(SceUID uid);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelGetMemBlockInfoByAddr(void *base, SceKernelMemBlockInfo *info);

/* rtc */
unsigned int sceRtcGetTickResolution(void);
int sceRtcGetTick(const SceDateTime *time, SceRtcTick *tick);
int sceRtcSetTick(SceDateTime *time, const SceRtcTick *tick);
int sceRtcSetTime64_t(SceDateTime *time, SceUInt64 ullTime);
int sceRtcConvertUtcToLocalTime(const SceRtcTick *utc, SceRtcTick *local_time);

#ifdef __cplusplus
}
#endif

#endif
