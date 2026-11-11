// The side menu the three bars open (Library, Charts, Settings), and the Settings page. Included
// by main.c ahead of artistpage.inc. The menu slides in from the left over the list's screen; a
// tap, or the D-pad and A, picks a page; B, right, a tap outside it or a swipe left puts it away.

#define RESCAN "sdmc:/3ds/player/rescan"   // there: read every tag again at the next launch

static bool drawer_on;
static bool drawer_flip;   // it came out or went away: a finger on the glass then is the menu's, till it lifts
static bool g_audio_ok;    // main's audio_ok, for the Settings page
static int rescan_set = -1;   // the flag file, as last looked at (-1: not yet)
static float drawer_t;   // how far out it is, 0 to 1, eased toward drawer_on
static int drawer_cur;
static const char *DRAWER[] = {"Library", "Charts", "Pair", "Settings"};
#define NDRAW 4

static int drawer_page(void) { return pair_on && !cc_page ? 2 : detail < 0 && tab == T_STATS ? 1 : detail < 0 && tab == T_SETTINGS ? 3 : 0; }
static void drawer_open(bool on) {
	if (on && !drawer_on) { drawer_cur = drawer_page(); audio_tick(TICK_GRAB); }
	if (on != drawer_on) drawer_flip = true;
	drawer_on = on;
	vel = 0;   // the list behind it stops where it is
}
static void drawer_pick(int k) {
	drawer_open(false);
	audio_tick(TICK_LAND);
	if (pair_on && !cc_page) {   // picked on the Pair page: it stays if it's the one picked, else it goes for the page
		if (k == 2) return;
		pair_leave();
	}
	if (k == 0) {   // back where the menu's pages were opened from; from Home, the tabs (Songs)
		if (detail < 0 && tab == T_HOME) page_to(T_SONGS);
		else if (detail < 0 && tab >= T_STATS) page_to(stats_from);
	}
	else if (k == 2) pair_open();   // its own page, over both screens
	else page_to(k == 1 ? T_STATS : T_SETTINGS);
}
static float drawer_w(float W) { return W > 320 ? 200 : 176; }

// ---- the icons -------------------------------------------------------------------------------

static void ic_library(float x, float y, u32 c) {   // books on a shelf, the last one leaning against the second
	rect(x - 7, y - 6, 2.2f, 12, c);
	rect(x - 3, y - 6, 2.2f, 12, c);
	// the leaning book: 20 degrees, turned about its bottom-left corner on the shelf, its top just touching the one beside it
	const float bx = x + 4.6f, by = y + 6.4f, s = 0.342f, k = 0.940f, h = 12.4f, w = 2.2f;   // sin, cos of 20 degrees
	float tlx = bx - s * h, tly = by - k * h;   // top-left
	C2D_DrawTriangle(bx, by, c, tlx, tly, c, tlx + w * k, tly - w * s, c, 0);
	C2D_DrawTriangle(bx, by, c, tlx + w * k, tly - w * s, c, bx + w * k, by - w * s, c, 0);
}
static void ic_charts(float x, float y, u32 c) {   // three bars
	rect(x - 7, y + 1, 3, 6, c);
	rect(x - 1.5f, y - 6, 3, 13, c);
	rect(x + 4, y - 2, 3, 9, c);
}
static void ic_pair(float x, float y, u32 c) {   // two arrows, one over the other, going opposite ways: an exchange
	C2D_DrawLine(x - 8, y - 4, c, x + 4.5f, y - 4, c, 1.8f, 0);
	C2D_DrawTriangle(x + 3, y - 7.4f, c, x + 8, y - 4, c, x + 3, y - 0.6f, c, 0);
	C2D_DrawLine(x + 8, y + 4, c, x - 4.5f, y + 4, c, 1.8f, 0);
	C2D_DrawTriangle(x - 3, y + 0.6f, c, x - 8, y + 4, c, x - 3, y + 7.4f, c, 0);
}
static void ic_gear(float x, float y, u32 c, u32 bg) {
	for (int k = 0; k < 8; k++) {
		float a = k * (float)M_PI / 4;
		C2D_DrawCircleSolid(x + 6 * cosf(a), y + 6 * sinf(a), 0, 1.9f, c);
	}
	C2D_DrawCircleSolid(x, y, 0, 6, c);
	C2D_DrawCircleSolid(x, y, 0, 2.6f, rgb(bg));
}

// ---- drawing ---------------------------------------------------------------------------------

static void draw_drawer(float W, float H) {
	if (drawer_t < 0.005f) return;
	float e = 1 - (1 - drawer_t) * (1 - drawer_t), pw = drawer_w(W), x0 = -pw * (1 - e);
	rect(0, 0, W, H, rgba(0, (u8)(150 * e)));   // the page behind, dimmed
	rect(x0, 0, pw, H, rgb(0x181818));
	rect(x0 + pw, 0, 1, H, rgba(0xffffff, 25));
	// the name as the README writes it, a green 3; both halves in one font, whichever has the whole name
	u32 nf = BOLD | (nf_has("3DStereo") ? NAMEF : 0);
	float nx = x0 + 16;
	nx += text("3", nx, 38, 18, rgb(GREEN), nf, 0);
	text("DStereo", nx, 38, 18, rgb(0xffffff), nf, 0);
	int at = drawer_page();
	for (int k = 0; k < NDRAW; k++) {
		float y = 62 + k * 38;
		bool here = k == at, foc = k == drawer_cur;
		if (foc) rrect(x0 + 8, y, pw - 16, 32, 6, rgba(0xffffff, here ? 30 : 18));
		u32 c = rgb(here ? GREEN : 0xffffff);
		if (k == 0) ic_library(x0 + 30, y + 16, c);
		else if (k == 1) ic_charts(x0 + 30, y + 16, c);
		else if (k == 2) ic_pair(x0 + 30, y + 16, c);
		else ic_gear(x0 + 30, y + 16, c, foc ? 0x2c2c2c : 0x181818);
		text(DRAWER[k], x0 + 50, y + 21, 12, c, BOLD, 0);
	}
}

// ---- input -----------------------------------------------------------------------------------

static void drawer_keys(u32 kd, u32 kr) {
	if (kr & KEY_DUP) drawer_cur = (drawer_cur + NDRAW - 1) % NDRAW;
	if (kr & KEY_DDOWN) drawer_cur = (drawer_cur + 1) % NDRAW;
	if (kd & KEY_A) drawer_pick(drawer_cur);
	else if (kd & (KEY_B | KEY_DRIGHT)) drawer_open(false);
}

// a tap while it's out (the touchscreen is 320 wide): an item, or anywhere else to put it away
static void drawer_tap(int x, int y) {
	if (x < drawer_w(320)) {
		for (int k = 0; k < NDRAW; k++)
			if (y >= 62 + k * 38 && y < 94 + k * 38) { drawer_pick(k); return; }
		return;
	}
	drawer_open(false);
}

// ---- the Settings page -----------------------------------------------------------------------

static const char *QLABEL[] = {"Stock", "Good", "Best"};
static const char *QSUB[] = {"System resampler \xc2\xb7 32.7 kHz", "System resampler \xc2\xb7 47.6 kHz", "Our resampler \xc2\xb7 47.6 kHz"};
static const char *RGSUB[] = {"Off", "Match songs", "Match albums", "Albums in order, else songs"};

static u64 set_flash[8];   // when a row was last pressed: the flash that says it took
static float swk_v[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // each switch's knob, 0 off to 1 on, easing

// a switch, Spotify's: the knob slides and the green fades in
static void draw_switch(float x, float y, int k, bool on) {
	float to = on ? 1 : 0;
	if (swk_v[k] < 0) swk_v[k] = to;
	swk_v[k] += (to - swk_v[k]) * 0.3f;
	if (fabsf(to - swk_v[k]) < 0.01f) swk_v[k] = to;
	float v = swk_v[k];
	rrect(x, y + 10, 26, 14, 7, rgb(mixc(GREEN, 0x535353, v)));
	C2D_DrawCircleSolid(x + 7 + 12 * v, y + 17, 0, 5, rgb(mixc(0, 0xd0d0d0, v)));
}

// a one-time thing, not a switch: a (+) in a ring to ask for it, Spotify's green check once it's queued;
// it pops a little when pressed
static void draw_once(float x, float y, bool on, u64 pressed) {
	u64 now = osGetTime();
	float pop = pressed && now - pressed < 250 ? 1 - (now - pressed) / 250.0f : 0, k = 1 + 0.35f * pop;
	float cx = x + 13, cy = y + 17;
	if (on) { ic_check(cx, cy, 9 * k); return; }
	u32 c = rgb(0xe8e8e8);
	for (int a = 0; a < 24; a++) {
		float t0 = a * (float)M_PI / 12, t1 = (a + 1) * (float)M_PI / 12;
		C2D_DrawLine(cx + 9 * k * cosf(t0), cy + 9 * k * sinf(t0), c, cx + 9 * k * cosf(t1), cy + 9 * k * sinf(t1), c, 1.5f, 0);
	}
	rect(cx - 4.5f * k, cy - 0.75f, 9 * k, 1.5f, c);
	rect(cx - 0.75f, cy - 4.5f * k, 1.5f, 9 * k, c);
}

// a band of light across [lo, hi]: clear at its two ends, alpha a in the middle
static void upd_sheen(float bx, float bw, float lo, float hi, float top, float h, float a) {
	float xs[3] = {bx, bx + bw / 2, bx + bw}, as[3] = {0, a, 0};
	for (int i = 0; i < 2; i++) {
		float l = fmaxf(xs[i], lo), r = fminf(xs[i + 1], hi);
		if (r <= l) continue;
		float al = as[i] + (as[i + 1] - as[i]) * (l - xs[i]) / (xs[i + 1] - xs[i]);
		float ar = as[i] + (as[i + 1] - as[i]) * (r - xs[i]) / (xs[i + 1] - xs[i]);
		u32 cl = rgba(GREEN, (u8)al), cr = rgba(GREEN, (u8)ar);
		C2D_DrawRectangle(l, top, 0, r - l, h, cl, cr, cl, cr);
	}
}

// four little level bars beside the row's end; lv 0..1 is how alive they are, slow for the install
static void upd_bars(float W, float y, u64 now, float lv, bool slow, u8 a) {
	static const float sp[4] = {1.0f, 1.37f, 0.83f, 1.19f}, ph[4] = {0, 1.7f, 3.1f, 4.4f};
	for (int i = 0; i < 4; i++) {
		float v = 0.5f + 0.5f * sinf(now / (slow ? 210.0f : 90.0f) * sp[i] + ph[i]);
		float hh = 3 + (slow ? 4 : 9) * v * lv;
		rect(W - 36 + i * 4.5f, y + 25 - hh, 3, hh, rgba(GREEN, a));
	}
}

// Settings > Update, the row as a meter: a sheen while it looks, green filling as it downloads, a pulse while it
// installs, a check when it's in, and on a failure the green drains back out. True: show the chevron.
static bool upd_row(float W, float y, u64 now) {
	static int seen = -1;
	static u64 since;
	static float fill, from;   // the green's width, 0..1, eased toward the percent; from: where a failure drains from
	int pc, st = upd_state(&pc);
	if (st != seen) {
		if (st == U_CHECK) fill = 0;
		if (st == U_FAIL) from = fill;
		seen = st;
		since = now;
	}
	float age = (float)(now - since), x0 = 0, top = y, w = W, h = ROW;   // the whole row, edge to edge like the cursor bar
	switch (st) {
	case U_CHECK:
		upd_sheen(x0 - 70 + (now % 1100) / 1100.0f * (w + 70), 70, x0, x0 + w, top, h, 40);
		return false;
	case U_GET:
		if (pc < 0) { upd_sheen(x0 - 70 + (now % 1100) / 1100.0f * (w + 70), 70, x0, x0 + w, top, h, 40); return false; }
		fill += (pc / 100.0f - fill) * 0.15f;
		rect(x0, top, fill * w, h, rgba(GREEN, 36));
		upd_bars(W, y, now, 1, false, 220);
		return false;
	case U_FINISH:
		fill = 1;
		rect(x0, top, w, h, rgba(GREEN, (u8)(38 + 18 * sinf(now / 260.0f))));
		upd_bars(W, y, now, 1, true, 220);
		return false;
	case U_DONE: {
		fill = 1;
		float e = clamp01(age / 600), a = 18 + 28 * (1 - e) * (1 - e);
		if (upd_bye) a += (82 - a) * clamp01((now - upd_bye) / 500.0f);   // the goodbye: the row lights up as it goes
		rect(x0, top, w, h, rgba(GREEN, (u8)a));
		float p = clamp01(age / 300);
		ic_check(W - 20, y + 17, 9 * (p + 0.35f * sinf(p * (float)M_PI)));
		return false;
	}
	case U_FAIL: {
		float p = clamp01(age / 700), e = 1 - (1 - p) * (1 - p) * (1 - p);
		fill = from * (1 - e);
		if (fill * w >= 1) rect(x0, top, fill * w, h, rgba(mixc(0x555555, GREEN, p), 36));
		if (p < 1 && from > 0) upd_bars(W, y, now, 1 - p, false, (u8)(220 * (1 - p)));
		return age > 600;
	}
	default: return true;   // idle, or already the newest
	}
}

static void draw_set_row(int k, float y) {
	float W = LW;
	char t[64], s[96];
	bool chev = false, busy = false;
	u64 now = osGetTime();
	if (rescan_set < 0) { struct stat st; rescan_set = !stat(RESCAN, &st); }
	if (k < 8 && set_flash[k] && now - set_flash[k] < 400) rect(0, y, W, ROW, rgba(0xffffff, (u8)(45 * (400 - (now - set_flash[k])) / 400)));
	switch (k) {
	case 0: snprintf(t, sizeof t, "Sound quality: %s", QLABEL[quality]); snprintf(s, sizeof s, "%s", QSUB[quality]); break;
	case 1: snprintf(t, sizeof t, "Normalize volume"); snprintf(s, sizeof s, "%s", RGSUB[rg_cfg]); break;   // ReplayGain: the files' own loudness tags
	case 2:
		busy = spot_busy() || (set_flash[2] && now - set_flash[2] < 700);
		snprintf(t, sizeof t, "Sync Spotify");
		snprintf(s, sizeof s, "%s", busy ? "Syncing" : spot_paired() ? "Playlists and likes" : "Not paired");
		chev = !busy;
		break;
	case 3:
		snprintf(t, sizeof t, "Rescan library");
		snprintf(s, sizeof s, "%s", rescan_set > 0 ? "Queued for next launch" : "Reread every tag, once");
		break;
	case 4:
		snprintf(t, sizeof t, "Extra details");
		snprintf(s, sizeof s, "%s", "File format on the player");
		break;
	case 5:
		snprintf(t, sizeof t, "Name on Spotify");
		snprintf(s, sizeof s, "%s", dev_name);
		chev = true;
		break;
	case 6:
		snprintf(t, sizeof t, "Clear cache");
		snprintf(s, sizeof s, "Covers, photos, lyrics");
		chev = true;
		break;
	case 7:
		snprintf(t, sizeof t, "Update");
		upd_line(s, sizeof s);
		chev = upd_row(W, y, now);
		break;
	default: snprintf(t, sizeof t, "About"); snprintf(s, sizeof s, "v%s · Lii-lac", VERSION); break;
	}
	text(t, 16, y + 15, 11, rgb(0xffffff), 0, W - 50);
	text(s, 16, y + 28, 9, busy ? rgba(GREEN, (u8)(150 + 105 * sinf(now / 180.0f))) : rgb(GREY), 0, W - 50);
	if (busy) {   // the spinner: a ring of dots, the bright one going round
		int head = (int)(now / 70) % 8;
		for (int d = 0; d < 8; d++) {
			float a = d * (float)M_PI / 4;
			int age = (head - d + 8) % 8;
			C2D_DrawCircleSolid(W - 20 + 6 * cosf(a), y + 17 + 6 * sinf(a), 0, 1.5f, rgba(GREEN, (u8)(255 - 200 * age / 7)));
		}
	}
	if (k == 3) draw_once(W - 40, y, rescan_set > 0, set_flash[3]);
	else if (k == 4) draw_switch(W - 40, y, 4, extra_details);
	else if (k <= 1) {   // the arrows: left and right change it
		C2D_DrawTriangle(W - 34, y + 17, rgb(GREY), W - 29, y + 13, rgb(GREY), W - 29, y + 21, rgb(GREY), 0);
		C2D_DrawTriangle(W - 14, y + 17, rgb(GREY), W - 19, y + 13, rgb(GREY), W - 19, y + 21, rgb(GREY), 0);
	} else if (chev) C2D_DrawTriangle(W - 18, y + 13, rgb(GREY), W - 18, y + 21, rgb(GREY), W - 13, y + 17, rgb(GREY), 0);
}

static void le_trim(char *s);   // listedit.inc
// A or a tap (dir 1), or left and right (-1, 1), on a row of the Settings page
static void set_activate(int k, int dir) {
	if (k == 2 || k == 3 || k == 5 || k == 6 || k == 7) set_flash[k] = osGetTime();
	switch (k) {
	case 0:
		if (!g_audio_ok) { say("The sound didn't start"); break; }
		quality = (quality + Q_COUNT + dir) % Q_COUNT;
		audio_quality(quality);
		say_quality(quality);
		break;
	case 1:
		rg_cfg = (rg_cfg + RG_AUTO + 1 + dir) % (RG_AUTO + 1);
		rg_sync();
		say(RGNAME[rg_cfg]);
		break;
	case 2:
		if (dir < 0) break;
		if (spot_busy()) say("Sync queued");
		spot_request(0, true, true);
		break;
	case 5: {   // what Spotify's device list calls this 3DS
		if (dir < 0) break;
		SwkbdState kb;
		char buf[64];
		swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 40);
		swkbdSetHintText(&kb, "What Spotify calls this 3DS");
		swkbdSetInitialText(&kb, dev_name);
		swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
		if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM) break;
		for (char *p = buf; *p; p++) if ((u8)*p < 0x20) *p = ' ';
		le_trim(buf);
		if (!buf[0] || !strcmp(buf, dev_name)) break;
		snprintf(dev_name, sizeof dev_name, "%s", buf);
		connect_set_name(dev_name);
		save_cfg();
		say("Saved");
		break;
	}
	case 4:
		extra_details = !extra_details;
		break;
	case 6:
		if (dir < 0) break;
		if (!g_audio_ok) { say("The sound didn't start"); break; }
		cc_open();
		break;
	case 7:   // Update: one press checks GitHub and installs a newer release; once one's in, the next opens it
		if (dir < 0) break;
		if (upd_done()) { if (!upd_bye) { upd_mark(); upd_bye = osGetTime(); } }
		else if (upd_busy()) say("Already updating");
		else upd_start();
		break;
	case 3: {
		struct stat st;
		if (!stat(RESCAN, &st)) remove(RESCAN);
		else {
			FILE *f = fopen(RESCAN, "w");
			if (f) fclose(f);
		}
		rescan_set = -1;
		break;
	}
	}
}
