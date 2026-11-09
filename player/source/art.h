// Cover art, decoded off the main thread: one big cover for whatever's playing, and a small
// thumbnail for every album, filled in one by one in the background.
#pragma once
#include <3ds.h>

#define ART_BIG   144
#define ART_THUMB 32

void art_init(void);
void art_exit(void);
void art_want(int track);    // the big cover for this song; the newest request wins
void art_ahead(const int *tracks, int n);   // songs likely next (and the one before): their covers are made ahead of time, so skipping never waits
void art_playing(bool on);   // music's playing: the thumbnails go gently, so the song gets the card first

// The big cover once it's decoded: ART_BIG² pixels as 0xRRGGBBAA (NULL when the song has
// none), and the colour the gradient uses. True once per result. The pixels hold still until
// the next art_want.
bool art_big(int *track, const u32 **px, u32 *color);

// Thumbnails finished since the last call, one album id per call; -1 when none.
int art_thumb(const u32 **px);
// Last run's thumbnails (the card's cache, read at art_init): put(album, px) for each, at once,
// so the first frame has them. Main thread, right after art_init.
void art_thumbs_cached(void (*put)(int album, const u32 *px));
// An album's thumbnail is on screen and not made yet: it goes ahead of the rest. Every frame is fine.
void art_thumb_need(int album);

// The playlists' own pictures (the sync's lists/<pid>.jpg), as ART_THUMB² tiles by list index:
// art_lists_want copies the paths (NULL: none) and starts over; art_list_thumb hands one over
// into px (ART_THUMB² u32), returning its index, -1 when none is ready.
#define ART_LISTS 64
void art_lists_want(char *const *paths, int n);
int art_list_thumb(u32 *px);

// The cover for an album or playlist page: a song's cover (track >= 0) or a picture file (path),
// ART_DETAIL square, and optionally a round ART_AVATAR picture for the owner line. The paths are
// copied, so the caller's strings may go. Returns the request's number; art_detail hands the
// result over once, with the number it answers (px NULL: no cover; avatar NULL: none).
#define ART_DETAIL 96
#define ART_AVATAR 32
int art_detail_want(int track, const char *path, const char *avatar);
bool art_detail(int *req, const u32 **px, u32 *color, const u32 **avatar);

// Big covers, ART_HOME square, for Home and the library's grids, in slots 0..ART_SLOTS-1 of the
// main thread's cache: an album's cover (track, a song of it), a picture file (path: a playlist's),
// or an artist's photo, round (artist: their name). px given: the card's shrunk copy is read right
// here, and true means px has it. Otherwise (or px NULL) the worker reads or makes it, newest asked
// first, and art_home_tile hands finished ones over, returning the slot (-1: none). A slot asked for
// again drops whatever it was making before. When it has nothing else to do, the worker shrinks every
// album's cover onto the card ahead of time, so a grid scrolled for the first time comes up sharp.
#define ART_HOME  88
#define ART_SLOTS 121
bool art_home_want(int slot, int track, const char *path, const char *artist, u32 *px);
int art_home_tile(u32 *px);
