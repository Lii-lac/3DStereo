#include "decoder.h"
#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include <opusfile.h>

struct Decoder {
	int fmt;
	u32 channels;
	union { drflac *flac; drmp3 mp3; drwav wav; stb_vorbis *vorbis; OggOpusFile *opus; } h;
	drmp3_seek_point *seek;   // MP3: built on the first seek, or every seek decodes from the top
	FILE *f;                  // ours, with a big buffer (Vorbis and Opus close theirs themselves)
};

// Every song is read into memory ahead of the decoder by a thread of its own (stream.c), so a song
// never waits on the card while something else is using it. Up to build 52 it was read straight
// off the card through a 128 KB buffer, and a busy card (a Spotify sync's writes) meant dropouts.
static FILE *big_open(const char *path) { return stream_open(path); }
static int whence(int o) { return o == 0 ? SEEK_SET : o == 1 ? SEEK_CUR : SEEK_END; }   // the dr_libs' SET, CUR, END
static size_t f_read(void *u, void *b, size_t n) { return fread(b, 1, n, (FILE *)u); }
static drflac_bool32 fl_seek(void *u, int off, drflac_seek_origin o) { return fseek((FILE *)u, off, whence(o)) == 0; }
static drflac_bool32 fl_tell(void *u, drflac_int64 *c) { long p = ftell((FILE *)u); if (p < 0) return DRFLAC_FALSE; *c = p; return DRFLAC_TRUE; }
static drmp3_bool32 mp_seek(void *u, int off, drmp3_seek_origin o) { return fseek((FILE *)u, off, whence(o)) == 0; }
static drmp3_bool32 mp_tell(void *u, drmp3_int64 *c) { long p = ftell((FILE *)u); if (p < 0) return DRMP3_FALSE; *c = p; return DRMP3_TRUE; }
static drwav_bool32 wv_seek(void *u, int off, drwav_seek_origin o) { return fseek((FILE *)u, off, whence(o)) == 0; }
static drwav_bool32 wv_tell(void *u, drwav_int64 *c) { long p = ftell((FILE *)u); if (p < 0) return DRWAV_FALSE; *c = p; return DRWAV_TRUE; }
static int op_read_cb(void *u, unsigned char *b, int n) { return n > 0 ? (int)fread(b, 1, n, (FILE *)u) : 0; }
static int op_seek_cb(void *u, opus_int64 off, int w) { return fseek((FILE *)u, (long)off, w); }
static opus_int64 op_tell_cb(void *u) { return ftell((FILE *)u); }
static int op_close_cb(void *u) { return fclose((FILE *)u); }

#define MP3_SEEK_POINTS 256

static const struct { const char *ext; int fmt; } EXT[] = {
	{".flac", FMT_FLAC}, {".mp3", FMT_MP3}, {".wav", FMT_WAV}, {".ogg", FMT_VORBIS}, {".oga", FMT_VORBIS}, {".opus", FMT_OPUS},
};

int fmt_of(const char *path) {
	const char *dot = strrchr(path, '.');
	if (!dot) return FMT_NONE;
	for (unsigned k = 0; k < sizeof EXT / sizeof *EXT; k++)
		if (!strcasecmp(dot, EXT[k].ext)) return EXT[k].fmt;
	return FMT_NONE;
}

const char *fmt_name(int fmt) {
	static const char *N[] = {"", "FLAC", "MP3", "WAV", "OGG", "OPUS"};
	return fmt >= 0 && fmt <= FMT_OPUS ? N[fmt] : "";
}

bool fmt_lossless(int fmt) { return fmt == FMT_FLAC || fmt == FMT_WAV; }

Decoder *dec_open(const char *path, DecInfo *info) {
	Decoder *d = calloc(1, sizeof *d);
	memset(info, 0, sizeof *info);
	d->fmt = info->fmt = fmt_of(path);
	bool ok = false;
	switch (d->fmt) {
	case FMT_FLAC:
		if ((d->f = big_open(path)) && (d->h.flac = drflac_open(f_read, fl_seek, fl_tell, d->f, NULL))) {
			info->rate = d->h.flac->sampleRate;
			info->channels = d->h.flac->channels;
			info->bits = d->h.flac->bitsPerSample;
			info->total = d->h.flac->totalPCMFrameCount;
			ok = true;
		}
		break;
	case FMT_MP3:
		// the Xing/Info header gives the length; without one it stays 0 and the library's
		// estimate stands in (counting would mean reading the whole file)
		if ((d->f = big_open(path)) && drmp3_init(&d->h.mp3, f_read, mp_seek, mp_tell, NULL, d->f, NULL)) {
			drmp3 *m = &d->h.mp3;
			info->rate = m->sampleRate;
			info->channels = m->channels;
			if (m->totalPCMFrameCount != DRMP3_UINT64_MAX && m->totalPCMFrameCount > m->delayInPCMFrames + m->paddingInPCMFrames)
				info->total = m->totalPCMFrameCount - m->delayInPCMFrames - m->paddingInPCMFrames;
			ok = true;
		}
		break;
	case FMT_WAV:
		if ((d->f = big_open(path)) && drwav_init(&d->h.wav, f_read, wv_seek, wv_tell, d->f, NULL)) {
			info->rate = d->h.wav.sampleRate;
			info->channels = d->h.wav.channels;
			info->bits = d->h.wav.bitsPerSample;
			info->total = d->h.wav.totalPCMFrameCount;
			ok = true;
		}
		break;
	case FMT_VORBIS: {
		int err;
		FILE *f = big_open(path);   // stb_vorbis closes it
		if (f && !(d->h.vorbis = stb_vorbis_open_file(f, 1, &err, NULL))) fclose(f);
		if (d->h.vorbis) {
			stb_vorbis_info vi = stb_vorbis_get_info(d->h.vorbis);
			info->rate = vi.sample_rate;
			info->channels = vi.channels;
			info->total = stb_vorbis_stream_length_in_samples(d->h.vorbis);
			ok = true;
		}
		break;
	}
	case FMT_OPUS: {
		int err;
		static const OpusFileCallbacks CB = {op_read_cb, op_seek_cb, op_tell_cb, op_close_cb};   // opusfile closes it
		FILE *f = big_open(path);
		if (f && !(d->h.opus = op_open_callbacks(f, &CB, NULL, 0, &err))) fclose(f);
		if (d->h.opus) {
			info->rate = 48000;
			info->channels = 2;   // op_read_float_stereo downmixes anything wider
			ogg_int64_t n = op_pcm_total(d->h.opus, -1);
			info->total = n > 0 ? (u64)n : 0;
			ok = true;
		}
		break;
	}
	}
	if (!ok || !info->rate || !info->channels) {
		if (ok) dec_close(d);
		else { if (d->f) fclose(d->f); free(d); }
		return NULL;
	}
	d->channels = info->channels;
	return d;
}

void dec_close(Decoder *d) {
	if (!d) return;
	switch (d->fmt) {
	case FMT_FLAC: drflac_close(d->h.flac); break;
	case FMT_MP3: drmp3_uninit(&d->h.mp3); free(d->seek); break;
	case FMT_WAV: drwav_uninit(&d->h.wav); break;
	case FMT_VORBIS: stb_vorbis_close(d->h.vorbis); break;
	case FMT_OPUS: op_free(d->h.opus); break;
	}
	if (d->f) fclose(d->f);   // FLAC, MP3, WAV: the file was ours to open, and is ours to close
	free(d);
}

int dec_read(Decoder *d, float *out, int frames) {
	switch (d->fmt) {
	case FMT_FLAC: return (int)drflac_read_pcm_frames_f32(d->h.flac, frames, out);
	case FMT_MP3: return (int)drmp3_read_pcm_frames_f32(&d->h.mp3, frames, out);
	case FMT_WAV: return (int)drwav_read_pcm_frames_f32(&d->h.wav, frames, out);
	case FMT_VORBIS: return stb_vorbis_get_samples_float_interleaved(d->h.vorbis, d->channels, out, frames * d->channels);
	case FMT_OPUS: {
		int done = 0;
		while (done < frames) {   // opusfile hands back one packet at a time
			int got = op_read_float_stereo(d->h.opus, out + 2 * done, 2 * (frames - done));
			if (got == OP_HOLE) continue;   // a damaged page: skip it, keep playing
			if (got <= 0) break;
			done += got;
		}
		return done;
	}
	}
	return 0;
}

bool dec_seek(Decoder *d, u64 frame) {
	switch (d->fmt) {
	case FMT_FLAC: return drflac_seek_to_pcm_frame(d->h.flac, frame);
	case FMT_MP3:
		if (!d->seek && (d->seek = malloc(MP3_SEEK_POINTS * sizeof *d->seek))) {
			// one pass over the frame headers (no decoding), then every seek is a short hop
			drmp3_uint32 n = MP3_SEEK_POINTS;
			if (drmp3_calculate_seek_points(&d->h.mp3, &n, d->seek)) drmp3_bind_seek_table(&d->h.mp3, n, d->seek);
		}
		return drmp3_seek_to_pcm_frame(&d->h.mp3, frame);
	case FMT_WAV: return drwav_seek_to_pcm_frame(&d->h.wav, frame);
	case FMT_VORBIS: return stb_vorbis_seek(d->h.vorbis, (unsigned)frame);
	case FMT_OPUS: return op_pcm_seek(d->h.opus, (ogg_int64_t)frame) == 0;
	}
	return false;
}
