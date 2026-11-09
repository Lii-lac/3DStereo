// A low-priority thread on our own core: it decodes while the main thread waits for vblank,
// so the UI never stalls on a JPEG.

#include "cardio.h"
#include "art.h"
#include "audio.h"
#include "library.h"
#include "meta.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "stb_image.h"

static LightLock lock;
static LightEvent wake;
static volatile bool run = true;
static Thread th;

static int want = -1;                  // big cover asked for
static int done = -1;                  // the last one decoded (or found missing)
static bool fresh, big_ok;             // done's result not yet handed over; whether it had art
static u32 big_px[ART_BIG * ART_BIG], big_color;

// The covers decoded ahead (art_ahead: the songs either side of the one playing) and the last few played, by
// album: skipping finds its picture already waiting, handed over from art_want itself, so it never blanks.
#define BIGC 5
static struct { int album; bool ok; u32 color, age; u32 *px; } bigc[BIGC];   // px: heap, from art_init (NULL: no cache)
static u32 *bigc_mem, bigc_seq;
static int ahead[4], ahead_n;
static int bigc_find(int album) {   // under lock
	for (int k = 0; k < BIGC; k++) if (bigc[k].px && bigc[k].album == album) return k;
	return -1;
}
static void bigc_put(int album, bool ok, u32 color, const u32 *px) {   // under lock: an empty place, else the one used longest ago
	int k = bigc_find(album);
	if (k < 0) {
		for (int j = 0; j < BIGC; j++) {
			if (!bigc[j].px) continue;
			if (bigc[j].album < 0) { k = j; break; }
			if (k < 0 || bigc[j].age < bigc[k].age) k = j;
		}
	}
	if (k < 0) return;
	bigc[k].album = album;
	bigc[k].ok = ok;
	bigc[k].color = color;
	bigc[k].age = ++bigc_seq;
	if (ok) memcpy(bigc[k].px, px, ART_BIG * ART_BIG * 4);
}

// the page cover: the newest request and the one answered, what it asked for (copied in), what came
static int d_req, d_done;
static int d_track = -1;
static char d_path[192], d_av[192];
static u32 d_px[ART_DETAIL * ART_DETAIL], d_avpx[ART_AVATAR * ART_AVATAR], d_color;
static bool d_ok, d_avok, d_fresh;

static volatile bool playing;          // art_playing
static int next_album;                 // thumbnails: the next album to try
static int thumb_q[64], thumb_n;       // finished, waiting for the main thread
static u32 *thumbs;                    // ART_THUMB² per album
static u8 *tried, *have;               // per album: looked at (from the card or the cache); has a cover
static int need[32], nneed;            // on screen right now and still missing: these first, newest first

// The playlists' own pictures, by list index (art_lists_want)
static char *l_path[ART_LISTS];
static int l_n, l_next, l_gen;
static u8 l_state[ART_LISTS];          // 0 to do, 1 ready for the main thread, 2 handed over or none
static u32 (*l_px)[ART_THUMB * ART_THUMB];   // ART_LISTS of them, on the heap (art_init)

// The Home page's covers, by slot (art_home_want): what each is to be, and the finished pixels
#define H2 (ART_HOME * ART_HOME)
// The big buffers live on the heap, not in the program's image: build 50's image grew past 3 MB with
// them in it, and the HOME Menu crashed in the kernel launching the CIA, as it had for build 39's
static struct { u8 state; int gen, track; u32 seq; char path[192], artist[128]; } *h_job;   // ART_SLOTS; state 1: to make
static u32 h_seq;                      // the order they were asked in: the newest is on screen
#define H_OUT 6
static struct { int slot, gen; u32 *px; } h_out[H_OUT];   // finished, for the main thread
static u32 *h_tmp, *p_tmp;             // the worker's: a cover being made, one ahead of time
static int h_nout;
static int h_pre;                      // the pass ahead of time: the next album to shrink

// ---- the thumbnails' cache on the card ---------------------------------------------------------
// Every album's 32² from last time, so a launch shows covers at once instead of decoding every
// JPEG again (the embedded ones are often 3000² and a fifth of a second each). Keyed by the album
// and its first song's file: a retag or a new file under the name makes it miss and decode again.
#define TCACHE "sdmc:/3ds/player/thumbs.cache"
#define TMAGIC 0x31485450u   // "PTH1"
#define T2 (ART_THUMB * ART_THUMB)
static u32 *tc_key, *tc_ok, *tc_px;    // last launch's: n keys, whether each had a cover, its pixels
static int tc_n;
static bool tc_dirty, tc_saved;

static bool decode(const Track *t, int s, int r, u32 *out, u32 *color);
static u32 fnv_s(u32 h, const char *s) { if (s) while (*s) h = (h ^ (u8)*s++) * 16777619u; return (h ^ 0xff) * 16777619u; }
static u32 album_key(int a) {
	Group *g = &lib.albums[a];
	u32 h = fnv_s(fnv_s(2166136261u, g->name), g->artist);
	if (g->n) {
		const Track *t = &lib.t[g->songs[0]];
		h = fnv_s(h, t->path);
		u32 v[4] = {(u32)t->size, (u32)t->mtime, t->pic_off, t->pic_len};
		for (int k = 0; k < 4; k++) h = (h ^ v[k]) * 16777619u;
	}
	return h;
}
static void tc_load(void) {
	FILE *f = card_open_big(TCACHE, "rb");
	if (!f) return;
	u32 hd[3];
	if (fread(hd, 4, 3, f) == 3 && hd[0] == TMAGIC && hd[1] == ART_THUMB && hd[2] < 100000) {
		int n = hd[2];
		tc_key = malloc(n * 4 + 4);
		tc_ok = malloc(n * 4 + 4);
		tc_px = malloc((size_t)n * T2 * 4 + 4);
		int k = 0;
		for (; tc_key && tc_ok && tc_px && k < n; k++) {
			u32 r[2];
			if (fread(r, 4, 2, f) != 2) break;
			tc_key[k] = r[0];
			tc_ok[k] = r[1];
			if (r[1] && fread(tc_px + (size_t)k * T2, 4, T2, f) != T2) break;
		}
		tc_n = k;
	}
	fclose(f);
}
static int tc_find(u32 key) { for (int k = 0; k < tc_n; k++) if (tc_key[k] == key) return k; return -1; }
static void tc_save(void) {   // every album looked at, through a .tmp so a cut-off file isn't read
	FILE *f = card_open_big(TCACHE ".tmp", "wb");
	if (!f) return;
	u32 n = 0;
	for (int a = 0; a < lib.nal; a++) n += tried[a];
	u32 hd[3] = {TMAGIC, ART_THUMB, n};
	bool ok = fwrite(hd, 4, 3, f) == 3;
	for (int a = 0; a < lib.nal && ok; a++) {
		if (!tried[a]) continue;
		u32 r[2] = {album_key(a), have[a]};
		ok = fwrite(r, 4, 2, f) == 2 && (!have[a] || fwrite(thumbs + (size_t)a * T2, 4, T2, f) == T2);
	}
	ok = !fclose(f) && ok;
	if (ok) { remove(TCACHE); ok = !rename(TCACHE ".tmp", TCACHE); }
	if (!ok) remove(TCACHE ".tmp");
}

// One album's thumbnail into thumbs, from the cache or the card; true if it has a cover. *disk:
// it was decoded (the slow way).
static bool thumb_make(int a, bool *disk) {
	u32 *dst = thumbs + (size_t)a * T2;
	int c = tc_find(album_key(a));
	*disk = c < 0;
	if (c >= 0) {
		if (tc_ok[c]) memcpy(dst, tc_px + (size_t)c * T2, T2 * 4);
		return tc_ok[c];
	}
	tc_dirty = true;
	Group *g = &lib.albums[a];
	bool ok = false;
	for (int k = 0; k < g->n && !ok; k++) ok = decode(&lib.t[g->songs[k]], ART_THUMB, 2, dst, NULL);
	return ok;
}
// ---- shrunk pictures kept on the card ----------------------------------------------------------
// A page's cover (96²) and owner picture (32²), and a playlist's tile (32²), already decoded and
// shrunk, beside the picture they came from (<picture>.d.raw / .a.raw / .t.raw) or, for an album,
// under covers/ by the album's key; the colour rides at the end. Read back in a few ms instead of
// decoding a JPEG. The sync deletes a playlist's when its picture changes (spotify.c fetch_pic).
#define COVERS "sdmc:/3ds/player/covers"
// covers/ is split into 16 folders by the top nibble of the key (covers/3/3f2a91c0.b.raw; build 100, it was 256 two-digit ones
// in builds 96 to 99, and walking 256 folders made Clear cache's count crawl): FAT searches a folder from its
// start for every open and stat, so one folder with a file per album got slower with every album (build 96)
static void cover_name(char *out, int cap, const char *pre, unsigned long key, char kind) {
	snprintf(out, cap, COVERS "/%lx/%s%08lx.%c.raw", (key >> 28) & 0xf, pre, key, kind);
}
static void raw_name(char *out, int cap, int track, const char *path, char kind) {
	if (track >= 0 && track < lib.nt) cover_name(out, cap, "", (unsigned long)album_key(lib.t[track].album_id), kind);
	else snprintf(out, cap, "%s.%c.raw", path, kind);
}
static bool raw_read(const char *p, u32 *px, int n, u32 *color) {   // n pixels, then the colour
	FILE *f = card_open_big(p, "rb");
	if (!f) return false;
	u32 c = 0;
	bool ok = fread(px, 4, n, f) == (size_t)n && fread(&c, 4, 1, f) == 1;
	fclose(f);
	if (ok && color) *color = c;
	return ok;
}
static void raw_write(const char *p, const u32 *px, int n, u32 color) {
	FILE *f = card_open_big(p, "wb");
	if (!f && !strncmp(p, COVERS, strlen(COVERS))) {   // first one in covers/, or in its folder
		char d[sizeof COVERS + 4];
		mkdir(COVERS, 0777);
		snprintf(d, sizeof d, "%.*s", (int)sizeof COVERS + 1, p);   // ".../covers/3"
		mkdir(d, 0777);
		f = card_open_big(p, "wb");
	}
	if (!f) return;
	bool ok = fwrite(px, 4, n, f) == (size_t)n && fwrite(&color, 4, 1, f) == 1;
	if (fclose(f) || !ok) remove(p);
}

static void thumb_done(int a, bool ok) {   // under lock
	tried[a] = 1;
	have[a] = ok;
	if (ok) thumb_q[thumb_n++] = a;
}

// The embedded picture, or a cover file in the song's folder.
static u8 *read_cover(const Track *t, u32 *len) {
	u8 *b = NULL;
	if (t->pic_len && t->pic_len < (8 << 20)) {
		FILE *f = card_open_big(t->path, "rb");
		if (f) {
			b = malloc(t->pic_len);
			if (b && (fseek(f, t->pic_off, SEEK_SET) || fread(b, 1, t->pic_len, f) != t->pic_len)) { free(b); b = NULL; }
			fclose(f);
			if (b) { *len = t->pic_len; return b; }
		}
	}
	static const char *NAMES[] = {"cover.jpg", "folder.jpg", "cover.png", "folder.png", "Cover.jpg", "Folder.jpg"};
	char p[768];
	const char *e = strrchr(t->path, '/');
	int dl = e ? e - t->path + 1 : 0;
	for (int k = 0; k < 6; k++) {
		snprintf(p, sizeof p, "%.*s%s", dl, t->path, NAMES[k]);
		FILE *f = card_open_big(p, "rb");
		if (!f) continue;
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (n > 0 && n < (8 << 20) && (b = malloc(n)) && fread(b, 1, n, f) == (size_t)n) { fclose(f); *len = n; return b; }
		free(b);
		b = NULL;
		fclose(f);
	}
	return NULL;
}

static u8 *read_file(const char *p, u32 *len) {
	FILE *f = card_open_big(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *b = n > 0 && n < (8 << 20) ? malloc(n) : NULL;
	if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
	fclose(f);
	if (b) *len = n;
	return b;
}

// Centre square, box-averaged down to s×s, corners rounded by r (r = s/2: a circle). The colour
// leans toward the saturated pixels, the way Spotify's gradients pick the cover's colour and not
// its grey. Takes b and frees it.
static bool decode_bytes(u8 *b, u32 len, int s, int r, u32 *out, u32 *color);
static bool decode(const Track *t, int s, int r, u32 *out, u32 *color) {
	u32 len = 0;
	u8 *b = read_cover(t, &len);
	return b && decode_bytes(b, len, s, r, out, color);
}
static bool decode_bytes(u8 *b, u32 len, int s, int r, u32 *out, u32 *color) {
	int w, h, ch;
	u8 *img = NULL;
	if (stbi_info_from_memory(b, len, &w, &h, &ch) && (s64)w * h <= 3000 * 3000)
		img = stbi_load_from_memory(b, len, &w, &h, &ch, 4);
	free(b);
	if (!img) return false;

	int m = w < h ? w : h, ox = (w - m) / 2, oy = (h - m) / 2;
	double cr = 0, cg = 0, cb = 0, cw = 0;
	for (int y = 0; y < s; y++) {
		int y0 = oy + y * m / s, y1 = oy + (y + 1) * m / s;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < s; x++) {
			int x0 = ox + x * m / s, x1 = ox + (x + 1) * m / s;
			if (x1 <= x0) x1 = x0 + 1;
			u32 sr = 0, sg = 0, sb = 0, n = 0;
			for (int yy = y0; yy < y1; yy++)
				for (const u8 *p = img + 4 * (yy * w + x0), *e = p + 4 * (x1 - x0); p < e; p += 4) { sr += p[0]; sg += p[1]; sb += p[2]; n++; }
			u32 pr = sr / n, pg = sg / n, pb = sb / n;
			u32 hi = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
			u32 lo = pr < pg ? (pr < pb ? pr : pb) : (pg < pb ? pg : pb);
			double wt = (hi - lo) + 8.0;
			cr += pr * wt; cg += pg * wt; cb += pb * wt; cw += wt;
			// alpha 0 outside the rounded corners
			u32 a;
			if (2 * r >= s) {   // a circle, its edge blended over a pixel
				float fx = x + 0.5f - s / 2.0f, fy = y + 0.5f - s / 2.0f, d = s / 2.0f - sqrtf(fx * fx + fy * fy);
				a = d >= 1 ? 255 : d <= 0 ? 0 : (u32)(d * 255);
			} else {
				int dx = x < r ? r - x : x >= s - r ? x - (s - r - 1) : 0;
				int dy = y < r ? r - y : y >= s - r ? y - (s - r - 1) : 0;
				a = dx * dx + dy * dy > r * r ? 0 : 255;
			}
			out[y * s + x] = pr << 24 | pg << 16 | pb << 8 | a;
		}
	}
	stbi_image_free(img);
	if (color) *color = (u32)(cr / cw) << 16 | (u32)(cg / cw) << 8 | (u32)(cb / cw);
	return true;
}

// A Home cover's shrunk copy on the card: an album's under covers/ by its key, a playlist's beside
// its picture, an artist's under covers/ by their name
static void home_raw(char *out, int cap, int track, const char *path, const char *artist) {
	if (artist[0]) cover_name(out, cap, "r", (unsigned long)fnv_s(2166136261u, artist), 'g');
	else raw_name(out, cap, track, path, 'g');   // 'g': 88 px (build 49's 80 px ones were 'h')
}
static bool home_make(int track, const char *path, const char *artist, u32 *px) {
	u32 len = 0;
	if (artist[0]) {   // the photo the Artists tab's face is made from, kept by the work ahead of time
		char p[192];
		u8 *b = meta_photo_file(artist, p, sizeof p) ? read_file(p, &len) : NULL;
		return b && decode_bytes(b, len, ART_HOME, ART_HOME / 2, px, NULL);
	}
	if (track >= 0 && track < lib.nt) {   // the song's cover, else any other song's on the album
		if (decode(&lib.t[track], ART_HOME, 3, px, NULL)) return true;
		Group *g = &lib.albums[lib.t[track].album_id];
		for (int k = 0; k < g->n; k++) if (g->songs[k] != track && decode(&lib.t[g->songs[k]], ART_HOME, 3, px, NULL)) return true;
		return false;
	}
	u8 *b = path[0] ? read_file(path, &len) : NULL;
	return b && decode_bytes(b, len, ART_HOME, 3, px, NULL);
}

// A song's big cover: the card's shrunk copy if an earlier play left one, else the JPEG decoded (and the copy kept).
static bool big_load(int track, u32 *px, u32 *c) {
	char rp[220];
	*c = lib.albums[lib.t[track].album_id].color;
	raw_name(rp, sizeof rp, track, "", 'b');
	if (raw_read(rp, px, ART_BIG * ART_BIG, c)) return true;
	*c = lib.albums[lib.t[track].album_id].color;
	bool ok = decode(&lib.t[track], ART_BIG, 4, px, c);
	if (ok) raw_write(rp, px, ART_BIG * ART_BIG, *c);
	return ok;
}

static void loader(void *arg) {
	(void)arg;
	static u32 px[ART_BIG * ART_BIG];
	while (run) {
		LightLock_Lock(&lock);
		int w = want;
		bool big = w >= 0 && w != done && w < lib.nt;
		bool qfull = thumb_n == 64;
		LightLock_Unlock(&lock);

		if (big) {
			u32 c;
			bool ok = big_load(w, px, &c);
			LightLock_Lock(&lock);
			bigc_put(lib.t[w].album_id, ok, c, px);
			if (want == w) {   // still wanted
				if (ok) memcpy(big_px, px, sizeof px);
				big_color = c;
				big_ok = ok;
				done = w;
				fresh = true;
			}
			LightLock_Unlock(&lock);
			continue;
		}
		// the songs either side of this one: their covers are made now, while nothing else is asked for
		if (bigc[0].px) {
			int aw = -1;
			LightLock_Lock(&lock);
			for (int k = 0; k < ahead_n && aw < 0; k++) {
				int tk = ahead[k];
				if (tk >= 0 && tk < lib.nt && bigc_find(lib.t[tk].album_id) < 0) aw = tk;
			}
			LightLock_Unlock(&lock);
			if (aw >= 0) {
				static u32 ax[ART_BIG * ART_BIG];
				u32 c;
				bool ok = big_load(aw, ax, &c);
				LightLock_Lock(&lock);
				bigc_put(lib.t[aw].album_id, ok, c, ax);
				LightLock_Unlock(&lock);
				continue;
			}
		}
		LightLock_Lock(&lock);
		int dr = d_req, dt = d_track;
		bool det = dr != d_done;
		char dp[192], da[192];
		snprintf(dp, sizeof dp, "%s", d_path);
		snprintf(da, sizeof da, "%s", d_av);
		LightLock_Unlock(&lock);
		if (det) {   // a page's cover, after the playing one and before the thumbnails
			static u32 cpx[ART_DETAIL * ART_DETAIL], apx[ART_AVATAR * ART_AVATAR];
			u32 c = dt >= 0 && dt < lib.nt ? lib.albums[lib.t[dt].album_id].color : 0x404040, len = 0;
			bool ok = false, aok = false;
			char rp[220];
			if (dt >= 0 || dp[0]) {   // the shrunk copy on the card, else the picture, shrunk and kept
				raw_name(rp, sizeof rp, dt, dp, 'd');
				if (!(ok = raw_read(rp, cpx, ART_DETAIL * ART_DETAIL, &c))) {
					if (dt >= 0 && dt < lib.nt) ok = decode(&lib.t[dt], ART_DETAIL, 3, cpx, &c);
					else { u8 *b = read_file(dp, &len); ok = b && decode_bytes(b, len, ART_DETAIL, 3, cpx, &c); }
					if (ok) raw_write(rp, cpx, ART_DETAIL * ART_DETAIL, c);
				}
			}
			if (da[0]) {
				raw_name(rp, sizeof rp, -1, da, 'a');
				if (!(aok = raw_read(rp, apx, ART_AVATAR * ART_AVATAR, NULL))) {
					u8 *b = read_file(da, &len);
					aok = b && decode_bytes(b, len, ART_AVATAR, ART_AVATAR / 2, apx, NULL);
					if (aok) raw_write(rp, apx, ART_AVATAR * ART_AVATAR, 0);
				}
			}
			LightLock_Lock(&lock);
			if (d_req == dr) {   // still the one wanted
				if (ok) memcpy(d_px, cpx, sizeof cpx);
				if (aok) memcpy(d_avpx, apx, sizeof apx);
				d_ok = ok;
				d_avok = aok;
				d_color = c;
				d_fresh = true;
			}
			if (dr > d_done) d_done = dr;   // a newer one may have been answered from the card already
			LightLock_Unlock(&lock);
			continue;
		}
		// big covers on screen (Home, the grids): the card's shrunk copy, else made from the picture
		LightLock_Lock(&lock);
		int hs = -1, hg = 0, ht = -1;
		char hp[192] = "", ha[128] = "";
		if (h_nout < H_OUT && h_job && h_tmp)
			for (int k = 0; k < ART_SLOTS; k++)
				if (h_job[k].state == 1 && (hs < 0 || h_job[k].seq > h_job[hs].seq)) hs = k;
		if (hs >= 0) {
			hg = h_job[hs].gen; ht = h_job[hs].track;
			snprintf(hp, sizeof hp, "%s", h_job[hs].path);
			snprintf(ha, sizeof ha, "%s", h_job[hs].artist);
		}
		LightLock_Unlock(&lock);
		if (hs >= 0 && h_tmp) {
			u32 *hpx = h_tmp;
			char rp[220];
			home_raw(rp, sizeof rp, ht, hp, ha);
			bool ok = raw_read(rp, hpx, H2, NULL);
			if (!ok) {
				audio_make_room(1500);   // a cover can be a big read: the song's first
				if ((ok = home_make(ht, hp, ha, hpx))) raw_write(rp, hpx, H2, 0);
			}
			LightLock_Lock(&lock);
			if (h_job[hs].gen == hg) {   // still what the slot wants
				h_job[hs].state = 0;
				if (ok && h_nout < H_OUT) { h_out[h_nout].slot = hs; h_out[h_nout].gen = hg; memcpy(h_out[h_nout].px, hpx, H2 * 4); h_nout++; }
			}
			LightLock_Unlock(&lock);
			continue;
		}
		// the thumbnails on screen, before anything ahead of time and with no pause
		LightLock_Lock(&lock);
		int a = -1;
		while (!qfull && nneed && a < 0) { int x = need[--nneed]; if (x >= 0 && x < lib.nal && !tried[x]) a = x; }
		LightLock_Unlock(&lock);
		if (a >= 0) {
			bool disk, ok = thumb_make(a, &disk);
			LightLock_Lock(&lock);
			thumb_done(a, ok);
			LightLock_Unlock(&lock);
			continue;
		}
		// the playlists' own pictures, a handful
		LightLock_Lock(&lock);
		int lk = l_next < l_n ? l_next++ : -1, lg = l_gen;
		char lp[192] = "";
		if (lk >= 0 && l_path[lk]) snprintf(lp, sizeof lp, "%s", l_path[lk]);
		LightLock_Unlock(&lock);
		if (lk >= 0) {
			static u32 tpx[T2];
			u32 len = 0;
			char rp[220];
			raw_name(rp, sizeof rp, -1, lp, 't');
			bool ok = lp[0] && raw_read(rp, tpx, T2, NULL);
			if (!ok && lp[0]) {
				u8 *b = read_file(lp, &len);
				if ((ok = b && decode_bytes(b, len, ART_THUMB, 2, tpx, NULL))) raw_write(rp, tpx, T2, 0);
			}
			LightLock_Lock(&lock);
			if (lg == l_gen) {
				ok = ok && l_px;
				if (ok) memcpy(l_px[lk], tpx, sizeof tpx);
				l_state[lk] = ok ? 1 : 2;
			}
			LightLock_Unlock(&lock);
			continue;
		}
		// every other album, one by one, and never while the song is short of sound (build 38)
		audio_make_room(3000);
		while (next_album < lib.nal && tried[next_album]) next_album++;
		if (next_album < lib.nal && !qfull) {
			a = next_album++;
			bool disk, ok = thumb_make(a, &disk);
			LightLock_Lock(&lock);
			thumb_done(a, ok);
			LightLock_Unlock(&lock);
			if (disk && playing) svcSleepThread(120 * 1000000LL);   // a cover's a big read: room between, for the song
			continue;
		}
		if (next_album >= lib.nal && tc_dirty && !tc_saved) { tc_saved = true; tc_save(); }   // once, when every album's been seen
		// every album's big cover onto the card, ahead of the grid (a stat each when it's there already)
		if (next_album >= lib.nal && h_pre < lib.nal && !qfull && p_tmp) {
			int pa = h_pre++;
			Group *g = &lib.albums[pa];
			char rp[220];
			struct stat st;
			if (g->n) home_raw(rp, sizeof rp, g->songs[0], "", "");
			if (g->n && stat(rp, &st)) {
				u32 *ppx = p_tmp;
				audio_make_room(3000);
				if (home_make(g->songs[0], "", "", ppx)) raw_write(rp, ppx, H2, 0);
				if (playing) svcSleepThread(150 * 1000000LL);   // room between, for the song
			}
			continue;
		}
		LightEvent_Wait(&wake);
	}
}

void art_init(void) {
	LightLock_Init(&lock);
	LightEvent_Init(&wake, RESET_ONESHOT);
	thumbs = malloc((lib.nal ? lib.nal : 1) * ART_THUMB * ART_THUMB * 4);
	tried = calloc(lib.nal + 1, 1);
	have = calloc(lib.nal + 1, 1);
	tc_load();
	l_px = calloc(ART_LISTS, sizeof *l_px);
	h_job = calloc(ART_SLOTS, sizeof *h_job);
	bigc_mem = malloc((size_t)BIGC * ART_BIG * ART_BIG * 4);
	for (int k = 0; k < BIGC; k++) { bigc[k].album = -1; bigc[k].px = bigc_mem ? bigc_mem + (size_t)k * ART_BIG * ART_BIG : NULL; }
	h_tmp = malloc(H2 * 4);
	p_tmp = malloc(H2 * 4);
	u32 *o = malloc((size_t)H_OUT * H2 * 4);
	for (int k = 0; k < H_OUT; k++) h_out[k].px = o ? o + (size_t)k * H2 : NULL;
	if (!o) h_tmp = NULL;   // no room for finished ones: none made
	for (int a = 0; a < lib.nal && thumbs && tried && have; a++) {   // last run's, before the thread starts
		int c = tc_find(album_key(a));
		if (c < 0) continue;
		if (tc_ok[c]) memcpy(thumbs + (size_t)a * T2, tc_px + (size_t)c * T2, T2 * 4);
		tried[a] = 1;
		have[a] = tc_ok[c];
	}
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	th = threadCreate(loader, NULL, 64 * 1024, prio + 1, -2, false);
}

void art_exit(void) {
	run = false;
	LightEvent_Signal(&wake);
	if (th) { threadJoin(th, U64_MAX); threadFree(th); }
	if (tc_dirty && !tc_saved && tried) tc_save();   // closed before the walk finished: what's done so far
	free(bigc_mem);
	bigc_mem = NULL;
	for (int k = 0; k < BIGC; k++) bigc[k].px = NULL;
	free(thumbs);
	free(tried);
	free(have);
	free(tc_key);
	free(tc_ok);
	free(tc_px);
	for (int k = 0; k < ART_LISTS; k++) free(l_path[k]);
}

void art_thumbs_cached(void (*put)(int, const u32 *)) {
	for (int a = 0; a < lib.nal && tried && have; a++)
		if (tried[a] && have[a]) put(a, thumbs + (size_t)a * T2);
}

void art_thumb_need(int album) {
	if (album < 0 || album >= lib.nal) return;
	LightLock_Lock(&lock);
	bool add = tried && !tried[album];
	for (int k = nneed - 1; k >= 0 && k >= nneed - 4 && add; k--) add = need[k] != album;   // asked a frame ago: still waiting
	if (add) {
		if (nneed == 32) { memmove(need, need + 1, 31 * sizeof *need); nneed--; }   // the oldest one has scrolled away
		need[nneed++] = album;
	}
	LightLock_Unlock(&lock);
	if (add) LightEvent_Signal(&wake);
}

void art_lists_want(char *const *paths, int n) {
	LightLock_Lock(&lock);
	l_gen++;
	l_n = n < ART_LISTS ? n : ART_LISTS;
	l_next = 0;
	for (int k = 0; k < ART_LISTS; k++) {
		free(l_path[k]);
		l_path[k] = k < l_n && paths[k] ? strdup(paths[k]) : NULL;
		l_state[k] = k < l_n && l_path[k] ? 0 : 2;
	}
	LightLock_Unlock(&lock);
	LightEvent_Signal(&wake);
}

int art_list_thumb(u32 *px) {
	int k = -1;
	LightLock_Lock(&lock);
	for (int j = 0; j < l_n && k < 0; j++)
		if (l_state[j] == 1) { k = j; l_state[j] = 2; memcpy(px, l_px[j], sizeof l_px[j]); }
	LightLock_Unlock(&lock);
	return k;
}

void art_playing(bool on) { playing = on; }
void art_want(int track) {
	LightLock_Lock(&lock);
	want = track;
	int k = track >= 0 && track < lib.nt ? bigc_find(lib.t[track].album_id) : -1;
	if (k >= 0 && track != done) {   // its picture is already made: handed over from here, no waiting for the worker
		memcpy(big_px, bigc[k].px, sizeof big_px);
		big_color = bigc[k].color;
		big_ok = bigc[k].ok;
		bigc[k].age = ++bigc_seq;
		done = track;
		fresh = true;
	}
	LightLock_Unlock(&lock);
	LightEvent_Signal(&wake);
}

void art_ahead(const int *tracks, int n) {
	LightLock_Lock(&lock);
	ahead_n = 0;
	for (int k = 0; k < n && ahead_n < 4; k++) ahead[ahead_n++] = tracks[k];
	LightLock_Unlock(&lock);
	LightEvent_Signal(&wake);
}

bool art_big(int *track, const u32 **px, u32 *color) {
	LightLock_Lock(&lock);
	bool ok = fresh && done == want;
	if (ok) { *track = done; *px = big_ok ? big_px : NULL; *color = big_color; fresh = false; }
	LightLock_Unlock(&lock);
	return ok;
}

int art_detail_want(int track, const char *path, const char *avatar) {
	// shrunk on an earlier visit: read right here, a few ms, so the page opens with its cover
	static u32 cpx[ART_DETAIL * ART_DETAIL], apx[ART_AVATAR * ART_AVATAR];
	char rp[220];
	u32 c = 0;
	bool has = track >= 0 || (path && path[0]), ok = false, aok = false;
	if (has) { raw_name(rp, sizeof rp, track, path ? path : "", 'd'); ok = raw_read(rp, cpx, ART_DETAIL * ART_DETAIL, &c); }
	if (avatar && avatar[0]) { raw_name(rp, sizeof rp, -1, avatar, 'a'); aok = raw_read(rp, apx, ART_AVATAR * ART_AVATAR, NULL); }
	bool all = ok && (!avatar || !avatar[0] || aok);
	LightLock_Lock(&lock);
	int r = ++d_req;
	d_track = track;
	snprintf(d_path, sizeof d_path, "%s", path ? path : "");
	snprintf(d_av, sizeof d_av, "%s", avatar ? avatar : "");
	d_fresh = false;
	if (all) {
		memcpy(d_px, cpx, sizeof cpx);
		if (aok) memcpy(d_avpx, apx, sizeof apx);
		d_ok = true;
		d_avok = aok;
		d_color = c;
		d_done = r;
		d_fresh = true;
	}
	LightLock_Unlock(&lock);
	if (!all) LightEvent_Signal(&wake);
	return r;
}

bool art_detail(int *req, const u32 **px, u32 *color, const u32 **avatar) {
	LightLock_Lock(&lock);
	bool ok = d_fresh && d_done == d_req;
	if (ok) {
		*req = d_done;
		*px = d_ok ? d_px : NULL;
		*avatar = d_avok ? d_avpx : NULL;
		*color = d_color;
		d_fresh = false;
	}
	LightLock_Unlock(&lock);
	return ok;
}

int art_thumb(const u32 **px) {
	int a = -1;
	LightLock_Lock(&lock);
	if (thumb_n) {
		a = thumb_q[--thumb_n];
		*px = thumbs + a * ART_THUMB * ART_THUMB;
	}
	LightLock_Unlock(&lock);
	if (a >= 0) LightEvent_Signal(&wake);   // room in the queue again
	return a;
}

bool art_home_want(int slot, int track, const char *path, const char *artist, u32 *px) {
	if (slot < 0 || slot >= ART_SLOTS || !h_job) return false;
	if (!path) path = "";
	if (!artist) artist = "";
	bool has = artist[0] || (track >= 0 && track < lib.nt) || path[0], ok = false;
	if (has && px) {   // shrunk on an earlier visit: a 30 KB read, so it comes up with the page
		char rp[220];
		home_raw(rp, sizeof rp, track, path, artist);
		ok = raw_read(rp, px, H2, NULL);
	}
	LightLock_Lock(&lock);
	h_job[slot].gen++;
	h_job[slot].track = track;
	h_job[slot].seq = ++h_seq;
	snprintf(h_job[slot].path, sizeof h_job[slot].path, "%s", path);
	snprintf(h_job[slot].artist, sizeof h_job[slot].artist, "%s", artist);
	h_job[slot].state = has && !ok ? 1 : 0;
	for (int k = 0; k < h_nout; k++) if (h_out[k].slot == slot) h_out[k].slot = -1;   // an old picture for it, not taken yet
	LightLock_Unlock(&lock);
	if (has && !ok) LightEvent_Signal(&wake);
	return ok;
}

int art_home_tile(u32 *px) {
	int s = -1;
	LightLock_Lock(&lock);
	while (h_nout && s < 0) {
		h_nout--;
		if (h_out[h_nout].slot >= 0 && h_job[h_out[h_nout].slot].gen == h_out[h_nout].gen) { s = h_out[h_nout].slot; memcpy(px, h_out[h_nout].px, H2 * 4); }
	}
	LightLock_Unlock(&lock);
	if (s >= 0) LightEvent_Signal(&wake);   // room for the next
	return s;
}
