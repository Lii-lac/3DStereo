#define _GNU_SOURCE   // strcasestr
#include "cardio.h"
#include "library.h"
#include "crumb.h"
#include "decoder.h"
#include "tags.h"
#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ROOT "sdmc:/music"
#define LISTS ROOT "/playlists"
#define CACHE_DIR "sdmc:/3ds/player"
#define CACHE CACHE_DIR "/library.cache"
#define CACHE_MAGIC 0x32434b50   // "PKC2": six strings a song (genre); the 3DS's Track didn't change size for it

Library lib;
static int cap;
static int *by_path;   // library indices sorted by path, for library_find

static char *dupn(const char *s, int n) { char *d = malloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }

// ---- the cache: last scan's tracks, trusted while a file's size and mtime hold ------------

// The strings, then every number from fmt on, written as they lie: the header carries
// sizeof(Track), so a build that changes the struct throws the old cache away.
static const size_t STR[] = {offsetof(Track, path), offsetof(Track, title), offsetof(Track, artist), offsetof(Track, album), offsetof(Track, album_artist), offsetof(Track, genre)};
#define NSTR 6
#define NUMS offsetof(Track, fmt)
static char **str(Track *t, int k) { return (char **)((char *)t + STR[k]); }

static Track *old;
static int nold;

static int by_path_cmp(const void *a, const void *b) { return strcmp(((const Track *)a)->path, ((const Track *)b)->path); }

// The whole cache comes off the card in one read and is cut up in memory (build 97): it used to be a
// fread of two bytes and another of the string for every one of the six strings of every song, and a
// fread of the numbers, 13 stdio calls a song, through a 64 KB buffer.
typedef struct { const u8 *p, *end; bool ok; } Cur;

static char *get_str(Cur *c) {
	u16 n;
	if (!c->ok || c->end - c->p < 2) { c->ok = false; return NULL; }
	memcpy(&n, c->p, 2);
	c->p += 2;
	if (n == 0xFFFF) return NULL;
	if (c->end - c->p < n) { c->ok = false; return NULL; }
	char *s = dupn((const char *)c->p, n);
	c->p += n;
	return s;
}

static void cache_load(void) {
	FILE *f = fopen(CACHE, "rb");
	if (!f) return;
	setvbuf(f, NULL, _IONBF, 0);   // unbuffered: the one big fread goes to the card in one piece
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *buf = n > 12 && n < (64 << 20) ? malloc(n) : NULL;
	if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (!buf) return;
	u32 h[3];
	memcpy(h, buf, sizeof h);
	if (h[0] == CACHE_MAGIC && h[1] == sizeof(Track) && h[2] < 100000) {
		old = calloc(h[2] ? h[2] : 1, sizeof(Track));
		Cur c = {buf + sizeof h, buf + n, true};
		for (u32 i = 0; i < h[2] && c.ok; i++) {   // a cut-off file keeps every whole entry
			Track *t = &old[nold];
			for (int k = 0; k < NSTR; k++) *str(t, k) = get_str(&c);
			bool ok = c.ok && t->path && t->title && c.end - c.p >= (long)(sizeof(Track) - NUMS);
			if (ok) { memcpy((char *)t + NUMS, c.p, sizeof(Track) - NUMS); c.p += sizeof(Track) - NUMS; nold++; }
			else { c.ok = false; for (int k = 0; k < NSTR; k++) free(*str(t, k)); }
		}
		qsort(old, nold, sizeof *old, by_path_cmp);
	}
	free(buf);
}

// check: the size and date have to match too (NULL sizes: trusted by name, as the fast path does)
static bool from_cache(Track *t, const char *path, const u64 *size, const u64 *mtime) {
	Track key = {.path = (char *)path};
	Track *o = nold ? bsearch(&key, old, nold, sizeof *old, by_path_cmp) : NULL;
	if (!o || !o->title || (size && (o->size != *size || o->mtime != *mtime))) return false;
	*t = *o;
	t->path = strdup(path);
	o->title = o->artist = o->album = o->album_artist = o->genre = NULL;   // theirs now; the path stays for bsearch
	return true;
}

static void cache_free(void) {
	for (int i = 0; i < nold; i++)
		for (int k = 0; k < NSTR; k++) free(*str(&old[i], k));
	free(old);
	old = NULL;
	nold = 0;
}

static void put_str(FILE *f, const char *s) {
	u16 n = !s ? 0xFFFF : strlen(s) < 0xFFFF ? strlen(s) : 0xFFFE;
	fwrite(&n, 2, 1, f);
	if (s) fwrite(s, 1, n, f);
}

static void cache_save(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(CACHE_DIR, 0777);
	FILE *f = card_open_big(CACHE, "wb");
	if (!f) return;
	u32 h[3] = {CACHE_MAGIC, sizeof(Track), lib.nt};
	fwrite(h, sizeof h, 1, f);
	for (int i = 0; i < lib.nt; i++) {
		Track *t = &lib.t[i];
		for (int k = 0; k < NSTR; k++) put_str(f, *str(t, k));
		fwrite((char *)t + NUMS, sizeof(Track) - NUMS, 1, f);
	}
	fclose(f);
}

// ---- the scan ----------------------------------------------------------------------------

// "name [dQw4w9WgXcQ].mp3" from a downloader: the video id is noise, and so is the extension.
static char *title_from_file(const char *path) {
	const char *s = strrchr(path, '/');
	s = s ? s + 1 : path;
	const char *dot = strrchr(s, '.');
	int n = dot && dot > s ? dot - s : (int)strlen(s);
	if (n > 14 && s[n - 1] == ']' && s[n - 13] == '[' && s[n - 14] == ' ') n -= 14;
	return dupn(s, n);
}

static char *folder_of(const char *path) {
	const char *e = strrchr(path, '/');
	if (!e) return NULL;
	const char *b = e - 1;
	while (b > path && *b != '/') b--;
	if (*b == '/') b++;
	if (e - b == 5 && !strncasecmp(b, "music", 5)) return NULL;   // loose in sdmc:/music
	return dupn(b, e - b);
}

// One song into the library: from the cache when it knows the name (no card reads at all: a
// song retagged in place waits for R held at launch, like the fast path), else its tags read.
// Up to build 22 one new song made every song here pay a size and a date check, a crawl.
// Returns whether the tags had to be read.
static u64 scan_ms[3], scan_t;   // where a scan's time goes: listing, dates, tags
static bool add(const char *path) {
	if (lib.nt == cap) { cap = cap ? cap * 2 : 64; lib.t = realloc(lib.t, cap * sizeof(Track)); }
	Track *t = &lib.t[lib.nt];
	memset(t, 0, sizeof *t);
	if (from_cache(t, path, NULL, NULL)) { lib.cached++; lib.nt++; return false; }
	scan_t = osGetTime();
	u64 size = 0, mtime = 0;   // the size from the read itself: one open of the file, not two
	archive_getmtime(path, &mtime);   // stat's st_mtime is always 0 on the SD card
	scan_ms[1] += osGetTime() - scan_t;
	scan_t = osGetTime();
	if (!tags_read(path, fmt_of(path), t, &size)) {
		for (int k = 0; k < NSTR; k++) free(*str(t, k));
		return true;
	}
	scan_ms[2] += osGetTime() - scan_t;
	scan_t = osGetTime();
	t->path = strdup(path);
	t->size = size;
	t->mtime = mtime;
	if (!t->title) t->title = title_from_file(path);
	if (!t->artist) t->artist = strdup("Unknown artist");
	if (!t->album && !(t->album = folder_of(path))) t->album = strdup("Unknown album");
	lib.nt++;
	return true;
}

// Just the names: a directory listing costs next to nothing, while the size and mtime checks
// are two filesystem calls per song, which is what made every launch crawl.
static char **paths;
static int np, pcap;
static int str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(const char *dir, int depth) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[768];
		if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
		bool is_dir = e->d_type == DT_DIR;
		struct stat st;
		if (e->d_type == DT_UNKNOWN) is_dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
		if (is_dir) { if (depth < 5) walk(p, depth + 1); }
		else if (fmt_of(p) != FMT_NONE) {
			if (np == pcap) { pcap = pcap ? pcap * 2 : 128; paths = realloc(paths, pcap * sizeof *paths); }
			paths[np++] = strdup(p);
		}
	}
	closedir(d);
}

// ---- grouping ----------------------------------------------------------------------------

u32 name_color(const char *s) {
	static const u32 PAL[] = {0x7a0f1c, 0x2e3b30, 0x1f2a44, 0x4b2a55, 0x33525e, 0x6b4a1a, 0x5a2a3a, 0x284a3c};
	u32 h = 2166136261u;
	while (*s) h = (h ^ (u8)*s++) * 16777619u;
	return PAL[h % 8];
}

// "Quadeca, Olēka" and "x feat. y" file under the first name.
static int lead_len(const char *a) {
	static const char *SEP[] = {", ", " feat", " ft.", " & ", " x "};
	int n = strlen(a);
	for (int k = 0; k < 5; k++) {
		const char *s = strcasestr(a, SEP[k]);
		if (s && s - a > 0 && s - a < n) n = s - a;
	}
	return n;
}

static Track *T(int i) { return &lib.t[i]; }
static int by_title(const void *a, const void *b) { return strcasecmp(T(*(int *)a)->title, T(*(int *)b)->title); }
static int by_path_i(const void *a, const void *b) { return strcasecmp(T(*(int *)a)->path, T(*(int *)b)->path); }
static int by_track(const void *a, const void *b) {
	Track *x = T(*(int *)a), *y = T(*(int *)b);
	if (x->disc != y->disc) return x->disc - y->disc;
	if (x->no != y->no) return x->no - y->no;
	return strcasecmp(x->title, y->title);
}
static int by_album_track(const void *a, const void *b) {
	Track *x = T(*(int *)a), *y = T(*(int *)b);
	if (x->year != y->year) return x->year - y->year;
	int c = strcasecmp(x->album, y->album);
	return c ? c : by_track(a, b);
}
static int by_name(const void *a, const void *b) { return strcasecmp(((Group *)a)->name, ((Group *)b)->name); }
// an artist's albums together, oldest first; songs are already in track order
static int by_artist_year(const void *a, const void *b) {
	const Group *x = a, *y = b;
	int c = strcasecmp(x->artist, y->artist);
	if (c) return c;
	int yx = T(x->songs[0])->year, yy = T(y->songs[0])->year;
	return yx != yy ? yx - yy : strcasecmp(x->name, y->name);
}

static void push(Group *g, int i) { g->songs = realloc(g->songs, (g->n + 1) * sizeof(int)); g->songs[g->n++] = i; }

static void group(void) {
	int n = lib.nt;
	lib.by_title = malloc((n ? n : 1) * sizeof(int));
	by_path = malloc((n ? n : 1) * sizeof(int));
	for (int i = 0; i < n; i++) lib.by_title[i] = by_path[i] = i;
	qsort(lib.by_title, n, sizeof(int), by_title);
	qsort(by_path, n, sizeof(int), by_path_i);

	lib.albums = calloc(n ? n : 1, sizeof(Group));
	lib.artists = calloc(n ? n : 1, sizeof(Group));
	for (int i = 0; i < n; i++) {
		Track *t = T(i);
		int a = 0;
		for (; a < lib.nal; a++) {
			Track *f = T(lib.albums[a].songs[0]);
			if (!strcmp(f->album, t->album) && !strcmp(f->album_artist ? f->album_artist : "", t->album_artist ? t->album_artist : "")) break;
		}
		if (a == lib.nal) lib.albums[lib.nal++].name = t->album;
		push(&lib.albums[a], i);

		int ll = lead_len(t->artist);
		for (a = 0; a < lib.nar; a++)
			if ((int)strlen(lib.artists[a].name) == ll && !strncmp(lib.artists[a].name, t->artist, ll)) break;
		if (a == lib.nar) lib.artists[lib.nar++].name = dupn(t->artist, ll);
		push(&lib.artists[a], i);
	}
	for (int a = 0; a < lib.nal; a++) {
		Group *g = &lib.albums[a];
		Track *f = T(g->songs[0]);
		g->artist = f->album_artist ? f->album_artist : f->artist;
		for (int k = 1; k < g->n && !f->album_artist; k++)
			if (strcmp(T(g->songs[k])->artist, f->artist)) { g->artist = "Various artists"; break; }
		qsort(g->songs, g->n, sizeof(int), by_track);
	}
	for (int a = 0; a < lib.nar; a++) qsort(lib.artists[a].songs, lib.artists[a].n, sizeof(int), by_album_track);
	qsort(lib.albums, lib.nal, sizeof(Group), by_artist_year);
	qsort(lib.artists, lib.nar, sizeof(Group), by_name);
	for (int a = 0; a < lib.nal; a++) {
		lib.albums[a].color = name_color(lib.albums[a].name);
		for (int k = 0; k < lib.albums[a].n; k++) T(lib.albums[a].songs[k])->album_id = a;
	}
	for (int a = 0; a < lib.nar; a++) {
		lib.artists[a].color = name_color(lib.artists[a].name);
		for (int k = 0; k < lib.artists[a].n; k++) T(lib.artists[a].songs[k])->artist_id = a;
	}
}

static int find_cmp(const void *key, const void *e) { return strcasecmp(key, T(*(const int *)e)->path); }

int library_find(const char *path) {
	int *i = lib.nt ? bsearch(path, by_path, lib.nt, sizeof(int), find_cmp) : NULL;   // FAT ignores case, so do we
	return i ? *i : -1;
}

// ---- playlists ---------------------------------------------------------------------------

// "sdmc:/music/playlists/../Album/./song.mp3" to "sdmc:/music/Album/song.mp3", in place.
// Needs a '/' after the device, which every path built below has.
static void tidy(char *p) {
	char *root = strchr(p, ':');
	root = root ? root + 1 : p;
	char *out = root, *s = root;
	while (*s) {
		while (*s == '/') s++;
		char *e = s;
		while (*e && *e != '/') e++;
		int n = e - s;
		if (n == 2 && s[0] == '.' && s[1] == '.') { while (out > root && *--out != '/'); }
		else if (n && !(n == 1 && *s == '.')) { *out++ = '/'; memmove(out, s, n); out += n; }
		s = e;
	}
	if (out == root) *out++ = '/';
	*out = 0;
}

static void list_free(Group *g) {
	free(g->name); free(g->songs); free(g->pid); free(g->desc); free(g->image); free(g->owner); free(g->owner_id);
	free(g->added); free(g->expl); free(g->file);
}

static void load_lists(void) {
	DIR *d = opendir(LISTS);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		const char *dot = strrchr(e->d_name, '.');
		if (e->d_name[0] == '.' || !dot || (strcasecmp(dot, ".m3u8") && strcasecmp(dot, ".m3u"))) continue;
		char p[768], line[1024], full[1100];
		if (snprintf(p, sizeof p, LISTS "/%s", e->d_name) >= (int)sizeof p) continue;
		FILE *f = card_open_big(p, "r");
		if (!f) continue;
		Group g = {0};
		u32 added = 0;      // the next song's, from the line before it
		bool expl = false, meta = false;
		char *img_url = NULL;
		while (fgets(line, sizeof line, f)) {
			char *s = line;
			if (!memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;
			s[strcspn(s, "\r\n")] = 0;
			while (*s == ' ' || *s == '\t') s++;
			if (*s == '#') {   // the sync's notes about the list and its songs; every other # line is a comment
				int yy, mm, dd;
				if (!strncmp(s, "#ADDED:", 7) && sscanf(s + 7, "%d-%d-%d", &yy, &mm, &dd) == 3) { added = yy * 10000 + mm * 100 + dd; meta = true; }
				else if (!strcmp(s, "#EXPLICIT")) { expl = true; meta = true; }
				else if (!strncmp(s, "#SPOTIFY:", 9) && !g.pid) { char *v = s + 9; while (*v == ' ') v++; g.pid = dupn(v, strcspn(v, " \t")); }
				else if (!strncmp(s, "#DESCRIPTION:", 13) && !g.desc && s[13]) g.desc = strdup(s + 13);
				else if (!strncmp(s, "#IMAGE:", 7) && !img_url && s[7]) img_url = strdup(s + 7);
				else if (!strncmp(s, "#PUBLIC:", 8)) g.pub = s[8] == '1';
				else if (!strncmp(s, "#TOTAL:", 7)) { unsigned tn = 0, ts = 0; if (sscanf(s + 7, "%u|%u", &tn, &ts) >= 1) { g.sp_total = tn; g.sp_secs = ts; } }
				else if (!strncmp(s, "#OWNER:", 7) && !g.owner) {
					const char *bar = strchr(s + 7, '|');
					if (bar) { g.owner_id = dupn(s + 7, bar - (s + 7)); g.owner = strdup(bar + 1); }
				}
				continue;
			}
			if (!*s) continue;
			u32 this_added = added;   // whatever the line before said, it's this line's alone
			bool this_expl = expl;
			added = 0;
			expl = false;
			for (char *c = s; *c; c++)
				if (*c == '\\') *c = '/';
			if (!strncasecmp(s, "sdmc:", 5)) snprintf(full, sizeof full, "sdmc:/%s", s + 5);
			else if (*s == '/') snprintf(full, sizeof full, "sdmc:%s", s);
			else if (strchr(s, ':')) continue;   // a PC path, "C:/Users/...": nothing we can find
			else snprintf(full, sizeof full, LISTS "/%s", s);
			tidy(full);
			int i = library_find(full);
			if (i < 0) continue;
			push(&g, i);
			g.added = realloc(g.added, g.n * sizeof(u32));
			g.expl = realloc(g.expl, g.n);
			g.added[g.n - 1] = this_added;
			g.expl[g.n - 1] = this_expl;
		}
		fclose(f);
		if (!meta) { free(g.added); free(g.expl); g.added = NULL; g.expl = NULL; }   // a plain list: nothing known
		if (g.pid && img_url) {   // the sync keeps its picture beside the others, by the playlist's id
			char ip[160];
			snprintf(ip, sizeof ip, LISTS_DIR "/%s.jpg", g.pid);
			g.image = strdup(ip);
		}
		free(img_url);
		if (!g.n) { list_free(&g); continue; }   // nothing in it is on the card
		g.name = dupn(e->d_name, dot - e->d_name);
		g.file = strdup(p);
		g.color = name_color(g.name);
		lib.lists = realloc(lib.lists, (lib.nl + 1) * sizeof(Group));
		lib.lists[lib.nl++] = g;
	}
	closedir(d);
	qsort(lib.lists, lib.nl, sizeof(Group), by_name);
}

// ---- Liked Songs -------------------------------------------------------------------------

#define LIKED "Liked Songs"
static void pin_liked(void) {
	int at = -1;
	for (int i = 0; i < lib.nl; i++) if (!strcasecmp(lib.lists[i].name, LIKED)) at = i;
	if (at < 0) {   // never liked anything yet: an empty one, so it's there to fill
		lib.lists = realloc(lib.lists, (lib.nl + 1) * sizeof(Group));
		lib.lists[lib.nl] = (Group){.name = dupn(LIKED, sizeof LIKED - 1)};
		at = lib.nl++;
	}
	Group g = lib.lists[at];
	g.color = 0x1ed760;
	// the 3DS keeps this one itself, one song at a time: no per-song notes to keep in step
	free(g.added); free(g.expl); g.added = NULL; g.expl = NULL;
	memmove(lib.lists + 1, lib.lists, at * sizeof(Group));
	lib.lists[0] = g;
}

bool library_is_liked(int track) {
	if (!lib.nl) return false;
	Group *g = &lib.lists[0];
	for (int k = 0; k < g->n; k++) if (g->songs[k] == track) return true;
	return false;
}

// Writing to the card takes long enough to stall a frame, so a like (or a listen) only builds
// the text; a quiet thread of its own puts it on the card, and the newest text per file wins.
static struct { const char *path, *dir; char *text; } slot[4];   // waiting to be written
static LightLock save_lock;
static LightEvent save_ev;
static Thread saver;
static volatile bool save_run;

// Overwritten in place and trimmed: the clusters it already has get reused, so FAT never goes
// hunting for free ones or rewrites the folder, which is what held up the music's reads.
static void write_file(const char *path, const char *dir, const char *text) {
	FILE *f = card_open_big(path, "r+");
	if (!f) { mkdir(dir, 0777); f = card_open_big(path, "w"); }
	if (!f) return;
	size_t n = strlen(text);
	fwrite(text, 1, n, f);
	fflush(f);
	ftruncate(fileno(f), n);
	fclose(f);
}

static void write_waiting(void) {
	for (int k = 0; k < 4; k++) {
		LightLock_Lock(&save_lock);
		char *t = slot[k].text;
		const char *p = slot[k].path, *d = slot[k].dir;
		slot[k].text = NULL;
		LightLock_Unlock(&save_lock);
		if (t) { write_file(p, d, t); free(t); }
	}
}

static void save_loop(void *arg) {
	(void)arg;
	while (save_run) {
		LightEvent_Wait(&save_ev);
		write_waiting();
	}
}

void bg_save(const char *path, const char *dir, char *text) {
	if (!saver) {
		LightLock_Init(&save_lock);
		LightEvent_Init(&save_ev, RESET_ONESHOT);
		save_run = true;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		saver = threadCreate(save_loop, NULL, 16 * 1024, prio + 2 < 0x3F ? prio + 2 : 0x3F, 0, false);
		if (!saver) { write_file(path, dir, text); free(text); return; }   // no thread: a hitch beats losing it
	}
	LightLock_Lock(&save_lock);
	int k = 0;
	while (k < 3 && slot[k].path && strcmp(slot[k].path, path)) k++;
	free(slot[k].text);
	slot[k].path = path;
	slot[k].dir = dir;
	slot[k].text = text;
	LightLock_Unlock(&save_lock);
	LightEvent_Signal(&save_ev);
}

static void save_liked(void) {
	Group *g = &lib.lists[0];
	size_t cap = 16, n = 0;
	for (int k = 0; k < g->n; k++) cap += strlen(lib.t[g->songs[k]].path) + 4;
	char *t = malloc(cap);
	if (!t) return;
	n += sprintf(t, "#EXTM3U\n");
	for (int k = 0; k < g->n; k++) {   // relative to the playlists folder, so a PC finds them too
		const char *p = lib.t[g->songs[k]].path;
		if (!strncmp(p, ROOT "/", sizeof ROOT)) n += sprintf(t + n, "../%s\n", p + sizeof ROOT);
		else n += sprintf(t + n, "%s\n", p);
	}
	bg_save(LISTS "/" LIKED ".m3u8", LISTS, t);
}

bool library_like_many(const int *songs, int n) {
	if (!lib.nl || n <= 0) return false;
	bool all = true;
	for (int k = 0; k < n && all; k++) all = library_is_liked(songs[k]);
	Group *g = &lib.lists[0];
	if (all) {   // every one's in already: out they all go
		int w = 0;
		for (int k = 0; k < g->n; k++) {
			bool drop = false;
			for (int j = 0; j < n && !drop; j++) drop = g->songs[k] == songs[j];
			if (!drop) g->songs[w++] = g->songs[k];
		}
		g->n = w;
	} else {     // the missing ones, on top, in the album's own order
		int add = 0;
		for (int k = 0; k < n; k++) add += !library_is_liked(songs[k]);
		g->songs = realloc(g->songs, (g->n + add) * sizeof(int));
		memmove(g->songs + add, g->songs, g->n * sizeof(int));
		int at = 0;
		for (int k = 0; k < n; k++) {
			bool had = false;
			for (int j = add; j < g->n + add && !had; j++) had = g->songs[j] == songs[k];
			if (!had) g->songs[at++] = songs[k];
		}
		g->n += add;
	}
	save_liked();
	return !all;
}

void library_exit(void) {
	if (!saver) return;
	save_run = false;
	LightEvent_Signal(&save_ev);
	threadJoin(saver, U64_MAX);
	threadFree(saver);
	saver = NULL;
	write_waiting();   // a like in its last moments
}

bool library_like(int track) {
	if (!lib.nl || track < 0 || track >= lib.nt) return false;
	Group *g = &lib.lists[0];
	for (int k = 0; k < g->n; k++)
		if (g->songs[k] == track) {
			memmove(g->songs + k, g->songs + k + 1, (g->n - k - 1) * sizeof(int));
			g->n--;
			save_liked();
			return false;
		}
	g->songs = realloc(g->songs, (g->n + 1) * sizeof(int));
	memmove(g->songs + 1, g->songs, g->n * sizeof(int));   // newest on top
	g->songs[0] = track;
	g->n++;
	save_liked();
	return true;
}

// ---- playlists changed on the 3DS --------------------------------------------------------------

void library_file_name(const char *title, char *out, int cap) {
	char t[512];
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)title; *p && n < (int)sizeof t - 1; p++)
		t[n++] = *p < 0x20 || strchr("<>:\"/\\|?*", *p) ? '_' : *p;
	t[n] = 0;
	char *s = t;
	while (*s == ' ' || *s == '.') s++;
	n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '.')) n--;
	if (!n) { snprintf(out, cap, "Playlist"); return; }
	if (n > 120) { n = 120; while (n && ((u8)s[n] & 0xC0) == 0x80) n--; }
	if (n > cap - 1) n = cap - 1;
	memcpy(out, s, n);
	out[n] = 0;
}

const char *library_rel(int track, char *buf, int cap) {
	const char *p = lib.t[track].path;
	if (!strncmp(p, ROOT "/", sizeof ROOT)) snprintf(buf, cap, "../%s", p + sizeof ROOT);
	else snprintf(buf, cap, "%s", p);
	return buf;
}

static char *read_text(const char *path) {
	FILE *f = card_open_big(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	char *s = n >= 0 ? malloc(n + 1) : NULL;
	if (s) { n = fread(s, 1, n, f); s[n] = 0; }
	fclose(f);
	return s;
}

// Straight onto the card, not through the quiet thread: the next change reads the file back, so
// it has to be there already. A playlist is a few KB, and it's a tap's worth of waiting.
static bool write_now(const char *path, const char *text) {
	FILE *f = card_open_big(path, "r+");
	if (!f) { mkdir(LISTS, 0777); f = card_open_big(path, "w"); }
	if (!f) return false;
	size_t n = strlen(text);
	bool ok = fwrite(text, 1, n, f) == n && !fflush(f);
	ftruncate(fileno(f), n);
	return !fclose(f) && ok;
}

typedef struct { char *s; size_t n, cap; } Txt;
static void tput(Txt *t, const char *s, size_t n) {
	if (t->n + n + 1 > t->cap) { t->cap = (t->n + n + 1) * 2; t->s = realloc(t->s, t->cap); }
	memcpy(t->s + t->n, s, n);
	t->n += n;
	t->s[t->n] = 0;
}
static void tline(Txt *t, const char *a, const char *b) { tput(t, a, strlen(a)); if (b) tput(t, b, strlen(b)); tput(t, "\n", 1); }

// The file's lines as they were, except #PLAYLIST and #DESCRIPTION when given (an empty desc
// takes the line out), no #SNAPSHOT, and append on the end
static char *list_text(const char *old, const char *name, const char *desc, const char *append) {
	Txt t = {0};
	if (!memcmp(old, "\xEF\xBB\xBF", 3)) old += 3;
	bool had_name = false, had_desc = false;
	for (const char *l = old; *l; l += strcspn(l, "\n"), l += *l == '\n') {
		while (*l == ' ' || *l == '\t') l++;
		if (!strncmp(l, "#PLAYLIST:", 10)) had_name = true;
		if (!strncmp(l, "#DESCRIPTION:", 13)) had_desc = true;
	}
	bool head = strncmp(old, "#EXTM3U", 7) != 0;   // a plain list: the header goes on first
	if (head) tline(&t, "#EXTM3U", NULL);
	for (const char *l = old; *l || head;) {
		if (head) {   // just under #EXTM3U: what the file didn't say yet
			head = false;
			if (name && !had_name) tline(&t, "#PLAYLIST:", name);
			if (desc && desc[0] && !had_desc) tline(&t, "#DESCRIPTION:", desc);
			continue;
		}
		size_t n = strcspn(l, "\n");
		const char *e = l + n;
		size_t m = n;
		if (m && l[m - 1] == '\r') m--;
		const char *s = l;
		while (s < l + m && (*s == ' ' || *s == '\t')) s++;
		if (!strncmp(s, "#PLAYLIST:", 10) && name) tline(&t, "#PLAYLIST:", name);
		else if (!strncmp(s, "#DESCRIPTION:", 13) && desc) { if (desc[0]) tline(&t, "#DESCRIPTION:", desc); }
		else if (!strncmp(s, "#SNAPSHOT:", 10)) {}
		else if (m || *e) { tput(&t, l, m); tput(&t, "\n", 1); }
		if (!strncmp(s, "#EXTM3U", 7)) head = true;
		l = *e ? e + 1 : e;
	}
	if (append) tput(&t, append, strlen(append));
	return t.s ? t.s : strdup("");
}

static void one_line(char *s) { for (; *s; s++) if ((u8)*s < 0x20) *s = ' '; }

const char *library_list_details(int k, const char *name, const char *desc) {
	if (k <= 0 || k >= lib.nl || !lib.lists[k].file) return "Can't change this playlist";
	Group *g = &lib.lists[k];
	char nm[512] = "", ds[1024] = "", fname[160], path[200];
	if (name) {
		snprintf(nm, sizeof nm, "%s", name);
		one_line(nm);
		char *a = nm;
		while (*a == ' ') a++;
		memmove(nm, a, strlen(a) + 1);
		for (int n = strlen(nm); n && nm[n - 1] == ' ';) nm[--n] = 0;
		if (!nm[0]) return "A playlist needs a name";
		library_file_name(nm, fname, sizeof fname);
		if (!strcasecmp(fname, LIKED)) return "That name's Liked Songs'";
		for (int j = 0; j < lib.nl; j++)
			if (j != k && !strcasecmp(lib.lists[j].name, fname)) return "There's a playlist called that";
		snprintf(path, sizeof path, LISTS "/%s.m3u8", fname);
	}
	if (desc) {
		snprintf(ds, sizeof ds, "%s", desc);
		one_line(ds);
		char *a = ds;
		while (*a == ' ') a++;
		memmove(ds, a, strlen(a) + 1);
		for (int n = strlen(ds); n && ds[n - 1] == ' ';) ds[--n] = 0;
	}
	char *old = read_text(g->file);
	if (!old) return "Couldn't read the playlist";
	char *text = list_text(old, name ? nm : NULL, desc ? ds : NULL, NULL);
	free(old);
	bool ok;
	if (name && strcmp(path, g->file)) {
		if (!strcasecmp(path, g->file)) {   // the same file to FAT: write it, then put the new case on it
			char tmp[210];
			snprintf(tmp, sizeof tmp, "%s.tmp", g->file);
			ok = write_now(g->file, text) && !rename(g->file, tmp) && !rename(tmp, path);
		} else {
			ok = write_now(path, text);
			if (ok) remove(g->file);
		}
		if (ok) {
			free(g->file);
			g->file = strdup(path);
			free(g->name);
			g->name = strdup(fname);
		}
	} else ok = write_now(g->file, text);
	free(text);
	if (!ok) return "Couldn't save the playlist";
	if (desc) { free(g->desc); g->desc = ds[0] ? strdup(ds) : NULL; }
	crumb("list: %s changed%s%s", g->name, name ? ", named" : "", desc ? ", described" : "");
	return NULL;
}

const char *library_list_public(int k, bool pub) {
	if (k <= 0 || k >= lib.nl || !lib.lists[k].file) return "Can't change this playlist";
	Group *g = &lib.lists[k];
	char *old = read_text(g->file);
	if (!old) return "Couldn't read the playlist";
	Txt t = {0};
	const char *o = old;
	if (!memcmp(o, "\xEF\xBB\xBF", 3)) o += 3;
	bool put = false;
	for (const char *l = o; *l;) {   // every line as it was, but #PUBLIC: says the new one (first under #EXTM3U if it wasn't there)
		size_t n = strcspn(l, "\n");
		const char *e = l + n;
		if (!strncmp(l, "#PUBLIC:", 8)) { if (!put) tline(&t, "#PUBLIC:", pub ? "1" : "0"); put = true; }
		else {
			tput(&t, l, n);
			tput(&t, "\n", 1);
			if (!put && !strncmp(l, "#EXTM3U", 7)) {
				// a later #PUBLIC: line would be the second: look ahead, and only add one if there's none
				if (!strstr(e, "\n#PUBLIC:")) { tline(&t, "#PUBLIC:", pub ? "1" : "0"); put = true; }
			}
		}
		l = *e ? e + 1 : e;
	}
	free(old);
	if (!put) { free(t.s); return "Couldn't change the playlist"; }
	bool ok = write_now(g->file, t.s ? t.s : "");
	free(t.s);
	if (!ok) return "Couldn't save the playlist";
	g->pub = pub;
	crumb("list: %s now %s", g->name, pub ? "public" : "private");
	return NULL;
}

const char *library_list_remove(int k, int track) {
	if (k <= 0 || k >= lib.nl || !lib.lists[k].file || track < 0 || track >= lib.nt) return "Can't change this playlist";
	Group *g = &lib.lists[k];
	int at = -1;
	for (int j = 0; j < g->n && at < 0; j++) if (g->songs[j] == track) at = j;
	if (at < 0) return "It isn't in there";
	char rel[1024];
	library_rel(track, rel, sizeof rel);
	char *old = read_text(g->file);
	if (!old) return "Couldn't read the playlist";
	Txt t = {0}, held = {0};   // held: a song's own lines over it, till its path says whether it stays
	bool gone = false;
	const char *o = old;
	if (!memcmp(o, "\xEF\xBB\xBF", 3)) o += 3;
	for (const char *l = o; *l;) {
		size_t n = strcspn(l, "\n"), m = n;
		const char *e = l + n;
		if (m && l[m - 1] == '\r') m--;
		bool own = !strncmp(l, "#ADDED:", 7) || !strncmp(l, "#EXPLICIT", 9) || !strncmp(l, "#EXTINF", 7);
		if (own) { tput(&held, l, m); tput(&held, "\n", 1); }
		else if (m && l[0] != '#') {   // a song
			bool it = !gone && m == strlen(rel) && !strncasecmp(l, rel, m);   // FAT doesn't mind the case, so neither do we
			if (it) gone = true;
			else { if (held.n) tput(&t, held.s, held.n); tput(&t, l, m); tput(&t, "\n", 1); }
			held.n = 0;
		} else { if (held.n) { tput(&t, held.s, held.n); held.n = 0; } if (m || *e) { tput(&t, l, m); tput(&t, "\n", 1); } }
		l = *e ? e + 1 : e;
	}
	if (held.n) tput(&t, held.s, held.n);
	free(held.s);
	free(old);
	if (!gone) { free(t.s); return "Couldn't find it in the file"; }
	bool ok = write_now(g->file, t.s ? t.s : "");
	free(t.s);
	if (!ok) return "Couldn't save the playlist";
	g->n--;   // and out of what's in memory, the dates and the E badges with it
	memmove(g->songs + at, g->songs + at + 1, (g->n - at) * sizeof(int));
	if (g->added) memmove(g->added + at, g->added + at + 1, (g->n - at) * sizeof(u32));
	if (g->expl) memmove(g->expl + at, g->expl + at + 1, g->n - at);
	crumb("list: a song out of %s", g->name);
	return NULL;
}

const char *library_list_delete(int k) {
	if (k <= 0 || k >= lib.nl || !lib.lists[k].file) return "Can't delete this playlist";
	if (remove(lib.lists[k].file)) return "Couldn't delete the playlist";
	crumb("list: %s deleted", lib.lists[k].name);
	library_reload_lists();
	return NULL;
}

const char *library_list_add(int k, int track) {
	if (k <= 0 || k >= lib.nl || !lib.lists[k].file || track < 0 || track >= lib.nt) return "Can't add to this playlist";
	Group *g = &lib.lists[k];
	for (int j = 0; j < g->n; j++) if (g->songs[j] == track) return "It's already in there";
	char rel[1024], add[1100], day[16];
	time_t now = time(NULL);
	struct tm tmv;
	strftime(day, sizeof day, "%Y-%m-%d", localtime_r(&now, &tmv));
	snprintf(add, sizeof add, "#ADDED:%s\n%s\n", day, library_rel(track, rel, sizeof rel));
	char *old = read_text(g->file);
	if (!old) return "Couldn't read the playlist";
	char *text = list_text(old, NULL, NULL, add);
	free(old);
	bool ok = write_now(g->file, text);
	free(text);
	if (!ok) return "Couldn't save the playlist";
	int n = g->n + 1;
	g->songs = realloc(g->songs, n * sizeof(int));
	g->songs[g->n] = track;
	// the date shows on the page; the songs before it keep theirs (or none)
	u32 *ad = realloc(g->added, n * sizeof(u32));
	if (!ad) { free(g->added); g->added = NULL; }
	else {
		if (!g->added) memset(ad, 0, g->n * sizeof(u32));
		g->added = ad;
		ad[g->n] = (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
	}
	u8 *ex = realloc(g->expl, n);
	if (!ex) { free(g->expl); g->expl = NULL; }
	else { if (!g->expl) memset(ex, 0, g->n); g->expl = ex; ex[g->n] = 0; }
	g->n = n;
	if (g->pid) { g->sp_total++; g->sp_secs += lib.t[track].secs; }
	crumb("list: %s + %s", g->name, lib.t[track].path);
	return NULL;
}

void library_scan(bool fresh, void (*progress)(int, int, int, const char *)) {
	u64 t0 = osGetTime();
	if (!fresh) cache_load();
	walk(ROOT, 0);
	scan_ms[0] = osGetTime() - t0;
	scan_ms[1] = scan_ms[2] = 0;
	qsort(paths, np, sizeof *paths, str_cmp);
	// The same names as last time: the cache is the library, no checking file by file. A song
	// retagged under the same name waits for R held at launch.
	bool same = !fresh && np == nold;
	for (int i = 0; same && i < np; i++) same = old[i].title && !strcmp(paths[i], old[i].path);
	if (same) {
		lib.t = old;
		lib.nt = lib.cached = cap = nold;
		old = NULL;
		nold = 0;
	} else {
		int reads = 0;
		for (int i = 0; i < np; i++)
			if (add(paths[i]) && progress) progress(++reads, i + 1, np, paths[i]);   // only real tag reads are worth a screen
		bool changed = lib.cached != lib.nt || lib.cached != nold;
		cache_free();
		if (changed) cache_save();
	}
	for (int i = 0; i < np; i++) free(paths[i]);
	free(paths);
	paths = NULL;
	np = pcap = 0;
	crumb("scan: %d songs, %d read (%d card reads), in %llu ms: cache+listing %llu, dates %llu, tags %llu", lib.nt,
	      lib.nt - lib.cached, tags_card_reads, osGetTime() - t0, scan_ms[0], scan_ms[1], scan_ms[2]);
	group();
	load_lists();
	pin_liked();
}

// ---- after a Spotify sync ------------------------------------------------------------------

// The sync rewrote the playlist files: read them again, but keep Liked Songs as it is in
// memory, since that's the copy the likes since then went into.
void library_reload_lists(void) {
	Group liked = lib.nl ? lib.lists[0] : (Group){0};
	for (int i = 1; i < lib.nl; i++) list_free(&lib.lists[i]);
	free(lib.lists);
	lib.lists = NULL;
	lib.nl = 0;
	load_lists();
	for (int i = 0; i < lib.nl; i++)
		if (!strcasecmp(lib.lists[i].name, LIKED)) {   // the file's copy: ours wins
			list_free(&lib.lists[i]);
			memmove(lib.lists + i, lib.lists + i + 1, (lib.nl - i - 1) * sizeof(Group));
			lib.nl--;
			break;
		}
	if (!liked.name) { pin_liked(); return; }
	lib.lists = realloc(lib.lists, (lib.nl + 1) * sizeof(Group));
	memmove(lib.lists + 1, lib.lists, lib.nl * sizeof(Group));
	lib.lists[0] = liked;
	lib.nl++;
}

// Spotify's side of the likes: drop what was unliked there, and put what was liked there on
// top, newest first, the order they're given in
void library_liked_apply(const int *add, int nadd, const int *drop, int ndrop) {
	if (!lib.nl || (nadd <= 0 && ndrop <= 0)) return;
	Group *g = &lib.lists[0];
	int w = 0;
	for (int k = 0; k < g->n; k++) {
		bool out = false;
		for (int j = 0; j < ndrop && !out; j++) out = g->songs[k] == drop[j];
		if (!out) g->songs[w++] = g->songs[k];
	}
	g->n = w;
	int *fresh = malloc((nadd > 0 ? nadd : 1) * sizeof(int)), nf = 0;
	for (int j = 0; j < nadd; j++)
		if (add[j] >= 0 && add[j] < lib.nt && !library_is_liked(add[j])) {
			bool dup = false;
			for (int i = 0; i < nf && !dup; i++) dup = fresh[i] == add[j];
			if (!dup) fresh[nf++] = add[j];
		}
	if (nf) {
		g->songs = realloc(g->songs, (g->n + nf) * sizeof(int));
		memmove(g->songs + nf, g->songs, g->n * sizeof(int));
		memcpy(g->songs, fresh, nf * sizeof(int));
		g->n += nf;
	}
	free(fresh);
	save_liked();
}
