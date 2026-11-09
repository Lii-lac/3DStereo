// Spotify Connect: Pocket shows up in Spotify's device list (on a PC, a phone, anywhere) as a
// game console called "Pocket", and Spotify drives it: play, pause, seek, skip, shuffle, repeat,
// the queue. Nothing is streamed: every song Spotify asks for plays from the card, matched the
// way the playlists are (spotmatch.c); one that isn't on the card is skipped.
// It's librespot's protocol (github.com/librespot-org/librespot, connect/ and core/dealer/): a
// websocket to Spotify's dealer for the commands, and the device's state PUT to spclient.
// The login is the desktop app's id, made once on a PC (tools/spotify_connect_pair.py writes
// sdmc:/3ds/player/spotify-connect.json); song lookups use Pocket's own pairing (spotify.json).
#pragma once
#include "audio.h"
#include <3ds.h>

bool connect_paired(void);
bool connect_login_dead(void);   // Spotify said the saved login was revoked: it needs pairing again
// Pairing on the 3DS (Spotify's device-code login, like a TV): connect_pair_start asks for a code and waits for it
// to be typed at spotify.com/pair; the status shows it. state: 0 idle, 1 asking, 2 waiting, 3 paired (connect_start
// after), 4 failed (err says why).
// kind (with state 4): why it failed, for the page to say in plain words
enum { PZ_NONE, PZ_NOWIFI, PZ_CLOCK, PZ_DNS, PZ_CONNECT, PZ_TIMEOUT, PZ_CERT, PZ_DROPPED, PZ_BUSY, PZ_SERVER, PZ_REFUSED, PZ_EXPIRED, PZ_DENIED, PZ_PORTAL, PZ_SAVE, PZ_UNKNOWN };
typedef struct { int state; int kind; long http; int skew_days; char code[16]; char url[40]; char err[80]; int secs_left; } ConnectPair;
void connect_pair_start(void);
void connect_pair_cancel(void);
void connect_pair_status(ConnectPair *out);
void connect_init(void);    // main thread, before anything else here
void connect_start(void);   // after net_init and sm_build: the thread, if paired
void connect_quit(void);    // tell the thread to stop
void connect_exit(void);    // and wait for it

bool connect_active(void);  // Pocket is Spotify's device right now
// The link to Spotify's dealer is open, so Pocket is in Spotify's device list (idle or not). The
// shut lid keeps the system and its Wi-Fi up for as long as this holds, or a phone couldn't find
// Pocket to pair with.
bool connect_online(void);
// Spotify's own context plays (false while Spotify follows the 3DS: when the 3DS was already playing
// at a handover, or picked something of its own since; then its queue plays and Spotify shows it)
bool connect_driving(void);
void connect_upcoming(const int *next, int n, int nq);   // main: the 3DS's own next few, as library indices; the first nq were queued by hand (Spotify shows them as its queue)
// main: the 3DS wants this song queued next. While Spotify drives it goes into Spotify's own queue (true: it'll
// be added, looked up and heard by Spotify in a moment); false when the 3DS's own queue should take it.
bool connect_queue_add(int track);
// What's coming, as Spotify has it while it drives: up to cap library indices (-1: a song that isn't on the card),
// queued[i] for the ones put in the queue by hand. 0 when Spotify isn't driving.
int connect_queue_view(int *tracks, bool *queued, int cap);
// Take the k-th upcoming song out of Spotify's order, or move one to another place (k counted from the song after the
// playing one, as connect_queue_view lists them). A song moved into the leading queued ones becomes queued, out of them not.
int connect_queue_clear(void);   // every song queued by hand, out of Spotify's order (while it drives); how many went
// A removed song, remembered so it can go back: where it sat in Spotify's order, what was on either side of it and how
// long the order was after it left. connect_queue_restore only puts it back if the order is still that one.
typedef struct { int at, val, left, right, n; } ConnectUndo;
bool connect_queue_remove(int k, ConnectUndo *u);   // false: nothing was there to remove
int connect_queue_restore(const ConnectUndo *u);    // the place it came back at (counted like k), -1 if the order has moved on
void connect_queue_move(int from, int to);
// main: where the 3DS's own queue came from, so a handover hands Spotify the album or playlist
// (pid: a synced playlist's Spotify id, else NULL; single: a queue of one song)
enum { SRC_OTHER, SRC_ALBUM, SRC_PLAYLIST };
void connect_source(int kind, const char *pid, bool single);

enum { CD_NONE, CD_LOAD, CD_PAUSE, CD_SEEK, CD_SKIP };
typedef struct {
	int kind;
	int track;      // CD_LOAD: the library index
	double secs;    // CD_LOAD, CD_SEEK
	bool paused;    // CD_LOAD, CD_PAUSE
	int dir;        // CD_SKIP: the 3DS's own queue, +1 or -1 (while Spotify follows the 3DS)
	char say[64];   // a toast to show, if any (with any kind, CD_NONE too)
	char src[64];   // CD_LOAD: where it's from, for the player's header
	char ctx[96];   // CD_LOAD: the context's uri (spotify:playlist:..., spotify:album:...), for Home's Recents
	bool vol_set;   // with any kind: Spotify's volume slider moved
	float vol;      // ...to this, 0..1, for audio_volume
	bool opts_set;  // with any kind: Spotify changed shuffle or repeat (or a handover brought them)
	bool shuffle;   // ...to these, for the 3DS's own buttons
	int repeat;     // ...0 off, 1 the context, 2 the song (the player's own numbering)
} ConnectDo;
// Every frame, on the main thread: what Spotify wants done (true if anything), and the player's
// own changes (a pause or a seek on the 3DS, a song ending) go back up to Spotify.
bool connect_poll(const AudioInfo *ai, ConnectDo *out);
// The audio worker's next song while Spotify drives: the library index, -1 to stop there, or -2
// when Spotify isn't driving (then the player's own queue decides). *pause: open it paused.
int connect_next(bool *pause);
void connect_skip(int dir);   // the 3DS's own skip buttons while Spotify drives
void connect_release(void);   // the 3DS played something of its own: Spotify lets go
// main, every frame: the 3DS's shuffle and repeat (0 off, 1 all, 2 one). A change goes up to
// Spotify; while Spotify drives, it also reorders Spotify's context the way its own button would.
void connect_options(bool shuffle, int repeat);
// The name Spotify's device list shows ("3DStereo" until it's changed in Settings); a change goes
// up with the next state, which is sent at once
void connect_set_name(const char *name);
// Asks the system's network manager (ndm:u) to keep the Wi-Fi as it is: an exclusive
// infrastructure state, locked. Every frame from the main thread with what's wanted; false before
// HOME and at exit. Whether the shut lid respects it is what build 46's log is for.
void connect_hold_wifi(bool on);
