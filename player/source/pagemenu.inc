// ---- the dots' menu, Edit details, and the delete question ---------------------------------
// Included by main.c after detailpage.inc. The dots on an album's or playlist's page open a menu
// the way Spotify's does, under the dots: a playlist of yours has Edit details, Make private (or
// public) and Delete; someone else's you follow, Remove from Your Library; an album, its artist and
// liking all of it. Edit details is Spotify's box: the cover, the name, the description, the private
// switch and Save (the picture stays as it is: there's nothing to pick one with on a 3DS). Delete asks
// first. All of it on the D-pad and the touchscreen; changes go up to Spotify with the next push.

enum { PM_NONE, PM_MENU, PM_EDIT, PM_ASK };
enum { MI_EDIT, MI_PUB, MI_DELETE, MI_REMOVE, MI_ARTIST, MI_LIKE };
static int pm_items[6], pm_n, pm_cur;
static char pe_name[304], pe_desc[304];   // Edit details: what Save will save
static bool pe_pub;
enum { PE_NAME, PE_DESC, PE_PUB, PE_SAVE, PE_CLOSE };

static bool pm_on(void) { return pm != PM_NONE && detail >= 0 && rich(); }
static bool pm_spotify(void) { return dkind == T_PLAYLISTS && detail > 0 && lib.lists[detail].pid; }

static void pm_open(void) {
	pm_n = 0;
	if (dkind == T_ALBUMS) {
		if (dp_artist >= 0) pm_items[pm_n++] = MI_ARTIST;
		pm_items[pm_n++] = MI_LIKE;
	} else if (detail > 0 && le_editable(detail)) {
		pm_items[pm_n++] = MI_EDIT;
		if (pm_spotify()) pm_items[pm_n++] = MI_PUB;   // a list that's only on the card is nobody's to see anyway
		pm_items[pm_n++] = MI_DELETE;
	} else if (pm_spotify()) pm_items[pm_n++] = MI_REMOVE;
	if (!pm_n) { say(detail == 0 ? "Liked Songs is made of your hearts" : "Nothing to change here"); return; }
	pm = PM_MENU;
	pm_cur = 0;
	audio_tick(TICK_GRAB);
}
static void pm_close(void) { pm = PM_NONE; }

static const char *pm_label(int it) {
	Group *g = &lib.lists[detail > 0 && dkind == T_PLAYLISTS ? detail : 0];
	switch (it) {
	case MI_EDIT: return "Edit details";
	case MI_PUB: return g->pub ? "Make private" : "Make public";
	case MI_DELETE: return "Delete";
	case MI_REMOVE: return "Remove from Your Library";
	case MI_ARTIST: return "Go to artist";
	default: {
		Group *a = dgroup();
		bool all = a->n > 0;
		for (int k = 0; k < a->n && all; k++) all = library_is_liked(a->songs[k]);
		return all ? "Remove from Liked Songs" : "Add to Liked Songs";
	}
	}
}

// ---- the icons -------------------------------------------------------------------------------

static void ic_ring(float x, float y, float r, float t, u32 c, u32 bg) { C2D_DrawCircleSolid(x, y, 0, r, c); C2D_DrawCircleSolid(x, y, 0, r - t, bg); }
static void ic_lock(float x, float y, u32 c, u32 bg, bool open) {
	ic_ring(x + (open ? 3 : 0), y - 2.5f, 3.6f, 1.3f, c, bg);   // the shackle (its bottom half hides under the body)
	rect(x - 4.5f, y - 1, 9, 7, c);
}
static void ic_pencil(float x, float y, u32 c) {
	C2D_DrawLine(x - 3.5f, y + 3.5f, c, x + 4, y - 4, c, 2.6f, 0);
	C2D_DrawTriangle(x - 5.5f, y + 5.5f, c, x - 4.8f, y + 2.2f, c, x - 2.2f, y + 4.8f, c, 0);
}
static void ic_minus(float x, float y, u32 c, u32 bg) { ic_ring(x, y, 6, 1.3f, c, bg); rect(x - 3, y - 0.65f, 6, 1.3f, c); }
static void pm_icon(int it, float x, float y, u32 c, u32 bg) {
	switch (it) {
	case MI_EDIT: ic_pencil(x, y, c); break;
	case MI_PUB: ic_lock(x, y, c, bg, !lib.lists[detail].pub); break;
	case MI_DELETE: case MI_REMOVE: ic_minus(x, y, c, bg); break;
	case MI_ARTIST: ic_tab(T_ARTISTS, x, y, c, bg); break;
	default: ic_heart(x, y, 4.2f, c); break;
	}
}

// ---- where things are (the list's screen: 400 up top, 320 on the touchscreen) ----------------

static void pm_menu_box(float W, float *x, float *y, float *w, float *h) {
	float dots = dkind == T_ALBUMS ? 130 : 96, mid = dp_hero(W) + DP_ACT / 2 - scroll[V_DETAIL];
	*w = W > 320 ? 200 : 186;
	*h = 8 + pm_n * 28;
	*x = dots - 10;
	if (*x + *w > W - 6) *x = W - 6 - *w;
	*y = mid + 14;
	float bot = W > 320 ? 236 : 186;   // above the mini player down there
	if (*y + *h > bot) *y = mid - 14 - *h;   // no room under the dots: over them
	if (*y < LIST0 + 2) *y = LIST0 + 2;
}
typedef struct { float x, y, w, h, cs, rx, rw, ny, dy, dh, by; } PeLay;
static PeLay pe_lay(float W) {
	PeLay L;
	bool wide = W > 320;
	L.w = wide ? 372 : 304;
	L.h = wide ? 204 : 200;
	L.x = (W - L.w) / 2;
	L.y = wide ? (240 - L.h) / 2 : 20;
	L.cs = wide ? 104 : 84;
	L.rx = L.x + 16 + L.cs + 12;
	L.rw = L.x + L.w - 16 - L.rx;
	L.ny = L.y + 42;
	L.dy = L.ny + 32;
	L.dh = L.cs - 32;
	L.by = L.y + L.h - 44;
	return L;
}
static float pe_pub_w(void) { return 30 + measure(pe_pub ? "Make private" : "Make public", 10) + 12; }

// ---- drawing ---------------------------------------------------------------------------------

static void pm_draw(float W, float H) {
	if (!pm_on()) return;
	u32 wh = rgb(0xffffff);
	if (pm == PM_MENU) {
		float x, y, w, h;
		pm_menu_box(W, &x, &y, &w, &h);
		rrect(x + 2, y + 4, w, h, 6, rgba(0, 90));   // its shadow
		rrect(x, y, w, h, 6, rgb(0x282828));
		for (int i = 0; i < pm_n; i++) {
			float iy = y + 4 + i * 28;
			bool foc = i == pm_cur && cursor_vis;
			if (foc) rrect(x + 4, iy, w - 8, 28, 3, rgba(0xffffff, 20));
			u32 c = rgb(pm_items[i] == MI_DELETE ? 0xffffff : 0xe8e8e8);
			pm_icon(pm_items[i], x + 20, iy + 14, c, foc ? 0x383838 : 0x282828);
			text(pm_label(pm_items[i]), x + 36, iy + 18, 11, c, 0, w - 44);
		}
		return;
	}
	rect(0, 0, W, H, rgba(0, 160));   // the page behind, dimmed
	if (pm == PM_ASK) {   // Spotify's question, word for word
		bool del = pm_items[0] == MI_DELETE;
		float w = W > 320 ? 320 : 290, h = 128, x = (W - w) / 2, y = (W > 320 ? 240 : 190) / 2 - h / 2;
		rrect(x, y, w, h, 8, rgb(0x282828));
		text(del ? "Delete from Your Library?" : "Remove from Your Library?", x + 18, y + 30, 14, wh, BOLD, w - 36);
		char s[400];
		snprintf(s, sizeof s, "This will %s %s from Your Library.", del ? "delete" : "remove", lib.lists[detail].name);
		text2(s, x + 18, y + 54, 13, 10, rgb(0xdddddd), 0, w - 36);
		float bw = 76, bh = 30, by = y + h - bh - 14, sx = x + w - 18 - bw, cx = sx - 12 - bw;
		if (cursor_vis && pm_cur == 0) ring(cx, by, bw, bh, 15);
		text("Cancel", cx + bw / 2, by + 20, 11, wh, C2D_AlignCenter | BOLD, 0);
		rrect(sx, by, bw, bh, 15, rgb(GREEN));
		text(del ? "Delete" : "Remove", sx + bw / 2, by + 20, 11, rgb(0), C2D_AlignCenter | BOLD, 0);
		if (cursor_vis && pm_cur == 1) ring(sx, by, bw, bh, 15);
		return;
	}
	// Edit details
	PeLay L = pe_lay(W);
	Group *g = &lib.lists[detail];
	rrect(L.x, L.y, L.w, L.h, 8, rgb(0x282828));
	text("Edit details", L.x + 16, L.y + 28, 15, wh, BOLD | NAMEF, 0);
	float cx = L.x + L.w - 22, cy = L.y + 22;   // the close button's X
	C2D_DrawLine(cx - 5, cy - 5, wh, cx + 5, cy + 5, wh, 1.6f, 0);
	C2D_DrawLine(cx - 5, cy + 5, wh, cx + 5, cy - 5, wh, 1.6f, 0);
	if (cursor_vis && pm_cur == PE_CLOSE) ring(cx - 11, cy - 11, 22, 22, 11);
	dp_cover(g, L.x + 16, L.ny, L.cs);
	rrect(L.rx, L.ny, L.rw, 26, 4, rgb(0x3e3e3e));   // the name
	if (pe_name[0]) text(pe_name, L.rx + 8, L.ny + 17, 11, wh, 0, L.rw - 16);
	else text("Add a name", L.rx + 8, L.ny + 17, 11, rgb(0x9a9a9a), 0, 0);
	if (cursor_vis && pm_cur == PE_NAME) ring(L.rx, L.ny, L.rw, 26, 4);
	rrect(L.rx, L.dy, L.rw, L.dh, 4, rgb(0x3e3e3e));   // the description
	if (pe_desc[0]) text2(pe_desc, L.rx + 8, L.dy + 16, 13, 10, wh, 0, L.rw - 16);
	else text("Add an optional description", L.rx + 8, L.dy + 16, 10, rgb(0x9a9a9a), 0, L.rw - 16);
	if (cursor_vis && pm_cur == PE_DESC) ring(L.rx, L.dy, L.rw, L.dh, 4);
	if (pm_spotify()) {   // the private switch, as a pill like Spotify's
		float pw = pe_pub_w();
		rstroke(L.x + 16, L.by + 4, pw, 26, 13, rgba(0xffffff, 150));
		ic_lock(L.x + 30, L.by + 16, wh, 0x282828, !pe_pub);
		text(pe_pub ? "Make private" : "Make public", L.x + 42, L.by + 21, 10, wh, BOLD, 0);
		if (cursor_vis && pm_cur == PE_PUB) ring(L.x + 16, L.by + 4, pw, 26, 13);
	}
	float sw = 72, sx = L.x + L.w - 16 - sw;
	rrect(sx, L.by + 2, sw, 30, 15, wh);
	text("Save", sx + sw / 2, L.by + 22, 12, rgb(0), C2D_AlignCenter | BOLD, 0);
	if (cursor_vis && pm_cur == PE_SAVE) ring(sx, L.by + 2, sw, 30, 15);
}

// ---- what they do ----------------------------------------------------------------------------

static void on_reloaded(bool lists);   // main.c: the lists were read again
static void pm_pick(int it) {
	int k = detail;
	Group *g = &lib.lists[k > 0 && dkind == T_PLAYLISTS ? k : 0];
	switch (it) {
	case MI_EDIT:
		snprintf(pe_name, sizeof pe_name, "%s", g->name);
		snprintf(pe_desc, sizeof pe_desc, "%s", g->desc ? g->desc : "");
		pe_pub = g->pub;
		pm = PM_EDIT;
		pm_cur = PE_NAME;
		break;
	case MI_PUB: {
		bool to = !g->pub;
		const char *e = library_list_public(k, to);
		if (e) { say(e); break; }
		spot_list_details(g->pid, NULL, NULL, to);
		say(to ? "Made public" : "Made private");
		pm_close();
		break;
	}
	case MI_DELETE: case MI_REMOVE:
		pm = PM_ASK;
		pm_items[0] = it;   // what the question's about (the menu's gone)
		pm_n = 1;
		pm_cur = 0;         // on Cancel: a slip of A doesn't delete anything
		break;
	case MI_ARTIST: {
		int a = dp_artist;
		pm_close();
		close_detail();
		ap_clear();   // a fresh stack, as the player's artist name does
		ap_open_local(a);
		break;
	}
	case MI_LIKE:
		pm_close();
		dp_press(2, NULL);
		break;
	}
}

static void pm_delete(void) {   // the question said yes
	int k = detail;
	if (spot_busy()) { say("Syncing with Spotify: try again in a moment"); return; }   // it may be writing this very list
	Group *g = &lib.lists[k];
	char name[256], pid[80] = "";
	snprintf(name, sizeof name, "%s", g->name);
	if (g->pid) snprintf(pid, sizeof pid, "%s", g->pid);
	const char *e = library_list_delete(k);
	if (e) { say(e); pm_close(); return; }
	if (pid[0]) spot_list_delete(pid);
	int p = pin_of(name);
	if (p >= 0) pin_toggle(name);
	pm_close();
	close_detail();
	on_reloaded(true);
	say("Removed from Your Library");
}

static void pe_keyboard(bool desc) {
	SwkbdState kb;
	char buf[304];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, desc ? 300 : 100);   // Spotify's own limits
	swkbdSetHintText(&kb, desc ? "Add an optional description" : "Give it a name");
	swkbdSetInitialText(&kb, desc ? pe_desc : pe_name);
	if (!desc) swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM) return;
	for (char *c = buf; *c; c++) if ((u8)*c < 0x20) *c = ' ';
	le_trim(buf);
	snprintf(desc ? pe_desc : pe_name, sizeof pe_name, "%s", buf);
}

static void pe_save(void) {
	int k = detail;
	Group *g = &lib.lists[k];
	bool nm = strcmp(pe_name, g->name) != 0, ds = strcmp(pe_desc, g->desc ? g->desc : "") != 0, pb = pe_pub != g->pub && pm_spotify();
	if (!pe_name[0]) { say("A playlist needs a name"); pm_cur = PE_NAME; return; }
	if (nm || ds) {
		char old[256];
		snprintf(old, sizeof old, "%s", g->name);
		const char *e = library_list_details(k, nm ? pe_name : NULL, ds ? pe_desc : NULL);
		if (e) { say(e); return; }
		g = &lib.lists[k];
		if (nm && strcmp(old, g->name)) {   // what's known by its name: its pin, and what's playing from it
			int p = pin_of(old);
			if (p >= 0) { free(pins[p]); pins[p] = strdup(g->name); pins_save(); }
			if (src_kind == T_PLAYLISTS && !strcmp(src_name, old)) snprintf(src_name, sizeof src_name, "%s", g->name);
		}
	}
	if (pb) { const char *e = library_list_public(k, pe_pub); if (e) { say(e); return; } }
	if (g->pid && (nm || ds || pb)) spot_list_details(g->pid, nm ? g->name : NULL, ds ? pe_desc : NULL, pb ? pe_pub : -1);
	pm_close();
	say(nm || ds || pb ? "Saved" : "Nothing changed");
	dirty = true;
}

// A or a tap on the Edit details box's field f
static void pe_do(int f) {
	if (f == PE_NAME || f == PE_DESC) pe_keyboard(f == PE_DESC);
	else if (f == PE_PUB && pm_spotify()) pe_pub = !pe_pub;
	else if (f == PE_SAVE) pe_save();
	else if (f == PE_CLOSE) pm_close();
}

// ---- the D-pad, a finger ---------------------------------------------------------------------

static void pm_keys(u32 kd, u32 kr) {
	if (!pm_on()) return;
	if (!cursor_vis && kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) { cursor_vis = true; return; }
	if (pm == PM_MENU) {
		if (kr & KEY_DUP) pm_cur = (pm_cur + pm_n - 1) % pm_n;
		if (kr & KEY_DDOWN) pm_cur = (pm_cur + 1) % pm_n;
		if (kd & KEY_A) { cursor_vis = true; pm_pick(pm_items[pm_cur]); }
		else if (kd & (KEY_B | KEY_DLEFT)) pm_close();
	} else if (pm == PM_ASK) {
		if (kr & (KEY_DLEFT | KEY_DRIGHT)) pm_cur ^= 1;
		if (kd & KEY_A) { if (pm_cur == 1) pm_delete(); else pm_close(); }
		else if (kd & KEY_B) pm_close();
	} else {   // Edit details: the name, the description; the switch and Save along the bottom; the X up top
		bool pub = pm_spotify();
		int c = pm_cur;
		if (kr & KEY_DUP) c = c == PE_NAME ? PE_CLOSE : c == PE_DESC ? PE_NAME : c == PE_CLOSE ? (pub ? PE_PUB : PE_SAVE) : PE_DESC;
		else if (kr & KEY_DDOWN) c = c == PE_CLOSE ? PE_NAME : c == PE_NAME ? PE_DESC : c == PE_DESC ? PE_SAVE : PE_CLOSE;
		else if (kr & (KEY_DLEFT | KEY_DRIGHT)) {
			if (c == PE_SAVE && pub) c = PE_PUB;
			else if (c == PE_PUB) c = PE_SAVE;
		}
		pm_cur = c;
		if (kd & KEY_A) { cursor_vis = true; pe_do(pm_cur); }
		else if (kd & KEY_B) pm_close();
	}
}

static void pm_tap(int x, int y) {   // the touchscreen: 320 wide
	float W = 320;
	if (pm == PM_MENU) {
		float mx, my, mw, mh;
		pm_menu_box(W, &mx, &my, &mw, &mh);
		if (x >= mx && x < mx + mw && y >= my + 4 && y < my + 4 + pm_n * 28) { pm_cur = (y - my - 4) / 28; pm_pick(pm_items[pm_cur]); }
		else pm_close();   // anywhere else puts it away
		return;
	}
	if (pm == PM_ASK) {
		float w = 290, h = 128, bx = (W - w) / 2, by0 = 95 - h / 2, bw = 76, bh = 30, by = by0 + h - bh - 14, sx = bx + w - 18 - bw, cx = sx - 12 - bw;
		if (y >= by && y < by + bh && x >= sx && x < sx + bw) pm_delete();
		else if (y >= by && y < by + bh && x >= cx && x < cx + bw) pm_close();
		else if (x < bx || x >= bx + w || y < by0 || y >= by0 + h) pm_close();
		return;
	}
	PeLay L = pe_lay(W);
	float sw = 72, sx = L.x + L.w - 16 - sw;
	if (x >= L.x + L.w - 36 && y < L.y + 36 && y >= L.y) pe_do(PE_CLOSE);
	else if (x >= L.rx && x < L.rx + L.rw && y >= L.ny && y < L.ny + 26) pe_do(PE_NAME);
	else if (x >= L.rx && x < L.rx + L.rw && y >= L.dy && y < L.dy + L.dh) pe_do(PE_DESC);
	else if (y >= L.by && y < L.by + 34 && x >= sx && x < sx + sw) pe_do(PE_SAVE);
	else if (y >= L.by && y < L.by + 34 && x >= L.x + 16 && x < L.x + 16 + pe_pub_w()) pe_do(PE_PUB);
	else if (x < L.x || x >= L.x + L.w || y < L.y || y >= L.y + L.h) pm_close();   // outside it: Spotify's box closes too
}
