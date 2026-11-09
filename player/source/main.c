// Pocket: the player. Top screen is Now Playing over a gradient pulled from the cover, bottom
// screen is the library with a mini player, both dressed like Spotify shrunk to a 3DS.

#define _GNU_SOURCE   // strcasestr
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "art.h"
#include "audio.h"
#include "connect.h"
#include "crumb.h"
#include "decoder.h"
#include "library.h"
#include "meta.h"
#include "net.h"
#include "spotify.h"
#include "spotmatch.h"
#include "stats.h"
#include <sys/stat.h>

#define GREEN 0x1ed760
#define BG    0x121212
#define GREY  0xb3b3b3
#define CHIP  0x2a2a2a
#define ROW   34
#define LIST0 30      // the list, below its header and above the mini player
#define LIST1 list1()

static u32 rgba(u32 hex, u8 a) { return C2D_Color32(hex >> 16, (hex >> 8) & 255, hex & 255, a); }
static u32 rgb(u32 hex) { return rgba(hex, 255); }
static u32 mixc(u32 a, u32 b, float t) {   // t of a, the rest b
	u32 o = 0;
	for (int s = 0; s <= 16; s += 8) o |= (u32)(((a >> s) & 255) * t + ((b >> s) & 255) * (1 - t)) << s;
	return o;
}

// ---- text --------------------------------------------------------------------------------

#define BOLD 0x80000000u
static C2D_TextBuf tbuf, mbuf;   // this frame's text, and scratch for measuring
static float SC(float px) { return px / 22.0f; }

static float measure(const char *s, float px) {
	C2D_Text t;
	float w = 0;
	C2D_TextBufClear(mbuf);
	C2D_TextParse(&t, mbuf, s);
	C2D_TextGetDimensions(&t, SC(px), SC(px), &w, NULL);
	return w;
}

static u32 fnv(const char *s) { u32 h = 2166136261u; while (*s) h = (h ^ (u8)*s++) * 16777619u; return h; }
static int utf8_back(const char *s, int n) { while (n > 0 && (s[n] & 0xC0) == 0x80) n--; return n; }

// How many bytes of s fit in maxw with "..." after them (all of it if s fits). Remembered,
// because measuring means parsing and the list asks the same question every frame.
static struct { u32 h; u16 w, px; int n; } fits[1024];
static int fit(const char *s, float px, float maxw) {
	u32 h = fnv(s);
	u16 w = (u16)maxw, p = (u16)(px * 10);
	int slot = (h ^ w * 31u ^ p * 131u) & 1023;
	if (fits[slot].h == h && fits[slot].w == w && fits[slot].px == p) return fits[slot].n;
	int len = strlen(s), n = len;
	if (len > 400) len = n = 400;
	if (len < (int)strlen(s) || measure(s, px) > maxw) {
		char b[512];
		int lo = 0, hi = len;
		while (lo < hi) {
			int mid = (lo + hi + 1) / 2;
			snprintf(b, sizeof b, "%.*s...", utf8_back(s, mid), s);
			if (measure(b, px) <= maxw) lo = mid;
			else hi = mid - 1;
		}
		n = utf8_back(s, lo);
		while (n > 0 && s[n - 1] == ' ') n--;
	}
	fits[slot].h = h; fits[slot].w = w; fits[slot].px = p; fits[slot].n = n;
	return n;
}

// Emoji: the system font has none, so they came out as a box with a "?". Hearts are drawn as
// hearts in their own colour (a name like "Lilac 💜"); any other emoji is left out.
static void ic_heart(float x, float y, float s, u32 c);
static void ic_check(float x, float y, float r);   // artistpage.inc: Spotify's green check
static u32 utf8_cp(const char *s, int *n) {
	const u8 *u = (const u8 *)s;
	if (u[0] < 0x80) { *n = 1; return u[0]; }
	if ((u[0] & 0xE0) == 0xC0 && u[1]) { *n = 2; return (u[0] & 31) << 6 | (u[1] & 63); }
	if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { *n = 3; return (u[0] & 15) << 12 | (u[1] & 63) << 6 | (u[2] & 63); }
	if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) { *n = 4; return (u32)(u[0] & 7) << 18 | (u[1] & 63) << 12 | (u[2] & 63) << 6 | (u[3] & 63); }
	*n = 1;
	return u[0];
}
static u32 heart_colour(u32 c);
// Not for the system font: what it has no glyph for (its "?" box), and the hearts we draw ourselves.
// Asked of the font itself, so a ★ or ♪ in a title, which it does have, stays.
static bool is_emoji(u32 c) {
	if (c < 0x80) return false;
	if (heart_colour(c)) return true;
	static struct { u32 c; bool no; } seen[256];
	int k = (c * 2654435761u) >> 24;
	if (seen[k].c != c) {
		CFNT_s *f = fontGetSystemFont();
		seen[k].c = c;
		seen[k].no = fontGlyphIndexFromCodePoint(f, c) == fontGetInfo(f)->alterCharIndex;
	}
	return seen[k].no;
}
static u32 heart_colour(u32 c) {   // 0 if it isn't a heart
	switch (c) {
	case 0x2764: return 0xff3b30;  case 0x1F9E1: return 0xff9500; case 0x1F49B: return 0xffd60a;
	case 0x1F49A: return 0x34c759; case 0x1F499: return 0x0a84ff; case 0x1F49C: return 0xaf52de;
	case 0x1F5A4: return 0x3a3a3c; case 0x1F90D: return 0xffffff; case 0x1F90E: return 0xa2845e;
	case 0x1FA77: case 0x1F496: case 0x1F495: case 0x1F497: case 0x1F493: case 0x1F498: case 0x1F49D: return 0xff6fb5;
	case 0x1FA75: return 0x5ac8fa; case 0x1FA76: return 0x8e8e93;
	}
	return 0;
}
// The big names (an artist's, a list's) in Figtree Bold, made at 40 px so it's only ever drawn at its
// size or smaller: the system font's glyphs are ~22 px, and stretched to 34 they went soft (Alex,
// build 38). It has Latin and Latin-1/Ext-A; a name with anything else (百景) keeps the system font.
// It lives on the card (sdmc:/3ds/player/namefont.bcfnt, 1 MB), not in the app: baked in (build 39), the
// CIA's code grew by that megabyte and the HOME Menu crashed in the kernel launching it.
#define NAMEF 0x40000000u
#define NAMEFONT "sdmc:/3ds/player/namefont.bcfnt"
static C2D_Font nfont;
static float nf_k = 1 / 22.0f;   // px -> its scale, set so its ascent matches the system font's at px
static bool nf_has(const char *s) {
	if (!nfont) return false;
	int alt = C2D_FontGetInfo(nfont)->alterCharIndex;
	for (const char *p = s; *p;) {
		int n;
		u32 c = utf8_cp(p, &n);
		p += n;
		if (C2D_FontGlyphIndexFromCodePoint(nfont, c) == alt) return false;
	}
	return true;
}
static float measure_name(const char *s, float px) {
	if (!nf_has(s)) return measure(s, px);
	C2D_Text t;
	float w = 0;
	C2D_TextBufClear(mbuf);
	C2D_TextFontParse(&t, nfont, mbuf, s);
	C2D_TextGetDimensions(&t, px * nf_k, px * nf_k, &w, NULL);
	return w;
}

static float text_plain(const char *s, float x, float y, float px, u32 col, u32 flags, float maxw);
static float text(const char *s, float x, float y, float px, u32 col, u32 flags, float maxw) {
	bool any = false;
	for (const char *p = s; *p && !any;) { int n; any = is_emoji(utf8_cp(p, &n)); p += n; }
	if (!any) return text_plain(s, x, y, px, col, flags, maxw);
	// runs of text, and the hearts between them
	char seg[8][256], clean[512];
	u32 hc[8];
	int ns = 0, cl = 0, sl = 0;
	seg[0][0] = 0;
	for (const char *p = s; *p;) {
		int n;
		u32 c = utf8_cp(p, &n), h = heart_colour(c);
		if (!is_emoji(c)) {
			if (sl + n < 255) { memcpy(seg[ns] + sl, p, n); sl += n; seg[ns][sl] = 0; }
			if (cl + n < 511) { memcpy(clean + cl, p, n); cl += n; }
		} else if (h && ns < 7) { hc[ns++] = h; sl = 0; seg[ns][0] = 0; }
		p += n;
	}
	clean[cl] = 0;
	float hw = px * 0.95f, total = 0;
	for (int k = 0; k <= ns; k++) total += (seg[k][0] ? (flags & NAMEF ? measure_name(seg[k], px) : measure(seg[k], px)) : 0) + (k < ns ? hw : 0);
	if (maxw > 0 && total > maxw) return text_plain(clean, x, y, px, col, flags, maxw);   // too long: cut, without them
	u32 align = flags & (C2D_AlignCenter | C2D_AlignRight);
	float cx = align == C2D_AlignCenter ? x - total / 2 : align == C2D_AlignRight ? x - total : x;
	u32 rest = flags & ~(C2D_AlignCenter | C2D_AlignRight);
	for (int k = 0; k <= ns; k++) {
		if (seg[k][0]) cx += text_plain(seg[k], cx, y, px, col, rest, 0);
		if (k < ns) { ic_heart(cx + hw / 2, y - px * 0.36f, px * 0.3f, rgb(hc[k])); cx += hw; }
	}
	return total;
}

// Drawn at its baseline; returns the width. maxw > 0 cuts it with "..." to fit.
static float text_plain(const char *s, float x, float y, float px, u32 col, u32 flags, float maxw) {
	char b[512];
	if ((flags & NAMEF) && nf_has(s)) {   // its own font, real bold: drawn once
		if (maxw > 0 && measure_name(s, px) > maxw) {
			int n = strlen(s);
			if (n > 400) n = 400;
			do {
				n = utf8_back(s, n - 1);
				while (n > 0 && s[n - 1] == ' ') n--;
				snprintf(b, sizeof b, "%.*s...", n, s);
			} while (n > 0 && measure_name(b, px) > maxw);
			s = b;
		}
		C2D_Text t;
		C2D_TextFontParse(&t, nfont, tbuf, s);
		float sc = px * nf_k, w = 0;
		C2D_TextGetDimensions(&t, sc, sc, &w, NULL);
		C2D_DrawText(&t, C2D_AtBaseline | C2D_WithColor | (flags & ~(BOLD | NAMEF)), x, y, 0, sc, sc, col);
		return w;
	}
	if (maxw > 0) {
		int n = fit(s, px, maxw);
		if (s[n]) { snprintf(b, sizeof b, "%.*s...", n, s); s = b; }
	}
	C2D_Text t;
	C2D_TextParse(&t, tbuf, s);
	float sc = SC(px), w = 0;
	C2D_TextGetDimensions(&t, sc, sc, &w, NULL);
	u32 f = C2D_AtBaseline | C2D_WithColor | (flags & ~(BOLD | NAMEF));
	C2D_DrawText(&t, f, x, y, 0, sc, sc, col);
	if (flags & BOLD) C2D_DrawText(&t, f, x + 0.5f, y, 0, sc, sc, col);   // the system font has no bold
	return w;
}

// Up to two lines, broken at a space; returns how many it took.
static int text2(const char *s, float x, float y, float lh, float px, u32 col, u32 flags, float maxw) {
	if (measure(s, px) <= maxw) { text(s, x, y, px, col, flags, 0); return 1; }
	char b[512];
	int best = 0;
	for (int i = 1; s[i] && i < 400; i++)
		if (s[i] == ' ') {
			snprintf(b, sizeof b, "%.*s", i, s);
			if (measure(b, px) <= maxw) best = i;
			else break;
		}
	if (!best) { text(s, x, y, px, col, flags, maxw); return 1; }
	snprintf(b, sizeof b, "%.*s", best, s);
	text(b, x, y, px, col, flags, 0);
	text(s + best + 1, x, y + lh, px, col, flags, maxw);
	return 2;
}

static void mmss(char *o, double s) { int t = s > 0 ? (int)s : 0; sprintf(o, "%d:%02d", t / 60, t % 60); }

// ---- shapes ------------------------------------------------------------------------------

static void rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0, w, h, c); }

// Rounded rectangles as a fan from the centre, so translucent ones don't double up at the
// corners the way rect-plus-circles would.
#define SEG 5
static int rr_pts(float x, float y, float w, float h, float r, float *px, float *py) {
	static float cs[SEG + 1], sn[SEG + 1];
	if (cs[0] == 0) for (int i = 0; i <= SEG; i++) { cs[i] = cosf(i * (float)M_PI / 2 / SEG); sn[i] = sinf(i * (float)M_PI / 2 / SEG); }
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	float cx[4] = {x + r, x + w - r, x + w - r, x + r}, cy[4] = {y + r, y + r, y + h - r, y + h - r};
	int n = 0;
	for (int c = 0; c < 4; c++)   // from left/top/right/bottom, clockwise on screen
		for (int i = 0; i <= SEG; i++) {
			float a = c == 0 ? -cs[i] : c == 1 ? sn[i] : c == 2 ? cs[i] : -sn[i];
			float b = c == 0 ? -sn[i] : c == 1 ? -cs[i] : c == 2 ? sn[i] : cs[i];
			px[n] = cx[c] + r * a;
			py[n] = cy[c] + r * b;
			n++;
		}
	return n;
}
static void rrect(float x, float y, float w, float h, float r, u32 c) {
	float px[4 * (SEG + 1)], py[4 * (SEG + 1)];
	int n = rr_pts(x, y, w, h, r, px, py);
	float mx = x + w / 2, my = y + h / 2;
	for (int i = 0; i < n; i++) { int j = (i + 1) % n; C2D_DrawTriangle(mx, my, c, px[i], py[i], c, px[j], py[j], c, 0); }
}
static void rstroke(float x, float y, float w, float h, float r, u32 c) {
	float ox[4 * (SEG + 1)], oy[4 * (SEG + 1)], ix[4 * (SEG + 1)], iy[4 * (SEG + 1)];
	int n = rr_pts(x, y, w, h, r, ox, oy);
	rr_pts(x + 1, y + 1, w - 2, h - 2, r - 1, ix, iy);
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		C2D_DrawTriangle(ox[i], oy[i], c, ox[j], oy[j], c, ix[i], iy[i], c, 0);
		C2D_DrawTriangle(ix[i], iy[i], c, ox[j], oy[j], c, ix[j], iy[j], c, 0);
	}
}

static void ic_play(float x, float y, float s, u32 c) { C2D_DrawTriangle(x - s * .38f, y - s * .5f, c, x - s * .38f, y + s * .5f, c, x + s * .5f, y, c, 0); }
static void ic_pause(float x, float y, float s, u32 c) { rect(x - s * .4f, y - s * .5f, s * .28f, s, c); rect(x + s * .12f, y - s * .5f, s * .28f, s, c); }
static void ic_next(float x, float y, float s, u32 c) {
	C2D_DrawTriangle(x - s * .45f, y - s * .5f, c, x - s * .45f, y + s * .5f, c, x + s * .3f, y, c, 0);
	rect(x + s * .3f, y - s * .5f, s * .16f, s, c);
}
static void ic_prev(float x, float y, float s, u32 c) {
	C2D_DrawTriangle(x + s * .45f, y - s * .5f, c, x + s * .45f, y + s * .5f, c, x - s * .3f, y, c, 0);
	rect(x - s * .46f, y - s * .5f, s * .16f, s, c);
}
static void ic_shuffle(float x, float y, u32 c) {
	C2D_DrawLine(x - 7, y - 4, c, x + 4, y + 4, c, 1.5f, 0);
	C2D_DrawLine(x - 7, y + 4, c, x + 4, y - 4, c, 1.5f, 0);
	C2D_DrawTriangle(x + 3, y + 1, c, x + 3, y + 7, c, x + 8, y + 4, c, 0);
	C2D_DrawTriangle(x + 3, y - 7, c, x + 3, y - 1, c, x + 8, y - 4, c, 0);
}
static void ic_repeat(float x, float y, u32 c, bool one) {
	C2D_DrawLine(x - 7, y - 4, c, x + 7, y - 4, c, 1.5f, 0);
	C2D_DrawLine(x + 7, y - 4.7f, c, x + 7, y + 4, c, 1.5f, 0);
	C2D_DrawLine(x + 7, y + 4, c, x - 3, y + 4, c, 1.5f, 0);
	C2D_DrawLine(x - 7, y - 4.7f, c, x - 7, y + 1, c, 1.5f, 0);
	C2D_DrawTriangle(x - 3, y + 1, c, x - 3, y + 7, c, x - 7.5f, y + 4, c, 0);
	if (one) text("1", x, y + 2.5f, 7, c, C2D_AlignCenter, 0);
}
static void ic_search(float x, float y, u32 c, u32 bg) {
	C2D_DrawCircleSolid(x - 1, y - 1, 0, 4.5f, c);
	C2D_DrawCircleSolid(x - 1, y - 1, 0, 3, bg);
	C2D_DrawLine(x + 2, y + 2, c, x + 5, y + 5, c, 2, 0);
}
static void ic_back(float x, float y, u32 c) {
	C2D_DrawLine(x + 2, y - 4.5f, c, x - 2.5f, y, c, 1.8f, 0);
	C2D_DrawLine(x - 2.5f, y, c, x + 2, y + 4.5f, c, 1.8f, 0);
}

// ---- art textures ------------------------------------------------------------------------

static C3D_Tex big_tex, atlas;   // the playing cover; 16x16 album thumbnails
static C3D_Tex faces_tex;        // 16x16 artists' faces, for the Artists tab
static bool face_ok[MI_FACES], faces_on;
static const Tex3DS_SubTexture big_sub = {ART_BIG, ART_BIG, 0.0f, 1.0f, ART_BIG / 256.0f, 1.0f - ART_BIG / 256.0f};
static int big_alb = -1;         // album of the cover the big texture holds, -1 none (it holds the last one until the next is there)
static int big_none = -1;        // a song the worker found no cover for
static bool *thumb_ok;
static bool ghost;   // drawing the page a slide is leaving: nothing it draws asks for anything
static int at_side = 512, at_cols = 16, at_slots = 256;   // the atlas: albums' tiles, then the playlists' (at nal + k)
static bool list_ok[ART_LISTS];
static u32 top_color = 0x404040, top_target = 0x404040;

static void put_nf(C3D_Tex *t, int ox, int oy, const u32 *px, int s) {   // no flush: many, then one C3D_TexFlush
	u32 *d = t->data;
	for (int y = 0; y < s; y++)
		for (int x = 0; x < s; x++) {
			int tx = ox + x, ty = oy + y;
			d[(((ty >> 3) * (t->width >> 3) + (tx >> 3)) << 6) +
			  ((tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3))] = px[y * s + x];
		}
}
static void put(C3D_Tex *t, int ox, int oy, const u32 *px, int s) { put_nf(t, ox, oy, px, s); C3D_TexFlush(t); }

static void tile(int slot, float x, float y, float s) {   // a 32² square of the atlas
	float h = 0.5f / at_side, u = (slot % at_cols) * 32.0f / at_side + h, v = (slot / at_cols) * 32.0f / at_side + h, d = 32.0f / at_side - 2 * h;
	Tex3DS_SubTexture st = {32, 32, u, 1 - v, u + d, 1 - v - d};   // half a texel in: no neighbour's edge bleeding in (Alex: lines between artists)
	C2D_Image im = {&atlas, &st};
	C2D_DrawImageAt(im, x, y, 0, NULL, s / 32, s / 32);
}
static bool atlas_dirty;   // tiles put since the last flush
static void put_tile(int slot, const u32 *px) { put_nf(&atlas, (slot % at_cols) * 32, (slot / at_cols) * 32, px, ART_THUMB); atlas_dirty = true; }
static void atlas_flush(void) { if (atlas_dirty) { C3D_TexFlush(&atlas); atlas_dirty = false; } }
static void put_thumb(int a, const u32 *px) { if (a >= 0 && a < at_slots && a < lib.nal) { put_tile(a, px); thumb_ok[a] = true; } }
static void thumb(int album, float x, float y, float s) {
	if (album >= 0 && album < at_slots && thumb_ok[album]) tile(album, x, y, s);
	else {
		rrect(x, y, s, s, 2, rgb(album >= 0 ? lib.albums[album].color : 0x333333));
		if (album >= 0 && !ghost) art_thumb_need(album);   // on screen: it goes first
	}
}
// a playlist's own picture, if the sync brought one; false if not (yet)
static bool list_tile(int k, float x, float y, float s) {
	if (k <= 0 || k >= ART_LISTS || !list_ok[k] || lib.nal + k >= at_slots) return false;
	tile(lib.nal + k, x, y, s);
	return true;
}
static void lists_want(void) {   // at launch, and whenever the lists are read again
	char *p[ART_LISTS] = {0};
	int n = lib.nl < ART_LISTS ? lib.nl : ART_LISTS;
	for (int k = 1; k < n; k++) p[k] = lib.lists[k].image;
	memset(list_ok, 0, sizeof list_ok);
	art_lists_want(p, n);
}

static void art_poll(void) {
	int tr;
	const u32 *px;
	u32 col;
	if (art_big(&tr, &px, &col)) {
		if (px) {
			int al = tr >= 0 && tr < lib.nt ? lib.t[tr].album_id : -1;
			if (al != big_alb) put(&big_tex, 0, 0, px, ART_BIG);   // the same album's cover is already there
			big_alb = al;
			big_none = -1;
		} else { big_alb = -1; big_none = tr; }
		top_target = col;
	}
	for (int a; (a = art_thumb(&px)) >= 0;) put_thumb(a, px);
	static u32 lpx[ART_THUMB * ART_THUMB];
	for (int k; (k = art_list_thumb(lpx)) >= 0;)
		if (k < ART_LISTS && lib.nal + k < at_slots) { put_tile(lib.nal + k, lpx); list_ok[k] = true; }
	atlas_flush();
	static u32 fpx[MI_FACE * MI_FACE];
	for (int x; (x = meta_face(fpx)) >= 0;)
		if (faces_on && x < MI_FACES) { put(&faces_tex, (x % 16) * MI_FACE, (x / 16) * MI_FACE, fpx, MI_FACE); face_ok[x] = true; }
}
static void put_face(int x, const u32 *px) {   // the cache's, at launch: one flush after them all
	if (faces_on && x >= 0 && x < MI_FACES) { put_nf(&faces_tex, (x % 16) * MI_FACE, (x / 16) * MI_FACE, px, MI_FACE); face_ok[x] = true; }
}
static void face(int artist, float x, float y, float s) {   // already round
	float h = 0.5f / 512, u = (artist % 16) * MI_FACE / 512.0f + h, v = (artist / 16) * MI_FACE / 512.0f + h, d = MI_FACE / 512.0f - 2 * h;
	Tex3DS_SubTexture st = {MI_FACE, MI_FACE, u, 1 - v, u + d, 1 - v - d};
	C2D_Image im = {&faces_tex, &st};
	C2D_DrawImageAt(im, x, y, 0, NULL, s / MI_FACE, s / MI_FACE);
}

// ---- queue -------------------------------------------------------------------------------

enum { T_SONGS, T_ALBUMS, T_ARTISTS, T_PLAYLISTS, V_DETAIL, T_STATS, T_SETTINGS, T_HOME };   // stats, settings: the side menu's pages; home: homepage.inc
#define NTAB 4
#define K_AP  10   // dkind of a song list opened from an artist page (ap_g, artistpage.inc)
#define K_APQ 11   // src_kind while an artist page's own queue plays
static Group ap_g;
static char ap_gkey[300];
static void ap_open_local(int artist);
static void ap_clear(void);

static LightLock qlock;
static int *q_orig, *q_play, qn, qpos;
static int qadd;   // how many songs queued by hand sit right after the one playing (circle pad down)
static float qclear_p;   // circle pad up held toward clearing the queue, 0..1 (drawn as a green line across both screens)
static bool shuffle;
static int repeat;                 // 0 off, 1 all, 2 one
static char src_name[320] = "YOUR LIBRARY";
static int src_kind = -1, src_id = -1;   // the album, artist or playlist it came from, if any

// ReplayGain as set (auto: album gain while an album plays in order, track gain otherwise),
// and what the worker was last told
#define RG_AUTO 3
static int rg_cfg = RG_OFF, rg_sent = -1;
static const char *RGNAME[] = {"Normalize volume: off", "Normalize volume: by song", "Normalize volume: by album", "Normalize volume: auto"};   // ReplayGain, by Spotify's name for it
static void rg_sync(void) {
	int e = rg_cfg != RG_AUTO ? rg_cfg : src_kind == T_ALBUMS && !shuffle ? RG_ALBUM : RG_TRACK;
	if (e != rg_sent) { rg_sent = e; audio_replaygain(e); }
}

static void shuffled(int first) {  // q_play = q_orig in random order, `first` at the front
	memcpy(q_play, q_orig, qn * sizeof(int));
	for (int i = qn - 1; i > 0; i--) { int j = rand() % (i + 1), t = q_play[i]; q_play[i] = q_play[j]; q_play[j] = t; }
	for (int i = 0; i < qn; i++) if (q_play[i] == first) { q_play[i] = q_play[0]; q_play[0] = first; break; }
	qpos = 0;
}

static void home_note(int track);   // homepage.inc: Recents
static void home_note_connect(int track, const char *ctx);
static void home_build(void);
static void set_queue(const int *songs, int n, int start) {
	connect_release();   // the 3DS's own pick: Spotify lets go
	LightLock_Lock(&qlock);
	q_orig = realloc(q_orig, (n ? n : 1) * sizeof(int));
	q_play = realloc(q_play, (n ? n : 1) * sizeof(int));
	memcpy(q_orig, songs, n * sizeof(int));
	qn = n;
	qadd = 0;
	if (shuffle) shuffled(songs[start]);
	else { memcpy(q_play, q_orig, n * sizeof(int)); qpos = start; }
	int t = q_play[qpos];
	LightLock_Unlock(&qlock);
	rg_sync();   // before the song starts, so its first second isn't at the old level
	audio_play(t);
	home_note(t);
}

static void set_shuffle(bool on) {
	LightLock_Lock(&qlock);
	shuffle = on;
	qadd = 0;
	if (qn) {
		int c = q_play[qpos];
		if (on) shuffled(c);
		else {
			memcpy(q_play, q_orig, qn * sizeof(int));
			for (qpos = 0; qpos < qn - 1 && q_play[qpos] != c; qpos++);
		}
	}
	LightLock_Unlock(&qlock);
	rg_sync();
}

// The worker asks when a song ends. Past the last one with repeat off, we go back to the
// first and wait there paused, like Spotify does.
static int auto_next(bool *pause) {
	int c = connect_next(pause);   // while Spotify drives, it says what's next
	if (c != -2) return c;
	LightLock_Lock(&qlock);
	int t = -1;
	if (qn) {
		if (repeat != 2) {
			if (qpos + 1 < qn) { qpos++; if (qadd > 0) qadd--; }   // a queued song is spent as it comes up
			else { qpos = 0; qadd = 0; *pause = repeat == 0; }
		}
		t = q_play[qpos];
	}
	LightLock_Unlock(&qlock);
	return t;
}

static void skip(int dir, const AudioInfo *ai) {
	if (connect_driving()) { connect_skip(dir); return; }
	LightLock_Lock(&qlock);
	int t = -1;
	bool pause = false;
	if (qn) {
		if (dir < 0 && ai->pos > 3) t = -2;   // back to the start of this one
		else if (dir > 0) { if (++qpos >= qn) { qpos = 0; qadd = 0; pause = repeat == 0; } else if (qadd > 0) qadd--; t = q_play[qpos]; }
		else if (qn > 1) { qpos = qpos > 0 ? qpos - 1 : qn - 1; qadd = 0; t = q_play[qpos]; }   // first song: round to the last
		else t = -2;
	}
	LightLock_Unlock(&qlock);
	if (t == -2) audio_seek(0, false);
	else if (t >= 0) { audio_play(t); if (pause) audio_pause(true); }
}

// Spotify Connect asked for something (connect.c): a song from its context as a queue of one (it
// knows what's next; auto_next asks it), a pause, a seek
static void connect_apply(const ConnectDo *d, const AudioInfo *ai) {
	if (d->kind == CD_LOAD && d->track >= 0 && d->track < lib.nt) {
		LightLock_Lock(&qlock);
		q_orig = realloc(q_orig, sizeof(int));
		q_play = realloc(q_play, sizeof(int));
		q_orig[0] = q_play[0] = d->track;
		qn = 1;
		qpos = 0;
		qadd = 0;
		LightLock_Unlock(&qlock);
		snprintf(src_name, sizeof src_name, "%s", d->src[0] ? d->src : "SPOTIFY");
		src_kind = src_id = -1;
		rg_sync();
		audio_play_at(d->track, d->secs, d->paused);
		home_note_connect(d->track, d->ctx);
	} else if (d->kind == CD_PAUSE) audio_pause(d->paused);
	else if (d->kind == CD_SEEK) audio_seek(d->secs, false);
	else if (d->kind == CD_SKIP) skip(d->dir, ai);   // Spotify follows the 3DS: its own queue
}

// ---- resume ------------------------------------------------------------------------------

// The queue by path (the library's indices change when songs come and go), the shuffled
// order as positions in it, the song and the second. Read back paused: blasting music the
// moment the app opens is rude.
#define RESUME "sdmc:/3ds/player/resume"

static void save_resume(const AudioInfo *ai) {
	if (!qn || !ai->open || ai->track < 0) { remove(RESUME); return; }
	int at = -1;   // the worker asks for the next song early, so qpos can be one ahead of the ear
	for (int k = 0; k < qn && at < 0; k++) {
		int j = (qpos - k + qn) % qn;
		if (q_play[j] == ai->track) at = j;
	}
	if (at < 0) return;
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/player", 0777);
	FILE *f = fopen(RESUME, "w");
	if (!f) return;
	fprintf(f, "pocket 1\npos %.2f\nqpos %d\nsrc %d %s\n", ai->pos, at, src_kind, src_name);
	for (int k = 0; k < qn; k++) fprintf(f, "o %s\n", lib.t[q_orig[k]].path);
	for (int k = 0; k < qn; k++)
		for (int j = 0; j < qn; j++)
			if (q_orig[j] == q_play[k]) { fprintf(f, "p %d\n", j); break; }
	fclose(f);
}

static Group *kind_groups(int kind, int *n) {
	if (kind == T_ALBUMS) { *n = lib.nal; return lib.albums; }
	if (kind == T_ARTISTS) { *n = lib.nar; return lib.artists; }
	if (kind == T_PLAYLISTS) { *n = lib.nl; return lib.lists; }
	if (kind == K_AP) { *n = ap_g.name ? 1 : 0; return &ap_g; }
	*n = 0;
	return NULL;
}

static bool load_resume(void) {
	FILE *f = fopen(RESUME, "r");
	if (!f) return false;
	char l[1024];
	double pos = 0;
	int at = 0, no = 0, np = 0, kind = -1, v;
	int *orig = malloc((lib.nt + 1) * sizeof(int)), *map = NULL, *play = malloc((lib.nt + 1) * sizeof(int));
	int nmap = 0;
	bool same = false, ok = fgets(l, sizeof l, f) && !strcmp(l, "pocket 1\n");
	while (ok && fgets(l, sizeof l, f)) {
		l[strcspn(l, "\r\n")] = 0;
		if (sscanf(l, "pos %lf", &pos) == 1 || sscanf(l, "qpos %d", &at) == 1) continue;
		if (!strncmp(l, "src ", 4) && sscanf(l + 4, "%d %n", &kind, &v) == 1) {
			snprintf(src_name, sizeof src_name, "%s", l + 4 + v);
		} else if (!strncmp(l, "o ", 2)) {   // a song that's gone off the card maps to -1
			map = realloc(map, (nmap + 1) * sizeof(int));
			int i = library_find(l + 2);
			map[nmap++] = i >= 0 ? no : -1;
			if (i >= 0 && no < lib.nt) orig[no++] = i;
		} else if (sscanf(l, "p %d", &v) == 1) {
			bool here = v >= 0 && v < nmap && map[v] >= 0 && np < lib.nt;
			if (v == at) { at = -2 - np; same = here; }   // where the saved song lands once the gone ones drop out
			if (here) play[np++] = orig[map[v]];
		}
	}
	fclose(f);
	free(map);
	ok = ok && np == no && no > 0;
	// at went from a position in the saved order to -2 - (where it is now); if its song is gone
	// we take the one that slid into its place, which is the one after it
	int qp = at <= -2 ? -2 - at : 0;
	if (ok && qp >= np) qp = 0;
	if (ok) {
		LightLock_Lock(&qlock);
		free(q_orig); free(q_play);
		q_orig = orig; q_play = play; qn = no; qpos = qp; qadd = 0;
		LightLock_Unlock(&qlock);
		src_kind = kind == K_APQ ? K_APQ : -1;   // an artist page's queue is known by its name alone
		src_id = 0;
		int n;
		Group *g = kind_groups(kind, &n);
		for (int k = 0; k < n; k++)
			if (!strcmp(g[k].name, src_name)) { src_kind = kind; src_id = k; break; }
		rg_sync();
		audio_play_at(q_play[qpos], same ? pos : 0, true);   // the song itself survived: pick up at the second
	} else { free(orig); free(play); }
	return ok;
}

// ---- views -------------------------------------------------------------------------------

static const char *TAB[] = {"Songs", "Albums", "Artists", "Playlists"};
static bool swapped = true;   // the player on the touchscreen, the list up top
static float list_w(void) { return swapped ? 400 : 320; }
static float list1(void) { return swapped ? 240 : 190; }   // up top there's no mini player
static int tab, detail = -1, dkind;    // detail: an album, artist or playlist opened over the tab
static char query[64];
static int *items, nitems;
static bool dirty = true;
static float scroll[T_HOME + 1], vel;

// The D-pad's scrolling: whatever it moves eases there instead of jumping, a quarter of the way a
// frame (about a fifth of a second). A finger, a new page or a jump drops everything in flight.
static struct { float *p, to; } eases[6];
static int neases;
static float ease_cur(float *p) { for (int i = 0; i < neases; i++) if (eases[i].p == p) return eases[i].to; return *p; }   // where it's headed
static void ease_to(float *p, float to) {
	int i = 0;
	while (i < neases && eases[i].p != p) i++;
	if (i == neases) { if (neases == 6) { *p = to; return; } neases++; }
	eases[i].p = p;
	eases[i].to = to;
}
static void ease_step(void) {
	for (int i = 0; i < neases;) {
		float d = eases[i].to - *eases[i].p;
		if (fabsf(d) < 0.5f) { *eases[i].p = eases[i].to; eases[i] = eases[--neases]; continue; }
		*eases[i].p += d * 0.25f;
		i++;
	}
}
static void ease_clear(void) { neases = 0; }
static int cur[T_HOME + 1];
#define NSET 8   // the Settings page's rows (drawer.inc)
static char dev_name[64] = "3DStereo";   // Settings: what Spotify calls this 3DS
static bool extra_details;   // Settings: the file's format and LOSSLESS on the player (off: just the song)
static void draw_set_row(int k, float y);
static void set_activate(int k, int dir);
static bool cursor_vis;
static int stat_kind;              // the stats page: 0 songs, 1 artists, 2 albums, by listens
static int stats_from = T_SONGS;   // the tab it was opened from, and goes back to
static u32 *sval;                  // listens and seconds per item on the stats page
static float *ssec;
static int by_listens(const void *a, const void *b) {
	int x = *(const int *)a, y = *(const int *)b;
	if (sval[x] != sval[y]) return sval[x] < sval[y] ? 1 : -1;
	if (ssec[x] != ssec[y]) return ssec[x] < ssec[y] ? 1 : -1;
	return x - y;
}

static int V(void) { return detail >= 0 ? V_DETAIL : tab; }
static bool songs_view(void) { return detail >= 0 || tab == T_SONGS; }
static bool match(const char *s) { return !query[0] || strcasestr(s, query); }
static Group *dgroup(void) { int n; return &kind_groups(dkind, &n)[detail]; }
// Where the rows are. On a tab they start under the 30 px header; an album or playlist page
// (detailpage.inc) puts its header, its buttons and (up top) the column names over them, and all of
// it scrolls together. Content coordinates: a row's screen y is its row_top minus the scroll.
static bool rich(void) { return detail >= 0 && (dkind == T_ALBUMS || dkind == T_PLAYLISTS); }
#define DP_ACT 40
static float dp_hero(float W) { return W > 320 ? 136 : 104; }
static float dp_cols(float W) { return W > 320 ? 22 : 0; }
static float rows_base(float W) { return rich() ? dp_hero(W) + DP_ACT + dp_cols(W) : LIST0; }
static float rows_foot(void) { return rich() ? (dkind == T_ALBUMS ? 44 : 8) : 0; }
// The three tabs of things (Playlists, Artists, Albums) are a grid of covers, names under them;
// Songs is rows with room to breathe, under a letter each when it's A to Z, with the letters down
// the right edge to jump by. Everything else (pages, charts, settings) is ROW-high rows.
enum { LAY_ROWS, LAY_GRID, LAY_SONGS };
static int lay(void) { return detail >= 0 ? LAY_ROWS : tab == T_ALBUMS || tab == T_ARTISTS || tab == T_PLAYLISTS ? LAY_GRID : tab == T_SONGS ? LAY_SONGS : LAY_ROWS; }
#define G_TOP (LIST0 + 8)
#define SROW 40   // a song, on the Songs tab
#define LETH 30   // ...and a letter over the ones it starts (22 and 13 px until build 61: too small to read)
static int g_cols(float W) { return W > 320 ? 4 : 3; }
static float g_cw(float W) { int c = g_cols(W); return (W - 24 - 12 * (c - 1)) / c; }
static float g_pitch(float W) { return g_cw(W) + (tab == T_ARTISTS ? 30 : 40); }   // the cover, a name (and a line), the gap
static float *ytop;   // Songs: each row's top, letters counted in
static float songs_end;
static bool letters;  // Songs, A to Z: the letters are shown
static char letter_of(int t) { u8 c = (u8)lib.t[t].title[0]; if (c >= 'a' && c <= 'z') c -= 32; return c >= 'A' && c <= 'Z' ? c : '#'; }

// item k's box in content coordinates (its screen y is y minus the scroll), on the list's screen
static void box(int k, float *x, float *y, float *w, float *h) {
	float W = list_w();
	int m = lay();
	if (m == LAY_GRID) {
		int c = g_cols(W);
		float cw = g_cw(W);
		*x = 12 + (k % c) * (cw + 12); *y = G_TOP + (k / c) * g_pitch(W); *w = cw; *h = g_pitch(W) - 8;
	} else if (m == LAY_SONGS && ytop) { *x = 0; *y = ytop[k]; *w = W; *h = SROW; }
	else { *x = 0; *y = rows_base(W) + k * ROW; *w = W; *h = ROW; }
}
static float row_top(int k) { float x, y, w, h; box(k, &x, &y, &w, &h); return y; }
static float row_h(void) { int m = lay(); return m == LAY_SONGS ? SROW : m == LAY_GRID ? g_pitch(list_w()) - 8 : ROW; }
static int row_at(float x, float y, int v) {   // the item under a touch, -1 for none
	int m = lay();
	float cy = y + scroll[v];
	if (m == LAY_GRID) {
		float W = list_w(), cw = g_cw(W);
		int c = g_cols(W), col = (int)floorf((x - 12) / (cw + 12)), r = (int)floorf((cy - G_TOP) / g_pitch(W));
		if (x < 12 || col >= c || x > 12 + col * (cw + 12) + cw || cy < G_TOP || cy - G_TOP - r * g_pitch(W) > g_pitch(W) - 8) return -1;
		int k = r * c + col;
		return k < nitems ? k : -1;
	}
	if (m == LAY_SONGS && ytop) {   // the last row starting above it, if it reaches that far
		int lo = 0, hi = nitems - 1, k = -1;
		while (lo <= hi) { int mid = (lo + hi) / 2; if (ytop[mid] <= cy) { k = mid; lo = mid + 1; } else hi = mid - 1; }
		return k >= 0 && cy < ytop[k] + SROW ? k : -1;
	}
	float f = (cy - rows_base(list_w())) / ROW;
	int k = (int)floorf(f);
	return f < 0 || k >= nitems ? -1 : k;
}
static bool row_shown(int v, int k) { float t = row_top(k) - ease_cur(&scroll[v]); return t >= LIST0 && t + row_h() <= LIST1; }
static int first_shown(int v) {
	for (int k = 0; k < nitems; k++) if (row_top(k) - scroll[v] >= LIST0 - 1) return k;
	return nitems ? nitems - 1 : 0;
}
static void reveal(int v) {   // scroll so the cursor's item is in full view (eased there)
	if (rich() && cur[v] == 0) { ease_to(&scroll[v], 0); return; }   // the first song brings the header back with it
	float t = row_top(cur[v]), h = row_h(), sc = ease_cur(&scroll[v]);
	if (lay() == LAY_SONGS && letters && (cur[v] == 0 || letter_of(items[cur[v]]) != letter_of(items[cur[v] - 1]))) t -= LETH;   // its letter too
	if (lay() == LAY_GRID && cur[v] < g_cols(list_w())) t = 0;   // the top row: right to the top
	if (t - sc < LIST0) sc = t - LIST0;
	if (t + h - sc > LIST1) sc = t + h - LIST1;
	ease_to(&scroll[v], sc < 0 ? 0 : sc);
}
static float max_scroll(void) {
	float W = list_w(), m;
	int l = lay();
	if (l == LAY_GRID) m = G_TOP + (nitems + g_cols(W) - 1) / g_cols(W) * g_pitch(W) + 4 - LIST1;
	else if (l == LAY_SONGS && ytop) m = songs_end + 6 - LIST1;
	else m = rows_base(W) + nitems * ROW + rows_foot() - LIST1;
	return m > 0 ? m : 0;
}
static void reveal_top(int v) {   // Songs, a letter jumped to: its letter at the top of the screen
	float sc = row_top(cur[v]) - LETH - LIST0, m = max_scroll();
	ease_to(&scroll[v], sc < 0 ? 0 : sc > m ? m : sc);
}
static void songs_layout(void) {   // ytop for the Songs tab as it is now
	ytop = realloc(ytop, (nitems + 1) * sizeof *ytop);
	float y = LIST0 + 2;
	for (int k = 0; k < nitems; k++) {
		if (letters && (k == 0 || letter_of(items[k]) != letter_of(items[k - 1]))) y += LETH;
		ytop[k] = y;
		y += SROW;
	}
	songs_end = y;
}
static void dp_summary(void);   // detailpage.inc

// Pinned playlists, by name, the newest first: they sit under Liked Songs. A file of their own,
// a name a line, since a name can be longer than a settings line.
#define PINS "sdmc:/3ds/player/pins"
static char **pins;
static int npins;
static void pins_load(void) {
	FILE *f = fopen(PINS, "r");
	if (!f) return;
	char l[512];
	while (fgets(l, sizeof l, f)) {
		l[strcspn(l, "\r\n")] = 0;
		if (!*l) continue;
		pins = realloc(pins, (npins + 1) * sizeof *pins);
		pins[npins++] = strdup(l);
	}
	fclose(f);
}
static void pins_save(void) {
	FILE *f = fopen(PINS, "w");
	if (!f) return;
	for (int k = 0; k < npins; k++) fprintf(f, "%s\n", pins[k]);
	fclose(f);
}
static int pin_of(const char *name) {
	for (int k = 0; k < npins; k++) if (!strcmp(pins[k], name)) return k;
	return -1;
}
static bool pin_toggle(const char *name) {   // true: pinned now
	int k = pin_of(name);
	if (k >= 0) {
		free(pins[k]);
		memmove(pins + k, pins + k + 1, (npins - k - 1) * sizeof *pins);
		npins--;
	} else {
		pins = realloc(pins, (npins + 1) * sizeof *pins);
		memmove(pins + 1, pins, npins * sizeof *pins);
		pins[0] = strdup(name);
		npins++;
	}
	pins_save();
	return k < 0;
}

// Each tab's order, SELECT+Y round them (Liked Songs and pinned playlists stay on top in all of them)
static void say(const char *s);
enum { NSORT_MAX = 5 };
static const char *SORTS[4][NSORT_MAX] = {
	{"Alphabetical", "Recently added", "Artist", "Album", "Genre"},
	{"Artist", "Alphabetical", "Recently added", "Year"},   // Artist: as they're kept, by artist then year
	{"Alphabetical", "Recently added", "Most songs"},
	{"Alphabetical", "Recently updated", "Creator"},
};
static const int NSORT[4] = {5, 4, 3, 3};
static int sort_by[4] = {1, 2, 1, 1};   // Recently added (Songs, Albums, Artists), Recently updated (Playlists)
static int recent(int t) { return t == T_ALBUMS ? 2 : 1; }   // which of a tab's orders is "recently added"
static bool ap_active(void);
static u64 *grp_new;   // per group of the tab being sorted: its newest song's date (or a playlist's latest add)

static int scmp(const char *a, const char *b) { return strcasecmp(a ? a : "\x7f", b ? b : "\x7f"); }   // missing ones last
static int by_track_order(int a, int b) {   // album, then disc and number
	Track *x = &lib.t[a], *y = &lib.t[b];
	int c = scmp(x->album, y->album);
	if (c) return c;
	if (x->disc != y->disc) return x->disc - y->disc;
	return x->no - y->no;
}
static int cmp_song(const void *pa, const void *pb) {
	int a = *(const int *)pa, b = *(const int *)pb, c = 0;
	Track *x = &lib.t[a], *y = &lib.t[b];
	switch (sort_by[T_SONGS]) {
	case 1: if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1; break;
	case 2: if ((c = scmp(x->artist, y->artist)) || (c = by_track_order(a, b))) return c; break;
	case 3: if ((c = by_track_order(a, b))) return c; break;
	case 4: if ((c = scmp(x->genre, y->genre)) || (c = scmp(x->artist, y->artist)) || (c = by_track_order(a, b))) return c; break;
	}
	return (c = scmp(x->title, y->title)) ? c : a - b;
}
static int cmp_group(const void *pa, const void *pb) {
	int a = *(const int *)pa, b = *(const int *)pb, c = 0, m = sort_by[tab];
	Group *g = tab == T_ALBUMS ? lib.albums : tab == T_ARTISTS ? lib.artists : lib.lists;
	if (m == recent(tab) && grp_new[a] != grp_new[b]) return grp_new[a] < grp_new[b] ? 1 : -1;
	if (tab == T_ALBUMS && m == 3) {
		int ya = g[a].n ? lib.t[g[a].songs[0]].year : 0, yb = g[b].n ? lib.t[g[b].songs[0]].year : 0;
		if (ya != yb) return yb - ya;   // newest first
	}
	if (tab == T_ARTISTS && m == 2 && g[a].n != g[b].n) return g[b].n - g[a].n;
	if (tab == T_PLAYLISTS && m == 2 && (c = scmp(g[a].owner, g[b].owner))) return c;
	return (c = scmp(g[a].name, g[b].name)) ? c : a - b;
}
static void sort_groups(int from, int to) {   // items[from..to) of the tab, in its order
	if (to - from < 2) return;
	int n = 0;
	Group *g = tab == T_ALBUMS ? lib.albums : tab == T_ARTISTS ? lib.artists : lib.lists;
	n = tab == T_ALBUMS ? lib.nal : tab == T_ARTISTS ? lib.nar : lib.nl;
	if (tab == T_ALBUMS && sort_by[tab] == 0) return;   // already in that order
	if (sort_by[tab] == recent(tab)) {
		grp_new = calloc(n + 1, sizeof *grp_new);
		for (int k = from; k < to; k++) {
			Group *x = &g[items[k]];
			for (int j = 0; j < x->n; j++) {
				// a list: the newest date a song went in (yyyymmdd; none known sorts last); else the newest file
				u64 v = tab == T_PLAYLISTS ? (x->added ? x->added[j] : 0) : lib.t[x->songs[j]].mtime;
				if (v > grp_new[items[k]]) grp_new[items[k]] = v;
			}
		}
	}
	qsort(items + from, to - from, sizeof(int), cmp_group);
	free(grp_new);
	grp_new = NULL;
}
static void next_sort(void) {   // SELECT+Y: on the four tabs only, not over a page or on the charts
	if (detail >= 0 || tab >= 4 || ap_active()) return;
	sort_by[tab] = (sort_by[tab] + 1) % NSORT[tab];
	ease_clear();
	scroll[tab] = 0;
	cur[tab] = 0;
	dirty = true;
	char s[48];
	snprintf(s, sizeof s, "Sorted by %s", SORTS[tab][sort_by[tab]]);
	say(s);
}

static void rebuild(void) {
	items = realloc(items, (lib.nt + lib.nal + lib.nar + lib.nl + NSET + 1) * sizeof(int));   // NSET: Settings, even with no music
	nitems = 0;
	if (detail >= 0) {
		Group *g = dgroup();
		for (int k = 0; k < g->n; k++) items[nitems++] = g->songs[k];
	} else if (tab == T_SONGS) {
		for (int k = 0; k < lib.nt; k++) {
			Track *t = &lib.t[lib.by_title[k]];
			if (match(t->title) || match(t->artist) || match(t->album) || (t->genre && match(t->genre))) items[nitems++] = lib.by_title[k];
		}
		if (sort_by[T_SONGS]) qsort(items, nitems, sizeof(int), cmp_song);
		letters = sort_by[T_SONGS] == 0;   // A to Z: under letters
		songs_layout();
	} else if (tab == T_ALBUMS) {
		for (int a = 0; a < lib.nal; a++) if (match(lib.albums[a].name) || match(lib.albums[a].artist)) items[nitems++] = a;
		sort_groups(0, nitems);
	} else if (tab == T_ARTISTS) {
		for (int a = 0; a < lib.nar; a++) if (match(lib.artists[a].name)) items[nitems++] = a;
		sort_groups(0, nitems);
	} else if (tab == T_STATS) {   // everything heard at least once, most listened first
		int n = stat_kind == 0 ? lib.nt : stat_kind == 1 ? lib.nar : lib.nal;
		sval = realloc(sval, (n + 1) * sizeof *sval);
		ssec = realloc(ssec, (n + 1) * sizeof *ssec);
		for (int a = 0; a < n; a++) {
			if (!stat_kind) { sval[a] = stats_plays(a); ssec[a] = stats_secs(a); continue; }
			Group *g = stat_kind == 1 ? &lib.artists[a] : &lib.albums[a];
			sval[a] = 0;
			ssec[a] = 0;
			for (int k = 0; k < g->n; k++) { sval[a] += stats_plays(g->songs[k]); ssec[a] += stats_secs(g->songs[k]); }
		}
		for (int a = 0; a < n; a++) if (sval[a]) items[nitems++] = a;
		qsort(items, nitems, sizeof(int), by_listens);
	} else if (tab == T_SETTINGS) {
		for (int k = 0; k < NSET; k++) items[nitems++] = k;
	} else if (tab == T_HOME) {
		home_build();   // its own items, not rows
	} else {   // Liked Songs, then the pinned ones (the newest pin on top), then the rest as they come
		bool *in = calloc(lib.nl + 1, 1);
		if (lib.nl) { in[0] = true; if (match(lib.lists[0].name)) items[nitems++] = 0; }
		for (int p = 0; p < npins; p++)
			for (int a = 1; a < lib.nl; a++)
				if (!in[a] && !strcmp(lib.lists[a].name, pins[p])) { in[a] = true; if (match(lib.lists[a].name)) items[nitems++] = a; break; }
		int rest = nitems;
		for (int a = 1; a < lib.nl; a++) if (!in[a] && match(lib.lists[a].name)) items[nitems++] = a;
		sort_groups(rest, nitems);
		free(in);
	}
	int v = V();
	if (cur[v] >= nitems) cur[v] = nitems ? nitems - 1 : 0;
	if (scroll[v] > max_scroll()) scroll[v] = max_scroll();
	if (rich()) dp_summary();   // its count and length, with the songs as they are now
	dirty = false;
}

static void dp_open(void);
// An album's or playlist's page: the cursor on its buttons (play, shuffle, the check, the dots),
// which, from 0, or -1 when it's on the songs. Up from the first song gets there; a page opens on play.
static int dp_btn = -1;
static int pm;                // the dots' menu or a box of theirs is open (pagemenu.inc)
static void pm_open(void);
static void open_detail(int kind, int g) { ease_clear(); dp_btn = kind == T_ALBUMS || kind == T_PLAYLISTS ? 0 : -1; dkind = kind; detail = g; scroll[V_DETAIL] = 0; cur[V_DETAIL] = 0; vel = 0; dirty = true; dp_open(); }
static void close_detail(void) { ease_clear(); dp_btn = -1; pm = 0; detail = -1; vel = 0; dirty = true; }
static void set_tab(int t) { ease_clear(); ap_clear(); if (detail >= 0) close_detail(); if (t != tab) { tab = t; vel = 0; dirty = true; } }

// The stats page slides in from the left and the tab it came from slides back from the right.
// While they move, the page being left is drawn beside the new one, a page's width away.
static int *items2, nitems2, nb_tab = -1, nb_side;   // that neighbour, and its side: -1 left, 1 right
static float hoff, hoff_v;   // where the current page sits sideways, and how fast it's going
static bool hdrag;           // a finger's dragging it
static void swap_pages(void) {
	int *p = items; items = items2; items2 = p;
	int n = nitems; nitems = nitems2; nitems2 = n;
	int t = tab; tab = nb_tab; nb_tab = t;
}
static void build_neighbour(int t, int side) {
	nb_tab = t;
	nb_side = side;
	swap_pages();
	bool was_dirty = dirty;
	rebuild();
	dirty = was_dirty;
	swap_pages();
}
static void page_commit(void) {   // the neighbour becomes the page; the old one goes round the other side
	swap_pages();
	if ((tab == T_STATS || tab == T_SETTINGS) && (nb_tab < NTAB || nb_tab == T_HOME)) stats_from = nb_tab;   // the tab to go back to
	hoff += nb_side * list_w();
	nb_side = -nb_side;
	vel = 0;
}
static void page_to(int t) {
	ease_clear();
	ap_clear();
	if (detail >= 0) close_detail();
	if (dirty) rebuild();
	if (t == tab) return;
	build_neighbour(t, t >= T_STATS ? -1 : 1);
	hoff = hoff_v = 0;
	page_commit();
	hoff = hoff_v = 0;   // there at once, no slide: the side menu is the motion
	nb_tab = -1;
}

static void play_list(int start) {
	if (!nitems) return;
	if (detail >= 0) { snprintf(src_name, sizeof src_name, "%s", dkind == K_AP ? ap_gkey : dgroup()->name); src_kind = dkind; src_id = detail; }
	else { snprintf(src_name, sizeof src_name, "%s", query[0] ? "YOUR SEARCH" : "YOUR LIBRARY"); src_kind = src_id = -1; }
	set_queue(items, nitems, start);
}

static bool playing_here(void) { return detail >= 0 && src_kind == dkind && src_id == detail && (dkind != K_AP || !strcmp(src_name, ap_gkey)); }

static void activate(int k, const AudioInfo *ai) {
	if (k < 0 || k >= nitems) return;
	if (detail < 0 && tab == T_SETTINGS) { set_activate(items[k], 1); return; }
	if (detail < 0 && tab == T_STATS) {   // a top song plays the chart from there; the rest open
		if (stat_kind == 1) ap_open_local(items[k]);
		else if (stat_kind) open_detail(T_ALBUMS, items[k]);
		else if (ai->open && items[k] == ai->track) audio_pause(!ai->paused);
		else { play_list(k); snprintf(src_name, sizeof src_name, "YOUR TOP SONGS"); }
		return;
	}
	if (songs_view()) {
		if (ai->open && items[k] == ai->track) audio_pause(!ai->paused);
		else play_list(k);
	} else if (tab == T_ARTISTS) ap_open_local(items[k]);
	else open_detail(tab, items[k]);
}

static void search(void) {
	SwkbdState kb;
	char buf[64];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof buf - 1);
	swkbdSetHintText(&kb, "Songs, albums, artists, playlists");
	swkbdSetInitialText(&kb, query);
	if (swkbdInputText(&kb, buf, sizeof buf) == SWKBD_BUTTON_CONFIRM) {
		snprintf(query, sizeof query, "%s", buf);
		ap_clear();
		if (detail >= 0) close_detail();
		if (tab == T_HOME && query[0]) tab = T_SONGS;   // Home has nothing to filter: the songs do
		ease_clear();
		scroll[tab] = 0;
		cur[tab] = 0;
		dirty = true;
	}
}

// ---- settings ----------------------------------------------------------------------------

#define CFG "sdmc:/3ds/player.cfg"
static int quality = Q_BEST;

static int sort_v;   // 2: the orders in the cfg were picked after build 60 made recent the default
static void load_cfg(void) {
	FILE *f = fopen(CFG, "r");
	if (!f) return;
	char l[160];
	int v;
	while (fgets(l, sizeof l, f)) {
		if (sscanf(l, "quality=%d", &v) == 1 && v >= 0 && v < Q_COUNT) quality = v;
		if (sscanf(l, "shuffle=%d", &v) == 1) shuffle = v;
		if (sscanf(l, "repeat=%d", &v) == 1 && v >= 0 && v < 3) repeat = v;
		if (sscanf(l, "swap=%d", &v) == 1) swapped = v;
		if (sscanf(l, "details=%d", &v) == 1) extra_details = v;
		if (sscanf(l, "sortv=%d", &v) == 1) sort_v = v;
		if (!strncmp(l, "devname=", 8)) { snprintf(dev_name, sizeof dev_name, "%.63s", l + 8); dev_name[strcspn(dev_name, "\r\n")] = 0; }
		if (sscanf(l, "replaygain=%d", &v) == 1 && v >= 0 && v <= RG_AUTO) rg_cfg = v;
		int s0, s1, s2, s3;
		if (sscanf(l, "sort=%d,%d,%d,%d", &s0, &s1, &s2, &s3) == 4) {
			int s4[4] = {s0, s1, s2, s3};
			for (int k = 0; k < 4; k++) sort_by[k] = s4[k] >= 0 && s4[k] < NSORT[k] ? s4[k] : 0;
		}
	}
	fclose(f);
	if (sort_v < 2) { static const int RECENT[4] = {1, 2, 1, 1}; memcpy(sort_by, RECENT, sizeof RECENT); }   // older cfg: the new defaults
}
// What Spotify calls this 3DS when nobody has named it: the console's own name, its model. The old default (3DStereo)
// counts as nobody having named it; a name typed in Settings stays.
static void dev_name_system(void) {
	static const char *OLD[] = {"3DStereo", "Nintendo 3DS", "Nintendo 3DS XL", "New Nintendo 3DS", "Nintendo 2DS", "New Nintendo 3DS XL", "New Nintendo 2DS XL"};   // earlier defaults
	bool unset = false;
	for (int k = 0; k < 7; k++) if (!strcmp(dev_name, OLD[k])) unset = true;
	if (!unset) return;
	static const char *MODEL[] = {"3DS", "3DS XL", "n3DS", "2DS", "n3DS XL", "n2DS XL"};   // CFGU's model numbers, in order
	u8 m = 0xff;
	u16 nm[16] = {0};
	char user[64] = "";
	if (R_SUCCEEDED(cfguInit())) {
		if (R_FAILED(CFGU_GetSystemModel(&m))) m = 0xff;
		if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0xA0000, nm))) {   // the profile name set in System Settings: 11 UTF-16 letters
			nm[11] = 0;
			if (utf16_to_utf8((uint8_t *)user, nm, sizeof user - 1) < 0) user[0] = 0;
		}
		cfguExit();
	}
	if (m >= 6) return;
	if (user[0]) snprintf(dev_name, sizeof dev_name, "%s's %s", user, MODEL[m]);
	else snprintf(dev_name, sizeof dev_name, "%s", MODEL[m]);
}
static void save_cfg(void) {
	FILE *f = fopen(CFG, "w");
	if (!f) return;
	fprintf(f, "quality=%d\nshuffle=%d\nrepeat=%d\nreplaygain=%d\nswap=%d\nsort=%d,%d,%d,%d\ndetails=%d\nsortv=2\ndevname=%s\n", quality, shuffle, repeat, rg_cfg, swapped,
	        sort_by[0], sort_by[1], sort_by[2], sort_by[3], extra_details, dev_name);
	fclose(f);
}

// ---- drawing -----------------------------------------------------------------------------

static C3D_RenderTarget *top, *bot;
static const char *QNAME[] = {"STOCK", "GOOD", "BEST"};

// A setting just changed: a pill drops in at the top of the player, sits, and floats off.
static struct { char s[48]; int q; u64 t0; } note;   // q >= 0: a quality badge, else just s
// a heart; pop runs 1 to 0 just after a like, for a little swell
static void ic_heart(float x, float y, float s, u32 c) {
	C2D_DrawCircleSolid(x - s * .5f, y - s * .25f, 0, s * .55f, c);
	C2D_DrawCircleSolid(x + s * .5f, y - s * .25f, 0, s * .55f, c);
	C2D_DrawTriangle(x - s * 1.04f, y - s * .1f, c, x + s * 1.04f, y - s * .1f, c, x, y + s * .95f, c, 0);
}
static u64 like_t0;
static float like_pop(void) {
	u64 d = osGetTime() - like_t0;
	return like_t0 && d < 260 ? 1 - d / 260.0f : 0;
}

static void say(const char *s) { snprintf(note.s, sizeof note.s, "%.47s", s); note.q = -1; note.t0 = osGetTime(); }   // a toast: a long name is cut
static void say_quality(int q) { note.q = q; note.s[0] = 0; note.t0 = osGetTime(); }

// ---- "queue this" (circle pad down) ------------------------------------------------------------------------
// What the 3DS plays next (up to 4), the song before this one, and how many of the next were queued by hand.
static int upcoming_get(int *up, int *prev, int *nq) {
	int nu = 0;
	*prev = -1;
	LightLock_Lock(&qlock);
	for (int k = 1; k <= 4 && qn > 1; k++) { int at = qpos + k; if (at >= qn) { if (repeat != 1) break; at %= qn; } up[nu++] = q_play[at]; }
	if (qn > 1) *prev = qpos > 0 ? q_play[qpos - 1] : repeat == 1 ? q_play[qn - 1] : -1;
	*nq = qadd < nu ? qadd : nu;
	LightLock_Unlock(&qlock);
	return nu;
}
// Out of the queue, and moved within it. k counts from the song after the playing one, the way the queue page lists
// them. Moving into the leading queued songs queues the song, out of them un-queues it.
static void queue_changed(void) {
	int up[4], prev, nq, nu = upcoming_get(up, &prev, &nq);
	connect_upcoming(up, nu, nq);   // Spotify hears it, if it's following the 3DS
}
// The last song taken out, kept for a few seconds so a swipe back can put it where it was. It goes back only if the
// queue still looks the way it did right after it left (same length, same neighbours): anything else has moved on.
#define UNDO_MS 10000
static struct { bool on, spot, queued; u64 t0; int at, j, v, n, left, right; ConnectUndo cu; } q_undo;
static bool queue_undo_ok(void) { return q_undo.on && osGetTime() - q_undo.t0 < UNDO_MS; }
static void queue_remove(int k) {
	if (connect_driving()) {
		ConnectUndo cu;
		if (connect_queue_remove(k, &cu)) { q_undo.on = q_undo.spot = true; q_undo.cu = cu; q_undo.t0 = osGetTime(); }
		return;
	}
	LightLock_Lock(&qlock);
	int at = qpos + 1 + k;
	if (at >= qn) { if (repeat != 1 || qn < 2) { LightLock_Unlock(&qlock); return; } at %= qn; }
	if (k < 0 || at == qpos) { LightLock_Unlock(&qlock); return; }   // never the one playing
	int v = q_play[at], oj = -1;
	memmove(q_play + at, q_play + at + 1, (qn - at - 1) * sizeof(int));
	for (int j = 0; j < qn; j++) if (q_orig[j] == v) { oj = j; memmove(q_orig + j, q_orig + j + 1, (qn - j - 1) * sizeof(int)); break; }
	qn--;
	q_undo.on = true; q_undo.spot = false; q_undo.t0 = osGetTime();
	q_undo.at = at; q_undo.j = oj; q_undo.v = v; q_undo.n = qn; q_undo.queued = k < qadd;
	q_undo.left = at > 0 ? q_play[at - 1] : -1;
	q_undo.right = at < qn ? q_play[at] : -1;
	if (at < qpos) qpos--;
	if (k < qadd) qadd--;
	LightLock_Unlock(&qlock);
	queue_changed();
	static bool hinted;
	if (!hinted) { hinted = true; say("Removed. Swipe right to undo"); }
}
// Swipe back: the last removed song returns to its place. The place it came back at, counted like k, or -1.
static int queue_restore(void) {
	bool ok = queue_undo_ok();
	q_undo.on = false;
	if (!ok) return -1;
	if (q_undo.spot) return connect_driving() ? connect_queue_restore(&q_undo.cu) : -1;
	if (connect_driving()) return -1;
	int k = -1;
	LightLock_Lock(&qlock);
	int at = q_undo.at;
	bool fits = qn == q_undo.n && at <= qn && (at > 0 ? q_play[at - 1] == q_undo.left : q_undo.left < 0) &&
	            (at < qn ? q_play[at] == q_undo.right : q_undo.right < 0);
	if (fits) {
		int *a = realloc(q_play, (qn + 1) * sizeof(int)), *o = realloc(q_orig, (qn + 1) * sizeof(int));
		if (a) q_play = a;
		if (o) q_orig = o;
		fits = a && o;
	}
	if (fits) {
		memmove(q_play + at + 1, q_play + at, (qn - at) * sizeof(int));
		q_play[at] = q_undo.v;
		int j = q_undo.j >= 0 && q_undo.j <= qn ? q_undo.j : qn;
		memmove(q_orig + j + 1, q_orig + j, (qn - j) * sizeof(int));
		q_orig[j] = q_undo.v;
		qn++;
		if (at <= qpos) qpos++;
		k = at > qpos ? at - qpos - 1 : qn - 1 - qpos + at;
		if (q_undo.queued && at > qpos && k <= qadd) qadd++;
	}
	LightLock_Unlock(&qlock);
	if (k >= 0) queue_changed();
	return k;
}
static void queue_move(int from, int to) {
	if (from == to || from < 0 || to < 0) return;
	if (connect_driving()) { connect_queue_move(from, to); return; }
	LightLock_Lock(&qlock);
	int a = qpos + 1 + from, b = qpos + 1 + to;
	if (a >= qn || b >= qn) { LightLock_Unlock(&qlock); return; }
	int v = q_play[a];
	if (a < b) memmove(q_play + a, q_play + a + 1, (b - a) * sizeof(int));
	else memmove(q_play + b + 1, q_play + b, (a - b) * sizeof(int));
	q_play[b] = v;
	if (!shuffle) {   // the order the songs go in is the order they're kept in
		if (a < b) memmove(q_orig + a, q_orig + a + 1, (b - a) * sizeof(int));
		else memmove(q_orig + b + 1, q_orig + b, (a - b) * sizeof(int));
		q_orig[b] = v;
	}
	int nq_ex = qadd - (from < qadd ? 1 : 0);
	bool q = from < qadd ? to <= nq_ex : to < nq_ex;
	qadd = nq_ex + (q ? 1 : 0);
	LightLock_Unlock(&qlock);
	queue_changed();
}
// Circle pad up, held three seconds: the queue goes back to its default, the songs queued by hand gone (Spotify's own
// queue too, while it drives). The playlist or album that's playing carries on as it was.
static void queue_clear(void) {
	if (connect_driving()) {
		int n = connect_queue_clear();
		say(n ? "Queue cleared" : "The queue is already empty");
		return;
	}
	LightLock_Lock(&qlock);
	int n = qadd;
	if (n > qn - qpos - 1) n = qn - qpos - 1;
	int *gone = n > 0 ? malloc(n * sizeof(int)) : NULL;
	if (n > 0 && gone) {
		memcpy(gone, q_play + qpos + 1, n * sizeof(int));
		memmove(q_play + qpos + 1, q_play + qpos + 1 + n, (qn - qpos - 1 - n) * sizeof(int));
		int len = qn;
		for (int g = 0; g < n; g++)
			for (int j = 0; j < len; j++)
				if (q_orig[j] == gone[g]) { memmove(q_orig + j, q_orig + j + 1, (len - j - 1) * sizeof(int)); len--; break; }
		qn -= n;
		qadd = 0;
	} else n = 0;
	free(gone);
	LightLock_Unlock(&qlock);
	if (n > 0) queue_changed();
	say(n ? "Queue cleared" : "The queue is already empty");
}
// These songs go in after the one playing and whatever was queued before them. While Spotify drives they go into
// Spotify's own queue; otherwise they're the 3DS's, and Spotify (when it follows the 3DS) shows them as its queue.
static void queue_add(const int *songs, int n) {
	if (n <= 0 || lib.nt <= 0) return;
	char b[48];
	if (!qn) {   // nothing's lined up: they just play
		snprintf(src_name, sizeof src_name, "YOUR QUEUE");
		src_kind = src_id = -1;
		set_queue(songs, n, 0);
		say(n == 1 ? "Playing it" : "Playing them");
		return;
	}
	if (connect_driving()) {
		int ok = 0;
		for (int k = 0; k < n; k++) if (connect_queue_add(songs[k])) ok++;
		if (ok) {
			if (n == 1) say("Added to Spotify's queue");
			else { snprintf(b, sizeof b, "Added %d songs to Spotify's queue", ok); say(b); }
			return;
		}
	}
	LightLock_Lock(&qlock);
	int *a = realloc(q_play, (qn + n) * sizeof(int)), *o = realloc(q_orig, (qn + n) * sizeof(int));
	if (a) q_play = a;
	if (o) q_orig = o;
	if (!a || !o) { LightLock_Unlock(&qlock); say("Out of memory"); return; }
	int at = qpos + 1 + qadd;
	if (at > qn) at = qn;
	int c = 0;
	while (c < qn && q_orig[c] != q_play[qpos]) c++;
	int ao = c + 1 + qadd;   // the same place in the unshuffled order, so un-shuffling keeps them near
	if (ao > qn) ao = qn;
	memmove(q_play + at + n, q_play + at, (qn - at) * sizeof(int));
	memcpy(q_play + at, songs, n * sizeof(int));
	memmove(q_orig + ao + n, q_orig + ao, (qn - ao) * sizeof(int));
	memcpy(q_orig + ao, songs, n * sizeof(int));
	qn += n;
	qadd += n;
	LightLock_Unlock(&qlock);
	int up[4], prev, nq, nu = upcoming_get(up, &prev, &nq);
	connect_upcoming(up, nu, nq);   // Spotify hears it, if it's following the 3DS
	if (n == 1) say("Added to the queue");
	else { snprintf(b, sizeof b, "Added %d songs to the queue", n); say(b); }
}
static bool note_live(void) { return note.t0 && osGetTime() - note.t0 < 1900; }

static void draw_note(float cx) {
	if (!note_live()) return;
	float t = (osGetTime() - note.t0) / 1000.0f, y = 6, a = 1;
	if (t < 0.28f) {   // in, with a little overshoot to land it
		float u = t / 0.28f - 1;
		y = -26 + 32 * (1 + 2.70158f * u * u * u + 1.70158f * u * u);
	} else if (t > 1.5f) {   // out: up a touch and gone
		float e = (t - 1.5f) / 0.4f;
		a = 1 - e;
		y = 6 - 9 * e * e;
	}
	u8 A = (u8)(255 * a);
	const char *s = note.q >= 0 ? (note.q == Q_STOCK ? "32.7 kHz out" : "47.6 kHz out") : note.s;
	float bw = note.q >= 0 ? measure(QNAME[note.q], 8) + 10 : 0, tw = measure(s, 10);
	float w = 20 + bw + (bw ? 7 : 0) + tw, x = cx - w / 2;
	rrect(x, y, w, 22, 11, rgba(0x000000, (u8)(170 * a)));
	if (bw) {
		u32 qc = note.q == Q_BEST ? GREEN : note.q == Q_GOOD ? 0xdddddd : 0x888888;
		rrect(x + 10, y + 4, bw, 14, 3, rgba(qc, A));
		text(QNAME[note.q], x + 15, y + 14.5f, 8, rgba(0, A), BOLD, 0);
	}
	text(s, x + 10 + bw + (bw ? 7 : 0), y + 15, 10, rgba(0xffffff, A), bw ? 0 : BOLD, 0);
}

#include "lyricsview.inc"
#include "queueview.inc"
#include "pairview.inc"
#include "clearview.inc"

// Now Playing, on whichever screen has it: 400 wide up top, or 320 on the touchscreen, where
// the cover shrinks to leave the words room and every control is a tap.
typedef struct { float W, cx, x0, cs, cy, tx, tw, bx, bw; } Layout;   // cy: the cover's top
static Layout P;           // the touchscreen's, for taps
static float scrub = -1;   // a finger on the progress bar: where it would land, 0..1
static Layout player_layout(float W) {
	bool wide = W > 320;
	Layout L = {.W = W, .cx = W / 2, .x0 = wide ? 20 : 12, .cs = wide ? ART_BIG : 112};
	// the cover and its words, centred between what's above them and the bar under them (Alex): up
	// top that's the screen's edge; on the touchscreen, the way-down and lyrics buttons (to y 27)
	L.cy = wide ? 30 : roundf((27 + 184 - L.cs) / 2);
	L.tx = L.x0 + L.cs + (wide ? 16 : 10);
	L.tw = W - L.tx - (wide ? 20 : 8);
	L.bx = L.x0;
	L.bw = W - 2 * L.x0;
	return L;
}

static void ic_down(float x, float y, u32 c) { C2D_DrawTriangle(x - 6, y - 3, c, x + 6, y - 3, c, x, y + 4, c, 0); }

static float art_y;   // where the artist's name sits on the touchscreen player: tap it for their page
static float like_y = 47;   // ...and the (+), beside the title
static int next_up = -1;   // the 3DS's own next song, for the player's Next line (-1: none, or Spotify decides)
static void draw_player(bool on_bot, float W, const AudioInfo *ai) {
	Layout L = player_layout(W);
	if (on_bot) P = L;
	float cx = L.cx, tx = L.tx, tw = L.tw;
	if (!lib.nt) {
		text("No songs yet", cx, 104, 18, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
		text("Put FLAC, MP3, OGG, OPUS or WAV files in sdmc:/music,", cx, 126, 10, rgb(GREY), C2D_AlignCenter, 0);
		text("then open 3DStereo again.", cx, 140, 10, rgb(GREY), C2D_AlignCenter, 0);
		return;
	}
	Track *t = ai->open ? &lib.t[ai->track] : NULL;
	bool lyrics = lyr_on && t;
	bool qview = q_on && t && !lyrics;
	u32 c0 = mixc(top_color, 0, 0.62f), lbg = mixc(top_color, 0, 0.78f);
	if (lyrics || qview) { rect(0, 0, W, 186, rgb(lbg)); C2D_DrawRectangle(0, 186, 0, W, 30, rgb(lbg), rgb(lbg), rgb(BG), rgb(BG)); }
	else C2D_DrawRectangle(0, 0, 0, W, 216, rgb(c0), rgb(c0), rgb(BG), rgb(BG));

	if (on_bot) {   // the way back down to the list, and the lyrics
		rrect(6, 5, 22, 22, 11, rgba(0, 70));
		ic_down(17, 16, rgb(0xffffff));
		lyr_chip(W, ai->open ? ai->track : -1);
		queue_chip(W);
	}
	if (ai->underruns) {
		char u[32];
		snprintf(u, sizeof u, "%lu dropouts", (unsigned long)ai->underruns);
		text(u, on_bot ? W - 80 : W - 8, 16, 8, rgba(0xff8080, 200), C2D_AlignRight, 0);
	}

	if (lyrics) {   // the song, small, up top; the words take the rest
		float x = on_bot ? 36 : 16, w = W - x - (on_bot ? 84 : 16);
		text(t->title, x, 15, 11, rgb(0xffffff), BOLD, w);
		text(t->artist, x, 27, 9, rgba(0xffffff, 180), 0, w);
		draw_lyrics(W, ai, 34, 182, lbg);
	} else if (qview) {   // the queue, in the same place
		text("Queue", on_bot ? 36 : 16, 21, 13, rgb(0xffffff), BOLD, W - (on_bot ? 120 : 32));
		draw_queue(W, ai, 34, 182);
	} else {

	// cover, with a soft shadow under it
	float x0 = L.x0, cs = L.cs, k = cs / ART_BIG;
	float cy = L.cy;
	rrect(x0 + 3, cy + 4, cs, cs, 5, rgba(0, 60));
	rrect(x0 + 1, cy + 2, cs, cs, 5, rgba(0, 60));
	if (t && big_alb >= 0 && big_none != ai->track) C2D_DrawImageAt((C2D_Image){&big_tex, &big_sub}, x0, cy, 0, NULL, k, k);
	else rrect(x0, cy, cs, cs, 4, rgb(t ? mixc(top_color, 0x282828, 0.5f) : 0x282828));

	if (!t) {
		text("Nothing playing", tx, L.cy + 32, 21, rgb(0xffffff), BOLD, tw);
		text(on_bot ? "Pick one up top: D-pad, then A." : "Pick a song on the bottom screen.", tx, L.cy + 52, 11, rgb(GREY), 0, tw);
	} else {
		// the words as one block, centred on the cover (build 58 left a hole where two lines went);
		// with Extra details on, the file's line under them is part of the block
		float nw = tw - 30, cyc = cy + L.cs / 2;
		int lines = measure(t->title, 21) > nw ? 2 : 1;
		// Extra details slides in and out: xd_v eases 0..1 with the setting (by time, so 30 fps idle draws match),
		// the block re-centres as the file's line grows, and the line itself rises and fades in
		static float xd_v = -1;
		static u64 xd_last;
		u64 xd_now = osGetTime();
		float xd_to = extra_details ? 1.0f : 0.0f;
		if (xd_v < 0) xd_v = xd_to;
		else {
			float dtm = xd_last ? (float)(xd_now - xd_last) : 16.0f;
			if (dtm > 100) dtm = 100;
			xd_v += (xd_to - xd_v) * (1 - expf(-dtm / 110.0f));
			if (fabsf(xd_to - xd_v) < 0.01f) xd_v = xd_to;
		}
		xd_last = xd_now;
		float bh = lines * 22 + 30 + 26 * xd_v;   // from the first baseline's top to the last line
		float y = cyc - bh / 2 + 16;
		text2(t->title, tx, y, 22, 21, rgb(0xffffff), BOLD, nw);
		y += 22 * (lines - 1);
		text(t->artist, tx, y + 19, 12, rgb(GREY), 0, nw);
		if (on_bot) art_y = y + 19;
		text(t->album, tx, y + 34, 10, rgba(GREY, 170), 0, nw);
		float dy = y + 34 + 14 + 8 * (1 - xd_v);   // the file's line, if it shows (it rises into place)
		u8 xa = (u8)(255 * xd_v);
		// no "Next" line along the cover's foot: Alex didn't want it (build 63)
		// liked: Spotify's green check (a tap on it: the playlists); not yet: a (+) in a ring (a tap likes it)
		float lx = tx + tw - 11, ly = ((cyc - bh / 2) + (y + 19)) / 2 - 2, k = 1 + 0.35f * like_pop();   // halfway down the title and artist
		like_y = ly;
		if (library_is_liked(ai->track)) ic_check(lx, ly, 9 * k);
		else {
			u32 c = rgb(0xe8e8e8);
			for (int a = 0; a < 36; a++) {
				float t0 = a * (float)M_PI / 18, t1 = (a + 1) * (float)M_PI / 18;
				C2D_DrawLine(lx + 9 * cosf(t0), ly + 9 * sinf(t0), c, lx + 9 * cosf(t1), ly + 9 * sinf(t1), c, 1.5f, 0);
			}
			rect(lx - 4.5f, ly - 0.75f, 9, 1.5f, c);
			rect(lx - 0.75f, ly - 4.5f, 1.5f, 9, c);
		}
		if (xd_v < 0.02f) goto details_done;   // the file's own facts, for whoever wants them (Settings)

		// what the file is, honestly: the lossless badge only for files that are
		char b[64], g[16] = "";
		float w = 0;
		if (fmt_lossless(ai->fmt)) {
			w = measure("LOSSLESS", 8) + 10;
			rstroke(tx, dy - 10.5f, w, 14, 3, rgba(GREY, (u8)(200 * xd_v)));
			text("LOSSLESS", tx + 5, dy, 8, rgba(GREY, xa), 0, 0);
			w += 6;
		}
		if (rg_cfg != RG_OFF && fabsf(ai->gain) >= 0.05f) snprintf(g, sizeof g, "  %+.1f dB", ai->gain);
		if (ai->bits) snprintf(b, sizeof b, "%s %lu-bit / %.1f kHz%s", fmt_name(ai->fmt), (unsigned long)ai->bits, ai->rate / 1000.0, g);
		else if (t->kbps) snprintf(b, sizeof b, "%s %lu kbps / %.1f kHz%s", fmt_name(ai->fmt), (unsigned long)t->kbps, ai->rate / 1000.0, g);
		else snprintf(b, sizeof b, "%s / %.1f kHz%s", fmt_name(ai->fmt), ai->rate / 1000.0, g);
		text(b, tx + w, dy, 9, rgba(GREY, xa), 0, tw - w);

	details_done:;
	}
	}   // not lyrics

	if (t || on_bot) {
		double len = t ? ai->len : 0, pos = t ? ai->pos : 0;
		if (on_bot && scrub >= 0 && len > 0) pos = scrub * len;
		float frac = len > 0 ? pos / len : 0, bx = L.bx, bw = L.bw;
		rrect(bx, 190, bw, 4, 2, rgba(0xffffff, 60));
		rrect(bx, 190, 4 + (bw - 4) * frac, 4, 2, rgb(0xffffff));
		C2D_DrawCircleSolid(bx + 4 + (bw - 4) * frac - 2, 192, 0, on_bot && scrub >= 0 ? 6 : 4, rgb(0xffffff));
		char p[16], l[16];
		mmss(p, pos);
		mmss(l, len);
		text(p, bx, 207, 9, rgb(GREY), 0, 0);
		text(l, bx + bw, 207, 9, rgb(GREY), C2D_AlignRight, 0);

		u32 on = rgb(GREEN), off = rgb(GREY), wh = rgb(0xffffff);
		ic_shuffle(cx - 78, 225, shuffle ? on : off);
		if (shuffle) C2D_DrawCircleSolid(cx - 80, 235, 0, 1.3f, on);
		ic_prev(cx - 40, 225, 10, wh);
		C2D_DrawCircleSolid(cx, 225, 0, 11, wh);
		if (!ai->open || ai->paused) ic_play(cx + 1.5f, 225, 9, rgb(0));
		else ic_pause(cx, 225, 9, rgb(0));
		ic_next(cx + 40, 225, 10, wh);
		ic_repeat(cx + 80, 225, repeat ? on : off, repeat == 2);
		if (repeat) C2D_DrawCircleSolid(cx + 80, 235, 0, 1.3f, on);
	}
	draw_note(cx);
}

static struct { float x0, x1; int id; } hits[8];   // header taps: tab ids, then these
static int nhits;
static bool rec_hits;   // only the touchscreen's header is tappable
enum { H_SEARCH = 10, H_BACK, H_PLAY, H_MENU, H_HOME, H_STAT = 20 };
static void hit(float x0, float x1, int id) {
	if (!rec_hits || nhits >= 8) return;
	hits[nhits].x0 = x0; hits[nhits].x1 = x1; hits[nhits++].id = id;
}

static float chip(float x, const char *s, bool on, int id, bool tight) {   // tight: beside a search
	float px = tight ? 9 : 10, pad = tight ? 7 : 10, w = measure(s, px) + 2 * pad;
	rrect(x, 5, w, 22, 11, rgb(on ? GREEN : CHIP));
	text(s, x + pad, 20.5f, px, rgb(on ? 0 : 0xffffff), 0, 0);
	hit(x, x + w, id);
	return x + w + 6;
}

// the way to the stats page: three bars, green while it's open
static float menu_chip(float x, bool on) {
	rrect(x, 5, 24, 22, 11, rgb(on ? GREEN : CHIP));
	for (int i = -1; i <= 1; i++) rect(x + 7, 15.25f + i * 4, 10, 1.5f, rgb(on ? 0 : 0xffffff));
	hit(x - 6, x + 27, H_MENU);
	return x + 30;
}

static void initial(float x, float y, const Group *g) {   // an artist's first letter on their colour
	C2D_DrawCircleSolid(x, y, 0, 13, rgb(g->color));
	char ini[8] = {0};
	memcpy(ini, g->name, (g->name[0] & 0x80) ? ((g->name[0] & 0xE0) == 0xC0 ? 2 : (g->name[0] & 0xF0) == 0xE0 ? 3 : 4) : 1);
	text(ini, x, y + 4.5f, 12, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
}

static void dur(char *s, size_t n, double secs) {
	int m = (int)(secs / 60);
	if (m >= 60) snprintf(s, n, "%dh %dm", m / 60, m % 60);
	else if (m) snprintf(s, n, "%dm", m);
	else snprintf(s, n, "%ds", (int)secs);
}

static float LW = 320;   // the list's width on the screen it's on

// a row on the stats page: its place, what it is, how long it's been heard, and its listens,
// with a hairline measuring it against number one
static void draw_stat_row(int k, float y, const AudioInfo *ai) {
	int i = items[k];
	char r[12], d[24], s[200], n[16];
	snprintf(r, sizeof r, "%d", k + 1);
	text(r, 20, y + 21, 9, rgb(k < 3 ? 0xffffff : GREY), C2D_AlignRight | (k < 3 ? BOLD : 0), 0);
	const char *name, *sub;
	bool now;
	if (stat_kind == 0) {
		Track *t = &lib.t[i];
		thumb(t->album_id, 26, y + 4, 26);
		name = t->title; sub = t->artist;
		now = ai->open && ai->track == i;
	} else if (stat_kind == 1) {
		if (i < MI_FACES && face_ok[i]) face(i, 26, y + 4, 26);   // their photo, as on the Artists tab
		else initial(39, y + 17, &lib.artists[i]);
		name = lib.artists[i].name; sub = "Artist";
		now = ai->open && lib.t[ai->track].artist_id == i;
	} else {
		thumb(i, 26, y + 4, 26);
		name = lib.albums[i].name; sub = lib.albums[i].artist;
		now = ai->open && lib.t[ai->track].album_id == i;
	}
	float w = LW - 120;
	text(name, 60, y + 15, 11, rgb(now ? GREEN : 0xffffff), 0, w);
	dur(d, sizeof d, ssec[i]);
	snprintf(s, sizeof s, "%s \xc2\xb7 %s", sub, d);
	text(s, 60, y + 28, 9, rgb(GREY), 0, w);
	snprintf(n, sizeof n, "%lu", (unsigned long)sval[i]);
	text(n, LW - 8, y + 18, 12, rgb(0xffffff), C2D_AlignRight | BOLD, 0);
	text(sval[i] == 1 ? "play" : "plays", LW - 8, y + 29, 8, rgb(GREY), C2D_AlignRight, 0);
	float top = sval[items[0]];
	rect(60, y + ROW - 3, (LW - 68) * sval[i] / (top > 0 ? top : 1), 1.5f, rgba(GREEN, 110));
}
static void dp_row(int k, float y, const AudioInfo *ai);
static void draw_row(int k, float y, const AudioInfo *ai) {
	if (rich()) { dp_row(k, y, ai); return; }
	bool roomy = detail < 0 && tab == T_SONGS;   // the Songs tab's own, SROW high
	if (detail < 0 && tab == T_STATS) { draw_stat_row(k, y, ai); return; }
	if (detail < 0 && tab == T_SETTINGS) { draw_set_row(items[k], y); return; }
	int i = items[k];
	float tw = LW - 92, gw = LW - 54;   // songs leave room for the length and the heart
	if (roomy) {
		Track *t = &lib.t[i];
		bool now = ai->open && ai->track == i;
		float rx = LW - 16, w = rx - 52 - 44;   // clear of the scroll handle
		thumb(t->album_id, 12, y + 5, 30);
		text(t->title, 52, y + 18, 11, rgb(now ? GREEN : 0xffffff), BOLD, w);
		if (sort_by[T_SONGS] == 4) {   // sorted by genre: it shows
			char gs[200];
			snprintf(gs, sizeof gs, "%s \xc2\xb7 %s", t->genre ? t->genre : "No genre", t->artist);
			text(gs, 52, y + 31, 9, rgb(GREY), 0, w);
		} else text(t->artist, 52, y + 31, 9, rgb(GREY), 0, w);
		char d[16];
		mmss(d, t->secs);
		text(d, rx, y + 25, 9, rgb(GREY), C2D_AlignRight, 0);
		if (library_is_liked(i)) ic_check(rx - 36, y + 21, 5.5f);
	} else if (songs_view()) {
		Track *t = &lib.t[i];
		thumb(t->album_id, 8, y + 4, 26);
		bool now = ai->open && ai->track == i;
		text(t->title, 42, y + 15, 11, rgb(now ? GREEN : 0xffffff), 0, tw);
		if (detail < 0 && tab == T_SONGS && sort_by[T_SONGS] == 4) {   // sorted by genre: it shows
			char gs[200];
			snprintf(gs, sizeof gs, "%s \xc2\xb7 %s", t->genre ? t->genre : "No genre", t->artist);
			text(gs, 42, y + 28, 9, rgb(GREY), 0, tw);
		} else text(t->artist, 42, y + 28, 9, rgb(GREY), 0, tw);
		char d[16];
		mmss(d, t->secs);
		text(d, LW - 8, y + 21, 9, rgb(GREY), C2D_AlignRight, 0);
		if (library_is_liked(i)) ic_check(LW - 44, y + 17, 5.5f);
	} else if (tab == T_ALBUMS) {
		Group *g = &lib.albums[i];
		thumb(i, 8, y + 4, 26);
		bool now = ai->open && lib.t[ai->track].album_id == i;
		text(g->name, 42, y + 15, 11, rgb(now ? GREEN : 0xffffff), 0, gw);
		char s[160];
		snprintf(s, sizeof s, "Album \xc2\xb7 %s", g->artist);
		text(s, 42, y + 28, 9, rgb(GREY), 0, gw);
	} else if (tab == T_ARTISTS) {
		Group *g = &lib.artists[i];
		if (i < MI_FACES && face_ok[i]) face(i, 8, y + 4, 26);   // their photo, once the card has it
		else initial(21, y + 17, g);
		bool now = ai->open && lib.t[ai->track].artist_id == i;
		text(g->name, 42, y + 15, 11, rgb(now ? GREEN : 0xffffff), 0, gw);
		char s[64];
		snprintf(s, sizeof s, "Artist \xc2\xb7 %d song%s", g->n, g->n == 1 ? "" : "s");
		text(s, 42, y + 28, 9, rgb(GREY), 0, gw);
	} else {
		Group *g = &lib.lists[i];
		if (i == 0) {   // Liked Songs: its own tile, a heart on green
			C2D_DrawRectangle(8, y + 4, 0, 26, 26, rgb(0x450af5), rgb(0x8a6cf0), rgb(0x6a7ee8), rgb(0xc4efd9));
			ic_heart(21, y + 17, 5.5f, rgb(0xffffff));
		} else if (!list_tile(i, 8, y + 4, 26)) thumb(lib.t[g->songs[0]].album_id, 8, y + 4, 26);   // its picture, else its first song's cover
		bool now = ai->open && src_kind == T_PLAYLISTS && src_id == i;
		text(g->name, 42, y + 15, 11, rgb(now ? GREEN : 0xffffff), 0, gw);
		char s[64];
		snprintf(s, sizeof s, "Playlist \xc2\xb7 %d song%s", g->n, g->n == 1 ? "" : "s");
		float sx = 42;
		if (i > 0 && pin_of(g->name) >= 0) {   // pinned: a green pushpin before the line, as Spotify shows it
			u32 c = rgb(GREEN);
			C2D_DrawCircleSolid(47.5f, y + 22.5f, 0, 2.6f, c);
			C2D_DrawTriangle(42.5f, y + 28.5f, c, 46.2f, y + 23.8f, c, 44.8f, y + 25.2f, c, 0);
			sx = 53;
		}
		text(s, sx, y + 28, 9, rgb(GREY), 0, gw - (sx - 42));
	}
}

// The list, at the width of whichever screen it's on. Up top there's no mini player and no
// finger, so the D-pad drives it. A ghost is the page sliding away: seen, never tapped.
static bool no_mini;
static void draw_mini(const AudioInfo *ai);

// The tabs: how much each is the one you're on, sprung so the green pill swells into the new tab
// (a touch past, then settles) while the old one gives it up
static float tab_o[T_HOME + 1], tab_ov[T_HOME + 1];
static void tab_spring(void) {
	for (int t = 0; t <= T_HOME; t++) {
		if (t >= NTAB && t != T_HOME) continue;
		float to = tab == t ? 1 : 0;
		tab_ov[t] = tab_ov[t] * 0.5f + (to - tab_o[t]) * 0.18f;
		tab_o[t] += tab_ov[t];
		if (fabsf(to - tab_o[t]) < 0.002f && fabsf(tab_ov[t]) < 0.002f) { tab_o[t] = to; tab_ov[t] = 0; }
	}
}
static float clamp01(float f) { return f < 0 ? 0 : f > 1 ? 1 : f; }

// The way in (intro_play, further down): the splash's tile drops through the hinge and lands as the Home
// chip, then the rest of the header fans out from under it and the page rises. The real screens draw as
// they always do; these are the few places they're told to hold back or to ride in (all 0/false when off).
static struct {
	bool on;          // playing: the header, Home and the mini player take their cues from the rest
	bool landed;      // the tile has become the Home chip: the header's chips may show
	bool content;     // Home's page may show
	float top_dy;     // how far below its place the player's screen rides, on its way up
	float rise;       // Home's page, the same
	float wash;       // Home's tint, 0 none, 1 all of it
	float mini;       // the mini player, the same as rise
	float house;      // Home's chip once landed: its house wiped in from the left, 0 not yet, 1 all of it
	float u[6];       // how far each header chip has come out from under the Home chip: menu, four tabs, search
} intro;

// The cursor on a list or a grid, gliding to the item it's on (content coordinates, the list's own
// screen): a bar behind a row, a ring round a cover. A new page, a new layout: it starts where it is.
static float hl_x, hl_y, hl_w, hl_h, hl_r;
static bool hl_ok, hl_ring;   // ring: round a cover or a button; else a bar behind a row
static int dp_nbtn(void);
static void dp_btn_box(int b, float *x, float *y, float *w, float *h, float *r);
static int hl_key = -1;
static bool home_active(void);
static void hl_step(void) {
	int v = V();
	if (!cursor_vis || home_active() || ap_active()) { hl_ok = false; return; }
	float x, y, w, h, r = 4;
	if (rich() && dp_btn >= 0) { dp_btn_box(dp_btn, &x, &y, &w, &h, &r); hl_ring = true; }
	else {
		if (!nitems || cur[v] >= nitems) { hl_ok = false; return; }
		box(cur[v], &x, &y, &w, &h);
		hl_ring = lay() == LAY_GRID;
		if (hl_ring) { h = w; if (tab == T_ARTISTS) r = w / 2; }   // round the cover, not its name
	}
	int key = v * 100003 + (detail + 1) * 17 + dkind * 7 + lay();
	if (!hl_ok || key != hl_key) { hl_x = x; hl_y = y; hl_w = w; hl_h = h; hl_r = r; hl_ok = true; hl_key = key; return; }
	hl_x += (x - hl_x) * 0.35f;
	hl_y += (y - hl_y) * 0.35f;
	hl_w += (w - hl_w) * 0.35f;
	hl_h += (h - hl_h) * 0.35f;
	hl_r += (r - hl_r) * 0.35f;
}

#include "drawer.inc"
#include "artistpage.inc"
#include "listedit.inc"
#include "detailpage.inc"
#include "covers.inc"
#include "homepage.inc"
#include "grid.inc"
#include "pagemenu.inc"
#include "addto.inc"
static void draw_list(bool on_bot, float W, const AudioInfo *ai) {
	LW = W;
	rec_hits = on_bot && !ghost;
	if (ap_active()) {
		if (rec_hits) nhits = 0;
		ap_draw(W, on_bot ? 190 : 240, ai);
		if (on_bot && !ghost && !no_mini) draw_mini(ai);
		return;
	}
	float cx = W / 2, R = W;   // R: the right edge everything in the header hangs off
	int v = V();
	bool home = home_active();

	if (home) home_draw(W, on_bot ? 190 : 240, ai);
	else if (!nitems && detail < 0 && tab == T_STATS) {
		text("No listens yet.", cx, 94, 11, rgb(GREY), C2D_AlignCenter, 0);
		text("A song counts once you've heard 30 seconds of it.", cx, 110, 9, rgb(GREY), C2D_AlignCenter, 0);
	} else if (!nitems && detail < 0 && tab == T_PLAYLISTS && !lib.nl && lib.nt) {
		text("No playlists yet.", cx, 94, 11, rgb(GREY), C2D_AlignCenter, 0);
		text("Put .m3u8 files in sdmc:/music/playlists", cx, 110, 9, rgb(GREY), C2D_AlignCenter, 0);
	} else if (!nitems && !rich()) text(lib.nt ? "Nothing matches that." : "Your library's empty.", cx, 100, 11, rgb(GREY), C2D_AlignCenter, 0);
	float rb = rows_base(W);
	if (rich()) dp_draw_hero(W, ai);
	int L = home ? LAY_ROWS : lay();
	float H = on_bot ? 190 : 240;
	bool hl_here = hl_ok && cursor_vis && !home && !ghost && W == list_w();   // the cursor, gliding
	if (hl_here && !hl_ring) rect(hl_x, hl_y - scroll[v], hl_w, hl_h, rgba(0xffffff, 22));
	if (home) {}
	else if (L == LAY_GRID) {   // covers, a row at a time, and the row under the screen asked for
		int c = g_cols(W);
		float cw = g_cw(W), pitch = g_pitch(W);
		int r0 = (int)floorf((scroll[v] - G_TOP) / pitch);
		for (int k = (r0 < 0 ? 0 : r0) * c; k < nitems; k++) {
			float y = G_TOP + (k / c) * pitch - scroll[v];
			if (y > H + pitch) break;
			if (y > H) cell_soon(k);
			else draw_cell(k, 12 + (k % c) * (cw + 12), y, cw, ai);
		}
	} else if (L == LAY_SONGS && ytop) {   // under their letters
		int lo = 0, hi = nitems - 1, first = 0;
		while (lo <= hi) { int mid = (lo + hi) / 2; if (ytop[mid] + SROW <= scroll[v]) { first = mid + 1; lo = mid + 1; } else hi = mid - 1; }
		for (int k = first; k < nitems; k++) {
			float y = ytop[k] - scroll[v];
			if (y - LETH > H) break;
			if (letters && (k == 0 || letter_of(items[k]) != letter_of(items[k - 1]))) {
				char c[2] = {letter_of(items[k]), 0};
				text(c, 12, y - 6, 20, rgb(GREEN), BOLD | NAMEF, 0);
			}
			draw_row(k, y, ai);
		}
		draw_handle(W, H, v);
	} else {
		int first = (int)floorf((scroll[v] - rb) / ROW);
		for (int k = first < 0 ? 0 : first; k < nitems; k++) {
			float y = rb + k * ROW - scroll[v];
			if (y > LIST1) break;
			draw_row(k, y, ai);
		}
	}
	if (hl_here && hl_ring) ring(hl_x, hl_y - scroll[v], hl_w, hl_h, hl_r);   // over the covers, or a page's button
	if (rich()) dp_footer(W);

	// header, drawn over the list so rows slide under it
	if (!rich()) rect(0, 0, W, LIST0, rgb(home ? (intro.on ? mixc(home_head_col(), BG, intro.wash) : home_head_col()) : BG));   // Home's: the top of its wash
	if (rec_hits) nhits = 0;
	if (rich()) dp_bar(W, ai);
	else if (detail >= 0) {
		rrect(6, 5, 22, 22, 11, rgb(CHIP));
		ic_back(16.5f, 16, rgb(0xffffff));
		hit(0, 32, H_BACK);
		Group *g = dgroup();
		text(g->name, 36, 15, 12, rgb(0xffffff), BOLD, R - 84);
		char s[160];
		snprintf(s, sizeof s, "%s \xc2\xb7 %d song%s", dkind == T_ALBUMS || dkind == K_AP ? g->artist : dkind == T_ARTISTS ? "Artist" : "Playlist", g->n, g->n == 1 ? "" : "s");
		text(s, 36, 26, 8, rgb(GREY), 0, R - 84);
		C2D_DrawCircleSolid(R - 20, 16, 0, 12, rgb(GREEN));
		bool here = ai->open && !ai->paused && playing_here();
		if (here) ic_pause(R - 20, 16, 9, rgb(0));
		else ic_play(R - 18.5f, 16, 9, rgb(0));
		hit(R - 36, R, H_PLAY);
	} else if (tab == T_SETTINGS) {
		menu_chip(6, drawer_on);
		text("Settings", 36, 20, 12, rgb(0xffffff), BOLD, 0);
	} else if (tab == T_STATS) {
		static const char *SK[] = {"Songs", "Artists", "Albums"};
		float x = menu_chip(6, drawer_on);
		for (int k = 0; k < 3; k++) x = chip(x, SK[k], stat_kind == k, H_STAT + k, false);
		char d[24], s[48];
		dur(d, sizeof d, stats_total_secs());
		snprintf(s, sizeof s, "%lu plays \xc2\xb7 %s", (unsigned long)stats_total_plays(), d);
		// the plays and the time if they leave air after the last pill (x is where the next pill would start, 6 past
		// the end of the last); else just the plays; else nothing. On the touchscreen it crowded the Albums pill.
		const float room = R - 10 - (x - 6) - 18;
		if (measure(s, 8) > room) snprintf(s, sizeof s, "%lu plays", (unsigned long)stats_total_plays());
		if (measure(s, 8) <= room) text(s, R - 10, 19.5f, 8, rgb(GREY), C2D_AlignRight, 0);
	} else if (intro.on) {   // the way in: the chips come out from under the Home chip, which is drawn last, over them
		if (intro.landed) {
			C3D_Mtx rest;
			C2D_ViewSave(&rest);
			const float hx = 36;   // where Home's chip sits: right after the menu's
			C2D_ViewTranslate((1 - intro.u[0]) * (hx + 1 - 6), 0);
			float x = menu_chip(6, drawer_on);
			C2D_ViewRestore(&rest);
			x += 32;
			for (int k = 0; k < NTAB; k++) {
				C2D_ViewTranslate((1 - intro.u[1 + k]) * (hx - x), 0);
				x = tab_chip(x, TAB_ORDER[k], tab_o[TAB_ORDER[k]]);
				C2D_ViewRestore(&rest);
			}
			C2D_ViewTranslate((1 - intro.u[5]) * (hx - (R - 30)), 0);
			rrect(R - 30, 5, 24, 22, 11, rgb(CHIP));
			ic_search(R - 18, 16, rgb(0xffffff), rgb(CHIP));
			C2D_ViewRestore(&rest);
			home_chip(hx, tab_o[T_HOME]);
			if (intro.house < 1) rect(hx + 5 + 17 * intro.house, 8.5f, 17 * (1 - intro.house), 15, rgb(GREEN));   // the chip's own green over the house, still to be wiped in
		}
	} else {
		float x = home_chip(menu_chip(6, drawer_on), tab_o[T_HOME]);
		for (int k = 0; k < NTAB; k++) x = tab_chip(x, TAB_ORDER[k], tab_o[TAB_ORDER[k]]);
		if (query[0]) {   // the query gets whatever room the chips leave
			float w = measure(query, 10), room = R - 6 - 34 - x;
			if (w > room) w = room;
			if (w < 16) w = 16;
			float x0 = R - 6 - (w + 34);
			rrect(x0, 5, w + 34, 22, 11, rgb(0xffffff));
			ic_search(x0 + 13, 16, rgb(0), rgb(0xffffff));
			text(query, x0 + 24, 19.5f, 10, rgb(0), 0, w);
			hit(x0, R, H_SEARCH);
		} else {
			rrect(R - 30, 5, 24, 22, 11, rgb(CHIP));
			ic_search(R - 18, 16, rgb(0xffffff), rgb(CHIP));
			hit(R - 34, R, H_SEARCH);
		}
	}
	if (on_bot && !ghost && !no_mini) {
		C3D_Mtx rest;
		C2D_ViewSave(&rest);
		if (intro.on) C2D_ViewTranslate(0, intro.mini);   // the way in: it rides up from under the screen
		draw_mini(ai);
		C2D_ViewRestore(&rest);
	}
	if (!ghost && W == list_w()) pm_draw(W, 240);   // the dots' menu, or its box, over all of it
}

// mini player: tap its body and the player comes down to the touchscreen
static void draw_mini(const AudioInfo *ai) {
	rect(0, 190, 320, 50, rgb(BG));
	Track *t = ai->open ? &lib.t[ai->track] : NULL;
	rrect(6, 194, 308, 40, 6, rgb(t ? mixc(top_color, 0x202020, 0.55f) : 0x202020));
	if (t) {
		thumb(t->album_id, 13, 200, 26);
		text(t->title, 46, 211, 11, rgb(0xffffff), BOLD, 186);
		text(t->artist, 46, 225, 9, rgba(0xffffff, 170), 0, 186);
		float frac = ai->len > 0 ? ai->pos / ai->len : 0;
		rect(12, 231, 296, 2, rgba(0xffffff, 50));
		rect(12, 231, 296 * frac, 2, rgb(0xffffff));
	} else text(lib.nt ? "Tap a song to play it" : "No music", 16, 218, 11, rgb(GREY), 0, 0);
	u32 wh = rgb(0xffffff);
	ic_prev(250, 212, 10, wh);
	if (!ai->open || ai->paused) ic_play(276, 212, 12, wh);
	else ic_pause(275, 212, 12, wh);
	ic_next(300, 212, 10, wh);
}

// ---- the swap: both screens ride one loop ------------------------------------------------

// Picture the two screens' contents on a belt that runs through both and loops round: pull
// down from the touchscreen's top edge and everything slides down, the top's content onto the
// touchscreen and the touchscreen's round the back and in from the top. sw is how far along,
// in whole screens; at ±1 the two have traded places.
#define GAP 18
#define HH (240 + GAP)
#define BACKDROP BG   // the same grey as everything, so nothing shows through the gap
static float sw, sw_v, sw_to;   // where it is, its speed, where the spring is pulling it
static bool sw_drag;
static int sw_dir = 1;          // which edge it started from: 1 the top, -1 the bottom
static bool sw_armed;           // the finger's far enough that letting go goes through
static bool sw_flipped;         // past halfway: the other arrangement is the one at rest
static bool sw_land;            // ...and it hasn't touched down yet
static float lift;              // 0 flat, 1 both screens lifted into cards
static float glow, grip;        // the seam's green, and the handle riding it
static bool sw_busy(void) { return sw_drag || sw != 0 || sw_v != 0; }

static void sw_start(int dir) { sw_dir = dir; sw_flipped = sw_land = sw_armed = false; }
static void sw_arm(bool a) {   // the detent: a click going over, a softer one coming back
	if (a == sw_armed) return;
	sw_armed = a;
	audio_tick(a ? TICK_ARM : TICK_GRAB);
}

static void set_swap(bool s);
static bool swap_touched;   // flipped this run: what's at rest now is the layout to keep, not the one it started with
static bool swap_unsaved;   // flipped, and not yet written to the cfg
static void sw_step(void) {
	if (!sw_drag) {
		float e0 = sw_to - sw;
		// a spring just under critical: quick, and it settles without bouncing past home
		sw_v = sw_v * 0.5f + (sw_to - sw) * 0.1f;
		sw += sw_v;
		if (sw >= 0.5f || sw <= -0.5f) {
			float d = sw > 0 ? 1 : -1;
			set_swap(!swapped);
			swap_touched = swap_unsaved = true;
			sw -= d;
			sw_to -= d;
			sw_flipped = sw_land = true;
		}
		// the first moment it reaches home is the landing: thunk
		if (sw_land && e0 * (sw_to - sw) <= 0) { sw_land = false; audio_tick(TICK_LAND); }
		if (sw_land && fabsf(sw_to - sw) < 0.01f) { sw_land = false; audio_tick(TICK_LAND); }   // it crept in
		if (fabsf(sw_to - sw) < 0.0015f && fabsf(sw_v) < 0.0015f) {
			sw = sw_to = 0;
			sw_v = 0;
			if (swap_unsaved) { swap_unsaved = false; save_cfg(); }   // settled: written now, so a crash or a power-off still remembers how it was left
		}
	}
	// lifted while held or travelling, eased straight down again: no overshoot
	float lt = sw_drag || sw_land || sw_to != 0 ? 1 : 0;
	lift += (lt - lift) * 0.22f;
	if (!lt && lift < 0.002f) lift = 0;
	glow += ((sw_drag ? sw_armed : sw_land) - glow) * 0.25f;
	grip += ((sw_drag || sw_land) - grip) * 0.2f;
	if (glow < 0.004f) glow = 0;
	if (grip < 0.004f) grip = 0;
}

// a corner of a card, rounded by painting the backdrop over what's outside the curve
static void corner(float x, float y, float sx, float sy, float r, u32 c) {
	float cx = x + sx * r, cy = y + sy * r, px = x, py = cy;
	for (int i = 1; i <= 6; i++) {
		float a = i * (float)M_PI / 12, qx = cx - sx * r * cosf(a), qy = cy - sy * r * sinf(a);
		C2D_DrawTriangle(x, y, c, px, py, c, qx, qy, c, 0);
		px = qx;
		py = qy;
	}
}

static void draw_slot(int slot, int screen, const AudioInfo *ai) {
	float o = ((slot - screen) + sw) * HH;   // where this slot's content sits on this screen
	while (o >= HH) o -= 2 * HH;
	while (o < -HH) o += 2 * HH;
	if (o <= -240 - GAP || o >= 240 + GAP) return;
	bool player = (slot == 0) != swapped;
	float W = screen ? 320 : 400, k = 1 - 0.06f * lift;
	C2D_ViewReset();
	C2D_ViewTranslate(W / 2, roundf(o) + 120 + (intro.on && player ? intro.top_dy : 0));
	if (k != 1) C2D_ViewScale(k, k);
	C2D_ViewTranslate(-W / 2, -120);
	rect(0, 0, W, 240, rgb(BG));
	if (player) draw_player(screen == 1, W, ai);
	else if (nb_tab >= 0 || hoff != 0) {   // mid-slide: the page it's leaving rides alongside
		C3D_Mtx m;
		C2D_ViewSave(&m);
		if (nb_tab >= 0) {
			C2D_ViewTranslate(roundf(hoff) + nb_side * W, 0);
			swap_pages();
			ghost = true;
			draw_list(screen == 1, W, ai);
			ghost = false;
			swap_pages();
			C2D_ViewRestore(&m);
		}
		C2D_ViewTranslate(roundf(hoff), 0);
		no_mini = true;
		draw_list(screen == 1, W, ai);
		no_mini = false;
		C2D_ViewRestore(&m);
		if (screen == 1) draw_mini(ai);
	} else draw_list(screen == 1, W, ai);
	if (!player && !ghost) draw_drawer(W, 240);
	if (!player && !ghost && W == list_w()) at_draw(W);   // Add to playlist, over everything on the list's screen
	if (!player && !ghost && screen == 1) dg_draw(W);   // a song being dragged onto a playlist
	if (lift > 0.01f) {
		float r = 12 * lift;
		u32 b = rgb(BACKDROP);
		corner(0, 0, 1, 1, r, b);
		corner(W, 0, -1, 1, r, b);
		corner(0, 240, 1, -1, r, b);
		corner(W, 240, -1, -1, r, b);
	}
}

// The swap's handle, like the bar at the bottom of a phone: drawn from a small texture made at
// launch (an anti-aliased pill in rows 0-15, its soft shadow in rows 16-63), in three slices so
// the round ends stay round at any length. Solid circles this small came out jagged.
static C3D_Tex bar_tex;
static bool bar_ok;
#define BAR_K  (5 / 12.0f)       // screen px per texel: the pill's 12 texels come out 5 px tall
#define BAR_AA (1 / BAR_K)       // a screen pixel, in texels
static void bar_make(void) {
	if (!(bar_ok = C3D_TexInit(&bar_tex, 64, 64, GPU_RGBA8))) return;
	C3D_TexSetFilter(&bar_tex, GPU_LINEAR, GPU_LINEAR);
	static u32 px[64 * 64];
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++) {
			float fx = x + 0.5f, fy = y + 0.5f, a;
			if (y < 16) {   // the pill: radius 6 round a line from x 8 to 56, its edge blended over 2.4 texels,
				            // which is one screen pixel at the 5/12 it's drawn at
				float cx = fx < 8 ? 8 : fx > 56 ? 56 : fx, d = sqrtf((fx - cx) * (fx - cx) + (fy - 8) * (fy - 8)) - 6;
				a = 0.5f - d / BAR_AA;
				a = a < 0 ? 0 : a > 1 ? 1 : a;
			} else {        // the shadow: a core of radius 6 round x 24 to 40, falling off softly
				float cx = fx < 24 ? 24 : fx > 40 ? 40 : fx, d = sqrtf((fx - cx) * (fx - cx) + (fy - 40) * (fy - 40)) - 6;
				a = d <= 0 ? 1 : expf(-d * d / 50);
			}
			px[y * 64 + x] = 0xFFFFFF00 | (u32)(a * 255 + 0.5f);
		}
	put_rect(&bar_tex, 0, 0, px, 64, 64);
}
// rows ty..ty+th of the texture, caps `cap` texels wide, drawn w by h at x, y, tinted col
static void slice3(float x, float y, float w, float h, int ty, int th, int cap, u32 col) {
	float k = h / th, cw = cap * k;
	if (w < 2 * cw) w = 2 * cw;
	C2D_ImageTint t;
	C2D_PlainImageTint(&t, col, 1);
	float v0 = 1 - ty / 64.0f, v1 = 1 - (ty + th) / 64.0f, c = cap / 64.0f;
	Tex3DS_SubTexture L = {cap, th, 0, v0, c, v1}, M = {64 - 2 * cap, th, c, v0, 1 - c, v1}, R = {cap, th, 1 - c, v0, 1, v1};
	C2D_DrawImageAt((C2D_Image){&bar_tex, &L}, x, y, 0, &t, k, k);
	C2D_DrawImageAt((C2D_Image){&bar_tex, &M}, x + cw, y, 0, &t, (w - 2 * cw) / (64 - 2 * cap), k);
	C2D_DrawImageAt((C2D_Image){&bar_tex, &R}, x + w - cw, y, 0, &t, k, k);
}
static void home_bar(float cx, float cy, float w, float alpha, u32 col) {
	if (!bar_ok || alpha <= 0) return;
	slice3(cx - (w + 14) / 2, cy - 9, w + 14, 18, 16, 48, 24, rgba(0, 110 * alpha));   // the shadow, so it reads on any cover
	float qh = 16 * BAR_K, qw = w + 4 * BAR_K;   // the quad: the pill plus the 2 texels round it
	slice3(cx - qw / 2, cy - qh / 2, qw, qh, 0, 16, 8, rgba(col, 255 * alpha));
}

static void draw_screen(int screen, const AudioInfo *ai) {
	C3D_RenderTarget *tg = screen ? bot : top;
	C2D_TargetClear(tg, rgb(BACKDROP));
	C2D_SceneBegin(tg);
	// the one higher up first, so the lower one covers anything that hangs off its bottom
	float o0 = fmodf(((0 - screen) + sw) * HH + 3 * HH, 2 * HH) - HH;
	float o1 = fmodf(((1 - screen) + sw) * HH + 3 * HH, 2 * HH) - HH;
	if (o0 <= o1) { draw_slot(0, screen, ai); draw_slot(1, screen, ai); }
	else { draw_slot(1, screen, ai); draw_slot(0, screen, ai); }
	C2D_ViewReset();
	if (screen == 1 && grip > 0) {   // the handle you're pulling, on the seam under your finger
		bool top_seam = (sw_dir > 0) != sw_flipped;
		float y = top_seam ? sw * HH - GAP / 2.0f : 240 + GAP / 2.0f + sw * HH;
		y = y < 7 ? 7 : y > 233 ? 233 : y;
		float p = fabsf(sw) * 4, w = 30 + 26 * (p > 1 ? 1 : p);
		home_bar(160, y, w, grip, mixc(GREEN, 0xffffff, glow));
	}
}

// ---- the player on the touchscreen -------------------------------------------------------

static void set_swap(bool s) {
	swapped = s;
	vel = 0;
	scrub = -1;
	if (!s) return;
	int v = V();   // up top there's no finger, so the cursor shows where A lands
	if (nitems && !row_shown(v, cur[v])) cur[v] = first_shown(v);
	cursor_vis = true;
}

static void play_any(void) {   // play with nothing picked: the whole library
	if (!lib.nt) return;
	set_tab(T_SONGS);
	rebuild();
	if (nitems) play_list(shuffle ? rand() % nitems : 0);
}

static bool on_bar(int y) { return y >= 180 && y < 204; }
static float bar_frac(int x) { float f = (x - P.bx) / P.bw; return f < 0 ? 0 : f > 1 ? 1 : f; }

// A tap (or a drag along the progress bar) that started at x, y and let go at xe.
static void like(int i) {
	bool on = library_like(i);
	say(on ? "Added to Liked Songs" : "Removed from Liked Songs");
	like_t0 = on ? osGetTime() : 0;
	dirty = true;
	spot_request(3000, false, false);   // to Spotify too, once the likes stop coming
}

// A Spotify sync changed Liked Songs, and maybe re-read every playlist: then the Groups are new,
// so the playlist playing is found again by name, and one open over the tab is closed
static void on_reloaded(bool lists) {
	if (lists) {
		if (src_kind == T_PLAYLISTS) {
			src_kind = -1;
			for (int k = 0; k < lib.nl; k++)
				if (!strcmp(lib.lists[k].name, src_name)) { src_kind = T_PLAYLISTS; src_id = k; break; }
		}
		if (detail > 0 && dkind == T_PLAYLISTS) close_detail();   // Liked Songs, at 0, stays put
		dg_on = false;   // a song in hand: the lists it was over are gone
		dg_n = 0;
		lists_want();    // their pictures, by the new order
	}
	dirty = true;
}

static void at_open(int t);   // addto.inc
static void player_tap(int x, int y, int xe, const AudioInfo *ai) {
	if (on_bar(y)) {
		if (ai->open && ai->len > 0) audio_seek(bar_frac(xe) * ai->len, false);
		scrub = -1;
		return;
	}
	if (y < 30 && x > P.W - 40) {
		if (!lyr_on && ai->open && lyr_none(ai->track)) say("No lyrics for this song");
		else { lyr_on = !lyr_on; lyr_y = -1; if (lyr_on) q_on = false; }
		return;
	}
	if (y < 30 && x > P.W - 72 && x <= P.W - 40) { queue_toggle(); return; }   // the queue chip, left of the lyrics one
	if (y < 30 && x < 36) { sw_start(1); sw_to = 1; return; }
	if (q_on && ai->open) { if (y >= 34 && y < 182) queue_tap(x, y, ai); else if (y >= 182 && y < 206) {} else goto controls; return; }
	if (lyr_on && ai->open) { if (y >= 30 && y < 182) lyr_tap(y, ai->track); else if (y >= 182 && y < 206) {} else goto controls; return; }
	if (x >= P.x0 && x < P.x0 + P.cs && y >= P.cy && y < P.cy + P.cs) { sw_start(1); sw_to = 1; return; }
	if (ai->open && x >= P.tx && y > art_y - 13 && y < art_y + 4) {   // the artist: their page, up on the list
		int a = lib.t[ai->track].artist_id;
		if (detail >= 0) close_detail();
		ap_clear();
		ap_open_local(a);
		sw_start(1);
		sw_to = 1;
		return;
	}
	if (ai->open && fabsf(x - (P.tx + P.tw - 11)) < 16 && fabsf(y - like_y) < 16) {   // the (+) likes it; the check opens the playlists
		if (library_is_liked(ai->track)) at_open(ai->track);
		else like(ai->track);
		return;
	}
controls:;
	float d = x - P.cx;
	if (y < 206 || d < -100 || d > 100) return;
	if (d < -59) set_shuffle(!shuffle);
	else if (d < -20) skip(-1, ai);
	else if (d < 20) { if (ai->open) audio_pause(!ai->paused); else play_any(); }
	else if (d < 59) skip(1, ai);
	else repeat = (repeat + 1) % 3;
}

// The splash while the library is read: the icon and the wordmark, no count (Alex: be optimistic,
// "reading 0 new songs" isn't anyone's business). Its texture lives only until the read is done.
extern const u8 splash_png[], splash_png_end[];
unsigned char *stbi_load_from_memory(const unsigned char *b, int len, int *w, int *h, int *ch, int want);
void stbi_image_free(void *p);
static void put_rect(C3D_Tex *t, int ox, int oy, const u32 *px, int w, int h);   // artistpage.inc
static C3D_Tex splash_tex;
static bool splash_ok, splash_tried;
static void splash_load(void) {
	splash_tried = true;
	int w = 0, h = 0, ch;
	u8 *img = stbi_load_from_memory(splash_png, splash_png_end - splash_png, &w, &h, &ch, 4);
	if (img && w == 400 && h == 240 && C3D_TexInit(&splash_tex, 512, 256, GPU_RGBA8)) {
		u32 *px = (u32 *)img;
		for (int i = 0; i < w * h; i++) {   // stb's bytes R G B A, the texture's 0xRRGGBBAA
			u8 *b = img + 4 * i;
			px[i] = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
		}
		put_rect(&splash_tex, 0, 0, px, w, h);
		splash_ok = true;
	}
	if (img) stbi_image_free(img);
	crumb("start: splash %s", splash_ok ? "up" : "missing");
}
static void splash_free(void) {
	if (splash_ok) C3D_TexDelete(&splash_tex);
	splash_ok = false;
}

static bool quit;   // the system asked us to close while the library was still being read

// While real tag reads are going on (new songs, or R held at launch) nine level bars climb under the wordmark, the
// same bars as on the banner's back. A normal boot reads nothing, shows none, and so stays the launch logo's twin.
static bool bars_on;       // a real read has started
static float bars_goal;    // how far through the files it is, 0..1
static float bars_shown;   // eased toward it
static u64 bars_t0;        // when the first read came (the bars' beat and the touchscreen's fade-in run from it; osGetTime itself is too big for a float)
// The touchscreen says what's going on: "Reading songs", the count, and the last few files, each one a name taken
// about four times a second (a whole library goes by far faster than anyone can read, and a list that changes every
// frame is just flicker).
static int scan_done, scan_total;
static char scan_names[4][96];   // oldest first
static void scan_push(const char *path) {
	const char *b = strrchr(path, '/');
	for (int k = 0; k < 3; k++) memcpy(scan_names[k], scan_names[k + 1], sizeof scan_names[k]);
	snprintf(scan_names[3], sizeof scan_names[3], "%.95s", b ? b + 1 : path);
}
static u32 ca(u32 hex, float a) { return rgba(hex, (u8)(255 * (a < 0 ? 0 : a > 1 ? 1 : a))); }
// the bars: reading only decides how many are lit; each lit one bounces on its own mix of beats, the middle ones livelier,
// between a 5 px stub and 17 px (clear of the wordmark), stepping with the screen
static void splash_bars(float p, float fold) {   // fold 0..1 sinks them away at the end
	enum { N = 9 };
	float t = (float)((osGetTime() - bars_t0) / 100 * 100), lit = p * N;
	for (int i = 0; i < N; i++) {
		float a = lit - i;
		a = a < 0 ? 0 : a > 1 ? 1 : a;
		bool on = lit > i;
		float s = 0.5f + 0.5f * (0.55f * sinf(t / 210 + i * 1.9f) + 0.3f * sinf(t / 113 + i * 3.7f + 1) + 0.15f * sinf(t / 61 + i * 5.3f));
		s *= (0.62f + 0.38f * sinf((float)M_PI * (i + 0.5f) / N)) * 1.12f;
		float h = on ? 5 + 12 * (s > 1 ? 1 : s) * a : 5;
		h *= 1 - fold;
		if (h < 0.6f) continue;
		rrect(200 - (N - 1) * 5 + i * 10 - 2.5f, 226 - h, 5, h, 2.5f, on ? rgb(GREEN) : rgb(0x262626));
	}
}
static void splash_text(float fold) {   // the touchscreen: 320 wide
	float a = (float)(osGetTime() - bars_t0) / 400;
	a = (a > 1 ? 1 : a) * (1 - fold);
	if (a <= 0.01f || scan_total <= 0) return;
	text("Reading songs", 160, 58, 13, ca(GREY, a), C2D_AlignCenter | BOLD, 0);
	// the count: the number right-aligned inside a block as wide as the finished one, so it doesn't shuffle as it grows
	char n[16], of[24], full[16];
	snprintf(n, sizeof n, "%d", scan_done);
	snprintf(full, sizeof full, "%d", scan_total);
	snprintf(of, sizeof of, " / %d", scan_total);
	float wn = measure_name(full, 40), wo = measure(of, 18), left = 160 - (wn + wo) / 2;
	text(n, left + wn, 112, 40, ca(scan_done >= scan_total ? GREEN : 0xffffff, a), C2D_AlignRight | BOLD | NAMEF, 0);
	text(of, left + wn, 112, 18, ca(GREY, a), BOLD, 0);
	static const float Y[4] = {150, 169, 188, 207}, O[4] = {0.16f, 0.3f, 0.55f, 1};
	for (int k = 0; k < 4; k++) {
		if (!scan_names[k][0]) continue;
		char name[96];
		snprintf(name, sizeof name, "%.95s", scan_names[k]);
		char *dot = strrchr(name, '.');
		char ext[16] = "";
		if (dot && strlen(dot) < sizeof ext) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
		float we = measure(ext, 11), wn2 = measure(name, 11);
		if (wn2 > 288 - we) wn2 = 288 - we;   // a long name loses its end, not its extension
		float x = 160 - (wn2 + we) / 2;
		text(name, x, Y[k], 11, ca(k == 3 ? 0xf2e6cf : 0xcfc9bb, a * O[k]), 0, wn2);
		text(ext, x + wn2, Y[k], 11, ca(GREEN, a * O[k] * 0.8f), 0, 0);
	}
}
static void splash_frame(float p, float fold) {   // one frame: the splash picture and the bars if a read is on; the touchscreen says what's being read
	C2D_TextBufClear(tbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(top, rgb(BG));
	C2D_SceneBegin(top);
	if (!splash_tried) splash_load();
	if (splash_ok) {
		static const Tex3DS_SubTexture st = {400, 240, 0.0f, 1.0f, 400 / 512.0f, 1.0f - 240 / 256.0f};
		C2D_DrawImageAt((C2D_Image){&splash_tex, &st}, 0, 0, 0, NULL, 1, 1);
	}
	if (bars_on) splash_bars(p, fold);
	C2D_TargetClear(bot, rgb(BG));
	if (bars_on) {
		C2D_SceneBegin(bot);
		splash_text(fold);
	}
	C3D_FrameEnd(0);
}
static void progress(int found, int done, int total, const char *path) {   // done of total files looked at, the file just read (0, 0, 0 from the places that just keep the splash up)
	if (!aptMainLoop()) quit = true;   // keep answering the system: sleep, the power button
	static u64 last, named;
	u64 now = osGetTime();
	if (found > 0 && total > 0) {
		if (!bars_on) { bars_on = true; bars_t0 = now; }
		bars_goal = (float)done / total;
		scan_done = done;
		scan_total = total;
		if (path && now - named >= 250) { named = now; scan_push(path); }
	}
	// ten times a second at most: every frame waits for the screen, which is time the card isn't read
	if (found > 1 && now - last < 100) return;
	last = now;
	bars_shown += (bars_goal - bars_shown) * 0.6f;
	splash_frame(bars_shown, 0);
}
// The read is done: the bars fill, dance a moment, and fold away, before the intro takes the splash over.
static void splash_bars_finish(void) {
	if (!bars_on) return;
	u64 t0 = osGetTime();
	scan_done = scan_total;   // N of N, and the count turns green
	for (u64 e; (e = osGetTime() - t0) < 560 && aptMainLoop();) {
		bars_shown += (1 - bars_shown) * 0.4f;
		splash_frame(bars_shown, e < 360 ? 0 : (e - 360) / 200.0f);
	}
	bars_on = false;
}

// ---- the way in --------------------------------------------------------------------------------
// The splash doesn't cut to Home. Once the library is read and everything's up, its letters fold into the
// tile, the cassette turns away, and the tile drops through the hinge and lands as Home's chip: the top
// screen's tile (centred over 400) and the touchscreen's chip (320 wide, 40 px in from the top's edge,
// the hinge between) are one strip, so the tile leaves one screen where it enters the other.
// The rest of the header fans out from under the chip, the player rises on the top screen, Home's page
// on the touchscreen. About a second; any press skips it. The tile and cassette are drawn from the
// splash.svg numbers, the letters are cut from the splash picture itself.
#define IGAP 56   // the hinge between the screens, in pixels at their pitch (about 11 mm)
#define SP_DEEP  0x0a6e39
#define SP_CREAM 0xf2e6cf
#define SP_FOOT  0xd9caa8
#define IN_LAND  600   // ms: the tile arrives at the chip
#define IN_END   1000
#define STICKS (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT | KEY_CSTICK_UP | KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT)

static float sg(float x, float a, float b) { return clamp01((x - a) / (b - a)); }
static float e_io(float p) { return p < .5f ? 4 * p * p * p : 1 - powf(-2 * p + 2, 3) / 2; }
static float e_out(float p) { float q = 1 - p; return 1 - q * q * q * q; }
static float e_in(float p) { return p * p * p; }
static float lerpf(float a, float b, float p) { return a + (b - a) * p; }

static void sp_tile(float cx, float cy, float w, float h, float r, float sd) {
	if (sd > 0.05f) rrect(cx - w / 2 + sd, cy - h / 2 + sd, w, h, r, rgb(SP_DEEP));
	rrect(cx - w / 2, cy - h / 2, w, h, r, rgb(GREEN));
}

typedef struct { float k, c, s; } Cas;   // the cassette's size on screen, and the cosine and sine of its turn
static void cas_xf(const Cas *a, float x, float y, float *X, float *Y) {   // splash.svg's cassette space to the strip
	x -= 198;
	y -= 60;
	*X = 194.25f + a->k * (x * a->c - y * a->s);
	*Y = 92.25f + a->k * (x * a->s + y * a->c);
}
static void cas_rr(const Cas *a, float x, float y, float w, float h, float r, u32 c) {
	float px[4 * (SEG + 1)], py[4 * (SEG + 1)], mx, my;
	int n = rr_pts(x, y, w, h, r, px, py);
	for (int i = 0; i < n; i++) cas_xf(a, px[i], py[i], &px[i], &py[i]);
	cas_xf(a, x + w / 2, y + h / 2, &mx, &my);
	for (int i = 0; i < n; i++) { int j = (i + 1) % n; C2D_DrawTriangle(mx, my, c, px[i], py[i], c, px[j], py[j], c, 0); }
}
static void cas_tri(const Cas *a, float x0, float y0, float x1, float y1, float x2, float y2, u32 c) {
	float X[3], Y[3];
	cas_xf(a, x0, y0, &X[0], &Y[0]);
	cas_xf(a, x1, y1, &X[1], &Y[1]);
	cas_xf(a, x2, y2, &X[2], &Y[2]);
	C2D_DrawTriangle(X[0], Y[0], c, X[1], Y[1], c, X[2], Y[2], c, 0);
}
static void sp_reel(const Cas *a, float x, u32 col, float spin) {
	float X, Y;
	cas_xf(a, x, 58.8634f, &X, &Y);
	C2D_DrawCircleSolid(X, Y, 0, 4.63749f * a->k, col);
	for (int i = 0; i < 6; i++) {   // the hub, a hexagon: it's what turns
		float t0 = spin + i * (float)M_PI / 3, t1 = t0 + (float)M_PI / 3;
		cas_tri(a, x, 58.8634f, x + 2.4f * cosf(t0), 58.8634f + 2.4f * sinf(t0), x + 2.4f * cosf(t1), 58.8634f + 2.4f * sinf(t1), rgb(BG));
	}
}
static void sp_cassette(float s, float deg, float spin) {
	if (s <= 0.01f) return;
	float th = deg * (float)M_PI / 180;
	Cas a = {1.15f * s, cosf(th), sinf(th)};
	for (int i = 6; i >= 1; i--) cas_rr(&a, 168.4f + .592f * i, 41.056f + .592f * i, 59.2f, 37.888f, 4.736f, rgb(SP_DEEP));
	cas_rr(&a, 168.4f, 41.056f, 59.2f, 37.888f, 4.736f, rgb(SP_CREAM));
	cas_rr(&a, 174.32f, 45.9814f, 47.36f, 3.7888f, 1.8944f, rgb(GREEN));
	cas_rr(&a, 179.056f, 52.4224f, 37.888f, 12.8819f, 6.44096f, rgb(BG));
	sp_reel(&a, 185.497f, rgb(GREEN), spin);
	sp_reel(&a, 210.503f, rgb(SP_CREAM), spin);
	cas_tri(&a, 181.424f, 78.944f, 185.568f, 70.6086f, 210.432f, 70.6086f, rgb(SP_FOOT));
	cas_tri(&a, 181.424f, 78.944f, 210.432f, 70.6086f, 214.576f, 78.944f, rgb(SP_FOOT));
}
// "3DStereo" cut into its eight letters (the picture's own pixels), each folding into the tile's middle
static void sp_word(float x) {
	static const float ox[8] = {112.4f, 134.0f, 159.092f, 181.556f, 194.192f, 213.956f, 227.168f, 246.932f};   // each one's origin on the baseline
	static const int cut[9] = {112, 134, 159, 181, 194, 214, 227, 247, 268};
	for (int i = 0; i < 8; i++) {
		float g = e_in(sg(x, i * 14, i * 14 + 150)), s = 1 - g;
		if (g >= 1) continue;
		float px = lerpf(ox[i], 196.5f, g), py = lerpf(197.6f, 94.5f, g);
		Tex3DS_SubTexture st = {cut[i + 1] - cut[i], 33, cut[i] / 512.0f, 1.0f - 168 / 256.0f, cut[i + 1] / 512.0f, 1.0f - 201 / 256.0f};
		C2D_DrawImageAt((C2D_Image){&splash_tex, &st}, px + (cut[i] - ox[i]) * s, py + (168 - 197.6f) * s, 0, NULL, s, s);
	}
}

static void intro_play(void) {
	if (splash_ok) C3D_TexSetFilter(&splash_tex, GPU_LINEAR, GPU_LINEAR);
	u64 t0 = osGetTime();
	for (;;) {
		if (!aptMainLoop()) { quit = true; break; }
		hidScanInput();
		if ((hidKeysDown() & ~STICKS) || (hidKeysHeld() & KEY_TOUCH)) break;   // a button or a finger skips it; a nudge of the stick doesn't
		float x = (float)(osGetTime() - t0);
		if (x >= IN_END) break;
		intro.on = true;
		intro.landed = x >= IN_LAND;
		// with the list up top (the usual layout) the tile only slides up the one screen to its chip and the
		// player rises below; with the player up top, the tile drops through the hinge to the chip on the touchscreen
		bool up = swapped;
		bool top_live = up ? x >= IN_LAND : x > 440, bot_live = up ? x > 440 : true;
		intro.top_dy = 44 * (1 - e_out(sg(x, 440, 780)));
		for (int i = 0; i < 6; i++) intro.u[i] = e_out(sg(x, IN_LAND + i * 20, IN_LAND + i * 20 + 240));
		intro.house = e_out(sg(x, IN_LAND, IN_LAND + 170));
		intro.content = x > 680;
		intro.rise = 44 * (1 - e_out(sg(x, 680, 980)));
		intro.wash = e_out(sg(x, 660, 900));
		intro.mini = 50 * (1 - e_out(sg(x, 720, 980)));
		// the tile, in the strip: it lands on the chip, 49 across the screen it's on and 16 down
		float tx = up ? 49 : 40 + 49, ty = up ? 16 : 240 + IGAP + 16;
		float m = e_io(sg(x, 260, IN_LAND)), m2 = e_in(sg(x, 380, IN_LAND));
		float cx = lerpf(196.55f, tx, m), cy = lerpf(94.55f, ty, m);
		float w = lerpf(92, 26, m), h = lerpf(92, 22, m2);
		float r = fminf(lerpf(20.24f, 11, m2), h / 2), sd = 6.9f * (1 - sg(x, 260, 440));

		AudioInfo ai = {.track = -1};
		audio_info(&ai);
		if (dirty) rebuild();
		C2D_TextBufClear(tbuf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		if (top_live) draw_screen(0, &ai);
		else {
			C2D_TargetClear(top, rgb(BG));
			C2D_SceneBegin(top);
		}
		C2D_ViewReset();
		if (splash_ok && x < 260) sp_word(x);
		if (!intro.landed) sp_tile(cx, cy, w, h, r, sd);
		float cs = 1 - e_in(sg(x, 80, 260));
		sp_cassette(cs, -8 - 200 * (1 - cs), x * 0.0062f);
		if (bot_live) draw_screen(1, &ai);
		else {
			C2D_TargetClear(bot, rgb(BG));
			C2D_SceneBegin(bot);
		}
		C2D_ViewReset();
		if (!intro.landed && !up) {
			C2D_ViewTranslate(-40, -(240 + IGAP));
			sp_tile(cx, cy, w, h, r, sd);
			C2D_ViewReset();
		}
		C3D_FrameEnd(0);
	}
	memset(&intro, 0, sizeof intro);
}

// ---- main --------------------------------------------------------------------------------

// Headphones in? The DSP knows for sure; the shared page's flag isn't always kept up to date.
static bool headset(bool dsp) {
	bool in = false;
	if (dsp && R_SUCCEEDED(DSP_GetHeadphoneStatus(&in))) return in;
	return osIsHeadsetConnected();
}

// every APT event, so a freeze inside aptJumpToHomeMenu still says how far it got
static aptHookCookie apt_crumbs;
static void on_apt(APT_HookType t, void *p) {
	(void)p;
	static const char *N[] = {"suspend", "restore", "sleep", "wakeup", "exit"};
	crumb("apt: %s", t < APTHOOK_COUNT ? N[t] : "?");
}

// The screen service, only for the moment it's needed. Builds up to 21 held it open from launch,
// and the HOME Menu, which wants it too, never came up: the HOME test froze the step it opened.
static void backlights(bool on) {
	if (R_FAILED(gspLcdInit())) return;
	if (on) GSPLCD_PowerOnAllBacklights();
	else GSPLCD_PowerOffAllBacklights();
	gspLcdExit();
}

// HOME, ours to answer: the DSP handed back first, then the system's own jump, then the way back.
// lid: the lid service is let go for the trip as well, so the HOME Menu has every session it wants.
// What shows the moment HOME is pressed, while the system brings its menu up (a second or so of
// nothing otherwise, which looks like a freeze): both screens dimmed over what was there, a spinner
// and a word on each. The spinner turns for as long as we're still getting ready (the sound has
// to be let go first); once the system's jump starts, its own animation takes over.
// The system's part (from aptJumpToHomeMenu until its menu is up, a second or so) freezes whatever
// was last drawn, and no app can draw then: a spinner left there looks stuck (Alex, builds 37 and
// 38). So the last frame before the jump trades the spinner for the HOME button's house, which
// reads as "over to the system" standing still.
static u64 home_t0;
static bool home_house;
static void home_frame(void) {
	bool first = !home_t0;
	if (first) home_t0 = osGetTime();
	int head = (int)((osGetTime() - home_t0) / 70) % 12;   // the bright dot goes round, a step every 70 ms
	C2D_TextBufClear(tbuf);   // the last frame's text is already on the screens
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	for (int sc = 0; sc < 2; sc++) {
		C2D_SceneBegin(sc ? bot : top);   // not cleared: drawn over the last frame
		float W = sc ? 320 : 400, cx = W / 2;
		if (first) rect(0, 0, W, 240, rgba(0, 175));   // dimmed once; after that only the card is redrawn
		if (W != list_w()) continue;   // the card only on the library's screen; the player's is just dimmed
		rrect(cx - 92, 76, 184, 98, 12, rgb(0x181818));   // opaque, so each frame's spinner starts clean
		if (home_house) {   // roof, walls, a door
			C2D_DrawTriangle(cx, 89, rgb(0xffffff), cx - 15, 104, rgb(0xffffff), cx + 15, 104, rgb(0xffffff), 0);
			rect(cx - 10, 103, 20, 15, rgb(0xffffff));
			rect(cx - 3, 109, 6, 9, rgb(0x181818));
		} else for (int k = 0; k < 12; k++) {   // the spinner, its tail fading behind the bright dot
			float a = (k - 3) * (float)M_PI / 6;
			int age = (head - k + 12) % 12;
			C2D_DrawCircleSolid(cx + 13 * cosf(a), 104 + 13 * sinf(a), 0, 2.4f, rgba(0xffffff, (u8)(255 - 215 * age / 11)));
		}
		text("Opening the HOME Menu", cx, 142, 12, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
		text("3DStereo keeps your place", cx, 160, 9, rgb(GREY), C2D_AlignCenter, 0);
	}
	C3D_FrameEnd(0);
}

static void jump_home(bool audio_ok, bool lid) {
	home_t0 = 0;
	home_house = false;
	home_frame();
	crumb_flush();
	crumb("HOME: audio %d", audio_ok);
	connect_hold_wifi(false);   // the HOME Menu gets the network as the system likes it; held again on return
	if (audio_ok) audio_suspend(home_frame);   // the spinner keeps turning while the sound lets go
	if (lid) ptmuExit();
	home_house = true;
	home_frame();   // what stays up while the system takes over
	crumb("HOME: jumping");
	aptJumpToHomeMenu();
	crumb("HOME: returned%s", aptShouldClose() ? ", to close" : "");
	if (lid && R_FAILED(ptmuInit())) crumb("HOME: the lid service didn't come back");
	// HOME Menu -> Close: the sound stays let go. Taking the DSP back and setting our clock while
	// the system was closing us froze the console on the HOME screen (build 36).
	if (audio_ok && !aptShouldClose()) audio_resume();
}

// Hold L at launch: Pocket starts with the screens and nothing else, and A brings up one more
// piece at a time, the way a normal launch does. Press HOME at each step; the log names the
// step it froze at, so whichever piece the HOME Menu can't live with gives itself away.
static const char *TEST_STEP[] = {
	"Graphics only",
	"New 3DS speedup",
	"Lid service",
	"Screen service, held open the way builds 1-21 did",
	"Your library",
	"Sound, paused on your last song",
	"Cover art",
	"Spotify and the network",
};
#define TEST_LAST 7
static int home_test(void) {
	int step = 0;
	bool audio_ok = false;
	LightLock_Init(&qlock);
	aptSetHomeAllowed(false);
	crumb("test: step 0, %s", TEST_STEP[0]);
	while (aptMainLoop()) {
		hidScanInput();
		u32 kd = hidKeysDown();
		if (kd & KEY_START) break;
		if (aptCheckHomePressRejected()) {
			crumb("test: HOME at step %d", step);
			jump_home(audio_ok, false);   // as it was: the test finds causes, it doesn't hide them
			continue;
		}
		AudioInfo ai = {.track = -1};
		if (audio_ok) audio_info(&ai);
		if (audio_ok && (kd & KEY_X)) {   // HOME with music playing goes another way through the DSP
			if (ai.open) audio_pause(!ai.paused);
			else play_any();
			crumb("test: %s", ai.open && !ai.paused ? "paused" : "playing");
		}
		// keep the background as busy as the real app: covers taken as they come, Spotify's thread run
		if (step >= 6) {
			const u32 *px;
			u32 c;
			int t;
			while (art_thumb(&px) >= 0) {}
			art_big(&t, &px, &c);
		}
		if (step >= 7) spot_poll(NULL, NULL);
		if ((kd & KEY_A) && step < TEST_LAST) {
			step++;
			crumb("test: step %d, %s", step, TEST_STEP[step]);
			switch (step) {
			case 1: osSetSpeedupEnable(true); break;
			case 2: ptmuInit(); break;
			case 3: gspLcdInit(); break;
			case 4: load_cfg(); progress(0, 0, 0, NULL); library_scan(false, progress); splash_free(); break;
			case 5:
				audio_ok = audio_init(auto_next, quality, RG_OFF);
				if (audio_ok) load_resume();
				crumb("test: sound %s", audio_ok ? "on" : "won't start");
				break;
			case 6: art_init(); if (ai.track >= 0) art_want(ai.track); break;
			case 7:   // what launch does, and then spot_poll below starts the thread and the sockets
				net_init(); spot_init(); sm_build(); meta_init(); spot_request(1500, false, true);
				meta_lyrics_want(ai.track);
				break;
			}
			crumb("test: step %d is up", step);
			hidScanInput();   // a press during the setup isn't the next step
		}
		C2D_TextBufClear(tbuf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, rgb(BG));
		C2D_SceneBegin(top);
		text("HOME test", 200, 36, 16, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
		for (int k = 0; k <= TEST_LAST; k++)
			text(TEST_STEP[k], 50, 68 + k * 21, 11, rgb(k <= step ? 0xffffff : GREY), k == step ? BOLD : 0, 0);
		text(">", 34, 68 + step * 21, 11, rgb(GREEN), BOLD, 0);
		C2D_TargetClear(bot, rgb(BG));
		C2D_SceneBegin(bot);
		text("Press HOME. If you come back,", 160, 90, 11, rgb(0xffffff), C2D_AlignCenter, 0);
		text(step < TEST_LAST ? "press A for the next step and HOME again." : "that was the last step.", 160, 110, 11, rgb(0xffffff), C2D_AlignCenter, 0);
		if (audio_ok) text(ai.open && !ai.paused ? "X pauses the music." : "X plays music, to try HOME playing too.", 160, 140, 10, rgb(GREY), C2D_AlignCenter, 0);
		text("START quits.", 160, 160, 10, rgb(GREY), C2D_AlignCenter, 0);
		C3D_FrameEnd(0);
	}
	crumb("test: quit at step %d", step);
	if (step >= 7) { meta_quit(); spot_quit(); meta_exit(); spot_exit(); }
	if (step >= 4) library_exit();
	if (audio_ok) audio_exit();
	if (step >= 6) art_exit();
	if (step >= 7) { sm_free(); net_exit(); }
	if (step >= 3) gspLcdExit();
	if (step >= 2) ptmuExit();
	C2D_TextBufDelete(tbuf);
	C2D_TextBufDelete(mbuf);
	if (nfont) { C2D_FontFree(nfont); nfont = NULL; }
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}

// Closing Settings or Stats (B): the page dips into the background colour the way the pair page does and the tab comes
// up out of it. One page is drawn at a time and nothing slides.
static int dip_t = -2;   // the tab it's going to, -2 when idle
static bool dip_done;
static u64 dip_t0;
static void dip_to(int t) { if (dip_t != -2) return; dip_t = t; dip_done = false; dip_t0 = osGetTime(); }
static void dip_step(void) {   // every frame, after the screens are drawn
	if (dip_t == -2) return;
	float e = (float)(osGetTime() - dip_t0);
	int a;
	if (e < PT_OUT) {
		float k = e / PT_OUT;
		a = (int)(255 * k * k);
	} else {
		if (!dip_done) { dip_done = true; page_to(dip_t); }   // dark enough to change the page under it
		float k = (e - PT_OUT) / PT_IN;
		if (k >= 1) { dip_t = -2; return; }
		a = (int)(255 * (1 - p_ease(k)));
	}
	pair_veil(a);
}

int main(void) {
	gfxInitDefault();
	hidScanInput();
	bool test = hidKeysHeld() & KEY_L;   // the HOME test: see home_test
	if (!test) osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
	crumb("---- 3DStereo build %d%s", BUILD, test ? ", HOME test" : "");
	// which way we were started: an installed title (CIA) or the Homebrew Launcher's 3dsx; their memory and permissions differ
	crumb("launch: %s, heap %lu KB, linear heap %lu KB, app memory %lu KB", envIsHomebrew() ? "3dsx" : "CIA",
	      (unsigned long)(envGetHeapSize() >> 10), (unsigned long)(envGetLinearHeapSize() >> 10),
	      (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10));
	aptHook(&apt_crumbs, on_apt, NULL);
	// four times the default: an artist page or a list of covers and names switches textures for
	// nearly every picture and line, both screens, twice in a swap, and build 29 ran out (svcBreak)
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);
	C2D_Init(16384);   // the swap draws both screens twice
	C2D_Prepare();
	{   // the whole file in one read (C2D_FontLoad goes through stdio's 1 KB buffer); the font keeps its own copy
		FILE *f = fopen(NAMEFONT, "rb");
		if (f) setvbuf(f, NULL, _IONBF, 0);   // unbuffered: the big fread goes to the card in one piece
		long n = 0;
		void *b = NULL;
		if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && n < (4 << 20) && (b = malloc(n))) {
			rewind(f);
			if (fread(b, 1, n, f) == (size_t)n) nfont = C2D_FontLoadFromMem(b, n);
		}
		if (f) fclose(f);
		free(b);
	}
	if (nfont) {
		FINF_s *sys = C2D_FontGetInfo(NULL), *nf = C2D_FontGetInfo(nfont);
		if (sys && nf && sys->ascent && nf->ascent) nf_k = sys->ascent / (22.0f * nf->ascent);
		crumb("start: name font, ascent %d vs the system's %d", nf ? nf->ascent : 0, sys ? sys->ascent : 0);
	} else crumb("start: no name font, the system font for names");
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tbuf = C2D_TextBufNew(8192);
	mbuf = C2D_TextBufNew(1024);
	if (test) return home_test();
	ptmuInit();
	srand(osGetTime());
	hidSetRepeatParameters(18, 5);
	LightLock_Init(&qlock);
	load_cfg();
	dev_name_system();
	pins_load();

	hidScanInput();
	bool asked = !remove(RESCAN);   // Settings asked for it: gone now, whatever else
	bool fresh = (hidKeysHeld() & KEY_R) || asked;   // R held at launch: read every tag again, cache or not
	// HOME is ours to answer from here on: see go_home
	aptSetHomeAllowed(false);
	progress(0, 0, 0, NULL);
	library_scan(fresh, progress);
	splash_bars_finish();
	hidScanInput();   // anything pressed during the read was meant for the read, not for us
	stats_load();
	// the network's threads: locks first, the matcher built once here (the threads only read it)
	net_init();
	spot_init();
	sm_build();
	thumb_ok = calloc(lib.nal + 1, 1);
	C3D_TexInit(&big_tex, 256, 256, GPU_RGBA8);
	at_side = lib.nal + ART_LISTS > 256 ? 1024 : 512;   // 32² tiles: the albums, then the playlists' pictures
	if (!C3D_TexInit(&atlas, at_side, at_side, GPU_RGBA8)) { at_side = 512; C3D_TexInit(&atlas, 512, 512, GPU_RGBA8); }
	at_cols = at_side / 32;
	at_slots = at_cols * at_cols;
	bar_make();
	mic_make();
	qi_make();
	dp_init();
	cc_init();
	home_init();   // after stats_load: with nothing played yet, the charts stand in
	tab = T_HOME;  // every launch opens on Home, as Spotify does
	tab_o[T_HOME] = 1;
	stats_from = T_HOME;
	faces_on = C3D_TexInit(&faces_tex, 512, 512, GPU_RGBA8);   // no room: the initials stay
	if (faces_on) {
		memset(faces_tex.data, 0, faces_tex.size);
		C3D_TexSetFilter(&faces_tex, GPU_LINEAR, GPU_LINEAR);
	}
	memset(big_tex.data, 0, big_tex.size);
	memset(atlas.data, 0, atlas.size);
	C3D_TexSetFilter(&big_tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilter(&atlas, GPU_LINEAR, GPU_LINEAR);
	C3D_TexInit(&ap_head_tex, 512, 256, GPU_RGBA8);
	C3D_TexInit(&ap_tiles, 512, 512, GPU_RGBA8);
	memset(ap_head_tex.data, 0, ap_head_tex.size);
	memset(ap_tiles.data, 0, ap_tiles.size);
	C3D_TexSetFilter(&ap_head_tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilter(&ap_tiles, GPU_LINEAR, GPU_LINEAR);
	art_init();
	// last run's covers and faces, before the first frame: nothing on screen waits for them
	art_thumbs_cached(put_thumb);
	atlas_flush();
	int nfaces = faces_on ? meta_faces_cached(put_face) : 0;
	if (nfaces) C3D_TexFlush(&faces_tex);
	crumb("start: %d faces from the cache", nfaces);
	lists_want();
	meta_init();   // its worker after the faces' file is read: they share what it has
	spot_request(1500, false, true);   // Spotify's lists, once the app's on its feet (if it's paired)
	connect_init();
	connect_set_name(dev_name);
	connect_start();   // Pocket in Spotify's device list (if that's paired too)

	rg_sent = rg_cfg == RG_AUTO ? RG_TRACK : rg_cfg;
	crumb("start: %d songs, sound next", lib.nt);   // a crash at launch says which side of this it was
	bool audio_ok = audio_init(auto_next, quality, rg_sent);
	g_audio_ok = audio_ok;
	crumb("start: sound %s", audio_ok ? "on" : "won't start");
	if (audio_ok) load_resume();
	// The song that's up when Home opens has its cover by then: asked for here and waited on, on the splash
	// (the card keeps a copy after the first time, so it's a few ms; the first ever decode is a few hundred).
	if (audio_ok) {
		AudioInfo a0 = {.track = -1};
		audio_info(&a0);
		if (a0.track >= 0) {
			art_want(a0.track);
			u64 t0 = osGetTime();
			bool got = false;
			while (!quit && osGetTime() - t0 < 2500) {
				int tr;
				const u32 *px;
				u32 col;
				if (art_big(&tr, &px, &col)) {
					if (px) { put(&big_tex, 0, 0, px, ART_BIG); big_alb = tr >= 0 && tr < lib.nt ? lib.t[tr].album_id : -1; big_none = -1; }
					else { big_alb = -1; big_none = tr; }
					top_target = top_color = col;   // no gradient sliding in from grey either
					got = true;
					break;
				}
				progress(0, 0, 0, NULL);   // answers the system, keeps the splash up
			}
			crumb("start: first cover %s in %llu ms", got ? "ready" : "not in time", osGetTime() - t0);
		}
	}
	bool want_swap = swapped;
	if (!audio_ok) swapped = false;   // the touchscreen has to explain why
	if (swapped) set_swap(true);
	if (audio_ok && !quit) intro_play();   // the splash's way into Home: the tile lands on the Home chip, up top in the usual layout, on the touchscreen in the other
	bool splash_due = true;   // freed after the first frame of the main loop, once the last of the intro's has been drawn
	#define IDLE_A_MS 8000    // this long without a press or a touch, and A is play/pause
	#define IDLE_DRAW_MS 1500 // this long without any input and nothing moving by itself: draw every other pass (30 fps)
	u64 last_in = 0;
	bool any_in = false;   // nothing pressed since launch: the first A is the list's, however long it took (Alex)
	bool sleep_ok = true, lcd_off = false, shut = false;
	int touching = 0;
	bool touch_eaten = (hidKeysHeld() & KEY_TOUCH) != 0;   // a touch the side menu came or went under (or the one that skipped the intro)
	bool swiping = false;
	float fv = 0;   // the finger's speed in a swipe, screens per frame
	float hv = 0;   // ...and sideways across the list, pixels per frame
	touchPosition t0 = {0}, tl = {0};
	float s0 = 0, ap_s0 = 0, ap_hs0 = 0;
	int ap_hshelf = -1;      // a shelf on the artist page being slid sideways
	bool ap_hmoved = false;
	bool ap_touch = false;   // the touch began on the artist page (which may close under the finger)
	bool rail_t = false;     // ...on the Songs tab's letters
	bool dragging = false;
	int last_track = -1;
	AudioInfo last = {.track = -1};
	bool jack = headset(audio_ok), hp_doubt = false;
	u64 jack_in_ms = 0, jack_out_ms = 0;   // when the plug last went in / out (the bounce check below)
	bool was_shut = false, lid_paused = false;
	u64 act_ms = osGetTime();   // the last press, stick or touch (for IDLE_DRAW_MS)
	bool draw_skipped = false;  // the pass before this one was a rest

	while (!quit && !cc_go && aptMainLoop()) {
		hidScanInput();
		u32 kd = hidKeysDown(), kr = hidKeysDownRepeat(), kh = hidKeysHeld(), ku = hidKeysUp();
		if (kd | kr | kh | ku) act_ms = osGetTime();
		if (pair_on) { pair_step(kd, kr); kd = kr = kh = ku = 0; }   // the pairing screen has the buttons and the touchscreen
		// lid shut: it's in a pocket, and L and R still reach through the shell, so nothing counts
		u8 shell = 1;
		PTMU_GetShellState(&shell);
		shut = !shell;
		if (shut) {
			kd = kr = kh = ku = 0;
			touching = 0;
			swiping = sw_drag = false;
			sw_armed = false;
			hdrag = false;
			scrub = -1;
			dg_on = false;
		}
		if (kd & KEY_START && kh & KEY_SELECT) break;   // SELECT+START back to the Homebrew Launcher; START alone is the menu's
		// HOME: the DSP handed back first, then we go ourselves; shut in a pocket, it's ignored
		if (aptCheckHomePressRejected() && !shut) {
			if (lcd_off) { backlights(true); lcd_off = false; }   // the lid opened this very frame
			jump_home(audio_ok, true);
			touching = 0;
			swiping = sw_drag = sw_armed = false;
			hdrag = false;
			dg_on = false;
			continue;
		}

		AudioInfo ai = {.track = -1};
		if (audio_ok) audio_info(&ai);

		// A after a while of nothing (or first thing after launch): play or pause, not whatever row a
		// cursor you've forgotten about is sitting on. Any press or touch makes A the list's again.
		bool idle = any_in && osGetTime() - last_in > IDLE_A_MS;
		if (kd || (kh & KEY_TOUCH)) { last_in = osGetTime(); any_in = true; }
		if (idle && (kd & KEY_A) && audio_ok && ai.open && !drawer_on && !(kh & KEY_SELECT)) {
			audio_pause(!ai.paused);
			say(ai.paused ? "Playing" : "Paused");
			kd &= ~KEY_A;
		}

		if (at_on) {
			at_keys(kd, kr);
			u32 m = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B | KEY_X | KEY_Y;
			kd &= ~m;
			kr &= ~m;
		}
		if (pm_on() && !drawer_on) {
			pm_keys(kd, kr);
			u32 m = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B | KEY_X | KEY_Y;
			kd &= ~m;
			kr &= ~m;
		}
		if (drawer_on) {
			drawer_keys(kd, kr);
			u32 m = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B | KEY_X | KEY_Y;
			kd &= ~m;
			kr &= ~m;
		}

		// headphones pulled: pause before the speakers tell the whole room; plugging back in doesn't resume
		bool jack_now = headset(audio_ok);   // every frame: a tenth of a second on the speakers is already too long
		u64 now_ms = osGetTime();
		if (jack_now != jack) {
			crumb("headphones %s", jack_now ? "in" : "out");
			if (jack_now) { jack_in_ms = now_ms; hp_doubt = false; }
			else {
				jack_out_ms = now_ms;
				// the plug's contacts bounce as it goes in: "in, out, in" inside a millisecond (build 65's log), which
				// isn't a pull. An out right after an in waits to see whether it stays out.
				if (now_ms - jack_in_ms < 600) hp_doubt = true;
				else if (ai.open && !ai.paused) { audio_hush(); ai.paused = true; say("Headphones out"); }
			}
		}
		if (hp_doubt && !jack_now && now_ms - jack_out_ms >= 300) {   // it stayed out: it was a pull after all
			hp_doubt = false;
			if (ai.open && !ai.paused) { audio_hush(); ai.paused = true; say("Headphones out"); }
		}
		jack = jack_now;
		// on the speakers, the lid is the pause button: shut pauses, open picks up where it was
		if (shut && !was_shut && !jack_now && ai.open && !ai.paused) { audio_pause(true); ai.paused = true; lid_paused = true; }
		else if (!shut && was_shut && lid_paused) {
			if (ai.open && ai.paused) { audio_pause(false); ai.paused = false; }
			lid_paused = false;
		}
		was_shut = shut;
		stats_heard(ai.track, ai.pos, ai.open && !ai.paused);
		{
			static bool dead_said, clk_said;
			int cy = net_clock_year();
			if (cy && !clk_said) { clk_said = true; char b[48]; snprintf(b, sizeof b, "The 3DS clock says %d: fix Date & Time", cy); say(b); }
			else if (connect_login_dead() && !dead_said) { dead_said = true; say("Spotify login ended: menu, Pair"); }
		}
		if (stats_changed() && tab == T_STATS) dirty = true;
		last = ai;
		art_poll();
		cc_poll();
		crumb_flush();   // the audio worker's lines: it never writes the card itself
		cc_tick();
		dp_poll();
		spot_poll(say, on_reloaded);
		ConnectDo cd;
		if (audio_ok && connect_poll(&ai, &cd)) {
			if (cd.vol_set) audio_volume(cd.vol);
			if (cd.opts_set) {   // Spotify's shuffle and repeat are the 3DS's now
				if (cd.shuffle != shuffle) set_shuffle(cd.shuffle);
				repeat = cd.repeat;
			}
			if (cd.say[0]) say(cd.say);
			connect_apply(&cd, &ai);
			dirty = true;
		}
		connect_options(shuffle, repeat);   // and the 3DS's go up to Spotify
		dg_tick();
		ap_poll();
		lyr_poll(ai.track, lyr_on);
		if (lyr_on && ai.open && lyr_none(ai.track)) lyr_on = false;   // a song with none: back to the cover, not a page saying so
		if (ai.track != last_track) {
			last_track = ai.track;
			int up[4], nu = 0, prev = -1;   // what the 3DS plays next, for Spotify when it follows the 3DS
			LightLock_Lock(&qlock);
			for (int k = 1; k <= 4 && qn > 1; k++) { int at = qpos + k; if (at >= qn) { if (repeat != 1) break; at %= qn; } up[nu++] = q_play[at]; }
			if (qn > 1) prev = qpos > 0 ? q_play[qpos - 1] : repeat == 1 ? q_play[qn - 1] : -1;
			int nqa = qadd < nu ? qadd : nu;
			LightLock_Unlock(&qlock);
			connect_upcoming(up, nu, nqa);
			next_up = nu && !connect_driving() ? up[0] : -1;
			const char *pid = src_kind == T_PLAYLISTS && src_id >= 0 && src_id < lib.nl ? lib.lists[src_id].pid : NULL;
			connect_source(src_kind == T_ALBUMS ? SRC_ALBUM : pid ? SRC_PLAYLIST : SRC_OTHER, pid, qn == 1);
			if (ai.track >= 0) art_want(ai.track);
			int warm[3], nw = 0;   // the covers either side, made now: a skip finds its picture waiting
			if (nu > 0) warm[nw++] = up[0];
			if (nu > 1) warm[nw++] = up[1];
			if (prev >= 0) warm[nw++] = prev;
			art_ahead(warm, nw);
			meta_lyrics_want(ai.track);
			meta_artist_soon(ai.track);
			lyr_y = -1;
		}
		for (int s = 0; s <= 16; s += 8) {   // ease the gradient toward the new cover's colour
			int a = (top_color >> s) & 255, b = (top_target >> s) & 255;
			a += (b - a) / 8 + (b > a) - (b < a);
			top_color = (top_color & ~(255u << s)) | (u32)a << s;
		}
		if (dirty) rebuild();
		int v = V();

		// buttons
		if (audio_ok) {
			// SELECT is only ever the first half of a combo now: SELECT+R changes ReplayGain, SELECT+L syncs
			// with Spotify now. (On its own it used to change the sound when let go; that's in Settings only.)
			if (kh & KEY_SELECT && kd & KEY_DLEFT && !ap_active()) { drawer_open(!drawer_on); kd &= ~KEY_DLEFT; kr &= ~KEY_DLEFT; }
			if (kh & KEY_SELECT && kd & KEY_R) {
				rg_cfg = (rg_cfg + 1) % (RG_AUTO + 1);
				rg_sync();
				say(RGNAME[rg_cfg]);
			} else if (kd & KEY_R) skip(1, &ai);
			if (kh & KEY_SELECT && kd & KEY_L) {
				if (spot_busy()) say("Syncing with Spotify next");   // queued behind the one running
				spot_request(0, true, true);
			} else if (kd & KEY_L) skip(-1, &ai);
			// X likes the song under the cursor while browsing songs, a whole album on the Albums
			// tab, otherwise what's playing
			int apx = ap_x_target();
			if (kh & KEY_SELECT && kd & KEY_X) {   // SELECT+X: the lyrics, on whichever screen the player is
				if (!lyr_on && ai.open && lyr_none(ai.track)) say("No lyrics for this song");
				else { lyr_on = !lyr_on; lyr_y = -1; if (lyr_on) q_on = false; }
			}
			if (kh & KEY_SELECT && kd & KEY_DUP && !ap_active()) {   // SELECT+up: the queue page, on whichever screen the player is
				queue_toggle();
				kd &= ~KEY_DUP; kr &= ~KEY_DUP;
			}
			// circle pad down: queue this thing (the song under the cursor, an album or a playlist whole)
			static u64 cp_t;
			if (kd & KEY_CPAD_DOWN && !(kh & KEY_SELECT) && osGetTime() - cp_t > 300) {
				cp_t = osGetTime();
				bool top_songs_q = detail < 0 && tab == T_STATS && stat_kind == 0;
				if (apx >= 0) queue_add(&apx, 1);
				else if ((songs_view() || top_songs_q) && cursor_vis && nitems) queue_add(&items[cur[v]], 1);
				else if (detail < 0 && tab == T_ALBUMS && cursor_vis && nitems) { Group *g = &lib.albums[items[cur[v]]]; queue_add(g->songs, g->n); }
				else if (detail < 0 && tab == T_PLAYLISTS && cursor_vis && nitems) { Group *g = &lib.lists[items[cur[v]]]; queue_add(g->songs, g->n); }
				else say("Pick a song or an album first");
			}
			// circle pad up, held three seconds: clear the queue. A green line fills across both screens meanwhile.
			static u64 cu_t0;
			static bool cu_done;
			if (kh & KEY_CPAD_UP && !(kh & KEY_SELECT)) {
				if (!cu_t0) { cu_t0 = osGetTime(); cu_done = false; }
				u64 held = osGetTime() - cu_t0;
				qclear_p = cu_done || held < 400 ? 0 : (float)held / 3000.0f;   // a tap or a nudge shows nothing
				if (!cu_done && held >= 3000) { cu_done = true; qclear_p = 0; queue_clear(); }
			} else { cu_t0 = 0; qclear_p = 0; }
			// X: let go quickly and it's the like (or the pin, or the album) as before; held, the song's playlists
			static u64 x_t0;
			static bool x_armed;
			bool x_tap = false;
			if (kd & KEY_X && !(kh & KEY_SELECT)) { x_armed = true; x_t0 = osGetTime(); }
			if (x_armed && !(kh & KEY_X)) { x_armed = false; x_tap = true; }
			else if (x_armed && osGetTime() - x_t0 >= 450) {
				x_armed = false;
				bool top_songs = detail < 0 && tab == T_STATS && stat_kind == 0;
				int t = apx >= 0 ? apx : (songs_view() || top_songs) && cursor_vis && nitems ? items[cur[v]] : ai.open ? ai.track : -1;
				if (t >= 0) at_open(t);
				else say("Pick a song first");
			}
			if (x_tap && apx >= 0) like(apx);
			else if (x_tap) {
				bool top_songs = detail < 0 && tab == T_STATS && stat_kind == 0;
				if (detail < 0 && tab == T_PLAYLISTS && cursor_vis && nitems) {   // a playlist: pin it, or unpin it
					int id = items[cur[v]];
					if (id == 0) say("Liked Songs is always on top");
					else {
						bool on = pin_toggle(lib.lists[id].name);
						say(on ? "Pinned to the top" : "Unpinned");
						rebuild();
						for (int k = 0; k < nitems; k++) if (items[k] == id) { cur[v] = k; break; }   // the cursor goes with it
						reveal(v);
					}
				} else if (detail < 0 && (tab == T_ALBUMS || (tab == T_STATS && stat_kind == 2)) && cursor_vis && nitems) {
					Group *g = &lib.albums[items[cur[v]]];
					bool on = library_like_many(g->songs, g->n);
					say(on ? "Album added to Liked Songs" : "Album removed from Liked Songs");
					dirty = true;
					spot_request(3000, false, false);
				} else {
					int i = (songs_view() || top_songs) && cursor_vis && nitems ? items[cur[v]] : ai.open ? ai.track : -1;
					if (i >= 0) like(i);
				}
			}
		}
		// an artist page has the D-pad, A and B to itself
		bool apv = ap_active();
		if (apv && audio_ok) ap_keys(kd, kr, &ai);
		bool homev = home_active();   // ...and so does Home
		if (homev) home_keys(kd, kr);
		// left and right jump five rows, on every list (Alex: the tabs are ZL and ZR's, and B goes
		// back); Settings keeps them for its values
		bool settings = detail < 0 && tab == T_SETTINGS;
		int step = apv || homev ? 0 : kr & KEY_DUP ? -1 : kr & KEY_DDOWN ? 1 : 0;
		int L = lay();
		bool by_letter = false;
		int to = -1;   // where the cursor goes, worked out here (the grid, the wraps), else cur + step
		if (L == LAY_GRID && !apv && !homev && cursor_vis && nitems && kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) {
			// up and down its column, left and right its row, and off any edge round to the other side
			int c = g_cols(list_w()), k = cur[v] < nitems ? cur[v] : nitems - 1, col = k % c, row = k / c, rows = (nitems + c - 1) / c;
			int inrow = nitems - row * c < c ? nitems - row * c : c;   // the last row can be short
			if (kr & KEY_DUP) { row = row > 0 ? row - 1 : rows - 1; if (row * c + col >= nitems) row--; }
			else if (kr & KEY_DDOWN) {
				if (row + 1 < rows && (row + 1) * c + col < nitems) row++;
				else if (row + 1 < rows) { row = rows - 1; col = nitems - 1 - row * c; }   // the last row's short: its last one
				else row = 0;
			}
			else if (kr & KEY_DLEFT) col = col > 0 ? col - 1 : inrow - 1;
			else col = col + 1 < inrow ? col + 1 : 0;
			to = row * c + col;
			step = to != k;
		} else if (L == LAY_GRID) step = !apv && !homev && kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT) ? 1 : 0;   // hidden: the first press shows it
		if (!apv && !homev && !settings && L != LAY_GRID && kr & (KEY_DLEFT | KEY_DRIGHT)) {
			int d = kr & KEY_DLEFT ? -1 : 1;
			if (L == LAY_SONGS && letters && cursor_vis && nitems) {   // by letter, the last one round to the first
				to = letter_step(cur[v], d);
				if (to == cur[v]) to = d > 0 ? 0 : letter_step(nitems - 1, -1);
				by_letter = true;
				step = to != cur[v];
			} else step = 5 * d;
		}
		// up off the top of a list: round to the bottom; down off the bottom: round to the top
		// (a page with buttons has them at the top, so down off its last song goes there)
		if (to < 0 && cursor_vis && nitems && (step == -1 || step == 1) && !apv && !homev) {
			if (cur[v] + step < 0 && !rich()) to = nitems - 1;
			else if (cur[v] + step >= nitems) {
				if (rich()) { dp_btn = 0; ease_to(&scroll[v], 0); step = 0; }
				else to = 0;
			}
		}
		if (rich() && !cursor_vis && dp_btn >= 0 && scroll[v] > dp_hero(list_w())) dp_btn = -1;   // scrolled down by hand: the songs in view, not back up
		if (rich() && !apv && dp_btn >= 0) {   // on the page's buttons: along them, or down to the songs
			if (kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) {
				if (!cursor_vis) { cursor_vis = true; ease_to(&scroll[v], 0); }
				else if (kr & KEY_DDOWN) { if (nitems) { dp_btn = -1; cur[v] = 0; reveal(v); } }
				else if (kr & KEY_DLEFT) dp_btn = dp_btn > 0 ? dp_btn - 1 : dp_nbtn() - 1;   // round, both ways
				else if (kr & KEY_DRIGHT) dp_btn = dp_btn + 1 < dp_nbtn() ? dp_btn + 1 : 0;
				else ease_to(&scroll[v], 0);
			}
			step = 0;
		} else if (rich() && !apv && cursor_vis && nitems && cur[v] == 0 && kr & KEY_DUP) {   // up off the first song
			dp_btn = 0;
			ease_to(&scroll[v], 0);
			step = 0;
		}
		if (step && nitems) {
			if (!cursor_vis) {
				cursor_vis = true;
				cur[v] = first_shown(v);
			} else cur[v] = to >= 0 ? to : cur[v] + step;
			if (cur[v] < 0) cur[v] = 0;
			if (cur[v] >= nitems) cur[v] = nitems - 1;
			if (by_letter) { reveal_top(v); bubble_t = osGetTime(); }   // the letter's start, at the top, and its letter big for a moment
			else reveal(v);
			vel = 0;
		}
		if (detail < 0 && tab == T_SETTINGS && !apv && kr & (KEY_DLEFT | KEY_DRIGHT) && nitems) {
			if (!cursor_vis) { cursor_vis = true; cur[v] = first_shown(v); }
			else if (items[cur[v]] <= 1 || items[cur[v]] == 3 || items[cur[v]] == 4) set_activate(items[cur[v]], kr & KEY_DLEFT ? -1 : 1);   // sound, ReplayGain, extra details
			kd &= ~(KEY_DLEFT | KEY_DRIGHT);
		}
		// the shoulders flip through the tabs, round and round (on the stats page, its charts);
		// seeking is the bar's job now
		// Settings and the charts have the menu on ZL too (build 98): it's the left end of their row, as it is of Home's
		if (detail < 0 && (tab == T_STATS || tab == T_SETTINGS) && drawer_on) { if (kd & KEY_ZR) drawer_open(false); }
		else if (detail < 0 && tab == T_SETTINGS) { if (kd & KEY_ZL && !apv) drawer_open(true); }
		else if (detail < 0 && tab == T_STATS) {
			if (kd & KEY_ZL && stat_kind == 0 && !apv) drawer_open(true);   // left of the first chart is the menu
			else if (kd & (KEY_ZR | KEY_ZL) && !apv) { stat_kind = (stat_kind + (kd & KEY_ZR ? 1 : 2)) % 3; scroll[T_STATS] = 0; cur[T_STATS] = 0; dirty = true; }
		} else if (tab < NTAB || tab == T_HOME) {   // Home, then the four tabs, round and round
			static const int ORD[NTAB + 1] = {T_HOME, T_PLAYLISTS, T_ARTISTS, T_ALBUMS, T_SONGS};   // as the header has them
			int at = 0;
			while (at < NTAB && ORD[at] != tab) at++;
			if (drawer_on) { if (kd & KEY_ZR) drawer_open(false); }   // the menu's out: ZR back to the tabs
			else if (kd & KEY_ZR) set_tab(ORD[(at + 1) % (NTAB + 1)]);
			else if (kd & KEY_ZL && at == 0) drawer_open(true);   // left of Home is the menu
			else if (kd & KEY_ZL) set_tab(ORD[at - 1]);
		}
		if (kh & KEY_SELECT && kd & KEY_Y) next_sort();
		else if (kd & KEY_Y && (tab < T_STATS || tab == T_HOME)) search();
		if (kd & KEY_A && rich() && dp_btn >= 0 && cursor_vis && audio_ok && !apv) { dp_press(dp_btn, &ai); kd &= ~KEY_A; }
		if (kd & KEY_A && (audio_ok || (detail < 0 && tab == T_SETTINGS)) && !apv && !homev) {
			if (!cursor_vis || !row_shown(v, cur[v])) {
				cursor_vis = true;
				cur[v] = first_shown(v);
			} else activate(cur[v], &ai);
		}
		if (kd & KEY_B && !apv) {
			if (detail >= 0) close_detail();
			else if (tab == T_STATS || tab == T_SETTINGS) dip_to(stats_from);   // B: the pair page's dip, not a cut
			else if (query[0]) { query[0] = 0; dirty = true; }
		}

		// touch: drag the list with a little momentum, tap anything
		if (dirty) rebuild();
		v = V();
		if (drawer_flip) {
			drawer_flip = false;
			if (touching) {   // whatever that finger was doing, it settles as if it had let go
				touch_eaten = true;
				if (swiping) { swiping = sw_drag = false; sw_to = 0; sw_armed = false; }
				hdrag = false;
				scrub = -1;
				ap_hmoved = false;
				dg_on = false;
			}
		}
		if (touch_eaten) {   // a touch the menu came or went under: nothing till it lifts
			if (!(kh & KEY_TOUCH)) { touch_eaten = false; touching = 0; dragging = false; }
		} else if (drawer_on && !swapped) {   // the side menu has the touchscreen while it's out
			if (kh & KEY_TOUCH) {
				touchPosition tp;
				hidTouchRead(&tp);
				if (!touching) t0 = tp;
				tl = tp;
				touching++;
				if (tp.px - t0.px < -30) drawer_open(false);   // swiped back in
			} else if (touching) {
				touching = 0;
				if (abs(tl.px - t0.px) < 10 && abs(tl.py - t0.py) < 10) drawer_tap(t0.px, t0.py);
			}
		} else if (at_on && !swapped) {   // Add to playlist has the touchscreen while it's up
			if (kh & KEY_TOUCH) {
				touchPosition tp;
				hidTouchRead(&tp);
				if (!touching) { t0 = tp; tl = tp; dragging = false; }
				if (abs(tp.py - t0.py) > 6) dragging = true;
				if (dragging) at_drag(tl.py - tp.py);
				tl = tp;
				touching++;
			} else if (touching) {
				touching = 0;
				cursor_vis = false;
				if (!dragging) at_tap(t0.px, t0.py);
				dragging = false;
			}
		} else if (pm_on() && !swapped) {   // the dots' menu or its box has the touchscreen while it's out
			if (kh & KEY_TOUCH) {
				touchPosition tp;
				hidTouchRead(&tp);
				if (!touching) t0 = tp;
				tl = tp;
				touching++;
			} else if (touching) {
				touching = 0;
				cursor_vis = false;
				if (abs(tl.px - t0.px) < 12 && abs(tl.py - t0.py) < 12) pm_tap(t0.px, t0.py);
			}
		} else if (kh & KEY_TOUCH && !touching && sw_busy()) {}   // mid-swap: let it land first
		else if (kh & KEY_TOUCH) {
			touchPosition tp;
			hidTouchRead(&tp);
			if (!touching) {
				ease_clear();
				t0 = tl = tp; s0 = scroll[v]; dragging = swiping = hdrag = false; vel = 0; fv = hv = 0;
				t0_ms = osGetTime();
				dg_on = false;
				ap_touch = ap_active();
				ap_s0 = ap_touch ? (ap_bio ? ap_bio_scroll : AP->scroll) : 0;
				ap_hshelf = -1;
				ap_hmoved = false;
				ap_vel = 0;
				lyr_y0 = lyr_y;
				q_y0 = q_y;
				if (q_on && swapped && ai.open) queue_touch_begin(tp.px, tp.py);
				h_touch = home_active();
				rail_t = !swapped && rail_on() && tp.px >= 302 && tp.py >= LIST0 && tp.py < LIST1;
				h_hmoved = false;
				h_s0 = h_sc;
				h_hs0 = h_hs;
				h_vel = 0;
			}
			// from the top edge down, or the bottom edge up: the screens trade places
			int dx = tp.px - t0.px, dy = tp.py - t0.py;
			if (touching && !swiping && !dragging && audio_ok && abs(dy) > 10 && abs(dy) > abs(dx) &&
			    ((t0.py < 30 && dy > 0) || (t0.py > 216 && dy < 0))) {
				swiping = sw_drag = true;
				scrub = -1;
				sw_start(t0.py < 30 ? 1 : -1);
				audio_tick(TICK_GRAB);
			}
			if (swiping) {
				float p = dy / (float)HH;   // the belt moves with the finger, pixel for pixel
				sw = p > 1 ? 1 : p < -1 ? -1 : p;
				sw_arm(sw * sw_dir > 0.25f);
				fv = fv * 0.5f + (tp.py - tl.py) / (float)HH * 0.5f;
			} else if (swapped) {
				if (on_bar(t0.py)) scrub = bar_frac(tp.px);
				else if (lyr_on && ai.open && t0.py >= 30 && t0.py < 182 && (dragging || abs(dy) > 6)) {   // the words, by hand
					dragging = true;
					lyr_y = lyr_y0 - dy > 0 ? lyr_y0 - dy : 0;   // never below 0: that means "start over"
					lyr_touch_t = osGetTime();
				} else if (q_on && ai.open && t0.py >= 34 && t0.py < 182) queue_touch_move(tp.px, tp.py, t0.px, t0.py, &dragging);   // the queue: scroll, swipe a row, lift one
			} else if (ap_touch && touching && t0.py < LIST1) {   // the artist page: up and down, or a shelf sideways
				if (!ap_active()) {}   // it closed under the finger (B, a tab): nothing left to move
				else if (!dragging && !ap_hmoved) {
					// down from the top edge belongs to the swap, so a scroll can't start there
					if (abs(dy) > 6 && abs(dy) >= abs(dx) && !(t0.py < LIST0 && dy > 0)) dragging = true;
					else if (abs(dx) > 8 && !ap_bio && (ap_hshelf = ap_shelf_at(t0.py)) >= 0) { ap_hmoved = true; ap_hs0 = AP->hs[ap_hshelf]; }
				}
				if (!ap_active()) {}
				else if (dragging) {
					ap_vel = tl.py - tp.py;
					float sc = ap_s0 + (t0.py - tp.py), mx = ap_bio ? ap_bio_max : ap_maxsc;
					sc = sc < 0 ? 0 : sc > mx ? mx : sc;
					if (ap_bio) ap_bio_scroll = sc; else AP->scroll = sc;
					cursor_vis = false;
				} else if (ap_hmoved && ap_hshelf >= 0) {
					float h = ap_hs0 + (t0.px - tp.px);
					AP->hs[ap_hshelf] = h < 0 ? 0 : h;   // the far end is held in shelf()
					cursor_vis = false;
				}
			} else if (h_touch && touching && t0.py >= LIST0 && t0.py < LIST1) {   // Home: up and down, or its shelf sideways
				if (!home_active()) {}   // it opened something under the finger
				else if (!dragging && !h_hmoved) {
					if (t0.px < 24 && dx > 10 && abs(dx) > 2 * abs(dy)) drawer_open(true);   // from the left edge: the side menu
					else if (abs(dy) > 6 && abs(dy) >= abs(dx)) dragging = true;
					else if (abs(dx) > 8 && home_shelf_at(t0.py)) h_hmoved = true;
				}
				if (!home_active() || drawer_flip) {}
				else if (dragging) {
					h_vel = tl.py - tp.py;
					float sc = h_s0 + (t0.py - tp.py);
					h_sc = sc < 0 ? 0 : sc > h_maxsc ? h_maxsc : sc;
					cursor_vis = false;
				} else if (h_hmoved) {
					float h = h_hs0 + (t0.px - tp.px);
					h_hs = h < 0 ? 0 : h;   // the far end is held in home_draw
					cursor_vis = false;
				}
			} else if (rail_t) rail_jump(tp.py);   // the letters: the list follows the finger
			else if (dg_on) dg_move(tp.px, tp.py);   // a song in hand: it follows the finger
			else if (touching && t0.py >= LIST0 && t0.py < LIST1) {
				// held still on a song: it lifts, to be dropped on a playlist
				if (!dragging && !hdrag && !drawer_on && t0.px >= 24 && abs(dx) < 7 && abs(dy) < 7 && osGetTime() - t0_ms >= DG_HOLD &&
				    audio_ok && !(rich() && t0.py < rows_base(320) - scroll[v]) && dg_try(t0.px, t0.py, v)) {}
				// sideways: the stats page in from the left, or back out the way it came
				else if (!dragging && !hdrag && !drawer_on && t0.px < 24 && dx > 10 && abs(dx) > 2 * abs(dy))
					drawer_open(true);   // from the left edge: the side menu (the rest of the touch is its)
				if (hdrag) {
					hoff = nb_side < 0 ? (dx > 0 ? dx : 0) : (dx < 0 ? dx : 0);
					hv = hv * 0.5f + (tp.px - tl.px) * 0.5f;
				} else if (abs(tp.py - t0.py) > 6 && !drawer_flip) dragging = true;
				if (dragging && !drawer_flip) {   // not the frame the side menu came out on
					vel = tl.py - tp.py;
					scroll[v] = s0 + (t0.py - tp.py);
					cursor_vis = false;
				}
			}
			tl = tp;
			touching++;
		} else if (touching) {
			touching = 0;
			if (dg_on) dg_drop(tl.px, tl.py);   // a song let go: onto a playlist, or back
			else if (swiping) {   // past a quarter, or flicked, it goes; otherwise it springs back
				swiping = sw_drag = false;
				float dir = sw_dir;
				sw_to = sw * dir > 0.25f || fv * dir > 0.012f ? dir : 0;
				sw_armed = false;
				sw_v = fv > 0.08f ? 0.08f : fv < -0.08f ? -0.08f : fv;
			} else if (hdrag) {   // past a quarter, or flicked, it goes; otherwise it slides back
				hdrag = false;
				if (hoff * -nb_side > list_w() / 4 || hv * -nb_side > 4) page_commit();
			} else if (swapped) { if (q_on && qg_mode) queue_touch_end(); else if (!dragging) player_tap(t0.px, t0.py, tl.px, &ai); }
			else if (ap_touch && t0.py < LIST1) {   // a page that closed mid-touch: letting go does nothing
				if (!ap_active()) {}
				else if (!dragging && !ap_hmoved) { cursor_vis = false; if (audio_ok) ap_tap(t0.px, t0.py, &ai); }
				else ap_vel = ap_vel > 25 ? 25 : ap_vel < -25 ? -25 : ap_vel;
			} else if (h_touch && t0.py >= LIST0 && t0.py < LIST1) {
				if (!home_active()) {}
				else if (!dragging && !h_hmoved) { cursor_vis = false; home_tap(t0.px, t0.py); }
				else h_vel = h_vel > 25 ? 25 : h_vel < -25 ? -25 : h_vel;
			} else if (rail_t) { rail_t = false; cursor_vis = false; }
			else if (!dragging) {
				int x = t0.px, y = t0.py;
				if (y < LIST0) {
					for (int h = 0; h < nhits; h++)
						if (x >= hits[h].x0 && x < hits[h].x1) {
							int id = hits[h].id;
							if (id < NTAB) set_tab(id);
							else if (id == H_HOME) set_tab(T_HOME);
							else if (id == H_MENU) drawer_open(!drawer_on);
							else if (id >= H_STAT) { stat_kind = id - H_STAT; scroll[T_STATS] = 0; cur[T_STATS] = 0; dirty = true; }
							else if (id == H_SEARCH) search();
							else if (id == H_BACK) close_detail();
							else if (id == H_PLAY && audio_ok && detail >= 0) {
								if (ai.open && playing_here()) audio_pause(!ai.paused);
								else play_list(shuffle && nitems ? rand() % nitems : 0);
							}
							break;
						}
				} else if (y < LIST1) {
					cursor_vis = false;
					if (rich() && audio_ok && dp_tap(x, y, &ai)) {}   // the page's header and buttons
					else if (audio_ok || (detail < 0 && tab == T_SETTINGS)) activate(row_at(x, y, v), &ai);
				} else if (y >= 194 && y < 236 && audio_ok) {
					if (x < 236) { sw_start(-1); sw_to = -1; }   // up from the mini player
					else if (x >= 236 && x < 262) skip(-1, &ai);
					else if (x >= 262 && x < 288) {
						if (ai.open) audio_pause(!ai.paused);
						else play_any();
					} else if (x >= 288) skip(1, &ai);
				}
			} else vel = vel > 25 ? 25 : vel < -25 ? -25 : vel;
		}
		if (!touching && fabsf(vel) > 0.3f) { scroll[v] += vel; vel *= 0.92f; }
		if (scroll[v] < 0) { scroll[v] = 0; vel = 0; }
		if (scroll[v] > max_scroll()) { scroll[v] = max_scroll(); vel = 0; }
		if (ap_n && !touching && fabsf(ap_vel) > 0.3f) {   // the artist page's own momentum
			float *sc = ap_bio ? &ap_bio_scroll : &AP->scroll, mx = ap_bio ? ap_bio_max : ap_maxsc;
			*sc += ap_vel;
			ap_vel *= 0.92f;
			if (*sc < 0) { *sc = 0; ap_vel = 0; }
			if (*sc > mx) { *sc = mx; ap_vel = 0; }
		}
		if (home_active() && !touching && fabsf(h_vel) > 0.3f) {   // Home's
			h_sc += h_vel;
			h_vel *= 0.92f;
			if (h_sc < 0) { h_sc = 0; h_vel = 0; }
			if (h_sc > h_maxsc) { h_sc = h_maxsc; h_vel = 0; }
		}

		// lid: playing music keeps the system awake; the screens go dark on their own
		bool playing = ai.open && !ai.paused;
		art_playing(playing);   // the background goes gently while a song plays
		meta_playing(playing);
		// While Spotify drives, a pause from the PC is a pause, not a goodbye: the system stays up (and
		// its Wi-Fi with it, which sleep takes away) for ten minutes, so play from the PC still lands
		static u64 last_playing;
		if (playing) last_playing = osGetTime();
		// Build 65: Spotify lists Pocket only while the dealer link is open, and a shut lid at rest let the
		// system sleep and take it, so a phone couldn't find Pocket to pair with. While the link is up, the
		// system stays up too (the screens still go dark).
		bool awake = playing || connect_online() || (connect_active() && osGetTime() - last_playing < 10 * 60 * 1000);
		if (sleep_ok == awake) { sleep_ok = !awake; aptSetSleepAllowed(sleep_ok); crumb("sleep %s", sleep_ok ? "allowed" : "held off"); }
		connect_hold_wifi(awake && (connect_active() || connect_online()));   // build 45: awake alone, the shut lid still took the Wi-Fi
		static bool was_lid = false;
		if (shut != was_lid) { was_lid = shut; crumb("lid %s, wifi %d, %s", shut ? "shut" : "open", net_wifi(), awake ? "awake" : "may sleep"); }
		if (shut && !lcd_off && awake) { backlights(false); lcd_off = true; }
		else if (!shut && lcd_off) { backlights(true); lcd_off = false; }
		// Build 66: shut and dark (nobody is looking), the clock drops to 268 MHz unless a song is playing
		// (its decoding wants the speed), and nothing is drawn: a short rest per pass instead of 60 frames a
		// second. The lid opening is still seen within a pass, and the next one draws.
		static bool fast = true;
		bool want_fast = !(shut && lcd_off) || playing;
		if (fast != want_fast) { fast = want_fast; osSetSpeedupEnable(fast); crumb("cpu %s", fast ? "804 MHz" : "268 MHz"); }
		if (shut && lcd_off) { svcSleepThread(100 * 1000000LL); continue; }
		sw_step();
		ease_step();
		tab_spring();
		hl_step();
		home_hl_step();
		drawer_t += ((drawer_on ? 1 : 0) - drawer_t) * 0.3f;
		if (fabsf(drawer_t - (drawer_on ? 1 : 0)) < 0.002f) drawer_t = drawer_on ? 1 : 0;
		if (!hdrag && nb_tab >= 0) {   // the same spring as the swap, sideways
			hoff_v = hoff_v * 0.5f - hoff * 0.1f;
			hoff += hoff_v;
			if (fabsf(hoff) < 0.5f && fabsf(hoff_v) < 0.5f) { hoff = hoff_v = 0; nb_tab = -1; }
		}

		// Build 97: both screens repainted 60 times a second with nobody touching anything. After IDLE_DRAW_MS of no
		// input, and nothing mid-move that needs the rate (a touch, a page change, the pair and clear pages, the hold
		// to clear), every other pass waits for the vblank instead of drawing: the logic above still runs at 60 and
		// the drawing is all clock-driven, so nothing slows down, it just steps at 30. One touch and it's 60 again.
		if (osGetTime() - act_ms > IDLE_DRAW_MS && !touching && !pair_on && !pt_dir && dip_t == -2 && qclear_p <= 0 && !cc_page && !draw_skipped) {
			draw_skipped = true;
			gspWaitForVBlank();
			continue;
		}
		draw_skipped = false;

		u64 frame_t0 = osGetTime();
		static u64 last_end;   // when the previous frame finished: the gap to frame_t0 is input, audio and the rest of the loop
		u64 t_before = last_end ? frame_t0 - last_end : 0;
		if (dirty) rebuild();
		u64 t_rebuilt = osGetTime();
		C2D_TextBufClear(tbuf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);   // waits for the queue: the intro's last frame has stopped reading the splash
		u64 t_began = osGetTime();
		if (splash_due) { splash_free(); splash_due = false; }
		if (audio_ok && (pair_on || pt_dir)) {   // the pair page, and the dip into it and back out (pairview.inc)
			int veil;
			bool page;
			pair_trans_step(&veil, &page);
			if (page) { if (cc_page) draw_clear(); else draw_pair(); }
			else { draw_screen(0, &ai); draw_screen(1, &ai); }
			pair_veil(veil);
		}
		else if (audio_ok) {
			draw_screen(0, &ai); draw_screen(1, &ai);
			if (qclear_p > 0) {   // holding circle pad up: the line fills, and at the end the queue goes
				C2D_SceneBegin(top);
				rect(0, 0, 400 * qclear_p, 3, rgb(GREEN));
				text("Hold to clear the queue", 200, 20, 10, rgba(0xffffff, 200), C2D_AlignCenter, 0);
				C2D_SceneBegin(bot);
				rect(0, 0, 320 * qclear_p, 3, rgb(GREEN));
			}
			dip_step();
		}
		else {
			C2D_TargetClear(top, rgb(BG));
			C2D_SceneBegin(top);
			draw_player(false, 400, &ai);
			C2D_TargetClear(bot, rgb(BG));
			C2D_SceneBegin(bot);
			text("Sound won't start.", 160, 100, 14, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
			text("sdmc:/3ds/dspfirm.cdc is missing: dump it with", 160, 122, 9, rgb(GREY), C2D_AlignCenter, 0);
			text("Rosalina > Miscellaneous options. START quits.", 160, 136, 9, rgb(GREY), C2D_AlignCenter, 0);
		}
		u64 t_drawn = osGetTime();
		C3D_FrameEnd(0);
		u64 t_end = osGetTime();
		u64 frame_ms = t_end - frame_t0;
		last_end = t_end;
		// so a lag on a tab can be read off the log, and where it went: the loop before the frame (input, audio), the
		// rebuild, the wait for the GPU's last frame (begin), our drawing (cpu), the swap (end), and what was running
		if (frame_ms > 60 || t_before > 60)
			crumb_late("slow frame: %u ms (tab %d, detail %d): before %u, rebuild %u, begin %u, draw %u, end %u; spot %d drawer %.2f",
			      (unsigned)frame_ms, tab, detail, (unsigned)t_before, (unsigned)(t_rebuilt - frame_t0), (unsigned)(t_began - t_rebuilt),
			      (unsigned)(t_drawn - t_began), (unsigned)(t_end - t_drawn), spot_busy(), drawer_t);
	}

	crumb_flush();
	crumb_flush_late();
	if (lcd_off) backlights(true);
	aptSetSleepAllowed(true);
	if (!swap_touched) swapped = want_swap;   // without sound it ran with the screens unswapped: that was not a choice
	save_cfg();
	stats_flush();
	meta_quit();   // both told first: either may be waiting on the other's Spotify login
	spot_quit();
	connect_hold_wifi(false);
	connect_quit();
	meta_exit();
	connect_exit();   // before spot_exit: its song lookups use Pocket's Spotify login
	spot_exit();   // before the saver closes: a sync's last likes still go onto the card
	library_exit();
	if (audio_ok) save_resume(&last);
	if (audio_ok) audio_exit();
	art_exit();
	if (cc_go) cc_run();   // Settings > Clear cache: every worker is joined and nothing writes the files back now
	ap_clear();
	meta_lyrics_free(lyr);
	sm_free();
	net_exit();
	C3D_TexDelete(&big_tex);
	C3D_TexDelete(&atlas);
	if (faces_on) C3D_TexDelete(&faces_tex);
	if (bar_ok) C3D_TexDelete(&bar_tex);
	if (mic_ok) C3D_TexDelete(&mic_tex);
	if (qi_ok) C3D_TexDelete(&qi_tex);
	if (dp_tex_ok) C3D_TexDelete(&dp_tex);
	if (cc_tex_ok) C3D_TexDelete(&cc_tex);
	C3D_TexDelete(&ap_head_tex);
	C3D_TexDelete(&ap_tiles);
	C2D_TextBufDelete(tbuf);
	C2D_TextBufDelete(mbuf);
	ptmuExit();
	if (nfont) { C2D_FontFree(nfont); nfont = NULL; }
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
