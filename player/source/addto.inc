// ---- Add to playlist ---------------------------------------------------------------------------
// Included by main.c after pagemenu.inc. Spotify's box for one song: Liked Songs, then your
// playlists (pinned ones first), each with a green check where the song is in it and an empty ring
// where it isn't. A or a tap flips one; Done saves every change (into the files, and up to Spotify
// with the next push), B or Cancel leaves it all as it was. X held opens it, and a tap on the green
// check beside a liked song's title.

#define AT_MAX 64
static bool at_on;
static int at_track, at_n, at_cur;   // at_cur: a row, or at_n Done, at_n + 1 Cancel
static int at_list[AT_MAX];
static bool at_in[AT_MAX], at_was[AT_MAX];
static float at_sc;

static bool at_has(int k, int t) {
	if (k == 0) return library_is_liked(t);
	Group *g = &lib.lists[k];
	for (int j = 0; j < g->n; j++) if (g->songs[j] == t) return true;
	return false;
}

static void at_open(int t) {
	if (t < 0 || t >= lib.nt || !lib.nl) return;
	at_n = 0;
	at_list[at_n++] = 0;
	bool *seen = calloc(lib.nl + 1, 1);
	for (int p = 0; p < npins && at_n < AT_MAX; p++)   // pinned first, as the Playlists tab has them
		for (int k = 1; k < lib.nl; k++)
			if (!seen[k] && le_editable(k) && !strcmp(lib.lists[k].name, pins[p])) { seen[k] = true; at_list[at_n++] = k; break; }
	for (int k = 1; k < lib.nl && at_n < AT_MAX; k++) if (!seen[k] && le_editable(k)) at_list[at_n++] = k;
	free(seen);
	for (int i = 0; i < at_n; i++) at_in[i] = at_was[i] = at_has(at_list[i], t);
	at_track = t;
	at_cur = 0;
	at_sc = 0;
	at_on = true;
	audio_tick(TICK_GRAB);
}

static void at_done(bool save) {
	at_on = false;
	if (!save) return;
	int changed = 0, last = -1;
	bool last_in = false;
	for (int i = 0; i < at_n; i++) {
		if (at_in[i] == at_was[i]) continue;
		int k = at_list[i];
		const char *e = NULL;
		if (k == 0) { library_like(at_track); like_t0 = at_in[i] ? osGetTime() : 0; }
		else if (at_in[i]) { e = library_list_add(k, at_track); if (!e && lib.lists[k].pid) spot_list_add(lib.lists[k].pid, at_track); }
		else { e = library_list_remove(k, at_track); if (!e && lib.lists[k].pid) spot_list_remove(lib.lists[k].pid, at_track); }
		if (e) { say(e); continue; }
		changed++;
		last = k;
		last_in = at_in[i];
	}
	if (!changed) return;
	spot_request(3000, false, false);   // the likes, and the lists', together
	dirty = true;
	char s[300];
	if (changed == 1) snprintf(s, sizeof s, "%s %s", last_in ? "Added to" : "Removed from", last == 0 ? "Liked Songs" : lib.lists[last].name);
	else snprintf(s, sizeof s, "Changes saved");
	say(s);
}

// where it all is, on the list's screen
typedef struct { float x, y, w, h, ry, rows_h, by; int vis; } AtLay;
#define AT_ROW 34
static AtLay at_lay(float W) {
	AtLay L;
	L.w = W > 320 ? 300 : 280;
	L.h = W > 320 ? 224 : 226;
	L.x = (W - L.w) / 2;
	L.y = W > 320 ? 8 : 7;
	L.ry = L.y + 34;
	L.by = L.y + L.h - 36;
	L.rows_h = L.by - 6 - L.ry;
	L.vis = (int)(L.rows_h / AT_ROW);
	return L;
}

static void at_check(float x, float y, bool on) {   // Spotify's: a green disc with a check, or a grey ring
	if (on) { ic_check(x, y, 7); return; }
	for (int a = 0; a < 32; a++) {
		float t0 = a * (float)M_PI / 16, t1 = (a + 1) * (float)M_PI / 16;
		C2D_DrawLine(x + 6.5f * cosf(t0), y + 6.5f * sinf(t0), rgb(0x9a9a9a), x + 6.5f * cosf(t1), y + 6.5f * sinf(t1), rgb(0x9a9a9a), 1.2f, 0);
	}
}

static void at_draw(float W) {
	if (!at_on) return;
	AtLay L = at_lay(W);
	rect(0, 0, W, 240, rgba(0, 150));
	rrect(L.x + 2, L.y + 4, L.w, L.h, 8, rgba(0, 90));
	rrect(L.x, L.y, L.w, L.h, 8, rgb(0x282828));
	text("Add to playlist", L.x + 14, L.y + 22, 10, rgb(GREY), BOLD, 0);
	// keep the cursor's row in view
	if (cursor_vis && at_cur < at_n) {
		if (at_cur * AT_ROW < at_sc) at_sc = at_cur * AT_ROW;
		if ((at_cur + 1) * AT_ROW > at_sc + L.rows_h) at_sc = (at_cur + 1) * AT_ROW - L.rows_h;
	}
	for (int i = 0; i < at_n; i++) {
		float y = L.ry + i * AT_ROW - at_sc;
		if (y < L.ry - 1 || y + AT_ROW > L.ry + L.rows_h + 1) continue;
		int k = at_list[i];
		Group *g = &lib.lists[k];
		bool foc = cursor_vis && at_cur == i;
		if (foc) rrect(L.x + 4, y, L.w - 8, AT_ROW, 4, rgba(0xffffff, 20));
		float cx = L.x + 12;
		if (k == 0) liked_tile(cx, y + 4, 26);
		else if (!list_tile(k, cx, y + 4, 26)) { if (g->n) thumb(lib.t[g->songs[0]].album_id, cx, y + 4, 26); else rrect(cx, y + 4, 26, 26, 3, rgb(0x3a3a3a)); }
		char s[48];
		int n = g->n + (at_in[i] && !at_was[i]) - (!at_in[i] && at_was[i]);   // as it will be
		snprintf(s, sizeof s, "%d song%s", n, n == 1 ? "" : "s");
		text(k == 0 ? "Liked Songs" : g->name, cx + 34, y + 15, 11, rgb(0xffffff), 0, L.w - 84);
		float sx = cx + 34;
		if (k > 0 && pin_of(g->name) >= 0) {   // pinned: Spotify's green pushpin
			u32 c = rgb(GREEN);
			C2D_DrawCircleSolid(sx + 5, y + 22.5f, 0, 2.4f, c);
			C2D_DrawTriangle(sx, y + 28, c, sx + 3.6f, y + 23.6f, c, sx + 2.3f, y + 25, c, 0);
			sx += 10;
		}
		text(s, sx, y + 28, 9, rgb(GREY), 0, 0);
		at_check(L.x + L.w - 20, y + AT_ROW / 2, at_in[i]);
	}
	if (at_n > L.vis) {   // a thin bar for where in the list it is
		float th = L.rows_h * L.vis / at_n, ty = L.ry + (L.rows_h - th) * (at_sc / ((at_n - L.vis) * AT_ROW > 0 ? (at_n - L.vis) * AT_ROW : 1));
		rect(L.x + L.w - 4, ty, 2, th, rgba(0xffffff, 60));
	}
	rect(L.x + 10, L.by - 4, L.w - 20, 1, rgba(0xffffff, 25));
	float dw = 64, dx = L.x + L.w - 12 - dw, cxb = dx - 8 - dw;
	text("Cancel", cxb + dw / 2, L.by + 19, 11, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
	if (cursor_vis && at_cur == at_n + 1) ring(cxb, L.by + 2, dw, 26, 13);
	rrect(dx, L.by + 2, dw, 26, 13, rgb(0xffffff));
	text("Done", dx + dw / 2, L.by + 19, 11, rgb(0), C2D_AlignCenter | BOLD, 0);
	if (cursor_vis && at_cur == at_n) ring(dx, L.by + 2, dw, 26, 13);
}

static void at_keys(u32 kd, u32 kr) {
	if (!at_on) return;
	if (!cursor_vis && kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) { cursor_vis = true; return; }
	int last = at_n + 1;
	if (kr & KEY_DUP) at_cur = at_cur == 0 ? at_n : at_cur > at_n ? at_n - 1 : at_cur - 1;   // the buttons count as one line
	else if (kr & KEY_DDOWN) at_cur = at_cur >= at_n ? 0 : at_cur + 1;
	else if (kr & (KEY_DLEFT | KEY_DRIGHT) && at_cur >= at_n) at_cur = at_cur == at_n ? last : at_n;
	if (kd & KEY_A) {
		cursor_vis = true;
		if (at_cur < at_n) at_in[at_cur] = !at_in[at_cur];
		else at_done(at_cur == at_n);
	} else if (kd & KEY_X) at_done(true);   // X opened it, X can close it, saved
	else if (kd & KEY_B) at_done(false);
}

static void at_tap(int x, int y) {   // the touchscreen, 320 wide
	AtLay L = at_lay(320);
	float dw = 64, dx = L.x + L.w - 12 - dw, cxb = dx - 8 - dw;
	if (y >= L.by && y < L.by + 30) {
		if (x >= dx && x < dx + dw) at_done(true);
		else if (x >= cxb && x < cxb + dw) at_done(false);
		return;
	}
	if (x < L.x || x >= L.x + L.w || y < L.y || y >= L.y + L.h) { at_done(false); return; }   // outside it: as Cancel
	if (y >= L.ry && y < L.ry + L.rows_h) {
		int i = (int)((y - L.ry + at_sc) / AT_ROW);
		if (i >= 0 && i < at_n) { at_cur = i; at_in[i] = !at_in[i]; }
	}
}
static void at_drag(float dy) {   // a finger up and down the rows
	AtLay L = at_lay(320);
	float mx = at_n * AT_ROW - L.rows_h;
	at_sc += dy;
	if (at_sc > mx) at_sc = mx;
	if (at_sc < 0) at_sc = 0;
}
