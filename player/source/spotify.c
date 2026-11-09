// Spotify's lists onto the card, and the 3DS's likes back to Spotify, on a quiet thread of its
// own. It's tools/spotify_sync.py in C: the same playlist files, the same state file, the same
// rules, so either one can sync and the other carries on from there.
#include "cardio.h"
#include "spotify.h"
#include "crumb.h"
#include "library.h"
#include "spotmatch.h"
#include "net.h"
#include <curl/curl.h>
#include <dirent.h>
#include <jansson.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DIR3DS  "sdmc:/3ds/player"
#define PAIR    DIR3DS "/spotify.json"
#define STATE   DIR3DS "/spotify-sync.json"
#define ROOT    "sdmc:/music"
#define LISTS   ROOT "/playlists"
#define LIKED   "Liked Songs"
#define MARK    "#SPOTIFY:"
#define API     "https://api.spotify.com/v1"
#define TOKEN_URL "https://accounts.spotify.com/api/token"

// ---- between the threads ------------------------------------------------------------------

static Thread th;
static LightLock lk;
static LightEvent wake;
static volatile bool quitting;
static bool go;            // main -> worker: a sync's inputs are ready
static bool done;          // worker -> main: its results are
static bool waiting, wait_loud, wait_full, running, run_loud;   // main's own
static bool run_full;      // main -> worker with go: the whole sync, or just the 3DS's likes up
static bool was_push;      // worker -> main with done: it was the quick one
static u64 due, next_auto; // next_auto: the quiet full sync every few minutes
#define AUTO_MS (5 * 60 * 1000)
static char later[48];     // something to say on the next poll

// the job: the 3DS's Liked Songs when it started
static int *L, nL;
// the result
static int *add, nadd, *drop, ndrop;
static bool ok, lists_changed, guarded;
static int nlists, nsent, nmissing, nguard;
static char err[48];

// Asked once, at spot_init, and remembered: the Settings page asks every frame, and a stat on the card waits in line
// behind whatever the background workers are reading or writing (build 91's log: 240-470 ms of every Settings frame while
// the artist prefetch ran). The pairing file is written on a PC with the card out, so a launch is the only time it changes.
static int paired_c = -1;
bool spot_paired(void) {
	if (paired_c < 0) {
		struct stat st;
		paired_c = !stat(PAIR, &st) || !stat(PAIR ".tmp", &st);
	}
	return paired_c;
}

bool spot_busy(void) { return running; }

static void fail(const char *m) { if (!err[0]) snprintf(err, sizeof err, "%s", m); }

// ---- the network --------------------------------------------------------------------------

static CURL *curl;        // this thread's own handle
static char token[1024];  // shared with meta.c's searches, under tok_lk
static char tok_scope[512];   // what the pairing allows, as the last refresh said (under tok_lk)
static u64 tok_until;     // when it runs out, a minute early (osGetTime), under tok_lk
static LightLock tok_lk;

static bool net_ok(void) {
	char e[38], m[48];
	if (!net_up(e, sizeof e)) { snprintf(m, sizeof m, "Spotify: %s", e); fail(m); return false; }
	if (!curl && !(curl = net_handle())) { fail("Spotify: the network won't start"); return false; }
	return true;
}

static void say_cerr(CURLcode rc) {
	if (quitting) return;
	fail(rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR ? "Spotify: secure connection failed" : "Spotify: can't reach it");
}

// One request on the one handle, so the connection to Spotify is kept between calls. The HTTP
// code, or -1 if it never got one.
static long req(const char *method, const char *url, const char *form, Buf *out, long *retry_after) {
	char auth[1100] = "";
	if (!form || form[0] == '{') {   // the Web API's; a form is the login's own request
		LightLock_Lock(&tok_lk);
		if (token[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
		LightLock_Unlock(&tok_lk);
	}
	CURLcode rc = CURLE_OK;
	long code = net_req(curl, method, url, form, auth[0] ? auth : NULL, out, retry_after, &quitting, &rc);
	if (code < 0) say_cerr(rc);
	return code;
}

static void nap(int ms) {   // a wait that a closing app cuts short
	for (; ms > 0 && !quitting; ms -= 100) svcSleepThread(100 * 1000000LL);
}

// Saved beside the old file, then swapped in, so a power cut leaves one whole copy or the other
static bool save_json(json_t *j, const char *path, size_t flags) {
	char tmp[128];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!j || json_dump_file(j, tmp, flags | JSON_ENSURE_ASCII)) return false;
	remove(path);
	return !rename(tmp, path);
}

static char *slurp(const char *path, size_t *len);
static json_t *json_card(const char *path) {   // whole file in one go, not jansson's 1 KB reads
	size_t n = 0;
	char *s = slurp(path, &n);
	if (!s) return NULL;
	json_t *j = json_loadb(s, n, 0, NULL);
	free(s);
	return j;
}

static json_t *load_json(const char *path) {
	char tmp[128];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	json_t *j = json_card(path);
	return j ? j : json_card(tmp);   // cut off between the two steps of a save
}

// The saved login for an access token, for whichever thread asks, on its own handle. Under the
// lock: Spotify can hand back a new login with the token and then the old one stops working, so
// two refreshes at once would spend the same login twice. stale: the token that just got a 401
// (NULL: any token will do); if another thread has already replaced it, that one's used.
static bool refresh_on(CURL *h, const char *stale, char *why, int cap, volatile bool *quit) {
	LightLock_Lock(&tok_lk);
	if (token[0] && (!stale || strcmp(token, stale))) { LightLock_Unlock(&tok_lk); return true; }
	token[0] = 0;
	why[0] = 0;
	json_t *pair = load_json(PAIR);
	const char *cid = json_string_value(json_object_get(pair, "client_id"));
	const char *rt = json_string_value(json_object_get(pair, "refresh_token"));
	if (!cid || !rt) { json_decref(pair); snprintf(why, cap, "Spotify: not paired"); LightLock_Unlock(&tok_lk); return false; }
	char *e_rt = curl_easy_escape(h, rt, 0), *e_cid = curl_easy_escape(h, cid, 0);
	size_t fn = strlen(e_rt) + strlen(e_cid) + 64;
	char *form = malloc(fn);
	snprintf(form, fn, "grant_type=refresh_token&refresh_token=%s&client_id=%s", e_rt, e_cid);
	curl_free(e_rt);
	curl_free(e_cid);
	Buf b = {0};
	CURLcode rc = CURLE_OK;
	// never cut short: if Spotify has already rotated the login, dropping its answer loses it
	(void)quit;
	long code = net_req(h, "POST", TOKEN_URL, form, NULL, &b, NULL, NULL, &rc);
	free(form);
	json_t *tok = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	if (code > 0 && code != 200) crumb("spotify: token %ld %.120s", code, b.d ? b.d : "");
	free(b.d);
	const char *at = json_string_value(json_object_get(tok, "access_token"));
	const char *nrt = json_string_value(json_object_get(tok, "refresh_token"));
	const char *scope = json_string_value(json_object_get(tok, "scope"));
	if (at && strlen(at) < sizeof token) snprintf(token, sizeof token, "%s", at);
	json_int_t life = json_integer_value(json_object_get(tok, "expires_in"));   // an hour, normally
	tok_until = osGetTime() + (u64)(life > 120 ? life - 60 : 60) * 1000;
	if (nrt && strcmp(nrt, rt)) {
		json_object_set_new(pair, "refresh_token", json_string(nrt));
		if (save_json(pair, PAIR, JSON_INDENT(2))) crumb("spotify: login rotated, saved");
	}
	if (token[0] && scope) snprintf(tok_scope, sizeof tok_scope, "%s", scope);
	if (token[0] && scope && scope[0]) {
		static const char *NEED[] = {"playlist-read-private", "user-library-read", "user-library-modify"};
		for (int k = 0; k < 3; k++)
			if (!strstr(scope, NEED[k])) { snprintf(why, cap, "Spotify: pair again on a PC"); token[0] = 0; break; }
	}
	json_decref(tok);
	json_decref(pair);
	if (!token[0] && !why[0]) {
		if (code < 0) snprintf(why, cap, rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR ? "Spotify: secure connection failed" : "Spotify: can't reach it");
		else snprintf(why, cap, code == 400 || code == 401 ? "Spotify: login expired, pair on a PC" : "Spotify: can't sign in");
	}
	bool ok = token[0] != 0;
	LightLock_Unlock(&tok_lk);
	return ok;
}

static bool refresh_stale(const char *stale) {
	char why[48];
	bool r = refresh_on(curl, stale, why, sizeof why, &quitting);
	if (!r && !quitting) fail(why);
	return r;
}
// A sync starts with a token that has time left: the one held if it does, a fresh one if not.
// (Up to build 22 every sync asked for a new one, about 4 s on a cold connection.)
static bool refresh(void) {
	char was[1024];
	LightLock_Lock(&tok_lk);
	u64 now = osGetTime();
	bool good = token[0] && now < tok_until && tok_until - now < 3700000ULL;   // a clock that jumped counts as run out
	snprintf(was, sizeof was, "%s", token);
	LightLock_Unlock(&tok_lk);
	return good || refresh_stale(was);
}

bool spot_token(CURL *h, const char *stale, char *out, int cap, char *why, int wcap, volatile bool *quit) {
	if (!spot_paired()) { snprintf(why, wcap, "Spotify: pair on a PC first"); return false; }
	if (!refresh_on(h, stale, why, wcap, quit)) return false;
	LightLock_Lock(&tok_lk);
	snprintf(out, cap, "%s", token);
	LightLock_Unlock(&tok_lk);
	return out[0] != 0;
}

// A Web API call: a 401 gets a fresh token once, a 429 waits its turn (3 tries). body: JSON, or NULL.
static long api_b(const char *method, const char *path, const char *body, Buf *out) {
	char url[4096];
	snprintf(url, sizeof url, "%s%s", strncmp(path, "http", 4) ? API : "", path);
	if (!token[0] && !refresh_stale(NULL)) return -1;
	bool refreshed = false;
	for (int tries = 0;; tries++) {
		char used[1024];
		LightLock_Lock(&tok_lk);
		snprintf(used, sizeof used, "%s", token);
		LightLock_Unlock(&tok_lk);
		long ra = 0, code = req(method, url, body, out, &ra);
		if (code == 401 && !refreshed) {
			refreshed = true;
			if (!refresh_stale(used)) return -1;
			continue;
		}
		if (code == 429 && tries < 3) { nap(ra > 0 && ra <= 10 ? (int)ra * 1000 + 500 : 3000); continue; }
		if (code < 0 || quitting) return -1;
		if (code >= 400 && code != 403 && code != 404) {
			crumb("spotify: %.200s", out->d ? out->d : "");
			char m[48];
			snprintf(m, sizeof m, "Spotify: error %ld", code);
			fail(m);
		}
		return code;
	}
}
static long api(const char *method, const char *path, Buf *out) { return api_b(method, path, NULL, out); }

// Every entry of every page; false if a page didn't come (code says why)
static bool pages(const char *first, void (*each)(json_t *, void *), void *ctx, long *code) {
	char *next = strdup(first);
	*code = 200;
	while (next) {
		Buf b = {0};
		long c = api("GET", next, &b);
		free(next);
		next = NULL;
		if (c != 200) { *code = c; free(b.d); return false; }
		json_t *j = json_loadb(b.d, b.n, 0, NULL);
		free(b.d);
		if (!j) { fail("Spotify: a reply made no sense"); *code = -1; return false; }
		size_t k;
		json_t *it;
		json_array_foreach(json_object_get(j, "items"), k, it) each(it, ctx);
		const char *nx = json_string_value(json_object_get(j, "next"));
		if (nx) next = strdup(nx);
		json_decref(j);
		if (quitting) { free(next); *code = -1; return false; }
	}
	return true;
}

// ---- small things -------------------------------------------------------------------------

typedef struct { char *s; size_t n, cap; } Str;
static void put(Str *s, const char *t) {
	size_t n = strlen(t);
	if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2; s->s = realloc(s->s, s->cap); }
	memcpy(s->s + s->n, t, n + 1);
	s->n += n;
}

static void sm_of(json_t *t, SmTrack *s) {
	memset(s, 0, sizeof *s);
	s->name = json_string_value(json_object_get(t, "name"));
	size_t k;
	json_t *a;
	json_array_foreach(json_object_get(t, "artists"), k, a) {
		const char *n = json_string_value(json_object_get(a, "name"));
		if (n && s->na < 8) s->artists[s->na++] = n;
	}
	s->ms = json_integer_value(json_object_get(t, "duration_ms"));
}

// "Artist, Artist - Title", for the songs a playlist has that the card doesn't
static void label(json_t *t, Str *out) {
	SmTrack s;
	sm_of(t, &s);
	for (int a = 0; a < s.na; a++) { if (a) put(out, ", "); put(out, s.artists[a]); }
	put(out, " - ");
	put(out, s.name ? s.name : "?");
}

// "sdmc:/music/Album/x.flac" as the playlists folder sees it: "../Album/x.flac"
static const char *rel_of(int i, char *buf, int cap) {
	const char *p = lib.t[i].path;
	if (!strncmp(p, ROOT "/", sizeof ROOT)) snprintf(buf, cap, "../%s", p + sizeof ROOT);
	else snprintf(buf, cap, "%s", p);
	return buf;
}

static int find_rel(const char *rel) {
	char full[1024];
	if (!strncmp(rel, "../", 3)) snprintf(full, sizeof full, ROOT "/%s", rel + 3);
	else if (!strncasecmp(rel, "sdmc:", 5)) snprintf(full, sizeof full, "%s", rel);
	else snprintf(full, sizeof full, LISTS "/%s", rel);
	for (char *c = full; *c; c++) if (*c == '\\') *c = '/';
	return library_find(full);
}

static char *slurp(const char *path, size_t *len) {
	FILE *f = card_open_big(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	char *s = n >= 0 ? malloc(n + 1) : NULL;
	if (s) { n = fread(s, 1, n, f); s[n] = 0; if (len) *len = n; }
	fclose(f);
	return s;
}

// FNV-1a, for the fingerprints that let a sync skip what hasn't changed
static u32 fnv(u32 h, const void *p, size_t n) {
	for (const u8 *b = p; n--; b++) { h ^= *b; h *= 16777619u; }
	return h;
}
static u32 fnv_s(u32 h, const char *s) { return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "", 1); }

// The card's music as the matcher sees it: every path with its size and date (a retag changes
// those), and a version for the matcher itself. Another value means a list has to match again.
#define SIG_VERSION 2u   // 2: the lists carry their pictures, descriptions and dates
static u32 lib_sig(void) {
	u32 h = 2166136261u, v = SIG_VERSION, nt = lib.nt;
	h = fnv(h, &v, 4);
	h = fnv(h, &nt, 4);
	for (int i = 0; i < lib.nt; i++) {
		h = fnv_s(h, lib.t[i].path);
		h = fnv(h, &lib.t[i].size, sizeof lib.t[i].size);
		h = fnv(h, &lib.t[i].mtime, sizeof lib.t[i].mtime);
	}
	return h;
}

// Overwritten in place and trimmed, like library.c's saves: FAT keeps the clusters it has.
// Nothing happens when the file already says this.
static bool write_if_new(const char *path, const char *text) {
	size_t n = strlen(text), had = 0;
	char *old = slurp(path, &had);
	bool same = old && had == n && !memcmp(old, text, n);
	free(old);
	if (same) return false;
	FILE *f = card_open_big(path, "r+");
	if (!f) { mkdir(LISTS, 0777); f = card_open_big(path, "w"); }
	if (!f) { crumb("spotify: can't write %s", path); return false; }
	fwrite(text, 1, n, f);
	fflush(f);
	ftruncate(fileno(f), n);
	fclose(f);
	return true;
}

// A name FAT takes: no <>:"/\|?* or control characters, no dots or spaces at the ends, and 120
// bytes at most, cut between characters (library.c's, which a rename on the 3DS uses too)
static void safe_name(const char *title, char *out, int cap) { library_file_name(title, out, cap); }

// ---- playlists ----------------------------------------------------------------------------

#define SNAP "#SNAPSHOT:"
// A file a sync made: the playlist it's from, the title it says, and the stamp on its last line
// (Spotify's snapshot of the list and the card's fingerprint when it was written; NULL if the
// file doesn't end with one, say it was cut off, so it's never trusted)
typedef struct { char *file, *pid, *title, *snap; } Ours;
typedef struct { char **v; int n; } Names;
static void name_add(Names *s, const char *x) { s->v = realloc(s->v, (s->n + 1) * sizeof(char *)); s->v[s->n++] = strdup(x); }
static bool name_has(const Names *s, const char *x) { for (int k = 0; k < s->n; k++) if (!strcasecmp(s->v[k], x)) return true; return false; }
static void names_free(Names *s) { for (int k = 0; k < s->n; k++) free(s->v[k]); free(s->v); *s = (Names){0}; }
static bool name_is(const Names *s, const char *x) { for (int k = 0; k < s->n; k++) if (!strcmp(s->v[k], x)) return true; return false; }

// ---- the 3DS's playlist changes, on their way up ------------------------------------------------

// Changes made on the 3DS to your own Spotify playlists, kept on the card until Spotify has them:
// [{"n": 7, "pid": "...", "name": "...", "desc": "..."}, {"n": 8, "pid": "...", "add": "../a/b.flac"}].
// A list's name and description go as one change, the newest wins; n tells a change apart from
// one made since it was sent. Under lk; the main thread adds, the worker takes away.
#define OUTBOX DIR3DS "/spotify-outbox.json"
static json_t *outbox;
static json_int_t out_seq;
static bool lists_scope;   // worker -> main with done: the pairing can't change playlists

static void outbox_load(void) {   // main thread, at spot_init
	json_t *j = load_json(OUTBOX);
	outbox = json_is_array(j) ? j : json_array();
	if (outbox != j) json_decref(j);
	size_t k;
	json_t *o;
	json_array_foreach(outbox, k, o) { json_int_t n = json_integer_value(json_object_get(o, "n")); if (n > out_seq) out_seq = n; }
}
static bool undo_pending(const char *pid, const char *key, const char *rel);
static void outbox_save(void) {   // under lk
	mkdir(DIR3DS, 0777);
	if (!json_array_size(outbox)) { remove(OUTBOX); return; }
	if (!save_json(outbox, OUTBOX, JSON_INDENT(1))) crumb("spotify: can't save the outbox");
}
static void outbox_pids(Names *out) {   // worker: the lists with a change waiting
	LightLock_Lock(&lk);
	size_t k;
	json_t *o;
	json_array_foreach(outbox, k, o) {
		const char *p = json_string_value(json_object_get(o, "pid"));
		if (p && !name_is(out, p)) name_add(out, p);
	}
	LightLock_Unlock(&lk);
}

void spot_list_details(const char *pid, const char *name, const char *desc, int pub) {
	if (!pid || !outbox) return;
	LightLock_Lock(&lk);
	json_t *o = NULL, *e;
	size_t k;
	json_array_foreach(outbox, k, e) {
		const char *p = json_string_value(json_object_get(e, "pid"));
		if (p && !strcmp(p, pid) && !json_object_get(e, "add") && !json_object_get(e, "del")) o = e;
	}
	if (!o) { o = json_object(); json_object_set_new(o, "pid", json_string(pid)); json_array_append_new(outbox, o); }
	json_object_set_new(o, "n", json_integer(++out_seq));
	if (name) json_object_set_new(o, "name", json_string(name));
	if (desc) json_object_set_new(o, "desc", json_string(desc));
	if (pub >= 0) json_object_set_new(o, "pub", json_boolean(pub));
	outbox_save();
	LightLock_Unlock(&lk);
	spot_request(2000, false, false);
}

void spot_list_remove(const char *pid, int track) {
	if (!pid || !outbox || track < 0 || track >= lib.nt) return;
	char rel[1024];
	library_rel(track, rel, sizeof rel);
	LightLock_Lock(&lk);
	bool undone = undo_pending(pid, "add", rel);   // put in and taken out before it went
	if (!undone) {
		json_t *o = json_object();
		json_object_set_new(o, "pid", json_string(pid));
		json_object_set_new(o, "rm", json_string(rel));
		json_object_set_new(o, "n", json_integer(++out_seq));
		json_array_append_new(outbox, o);
		outbox_save();
	}
	LightLock_Unlock(&lk);
	if (!undone) spot_request(2000, false, false);
}

void spot_list_delete(const char *pid) {
	if (!pid || !outbox) return;
	json_t *o = json_object();
	json_object_set_new(o, "pid", json_string(pid));
	json_object_set_new(o, "del", json_true());
	LightLock_Lock(&lk);
	for (size_t j = json_array_size(outbox); j-- > 0;) {   // what was waiting for it can't land now
		const char *p = json_string_value(json_object_get(json_array_get(outbox, j), "pid"));
		if (p && !strcmp(p, pid)) json_array_remove(outbox, j);
	}
	json_object_set_new(o, "n", json_integer(++out_seq));
	json_array_append_new(outbox, o);
	outbox_save();
	LightLock_Unlock(&lk);
	spot_request(1000, false, false);
}

// A song waiting to go into (or come out of) this list, still in the outbox: dropped, and true
static bool undo_pending(const char *pid, const char *key, const char *rel) {   // (lk)
	for (size_t j = json_array_size(outbox); j-- > 0;) {
		json_t *e = json_array_get(outbox, j);
		const char *p = json_string_value(json_object_get(e, "pid")), *r = json_string_value(json_object_get(e, key));
		if (p && r && !strcmp(p, pid) && !strcmp(r, rel)) { json_array_remove(outbox, j); outbox_save(); return true; }
	}
	return false;
}

void spot_list_add(const char *pid, int track) {
	if (!pid || !outbox || track < 0 || track >= lib.nt) return;
	char rel[1024];
	library_rel(track, rel, sizeof rel);
	LightLock_Lock(&lk);
	bool undone = undo_pending(pid, "rm", rel);   // taken out and put back before it went: nothing to send
	LightLock_Unlock(&lk);
	if (undone) return;
	json_t *o = json_object();
	json_object_set_new(o, "pid", json_string(pid));
	json_object_set_new(o, "add", json_string(rel));
	LightLock_Lock(&lk);
	json_object_set_new(o, "n", json_integer(++out_seq));
	json_array_append_new(outbox, o);
	outbox_save();
	LightLock_Unlock(&lk);
	spot_request(2000, false, false);
}

typedef struct { Str rels, missing; int nmiss; u32 secs; } ListCtx;
static void each_item(json_t *it, void *ud) {
	ListCtx *c = ud;
	json_t *t = json_object_get(it, "item");   // the 2026 shape; "track" is a yes/no there now
	if (!json_is_object(t)) t = json_object_get(it, "track");
	if (!json_is_object(t)) return;
	const char *type = json_string_value(json_object_get(t, "type"));
	if (type && strcmp(type, "track")) return;   // a podcast episode
	SmTrack s;
	sm_of(t, &s);
	c->secs += s.ms / 1000;   // the whole list's length, as Spotify gives it
	int i = sm_match(&s);
	char rel[1024];
	if (i >= 0) {
		// the page's "Date added" and E badge, on lines of their own just before the song
		const char *at = json_string_value(json_object_get(it, "added_at"));
		if (at && strlen(at) >= 10 && at[4] == '-') {
			char d[11];
			memcpy(d, at, 10);
			d[10] = 0;
			put(&c->rels, "#ADDED:");
			put(&c->rels, d);
			put(&c->rels, "\n");
		}
		if (json_is_true(json_object_get(t, "explicit"))) put(&c->rels, "#EXPLICIT\n");
		put(&c->rels, rel_of(i, rel, sizeof rel));
		put(&c->rels, "\n");
	}
	else { put(&c->missing, "# "); label(t, &c->missing); put(&c->missing, "\n"); c->nmiss++; }
}

typedef struct { char *id, *name, *snap, *desc, *img, *oid, *oname; bool pub; int total; bool kept; } Pl;   // kept: it has a file

// Spotify's descriptions come HTML-escaped, sometimes with a link in them: plain text on one line
static char *clean_desc(const char *in) {
	if (!in) return strdup("");
	char *o = malloc(strlen(in) + 1), *w = o;
	static const struct { const char *e; char c; } E[] = {{"&amp;", '&'}, {"&quot;", '"'}, {"&#x27;", '\''}, {"&#39;", '\''},
	                                                     {"&lt;", '<'}, {"&gt;", '>'}, {"&#x2F;", '/'}, {"&nbsp;", ' '}};
	for (const char *r = in; *r;) {
		if (*r == '<') { const char *e = strchr(r, '>'); if (e) { r = e + 1; continue; } }
		if (r[0] == '&' && r[1] == '#') {   // a numbered one, &#8217; or &#x2019;: written out as UTF-8
			char *end;
			unsigned long cp = r[2] == 'x' || r[2] == 'X' ? strtoul(r + 3, &end, 16) : strtoul(r + 2, &end, 10);
			if (*end == ';' && cp && cp < 0x110000) {
				if (cp < 0x80) *w++ = cp < 0x20 ? ' ' : (char)cp;
				else if (cp < 0x800) { *w++ = 0xC0 | cp >> 6; *w++ = 0x80 | (cp & 63); }
				else if (cp < 0x10000) { *w++ = 0xE0 | cp >> 12; *w++ = 0x80 | (cp >> 6 & 63); *w++ = 0x80 | (cp & 63); }
				else { *w++ = 0xF0 | cp >> 18; *w++ = 0x80 | (cp >> 12 & 63); *w++ = 0x80 | (cp >> 6 & 63); *w++ = 0x80 | (cp & 63); }
				r = end + 1;
				continue;
			}
		}
		bool done = false;
		for (size_t k = 0; k < sizeof E / sizeof *E && !done; k++)
			if (!strncmp(r, E[k].e, strlen(E[k].e))) { *w++ = E[k].c; r += strlen(E[k].e); done = true; }
		if (done) continue;
		*w++ = (u8)*r < 0x20 ? ' ' : *r;   // no line breaks or tabs: it's one line in the file
		r++;
	}
	*w = 0;
	return o;
}
static char *one_line(const char *v) {   // for the file: no line breaks, and no '|' where it separates
	char *o = strdup(v ? v : "");
	for (char *c = o; *c; c++) if ((u8)*c < 0x20 || *c == '|') *c = ' ';
	return o;
}
// the smallest picture at least 300 wide, else the first
static const char *pic_url(json_t *imgs) {
	const char *best = NULL, *first = NULL;
	int bw = 1 << 30;
	size_t k;
	json_t *im;
	json_array_foreach(imgs, k, im) {
		const char *u = json_string_value(json_object_get(im, "url"));
		if (!u) continue;
		if (!first) first = u;
		int w = json_integer_value(json_object_get(im, "width"));
		if (w >= 300 && w < bw) { bw = w; best = u; }
	}
	return best ? best : first;
}
typedef struct { Pl *v; int n; } PlList;
static void each_playlist(json_t *it, void *ud) {
	PlList *l = ud;
	const char *id = json_string_value(json_object_get(it, "id"));
	if (!id) return;
	const char *name = json_string_value(json_object_get(it, "name"));
	const char *snap = json_string_value(json_object_get(it, "snapshot_id"));
	const char *img = pic_url(json_object_get(it, "images"));
	json_t *own = json_object_get(it, "owner");
	json_t *tot = json_object_get(json_object_get(it, "items"), "total");   // the 2026 name, then the old one
	if (!json_is_integer(tot)) tot = json_object_get(json_object_get(it, "tracks"), "total");
	l->v = realloc(l->v, (l->n + 1) * sizeof(Pl));
	l->v[l->n++] = (Pl){strdup(id), strdup(name ? name : ""), snap ? strdup(snap) : NULL,
	                    clean_desc(json_string_value(json_object_get(it, "description"))), img ? one_line(img) : NULL,
	                    one_line(json_string_value(json_object_get(own, "id"))), one_line(json_string_value(json_object_get(own, "display_name"))),
	                    json_is_true(json_object_get(it, "public")), (int)json_integer_value(tot)};
}

// A playlist's picture onto the card as lists/<name>.jpg, with the url it came from beside it in
// <name>.url: fetched again only when that changes or the file's missing. Through a .tmp, so a
// cut-off download is never taken for the picture.
// Spotify hands out the same picture from alternating hosts (image-cdn-ak / image-cdn-fa .spotifycdn.com): the picture is
// what comes after the host, so that's what "the same url" means. Compared whole, every launch took half the pictures for
// new ones, downloaded them again and threw away art.c's shrunk copies (the Playlists tab then decoded them all over).
static const char *pic_path(const char *u) {
	const char *s = strstr(u, "://");
	s = s ? strchr(s + 3, '/') : NULL;
	return s ? s : u;
}
static void fetch_pic(const char *url, const char *name) {
	char jp[160], up[160], tmp[170];
	snprintf(jp, sizeof jp, LISTS_DIR "/%s.jpg", name);
	snprintf(up, sizeof up, LISTS_DIR "/%s.url", name);
	char *old = slurp(up, NULL);
	struct stat st;
	bool same = old && !strcmp(pic_path(old), pic_path(url)) && !stat(jp, &st) && st.st_size > 0;
	free(old);
	if (same) return;
	Buf b = {0};
	CURLcode rc = CURLE_OK;
	long code = net_req(curl, "GET", url, NULL, NULL, &b, NULL, &quitting, &rc);   // the picture server needs no login
	if (code == 200 && b.n) {
		snprintf(tmp, sizeof tmp, "%s.tmp", jp);
		FILE *f = card_open_big(tmp, "wb");
		bool ok = f && fwrite(b.d, 1, b.n, f) == b.n;
		if (f) fclose(f);
		if (ok) {
			remove(jp);
			if (!rename(tmp, jp)) {
				f = card_open(up, "wb");
				if (f) { fputs(url, f); fclose(f); }
				for (const char *k = "dat"; *k; k++) {   // art.c's shrunk copies of the old picture: made again from this one
					char rp[180];
					snprintf(rp, sizeof rp, "%s.%c.raw", jp, *k);
					remove(rp);
				}
			}
		} else remove(tmp);
	}
	free(b.d);
}

// You, for your playlists' owner line: your picture as lists/me.jpg and your id in me.id
static void fetch_me(void) {
	Buf b = {0};
	if (api("GET", "/me", &b) != 200) { free(b.d); return; }
	json_t *j = json_loadb(b.d, b.n, 0, NULL);
	free(b.d);
	const char *id = json_string_value(json_object_get(j, "id"));
	const char *nm = json_string_value(json_object_get(j, "display_name"));
	const char *u = pic_url(json_object_get(j, "images"));
	if (nm && nm[0]) {   // your name, for your lists' owner line (theirs says your id)
		FILE *f = card_open(LISTS_DIR "/me.name", "wb");
		if (f) { fputs(nm, f); fclose(f); }
	}
	if (id) {
		char p[96];
		snprintf(p, sizeof p, LISTS_DIR "/me.id");
		char *old = slurp(p, NULL);
		if (!old || strcmp(old, id)) { FILE *f = card_open(p, "wb"); if (f) { fputs(id, f); fclose(f); } }
		free(old);
	}
	if (u) fetch_pic(u, "me");
	json_decref(j);
}

// Lists Spotify won't share with us (403/404), by playlist and stamp, for as long as the app runs:
// asked once, not every five minutes
static Names skipped;
static bool skip_has(const char *pid, const char *stamp) {
	char k[256];
	snprintf(k, sizeof k, "%s %s", pid, stamp);
	for (int j = 0; j < skipped.n; j++) if (!strcmp(skipped.v[j], k)) return true;   // ids care about case
	return false;
}
static void skip_add(const char *pid, const char *stamp) {
	char k[256];
	snprintf(k, sizeof k, "%s %s", pid, stamp);
	if (!skip_has(pid, stamp)) name_add(&skipped, k);
}

static bool sync_lists(void) {
	Ours *ours = NULL;
	int nours = 0;
	Names theirs = {0}, used = {0}, written = {0};
	DIR *d = opendir(LISTS);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			const char *dot = strrchr(e->d_name, '.');
			if (e->d_name[0] == '.' || !dot || (strcasecmp(dot, ".m3u8") && strcasecmp(dot, ".m3u"))) continue;
			char p[768];
			if (snprintf(p, sizeof p, LISTS "/%s", e->d_name) >= (int)sizeof p) continue;
			char *text = slurp(p, NULL), *pid = NULL, *title = NULL, *last = NULL;
			char *sv = NULL;
			for (char *ln = text ? strtok_r(text, "\n", &sv) : NULL; ln; ln = strtok_r(NULL, "\n", &sv)) {
				if (!memcmp(ln, "\xEF\xBB\xBF", 3)) ln += 3;
				ln[strcspn(ln, "\r")] = 0;
				if (!*ln) continue;
				last = ln;
				if (!title && !strncmp(ln, "#PLAYLIST:", 10)) title = strdup(ln + 10);
				char *q = ln;
				while (*q == ' ' || *q == '\t') q++;
				if (pid || strncmp(q, MARK, strlen(MARK))) continue;
				char *v = q + strlen(MARK);
				while (*v == ' ') v++;
				size_t vn = strcspn(v, " \t");
				if (vn) pid = strndup(v, vn);
			}
			char *snap = last && !strncmp(last, SNAP, strlen(SNAP)) ? strdup(last + strlen(SNAP)) : NULL;
			free(text);
			if (pid) {
				ours = realloc(ours, (nours + 1) * sizeof(Ours));
				ours[nours++] = (Ours){strdup(e->d_name), pid, title, snap};
			} else {
				free(title);
				free(snap);   // one of Alex's own: never touched, and its name is taken
				char stem[768];
				snprintf(stem, sizeof stem, "%.*s", (int)(dot - e->d_name), e->d_name);
				name_add(&theirs, stem);
			}
		}
		closedir(d);
	}
	name_add(&used, LIKED);
	for (int k = 0; k < theirs.n; k++) name_add(&used, theirs.v[k]);
	// changed on the 3DS and not on Spotify yet: the card's copy stands, under the name it has
	Names pend = {0};
	outbox_pids(&pend);
	for (int o = 0; o < nours; o++)
		if (name_is(&pend, ours[o].pid)) {
			char stem[768];
			const char *dot = strrchr(ours[o].file, '.');
			snprintf(stem, sizeof stem, "%.*s", dot ? (int)(dot - ours[o].file) : (int)strlen(ours[o].file), ours[o].file);
			name_add(&used, stem);
		}

	PlList pls = {0};
	long code;
	u32 sig = lib_sig();
	int nfetched = 0;
	bool good = pages("/me/playlists?limit=50", each_playlist, &pls, &code);
	char **seen = calloc(pls.n + 1, sizeof(char *));
	int nseen = 0;
	for (int k = 0; good && k < pls.n; k++) {
		const char *pid = pls.v[k].id;
		if (name_is(&pend, pid)) {
			bool any = false;
			for (int o = 0; o < nours; o++) if (!strcmp(ours[o].pid, pid)) { name_add(&written, ours[o].file); any = true; }
			if (any) { seen[nseen++] = (char *)pid; pls.v[k].kept = true; nlists++; continue; }
		}
		char title[512];
		snprintf(title, sizeof title, "%s", pls.v[k].name);
		for (char *c = title; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
		char *t = title;
		while (*t == ' ') t++;
		int tn = strlen(t);
		while (tn && t[tn - 1] == ' ') t[--tn] = 0;
		if (!*t) t = "Untitled playlist";

		// its name first: the file it would be, which a skip has to match
		char name[160], base[128];
		safe_name(t, name, sizeof name);
		if (name_has(&theirs, name) || !strcasecmp(name, LIKED)) {   // one of Alex's has the name
			char both[600];
			snprintf(both, sizeof both, "%s (Spotify)", t);
			safe_name(both, name, sizeof name);
		}
		snprintf(base, sizeof base, "%.120s", name);   // safe_name keeps it to 120
		for (int n = 2; name_has(&used, name); n++) snprintf(name, sizeof name, "%s %d", base, n);   // two with one name
		char dest[768], file[200], stamp[160] = "";
		snprintf(file, sizeof file, "%s.m3u8", name);
		snprintf(dest, sizeof dest, LISTS "/%s", file);
		Pl *pl = &pls.v[k];
		u32 mh = fnv_s(fnv_s(fnv_s(fnv_s(2166136261u, pl->desc), pl->img), pl->oid), pl->oname);
		mh = fnv(mh, &pl->pub, sizeof pl->pub);
		if (pl->snap) snprintf(stamp, sizeof stamp, "%.120s %08lx %08lx", pl->snap, (unsigned long)sig, (unsigned long)mh);
		if (stamp[0] && skip_has(pid, stamp)) continue;   // refused earlier in this run of the app, unchanged since

		// unchanged on Spotify and on the card since we wrote it: nothing to fetch
		bool same = false;
		for (int o = 0; o < nours && stamp[0] && !same; o++)
			same = !strcmp(ours[o].pid, pid) && !strcasecmp(ours[o].file, file) && ours[o].title && !strcmp(ours[o].title, t)
			       && ours[o].snap && !strcmp(ours[o].snap, stamp);
		ListCtx c = {0};
		if (!same) {
			char url[160];
			snprintf(url, sizeof url, "/playlists/%s/items?limit=50", pid);
			if (!pages(url, each_item, &c, &code)) {
				free(c.rels.s);
				free(c.missing.s);
				if (code == 403 || code == 404) {   // Spotify's mixes, some other people's
					crumb("spotify: skipped %s (%ld)", t, code);
					if (stamp[0]) skip_add(pid, stamp);
					continue;
				}
				good = false;
				break;
			}
			nfetched++;
		}
		name_add(&used, name);
		seen[nseen++] = (char *)pid;
		pl->kept = true;
		for (int o = 0; o < nours; o++)   // renamed on Spotify: the old file goes
			if (!strcmp(ours[o].pid, pid) && strcasecmp(ours[o].file, file) && !name_has(&written, ours[o].file)) {
				char p[768];
				snprintf(p, sizeof p, LISTS "/%s", ours[o].file);
				if (!remove(p)) lists_changed = true;
			}
		name_add(&written, file);
		nlists++;
		if (same) continue;
		Str s = {0};
		put(&s, "#EXTM3U\n#PLAYLIST:");
		put(&s, t);
		put(&s, "\n" MARK);
		put(&s, pid);
		put(&s, "\n");
		if (pl->desc[0]) { put(&s, "#DESCRIPTION:"); put(&s, pl->desc); put(&s, "\n"); }
		if (pl->img) { put(&s, "#IMAGE:"); put(&s, pl->img); put(&s, "\n"); }
		if (pl->oid[0]) { put(&s, "#OWNER:"); put(&s, pl->oid); put(&s, "|"); put(&s, pl->oname); put(&s, "\n"); }
		char tl[64];
		snprintf(tl, sizeof tl, "#PUBLIC:%d\n#TOTAL:%d|%lu\n", pl->pub, pl->total, (unsigned long)c.secs);
		put(&s, tl);
		if (c.rels.s) put(&s, c.rels.s);
		if (c.nmiss) {
			char h[48];
			snprintf(h, sizeof h, "# not on the card (%d):\n", c.nmiss);
			put(&s, h);
			put(&s, c.missing.s);
		}
		if (stamp[0]) { put(&s, SNAP); put(&s, stamp); put(&s, "\n"); }   // last: a cut-off file has none
		if (write_if_new(dest, s.s)) lists_changed = true;
		free(s.s);
		free(c.rels.s);
		free(c.missing.s);
	}
	if (good)   // unfollowed or deleted on Spotify (only once the whole list came through)
		for (int o = 0; o < nours; o++) {
			bool still = !strcmp(ours[o].pid, "liked");
			for (int k = 0; k < nseen && !still; k++) still = !strcmp(seen[k], ours[o].pid);
			if (still || name_has(&written, ours[o].file)) continue;
			char p[768];
			snprintf(p, sizeof p, LISTS "/%s", ours[o].file);
			if (!remove(p)) lists_changed = true;
			for (int x = 0; x < 2; x++) {
				snprintf(p, sizeof p, LISTS_DIR "/%s.%s", ours[o].pid, x ? "url" : "jpg");
				remove(p);
			}
		}
	free(seen);
	if (good) crumb("spotify: %d lists, %d fetched, the rest unchanged", nlists, nfetched);
	if (good) {   // the pictures: every list's checked each time, so one that didn't come last time does now
		mkdir(LISTS_DIR, 0777);
		for (int k = 0; k < pls.n && !quitting; k++) if (pls.v[k].img && pls.v[k].kept) fetch_pic(pls.v[k].img, pls.v[k].id);
		fetch_me();
	}
	for (int k = 0; k < pls.n; k++) {
		Pl *pl = &pls.v[k];
		free(pl->id); free(pl->name); free(pl->snap); free(pl->desc); free(pl->img); free(pl->oid); free(pl->oname);
	}
	free(pls.v);
	for (int o = 0; o < nours; o++) { free(ours[o].file); free(ours[o].pid); free(ours[o].title); free(ours[o].snap); }
	free(ours);
	names_free(&theirs);
	names_free(&used);
	names_free(&written);
	names_free(&pend);
	if (!good) fail("Spotify: playlists didn't come through");
	return good;
}

// ---- Liked Songs, both ways ---------------------------------------------------------------

// Per song on the card: in L (the 3DS's likes now), in P (what both sides agreed on last time,
// from the state file), in S (Spotify's likes now)
typedef struct { bool inL, inP, inS; char *pid, *sid, *fid; } Mark;
typedef struct { Mark *m; int *order, n; int missing; } LikedCtx;

static void each_like(json_t *it, void *ud) {
	LikedCtx *c = ud;
	json_t *t = json_object_get(it, "track");
	if (!json_is_object(t)) return;
	SmTrack s;
	sm_of(t, &s);
	int i = sm_match(&s);
	const char *id = json_string_value(json_object_get(t, "id"));
	if (i < 0 || !id) { c->missing++; return; }
	if (c->m[i].inS) return;
	c->m[i].inS = true;
	c->m[i].sid = strdup(id);
	c->order[c->n++] = i;   // Spotify's order: newest first
}

// A Spotify id for a song on the card that Spotify doesn't have as liked: a search by its
// title and artist, kept only if it really is that song. NULL if not found; *bad on an error.
static char *find_id(int i, bool *bad) {
	char artist[2][256];
	bool have[2] = {sm_artist(i, 0, artist[0], 256), sm_artist(i, 1, artist[1], 256)};
	if (!have[0]) return NULL;
	char title[256];
	snprintf(title, sizeof title, "%s", sm_title(i));
	for (char *c = title; *c; c++) if (*c == '"') *c = ' ';
	for (int w = 0; w < 2; w++) {
		if (!have[w] || (w && !strcmp(artist[0], artist[1]))) continue;
		for (char *c = artist[w]; *c; c++) if (*c == '"') *c = ' ';
		char q[1100];
		snprintf(q, sizeof q, "track:\"%s\" artist:\"%s\"", title, artist[w]);
		char *e = curl_easy_escape(curl, q, 0), path[2048];
		snprintf(path, sizeof path, "/search?type=track&limit=5&q=%s", e);
		curl_free(e);
		Buf b = {0};
		long code = api("GET", path, &b);
		if (code != 200) { free(b.d); if (code < 0 || code >= 500 || code == 429) *bad = true; return NULL; }
		json_t *j = json_loadb(b.d, b.n, 0, NULL);
		free(b.d);
		size_t k;
		json_t *t;
		char *got = NULL;
		json_array_foreach(json_object_get(json_object_get(j, "tracks"), "items"), k, t) {
			SmTrack s;
			sm_of(t, &s);
			const char *id = json_string_value(json_object_get(t, "id"));
			if (id && sm_matches(&s, i, 4)) { got = strdup(id); break; }
		}
		json_decref(j);
		if (got) return got;
	}
	return NULL;
}

// PUT hearts, DELETE unhearts, 40 at a time: /me/library is the 2026 way, /me/tracks says 403
static bool library_call(const char *method, char **ids, int n) {
	for (int k = 0; k < n; k += 40) {
		Str s = {0};
		put(&s, "/me/library?uris=");
		for (int j = k; j < n && j < k + 40; j++) {
			if (j > k) put(&s, ",");
			put(&s, "spotify:track:");
			put(&s, ids[j]);
		}
		Buf b = {0};
		long code = api(method, s.s, &b);
		free(s.s);
		free(b.d);
		if (code < 200 || code >= 300) { fail("Spotify: likes didn't go through"); return false; }
	}
	return true;
}

// The outbox up to Spotify, oldest first. False only when the network let us down (what's left
// waits for the next try); a change Spotify refuses, or a song it hasn't got, is dropped and logged.
static bool push_lists(void) {
	LightLock_Lock(&lk);
	json_t *ops = json_array_size(outbox) ? json_deep_copy(outbox) : NULL;
	LightLock_Unlock(&lk);
	if (!ops) return true;
	LightLock_Lock(&tok_lk);
	bool known = tok_scope[0], can = strstr(tok_scope, "playlist-modify-private") && strstr(tok_scope, "playlist-modify-public");
	LightLock_Unlock(&tok_lk);
	if (known && !can) { lists_scope = true; json_decref(ops); return true; }
	json_t *gone = json_array();   // the n of each change that's done with, sent or dropped
	bool good = true;
	size_t k;
	json_t *o;
	json_array_foreach(ops, k, o) {
		if (quitting) { good = false; break; }
		const char *pid = json_string_value(json_object_get(o, "pid"));
		const char *rel = json_string_value(json_object_get(o, "add"));
		json_t *n = json_object_get(o, "n");
		if (!pid || strlen(pid) > 64) { json_array_append(gone, n); continue; }
		char path[200];
		Buf b = {0};
		long code;
		const char *rm = json_string_value(json_object_get(o, "rm"));
		if (json_is_true(json_object_get(o, "del"))) {   // Spotify's Delete: out of your library (DELETE /playlists/{id}/followers is gone)
			snprintf(path, sizeof path, "/me/library?uris=spotify%%3Aplaylist%%3A%.64s", pid);
			code = api("DELETE", path, &b);
			crumb("spotify: list deleted: %ld", code);
		} else if (rm) {   // a song out: DELETE /playlists/{id}/items, {"items": [{"uri": ...}]}
			int i = find_rel(rm);
			if (i < 0) { crumb("spotify: list remove, not on the card now: %s", rm); json_array_append(gone, n); continue; }
			bool bad = false;
			char *id = find_id(i, &bad);
			if (bad) { good = false; break; }
			if (!id) { json_array_append(gone, n); continue; }   // not on Spotify: it was never in Spotify's copy
			char body[160];
			snprintf(body, sizeof body, "{\"items\":[{\"uri\":\"spotify:track:%.40s\"}]}", id);
			free(id);
			snprintf(path, sizeof path, "/playlists/%s/items", pid);
			code = api_b("DELETE", path, body, &b);
		} else if (rel) {
			int i = find_rel(rel);
			if (i < 0) { crumb("spotify: list add, not on the card now: %s", rel); json_array_append(gone, n); continue; }
			bool bad = false;
			char *id = find_id(i, &bad);
			if (bad) { good = false; break; }
			if (!id) { crumb("spotify: not on Spotify, so not added: %s", rel); nmissing++; json_array_append(gone, n); continue; }
			snprintf(path, sizeof path, "/playlists/%s/items?uris=spotify:track:%.40s", pid, id);
			code = api("POST", path, &b);
			if (code == 404) {   // the old name for it
				snprintf(path, sizeof path, "/playlists/%s/tracks?uris=spotify:track:%.40s", pid, id);
				code = api("POST", path, &b);
			}
			free(id);
		} else {
			json_t *body = json_object();
			json_t *nm = json_object_get(o, "name"), *ds = json_object_get(o, "desc");
			if (json_is_string(nm)) json_object_set(body, "name", nm);
			if (json_is_string(ds)) json_object_set(body, "description", ds);
			json_t *pb = json_object_get(o, "pub");
			if (json_is_boolean(pb)) json_object_set(body, "public", pb);
			char *s = json_object_size(body) ? json_dumps(body, JSON_COMPACT) : NULL;
			json_decref(body);
			if (!s) { json_array_append(gone, n); continue; }
			snprintf(path, sizeof path, "/playlists/%s", pid);
			code = api_b("PUT", path, s, &b);
			free(s);
		}
		if (code >= 200 && code < 300) { nsent++; json_array_append(gone, n); }
		else if (code == 403 && !known) { lists_scope = true; crumb("spotify: list change: 403 %.120s", b.d ? b.d : ""); free(b.d); break; }
		else if (code >= 400 && code < 500 && code != 429) { crumb("spotify: list change refused (%ld) %.120s", code, b.d ? b.d : ""); json_array_append(gone, n); }
		else { free(b.d); good = false; break; }
		free(b.d);
	}
	json_decref(ops);
	LightLock_Lock(&lk);   // the done ones out; one changed since it was sent has a new n, and stays
	size_t ng = json_array_size(gone);
	for (size_t j = json_array_size(outbox); ng && j-- > 0;) {
		json_int_t nj = json_integer_value(json_object_get(json_array_get(outbox, j), "n"));
		for (size_t g = 0; g < ng; g++)
			if (json_integer_value(json_array_get(gone, g)) == nj) { json_array_remove(outbox, j); break; }
	}
	if (ng) outbox_save();
	LightLock_Unlock(&lk);
	json_decref(gone);
	return good;
}

static bool sync_liked(void) {
	int n = lib.nt;
	Mark *m = calloc(n ? n : 1, sizeof(Mark));
	int *Lo = malloc((nL + 1) * sizeof(int)), nLo = 0;
	for (int k = 0; k < nL; k++)
		if (L[k] >= 0 && L[k] < n && !m[L[k]].inL) { m[L[k]].inL = true; Lo[nLo++] = L[k]; }

	int nP = 0, *Po = malloc((n + 1) * sizeof(int)), nPo = 0;
	json_t *st = load_json(STATE);
	size_t k;
	json_t *e;
	json_array_foreach(json_object_get(st, "liked"), k, e) {
		nP++;
		const char *rel = json_string_value(json_object_get(e, "rel"));
		int i = rel ? find_rel(rel) : -1;
		if (i < 0 || m[i].inP) continue;   // gone from the card: it can't tell us anything
		m[i].inP = true;
		const char *id = json_string_value(json_object_get(e, "id"));
		m[i].pid = id ? strdup(id) : NULL;
		Po[nPo++] = i;
	}
	// what Spotify's likes looked like after the last sync that sent nothing itself
	json_t *jt = json_object_get(st, "spotify_total"), *jh = json_object_get(st, "spotify_head");
	json_t *js = json_object_get(st, "libsig"), *jm = json_object_get(st, "spotify_missing");
	bool have_head = json_is_integer(jt) && json_is_integer(jh) && json_is_integer(js) && json_is_integer(jm);
	json_int_t was_total = json_integer_value(jt), was_head = json_integer_value(jh);
	json_int_t was_sig = json_integer_value(js), was_missing = json_integer_value(jm);
	json_decref(st);

	// Spotify's newest 50 and how many there are. A like goes on top and an unlike takes one off,
	// so if both match the last sync, and the card's the same, nothing changed there: the rest of
	// the pages (about twenty) aren't needed, and Spotify's likes are the ones the notes have ids for.
	LikedCtx c = {m, malloc((n + 1) * sizeof(int)), 0, 0};
	long code = 0;
	u32 sig = lib_sig(), head = 2166136261u;
	json_int_t total = -1;
	Buf b1 = {0};
	long c1 = api("GET", "/me/tracks?limit=50", &b1);
	json_t *j1 = c1 == 200 ? json_loadb(b1.d, b1.n, 0, NULL) : NULL;
	free(b1.d);
	bool good = j1 != NULL;
	if (good) {
		size_t q;
		json_t *it;
		total = json_integer_value(json_object_get(j1, "total"));
		json_array_foreach(json_object_get(j1, "items"), q, it) {   // uri, not id: local files have none
			head = fnv_s(head, json_string_value(json_object_get(json_object_get(it, "track"), "uri")));
			head = fnv_s(head, json_string_value(json_object_get(it, "added_at")));
		}
		if (have_head && total == was_total && (json_int_t)head == was_head && (json_int_t)sig == was_sig) {
			for (int q2 = 0; q2 < nPo; q2++) {
				Mark *x = &m[Po[q2]];
				if (!x->pid || x->inS) continue;
				x->inS = true;
				x->sid = strdup(x->pid);
				c.order[c.n++] = Po[q2];
			}
			c.missing = (int)was_missing;
			crumb("spotify: likes unchanged on Spotify (%lld), first page only", (long long)total);
		} else {
			json_array_foreach(json_object_get(j1, "items"), q, it) each_like(it, &c);
			const char *nx = json_string_value(json_object_get(j1, "next"));
			if (nx) good = pages(nx, each_like, &c, &code);
		}
	}
	json_decref(j1);
	if (!good) fail("Spotify: likes didn't come through");
	nmissing = c.missing;

	int *gh = malloc((n + 1) * sizeof(int)), ngh = 0;   // unliked on the 3DS: unheart
	int *nh = malloc((n + 1) * sizeof(int)), nnh = 0;   // liked on the 3DS: heart
	add = malloc((n + 1) * sizeof(int));
	drop = malloc((n + 1) * sizeof(int));
	nadd = ndrop = 0;
	if (good) {
		for (int j = 0; j < nLo; j++) { Mark *x = &m[Lo[j]]; if (!x->inP && !x->inS) nh[nnh++] = Lo[j]; }
		for (int j = 0; j < nPo; j++) { Mark *x = &m[Po[j]]; if (!x->inL && x->inS) gh[ngh++] = Po[j]; }
		for (int j = 0; j < c.n; j++) { Mark *x = &m[c.order[j]]; if (!x->inL && !x->inP) add[nadd++] = c.order[j]; }
		for (int j = 0; j < nLo; j++) { Mark *x = &m[Lo[j]]; if (x->inP && x->pid && !x->inS) drop[ndrop++] = Lo[j]; }
		// a lot gone at once looks like a wiped list, not a change of heart: put them back
		if (ngh > 10 && ngh * 4 > nP) {
			crumb("spotify: %d likes gone at once, putting them back", ngh);
			for (int j = 0; j < ngh; j++) add[nadd++] = gh[j];
			nguard = ngh;
			ngh = 0;
			guarded = true;
		}
	}
	// hearts first: a song not found on Spotify stays liked here, without an id
	char **ids = malloc((n + 1) * sizeof(char *));
	int nids = 0;
	for (int j = 0; good && j < nnh && !quitting; j++) {
		bool bad = false;
		char *id = find_id(nh[j], &bad);
		if (bad) { good = false; break; }
		if (id) { m[nh[j]].fid = id; ids[nids++] = id; }
		else crumb("spotify: not on Spotify: %s", lib.t[nh[j]].path);
	}
	if (quitting) good = false;
	if (good && nids) good = library_call("PUT", ids, nids);
	int hearted = nids;
	nids = 0;
	for (int j = 0; j < ngh; j++) ids[nids++] = m[gh[j]].sid;
	if (good && nids) good = library_call("DELETE", ids, nids);
	free(ids);
	if (good) nsent += hearted + ngh;

	if (good && !quitting) {   // what both sides agree on now: Spotify's news on top, then the 3DS's
		json_t *snap = json_array();
		bool *put_in = calloc(n ? n : 1, 1), *dropped = calloc(n ? n : 1, 1);
		for (int j = 0; j < ndrop; j++) dropped[drop[j]] = true;
		char rel[1024];
		for (int pass = 0; pass < 2; pass++) {
			int *v = pass ? Lo : add, nv = pass ? nLo : nadd;
			for (int j = 0; j < nv; j++) {
				int i = v[j];
				if (put_in[i] || dropped[i]) continue;
				put_in[i] = true;
				Mark *x = &m[i];
				const char *id = x->sid ? x->sid : x->fid ? x->fid : x->pid;
				json_t *o = json_object();
				json_object_set_new(o, "rel", json_string(rel_of(i, rel, sizeof rel)));
				json_object_set_new(o, "id", id ? json_string(id) : json_null());
				json_array_append_new(snap, o);
			}
		}
		free(put_in);
		free(dropped);
		char when[32];
		time_t now = time(NULL);
		struct tm tmv;
	strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime_r(&now, &tmv));
		json_t *out = json_pack("{s:o, s:s}", "liked", snap, "synced", when);
		// Spotify's first page as it is now, for the next sync's shortcut, but only when this sync
		// changed nothing there itself: otherwise an undo on the phone could put it back to these
		// very values and hide the undo
		if (out && nsent == 0 && !guarded) {
			json_object_set_new(out, "spotify_total", json_integer(total));
			json_object_set_new(out, "spotify_head", json_integer(head));
			json_object_set_new(out, "libsig", json_integer(sig));
			json_object_set_new(out, "spotify_missing", json_integer(nmissing));
		}
		mkdir(DIR3DS, 0777);
		if (!save_json(out, STATE, JSON_INDENT(1))) { fail("Spotify: can't save its notes"); good = false; }
		json_decref(out);
	}
	if (!good) nadd = ndrop = 0;   // nothing half done reaches the list

	for (int i = 0; i < n; i++) { free(m[i].pid); free(m[i].sid); free(m[i].fid); }
	free(m); free(Lo); free(Po); free(c.order); free(gh); free(nh);
	return good;
}

// The quick one, after a like or an unlike on the 3DS: those go up, nothing comes down (Spotify's
// own changes wait for the next whole sync). L against the notes only: liked here and not in them,
// heart it; in them with an id and not liked here, unheart it. 1 done, 0 failed, -1 it needs the
// whole sync (no notes yet, or so many gone at once that the whole sync's guard has to look).
static int push_liked(void) {
	json_t *st = load_json(STATE);
	if (!json_is_array(json_object_get(st, "liked"))) { json_decref(st); return -1; }
	int n = lib.nt;
	Mark *m = calloc(n ? n : 1, sizeof(Mark));
	int *Lo = malloc((nL + 1) * sizeof(int)), nLo = 0;
	for (int k = 0; k < nL; k++)
		if (L[k] >= 0 && L[k] < n && !m[L[k]].inL) { m[L[k]].inL = true; Lo[nLo++] = L[k]; }
	int nP = 0, *Po = malloc((n + 1) * sizeof(int)), nPo = 0;
	size_t k;
	json_t *e;
	json_array_foreach(json_object_get(st, "liked"), k, e) {   // only songs the card still has
		nP++;
		const char *rel = json_string_value(json_object_get(e, "rel"));
		int i = rel ? find_rel(rel) : -1;
		if (i < 0 || m[i].inP) continue;
		m[i].inP = true;
		const char *id = json_string_value(json_object_get(e, "id"));
		m[i].pid = id ? strdup(id) : NULL;
		Po[nPo++] = i;
	}
	json_decref(st);
	int *nh = malloc((nLo + 1) * sizeof(int)), nnh = 0, *gh = malloc((nPo + 1) * sizeof(int)), ngh = 0;
	for (int j = 0; j < nLo; j++) if (!m[Lo[j]].inP) nh[nnh++] = Lo[j];
	for (int j = 0; j < nPo; j++) if (!m[Po[j]].inL && m[Po[j]].pid) gh[ngh++] = Po[j];
	int r = ngh > 10 && ngh * 4 > nP ? -1 : 1;
	char **ids = malloc((nnh + ngh + 1) * sizeof(char *));
	int nids = 0;
	for (int j = 0; r == 1 && j < nnh && !quitting; j++) {
		bool bad = false;
		char *id = find_id(nh[j], &bad);
		if (bad) { r = 0; break; }
		if (id) { m[nh[j]].fid = id; ids[nids++] = id; }
		else crumb("spotify: not on Spotify: %s", lib.t[nh[j]].path);
	}
	if (quitting && r == 1) r = 0;
	if (r == 1 && nids && !library_call("PUT", ids, nids)) r = 0;
	int hearted = nids;
	nids = 0;
	for (int j = 0; j < ngh; j++) ids[nids++] = m[gh[j]].pid;
	if (r == 1 && nids && !library_call("DELETE", ids, nids)) r = 0;
	free(ids);
	if (r == 1 && (nnh || ngh)) {   // the notes: the 3DS's list as it is now, each with its id
		json_t *snap = json_array();
		char rel[1024];
		for (int j = 0; j < nLo; j++) {
			Mark *x = &m[Lo[j]];
			const char *id = x->fid ? x->fid : x->pid;
			json_t *o = json_object();
			json_object_set_new(o, "rel", json_string(rel_of(Lo[j], rel, sizeof rel)));
			json_object_set_new(o, "id", id ? json_string(id) : json_null());
			json_array_append_new(snap, o);
		}
		// unliked here but never found on Spotify: kept, so if the phone has liked it since, the
		// whole sync still sees an unlike from the 3DS rather than a new like from Spotify
		for (int j = 0; j < nPo; j++) {
			Mark *x = &m[Po[j]];
			if (x->inL || x->pid) continue;
			json_t *o = json_object();
			json_object_set_new(o, "rel", json_string(rel_of(Po[j], rel, sizeof rel)));
			json_object_set_new(o, "id", json_null());
			json_array_append_new(snap, o);
		}
		char when[32];
		time_t now = time(NULL);
		struct tm tmv;
		strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime_r(&now, &tmv));
		// no first-page notes: what we just sent moved Spotify's, so the next whole sync reads it all
		json_t *out = json_pack("{s:o, s:s}", "liked", snap, "synced", when);
		mkdir(DIR3DS, 0777);
		if (!save_json(out, STATE, JSON_INDENT(1))) { fail("Spotify: can't save its notes"); r = 0; }
		json_decref(out);
		if (r == 1) nsent += hearted + ngh;
	}
	for (int i = 0; i < n; i++) { free(m[i].pid); free(m[i].fid); }
	free(m); free(Lo); free(Po); free(nh); free(gh);
	return r;
}

// ---- the thread ---------------------------------------------------------------------------

static void sync_once(bool full) {
	err[0] = 0;
	lists_changed = guarded = was_push = lists_scope = false;
	nlists = nsent = nmissing = nguard = 0;
	u64 t0 = osGetTime();
	crumb("spotify: %s starts, %d liked here", full ? "sync" : "push", nL);
	ok = net_ok();
	if (ok) { sm_build(); ok = refresh(); }
	if (ok) ok = push_lists();   // before the lists come down, so they come down with the changes in
	if (ok && !full) {
		int r = push_liked();
		if (r < 0) { full = true; crumb("spotify: the push needs the whole sync"); }
		else { ok = r == 1; was_push = true; }
	}
	if (ok && full) ok = sync_lists();
	if (ok && full) ok = sync_liked();
	crumb("spotify: %s %s in %llu ms: %d lists, +%d -%d, %d sent, %d missing%s%s", was_push ? "push" : "sync",
	      ok ? "done" : "failed", osGetTime() - t0, nlists, nadd, ndrop, nsent, nmissing, err[0] ? ": " : "", err);
}

static void worker(void *arg) {
	(void)arg;
	while (!quitting) {
		LightEvent_Wait(&wake);
		LightLock_Lock(&lk);
		bool mine = go, full = run_full;
		go = false;
		LightLock_Unlock(&lk);
		if (!mine || quitting) continue;
		sync_once(full);
		LightLock_Lock(&lk);
		done = true;
		LightLock_Unlock(&lk);
	}
}

void spot_init(void) { LightLock_Init(&lk); LightLock_Init(&tok_lk); spot_paired(); if (!outbox) outbox_load(); }

static bool start_thread(void) {
	if (th) return true;
	LightEvent_Init(&wake, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	th = threadCreate(worker, NULL, 256 * 1024, prio + 4 < 0x3F ? prio + 4 : 0x3F, 0, false);
	if (!th) crumb("spotify: no thread");
	return th != NULL;
}

// ---- the main thread's side ---------------------------------------------------------------

void spot_request(int delay_ms, bool loud, bool full) {
	if (!spot_paired()) {
		if (loud) snprintf(later, sizeof later, "Spotify: pair on a PC first");
		return;
	}
	u64 at = osGetTime() + (delay_ms > 0 ? delay_ms : 0);
	if (!waiting) { due = at; wait_loud = loud; wait_full = full; }
	else {
		if (loud || wait_loud) { if (at < due) due = at; wait_loud |= loud; }
		else due = at;   // a run of likes: one push after the last
		wait_full |= full;   // a whole sync waiting does the push's work too
	}
	waiting = true;
}

static void free_result(void) { free(add); free(drop); add = drop = NULL; nadd = ndrop = 0; }

// what it did, in a toast's 48 characters
static bool result_line(char *s, int cap, bool loud) {
	if (!ok) { snprintf(s, cap, "%s", err[0] ? err : "Spotify: sync failed"); return loud; }
	if (lists_scope) { snprintf(s, cap, "Pair again on a PC to edit Spotify lists"); return true; }
	if (was_push) {
		if (nsent == 1) snprintf(s, cap, "Sent to Spotify");
		else snprintf(s, cap, "Sent %d to Spotify", nsent);
		return nsent > 0;
	}
	if (guarded) { snprintf(s, cap, "Spotify: put back %d likes", nguard); return true; }
	int n = snprintf(s, cap, "Spotify: %d list%s", nlists, nlists == 1 ? "" : "s");
	if ((nadd || ndrop) && n < cap) n += snprintf(s + n, cap - n, ", +%d -%d likes", nadd, ndrop);
	if (nsent && n < cap) snprintf(s + n, cap - n, ", %d sent", nsent);
	return loud || nadd || ndrop || nsent;
}

static void apply(void (*say)(const char *), void (*reloaded)(bool)) {
	if (ok) {
		library_liked_apply(add, nadd, drop, ndrop);
		if (lists_changed) library_reload_lists();
		if (reloaded && (lists_changed || nadd || ndrop)) reloaded(lists_changed);
	}
	char line[48];
	if (result_line(line, sizeof line, run_loud) && say) say(line);
	free_result();
	free(L);
	L = NULL;
	nL = 0;
	if (!was_push) next_auto = osGetTime() + AUTO_MS;   // the next quiet one, from the last whole one's end
}

void spot_poll(void (*say)(const char *), void (*reloaded)(bool lists)) {
	if (later[0]) { if (say) say(later); later[0] = 0; }
	if (running) {
		LightLock_Lock(&lk);
		bool fin = done;
		done = false;
		LightLock_Unlock(&lk);
		if (!fin) return;
		running = false;
		apply(say, reloaded);
	}
	if (!waiting && next_auto && osGetTime() >= next_auto) { next_auto = 0; spot_request(0, false, true); }
	if (!waiting || osGetTime() < due || !lib.nl) return;
	if (!start_thread()) { waiting = false; return; }
	Group *g = &lib.lists[0];   // the 3DS's likes as of now; the main thread owns the list
	L = malloc((g->n + 1) * sizeof(int));
	memcpy(L, g->songs, g->n * sizeof(int));
	nL = g->n;
	run_loud = wait_loud;
	waiting = false;
	running = true;
	if (run_loud && say) say("Syncing with Spotify...");
	LightLock_Lock(&lk);
	run_full = wait_full;
	go = true;
	LightLock_Unlock(&lk);
	LightEvent_Signal(&wake);
}

void spot_quit(void) {   // stop now; spot_exit joins
	quitting = true;
	if (th) LightEvent_Signal(&wake);
}

void spot_exit(void) {
	if (!th) return;
	quitting = true;
	LightEvent_Signal(&wake);
	threadJoin(th, U64_MAX);
	threadFree(th);
	th = NULL;
	// a sync that finished as we closed: its likes still reach the card, or the next sync
	// would take them for unlikes
	if (running && done) apply(NULL, NULL);
	running = false;
	if (curl) curl_easy_cleanup(curl);
	curl = NULL;
}
