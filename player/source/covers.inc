// ---- big covers ------------------------------------------------------------------------------
// Included by main.c after artistpage.inc. One texture of ART_HOME² covers for Home and the
// library's grids, ART_SLOTS of them, kept by what they show (a key: "A" + album "name\tartist",
// "P" + a playlist's name, "R" + an artist's name) and handed out least recently drawn first. A
// cover asked for this frame comes from the card's shrunk copy right away, a few a frame (cc_sync);
// the rest the worker reads or makes (art.c) and cc_poll puts in as they come. Till then, the
// small picture the tabs have stands in.

#define CC_COLS 11   // to a row of the 1024² texture
#define CC_PITCH (ART_HOME + 4)   // each slot 2 px of nothing all round: linear filtering never reaches the next one
static C3D_Tex cc_tex;
static bool cc_tex_ok;
static struct { char key[200]; u32 h, used; bool ok; } cc[ART_SLOTS];
static u32 cc_frame = 1;   // this frame, for least recently drawn
static int cc_sync;        // reads off the card still allowed this frame
static u32 *cc_px;         // a cover on its way in (the heap's, not the program image's)

static void cc_init(void) {
	cc_tex_ok = C3D_TexInit(&cc_tex, 1024, 1024, GPU_RGBA8);
	if (cc_tex_ok) { memset(cc_tex.data, 0, cc_tex.size); C3D_TexSetFilter(&cc_tex, GPU_LINEAR, GPU_LINEAR); }
	if (!(cc_px = malloc(ART_HOME * ART_HOME * 4))) cc_tex_ok = false;
}
static void cc_put(int s, const u32 *px) { put_px(&cc_tex, (s % CC_COLS) * CC_PITCH + 2, (s / CC_COLS) * CC_PITCH + 2, px, ART_HOME, ART_HOME); cc[s].ok = true; }
static void cc_tick(void) { cc_frame++; cc_sync = 3; }   // each frame, before drawing
static void cc_poll(void) {
	if (!cc_tex_ok) return;
	bool put = false;
	for (int s; (s = art_home_tile(cc_px)) >= 0;) { cc_put(s, cc_px); put = true; }
	if (put) C3D_TexFlush(&cc_tex);
}

// The slot for key, asked for if it's new (draw: it's on screen, so the card may be read now)
static int cc_get(const char *key, int track, const char *path, const char *artist, bool draw) {
	if (!cc_tex_ok || ghost) return -1;
	u32 h = fnv(key);
	for (int s = 0; s < ART_SLOTS; s++) if (cc[s].h == h && !strcmp(cc[s].key, key)) { cc[s].used = cc_frame; return s; }
	int s = -1;
	for (int j = 0; j < ART_SLOTS; j++) if (cc[j].used != cc_frame && (s < 0 || cc[j].used < cc[s].used)) s = j;
	if (s < 0) return -1;
	snprintf(cc[s].key, sizeof cc[s].key, "%s", key);
	cc[s].h = h;
	cc[s].used = cc_frame;
	cc[s].ok = false;
	bool now = draw && cc_sync > 0;
	if (now) cc_sync--;
	if (art_home_want(s, track, path, artist, now ? cc_px : NULL)) { cc_put(s, cc_px); C3D_TexFlush(&cc_tex); }
	return s;
}
static bool cc_draw(int s, float x, float y, float size) {   // false: not here yet
	if (s < 0 || !cc[s].ok) return false;
	float h = 0.5f / 1024, u = ((s % CC_COLS) * CC_PITCH + 2) / 1024.0f + h, v = ((s / CC_COLS) * CC_PITCH + 2) / 1024.0f + h, d = ART_HOME / 1024.0f - 2 * h;
	Tex3DS_SubTexture st = {ART_HOME, ART_HOME, u, 1 - v, u + d, 1 - v - d};
	C2D_DrawImageAt((C2D_Image){&cc_tex, &st}, x, y, 0, NULL, size / ART_HOME, size / ART_HOME);
	return true;
}

// What each kind of thing shows, by group index
static int cc_album(int a, bool draw) {
	if (a < 0 || a >= lib.nal || !lib.albums[a].n) return -1;
	char k[200];
	snprintf(k, sizeof k, "A%s\t%s", lib.albums[a].name, lib.albums[a].artist ? lib.albums[a].artist : "");
	return cc_get(k, lib.albums[a].songs[0], NULL, NULL, draw);
}
static int cc_list(int i, bool draw) {   // Liked Songs (0) is drawn, not a picture
	if (i <= 0 || i >= lib.nl) return -1;
	Group *g = &lib.lists[i];
	if (!g->image && !g->n) return -1;
	char k[200];
	snprintf(k, sizeof k, "P%s", g->name);
	return cc_get(k, g->image ? -1 : g->songs[0], g->image, NULL, draw);
}
static int cc_artist(int a, bool draw) {
	if (a < 0 || a >= lib.nar) return -1;
	char k[200];
	snprintf(k, sizeof k, "R%s", lib.artists[a].name);
	return cc_get(k, -1, NULL, lib.artists[a].name, draw);
}

// The big covers, drawn: the picture, or what stands in for it
static void liked_tile(float x, float y, float s) {   // Liked Songs: a heart on Spotify's violet, the same as its page
	C2D_DrawRectangle(x, y, 0, s, s, rgb(0x450af5), rgb(0x8a6cf0), rgb(0x6a7ee8), rgb(0xc4efd9));
	ic_heart(x + s / 2, y + s / 2, s * 0.21f, rgb(0xffffff));
}
static void big_album(int a, float x, float y, float s) { if (!cc_draw(cc_album(a, true), x, y, s)) thumb(a, x, y, s); }
static void big_list(int i, float x, float y, float s) {
	if (i == 0) { liked_tile(x, y, s); return; }
	if (cc_draw(cc_list(i, true), x, y, s) || list_tile(i, x, y, s)) return;
	Group *g = &lib.lists[i];
	if (g->n) thumb(lib.t[g->songs[0]].album_id, x, y, s);
	else rrect(x, y, s, s, 3, rgb(0x333333));
}
static void big_artist(int a, float x, float y, float s) {
	if (cc_draw(cc_artist(a, true), x, y, s)) return;
	if (a < MI_FACES && face_ok[a]) { face(a, x, y, s); return; }
	Group *g = &lib.artists[a];   // their first letter on their colour
	C2D_DrawCircleSolid(x + s / 2, y + s / 2, 0, s / 2, rgb(g->color));
	char ini[8] = {0};
	memcpy(ini, g->name, (g->name[0] & 0x80) ? ((g->name[0] & 0xE0) == 0xC0 ? 2 : (g->name[0] & 0xF0) == 0xE0 ? 3 : 4) : 1);
	text(ini, x + s / 2, y + s * 0.67f, s * 0.46f, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
}
