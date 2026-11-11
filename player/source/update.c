#include "update.h"
#include "crumb.h"
#include "net.h"
#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define LATEST "https://api.github.com/repos/Lii-lac/3DStereo/releases/latest"
#define CA     "sdmc:/3ds/player/cacert.pem"   // net.c writes it there
#define MARK   "sdmc:/3ds/player/updated"      // upd_mark: the next launch is the new version
#define CIA_TMP "sdmc:/3ds/player/update.cia"   // the CIA, downloaded whole before it's installed
#define CHUNK  (64 * 1024)

extern int __system_argc;
extern char **__system_argv;

static LightLock lk;
static Thread th;
static volatile bool quitting;
static int state = U_IDLE;
static int pct = -1;          // the download, 0 to 100 (-1: size not known yet)
static char newver[24];       // the release's version, "1.2.3"
static char why[48];          // U_FAIL: what went wrong, for the row

static void set(int s, const char *w) {
	LightLock_Lock(&lk);
	state = s;
	if (w) snprintf(why, sizeof why, "%s", w);
	LightLock_Unlock(&lk);
}

// "v1.2.3" or "1.2.3" as one comparable number; -1 when it isn't a version
static long vnum(const char *s) {
	int a = 0, b = 0, c = 0;
	if (*s == 'v' || *s == 'V') s++;
	if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 2 || a < 0 || b < 0 || c < 0) return -1;
	return a * 1000000L + b * 1000L + c;
}

static bool ends_with(const char *s, const char *tail) {
	size_t n = strlen(s), m = strlen(tail);
	return n >= m && !strcasecmp(s + n - m, tail);
}

// ---- the download --------------------------------------------------------------------------

typedef struct {
	FILE *f;      // where the download goes: the new 3dsx, or the CIA before it's installed
	u64 off;
	u8 head[4];   // the first bytes, to check a 3dsx is one
	bool bad;
} Sink;

static size_t on_data(char *p, size_t sz, size_t n, void *ud) {
	Sink *s = ud;
	size_t len = sz * n;
	for (size_t i = 0; i < len && s->off + i < 4; i++) s->head[s->off + i] = (u8)p[i];
	if (fwrite(p, 1, len, s->f) != len) { s->bad = true; return 0; }
	s->off += len;
	return len;
}

static int on_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
	(void)ud; (void)ut; (void)un;
	if (total > 0) {
		LightLock_Lock(&lk);
		pct = (int)(now * 100 / total);
		LightLock_Unlock(&lk);
	}
	return quitting;   // nonzero stops it
}

static bool fetch(CURL *h, const char *url, Sink *s) {
	curl_easy_reset(h);
	curl_easy_setopt(h, CURLOPT_URL, url);
	curl_easy_setopt(h, CURLOPT_CAINFO, CA);
	curl_easy_setopt(h, CURLOPT_USERAGENT, "3DStereo");
	curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);   // GitHub hands the file over from its storage host
	curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, s);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);   // no total limit: a slow Wi-Fi takes as long as it takes,
	curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);   // but half a minute with nothing coming is a dead line
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
	u64 t0 = osGetTime();
	CURLcode rc = curl_easy_perform(h);
	crumb("update: %.80s: %s, %llu bytes, %llu ms", url, curl_easy_strerror(rc), s->off, osGetTime() - t0);
	return rc == CURLE_OK && !s->bad;
}

// Started from a CIA: the whole file lands on the card first, then goes into the system's installer in one
// go. Fed straight off the network, the installer gave up when the Wi-Fi stalled mid-header (a Spotify sync
// running alongside: three tries cut at byte 13790, inside the TMD); from the card it never waits.
static bool cia_from_file(const char *path, u64 size) {
	FILE *f = fopen(path, "rb");
	u8 *buf = malloc(CHUNK);
	if (!f || !buf) { if (f) fclose(f); free(buf); set(U_FAIL, f ? "out of memory" : "can't read the download"); return false; }
	Result r = amInit();
	if (R_FAILED(r)) { crumb("update: amInit %08lx", r); fclose(f); free(buf); set(U_FAIL, "the installer won't open"); return false; }
	Handle cia;
	r = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
	if (R_FAILED(r)) { crumb("update: AM_StartCiaInstall %08lx", r); amExit(); fclose(f); free(buf); set(U_FAIL, "the installer said no"); return false; }
	u64 off = 0;
	bool ok = true;
	while (off < size) {
		size_t n = fread(buf, 1, size - off < CHUNK ? (size_t)(size - off) : CHUNK, f);
		if (!n) { crumb("update: read stopped at %llu of %llu", off, size); ok = false; break; }
		u32 w = 0;
		r = FSFILE_Write(cia, &w, off, buf, n, 0);
		if (R_FAILED(r) || w != n) { crumb("update: install write at %llu: %08lx, %lu of %u", off, r, w, (unsigned)n); ok = false; break; }
		off += n;
	}
	fclose(f);
	free(buf);
	if (ok) {
		r = AM_FinishCiaInstall(cia);
		if (R_FAILED(r)) { crumb("update: AM_FinishCiaInstall %08lx", r); set(U_FAIL, "the install didn't take"); ok = false; }
	} else {
		AM_CancelCIAInstall(cia);
		set(U_FAIL, "the installer stopped partway");
	}
	amExit();
	return ok;
}

static bool install_cia(CURL *h, const char *url, u64 size) {
	Sink s = {0};
	remove(CIA_TMP);
	if (!(s.f = fopen(CIA_TMP, "wb"))) { set(U_FAIL, "can't write to the card"); return false; }
	bool ok = fetch(h, url, &s);
	if (fclose(s.f)) ok = false;
	if (ok && size && s.off != size) { crumb("update: %llu of %llu bytes", s.off, size); ok = false; }   // cut short: the installer never sees it
	if (!ok) { remove(CIA_TMP); set(U_FAIL, quitting ? "stopped" : "the download broke off"); return false; }
	set(U_FINISH, NULL);
	ok = cia_from_file(CIA_TMP, s.off);
	remove(CIA_TMP);
	return ok;
}

// Started from the Homebrew Launcher: the new 3dsx goes next to ours, and only a whole one replaces it.
static bool install_3dsx(CURL *h, const char *url, u64 size) {
	const char *self = __system_argc > 0 ? __system_argv[0] : NULL;
	if (!self || strncmp(self, "sdmc:/", 6) || !ends_with(self, ".3dsx") || strlen(self) > 280) { set(U_FAIL, "can't tell which 3dsx is ours"); return false; }
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%.280s.new", self);
	Sink s = {0};
	if (!(s.f = fopen(tmp, "wb"))) { set(U_FAIL, "can't write to the card"); return false; }
	bool ok = fetch(h, url, &s);
	if (fclose(s.f)) ok = false;
	if (ok && size && s.off != size) { crumb("update: %llu of %llu bytes", s.off, size); ok = false; }   // cut short: never in place of a whole one
	if (ok && memcmp(s.head, "3DSX", 4)) { crumb("update: not a 3dsx"); ok = false; }
	if (!ok) { remove(tmp); set(U_FAIL, quitting ? "stopped" : "the download broke off"); return false; }
	set(U_FINISH, NULL);
	// the card won't rename onto a file that's there: ours steps aside first, and comes back if the new one won't go in
	char old[300];
	snprintf(old, sizeof old, "%.280s.old", self);
	remove(old);
	if (rename(self, old)) { crumb("update: rename %s failed", self); remove(tmp); set(U_FAIL, "couldn't swap the file in"); return false; }
	if (rename(tmp, self)) {
		crumb("update: rename %s failed", tmp);
		if (!rename(old, self)) remove(tmp);   // ours is back; if it isn't, both stay on the card for a hand to sort out
		else crumb("update: %s and %s are both left", old, tmp);
		set(U_FAIL, "couldn't swap the file in");
		return false;
	}
	remove(old);
	return true;
}

// ---- a CIA already on the card --------------------------------------------------------------

// where a CIA dropped on the card by hand is looked for: the root, and the folders people keep them in
static const char *DROP[] = {"sdmc:/", "sdmc:/cias/", "sdmc:/1. cias/"};

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// A CIA's version as vnum's number, read out of its TMD; -1 unless it's this app (same title ID) and readable.
static long cia_ver(const char *path, u64 tid, u64 *size) {
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	long v = -1;
	u8 h[0x20], t[0x300];
	if (fread(h, 1, sizeof h, f) == sizeof h) {
		u32 al = 63;
		u64 at = (((((u64)le32(h) + al) & ~al) + le32(h + 8) + al) & ~al);   // past the header, then the certificates
		at = (at + le32(h + 12) + al) & ~al;                                  // past the ticket: the TMD
		if (!fseek(f, (long)at, SEEK_SET) && fread(t, 1, sizeof t, f) == sizeof t) {
			u32 sig = be32(t), body = sig == 0x10004 ? 0x140 : sig == 0x10003 ? 0x240 : 0;   // RSA-2048 or -4096, padded
			if (body) {
				u64 id = (u64)be32(t + body + 0x4C) << 32 | be32(t + body + 0x50);
				u32 tv = t[body + 0x9C] << 8 | t[body + 0x9D];
				if (id == tid) v = (tv >> 10) * 1000000L + ((tv >> 4) & 63) * 1000L + (tv & 15);
			}
		}
		if (!fseek(f, 0, SEEK_END)) *size = (u64)ftell(f);
	}
	fclose(f);
	return v;
}

// The newest CIA of this app on the card that's newer than this one, into path; false when there's none.
static bool local_cia(char *path, size_t cap, u64 *size, long *ver) {
	u64 tid = 0;
	if (R_FAILED(APT_GetProgramID(&tid)) || !tid) return false;
	long have = vnum(VERSION), best = have;
	for (size_t d = 0; d < sizeof DROP / sizeof *DROP; d++) {
		DIR *dir = opendir(DROP[d]);
		if (!dir) continue;
		struct dirent *e;
		while ((e = readdir(dir))) {
			if (!ends_with(e->d_name, ".cia")) continue;
			char p[300];
			if (snprintf(p, sizeof p, "%s%s", DROP[d], e->d_name) >= (int)sizeof p) continue;
			u64 sz = 0;
			long v = cia_ver(p, tid, &sz);
			if (v > best && sz) { best = v; snprintf(path, cap, "%s", p); *size = sz; }
		}
		closedir(dir);
	}
	if (best == have) return false;
	*ver = best;
	return true;
}

// ---- the run -------------------------------------------------------------------------------

static void worker(void *arg) {
	(void)arg;
	char err[48];
	if (!envIsHomebrew()) {   // a newer CIA of ours already on the card goes in first, no Wi-Fi needed, and is gone after
		char path[300];
		u64 size = 0;
		long v = 0;
		if (local_cia(path, sizeof path, &size, &v)) {
			LightLock_Lock(&lk);
			snprintf(newver, sizeof newver, "%ld.%ld.%ld", v / 1000000, v / 1000 % 1000, v % 1000);
			LightLock_Unlock(&lk);
			crumb("update: %s on the card is v%s, this is %s", path, newver, VERSION);
			set(U_FINISH, NULL);
			if (cia_from_file(path, size)) { remove(path); crumb("update: v%s installed from the card", newver); set(U_DONE, NULL); }
			return;
		}
	}
	if (!net_up(err, sizeof err)) { set(U_FAIL, err); return; }
	if (net_clock_year()) { set(U_FAIL, "the 3DS's clock is wrong"); return; }
	CURL *h = net_handle();
	if (!h) { set(U_FAIL, "the network won't start"); return; }
	Buf b = {0};
	CURLcode ce;
	long code = net_req(h, "GET", LATEST, NULL, "Accept: application/vnd.github+json", &b, NULL, &quitting, &ce);
	json_t *j = code == 200 && b.d ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	free(b.d);
	if (!j) {
		if (code == 404) set(U_FAIL, "no release on GitHub yet");
		else if (code > 0) { snprintf(err, sizeof err, "GitHub said %ld", code); set(U_FAIL, err); }
		else set(U_FAIL, quitting ? "stopped" : "GitHub didn't answer");
		curl_easy_cleanup(h);
		return;
	}
	const char *tag = json_string_value(json_object_get(j, "tag_name"));
	long have = vnum(VERSION), got = tag ? vnum(tag) : -1;
	crumb("update: latest %s, this is %s", tag ? tag : "(none)", VERSION);
	if (got < 0) { set(U_FAIL, "the release has no version"); goto out; }
	LightLock_Lock(&lk);
	snprintf(newver, sizeof newver, "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
	LightLock_Unlock(&lk);
	if (have >= 0 && got <= have) { set(U_CURRENT, NULL); goto out; }
	bool cia = !envIsHomebrew();
	const char *want = cia ? ".cia" : ".3dsx", *url = NULL;
	json_int_t size = 0;   // what GitHub says the file is; 0: it didn't say
	size_t i;
	json_t *a;
	json_array_foreach(json_object_get(j, "assets"), i, a) {
		const char *name = json_string_value(json_object_get(a, "name"));
		if (name && ends_with(name, want)) {
			url = json_string_value(json_object_get(a, "browser_download_url"));
			size = json_integer_value(json_object_get(a, "size"));
			break;
		}
	}
	if (!url) { set(U_FAIL, cia ? "the release has no .cia" : "the release has no .3dsx"); goto out; }
	set(U_GET, NULL);
	if (size < 0) size = 0;
	if (cia ? install_cia(h, url, (u64)size) : install_3dsx(h, url, (u64)size)) { crumb("update: v%s installed", newver); set(U_DONE, NULL); }
out:
	json_decref(j);
	curl_easy_cleanup(h);
}

void upd_init(void) { LightLock_Init(&lk); }

bool upd_busy(void) {
	LightLock_Lock(&lk);
	bool b = state == U_CHECK || state == U_GET || state == U_FINISH;
	LightLock_Unlock(&lk);
	return b;
}

bool upd_done(void) {
	LightLock_Lock(&lk);
	bool d = state == U_DONE;
	LightLock_Unlock(&lk);
	return d;
}

void upd_start(void) {
	if (upd_busy() || upd_done()) return;
	if (th) { threadJoin(th, U64_MAX); threadFree(th); th = NULL; }   // the last run, finished
	LightLock_Lock(&lk);
	state = U_CHECK;
	pct = -1;
	LightLock_Unlock(&lk);
	quitting = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	th = threadCreate(worker, NULL, 128 * 1024, prio + 3 < 0x3F ? prio + 3 : 0x3F, 0, false);
	if (!th) { crumb("update: no thread"); set(U_FAIL, "out of memory"); }
}

void upd_line(char *out, int cap) {
	LightLock_Lock(&lk);
	switch (state) {
	case U_CHECK: snprintf(out, cap, "Checking GitHub"); break;
	case U_GET:
		if (pct >= 0) snprintf(out, cap, "Downloading v%s \xc2\xb7 %d%%", newver, pct);
		else snprintf(out, cap, "Downloading v%s", newver);
		break;
	case U_FINISH: snprintf(out, cap, "Installing v%s", newver); break;
	case U_CURRENT: snprintf(out, cap, "Up to date \xc2\xb7 v%s", VERSION); break;
	case U_DONE: snprintf(out, cap, "v%s installed \xc2\xb7 tap to restart", newver); break;
	case U_FAIL: snprintf(out, cap, "Didn't update: %s", why); break;
	default: snprintf(out, cap, "Get the newest version from GitHub"); break;
	}
	LightLock_Unlock(&lk);
}

int upd_state(int *p) {
	LightLock_Lock(&lk);
	int s = state;
	if (p) *p = pct;
	LightLock_Unlock(&lk);
	return s;
}

void upd_mark(void) {
	FILE *f = fopen(MARK, "w");
	if (f) fclose(f);
}

bool upd_just_updated(void) {
	return !remove(MARK);
}

void upd_exit(void) {
	if (!th) return;
	quitting = true;
	threadJoin(th, U64_MAX);
	threadFree(th);
	th = NULL;
}
