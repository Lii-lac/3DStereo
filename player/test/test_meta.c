// meta.c's parsing on a PC, against real pages saved in player/test/data (no network). From the
// project root:
//   docker run --rm -v "$(pwd -W):/src" -w /src ubuntu:24.04 sh -c "apt-get -qq update && apt-get -qq install -y
//     gcc libjansson-dev && gcc -O1 -Wall -fsanitize=address,undefined -Iplayer/test/fake_meta -Iplayer/source
//     player/test/test_meta.c player/source/spotmatch.c -ljansson -lm -o /tmp/t && /tmp/t player/test/data"
#include "../source/meta.c"
#include <stdarg.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"

// the network and the log, stubbed: nothing here goes online
bool net_up(char *e, int c) { (void)e; (void)c; return false; }
bool net_wifi(void) { return false; }
CURL *net_handle(void) { return NULL; }
long net_req(CURL *h, const char *m, const char *u, const char *f, const char *a, Buf *o, long *r, volatile bool *q, CURLcode *c) {
	(void)h; (void)m; (void)u; (void)f; (void)a; (void)o; (void)r; (void)q; (void)c;
	return -1;
}
bool spot_token(CURL *h, const char *s, char *o, int c, char *w, int wc, volatile bool *q) {
	(void)h; (void)s; (void)o; (void)c; (void)w; (void)wc; (void)q;
	return false;
}
bool spot_paired(void) { return false; }
int audio_make_room(int max_ms) { (void)max_ms; return 0; }
void crumb(const char *fmt, ...) {
	va_list a;
	va_start(a, fmt);
	fputs("  crumb: ", stdout);
	vprintf(fmt, a);
	va_end(a);
	putchar('\n');
}

Library lib;
static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)

static Track T[6];
static Group AL[3], AR[2];
static void fake_lib(void) {   // a tiny card: two glaive songs, features, and two names that mustn't split
	T[0] = (Track){.path = "sdmc:/music/a.flac", .title = "the prom", .artist = "glaive", .album = "the prom", .secs = 132, .album_id = 0, .artist_id = 0};
	T[1] = (Track){.path = "sdmc:/music/b.flac", .title = "astrid", .artist = "glaive", .album = "astrid", .secs = 101, .album_id = 1, .artist_id = 0};
	T[2] = (Track){.path = "sdmc:/music/c.flac", .title = "MEANT TO BE", .artist = "Tiffany Day, glaive", .album = "MEANT TO BE", .secs = 178, .album_id = 2, .artist_id = 1};
	T[3] = (Track){.path = "sdmc:/music/d.flac", .title = "Highway to Hell", .artist = "AC/DC", .album = "x", .secs = 208, .album_id = 2, .artist_id = 1};
	T[4] = (Track){.path = "sdmc:/music/e.flac", .title = "x", .artist = "kurtains feat. glaive", .album = "x", .secs = 100, .album_id = 2, .artist_id = 1};
	T[5] = (Track){.path = "sdmc:/music/f.flac", .title = "y", .artist = "Featherweight", .album = "x", .secs = 100, .album_id = 2, .artist_id = 1};
	lib.t = T;
	lib.nt = 6;
	static int s0[] = {0}, s1[] = {1}, s2[] = {2, 3, 4, 5};
	AL[0] = (Group){.name = "the prom", .artist = "glaive", .songs = s0, .n = 1};
	AL[1] = (Group){.name = "astrid", .artist = "glaive", .songs = s1, .n = 1};
	AL[2] = (Group){.name = "MEANT TO BE", .artist = "Tiffany Day", .songs = s2, .n = 4};
	lib.albums = AL;
	lib.nal = 3;
	AR[0] = (Group){.name = "glaive"};
	AR[1] = (Group){.name = "kurtains"};
	lib.artists = AR;
	lib.nar = 2;
}

static char *file(const char *dir, const char *name) {
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	char *s = slurp(p, NULL);
	if (!s) { perror(p); exit(2); }
	return s;
}

static Artist *page_of(const char *dir, const char *id) {   // fetch_page without the network
	char n[128];
	snprintf(n, sizeof n, "artist_%s.html", id);
	char *html = file(dir, n);
	json_t *st = initial_state(html);
	free(html);
	CHECK(st, "no initialState in %s", n);
	char key[64];
	snprintf(key, sizeof key, "spotify:artist:%s", id);
	json_t *a = json_incref(json_object_get(jp(st, "entities.items"), key));
	json_decref(st);
	CHECK(a, "no artist %s in the state", id);
	json_object_del(a, "relatedMusicVideos");
	json_t *ms = json_object();
	snprintf(n, sizeof n, "embed_%s.html", id);
	html = file(dir, n);
	json_t *nd = next_data(html);
	free(html);
	CHECK(nd, "no __NEXT_DATA__ in %s", n);
	size_t k;
	json_t *t;
	json_array_foreach(jp(nd, "props.pageProps.state.data.entity.trackList"), k, t)
		if (js(t, "uri")) json_object_set(ms, js(t, "uri"), json_object_get(t, "duration"));
	json_decref(nd);
	json_t *page = json_pack("{s:I, s:o, s:o}", "fetched", (json_int_t)1793000000, "artist", a, "ms", ms);
	char *dump = json_dumps(page, JSON_COMPACT);   // round trip through the card's cache, as do_artist does
	json_decref(page);
	printf("cached page: %zu bytes\n", strlen(dump));
	page = json_loads(dump, 0, NULL);
	free(dump);
	Artist *r = build(page, id);
	json_decref(page);
	return r;
}

static void show(const Artist *a) {
	printf("%s: %u listeners, %u followers, colour %06x, fetched %s, header %d, avatar %d, %d images\n", a->name,
	       (unsigned)a->listeners, (unsigned)a->followers, (unsigned)a->color, a->fetched, a->has_header, a->avatar, njobs);
	for (int k = 0; k < a->ntop; k++)
		printf("  top %d: %-28s %12llu plays %6u ms %s img %d local %d  [%s]\n", k + 1, a->top[k].name, (unsigned long long)a->top[k].plays,
		       (unsigned)a->top[k].ms, a->top[k].explicit_ ? "E" : " ", a->top[k].img, a->top[k].local, a->top[k].artists);
	printf("  popular %d, albums %d, singles %d, appears %d, related %d, gigs %d of %d\n", a->npop, a->nalb, a->nsing, a->napp, a->nrel, a->ngig, a->gigs_total);
	for (int k = 0; k < a->npop; k++) printf("    pop: %s (%d %s) local %d\n", a->pop[k].name, a->pop[k].year, a->pop[k].type, a->pop[k].local);
	for (int k = 0; k < a->nrel && k < 3; k++) printf("    rel: %s %s local %d\n", a->rel[k].name, a->rel[k].id, a->rel[k].local);
	for (int k = 0; k < a->ngig; k++) printf("    gig: %04d-%02d-%02d %s, %s\n", a->gig[k].y, a->gig[k].mo, a->gig[k].d, a->gig[k].city, a->gig[k].venue);
	printf("  bio: %.120s...\n", a->bio ? a->bio : "(none)");
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "player/test/data";
	fake_lib();
	sm_build();

	// who's on which song
	CHECK(meta_on_song(0, "glaive"), "glaive on the prom");
	CHECK(meta_on_song(2, "glaive") && meta_on_song(2, "Tiffany Day"), "both on MEANT TO BE");
	CHECK(meta_on_song(3, "AC/DC") && !meta_on_song(3, "AC"), "AC/DC stays whole");
	CHECK(meta_on_song(4, "glaive") && meta_on_song(4, "kurtains"), "feat. splits");
	CHECK(meta_on_song(5, "Featherweight") && !meta_on_song(5, "Fea"), "Featherweight isn't a feat");
	CHECK(!meta_on_song(1, "Glaive2"), "no near misses");

	// base64, without padding
	size_t n;
	char *d = unb64("aGVsbG8gd29ybGQ", 15, &n);
	CHECK(n == 11 && !strcmp(d, "hello world"), "unb64 got %zu '%s'", n, d);
	free(d);

	// a biography's HTML to text
	char *p = plain("<p>First &amp; best.</p><p>Second <a href=\"x\">link</a>&#39;s   end&nbsp;here</p>\n\n");
	CHECK(p && !strcmp(p, "First & best.\nSecond link's end here"), "plain got '%s'", p ? p : "(null)");
	free(p);

	// glaive's real page
	Artist *g = page_of(dir, "4cJKDGSv4Dz9QycXYmo565");
	show(g);
	CHECK(!strcmp(g->name, "glaive"), "name");
	CHECK(g->listeners > 100000 && g->followers > 10000, "stats");
	CHECK(g->ntop == 5, "5 top songs, got %d", g->ntop);
	int with_ms = 0, prom = -1;
	for (int k = 0; k < g->ntop; k++) {
		with_ms += g->top[k].ms > 0;
		if (!strcmp(g->top[k].name, "the prom")) prom = k;
	}
	CHECK(with_ms == g->ntop, "every top song has a length (%d of %d)", with_ms, g->ntop);
	CHECK(prom >= 0 && g->top[prom].local == 0, "the prom matched to the card");
	CHECK(g->has_header && g->avatar > 0 && jobs[0].url && jobs[0].kind == 0, "pictures queued");
	CHECK(g->nrel > 0 && g->rel[0].id[0], "fans also like");
	for (int k = 0; k < g->nrel; k++)
		if (!strcmp(g->rel[k].name, "kurtains")) CHECK(g->rel[k].local == 1, "kurtains is on the card");
	for (int k = 1; k < njobs; k++) CHECK(jobs[k].url && strstr(jobs[k].url, "i.scdn.co"), "job %d url", k);
	for (int k = 1; k < njobs; k++)
		for (int j = 1; j < k; j++) CHECK(strcmp(jobs[k].url, jobs[j].url) || jobs[k].kind != jobs[j].kind, "dup job %d %d", j, k);
	meta_artist_free(g);

	Artist *ku = page_of(dir, "7b2FbxYmnpGMPqDe04QmyP");
	show(ku);
	CHECK(!strcmp(ku->name, "kurtains") && ku->ntop > 0, "kurtains");
	meta_artist_free(ku);

	// lyrics: LRC with several stamps on a line, out of order; then LRCLIB's real answer
	Lyrics L = {0};
	parse_lyrics(&L, "[00:10.50]b\n[00:01.00][00:20.00]a\n[00:15.00]\n[01:02.25] c \r\n", true);
	CHECK(L.synced && L.n == 5, "lrc lines %d", L.n);
	CHECK(L.n == 5 && L.l[0].ms == 1000 && !strcmp(L.l[0].text, "a") && L.l[1].ms == 10500 && L.l[3].ms == 20000 &&
	      L.l[4].ms == 62250 && !strcmp(L.l[4].text, "c"), "lrc order/times");
	for (int k = 0; k < L.n; k++) free(L.l[k].text);
	free(L.l);
	char *lj = file(dir, "lrc_prom.json");
	json_t *j = json_loads(lj, 0, NULL);
	free(lj);
	Lyrics *pr = lyrics_from(j);
	json_decref(j);
	printf("the prom: %d lines, synced %d; %u ms \"%s\" ... %u ms \"%s\"\n", pr->n, pr->synced, (unsigned)pr->l[0].ms, pr->l[0].text,
	       (unsigned)pr->l[pr->n - 1].ms, pr->l[pr->n - 1].text);
	CHECK(pr->synced && pr->n > 20, "the prom synced");
	for (int k = 1; k < pr->n; k++) CHECK(pr->l[k].ms >= pr->l[k - 1].ms, "sorted at %d", k);
	meta_lyrics_free(pr);
	json_t *pj = json_loads("{\"plainLyrics\":\"\\n\\nverse one\\nline two\\n\\n\\nverse two\",\"syncedLyrics\":null}", 0, NULL);
	Lyrics *pl = lyrics_from(pj);
	json_decref(pj);
	CHECK(!pl->synced && pl->n == 4 && !strcmp(pl->l[0].text, "verse one") && !pl->l[2].text[0], "plain lyrics %d", pl->n);
	meta_lyrics_free(pl);

	for (int k = 0; k < njobs; k++) free(jobs[k].url);
	sm_free();
	printf(bad ? "%d FAILED\n" : "all good\n", bad);
	return bad != 0;
}
