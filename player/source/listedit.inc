// Your playlists, changed on the 3DS: a tap on a playlist's title or description (the touchscreen)
// opens the keyboard for it, and a song held still for half a second lifts off its row and can be
// dragged onto one of your playlists, which slide in down the right side. A list from a Spotify
// sync can be changed only if it's yours; the change goes up with the next push (spot_list_*).
// Included by main.c ahead of detailpage.inc, which draws the title and asks about taps.

// ---- whose lists are whose ---------------------------------------------------------------------

static char le_me_id[64];   // your Spotify id, lists/me.id (the sync writes it)
static u64 le_me_t;
static const char *le_me(void) {
	if (!le_me_id[0] && osGetTime() - le_me_t > 10000) {   // not there yet: looked for again now and then
		le_me_t = osGetTime();
		FILE *f = fopen(LISTS_DIR "/me.id", "r");
		if (f) { if (!fgets(le_me_id, sizeof le_me_id, f)) le_me_id[0] = 0; fclose(f); le_me_id[strcspn(le_me_id, "\r\n")] = 0; }
	}
	return le_me_id;
}

// Your display name (lists/me.name, the sync writes it from /me), for your lists' owner line: the
// lists' own owner name came through as your account id (bowr...) instead of "Lilac" (Alex, build 59)
static char le_name[128];
static u64 le_name_t;
static const char *owner_of(const Group *g) {
	if (!g->owner_id) return g->owner;
	const char *me = le_me();
	if (!me[0] || strcmp(g->owner_id, me)) return g->owner;
	if (!le_name[0] && osGetTime() - le_name_t > 10000) {
		le_name_t = osGetTime();
		FILE *f = fopen(LISTS_DIR "/me.name", "r");
		if (f) { if (!fgets(le_name, sizeof le_name, f)) le_name[0] = 0; fclose(f); le_name[strcspn(le_name, "\r\n")] = 0; }
	}
	return le_name[0] ? le_name : g->owner;
}

// A list the 3DS may change: one of the card's own, or a Spotify one you own (Liked Songs: k 0,
// only by liking)
static bool le_editable(int k) {
	if (k <= 0 || k >= lib.nl || !lib.lists[k].file) return false;
	Group *g = &lib.lists[k];
	if (!g->pid) return true;
	const char *me = le_me();
	return me[0] && g->owner_id && !strcmp(g->owner_id, me);
}

// ---- the title and the description -------------------------------------------------------------

static void le_trim(char *s) {
	char *a = s;
	while (*a == ' ') a++;
	memmove(s, a, strlen(a) + 1);
	for (int n = strlen(s); n && s[n - 1] == ' ';) s[--n] = 0;
}

// The keyboard for the open playlist's name (desc false) or description
static void le_edit(bool desc) {
	if (detail <= 0 || dkind != T_PLAYLISTS || !le_editable(detail)) return;
	int k = detail;
	Group *g = &lib.lists[k];
	SwkbdState kb;
	char buf[304], old[256];
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, desc ? 300 : 100);   // Spotify's own limits
	swkbdSetHintText(&kb, desc ? "Add an optional description" : "Give it a name");
	swkbdSetInitialText(&kb, desc ? (g->desc ? g->desc : "") : g->name);
	if (!desc) swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (swkbdInputText(&kb, buf, sizeof buf) != SWKBD_BUTTON_CONFIRM) return;
	for (char *c = buf; *c; c++) if ((u8)*c < 0x20) *c = ' ';   // one line, as the file keeps it
	le_trim(buf);
	if (desc ? !strcmp(buf, g->desc ? g->desc : "") : !strcmp(buf, g->name)) return;   // as it was
	snprintf(old, sizeof old, "%s", g->name);
	const char *e = library_list_details(k, desc ? NULL : buf, desc ? buf : NULL);
	if (e) { say(e); return; }
	g = &lib.lists[k];
	if (g->pid) spot_list_details(g->pid, desc ? NULL : buf, desc ? buf : NULL, -1);
	if (!desc && strcmp(old, g->name)) {   // what's known by its name: its pin, and what's playing from it
		int p = pin_of(old);
		if (p >= 0) { free(pins[p]); pins[p] = strdup(g->name); pins_save(); }
		if (src_kind == T_PLAYLISTS && !strcmp(src_name, old)) snprintf(src_name, sizeof src_name, "%s", g->name);
	}
	say(desc ? (buf[0] ? "Description saved" : "Description removed") : "Renamed");
	dirty = true;
}

// ---- a song, dragged onto a playlist -----------------------------------------------------------

#define DG_W    170     // the playlists' panel, down the right of the touchscreen
#define DG_TOP  30
#define DG_ROW  32
#define DG_HOLD 450     // ms a finger stays still on a song before it lifts
static u64 t0_ms;       // when the touch began (main sets it)
static bool dg_on;      // a song's in hand
static int dg_track = -1, dg_x, dg_y, dg_hover = -1;
static int dg_list[96], dg_n;   // the lists it can go in, by lib.lists index
static float dg_t, dg_scroll;   // the panel: how far out (0 to 1), and scrolled

static float dg_max(void) { float m = dg_n * DG_ROW - (240 - DG_TOP); return m > 0 ? m : 0; }
static int dg_at(int x, int y) {   // the list under the finger, -1 if none
	if (x < 320 - DG_W || y < DG_TOP) return -1;
	int r = (int)floorf((y - DG_TOP + dg_scroll) / DG_ROW);
	return r >= 0 && r < dg_n ? r : -1;
}

// A finger held still on a row (the touchscreen, the list there): the song lifts, if it's a song
static bool dg_try(int x, int y, int v) {
	if (!songs_view() || (detail >= 0 && dkind == K_AP) || !lib.nl) return false;
	int r = row_at(x, y, v);
	if (r < 0 || r >= nitems) return false;
	int here = detail >= 0 && dkind == T_PLAYLISTS ? detail : -1;
	dg_n = 0;
	if (here != 0) dg_list[dg_n++] = 0;   // Liked Songs first, as Spotify has it
	for (int k = 1; k < lib.nl && dg_n < (int)(sizeof dg_list / sizeof *dg_list); k++)
		if (k != here && le_editable(k)) dg_list[dg_n++] = k;
	if (!dg_n) return false;
	dg_on = true;
	dg_track = items[r];
	dg_x = x;
	dg_y = y;
	dg_hover = -1;
	dg_scroll = 0;
	audio_tick(TICK_GRAB);
	return true;
}

static void dg_move(int x, int y) {
	dg_x = x;
	dg_y = y;
	int h = dg_at(x, y);
	if (h != dg_hover && h >= 0) audio_tick(TICK_ARM);
	dg_hover = h;
}

// Every frame: the panel in or out, and scrolled by a finger held near its top or bottom
static void dg_tick(void) {
	float to = dg_on ? 1 : 0;
	dg_t += (to - dg_t) * 0.3f;
	if (fabsf(to - dg_t) < 0.01f) dg_t = to;
	if (!dg_on || dg_x < 320 - DG_W) return;
	float s = dg_y < DG_TOP + 22 ? -(DG_TOP + 22 - dg_y) * 0.25f : dg_y > 218 ? (dg_y - 218) * 0.25f : 0;
	if (s) {
		dg_scroll += s;
		dg_scroll = dg_scroll < 0 ? 0 : dg_scroll > dg_max() ? dg_max() : dg_scroll;
		dg_hover = dg_at(dg_x, dg_y);
	}
}

// The finger's up: into the list under it, or back where it came from
static void dg_drop(int x, int y) {
	if (!dg_on) return;
	dg_on = false;
	int r = dg_at(x, y), t = dg_track;
	dg_hover = -1;
	if (r < 0 || t < 0 || t >= lib.nt) return;
	int k = dg_list[r];
	audio_tick(TICK_LAND);
	if (k == 0) {   // Liked Songs: a like
		if (library_is_liked(t)) { say("Already in Liked Songs"); return; }
		library_like(t);
		spot_request(3000, false, false);
		say("Added to Liked Songs");
		dirty = true;
		return;
	}
	const char *e = library_list_add(k, t);
	if (e) { say(e); return; }
	Group *g = &lib.lists[k];
	if (g->pid) spot_list_add(g->pid, t);
	char s[48];   // a toast's worth
	snprintf(s, sizeof s, "Added to %s", g->name);
	say(s);
	if (detail == k && dkind == T_PLAYLISTS) dp_summary();
	dirty = true;
}

static void dg_cover(int k, float x, float y, float s) {   // a list's little square
	Group *g = &lib.lists[k];
	if (k == 0) {
		C2D_DrawRectangle(x, y, 0, s, s, rgb(0x450af5), rgb(0x8a6cf0), rgb(0x6a7ee8), rgb(0xc4efd9));
		ic_heart(x + s / 2, y + s / 2, s * 0.17f, rgb(0xffffff));
	} else if (list_tile(k, x, y, s)) {}
	else if (g->n) thumb(lib.t[g->songs[0]].album_id, x, y, s);
	else rrect(x, y, s, s, 3, rgb(g->color));
}

// Over the list's screen (the touchscreen): the playlists down the right, and the song at the finger
static void dg_draw(float W) {
	if (dg_t < 0.01f) return;
	float e = 1 - (1 - dg_t) * (1 - dg_t), x0 = W - DG_W * e;
	rect(0, 0, x0, 240, rgba(0, (u8)(110 * e)));
	rect(x0, 0, DG_W, 240, rgb(0x181818));
	rect(x0 - 1, 0, 1, 240, rgba(0xffffff, 25));
	for (int r = 0; r < dg_n; r++) {
		float y = DG_TOP + r * DG_ROW - dg_scroll;
		if (y + DG_ROW < DG_TOP || y > 240) continue;
		if (r == dg_hover) rrect(x0 + 4, y + 1, DG_W - 8, DG_ROW - 2, 5, rgba(0xffffff, 30));
		dg_cover(dg_list[r], x0 + 10, y + 4, 24);
		const char *nm = dg_list[r] == 0 ? "Liked Songs" : lib.lists[dg_list[r]].name;
		text(nm, x0 + 42, y + 20, 10, rgb(r == dg_hover ? GREEN : 0xffffff), 0, DG_W - 50);
	}
	rect(x0, 0, DG_W, DG_TOP, rgb(0x181818));   // the heading stays put over the rows
	text("Add to playlist", x0 + 10, 20, 11, rgb(0xffffff), BOLD, DG_W - 20);
	if (dg_on && dg_track >= 0 && dg_track < lib.nt) {   // the song, lifted, a little up and left of the finger
		Track *t = &lib.t[dg_track];
		float cw = 150, ch = 32, cx = dg_x - 24, cy = dg_y - ch - 6;
		cx = cx < 2 ? 2 : cx + cw > W - 2 ? W - 2 - cw : cx;
		cy = cy < 2 ? 2 : cy;
		rrect(cx + 1, cy + 3, cw, ch, 6, rgba(0, 90));
		rrect(cx, cy, cw, ch, 6, rgb(0x2e2e2e));
		thumb(t->album_id, cx + 4, cy + 4, 24);
		text(t->title, cx + 34, cy + 14, 10, rgb(0xffffff), 0, cw - 40);
		text(t->artist, cx + 34, cy + 26, 8, rgb(GREY), 0, cw - 40);
	}
}
