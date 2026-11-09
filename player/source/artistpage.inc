// ---- artist pages -------------------------------------------------------------------------
// Included by main.c, after the drawing helpers and the queue: one artist's page the way Spotify
// lays it out (photo across the top with the name on it, monthly listeners, play and shuffle,
// Popular, your library, Discography, Fans also like, Appears on, On tour, About), drawn in the
// list's place. The data comes from meta.c; the songs it plays are the ones on the card.

static C3D_Tex ap_head_tex, ap_tiles;   // the photo; 7×7 tiles of 72 px
static bool ap_have[MI_SLOTS];
static Artist *ap_a;                    // the page's data once it's here
static int ap_gen = -1;
static char ap_tex_id[32];               // whose pictures the textures hold, so going back in shows them at once
static char ap_tex_name[128];            // ...by the name the page was opened with, for the frames before it lands

typedef struct {
	char name[256], id[32];
	int local;            // the library's artist, -1 if they aren't in it
	float scroll, hs[8];  // the page, and each shelf sideways
	int fk, fi;           // what the cursor's on: a kind and which one
	int chip;             // Discography: popular, albums, singles
} ApEntry;
static ApEntry ap_st[8];
static int ap_n;
#define AP (&ap_st[ap_n - 1])
static int *ap_songs, ap_nsongs;        // every song of theirs on the card, features too
static bool ap_bio;
static float ap_bio_scroll, ap_vel, ap_maxsc, ap_bio_max;
static char ap_gname[256];              // the title of the list opened from the page (ap_g, ap_gkey: main.c)

enum { F_PLAY, F_SHUF, F_TOP, F_LIKED, F_LOCAL, F_CHIP, F_DISC, F_REL, F_APP, F_ABOUT };
typedef struct { u8 kind; s8 shelf; s16 k, line; float x, y, w, h; } Foc;   // x without the shelf's scroll; y on the page
static Foc foc[112];
static int nfoc;
static bool ap_rec;

static bool ap_active(void) { return ap_n > 0 && detail < 0; }

static void corner(float x, float y, float sx, float sy, float r, u32 c);
static void ap_unwrap(void);

static void put_px(C3D_Tex *t, int ox, int oy, const u32 *px, int w, int h) {   // no flush: put_rect, or flush once after many
	u32 *d = t->data;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int tx = ox + x, ty = oy + y;
			d[(((ty >> 3) * (t->width >> 3) + (tx >> 3)) << 6) +
			  ((tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3))] = px[y * w + x];
		}
}
static void put_rect(C3D_Tex *t, int ox, int oy, const u32 *px, int w, int h) { put_px(t, ox, oy, px, w, h); C3D_TexFlush(t); }

// ---- numbers and words -------------------------------------------------------------------

static void commas(char *o, int cap, u64 v) {
	char d[32];
	int n = snprintf(d, sizeof d, "%llu", (unsigned long long)v), k = 0;
	for (int i = 0; i < n && k < cap - 1; i++) {
		if (i && (n - i) % 3 == 0 && k < cap - 2) o[k++] = ',';
		o[k++] = d[i];
	}
	o[k] = 0;
}
static void short_num(char *o, int cap, u64 v) {   // 1.7M, 403K, 950
	if (v >= 1000000) { double m = v / 1e6; snprintf(o, cap, m >= 100 ? "%.0fM" : "%.1fM", m); }
	else if (v >= 1000) snprintf(o, cap, "%lluK", (unsigned long long)(v / 1000));
	else snprintf(o, cap, "%llu", (unsigned long long)v);
	char *z = strstr(o, ".0M");
	if (z) memmove(z, z + 2, strlen(z + 2) + 1);
}
static const char *type_name(const char *t) {
	if (!t) return "Album";
	if (!strcmp(t, "SINGLE")) return "Single";
	if (!strcmp(t, "EP")) return "EP";
	if (!strcmp(t, "COMPILATION")) return "Compilation";
	return "Album";
}

// ---- small pictures ----------------------------------------------------------------------

static void ap_tile(int slot, float x, float y, float s, u32 fallback, bool round, u8 alpha) {
	if (slot > 0 && slot < MI_SLOTS && ap_have[slot]) {
		int i = slot - 1;
		float h = 0.5f / 512, u = (i % 7) * 72 / 512.0f + h, v = (i / 7) * 72 / 512.0f + h, d = 72 / 512.0f - 2 * h;
		Tex3DS_SubTexture st = {72, 72, u, 1 - v, u + d, 1 - v - d};
		C2D_ImageTint tint;
		if (alpha < 255) C2D_AlphaImageTint(&tint, alpha / 255.0f);
		C2D_DrawImageAt((C2D_Image){&ap_tiles, &st}, x, y, 0, alpha < 255 ? &tint : NULL, s / 72, s / 72);
	} else if (round) C2D_DrawCircleSolid(x + s / 2, y + s / 2, 0, s / 2, rgba(fallback, alpha));
	else rrect(x, y, s, s, 3, rgba(fallback, alpha));
}

static void ic_check(float x, float y, float r) {   // Spotify's green "it's in your library"
	C2D_DrawCircleSolid(x, y, 0, r, rgb(GREEN));
	u32 k = rgb(0);
	C2D_DrawLine(x - r * .45f, y, k, x - r * .1f, y + r * .38f, k, r * .28f, 0);
	C2D_DrawLine(x - r * .1f, y + r * .38f, k, x + r * .5f, y - r * .35f, k, r * .28f, 0);
}
static void ic_saved(float x, float y, float r) {   // ...and its green arrow for "downloaded": on the card
	C2D_DrawCircleSolid(x, y, 0, r, rgb(GREEN));
	u32 k = rgb(0);
	rect(x - r * .12f, y - r * .55f, r * .24f, r * .6f, k);
	C2D_DrawTriangle(x - r * .5f, y + r * .02f, k, x + r * .5f, y + r * .02f, k, x, y + r * .55f, k, 0);
}
static void ic_dots(float x, float y, u32 c) { for (int i = -1; i <= 1; i++) C2D_DrawCircleSolid(x + i * 5, y, 0, 1.4f, c); }

// ---- opening, going back -----------------------------------------------------------------

static void ap_songs_of(const char *name) {
	free(ap_songs);
	ap_songs = malloc((lib.nt + 1) * sizeof(int));
	ap_nsongs = 0;
	for (int a = 0; a < lib.nal; a++)   // album by album, in track order, like the album pages
		for (int j = 0; j < lib.albums[a].n; j++) {
			int i = lib.albums[a].songs[j];
			if (meta_on_song(i, name)) ap_songs[ap_nsongs++] = i;
		}
}

static void ap_poll(void);
static void ap_request(void) {
	meta_artist_free(ap_a);
	ap_a = NULL;
	ap_unwrap();
	ap_gen = -1;   // ap_have stays: ap_poll clears it only if the page is someone else's
	ap_bio = false;
	ap_vel = 0;
	ap_songs_of(AP->name);
	meta_artist_want(AP->name, AP->id);   // from the card if it's there, this very call
	ap_poll();                            // ...and onto the page now, not next frame
}

static void ap_push(const char *name, const char *id, int local) {
	ease_clear();   // the slot may be reused: nothing still easing into it
	if (ap_n == 8) { memmove(ap_st, ap_st + 1, 7 * sizeof(ApEntry)); ap_n = 7; }
	ApEntry *e = &ap_st[ap_n++];
	memset(e, 0, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", name);
	snprintf(e->id, sizeof e->id, "%s", id ? id : "");
	e->local = local;
	e->fk = F_PLAY;
	ap_request();
}
static void ap_open_local(int artist) { if (artist >= 0 && artist < lib.nar) ap_push(lib.artists[artist].name, NULL, artist); }

static void ap_clear(void) {
	ap_n = 0;
	ap_bio = false;
	meta_artist_free(ap_a);
	ap_a = NULL;
	ap_unwrap();
	ap_gen = -1;
}

static void ap_back(void) {
	ease_clear();
	if (ap_bio) { ap_bio = false; return; }
	if (ap_n > 1) {
		float sc = ap_st[ap_n - 2].scroll;
		ap_n--;
		ap_request();
		AP->scroll = sc;
	} else ap_clear();
}

// The page's data and its pictures as they arrive (every frame, on the main thread)
static void ap_poll(void) {
	int g;
	Artist *a = meta_artist_take(&g);
	if (a) {
		if (ap_n) {
			bool again = g == ap_gen;   // the same tap's page again, fresher: its pictures may sit in other slots now
			meta_artist_free(ap_a);
			ap_a = a;
			ap_unwrap();
			ap_gen = g;
			if (again || strcmp(ap_tex_id, a->id)) {   // someone else's, or reshuffled: wait for theirs (ready ones come this same call)
				memset(ap_have, 0, sizeof ap_have);
				snprintf(ap_tex_id, sizeof ap_tex_id, "%s", a->id);
			}
			snprintf(ap_tex_name, sizeof ap_tex_name, "%s", AP->name);
			snprintf(AP->id, sizeof AP->id, "%s", a->id);
			if (AP->chip == 0 && !a->npop) AP->chip = a->nalb ? 1 : 2;
		} else meta_artist_free(a);
	}
	const u32 *px;
	int w, h, s;
	bool head = false, tiles = false;
	while ((s = meta_img(&g, &px, &w, &h)) >= 0) {   // every one waiting: a page's pictures come together
		if (ap_a && g == ap_gen && s < MI_SLOTS) {
			if (s == 0) { put_px(&ap_head_tex, 0, 0, px, w, h); head = true; }
			else { put_px(&ap_tiles, ((s - 1) % 7) * 72, ((s - 1) / 7) * 72, px, w, h); tiles = true; }
			ap_have[s] = true;
		}
		meta_img_done();
	}
	if (head) C3D_TexFlush(&ap_head_tex);
	if (tiles) C3D_TexFlush(&ap_tiles);
}

// ---- what the page plays and opens -------------------------------------------------------

static bool ap_playing(void) { return ap_n && src_kind == K_APQ && !strcmp(src_name, ap_a ? ap_a->name : AP->name); }

// Popular first (from song k, or all of it), then the rest of theirs on the card
static void ap_play(int k) {
	int *q = malloc((ap_nsongs + 12) * sizeof(int)), n = 0;
	bool *in = calloc(lib.nt + 1, 1);
	int k0 = k > 0 ? k : 0;
	if (ap_a)   // from song k round to the one before it
		for (int j = 0; j < ap_a->ntop; j++) {
			int t = ap_a->top[(k0 + j) % ap_a->ntop].local;
			if (t >= 0 && !in[t]) { in[t] = true; q[n++] = t; }
		}
	for (int j = 0; j < ap_nsongs; j++) if (!in[ap_songs[j]]) { in[ap_songs[j]] = true; q[n++] = ap_songs[j]; }
	free(in);
	if (n) {
		snprintf(src_name, sizeof src_name, "%s", ap_a ? ap_a->name : AP->name);
		src_kind = K_APQ;
		src_id = 0;
		set_queue(q, n, k < 0 && shuffle ? rand() % n : 0);
	} else say("None of theirs are on your 3DS");
	free(q);
}

static void ap_list(bool liked) {
	free(ap_g.songs);
	ap_g.songs = malloc((ap_nsongs + 1) * sizeof(int));
	ap_g.n = 0;
	for (int j = 0; j < ap_nsongs; j++) if (!liked || library_is_liked(ap_songs[j])) ap_g.songs[ap_g.n++] = ap_songs[j];
	snprintf(ap_gname, sizeof ap_gname, "%s", ap_a ? ap_a->name : AP->name);
	snprintf(ap_gkey, sizeof ap_gkey, "%s:%s", liked ? "liked" : "card", ap_gname);
	ap_g.name = ap_gname;
	ap_g.artist = liked ? "Liked Songs" : "On this 3DS";
	ap_g.color = 0;
	open_detail(K_AP, 0);
}

static int ap_disc(MRelease **v) {   // the shelf the chips have picked
	if (!ap_a) { *v = NULL; return 0; }
	int c = AP->chip;
	if (c == 0 && ap_a->npop) { *v = ap_a->pop; return ap_a->npop; }
	if (c <= 1 && ap_a->nalb) { *v = ap_a->albums; return ap_a->nalb; }
	if (ap_a->nsing) { *v = ap_a->singles; return ap_a->nsing; }
	*v = ap_a->albums;
	return ap_a->nalb;
}

static void ap_activate(int kind, int k, const AudioInfo *ai) {
	if (kind == F_PLAY) {
		if (ap_playing() && ai->open) audio_pause(!ai->paused);
		else ap_play(-1);
	} else if (kind == F_SHUF) {
		set_shuffle(!shuffle);
		say(shuffle ? "Shuffle on" : "Shuffle off");
	} else if (kind == F_TOP && ap_a && k < ap_a->ntop) {
		int t = ap_a->top[k].local;
		if (t < 0) say("Not on your 3DS");
		else if (ai->open && ai->track == t) audio_pause(!ai->paused);
		else ap_play(k);
	} else if (kind == F_LIKED || kind == F_LOCAL) ap_list(kind == F_LIKED);
	else if (kind == F_CHIP) { AP->chip = k; AP->hs[0] = 0; }
	else if (kind == F_DISC || kind == F_APP) {
		MRelease *v;
		int n = kind == F_DISC ? ap_disc(&v) : (v = ap_a ? ap_a->appears : NULL, ap_a ? ap_a->napp : 0);
		if (k < n && v[k].local >= 0) open_detail(T_ALBUMS, v[k].local);
		else if (k < n) say("Not on your 3DS");
	} else if (kind == F_REL && ap_a && k < ap_a->nrel) ap_push(ap_a->rel[k].name, ap_a->rel[k].id, ap_a->rel[k].local);
	else if (kind == F_ABOUT) { ap_bio = true; ap_bio_scroll = 0; }
}

// ---- drawing -----------------------------------------------------------------------------

static float ap_sc;   // this frame's scroll, for reg()
static void reg(int kind, int k, int line, int shelf, float x, float y, float w, float h) {
	if (!ap_rec || nfoc >= (int)(sizeof foc / sizeof *foc)) return;
	foc[nfoc++] = (Foc){kind, shelf, k, line, x, y + ap_sc, w, h};
}
static bool focused(int kind, int k) { return cursor_vis && ap_n && AP->fk == kind && AP->fi == k; }
static void ring(float x, float y, float w, float h, float r) { rstroke(x - 2, y - 2, w + 4, h + 4, r + 2, rgba(0xffffff, 220)); }

static float heading(const char *s, float y) { text(s, 12, y + 18, 15, rgb(0xffffff), BOLD, 0); return y + 28; }

// a shelf of cards that slides sideways: covers (round for artists), a name and a line under it
static float shelf(int shelf_i, int kind, int line, float y, float W, int n,
                   const char *(*name)(int), void (*sub)(int, char *, int), int (*img)(int), bool round, int (*local)(int)) {
	float cw = 72, gap = 12, x0 = 12, hs = AP->hs[shelf_i];
	for (int k = 0; k < n; k++) {
		float cx = x0 + k * (cw + gap);
		reg(kind, k, line, shelf_i, cx, y, cw, 100);
		float x = cx - hs;
		if (x > W || x + cw < 0) continue;
		bool here = local(k) >= 0;
		ap_tile(img(k), x, y, cw, 0x333333, round, 255);
		if (focused(kind, k)) ring(x, y, cw, cw, round ? cw / 2 : 4);
		text(name(k), x, y + cw + 13, 10, rgb(0xffffff), BOLD, cw);
		char s[64];
		sub(k, s, sizeof s);
		float tx = x;
		if (here) { ic_saved(x + 4, y + cw + 22.5f, 4); tx += 11; }
		text(s, tx, y + cw + 26, 9, rgb(GREY), 0, cw - (tx - x));
	}
	float total = x0 + n * (cw + gap) - gap + 12 - W;
	if (ap_rec && AP->hs[shelf_i] > (total > 0 ? total : 0)) AP->hs[shelf_i] = total > 0 ? total : 0;   // the list's own screen only
	return y + 100 + 14;
}

static MRelease *cur_rel;
static const char *rel_name(int k) { return cur_rel[k].name; }
static void rel_sub(int k, char *s, int n) {
	if (cur_rel[k].year) snprintf(s, n, "%d \xc2\xb7 %s", cur_rel[k].year, type_name(cur_rel[k].type));
	else snprintf(s, n, "%s", type_name(cur_rel[k].type));
}
static int rel_img(int k) { return cur_rel[k].img; }
static int rel_local(int k) { return cur_rel[k].local; }
static const char *art_name(int k) { return ap_a->rel[k].name; }
static void art_sub(int k, char *s, int n) { (void)k; snprintf(s, n, "Artist"); }
static int art_img(int k) { return ap_a->rel[k].img; }
static int art_local(int k) { return ap_a->rel[k].local; }

// a row for the songs on the card: their face with a badge, a count, "By ..."
static float lib_row(int kind, int line, float y, float W, int n, int releases, bool liked) {
	reg(kind, 0, line, -1, 0, y, W, 52);
	if (focused(kind, 0)) rect(0, y, W, 52, rgba(0xffffff, 22));
	if (ap_a && ap_a->avatar > 0 && ap_have[ap_a->avatar]) ap_tile(ap_a->avatar, 12, y + 4, 44, 0, true, 255);
	else if (AP->local >= 0) {
		C2D_DrawCircleSolid(34, y + 26, 0, 22, rgb(lib.artists[AP->local].color));
		initial(34, y + 22, &lib.artists[AP->local]);
	} else C2D_DrawCircleSolid(34, y + 26, 0, 22, rgb(0x333333));
	if (liked) {   // a heart badge, like "You liked"
		C2D_DrawCircleSolid(50, y + 42, 0, 8, rgb(GREEN));
		ic_heart(50, y + 42, 3.4f, rgb(0));
	} else ic_saved(50, y + 42, 8);
	char s[96];
	snprintf(s, sizeof s, "%s \xc2\xb7 %d song%s \xc2\xb7 %d release%s", liked ? "Liked Songs" : "On this 3DS", n, n == 1 ? "" : "s", releases, releases == 1 ? "" : "s");
	text(s, 68, y + 24, 11, rgb(0xffffff), BOLD, W - 80);
	snprintf(s, sizeof s, "By %s", ap_a ? ap_a->name : AP->name);
	text(s, 68, y + 38, 9, rgb(GREY), 0, W - 80);
	return y + 56;
}

typedef struct { const Artist *a; float w; int n, st[80], ln[80]; } Wrapped;
static Wrapped about_ws[2], bio_ws[2];   // per width, like the lyrics
#define about_w about_ws[LW > 320]
#define bio_w bio_ws[LW > 320]
static void ap_unwrap(void) {   // ap_a changed: the old one's freed, and a new one may land at its address
	for (int k = 0; k < 2; k++) { about_ws[k].a = bio_ws[k].a = NULL; about_ws[k].w = bio_ws[k].w = -1; }
}
static void bio_wrap(Wrapped *c, float w, int cap) {
	if (c->a == ap_a && c->w == w) return;
	c->a = ap_a;
	c->w = w;
	c->n = ap_a && ap_a->bio ? wrap(ap_a->bio, 10, w, c->st, c->ln, cap < 80 ? cap : 80) : 0;
}

static void ap_draw_bio(float W, float H) {
	rect(0, 0, W, H, rgb(BG));
	bio_wrap(&bio_w, W - 24, 80);
	float y = 44 - ap_bio_scroll;
	char s[64];
	if (ap_a) {
		commas(s, sizeof s, ap_a->listeners);
		strcat(s, " monthly listeners");
		text(s, 12, y, 12, rgb(0xffffff), BOLD, 0);
		y += 16;
		commas(s, sizeof s, ap_a->followers);
		strcat(s, " followers");
		text(s, 12, y, 10, rgb(GREY), 0, 0);
		y += 22;
		for (int i = 0; i < bio_w.n; i++, y += 15) {
			if (y < 20 || y > H + 14) continue;
			char b[512];
			snprintf(b, sizeof b, "%.*s", bio_w.ln[i] > 500 ? 500 : bio_w.ln[i], ap_a->bio + bio_w.st[i]);
			text(b, 12, y, 10, rgb(0xdddddd), 0, 0);
		}
		if (!bio_w.n) text("No biography on Spotify.", 12, y, 10, rgb(GREY), 0, 0);
	}
	if (ap_rec) {
		ap_bio_max = y + ap_bio_scroll + 16 - H;
		if (ap_bio_max < 0) ap_bio_max = 0;
	}
	rect(0, 0, W, 30, rgb(BG));
	rrect(6, 5, 22, 22, 11, rgb(CHIP));
	ic_back(16.5f, 16, rgb(0xffffff));
	snprintf(s, sizeof s, "About");
	text(s, 36, 15, 12, rgb(0xffffff), BOLD, W - 48);
	text(ap_a ? ap_a->name : AP->name, 36, 26, 8, rgb(GREY), 0, W - 48);
}

static void ap_draw(float W, float H, const AudioInfo *ai) {
	ApEntry *e = AP;
	ap_rec = !ghost && W == list_w();
	if (ap_rec) nfoc = 0;
	if (ap_bio) { ap_draw_bio(W, H); return; }
	Artist *a = ap_a;
	float hh = 176 * W / 400, sc = e->scroll;
	ap_sc = sc;
	u32 col = a && a->color ? a->color : e->local >= 0 ? lib.artists[e->local].color : 0x404040;
	const char *name = a ? a->name : e->name;

	// the photo, riding up at half speed and dimming as it goes
	float hy = -sc * 0.5f;
	// the photo; before the page lands, the one already in the texture if it's this artist's
	if (ap_have[0] && (a || (ap_tex_name[0] && !strcmp(ap_tex_name, e->name)))) {
		Tex3DS_SubTexture hs = {MI_HEAD_W, MI_HEAD_H, 0, 1, MI_HEAD_W / 512.0f, 1 - MI_HEAD_H / 256.0f};
		C2D_DrawImageAt((C2D_Image){&ap_head_tex, &hs}, 0, hy, 0, NULL, W / MI_HEAD_W, W / MI_HEAD_W);
	} else C2D_DrawRectangle(0, hy, 0, W, hh, rgb(mixc(col, 0xffffff, 0.8f)), rgb(mixc(col, 0xffffff, 0.8f)), rgb(mixc(col, 0, 0.6f)), rgb(mixc(col, 0, 0.6f)));
	C2D_DrawRectangle(0, hy + hh * 0.3f, 0, W, hh * 0.7f + 1, rgba(0, 0), rgba(0, 0), rgba(0, 165), rgba(0, 165));
	if (sc > 0) rect(0, hy, W, hh, rgba(0, (u8)(sc >= hh ? 210 : 210 * sc / hh)));

	float y0 = hh - sc;
	float px = W > 320 ? 34 : 28;
	while (px > 18 && measure_name(name, px) > W - 24) px -= 2;
	bool lis = a && a->listeners;
	// its baseline high enough that a g or a y clears the listeners line (whose tops are ~16 up)
	text(name, 12, y0 - (lis ? 20 + px * 0.25f : 12), px, rgb(0xffffff), BOLD | NAMEF, W - 24);
	char s[96], n1[32];
	if (lis) {
		short_num(n1, sizeof n1, a->listeners);
		snprintf(s, sizeof s, "%s monthly listeners", n1);
		text(s, 12, y0 - 8, 10, rgb(0xffffff), 0, 0);
	}

	// under the photo: the page, washed with their colour at the top
	rect(0, y0, W, (H > y0 ? H - y0 : 0) + 1, rgb(BG));
	C2D_DrawRectangle(0, y0, 0, W, 110, rgb(mixc(col, BG, 0.45f)), rgb(mixc(col, BG, 0.45f)), rgb(BG), rgb(BG));
	float y = y0 + 10;
	int line = 0;

	// play, shuffle
	reg(F_PLAY, 0, line, -1, 12, y, 40, 40);
	C2D_DrawCircleSolid(32, y + 20, 0, 20, rgb(GREEN));
	if (ap_playing() && ai->open && !ai->paused) ic_pause(32, y + 20, 14, rgb(0));
	else ic_play(34, y + 20, 14, rgb(0));
	if (focused(F_PLAY, 0)) ring(12, y, 40, 40, 20);
	reg(F_SHUF, 0, line, -1, 60, y + 6, 30, 28);
	ic_shuffle(75, y + 20, shuffle ? rgb(GREEN) : rgb(GREY));
	if (shuffle) C2D_DrawCircleSolid(73, y + 31, 0, 1.3f, rgb(GREEN));
	if (focused(F_SHUF, 0)) ring(60, y + 6, 30, 28, 6);
	if (a && a->offline) {
		snprintf(s, sizeof s, "Saved %s", a->fetched);
		text(s, W - 12, y + 24, 8, rgb(GREY), C2D_AlignRight, 0);
	}
	y += 50;
	line++;

	if (!a) {
		char why[48] = "";
		MState st = meta_artist_state(why, sizeof why);
		text(st == MS_FAILED ? why : "Getting their page from Spotify...", 12, y + 12, 10, rgb(GREY), 0, W - 24);
		y += 26;
	}

	// Popular
	if (a && a->ntop) {
		y = heading("Popular", y);
		int nt = a->ntop < 5 ? a->ntop : 5;
		bool wide = W > 320;
		for (int k = 0; k < nt; k++, line++) {
			MTop *m = &a->top[k];
			reg(F_TOP, k, line, -1, 0, y, W, 38);
			if (focused(F_TOP, k)) rect(0, y, W, 38, rgba(0xffffff, 22));
			bool here = m->local >= 0, now = here && ai->open && ai->track == m->local;
			u8 al = here ? 255 : 110;
			snprintf(s, sizeof s, "%d", k + 1);
			if (now && !ai->paused) for (int b = 0; b < 3; b++) rect(14 + b * 4, y + 24 - (3 + (b * 7 + osGetTime() / 90) % 8), 2.5f, 3 + (b * 7 + osGetTime() / 90) % 8, rgb(GREEN));
			else text(s, 22, y + 23, 11, rgba(now ? GREEN : GREY, al), C2D_AlignRight, 0);
			ap_tile(m->img, 32, y + 5, 28, 0x333333, false, al);
			float rx = W - 12;
			char d[16] = "";
			u32 ms = m->ms ? m->ms : here ? lib.t[m->local].secs * 1000 : 0;
			if (ms) mmss(d, ms / 1000.0);
			text(d, rx, y + 23, 9, rgba(GREY, al), C2D_AlignRight, 0);
			if (here && library_is_liked(m->local)) ic_check(rx - 44, y + 19, 6);
			float tw = W - 68 - (wide ? 170 : 74);
			if (wide && m->plays) { commas(n1, sizeof n1, m->plays); text(n1, rx - 64, y + 23, 9, rgba(GREY, al), C2D_AlignRight, 0); }
			text(m->name, 68, y + 17, 11, rgba(now ? GREEN : 0xffffff, al), 0, tw);
			float sx = 68;
			if (m->explicit_) {
				rrect(sx, y + 23, 9, 9, 1.5f, rgba(GREY, al));
				text("E", sx + 4.5f, y + 30.5f, 7, rgb(BG), C2D_AlignCenter | BOLD, 0);
				sx += 12;
			}
			if (here) { ic_saved(sx + 4, y + 27.5f, 4); sx += 11; }
			if (!wide && m->plays) commas(s, sizeof s, m->plays);   // the phone app's way: plays under the title
			else snprintf(s, sizeof s, "%s", m->artists);
			text(s, sx, y + 31, 9, rgba(GREY, al), 0, tw - (sx - 68));
			y += 38;
		}
		y += 8;
	}

	// what of theirs is on the card
	int nl = 0, nrel = 0, nrl = 0;
	bool *seen = calloc(lib.nal + 1, 1), *seenl = calloc(lib.nal + 1, 1);
	for (int j = 0; j < ap_nsongs; j++) {
		int t = ap_songs[j], al = lib.t[t].album_id;
		if (al >= 0 && al < lib.nal && !seen[al]) { seen[al] = true; nrel++; }
		if (library_is_liked(t)) { nl++; if (al >= 0 && al < lib.nal && !seenl[al]) { seenl[al] = true; nrl++; } }
	}
	free(seen);
	free(seenl);
	if (nl) { y = lib_row(F_LIKED, line, y, W, nl, nrl, true); line++; }
	if (ap_nsongs) { y = lib_row(F_LOCAL, line, y, W, ap_nsongs, nrel, false); line++; }
	if (nl || ap_nsongs) y += 8;

	if (a) {
		// Discography, with its chips
		MRelease *v;
		int nd = ap_disc(&v);
		if (nd) {
			y = heading("Discography", y);
			static const char *CH[] = {"Popular releases", "Albums", "Singles and EPs"};
			int have[3] = {a->npop, a->nalb, a->nsing};
			float x = 12;
			int shown = e->chip == 0 && a->npop ? 0 : e->chip <= 1 && a->nalb ? 1 : 2;
			for (int c = 0; c < 3; c++) {
				if (!have[c]) continue;
				float w = measure(CH[c], 10) + 20;
				reg(F_CHIP, c, line, -1, x, y, w, 22);
				rrect(x, y, w, 22, 11, rgb(shown == c ? 0xffffff : CHIP));
				text(CH[c], x + 10, y + 15, 10, rgb(shown == c ? 0 : 0xffffff), 0, 0);
				if (focused(F_CHIP, c)) ring(x, y, w, 22, 11);
				x += w + 6;
			}
			y += 32;
			line++;
			cur_rel = v;
			y = shelf(0, F_DISC, line++, y, W, nd, rel_name, rel_sub, rel_img, false, rel_local);
		}
		if (a->nrel) {
			y = heading("Fans also like", y);
			y = shelf(1, F_REL, line++, y, W, a->nrel, art_name, art_sub, art_img, true, art_local);
		}
		if (a->napp) {
			y = heading("Appears on", y);
			cur_rel = a->appears;
			y = shelf(2, F_APP, line++, y, W, a->napp, rel_name, rel_sub, rel_img, false, rel_local);
		}
		if (a->ngig) {
			y = heading("On tour", y);
			static const char *MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
			for (int k = 0; k < a->ngig && k < 3; k++) {
				MGig *g = &a->gig[k];
				rrect(12, y + 2, 34, 36, 4, rgb(CHIP));
				text(g->mo >= 1 && g->mo <= 12 ? MON[g->mo - 1] : "", 29, y + 15, 8, rgb(GREY), C2D_AlignCenter, 0);
				snprintf(s, sizeof s, "%d", g->d);
				text(s, 29, y + 32, 14, rgb(0xffffff), C2D_AlignCenter | BOLD, 0);
				text(g->city ? g->city : g->title ? g->title : "", 56, y + 18, 11, rgb(0xffffff), BOLD, W - 68);
				text(g->venue ? g->venue : "", 56, y + 32, 9, rgb(GREY), 0, W - 68);
				y += 42;
			}
			if (a->gigs_total > 3) {
				snprintf(s, sizeof s, "%d more dates on Spotify", a->gigs_total - 3);
				text(s, 12, y + 10, 9, rgb(GREY), 0, 0);
				y += 18;
			}
			y += 10;
		}
		// About: their photo, the listeners, the start of the bio
		y = heading("About", y);
		float cw = W - 24, ih = ap_have[0] ? 96 : 0;
		bio_wrap(&about_w, cw - 24, 3);
		float ch = ih + 34 + (about_w.n ? about_w.n * 14 + 6 : 0) + 10;
		reg(F_ABOUT, 0, line++, -1, 12, y, cw, ch);
		rrect(12, y, cw, ch, 8, rgb(0x242424));
		if (ih) {   // the photo's middle band, the card's width
			float k = cw / MI_HEAD_W, band = ih / k, top = (MI_HEAD_H - band) / 2;
			Tex3DS_SubTexture hs = {MI_HEAD_W, (u16)band, 0, 1 - top / 256.0f, MI_HEAD_W / 512.0f, 1 - (top + band) / 256.0f};
			C2D_DrawImageAt((C2D_Image){&ap_head_tex, &hs}, 12, y, 0, NULL, k, k);
			corner(12, y, 1, 1, 8, rgb(BG));
			corner(12 + cw, y, -1, 1, 8, rgb(BG));
		}
		commas(n1, sizeof n1, a->listeners);
		snprintf(s, sizeof s, "%s monthly listeners", n1);
		text(s, 24, y + ih + 24, 12, rgb(0xffffff), BOLD, cw - 24);
		for (int i = 0; i < about_w.n; i++) {
			char b[512];
			snprintf(b, sizeof b, "%.*s", about_w.ln[i] > 500 ? 500 : about_w.ln[i], a->bio + about_w.st[i]);
			text(b, 24, y + ih + 44 + i * 14, 10, rgb(0xdddddd), 0, i == 2 ? cw - 24 - 14 : cw - 24);
		}
		if (focused(F_ABOUT, 0)) ring(12, y, cw, ch, 8);
		y += ch + 12;
		text("From Spotify's artist page", 12, y + 8, 8, rgba(GREY, 150), 0, 0);
		y += 20;
	}
	if (ap_rec) {
		ap_maxsc = y + sc - H;
		if (ap_maxsc < 0) ap_maxsc = 0;
	}

	// once the photo's gone, a bar in their colour with the name and a play button
	float t = (sc - (hh - 56)) / 26;
	if (t > 0) {
		if (t > 1) t = 1;
		rect(0, 0, W, 30, rgba(mixc(col, 0, 0.5f), (u8)(255 * t)));
		text(name, 36, 20, 13, rgba(0xffffff, (u8)(255 * t)), BOLD, W - 80);
		C2D_DrawCircleSolid(W - 20, 15, 0, 11, rgba(GREEN, (u8)(255 * t)));
		if (ap_playing() && ai->open && !ai->paused) ic_pause(W - 20, 15, 8, rgba(0, (u8)(255 * t)));
		else ic_play(W - 18.5f, 15, 8, rgba(0, (u8)(255 * t)));
	}
	rrect(6, 5, 22, 22, 11, rgba(0, 120));
	ic_back(16.5f, 16, rgb(0xffffff));
}

// ---- the D-pad, a finger -----------------------------------------------------------------

static int ap_find(int kind, int k) {
	for (int i = 0; i < nfoc; i++) if (foc[i].kind == kind && foc[i].k == k) return i;
	return -1;
}
static float foc_cx(const Foc *f) { return f->x - (f->shelf >= 0 ? AP->hs[f->shelf] : 0) + f->w / 2; }

static void ap_show(const Foc *f, float H) {   // scroll so it's in view
	float top = 34, sc = ease_cur(&AP->scroll);
	if (f->line == 0) sc = 0;
	else if (f->y - sc < top) sc = f->y - top;
	else if (f->y + f->h - sc > H - 4) sc = f->y + f->h - (H - 4);
	ease_to(&AP->scroll, sc < 0 ? 0 : sc > ap_maxsc ? ap_maxsc : sc);
	if (f->shelf >= 0) {   // and sideways, inside its shelf
		float W = list_w(), hs = ease_cur(&AP->hs[f->shelf]), x = f->x - hs;
		if (x < 12) hs = f->x - 12;
		else if (x + f->w > W - 12) hs = f->x + f->w - (W - 12);
		ease_to(&AP->hs[f->shelf], hs < 0 ? 0 : hs);
	}
}

static void ap_keys(u32 kd, u32 kr, const AudioInfo *ai) {
	float H = LIST1;
	if (kd & KEY_B) { ap_back(); return; }
	if (ap_bio) {
		if (kr & (KEY_DUP | KEY_DDOWN)) {
			ap_bio_scroll += kr & KEY_DUP ? -30 : 30;
			ap_bio_scroll = ap_bio_scroll < 0 ? 0 : ap_bio_scroll > ap_bio_max ? ap_bio_max : ap_bio_scroll;
		}
		if (kd & KEY_DLEFT) ap_back();
		return;
	}
	if (!nfoc) return;
	int fi = ap_find(AP->fk, AP->fi);
	if (fi < 0) fi = 0;
	if (!cursor_vis && (kr & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT) || kd & KEY_A)) {
		// the first press shows the cursor on the first thing in view
		cursor_vis = true;
		for (int i = 0; i < nfoc; i++) if (foc[i].y - AP->scroll >= 30 || i == nfoc - 1) { fi = i; break; }
		AP->fk = foc[fi].kind; AP->fi = foc[fi].k;
		ap_show(&foc[fi], H);
		return;
	}
	Foc *f = &foc[fi];
	int to = -1;
	if (kr & (KEY_DUP | KEY_DDOWN)) {
		int dir = kr & KEY_DUP ? -1 : 1, want = -1;
		for (int i = 0; i < nfoc; i++)
			if ((foc[i].line - f->line) * dir > 0 && (want < 0 || abs(foc[i].line - f->line) < abs(want - f->line))) want = foc[i].line;
		float best = 1e9, cx = foc_cx(f);
		for (int i = 0; i < nfoc; i++)
			if (foc[i].line == want && fabsf(foc_cx(&foc[i]) - cx) < best) { best = fabsf(foc_cx(&foc[i]) - cx); to = i; }
		if (to < 0 && dir < 0) ease_to(&AP->scroll, 0);
	} else if (kr & (KEY_DLEFT | KEY_DRIGHT)) {
		int dir = kr & KEY_DLEFT ? -1 : 1;
		float best = 1e9;
		for (int i = 0; i < nfoc; i++)
			if (i != fi && foc[i].line == f->line && (foc[i].x - f->x) * dir > 0 && fabsf(foc[i].x - f->x) < best) { best = fabsf(foc[i].x - f->x); to = i; }
		if (to < 0 && dir < 0 && kd & KEY_DLEFT) { bool alone = true; for (int i = 0; i < nfoc; i++) if (i != fi && foc[i].line == f->line) alone = false; if (alone) { ap_back(); return; } }
		if (to < 0)   // off the end of a shelf or a row of buttons: round to its other end
			for (int i = 0; i < nfoc; i++)
				if (i != fi && foc[i].line == f->line && (to < 0 || (dir > 0 ? foc[i].x < foc[to].x : foc[i].x > foc[to].x))) to = i;
	}
	if (to >= 0) { AP->fk = foc[to].kind; AP->fi = foc[to].k; ap_show(&foc[to], H); }
	if (kd & KEY_A) ap_activate(f->kind, f->k, ai);
}

// a tap at x, y on the touchscreen (the page is there when the list is)
static void ap_tap(int x, int y, const AudioInfo *ai) {
	if (y < 30 && x < 36) { ap_back(); return; }
	if (ap_bio) return;
	if (y < 30 && x > list_w() - 36 && AP->scroll > 176 * list_w() / 400 - 56) { ap_activate(F_PLAY, 0, ai); return; }
	for (int i = nfoc - 1; i >= 0; i--) {
		Foc *f = &foc[i];
		float fx = f->x - (f->shelf >= 0 ? AP->hs[f->shelf] : 0), fy = f->y - AP->scroll;
		if (x >= fx && x < fx + f->w && y >= fy && y < fy + f->h) {
			AP->fk = f->kind; AP->fi = f->k;
			ap_activate(f->kind, f->k, ai);
			return;
		}
	}
}

static int ap_shelf_at(int y) {   // which shelf a finger came down on, -1 none
	for (int i = 0; i < nfoc; i++) {
		float fy = foc[i].y - AP->scroll;
		if (foc[i].shelf >= 0 && y >= fy && y < fy + foc[i].h) return foc[i].shelf;
	}
	return -1;
}

// what X likes on the page: a Popular song of yours under the cursor, -1 for none
static int ap_x_target(void) {
	if (!ap_active() || ap_bio || !cursor_vis || !ap_a || AP->fk != F_TOP || AP->fi >= ap_a->ntop) return -1;
	return ap_a->top[AP->fi].local;
}
