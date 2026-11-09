// An album or playlist page, drawn the way Spotify draws it: the header scrolls away with the
// songs (the cover, "Album" or "Public Playlist", the title as large as it fits, the description,
// then the owner's or artist's picture and the line under it), washed in the cover's colour; the
// green play button, shuffle and the liked check under it; numbered rows; and a bar at the top
// that fills in with the colour and the title once the header has gone under it. Included by
// main.c, after artistpage.inc (put_rect, ic_check, ic_dots). The geometry the list shares with
// it (rows_base, row_top, row_at, max_scroll) lives in main.c.

static C3D_Tex dp_tex;          // the cover at (0,0) 96², the owner's picture at (96,0) 32²
static bool dp_tex_ok, dp_cov, dp_av;
static int dp_req = -1;         // the art request this page is waiting on
static u32 dp_col = 0x404040;   // the cover's colour, which the header is washed in
static int dp_artist = -1;      // an album's artist: their face, and a tap on their name
static bool dp_artist_exact;    // ...found by the album artist's own name (not "Various artists")
static int dp_type, dp_year;
static double dp_secs;
static char dp_me[64];          // your Spotify id (lists/me.id): your picture goes on your lists
static float dp_name_x0, dp_name_x1, dp_name_y;   // where the artist's name was drawn (touchscreen)
static float dp_tx, dp_title_y0 = 1e9f, dp_title_y1 = -1e9f, dp_desc_y0 = 1e9f, dp_desc_y1 = -1e9f;   // ...a list's title and description

static const char *DP_TYPE[] = {"Album", "Single", "EP", "Playlist", "Public Playlist"};

static void dp_init(void) {
	dp_tex_ok = C3D_TexInit(&dp_tex, 128, 128, GPU_RGBA8);
	if (!dp_tex_ok) return;
	memset(dp_tex.data, 0, dp_tex.size);
	C3D_TexSetFilter(&dp_tex, GPU_LINEAR, GPU_LINEAR);
}

static int dp_top_no;
// The page's length and year, again whenever its songs change (a like on Liked Songs, a sync)
static void dp_summary(void) {
	dp_secs = 0;
	dp_year = 0;
	if (!rich()) return;
	Group *g = dgroup();
	dp_top_no = g->n;
	for (int k = 0; k < g->n; k++) {
		Track *t = &lib.t[g->songs[k]];
		dp_secs += t->secs;
		if (t->year > dp_year) dp_year = t->year;
		if (t->no > dp_top_no) dp_top_no = t->no;
	}
}

// A page has opened (open_detail): what it is, and its cover asked for
static void dp_open(void) {
	dp_cov = dp_av = false;
	dp_artist = -1;
	dp_artist_exact = false;
	dp_summary();
	if (!rich()) return;
	Group *g = dgroup();
	int top_no = dp_top_no;
	if (dkind == T_ALBUMS) {
		dp_col = g->color;
		// Spotify's rule (1-3 songs under half an hour a single, up to 6 an EP), counted by the
		// highest track number, so half an album on the card is still an album
		dp_type = dp_secs < 1800 && top_no <= 3 ? 1 : dp_secs < 1800 && top_no <= 6 ? 2 : 0;
		for (int a = 0; a < lib.nar && g->artist; a++)
			if (!strcasecmp(lib.artists[a].name, g->artist)) { dp_artist = a; dp_artist_exact = true; break; }
		if (dp_artist < 0 && g->n) dp_artist = lib.t[g->songs[0]].artist_id;
		dp_req = art_detail_want(g->n ? g->songs[0] : -1, NULL, NULL);
	} else {
		dp_col = detail == 0 ? 0x5038a0 : g->color;   // Liked Songs: Spotify's purple-blue
		dp_type = g->pub ? 4 : 3;
		if (!dp_me[0]) {
			FILE *f = fopen(LISTS_DIR "/me.id", "r");
			if (f) { if (!fgets(dp_me, sizeof dp_me, f)) dp_me[0] = 0; fclose(f); dp_me[strcspn(dp_me, "\r\n")] = 0; }
		}
		bool mine = detail == 0 || (g->owner_id && dp_me[0] && !strcmp(g->owner_id, dp_me));
		dp_req = art_detail_want(-1, g->image, mine ? LISTS_DIR "/me.jpg" : NULL);
	}
}

// Every frame: the cover and the picture, once decoded, if they're still this page's
static void dp_poll(void) {
	int r;
	const u32 *px, *av;
	u32 c;
	if (!art_detail(&r, &px, &c, &av) || r != dp_req || !dp_tex_ok) return;
	if (px) { put_rect(&dp_tex, 0, 0, px, ART_DETAIL, ART_DETAIL); dp_cov = true; dp_col = c; }
	if (av) { put_rect(&dp_tex, 96, 0, av, ART_AVATAR, ART_AVATAR); dp_av = true; }
}

static void dp_img(int sx, int sy, int sw, float x, float y, float s) {   // a square of dp_tex
	Tex3DS_SubTexture st = {sw, sw, sx / 128.0f, 1 - sy / 128.0f, (sx + sw) / 128.0f, 1 - (sy + sw) / 128.0f};
	C2D_DrawImageAt((C2D_Image){&dp_tex, &st}, x, y, 0, NULL, s / sw, s / sw);
}

static void dp_cover(Group *g, float x, float y, float s) {
	for (int k = 3; k >= 1; k--) rrect(x - k + 3, y - k + 5, s + 2 * k - 2, s + 2 * k - 2, 3 + k, rgba(0, 30));   // its soft shadow
	if (dp_cov) { dp_img(0, 0, 96, x, y, s); return; }
	if (dkind == T_PLAYLISTS && detail == 0) {   // Liked Songs: the heart on Spotify's gradient
		C2D_DrawRectangle(x, y, 0, s, s, rgb(0x450af5), rgb(0x8a6cf0), rgb(0x6a7ee8), rgb(0xc4efd9));
		ic_heart(x + s / 2, y + s / 2, s * 0.17f, rgb(0xffffff));
		return;
	}
	if (dkind == T_ALBUMS) { thumb(detail, x, y, s); return; }   // the small one until the big one's in
	int al[4], n = 0;   // a list without a picture of its own: Spotify's mosaic of its first four albums
	for (int k = 0; k < g->n && n < 4; k++) {
		int a = lib.t[g->songs[k]].album_id;
		bool seen = false;
		for (int j = 0; j < n; j++) seen |= al[j] == a;
		if (!seen) al[n++] = a;
	}
	if (n < 4) { thumb(n ? al[0] : -1, x, y, s); return; }
	for (int q = 0; q < 4; q++) thumb(al[q], x + (q % 2) * s / 2, y + (q / 2) * s / 2, s / 2);
}

static void dp_len(char *o, int cap, double secs) {   // "1 hr 8 min", "2 min 55 sec", as Spotify writes it
	int t = (int)secs, h = t / 3600, m = t % 3600 / 60, s = t % 60;
	if (h) snprintf(o, cap, "%d hr %d min", h, m);
	else snprintf(o, cap, "%d min %d sec", m, s);
}

// the largest of Spotify's title sizes that fits on one line, worked out once per page and width
static float dp_title_px(const char *t, bool wide, float w) {
	static int kd[2] = {-2, -2}, kk[2];
	static float kw[2], px[2];
	if (kd[wide] != detail || kk[wide] != dkind || kw[wide] != w) {
		static const float SZ[2][5] = {{24, 20, 16, 13, 11}, {34, 28, 22, 17, 13}};
		kd[wide] = detail;
		kk[wide] = dkind;
		kw[wide] = w;
		px[wide] = SZ[wide][4];
		for (int k = 0; k < 5; k++) if (measure_name(t, SZ[wide][k]) <= w) { px[wide] = SZ[wide][k]; break; }
	}
	return px[wide];
}

static void dp_draw_hero(float W, const AudioInfo *ai) {
	Group *g = dgroup();
	bool wide = W > 320, pl = dkind == T_PLAYLISTS;
	float sc = scroll[V_DETAIL], H = dp_hero(W), y0 = -sc;
	u32 c = dp_col;

	// the colour: the header in it, then a wash under the play button that fades into the page
	u32 top = mixc(c, 0, 0.62f), bot = mixc(c, 0, 0.42f), under = mixc(c, BG, 0.3f);
	C2D_DrawRectangle(0, y0, 0, W, H, rgb(top), rgb(top), rgb(bot), rgb(bot));
	C2D_DrawRectangle(0, y0 + H, 0, W, 120, rgb(under), rgb(under), rgb(BG), rgb(BG));

	float cs = wide ? 92 : 64, cx = 12, cy = y0 + LIST0;
	dp_cover(g, cx, cy, cs);

	float tx = cx + cs + (wide ? 16 : 12), tw = W - tx - 12;
	const char *title = detail == 0 && pl ? "Liked Songs" : g->name;
	bool edit = pl && le_editable(detail);   // yours: a tap on the title or the description changes it
	bool has = pl && g->desc && g->desc[0], desc = has || edit;
	float ym = cy + cs - 3;                                 // the line along the cover's foot
	float yd = ym - (wide ? 15 : 13);                       // the description over it
	// the title over that, its descenders (the g in "Tengo") clear of what's under it, and the
	// "Single" over the title's capitals, not over the font's whole ascent (Alex, build 62: the
	// title sat low, into the artist line, with a hole above it). px is the ascent; capitals ~0.72
	// of it, descenders ~0.26.
	float px = dp_title_px(title, wide, tw);
	float below = desc ? yd - (wide ? 9 : 8) : ym - 11;    // the top of the description, or of the artist line
	float yt = below - 2 - 0.26f * px;
	text(DP_TYPE[dp_type], tx, yt - 0.72f * px - 5, wide ? 10 : 8, rgb(0xffffff), BOLD, tw);
	text(title, tx, yt, px, rgb(0xffffff), BOLD | NAMEF, tw);
	if (has) text(g->desc, tx, yd, wide ? 9 : 8, rgba(0xffffff, 170), 0, tw);
	else if (desc) text("Add a description", tx, yd, wide ? 9 : 8, rgba(0xffffff, 90), 0, tw);
	if (!wide && !ghost) {   // for a tap on the touchscreen (none unless it's yours)
		dp_tx = tx;
		dp_title_y0 = edit ? yt - 0.72f * px - 3 : 1e9f;
		dp_title_y1 = edit ? yt + 4 : -1e9f;
		dp_desc_y0 = edit ? yd - 10 : 1e9f;
		dp_desc_y1 = edit ? yd + 4 : -1e9f;
	}

	// who, then what: "brakence • 2026 • 18 songs, 1 hr 8 min"
	float x = tx, ay = ym - 4;
	const char *who = pl ? (owner_of(g) ? owner_of(g) : detail == 0 ? "You" : NULL) : g->artist;
	if (pl ? (dp_av || who) : dp_artist >= 0) {
		if (pl && dp_av) dp_img(96, 0, 32, x, ay - 7, 14);
		else if (!pl && dp_artist < MI_FACES && face_ok[dp_artist]) face(dp_artist, x, ay - 7, 14);
		else {
			const char *nm = who ? who : "?";
			C2D_DrawCircleSolid(x + 7, ay, 0, 7, rgb(name_color(nm)));
			char ini[8] = {0};
			memcpy(ini, nm, (nm[0] & 0x80) ? ((nm[0] & 0xE0) == 0xC0 ? 2 : (nm[0] & 0xF0) == 0xE0 ? 3 : 4) : 1);
			text(ini, x + 7, ay + 3, 8, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
		}
		x += 18;
	}
	char s[160], d[40];
	dp_len(d, sizeof d, dp_secs);
	if (pl && g->sp_total > g->n) {   // Spotify's whole list, as it says it
		if (g->sp_secs) dp_len(d, sizeof d, g->sp_secs);
		snprintf(s, sizeof s, "%d of %d songs, %s", g->n, g->sp_total, d);
	}
	else snprintf(s, sizeof s, "%d song%s, %s", g->n, g->n == 1 ? "" : "s", d);
	char rest[200];
	if (!pl && dp_year) snprintf(rest, sizeof rest, " \xe2\x80\xa2 %d \xe2\x80\xa2 %s", dp_year, s);
	else snprintf(rest, sizeof rest, "%s%s", who ? " \xe2\x80\xa2 " : "", s);
	float nw = 0;
	if (who) {
		float room = tw - (x - tx) - measure(rest, 9);
		nw = text(who, x, ym, 9, rgb(0xffffff), BOLD, room > 40 ? room : 40);
		if (nw > (room > 40 ? room : 40)) nw = room > 40 ? room : 40;
	}
	if (!wide && !ghost) { dp_name_x0 = tx; dp_name_x1 = x + nw; dp_name_y = ym; }   // for a tap on the touchscreen
	text(rest, x + nw, ym, 9, rgba(0xffffff, 200), 0, tw - (x + nw - tx));

	// play, shuffle, the check (albums), and more
	float ya = y0 + H, mid = ya + DP_ACT / 2;
	bool here = ai->open && !ai->paused && playing_here();
	C2D_DrawCircleSolid(28, mid, 0, 16, rgb(GREEN));
	if (here) ic_pause(28, mid, 12, rgb(0));
	else ic_play(29.5f, mid, 12, rgb(0));
	ic_shuffle(64, mid, shuffle ? rgb(GREEN) : rgb(0xc0c0c0));
	if (shuffle) C2D_DrawCircleSolid(62, mid + 11, 0, 1.3f, rgb(GREEN));
	float dx = 96;
	if (!pl) {
		bool all = g->n > 0;
		for (int k = 0; k < g->n && all; k++) all = library_is_liked(g->songs[k]);
		if (all) ic_check(dx, mid, 8);
		else {   // Spotify's "add to your library": a grey ring with a plus
			for (int a = 0; a < 40; a++) {
				float t0 = a * (float)M_PI / 20;
				C2D_DrawCircleSolid(dx + 8 * cosf(t0), mid + 8 * sinf(t0), 0, 0.75f, rgb(0xc0c0c0));
			}
			rect(dx - 3.5f, mid - 0.6f, 7, 1.2f, rgb(0xc0c0c0));
			rect(dx - 0.6f, mid - 3.5f, 1.2f, 7, rgb(0xc0c0c0));
		}
		dx += 34;
	}
	ic_dots(dx, mid, rgb(0xc0c0c0));

	// the column names, where there's room for the columns
	if (wide) {
		float yc = ya + DP_ACT, b = yc + 14;
		text("#", 22, b, 9, rgb(GREY), C2D_AlignRight, 0);
		text("Title", 34, b, 9, rgb(GREY), 0, 0);
		if (pl && g->added) text("Date added", W - 150, b, 9, rgb(GREY), 0, 0);
		C2D_DrawCircleSolid(W - 20, b - 3.5f, 0, 4.2f, rgb(GREY));   // the clock
		C2D_DrawCircleSolid(W - 20, b - 3.5f, 0, 3.2f, rgb(BG));
		rect(W - 20.4f, b - 6, 0.9f, 2.8f, rgb(GREY));
		rect(W - 20.4f, b - 3.9f, 2, 0.9f, rgb(GREY));
		rect(12, yc + dp_cols(W) - 2, W - 24, 1, rgba(0xffffff, 30));
	}
}

static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void dp_row(int k, float y, const AudioInfo *ai) {
	Group *g = dgroup();
	float W = LW;
	bool wide = W > 320, pl = dkind == T_PLAYLISTS;
	int i = items[k];
	Track *t = &lib.t[i];
	bool now = ai->open && ai->track == i;
	char s[48];
	if (now && !ai->paused)   // Spotify's little equaliser where the number was
		for (int b = 0; b < 3; b++) {
			int h = 3 + (b * 7 + (int)(osGetTime() / 90)) % 8;
			rect(12 + b * 4, y + 22 - h, 2.5f, h, rgb(GREEN));
		}
	else { snprintf(s, sizeof s, "%d", k + 1); text(s, 22, y + 21, 10, rgb(now ? GREEN : GREY), C2D_AlignRight, 0); }
	float x = 34;
	if (pl) { thumb(t->album_id, 32, y + 4, 26); x = 66; }
	float rx = W - 8, mid = wide && pl && g->added ? W - 150 : 0;
	float tw = (mid ? mid - 10 : rx - 52) - x;
	text(t->title, x, y + 15, 11, rgb(now ? GREEN : 0xffffff), 0, tw);
	float sx = x;
	if (g->expl && k < g->n && g->expl[k]) {   // the E badge
		rrect(sx, y + 20.5f, 9, 9, 1.5f, rgb(GREY));
		text("E", sx + 4.5f, y + 28, 7, rgb(BG), C2D_AlignCenter | BOLD, 0);
		sx += 12;
	}
	text(t->artist, sx, y + 28, 9, rgb(GREY), 0, tw - (sx - x));
	if (mid && k < g->n && g->added[k]) {
		u32 a = g->added[k];
		int mo = a / 100 % 100;
		snprintf(s, sizeof s, "%s %d, %d", mo >= 1 && mo <= 12 ? MON[mo - 1] : "?", (int)(a % 100), (int)(a / 10000));
		text(s, mid, y + 21, 9, rgb(GREY), 0, 0);
	}
	if (library_is_liked(i)) ic_check(rx - 40, y + 17, 6);
	char d[16];
	mmss(d, t->secs);
	text(d, rx, y + 21, 9, rgb(GREY), C2D_AlignRight, 0);
}

static void dp_footer(float W) {   // under an album: its year, as the start of Spotify's credits
	if (!nitems) {
		float y = rows_base(W) - scroll[V_DETAIL] + 28;
		text(dkind == T_PLAYLISTS && detail == 0 ? "Songs you like will appear here" : "Nothing in here yet",
		     W / 2, y, 11, rgb(GREY), C2D_AlignCenter, 0);
		return;
	}
	if (dkind != T_ALBUMS || !dp_year) return;
	float y = rows_base(W) + nitems * ROW - scroll[V_DETAIL] + 22;
	char s[16];
	snprintf(s, sizeof s, "%d", dp_year);
	text(s, 12, y, 9, rgb(GREY), 0, 0);
}

// the bar along the top: the back button, filling with the colour and the title once the
// header's title has scrolled under it
static void dp_bar(float W, const AudioInfo *ai) {
	float sc = scroll[V_DETAIL], H = dp_hero(W), f = (sc - (H - 70)) / 40;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	if (f > 0) rect(0, 0, W, LIST0, rgba(mixc(dp_col, 0, 0.5f), (u8)(255 * f)));
	rrect(6, 5, 22, 22, 11, rgba(0, 120));
	ic_back(16.5f, 16, rgb(0xffffff));
	hit(0, 32, H_BACK);
	if (f > 0.5f) {
		u8 a = (u8)(255 * (f - 0.5f) * 2);
		Group *g = dgroup();
		text(detail == 0 && dkind == T_PLAYLISTS ? "Liked Songs" : g->name, 66, 20, 12, rgba(0xffffff, a), BOLD, W - 78);
		C2D_DrawCircleSolid(48, 16, 0, 11, rgba(GREEN, a));
		bool here = ai->open && !ai->paused && playing_here();
		if (here) ic_pause(48, 16, 8, rgba(0, a));
		else ic_play(49, 16, 8, rgba(0, a));
		hit(36, 60, H_PLAY);
	}
}

// The buttons under the header, for the D-pad: play, shuffle, (an album's) the check, the dots
static int dp_nbtn(void) { return dkind == T_ALBUMS ? 4 : 3; }
static int dp_bkind(int b) { return dkind == T_ALBUMS || b < 2 ? b : 3; }   // 0 play, 1 shuffle, 2 the check, 3 the dots
static void dp_btn_box(int b, float *x, float *y, float *w, float *h, float *r) {   // on the page, the list's screen
	float mid = dp_hero(list_w()) + DP_ACT / 2, dots = dkind == T_ALBUMS ? 130 : 96;
	switch (dp_bkind(b)) {
	case 0: *x = 12; *y = mid - 16; *w = *h = 32; *r = 16; break;
	case 1: *x = 52; *y = mid - 11; *w = 24; *h = 22; *r = 7; break;
	case 2: *x = 85; *y = mid - 11; *w = *h = 22; *r = 11; break;
	default: *x = dots - 12; *y = mid - 9; *w = 24; *h = 18; *r = 9; break;
	}
}
static void dp_press(int b, const AudioInfo *ai) {
	switch (dp_bkind(b)) {
	case 0:
		if (ai->open && playing_here()) audio_pause(!ai->paused);
		else play_list(shuffle && nitems ? rand() % nitems : 0);
		break;
	case 1:
		set_shuffle(!shuffle);
		say(shuffle ? "Shuffle on" : "Shuffle off");
		break;
	case 2: {
		Group *g = dgroup();
		bool on = library_like_many(g->songs, g->n);
		say(on ? "Album added to Liked Songs" : "Album removed from Liked Songs");
		dirty = true;
		spot_request(3000, false, false);
		break;
	}
	default:   // the dots: their menu (pagemenu.inc)
		pm_open();
	}
}

// A tap on the page above the rows (the touchscreen is 320 wide); true if it was there
static bool dp_tap(int x, int y, const AudioInfo *ai) {
	float W = 320, ya = dp_hero(W) - scroll[V_DETAIL];
	if (y >= ya && y < ya + DP_ACT) {
		float dots = dkind == T_ALBUMS ? 130 : 96;
		if (x < 46) dp_press(0, ai);
		else if (x < 80) set_shuffle(!shuffle);
		else if (x < 114 && dkind == T_ALBUMS) dp_press(2, ai);
		else if (x >= dots - 16 && x < dots + 16) dp_press(dp_nbtn() - 1, ai);
		return true;
	}
	if (y < ya) {   // the artist's name, under the title: their page
		if (dkind == T_PLAYLISTS && x >= dp_tx - 4 && y >= dp_title_y0 && y <= dp_title_y1) le_edit(false);
		else if (dkind == T_PLAYLISTS && x >= dp_tx - 4 && y >= dp_desc_y0 && y <= dp_desc_y1) le_edit(true);
		else if (dkind == T_ALBUMS && dp_artist >= 0 && dp_artist_exact && x >= dp_name_x0 - 4 && x <= dp_name_x1 + 4 && y >= dp_name_y - 14 && y <= dp_name_y + 6) {
			int a = dp_artist;
			close_detail();
			ap_clear();   // a fresh stack, as the player's artist name does
			ap_open_local(a);
		}
		return true;
	}
	return false;
}
