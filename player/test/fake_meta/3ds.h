// enough of libctru for meta.c to build on a PC (test_meta.c): no threads run, locks do nothing
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
typedef int LightLock;
typedef int LightEvent;
typedef void *Thread;
#define RESET_ONESHOT 0
#define CUR_THREAD_HANDLE 0
#define U64_MAX UINT64_MAX
static inline void LightLock_Init(LightLock *l) { (void)l; }
static inline void LightLock_Lock(LightLock *l) { (void)l; }
static inline void LightLock_Unlock(LightLock *l) { (void)l; }
static inline void LightEvent_Init(LightEvent *e, int m) { (void)e; (void)m; }
static inline void LightEvent_Signal(LightEvent *e) { (void)e; }
static inline void LightEvent_Wait(LightEvent *e) { (void)e; }
static inline void svcSleepThread(s64 ns) { (void)ns; }
static inline int svcGetThreadPriority(s32 *p, int h) { (void)h; *p = 0x30; return 0; }
static inline Thread threadCreate(void (*f)(void *), void *a, int s, int p, int c, bool d) { (void)f; (void)a; (void)s; (void)p; (void)c; (void)d; return 0; }
static inline void threadJoin(Thread t, u64 to) { (void)t; (void)to; }
static inline void threadFree(Thread t) { (void)t; }
static inline int LightEvent_WaitTimeout(LightEvent *e, s64 ns) { (void)e; (void)ns; return 1; }
static inline u64 osGetTime(void) { return 0; }
// for test_lists.c, which builds library.c and spotify.c too
typedef s32 Result;
Result archive_getmtime(const char *path, u64 *mtime);
static inline int svcSetThreadPriority(int h, s32 p) { (void)h; (void)p; return 0; }
