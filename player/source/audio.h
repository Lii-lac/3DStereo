// Playback: one song at a time through ndsp, the next one joined on without a gap, and the I2S
// line at 47.6 kHz for Good and Best. A worker thread owns the decoders and the channel;
// everything here just posts to it.
#pragma once
#include <3ds.h>

enum { Q_STOCK, Q_GOOD, Q_BEST, Q_COUNT };
enum { RG_OFF, RG_TRACK, RG_ALBUM };   // ReplayGain: album gain falls back to track gain

typedef struct {
	int track;          // library index of what's heard, -1 before anything opened
	bool open, paused;
	int quality;
	int fmt;            // FMT_* from decoder.h
	u32 rate, bits;     // bits 0 for lossy formats
	double pos, len;    // seconds actually heard, and the song's length
	float gain;         // ReplayGain applied, dB
	u32 underruns;
} AudioInfo;

// Called by the worker when the decoder reaches the end of a song, a buffer or so before the
// speaker does, so the next one can follow without a gap: the next library index, or -1 to
// stop. *pause asks it to open that song paused (the end of the queue with repeat off).
typedef int (*NextFn)(bool *pause);

bool audio_init(NextFn next, int quality, int replaygain);   // false: ndsp wouldn't start (no dspfirm.cdc)
void audio_exit(void);
void audio_play(int track);
void audio_play_at(int track, double secs, bool paused);    // picking up where we left off
void audio_pause(bool paused);
void audio_hush(void);   // paused this instant, from any thread, not when the worker next looks
void audio_seek(double secs, bool relative);
void audio_quality(int q);
void audio_replaygain(int rg);
void audio_info(AudioInfo *out);
// The music's level under the 3DS's own slider, 0..1 (1: as loud as the slider says). Spotify's
// volume slider sets it while Pocket is Spotify's device (connect.c).
void audio_volume(float v);

// the swap's feel, since there's no rumble: tiny sounds on their own channel, under the music
enum { TICK_GRAB, TICK_ARM, TICK_LAND, TICK_COUNT };
void audio_tick(int kind);

// Card work that can wait (pictures, faces, the pass ahead of time) calls this first: while a song
// plays with under half its buffers queued, it sleeps until the song has caught up (max_ms at most),
// so the song's reads aren't stuck in line behind ours. Returns the ms it waited.
int audio_make_room(int max_ms);

// HOME: the DSP handed back to the system before we go, and taken again when we're back.
// waiting (or NULL) is called over and over while the sound lets go: a frame of the HOME spinner.
// After HOME Menu -> Close, skip audio_resume: audio_exit then leaves the DSP alone.
void audio_suspend(void (*waiting)(void));
void audio_resume(void);
