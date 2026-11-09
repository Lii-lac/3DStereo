// Files on the card through a buffer. stdio's own is BUFSIZ, 1 KB on devkitARM (the card's fstat
// gives no block size), so reading a 281 KB artist photo was 275 trips to the card at a few ms each:
// 1.5 s of the 2 s a tap took (build 37's log). card_open_big (64 KB) is for anything that can be
// large or is read in many pieces: covers, photos, caches, lists, json. card_open (4 KB, build 97) is
// for the one-line files (ids, urls, volume, markers): a 64 KB malloc for forty bytes, with the art,
// meta and Spotify threads each holding one, was heap for nothing.
#pragma once
#include <stdio.h>

#define CARD_BUF (4 * 1024)
#define CARD_BUF_BIG (64 * 1024)
static inline FILE *card_open_sized(const char *path, const char *mode, size_t buf) {
	FILE *f = fopen(path, mode);
	if (f) setvbuf(f, NULL, _IOFBF, buf);
	return f;
}
static inline FILE *card_open(const char *path, const char *mode) { return card_open_sized(path, mode, CARD_BUF); }
static inline FILE *card_open_big(const char *path, const char *mode) { return card_open_sized(path, mode, CARD_BUF_BIG); }
