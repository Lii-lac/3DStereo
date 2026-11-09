#include "crumb.h"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define LOG "sdmc:/3ds/player/home.log"
#define OLD "sdmc:/3ds/player/home.old.log"   // the one before: up to build 52 a full log was deleted, and
                                              // build 50's first eight dropouts went with it

// several threads write here now; a zeroed LightLock is a free one, so no init is needed
static LightLock lk, slk;
static char soon[4096];   // crumb_soon's lines, till crumb_flush
static int nsoon;

static void put(const char *line) {   // under lk
	struct stat st;
	if (!stat(LOG, &st) && st.st_size > 256 * 1024) { remove(OLD); rename(LOG, OLD); }
	FILE *f = fopen(LOG, "a");
	if (!f) return;
	fputs(line, f);
	fclose(f);
}

void crumb(const char *fmt, ...) {
	char b[512];
	int n = snprintf(b, sizeof b, "%llu ", (unsigned long long)osGetTime());
	va_list a;
	va_start(a, fmt);
	vsnprintf(b + n, sizeof b - n - 1, fmt, a);
	va_end(a);
	strcat(b, "\n");
	LightLock_Lock(&lk);
	put(b);
	LightLock_Unlock(&lk);
}

void crumb_soon(const char *fmt, ...) {
	char b[256];
	int n = snprintf(b, sizeof b, "%llu ", (unsigned long long)osGetTime());
	va_list a;
	va_start(a, fmt);
	vsnprintf(b + n, sizeof b - n - 1, fmt, a);
	va_end(a);
	strcat(b, "\n");
	int len = strlen(b);
	LightLock_Lock(&slk);
	if (nsoon + len < (int)sizeof soon) { memcpy(soon + nsoon, b, len + 1); nsoon += len; }
	LightLock_Unlock(&slk);
}

static char late[4096];   // crumb_late's lines, till crumb_flush_late
static int nlate;
static LightLock llk;

void crumb_late(const char *fmt, ...) {
	char b[320];
	int n = snprintf(b, sizeof b, "%llu ", (unsigned long long)osGetTime());
	va_list a;
	va_start(a, fmt);
	vsnprintf(b + n, sizeof b - n - 1, fmt, a);
	va_end(a);
	strcat(b, "\n");
	int len = strlen(b);
	LightLock_Lock(&llk);
	if (nlate + len < (int)sizeof late) { memcpy(late + nlate, b, len + 1); nlate += len; }
	LightLock_Unlock(&llk);
}

void crumb_flush_late(void) {
	if (!nlate) return;
	static char out[sizeof late];
	LightLock_Lock(&llk);
	memcpy(out, late, nlate + 1);
	nlate = 0;
	late[0] = 0;
	LightLock_Unlock(&llk);
	LightLock_Lock(&lk);
	put(out);
	LightLock_Unlock(&lk);
}

void crumb_flush(void) {
	if (!nsoon) return;
	static char out[sizeof soon];
	LightLock_Lock(&slk);
	memcpy(out, soon, nsoon + 1);
	nsoon = 0;
	soon[0] = 0;
	LightLock_Unlock(&slk);
	LightLock_Lock(&lk);
	put(out);
	LightLock_Unlock(&lk);
}
