#include "tags.h"
#include "decoder.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

static u32 be24(const u8 *b) { return b[0] << 16 | b[1] << 8 | b[2]; }
static u32 be32(const u8 *b) { return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static u32 le16(const u8 *b) { return b[1] << 8 | b[0]; }
static u32 le32(const u8 *b) { return (u32)b[3] << 24 | b[2] << 16 | b[1] << 8 | b[0]; }
static u64 le64(const u8 *b) { return (u64)le32(b + 4) << 32 | le32(b); }
static u32 syncsafe(const u8 *b) { return (b[0] & 127) << 21 | (b[1] & 127) << 14 | (b[2] & 127) << 7 | (b[3] & 127); }

static char *dupn(const char *s, int n) { char *d = malloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }

// ---- KEY=value: Vorbis comments, ID3's TXXX and the RIFF INFO list all end up here ---------

static bool key_is(const char *k, int kl, const char *name) { return (int)strlen(name) == kl && !strncasecmp(k, name, kl); }

// ID3's numbered genres, for files that say "(17)" or "17" rather than "Rock"
static const char *ID3G[] = {"Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
	"New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
	"Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion", "Trance",
	"Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul",
	"Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave",
	"Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
	"Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave",
	"Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock"};
static char *genre_name(const char *v, int vl) {
	char *g = dupn(v, vl);   // v may not end where the value does: parse a copy that does
	int a = 0, n = 0, ng = (int)(sizeof ID3G / sizeof *ID3G);
	if (vl > 2 && g[0] == '(' && sscanf(g, "(%d)%n", &a, &n) == 1 && n == vl && a >= 0 && a < ng) { free(g); return strdup(ID3G[a]); }
	bool digits = vl > 0 && vl <= 3;
	for (int k = 0; k < vl && digits; k++) digits = g[k] >= '0' && g[k] <= '9';
	if (digits && (a = atoi(g)) < ng) { free(g); return strdup(ID3G[a]); }
	return g;
}

static void kv(Track *t, const char *k, int kl, const char *v, int vl) {
	while (vl && (!v[vl - 1] || v[vl - 1] == ' ')) vl--;
	if (vl <= 0) return;
	char num[32];
	int nl = vl < 31 ? vl : 31;
	memcpy(num, v, nl);
	num[nl] = 0;
	// the first of each wins
	if (key_is(k, kl, "TITLE")) { if (!t->title) t->title = dupn(v, vl); }
	else if (key_is(k, kl, "ARTIST")) { if (!t->artist) t->artist = dupn(v, vl); }
	else if (key_is(k, kl, "ALBUM")) { if (!t->album) t->album = dupn(v, vl); }
	else if (key_is(k, kl, "ALBUMARTIST") || key_is(k, kl, "ALBUM_ARTIST") || key_is(k, kl, "ALBUM ARTIST")) { if (!t->album_artist) t->album_artist = dupn(v, vl); }
	else if (key_is(k, kl, "GENRE")) { if (!t->genre) t->genre = genre_name(v, vl); }
	else if (key_is(k, kl, "TRACKNUMBER")) { if (!t->no) t->no = atoi(num); }   // "3/12" is 3
	else if (key_is(k, kl, "DISCNUMBER")) { if (!t->disc) t->disc = atoi(num); }
	else if (key_is(k, kl, "DATE") || key_is(k, kl, "YEAR")) { if (!t->year) t->year = atoi(num); }   // "2019-05-01" too
	else if (key_is(k, kl, "REPLAYGAIN_TRACK_GAIN")) { if (isnan(t->gain_t)) t->gain_t = strtof(num, NULL); }
	else if (key_is(k, kl, "REPLAYGAIN_ALBUM_GAIN")) { if (isnan(t->gain_a)) t->gain_a = strtof(num, NULL); }
	else if (key_is(k, kl, "REPLAYGAIN_TRACK_PEAK")) { if (!t->peak_t) t->peak_t = strtof(num, NULL); }
	else if (key_is(k, kl, "REPLAYGAIN_ALBUM_PEAK")) { if (!t->peak_a) t->peak_a = strtof(num, NULL); }
	// Opus: Q7.8 dB against -23 LUFS on top of the header gain; ReplayGain aims 5 dB louder
	else if (key_is(k, kl, "R128_TRACK_GAIN")) { if (isnan(t->gain_t)) t->gain_t = atoi(num) / 256.0f + 5; }
	else if (key_is(k, kl, "R128_ALBUM_GAIN")) { if (isnan(t->gain_a)) t->gain_a = atoi(num) / 256.0f + 5; }
}

static void comments(Track *t, const u8 *b, u32 len) {
	if (len < 8) return;
	u32 p = 4 + le32(b);   // past the vendor string
	if (p < 4 || p + 4 > len) return;
	u32 n = le32(b + p);
	p += 4;
	while (n-- && p + 4 <= len) {
		u32 l = le32(b + p);
		p += 4;
		if (l > len - p) return;
		const char *s = (const char *)b + p, *eq = memchr(s, '=', l);
		if (eq) kv(t, s, eq - s, eq + 1, l - (eq - s) - 1);
		p += l;
	}
}

// ---- FLAC --------------------------------------------------------------------------------

static int pic_type;   // the cover we kept so far: 3 is the front, and beats any other

// STREAMINFO, the comments, and where the cover sits. The picture itself is skipped.
static bool flac(FILE *f, Track *t) {
	u8 h[10];
	if (fread(h, 1, 4, f) != 4) return false;
	if (!memcmp(h, "ID3", 3)) {
		if (fread(h + 4, 1, 6, f) != 6) return false;
		fseek(f, 10 + syncsafe(h + 6), SEEK_SET);
		if (fread(h, 1, 4, f) != 4) return false;
	}
	if (memcmp(h, "fLaC", 4)) return false;
	bool ok = false;
	for (bool last = false; !last;) {
		u8 bh[4];
		if (fread(bh, 1, 4, f) != 4) break;
		last = bh[0] & 0x80;
		int type = bh[0] & 0x7f;
		u32 len = be24(bh + 1);
		long start = ftell(f);
		if (type == 0 && len >= 18) {
			u8 s[18];
			if (fread(s, 1, 18, f) != 18) break;
			t->rate = s[10] << 12 | s[11] << 4 | s[12] >> 4;
			t->bits = ((s[12] & 1) << 4 | s[13] >> 4) + 1;
			u64 total = (u64)(s[13] & 15) << 32 | be32(s + 14);
			if (t->rate) t->secs = total / t->rate;
			ok = t->rate > 0;
		} else if (type == 4 && len < (1 << 20)) {
			u8 *b = malloc(len);
			if (b && fread(b, 1, len, f) == len) comments(t, b, len);
			free(b);
		} else if (type == 6 && len > 32 && pic_type != 3) {
			u8 b[8];
			if (fread(b, 1, 8, f) != 8) break;
			int ptype = be32(b);
			fseek(f, be32(b + 4), SEEK_CUR);   // mime
			if (fread(b, 1, 4, f) != 4) break;
			fseek(f, be32(b) + 16, SEEK_CUR);  // description, size, depth, colours
			if (fread(b, 1, 4, f) != 4) break;
			u32 plen = be32(b);
			if (plen && (pic_type < 0 || ptype == 3)) { t->pic_off = ftell(f); t->pic_len = plen; pic_type = ptype; }
		}
		fseek(f, start + len, SEEK_SET);
	}
	return ok;
}

// ---- MP3: ID3v2 (2.2 to 2.4), the first frame's Xing/Info/VBRI header, ID3v1 ------------

// ID3 text in its declared encoding (Latin-1, UTF-16 with or without a BOM, UTF-8), as UTF-8.
// Stops at the first NUL, so a v2.4 list keeps its first value.
static char *id3_text(int enc, const u8 *b, u32 n) {
	char *o = malloc(n * 2 + 1);
	int k = 0;
	if (enc == 0) {
		for (u32 i = 0; i < n && b[i]; i++) {
			if (b[i] < 0x80) o[k++] = b[i];
			else { o[k++] = 0xC0 | b[i] >> 6; o[k++] = 0x80 | (b[i] & 63); }
		}
	} else if (enc == 3) {
		for (u32 i = 0; i < n && b[i]; i++) o[k++] = b[i];
	} else {
		bool be = enc == 2;
		u32 i = 0;
		if (enc == 1 && n >= 2) {
			if (b[0] == 0xFE && b[1] == 0xFF) { be = true; i = 2; }
			else if (b[0] == 0xFF && b[1] == 0xFE) i = 2;
		}
		for (; i + 1 < n; i += 2) {
			u32 c = be ? b[i] << 8 | b[i + 1] : b[i + 1] << 8 | b[i];
			if (!c) break;
			if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
				u32 c2 = be ? b[i + 2] << 8 | b[i + 3] : b[i + 3] << 8 | b[i + 2];
				if (c2 >= 0xDC00 && c2 < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2; }
			}
			if (c < 0x80) o[k++] = c;
			else if (c < 0x800) { o[k++] = 0xC0 | c >> 6; o[k++] = 0x80 | (c & 63); }
			else if (c < 0x10000) { o[k++] = 0xE0 | c >> 12; o[k++] = 0x80 | (c >> 6 & 63); o[k++] = 0x80 | (c & 63); }
			else { o[k++] = 0xF0 | c >> 18; o[k++] = 0x80 | (c >> 12 & 63); o[k++] = 0x80 | (c >> 6 & 63); o[k++] = 0x80 | (c & 63); }
		}
	}
	o[k] = 0;
	if (!k) { free(o); return NULL; }
	return o;
}

// Where the text after a NUL-terminated string starts, in that encoding's terms.
static u32 past_string(int enc, const u8 *b, u32 n, u32 p) {
	if (enc == 1 || enc == 2) { while (p + 1 < n && (b[p] || b[p + 1])) p += 2; return p + 2; }
	while (p < n && b[p]) p++;
	return p + 1;
}

static void id3_frame(Track *t, const char *id, int idl, const u8 *b, u32 n) {
	static const struct { const char *v3, *v2, *key; } MAP[] = {
		{"TIT2", "TT2", "TITLE"}, {"TPE1", "TP1", "ARTIST"}, {"TALB", "TAL", "ALBUM"}, {"TPE2", "TP2", "ALBUMARTIST"},
		{"TRCK", "TRK", "TRACKNUMBER"}, {"TPOS", "TPA", "DISCNUMBER"}, {"TYER", "TYE", "YEAR"}, {"TDRC", "TDA", "DATE"},
		{"TCON", "TCO", "GENRE"},
	};
	if (n < 2) return;
	int enc = b[0];
	if (!memcmp(id, idl == 4 ? "TXXX" : "TXX", idl)) {   // a description, then the value
		u32 v = past_string(enc, b, n, 1);
		if (v >= n) return;
		char *d = id3_text(enc, b + 1, v - 1), *val = id3_text(enc, b + v, n - v);
		if (d && val) kv(t, d, strlen(d), val, strlen(val));
		free(d);
		free(val);
		return;
	}
	for (unsigned k = 0; k < sizeof MAP / sizeof *MAP; k++)
		if (!memcmp(id, idl == 4 ? MAP[k].v3 : MAP[k].v2, idl)) {
			char *v = id3_text(enc, b + 1, n - 1);
			if (v) kv(t, MAP[k].key, strlen(MAP[k].key), v, strlen(v));
			free(v);
			return;
		}
}

// APIC/PIC: encoding, mime (or a 3-letter format in 2.2), picture type, description, data.
static void id3_pic(FILE *f, long data, u32 len, int ver, Track *t) {
	u8 b[512];
	u32 n = len < sizeof b ? len : sizeof b;
	fseek(f, data, SEEK_SET);
	if (n < 4 || fread(b, 1, n, f) != n) return;
	int enc = b[0];
	u32 p = ver == 2 ? 4 : past_string(0, b, n, 1);
	if (p >= n) return;
	int type = b[p++];
	p = past_string(enc, b, n, p);
	if (p >= n) return;
	if (t->pic_len && !(type == 3 && pic_type != 3)) return;
	t->pic_off = data + p;
	t->pic_len = len - p;
	pic_type = type;
}

static u32 deunsync(u8 *b, u32 n) {
	u32 o = 0;
	for (u32 i = 0; i < n; i++) {
		b[o++] = b[i];
		if (b[i] == 0xFF && i + 1 < n && !b[i + 1]) i++;
	}
	return o;
}

// Returns where the audio starts: just past the tag, or 0 with none.
static long id3v2(FILE *f, Track *t) {
	u8 h[10];
	if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return 0;
	int ver = h[3], flags = h[5];
	long end = 10 + syncsafe(h + 6);
	if (ver < 2 || ver > 4) return end;
	long p = 10;
	if (flags & 0x40 && ver >= 3) {   // extended header
		u8 e[4];
		if (fread(e, 1, 4, f) != 4) return end;
		p += ver == 4 ? syncsafe(e) : be32(e) + 4;
	}
	int idl = ver == 2 ? 3 : 4, hl = ver == 2 ? 6 : 10;
	while (p + hl <= end) {
		u8 fh[10];
		fseek(f, p, SEEK_SET);
		if (fread(fh, 1, hl, f) != (size_t)hl || !fh[0]) break;   // padding
		u32 len = ver == 2 ? be24(fh + 3) : ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
		int ff = ver == 2 ? 0 : fh[9];
		long data = p + hl;
		p = data + len;
		if (!len || p > end) break;
		if (ver == 3 && ff & 0xE0) continue;   // compressed, encrypted or grouped
		if (ver == 4 && ff & 0x4C) continue;
		bool unsync = flags & 0x80 || (ver == 4 && ff & 0x02);
		if (ver == 4 && ff & 0x01) { if (len < 4) continue; data += 4; len -= 4; }   // data length indicator
		if (!memcmp(fh, ver == 2 ? "PIC" : "APIC", idl)) {
			if (!unsync) id3_pic(f, data, len, ver, t);   // art.c reads the bytes as they lie
			continue;
		}
		if (fh[0] != 'T' || len > 65536) continue;
		u8 *b = malloc(len);
		fseek(f, data, SEEK_SET);
		if (fread(b, 1, len, f) == len) id3_frame(t, (const char *)fh, idl, b, unsync ? deunsync(b, len) : len);
		free(b);
	}
	return end + (ver == 4 && flags & 0x10 ? 10 : 0);   // footer
}

static void id3v1(FILE *f, long size, Track *t) {
	u8 b[128];
	if (size < 128 || fseek(f, size - 128, SEEK_SET) || fread(b, 1, 128, f) != 128 || memcmp(b, "TAG", 3)) return;
	static const struct { int at, n; const char *key; } F[] = {{3, 30, "TITLE"}, {33, 30, "ARTIST"}, {63, 30, "ALBUM"}, {93, 4, "YEAR"}};
	for (int k = 0; k < 4; k++) {
		char *v = id3_text(0, b + F[k].at, F[k].n);
		if (v) kv(t, F[k].key, strlen(F[k].key), v, strlen(v));
		free(v);
	}
	if (!b[125] && b[126] && !t->no) t->no = b[126];
}

static const u16 KBPS[5][15] = {
	{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},   // MPEG-1 layer I
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},      // MPEG-1 layer II
	{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},       // MPEG-1 layer III
	{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},      // MPEG-2/2.5 layer I
	{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},           // MPEG-2/2.5 layers II, III
};

// Frame header fields at b, or false when b isn't one.
static bool mp3_header(const u8 *b, u32 *rate, u32 *kbps, u32 *spf, u32 *bytes) {
	if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
	int ver = b[1] >> 3 & 3, layer = b[1] >> 1 & 3, bri = b[2] >> 4, sri = b[2] >> 2 & 3, pad = b[2] >> 1 & 1;
	if (ver == 1 || !layer || !bri || bri == 15 || sri == 3) return false;
	static const u16 SR[3] = {44100, 48000, 32000};
	bool v1 = ver == 3;
	*rate = SR[sri] >> (v1 ? 0 : ver == 2 ? 1 : 2);
	*kbps = KBPS[v1 ? 3 - layer : layer == 3 ? 3 : 4][bri];
	*spf = layer == 3 ? 384 : (layer == 2 || v1) ? 1152 : 576;
	*bytes = layer == 3 ? (12000 * *kbps / *rate + pad) * 4 : (*spf / 8) * 1000 * *kbps / *rate + pad;
	return true;
}

static bool mp3(FILE *f, long start, long size, Track *t) {
	static u8 b[8192];
	fseek(f, start, SEEK_SET);
	u32 n = fread(b, 1, sizeof b, f);
	for (u32 i = 0; i + 4 <= n; i++) {
		u32 rate, kbps, spf, len, r2, k2, s2, l2;
		if (!mp3_header(b + i, &rate, &kbps, &spf, &len)) continue;
		if (i + len + 4 <= n && !mp3_header(b + i + len, &r2, &k2, &s2, &l2)) continue;   // a false sync
		bool v1 = (b[i + 1] >> 3 & 3) == 3, mono = b[i + 3] >> 6 == 3;
		u32 side = v1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
		const u8 *x = b + i + 4 + side;
		u32 frames = 0, vbytes = 0;
		if (x + 16 <= b + n && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
			u32 fl = be32(x + 4);
			const u8 *q = x + 8;
			if (fl & 1) { frames = be32(q); q += 4; }
			if (fl & 2) vbytes = be32(q);
		} else if (i + 36 + 18 <= n && !memcmp(b + i + 36, "VBRI", 4)) {
			vbytes = be32(b + i + 36 + 10);
			frames = be32(b + i + 36 + 14);
		}
		u64 audio = size - start - i;
		t->rate = rate;
		t->bits = 0;
		if (frames) {
			t->secs = (u64)frames * spf / rate;
			if (!vbytes) vbytes = audio;
			t->kbps = t->secs ? (u64)vbytes * 8 / t->secs / 1000 : kbps;
		} else {
			t->kbps = kbps;
			t->secs = audio * 8 / (kbps * 1000);
		}
		return true;
	}
	return false;
}

// ---- Ogg Vorbis and Opus: the first two packets, and the last page's granule position ------

#define OGG_KEEP (256 << 10)   // comment packets carrying cover art get cut here, harmlessly

static bool ogg(FILE *f, long size, int fmt, Track *t) {
	u8 *pk = malloc(OGG_KEEP), hdr[27], seg[255];
	u32 plen = 0, channels = 0, preskip = 0;
	int npk = 0;
	while (npk < 2) {
		if (fread(hdr, 1, 27, f) != 27 || memcmp(hdr, "OggS", 4)) break;
		int ns = hdr[26];
		if (fread(seg, 1, ns, f) != (size_t)ns) break;
		for (int s = 0; s < ns && npk < 2; s++) {
			u32 l = seg[s], keep = plen + l <= OGG_KEEP ? l : plen < OGG_KEEP ? OGG_KEEP - plen : 0;
			if (keep && fread(pk + plen, 1, keep, f) != keep) goto done;
			if (l > keep) fseek(f, l - keep, SEEK_CUR);
			plen += keep;
			if (l == 255) continue;   // the packet goes on
			if (npk == 0) {
				if (fmt == FMT_VORBIS && plen >= 28 && pk[0] == 1 && !memcmp(pk + 1, "vorbis", 6)) {
					channels = pk[11];
					t->rate = le32(pk + 12);
				} else if (fmt == FMT_OPUS && plen >= 19 && !memcmp(pk, "OpusHead", 8)) {
					channels = pk[9];
					preskip = le16(pk + 10);
					t->rate = 48000;
				} else goto done;
			} else if (fmt == FMT_VORBIS && plen > 7 && pk[0] == 3) comments(t, pk + 7, plen - 7);
			else if (fmt == FMT_OPUS && plen > 8 && !memcmp(pk, "OpusTags", 8)) comments(t, pk + 8, plen - 8);
			npk++;
			plen = 0;
		}
	}
	if (channels && t->rate) {
		long tail = size < 65536 ? size : 65536;
		fseek(f, size - tail, SEEK_SET);
		u32 n = fread(pk, 1, tail, f);
		for (long i = (long)n - 27; i >= 0; i--)
			if (!memcmp(pk + i, "OggS", 4)) {
				u64 g = le64(pk + i + 6);
				if (g == ~0ull) continue;   // a page nothing finishes on
				if (fmt == FMT_OPUS) g = g > preskip ? g - preskip : 0;
				t->secs = g / t->rate;
				break;
			}
		if (t->secs) t->kbps = (u64)size * 8 / t->secs / 1000;
	}
done:
	free(pk);
	t->bits = 0;
	return channels && t->rate;
}

// ---- WAV ---------------------------------------------------------------------------------

static bool wav(FILE *f, Track *t) {
	u8 h[12];
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
	u32 bpf = 0;
	bool ok = false;
	for (;;) {
		u8 c[8];
		if (fread(c, 1, 8, f) != 8) break;
		u32 len = le32(c + 4);
		long start = ftell(f);
		if (!memcmp(c, "fmt ", 4) && len >= 16) {
			u8 b[16];
			if (fread(b, 1, 16, f) != 16) break;
			t->rate = le32(b + 4);
			bpf = le16(b + 12);
			t->bits = le16(b + 14);
		} else if (!memcmp(c, "data", 4)) {
			if (bpf && t->rate) { t->secs = len / bpf / t->rate; ok = true; }
		} else if (!memcmp(c, "LIST", 4) && len > 4 && len < 65536) {
			static const struct { const char *id, *key; } INFO[] = {
				{"INAM", "TITLE"}, {"IART", "ARTIST"}, {"IPRD", "ALBUM"}, {"ITRK", "TRACKNUMBER"}, {"ICRD", "DATE"}, {"IGNR", "GENRE"},
			};
			u8 *b = malloc(len);
			if (fread(b, 1, len, f) == len && !memcmp(b, "INFO", 4))
				for (u32 p = 4; p + 8 <= len;) {
					u32 l = le32(b + p + 4);
					if (l > len - p - 8) break;
					for (int k = 0; k < (int)(sizeof INFO / sizeof *INFO); k++)
						if (!memcmp(b + p, INFO[k].id, 4)) {
							char *v = id3_text(0, b + p + 8, l);   // Latin-1 by the old spec
							if (v) kv(t, INFO[k].key, strlen(INFO[k].key), v, strlen(v));
							free(v);
						}
					p += 8 + l + (l & 1);
				}
			free(b);
		}
		if (fseek(f, start + len + (len & 1), SEEK_SET)) break;
	}
	return ok;
}

// The file's first 32 KB read from the card in one go, then a stream over it: the tags nearly always
// sit in there, so the readers' many small reads and hops (over a cover, say) are memory, and only
// what lies past it (an MP3's first frame after a big cover, an ID3v1 tag at the end) goes back to
// the card. Up to build 27 each of those was a trip of its own, a dozen or so a song.
#define HEAD (32 * 1024)
#ifdef _NEWLIB_VERSION
# ifdef __LARGE64_FILES
typedef _fpos64_t hpos_t;
# else
typedef fpos_t hpos_t;
# endif
#else   // a PC build for player/test/test_tags.c: glibc has no funopen, the test brings one
typedef long hpos_t;
# define _READ_WRITE_BUFSIZE_TYPE int
FILE *funopen(const void *, int (*)(void *, char *, int), int (*)(void *, const char *, int), long (*)(void *, long, int), int (*)(void *));
#endif
// A window of HEAD bytes from base: a read outside it moves it there with one read from the card,
// so an Ogg file's last 64 KB or the MP3 frame after a big cover cost a trip or two, not one per KB.
typedef struct { int fd; long pos, size, base, hn; u8 head[HEAD]; } Head;
int tags_card_reads;
static bool head_fill(Head *h, long at) {
	if (lseek(h->fd, at, SEEK_SET) < 0) return false;
	long k = read(h->fd, h->head, HEAD);
	tags_card_reads++;
	h->base = at;
	h->hn = k > 0 ? k : 0;
	return h->hn > 0;
}
static int head_read(void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n) {
	Head *h = c;
	int got = 0;
	while (got < (int)n && h->pos < h->size) {
		if ((h->pos < h->base || h->pos >= h->base + h->hn) && !head_fill(h, h->pos)) break;
		long k = h->base + h->hn - h->pos;
		if (k > (long)n - got) k = n - got;
		memcpy(buf + got, h->head + (h->pos - h->base), k);
		h->pos += k;
		got += k;
	}
	return got;
}
static hpos_t head_seek(void *c, hpos_t off, int whence) {
	Head *h = c;
	long p = whence == SEEK_SET ? (long)off : whence == SEEK_CUR ? h->pos + (long)off : h->size + (long)off;
	if (p < 0) return -1;
	h->pos = p;
	return p;
}
static int head_close(void *c) {
	Head *h = c;
	close(h->fd);
	free(h);
	return 0;
}
static FILE *head_open(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return NULL;
	Head *h = malloc(sizeof *h);
	long size = h ? lseek(fd, 0, SEEK_END) : -1;
	if (size < 0 || lseek(fd, 0, SEEK_SET) < 0) { free(h); close(fd); return NULL; }
	h->fd = fd;
	h->pos = 0;
	h->size = size;
	h->base = h->hn = 0;
	head_fill(h, 0);   // the tags, nearly always: the one read most files need
	FILE *f = funopen(h, head_read, NULL, head_seek, head_close);
	if (!f) { close(fd); free(h); }
	return f;
}

bool tags_read(const char *path, int fmt, Track *t, u64 *fsize) {
	t->gain_t = t->gain_a = NAN;
	t->fmt = fmt;
	pic_type = -1;
	FILE *f = head_open(path);
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	rewind(f);
	if (fsize) *fsize = size > 0 ? (u64)size : 0;
	bool ok = false;
	switch (fmt) {
	case FMT_FLAC: ok = flac(f, t); break;
	case FMT_MP3: {
		long at = id3v2(f, t);
		ok = mp3(f, at, size, t);
		if (ok && !t->title) id3v1(f, size, t);
		break;
	}
	case FMT_VORBIS: case FMT_OPUS: ok = ogg(f, size, fmt, t); break;
	case FMT_WAV: ok = wav(f, t); break;
	}
	fclose(f);
	return ok;
}
