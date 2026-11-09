// ---- lyrics on the player -----------------------------------------------------------------
// Included by main.c before draw_player. Spotify's lyrics view, shrunk: the song's colour behind
// big bold lines; the line being sung in white, the ones before it faded, the ones to come dark.
// Synced lyrics follow the song (a finger can scroll away for a moment, and tapping a line jumps
// there); plain ones just scroll. They come from LRCLIB through meta.c.

// Greedy word wrap: where each line starts and how long it is; a newline ends a line
static int wrap(const char *s, float px, float maxw, int *st, int *ln, int cap) {
	int n = 0, i = 0, L = strlen(s);
	char b[512];
	while (i < L && n < cap) {
		int best = -1, j = i;
		for (;;) {
			int k = j;
			while (k < L && s[k] != ' ' && s[k] != '\n') k++;
			snprintf(b, sizeof b, "%.*s", k - i > 500 ? 500 : k - i, s + i);
			if (best >= 0 && measure(b, px) > maxw) break;
			best = k;
			if (k >= L || s[k] == '\n') break;
			j = k + 1;
		}
		st[n] = i;
		ln[n++] = best - i;
		i = best;
		if (i < L && (s[i] == ' ' || s[i] == '\n')) i++;
	}
	return n;
}

#define DOTS "\xe2\x80\xa2 \xe2\x80\xa2 \xe2\x80\xa2"   // a gap in the words, as Spotify shows it

static Lyrics *lyr;
static bool lyr_on;
static float lyr_y = -1, lyr_y0, lyr_top;   // the view's scroll; at a touch's start; the area's top
static u64 lyr_touch_t, lyr_retry;          // the last scroll by hand: the song takes over 3 s later

typedef struct { const Lyrics *l; float w, px, total; int nsub, cap; int *first, *count, *st, *ln; float *y; } LWrap;
static LWrap lws[2];   // one per width (the 400 and 320 screens), so a swap doesn't re-wrap every frame
static LWrap *lw = &lws[0];

static const char *lyr_text(int i) { return lyr->l[i].text[0] ? lyr->l[i].text : lyr->synced ? DOTS : ""; }

static void lw_reset(void) {   // a new Lyrics may sit at the old one's freed address: forget both layouts
	for (int k = 0; k < 2; k++) { lws[k].l = NULL; lws[k].w = -1; }
}

static void lyr_layout(float w, float px) {
	lw = &lws[w > 300];
	if (lw->l == lyr && lw->w == w && lw->px == px) return;
	free(lw->first); free(lw->count); free(lw->st); free(lw->ln); free(lw->y);
	memset(lw, 0, sizeof *lw);
	lw->l = lyr; lw->w = w; lw->px = px;
	if (!lyr || !lyr->n) return;
	lw->cap = lyr->n * 6 + 8;
	lw->first = malloc(lyr->n * sizeof(int));
	lw->count = malloc(lyr->n * sizeof(int));
	lw->y = malloc(lyr->n * sizeof(float));
	lw->st = malloc(lw->cap * sizeof(int));
	lw->ln = malloc(lw->cap * sizeof(int));
	float lh = px * 1.3f, y = 0;
	for (int i = 0; i < lyr->n; i++) {
		const char *t = lyr_text(i);
		lw->first[i] = lw->nsub;
		lw->y[i] = y;
		int c = t[0] ? wrap(t, px, w, lw->st + lw->nsub, lw->ln + lw->nsub, lw->cap - lw->nsub) : 0;
		lw->count[i] = c;
		lw->nsub += c;
		y += c ? c * lh + (lyr->synced ? px * 0.55f : 0) : lh * 0.6f;
	}
	lw->total = y;
}

static int lyr_cur(double pos) {   // the line being sung: a quarter second early, like Spotify
	u32 ms = (u32)(pos * 1000) + 250;
	int c = -1;
	for (int i = 0; i < lyr->n && lyr->l[i].ms <= ms; i++) c = i;
	return c;
}

static void draw_lyrics(float W, const AudioInfo *ai, float top, float bot, u32 bg) {
	lyr_top = top;
	float px = W > 320 ? 16 : 14, lh = px * 1.3f, cx = W / 2;
	const char *msg = NULL;
	if (!lyr || lyr->track != ai->track) msg = "Looking for lyrics...";
	else if (!lyr->n) msg = lyr->offline ? "Lyrics need Wi-Fi the first time." : lyr->instrumental ? "Instrumental" : "No lyrics for this one.";
	if (msg) { text(msg, cx, (top + bot) / 2, 12, rgba(0xffffff, 200), C2D_AlignCenter | BOLD, W - 32); return; }

	lyr_layout(W - 32, px);
	int cur = lyr->synced ? lyr_cur(ai->pos) : -1;
	float h = bot - top, want = cur >= 0 ? lw->y[cur] - h * 0.3f : lyr_y;
	if (want < 0) want = 0;
	float maxy = lw->total + 30 - h * 0.6f;
	if (maxy < 0) maxy = 0;
	if (lyr_y < 0) lyr_y = cur >= 0 ? want : 0;
	if (lyr->synced && osGetTime() - lyr_touch_t > 3000) lyr_y += (want - lyr_y) * 0.12f;
	lyr_y = lyr_y < 0 ? 0 : lyr_y > maxy ? maxy : lyr_y;

	u32 r = bg >> 16 & 255, g = bg >> 8 & 255, b = bg & 255;
	bool light = r * 3 + g * 6 + b > 1000;   // dark words for what's coming, if they'll show
	for (int i = 0; i < lyr->n; i++) {
		float y0 = top + 4 + lw->y[i] - lyr_y;
		if (y0 > bot) break;
		if (y0 + lw->count[i] * lh < top) continue;
		u32 c = 0xffffff;
		u8 a = 235;
		if (lyr->synced) {
			if (i == cur) a = 255;
			else if (i < cur) a = 150;
			else if (light) { c = 0; a = 210; }
			else a = 80;
		}
		const char *t = lyr_text(i);
		for (int j = 0; j < lw->count[i]; j++) {
			float yy = y0 + j * lh + px * 0.85f;
			float f = (yy - top) / 18, f2 = (bot - yy) / 14;   // fade out at the edges
			if (f2 < f) f = f2;
			if (f <= 0) continue;
			if (f > 1) f = 1;
			int s0 = lw->st[lw->first[i] + j], n = lw->ln[lw->first[i] + j];
			char buf[512];
			snprintf(buf, sizeof buf, "%.*s", n > 500 ? 500 : n, t + s0);
			text(buf, 16, yy, px, rgba(c, (u8)(a * f)), BOLD, 0);
		}
	}
	float cy = top + 4 + lw->total - lyr_y + 18;
	if (cy > top + 8 && cy < bot) text("Lyrics from LRCLIB", 16, cy, 8, rgba(0xffffff, 140), 0, 0);
}

static void lyr_tap(int y, int track) {   // a synced line of this song: it jumps there
	LWrap *w = &lws[0];   // taps only come from the touchscreen, 320 wide
	if (!lyr || lyr->track != track || !lyr->synced || !w->y || w->l != lyr) return;
	float py = y - lyr_top - 4 + lyr_y;
	for (int i = lyr->n - 1; i >= 0; i--)
		if (w->count[i] && py >= w->y[i]) {
			if (py < w->y[i] + w->count[i] * w->px * 1.3f) { audio_seek(lyr->l[i].ms / 1000.0, false); lyr_touch_t = 0; }
			return;
		}
}

static void lyr_poll(int track, bool view_on) {
	Lyrics *n = meta_lyrics_take();
	if (n) { meta_lyrics_free(lyr); lyr = n; lyr_y = -1; lw_reset(); }
	// it came back without Wi-Fi: try again now and then while the view's up
	if (view_on && lyr && lyr->offline && lyr->track == track && osGetTime() > lyr_retry) {
		lyr_retry = osGetTime() + 15000;
		meta_lyrics_again();
	}
}

// Spotify's lyrics button: a microphone leaning right, drawn once into a little texture with soft
// edges (build 57's was circles and lines, and looked it). Grey; green with a dot under it while
// the words are up; dim when the song has none (a tap then says so, Spotify's way, no empty page).
static void put_rect(C3D_Tex *t, int ox, int oy, const u32 *px, int w, int h);   // artistpage.inc
static C3D_Tex mic_tex;
static bool mic_ok;
// Spotify's own: one thin outline, a round head flowing into a body that tapers to a point down to
// the left, and a line across where the head ends (Alex's screenshot, build 60; mine before it was
// a lollipop, then a sticky grenade). A capsule whose radius narrows from head to tip.
static bool mic_in(float fx, float fy) {
	const float HX = 21, HY = 10, RA = 5.2f, TX = 7.6f, TY = 24.2f, RB = 1.4f, W = 1.5f;
	float vx = TX - HX, vy = TY - HY, L = sqrtf(vx * vx + vy * vy), ux = vx / L, uy = vy / L;
	float qx = fx - HX, qy = fy - HY, t = qx * ux + qy * uy, tt = t < 0 ? 0 : t > L ? L : t;
	float dx = qx - ux * tt, dy = qy - uy * tt, d = sqrtf(dx * dx + dy * dy) - (RA + (RB - RA) * tt / L);
	return fabsf(d) < W / 2 || (fabsf(t - RA * 0.55f) < W / 2 && d < 0);
}
static void mic_make(void) {   // 32² white, its alpha the shape's coverage (4×4 samples a texel), drawn at 18 px
	if (!(mic_ok = C3D_TexInit(&mic_tex, 32, 32, GPU_RGBA8))) return;
	C3D_TexSetFilter(&mic_tex, GPU_LINEAR, GPU_LINEAR);
	static u32 px[32 * 32];
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			int n = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) n += mic_in(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
			px[y * 32 + x] = 0xFFFFFF00 | (u32)(n * 255 / 16);
		}
	put_rect(&mic_tex, 0, 0, px, 32, 32);
}
static bool lyr_none(int track) {   // this song's looked up, and there aren't any
	return lyr && lyr->track == track && lyr->n == 0 && !lyr->offline;
}
static void lyr_chip(float W, int track) {
	float cx = W - 17, cy = 16;
	C2D_DrawCircleSolid(cx, cy, 0, 11, rgba(0, 70));
	if (!mic_ok) return;
	u32 c = lyr_on ? rgb(GREEN) : lyr_none(track) ? rgba(0xffffff, 60) : rgb(0xd8d8d8);
	C2D_ImageTint t;
	C2D_PlainImageTint(&t, c, 1);
	Tex3DS_SubTexture st = {32, 32, 0, 1, 1, 0};
	C2D_DrawImageAt((C2D_Image){&mic_tex, &st}, cx - 9, cy - 9, 0, &t, 18 / 32.0f, 18 / 32.0f);
	if (lyr_on) C2D_DrawCircleSolid(cx, cy + 9.2f, 0, 1.2f, rgb(GREEN));
}
