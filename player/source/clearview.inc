// The Clear cache page (Settings > Clear cache): what 3DStereo keeps on the card so things come up faster, as a short
// list of boxes to tick, like clearing a browser's cache. Each row counts its own size as the page opens (a slice of every
// frame, so the page never waits on the card); the top screen adds up what's ticked, as a number and a bar made of the
// rows' colours. Clear closes 3DStereo the usual way first (every worker joined, nothing left to write the files back),
// removes the ticked ones with that number counting down, and starts 3DStereo again, so nothing stale stays in memory.
// It rides the Pair page's transitions (pairview.inc): the same dip in and out, the same slot in main's loop.
#include <dirent.h>

#define CC_N 6
#define CC_ROW 30
#define CC_Y0 8
#define PD "sdmc:/3ds/player"
typedef struct { const char *name, *sub; const char *dir[3]; const char *file[2]; u32 col; bool def, rec, shards; } CcCat;
// dir: every plain file in it goes (not its folders: those are listed on their own). file: single files.
// rec: the folders are cache all the way down, so clearing is one recursive delete each (a file at a time is slow on FAT).
// shards: dir[0] also holds 16 folders named 0..f (covers/, build 100), counted along with it. (Builds 96 to 99 made 256 two-digit
// ones: the count doesn't walk those, they only go with the recursive delete.)
static const CcCat CC[CC_N] = {
	{"Album covers", "Redrawn as you browse", {PD "/covers"}, {PD "/thumbs.cache", PD "/thumbs.cache.tmp"}, 0x1ed760, true, true, true},
	{"Artists", "Photos and info", {PD "/artists/img", PD "/artists/faces", PD "/artists"}, {PD "/faces.cache", PD "/faces.cache.tmp"}, 0x8ab4f8, true, true, false},
	{"Lyrics", "Fetched on open", {PD "/lyrics"}, {0}, 0xe8c55a, true, true, false},
	{"Playlist pictures", "Back on sync", {PD "/lists"}, {0}, 0xd96b82, true, false, false},
	{"Spotify matches", "Found as songs play", {0}, {PD "/connect-ids"}, 0x7be3a6, false, false, false},
	{"Library index", "Tags reread at launch", {0}, {PD "/library.cache", PD "/library.cache.tmp"}, 0xb3b3b3, false, false, false},
};

typedef struct { u64 bytes; int files; bool done; } CcSize;
static CcSize cc_sz[CC_N];          // what each row holds, counted
static u64 cc_freed[CC_N];          // what each has given back so far (clearing)
static bool cc_gone[CC_N];
static bool cc_sel[CC_N];
static float cc_tw[CC_N];           // each box's fill, easing to its state
static int cc_cur;                  // the row the D-pad is on; CC_N is the Clear button
static bool cc_keys;                // the D-pad moved last (a touch hides the cursor)
static int cc_phase;                // 0 choosing, 1 clearing, 2 cleared
static int cc_scan_k;               // the row being counted
static int cc_walk_k;               // the row being cleared
static u64 cc_want0;                // what was ticked when Clear was pressed
static bool cc_go;                  // main's loop: stop, shut down, then cc_run
static u64 cc_t_done;               // when the clearing finished
static struct { DIR *d; int src, pass, hit; } cw;   // where the count or the clearing is within a row

static Handle cw_h;                 // the folder being counted, read straight from the card's file system
static FS_Archive cw_a;             // the card, opened once for a whole row (not once per shard)
static bool cw_ho, cw_ao;           // cw_h is open; cw_a is open
static void cc_walk_reset(void) {
	if (cw.d) closedir(cw.d);
	if (cw_ho) { FSDIR_Close(cw_h); cw_ho = false; }
	if (cw_ao) { FSUSER_CloseArchive(cw_a); cw_ao = false; }
	memset(&cw, 0, sizeof cw);
}
static bool cc_keep(const char *n) { return !strcmp(n, "me.id") || !strcmp(n, "me.name"); }   // who "my" playlists are: not a cache

// Row k: add up what's in it (del false), or remove it (del true), until `until` (osGetTime). bytes and files are what was
// added up or what really went. True once the row is through; false if the time ran out (call again).
static bool cc_walk(int k, bool del, u64 *bytes, int *files, u64 until) {
	const CcCat *c = &CC[k];
	int nd = 0, nf = 0;
	while (nd < 3 && c->dir[nd]) nd++;
	while (nf < 2 && c->file[nf]) nf++;
	int nsh = c->shards ? 16 : 0;   // sources in order: the folders, then their shards, then the single files
	char p[400], dp[96];
	struct stat st;
	while (cw.src < nd + nsh + nf) {
		if (del && c->rec && cw.src < nd) {   // the whole folder tree, one call (the shards go with it); not-there is fine
			FS_Archive a;
			if (R_SUCCEEDED(FSUSER_OpenArchive(&a, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
				FSUSER_DeleteDirectoryRecursively(a, fsMakePath(PATH_ASCII, c->dir[cw.src] + 5));
				FSUSER_CloseArchive(a);
			}
			if (!cw.src) { *bytes += cc_sz[k].bytes; *files += cc_sz[k].files; }   // counted at the page open: the number to count down by
			cw.src++;
			continue;
		}
		if (del && c->rec && cw.src < nd + nsh) { cw.src = nd + nsh; continue; }   // inside what was just deleted
		if (cw.src >= nd + nsh) {
			const char *f = c->file[cw.src - nd - nsh];
			if (!stat(f, &st) && S_ISREG(st.st_mode) && (!del || !remove(f))) { *bytes += st.st_size; ++*files; }
			cw.src++;
			continue;
		}
		const char *dpath = c->dir[cw.src < nd ? cw.src : 0];
		if (cw.src >= nd) { snprintf(dp, sizeof dp, "%s/%x", c->dir[0], cw.src - nd); dpath = dp; }   // a shard
		if (!del) {   // counting: one request returns a batch of entries WITH their sizes, so no stat() per file (on FAT each
		              // stat searches the folder from its start, which made thousands of covers crawl)
			if (!cw_ho) {
				if (!cw_ao) {
					if (R_FAILED(FSUSER_OpenArchive(&cw_a, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) { cw.src++; continue; }
					cw_ao = true;
				}
				if (R_FAILED(FSUSER_OpenDirectory(&cw_h, cw_a, fsMakePath(PATH_ASCII, dpath + 5)))) {   // "sdmc:" off; not there is fine
					cw.src++;
					continue;
				}
				cw_ho = true;
			}
			static FS_DirectoryEntry fe[64];
			u32 got = 0;
			while (R_SUCCEEDED(FSDIR_Read(cw_h, &got, 64, fe)) && got) {
				for (u32 i = 0; i < got; i++) {
					if (fe[i].attributes & FS_ATTRIBUTE_DIRECTORY) continue;
					char nm[16];
					int n = 0;
					while (n < 15 && fe[i].name[n] && fe[i].name[n] < 128) { nm[n] = (char)fe[i].name[n]; n++; }
					nm[n] = 0;
					if (cc_keep(nm)) continue;
					*bytes += fe[i].fileSize;
					++*files;
				}
				if (osGetTime() >= until) return false;
			}
			FSDIR_Close(cw_h);
			cw_ho = false;
			cw.src++;
			continue;
		}
		if (!cw.d) {
			cw.d = opendir(dpath);
			cw.hit = 0;
			if (!cw.d) { cw.src++; cw.pass = 0; continue; }
		}
		struct dirent *e;
		while ((e = readdir(cw.d))) {
			snprintf(p, sizeof p, "%s/%s", dpath, e->d_name);
			if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;   // also "." and ".."
			if (cc_keep(e->d_name)) continue;
			if (del) {
				if (remove(p)) continue;
				cw.hit++;
			}
			*bytes += st.st_size;
			++*files;
			if (osGetTime() >= until) return false;
		}
		closedir(cw.d);
		cw.d = NULL;
		if (del && cw.hit && ++cw.pass < 3) continue;   // go through it again: removing while reading can skip a few
		cw.src++;
		cw.pass = 0;
	}
	if (cw_ao) { FSUSER_CloseArchive(cw_a); cw_ao = false; }
	cw.src = 0;
	return true;
}

static void cc_fmt(u64 b, char *num, size_t nn, const char **unit) {
	if (b < 1024) { snprintf(num, nn, "%u", (unsigned)b); *unit = "B"; }
	else if (b < 1024 * 1024) { snprintf(num, nn, "%u", (unsigned)((b + 512) / 1024)); *unit = "KB"; }
	else { snprintf(num, nn, "%.1f", b / 1048576.0); *unit = "MB"; }
}
static void cc_size_str(u64 b, char *out, size_t n) { char d[16]; const char *u; cc_fmt(b, d, sizeof d, &u); snprintf(out, n, "%s %s", d, u); }

static u64 cc_ticked(bool left) {   // what the ticked rows hold (left: what's still there to clear)
	u64 s = 0;
	for (int k = 0; k < CC_N; k++)
		if (cc_sel[k]) s += left ? (cc_sz[k].bytes > cc_freed[k] ? cc_sz[k].bytes - cc_freed[k] : 0) : cc_sz[k].bytes;
	return s;
}
static bool cc_any(void) { for (int k = 0; k < CC_N; k++) if (cc_sel[k]) return true; return false; }
static bool cc_ready(void) { for (int k = 0; k < CC_N; k++) if (cc_sel[k] && !cc_sz[k].done) return false; return true; }

static int cc_row_k = -1;           // the row whose count is being timed, and when it began (home.log gets how long each took)
static u64 cc_row_t0;
static void cc_scan_step(void) {   // most of each frame's time on counting (the page has nothing else to do: 10 ms of the 16)
	u64 until = osGetTime() + 10;
	while (cc_scan_k < CC_N && osGetTime() < until) {
		CcSize *z = &cc_sz[cc_scan_k];
		if (cc_row_k != cc_scan_k) { cc_row_k = cc_scan_k; cc_row_t0 = osGetTime(); }
		if (!cc_walk(cc_scan_k, false, &z->bytes, &z->files, until)) break;
		z->done = true;
		crumb("clear cache: %s counted, %d files, %u KB, %u ms", CC[cc_scan_k].name, z->files, (unsigned)(z->bytes >> 10), (unsigned)(osGetTime() - cc_row_t0));
		cc_scan_k++;
	}
}

static void cc_open(void) {
	cc_walk_reset();
	memset(cc_sz, 0, sizeof cc_sz);
	memset(cc_freed, 0, sizeof cc_freed);
	memset(cc_gone, 0, sizeof cc_gone);
	for (int k = 0; k < CC_N; k++) cc_tw[k] = cc_sel[k] = CC[k].def;
	cc_scan_k = cc_walk_k = cc_phase = 0;
	cc_row_k = -1;
	cc_cur = 0;
	cc_keys = false;
	cc_go = false;
	cc_page = true;
	pair_on = true;
	pt_dir = 1;
	pt_t0 = osGetTime();
	p_force = -1;
}
static void cc_close(void) {
	ConnectPair z;
	memset(&z, 0, sizeof z);
	cc_walk_reset();
	pair_close(0, &z);
}
static void cc_toggle(int k) { cc_sel[k] = !cc_sel[k]; audio_tick(TICK_LAND); }
static void cc_start(void) {
	if (!cc_any()) { say("Tick something"); return; }
	if (!cc_ready()) { say("Still counting"); return; }
	cc_walk_reset();
	cc_want0 = cc_ticked(false);
	cc_phase = 1;
	cc_go = true;   // main's loop ends; 3DStereo shuts down the usual way, and cc_run does the rest
}

static void cc_step(u32 kd) {   // every frame while the page is up
	if (cc_phase) return;
	cc_scan_step();
	bool back = kd & KEY_B, go = false;
	if (kd & (KEY_DUP | KEY_DDOWN)) {
		cc_keys = true;
		cc_cur = (cc_cur + (kd & KEY_DUP ? CC_N : 1)) % (CC_N + 1);
		audio_tick(TICK_GRAB);
	}
	if (kd & KEY_A) { cc_keys = true; if (cc_cur < CC_N) cc_toggle(cc_cur); else go = true; }
	if (kd & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		cc_keys = false;
		if (tp.py >= 196) { if (tp.px < 112) back = true; else if (tp.px >= 118) go = true; }
		else if (tp.py >= CC_Y0 && tp.py < CC_Y0 + CC_ROW * CC_N) { cc_cur = (tp.py - CC_Y0) / CC_ROW; cc_toggle(cc_cur); }
	}
	if (back) { cc_close(); return; }
	if (go) cc_start();
}

// ---- drawing -------------------------------------------------------------------------------------

static void cc_check(float x, float y, float s, u32 c) {   // a tick in a box centred on (x, y)
	C2D_DrawLine(x - 4.6f * s, y + 0.4f * s, c, x - 1.4f * s, y + 3.8f * s, c, 2.2f, 0);
	C2D_DrawLine(x - 1.4f * s, y + 3.8f * s, c, x + 4.6f * s, y - 3.6f * s, c, 2.2f, 0);
}

static void draw_clear(void) {
	u64 now = osGetTime();
	float t = (float)now;
	for (int k = 0; k < CC_N; k++) cc_tw[k] += ((cc_sel[k] ? 1.0f : 0.0f) - cc_tw[k]) * 0.35f;
	bool counting = false;
	for (int k = 0; k < CC_N; k++) if (cc_sel[k] && !cc_sz[k].done) counting = true;
	u64 shown = cc_ticked(cc_phase >= 1), denom = cc_phase ? cc_want0 : cc_ticked(false);

	// ---------------- the top screen: what will go ----------------
	C2D_TargetClear(top, rgb(BG));
	C2D_SceneBegin(top);
	text("Clear cache", 200, 36, 12, rgba(0xffffff, 170), C2D_AlignCenter, 0);
	if (cc_phase == 2) {
		float a = p_ease((float)(now - cc_t_done) / 350.0f);
		char s[24];
		cc_size_str(cc_want0, s, sizeof s);
		text("Cleared", 200, floorf(112 + (1 - a) * 6 + 0.5f), 34, rgba(GREEN, (u8)(255 * a)), C2D_AlignCenter | BOLD | NAMEF, 0);
		text(s, 200, 138, 12, rgba(0xffffff, (u8)(170 * a)), C2D_AlignCenter, 0);
#ifdef CC_RELAUNCH
		text("3DStereo is opening again", 200, 190, 10, rgba(0xffffff, (u8)(110 * a)), C2D_AlignCenter, 0);
#else
		text("Open 3DStereo again", 200, 190, 10, rgba(0xffffff, (u8)(110 * a)), C2D_AlignCenter, 0);
#endif
	} else {
		char num[16];
		const char *unit;
		static float shown_f;   // the big number counts to its new value instead of jumping
		if (cc_phase || fabsf(shown_f - (float)shown) < 1024) shown_f = (float)shown;
		else shown_f += ((float)shown - shown_f) * 0.3f;
		cc_fmt((u64)shown_f, num, sizeof num, &unit);
		// Figtree (NAMEF) at 40 px, not the system font stretched to 44: its glyphs are ~22 px, so it went soft;
		// the position is a whole pixel too, a half pixel blurs a texture
		float wn = measure_name(num, 40), wu = measure_name(unit, 18), x0 = floorf(200 - (wn + 6 + wu) / 2), dy = p_dy;
		float by = floorf(118 + dy + 0.5f);
		text(num, x0, by, 40, rgb(0xffffff), BOLD | NAMEF, 0);
		text(unit, floorf(x0 + wn + 6), by, 18, rgb(GREY), BOLD | NAMEF, 0);
		rrect(40, 150 + dy, 320, 10, 5, rgba(0xffffff, 26));
		float dw = (float)denom;   // the bar's whole, each row counted by how far its tick has come (cc_tw): ticking grows a segment in, unticking shrinks it away
		if (!cc_phase) { dw = 0; for (int k = 0; k < CC_N; k++) dw += cc_tw[k] * (float)cc_sz[k].bytes; }
		if (dw > 0) {   // the bar: the ticked rows in their colours, shrinking as they go
			float x = 40;
			for (int k = 0; k < CC_N; k++) {
				if (cc_tw[k] < 0.01f) continue;
				u64 left = cc_sz[k].bytes > cc_freed[k] ? cc_sz[k].bytes - cc_freed[k] : 0;
				float w = 320.0f * cc_tw[k] * left / dw - 2;
				if (w < 1.5f) continue;
				rrect(x, 150 + dy, w, 10, w < 10 ? w / 2 : 5, rgb(CC[k].col));
				x += w + 2;
			}
		}
		if (cc_phase) text("Clearing", 200, 182, 10, rgba(0xffffff, 110), C2D_AlignCenter, 0);
		else if (!cc_any()) text("Tick what to clear", 200, 182, 10, rgba(0xffffff, 110), C2D_AlignCenter, 0);
		else if (counting) text("Counting", 200, 182, 10, rgba(0xffffff, (u8)(70 + 50 * sinf(t / 220.0f))), C2D_AlignCenter, 0);
		else text("will be cleared", 200, 182, 10, rgba(0xffffff, 110), C2D_AlignCenter, 0);
	}

	// ---------------- the touchscreen: the boxes ----------------
	C2D_TargetClear(bot, rgb(BG));
	C2D_SceneBegin(bot);
	for (int k = 0; k < CC_N; k++) {
		float y = CC_Y0 + k * CC_ROW + p_dy;
		bool on = cc_sel[k], dim = cc_phase && !on;
		if (cc_keys && !cc_phase && cc_cur == k) rrect(6, y + 1, 308, CC_ROW - 2, 6, rgba(0xffffff, 20));
		float cx = 25, cy = y + 15, s = cc_tw[k];
		rstroke(cx - 9, cy - 9, 18, 18, 4, rgba(0xffffff, dim ? 50 : 120));
		if (s > 0.03f) {
			rrect(cx - 9 * s, cy - 9 * s, 18 * s, 18 * s, 4 * s, rgb(CC[k].col));
			if (s > 0.55f) cc_check(cx, cy, s, rgb(0x121212));
		}
		text(CC[k].name, 44, y + 14, 11, rgba(0xffffff, dim ? 90 : 255), 0, 190);
		text(CC[k].sub, 44, y + 25, 8, rgba(GREY, dim ? 70 : 255), 0, 190);
		char sz[24];
		if (cc_phase && on && cc_gone[k]) text("Cleared", 308, y + 18, 10, rgb(GREEN), C2D_AlignRight, 0);
		else if (!cc_sz[k].done) text("...", 308, y + 18, 10, rgba(0xffffff, (u8)(70 + 50 * sinf(t / 220.0f + k))), C2D_AlignRight, 0);
		else {
			cc_size_str(cc_phase && on ? (cc_sz[k].bytes > cc_freed[k] ? cc_sz[k].bytes - cc_freed[k] : 0) : cc_sz[k].bytes, sz, sizeof sz);
			text(sz, 308, y + 18, 10, rgba(0xffffff, dim ? 70 : on ? 220 : 130), C2D_AlignRight, 0);
		}
	}
	if (!cc_phase) {
		bool can = cc_any() && cc_ready();
		char lab[32] = "Clear";
		if (cc_any() && !cc_ready()) snprintf(lab, sizeof lab, "Counting");
		else if (cc_any()) { char d[16]; const char *u; cc_fmt(cc_ticked(false), d, sizeof d, &u); snprintf(lab, sizeof lab, "Clear %s %s", d, u); }
		p_button(14, 204, 92, "Back", false);
		p_button(122, 204, 184, lab, can);
		if (cc_keys && cc_cur == CC_N) rstroke(120, 202, 188, 30, 15, rgba(0xffffff, 150));
	}
}

// Called by main after the shutdown, with the screens still up and no worker left: removes the ticked rows with the number
// counting down, holds "Cleared" a moment, and asks for 3DStereo to start again.
static void cc_run(void) {
	u64 hold = 0;
	cc_walk_reset();
	cc_walk_k = 0;
	for (;;) {
		bool closing = !aptMainLoop();   // the system asked us to close: finish quietly, no more frames
		u64 until = osGetTime() + 12;
		while (cc_walk_k < CC_N && osGetTime() < until) {
			if (!cc_sel[cc_walk_k]) { cc_walk_k++; continue; }
			u64 b = 0;
			int f = 0;
			bool fin = cc_walk(cc_walk_k, true, &b, &f, until);
			cc_freed[cc_walk_k] += b;
			if (!fin) break;
			cc_gone[cc_walk_k++] = true;
		}
		if (cc_walk_k >= CC_N && !hold) { hold = osGetTime(); cc_phase = 2; cc_t_done = hold; }
		if (!closing) {
			p_dy = 0;
			C2D_TextBufClear(tbuf);
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			draw_clear();
			C3D_FrameEnd(0);
		}
		if (hold && (closing || osGetTime() - hold > 1500)) break;
	}
#ifdef CC_RELAUNCH
	aptSetChainloaderToSelf();
#endif
}
