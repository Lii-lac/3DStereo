// tags.c's 32 KB head reader against the reader before it (tags.c.pre28), on real files: every
// field must come out the same. Both are built into one program, the old one renamed.
//   docker run --rm -v "$(pwd -W):/src" -v "C:/Users/alexd/Music:/music" -w /src ubuntu:24.04 sh -c
//     "apt-get -qq update && apt-get -qq install -y gcc >/dev/null && sh player/test/test_tags.sh /music"
#define _GNU_SOURCE
#include "tags.h"
#include "decoder.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Library lib;
bool tags_read_old(const char *path, int fmt, Track *t);

// funopen for glibc, on fopencookie
typedef struct { void *c; int (*r)(void *, char *, int); long (*s)(void *, long, int); int (*cl)(void *); } Sh;
static ssize_t sh_read(void *c, char *b, size_t n) { Sh *s = c; return s->r(s->c, b, (int)n); }
static int sh_seek(void *c, off64_t *o, int w) { Sh *s = c; long p = s->s(s->c, (long)*o, w); if (p < 0) return -1; *o = p; return 0; }
static int sh_close(void *c) { Sh *s = c; int r = s->cl(s->c); free(s); return r; }
FILE *funopen(const void *c, int (*r)(void *, char *, int), int (*w)(void *, const char *, int), long (*sk)(void *, long, int), int (*cl)(void *)) {
	(void)w;
	Sh *s = malloc(sizeof *s);
	*s = (Sh){(void *)c, r, sk, cl};
	return fopencookie(s, "r", (cookie_io_functions_t){sh_read, NULL, sh_seek, sh_close});
}

static int ext_fmt(const char *p) {
	const char *e = strrchr(p, '.');
	if (!e) return FMT_NONE;
	if (!strcasecmp(e, ".flac")) return FMT_FLAC;
	if (!strcasecmp(e, ".mp3")) return FMT_MP3;
	if (!strcasecmp(e, ".ogg") || !strcasecmp(e, ".oga")) return FMT_VORBIS;
	if (!strcasecmp(e, ".opus")) return FMT_OPUS;
	if (!strcasecmp(e, ".wav")) return FMT_WAV;
	return FMT_NONE;
}
static int same_s(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }
static int same_f(float a, float b) { return (isnan(a) && isnan(b)) || a == b; }
static int n, bad;
static void walk(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[2048];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (e->d_type == DT_DIR) { walk(p); continue; }
		int fmt = ext_fmt(p);
		if (fmt == FMT_NONE) continue;
		Track a = {0}, b = {0};
		u64 size = 0;
		bool oka = tags_read_old(p, fmt, &a), okb = tags_read(p, fmt, &b, &size);
		n++;
		int same = oka == okb && same_s(a.title, b.title) && same_s(a.artist, b.artist) && same_s(a.album, b.album) &&
		           same_s(a.album_artist, b.album_artist) && same_s(a.genre, b.genre) && a.disc == b.disc && a.no == b.no &&
		           a.year == b.year && a.secs == b.secs && a.rate == b.rate && a.bits == b.bits && a.kbps == b.kbps &&
		           a.pic_off == b.pic_off && a.pic_len == b.pic_len && same_f(a.gain_t, b.gain_t) && same_f(a.gain_a, b.gain_a) &&
		           a.peak_t == b.peak_t && a.peak_a == b.peak_a;
		if (!same) { bad++; printf("DIFFERENT: %s\n", p); }
	}
	closedir(d);
}
int main(int argc, char **argv) {
	for (int k = 1; k < argc; k++) walk(argv[k]);
	printf("%d files, %d different\n", n, bad);
	return bad != 0;
}
