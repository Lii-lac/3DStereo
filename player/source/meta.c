// Artist pages and lyrics: one thread, one curl handle, everything cached under
// sdmc:/3ds/player/artists and sdmc:/3ds/player/lyrics.
//
// An artist's page is open.spotify.com/artist/<id>: to a client that isn't a browser it comes
// with the page's data inside, base64 JSON in <script id="initialState">. The top songs' lengths
// come from the small embed page. Finding the id takes the Web API's search (Spotify's login from
// spotify.c), once per artist; ids.json remembers it.
#include "cardio.h"
#include "meta.h"
#include "audio.h"
#include "crumb.h"
#include "library.h"
#include "net.h"
#include "spotify.h"
#include "spotmatch.h"
#include <ctype.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "stb_image.h"

#define DIR3DS  "sdmc:/3ds/player"
#define ADIR    DIR3DS "/artists"
#define IDS     ADIR "/ids.json"
#define IMGDIR  ADIR "/img"
#define FACEDIR ADIR "/faces"   // the Artists tab's round faces, ready made: <id>.raw
#define LDIR    DIR3DS "/lyrics"
#define API     "https://api.spotify.com/v1"
#define FRESH_DAYS 7     // an artist's page; the work ahead of time refreshes it a day early
#define NONE_DAYS  14    // "not on Spotify", before it's searched for again ahead of time
#define NOLYR_DAYS 14

static Thread th;
static LightLock lk;
static LightEvent wake;
static volatile bool quitting;
static CURL *curl;
static bool net_tried;

// ---- requests and results, under lk -----------------------------------------------------------

static int a_gen, a_done_gen;          // the newest artist request, and the last one answered
static int a_shown_gen = -1;           // ...and the last one the main thread answered itself from the card (card_now)
// The page builder's things, which the main thread (card_now) and the worker both use: jobs, njobs,
// next pictures' flags and gathered pixels, and the shrunk pictures kept in memory. Taken before lk.
static LightLock blk;
static char a_local[256], a_id[32];
static Artist *a_ready;                // answered, not yet taken
static MState a_state;
static char a_why[48];

typedef struct { char *url; int kind; } ImgJob;   // kind 0: header, 1: square, 2: round
static ImgJob jobs[MI_SLOTS];
static int njobs, next_job, jobs_gen;
// Pictures ready for the main thread, oldest first: a whole page's can wait at once, so a page
// whose pictures are already shrunk arrives with all of them in the same frame
typedef struct { int slot, gen, w, h; u32 *px; } OutImg;
static OutImg outq[MI_SLOTS];
static int nout;
#define QUICK_N 12   // pictures a page the worker builds sends with it (card_now: only the photo)
static bool jdone[MI_SLOTS];   // the page's jobs already handed over (by do_artist's quick pass)
static void outq_clear(void) { for (int k = 0; k < nout; k++) free(outq[k].px); nout = 0; }   // under lk

static int l_want = -1, l_done = -2;

// The work ahead of time is running: a 429 isn't waited out but ends it for a while, and the
// searches stop between requests when something real is asked for
static bool pf_on, pf_429;
// A page or lyrics asked for while the work ahead of time is on the network: that transfer stops
// (the step is tried again later), so a tap never waits behind someone else's photo
static volatile bool pf_cut;
static Lyrics *l_ready;
static MState l_state;

// ---- small things -------------------------------------------------------------------------------

static void nap(int ms) { for (; ms > 0 && !quitting; ms -= 100) svcSleepThread(100 * 1000000LL); }

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

// jansson's json_load_file reads through stdio's 1 KB buffer: whole file in one go instead
static json_t *json_card(const char *path) {
	size_t n = 0;
	char *s = slurp(path, &n);
	if (!s) return NULL;
	json_t *j = json_loadb(s, n, 0, NULL);
	free(s);
	return j;
}

static bool spit(const char *path, const void *d, size_t n) {
	char tmp[160];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = card_open_big(tmp, "wb");
	if (!f) return false;
	bool ok = fwrite(d, 1, n, f) == n;
	ok = !fclose(f) && ok;
	if (ok) { remove(path); ok = !rename(tmp, path); }
	else remove(tmp);
	return ok;
}

static void dirs(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(DIR3DS, 0777);
	mkdir(ADIR, 0777);
	mkdir(IMGDIR, 0777);
	mkdir(FACEDIR, 0777);
	mkdir(LDIR, 0777);
}

static u32 fnv(const char *s) { u32 h = 2166136261u; while (*s) h = (h ^ (u8)*s++) * 16777619u; return h; }

static long days_since(time_t t) { time_t now = time(NULL); return now > t ? (long)((now - t) / 86400) : 0; }

static bool online(void) {
	if (!net_tried) {
		net_tried = true;
		char e[40];
		if (!net_up(e, sizeof e)) crumb("meta: %s", e);
	}
	if (!net_wifi()) return false;
	if (!curl) curl = net_handle();
	return curl != NULL;
}

static long get(const char *url, const char *auth, Buf *b) {
	for (int tries = 0;; tries++) {
		long ra = 0, code = net_req(curl, "GET", url, NULL, auth, b, &ra, pf_on ? &pf_cut : &quitting, NULL);
		if (pf_on && pf_cut) return -1;
		if (code == 429 && pf_on) { pf_429 = true; return code; }
		if (code == 429 && tries < 2) { nap(ra > 0 && ra <= 10 ? (int)ra * 1000 + 500 : 3000); continue; }
		return code;
	}
}

static const char *js(json_t *o, const char *k) { return json_string_value(json_object_get(o, k)); }
static json_t *jp(json_t *o, const char *path) {   // "a.b.c"
	char k[64];
	while (o && *path) {
		int n = strcspn(path, ".");
		snprintf(k, sizeof k, "%.*s", n, path);
		o = json_object_get(o, k);
		path += n + (path[n] == '.');
	}
	return o;
}
static char *dup0(const char *s) { return s ? strdup(s) : NULL; }

// "spotify:artist:4cJK..." -> "4cJK..."
static const char *tail(const char *uri) { const char *c = uri ? strrchr(uri, ':') : NULL; return c ? c + 1 : uri; }

// ---- base64 and HTML ------------------------------------------------------------------------

static int b64v(int c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}
static char *unb64(const char *s, size_t n, size_t *out) {
	char *o = malloc(n * 3 / 4 + 4);
	if (!o) return NULL;
	u32 acc = 0;
	int bits = 0;
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		int v = b64v((u8)s[i]);
		if (v < 0) continue;
		acc = acc << 6 | v;
		if ((bits += 6) >= 8) { bits -= 8; o[k++] = acc >> bits & 255; }
	}
	o[k] = 0;
	*out = k;
	return o;
}

// The page's data: the text between <script id="initialState" ...> and </script>
static json_t *initial_state(const char *html) {
	const char *a = strstr(html, "id=\"initialState\"");
	if (!a || !(a = strchr(a, '>'))) return NULL;
	const char *b = strstr(++a, "</script>");
	if (!b) return NULL;
	size_t n = 0;
	char *j = unb64(a, b - a, &n);
	json_t *r = j ? json_loadb(j, n, 0, NULL) : NULL;
	free(j);
	return r;
}

static json_t *next_data(const char *html) {
	const char *a = strstr(html, "id=\"__NEXT_DATA__\"");
	if (!a || !(a = strchr(a, '>'))) return NULL;
	const char *b = strstr(++a, "</script>");
	return b ? json_loadb(a, b - a, 0, NULL) : NULL;
}

// A biography as plain text: tags out, the common entities back to characters, runs of space
// folded, paragraphs kept as one newline
static char *plain(const char *s) {
	if (!s) return NULL;
	size_t n = strlen(s);
	char *o = malloc(n + 1), *w = o;
	bool sp = true;
	while (*s) {
		if (*s == '<') {
			bool br = !strncasecmp(s, "<br", 3) || !strncasecmp(s, "<p", 2) || !strncasecmp(s, "</p", 3);
			while (*s && *s != '>') s++;
			if (*s) s++;
			if (br && w > o && w[-1] != '\n') { while (w > o && w[-1] == ' ') w--; *w++ = '\n'; sp = true; }
			continue;
		}
		if (*s == '&') {
			static const struct { const char *e, *c; } E[] = {{"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"},
			                                                 {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}};
			bool hit = false;
			for (unsigned k = 0; k < sizeof E / sizeof *E && !hit; k++)
				if (!strncmp(s, E[k].e, strlen(E[k].e))) { *w++ = E[k].c[0]; s += strlen(E[k].e); hit = true; }
			if (hit) { sp = false; continue; }
		}
		if (*s == '\r' || *s == '\n' || *s == '\t' || *s == ' ') {
			if (*s == '\n' && s[1] == '\n' && w > o && w[-1] != '\n') { *w++ = '\n'; sp = true; s += 2; continue; }
			if (!sp) { *w++ = ' '; sp = true; }
			s++;
			continue;
		}
		*w++ = *s++;
		sp = false;
	}
	while (w > o && (w[-1] == ' ' || w[-1] == '\n')) w--;
	*w = 0;
	if (!o[0]) { free(o); return NULL; }
	return o;
}

// ---- the artist's id ------------------------------------------------------------------------

static bool same_name(const char *a, const char *b) {
	char x[256], y[256];
	sm_norm(a, x, sizeof x);
	sm_norm(b, y, sizeof y);
	return x[0] && !strcmp(x, y);
}

// Is `name` one of the artists on song i? The tag's "A, B & C" split up.
static bool on_song(int i, const char *name) {
	const char *s = lib.t[i].artist;
	char part[256];
	while (s && *s) {
		int n = 0;
		while (s[n] && s[n] != ',' && s[n] != ';' && strncmp(s + n, " & ", 3) && strncasecmp(s + n, " feat. ", 7) &&
		       strncasecmp(s + n, " feat ", 6) && strncasecmp(s + n, " ft. ", 5)) n++;
		snprintf(part, sizeof part, "%.*s", n, s);
		char *p = part;
		while (*p == ' ') p++;
		int m = strlen(p);
		while (m && p[m - 1] == ' ') p[--m] = 0;
		if (same_name(p, name)) return true;
		s += n;
		if (!strncasecmp(s, " feat. ", 7)) s += 7;
		else if (!strncasecmp(s, " feat ", 6)) s += 6;
		else if (!strncasecmp(s, " ft. ", 5)) s += 5;
		else if (!strncmp(s, " & ", 3)) s += 3;
		else if (*s) s++;
	}
	return false;
}

bool meta_on_song(int track, const char *name) { return track >= 0 && track < lib.nt && name && on_song(track, name); }

static char token[1024], tok_why[48];   // tok_why: why there's no token, for the page
static long api(const char *path, Buf *b) {
	char url[2048], auth[1100];
	snprintf(url, sizeof url, API "%s", path);
	if (!token[0] && !spot_token(curl, NULL, token, sizeof token, tok_why, sizeof tok_why, &quitting)) return -2;
	for (int t = 0; t < 2; t++) {
		snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
		long code = get(url, auth, b);
		if (code != 401 || t) return code;
		char stale[1024];
		snprintf(stale, sizeof stale, "%s", token);
		if (!spot_token(curl, stale, token, sizeof token, tok_why, sizeof tok_why, &quitting)) { token[0] = 0; return -2; }
	}
	return -1;
}

static void sm_of(json_t *t, SmTrack *s) {
	memset(s, 0, sizeof *s);
	s->name = js(t, "name");
	size_t k;
	json_t *a;
	json_array_foreach(json_object_get(t, "artists"), k, a) if (js(a, "name") && s->na < 8) s->artists[s->na++] = js(a, "name");
	s->ms = json_integer_value(json_object_get(t, "duration_ms"));
}

static void quote_safe(char *s) { for (; *s; s++) if (*s == '"') *s = ' '; }

// The id by a song of theirs on the card (the surest way: the search has to find that very
// song), or else by the name alone. "" if neither works; *why says what went wrong.
// Asked for by the person holding it, while the work ahead of time has the thread
static bool asked_now(void) {
	LightLock_Lock(&lk);
	int k = next_job;
	while (k < njobs && jdone[k]) k++;   // the open page's pictures still to send count too
	bool r = a_gen != a_done_gen || (l_want >= 0 && l_want != l_done && l_want < lib.nt) || (jobs_gen == a_gen && k < njobs);
	LightLock_Unlock(&lk);
	return r;
}
static bool give_way(void) { return pf_on && asked_now(); }
// The worker's priority: above the main thread's while it answers a tap or waits, below it for the
// slow things (see worker)
static s32 prio_hi = 0x2F, prio_lo = 0x33;
static void prio(bool hi) { svcSetThreadPriority(CUR_THREAD_HANDLE, hi ? prio_hi : prio_lo); }   // the worker only

// A pause between pieces of work ahead of time that ends the moment a page or lyrics are asked for
// (waiting above the main thread, so a tap gets the core at once)
static void nap_w(int ms) {
	prio(true);
	u64 end = osGetTime() + ms;
	while (!quitting && !asked_now()) {
		u64 now = osGetTime();
		if (now >= end) break;
		LightEvent_WaitTimeout(&wake, (s64)(end - now) * 1000000LL);
	}
}

static void find_artist(const char *name, char *id, int cap, char *why, int wcap) {
	id[0] = 0;
	int tried = 0;
	bool answered = true;   // every search came back and made sense: only then is it "not on Spotify"
	for (int i = 0; i < lib.nt && tried < 3 && !id[0] && !quitting && !pf_429 && !give_way(); i++) {
		if (!on_song(i, name)) continue;
		tried++;
		char q[700], t[300], a[300];
		snprintf(t, sizeof t, "%s", sm_title(i));
		snprintf(a, sizeof a, "%s", name);
		quote_safe(t);
		quote_safe(a);
		snprintf(q, sizeof q, "track:\"%s\" artist:\"%s\"", t, a);
		char *e = curl_easy_escape(curl, q, 0), path[1600];
		snprintf(path, sizeof path, "/search?type=track&limit=5&q=%s", e);
		curl_free(e);
		Buf b = {0};
		long code = api(path, &b);
		if (code == -2) { snprintf(why, wcap, "%s", tok_why[0] ? tok_why : "Spotify: can't sign in"); free(b.d); return; }
		json_t *j = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
		free(b.d);
		if (!j) answered = false;
		size_t k, m;
		json_t *tr, *ar;
		json_array_foreach(jp(j, "tracks.items"), k, tr) {
			SmTrack s;
			sm_of(tr, &s);
			if (!sm_matches(&s, i, 4)) continue;
			json_array_foreach(json_object_get(tr, "artists"), m, ar)
				if (js(ar, "id") && js(ar, "name") && same_name(js(ar, "name"), name)) { snprintf(id, cap, "%s", js(ar, "id")); break; }
			if (id[0]) break;
		}
		json_decref(j);
	}
	if (id[0] || quitting || pf_429) return;
	if (give_way()) { snprintf(why, wcap, "later"); return; }
	char q[400], a[300];
	snprintf(a, sizeof a, "%s", name);
	quote_safe(a);
	snprintf(q, sizeof q, "artist:\"%s\"", a);
	char *e = curl_easy_escape(curl, q, 0), path[1200];
	snprintf(path, sizeof path, "/search?type=artist&limit=10&q=%s", e);
	curl_free(e);
	Buf b = {0};
	long code = api(path, &b);
	if (code == -2) { snprintf(why, wcap, "%s", tok_why[0] ? tok_why : "Spotify: can't sign in"); free(b.d); return; }
	json_t *j = code == 200 ? json_loadb(b.d, b.n, 0, NULL) : NULL;
	free(b.d);
	size_t k;
	json_t *ar;
	json_array_foreach(jp(j, "artists.items"), k, ar)
		if (js(ar, "id") && js(ar, "name") && same_name(js(ar, "name"), name)) { snprintf(id, cap, "%s", js(ar, "id")); break; }
	bool parsed = j != NULL;
	json_decref(j);
	if (!id[0] && !why[0])
		snprintf(why, wcap, code == 200 && parsed && answered ? "Not on Spotify" : code < 0 ? "Can't reach Spotify" : "Spotify said no");
}

// ids.json: name -> Spotify id, or "none:<unix time>" when the work ahead of time found nothing
// (an artist opened by hand is always searched for again)
// Names to Spotify ids, read from the card once and kept (up to build 36 every look-up parsed
// ids.json off the card again: every face at launch, every artist ahead of time, every tap).
// Both threads use it (card_now on the main one), under its own lock.
static json_t *ids_mem;
static LightLock idlk;
static bool idlk_ok;
static json_t *ids_lock(void) {
	if (!idlk_ok) { LightLock_Init(&idlk); idlk_ok = true; }   // meta_init's, or the first use (tests)
	LightLock_Lock(&idlk);
	if (!ids_mem) {
		ids_mem = json_card(IDS);
		if (!json_is_object(ids_mem)) { json_decref(ids_mem); ids_mem = json_object(); }
	}
	return ids_mem;
}
static void id_cached(const char *name, char *id, int cap) {
	const char *v = js(ids_lock(), name);
	snprintf(id, cap, "%s", v && strncmp(v, "none:", 5) ? v : "");
	LightLock_Unlock(&idlk);
}
static bool id_none_lately(const char *name) {
	const char *v = js(ids_lock(), name);
	bool r = v && !strncmp(v, "none:", 5) && days_since((time_t)strtoll(v + 5, NULL, 10)) < NONE_DAYS;
	LightLock_Unlock(&idlk);
	return r;
}
static void id_save(const char *name, const char *id) {
	json_t *m = ids_lock();
	json_object_set_new(m, name, json_string(id));
	char *s = json_dumps(m, JSON_INDENT(1) | JSON_SORT_KEYS);
	LightLock_Unlock(&idlk);
	if (s) spit(IDS, s, strlen(s));   // the worker's only: card_now never saves
	free(s);
}

// A fresh page may bring a new photo: the ready-made face goes, and is made again from it
static void face_forget(const char *id) {
	char p[96];
	snprintf(p, sizeof p, FACEDIR "/%s.raw", id);
	remove(p);
}

// ---- the page --------------------------------------------------------------------------------

// The page's data for this artist, trimmed to what we show, as it's kept on the card:
// {"fetched": unix time, "artist": {...the page's artist...}, "ms": {track uri: length}}
static json_t *fetch_page(const char *id) {
	char url[128];
	snprintf(url, sizeof url, "https://open.spotify.com/artist/%s", id);
	Buf b = {0};
	long code = get(url, NULL, &b);
	json_t *st = code == 200 && b.d ? initial_state(b.d) : NULL;
	free(b.d);
	b = (Buf){0};
	char key[64];
	snprintf(key, sizeof key, "spotify:artist:%s", id);
	json_t *a = json_object_get(jp(st, "entities.items"), key);
	if (!a) { crumb("meta: no data in the page for %s (%ld)", id, code); json_decref(st); return NULL; }
	json_incref(a);
	json_decref(st);
	json_object_del(a, "relatedMusicVideos");
	json_object_del(a, "preReleaseV2");

	json_t *ms = json_object();   // the top songs' lengths, from the embed
	snprintf(url, sizeof url, "https://open.spotify.com/embed/artist/%s", id);
	if (get(url, NULL, &b) == 200 && b.d) {
		json_t *nd = next_data(b.d);
		size_t k;
		json_t *t;
		json_array_foreach(jp(nd, "props.pageProps.state.data.entity.trackList"), k, t)
			if (js(t, "uri")) json_object_set(ms, js(t, "uri"), json_object_get(t, "duration"));
		json_decref(nd);
	}
	free(b.d);
	return json_pack("{s:I, s:o, s:o}", "fetched", (json_int_t)time(NULL), "artist", a, "ms", ms);
}

// the smallest picture at least `want` wide
static const char *pick(json_t *sources, int want) {
	const char *best = NULL;
	int bw = 1 << 30, big = 0;
	const char *biggest = NULL;
	size_t k;
	json_t *s;
	json_array_foreach(sources, k, s) {
		int w = json_integer_value(json_object_get(s, "width"));
		if (w >= want && w < bw) { bw = w; best = js(s, "url"); }
		if (w > big) { big = w; biggest = js(s, "url"); }
	}
	return best ? best : biggest ? biggest : js(json_array_get(sources, 0), "url");
}

static int add_job(const char *url, int kind) {   // a slot for this picture (shared if it's the same one)
	if (!url) return -1;
	for (int k = 1; k < njobs; k++) if (jobs[k].kind == kind && !strcmp(jobs[k].url, url)) return k;
	if (njobs >= MI_SLOTS) return -1;
	jobs[njobs] = (ImgJob){strdup(url), kind};
	return njobs++;
}

static void releases(json_t *items, MRelease *out, int *n, int cap, bool wrapped) {
	size_t k;
	json_t *it;
	json_array_foreach(items, k, it) {
		if (*n >= cap) break;
		json_t *r = wrapped ? json_array_get(jp(it, "releases.items"), 0) : it;
		if (!r || !js(r, "name")) continue;
		MRelease *m = &out[(*n)++];
		m->name = strdup(js(r, "name"));
		m->uri = dup0(js(r, "uri"));
		m->type = dup0(js(r, "type"));
		m->year = json_integer_value(jp(r, "date.year"));
		m->img = add_job(pick(jp(r, "coverArt.sources"), MI_TILE), 1);
		m->local = -1;
		for (int a = 0; a < lib.nal; a++)
			if (same_name(lib.albums[a].name, m->name)) { m->local = a; break; }
	}
}

static u32 hexcol(const char *s) { return s && s[0] == '#' ? (u32)strtoul(s + 1, NULL, 16) : 0; }

static Artist *build(json_t *page, const char *id) {
	json_t *a = json_object_get(page, "artist"), *ms = json_object_get(page, "ms");
	Artist *r = calloc(1, sizeof *r);
	snprintf(r->id, sizeof r->id, "%s", id);
	r->name = strdup(js(json_object_get(a, "profile"), "name") ? js(json_object_get(a, "profile"), "name") : "?");
	r->bio = plain(js(jp(a, "profile.biography"), "text"));
	r->listeners = json_integer_value(jp(a, "stats.monthlyListeners"));
	r->followers = json_integer_value(jp(a, "stats.followers"));
	r->color = hexcol(js(jp(a, "visuals.avatarImage.extractedColors.colorDark"), "hex"));
	time_t when = json_integer_value(json_object_get(page, "fetched"));
	struct tm tmv;
	strftime(r->fetched, sizeof r->fetched, "%Y-%m-%d", localtime_r(&when, &tmv));

	for (int k = 0; k < njobs; k++) free(jobs[k].url);
	njobs = 1;
	jobs[0] = (ImgJob){NULL, 0};
	const char *face = pick(jp(a, "visuals.avatarImage.sources"), 640);
	if (face) { jobs[0].url = strdup(face); r->has_header = true; }
	r->avatar = add_job(pick(jp(a, "visuals.avatarImage.sources"), MI_TILE), 2);

	size_t k;
	json_t *it;
	json_array_foreach(jp(a, "discography.topTracks.items"), k, it) {
		if (r->ntop >= 10) break;
		json_t *t = json_object_get(it, "track");
		if (!js(t, "name")) continue;
		MTop *m = &r->top[r->ntop++];
		m->name = strdup(js(t, "name"));
		m->uri = dup0(js(t, "uri"));
		const char *pc = js(t, "playcount");
		m->plays = pc ? strtoull(pc, NULL, 10) : 0;
		m->explicit_ = js(json_object_get(t, "contentRating"), "label") && !strcmp(js(json_object_get(t, "contentRating"), "label"), "EXPLICIT");
		m->ms = m->uri ? json_integer_value(json_object_get(ms, m->uri)) : 0;
		char who[300] = "";
		SmTrack s = {.name = m->name, .ms = m->ms};
		size_t j;
		json_t *ar;
		json_array_foreach(jp(t, "artists.items"), j, ar) {
			const char *n = js(json_object_get(ar, "profile"), "name");
			if (!n) continue;
			if (s.na < 8) s.artists[s.na++] = n;
			size_t L = strlen(who);
			snprintf(who + L, sizeof who - L, "%s%s", L ? ", " : "", n);
		}
		m->artists = strdup(who);
		m->img = add_job(pick(jp(t, "albumOfTrack.coverArt.sources"), MI_TILE), 1);
		m->local = sm_match(&s);
	}
	releases(jp(a, "discography.popularReleasesAlbums.items"), r->pop, &r->npop, 10, false);
	releases(jp(a, "discography.albums.items"), r->albums, &r->nalb, 16, true);
	releases(jp(a, "discography.singles.items"), r->singles, &r->nsing, 16, true);
	json_array_foreach(jp(a, "relatedContent.relatedArtists.items"), k, it) {
		if (r->nrel >= 10) break;
		const char *n = js(json_object_get(it, "profile"), "name"), *u = js(it, "uri");
		if (!n || !u) continue;
		MArtist *m = &r->rel[r->nrel++];
		m->name = strdup(n);
		m->id = strdup(tail(u));
		m->img = add_job(pick(jp(it, "visuals.avatarImage.sources"), MI_TILE), 2);
		m->local = -1;
		for (int x = 0; x < lib.nar; x++) if (same_name(lib.artists[x].name, n)) { m->local = x; break; }
	}
	releases(jp(a, "relatedContent.appearsOn.items"), r->appears, &r->napp, 10, true);
	json_array_foreach(jp(a, "goods.concerts.items"), k, it) {
		if (r->ngig >= 6) break;
		json_t *c = json_object_get(it, "data");
		const char *d = js(c, "startDateIsoString");
		if (!d) continue;
		MGig *g = &r->gig[r->ngig++];
		sscanf(d, "%d-%d-%d", &g->y, &g->mo, &g->d);
		g->title = dup0(js(c, "title"));
		g->venue = dup0(js(json_object_get(c, "location"), "name"));
		g->city = dup0(js(json_object_get(c, "location"), "city"));
	}
	r->gigs_total = json_integer_value(jp(a, "goods.concerts.totalCount"));
	if (r->gigs_total < r->ngig) r->gigs_total = r->ngig;
	return r;
}

void meta_artist_free(Artist *a) {
	if (!a) return;
	free(a->name); free(a->bio);
	for (int k = 0; k < a->ntop; k++) { free(a->top[k].name); free(a->top[k].uri); free(a->top[k].artists); }
	MRelease *L[] = {a->pop, a->albums, a->singles, a->appears};
	int N[] = {a->npop, a->nalb, a->nsing, a->napp};
	for (int l = 0; l < 4; l++) for (int k = 0; k < N[l]; k++) { free(L[l][k].name); free(L[l][k].uri); free(L[l][k].type); }
	for (int k = 0; k < a->nrel; k++) { free(a->rel[k].name); free(a->rel[k].id); }
	for (int k = 0; k < a->ngig; k++) { free(a->gig[k].title); free(a->gig[k].venue); free(a->gig[k].city); }
	free(a);
}

// ---- kept in memory -----------------------------------------------------------------------------

// The last few pages, parsed, and the pictures already shrunk: an artist opened again (back out,
// tap again) is built from these at once, with no card and no JPEG decoding. Worker only.
#define PAGE_KEEP 4
static struct { char id[32]; json_t *page; u32 used; } pages_mem[PAGE_KEEP];
static u32 mem_clock;
static json_t *page_mem(const char *id) {
	for (int k = 0; k < PAGE_KEEP; k++)
		if (pages_mem[k].page && !strcmp(pages_mem[k].id, id)) { pages_mem[k].used = ++mem_clock; json_incref(pages_mem[k].page); return pages_mem[k].page; }
	return NULL;
}
static void page_keep(const char *id, json_t *page) {
	int at = 0;
	for (int k = 0; k < PAGE_KEEP; k++) {
		if (pages_mem[k].page && !strcmp(pages_mem[k].id, id)) { at = k; break; }
		if (!pages_mem[k].page || pages_mem[k].used < pages_mem[at].used) at = k;
	}
	json_decref(pages_mem[at].page);
	snprintf(pages_mem[at].id, sizeof pages_mem[at].id, "%s", id);
	pages_mem[at].page = page;
	json_incref(page);
	pages_mem[at].used = ++mem_clock;
}

#define PIC_BUDGET (6 << 20)   // bytes of shrunk pictures kept
typedef struct { char *url; int kind, w, h; u32 *px, used; } Pic;
static Pic *pics_mem;
static int npics;
static size_t pics_bytes;
static u32 *pic_mem(const char *url, int kind, int *w, int *h) {   // a copy, the caller's to free
	for (int k = 0; k < npics; k++)
		if (pics_mem[k].kind == kind && !strcmp(pics_mem[k].url, url)) {
			Pic *p = &pics_mem[k];
			p->used = ++mem_clock;
			u32 *c = malloc((size_t)p->w * p->h * 4);
			if (c) memcpy(c, p->px, (size_t)p->w * p->h * 4);
			*w = p->w;
			*h = p->h;
			return c;
		}
	return NULL;
}
static void pic_keep(const char *url, int kind, const u32 *px, int w, int h) {
	size_t n = (size_t)w * h * 4;
	while (npics && pics_bytes + n > PIC_BUDGET) {   // the least lately used go
		int o = 0;
		for (int k = 1; k < npics; k++) if (pics_mem[k].used < pics_mem[o].used) o = k;
		pics_bytes -= (size_t)pics_mem[o].w * pics_mem[o].h * 4;
		free(pics_mem[o].url);
		free(pics_mem[o].px);
		pics_mem[o] = pics_mem[--npics];
	}
	u32 *c = malloc(n);
	Pic *grown = realloc(pics_mem, (npics + 1) * sizeof *pics_mem);
	if (!c || !grown) { free(c); if (grown) pics_mem = grown; return; }
	pics_mem = grown;
	memcpy(c, px, n);
	pics_mem[npics++] = (Pic){strdup(url), kind, w, h, c, ++mem_clock};
	pics_bytes += n;
}

static void quick_pass(int upto);
static void quick_publish(int gen);
static bool publish(int gen, json_t *page, const char *id, bool offline);
static void fail_artist(int gen, const char *why);
static void card_now(int gen, const char *local, const char *given);
static void do_artist(int gen, const char *local, const char *given) {
	char id[32] = "", why[48] = "", path[96];
	if (given && given[0]) snprintf(id, sizeof id, "%s", given);
	if (!id[0]) id_cached(local, id, sizeof id);
	bool net = false;
	if (!id[0]) {
		net = online();
		if (!net) snprintf(why, sizeof why, "No Wi-Fi");
		else {
			prio(false);   // the network: below the main thread, the screen goes on
			find_artist(local, id, sizeof id, why, sizeof why);
			prio(true);
			if (id[0]) id_save(local, id);
		}
	}
	json_t *page = NULL;
	bool offline = false;
	LightLock_Lock(&blk);   // the pages in memory are card_now's too
	json_t *kept = id[0] ? page_mem(id) : NULL;
	LightLock_Unlock(&blk);
	if (kept && days_since(json_integer_value(json_object_get(kept, "fetched"))) < FRESH_DAYS) page = kept;   // seen lately: no card at all
	else json_decref(kept);
	if (id[0] && !page) {
		snprintf(path, sizeof path, ADIR "/%s.json", id);
		json_t *cached = json_card(path);
		bool fresh = cached && days_since(json_integer_value(json_object_get(cached, "fetched"))) < FRESH_DAYS;
		if (fresh) page = cached;
		else if (cached && (net || online())) {
			// a week old: the card's copy now (card_now may have shown it already), Spotify's when
			// it comes (if it says anything new)
			LightLock_Lock(&lk);
			bool on_screen = a_shown_gen == gen;
			LightLock_Unlock(&lk);
			bool shown = on_screen || publish(gen, cached, id, false);
			json_t *got = NULL;
			if (shown) {
				prio(false);   // shown already: the fresh copy comes below the main thread
				pf_cut = quitting || asked_now();   // another tap meanwhile: this can wait for next time
				pf_on = true;
				got = fetch_page(id);
				pf_on = false;
				pf_429 = false;
			}
			if (got && !pf_cut) {
				char *s = json_dumps(got, JSON_COMPACT);
				if (s) spit(path, s, strlen(s));
				free(s);
				LightLock_Lock(&blk);
				page_keep(id, got);
				LightLock_Unlock(&blk);
				if (!json_equal(json_object_get(got, "artist"), json_object_get(cached, "artist"))) {
					face_forget(id);   // their photo may have changed with the page
					publish(gen, got, id, false);
				}
			}
			crumb("meta: artist %s -> %s from the card, %s", local, id, got && !pf_cut ? "refreshed" : "not refreshed");
			json_decref(got);
			json_decref(cached);
			if (!shown) fail_artist(gen, "Couldn't get this artist");
			LightLock_Lock(&lk);
			if (gen == a_gen) a_done_gen = gen;   // answered, fresh copy or not (else this would run again)
			LightLock_Unlock(&lk);
			return;
		} else {
			if (net || online()) { prio(false); page = fetch_page(id); prio(true); }   // the network and the big page's parse: below the screen
			if (page) {
				char *s = json_dumps(page, JSON_COMPACT);
				if (s) spit(path, s, strlen(s));
				face_forget(id);   // their photo may have changed with the page
				free(s);
				json_decref(cached);
			} else if (cached) { page = cached; offline = true; }
		}
		if (!page) { if (!why[0]) snprintf(why, sizeof why, online() ? "Spotify didn't send the page" : "No Wi-Fi"); }
		else if (!offline) { LightLock_Lock(&blk); page_keep(id, page); LightLock_Unlock(&blk); }
	}
	bool shown = page && publish(gen, page, id, offline);
	json_decref(page);
	if (!shown) fail_artist(gen, why[0] ? why : "Couldn't get this artist");
	crumb("meta: artist %s -> %s %s", local, id[0] ? id : "-", why);
}

// The page from the card, built right here on the main thread when it's tapped, so the very next
// frame has all of it: the name, monthly listeners, the colour, Popular and the rest, and the photo
// and first pictures already shrunk (build 35 and before: the worker did this, and the page drew a
// guessed colour and no listeners until it got round to it). The worker then only sends the other
// pictures, and fetches a fresh copy if this one's a week old (do_artist, a_shown_gen).
static void card_now(int gen, const char *local, const char *given) {
	char id[32] = "", path[96];
	if (given && given[0]) snprintf(id, sizeof id, "%s", given);
	else id_cached(local, id, sizeof id);
	if (!id[0]) return;
	u64 t0 = osGetTime(), t1, t2;
	LightLock_Lock(&blk);
	json_t *page = page_mem(id);   // opened before in this run: no card, no parse
	bool mem = page != NULL;
	if (!page) {
		snprintf(path, sizeof path, ADIR "/%s.json", id);
		page = json_card(path);
		if (page) page_keep(id, page);
	}
	if (!page) { LightLock_Unlock(&blk); return; }
	bool fresh = days_since(json_integer_value(json_object_get(page, "fetched"))) < FRESH_DAYS;
	t1 = osGetTime();
	Artist *r = build(page, id);
	t2 = osGetTime();
	// the photo, their picture and Popular's first covers: what's on screen when it opens. The
	// rest come from the worker a moment later (asked_now holds the work ahead of time back).
	if (r) quick_pass(7);
	LightLock_Lock(&lk);
	if (r && gen == a_gen) {
		meta_artist_free(a_ready);
		a_ready = r;
		a_state = MS_READY;
		jobs_gen = gen;
		next_job = 0;
		outq_clear();
		quick_publish(gen);
		a_shown_gen = gen;
		if (fresh) a_done_gen = gen;   // nothing for the worker but the rest of the pictures
		r = NULL;
	}
	LightLock_Unlock(&lk);
	LightLock_Unlock(&blk);
	meta_artist_free(r);
	json_decref(page);
	crumb("meta: tap %s from the card in %llu ms (page %s %llu, build %llu, pictures %llu)", id, osGetTime() - t0,
	      mem ? "memory" : "card", t1 - t0, t2 - t1, osGetTime() - t2);
}

// The page out to the main thread, with every picture already shrunk; false if it's not wanted
// any more (another tap) or wouldn't build. A second one for the same tap replaces the first.
static bool publish(int gen, json_t *page, const char *id, bool offline) {
	LightLock_Lock(&lk);
	bool still = gen == a_gen;
	LightLock_Unlock(&lk);
	if (!still) return false;
	LightLock_Lock(&blk);
	Artist *r = build(page, id);
	if (!r) { LightLock_Unlock(&blk); return false; }
	r->offline = offline;
	quick_pass(QUICK_N);
	LightLock_Lock(&lk);
	bool ok = gen == a_gen;
	if (ok) {
		a_done_gen = gen;
		meta_artist_free(a_ready);
		a_ready = r;
		a_state = MS_READY;
		jobs_gen = gen;
		next_job = 0;
		outq_clear();          // anything left from the page before
		quick_publish(gen);    // the pictures already shrunk, with the page
		r = NULL;
	}
	LightLock_Unlock(&lk);
	LightLock_Unlock(&blk);
	meta_artist_free(r);
	return ok;
}
static void fail_artist(int gen, const char *why) {
	LightLock_Lock(&lk);
	if (gen == a_gen) {
		a_done_gen = gen;
		a_state = MS_FAILED;
		snprintf(a_why, sizeof a_why, "%s", why);
	}
	LightLock_Unlock(&lk);
}

// ---- pictures --------------------------------------------------------------------------------

static void img_path(const char *url, char *path, int cap) {
	const char *name = strrchr(url, '/');
	snprintf(path, cap, IMGDIR "/%.80s.jpg", name ? name + 1 : "x");
}

// The picture's file on the card, or fetched and kept there first
static u8 *img_bytes(const char *url, size_t *n) {
	char path[160];
	img_path(url, path, sizeof path);
	u8 *b = (u8 *)slurp(path, n);
	if (b) return b;
	if (!online()) return NULL;
	Buf bb = {0};
	if (get(url, NULL, &bb) != 200 || !bb.d) { free(bb.d); return NULL; }
	spit(path, bb.d, bb.n);
	*n = bb.n;
	return (u8 *)bb.d;
}

// box-averaged down from the (sx, sy, sw, sh) part of img to w×h; kind 1 rounds the corners a
// touch, 2 cuts a circle
static void shrink(const u8 *img, int iw, int sx, int sy, int sw, int sh, u32 *out, int w, int h, int kind) {
	float r = kind == 2 ? w / 2.0f : 3;
	for (int y = 0; y < h; y++) {
		int y0 = sy + y * sh / h, y1 = sy + (y + 1) * sh / h;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < w; x++) {
			int x0 = sx + x * sw / w, x1 = sx + (x + 1) * sw / w;
			if (x1 <= x0) x1 = x0 + 1;
			u32 cr = 0, cg = 0, cb = 0, n = 0;
			for (int yy = y0; yy < y1; yy++)
				for (const u8 *p = img + 4 * (yy * iw + x0), *e = p + 4 * (x1 - x0); p < e; p += 4) { cr += p[0]; cg += p[1]; cb += p[2]; n++; }
			u32 a = 255;
			if (kind == 2) {
				float dx = x + 0.5f - w / 2.0f, dy = y + 0.5f - h / 2.0f, d = r - __builtin_sqrtf(dx * dx + dy * dy);
				a = d >= 1 ? 255 : d <= 0 ? 0 : (u32)(d * 255);
			} else if (kind == 1) {
				int cx = x < r ? r - x : x >= w - r ? x - (w - r - 1) : 0, cy = y < r ? r - y : y >= h - r ? y - (h - r - 1) : 0;
				if (cx * cx + cy * cy > r * r) a = 0;
			}
			out[y * w + x] = (cr / n) << 24 | (cg / n) << 16 | (cb / n) << 8 | a;
		}
	}
}

// The shrunk picture kept on the card beside its JPEG (<name>.k<kind>.raw, w×h×4): a page opened
// after a restart reads these instead of decoding every JPEG again. A new photo has a new URL,
// so a new name; nothing here goes stale.
static void raw_path(const char *url, int kind, char *path, int cap) {
	const char *name = strrchr(url, '/');
	snprintf(path, cap, IMGDIR "/%.80s.k%d.raw", name ? name + 1 : "x", kind);
}
static u32 *pic_card(const char *url, int kind, int w, int h) {
	char p[160];
	raw_path(url, kind, p, sizeof p);
	size_t n = 0;
	char *b = slurp(p, &n);
	if (b && n == (size_t)w * h * 4) return (u32 *)b;
	free(b);
	return NULL;
}
// Already shrunk, in memory or on the card: no JPEG, no network. NULL if neither has it.
static u32 *pic_quick(const char *url, int kind, int w, int h) {
	int mw, mh;
	u32 *px = pic_mem(url, kind, &mw, &mh);
	if (px && (mw != w || mh != h)) { free(px); px = NULL; }
	if (!px && (px = pic_card(url, kind, w, h))) pic_keep(url, kind, px, w, h);
	return px;
}
static void hand_over(int gen, int k, int w, int h, u32 *px) {   // under lk; takes px
	if (gen == a_gen && nout < MI_SLOTS) { outq[nout++] = (OutImg){k, gen, w, h, px}; px = NULL; }
	free(px);
}

// A page about to go out: every picture it has ready-shrunk goes with it, so it opens whole.
// quick_pass gathers them (no lock), quick_publish hands them over with the page (under lk), so
// the main thread never sees the pictures before the page they belong to.
static u32 *quick_got[MI_SLOTS];
// Only the first few: the photo, their picture and Popular's covers, what the page shows first. The
// rest (further down) follow from do_image a moment later, so the tap's wait stays short.
static void quick_pass(int upto) {
	int n = njobs < upto ? njobs : upto;
	for (int k = 0; k < MI_SLOTS; k++) { free(quick_got[k]); quick_got[k] = NULL; jdone[k] = false; }   // a page that never went out
	for (int k = 0; k < n; k++) {
		quick_got[k] = jobs[k].url ? pic_quick(jobs[k].url, jobs[k].kind, jobs[k].kind ? MI_TILE : MI_HEAD_W, jobs[k].kind ? MI_TILE : MI_HEAD_H) : NULL;
	}
}
static void quick_publish(int gen) {   // under lk
	int n = njobs < MI_SLOTS ? njobs : MI_SLOTS;
	for (int k = 0; k < n; k++)
		if (quick_got[k]) {
			hand_over(gen, k, jobs[k].kind ? MI_TILE : MI_HEAD_W, jobs[k].kind ? MI_TILE : MI_HEAD_H, quick_got[k]);
			quick_got[k] = NULL;
			jdone[k] = true;
		}
}

static bool do_image(void) {
	LightLock_Lock(&blk);   // jobs and the shrunk pictures in memory are card_now's too
	LightLock_Lock(&lk);
	int gen = jobs_gen, k = next_job;
	while (k < njobs && jdone[k]) k++;
	bool go = nout < MI_SLOTS && gen == a_gen && k < njobs;
	char *url = go && jobs[k].url ? strdup(jobs[k].url) : NULL;
	int kind = go ? jobs[k].kind : 0;
	next_job = go ? k + 1 : k;
	LightLock_Unlock(&lk);
	int w = kind ? MI_TILE : MI_HEAD_W, h = kind ? MI_TILE : MI_HEAD_H, mw = 0, mh = 0;
	u32 *px = go && url ? pic_mem(url, kind, &mw, &mh) : NULL;   // memory under the lock; the card after it
	if (px && (mw != w || mh != h)) { free(px); px = NULL; }
	LightLock_Unlock(&blk);
	if (!go) return false;
	if (!url) return true;
	if (!px && (px = pic_card(url, kind, w, h))) { LightLock_Lock(&blk); pic_keep(url, kind, px, w, h); LightLock_Unlock(&blk); }
	if (px) { free(url); goto hand; }   // shrunk before: straight over
	size_t n = 0;
	u8 *b = img_bytes(url, &n);
	int iw, ih, ch;
	u8 *img = b && stbi_info_from_memory(b, n, &iw, &ih, &ch) && (s64)iw * ih <= 2000 * 2000 ? stbi_load_from_memory(b, n, &iw, &ih, &ch, 4) : NULL;
	free(b);
	if (!img) { free(url); return true; }
	px = malloc(w * h * 4);
	if (!px) { stbi_image_free(img); free(url); return true; }
	if (kind) {   // the middle square
		int m = iw < ih ? iw : ih;
		shrink(img, iw, (iw - m) / 2, (ih - m) / 2, m, m, px, w, h, kind);
	} else {      // a wide band, a little above the middle, where faces usually are
		int sw = iw, sh = iw * h / w;
		if (sh > ih) { sh = ih; sw = ih * w / h; }
		int sy = (ih - sh) * 3 / 10;
		shrink(img, iw, (iw - sw) / 2, sy, sw, sh, px, w, h, 0);
	}
	stbi_image_free(img);
	LightLock_Lock(&blk);
	pic_keep(url, kind, px, w, h);
	LightLock_Unlock(&blk);
	char rp[160];
	raw_path(url, kind, rp, sizeof rp);
	spit(rp, px, (size_t)w * h * 4);   // for the next time, restart or not
	free(url);
hand:
	LightLock_Lock(&lk);
	hand_over(gen, k, w, h, px);
	LightLock_Unlock(&lk);
	return true;
}

// ---- lyrics -----------------------------------------------------------------------------------

void meta_lyrics_free(Lyrics *l) {
	if (!l) return;
	for (int k = 0; k < l->n; k++) free(l->l[k].text);
	free(l->l);
	free(l);
}

static void lpush(Lyrics *L, u32 ms, const char *s, int n) {
	L->l = realloc(L->l, (L->n + 1) * sizeof(LLine));
	L->l[L->n].ms = ms;
	L->l[L->n].text = malloc(n + 1);
	memcpy(L->l[L->n].text, s, n);
	L->l[L->n].text[n] = 0;
	L->n++;
}

static int by_ms(const void *a, const void *b) { u32 x = ((const LLine *)a)->ms, y = ((const LLine *)b)->ms; return (x > y) - (x < y); }

// LRC: "[mm:ss.xx] words", a line may carry several times; plain: a line each
static void parse_lyrics(Lyrics *L, const char *s, bool synced) {
	while (*s) {
		const char *e = strchr(s, '\n');
		int n = e ? e - s : (int)strlen(s);
		const char *p = s, *end = s + n;
		u32 times[8];
		int nt = 0;
		while (synced && p < end && *p == '[') {
			int m = 0;
			float sec = 0;
			const char *close = memchr(p, ']', end - p);
			if (!close) break;
			if (sscanf(p, "[%d:%f]", &m, &sec) == 2 && nt < 8) times[nt++] = (u32)(m * 60000 + sec * 1000 + 0.5f);
			p = close + 1;
		}
		while (p < end && *p == ' ') p++;
		int len = end - p;
		while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) len--;
		if (synced) for (int k = 0; k < nt; k++) lpush(L, times[k], p, len);
		else if (len || (L->n && L->l[L->n - 1].text[0])) lpush(L, 0, p, len);   // one blank between verses
		s = e ? e + 1 : end;
	}
	if (synced) qsort(L->l, L->n, sizeof(LLine), by_ms);
	L->synced = synced && L->n;
}

static Lyrics *lyrics_from(json_t *j) {
	Lyrics *L = calloc(1, sizeof *L);
	L->instrumental = json_is_true(json_object_get(j, "instrumental"));
	const char *syn = js(j, "syncedLyrics"), *pl = js(j, "plainLyrics");
	if (syn && *syn) parse_lyrics(L, syn, true);
	if (!L->n && pl && *pl) parse_lyrics(L, pl, false);
	while (L->n && !L->l[0].text[0] && !L->synced) { free(L->l[0].text); memmove(L->l, L->l + 1, --L->n * sizeof(LLine)); }
	return L;
}

static void do_lyrics(int track) {
	Track *t = &lib.t[track];
	char key[160];
	snprintf(key, sizeof key, LDIR "/%08lx", (unsigned long)fnv(t->path));
	char pj[180], pn[180];
	snprintf(pj, sizeof pj, "%s.json", key);
	snprintf(pn, sizeof pn, "%s.none", key);
	json_t *j = json_card(pj);
	struct stat st;
	bool none_recent = !j && !stat(pn, &st) && days_since(st.st_mtime) < NOLYR_DAYS;
	bool offline = !j && !none_recent && !online();
	if (!j && !none_recent && !offline) {
		const char *lead = t->artist_id >= 0 && t->artist_id < lib.nar ? lib.artists[t->artist_id].name : t->artist;
		char *ea = curl_easy_escape(curl, lead, 0), *et = curl_easy_escape(curl, t->title, 0), *eb = curl_easy_escape(curl, t->album, 0);
		char url[1400];
		snprintf(url, sizeof url, "https://lrclib.net/api/get?artist_name=%s&track_name=%s&album_name=%s&duration=%lu", ea, et, eb, (unsigned long)t->secs);
		Buf b = {0};
		long code = get(url, NULL, &b);
		if (code == 200) j = json_loadb(b.d, b.n, 0, NULL);
		if (code < 0 || code >= 500) offline = true;
		if (code == 404) {   // not that exact song: the closest length among the search's
			snprintf(url, sizeof url, "https://lrclib.net/api/search?artist_name=%s&track_name=%s", ea, et);
			long sc = get(url, NULL, &b);
			if (sc == 200) {
				json_t *arr = json_loadb(b.d, b.n, 0, NULL), *it, *best = NULL;
				size_t k;
				double bd = 4;
				json_array_foreach(arr, k, it) {
					double d = json_number_value(json_object_get(it, "duration")) - t->secs;
					if (d < 0) d = -d;
					bool words = (js(it, "syncedLyrics") && *js(it, "syncedLyrics")) || (js(it, "plainLyrics") && *js(it, "plainLyrics")) || json_is_true(json_object_get(it, "instrumental"));
					if (words && d < bd) { bd = d; best = it; }
				}
				if (best) j = json_incref(best);
				json_decref(arr);
			}
			if (!j && sc == 200) { FILE *f = card_open(pn, "w"); if (f) fclose(f); }   // only a real "none"
		}
		free(b.d);
		curl_free(ea); curl_free(et); curl_free(eb);
		if (j) {
			json_t *keep = json_pack("{s:O?, s:O?, s:O?}", "syncedLyrics", json_object_get(j, "syncedLyrics"),
			                         "plainLyrics", json_object_get(j, "plainLyrics"), "instrumental", json_object_get(j, "instrumental"));
			char *s = keep ? json_dumps(keep, JSON_COMPACT) : NULL;
			if (s) spit(pj, s, strlen(s));
			free(s);
			json_decref(keep);
			remove(pn);
		}
	}
	Lyrics *L = j ? lyrics_from(j) : calloc(1, sizeof(Lyrics));
	json_decref(j);
	L->track = track;
	L->offline = offline;
	LightLock_Lock(&lk);
	if (l_want == track) {
		meta_lyrics_free(l_ready);
		l_ready = L;
		l_state = MS_READY;
		L = NULL;
	}
	l_done = track;
	LightLock_Unlock(&lk);
	meta_lyrics_free(L);
}

// ---- faces for the Artists tab ----------------------------------------------------------------

// Each artist's photo, round, from the card only: their page and its picture, as the work ahead of
// time (or an open page) left them. Made once into faces/<id>.raw, so a launch reads 4 KB per
// artist instead of their whole page. No network here at all.
#define FACE_Q 8
static struct { int x; u32 px[MI_FACE * MI_FACE]; } face_q[FACE_Q];   // under lk
static int face_n;
static int fc_next;                  // the pass at launch over every artist (worker only)
static bool fc_again[MI_FACES];      // fetched ahead of time since: look again (worker only)

static bool face_make(const char *id, u32 *px) {
	char path[160];
	size_t n = 0;
	snprintf(path, sizeof path, FACEDIR "/%s.raw", id);
	char *raw = slurp(path, &n);
	if (raw && n == sizeof(u32) * MI_FACE * MI_FACE) { memcpy(px, raw, n); free(raw); return true; }
	free(raw);
	snprintf(path, sizeof path, ADIR "/%s.json", id);
	json_t *page = json_card(path);
	const char *url = pick(jp(json_object_get(page, "artist"), "visuals.avatarImage.sources"), MI_TILE);
	char ip[160] = "";
	if (url) img_path(url, ip, sizeof ip);
	json_decref(page);
	if (!ip[0]) return false;
	u8 *b = (u8 *)slurp(ip, &n);
	if (!b) return false;
	int iw, ih, ch;
	u8 *img = stbi_info_from_memory(b, n, &iw, &ih, &ch) && (s64)iw * ih <= 2000 * 2000 ? stbi_load_from_memory(b, n, &iw, &ih, &ch, 4) : NULL;
	free(b);
	if (!img) return false;
	int m = iw < ih ? iw : ih;
	shrink(img, iw, (iw - m) / 2, (ih - m) / 2, m, m, px, MI_FACE, MI_FACE, 2);
	stbi_image_free(img);
	snprintf(path, sizeof path, FACEDIR "/%s.raw", id);
	spit(path, px, sizeof(u32) * MI_FACE * MI_FACE);
	return true;
}

// The photo face_make shrinks, for anyone who wants it bigger (the Home page, from art.c's thread):
// only what's on the card already, never fetched from here
bool meta_photo_file(const char *name, char *path, int cap) {
	char id[32], p[160];
	path[0] = 0;
	id_cached(name, id, sizeof id);
	if (!id[0]) return false;
	snprintf(p, sizeof p, ADIR "/%s.json", id);
	json_t *page = json_card(p);
	const char *url = pick(jp(json_object_get(page, "artist"), "visuals.avatarImage.sources"), MI_TILE);
	if (url) img_path(url, path, cap);
	json_decref(page);
	struct stat st;
	return path[0] && !stat(path, &st);
}

// Every face from last time in one file, by artist name: main reads it before the first frame, so
// the Artists tab never waits for the pass below (one file per artist, with naps while a song plays)
#define FACES_CACHE DIR3DS "/faces.cache"
#define FC_MAGIC 0x31434650u   // "PFC1"
#define FC_PX (MI_FACE * MI_FACE)
static u32 (*fc_px)[FC_PX];          // every face this run has, for the file (worker; main at load)
static u8 fc_have[MI_FACES], fc_cached[MI_FACES];
static bool fc_dirty;
static u32 name_hash(const char *s) { u32 h = 2166136261u; while (*s) h = (h ^ (u8)*s++) * 16777619u; return h; }

int meta_faces_cached(void (*put)(int x, const u32 *px)) {
	int lim = lib.nar < MI_FACES ? lib.nar : MI_FACES, got = 0;
	if (!fc_px) fc_px = calloc(MI_FACES, sizeof *fc_px);
	FILE *f = card_open_big(FACES_CACHE, "rb");
	if (!f || !fc_px) { if (f) fclose(f); return 0; }
	u32 hd[3];
	if (fread(hd, 4, 3, f) == 3 && hd[0] == FC_MAGIC && hd[1] == MI_FACE && hd[2] <= 4096) {
		u32 *hs = malloc(lim * 4 + 4);
		for (int x = 0; hs && x < lim; x++) hs[x] = name_hash(lib.artists[x].name);
		static u32 px[FC_PX];
		for (u32 k = 0; hs && k < hd[2]; k++) {
			u32 h;
			if (fread(&h, 4, 1, f) != 1 || fread(px, 4, FC_PX, f) != FC_PX) break;
			for (int x = 0; x < lim; x++)
				if (hs[x] == h && !fc_have[x]) {
					memcpy(fc_px[x], px, sizeof px);
					fc_have[x] = fc_cached[x] = 1;
					put(x, px);
					got++;
					break;
				}
		}
		free(hs);
	}
	fclose(f);
	return got;
}
static void faces_save(void) {   // worker, once the pass is through and something's new
	int lim = lib.nar < MI_FACES ? lib.nar : MI_FACES;
	u32 n = 0;
	for (int x = 0; x < lim; x++) n += fc_have[x];
	FILE *f = card_open_big(FACES_CACHE ".tmp", "wb");
	if (!f) return;
	u32 hd[3] = {FC_MAGIC, MI_FACE, n};
	bool ok = fwrite(hd, 4, 3, f) == 3;
	for (int x = 0; x < lim && ok; x++) {
		if (!fc_have[x]) continue;
		u32 h = name_hash(lib.artists[x].name);
		ok = fwrite(&h, 4, 1, f) == 1 && fwrite(fc_px[x], 4, FC_PX, f) == FC_PX;
	}
	ok = !fclose(f) && ok;
	if (ok) { remove(FACES_CACHE); ok = !rename(FACES_CACHE ".tmp", FACES_CACHE); }
	if (!ok) remove(FACES_CACHE ".tmp");
	crumb("meta: %lu faces saved%s", (unsigned long)n, ok ? "" : " (failed)");
}

static bool face_step(void) {
	LightLock_Lock(&lk);
	bool full = face_n >= FACE_Q;
	LightLock_Unlock(&lk);
	if (full || quitting) return false;   // main takes them every frame, and wakes us
	int lim = lib.nar < MI_FACES ? lib.nar : MI_FACES, x = -1;
	for (int k = 0; k < lim && x < 0; k++) if (fc_again[k]) { fc_again[k] = false; x = k; }
	while (x < 0 && fc_next < lim && fc_cached[fc_next]) fc_next++;   // shown already, from the file
	if (x < 0 && fc_next < lim) x = fc_next++;
	if (x < 0) {
		// the file, at most every two minutes while the work ahead of time keeps making faces (each
		// one is ~250 KB on the card, and build 30 wrote it after every single face); the rest at exit
		static u64 fc_saved_at;
		if (fc_dirty && fc_px && (!fc_saved_at || osGetTime() - fc_saved_at > 120000)) { fc_dirty = false; fc_saved_at = osGetTime(); faces_save(); }
		return false;
	}
	char id[32];
	id_cached(lib.artists[x].name, id, sizeof id);
	static u32 px[MI_FACE * MI_FACE];
	if (!id[0] || !face_make(id, px)) return true;
	if (fc_px && (!fc_have[x] || memcmp(fc_px[x], px, sizeof px))) { memcpy(fc_px[x], px, sizeof px); fc_have[x] = 1; fc_dirty = true; }
	LightLock_Lock(&lk);
	face_q[face_n].x = x;
	memcpy(face_q[face_n].px, px, sizeof px);
	face_n++;
	LightLock_Unlock(&lk);
	return true;
}

int meta_face(u32 *px) {
	LightLock_Lock(&lk);
	int x = -1;
	if (face_n) {
		face_n--;
		x = face_q[face_n].x;
		memcpy(px, face_q[face_n].px, sizeof face_q[0].px);
	}
	LightLock_Unlock(&lk);
	if (x >= 0) LightEvent_Signal(&wake);   // room for the next
	return x;
}

// ---- ahead of time ---------------------------------------------------------------------------

// Pages for the artists on the card, fetched while nothing else is wanted, so one opens straight
// from the card instead of after a search, two page loads and its pictures. The song playing's
// artist goes first (its name on the player is the likeliest tap), then every artist, most songs
// first. One small step per turn of the thread (a search gives way between its requests), so
// anything asked for meanwhile waits for one page (two requests) at most, and the open page's own pictures
// always come first. Pauses between artists so Spotify isn't hammered; a 429 or no sign-in backs
// off for minutes; nothing without Wi-Fi; "not on Spotify" is remembered for two weeks.
#define PF_IMGS  7      // the first screen: the header, the face, the top five songs' covers
#define PF_GAP   1500   // ms between artists that took the network
#define PF_SEARCH_GAP 3000   // ...and after a search, which shares a limit with the Liked Songs sync
static int *pf_order, pf_n, pf_next;    // the pass over the library (worker only, after init)
static int pf_hint = -1;                // the song playing's artist, under lk
static bool *pf_done;                   // artists already seen to this run (worker only)
static int pf_cur = -1, pf_step, pf_nurl;
static char pf_id[32], *pf_urls[PF_IMGS];
static bool pf_net, pf_searched;        // this artist cost a request; a search
static int pf_fails;                    // pages that wouldn't come for this artist
static u64 pf_after;                    // not before this
static u64 pf_nosign;                   // no Spotify sign-in until then: artists that need a search wait

static int by_songs(const void *a, const void *b) {
	return lib.artists[*(const int *)b].n - lib.artists[*(const int *)a].n;
}

static void pf_finish(void) {
	if (pf_cur >= 0 && pf_done) pf_done[pf_cur] = true;
	for (int k = 0; k < pf_nurl; k++) free(pf_urls[k]);
	pf_nurl = 0;
	pf_cur = -1;
	if (pf_net) pf_after = osGetTime() + (pf_searched ? PF_SEARCH_GAP : PF_GAP);
	pf_net = pf_searched = false;
	pf_fails = 0;
}
static bool pf_later(int ms) { pf_after = osGetTime() + ms; return false; }   // the same artist, then

static void pf_url(const char *u) { if (u && pf_nurl < PF_IMGS) pf_urls[pf_nurl++] = strdup(u); }

// One step; false when there's nothing to do now (pf_after says when to look again)
static bool prefetch_step(void) {
	if (!pf_done || quitting) return false;
	if (pf_cur < 0) {
		if (osGetTime() < pf_after) return false;
		LightLock_Lock(&lk);
		int h = pf_hint;
		pf_hint = -1;
		LightLock_Unlock(&lk);
		if (h >= 0 && h < lib.nar && !pf_done[h]) pf_cur = h;
		while (pf_cur < 0 && pf_next < pf_n) {
			int x = pf_order[pf_next++];
			if (!pf_done[x]) pf_cur = x;
		}
		if (pf_cur < 0) return false;
		pf_step = 0;
		pf_id[0] = 0;
		return true;
	}
	const char *name = lib.artists[pf_cur].name;
	if (!name || !name[0] || !strcasecmp(name, "Unknown artist") || !strcasecmp(name, "Various artists")) { pf_finish(); return true; }
	if (pf_step == 0) {   // who they are on Spotify
		id_cached(name, pf_id, sizeof pf_id);
		if (!pf_id[0]) {
			if (id_none_lately(name)) { pf_finish(); return true; }
			if (osGetTime() < pf_nosign) { pf_cur = -1; return true; }   // not done: next launch, or after the wait
			if (!online()) return pf_later(60000);
			char why[48] = "";
			pf_net = pf_searched = true;
			pf_cut = quitting || asked_now();
			pf_on = true;
			find_artist(name, pf_id, sizeof pf_id, why, sizeof why);
			pf_on = false;
			if (pf_cut) { pf_id[0] = 0; return true; }   // cut short for a tap: this step again later
			if (pf_429) { pf_429 = false; crumb("meta: ahead: Spotify says slow down"); return pf_later(5 * 60000); }
			if (!pf_id[0]) {
				if (!strcmp(why, "later")) return true;   // something real first (the loop sees it now), then this one again
				if (!strcmp(why, "Not on Spotify")) {
					char v[32];
					snprintf(v, sizeof v, "none:%lld", (long long)time(NULL));
					id_save(name, v);
					pf_finish();
					return true;
				}
				crumb("meta: ahead: %s: %s", name, why[0] ? why : "?");
				if (!strncmp(why, "Spotify", 7) && strcmp(why, "Spotify said no")) {
					// no sign-in: ten minutes without searches, but the artists already known go on
					pf_nosign = osGetTime() + 10 * 60000;
					pf_cur = -1;
					pf_fails = 0;
					return true;
				}
				return pf_later(60000);   // can't reach it, or Spotify said no: the same artist in a minute
			}
			id_save(name, pf_id);
		}
		pf_step = 1;
		return true;
	}
	if (pf_step == 1) {   // their page, unless the card's copy is fresh; then which pictures it wants
		char path[96];
		snprintf(path, sizeof path, ADIR "/%s.json", pf_id);
		json_t *page = json_card(path);
		bool fresh = page && days_since(json_integer_value(json_object_get(page, "fetched"))) < FRESH_DAYS - 1;
		if (!fresh) {
			if (!online()) { json_decref(page); return pf_later(60000); }
			pf_net = true;
			pf_cut = quitting || asked_now();
			pf_on = true;
			json_t *got = fetch_page(pf_id);
			pf_on = false;
			if (pf_cut) { json_decref(got); json_decref(page); return true; }   // cut short for a tap: again later
			if (pf_429) { pf_429 = false; json_decref(got); json_decref(page); return pf_later(5 * 60000); }
			if (got) {
				char *s = json_dumps(got, JSON_COMPACT);
				if (s) spit(path, s, strlen(s));
				face_forget(pf_id);
				free(s);
				json_decref(page);
				page = got;
			} else if (!page) {   // nothing on the card either: a minute, three times, then let it be
				if (++pf_fails < 3) return pf_later(60000);
				crumb("meta: ahead: no page for %s", name);
				pf_finish();
				return true;
			}
		}
		json_t *a = json_object_get(page, "artist");
		pf_url(pick(jp(a, "visuals.avatarImage.sources"), 640));
		pf_url(pick(jp(a, "visuals.avatarImage.sources"), MI_TILE));
		size_t k;
		json_t *it;
		json_array_foreach(jp(a, "discography.topTracks.items"), k, it) {
			if (k >= 5) break;
			pf_url(pick(jp(json_object_get(it, "track"), "albumOfTrack.coverArt.sources"), MI_TILE));
		}
		json_decref(page);
		pf_step = 2;
		return true;
	}
	int k = pf_step - 2;   // the pictures, one a turn
	if (k >= pf_nurl) {
		crumb("meta: ahead: %s ready", name);
		if (pf_cur < MI_FACES) fc_again[pf_cur] = true;   // their face can be made now
		pf_finish();
		return true;
	}
	char path[160];
	img_path(pf_urls[k], path, sizeof path);
	struct stat st;
	if (stat(path, &st)) {
		if (!online()) return pf_later(60000);
		pf_net = true;
		size_t n = 0;
		pf_cut = quitting || asked_now();
		pf_on = true;
		free(img_bytes(pf_urls[k], &n));
		pf_on = false;
		if (pf_cut) return true;   // cut short for a tap: this picture again later
		if (pf_429) { pf_429 = false; return pf_later(5 * 60000); }
	}
	pf_step++;
	return true;
}

static bool pf_pending(void) {
	LightLock_Lock(&lk);
	bool h = pf_hint >= 0;
	LightLock_Unlock(&lk);
	return pf_done && (pf_cur >= 0 || pf_next < pf_n || h);
}

// The open page still has pictures to come (one may be waiting for the main thread to take it)
static bool pics_pending(void) {
	LightLock_Lock(&lk);
	int k = next_job;
	while (k < njobs && jdone[k]) k++;
	bool r = jobs_gen == a_gen && k < njobs;
	LightLock_Unlock(&lk);
	return r;
}

static volatile bool m_playing;
void meta_playing(bool on) { m_playing = on; }

void meta_artist_soon(int track) {
	if (track < 0 || track >= lib.nt) return;
	int x = lib.t[track].artist_id;
	if (x < 0 || x >= lib.nar) return;
	LightLock_Lock(&lk);
	pf_hint = x;
	LightLock_Unlock(&lk);
	LightEvent_Signal(&wake);
}

// ---- the thread ------------------------------------------------------------------------------

// This thread shares the main thread's core, below it. A tap's page doesn't wait for it: card_now
// builds that on the main thread. (prio() is a leftover of build 35's boost, which starved the
// main thread; both priorities are the low one now.)

static void worker(void *arg) {
	(void)arg;
	dirs();
	while (!quitting) {
		crumb_flush_late();   // the main thread's slow-frame lines: written here, never on its own thread, where a write waits behind the card's other users
		prio(true);
		LightLock_Lock(&lk);
		int lw = l_want, ld = l_done, ag = a_gen, ad = a_done_gen;
		char local[256], id[32];
		snprintf(local, sizeof local, "%s", a_local);
		snprintf(id, sizeof id, "%s", a_id);
		LightLock_Unlock(&lk);
		if (ag != ad) { do_artist(ag, local, id); continue; }   // a tap first: the lyrics can wait a moment
		prio(false);
		if (lw >= 0 && lw != ld && lw < lib.nt) { do_lyrics(lw); continue; }
		// Everything below reads the card back to back, and the song's reads waited in line behind
		// it: six dropouts in the 14 s after a tap (build 37's log). The song first.
		int held = audio_make_room(3000);
		if (held >= 500) crumb("meta: held back %d ms for the song", held);
		if (do_image()) continue;
		if (pics_pending()) { prio(true); LightEvent_Wait(&wake); continue; }   // the page in hand's pictures before anything ahead
		if (face_step()) { if (m_playing) nap_w(60); continue; }   // from the card only: quick
		if (prefetch_step()) { if (m_playing) nap_w(400); continue; }   // the network and the card: gently while a song plays (a tap ends it)
		prio(true);   // waiting: a tap gets the core the moment it comes
		if (pf_pending()) {   // more to fetch ahead: wake for it, or for anything asked first
			u64 now = osGetTime();
			s64 ms = pf_after > now ? (s64)(pf_after - now) : 100;
			LightEvent_WaitTimeout(&wake, (ms < 100 ? 100 : ms > 60000 ? 60000 : ms) * 1000000LL);
		} else LightEvent_Wait(&wake);
	}
}

void meta_init(void) {
	LightLock_Init(&lk);
	LightLock_Init(&blk);
	if (!idlk_ok) { LightLock_Init(&idlk); idlk_ok = true; }
	ids_lock();   // read now, before any tap wants it
	LightLock_Unlock(&idlk);
	LightEvent_Init(&wake, RESET_ONESHOT);
	if (lib.nar) {   // the pass ahead of time: most songs first, from a little after launch
		pf_order = malloc(lib.nar * sizeof(int));
		pf_done = calloc(lib.nar, 1);
		for (int x = 0; x < lib.nar; x++) pf_order[x] = x;
		qsort(pf_order, lib.nar, sizeof(int), by_songs);
		pf_n = lib.nar;
		pf_after = osGetTime() + 10000;   // the launch's Spotify sync first
	}
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	// Build 35 put this thread above the main thread while it answered a tap or waited, and the
	// main thread starved: HOME took 34 s to get past the audio lock (build 36's log). The tap is
	// card_now's job on the main thread now, so the worker stays below it, always.
	prio_lo = prio + 3 < 0x3F ? prio + 3 : 0x3F;
	prio_hi = prio_lo;
	th = threadCreate(worker, NULL, 128 * 1024, prio_lo, 0, false);
	if (!th) crumb("meta: no thread");
}

void meta_quit(void) { quitting = true; pf_cut = true; if (th) LightEvent_Signal(&wake); }

static void pf_free(void) {
	for (int k = 0; k < pf_nurl; k++) free(pf_urls[k]);
	pf_nurl = 0;
	free(pf_order);
	free(pf_done);
	pf_order = NULL;
	pf_done = NULL;
	pf_n = pf_next = 0;
}

void meta_exit(void) {
	if (!th) { pf_free(); return; }
	quitting = true;
	LightEvent_Signal(&wake);
	threadJoin(th, U64_MAX);
	threadFree(th);
	th = NULL;
	if (fc_dirty && fc_px) { fc_dirty = false; faces_save(); }   // faces made since the last save
	if (curl) curl_easy_cleanup(curl);
	curl = NULL;
	meta_artist_free(a_ready);
	meta_lyrics_free(l_ready);
	outq_clear();
	for (int k = 0; k < MI_SLOTS; k++) free(quick_got[k]);
	for (int k = 0; k < njobs; k++) free(jobs[k].url);
	pf_free();
}

void meta_artist_want(const char *local, const char *id) {
	LightLock_Lock(&lk);
	a_gen++;
	pf_cut = true;   // whatever the work ahead of time is fetching stops for this
	snprintf(a_local, sizeof a_local, "%s", local ? local : "");
	snprintf(a_id, sizeof a_id, "%s", id ? id : "");
	a_state = MS_LOADING;
	a_why[0] = 0;
	meta_artist_free(a_ready);
	a_ready = NULL;
	outq_clear();
	int gen = a_gen;
	LightLock_Unlock(&lk);
	card_now(gen, local, id);   // before the worker hears of it: it has nothing to do if this did it all
	LightEvent_Signal(&wake);
}

Artist *meta_artist_take(int *gen) {
	LightLock_Lock(&lk);
	Artist *a = a_ready;
	a_ready = NULL;
	if (gen) *gen = a_gen;
	LightLock_Unlock(&lk);
	return a;
}

MState meta_artist_state(char *why, int cap) {
	LightLock_Lock(&lk);
	MState s = a_state;
	if (why) snprintf(why, cap, "%s", a_why);
	LightLock_Unlock(&lk);
	return s;
}

int meta_img(int *gen, const u32 **px, int *w, int *h) {
	LightLock_Lock(&lk);
	int s = -1;
	if (nout) { s = outq[0].slot; *gen = outq[0].gen; *px = outq[0].px; *w = outq[0].w; *h = outq[0].h; }
	LightLock_Unlock(&lk);
	return s;
}

void meta_img_done(void) {   // the oldest one, taken
	LightLock_Lock(&lk);
	if (nout) {
		free(outq[0].px);
		memmove(outq, outq + 1, (nout - 1) * sizeof *outq);
		nout--;
	}
	LightLock_Unlock(&lk);
	LightEvent_Signal(&wake);
}

void meta_lyrics_want(int track) {
	LightLock_Lock(&lk);
	bool changed = l_want != track;
	l_want = track;
	if (changed) {
		pf_cut = true;
		meta_lyrics_free(l_ready);
		l_ready = NULL;
		l_state = track >= 0 ? MS_LOADING : MS_IDLE;
		if (track >= 0 && l_done == track) l_done = -2;   // asked again: look again
	}
	LightLock_Unlock(&lk);
	if (changed) LightEvent_Signal(&wake);
}

void meta_lyrics_again(void) {   // look again (it came back offline)
	LightLock_Lock(&lk);
	l_done = -2;
	LightLock_Unlock(&lk);
	LightEvent_Signal(&wake);
}

Lyrics *meta_lyrics_take(void) {
	LightLock_Lock(&lk);
	Lyrics *l = l_ready;
	l_ready = NULL;
	LightLock_Unlock(&lk);
	return l;
}

MState meta_lyrics_state(void) {
	LightLock_Lock(&lk);
	MState s = l_state;
	LightLock_Unlock(&lk);
	return s;
}
