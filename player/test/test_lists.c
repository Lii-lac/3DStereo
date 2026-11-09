// Playlists changed on the 3DS (build 29), on a PC: library.c's renames, descriptions and adds on
// real files, and spotify.c's outbox against a fake Spotify that records every request. Runs in a
// scratch folder with its own sdmc:/ tree. From the project root:
//   docker run --rm -e ASAN_OPTIONS=detect_leaks=0 -v "$(pwd -W):/src" -w /src ubuntu:24.04 sh -c "apt-get -qq update && apt-get -qq install -y
//     gcc libjansson-dev >/dev/null && sh player/test/test_lists.sh"
#include <stdarg.h>
#include <stdio.h>
#include "../source/spotify.c"
#include "../source/decoder.h"

// ---- what the 3DS side would give ----------------------------------------------------------------

Result archive_getmtime(const char *p, u64 *t) { (void)p; *t = 1; return 0; }
int tags_card_reads;
int fmt_of(const char *p) { const char *d = strrchr(p, '.'); return d && !strcmp(d, ".flac") ? FMT_FLAC : FMT_NONE; }
// tags from the name: "sdmc:/music/Artist/Title.flac", 200 s each
bool tags_read(const char *path, int fmt, Track *t, u64 *size) {
	(void)fmt;
	const char *a = path + strlen("sdmc:/music/"), *s = strchr(a, '/'), *d = strrchr(path, '.');
	t->artist = strndup(a, s - a);
	t->title = strndup(s + 1, d - s - 1);
	t->album = strdup(t->title);
	t->secs = 200;
	t->gain_t = t->gain_a = 0;
	*size = 1;
	return true;
}
void crumb(const char *fmt, ...) {
	va_list a;
	va_start(a, fmt);
	fputs("  crumb: ", stdout);
	vprintf(fmt, a);
	va_end(a);
	putchar('\n');
}
bool net_up(char *e, int c) { (void)e; (void)c; return true; }
bool net_wifi(void) { return true; }
CURL *net_handle(void) { return (CURL *)1; }

// ---- a fake Spotify ------------------------------------------------------------------------------

static char reqs[64][600];
static int nreq;
static long put_code = 200;   // what a PUT /playlists/<id> gets
static const char *list_name = "Old Name";
static long fake(const char *m, const char *u, const char *f, Buf *o) {
	snprintf(reqs[nreq < 63 ? nreq++ : 63], 600, "%s %s %s", m, u + (strncmp(u, API, strlen(API)) ? 0 : strlen(API)), f ? f : "-");
	const char *p = u + strlen(API);
	char body[2000] = "";
	long code = 200;
	if (!strncmp(p, "/search", 7))
		snprintf(body, sizeof body, "{\"tracks\":{\"items\":[{\"id\":\"TRACK2\",\"name\":\"Song Two\",\"duration_ms\":200000,\"artists\":[{\"name\":\"Band\"}]}]}}");
	else if (!strcmp(m, "PUT") && !strncmp(p, "/playlists/", 11)) code = put_code;
	else if (!strcmp(m, "POST")) code = 201;
	else if (!strncmp(p, "/me/playlists", 13))
		snprintf(body, sizeof body, "{\"items\":[{\"id\":\"PID1\",\"name\":\"%s\",\"snapshot_id\":\"S2\",\"description\":\"\","
		         "\"owner\":{\"id\":\"me\",\"display_name\":\"Lilac\"},\"public\":false,\"items\":{\"total\":1}}],\"next\":null}", list_name);
	else if (!strncmp(p, "/playlists/PID1/items", 21))
		snprintf(body, sizeof body, "{\"items\":[{\"added_at\":\"2026-01-01T00:00:00Z\",\"item\":{\"type\":\"track\",\"id\":\"TRACK1\","
		         "\"name\":\"Song One\",\"duration_ms\":200000,\"artists\":[{\"name\":\"Band\"}]}}],\"next\":null}");
	else if (!strcmp(p, "/me")) snprintf(body, sizeof body, "{\"id\":\"me\"}");
	o->d = strdup(body);
	o->n = strlen(body);
	return code;
}
long net_req(CURL *h, const char *m, const char *u, const char *f, const char *a, Buf *o, long *r, volatile bool *q, CURLcode *c) {
	(void)h; (void)a; (void)r; (void)q; (void)c;
	free(o->d);
	*o = (Buf){0};
	return fake(m, u, f, o);
}

// ---- checks --------------------------------------------------------------------------------------

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)
static char *rd(const char *p) {   // leaks a little: a test
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	static char b[8192];
	size_t n = fread(b, 1, sizeof b - 1, f);
	b[n] = 0;
	fclose(f);
	return strdup(b);
}
static void wr(const char *p, const char *s) { FILE *f = fopen(p, "wb"); fputs(s, f); fclose(f); }
static int list_named(const char *n) { for (int k = 0; k < lib.nl; k++) if (!strcmp(lib.lists[k].name, n)) return k; return -1; }
static int track_titled(const char *t) { for (int i = 0; i < lib.nt; i++) if (!strcmp(lib.t[i].title, t)) return i; return -1; }
static bool sent(const char *s) { for (int k = 0; k < nreq; k++) if (strstr(reqs[k], s)) return true; return false; }
static void show_reqs(void) { for (int k = 0; k < nreq; k++) printf("    %s\n", reqs[k]); }

int main(void) {
	mkdir("sdmc:", 0777); mkdir("sdmc:/music", 0777); mkdir("sdmc:/music/Band", 0777); mkdir("sdmc:/music/playlists", 0777);
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/player", 0777); mkdir(LISTS_DIR, 0777);
	wr("sdmc:/music/Band/Song One.flac", "x");
	wr("sdmc:/music/Band/Song Two.flac", "x");
	wr("sdmc:/music/Band/Song Three.flac", "x");
	wr(LISTS_DIR "/me.id", "me");
	// as a sync writes one, and one of Alex's own, plain
	wr("sdmc:/music/playlists/Old Name.m3u8", "#EXTM3U\n#PLAYLIST:Old Name\n#SPOTIFY:PID1\n#DESCRIPTION:was here\n#OWNER:me|Lilac\n"
	   "#PUBLIC:0\n#TOTAL:2|400\n#ADDED:2026-01-01\n../Band/Song One.flac\n# not on the card (1):\n# Gone - Band\n#SNAPSHOT:S1 0 0\n");
	wr("sdmc:/music/playlists/chill.m3u8", "../Band/Song One.flac\r\n");
	library_scan(true, NULL);
	CHECK(lib.nt == 3, "3 songs, got %d", lib.nt);
	int sp = list_named("Old Name"), lo = list_named("chill"), two = track_titled("Song Two"), three = track_titled("Song Three");
	CHECK(sp > 0 && lo > 0 && two >= 0 && three >= 0, "lists %d %d, songs %d %d", sp, lo, two, three);
	CHECK(sp > 0 && lib.lists[sp].file && !strcmp(lib.lists[sp].file, "sdmc:/music/playlists/Old Name.m3u8"), "file kept");

	// a Spotify list: renamed (with a character FAT won't take) and described
	CHECK(!library_list_details(sp, "New: Name", "  fresh  "), "details");
	CHECK(!strcmp(lib.lists[sp].name, "New_ Name"), "name %s", lib.lists[sp].name);
	CHECK(!rd("sdmc:/music/playlists/Old Name.m3u8"), "old file gone");
	char *t = rd("sdmc:/music/playlists/New_ Name.m3u8");
	CHECK(t && strstr(t, "#PLAYLIST:New: Name\n") && strstr(t, "#DESCRIPTION:fresh\n") && !strstr(t, "#SNAPSHOT") && strstr(t, "#SPOTIFY:PID1\n")
	      && strstr(t, "# Gone - Band\n"), "rewritten:\n%s", t);
	free(t);
	CHECK(!library_list_add(sp, two), "add");
	CHECK(library_list_add(sp, two) != NULL, "twice refused");
	t = rd("sdmc:/music/playlists/New_ Name.m3u8");
	CHECK(t && strstr(t, "#ADDED:") && strstr(t, "../Band/Song Two.flac\n"), "added:\n%s", t);
	free(t);
	CHECK(lib.lists[sp].n == 2 && lib.lists[sp].songs[1] == two && lib.lists[sp].added && lib.lists[sp].added[1] > 20260000, "in memory");
	CHECK(!library_list_details(sp, NULL, ""), "description off");
	t = rd("sdmc:/music/playlists/New_ Name.m3u8");
	CHECK(t && !strstr(t, "#DESCRIPTION"), "no description:\n%s", t);
	free(t);
	CHECK(!lib.lists[sp].desc, "desc NULL");

	// one of Alex's plain lists: the header goes on; a case-only rename keeps one file
	CHECK(!library_list_details(lo, "Chill", NULL), "case rename");
	t = rd("sdmc:/music/playlists/Chill.m3u8");
	CHECK(t && !strncmp(t, "#EXTM3U\n#PLAYLIST:Chill\n../Band/Song One.flac\n", 46), "plain:\n%s", t);
	free(t);
	DIR *d = opendir("sdmc:/music/playlists");
	int files = 0;
	for (struct dirent *e; (e = readdir(d));) files += e->d_name[0] != '.';
	closedir(d);
	CHECK(files == 2, "files %d (New_ Name, Chill)", files);
	CHECK(library_list_details(lo, "new_ name", NULL) != NULL, "a taken name refused");
	CHECK(library_list_details(lo, "   ", NULL) != NULL, "a blank name refused");
	CHECK(library_list_details(0, "x", NULL) != NULL, "Liked Songs refused");
	CHECK(!library_list_add(lo, three), "plain add");

	// read back as the next launch would
	library_reload_lists();
	sp = list_named("New_ Name");
	lo = list_named("Chill");
	CHECK(sp > 0 && lib.lists[sp].n == 2 && lib.lists[sp].pid && !strcmp(lib.lists[sp].pid, "PID1"), "reloaded sp");
	CHECK(lo > 0 && lib.lists[lo].n == 2, "reloaded plain");

	// ---- the outbox ----
	spot_init();
	snprintf(token, sizeof token, "TOKEN");
	tok_until = (u64)-1 / 2;
	snprintf(tok_scope, sizeof tok_scope, "playlist-read-private user-library-read user-library-modify");   // an old pairing
	curl = (CURL *)1;
	sm_build();
	spot_list_details("PID1", "New: Name", NULL);
	spot_list_details("PID1", NULL, "");   // the same list again: one change
	spot_list_add("PID1", two);
	CHECK(json_array_size(outbox) == 2, "two changes, got %zu", json_array_size(outbox));
	t = rd(OUTBOX);
	CHECK(t && strstr(t, "\"name\": \"New: Name\"") && strstr(t, "\"desc\": \"\"") && strstr(t, "Song Two.flac"), "outbox file:\n%s", t);
	free(t);
	lists_scope = false;
	CHECK(push_lists() && lists_scope && json_array_size(outbox) == 2 && nreq == 0, "old pairing: kept, nothing sent");

	// the pairing allows it, but Spotify says no to the name for now (a 500): it waits
	snprintf(tok_scope, sizeof tok_scope, "playlist-modify-private playlist-modify-public");
	put_code = 500;
	lists_scope = false;
	CHECK(!push_lists() && json_array_size(outbox) == 2, "a 500 keeps both");
	// ...and a sync meanwhile leaves the card's copy alone (Spotify still says the old name)
	nreq = 0;
	L = NULL; nL = 0;
	err[0] = 0;
	CHECK(sync_lists(), "sync: %s", err);
	CHECK(rd("sdmc:/music/playlists/New_ Name.m3u8") && !rd("sdmc:/music/playlists/Old Name.m3u8"), "the waiting change stands");
	CHECK(!sent("/playlists/PID1/items?limit"), "not fetched");

	put_code = 200;
	nreq = 0;
	nsent = 0;
	CHECK(push_lists() && json_array_size(outbox) == 0 && nsent == 2, "sent, %zu left", json_array_size(outbox));
	show_reqs();
	CHECK(sent("PUT /playlists/PID1 {\"name\":\"New: Name\",\"description\":\"\"}"), "the PUT");
	CHECK(sent("POST /playlists/PID1/items?uris=spotify:track:TRACK2 -"), "the POST");
	CHECK(!rd(OUTBOX), "outbox file gone when empty");

	// Spotify has the new name now: the sync reads it fresh into the same file
	list_name = "New: Name";
	nreq = 0;
	CHECK(sync_lists(), "sync 2: %s", err);
	CHECK(sent("/playlists/PID1/items?limit"), "fetched");
	t = rd("sdmc:/music/playlists/New_ Name.m3u8");
	CHECK(t && strstr(t, "#SNAPSHOT:S2"), "fresh:\n%s", t);
	free(t);

	printf(bad ? "%d FAILED\n" : "all good\n", bad);
	return bad != 0;
}
