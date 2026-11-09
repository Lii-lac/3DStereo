// The music on the SD card: every file Pocket plays under sdmc:/music, tags read straight from
// the headers (and kept in a cache so the next launch only reads what changed), grouped into
// albums and artists, plus the .m3u8 playlists in sdmc:/music/playlists.
#pragma once
#include <3ds.h>

typedef struct {
	char *path, *title, *artist, *album, *album_artist;   // album_artist only if tagged
	char *genre;                  // NULL if untagged
	int album_id, artist_id;
	int fmt, disc, no, year;
	u32 secs, rate, bits, kbps;   // bits 0 and kbps set for lossy files
	u32 pic_off, pic_len;         // embedded cover, 0 if none
	float gain_t, gain_a;         // ReplayGain in dB, NAN if untagged
	float peak_t, peak_a;         // linear sample peaks, 0 if untagged
	u64 size, mtime;              // what the cache checks
} Track;

typedef struct {
	char *name, *artist;    // artist: the album artist, or "Various artists"; NULL for playlists
	int *songs, n;          // library indices: disc/track order, or the playlist's own order
	u32 color;              // tile colour until the art loads
	// a playlist from a Spotify sync (all NULL/0 otherwise, and always for Liked Songs)
	char *pid, *desc, *image, *owner, *owner_id;   // image: its picture's copy on the card
	bool pub;
	int sp_total;           // Spotify's whole list, songs not on the card too
	u32 sp_secs;
	u32 *added;             // per song, parallel to songs: when it went in, as yyyymmdd (0 unknown)
	u8 *expl;               // ...and whether it's explicit (NULL arrays: nothing known)
	char *file;             // a playlist's .m3u8 on the card (NULL for Liked Songs and the rest)
} Group;

typedef struct {
	Track *t; int nt;
	int *by_title;          // all songs, A to Z
	Group *albums; int nal;
	Group *artists; int nar;
	Group *lists; int nl;   // playlists
	int cached;             // how many came out of the cache rather than a fresh read
} Library;

extern Library lib;
void library_scan(bool fresh, void (*progress)(int found, int done, int total, const char *path));   // fresh: ignore the cache. progress: every real tag read, with how many files are looked at of how many, and the file just read
int library_find(const char *path);                          // index by path, -1 if gone
u32 name_color(const char *s);
#define LISTS_DIR "sdmc:/3ds/player/lists"   // the sync's playlist pictures: <pid>.jpg, me.jpg, me.id

// Liked Songs: always the first playlist, newest first, kept as an ordinary .m3u8 in the
// playlists folder so the PC can read it too
bool library_is_liked(int track);
bool library_like(int track);   // likes it, or unlikes it if it was; true if it's liked now
bool library_like_many(const int *songs, int n);   // a whole album: all in, or all out if they were
void library_exit(void);        // waits for the last like to reach the card

// After a Spotify sync (spotify.c): read the playlist files again, keeping Liked Songs as it is in
// memory; and apply Spotify's likes and unlikes (add: newest first, on top)
void library_reload_lists(void);
void library_liked_apply(const int *add, int nadd, const int *drop, int ndrop);

// Changing a playlist on the 3DS (k > 0: Liked Songs is the hearts'). The card's file is rewritten
// at once, and a list from a Spotify sync loses its snapshot line, so the next sync after the
// change has reached Spotify reads it fresh. Each returns NULL when done, or a toast line.
const char *library_list_details(int k, const char *name, const char *desc);   // NULL: as it is
const char *library_list_add(int k, int track);
const char *library_list_public(int k, bool pub);
const char *library_list_remove(int k, int track);   // its line (and the #ADDED / #EXPLICIT over it) out of the file   // what the file says (#PUBLIC:), and Group.pub
// The playlist's file off the card and the lists read again: every index past k shifts (main's
// on_reloaded(true) after it). NULL done, else a toast line.
const char *library_list_delete(int k);
// A name FAT takes for a playlist's file: no <>:"/\|?* or control characters, no dots or spaces
// at the ends, 120 bytes at most (the sync names its files the same way)
void library_file_name(const char *title, char *out, int cap);
// A song's path the way a playlist in sdmc:/music/playlists writes it ("../Artist/song.flac")
const char *library_rel(int track, char *buf, int cap);

// Hands text to the quiet thread that writes files (path and dir must stay put: literals);
// it frees the text, and a newer text for the same file replaces one still waiting
void bg_save(const char *path, const char *dir, char *text);
