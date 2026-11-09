// ---- the grid, the Songs tab's letters, the header's tabs -------------------------------------
// Included by main.c after homepage.inc. Playlists, Artists and Albums are covers in a grid with
// their names under them (lay(), box() and the rest of the geometry live in main.c); Songs is rows
// under a letter each, with the letters down the right edge to jump by.

// a cover in the grid: Playlists, Artists (round, the name under it centred), Albums
static void draw_cell(int k, float x, float y, float cw, const AudioInfo *ai) {
	int i = items[k];
	bool now = false, round = tab == T_ARTISTS;
	const char *name;
	char sub[200] = "";
	if (tab == T_ALBUMS) {
		big_album(i, x, y, cw);
		name = lib.albums[i].name;
		snprintf(sub, sizeof sub, "%s", lib.albums[i].artist ? lib.albums[i].artist : "");
		now = ai->open && lib.t[ai->track].album_id == i;
	} else if (round) {
		big_artist(i, x, y, cw);
		name = lib.artists[i].name;
		now = ai->open && lib.t[ai->track].artist_id == i;
	} else {
		Group *g = &lib.lists[i];
		big_list(i, x, y, cw);
		name = g->name;
		if (i == 0 || !owner_of(g)) snprintf(sub, sizeof sub, "%d song%s", g->n, g->n == 1 ? "" : "s");
		else snprintf(sub, sizeof sub, "%s", owner_of(g));
		now = ai->open && src_kind == T_PLAYLISTS && src_id == i;
	}
	u32 nc = rgb(now ? GREEN : 0xffffff);
	if (round) { text(name, x + cw / 2, y + cw + 14, 10, nc, C2D_AlignCenter | BOLD, cw + 8); return; }
	text(name, x, y + cw + 14, 10, nc, BOLD, cw);
	float sx = x;
	if (tab == T_PLAYLISTS && i > 0 && pin_of(name) >= 0) {   // pinned: a green pushpin before the line, as Spotify shows it
		u32 c = rgb(GREEN);
		C2D_DrawCircleSolid(x + 5.5f, y + cw + 20.5f, 0, 2.6f, c);
		C2D_DrawTriangle(x + 0.5f, y + cw + 26.5f, c, x + 4.2f, y + cw + 21.8f, c, x + 2.8f, y + cw + 23.2f, c, 0);
		sx += 10;
	}
	text(sub, sx, y + cw + 26, 9, rgb(GREY), 0, cw - (sx - x));
}
static void cell_soon(int k) {   // a row just under the screen: its covers asked for, not drawn
	if (tab == T_ALBUMS) cc_album(items[k], false);
	else if (tab == T_ARTISTS) cc_artist(items[k], false);
	else cc_list(items[k], false);
}

// Songs' fast scroll, the way Android's is: a slim handle down the right edge where the list is;
// a finger on it drags the whole list, and the letter at the top shows big beside it while it moves
// (and for a moment after a jump by letter on the D-pad). The tiny A-to-Z rail it replaces sat on
// top of the songs and its letters ran into each other (Alex, build 59).
static u64 bubble_t;   // a jump by letter, or the handle being dragged: the bubble shows till 700 ms after
static bool rail_on(void) { return lay() == LAY_SONGS && nitems > 12; }
static float handle_y(float H, int v, float *hh) {   // the handle's top, and its height
	float top = LIST0 + 6, bot = H - 6, m = max_scroll(), span = bot - top;
	*hh = span * (H - LIST0) / (m + H - LIST0);
	if (*hh < 22) *hh = 22;
	return top + (m > 0 ? (span - *hh) * scroll[v] / m : 0);
}
static void draw_handle(float W, float H, int v) {
	if (!rail_on()) return;
	float hh, y = handle_y(H, v, &hh);
	bool hot = osGetTime() - bubble_t < 700;
	rrect(W - (hot ? 8 : 6), y, hot ? 6 : 4, hh, 3, hot ? rgb(GREEN) : rgba(0xffffff, 90));
	if (!hot || !letters) return;
	// the letter at the top of the screen, in a green drop whose point touches the handle (Android's
	// fast-scroll bubble, in our green): a disc, and a corner of it pulled out to a point at the right
	char ltr[2] = {letter_of(items[first_shown(v)]), 0};
	float r = 15, cy = y + hh / 2, cx = W - 12 - r * 1.42f;
	if (cy < LIST0 + r + 4) cy = LIST0 + r + 4;
	if (cy > H - r - 4) cy = H - r - 4;
	u32 g = rgb(GREEN);
	C2D_DrawCircleSolid(cx, cy, 0, r, g);
	C2D_DrawTriangle(cx + r * 0.7071f, cy - r * 0.7071f, g, cx + r * 0.7071f, cy + r * 0.7071f, g, cx + r * 1.42f, cy, g, 0);   // the point
	// centred by the letter's own height: a capital stands about 0.72 of the size above its baseline
	float px = 20;
	text(ltr, cx, cy + px * 0.36f, px, rgb(0), C2D_AlignCenter | BOLD | NAMEF, 0);
}
static void rail_jump(float y) {   // the handle under a finger at y: the list goes where it says
	float top = LIST0 + 6, bot = LIST1 - 6, hh;
	handle_y(LIST1, V(), &hh);
	float f = (y - top - hh / 2) / (bot - top - hh);
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	int v = V();
	scroll[v] = f * max_scroll();
	vel = 0;
	bubble_t = osGetTime();
	if (cursor_vis) cur[v] = first_shown(v);
}
// left and right on Songs: the start of the next letter, or of this one (then the one before)
static int letter_step(int k, int dir) {
	if (k < 0 || k >= nitems) return k;
	char c = letter_of(items[k]);
	if (dir > 0) {
		int j = k;
		while (j < nitems && letter_of(items[j]) == c) j++;
		return j < nitems ? j : k;
	}
	if (k > 0 && letter_of(items[k - 1]) == c) { while (k > 0 && letter_of(items[k - 1]) == c) k--; return k; }
	if (k == 0) return 0;
	k--;
	c = letter_of(items[k]);
	while (k > 0 && letter_of(items[k - 1]) == c) k--;
	return k;
}

// the header's tabs: an icon each, and the one you're on says its name
static void ic_tab(int t, float x, float y, u32 c, u32 bg) {
	if (t == T_PLAYLISTS) {   // lines, and a note
		rect(x - 6.5f, y - 4.5f, 7.5f, 1.6f, c);
		rect(x - 6.5f, y - 1, 7.5f, 1.6f, c);
		rect(x - 6.5f, y + 2.5f, 4.5f, 1.6f, c);
		rect(x + 3.6f, y - 4.5f, 1.4f, 7.5f, c);
		rect(x + 3.6f, y - 4.5f, 3, 1.4f, c);
		C2D_DrawCircleSolid(x + 2.6f, y + 3.2f, 0, 2, c);
	} else if (t == T_ARTISTS) {   // a head and shoulders
		C2D_DrawCircleSolid(x, y - 2.6f, 0, 2.9f, c);
		for (int i = 0; i < 8; i++) {
			float a0 = (float)M_PI * (1 + i / 8.0f), a1 = (float)M_PI * (1 + (i + 1) / 8.0f);
			C2D_DrawTriangle(x, y + 6, c, x + 5.6f * cosf(a0), y + 6 + 5.6f * sinf(a0), c, x + 5.6f * cosf(a1), y + 6 + 5.6f * sinf(a1), c, 0);
		}
	} else if (t == T_ALBUMS) {   // a record
		C2D_DrawCircleSolid(x, y, 0, 6, c);
		C2D_DrawCircleSolid(x, y, 0, 1.7f, rgb(bg));
	} else {   // two notes, beamed
		C2D_DrawCircleSolid(x - 3.4f, y + 3.6f, 0, 2.1f, c);
		C2D_DrawCircleSolid(x + 3.6f, y + 2.6f, 0, 2.1f, c);
		rect(x - 1.8f, y - 4.2f, 1.4f, 7.8f, c);
		rect(x + 5.2f, y - 5.2f, 1.4f, 7.8f, c);
		C2D_DrawTriangle(x - 1.8f, y - 4.2f, c, x + 6.6f, y - 5.2f, c, x + 6.6f, y - 3, c, 0);
		C2D_DrawTriangle(x - 1.8f, y - 4.2f, c, x + 6.6f, y - 3, c, x - 1.8f, y - 2, c, 0);
	}
}
// o: how much it's the tab you're on (tab_spring), 0 an icon, 1 a green pill with its name, a
// little past 1 for a moment as it lands
static float tab_chip(float x, int t, float o) {
	float c = clamp01(o), w = 26 + (measure(TAB[t], 11) + 11) * (o > 0 ? o : 0);
	u32 bg = mixc(GREEN, CHIP, c);
	rrect(x, 5, w, 22, 11, rgb(bg));
	ic_tab(t, x + 13, 16, rgb(mixc(0, 0xffffff, c)), bg);
	float a = clamp01((c - 0.8f) / 0.2f);   // the name, once there's room for it
	if (a > 0) text(TAB[t], x + 25, 20.5f, 11, rgba(0, (u8)(255 * a)), BOLD, 0);
	hit(x - 3, x + w + 3, t);
	return x + w + 6;
}
static const int TAB_ORDER[NTAB] = {T_PLAYLISTS, T_ARTISTS, T_ALBUMS, T_SONGS};   // as the header has them
