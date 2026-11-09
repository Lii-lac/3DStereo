// Just enough of libctru for stream.c on a PC: locks, events and threads on pthreads
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef int64_t s64; typedef int32_t s32; typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define U64_MAX UINT64_MAX
typedef pthread_mutex_t LightLock;
static inline void LightLock_Init(LightLock *l) { pthread_mutex_init(l, NULL); }
static inline void LightLock_Lock(LightLock *l) { pthread_mutex_lock(l); }
static inline void LightLock_Unlock(LightLock *l) { pthread_mutex_unlock(l); }
typedef struct { pthread_mutex_t m; pthread_cond_t c; int on; } LightEvent;
#define RESET_ONESHOT 0
static inline void LightEvent_Init(LightEvent *e, int k) { (void)k; pthread_mutex_init(&e->m, NULL); pthread_cond_init(&e->c, NULL); e->on = 0; }
static inline void LightEvent_Signal(LightEvent *e) { pthread_mutex_lock(&e->m); e->on = 1; pthread_cond_signal(&e->c); pthread_mutex_unlock(&e->m); }
static inline void LightEvent_Wait(LightEvent *e) { pthread_mutex_lock(&e->m); while (!e->on) pthread_cond_wait(&e->c, &e->m); e->on = 0; pthread_mutex_unlock(&e->m); }
typedef struct { pthread_t t; } *Thread;
typedef void (*ThreadFunc)(void *);
struct targ { ThreadFunc f; void *a; };
static void *tramp(void *p) { struct targ t = *(struct targ *)p; free(p); t.f(t.a); return NULL; }
static inline Thread threadCreate(ThreadFunc f, void *a, size_t st, int prio, int core, bool det) {
	(void)st; (void)prio; (void)core; (void)det;
	Thread t = malloc(sizeof *t); struct targ *g = malloc(sizeof *g); g->f = f; g->a = a;
	pthread_create(&t->t, NULL, tramp, g); return t; }
static inline void threadJoin(Thread t, u64 to) { (void)to; pthread_join(t->t, NULL); }
static inline void threadFree(Thread t) { free(t); }
#define CUR_THREAD_HANDLE 0
static inline int svcGetThreadPriority(s32 *p, int h) { (void)h; *p = 0x30; return 0; }
