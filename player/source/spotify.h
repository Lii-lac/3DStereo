// Spotify, for its lists only: over Wi-Fi, Pocket copies your Spotify playlists and Liked Songs
// onto the card as playlists of the files you already have, and sends your likes and unlikes
// from the 3DS back. Nothing is streamed. The login is made once on a PC (tools/spotify_pair.py
// writes sdmc:/3ds/player/spotify.json); the PC tool tools/spotify_sync.py keeps the same state
// file, so either can sync.
#pragma once
#include <3ds.h>

bool spot_paired(void);
// A sync in delay_ms (a sooner request wins; a later one pushes a waiting quiet sync back, so a
// run of likes makes one push). loud: say how it went, errors too. full: everything, both ways;
// otherwise only the 3DS's own likes and unlikes go up, which takes a second, not half a minute.
// A quiet full sync also runs by itself five minutes after the last one finished.
void spot_request(int delay_ms, bool loud, bool full);
// Every frame, on the main thread: starts a sync that's due, and applies one that's finished.
// say gets the line to show, if any; reloaded runs after Liked Songs changed, with lists true if
// the playlists were read again too (every Group in lib.lists is new then).
void spot_poll(void (*say)(const char *), void (*reloaded)(bool lists));
bool spot_busy(void);
void spot_quit(void);   // tell the thread to stop (before meta_exit: it may wait on Spotify's login)
void spot_exit(void);
void spot_init(void);   // on the main thread, before anything else here

// A change made on the 3DS to one of your Spotify playlists (pid: its Spotify id), already on the
// card (library_list_details / library_list_add); sent with the next push, and kept on the card
// until Spotify has it. name, desc NULL: as they are. Needs the playlist-modify pairing.
// name, desc: NULL unchanged; pub: -1 unchanged, else private (0) or public (1)
void spot_list_details(const char *pid, const char *name, const char *desc, int pub);
// Your playlist deleted on the 3DS: Spotify takes it out of your library too (DELETE /me/library,
// which is what Spotify's own Delete does since February 2026; anyone else's you follow, the same)
void spot_list_delete(const char *pid);
void spot_list_add(const char *pid, int track);
void spot_list_remove(const char *pid, int track);   // the song taken out of a playlist of yours

// An access token for another thread's Web API call (meta.c's searches), on that thread's own
// curl handle. stale: a token that just got a 401, so a fresh one is fetched (NULL: any).
#include <curl/curl.h>
bool spot_token(CURL *h, const char *stale, char *out, int cap, char *why, int wcap, volatile bool *quit);   // why: a toast line
