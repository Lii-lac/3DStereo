// Artists and lyrics from the internet, on a quiet thread of its own: an artist's page as Spotify
// shows it (open.spotify.com's own artist page, the one any browser gets), and lyrics from
// LRCLIB. Everything is cached on the card, so a page seen once opens offline too.
#pragma once
#include <3ds.h>

// ---- artists --------------------------------------------------------------------------------

#define MI_TILE   72    // covers and faces, square
#define MI_HEAD_W 400   // the photo across the top, cropped wide
#define MI_HEAD_H 176
#define MI_SLOTS  49    // image slots: 0 the header, 1..48 tiles

typedef struct { char *name, *uri, *artists; u64 plays; u32 ms; bool explicit_; int img, local; } MTop;
typedef struct { char *name, *uri, *type; int year, img, local; } MRelease;   // local: an album id
typedef struct { char *name, *id; int img, local; } MArtist;                  // local: an artist id
typedef struct { char *title, *venue, *city; int y, mo, d; } MGig;

typedef struct {
	char id[32];
	char *name, *bio;            // bio: plain text, may be NULL
	u32 listeners, followers;
	u32 color;                   // Spotify's dark colour for this artist, 0xRRGGBB
	bool has_header;             // slot 0 is coming
	int avatar;                  // their face, round: a tile slot, -1 if none
	MTop top[10]; int ntop;
	MRelease pop[10], albums[16], singles[16], appears[10]; int npop, nalb, nsing, napp;
	MArtist rel[10]; int nrel;
	MGig gig[6]; int ngig, gigs_total;
	char fetched[16];            // "2026-11-04"
	bool offline;                // the card's copy: a fresh one couldn't be had
} Artist;

typedef enum { MS_IDLE, MS_LOADING, MS_READY, MS_FAILED } MState;

void meta_init(void);   // after the library's read and spot_init
void meta_quit(void);   // stop now (with spot_quit, before either join)
void meta_exit(void);

// The page for one of the library's artists (local: its name) or any Spotify artist (id, with
// local the name to show while it loads). The newest request wins.
void meta_artist_want(const char *local, const char *id);
// A page that's ready, handed over once: it's the caller's now (meta_artist_free). NULL if none.
// gen tells which request it answers.
Artist *meta_artist_take(int *gen);
void meta_artist_free(Artist *a);
MState meta_artist_state(char *why, int cap);   // the newest request's
// The artist of this song is likely to be opened next: their page and its first pictures go to
// the front of the queue fetched ahead of time (which otherwise works through every artist on
// the card, quietly, whenever nothing else is asked for).
void meta_artist_soon(int track);
void meta_playing(bool on);   // music's playing: the work ahead of time takes its time, so the song gets the card

// The Artists tab's faces: an artist's photo, round, MI_FACE square (0xRRGGBBAA), made from what
// the card has (the page and picture fetched ahead of time). One per call as they're made: the
// artist's index into lib.artists, or -1 when none is waiting. Past MI_FACES artists, none.
#define MI_FACE  32
#define MI_FACES 256
int meta_face(u32 *px);
// Last run's faces, from one file: put(x, px) for each artist it has. Main thread, after the
// library's read and before meta_init's worker starts; the count it put.
int meta_faces_cached(void (*put)(int x, const u32 *px));
// The card's copy of an artist's photo (the picture their face is made from), by name: true and
// its path if it's there. Reads the card; any thread.
bool meta_photo_file(const char *name, char *path, int cap);

// Is `name` one of the artists on song i ("A, B & C feat. D" split up, compared loosely)?
bool meta_on_song(int track, const char *name);

// Pictures for the page in hand, one per call as they're decoded: the slot, its pixels
// (0xRRGGBBAA, w×h), and the request it belongs to. -1 when none. Call meta_img_done after.
int meta_img(int *gen, const u32 **px, int *w, int *h);
void meta_img_done(void);

// ---- lyrics ---------------------------------------------------------------------------------

typedef struct { u32 ms; char *text; } LLine;
typedef struct { int track; bool synced, instrumental, offline; LLine *l; int n; } Lyrics;   // offline: not cached, no Wi-Fi

void meta_lyrics_want(int track);    // the song playing; -1 for none
Lyrics *meta_lyrics_take(void);      // a finished lookup, the caller's now (n 0: none found)
void meta_lyrics_free(Lyrics *l);
MState meta_lyrics_state(void);
void meta_lyrics_again(void);       // retry the one that came back offline
