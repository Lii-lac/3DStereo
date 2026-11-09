// The Pair page (the side menu's "Pair"): pairing with Spotify on the 3DS itself, over both screens. Spotify's
// device-code login, the way a TV does it (connect.c): the 3DS asks for a code, shows it, the person types it at
// spotify.com/pair on a phone or a PC, and the 3DS (asking every few seconds) is handed the login.
// The page is a ticket: the code is typed onto it, a row of punched holes is the clock, and when it's paired the ticket
// tears at its perforation and the stub (the exchange mark on green) falls through the hinge onto the touchscreen.
// A failure says why in plain words (pair_words). B: back. A or a tap on the button: a new code.
static bool pair_on;
// The side menu (drawer.inc) is on this page too (build 99): the three bars at the top left of the list's screen (up top
// while the screens are swapped, else the touchscreen), ZL or a tap opens it, ZR or B puts it away, and a page picked
// from it takes the place of this one.
static bool drawer_on;
static void drawer_open(bool on);
static void drawer_keys(u32 kd, u32 kr);
static void drawer_tap(int x, int y);
static void draw_drawer(float W, float H);
static float menu_chip(float x, bool on);
static bool rec_hits;
static bool cc_page;         // the page up (or leaving) is the Clear cache page, not this one: clearview.inc
static void cc_step(u32 kd);
static void draw_clear(void);
static int p_last = -1;      // the mode last drawn, for the timers
static u64 p_t0;             // when it began
static u64 pair_ok_t;
static int p_total;          // the code's life in seconds, from its first sighting
static bool p_linked;        // opened while Connect is already up
// Coming and going: the page doesn't cut in. The screen it's leaving dips into the background colour, the page rises out of
// it (the ticket settling up from a little below), and the same way back to the library. pt_dir: 1 in, -1 out, 0 neither.
static int pt_dir;
static u64 pt_t0;
static int p_force = -1;     // while it leaves: the mode it was in, drawn from the status it had (the page is no longer asking)
static ConnectPair p_snap;
static float p_dy;           // how far below its place the ticket is while it rises in
#define PT_OUT 160.0f        // ms the leaving screen takes to go
#define PT_IN 320.0f         // ms the arriving one takes to come

static void pair_close(int mode, const ConnectPair *st) {
	p_force = mode;
	p_snap = *st;
	pair_on = false;
	pt_dir = -1;
	pt_t0 = osGetTime();
}

static void pair_open(void) {
	cc_page = false;
	pair_on = true;
	pt_dir = 1;
	pt_t0 = osGetTime();
	p_force = -1;
	pair_ok_t = 0;
	p_last = -1;
	p_total = 0;
	p_linked = connect_online() && !connect_login_dead();
	if (p_linked) return;                       // already linked: show it, a tap pairs again
	if (net_clock_year()) return;               // no code can be had with that clock: the page says so
	if (!net_wifi()) return;                    // ...nor without Wi-Fi
	connect_pair_start();
}
static void pair_again(void) {
	pair_ok_t = 0;
	p_last = -1;
	p_total = 0;
	p_linked = false;
	if (net_clock_year() || !net_wifi()) return;
	connect_pair_start();
}
// what the page is in: 0 linked, 1 asking, 2 code, 3 paired, 4 a failure
static int pair_mode(const ConnectPair *st) {
	if (p_linked) return 0;
	if (net_clock_year() || !net_wifi()) return 4;
	return st->state == 0 ? 1 : st->state;
}
// Off to another page from the side menu: the same goodbye as Back
static void pair_leave(void) {
	ConnectPair st;
	connect_pair_status(&st);
	int mode = pair_mode(&st);
	connect_pair_cancel();
	pair_close(mode, &st);
}
static void pair_step(u32 kd, u32 kr) {   // every frame while the page is up; the buttons and the touchscreen are its own
	if (cc_page) { cc_step(kd); return; }
	ConnectPair st;
	connect_pair_status(&st);
	int mode = pair_mode(&st);
	if (drawer_on) {   // the side menu is out over the page: it has the buttons, and the touchscreen unless the screens are swapped
		if (mode == 3) drawer_open(false);   // the code was typed while it was out: let the ticket tear in peace
		else {
			if (kd & KEY_ZR) drawer_open(false);
			else {
				drawer_keys(kd, kr);
				if (kd & KEY_TOUCH && !swapped) {
					touchPosition tp;
					hidTouchRead(&tp);
					drawer_tap(tp.px, tp.py);
				}
			}
			return;
		}
	}
	if (mode != 3 && kd & KEY_ZL) { drawer_open(true); return; }   // left of the page is the menu
	bool again = false, back = kd & KEY_B;
	if (kd & KEY_A && (mode == 0 || mode == 2 || mode == 4)) again = true;
	if (kd & KEY_TOUCH) {
		touchPosition tp;
		hidTouchRead(&tp);
		if (!swapped && mode != 3 && tp.px < 36 && tp.py < 34) { drawer_open(true); return; }   // the three bars
		if (tp.py >= 196 && tp.px < 120) back = true;
		else if (tp.py >= 196 && tp.px >= 200 && mode != 1 && mode != 3) again = true;
	}
	if (back) { connect_pair_cancel(); pair_close(mode, &st); return; }
	if (again) { connect_pair_cancel(); pair_again(); return; }
	if (mode == 3) {   // paired: the ticket tears and the stub lands; then the Connect thread starts with the new login
		if (!pair_ok_t) pair_ok_t = osGetTime();
		if (osGetTime() - pair_ok_t > 4200) {
			pair_close(mode, &st);
			pair_ok_t = 0;
			connect_start();
			say("Paired. Look for it in Spotify's devices");
		}
	}
}

static float p_ease(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return 1 - (1 - t) * (1 - t) * (1 - t); }
static float p_easeio(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2; }
// Every frame the page is up or on its way out: how much of the background colour is over both screens (0..255), and
// which page to draw under it, the pair page or the library.
static void pair_trans_step(int *veil, bool *pair_page) {
	*veil = 0;
	p_dy = 0;
	*pair_page = pair_on;
	if (!pt_dir) return;
	float e = (float)(osGetTime() - pt_t0);
	if (e < PT_OUT) {   // the screen that's leaving goes, slowly at first
		float k = e / PT_OUT;
		*veil = (int)(255 * k * k);
		*pair_page = pt_dir < 0;
		return;
	}
	float k = (e - PT_OUT) / PT_IN;
	if (k >= 1) { pt_dir = 0; p_force = -1; *pair_page = pair_on; return; }
	*veil = (int)(255 * (1 - p_ease(k)));
	*pair_page = pt_dir > 0;
	if (pt_dir > 0) p_dy = (1 - p_ease(k)) * 18;
}
static void pair_veil(int a) {
	if (a <= 0) return;
	C2D_SceneBegin(top);
	rect(0, 0, 400, 240, rgba(BG, (u8)(a > 255 ? 255 : a)));
	C2D_SceneBegin(bot);
	rect(0, 0, 320, 240, rgba(BG, (u8)(a > 255 ? 255 : a)));
}
static void p_arc(float cx, float cy, float r, float a0, float a1, float w, u32 c) {   // clockwise from a0, radians
	int n = (int)(fabsf(a1 - a0) * r / 2.5f) + 2;
	for (int i = 0; i < n; i++) {
		float t0 = a0 + (a1 - a0) * i / n, t1 = a0 + (a1 - a0) * (i + 1) / n;
		C2D_DrawLine(cx + r * cosf(t0), cy + r * sinf(t0), c, cx + r * cosf(t1), cy + r * sinf(t1), c, w, 0);
	}
}
static void p_exch(float x, float y, float s, u32 c) {   // the exchange mark: one arrow over another, opposite ways
	C2D_DrawLine(x - 8 * s, y - 4 * s, c, x + 4.5f * s, y - 4 * s, c, 1.8f * s, 0);
	C2D_DrawTriangle(x + 3 * s, y - 7.4f * s, c, x + 8 * s, y - 4 * s, c, x + 3 * s, y - 0.6f * s, c, 0);
	C2D_DrawLine(x + 8 * s, y + 4 * s, c, x - 4.5f * s, y + 4 * s, c, 1.8f * s, 0);
	C2D_DrawTriangle(x - 3 * s, y + 0.6f * s, c, x - 8 * s, y + 4 * s, c, x - 3 * s, y + 7.4f * s, c, 0);
}
#define TK_W 300
#define TK_H 96
#define TK_STUB 74
#define P_CREAM 0xf2e6cf
// The ticket's body, centred at (cx, cy), the code typed on it (typed: letters shown, a fraction past the count is the
// next one rising in), its left edge squared where the stub tears away. a: how visible, 0..255.
static void tk_body(float cx, float cy, const char *code, float typed, u8 a, bool placeholder, float t) {
	float x0 = cx - TK_W / 2 + TK_STUB, w = TK_W - TK_STUB, y0 = cy - TK_H / 2;
	rrect(x0, y0, w, TK_H, 10, rgba(P_CREAM, a));
	rect(x0, y0, 14, TK_H, rgba(P_CREAM, a));   // the torn side is square
	text("ADMIT ONE", x0 + 18, y0 + 18, 8, rgba(0x6d6450, a), BOLD, 0);
	int n = code && code[0] ? (int)strlen(code) : 6;
	for (int i = 0; i < n; i++) {
		float px = x0 + 34 + i * 32;
		if (placeholder) {   // asking: six dashes, breathing
			float br = 0.5f + 0.5f * sinf(t / 260.0f + i * 0.5f);
			rrect(px - 9, y0 + 56, 18, 4, 2, rgba(0x6d6450, (u8)(a * (0.25f + 0.35f * br))));
			continue;
		}
		float k = typed - i;
		k = k < 0 ? 0 : k > 1 ? 1 : k;
		float e = p_ease(k);
		char b[2] = {code[i], 0};
		text(b, px, y0 + 66 + (1 - e) * 8, 40, rgba(0x121212, (u8)(a * e)), C2D_AlignCenter | BOLD, 0);
	}
}
// The stub: a rounded tab with the exchange mark in a circle (green once paired), squared where it tears.
static void tk_stub(float cx, float cy, bool green, u8 a) {   // centre of the stub
	float x0 = cx - TK_STUB / 2, y0 = cy - TK_H / 2;
	rrect(x0, y0, TK_STUB, TK_H, 10, rgba(P_CREAM, a));
	rect(x0 + TK_STUB - 14, y0, 14, TK_H, rgba(P_CREAM, a));
	C2D_DrawCircleSolid(cx - 4, cy, 0, 25, green ? rgba(GREEN, a) : rgba(0xe2d5bb, a));
	p_exch(cx - 4, cy, 1.5f, green ? rgba(0x121212, a) : rgba(0x9a8f78, a));
}
static void tk_perf(float cx, float cy, float gap, u8 a) {   // the perforation, between the stub and the body
	float x = cx - TK_W / 2 + TK_STUB;
	for (float y = cy - TK_H / 2 + 5; y < cy + TK_H / 2 - 4; y += 7) rect(x - 0.7f + gap, y, 1.4f, 3.4f, rgba(0xb9ad92, a));
}
static void tk_holes(float f, bool alive_all, u8 a) {   // twelve punched holes: the clock; one more gone each stretch
	for (int i = 0; i < 12; i++) {
		bool on = alive_all || i < 12 * (1 - f);
		C2D_DrawCircleSolid(88 + i * 20.3f, 190 + p_dy, 0, 3.6f, rgba(on ? P_CREAM : 0xffffff, (u8)((on ? 205 : 30) * a / 255)));
	}
}

// Why it failed, in plain words: a title and two lines of what to do about it.
static void pair_words(const ConnectPair *st, char *title, size_t tn, const char **l1, const char **l2) {
	int k = net_clock_year() ? PZ_CLOCK : !net_wifi() ? PZ_NOWIFI : st->kind;
	*l1 = *l2 = "";
	switch (k) {
	case PZ_NOWIFI:
		snprintf(title, tn, "Wi-Fi is off");
		*l1 = "Turn it on, then try again.";
		break;
	case PZ_CLOCK: {
		int y = net_clock_year();
		if (y) snprintf(title, tn, "Your 3DS clock says %d", y);
		else if (st->skew_days > 1) snprintf(title, tn, "Your 3DS clock is %d days off", st->skew_days);
		else snprintf(title, tn, "Your 3DS clock is wrong");
		*l1 = "Spotify won't talk to a clock that far off.";
		*l2 = "System Settings, Other Settings, Date & Time.";
		break;
	}
	case PZ_DNS:
		snprintf(title, tn, "Can't find Spotify");
		*l1 = "The Wi-Fi isn't reaching the internet.";
		*l2 = "Check the router, or try another network.";
		break;
	case PZ_CONNECT:
		snprintf(title, tn, "Can't connect to Spotify");
		*l1 = "Something is blocking it: a firewall,";
		*l2 = "or a school or hotel network?";
		break;
	case PZ_TIMEOUT:
		snprintf(title, tn, "Spotify took too long");
		*l1 = "The Wi-Fi is weak or slow.";
		*l2 = "Move closer to the router and try again.";
		break;
	case PZ_CERT:
		snprintf(title, tn, "Spotify's certificate wasn't trusted");
		*l1 = "Usually the 3DS clock. If that's right,";
		*l2 = "delete sdmc:/3ds/player/cacert.pem and reopen.";
		break;
	case PZ_DROPPED:
		snprintf(title, tn, "The connection dropped");
		*l1 = "That happens on shaky Wi-Fi.";
		*l2 = "Try again.";
		break;
	case PZ_BUSY:
		snprintf(title, tn, "Spotify says slow down");
		*l1 = "Too many tries. Wait a minute,";
		*l2 = "then ask for a new code.";
		break;
	case PZ_SERVER:
		snprintf(title, tn, "Spotify is having trouble");
		*l1 = "That's on their side.";
		*l2 = "Try again in a few minutes.";
		break;
	case PZ_REFUSED:
		snprintf(title, tn, "Spotify turned this login down");
		*l1 = "It may have stopped allowing it.";
		*l2 = "The details are in home.log.";
		break;
	case PZ_EXPIRED:
		snprintf(title, tn, "That code ran out");
		*l1 = "Codes last about an hour.";
		*l2 = "Ask for a new one.";
		break;
	case PZ_DENIED:
		snprintf(title, tn, "Spotify said no");
		*l1 = "The request was declined on its page.";
		*l2 = "Ask for a new code.";
		break;
	case PZ_PORTAL:
		snprintf(title, tn, "Your Wi-Fi is in the way");
		*l1 = "A login page is answering instead of Spotify.";
		*l2 = "Sign in to the network on a phone first.";
		break;
	case PZ_SAVE:
		snprintf(title, tn, "Couldn't save the login");
		*l1 = "The SD card wouldn't take it.";
		*l2 = "Is it locked, or full?";
		break;
	default:
		snprintf(title, tn, "That didn't work");
		*l1 = "Try again in a moment.";
		break;
	}
}

static void p_button(float x, float y, float w, const char *label, bool primary) {
	rrect(x, y, w, 26, 13, primary ? rgb(GREEN) : rgba(0xffffff, 28));
	text(label, x + w / 2, y + 17.5f, 11, primary ? rgb(0) : rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
}

static void draw_pair(void) {
	ConnectPair st;
	connect_pair_status(&st);
	int mode = pair_mode(&st);
	if (p_force >= 0) { mode = p_force; st = p_snap; }   // leaving: it goes as it was
	u64 now = osGetTime();
	float t = (float)now;
	if (mode != p_last) { p_last = mode; p_t0 = now; }
	float el = (now - p_t0) / 1000.0f;   // seconds in this mode
	if (mode == 2 && !p_total) p_total = st.secs_left > 0 ? st.secs_left : 600;
	float frac = mode == 2 && p_total > 0 ? (float)st.secs_left / p_total : 0;
	float sway = sinf(t / 900.0f) * 1.5f;   // the ticket hangs: it moves a hair, nothing else does

	// ---------------- the top screen ----------------
	C2D_TargetClear(top, rgb(BG));
	C2D_SceneBegin(top);
	if (mode == 1 || mode == 2) {
		bool code = mode == 2 && st.code[0];
		if (code) {
			text("Open", 148, 34, 12, rgba(0xffffff, 190), C2D_AlignRight, 0);
			text(st.url, 154, 34, 12, rgb(GREEN), BOLD, 0);
			text("and type", 154 + measure(st.url, 12) + 5, 34, 12, rgba(0xffffff, 190), 0, 0);
		} else text("Asking Spotify", 200, 34, 12, rgba(0xffffff, 170), C2D_AlignCenter, 0);
		float cy = 112 + sway + p_dy;
		tk_stub(200 - TK_W / 2 + TK_STUB / 2, cy, false, 255);
		tk_body(200, cy, code ? st.code : NULL, el * 1000.0f / 110.0f, 255, !code, t);
		tk_perf(200, cy, 0, 255);
		tk_holes(code ? 1 - frac : 0, !code, 255);
		if (code) {
			char b[16];
			snprintf(b, sizeof b, "%d:%02d", st.secs_left / 60, st.secs_left % 60);
			text(b, 200, 214, 10, rgba(0xffffff, 110), C2D_AlignCenter, 0);
		}
	} else if (mode == 3) {
		float u = el / 3.2f;
		float tear = p_easeio((u - 0.05f) / 0.2f), drop = p_easeio((u - 0.3f) / 0.5f);
		float cy = 112, sy = drop * 330;
		float fade = 1 - (u - 0.15f) / 0.35f;
		fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
		u8 bodyA = (u8)(255 * fade);
		if (bodyA > 2) tk_body(200 + tear * 60, cy, st.code, 6, bodyA, false, t);
		if (tear < 0.05f) tk_perf(200, cy, 0, 255);
		if (sy < 270) {   // the stub, tilting a little as it goes, over the bottom edge
			float cx = 200 - TK_W / 2 + TK_STUB / 2 - tear * 12;
			C2D_ViewReset();
			C2D_ViewTranslate(cx, cy + sy);
			C2D_ViewRotate(-(tear * 0.12f + drop * 0.1f));
			tk_stub(0, 0, u > 0.2f, 255);
			C2D_ViewReset();
		}
	} else if (mode == 0) {
		text(dev_name, 200, 104, 17, rgb(0xffffff), C2D_AlignCenter | BOLD, 380);
		text("is in your Spotify devices.", 200, 126, 11, rgba(0xffffff, 170), C2D_AlignCenter, 380);
	} else if (mode == 4) {
		char title[64];
		const char *l1, *l2;
		pair_words(&st, title, sizeof title, &l1, &l2);
		float a = p_ease(el / 0.35f);
		text(title, 200, 84 + (1 - a) * 8, 16, rgba(0xffffff, (u8)(255 * a)), C2D_AlignCenter | BOLD, 380);
		text(l1, 200, 110, 10, rgba(0xffffff, (u8)(175 * a)), C2D_AlignCenter, 380);
		text(l2, 200, 126, 10, rgba(0xffffff, (u8)(175 * a)), C2D_AlignCenter, 380);
		if (st.err[0] && !net_clock_year() && net_wifi()) text(st.err, 200, 148, 8, rgba(0xffffff, 80), C2D_AlignCenter, 380);   // the plain fact, for whoever reads logs
		tk_stub(200 - TK_W / 2 + TK_STUB / 2, 196, false, 40);   // a ticket nobody could use
		tk_body(200, 196, NULL, 0, 40, true, 0);
	}
	if (swapped) {   // the list's screen is up here: the menu's three bars at their usual corner, and the menu itself
		bool rh = rec_hits;
		rec_hits = false;
		menu_chip(6, drawer_on);
		rec_hits = rh;
		draw_drawer(400, 240);
	}

	// ---------------- the touchscreen ----------------
	C2D_TargetClear(bot, rgb(BG));
	C2D_SceneBegin(bot);
	if (mode == 1 || mode == 2) {   // an empty slot, breathing, where the stub will land
		float br = 0.14f + 0.08f * sinf(t / 500.0f);
		rstroke(100, 64 + p_dy, 120, 96, 12, rgba(0xffffff, (u8)(255 * br)));
		if (mode == 2) text("Waiting for you", 160, 190, 11, rgba(0xffffff, 150), C2D_AlignCenter, 0);
	} else if (mode == 3) {
		float u = el / 3.2f, drop = p_easeio((u - 0.3f) / 0.5f);
		float sy = drop * 330, by = sy - 218;   // in from the top as it leaves the other screen's bottom
		if (sy > 100) {
			float land = u > 0.8f ? (u - 0.8f) / 0.2f : 0;
			float bounce = land > 0 && land < 1 ? -sinf(land * 3.14159f) * 5 : 0;
			float cy = sy >= 330 ? 112 + bounce : by + bounce;
			C2D_ViewReset();
			C2D_ViewTranslate(160, cy);
			C2D_ViewRotate(-0.1f + drop * 0.1f);
			tk_stub(0, 0, true, 255);
			C2D_ViewReset();
		}
		if (u > 0.82f) {
			float k = p_ease((u - 0.82f) / 0.15f);
			text("Linked", 160, 196, 13, rgba(GREEN, (u8)(255 * k)), C2D_AlignCenter | BOLD, 0);
		}
	} else if (mode == 0) {
		for (int k = 0; k < 2; k++) {   // a ring leaves the stub now and then
			float e = fmodf(t / 2600.0f + k * 0.5f, 1.0f);
			p_arc(160, 112, 30 + e * 46, 0, 6.2832f, 2.0f, rgba(GREEN, (u8)(120 * (1 - e))));
		}
		tk_stub(160, 112, true, 255);
		text("Linked", 160, 196, 13, rgb(GREEN), C2D_AlignCenter | BOLD, 0);
	}
	p_button(14, 204, 92, "Back", false);
	if (mode == 0) p_button(214, 204, 92, "Pair again", false);
	else if (mode == 2) p_button(214, 204, 92, "New code", false);
	else if (mode == 4) p_button(214, 204, 92, "Try again", true);
	if (!swapped) {   // ...or here
		bool rh = rec_hits;
		rec_hits = false;
		menu_chip(6, drawer_on);
		rec_hits = rh;
		draw_drawer(320, 240);
	}
}
