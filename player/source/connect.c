#define _GNU_SOURCE   // memmem
#include "connect.h"
#include "cardio.h"
#include "cpb.h"
#include "crumb.h"
#include "library.h"
#include "net.h"
#include "spotify.h"
#include "spotmatch.h"
#include <curl/curl.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <zlib.h>

#ifndef VERSION
#define VERSION "dev"
#endif
#ifndef BUILD
#define BUILD 0
#endif
#define DIR3DS   "sdmc:/3ds/player"
#define PAIR     DIR3DS "/spotify-connect.json"
#define ACCESS   DIR3DS "/connect-access"   // the last access token and when it ends: a quick relaunch need not refresh the login
#define DEVFILE  DIR3DS "/connect-device"
#define CA       DIR3DS "/cacert.pem"
#define TOKEN_URL "https://accounts.spotify.com/api/token"
#define NAME     "3DStereo"   // as Spotify's device list shows it, unless Settings says otherwise
static char dev_name[64] = NAME;   // (lk)
#define MAXCTX   1000      // songs kept from a context (librespot sends 80 ahead; we keep more to skip through)
#define LOOK     60        // how far ahead to look for a song that's on the card
#define UNIX_1900 2208988800000ULL   // osGetTime counts from 1900

// ---- between the threads ------------------------------------------------------------------

static Thread th;
static LightLock lk;           // everything below marked (lk)
static volatile bool quitting;
static bool inited;

// the context, in Spotify's order; ord[] is the play order (shuffle reorders it)  (lk)
static char (*c_uri)[192], (*c_uid)[48];   // 192: a local file's uri carries artist, album and title
static bool *c_q;              // put in the queue by hand
static int c_n, *ord, ord_n, pos;    // pos: where in ord the current song is
static char ctx_uri[256], session[33], playback[33];
static u8 origin[512], ctx_restr[1024];
static size_t norigin, nctx_restr;
static bool shuffle, rep_ctx, rep_track;

static bool active, loaded, paused;  // (lk) what Spotify is told
static bool mirror;                  // (lk) Spotify follows the 3DS: its own queue plays, Spotify shows it and drives it
static bool mirror_due;              // (lk) main -> worker: tell Spotify what the 3DS is playing now
static int up_next[4], n_up;         // (lk) the 3DS's own queue after the current song, from main
static int n_upq;                    // (lk) ...the first of them queued by hand
static int qa[64], qa_n;              // (lk) songs main wants in Spotify's queue, for the worker to look up and add
static int src_kind_ = SRC_OTHER;    // (lk) ...and where it came from
static char src_pid[64];
static bool src_single;
static u32 volume = 65535;           // (lk) Spotify's slider, 0..65535
static bool vol_due;                 // (lk) worker -> main: apply it
#define VOLFILE DIR3DS "/connect-volume"
static int cur_local = -1;           // (lk) the library index playing for ord[pos]
static int64_t cur_dur;              // (lk) ms
static int64_t rep_pos, rep_at;      // (lk) position at a moment of Spotify's clock, as last told
static uint64_t started_at;
static u32 last_id;
static char last_from[64];

// next, found ahead so the audio worker can follow on without waiting  (lk)
static int nx_pos = -1, nx_local = -1;
static int64_t nx_dur;
static bool nx_want;           // main -> worker: look again
static bool nx_done;           // (lk) looked already for this song
static u64 trust_rep_until;    // (lk) an action is on its way to the player: rep_pos is right, the player not yet
static int pend_pos = -1, pend_local = -1;   // the audio worker took nx; main commits it when heard
static int64_t pend_dur;

// worker -> main
static ConnectDo todo;
static bool todo_set;
static u64 grace_until;        // main: no "the player changed" while an action settles
// main -> worker
static bool put_due, release_due, skip_due;
// Shuffle and repeat are one setting for the person holding the 3DS, wherever they're changed (up to
// build 54 Spotify saw them off whenever the 3DS drove, and its own toggles never reached the 3DS):
// opts_due sends Spotify's to main; m_shuffle/m_repeat are main's as last told, -1 before then
static bool opts_due;
static int m_shuffle = -1, m_repeat = -1;
static void opts_from_spotify(void) {   // (lk) Spotify's options are the ones now: main takes them too
	opts_due = true;
	m_shuffle = shuffle;
	m_repeat = rep_track ? 2 : rep_ctx ? 1 : 0;
}
static void opts_from_3ds(void) {   // (lk) the 3DS's are the ones now (Spotify follows it)
	if (m_shuffle < 0) { shuffle = rep_ctx = rep_track = false; return; }
	shuffle = m_shuffle;
	rep_ctx = m_repeat >= 1;
	rep_track = m_repeat == 2;
}
static int skip_dir;
// main's snapshot of the player, for the worker's state  (lk)
static int snap_track = -1;
static double snap_pos;
static bool snap_paused, snap_open;
static u64 snap_at;

static bool filled;             // the context matched in bulk already
static int warm_at;             // where warm() looks next
static u64 lost_at;            // (lk) the socket dropped while Spotify was driving: we keep playing and get back in
static int64_t clock_off;      // Spotify's clock minus ours, ms (from the Date of its replies)

static int64_t now_ms(void) { return (int64_t)(osGetTime() - UNIX_1900) + clock_off; }

bool connect_paired(void) {
	struct stat st;
	return !stat(PAIR, &st) || !stat(PAIR ".tmp", &st);   // ".tmp": a swap the power cut short
}

// ---- requests -----------------------------------------------------------------------------

static CURL *hh;               // the worker's handle for requests (the websocket has its own)
static volatile bool login_dead;   // Spotify says the saved login is revoked: pair again
static volatile bool no_abort;     // a login refresh is on its way: quitting waits for it (briefly), the new login would be lost
static volatile bool pair_fresh;   // a new login was just made on the 3DS: the worker need not wait out its back-off
static u64 nab_t0;
bool connect_login_dead(void) { return login_dead; }
static char tok[1200];
static u64 tok_until;
static char devid[41];
static char spclient[128], dealer[128];

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
	Buf *b = ud;
	size_t k = sz * n;
	if (b->n + k > (6u << 20)) return 0;
	char *g = realloc(b->d, b->n + k + 1);
	if (!g) return 0;
	memcpy(g + b->n, p, k);
	b->d = g;
	b->n += k;
	b->d[b->n] = 0;
	return k;
}

static int mon(const char *m) {
	static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
	for (int i = 0; i < 12; i++) if (!strncmp(M + 3 * i, m, 3)) return i;
	return -1;
}

static int64_t days_from_civil(int y, int m, int d) {   // m 1..12; days since 1970-01-01
	y -= m <= 2;
	int era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// "Date: Thu, 05 Nov 2026 18:30:00 GMT": Spotify's clock, since the 3DS's is whatever it was set to
static size_t on_header(char *p, size_t sz, size_t n, void *ud) {
	(void)ud;
	size_t k = sz * n;
	if (k > 30 && !strncasecmp(p, "date:", 5)) {
		char wd[8], ms[8];
		int d, y, hh_, mm, ss;
		if (sscanf(p + 5, " %3s, %d %3s %d %d:%d:%d", wd, &d, ms, &y, &hh_, &mm, &ss) == 7 && mon(ms) >= 0) {
			int64_t t = (days_from_civil(y, mon(ms) + 1, d) * 86400 + hh_ * 3600 + mm * 60 + ss) * 1000 + 500;
			int64_t off = t - (int64_t)(osGetTime() - UNIX_1900);
			// a second's resolution: move only when it's clearly off, so the bar doesn't jitter
			if (llabs(off - clock_off) > 1500) { clock_off = off; crumb("connect: clock off by %lld ms", (long long)off); }
		}
	}
	return k;
}

static int on_xfer(void *p, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
	(void)p; (void)a; (void)b; (void)c; (void)d;
	return quitting && (!no_abort || osGetTime() - nab_t0 > 4000);   // a refresh already sent: Spotify has rotated the login, so let the answer land
}

// One request on hh. hdrs: NULL-terminated extra header lines. body may be binary (n bytes).
static long creq(const char *method, const char *url, const char *const *hdrs, const void *body, size_t n, Buf *out) {
	free(out->d);
	*out = (Buf){0};
	struct curl_slist *h = NULL;
	for (int i = 0; hdrs && hdrs[i]; i++) h = curl_slist_append(h, hdrs[i]);
	if (!body && strcmp(method, "GET")) h = curl_slist_append(h, "Content-Length: 0");
	curl_easy_reset(hh);
	curl_easy_setopt(hh, CURLOPT_URL, url);
	curl_easy_setopt(hh, CURLOPT_CAINFO, CA);
	curl_easy_setopt(hh, CURLOPT_USERAGENT, "3DStereo");
	curl_easy_setopt(hh, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(hh, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(hh, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(hh, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(hh, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(hh, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(hh, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(hh, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(hh, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(hh, CURLOPT_XFERINFOFUNCTION, on_xfer);
	if (body) {
		curl_easy_setopt(hh, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(hh, CURLOPT_POSTFIELDSIZE, (long)n);
	}
	if (strcmp(method, "GET") && strcmp(method, "POST")) curl_easy_setopt(hh, CURLOPT_CUSTOMREQUEST, method);
	u64 t0 = osGetTime();
	CURLcode rc = curl_easy_perform(hh);
	curl_slist_free_all(h);
	if (rc != CURLE_OK) {
		if (!quitting) crumb("connect: %s %.70s: %s", method, url, curl_easy_strerror(rc));
		return -1;
	}
	long code = 0;
	curl_easy_getinfo(hh, CURLINFO_RESPONSE_CODE, &code);
	crumb("connect: %s %.60s %ld, %u bytes, %llu ms", method, url, code, (unsigned)out->n, osGetTime() - t0);
	return code;
}

static json_t *load_pair(void) {
	FILE *f = card_open(PAIR, "rb");
	if (!f) f = card_open(PAIR ".tmp", "rb");   // the new login written, the old one gone, the rename not done
	if (!f) return NULL;
	json_t *j = json_loadf(f, 0, NULL);
	fclose(f);
	return j;
}

// A login on the card: written beside the old one, then swapped in (FAT can't rename over a file).
static bool login_save(json_t *pair) {
	if (json_dump_file(pair, PAIR ".tmp", JSON_INDENT(2))) return false;
	remove(PAIR);
	return !rename(PAIR ".tmp", PAIR);
}
static bool access_cached(void) {   // an earlier run's access token, still good: no refresh, so no rotation to lose
	FILE *f = card_open(ACCESS, "rb");
	if (!f) return false;
	char a[1100] = "";
	long long exp = 0;
	bool ok = fscanf(f, "%1099s %lld", a, &exp) == 2;
	fclose(f);
	long long now = (long long)time(NULL);
	if (!ok || strlen(a) < 20 || exp < now + 300) return false;
	snprintf(tok, sizeof tok, "%s", a);
	tok_until = osGetTime() + (u64)(exp - now - 120) * 1000;
	return true;
}
static void access_save(const char *a, long life) {
	FILE *f = fopen(ACCESS, "wb");
	if (!f) return;
	fprintf(f, "%s %lld\n", a, (long long)time(NULL) + life);
	fclose(f);
}

// The desktop-id login for an access token; Spotify may hand back a new login with it.
static bool token(bool force) {
	if (!force && tok[0] && osGetTime() < tok_until) return true;
	if (!force && !tok[0] && access_cached()) { crumb("connect: access token from the card"); return true; }
	tok[0] = 0;
	json_t *pair = load_pair();
	const char *cid = json_string_value(json_object_get(pair, "client_id"));
	const char *rt = json_string_value(json_object_get(pair, "refresh_token"));
	if (!cid || !rt) { json_decref(pair); crumb("connect: not paired"); return false; }
	char *e_rt = curl_easy_escape(hh, rt, 0), *e_cid = curl_easy_escape(hh, cid, 0);
	size_t fn = strlen(e_rt) + strlen(e_cid) + 64;
	char *form = malloc(fn);
	snprintf(form, fn, "grant_type=refresh_token&refresh_token=%s&client_id=%s", e_rt, e_cid);
	curl_free(e_rt);
	curl_free(e_cid);
	static const char *H[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
	Buf b = {0};
	nab_t0 = osGetTime();
	no_abort = true;
	long code = creq("POST", TOKEN_URL, H, form, strlen(form), &b);
	no_abort = false;
	free(form);
	json_t *t = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	if (code > 0 && code != 200) crumb("connect: token %ld %.120s", code, b.d ? b.d : "");
	if (code == 400 && b.d && strstr(b.d, "invalid_grant")) { login_dead = true; remove(ACCESS); }
	free(b.d);
	const char *at = json_string_value(json_object_get(t, "access_token"));
	const char *nrt = json_string_value(json_object_get(t, "refresh_token"));
	if (at && strlen(at) < sizeof tok) snprintf(tok, sizeof tok, "%s", at);
	json_int_t life = json_integer_value(json_object_get(t, "expires_in"));
	tok_until = osGetTime() + (u64)(life > 300 ? life - 120 : 180) * 1000;
	if (nrt && strcmp(nrt, rt)) {   // rotated: the old one stops working, so save before anything else
		json_object_set_new(pair, "refresh_token", json_string(nrt));
		if (login_save(pair)) crumb("connect: login rotated, saved");
	}
	if (tok[0]) { login_dead = false; access_save(tok, (long)life); }
	json_decref(t);
	json_decref(pair);
	return tok[0] != 0;
}

static void device_id(void) {
	FILE *f = card_open(DEVFILE, "rb");
	if (f) { size_t n = fread(devid, 1, 40, f); fclose(f); devid[n] = 0; }
	if (strlen(devid) == 40) return;
	srand(osGetTime());
	for (int i = 0; i < 40; i++) devid[i] = "0123456789abcdef"[rand() & 15];
	devid[40] = 0;
	f = fopen(DEVFILE, "wb");
	if (f) { fwrite(devid, 1, 40, f); fclose(f); }
}

static bool apresolve(void) {
	Buf b = {0};
	long code = creq("GET", "https://apresolve.spotify.com/?type=dealer&type=spclient", NULL, NULL, 0, &b);
	json_t *j = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	free(b.d);
	const char *d = json_string_value(json_array_get(json_object_get(j, "dealer"), 0));
	const char *s = json_string_value(json_array_get(json_object_get(j, "spclient"), 0));
	snprintf(dealer, sizeof dealer, "%s", d ? d : "dealer.spotify.com:443");
	snprintf(spclient, sizeof spclient, "%s", s ? s : "spclient.wg.spotify.com:443");
	json_decref(j);
	return true;
}

// ---- the websocket ------------------------------------------------------------------------
// curl here was built without websockets, so it's curl's TLS (CONNECT_ONLY) with the framing done
// by hand: RFC 6455, client frames masked.

static CURL *ws;
static curl_socket_t wsfd = CURL_SOCKET_BAD;
static u8 *rx; static size_t rxn, rxcap;      // bytes read, not yet framed
static u8 *msg; static size_t msgn;           // a message being put together from fragments
static int msg_op;
static u64 ws_last_rx, ws_last_ping;

static bool wait_fd(bool write, int ms) {
	fd_set s;
	FD_ZERO(&s);
	FD_SET(wsfd, &s);
	struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
	return select(wsfd + 1, write ? NULL : &s, write ? &s : NULL, NULL, &tv) > 0;
}

static bool ws_raw_send(const void *d, size_t n) {
	const u8 *p = d;
	u64 t0 = osGetTime();
	while (n && !quitting) {
		size_t sent = 0;
		CURLcode rc = curl_easy_send(ws, p, n, &sent);
		if (rc == CURLE_AGAIN) { if (osGetTime() - t0 > 10000) return false; wait_fd(true, 200); continue; }
		if (rc != CURLE_OK) { crumb("connect: ws send: %s", curl_easy_strerror(rc)); return false; }
		p += sent;
		n -= sent;
	}
	return n == 0;
}

static bool ws_send(int op, const void *d, size_t n) {
	u8 h[14];
	size_t k = 0;
	h[k++] = 0x80 | op;
	if (n < 126) h[k++] = 0x80 | n;
	else if (n < 65536) { h[k++] = 0x80 | 126; h[k++] = n >> 8; h[k++] = n; }
	else { h[k++] = 0x80 | 127; for (int i = 7; i >= 0; i--) h[k++] = (u64)n >> (8 * i); }
	u8 mask[4];
	for (int i = 0; i < 4; i++) mask[i] = rand();
	memcpy(h + k, mask, 4);
	k += 4;
	u8 *f = malloc(k + n);
	if (!f) return false;
	memcpy(f, h, k);
	for (size_t i = 0; i < n; i++) f[k + i] = ((const u8 *)d)[i] ^ mask[i & 3];
	bool ok = ws_raw_send(f, k + n);
	free(f);
	return ok;
}

static void ws_close(void) {
	if (ws) {
		if (wsfd != CURL_SOCKET_BAD) ws_send(8, "\x03\xe8", 2);
		curl_easy_cleanup(ws);
	}
	ws = NULL;
	wsfd = CURL_SOCKET_BAD;
	rxn = msgn = 0;
}

// Reads what's there (waiting up to ms for something); false when the connection is gone.
static bool ws_fill(int ms) {
	bool any = false;
	for (;;) {
		if (rxcap - rxn < 16384) {
			size_t c = rxcap ? rxcap * 2 : 65536;
			u8 *g = realloc(rx, c);
			if (!g) { crumb("connect: ws buffer: no memory for %u", (unsigned)c); return false; }
			rx = g;
			rxcap = c;
		}
		size_t got = 0;
		CURLcode rc = curl_easy_recv(ws, rx + rxn, rxcap - rxn, &got);
		if (rc == CURLE_AGAIN) {
			if (any || ms <= 0) return true;
			if (!wait_fd(false, ms)) return true;
			ms = 0;
			continue;
		}
		if (rc != CURLE_OK) { crumb("connect: ws recv: %s", curl_easy_strerror(rc)); return false; }
		if (got == 0) { crumb("connect: ws closed by Spotify"); return false; }
		rxn += got;
		any = true;
		ws_last_rx = osGetTime();
	}
}

static bool ws_open(void) {
	char url[200];
	snprintf(url, sizeof url, "https://%s/", dealer);
	ws = curl_easy_init();
	if (!ws) return false;
	curl_easy_setopt(ws, CURLOPT_URL, url);
	curl_easy_setopt(ws, CURLOPT_CAINFO, CA);
	curl_easy_setopt(ws, CURLOPT_CONNECT_ONLY, 1L);
	curl_easy_setopt(ws, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(ws, CURLOPT_NOSIGNAL, 1L);
	CURLcode rc = curl_easy_perform(ws);
	if (rc != CURLE_OK) { crumb("connect: dealer: %s", curl_easy_strerror(rc)); ws_close(); return false; }
	curl_easy_getinfo(ws, CURLINFO_ACTIVESOCKET, &wsfd);
	u8 kb[16];
	for (int i = 0; i < 16; i++) kb[i] = rand();
	static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char key[25];
	for (int i = 0, o = 0; i < 16; i += 3) {
		u32 v = kb[i] << 16 | (i + 1 < 16 ? kb[i + 1] << 8 : 0) | (i + 2 < 16 ? kb[i + 2] : 0);
		key[o++] = A[(v >> 18) & 63];
		key[o++] = A[(v >> 12) & 63];
		key[o++] = i + 1 < 16 ? A[(v >> 6) & 63] : '=';
		key[o++] = i + 2 < 16 ? A[v & 63] : '=';
		key[o] = 0;
	}
	char host[128];
	snprintf(host, sizeof host, "%s", dealer);
	char *colon = strchr(host, ':');
	if (colon) *colon = 0;
	size_t qn = strlen(tok) + 512;
	char *q = malloc(qn);
	snprintf(q, qn, "GET /?access_token=%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
	         "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: 3DStereo\r\n\r\n", tok, host, key);
	bool ok = ws_raw_send(q, strlen(q));
	free(q);
	rxn = 0;
	u64 t0 = osGetTime();
	char *end = NULL;
	while (ok && !quitting && osGetTime() - t0 < 10000) {
		if (!ws_fill(500)) { ok = false; break; }
		if (rxn && (end = memmem(rx, rxn, "\r\n\r\n", 4))) break;
	}
	if (!end) { crumb("connect: dealer: no upgrade"); ws_close(); return false; }
	if (rxn < 12 || memcmp(rx + 9, "101", 3)) { crumb("connect: dealer said %.40s", (char *)rx); ws_close(); return false; }
	size_t hl = end + 4 - (char *)rx;
	memmove(rx, rx + hl, rxn - hl);
	rxn -= hl;
	ws_last_rx = ws_last_ping = osGetTime();
	crumb("connect: dealer open (%s)", dealer);
	return true;
}

// The next whole text message (NUL-terminated, caller frees), NULL if none yet; *dead when the
// connection is gone. Pings are answered here.
static char *ws_next(bool *dead) {
	for (;;) {
		if (rxn < 2) return NULL;
		u8 b0 = rx[0], b1 = rx[1];
		size_t k = 2;
		u64 n = b1 & 0x7F;
		if (n == 126) { if (rxn < 4) return NULL; n = rx[2] << 8 | rx[3]; k = 4; }
		else if (n == 127) { if (rxn < 10) return NULL; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | rx[2 + i]; k = 10; }
		if (b1 & 0x80) k += 4;   // servers don't mask, but just in case
		if (n > (8u << 20)) { crumb("connect: ws frame too big (op %d, %llu bytes, %u buffered): lost sync", b0 & 15, (unsigned long long)n, (unsigned)rxn); *dead = true; return NULL; }
		if (rxn < k + n) return NULL;
		u8 *p = rx + k;
		if (b1 & 0x80) for (u64 i = 0; i < n; i++) p[i] ^= rx[k - 4 + (i & 3)];
		int op = b0 & 0x0F;
		bool fin = b0 & 0x80;
		char *out = NULL;
		if (op == 9) ws_send(10, p, n);               // ping: pong it back
		else if (op == 8) {                           // close: Spotify says why
			int code = n >= 2 ? p[0] << 8 | p[1] : 0;
			crumb("connect: ws close from Spotify, code %d: %.*s", code, n > 2 ? (int)(n - 2 > 120 ? 120 : n - 2) : 0, n > 2 ? (char *)p + 2 : "");
			*dead = true;
		}
		else if (op == 1 || op == 2 || op == 0) {
			if (op) { msgn = 0; msg_op = op; }
			u8 *g = realloc(msg, msgn + n + 1);
			if (g) { msg = g; memcpy(msg + msgn, p, n); msgn += n; msg[msgn] = 0; }
			if (op == 0 || !fin) crumb("connect: ws fragment op %d fin %d %llu bytes (%u so far)", op, fin, (unsigned long long)n, (unsigned)msgn);
			if (op == 2 && fin) crumb("connect: ws binary message, %u bytes (ignored)", (unsigned)msgn);
			if (fin && msg_op == 1) { out = malloc(msgn + 1); if (out) memcpy(out, msg, msgn + 1); msgn = 0; }
		}
		else if (op != 10) crumb("connect: ws frame op %d fin %d %llu bytes", op, fin, (unsigned long long)n);
		memmove(rx, rx + k + n, rxn - k - n);
		rxn -= k + n;
		if (out || *dead) return out;
	}
}

// ---- payloads -----------------------------------------------------------------------------

// base64, then gunzip if it is gzip: the bytes (caller frees), *n their count
static u8 *unpack(const char *b64, bool gz, size_t *n) {
	u8 *raw = malloc(strlen(b64) / 4 * 3 + 4);
	if (!raw) return NULL;
	long rn = b64_decode(b64, raw);
	if (rn < 0) { free(raw); return NULL; }
	if (!gz && !(rn > 2 && raw[0] == 0x1f && raw[1] == 0x8b)) { *n = rn; raw[rn] = 0; return raw; }
	z_stream z = {0};
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) { free(raw); return NULL; }
	size_t cap = rn * 4 + 1024, on = 0;
	u8 *out = malloc(cap + 1);
	z.next_in = raw;
	z.avail_in = rn;
	int r = Z_OK;
	while (out && r == Z_OK) {
		if (cap - on < 4096) { cap *= 2; u8 *g = realloc(out, cap + 1); if (!g) { free(out); out = NULL; break; } out = g; }
		z.next_out = out + on;
		z.avail_out = cap - on;
		r = inflate(&z, Z_NO_FLUSH);
		on = cap - z.avail_out;
	}
	inflateEnd(&z);
	free(raw);
	if (!out || r != Z_STREAM_END) { free(out); return NULL; }
	out[on] = 0;
	*n = on;
	return out;
}

static const char *jstr(json_t *o, const char *k) { return json_string_value(json_object_get(o, k)); }

// ---- songs: Spotify's to the card's -------------------------------------------------------
// Every match is kept: in memory, and the ones on the card in sdmc:/3ds/player/connect-ids too
// (seeded from the sync's Liked Songs), so a known song needs no network at all. That's what
// lets the music go on with the lid shut, when the 3DS's Wi-Fi is off. A whole album or playlist
// is matched in one go from the Web API (Spotify refuses ?ids= batches to Pocket's app now).
#define IDS DIR3DS "/connect-ids"
#define SYNC DIR3DS "/spotify-sync.json"
typedef struct { char id[23]; int local; } Seen;
static Seen *seen; static int nseen, seen_cap;
static CURL *webh;   // Pocket's own pairing, for the Web API (the desktop id is rate-limited there)
static char webtok[1200];

static Seen *seen_get(const char *id) {
	for (int i = nseen - 1; i >= 0; i--) if (!strcmp(seen[i].id, id)) return &seen[i];
	return NULL;
}

static void seen_put(const char *id, int local, bool save) {
	Seen *s = seen_get(id);
	if (s) { if (s->local == local) return; s->local = local; }
	else {
		if (nseen == seen_cap) {
			int c = seen_cap ? seen_cap * 2 : 512;
			Seen *g = realloc(seen, c * sizeof *g);
			if (!g) return;
			seen = g; seen_cap = c;
		}
		snprintf(seen[nseen].id, sizeof seen[nseen].id, "%s", id);
		seen[nseen++].local = local;
	}
	if (save && local >= 0 && local < lib.nt) {   // by path: the library's indices change between launches
		FILE *f = fopen(IDS, "a");
		if (f) { fprintf(f, "%s\t%s\n", id, lib.t[local].path); fclose(f); }
	}
}

static int rel_find(const char *rel) {   // a playlist's "../Album/x.flac", or a whole path
	char full[1024];
	if (!strncmp(rel, "../", 3)) snprintf(full, sizeof full, "sdmc:/music/%s", rel + 3);
	else snprintf(full, sizeof full, "%s", rel);
	for (char *c = full; *c; c++) if (*c == '\\') *c = '/';
	return library_find(full);
}

static void seed(void) {
	int n0 = nseen;
	FILE *f = card_open_big(IDS, "rb");
	char line[1200];
	while (f && fgets(line, sizeof line, f)) {
		char *tab = strchr(line, '\t');
		if (!tab || tab - line != 22) continue;
		*tab = 0;
		char *nl = strpbrk(tab + 1, "\r\n");
		if (nl) *nl = 0;
		int i = rel_find(tab + 1);
		if (i >= 0) seen_put(line, i, false);
	}
	if (f) fclose(f);
	int from_ids = nseen - n0;
	f = card_open_big(SYNC, "rb");
	json_t *j = f ? json_loadf(f, 0, NULL) : NULL;
	if (f) fclose(f);
	json_t *liked = json_object_get(j, "liked");
	for (size_t k = 0; k < json_array_size(liked); k++) {
		json_t *e = json_array_get(liked, k);
		const char *id = jstr(e, "id"), *rel = jstr(e, "rel");
		if (!id || !rel || strlen(id) != 22 || seen_get(id)) continue;
		int i = rel_find(rel);
		if (i >= 0) seen_put(id, i, false);
	}
	json_decref(j);
	crumb("connect: %d songs known (%d from connect-ids)", nseen - n0, from_ids);
}

// A Web API GET with Pocket's own login: 200's body parsed, or NULL. *net_down: it never got an answer.
static json_t *web_get(const char *path, bool *net_down) {
	char url[600], why[48];
	snprintf(url, sizeof url, "%s%s", strncmp(path, "http", 4) ? "https://api.spotify.com/v1" : "", path);
	*net_down = false;
	if (!webh && !(webh = net_handle())) { *net_down = true; return NULL; }
	for (int tries = 0; tries < 3 && !quitting; tries++) {
		if (!webtok[0] && !spot_token(webh, NULL, webtok, sizeof webtok, why, sizeof why, &quitting)) {
			crumb("connect: %s", why);
			*net_down = true;
			return NULL;
		}
		char auth[1300];
		snprintf(auth, sizeof auth, "Authorization: Bearer %s", webtok);
		Buf b = {0};
		long ra = 0;
		CURLcode cr;
		long code = net_req(webh, "GET", url, NULL, auth, &b, &ra, &quitting, &cr);
		if (code == 401) {
			char st[1200];
			snprintf(st, sizeof st, "%s", webtok);
			webtok[0] = 0;
			spot_token(webh, st, webtok, sizeof webtok, why, sizeof why, &quitting);
			free(b.d);
			continue;
		}
		if (code == 429) { free(b.d); svcSleepThread((ra > 0 && ra < 5 ? ra : 2) * 1000000000LL); continue; }
		json_t *t = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
		free(b.d);
		if (code < 0) *net_down = true;
		return t;
	}
	*net_down = true;
	return NULL;
}

static int match_json(json_t *t) {   // a Web API track object, matched to the card
	SmTrack s = {0};
	s.name = json_string_value(json_object_get(t, "name"));
	json_t *ar = json_object_get(t, "artists");
	for (size_t k = 0; k < json_array_size(ar) && s.na < 8; k++) {
		const char *a = json_string_value(json_object_get(json_array_get(ar, k), "name"));
		if (a) s.artists[s.na++] = a;
	}
	s.ms = json_integer_value(json_object_get(t, "duration_ms"));
	return s.name ? sm_match(&s) : -1;
}

// 1: known (*local -1 when it's not on the card); 0: no answer (no Wi-Fi), so nobody knows
static int lookup(const char *uri, int *local, int *dur) {
	const char *id = strrchr(uri, ':');
	id = id ? id + 1 : uri;
	*local = -1; *dur = 0;
	if (strncmp(uri, "spotify:track:", 14)) return 1;   // episodes, local files: never on the card
	Seen *s = seen_get(id);
	if (!s) {
		char path[64];
		snprintf(path, sizeof path, "/tracks/%s", id);
		bool down;
		json_t *t = web_get(path, &down);
		if (!t && down) return 0;
		int i = t ? match_json(t) : -1;   // a 404: gone from Spotify, so not on the card either
		crumb("connect: %s %s -> %d", id, t ? jstr(t, "name") : "?", i);
		json_decref(t);
		seen_put(id, i, true);
		s = seen_get(id);
		if (!s) return 0;
	}
	*local = s->local;
	*dur = s->local >= 0 && s->local < lib.nt ? (int)lib.t[s->local].secs * 1000 : 0;
	return 1;
}

// A whole album or playlist matched at once (a page of 50 per request), before its songs come up
static void fill_context(const char *uri) {
	const char *id = strrchr(uri, ':');
	if (!id) return;
	id++;
	char next[600];
	bool album = !strncmp(uri, "spotify:album:", 14), list = !strncmp(uri, "spotify:playlist:", 17);
	if (album) snprintf(next, sizeof next, "/albums/%s/tracks?limit=50", id);
	else if (list) snprintf(next, sizeof next, "/playlists/%s/items?limit=50", id);
	else return;
	int got = 0, on = 0;
	for (int pages = 0; next[0] && pages < 20 && !quitting; pages++) {
		bool down;
		json_t *j = web_get(next, &down);
		next[0] = 0;
		if (!j) break;
		json_t *items = json_object_get(j, "items");
		for (size_t k = 0; k < json_array_size(items); k++) {
			json_t *it = json_array_get(items, k), *t = it;
			if (list) { t = json_object_get(it, "item"); if (!json_is_object(t)) t = json_object_get(it, "track"); }
			const char *u = jstr(t, "uri");
			if (!u || strncmp(u, "spotify:track:", 14) || seen_get(u + 14)) continue;
			int i = match_json(t);
			seen_put(u + 14, i, true);
			got++;
			on += i >= 0;
		}
		const char *nx = jstr(j, "next");
		if (nx) snprintf(next, sizeof next, "%s", nx);
		json_decref(j);
	}
	crumb("connect: %s matched in bulk: %d new, %d on the card", uri, got, on);
}

// ---- the context --------------------------------------------------------------------------

static void ctx_clear(void) { c_n = ord_n = pos = 0; nx_pos = pend_pos = -1; filled = false; warm_at = 0; }

static void ctx_add(const char *uri, const char *uid, bool queued) {
	if (c_n >= MAXCTX + 50) return;
	if (!(c_n & 63)) {
		void *a = realloc(c_uri, (c_n + 64) * sizeof *c_uri), *b = realloc(c_uid, (c_n + 64) * sizeof *c_uid);
		void *c = realloc(c_q, (c_n + 64) * sizeof *c_q), *d = realloc(ord, (c_n + 64) * sizeof *ord);
		if (a) c_uri = a;
		if (b) c_uid = b;
		if (c) c_q = c;
		if (d) ord = d;
		if (!a || !b || !c || !d) return;
	}
	snprintf(c_uri[c_n], sizeof c_uri[c_n], "%s", uri);
	if (uid && *uid) snprintf(c_uid[c_n], sizeof c_uid[c_n], "%s", uid);
	else for (int i = 0; i < 16; i++) c_uid[c_n][i] = "0123456789abcdef"[rand() & 15], c_uid[c_n][16] = 0;
	c_q[c_n] = queued;
	ord[ord_n++] = c_n++;
}

static void reshuffle(void) {   // everything after the current song, in a new order
	for (int i = ord_n - 1; i > pos + 1; i--) { int j = pos + 1 + rand() % (i - pos), t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
}

static void unshuffle(void) {
	int c = ord_n ? ord[pos] : 0;
	for (int i = 0; i < ord_n; i++) ord[i] = i;
	pos = c < ord_n ? c : 0;
}

static void spget_json(const char *path, json_t **out) {
	char url[600], auth[1300];
	snprintf(url, sizeof url, "https://%s%s", spclient, path);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", tok);
	const char *H[] = {auth, "Accept: application/json", NULL};
	Buf b = {0};
	long code = creq("GET", url, H, NULL, 0, &b);
	*out = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	if (code != 200) crumb("connect: %s -> %ld %.100s", path, code, b.d ? b.d : "");
	free(b.d);
}

static void page_tracks(json_t *pg, int *fetched, json_t *later);

// librespot's get_context: any context, artist lists and stations too, page by page
static void resolve(const char *uri) {
	char path[400];
	snprintf(path, sizeof path, "/context-resolve/v1/%s", uri);
	json_t *c = NULL;
	spget_json(path, &c);
	json_t *pages = json_object_get(c, "pages"), *later = json_array();
	int fetched = 0;
	for (size_t i = 0; i < json_array_size(pages); i++) json_array_append(later, json_array_get(pages, i));
	while (json_array_size(later) && c_n < MAXCTX && fetched < 20 && !quitting) {
		json_t *pg = json_incref(json_array_get(later, 0));
		json_array_remove(later, 0);
		page_tracks(pg, &fetched, later);
		json_decref(pg);
	}
	json_decref(later);
	json_decref(c);
}

static void page_tracks(json_t *pg, int *fetched, json_t *later) {
	json_t *tr = json_object_get(pg, "tracks");
	json_t *own = NULL;
	if (!json_array_size(tr)) {
		const char *u = json_string_value(json_object_get(pg, "page_url"));
		if (!u) return;
		if (!strncmp(u, "hm://", 5)) u += 4;
		spget_json(u, &own);
		(*fetched)++;
		pg = own;
		tr = json_object_get(pg, "tracks");
	}
	for (size_t k = 0; k < json_array_size(tr) && c_n < MAXCTX; k++) {
		json_t *t = json_array_get(tr, k);
		const char *u = json_string_value(json_object_get(t, "uri"));
		char g[40];
		if (!u || !*u) {   // gid only
			const char *gid = json_string_value(json_object_get(t, "gid"));
			u8 raw[24];
			long n = gid ? b64_decode(gid, raw) : -1;
			if (n != 16) continue;
			char b62[23];
			gid_b62(raw, 16, b62);
			snprintf(g, sizeof g, "spotify:track:%s", b62);
			u = g;
		}
		if (!strncmp(u, "spotify:track:", 14)) ctx_add(u, json_string_value(json_object_get(t, "uid")), false);
	}
	const char *nx = json_string_value(json_object_get(pg, "next_page_url"));
	if (nx) { json_t *o = json_object(); json_object_set_new(o, "page_url", json_string(nx)); json_array_append_new(later, o); }
	json_decref(own);
}

static int find(const char *uri, const char *uid) {   // its place in ord, -1 if not there
	for (int i = 0; i < ord_n; i++) if (uid && *uid && !strcmp(c_uid[ord[i]], uid)) return i;
	for (int i = 0; i < ord_n; i++) if (uri && *uri && !strcmp(c_uri[ord[i]], uri)) return i;
	return -1;
}

static void new_ids(void) {
	for (int i = 0; i < 32; i++) session[i] = "0123456789abcdef"[rand() & 15], playback[i] = "0123456789abcdef"[rand() & 15];
	session[32] = playback[32] = 0;
}

// ---- the state up to Spotify --------------------------------------------------------------

static char conn_id[256];

static int64_t heard_ms(void) {   // (lk) where the song is on Spotify's clock now
	if (!loaded) return 0;
	if (osGetTime() < trust_rep_until) return rep_pos + (paused ? 0 : now_ms() - rep_at);
	if (snap_open && snap_track == cur_local) {
		double p = snap_pos * 1000;
		if (!snap_paused) p += (double)(osGetTime() - snap_at);
		return (int64_t)p;
	}
	return rep_pos + (paused ? 0 : now_ms() - rep_at);
}

static long put_state(int reason) {
	LightLock_Lock(&lk);
	int lo = pos - 10 > 0 ? pos - 10 : 0, hi = pos + 81 < ord_n ? pos + 81 : ord_n;
	int wn = loaded ? hi - lo : 0;
	const char **u = malloc((wn + 1) * sizeof *u), **d = malloc((wn + 1) * sizeof *d);
	bool *q = malloc(wn + 1);
	for (int i = 0; i < wn; i++) { int c = ord[lo + i]; u[i] = c_uri[c]; d[i] = c_uid[c]; q[i] = c_q[c]; }
	int64_t now = now_ms(), p = heard_ms();
	rep_pos = p;
	rep_at = now;
	char ver[32];
	snprintf(ver, sizeof ver, "pocket %s", VERSION);
	CState s = {.name = dev_name, .device_id = devid, .volume = volume, .client_id = "65b708073fc0480ea92a077233ca87bd", .version = ver,
	            .active = active, .loaded = loaded && wn > 0, .paused = paused, .reason = reason, .now_ms = now,
	            .pos_ms = p, .dur_ms = cur_dur, .started_at = started_at, .last_msg_id = last_id, .last_from = last_from,
	            .ctx_uri = ctx_uri, .session_id = session, .playback_id = playback, .uris = u, .uids = d, .queued = q,
	            .n = wn, .index = pos - lo, .origin = origin, .norigin = norigin, .ctx_restr = ctx_restr,
	            .nctx_restr = nctx_restr, .shuffle = shuffle, .rep_ctx = rep_ctx, .rep_track = rep_track,
	            .next_max = 80, .prev_max = 10};
	Pb b = {0};
	cpb_state(&b, &s);
	bool was_paused = paused, was_active = active;
	LightLock_Unlock(&lk);
	free(u); free(d); free(q);
	char url[300], auth[1300], cid[300];
	snprintf(url, sizeof url, "https://%s/connect-state/v1/devices/%s", spclient, devid);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", tok);
	snprintf(cid, sizeof cid, "X-Spotify-Connection-Id: %s", conn_id);
	const char *H[] = {auth, cid, "Content-Type: application/x-protobuf", NULL};
	Buf r = {0};
	long code = creq("PUT", url, H, b.d, b.n, &r);
	if (code != 200) {
		char why[101] = "";   // only the printable start of the body: a cut-off request left protobuf bytes here, and the log showed them as garbage
		for (size_t i = 0, k = 0; code > 0 && r.d && i < r.n && k < 100; i++) if ((unsigned char)r.d[i] >= 32 && (unsigned char)r.d[i] < 127) why[k++] = r.d[i], why[k] = 0;
		crumb("connect: state %ld %s", code, why);
	}
	else crumb("connect: state up (%s, %s, %lld ms)", was_active ? "active" : "idle", was_paused ? "paused" : "playing", (long long)p);
	free(r.d);
	pb_free(&b);
	return code;
}

static void put_inactive(void) {
	char url[300], auth[1300], cid[300];
	snprintf(url, sizeof url, "https://%s/connect-state/v1/devices/%s/inactive?notify=false", spclient, devid);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", tok);
	snprintf(cid, sizeof cid, "X-Spotify-Connection-Id: %s", conn_id);
	const char *H[] = {auth, cid, NULL};
	Buf r = {0};
	creq("PUT", url, H, NULL, 0, &r);
	free(r.d);
}

// ---- playing a place in the context -------------------------------------------------------

static void post(ConnectDo d) {   // (lk) for main
	trust_rep_until = osGetTime() + 2000;
	if (todo_set && !d.say[0] && todo.say[0]) snprintf(d.say, sizeof d.say, "%s", todo.say);
	todo = d;
	todo_set = true;
}

static void say(const char *s) {   // (lk)
	if (todo_set) snprintf(todo.say, sizeof todo.say, "%s", s);
	else { todo = (ConnectDo){.kind = CD_NONE}; snprintf(todo.say, sizeof todo.say, "%s", s); todo_set = true; }
}

static void src_line(char *out, int cap) {
	const char *k = !strncmp(ctx_uri, "spotify:album:", 14) ? "AN ALBUM" : !strncmp(ctx_uri, "spotify:playlist:", 17) ? "A PLAYLIST"
	              : strstr(ctx_uri, "artist") ? "AN ARTIST" : "SPOTIFY";
	snprintf(out, cap, "SPOTIFY: %s", k);
}

// From place p on (forward, wrapping only with repeat), the first song that's on the card.
// Called without lk (lookups go to the network); reads ord/c_uri, which only this thread changes.
static int first_on_card(int p, int dir, int *local, int *dur, bool *wrapped) {
	*wrapped = false;
	for (int k = 0; k < LOOK && ord_n; k++, p += dir) {
		if (p >= ord_n) { if (!rep_ctx) return -1; p = 0; *wrapped = true; }
		if (p < 0) { if (!rep_ctx) return -1; p = ord_n - 1; *wrapped = true; }
		if (quitting) return -1;
		if (lookup(c_uri[ord[p]], local, dur) && *local >= 0) return p;   // no Wi-Fi: unknown, so on to one we know
	}
	return -1;
}

static void find_next(void) {   // what follows, ready for the audio worker
	int local = -1, dur = 0;
	bool w;
	int p = rep_track ? pos : first_on_card(pos + 1, 1, &local, &dur, &w);
	if (rep_track) { local = cur_local; dur = cur_dur; }
	LightLock_Lock(&lk);
	nx_pos = p; nx_local = p >= 0 ? local : -1; nx_dur = dur; nx_done = true;
	LightLock_Unlock(&lk);
}

// Play ord[p] (or the first song from it that's on the card) at ms; false if nothing was.
static bool load(int p, int dir, int64_t ms, bool want_paused) {
	int local, dur;
	bool w;
	int at = first_on_card(p, dir, &local, &dur, &w);
	LightLock_Lock(&lk);
	if (at < 0) {
		loaded = true;
		paused = true;
		pos = p >= 0 && p < ord_n ? p : 0;
		cur_local = -1;
		post((ConnectDo){.kind = CD_PAUSE, .paused = true});
		say("None of these are on the card");
		LightLock_Unlock(&lk);
		return false;
	}
	if (at != p) {
		char t[64];
		snprintf(t, sizeof t, "Not on the card, skipped %d", (at - p) * dir > 0 ? (at - p) * dir : 1);
		say(t);
		ms = 0;
	}
	if (dur > 0 && ms >= dur - 1000) ms = 0;   // a stale position past the end: from the top
	pos = at; cur_local = local; cur_dur = dur; loaded = true; paused = want_paused;
	rep_pos = ms; rep_at = now_ms();
	for (int i = 0; i < 32; i++) playback[i] = "0123456789abcdef"[rand() & 15];
	nx_pos = pend_pos = -1;
	nx_done = false;
	ConnectDo d = {.kind = CD_LOAD, .track = local, .secs = ms / 1000.0, .paused = want_paused};
	src_line(d.src, sizeof d.src);
	snprintf(d.ctx, sizeof d.ctx, "%.95s", ctx_uri);   // uris are short: 95 holds any
	post(d);
	LightLock_Unlock(&lk);
	return true;
}

static void activate(void) {   // (lk)
	if (!active) { active = true; started_at = now_ms(); new_ids(); { char sb[96]; snprintf(sb, sizeof sb, "Spotify is playing on %s", dev_name); say(sb); } vol_due = true; }
}

// ---- commands -----------------------------------------------------------------------------


static void origin_from_json(json_t *po) {   // (lk) PlayOrigin's fields, from the command's JSON
	Pb b = {0};
	static const char *F[] = {NULL, "feature_identifier", "feature_version", "view_uri", "external_referrer", "referrer_identifier", "device_identifier"};
	for (int f = 1; f <= 6; f++) pb_str(&b, f, jstr(po, F[f]));
	json_t *fc = json_object_get(po, "feature_classes");
	for (size_t i = 0; i < json_array_size(fc); i++) pb_str(&b, 7, json_string_value(json_array_get(fc, i)));
	norigin = b.n < sizeof origin ? b.n : 0;
	if (norigin) memcpy(origin, b.d, norigin);
	pb_free(&b);
}

// ---- the other way: the 3DS's song as Spotify's ------------------------------------------

// spotify:local:artist:album:title:secs, which Spotify shows as a local file (its own words + for spaces)
static void local_uri(int i, char *out, int cap) {
	const Track *t = &lib.t[i];
	const char *parts[3] = {t->artist ? t->artist : "", t->album ? t->album : "", t->title ? t->title : ""};
	int o = snprintf(out, cap, "spotify:local");
	for (int k = 0; k < 3 && o < cap - 4; k++) {
		out[o++] = ':';
		for (const unsigned char *c = (const unsigned char *)parts[k]; *c && o < cap - 4; c++) {
			if (*c == ' ') out[o++] = '+';
			else if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c)) out[o++] = *c;
			else o += snprintf(out + o, cap - o, "%%%02X", *c);
		}
	}
	snprintf(out + o, cap - o, ":%u", (unsigned)t->secs);
}

// The 3DS's song i as a Spotify uri: known already, or (net) searched for by title and artist with
// the same rules as the sync; a local-file uri when Spotify hasn't got it.
static void rev_lookup(int i, bool net, char *out, int cap) {
	for (int k = nseen - 1; k >= 0; k--) if (seen[k].local == i) { snprintf(out, cap, "spotify:track:%s", seen[k].id); return; }
	if (net && !webh) webh = net_handle();
	if (net && i >= 0 && i < lib.nt && webh) {
		char artist[256] = "", q[700], path[1200];
		sm_artist(i, 1, artist, sizeof artist);
		snprintf(q, sizeof q, "track:\"%s\" artist:\"%s\"", sm_title(i) ? sm_title(i) : (lib.t[i].title ? lib.t[i].title : ""), artist);
		char *e = curl_easy_escape(webh, q, 0);
		snprintf(path, sizeof path, "/search?type=track&limit=5&q=%s", e ? e : "");
		curl_free(e);
		bool down;
		json_t *j = web_get(path, &down);
		json_t *items = json_object_get(json_object_get(j, "tracks"), "items");
		for (size_t k = 0; k < json_array_size(items); k++) {
			json_t *t = json_array_get(items, k);
			const char *u = jstr(t, "uri");
			SmTrack st = {0};
			st.name = jstr(t, "name");
			json_t *ar = json_object_get(t, "artists");
			for (size_t a = 0; a < json_array_size(ar) && st.na < 8; a++) {
				const char *an = jstr(json_array_get(ar, a), "name");
				if (an) st.artists[st.na++] = an;
			}
			st.ms = json_integer_value(json_object_get(t, "duration_ms"));
			if (u && !strncmp(u, "spotify:track:", 14) && st.name && sm_matches(&st, i, 4)) {
				seen_put(u + 14, i, true);
				snprintf(out, cap, "%s", u);
				json_decref(j);
				crumb("connect: the 3DS's %d is %s", i, u);
				return;
			}
		}
		json_decref(j);
	}
	local_uri(i, out, cap);
}

static void album_of(const char *uri, char *out, int cap) {   // a track's album, from the Web API
	char path[64];
	snprintf(path, sizeof path, "/tracks/%.22s", uri + 14);
	bool down;
	json_t *t = web_get(path, &down);
	const char *a = jstr(json_object_get(t, "album"), "uri");
	if (a) snprintf(out, cap, "%s", a);
	json_decref(t);
}

static void resolve(const char *uri);
static int find(const char *uri, const char *uid);

// Spotify drives the 3DS's album or playlist from the song already playing, without reloading it
static bool adopt_ctx(const char *ctx, const char *uri, int t, int64_t at, bool pz) {
	LightLock_Lock(&lk);
	ctx_clear();
	snprintf(ctx_uri, sizeof ctx_uri, "%s", ctx);
	norigin = nctx_restr = 0;
	opts_from_3ds();   // the 3DS's shuffle and repeat go with it
	LightLock_Unlock(&lk);
	resolve(ctx);
	int p = find(uri, NULL);
	if (p < 0) { crumb("connect: %s isn't in %s, Spotify just follows", uri, ctx); return false; }
	LightLock_Lock(&lk);
	pos = p; cur_local = t; cur_dur = (int64_t)lib.t[t].secs * 1000; loaded = true; paused = pz;
	rep_pos = at; rep_at = now_ms();
	for (int i = 0; i < 32; i++) playback[i] = "0123456789abcdef"[rand() & 15];
	mirror = false;
	nx_pos = pend_pos = -1; nx_done = false; filled = false;
	LightLock_Unlock(&lk);
	crumb("connect: Spotify takes the 3DS's %s from song %d", ctx, p);
	return true;
}

// Spotify shows what the 3DS plays: the song and the 3DS's own next few as the context
static void do_mirror(void) {
	LightLock_Lock(&lk);
	int t = snap_open ? snap_track : -1, nu = n_up, nq = n_upq, up[4];
	memcpy(up, up_next, sizeof up);
	bool pz = snap_paused;
	int64_t at = (int64_t)(snap_pos * 1000);
	mirror_due = false;
	LightLock_Unlock(&lk);
	if (t < 0 || t >= lib.nt) return;
	char uri[192];
	rev_lookup(t, true, uri, sizeof uri);
	// the 3DS is in an album or a synced playlist (or a queue of one, left over from Spotify): Spotify
	// gets that whole context and drives on from this song, which keeps playing as it is
	char want[256] = "";
	LightLock_Lock(&lk);
	int sk = src_kind_;
	bool single = src_single;
	if (sk == SRC_PLAYLIST && src_pid[0]) snprintf(want, sizeof want, "spotify:playlist:%s", src_pid);
	LightLock_Unlock(&lk);
	if (!want[0] && (sk == SRC_ALBUM || single) && !strncmp(uri, "spotify:track:", 14)) album_of(uri, want, sizeof want);
	if (want[0] && adopt_ctx(want, uri, t, at, pz)) return;
	LightLock_Lock(&lk);
	ctx_clear();
	snprintf(ctx_uri, sizeof ctx_uri, "%s", uri);
	norigin = nctx_restr = 0;
	opts_from_3ds();   // Spotify follows the 3DS: its shuffle and repeat too
	ctx_add(uri, NULL, false);
	for (int k = 0; k < nu; k++) {
		char u[192];
		rev_lookup(up[k], false, u, sizeof u);   // the known ones only: no waiting on the network for these
		ctx_add(u, NULL, k < nq);   // the ones queued by hand are Spotify's queue, the rest its context
	}
	pos = 0; cur_local = t; cur_dur = (int64_t)lib.t[t].secs * 1000; loaded = true; paused = pz;
	rep_pos = at; rep_at = now_ms();
	for (int i = 0; i < 32; i++) playback[i] = "0123456789abcdef"[rand() & 15];
	nx_pos = pend_pos = -1; nx_done = true;
	filled = true;
	LightLock_Unlock(&lk);
	crumb("connect: Spotify follows the 3DS: %s", uri);
}

static void cmd_transfer(json_t *cmd) {
	const char *data = jstr(cmd, "data");
	if (!data) return;
	u8 *raw = malloc(strlen(data) / 4 * 3 + 4);
	long n = b64_decode(data, raw);
	static char pu[64][64], pi[64][64];
	Transfer t = {.page_uris = pu, .page_uids = pi, .npages_max = 64};
	if (n <= 0 || !cpb_transfer(raw, n, &t)) { free(raw); crumb("connect: transfer unreadable"); return; }
	const char *rp = jstr(json_object_get(cmd, "options"), "restore_paused");
	bool want_paused = rp && !strcmp(rp, "pause") ? true : rp && !strcmp(rp, "resume") ? false : t.paused;
	int64_t ms = t.pos;
	if (!t.paused && t.timestamp > 0) ms += now_ms() - t.timestamp;
	crumb("connect: transfer %s %s at %lld ms%s", t.ctx_uri, t.track_uri, (long long)ms, want_paused ? " paused" : "");
	// Pocket's already playing: Spotify takes what's on the 3DS, not its own old state (build 45's
	// came at 459639 ms, its own stale state, and the song ended on the spot)
	LightLock_Lock(&lk);
	bool here = snap_open && !snap_paused && snap_track >= 0 && osGetTime() - snap_at < 2000;
	bool driving = active && !mirror;
	if (here) { activate(); if (!driving) mirror = true; }
	LightLock_Unlock(&lk);
	if (here) {
		free(raw);
		if (!driving) do_mirror();
		return;
	}
	LightLock_Lock(&lk);
	ctx_clear();
	snprintf(ctx_uri, sizeof ctx_uri, "%s", t.ctx_uri[0] ? t.ctx_uri : t.track_uri);
	norigin = t.norigin < sizeof origin ? t.norigin : 0;
	if (norigin) memcpy(origin, t.origin, norigin);
	nctx_restr = t.nctx_restr < sizeof ctx_restr ? t.nctx_restr : 0;
	if (nctx_restr) memcpy(ctx_restr, t.ctx_restr, nctx_restr);
	shuffle = t.shuffle; rep_ctx = t.rep_ctx; rep_track = t.rep_track;
	opts_from_spotify();
	activate();
	LightLock_Unlock(&lk);
	free(raw);
	LightLock_Lock(&lk);
	mirror = false;
	LightLock_Unlock(&lk);
	// librespot: a context uri is always resolved in full; the transfer's pages are only a slice
	if (t.ctx_uri[0] && strncmp(t.ctx_uri, "spotify:track:", 14)) resolve(t.ctx_uri);
	if (c_n <= 1) { ctx_clear(); for (int i = 0; i < t.npage_tracks; i++) ctx_add(pu[i], pi[i], false); }
	int p = find(t.track_uri, t.current_uid[0] ? t.current_uid : t.track_uid);
	if (p < 0) { ctx_add(t.track_uri, t.current_uid, false); p = ord_n - 1; }
	LightLock_Lock(&lk);
	pos = p;
	if (shuffle) reshuffle();
	LightLock_Unlock(&lk);
	load(p, 1, ms, want_paused);
}

static void cmd_play(json_t *cmd) {
	json_t *ctx = json_object_get(cmd, "context"), *opt = json_object_get(cmd, "options");
	json_t *skip = json_object_get(opt, "skip_to");
	const char *uri = jstr(ctx, "uri");
	LightLock_Lock(&lk);
	mirror = false;
	ctx_clear();
	snprintf(ctx_uri, sizeof ctx_uri, "%s", uri ? uri : "");
	origin_from_json(json_object_get(cmd, "play_origin"));
	nctx_restr = 0;
	json_t *ov = json_object_get(opt, "player_options_override");
	if (json_is_boolean(json_object_get(ov, "shuffling_context"))) shuffle = json_is_true(json_object_get(ov, "shuffling_context"));
	if (json_is_boolean(json_object_get(ov, "repeating_context"))) rep_ctx = json_is_true(json_object_get(ov, "repeating_context"));
	if (json_is_boolean(json_object_get(ov, "repeating_track"))) rep_track = json_is_true(json_object_get(ov, "repeating_track"));
	if (ov) opts_from_spotify();
	activate();
	LightLock_Unlock(&lk);
	json_t *pages = json_object_get(ctx, "pages");
	bool inline_tracks = false;
	for (size_t i = 0; i < json_array_size(pages); i++) if (json_array_size(json_object_get(json_array_get(pages, i), "tracks"))) inline_tracks = true;
	if (uri && *uri && strncmp(uri, "spotify:track:", 14)) resolve(uri);
	if (!c_n && inline_tracks) {
		json_t *later = json_array();
		int f = 0;
		for (size_t i = 0; i < json_array_size(pages); i++) page_tracks(json_array_get(pages, i), &f, later);
		json_decref(later);
		if (!ctx_uri[0]) snprintf(ctx_uri, sizeof ctx_uri, "%s", c_n ? c_uri[0] : "");
	}
	if (!c_n && uri && !strncmp(uri, "spotify:track:", 14)) ctx_add(uri, NULL, false);
	const char *tu = jstr(skip, "track_uri"), *ti = jstr(skip, "track_uid");
	int p = (tu || ti) ? find(tu, ti) : -1;
	if (p < 0 && json_is_integer(json_object_get(skip, "track_index"))) p = json_integer_value(json_object_get(skip, "track_index"));
	if (p < 0 && tu) { ctx_add(tu, ti, false); p = ord_n - 1; }
	if (p < 0 || p >= ord_n) p = 0;
	LightLock_Lock(&lk);
	pos = p;
	if (shuffle) {
		if (!tu && !ti && !json_is_integer(json_object_get(skip, "track_index")) && ord_n) {   // shuffle play: anywhere
			int r = rand() % ord_n, t = ord[0]; ord[0] = ord[r]; ord[r] = t; pos = p = 0;
		}
		reshuffle();
	}
	LightLock_Unlock(&lk);
	json_int_t seek = json_integer_value(json_object_get(opt, "seek_to"));
	bool ip = json_is_true(json_object_get(opt, "initially_paused"));
	load(p, 1, seek, ip);
}

static void cmd_skip(int dir, const char *to_uri) {
	LightLock_Lock(&lk);
	if (mirror) {   // the 3DS's own queue decides; Spotify hears the new song from connect_poll
		post((ConnectDo){.kind = CD_SKIP, .dir = dir});
		LightLock_Unlock(&lk);
		return;
	}
	bool restart = dir < 0 && heard_ms() > 3000;
	int p = pos + dir;
	if (to_uri) { int f = find(to_uri, NULL); if (f >= 0) p = f; }
	bool was_paused = paused;
	if (restart) {
		rep_pos = 0; rep_at = now_ms();
		post((ConnectDo){.kind = CD_SEEK, .secs = 0});
		LightLock_Unlock(&lk);
		return;
	}
	if (p >= ord_n && !rep_ctx) {   // the end: back to the top, paused, like Spotify does
		LightLock_Unlock(&lk);
		load(0, 1, 0, true);
		return;
	}
	if (p < 0) p = rep_ctx ? ord_n - 1 : 0;
	if (p >= ord_n) p = 0;
	LightLock_Unlock(&lk);
	load(p, dir < 0 ? -1 : 1, 0, was_paused);
}

static void command(json_t *req) {
	json_t *cmd = json_object_get(req, "command");
	const char *ep = jstr(cmd, "endpoint");
	if (!ep) return;
	LightLock_Lock(&lk);
	last_id = json_integer_value(json_object_get(req, "message_id"));
	snprintf(last_from, sizeof last_from, "%s", jstr(req, "sent_by_device_id") ? jstr(req, "sent_by_device_id") : "");
	LightLock_Unlock(&lk);
	crumb("connect: command %s", ep);
	if (!strcmp(ep, "transfer")) cmd_transfer(cmd);
	else if (!strcmp(ep, "play")) cmd_play(cmd);
	else if (!strcmp(ep, "pause") || !strcmp(ep, "resume")) {
		bool pz = !strcmp(ep, "pause");
		LightLock_Lock(&lk);
		if (!loaded || cur_local < 0) { LightLock_Unlock(&lk); if (!pz && ord_n) load(pos, 1, rep_pos, false); return; }
		rep_pos = heard_ms(); rep_at = now_ms();
		paused = pz;
		post((ConnectDo){.kind = CD_PAUSE, .paused = pz});
		LightLock_Unlock(&lk);
	} else if (!strcmp(ep, "seek_to")) {
		json_int_t v = json_integer_value(json_object_get(cmd, "value"));
		const char *rel = jstr(cmd, "relative");
		LightLock_Lock(&lk);
		if (rel && !strcmp(rel, "current")) v += heard_ms();
		if (v < 0) v = 0;
		rep_pos = v; rep_at = now_ms();
		post((ConnectDo){.kind = CD_SEEK, .secs = v / 1000.0});
		LightLock_Unlock(&lk);
	} else if (!strcmp(ep, "skip_next")) cmd_skip(1, jstr(json_object_get(cmd, "track"), "uri"));
	else if (!strcmp(ep, "skip_prev")) cmd_skip(-1, NULL);
	else if (!strcmp(ep, "set_shuffling_context") || !strcmp(ep, "set_repeating_context") || !strcmp(ep, "set_repeating_track") || !strcmp(ep, "set_options")) {
		LightLock_Lock(&lk);
		bool was = shuffle;
		json_t *v = json_object_get(cmd, "value");
		if (!strcmp(ep, "set_shuffling_context")) shuffle = json_is_true(v);
		else if (!strcmp(ep, "set_repeating_context")) rep_ctx = json_is_true(v);
		else if (!strcmp(ep, "set_repeating_track")) rep_track = json_is_true(v);
		else {
			if (json_is_boolean(json_object_get(cmd, "shuffling_context"))) shuffle = json_is_true(json_object_get(cmd, "shuffling_context"));
			if (json_is_boolean(json_object_get(cmd, "repeating_context"))) rep_ctx = json_is_true(json_object_get(cmd, "repeating_context"));
			if (json_is_boolean(json_object_get(cmd, "repeating_track"))) rep_track = json_is_true(json_object_get(cmd, "repeating_track"));
		}
		if (rep_track) rep_ctx = true;   // Spotify's "repeat one" is both
		if (shuffle && !was) reshuffle();
		else if (!shuffle && was) unshuffle();
		opts_from_spotify();   // the 3DS's buttons light up the same
		nx_want = true;
		put_due = true;
		LightLock_Unlock(&lk);
	} else if (!strcmp(ep, "add_to_queue")) {
		const char *u = jstr(json_object_get(cmd, "track"), "uri");
		if (u && ord_n) {
			LightLock_Lock(&lk);
			ctx_add(u, jstr(json_object_get(cmd, "track"), "uid"), true);
			int v = ord[ord_n - 1], at = pos + 1;   // after the current song and whatever was queued before it
			while (at < ord_n - 1 && c_q[ord[at]]) at++;
			memmove(ord + at + 1, ord + at, (ord_n - 1 - at) * sizeof *ord);
			ord[at] = v;
			nx_want = true;
			LightLock_Unlock(&lk);
			say("Added to the queue");
		}
	} else crumb("connect: (not handled: %s)", ep);
}

// ---- the dealer's messages ----------------------------------------------------------------

static void handle(json_t *m) {
	const char *type = jstr(m, "type"), *uri = jstr(m, "uri");
	crumb("connect: dealer %s %.70s", type ? type : "?", uri ? uri : jstr(m, "message_ident") ? jstr(m, "message_ident") : "");
	json_t *hd = json_object_get(m, "headers");
	bool gz = jstr(hd, "Transfer-Encoding") && !strcmp(jstr(hd, "Transfer-Encoding"), "gzip");
	if (type && !strcmp(type, "message") && uri && !strncmp(uri, "hm://pusher/v1/connections/", 27)) {
		const char *c = jstr(hd, "Spotify-Connection-Id");
		if (c) { snprintf(conn_id, sizeof conn_id, "%s", c); crumb("connect: connection id"); }
		long code = put_state(REASON_NEW_DEVICE);
		if (code == 200) {
			crumb("connect: 3DStereo is in the device list");
			LightLock_Lock(&lk);
			if (lost_at) crumb("connect: back in after %llu ms, still playing", osGetTime() - lost_at);
			lost_at = 0;
			LightLock_Unlock(&lk);
		}
		else if (code == 401) tok_until = 0;
	} else if (type && !strcmp(type, "request")) {
		const char *key = jstr(m, "key");
		const char *b = jstr(json_object_get(m, "payload"), "compressed");
		size_t n = 0;
		u8 *body = b ? unpack(b, gz, &n) : NULL;
		json_t *req = body ? json_loadb((char *)body, n, 0, NULL) : NULL;
		free(body);
		(void)key;   // the reader thread has answered it already
		u64 t0 = osGetTime();
		if (req) command(req);
		json_decref(req);
		put_state(REASON_PLAYER_STATE_CHANGED);
		crumb("connect: request done in %llu ms", osGetTime() - t0);
	} else if (type && !strcmp(type, "message") && uri && !strncmp(uri, "hm://connect-state/v1/connect/volume", 36)) {
		json_t *pl = json_array_get(json_object_get(m, "payloads"), 0);
		size_t n = 0;
		u8 *body = json_string_value(pl) ? unpack(json_string_value(pl), gz, &n) : NULL;
		long v = body ? cpb_volume(body, n) : -1;
		free(body);
		if (v >= 0) {
			LightLock_Lock(&lk);
			volume = v;
			vol_due = true;
			LightLock_Unlock(&lk);
			crumb("connect: volume %ld", v);
			FILE *f = fopen(VOLFILE, "w");
			if (f) { fprintf(f, "%ld\n", v); fclose(f); }
			put_state(REASON_VOLUME_CHANGED);
		}
	} else if (type && !strcmp(type, "message") && uri && !strncmp(uri, "hm://connect-state/v1/cluster", 29)) {
		json_t *pl = json_array_get(json_object_get(m, "payloads"), 0);
		size_t n = 0;
		u8 *body = json_string_value(pl) ? unpack(json_string_value(pl), gz, &n) : NULL;
		char who[64];
		if (body && cpb_cluster_active(body, n, who, sizeof who)) {
			LightLock_Lock(&lk);
			bool lost = active && who[0] && strcmp(who, devid);
			if (lost) {
				active = false;
				paused = true;
				rep_pos = heard_ms(); rep_at = now_ms();
				post((ConnectDo){.kind = CD_PAUSE, .paused = true});
				say("Spotify moved to another device");
				vol_due = true;   // inactive: the 3DS's own level again
			}
			LightLock_Unlock(&lk);
			if (lost) { crumb("connect: %s took over", who); put_inactive(); }
		}
		free(body);
	}
}

// ---- the worker ---------------------------------------------------------------------------

static void nap(int ms) { for (; ms > 0 && !quitting && !pair_fresh; ms -= 100) svcSleepThread(100 * 1000000LL); }

// ---- the socket's own thread --------------------------------------------------------------
// Spotify hangs up ("Request disconnect", code 1000) on a device that takes a few seconds to answer
// a command, and resolving a context plus a lookup or two over the 3DS's Wi-Fi does (build 42's log:
// 2.7 s). So this thread owns the socket: it answers every request the moment it lands, pongs and
// pings, and hands the messages to the worker, which does the slow part (librespot answers first too).

typedef struct In { struct In *next; json_t *m; } In;
static In *in_head, *in_tail;
static LightLock inlk;
static LightEvent inev;
static volatile bool rd_dead, rd_stop;
static volatile bool online;   // the dealer socket is open and a session is running
static Thread rd;

static void in_push(json_t *m) {
	In *e = malloc(sizeof *e);
	if (!e) { json_decref(m); return; }
	e->next = NULL; e->m = m;
	LightLock_Lock(&inlk);
	if (in_tail) in_tail->next = e; else in_head = e;
	in_tail = e;
	LightLock_Unlock(&inlk);
	LightEvent_Signal(&inev);
}

static json_t *in_pop(void) {
	LightLock_Lock(&inlk);
	In *e = in_head;
	if (e) { in_head = e->next; if (!in_head) in_tail = NULL; }
	LightLock_Unlock(&inlk);
	json_t *m = e ? e->m : NULL;
	free(e);
	return m;
}

static void reader(void *arg) {
	(void)arg;
	while (!rd_stop && !quitting) {
		if (!ws_fill(100)) break;
		bool dead = false;
		char *text;
		while ((text = ws_next(&dead))) {
			json_t *m = json_loads(text, 0, NULL);
			free(text);
			if (!m) continue;
			const char *type = jstr(m, "type"), *key = jstr(m, "key");
			if (type && !strcmp(type, "request")) {
				char reply[300];
				snprintf(reply, sizeof reply, "{\"type\":\"reply\",\"key\":\"%s\",\"payload\":{\"success\":true}}", key ? key : "");
				ws_send(1, reply, strlen(reply));
				crumb("connect: answered %.40s", key ? key : "?");
			}
			in_push(m);
		}
		if (dead) { crumb("connect: session ends: the socket's dead"); break; }
		u64 now = osGetTime();
		if (now - ws_last_ping > 30000) { ws_last_ping = now; if (!ws_send(9, "", 0)) break; }
		if (now - ws_last_rx > 75000) { crumb("connect: dealer went quiet"); break; }
	}
	rd_dead = true;
	LightEvent_Signal(&inev);
}

// One step of getting ahead while nothing else is going on: the whole album or playlist matched in
// bulk once, then the next 30 songs looked up one at a time. Known songs need no Wi-Fi later.
static void warm(void) {
	char cu[256];
	LightLock_Lock(&lk);
	bool go = active && loaded, f = filled;
	int from = pos;
	snprintf(cu, sizeof cu, "%s", ctx_uri);
	filled = true;
	LightLock_Unlock(&lk);
	if (!go) return;
	if (!f) { fill_context(cu); LightLock_Lock(&lk); if (nx_pos < 0) nx_done = false; LightLock_Unlock(&lk); return; }
	for (int k = 1; k <= 30 && from + k < ord_n; k++) {
		const char *u = c_uri[ord[from + k]];
		if (strncmp(u, "spotify:track:", 14) || seen_get(u + 14)) continue;
		int l, d;
		if (!lookup(u, &l, &d)) return;   // no Wi-Fi: later
		LightLock_Lock(&lk);
		if (nx_pos < 0 || (l >= 0 && from + k < nx_pos)) nx_done = false;   // a nearer one for next
		LightLock_Unlock(&lk);
		return;
	}
}

static void session_run(void) {
	if (!token(false) || !apresolve() || !ws_open()) return;
	if (!webtok[0]) {   // Pocket's own login ready before the first command needs a lookup (4 s otherwise)
		char why[48];
		if (!webh) webh = net_handle();
		if (webh && !spot_token(webh, NULL, webtok, sizeof webtok, why, sizeof why, &quitting)) crumb("connect: %s", why);
	}
	conn_id[0] = 0;
	rd_dead = rd_stop = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	rd = threadCreate(reader, NULL, 48 * 1024, prio > 0x19 ? prio - 1 : prio, 0, false);   // a notch above this one
	if (!rd) { crumb("connect: no reader thread"); ws_close(); return; }
	online = true;
	for (;;) {
		if (quitting || rd_dead) break;
		LightEvent_WaitTimeout(&inev, 100 * 1000000LL);
		json_t *m;
		bool busy = false;
		while (!quitting && (m = in_pop())) { handle(m); json_decref(m); busy = true; }
		if (osGetTime() > tok_until) token(true);   // the PUTs' token (the socket keeps the one it opened with)
		if (!conn_id[0]) continue;
		LightLock_Lock(&lk);
		bool mir = mirror_due, rel = release_due, put = put_due, sk = skip_due, want = nx_want || (loaded && active && !nx_done && cur_local >= 0 && pend_pos < 0);
		int sd = skip_dir;
		release_due = put_due = skip_due = false;
		nx_want = false;
		if (rel) { ord_n = c_n = 0; pos = 0; }
		LightLock_Unlock(&lk);
		if (rel) { put_inactive(); put_state(REASON_BECAME_INACTIVE); }
		LightLock_Lock(&lk);
		int qn_ = qa_n, qs[64];
		memcpy(qs, qa, sizeof qs);
		qa_n = 0;
		LightLock_Unlock(&lk);
		for (int k = 0; k < qn_ && active && !quitting; k++) {   // the 3DS's "queue this" while Spotify drives: into Spotify's queue
			char u[192];
			rev_lookup(qs[k], true, u, sizeof u);
			LightLock_Lock(&lk);
			if (ord_n) {
				ctx_add(u, NULL, true);
				int v = ord[ord_n - 1], at = pos + 1;   // after the current song and whatever was queued before it
				while (at < ord_n - 1 && c_q[ord[at]]) at++;
				memmove(ord + at + 1, ord + at, (ord_n - 1 - at) * sizeof *ord);
				ord[at] = v;
				nx_want = true;
			}
			LightLock_Unlock(&lk);
			crumb("connect: queued %s from the 3DS", u);
			put = true;
		}
		if (mir && active) { do_mirror(); put = true; }
		if (sk) { cmd_skip(sd, NULL); put = true; }
		if (put) put_state(REASON_PLAYER_STATE_CHANGED);
		if (want) find_next();
		if (!busy && !rel && !sk && !put && !want && !mir) warm();
	}
	online = false;
	rd_stop = true;
	threadJoin(rd, U64_MAX);
	threadFree(rd);
	rd = NULL;
	json_t *m;
	while ((m = in_pop())) json_decref(m);
	ws_close();
}

static void worker(void *arg) {
	(void)arg;
	device_id();
	seed();
	FILE *vf = card_open(VOLFILE, "rb");
	long sv = -1;
	if (vf) { if (fscanf(vf, "%ld", &sv) != 1) sv = -1; fclose(vf); }
	if (sv >= 0 && sv <= 65535) { LightLock_Lock(&lk); volume = sv; LightLock_Unlock(&lk); }
	int backoff = 1000, wifi_was = -1;
	while (!quitting) {
		char e[48];
		if (pair_fresh) { pair_fresh = false; backoff = 1000; tok[0] = 0; }   // a login just made on the 3DS: use it now
		bool up = connect_paired() && net_up(e, sizeof e);
		if ((int)up != wifi_was) { wifi_was = up; crumb("connect: network %s", up ? "up" : e); }
		if (!up) { nap(1000); continue; }
		if (!hh && !(hh = net_handle())) { nap(3000); continue; }
		u64 t0 = osGetTime();
		session_run();
		LightLock_Lock(&lk);
		// a drop (the lid shuts and the 3DS's Wi-Fi goes with it): the music goes on from the songs
		// already known, and the next connection's first state says we're still the one playing.
		// Never a pause for it: build 43 stopped the music when the first try after the lid opened failed.
		if (active && !lost_at) lost_at = osGetTime();
		LightLock_Unlock(&lk);
		if (quitting) break;
		backoff = osGetTime() - t0 > 10000 ? 1000 : backoff * 2 > 60000 ? 60000 : backoff * 2;
		crumb("connect: reconnecting in %d ms", backoff);
		nap(backoff);
	}
	if (hh) curl_easy_cleanup(hh);
	if (webh) curl_easy_cleanup(webh);
	hh = webh = NULL;
}

// ---- the main thread's side ---------------------------------------------------------------

void connect_init(void) { if (!inited) { LightLock_Init(&lk); LightLock_Init(&inlk); LightEvent_Init(&inev, RESET_ONESHOT); inited = true; } }

// ---- pairing from the 3DS itself --------------------------------------------------------------------------------------
// Spotify's device-code login, the way a TV does it: ask for a code, show it, the person types it at spotify.com/pair
// on a phone or a PC, and the 3DS (asking every few seconds) is handed the login. No dev app, no browser here, nothing
// hosted. The desktop app's id, the same one the Connect login has always been.
// When it fails, the page says why in plain words (PZ_*): the clock, the Wi-Fi, a certificate, a code that ran out...
#define DEVICE_URL "https://accounts.spotify.com/oauth2/device/authorize"
#define PAIR_ID    "65b708073fc0480ea92a077233ca87bd"
#define CLOCK_TOL  (36 * 3600)   // a clock this far from the real time (a time zone, an AM that should be PM) is still fine for a certificate
static const char PAIR_SCOPES[] = "streaming user-read-private user-read-email user-read-playback-state user-modify-playback-state "
                                  "user-read-currently-playing playlist-read-private playlist-read-collaborative user-library-read app-remote-control";
static Thread pth;
static volatile int pr_state;      // 0 idle, 1 asking, 2 waiting for the code to be typed, 3 paired, 4 failed
static volatile int pr_kind;       // PZ_*, with state 4
static volatile long pr_http, pr_skew_days;
static char pr_code[16], pr_url[40], pr_err[80];
static volatile u64 pr_end;
static volatile bool pr_stop;
static void pr_fail(int kind, const char *detail) {
	snprintf(pr_err, sizeof pr_err, "%s", detail ? detail : "");
	pr_kind = kind;
	pr_state = 4;
}

// What a failed request means, in the app's terms: a curl error (http 0), or an HTTP status.
static int pair_kind(CURLcode ce, long http) {
	if (http > 0) return http == 429 ? PZ_BUSY : http >= 500 ? PZ_SERVER : PZ_REFUSED;
	switch (ce) {
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY: return PZ_DNS;
	case CURLE_COULDNT_CONNECT: return PZ_CONNECT;
	case CURLE_OPERATION_TIMEDOUT: return PZ_TIMEOUT;
	case CURLE_PEER_FAILED_VERIFICATION:   // the certificate's dates or its signer: mostly a wrong clock, else an old bundle
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CONNECT_ERROR: return PZ_CERT;
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
	case CURLE_GOT_NOTHING: return PZ_DROPPED;
	default: return PZ_UNKNOWN;
	}
}

// "Date: Thu, 05 Nov 2026 18:30:00 GMT" -> unix seconds, -1 if it isn't one
static int64_t parse_http_date(const char *s) {
	char wd[8], ms[8];
	int d, y, hh_, mm, ss;
	if (sscanf(s, " %3s, %d %3s %d %d:%d:%d", wd, &d, ms, &y, &hh_, &mm, &ss) != 7 || mon(ms) < 0) return -1;
	return days_from_civil(y, mon(ms) + 1, d) * 86400 + hh_ * 3600 + mm * 60 + ss;
}
static int64_t pr_date_s;
static size_t pr_hdr(char *p, size_t sz, size_t n, void *ud) {
	(void)ud;
	size_t k = sz * n;
	if (k > 30 && !strncasecmp(p, "date:", 5)) { int64_t t = parse_http_date(p + 5); if (t > 0) pr_date_s = t; }
	return k;
}
// The real time, from any plain-http server's Date header (no TLS: this works with exactly the clock that breaks
// TLS). *skew: the 3DS's clock minus the real time, in seconds. False if nobody answered.
static bool clock_probe(CURL *h, long *skew) {
	static const char *U[] = {"http://www.gstatic.com/generate_204", "http://detectportal.firefox.com/success.txt", NULL};
	for (int i = 0; U[i] && !pr_stop; i++) {
		pr_date_s = 0;
		curl_easy_reset(h);
		curl_easy_setopt(h, CURLOPT_URL, U[i]);
		curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
		curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, pr_hdr);
		curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 6L);
		curl_easy_setopt(h, CURLOPT_TIMEOUT, 8L);
		curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(h, CURLOPT_USERAGENT, "3DStereo");
		if (curl_easy_perform(h) == CURLE_OK && pr_date_s > 0) { *skew = (long)((int64_t)time(NULL) - pr_date_s); return true; }
	}
	return false;
}

static void pair_worker(void *arg) {
	(void)arg;
	char e[48] = "";
	if (!net_up(e, sizeof e)) { pr_fail(strstr(e, "Wi-Fi") ? PZ_NOWIFI : PZ_UNKNOWN, e); return; }
	CURL *h = net_handle();
	if (!h) { pr_fail(PZ_UNKNOWN, "no network handle"); return; }
	Buf b = {0};
	CURLcode ce = CURLE_OK;
	// the clock first: a wrong one makes every certificate look expired, and the errors that follow say nothing about it
	long skew = 0;
	bool timed = clock_probe(h, &skew);
	if (timed) crumb("connect: the 3DS's clock is %ld s from the real time", skew);
	if ((timed && labs(skew) > CLOCK_TOL) || (!timed && net_clock_year())) {
		pr_skew_days = timed ? labs(skew) / 86400 : 0;
		crumb("connect: pairing stopped: the clock is wrong (%d)", net_clock_year());
		curl_easy_cleanup(h);
		pr_fail(PZ_CLOCK, "");
		return;
	}
	char form[1400], dc[160] = "";
	char *esc = curl_easy_escape(h, PAIR_SCOPES, 0);
	snprintf(form, sizeof form, "client_id=%s&scope=%s", PAIR_ID, esc ? esc : "");
	curl_free(esc);
	long code = -1;
	for (int tryn = 0; tryn < 3 && !pr_stop; tryn++) {   // a Wi-Fi that has just woken, a DNS answer that's late: a second try
		code = net_req(h, "POST", DEVICE_URL, form, NULL, &b, NULL, &pr_stop, &ce);
		if (code > 0) break;
		crumb("connect: pairing request %d failed: %s", tryn + 1, curl_easy_strerror(ce));
		for (int w = 0; w < 1500 && !pr_stop; w += 100) svcSleepThread(100 * 1000000LL);
	}
	json_t *j = code == 200 && b.d ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	const char *d0 = json_string_value(json_object_get(j, "device_code")), *uc = json_string_value(json_object_get(j, "user_code"));
	const char *vu = json_string_value(json_object_get(j, "verification_uri"));
	long ex = (long)json_integer_value(json_object_get(j, "expires_in")), iv = (long)json_integer_value(json_object_get(j, "interval"));
	if (!d0 || !uc || strlen(d0) >= sizeof dc || strlen(uc) >= sizeof pr_code) {
		pr_http = code > 0 ? code : 0;
		int kind = code > 200 ? pair_kind(CURLE_OK, code) : code == 200 ? PZ_PORTAL : pair_kind(ce, 0);   // 200 and not Spotify's answer: a login page in the way
		char why[80];
		if (code > 0) snprintf(why, sizeof why, "HTTP %ld", code);
		else snprintf(why, sizeof why, "%.70s", curl_easy_strerror(ce));
		if (code > 0) crumb("connect: pairing request answered %ld %.100s", code, b.d ? b.d : "");
		json_decref(j);
		free(b.d);
		curl_easy_cleanup(h);
		pr_fail(pr_stop ? PZ_NONE : kind, pr_stop ? "Cancelled" : why);
		return;
	}
	snprintf(dc, sizeof dc, "%s", d0);
	snprintf(pr_code, sizeof pr_code, "%s", uc);
	if (vu && !strncmp(vu, "https://", 8)) vu += 8;
	snprintf(pr_url, sizeof pr_url, "%s", vu && *vu ? vu : "spotify.com/pair");
	json_decref(j);
	pr_end = osGetTime() + (u64)(ex > 0 ? ex : 600) * 1000;
	pr_state = 2;
	crumb("connect: pairing code asked for, waiting");
	long wait = (iv > 0 ? iv : 5) * 1000;
	int misses = 0;   // answers that weren't an answer, in a row
	while (!pr_stop && !quitting && osGetTime() < pr_end && pr_state == 2) {
		for (long w = 0; w < wait && !pr_stop && !quitting; w += 100) svcSleepThread(100 * 1000000LL);
		if (pr_stop || quitting) break;
		char *edc = curl_easy_escape(h, dc, 0);
		snprintf(form, sizeof form, "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code&device_code=%s&client_id=%s", edc ? edc : "", PAIR_ID);
		curl_free(edc);
		long c = net_req(h, "POST", TOKEN_URL, form, NULL, &b, NULL, &pr_stop, &ce);
		json_t *t = c > 0 && b.d ? json_loadb(b.d, b.n, 0, NULL) : NULL;
		pr_http = c > 0 ? c : 0;
		if (c == 200 && t) {
			misses = 0;
			const char *rt = json_string_value(json_object_get(t, "refresh_token")), *at = json_string_value(json_object_get(t, "access_token"));
			if (rt) {
				json_t *pair = json_pack("{s:s,s:s}", "client_id", PAIR_ID, "refresh_token", rt);
				bool ok = pair && login_save(pair);
				json_decref(pair);
				if (ok) {
					remove(ACCESS);
					if (at) access_save(at, (long)json_integer_value(json_object_get(t, "expires_in")));
					login_dead = false;
					pair_fresh = true;
					pr_state = 3;
					crumb("connect: paired from the 3DS");
				} else pr_fail(PZ_SAVE, "");
			} else pr_fail(PZ_REFUSED, "no login in the answer");
		} else if (c == 400 && t) {
			misses = 0;
			const char *er = json_string_value(json_object_get(t, "error"));
			if (er && !strcmp(er, "slow_down")) wait += 5000;
			else if (!er || strcmp(er, "authorization_pending")) {
				crumb("connect: pairing answer %s", er ? er : "?");
				pr_fail(er && !strcmp(er, "expired_token") ? PZ_EXPIRED : er && !strcmp(er, "access_denied") ? PZ_DENIED : PZ_REFUSED, er ? er : "");
			}
		} else if (c == 429) wait += 5000;   // told to slow down: it does
		else if (c == 200) { if (++misses >= 3) pr_fail(PZ_PORTAL, "not Spotify's answer"); }
		else if (c > 0 && c < 500) pr_fail(pair_kind(CURLE_OK, c), "HTTP");
		else if (++misses >= 5) {   // no answer, or Spotify erroring, five times running
			char why[80];
			if (c > 0) snprintf(why, sizeof why, "HTTP %ld", c);
			else snprintf(why, sizeof why, "%.70s", curl_easy_strerror(ce));
			pr_fail(pair_kind(ce, c > 0 ? c : 0), why);
		}
		json_decref(t);
	}
	if (pr_state == 2) pr_fail(pr_stop ? PZ_NONE : PZ_EXPIRED, pr_stop ? "Cancelled" : "");
	free(b.d);
	curl_easy_cleanup(h);
}
void connect_pair_start(void) {
	if (pth && pr_state >= 1 && pr_state <= 2) return;   // one at a time
	if (pth) { threadJoin(pth, U64_MAX); threadFree(pth); pth = NULL; }
	pr_stop = false;
	pr_code[0] = pr_url[0] = pr_err[0] = 0;
	pr_kind = PZ_NONE;
	pr_http = pr_skew_days = 0;
	pr_state = 1;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	pth = threadCreate(pair_worker, NULL, 128 * 1024, prio + 3 < 0x3F ? prio + 3 : 0x3F, 0, false);
	if (!pth) pr_fail(PZ_UNKNOWN, "no thread");
}
void connect_pair_cancel(void) { pr_stop = true; }
void connect_pair_status(ConnectPair *o) {
	o->state = pr_state;
	o->kind = pr_kind;
	o->http = pr_http;
	o->skew_days = (int)pr_skew_days;
	snprintf(o->code, sizeof o->code, "%s", pr_code);
	snprintf(o->url, sizeof o->url, "%s", pr_url);
	snprintf(o->err, sizeof o->err, "%s", pr_err);
	u64 now = osGetTime();
	o->secs_left = pr_state == 2 && pr_end > now ? (int)((pr_end - now) / 1000) : 0;
}

void connect_start(void) {
	if (th || !connect_paired()) return;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	th = threadCreate(worker, NULL, 128 * 1024, prio + 3 < 0x3F ? prio + 3 : 0x3F, 0, false);
	crumb(th ? "connect: started" : "connect: no thread");
}

void connect_quit(void) { quitting = true; pr_stop = true; }

void connect_exit(void) {
	quitting = true;
	pr_stop = true;
	if (pth) { threadJoin(pth, U64_MAX); threadFree(pth); pth = NULL; }
	if (th) { threadJoin(th, U64_MAX); threadFree(th); th = NULL; }
}

bool connect_online(void) { return online; }

bool connect_active(void) {
	if (!inited) return false;
	LightLock_Lock(&lk);
	bool a = active;
	LightLock_Unlock(&lk);
	return a;
}

bool connect_driving(void) {
	if (!inited) return false;
	LightLock_Lock(&lk);
	bool a = active && !mirror;
	LightLock_Unlock(&lk);
	return a;
}

void connect_source(int kind, const char *pid, bool single) {
	if (!inited) return;
	LightLock_Lock(&lk);
	src_kind_ = kind;
	snprintf(src_pid, sizeof src_pid, "%s", pid ? pid : "");
	src_single = single;
	LightLock_Unlock(&lk);
}

void connect_upcoming(const int *next, int n, int nq) {
	if (!inited) return;
	LightLock_Lock(&lk);
	int nn = n < 4 ? n : 4, q = nq < nn ? nq : nn;
	bool same = nn == n_up && q == n_upq && !memcmp(up_next, next, nn * sizeof *next);
	n_up = nn;
	n_upq = q < 0 ? 0 : q;
	memcpy(up_next, next, n_up * sizeof *next);
	if (!same && active && mirror) mirror_due = true;   // Spotify shows the 3DS's queue: it hears the change
	LightLock_Unlock(&lk);
}

bool connect_queue_add(int track) {
	if (!inited || track < 0 || track >= lib.nt) return false;
	LightLock_Lock(&lk);
	bool drive = active && !mirror && loaded;
	if (drive && qa_n < 64) qa[qa_n++] = track;
	LightLock_Unlock(&lk);
	return drive;
}

int connect_queue_clear(void) {   // Spotify's own queue, while it drives: every song queued by hand after the playing one goes
	LightLock_Lock(&lk);
	int gone = 0;
	if (active && !mirror && loaded) {
		int w = pos + 1;
		for (int k = pos + 1; k < ord_n; k++) {
			if (c_q[ord[k]]) { gone++; continue; }
			ord[w++] = ord[k];
		}
		ord_n = w;
		if (gone) { nx_want = true; put_due = true; }
	}
	LightLock_Unlock(&lk);
	return gone;
}

bool connect_queue_remove(int k, ConnectUndo *u) {
	bool ok = false;
	LightLock_Lock(&lk);
	int at = pos + 1 + k;
	if (active && !mirror && loaded && k >= 0 && at < ord_n) {
		int v = ord[at];
		memmove(ord + at, ord + at + 1, (ord_n - at - 1) * sizeof *ord);
		ord_n--;
		nx_want = true;
		put_due = true;
		if (u) { u->at = at; u->val = v; u->n = ord_n; u->left = ord[at - 1]; u->right = at < ord_n ? ord[at] : -1; }
		ok = true;
	}
	LightLock_Unlock(&lk);
	return ok;
}

int connect_queue_restore(const ConnectUndo *u) {
	int k = -1;
	LightLock_Lock(&lk);
	if (active && !mirror && loaded && u->n == ord_n && u->at >= pos + 1 && u->at <= ord_n && u->val >= 0 && u->val < c_n &&
	    ord[u->at - 1] == u->left && (u->at < ord_n ? ord[u->at] : -1) == u->right) {
		bool there = false;
		for (int i = 0; i < ord_n; i++) if (ord[i] == u->val) { there = true; break; }
		if (!there) {
			memmove(ord + u->at + 1, ord + u->at, (ord_n - u->at) * sizeof *ord);
			ord[u->at] = u->val;
			ord_n++;
			k = u->at - pos - 1;
			nx_want = true;
			put_due = true;
		}
	}
	LightLock_Unlock(&lk);
	return k;
}

void connect_queue_move(int from, int to) {
	LightLock_Lock(&lk);
	int a = pos + 1 + from, b = pos + 1 + to;
	if (active && !mirror && loaded && from >= 0 && to >= 0 && a < ord_n && b < ord_n && a != b) {
		int nq = 0;
		while (pos + 1 + nq < ord_n && c_q[ord[pos + 1 + nq]]) nq++;
		int v = ord[a];
		if (a < b) memmove(ord + a, ord + a + 1, (b - a) * sizeof *ord);
		else memmove(ord + b + 1, ord + b, (a - b) * sizeof *ord);
		ord[b] = v;
		int nq_ex = nq - (from < nq ? 1 : 0);
		c_q[v] = from < nq ? to <= nq_ex : to < nq_ex;
		nx_want = true;
		put_due = true;
	}
	LightLock_Unlock(&lk);
}

int connect_queue_view(int *tracks, bool *queued, int cap) {
	if (!inited) return 0;
	LightLock_Lock(&lk);
	int n = 0;
	if (active && !mirror && loaded)
		for (int k = pos + 1; k < ord_n && n < cap; k++) {
			const char *u = c_uri[ord[k]];
			Seen *sn = !strncmp(u, "spotify:track:", 14) ? seen_get(u + 14) : NULL;
			tracks[n] = sn && sn->local >= 0 && sn->local < lib.nt ? sn->local : -1;
			queued[n] = c_q[ord[k]];
			n++;
		}
	LightLock_Unlock(&lk);
	return n;
}

bool connect_poll(const AudioInfo *ai, ConnectDo *out) {
	if (!th) return false;
	LightLock_Lock(&lk);
	snap_track = ai->track; snap_pos = ai->pos; snap_paused = ai->paused; snap_open = ai->open; snap_at = osGetTime();
	bool got = todo_set;
	if (got) { *out = todo; todo_set = false; grace_until = osGetTime() + 2000; }
	else *out = (ConnectDo){.kind = CD_NONE};
	if (vol_due) {   // cubic, so the slider's middle is about half as loud rather than barely quieter
		float x = active ? volume / 65535.0f : 1.0f;
		out->vol_set = true;
		out->vol = x * x * x;
		vol_due = false;
		got = true;
	}
	if (opts_due) {
		out->opts_set = true;
		out->shuffle = shuffle;
		out->repeat = rep_track ? 2 : rep_ctx ? 1 : 0;
		opts_due = false;
		got = true;
	}
	if (active && mirror && !got && osGetTime() > grace_until && ai->open && ai->track >= 0 && ai->track != cur_local && !mirror_due)
		mirror_due = true;   // the 3DS's queue moved on: Spotify hears which song
	if (active && !got && osGetTime() > grace_until) {
		if (pend_pos >= 0 && ai->track == pend_local) {   // the next song is the one heard now
			pos = pend_pos; cur_local = pend_local; cur_dur = pend_dur;
			pend_pos = -1; nx_pos = -1; nx_done = false;
			for (int i = 0; i < 32; i++) playback[i] = "0123456789abcdef"[rand() & 15];
			put_due = true;
		} else if (cur_local >= 0 && ai->open && ai->track == cur_local) {
			if (ai->paused != paused) { paused = ai->paused; put_due = true; }   // the 3DS's own pause
			else {
				int64_t want = rep_pos + (paused ? 0 : now_ms() - rep_at);
				if (llabs((int64_t)(ai->pos * 1000) - want) > 3000) put_due = true;   // a seek on the 3DS
			}
		} else if (cur_local >= 0 && !ai->open && !paused) { paused = true; put_due = true; }   // ran out
	}
	LightLock_Unlock(&lk);
	return got;
}

int connect_next(bool *pause) {
	if (!inited) return -2;
	LightLock_Lock(&lk);
	int r = -2;
	if (active && !mirror) {
		if (nx_pos >= 0 && nx_local >= 0) { r = nx_local; pend_pos = nx_pos; pend_local = nx_local; pend_dur = nx_dur; nx_pos = -1; }
		else { r = -1; *pause = true; }   // the end, or not found yet: stop; Spotify hears it from connect_poll
	}
	LightLock_Unlock(&lk);
	return r;
}

void connect_skip(int dir) {
	LightLock_Lock(&lk);
	skip_due = true;
	skip_dir = dir;
	LightLock_Unlock(&lk);
}

void connect_release(void) {
	if (!inited) return;
	LightLock_Lock(&lk);
	// the 3DS picked something of its own: it plays its own queue now and Spotify follows it
	if (active) { mirror = true; mirror_due = true; nx_pos = pend_pos = -1; }
	LightLock_Unlock(&lk);
}

void connect_set_name(const char *name) {
	if (!name || !name[0]) name = NAME;
	if (!inited) { snprintf(dev_name, sizeof dev_name, "%s", name); return; }
	LightLock_Lock(&lk);
	snprintf(dev_name, sizeof dev_name, "%s", name);
	put_due = true;   // Spotify hears it with the state
	LightLock_Unlock(&lk);
}

void connect_options(bool sh, int repeat) {
	if (!inited) return;
	LightLock_Lock(&lk);
	if (sh != m_shuffle || repeat != m_repeat) {
		bool first = m_shuffle < 0;
		m_shuffle = sh;
		m_repeat = repeat;
		if (!first && active) {   // a press on the 3DS: Spotify's buttons follow, and its order if it's driving
			bool was = shuffle;
			opts_from_3ds();
			if (!mirror) {
				if (shuffle && !was) reshuffle();
				else if (!shuffle && was) unshuffle();
				nx_want = true;
			}
			put_due = true;
		}
	}
	LightLock_Unlock(&lk);
}

void connect_hold_wifi(bool on) {
	static bool ndm_ok, held;
	if (on == held) return;
	if (on) {
		if (!ndm_ok) {
			Result r = ndmuInit();
			if (R_FAILED(r)) { crumb("wifi hold: ndm:u won't open (%08lx)", (unsigned long)r); held = true; return; }   // don't retry every frame
			ndm_ok = true;
		}
		Result a = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
		Result b = NDMU_LockState();
		crumb("wifi hold: on (exclusive %08lx, lock %08lx)", (unsigned long)a, (unsigned long)b);
	} else if (ndm_ok) {
		Result b = NDMU_UnlockState();
		Result a = NDMU_LeaveExclusiveState();
		crumb("wifi hold: off (unlock %08lx, leave %08lx)", (unsigned long)b, (unsigned long)a);
	}
	held = on;
}
