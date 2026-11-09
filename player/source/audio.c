// The engine from flactest, grown up: the clock switch from build 3, every format decoder.c
// knows, our windowed-sinc resampler for Best, and a worker thread that owns all of it. Song
// changes, seeks and pauses arrive as commands; the UI only ever reads the published AudioInfo.
// Songs at the same rate are joined inside the stream itself, so an album plays gapless.

#include "audio.h"
#include "crumb.h"
#include "decoder.h"
#include "library.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2S1_CNT ((vu16 *)0x1EC45000)   // 0x10145000 as the ARM11 kernel maps it
#define I2S_EN   0x8000
#define I2S_47K  0x2000
#define NDAC_47K 0x85                   // codec page 0 reg 11 for line 1 at 47.6k (0x87 at 32.7k)

static const double RATE[2] = {32728.498046875, 47605.088068181818};

// ---- clock -------------------------------------------------------------------------------

// svcBackdoor runs these in SVC mode on our stack; an IRQ in there kills the kernel, so
// interrupts stay off for the few instructions we need.
static volatile u16 k_val, k_read;
static s32 k_get(void) {
	u32 c;
	__asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(c) :: "memory");
	k_read = *I2S1_CNT;
	__asm__ volatile("msr cpsr_c, %0" :: "r"(c) : "memory");
	return 0;
}
static s32 k_set(void) {
	u32 c;
	__asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(c) :: "memory");
	*I2S1_CNT = *I2S1_CNT & ~I2S_EN;   // frequency bits only take while the line is off
	*I2S1_CNT = k_val;
	k_read = *I2S1_CNT;
	__asm__ volatile("msr cpsr_c, %0" :: "r"(c) : "memory");
	return 0;
}
static u16 i2s_get(void) { svcBackdoor(k_get); return k_read; }
static u16 i2s_set(u16 v) { k_val = v; svcBackdoor(k_set); return k_read; }

static bool have_cdc;
static u8 ndac_orig = 0x87;   // the stock divider, until we read the real one going fast
static u8 ndac_get(void) { u8 v = 0; if (have_cdc) CDCCHK_ReadRegisters2(0, 11, &v, 1); return v; }
static void ndac_set(u8 v) { if (have_cdc) CDCCHK_WriteRegisters2(0, 11, &v, 1); }

static LightLock clock_lock;   // the worker switches modes, HOME restores
static volatile bool rewake;   // the DSP woke up: the worker reads the clock again
static int clock_fast;
// Up to build 19 a wake-up forced a rewrite with the values read at launch, even in Stock:
// the line switched off and on under a running DSP, and HOME froze a few seconds later, every
// time. Now the registers are only touched when the speed really changes, and always from
// what they hold right now, so the system's own settings are never overwritten.
static void clock_read(void) { clock_fast = (i2s_get() & I2S_47K) != 0; }
static void clock_set(int fast) {
	LightLock_Lock(&clock_lock);
	u16 now = i2s_get();
	if (((now & I2S_47K) != 0) != (fast != 0)) {
		ndspSetMasterVol(0.0f);
		svcSleepThread(30000000);
		if (fast) { u8 n = ndac_get(); ndac_orig = n && n != NDAC_47K ? n : 0x87; }
		ndac_set(fast ? NDAC_47K : ndac_orig);
		i2s_set(fast ? (now | I2S_47K | I2S_EN) : ((now & ~I2S_47K) | I2S_EN));
		svcSleepThread(30000000);
		ndspSetMasterVol(1.0f);
		crumb_soon("clock: %s (i2s %04x)", fast ? "47.6k" : "stock", now);
	}
	clock_fast = fast;
	LightLock_Unlock(&clock_lock);
}

// ---- decode + resample -------------------------------------------------------------------

#define OUT_N  4096     // frames per wavebuf: ~86 ms at 47.6k
#define NBUF   64      // ~5.6 s queued (32, 2.8 s, until build 60, which saw it dip to 2 mid-song with no slow read): a slow moment on the card never reaches the speaker (build 38:
                       // 16 ran dry six times while a tap's pictures came off the card)
#define CHUNK  1024     // frames per decode call
#define MAX_CH 8
#define PHASES 512      // sinc table rows; we interpolate between neighbours
#define BETA   9.6      // Kaiser: ~-95 dB stopband

// What a stretch of output belongs to: every wavebuf carries one, so the screen shows the song
// coming out of the speaker, not the one the decoder has already moved on to.
typedef struct { int track, serial, fmt; u32 rate, bits; double len; float gain; } Meta;

static Decoder *dec;           // the song being decoded
static DecInfo di;
static Meta dm;
static float dgain = 1;        // its ReplayGain, linear
static float vol = 1;          // the music channel's level: Spotify's slider while it drives (audio_volume)
static Decoder *pend;          // the next song, opened at the end of this one
static DecInfo pdi;
static int pend_track = -1, serial;
static bool pend_pause, asked; // asked: next_fn already answered for this song
static int mode = -1, rg_mode;
static bool eof, drained;
static float tmp[CHUNK * MAX_CH], stage[CHUNK * 2];
static Meta hm;                // the song the next output sample belongs to
static double out_src;         // its source frame for that sample
static NextFn next_fn;

static float gain_db(int i) {
	if (rg_mode == RG_OFF || i < 0 || i >= lib.nt) return 0;
	Track *t = &lib.t[i];
	bool album = rg_mode == RG_ALBUM && !isnan(t->gain_a);
	float g = album ? t->gain_a : t->gain_t, peak = album ? t->peak_a : t->peak_t;
	if (isnan(g)) return 0;
	if (peak > 0 && g > -20 * log10f(peak)) g = -20 * log10f(peak);   // never push it into clipping
	return g;
}

static Meta meta_of(int i, const DecInfo *d) {
	Meta m = {.track = i, .serial = ++serial, .fmt = d->fmt, .rate = d->rate, .bits = d->bits, .gain = gain_db(i)};
	m.len = d->total ? (double)d->total / d->rate : lib.t[i].secs;
	return m;
}

static void take(Decoder *x, const DecInfo *d, int i) {
	dec_close(dec);
	dec = x;
	di = *d;
	dm = meta_of(i, d);
	dgain = powf(10, dm.gain / 20);
}

// Song i, or failing that whatever next_fn offers after it, as many times as there are songs.
static Decoder *open_dec(int *i, bool *pause, DecInfo *d) {
	for (int tries = 0; tries <= lib.nt && *i >= 0; tries++) {
		if (*i < lib.nt) {
			Decoder *x = dec_open(lib.t[*i].path, d);
			if (x && d->channels <= MAX_CH) return x;
			dec_close(x);
		}
		*i = next_fn(pause);
	}
	*i = -1;
	return NULL;
}

// ---- the next song, opened ahead ------------------------------------------------------------
// Up to build 58 the next song was opened the moment the decoder ran dry, inside the worker: its
// file, its stream and its headers (a FLAC with a big cover reads past the picture), 290 ms every
// time in build 53's log, all of it with the speaker living off the queue. Now the worker asks
// six seconds before the end, and the opener (a thread of its own, on the audio's core below the
// worker) opens it; at the boundary it's already there. If it isn't yet, the worker waits for it,
// as it always used to, and a song that won't open falls back to the old way.
static Thread op_th;
static LightLock op_lk;
static LightEvent op_go, op_fin;
static volatile bool op_run = true;
static int op_want = -1;                 // (op_lk) the song asked for
static u32 op_req, op_done;              // (op_lk) asks made, and answered: equal when it's idle
static Decoder *op_dec;                  // (op_lk) what the newest ask got
static DecInfo op_di;
static bool op_mine;                     // worker: the pending next is the opener's to hand over

static void opener(void *arg) {
	(void)arg;
	while (op_run) {
		LightLock_Lock(&op_lk);
		u32 g = op_req;
		int w = op_want;
		bool todo = op_done != op_req;
		LightLock_Unlock(&op_lk);
		if (!todo) { LightEvent_Wait(&op_go); continue; }
		DecInfo d = {0};
		Decoder *x = w >= 0 && w < lib.nt ? dec_open(lib.t[w].path, &d) : NULL;
		if (x && d.channels > MAX_CH) { dec_close(x); x = NULL; }
		LightLock_Lock(&op_lk);
		if (g == op_req) { Decoder *old = op_dec; op_dec = x; op_di = d; x = old; }   // the one still wanted
		op_done = g;
		LightLock_Unlock(&op_lk);
		dec_close(x);   // asked for something else meanwhile, or the old answer nobody took
		LightEvent_Signal(&op_fin);
	}
}
static void op_ask(int i) {   // worker
	LightLock_Lock(&op_lk);
	op_want = i;
	op_req++;
	Decoder *x = op_dec;
	op_dec = NULL;
	LightLock_Unlock(&op_lk);
	dec_close(x);
	op_mine = true;
	LightEvent_Signal(&op_go);
}
static void op_drop(void) {   // worker: whatever it's opening or has opened isn't wanted now
	if (!op_mine) return;
	LightLock_Lock(&op_lk);
	op_want = -1;
	op_req++;
	Decoder *x = op_dec;
	op_dec = NULL;
	LightLock_Unlock(&op_lk);
	dec_close(x);
	op_mine = false;
	LightEvent_Signal(&op_go);
}
static Decoder *op_take(DecInfo *d) {   // worker: the answer, waiting for it if it's not in yet
	for (;;) {
		LightLock_Lock(&op_lk);
		bool done = op_done == op_req;
		Decoder *x = done ? op_dec : NULL;
		if (done) { op_dec = NULL; *d = op_di; }
		LightLock_Unlock(&op_lk);
		if (done) { op_mine = false; return x; }
		LightEvent_Wait(&op_fin);
	}
}

static void drop_pending(void) { op_drop(); dec_close(pend); pend = NULL; asked = false; }

// The decoder ran dry. Ask for the next song once; if it can be joined straight on (same rate,
// not meant to start paused) it takes over mid-stream and nothing downstream notices.
static bool join_next(void) {
	if (!asked) {   // nobody asked ahead (a short song, a seek near the end): the old way, here
		asked = true;
		bool p = false;
		int i = next_fn(&p);
		pend = i >= 0 ? open_dec(&i, &p, &pdi) : NULL;
		pend_track = i;
		pend_pause = p;
	} else if (op_mine) {   // asked six seconds ago: the opener's answer
		DecInfo d;
		Decoder *x = op_take(&d);
		if (x) { pend = x; pdi = d; }
		else if (pend_track >= 0) {   // that one wouldn't open: whatever comes after it, the old way
			int i = next_fn(&pend_pause);
			pend = i >= 0 ? open_dec(&i, &pend_pause, &pdi) : NULL;
			pend_track = i;
		}
	}
	if (!pend || pend_pause || pdi.rate != di.rate) return false;
	take(pend, &pdi, pend_track);
	pend = NULL;
	asked = false;
	return true;
}

// Every channel layout becomes stereo float at the song's gain: mono is doubled, extra
// channels are dropped.
// What the worker's been doing, for the log when the queue runs low (build 60: build 59 dipped to
// 2 of 32 three times with no slow refill, so: too little CPU, or not woken, or held somewhere?)
static u64 st_dec, st_fill, st_gap, st_last;   // ticks: decoding, the whole refill, the longest gap between two goes
static int st_bufs, st_wakes;
static u64 st_t0;
#define TMS(t) ((t) / 268111.856)   // ticks to ms (SYSCLOCK_ARM11)
static int read_stereo_(float *dst, int n);
static int read_stereo(float *dst, int n) {
	u64 t0 = svcGetSystemTick();
	int r = read_stereo_(dst, n);
	st_dec += svcGetSystemTick() - t0;
	return r;
}
static int read_stereo_(float *dst, int n) {
	int ch = di.channels, got;
	if (ch == 2) got = dec_read(dec, dst, n);
	else {
		got = dec_read(dec, tmp, n);
		for (int i = 0; i < got; i++) { dst[2 * i] = tmp[i * ch]; dst[2 * i + 1] = tmp[i * ch + (ch > 1)]; }
	}
	if (dgain != 1) for (int i = 0; i < 2 * got; i++) dst[i] *= dgain;
	return got;
}

// 16-bit sources in the raw modes go out bit-exact; anything we computed gets TPDF dither.
static u32 rng = 22222;
static inline float tpdf(void) {
	rng = rng * 1664525u + 1013904223u; float a = (rng >> 8) * (1.0f / 16777216);
	rng = rng * 1664525u + 1013904223u; float b = (rng >> 8) * (1.0f / 16777216);
	return a - b;
}
static inline s16 to16(float x, bool dither) {
	float v = x * 32768.0f + (dither ? tpdf() : 0.0f);
	if (v > 32767.0f) v = 32767.0f;
	if (v < -32768.0f) v = -32768.0f;
	return (s16)(v < 0 ? v - 0.5f : v + 0.5f);
}

// Polyphase windowed sinc. Output frame at input position t (n0 = floor t, p = t - n0) is
// sum over k of x[n0 - half + 1 + k] * h(p + half - 1 - k). Cutoff sits at the narrower
// Nyquist, so 44.1k upsampling keeps everything to ~20 kHz and kills the images that would
// otherwise fold back into the audible band at 47.6k.
static struct { u32 in_rate; int half, taps; double step, t; float *coef; } rs;
static float *ibuf;          // stereo float input, ilen frames valid
static int ilen, icap;
static bool padded;
static bool joined;          // the next song starts at ibuf frame `boundary`
static double boundary;

static double bessel_i0(double x) {
	double s = 1, t = 1;
	for (int k = 1; k < 50; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
	return s;
}

static void rs_build(u32 in_rate) {
	if (rs.coef && rs.in_rate == in_rate) return;
	double sc = RATE[1] / in_rate;
	if (sc > 1) sc = 1;
	free(rs.coef);
	free(ibuf);
	rs.in_rate = in_rate;
	rs.half = (int)ceil(32 / sc);
	rs.taps = 2 * rs.half;
	rs.step = in_rate / RATE[1];
	rs.coef = malloc((PHASES + 1) * rs.taps * sizeof(float));
	double i0b = bessel_i0(BETA);
	for (int ph = 0; ph <= PHASES; ph++)
		for (int k = 0; k < rs.taps; k++) {
			double x = (double)ph / PHASES + rs.half - 1 - k, w = x / rs.half, s = sc * x;
			double win = fabs(w) >= 1 ? 0 : bessel_i0(BETA * sqrt(1 - w * w)) / i0b;
			double snc = fabs(s) < 1e-9 ? 1 : sin(M_PI * s) / (M_PI * s);
			rs.coef[ph * rs.taps + k] = (float)(sc * snc * win);
		}
	icap = (int)(OUT_N * rs.step) + 2 * rs.taps + 2 * CHUNK;
	ibuf = malloc(icap * 2 * sizeof(float));
}

static void rs_reset(void) {
	joined = false;
	if (!ibuf) return;
	ilen = rs.half - 1;   // zeros before the first real frame, so it can be the first output
	memset(ibuf, 0, ilen * 2 * sizeof(float));
	rs.t = ilen;
	padded = false;
}

static int fill_ours(s16 *out, int n) {
	int need = (int)(rs.t + n * rs.step) + rs.half + 2;
	if (need > icap - rs.taps) need = icap - rs.taps;
	while (ilen < need && !eof) {
		int want = need - ilen < CHUNK ? need - ilen : CHUNK;
		int got = read_stereo(ibuf + 2 * ilen, want);
		if (got > 0) ilen += got;
		else if (joined) break;   // a song shorter than a buffer: let the last join pass first
		else if (join_next()) { joined = true; boundary = ilen; }
		else eof = true;
	}
	if (eof && !padded) {   // trailing zeros so the last real frames come out
		memset(ibuf + 2 * ilen, 0, rs.half * 2 * sizeof(float));
		ilen += rs.half;
		padded = true;
	}

	const int taps = rs.taps, half = rs.half;
	int j = 0;
	for (; j < n; j++) {
		if (joined && rs.t >= boundary) break;   // the next song gets a buffer of its own
		int n0 = (int)rs.t;
		if (n0 + half >= ilen) break;
		float fp = (float)(rs.t - n0) * PHASES;
		int ph = (int)fp;
		float a = fp - ph;
		const float *c0 = rs.coef + ph * taps, *c1 = c0 + taps, *x = ibuf + 2 * (n0 - half + 1);
		float l = 0, r = 0;
		for (int k = 0; k < taps; k++) {
			float c = c0[k] + a * (c1[k] - c0[k]);
			l += c * x[2 * k];
			r += c * x[2 * k + 1];
		}
		out[2 * j] = to16(l, true);
		out[2 * j + 1] = to16(r, true);
		rs.t += rs.step;
	}
	int drop = (int)rs.t - half + 1;   // input no future output can reach
	if (drop > 0) {
		memmove(ibuf, ibuf + 2 * drop, (ilen - drop) * 2 * sizeof(float));
		ilen -= drop;
		rs.t -= drop;
		boundary -= drop;
	}
	return j;
}

// ---- ndsp --------------------------------------------------------------------------------

static ndspWaveBuf wb[NBUF];
static double wb_src[NBUF];   // out_src at the start of each buffer, for the play position
static Meta wm[NBUF];
static s16 *pcm;
static int next;
static bool primed, paused, cut;
static u32 underruns;
static LightEvent frame_ev;
static void on_frame(void *d) { (void)d; LightEvent_Signal(&frame_ev); }
static bool idle(ndspWaveBuf *b) { return b->status == NDSP_WBUF_FREE || b->status == NDSP_WBUF_DONE; }
static double ratio(void) { return mode == Q_BEST ? rs.step : 1.0; }   // source frames per output sample

// In the raw modes a join ends the buffer in hand; the next one starts the new song.
static int fill_raw(s16 *out, int n, int k) {
	int done = 0;
	while (done < n && !eof) {
		int got = read_stereo(stage, n - done < CHUNK ? n - done : CHUNK);
		if (got <= 0) {
			if (!join_next()) { eof = true; break; }
			if (done) { cut = true; break; }
			hm = dm;
			out_src = wb_src[k] = 0;
			wm[k] = hm;
			continue;
		}
		bool dither = hm.bits != 16 || dgain != 1;
		for (int i = 0; i < 2 * got; i++) out[2 * done + i] = to16(stage[i], dither);
		done += got;
	}
	return done;
}

static int playing(void) {
	if (!primed) return -1;
	int seq = ndspChnGetWaveBufSeq(0);
	for (int k = 0; k < NBUF; k++)
		if (!idle(&wb[k]) && wb[k].sequence_id == seq) return k;
	return -1;
}

// Seconds into the song being heard.
static double heard(void) {
	int k = playing();
	if (k >= 0) return (wb_src[k] + ndspChnGetSamplePos(0) * ratio()) / wm[k].rate;
	return dec ? out_src / di.rate : 0;
}

static void chan_setup(void) {
	if (!dec) return;
	ndspChnSetInterp(0, mode == Q_BEST ? NDSP_INTERP_NONE : NDSP_INTERP_POLYPHASE);
	// ndsp computes its step as rate / NDSP_SAMPLE_RATE, assuming 32.7k out
	double r = mode == Q_BEST ? NDSP_SAMPLE_RATE
	         : mode == Q_GOOD ? di.rate * NDSP_SAMPLE_RATE / RATE[1]
	         : di.rate;
	ndspChnSetRate(0, (float)r);
}

static bool was_full;   // full once since the last flush: a low queue before that is just filling up
static void flush(double at) {
	was_full = false;
	ndspChnWaveBufClear(0);
	for (int k = 0; k < NBUF; k++) wb[k].status = NDSP_WBUF_FREE;
	next = 0;
	primed = false;
	drained = false;
	cut = false;
	rs_reset();
	out_src = at;
	hm = dm;
}

static void seek_to(double secs) {
	double len = dm.len > 0 ? dm.len : 1;
	if (secs > len - 0.05) secs = len - 0.05;
	if (secs < 0) secs = 0;
	u64 to = (u64)(secs * di.rate);
	if (!dec_seek(dec, to)) { dec_seek(dec, 0); to = 0; }
	eof = false;
	flush(to);
}

static void start(Decoder *x, const DecInfo *d, int i) {
	take(x, d, i);
	rs_build(di.rate);
	eof = false;
	flush(0);
	chan_setup();
}

static void stop(void) {
	dec_close(dec);
	dec = NULL;
	dm = (Meta){.track = -1};
	flush(0);
}

// The fast clock is only ever on while music is actually playing. Paused, or about to leave for
// HOME, the stock one goes back on here, on the worker's live path, never inside a system hook:
// so whatever the system does next (HOME, sleep, an applet) finds the console as it left it.
static LightLock work_lock;    // held by the worker while it works, and by HOME while the DSP is away
static bool dsp_gone;          // ndsp wouldn't come back after HOME: the worker just idles
static void clock_apply(bool p) {
	int want = mode != Q_STOCK && !p;
	if (want != clock_fast) clock_set(want);
}
static void set_paused(bool p) {
	if (!p) clock_apply(false);   // the right clock on before a single sample plays
	paused = p;
	ndspChnSetPaused(0, p);
	if (p) clock_apply(true);
}

// The decoder runs up to a buffer ahead of the speaker, so right after a join it's already in
// the next song. A seek or a mode change means the song being heard, so step back to it; the
// one we ran ahead to waits as the pending next, and next_fn isn't asked twice.
static void unswap(void) {
	int k = playing();
	if (k < 0 || wm[k].serial == dm.serial || wm[k].track < 0) return;
	DecInfo d;
	Decoder *x = dec_open(lib.t[wm[k].track].path, &d);
	if (!x) return;   // gone from the card: the seek lands in the new song instead
	op_drop();
	dec_close(pend);
	pend = dec; pdi = di; pend_track = dm.track; pend_pause = false; asked = true;
	dec = x;
	di = d;
	dm = wm[k];
	dgain = powf(10, dm.gain / 20);
}

static void play(int i, double secs, bool pause) {
	drop_pending();
	DecInfo d;
	Decoder *x = open_dec(&i, &pause, &d);
	if (!x) { stop(); set_paused(true); return; }
	start(x, &d, i);
	if (secs > 0) seek_to(secs);
	set_paused(pause);
}

static void set_mode(int m) {
	double at = heard();   // keep the place: what's actually being heard
	unswap();
	mode = m;
	clock_apply(paused);
	if (dec) seek_to(at);
	else flush(0);
	chan_setup();
}

static void set_rg(int r) {
	rg_mode = r;
	if (!dec) return;
	dm.gain = gain_db(dm.track);
	dgain = powf(10, dm.gain / 20);
	if (hm.serial == dm.serial) hm.gain = dm.gain;
}

static bool fill(int k) {
	ndspWaveBuf *b = &wb[k];
	if (joined && rs.t >= boundary) { out_src = rs.t - boundary; joined = false; hm = dm; }
	wm[k] = hm;
	wb_src[k] = out_src;
	int got = mode == Q_BEST ? fill_ours(b->data_pcm16, OUT_N) : fill_raw(b->data_pcm16, OUT_N, k);
	if (got <= 0) { drained = eof; return false; }
	out_src += got * ratio();
	if (cut) { cut = false; hm = dm; out_src = 0; }
	b->nsamples = got;
	DSP_FlushDataCache(b->data_pcm16, got * 4);
	ndspChnWaveBufAdd(0, b);
	return true;
}

// ---- worker ------------------------------------------------------------------------------

static volatile bool run = true;
static volatile int queued = -1;   // buffers queued while a song plays (worker), -1 when nothing is
static LightLock cmd_lock;
static struct { int play, pause, quality, rg; bool seek, rel, play_paused; double secs, play_at; } cmd = {-1, -1, Q_BEST, RG_OFF};

static AudioInfo info;
static LightLock info_lock;

static void publish(void) {
	AudioInfo s = {.track = -1, .open = dec != NULL, .paused = paused, .quality = mode, .underruns = underruns};
	if (dec) {
		int k = playing();
		const Meta *m = k >= 0 ? &wm[k] : &hm;
		s.track = m->track;
		s.fmt = m->fmt;
		s.rate = m->rate;
		s.bits = m->bits;
		s.len = m->len;
		s.gain = m->gain;
		s.pos = heard();
		if (s.pos > s.len) s.pos = s.len;
	}
	LightLock_Lock(&info_lock);
	info = s;
	LightLock_Unlock(&info_lock);
}

static void worker(void *arg) {
	(void)arg;
	while (run) {
		LightLock_Lock(&work_lock);
		if (!run) { LightLock_Unlock(&work_lock); break; }   // closed while parked (HOME Menu -> Close)
		if (dsp_gone) { LightLock_Unlock(&work_lock); LightEvent_Wait(&frame_ev); continue; }
		LightLock_Lock(&cmd_lock);
		int play_i = cmd.play, pause = cmd.pause, q = cmd.quality, rg = cmd.rg;
		bool seek = cmd.seek, rel = cmd.rel, play_paused = cmd.play_paused;
		double secs = cmd.secs, play_at = cmd.play_at;
		cmd.play = cmd.pause = -1;
		cmd.seek = cmd.play_paused = false;
		cmd.play_at = 0;
		LightLock_Unlock(&cmd_lock);

		if (rewake) { rewake = false; clock_read(); }   // whatever it woke at, from the hardware
		if (q != mode) set_mode(q);
		if (rg != rg_mode) set_rg(rg);
		if (play_i >= 0) play(play_i, play_at, play_paused);
		if (seek && dec) {
			double now = heard();
			unswap();
			seek_to(rel ? now + secs : secs);
		}
		if (pause >= 0 && pause != paused) set_paused(pause);

		if (dec) {
			int n = 0;
			for (int k = 0; k < NBUF; k++) n += idle(&wb[k]);
			if (n == NBUF && drained) {   // song over, and the next couldn't be joined on
				if (pend) {
					Decoder *x = pend;
					int i = pend_track;
					bool p = pend_pause;
					pend = NULL;
					asked = false;
					start(x, &pdi, i);
					set_paused(p);
				} else { asked = false; set_paused(true); seek_to(0); }
			} else if (n == NBUF && primed && !paused) {   // ran dry: in the log, once a second at most
				static u64 said;
				underruns++;
				if (osGetTime() - said > 1000) { said = osGetTime(); crumb_soon("audio: dropout %lu (track %d)", (unsigned long)underruns, dm.track); }
			} else if (NBUF - n <= 8 && primed && !paused && !drained && was_full) {   // close to it: what lag sounds like, before a dropout (build 36: Alex heard lag, the log had none)
				static u64 said;
				if (osGetTime() - said > 2000) {
					said = osGetTime();
					crumb_soon("audio: running low, %d of %d queued (track %d); since %.0f ms: %d bufs filled, refill %.0f ms (decoding %.0f), %d wakes, longest gap %.0f ms",
					           NBUF - n, NBUF, dm.track, TMS(svcGetSystemTick() - st_t0), st_bufs, TMS(st_fill), TMS(st_dec), st_wakes, TMS(st_gap));
				}
			}
			// two at a time, so a refill of all sixteen never keeps a skip or a pause waiting
			u64 tf = osGetTime(), tk = svcGetSystemTick();
			if (st_last && tk - st_last > st_gap) st_gap = tk - st_last;
			st_last = tk;
			st_wakes++;
			if (TMS(tk - st_t0) > 3000) { st_t0 = tk; st_dec = st_fill = st_gap = 0; st_bufs = st_wakes = 0; }   // a window of about three seconds
			for (int f = 0; f < 2 && dec && idle(&wb[next]) && fill(next); f++) { next = (next + 1) % NBUF; primed = true; st_bufs++; }
			st_fill += svcGetSystemTick() - tk;
			if (osGetTime() - tf > 150) {   // a refill that long is the card (or the decoder) holding the song up
				static u64 said;
				if (osGetTime() - said > 2000) { said = osGetTime(); crumb_soon("audio: a refill took %lld ms", (long long)(osGetTime() - tf)); }
			}
			if (dec && !asked && primed && !paused && !drained && dm.len > 6 && out_src / di.rate > dm.len - 6) {
				asked = true;   // the next song, asked for now and opened by the opener
				bool p = false;
				int i = next_fn(&p);
				pend = NULL;
				pend_track = i;
				pend_pause = p;
				if (i >= 0) op_ask(i);
			}
			int q = 0;
			for (int k = 0; k < NBUF; k++) q += !idle(&wb[k]);
			if (q >= NBUF - 2) was_full = true;   // the queue's been full since the last start or seek
			queued = dec && primed && !paused && !drained ? q : -1;
		} else queued = -1;
		clock_apply(paused);   // the wake-up above, or the way back from HOME
		publish();
		LightLock_Unlock(&work_lock);
		LightEvent_Wait(&frame_ev);
	}
}

// ---- HOME, applets and sleep expect the stock clock, or their sounds play 1.45x high -------

// Build 6 switched it back from an APT hook and build 7 from the DSP's sleep hook, and both
// froze the console on HOME: touching the codec or I2S while the system is putting the DSP
// to sleep is the one thing it won't survive. Now nothing is changed in a hook at all. HOME
// is refused, and main puts the stock clock back on the live path before going HOME itself;
// sleep is only allowed while paused, which is already stock. The hook just notes the wake.
static dspHookCookie dsp_hook;
static void on_dsp(DSP_HookType t) {
	if (t != DSPHOOK_ONSLEEP) rewake = true;   // ONWAKEUP, ONCANCEL: ndsp is running again
}

// ---- ticks -------------------------------------------------------------------------------

// Made once at start: a pick-up tick, a crisper click for the detent, a low thunk for landing.
// Channel 1, mixed under whatever's playing; at the fast clock its rate is scaled like ours.
#define TICK_HZ 32000
static s16 *tick_pcm;
static ndspWaveBuf tick_wb[TICK_COUNT];
static void tick_chan(void);
static void tick_make(void) {
	static const struct { float f, f2, decay, amp, ms; } T[TICK_COUNT] = {
		{2200, 0, 900, 0.10f, 12},     // grab: barely there
		{3100, 1550, 700, 0.16f, 16},  // arm: a dry click with a little body
		{150, 2400, 60, 0.30f, 70},    // land: a soft thump with a click on top
	};
	int n[TICK_COUNT], tot = 0;
	for (int k = 0; k < TICK_COUNT; k++) tot += n[k] = TICK_HZ * T[k].ms / 1000;
	tick_pcm = linearAlloc(tot * 2);
	if (!tick_pcm) return;
	s16 *p = tick_pcm;
	for (int k = 0; k < TICK_COUNT; k++) {
		for (int i = 0; i < n[k]; i++) {
			float t = (float)i / TICK_HZ, e = expf(-t * T[k].decay), v = sinf(2 * M_PI * T[k].f * t) * e;
			if (T[k].f2) v += sinf(2 * M_PI * T[k].f2 * t) * expf(-t * 1400) * (k == TICK_LAND ? 0.5f : 0.6f);
			if (i < 16) v *= i / 16.0f;   // no click of its own at the start
			v *= T[k].amp;
			p[i] = (s16)(v > 1 ? 32767 : v < -1 ? -32767 : v * 32767);
		}
		tick_wb[k].data_vaddr = p;
		tick_wb[k].nsamples = n[k];
		p += n[k];
	}
	DSP_FlushDataCache(tick_pcm, tot * 2);
	ndspChnReset(1);
	tick_chan();
}

static void tick_chan(void) {
	ndspChnSetFormat(1, NDSP_FORMAT_MONO_PCM16);
	ndspChnSetInterp(1, NDSP_INTERP_LINEAR);
	float mix[12] = {0.8f, 0.8f};
	ndspChnSetMix(1, mix);
}

void audio_tick(int kind) {
	if (!tick_pcm || dsp_gone || kind < 0 || kind >= TICK_COUNT) return;
	if (dec && !paused) return;   // never over the music
	ndspChnWaveBufClear(1);
	ndspChnSetRate(1, clock_fast ? (float)(TICK_HZ * NDSP_SAMPLE_RATE / RATE[1]) : TICK_HZ);
	for (int k = 0; k < TICK_COUNT; k++) tick_wb[k].status = NDSP_WBUF_FREE;
	ndspChnWaveBufAdd(1, &tick_wb[kind]);
}

// ---- API ---------------------------------------------------------------------------------


static Thread th;

bool audio_init(NextFn next, int quality, int replaygain) {
	LightLock_Init(&clock_lock);
	LightLock_Init(&work_lock);
	dspHook(&dsp_hook, on_dsp);   // before ndspInit, so ndsp's own hook runs ahead of ours
	if (R_FAILED(ndspInit())) { dspUnhook(&dsp_hook); return false; }
	// the stock values only mean anything once ndsp has the DSP and the codec running
	have_cdc = R_SUCCEEDED(cdcChkInit());
	clock_read();   // normally stock; if a crashed run left it fast, the worker puts it back
	next_fn = next;
	cmd.quality = quality;
	cmd.rg = replaygain;
	LightLock_Init(&cmd_lock);
	LightLock_Init(&info_lock);
	LightEvent_Init(&frame_ev, RESET_ONESHOT);

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetCallback(on_frame, NULL);
	ndspChnReset(0);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {vol, vol};
	ndspChnSetMix(0, mix);
	pcm = linearAlloc(OUT_N * 4 * NBUF);
	memset(wb, 0, sizeof wb);
	for (int k = 0; k < NBUF; k++) wb[k].data_vaddr = pcm + k * OUT_N * 2;
	info.track = -1;
	paused = true;
	tick_make();

	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	LightLock_Init(&op_lk);
	LightEvent_Init(&op_go, RESET_ONESHOT);
	LightEvent_Init(&op_fin, RESET_ONESHOT);
	op_th = threadCreate(opener, NULL, 64 * 1024, prio + 1, 2, false);   // the audio's core, under the worker
	if (!op_th) op_th = threadCreate(opener, NULL, 64 * 1024, prio + 1, -2, false);
	th = threadCreate(worker, NULL, 64 * 1024, prio - 1, 2, false);   // the New 3DS's spare core
	bool core2 = th != NULL;
	if (!th) th = threadCreate(worker, NULL, 64 * 1024, prio - 1, -2, false);
	crumb("audio: worker on %s", core2 ? "core 2" : th ? "core 0 (no core 2)" : "no thread");
	return true;
}

static bool parked;   // audio_suspend has the worker held and the DSP let go
static void op_exit(void) {
	op_run = false;
	LightEvent_Signal(&op_go);
	if (op_th) { threadJoin(op_th, U64_MAX); threadFree(op_th); op_th = NULL; }
	dec_close(op_dec);
	op_dec = NULL;
}
void audio_exit(void) {
	run = false;
	if (parked) {
		// closed from the HOME Menu while we were away: the system has the DSP, and touching it,
		// the codec or the I2S clock now froze the console (build 36). Let the worker go, free ours.
		LightLock_Unlock(&work_lock);
		LightEvent_Signal(&frame_ev);
		if (th) { threadJoin(th, U64_MAX); threadFree(th); }
		op_exit();
		dspUnhook(&dsp_hook);
		dec_close(dec);
		dec_close(pend);
		linearFree(pcm);
		if (tick_pcm) linearFree(tick_pcm);
		crumb("audio: closed while away, the DSP left alone");
		return;
	}
	LightEvent_Signal(&frame_ev);
	if (th) { threadJoin(th, U64_MAX); threadFree(th); }
	op_exit();
	ndspChnWaveBufClear(0);
	ndspChnWaveBufClear(1);
	if (!dsp_gone) {
		if (clock_fast) clock_set(0);
		ndspExit();
	}
	dspUnhook(&dsp_hook);
	if (have_cdc) cdcChkExit();
	dec_close(dec);
	dec_close(pend);
	linearFree(pcm);
	if (tick_pcm) linearFree(tick_pcm);
}

static void post(void) { LightEvent_Signal(&frame_ev); }
// HOME: build 10 put the stock clock back first and still froze, so the DSP goes away
// entirely: no ndsp, no codec service, no clock of ours, nothing for the system to trip on.
// The decoder keeps its place, and the song picks up there when we're back.
static double away_at;
// The letting go itself (~0.4 s: ndspExit alone is ~150 ms, and every crumb is a write to the card).
// Run on its own thread, on the worker's core while the worker is parked, so the main thread is free
// to keep the spinner turning the whole time (build 37 drew it once and then sat in here).
static volatile bool let_done;
static void let_go(void *arg) {
	(void)arg;
	unswap();
	away_at = heard();
	ndspChnWaveBufClear(0);
	ndspChnWaveBufClear(1);
	if (clock_fast) { clock_set(0); crumb("suspend: clock stock"); }
	ndspExit();
	crumb("suspend: ndsp off");
	if (have_cdc) cdcChkExit();
	have_cdc = false;
	crumb("suspend: codec off");
	let_done = true;
}

void audio_suspend(void (*waiting)(void)) {
	crumb("suspend: track %d paused %d mode %d clock %d", dm.track, paused, mode, clock_fast);
	// the worker is parked until audio_resume. Tried, not waited on: LightLock isn't fair, and a
	// busy worker kept winning it for 31 s in build 36. Between tries, waiting() draws a frame of
	// the HOME spinner (a frame's wait is room for the worker to let go); after a second, wait.
	u64 t0 = osGetTime();
	int tries = 0;
	while (LightLock_TryLock(&work_lock)) {
		if (osGetTime() - t0 > 1000) { LightLock_Lock(&work_lock); break; }
		if (waiting) waiting(); else svcSleepThread(2000000LL);
		tries++;
	}
	parked = true;
	queued = -1;   // nothing plays now: card work needn't wait for us
	crumb("suspend: worker parked after %llu ms, %d tries", osGetTime() - t0, tries);
	if (dsp_gone) return;
	let_done = false;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	Thread t = waiting ? threadCreate(let_go, NULL, 16 * 1024, prio - 1, 2, false) : NULL;
	if (!t && waiting) t = threadCreate(let_go, NULL, 16 * 1024, prio + 1, -2, false);
	if (!t) { let_go(NULL); return; }
	int frames = 0;
	while (!let_done) { waiting(); frames++; }   // each frame waits for the screen, so this isn't a spin
	threadJoin(t, U64_MAX);
	threadFree(t);
	crumb("suspend: let go after %llu ms, %d spinner frames", osGetTime() - t0, frames);
}

int audio_make_room(int max_ms) {
	int waited = 0, q = queued;
	if (q < 0 || q >= NBUF / 2) return 0;
	while (run && waited < max_ms) {   // once it's waiting, until the song is well ahead again
		q = queued;
		if (q < 0 || q >= NBUF * 3 / 4) break;
		svcSleepThread(20 * 1000000LL);
		waited += 20;
	}
	return waited;
}

void audio_resume(void) {
	crumb("resume: back");
	parked = false;
	if (R_FAILED(ndspInit())) { crumb("resume: ndsp failed"); dsp_gone = true; LightLock_Unlock(&work_lock); return; }
	crumb("resume: ndsp on");
	dsp_gone = false;
	have_cdc = R_SUCCEEDED(cdcChkInit());
	clock_read();   // whatever it came back at; the worker sets ours from there
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetCallback(on_frame, NULL);
	ndspChnReset(0);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {vol, vol};
	ndspChnSetMix(0, mix);
	ndspChnReset(1);
	tick_chan();
	for (int k = 0; k < TICK_COUNT; k++) tick_wb[k].status = NDSP_WBUF_FREE;
	chan_setup();
	if (dec) seek_to(away_at);
	else flush(0);
	ndspChnSetPaused(0, paused);
	LightLock_Unlock(&work_lock);
	post();
	crumb("resume: done at %.1f", away_at);
}

void audio_play(int track) { audio_play_at(track, 0, false); }
void audio_play_at(int track, double secs, bool paused) {
	LightLock_Lock(&cmd_lock);
	cmd.play = track;
	cmd.play_at = secs;
	cmd.play_paused = paused;
	cmd.seek = false;
	LightLock_Unlock(&cmd_lock);
	post();
}
void audio_pause(bool p) { LightLock_Lock(&cmd_lock); cmd.pause = p; LightLock_Unlock(&cmd_lock); post(); }
void audio_hush(void) { ndspChnSetPaused(0, true); audio_pause(true); }   // the worker catches up and agrees
void audio_quality(int q) { LightLock_Lock(&cmd_lock); cmd.quality = q; LightLock_Unlock(&cmd_lock); post(); }
void audio_replaygain(int rg) { LightLock_Lock(&cmd_lock); cmd.rg = rg; LightLock_Unlock(&cmd_lock); post(); }
void audio_seek(double secs, bool rel) {
	LightLock_Lock(&cmd_lock);
	if (cmd.seek && rel && cmd.rel) cmd.secs += secs;   // two presses before the worker woke
	else { cmd.secs = secs; cmd.rel = rel; }
	cmd.seek = true;
	LightLock_Unlock(&cmd_lock);
	post();
}
void audio_info(AudioInfo *out) { LightLock_Lock(&info_lock); *out = info; LightLock_Unlock(&info_lock); }

void audio_volume(float v) {
	if (v < 0) v = 0;
	if (v > 1) v = 1;
	if (v == vol) return;
	vol = v;
	float mix[12] = {vol, vol};
	ndspChnSetMix(0, mix);   // the DSP takes it at its next frame; the ticks on channel 1 keep their own level
}
