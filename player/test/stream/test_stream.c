// stream.c against the plain file: every byte the decoders could ask for, through random reads,
// little hops back, jumps anywhere, reads past the end, with the "card" made slow on purpose.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "stream.h"

static int slow_us;   // every fread of the reader's: this long first (a busy card)
size_t fread(void *p, size_t s, size_t n, FILE *f) {
	static size_t (*real)(void *, size_t, size_t, FILE *);
	if (!real) real = dlsym(RTLD_NEXT, "fread");
	if (slow_us && f && f->_flags && n * s >= 4096) usleep(slow_us);
	return real(p, s, n, f);
}

int main(void) {
	const char *path = "/tmp/song.bin";
	long size = 23 * 1024 * 1024 + 12345;   // over the 16 MB ring, so it wraps and drops what's behind
	FILE *w = fopen(path, "wb");
	unsigned x = 1;
	unsigned char *ref = malloc(size);
	for (long i = 0; i < size; i++) { x = x * 1103515245u + 12345u; ref[i] = x >> 16; }
	fwrite(ref, 1, size, w); fclose(w);
	int fails = 0;
	for (int pass = 0; pass < 3; pass++) {
		slow_us = pass == 1 ? 3000 : pass == 2 ? 200 : 0;
		FILE *f = stream_open(path);
		if (!f) { printf("open failed\n"); return 1; }
		long pos = 0;
		unsigned char *buf = malloc(400000);
		srand(pass + 7);
		for (int op = 0; op < 6000; op++) {
			int r = rand() % 100;
			if (r < 4) pos = rand() % (size + 100);                        // a jump anywhere (even past the end)
			else if (r < 12) { pos -= rand() % 300000; if (pos < 0) pos = 0; } // a hop back
			else if (r < 14) pos += rand() % 3000000;                        // a hop ahead
			if (r < 14) fseek(f, pos, SEEK_SET);
			if (ftell(f) != pos) { printf("tell %ld != %ld\n", ftell(f), pos); fails++; }
			size_t n = 1 + rand() % (r % 3 == 0 ? 400000 : 5000);
			size_t got = fread(buf, 1, n, f);
			size_t want = pos >= size ? 0 : (size_t)(size - pos) < n ? (size_t)(size - pos) : n;
			if (got != want || (got && memcmp(buf, ref + pos, got))) { printf("pass %d op %d: at %ld asked %zu got %zu want %zu%s\n", pass, op, pos, n, got, want, got == want ? " (bytes differ)" : ""); fails++; if (fails > 5) return 1; }
			pos += got;
			if (pos >= size) { clearerr(f); pos = 0; fseek(f, 0, SEEK_SET); }
		}
		// straight through, the way a song plays
		fseek(f, 0, SEEK_SET);
		long at = 0; size_t got;
		while ((got = fread(buf, 1, 8192, f)) > 0) { if (memcmp(buf, ref + at, got)) { printf("pass %d: straight read differs at %ld\n", pass, at); fails++; break; } at += got; }
		if (at != size) { printf("pass %d: straight read ended at %ld of %ld\n", pass, at, size); fails++; }
		fclose(f);
		free(buf);
		printf("pass %d (card delay %d us): %s\n", pass, slow_us, fails ? "FAIL" : "ok");
	}
	printf(fails ? "FAILED\n" : "all good\n");
	return fails != 0;
}
