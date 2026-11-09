// Which file on the card is this Spotify song? The same rules as Index.match in
// tools/spotify_sync.py: the title with its decorations off, at least one artist in common, and
// the closest length within 6 seconds. Reads lib.t only, which never changes after the scan.
#pragma once
#include "library.h"

typedef struct {
	const char *name;
	const char *artists[8];
	int na;
	u32 ms;   // 0 if Spotify didn't say
} SmTrack;

void sm_norm(const char *s, char *out, int cap);    // lowercase words, accents off, "&" as "and"
void sm_loose(const char *title, char *out, int cap);   // no (feat. X), [Remastered], - Radio Edit
void sm_build(void);                                // the index over lib.t; once is enough
void sm_free(void);
int sm_match(const SmTrack *t);                     // its library index, or -1
bool sm_matches(const SmTrack *t, int i, int slack);   // a search result for song i?
const char *sm_title(int i);                        // the title it's matched by
bool sm_artist(int i, int which, char *out, int cap);   // 0: the whole tag, 1: the lead artist
