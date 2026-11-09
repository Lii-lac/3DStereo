// ---- Home ------------------------------------------------------------------------------------
// Included by main.c after artistpage.inc and detailpage.inc. The page Pocket opens on, the way
// Spotify's does: what you've played lately, the newest eight as a grid of small tiles up top (a
// cover and a name, two to a row), then all of it as a shelf of big covers, Recents. An album, a
// playlist or an artist goes on it when you start playing from it; a song started from the Songs
// tab, a search or the charts puts its album there; Spotify Connect, its playlist or the song's
// album, once for each thing it plays from. Nothing played yet: the most played albums stand in.

#define RECENTS "sdmc:/3ds/player/recents"
#define H_GRID 8
#define H_MAX 24

typedef struct { char kind, *key; } Recent;   // 'A' an album ("name\tartist"), 'P' a playlist, 'R' an artist (their names)
static Recent rec[H_MAX];
static int nrec;

typedef struct { char kind; int id; } HItem;   // a recent that's on the card, and its group
static HItem hi[H_MAX];
static int nhi;

static u32 home_col = 0x404040;         // the newest one's colour, washed down from the top

static float h_sc, h_vel, h_maxsc, h_hs;   // the page's scroll, its momentum and end; the shelf, sideways
static int h_fk, h_fi;                     // the cursor: 0 the grid, 1 the shelf; and which
static bool h_touch, h_hmoved;             // a finger came down on the page; ...and is sliding the shelf
static float h_s0, h_hs0;

static float hr_x, hr_y, hr_w, hr_h, hr_r;   // the cursor's ring, gliding (x on screen, y on the page)
static bool hr_ok;
static bool home_active(void) { return tab == T_HOME && detail < 0 && !ap_active(); }

// ---- what's been played ----------------------------------------------------------------------

static void album_key_of(int a, char *k, int cap) { snprintf(k, cap, "%s\t%s", lib.albums[a].name, lib.albums[a].artist ? lib.albums[a].artist : ""); }

static void rec_save(void) {
	size_t cap = 32;
	for (int k = 0; k < nrec; k++) cap += strlen(rec[k].key) + 4;
	char *t = malloc(cap);
	if (!t) return;
	size_t n = sprintf(t, "pocket-recents 1\n");
	for (int k = 0; k < nrec; k++) n += sprintf(t + n, "%c %s\n", rec[k].kind, rec[k].key);
	bg_save(RECENTS, "sdmc:/3ds/player", t);   // the quiet thread's: a song's start never waits on the card
}

static void rec_push(char kind, const char *key) {   // to the front, once
	if (!key || !key[0]) return;
	if (nrec && rec[0].kind == kind && !strcmp(rec[0].key, key)) return;
	int at = -1;
	for (int k = 0; k < nrec && at < 0; k++) if (rec[k].kind == kind && !strcmp(rec[k].key, key)) at = k;
	char *s;
	if (at >= 0) { s = rec[at].key; memmove(rec + 1, rec, at * sizeof *rec); }
	else {
		if (!(s = strdup(key))) return;
		if (nrec == H_MAX) free(rec[--nrec].key);
		memmove(rec + 1, rec, nrec * sizeof *rec);
		nrec++;
	}
	rec[0].kind = kind;
	rec[0].key = s;
	rec_save();
	if (tab == T_HOME) dirty = true;
}

static void rec_album(int a) { if (a >= 0 && a < lib.nal) { char k[600]; album_key_of(a, k, sizeof k); rec_push('A', k); } }

// The 3DS started a queue (set_queue): what it's from, as src_kind/src_id/src_name say
static void home_note(int track) {
	if (track < 0 || track >= lib.nt) return;
	if (src_kind == T_PLAYLISTS && src_id >= 0 && src_id < lib.nl) rec_push('P', lib.lists[src_id].name);
	else if (src_kind == K_APQ) rec_push('R', src_name);
	else if (src_kind == K_AP) { const char *c = strchr(src_name, ':'); rec_push('R', c ? c + 1 : src_name); }   // "card:Name", "liked:Name"
	else rec_album(src_kind == T_ALBUMS && src_id >= 0 && src_id < lib.nal ? src_id : lib.t[track].album_id);
}

// Spotify Connect loaded a song: once per context it plays from, not every song of it
static void home_note_connect(int track, const char *ctx) {
	static char last[96] = "\x01";
	if (track < 0 || track >= lib.nt || !strcmp(last, ctx)) return;
	snprintf(last, sizeof last, "%s", ctx);
	if (!strncmp(ctx, "spotify:playlist:", 17))
		for (int k = 1; k < lib.nl; k++)
			if (lib.lists[k].pid && !strcmp(lib.lists[k].pid, ctx + 17)) { rec_push('P', lib.lists[k].name); return; }
	if (strstr(ctx, "collection")) { if (lib.nl) rec_push('P', lib.lists[0].name); return; }   // their Liked Songs
	if (strstr(ctx, ":artist:")) { int a = lib.t[track].artist_id; if (a >= 0 && a < lib.nar) rec_push('R', lib.artists[a].name); return; }
	rec_album(lib.t[track].album_id);
}

static void rec_seed(void) {   // nothing played since Home came: the most played albums, from the charts
	u32 *p = calloc(lib.nal + 1, sizeof *p);
	if (!p) return;
	for (int t = 0; t < lib.nt; t++) { int a = lib.t[t].album_id; if (a >= 0 && a < lib.nal) p[a] += stats_plays(t); }
	while (nrec < H_GRID) {
		int best = -1;
		for (int a = 0; a < lib.nal; a++) if (p[a] && (best < 0 || p[a] > p[best])) best = a;
		if (best < 0) break;
		p[best] = 0;
		char k[600];
		album_key_of(best, k, sizeof k);
		if ((rec[nrec].key = strdup(k))) rec[nrec++].kind = 'A';
	}
	free(p);
}

static void rec_load(void) {
	FILE *f = fopen(RECENTS, "r");
	char l[600];
	if (f && fgets(l, sizeof l, f) && !strcmp(l, "pocket-recents 1\n"))
		while (nrec < H_MAX && fgets(l, sizeof l, f)) {
			l[strcspn(l, "\r\n")] = 0;
			if ((l[0] == 'A' || l[0] == 'P' || l[0] == 'R') && l[1] == ' ' && l[2] && (rec[nrec].key = strdup(l + 2))) rec[nrec++].kind = l[0];
		}
	if (f) fclose(f);
	if (!nrec) rec_seed();
}

// ---- the page's items and their covers -------------------------------------------------------

static void home_init(void) { rec_load(); }

static u32 h_color(const HItem *h) {
	if (h->kind == 'A') return lib.albums[h->id].color;
	if (h->kind == 'R') return lib.artists[h->id].color;
	Group *g = &lib.lists[h->id];
	return h->id == 0 ? 0x5038a0 : g->n ? lib.albums[lib.t[g->songs[0]].album_id].color : 0x404040;
}

// rebuild(), on Home: the recents that are on the card
static void home_build(void) {
	nhi = 0;
	for (int k = 0; k < nrec; k++) {
		HItem h = {rec[k].kind, -1};
		const char *key = rec[k].key;
		if (h.kind == 'A') {
			const char *tb = strchr(key, '\t');
			int nl = tb ? tb - key : (int)strlen(key);
			const char *ar = tb ? tb + 1 : "";
			for (int a = 0; a < lib.nal && h.id < 0; a++)
				if (!strncmp(lib.albums[a].name, key, nl) && !lib.albums[a].name[nl] && !strcmp(lib.albums[a].artist ? lib.albums[a].artist : "", ar)) h.id = a;
		} else if (h.kind == 'P') {
			for (int a = 0; a < lib.nl && h.id < 0; a++) if (!strcmp(lib.lists[a].name, key)) h.id = a;
		} else for (int a = 0; a < lib.nar && h.id < 0; a++) if (!strcmp(lib.artists[a].name, key)) h.id = a;
		if (h.id >= 0) hi[nhi++] = h;
	}
	if (nhi) home_col = h_color(&hi[0]);
	if (h_fk == 0 && h_fi >= (nhi < H_GRID ? nhi : H_GRID)) h_fi = 0;
	if (h_fk == 1 && h_fi >= nhi) h_fi = nhi ? nhi - 1 : 0;
}

// ---- drawing ---------------------------------------------------------------------------------

static const char *h_name(const HItem *h) { return h->kind == 'A' ? lib.albums[h->id].name : h->kind == 'P' ? lib.lists[h->id].name : lib.artists[h->id].name; }
static void h_sub(const HItem *h, char *s, int n) {
	if (h->kind == 'A') snprintf(s, n, "Album \xc2\xb7 %s", lib.albums[h->id].artist ? lib.albums[h->id].artist : "");
	else if (h->kind == 'R') snprintf(s, n, "Artist");
	else if (owner_of(&lib.lists[h->id])) snprintf(s, n, "Playlist \xc2\xb7 %s", owner_of(&lib.lists[h->id]));
	else snprintf(s, n, "Playlist \xc2\xb7 %d song%s", lib.lists[h->id].n, lib.lists[h->id].n == 1 ? "" : "s");
}
static bool h_now(const HItem *h, const AudioInfo *ai) {   // what's playing comes from it
	if (!ai->open || ai->track < 0) return false;
	if (h->kind == 'A') return src_kind == T_ALBUMS ? src_id == h->id : src_kind < 0 && lib.t[ai->track].album_id == h->id;
	if (h->kind == 'P') return src_kind == T_PLAYLISTS && src_id == h->id;
	const char *c = src_kind == K_AP ? strchr(src_name, ':') : NULL, *n = lib.artists[h->id].name;
	return (src_kind == K_APQ && !strcmp(src_name, n)) || (c && !strcmp(c + 1, n));
}

static void h_cover(const HItem *h, float x, float y, float s, bool big) {
	if (h->kind == 'A') { if (big) big_album(h->id, x, y, s); else thumb(h->id, x, y, s); }
	else if (h->kind == 'R') { if (big) big_artist(h->id, x, y, s); else if (h->id < MI_FACES && face_ok[h->id]) face(h->id, x, y, s); else big_artist(h->id, x, y, s); }
	else if (big || h->id == 0) big_list(h->id, x, y, s);
	else if (!list_tile(h->id, x, y, s)) { Group *g = &lib.lists[h->id]; if (g->n) thumb(lib.t[g->songs[0]].album_id, x, y, s); else rrect(x, y, s, s, 3, rgb(0x333333)); }
}

static void h_bars(float x, float y, bool moving) {   // Spotify's green "playing from this", three bars
	for (int b = 0; b < 3; b++) {
		float h = moving ? 3 + (b * 7 + osGetTime() / 90) % 8 : 3 + b % 2 * 3;
		rect(x + b * 4, y - h, 2.5f, h, rgb(GREEN));
	}
}

// what the cursor can be on, as last drawn on the list's own screen (y on the page)
static struct { u8 kind; s16 k, line; float x, y, w, h; } hf[H_GRID + H_MAX];
static int nhf;
static bool h_rec;
static void h_reg(int kind, int k, int line, float x, float y, float w, float h) {
	if (h_rec && nhf < (int)(sizeof hf / sizeof *hf)) { hf[nhf].kind = kind; hf[nhf].k = k; hf[nhf].line = line; hf[nhf].x = x; hf[nhf].y = y + h_sc; hf[nhf].w = w; hf[nhf].h = h; nhf++; }
}
static u32 home_head_col(void) { return mixc(home_col, BG, 0.34f); }

static void home_draw(float W, float H, const AudioInfo *ai) {
	h_rec = !ghost && W == list_w();
	if (h_rec) nhf = 0;
	u32 c0 = home_head_col();
	u32 ct = intro.on ? rgba(c0, (u8)(255 * intro.wash)) : rgb(c0);
	C2D_DrawRectangle(0, LIST0, 0, W, 150, ct, ct, rgb(BG), rgb(BG));   // held still; the page slides over it
	C3D_Mtx rest;
	C2D_ViewSave(&rest);
	if (intro.on) {   // the way in: the page isn't here yet, then it rises
		if (!intro.content) return;
		C2D_ViewTranslate(0, intro.rise);
	}
	if (!nhi) {
		text("Nothing played yet", W / 2, 96, 12, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
		text("Albums, playlists and artists you play show up here.", W / 2, 112, 9, rgb(GREY), C2D_AlignCenter, 0);
		if (h_rec) h_maxsc = 0;
		C2D_ViewRestore(&rest);
		return;
	}
	float y = LIST0 + 8 - h_sc, gap = 8, tw = (W - 24 - gap) / 2, th = 32;
	int ng = nhi < H_GRID ? nhi : H_GRID;
	for (int k = 0; k < ng; k++) {   // the grid: the newest eight
		float x = 12 + (k & 1) * (tw + gap), ty = y + (k >> 1) * (th + 6);
		h_reg(0, k, k >> 1, x, ty, tw, th);
		if (ty > H || ty + th < LIST0) continue;
		HItem *h = &hi[k];
		bool now = h_now(h, ai);
		rrect(x, ty, tw, th, 4, rgba(0xffffff, 24));
		h_cover(h, x, ty, th, false);
		text(h_name(h), x + th + 8, ty + 20, 10, rgb(now ? GREEN : 0xffffff), BOLD, tw - th - (now ? 34 : 16));
		if (now) h_bars(x + tw - 20, ty + 22, !ai->paused);
	}
	y += (ng + 1) / 2 * (th + 6) + 4;
	y = heading("Recents", y);
	float cw = W > 320 ? 80 : 72, cg = W > 320 ? 14 : 12;
	int line = (ng + 1) / 2;
	for (int k = 0; k < nhi; k++) {   // the shelf: all of it, big
		float cx = 12 + k * (cw + cg);
		h_reg(1, k, line, cx, y, cw, cw + 32);
		float x = cx - h_hs;
		if (x > W || x + cw < 0 || y > H || y + cw + 32 < LIST0) continue;
		HItem *h = &hi[k];
		bool now = h_now(h, ai);
		h_cover(h, x, y, cw, true);
		float nw = cw;
		if (now) { h_bars(x, y + cw + 13, !ai->paused); nw -= 15; }
		text(h_name(h), x + (now ? 15 : 0), y + cw + 13, 10, rgb(now ? GREEN : 0xffffff), BOLD, nw);
		char s[200];
		h_sub(h, s, sizeof s);
		text(s, x, y + cw + 26, 9, rgb(GREY), 0, cw);
	}
	if (h_rec && hr_ok && cursor_vis) ring(hr_x, hr_y - h_sc, hr_w, hr_h, hr_r);   // the cursor, gliding
	float total = 12 + nhi * (cw + cg) - cg + 12 - W;
	if (h_rec && h_hs > (total > 0 ? total : 0)) h_hs = total > 0 ? total : 0;
	y += cw + 32 + 12;
	if (h_rec) { h_maxsc = y + h_sc - H; if (h_maxsc < 0) h_maxsc = 0; }
	C2D_ViewRestore(&rest);
}

// the header's way here: a house, filled in while you're on it
static float home_chip(float x, float o) {
	float k = clamp01(o);
	u32 bc = mixc(GREEN, CHIP, k);
	rrect(x, 5, 26, 22, 11, rgb(bc));
	u32 c = rgb(mixc(0, 0xffffff, k)), b = rgb(bc);
	float cx = x + 13;
	C2D_DrawTriangle(cx, 9.5f, c, cx - 6.5f, 15.5f, c, cx + 6.5f, 15.5f, c, 0);
	rect(cx - 4.5f, 15, 9, 7.5f, c);
	rect(cx - 1.4f, 18, 2.8f, 4.5f, b);
	hit(x - 3, x + 29, H_HOME);
	return x + 32;
}

// ---- the D-pad, a finger ---------------------------------------------------------------------

static int h_find(int kind, int k) { for (int i = 0; i < nhf; i++) if (hf[i].kind == kind && hf[i].k == k) return i; return -1; }
static float hf_x(int i) { return hf[i].x - (hf[i].kind == 1 ? h_hs : 0); }

// the cursor's ring, gliding from tile to tile: home_hl_step
static void home_hl_step(void) {
	int i = h_find(h_fk, h_fi);
	if (!cursor_vis || !home_active() || i < 0) { hr_ok = false; return; }
	bool shelf = hf[i].kind == 1;
	float x = hf_x(i), y = hf[i].y, w = hf[i].w, h = shelf ? hf[i].w : hf[i].h;
	float r = shelf && hf[i].k < nhi && hi[hf[i].k].kind == 'R' ? w / 2 : 4;
	if (!hr_ok) { hr_x = x; hr_y = y; hr_w = w; hr_h = h; hr_r = r; hr_ok = true; return; }
	hr_x += (x - hr_x) * 0.35f;
	hr_y += (y - hr_y) * 0.35f;
	hr_w += (w - hr_w) * 0.35f;
	hr_h += (h - hr_h) * 0.35f;
	hr_r += (r - hr_r) * 0.35f;
}

static void h_show(int i) {   // scroll so it's in view, and the shelf sideways
	float top = LIST0 + 4, H = LIST1, sc = ease_cur(&h_sc);
	if (hf[i].line == 0) sc = 0;
	else if (hf[i].y - sc < top) sc = hf[i].y - top;
	else if (hf[i].y + hf[i].h - sc > H - 4) sc = hf[i].y + hf[i].h - (H - 4);
	ease_to(&h_sc, sc < 0 ? 0 : sc > h_maxsc ? h_maxsc : sc);
	if (hf[i].kind == 1) {
		float W = list_w(), hs = ease_cur(&h_hs), x = hf[i].x - hs;
		if (x < 12) hs = hf[i].x - 12;
		else if (x + hf[i].w > W - 12) hs = hf[i].x + hf[i].w - (W - 12);
		ease_to(&h_hs, hs < 0 ? 0 : hs);
	}
}

static void h_open(int k) {
	if (k < 0 || k >= nhi) return;
	HItem *h = &hi[k];
	vel = h_vel = 0;
	if (h->kind == 'A') open_detail(T_ALBUMS, h->id);
	else if (h->kind == 'P') open_detail(T_PLAYLISTS, h->id);
	else ap_open_local(h->id);
}

static void home_keys(u32 kd, u32 kr) {
	if (!nhf) return;
	int fi = h_find(h_fk, h_fi);
	if (!cursor_vis || fi < 0) {   // the first press shows the cursor on the first thing in view
		if (!(kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT) || kd & KEY_A)) return;
		cursor_vis = true;
		fi = nhf - 1;
		for (int i = 0; i < nhf; i++) if (hf[i].y - h_sc >= LIST0) { fi = i; break; }
		h_fk = hf[fi].kind;
		h_fi = hf[fi].k;
		h_show(fi);
		return;
	}
	int to = -1;
	if (kr & (KEY_DUP | KEY_DDOWN)) {   // the nearest thing on the next line that way
		int dir = kr & KEY_DUP ? -1 : 1, want = -1;
		for (int i = 0; i < nhf; i++)
			if ((hf[i].line - hf[fi].line) * dir > 0 && (want < 0 || abs(hf[i].line - hf[fi].line) < abs(want - hf[fi].line))) want = hf[i].line;
		if (want < 0)   // off the top or the bottom: round to the line at the other end
			for (int i = 0; i < nhf; i++) if (want < 0 || (dir < 0 ? hf[i].line > want : hf[i].line < want)) want = hf[i].line;
		float best = 1e9, cx = hf_x(fi) + hf[fi].w / 2;
		for (int i = 0; i < nhf; i++)
			if (hf[i].line == want && fabsf(hf_x(i) + hf[i].w / 2 - cx) < best) { best = fabsf(hf_x(i) + hf[i].w / 2 - cx); to = i; }
	} else if (kr & (KEY_DLEFT | KEY_DRIGHT)) {
		int dir = kr & KEY_DLEFT ? -1 : 1;
		float best = 1e9;
		for (int i = 0; i < nhf; i++)
			if (i != fi && hf[i].line == hf[fi].line && (hf[i].x - hf[fi].x) * dir > 0 && fabsf(hf[i].x - hf[fi].x) < best) { best = fabsf(hf[i].x - hf[fi].x); to = i; }
		if (to < 0)   // off the end of the line: round to its other end
			for (int i = 0; i < nhf; i++)
				if (i != fi && hf[i].line == hf[fi].line && (to < 0 || (dir > 0 ? hf[i].x < hf[to].x : hf[i].x > hf[to].x))) to = i;
	}
	if (to >= 0) { h_fk = hf[to].kind; h_fi = hf[to].k; h_show(to); }
	else if (kd & KEY_A) h_open(hf[fi].k);
}

static void home_tap(int x, int y) {
	for (int i = nhf - 1; i >= 0; i--) {
		float fx = hf_x(i), fy = hf[i].y - h_sc;
		if (x >= fx && x < fx + hf[i].w && y >= fy && y < fy + hf[i].h) { h_fk = hf[i].kind; h_fi = hf[i].k; h_open(hf[i].k); return; }
	}
}

static bool home_shelf_at(int y) {   // a finger came down on the shelf
	for (int i = 0; i < nhf; i++) if (hf[i].kind == 1) { float fy = hf[i].y - h_sc; return y >= fy && y < fy + hf[i].h; }
	return false;
}
