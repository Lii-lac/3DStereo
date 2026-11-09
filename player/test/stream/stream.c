// A song, read off the card ahead of the decoder into memory, so the decoder never waits on the card.
//
// Build 50's dropouts all came while something else had the card (the Spotify sync's writes, covers
// being saved): the decoder read the file 128 KB at a time, straight off the card, from the audio
// worker itself, and a read stuck behind someone else's write for a second ran the 2.8 s of queued
// sound dry. Now each song has a reader thread that keeps up to 16 MB of it (a couple of minutes of
// a 16-bit FLAC) in a ring ahead of where the decoder is, and the decoder reads that memory. The card
// can stall for as long as that lasts and nobody hears it. The reader sits on the audio's core, above
// the worker, and spends its life waiting on the card, so it costs the decoder nothing.
//
// The decoders take it as a FILE (fopencookie), so dr_flac, dr_mp3, dr_wav, stb_vorbis and opusfile
// are none the wiser. A seek inside what's held is free; outside it, the reader starts over there.

#define _GNU_SOURCE
#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RING_MAX (16 << 20)
#define BACK     (512 * 1024)   // kept behind the decoder, for the little hops back a decoder makes
#define CHUNK    (256 * 1024)   // one trip to the card
#define FIRST    (32 * 1024)    // ...the first after a start or a jump: the decoder's waiting on it

typedef struct {
	FILE *f;                   // the card's, unbuffered, the reader's alone
	u8 *buf;
	s64 cap, size;
	s64 lo, hi;                // bytes [lo, hi) of the file are held, byte p at buf[p % cap]
	s64 pos;                   // the decoder's place
	s64 want;                  // a jump outside what's held: the reader starts over here (-1 none)
	int gen;                   // bumped by every jump, so a read already under way is thrown away
	bool fail, quit;
	LightLock lk;
	LightEvent more, room;     // the reader has more for the decoder; the decoder has made room or jumped
	Thread th;
} Stream;

static void reader(void *arg) {
	Stream *s = arg;
	s64 at = 0;   // where the card's file is
	bool fresh = true;
	while (1) {
		LightLock_Lock(&s->lk);
		if (s->quit) { LightLock_Unlock(&s->lk); break; }
		if (s->want >= 0) { s->lo = s->hi = s->want; s->want = -1; fresh = true; }
		s64 keep = s->pos - BACK;   // what's behind the decoder by more than BACK can go
		if (keep > s->lo) s->lo = keep < s->hi ? keep : s->hi;
		s64 hi = s->hi, room = s->cap - (s->hi - s->lo), n = fresh ? FIRST : CHUNK;
		int gen = s->gen;
		if (hi >= s->size || room <= 0 || s->fail) { LightLock_Unlock(&s->lk); LightEvent_Wait(&s->room); continue; }
		if (n > room) n = room;
		if (n > s->size - hi) n = s->size - hi;
		s64 off = hi % s->cap;
		if (n > s->cap - off) n = s->cap - off;   // up to the ring's end; the rest next time round
		LightLock_Unlock(&s->lk);

		// the region [hi, hi + n) is free: the decoder reads only below hi
		if (at != hi && fseek(s->f, (long)hi, SEEK_SET)) { LightLock_Lock(&s->lk); s->fail = true; LightLock_Unlock(&s->lk); LightEvent_Signal(&s->more); continue; }
		size_t got = fread(s->buf + off, 1, (size_t)n, s->f);
		at = hi + got;
		fresh = false;
		LightLock_Lock(&s->lk);
		if (gen == s->gen && s->want < 0) {
			s->hi += got;
			if (got < (size_t)n) s->fail = true;   // the card said no: what's held plays, then it ends
		}
		LightLock_Unlock(&s->lk);
		LightEvent_Signal(&s->more);
	}
}

static ssize_t c_read(void *c, char *b, size_t n) {
	Stream *s = c;
	LightLock_Lock(&s->lk);
	if (s->pos >= s->size) { LightLock_Unlock(&s->lk); return 0; }
	// outside what's held (or coming soon): start over there
	if (s->want < 0 && (s->pos < s->lo || s->pos > s->hi + 2 * CHUNK)) {
		s->want = s->pos;
		s->lo = s->hi = s->pos;
		s->gen++;
		s->fail = false;
		LightEvent_Signal(&s->room);
	}
	while (!s->quit && (s->want >= 0 || s->pos >= s->hi) && !(s->fail && s->want < 0 && s->pos >= s->hi)) {
		LightLock_Unlock(&s->lk);
		LightEvent_Signal(&s->room);
		LightEvent_Wait(&s->more);
		LightLock_Lock(&s->lk);
	}
	s64 k = s->hi - s->pos;
	if (k <= 0) { LightLock_Unlock(&s->lk); return 0; }   // the card failed here: the end, as far as anyone can tell
	if (k > (s64)n) k = n;
	s64 off = s->pos % s->cap, first = s->cap - off < k ? s->cap - off : k;
	memcpy(b, s->buf + off, first);
	if (first < k) memcpy(b + first, s->buf, k - first);
	s->pos += k;
	LightLock_Unlock(&s->lk);
	LightEvent_Signal(&s->room);   // room behind it, soon
	return k;
}

static int c_seek(void *c, off_t *off, int whence) {
	Stream *s = c;
	LightLock_Lock(&s->lk);
	s64 p = whence == SEEK_SET ? *off : whence == SEEK_CUR ? s->pos + *off : s->size + *off;
	if (p < 0) { LightLock_Unlock(&s->lk); return -1; }
	s->pos = p;
	*off = p;
	LightLock_Unlock(&s->lk);
	return 0;
}

static int c_close(void *c) {
	Stream *s = c;
	LightLock_Lock(&s->lk);
	s->quit = true;
	LightLock_Unlock(&s->lk);
	LightEvent_Signal(&s->room);
	LightEvent_Signal(&s->more);
	if (s->th) { threadJoin(s->th, U64_MAX); threadFree(s->th); }
	fclose(s->f);
	free(s->buf);
	free(s);
	return 0;
}

FILE *stream_open(const char *path) {
	Stream *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	if (!(s->f = fopen(path, "rb"))) { free(s); return NULL; }
	setvbuf(s->f, NULL, _IONBF, 0);   // the reads are big already: straight to the card
	struct stat st;
	s->size = !fstat(fileno(s->f), &st) ? st.st_size : 0;
	if (s->size <= 0) { fseek(s->f, 0, SEEK_END); s->size = ftell(s->f); fseek(s->f, 0, SEEK_SET); }
	s64 cap = s->size < RING_MAX ? s->size : RING_MAX;
	if (cap < CHUNK) cap = CHUNK;
	while (cap >= CHUNK && !(s->buf = malloc(cap))) cap /= 2;   // short of memory: a smaller ring still beats none
	if (!s->buf) { fclose(s->f); free(s); return NULL; }
	s->cap = cap;
	s->want = -1;
	LightLock_Init(&s->lk);
	LightEvent_Init(&s->more, RESET_ONESHOT);
	LightEvent_Init(&s->room, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s->th = threadCreate(reader, s, 16 * 1024, prio - 2, 2, false);   // the audio's core, a notch above its worker
	if (!s->th) s->th = threadCreate(reader, s, 16 * 1024, prio - 2, -2, false);
	if (!s->th) { fclose(s->f); free(s->buf); free(s); return NULL; }
	static const cookie_io_functions_t IO = {c_read, NULL, c_seek, c_close};
	FILE *f = fopencookie(s, "rb", IO);
	if (!f) { c_close(s); return NULL; }
	setvbuf(f, NULL, _IOFBF, 32 * 1024);   // the decoders' little reads (stb_vorbis's are a byte at a time) from memory
	return f;
}
