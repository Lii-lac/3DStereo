// One face for every format Pocket plays: FLAC, MP3, WAV, Ogg Vorbis and Opus. Frames come
// out as interleaved float in the file's own channel layout (Opus is always stereo at 48k).
#pragma once
#include <3ds.h>

enum { FMT_NONE, FMT_FLAC, FMT_MP3, FMT_WAV, FMT_VORBIS, FMT_OPUS };

typedef struct Decoder Decoder;

typedef struct {
	int fmt;
	u32 rate, channels;
	u32 bits;      // 0 for lossy formats: what comes out was computed, never bit-exact
	u64 total;     // frames, 0 if unknown
} DecInfo;

int fmt_of(const char *path);          // by extension, FMT_NONE if we don't play it
const char *fmt_name(int fmt);
bool fmt_lossless(int fmt);

Decoder *dec_open(const char *path, DecInfo *info);
void dec_close(Decoder *d);
int dec_read(Decoder *d, float *out, int frames);   // 0 at the end
bool dec_seek(Decoder *d, u64 frame);
