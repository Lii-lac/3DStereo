// The queue page, Spotify's way: a chip beside the lyrics chip (Spotify's own queue icon: a hollow rounded bar over
// two lines), and a page in the cover's place: the song playing, what was queued by hand ("Next in queue"),
// then the rest ("Next from <where it came from>"). While Spotify drives it's Spotify's own order; otherwise the
// 3DS's. On the touchscreen: drag to scroll; a tap on a song jumps to it (the 3DS's own queue); swipe a row left
// and a red tab with a trash can uncovers: a hesitant swipe leaves it open (tap it to remove), a confident one
// carries through and removes; the handle on the right lifts a row to drag it somewhere else.
#define QW 320            // the touchscreen's width, where every touch comes from
#define QR_H 28
#define QH_H 20
#define QS_OPEN 56        // the red tab's width
#define QS_RED 0xd8404a
static bool q_on;          // the page is up
static float q_y, q_y0;    // scrolled by this much; where a drag began
static int q_view[64];     // what's coming, as library indices (-1: a song that isn't on the card)
static bool q_viewq[64];   // ...put in the queue by hand
static int q_view_n;
static C3D_Tex qi_tex;
static bool qi_ok;
typedef struct { float y; int kind, k; } QRow;   // kind 0: a header (k: 0 now playing, 1 queue, 2 context), 1: the playing song, 2: view[k]
static QRow qrows[140];
static int qrow_n;
static float qtotal;
static float qe[64];        // each row's displayed y, easing to its place
static bool qe_ok;          // ...false: take the places at once
static float qid_y[64];     // where each sat when a reorder began
// gestures
static int qg_mode;         // 0 none, 1 scroll, 2 swipe, 3 reorder, 4 swipe right to undo a removal
static int qg_k = -1;       // the row the touch began on
static bool qg_handle;      // ...on the handle
static float qs_base, qs_v;
static int qs_lx;
static u64 qs_mv;            // when the finger last moved on a swipe: the pause before letting go is hesitation
static int qx_k = -1;       // the row slid left, showing its trash (or on its way out)
static float qx_off, qx_goal, qx_h = 1;
static bool qx_going;
static bool qx_in;          // ...or a row coming back (undo): it opens a gap while the red tab draws back
static float qu_dx;         // an undo swipe's travel
static int qr_from = -1, qr_to;   // a row lifted, and where it'd land
static float qr_fy;         // ...the finger's y

static void queue_view_make(void) {
	if (connect_driving()) { q_view_n = connect_queue_view(q_view, q_viewq, 64); return; }
	q_view_n = 0;
	LightLock_Lock(&qlock);
	for (int k = 1; k <= 64 && qn > 1; k++) {
		int at = qpos + k;
		if (at >= qn) { if (repeat != 1) break; at %= qn; if (at == qpos) break; }
		q_view[q_view_n] = q_play[at];
		q_viewq[q_view_n++] = k <= qadd;
	}
	LightLock_Unlock(&qlock);
}
static void queue_reset(void) { qg_mode = 0; qg_k = -1; qx_k = -1; qx_off = qx_goal = 0; qx_h = 1; qx_going = false; qx_in = false; qr_from = -1; qe_ok = false; q_y = 0; }
static void queue_toggle(void) { q_on = !q_on; queue_reset(); if (q_on) lyr_on = false; }

// Spotify's icon (measured off its own, 23 x 24 px): a rounded bar drawn as an outline, a gap, two bars. In a
// 32² texture at (4, 4), alpha = the shape's coverage, white; tinted when it's drawn.
static bool qi_in(float fx, float fy) {
	float x = fx - 4, y = fy - 4;
	if (x < 0 || x > 23.4f) return false;
	if (y >= 0.2f && y <= 8.9f) {   // the outline: 2 px wide, corners round
		float w = 2.0f, r = 3.0f, cx = x < r ? r : x > 23.4f - r ? 23.4f - r : x, cy = y < 0.2f + r ? 0.2f + r : y > 8.9f - r ? 8.9f - r : y;
		float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
		bool corner = (x < r || x > 23.4f - r) && (y < 0.2f + r || y > 8.9f - r);
		if (corner) return d <= r && d >= r - w;
		return x < w || x > 23.4f - w || y < 0.2f + w || y > 8.9f - w;
	}
	return (y >= 14.0f && y <= 16.6f) || (y >= 21.8f && y <= 24.4f);
}
static void qi_make(void) {
	if (!(qi_ok = C3D_TexInit(&qi_tex, 32, 32, GPU_RGBA8))) return;
	C3D_TexSetFilter(&qi_tex, GPU_LINEAR, GPU_LINEAR);
	static u32 px[32 * 32];
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			int n = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) n += qi_in(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
			px[y * 32 + x] = 0xFFFFFF00 | (u32)(n * 255 / 16);
		}
	put_rect(&qi_tex, 0, 0, px, 32, 32);
}
static void queue_chip(float W) {   // beside the lyrics chip, the same size and colours: grey, green with a dot under it while the page is up
	float cx = W - 47, cy = 16;
	C2D_DrawCircleSolid(cx, cy, 0, 11, rgba(0, 70));
	if (!qi_ok) return;
	C2D_ImageTint t;
	C2D_PlainImageTint(&t, q_on ? rgb(GREEN) : rgb(0xd8d8d8), 1);
	Tex3DS_SubTexture st = {32, 32, 0, 1, 1, 0};
	const float qs = 18 * 0.97f;   // 3% under the lyrics icon's size: Spotify's glyph is wider than its mic, and filled the circle
	C2D_DrawImageAt((C2D_Image){&qi_tex, &st}, cx - qs / 2, cy - qs / 2, 0, &t, qs / 32, qs / 32);
	if (q_on) C2D_DrawCircleSolid(cx, cy + 9.2f, 0, 1.2f, rgb(GREEN));
}

static void ic_trash(float x, float y, float s, u32 c, u32 hole) {   // a lidded can, 12 px wide at s = 1
	rect(x - 6 * s, y - 5.2f * s, 12 * s, 1.7f * s, c);                    // the lid
	rect(x - 2 * s, y - 7.2f * s, 4 * s, 2.2f * s, c);                     // its handle
	C2D_DrawTriangle(x - 4.6f * s, y - 3.0f * s, c, x + 4.6f * s, y - 3.0f * s, c, x - 3.7f * s, y + 6.6f * s, c, 0);   // the body, a trapezoid
	C2D_DrawTriangle(x + 4.6f * s, y - 3.0f * s, c, x + 3.7f * s, y + 6.6f * s, c, x - 3.7f * s, y + 6.6f * s, c, 0);
	rect(x - 1.9f * s, y - 1.2f * s, 0.9f * s, 5.4f * s, hole);            // the slots
	rect(x + 1.0f * s, y - 1.2f * s, 0.9f * s, 5.4f * s, hole);
}

static float qrow_h(int k) { return k == qx_k ? QR_H * qx_h : QR_H; }

// The page's rows, top to bottom, with their places: the song playing, then the queued ones, then the rest. While a
// row is lifted the places are the ones it would leave behind.
static void queue_layout(void) {
	int n = q_view_n, ordv[64], nq = 0;
	for (int k = 0; k < n; k++) { ordv[k] = k; if (q_viewq[k]) nq++; }
	int nqv = nq;
	if (qg_mode == 3 && qr_from >= 0 && qr_from < n) {
		int v = qr_from, to = qr_to < 0 ? 0 : qr_to >= n ? n - 1 : qr_to, tmp[64], m = 0;
		for (int k = 0; k < n; k++) if (k != v) tmp[m++] = k;
		for (int p = 0; p < n; p++) ordv[p] = p < to ? tmp[p] : p == to ? v : tmp[p - 1];
		int nq_ex = nq - (v < nq ? 1 : 0);
		nqv = nq_ex + ((v < nq ? to <= nq_ex : to < nq_ex) ? 1 : 0);
	}
	float y = 0;
	qrow_n = 0;
	qrows[qrow_n++] = (QRow){y, 0, 0}; y += QH_H;
	qrows[qrow_n++] = (QRow){y, 1, 0}; y += QR_H;
	if (nqv) { qrows[qrow_n++] = (QRow){y, 0, 1}; y += QH_H; }
	for (int p = 0; p < n && qrow_n < 138; p++) {
		if (p == nqv && n > nqv) { qrows[qrow_n++] = (QRow){y, 0, 2}; y += QH_H; }
		qrows[qrow_n++] = (QRow){y, 2, ordv[p]};
		y += qrow_h(ordv[p]);
	}
	qtotal = y;
}
static int qrow_at(float sy) {   // the song's row under a touchscreen y, -1 if none
	float py = sy - 34 - 4 + q_y;
	for (int i = 0; i < qrow_n; i++)
		if (qrows[i].kind == 2 && py >= qe[qrows[i].k] && py < qe[qrows[i].k] + qrow_h(qrows[i].k)) return qrows[i].k;
	return -1;
}

static void draw_qrow(float W, float y, int k, float off, float hs, bool lifted) {
	float rh = QR_H * hs;
	if (hs < 0.04f) return;
	u32 white = rgb(0xffffff);
	int tr = q_view[k];
	if (lifted) {   // lifted: a card over the rest, with a shadow
		rrect(4, y + 2, W - 8, rh, 5, rgba(0, 90));
		rrect(4, y, W - 8, rh, 5, rgba(0xffffff, 40));
	}
	if (off < -0.5f) {   // the red tab uncovered from the right; the can sits at the screen's edge and is uncovered, it doesn't travel
		float rw = -off, pop = rw > QW * 0.62f ? 1.2f : 1, vis = (rw - 12) / 26;
		vis = vis < 0 ? 0 : vis > 1 ? 1 : vis;
		rect(W + off, y, rw, rh, rgb(QS_RED));
		if (hs > 0.5f && vis > 0) ic_trash(W - 28, y + rh / 2, 0.9f * pop, rgba(0xffffff, (u8)(255 * vis)), rgb(QS_RED));
	}
	if (hs < 0.6f) return;
	float x = off;
	if (tr < 0 || tr >= lib.nt) {   // Spotify has it, the card doesn't
		rrect(16 + x, y + 3, 22, 22, 2, rgba(0xffffff, 40));
		text("Not on this 3DS", 46 + x, y + 17, 10, rgba(0xffffff, 120), 0, W - 100);
	} else {
		Track *t = &lib.t[tr];
		thumb(t->album_id, 16 + x, y + 3, 22);
		text(t->title, 46 + x, y + 12, 11, white, BOLD, W - 100);
		text(t->artist, 46 + x, y + 24, 9, rgba(0xffffff, 170), 0, W - 100);
	}
	float hx = W - 22 + x;   // the handle: three lines
	for (int l = 0; l < 3; l++) rect(hx, y + 9 + l * 4, 12, 1.4f, rgba(0xffffff, lifted ? 200 : 90));
}

static void queue_anim(float W, float top, float bot) {
	// the row slid left settles where it was let go
	if (qx_k >= 0 && qg_mode != 2 && qx_in) {   // a row coming back: the gap opens and the row slides in from the left, the red tab drawing back
		qx_h += 0.12f;
		if (qx_h > 1) qx_h = 1;
		qx_off += (0 - qx_off) * 0.28f;
		if (qx_off > -0.6f) qx_off = 0;
		qx_goal = 0;
		if (qx_h >= 1 && qx_off == 0) { qx_in = false; qx_k = -1; }
	} else if (qx_k >= 0 && qg_mode != 2) {
		qx_off += (qx_goal - qx_off) * 0.32f;
		if (fabsf(qx_goal - qx_off) < 0.6f) qx_off = qx_goal;
		if (qx_going && qx_off <= -W + 2) {   // out of sight: the rows close the gap, then it's gone
			qx_h -= 0.11f;
			if (qx_h <= 0) { int k = qx_k; qx_k = -1; qx_going = false; qx_h = 1; qx_off = qx_goal = 0; queue_remove(k); qe_ok = false; }
		} else if (!qx_going && qx_goal == 0 && qx_off == 0) qx_k = -1;
	}
	// a lifted row near an edge scrolls the page
	if (qg_mode == 3) {
		float h = bot - top - 4, maxy = qtotal > h ? qtotal - h : 0;
		if (qr_fy < top + 18) q_y -= 3;
		else if (qr_fy > bot - 18) q_y += 3;
		q_y = q_y < 0 ? 0 : q_y > maxy ? maxy : q_y;
	}
}

static void draw_queue(float W, const AudioInfo *ai, float top, float bot) {
	queue_view_make();
	if (qx_k >= q_view_n) { qx_k = -1; qx_off = qx_goal = 0; qx_h = 1; qx_going = false; qx_in = false; }
	if (qr_from >= q_view_n) { qr_from = -1; qg_mode = 0; }
	queue_layout();
	queue_anim(W, top, bot);
	queue_layout();
	float h = bot - top - 4, maxy = qtotal > h ? qtotal - h : 0;
	q_y = q_y < 0 ? 0 : q_y > maxy ? maxy : q_y;
	for (int i = 0; i < qrow_n; i++) {
		if (qrows[i].kind != 2) continue;
		int k = qrows[i].k;
		if (!qe_ok) qe[k] = qrows[i].y;
		else { qe[k] += (qrows[i].y - qe[k]) * 0.4f; if (fabsf(qrows[i].y - qe[k]) < 0.3f) qe[k] = qrows[i].y; }
	}
	qe_ok = true;
	for (int i = 0; i < qrow_n; i++) {
		float y = top + 4 + qrows[i].y - q_y;
		if (qrows[i].kind == 0) {
			if (y < top || y + QH_H > bot) continue;
			char b[340];
			const char *s = qrows[i].k == 0 ? "Now playing" : qrows[i].k == 1 ? "Next in queue" : b;
			if (qrows[i].k == 2) snprintf(b, sizeof b, "Next from %s", src_name);
			text(s, 16, y + 13, 10, rgba(0xffffff, 200), BOLD, W - 32);
			continue;
		}
		if (qrows[i].kind == 1) {
			if (y < top || y + QR_H > bot) continue;
			if (ai->track < 0 || ai->track >= lib.nt) continue;
			Track *t = &lib.t[ai->track];
			thumb(t->album_id, 16, y + 3, 22);
			text(t->title, 46, y + 12, 11, rgb(GREEN), BOLD, W - 62);
			text(t->artist, 46, y + 24, 9, rgba(0xffffff, 170), 0, W - 62);
			continue;
		}
		int k = qrows[i].k;
		if (qg_mode == 3 && k == qr_from) continue;   // drawn last, under the finger
		float ry = top + 4 + qe[k] - q_y, rh = qrow_h(k);
		if (ry < top || ry + rh > bot) continue;       // whole rows only
		draw_qrow(W, ry, k, k == qx_k ? qx_off : 0, k == qx_k ? qx_h : 1, false);
	}
	if (qg_mode == 3 && qr_from >= 0 && qr_from < q_view_n) {
		float ly = qr_fy - QR_H / 2;
		ly = ly < top ? top : ly > bot - QR_H ? bot - QR_H : ly;
		draw_qrow(W, ly, qr_from, 0, 1, true);
	}
	if (qtotal > h) {   // a thin scroll mark at the right edge
		float bh = (bot - top) * h / qtotal, by = top + (bot - top - bh) * (maxy > 0 ? q_y / maxy : 0);
		rrect(W - 4, by, 2, bh, 1, rgba(0xffffff, 70));
	}
}

// ---- touches (from the touchscreen, only while the page is up on it) ----------------------------------------------
static void queue_places(void);
static void queue_touch_begin(int x, int y) {
	queue_places();
	qg_mode = 0;
	qg_k = qrow_at(y);
	qg_handle = x >= QW - 44;
	qs_v = 0;
	qs_lx = x;
	qs_mv = osGetTime();
	qs_base = qg_k >= 0 && qg_k == qx_k ? qx_off : 0;
	if (qx_k >= 0 && !qx_going && qg_k != qx_k) qx_goal = 0;   // a touch anywhere else shuts an open tab
	q_y0 = q_y;
}
static void queue_touch_move(int x, int y, int x0, int y0, bool *dragging) {
	int dx = x - x0, dy = y - y0;
	if (qg_mode == 0) {
		if (qg_k >= 0 && !qx_going && abs(dx) > 8 && abs(dx) * 10 > abs(dy) * 12 && (dx < 0 || qg_k == qx_k)) {
			qg_mode = 2;
			if (qx_k != qg_k) { qx_k = qg_k; qx_off = qs_base = 0; qx_h = 1; }
			qx_in = false;
		} else if (dx > 8 && abs(dx) * 10 > abs(dy) * 12 && !qx_going && (qg_k < 0 || qg_k != qx_k) && queue_undo_ok()) {   // right after a removal: a swipe back puts it back
			qg_mode = 4;
			qu_dx = 0;
		} else if (abs(dy) > 6 && abs(dy) >= abs(dx)) {
			if (qg_k >= 0 && qg_handle && !qx_going) {   // the handle lifts the row
				qg_mode = 3;
				qr_from = qr_to = qg_k;
				if (qx_k >= 0) { qx_goal = 0; }
			} else {
				qg_mode = 1;
				if (qx_k >= 0 && !qx_going) qx_goal = 0;
			}
		}
		if (qg_mode) *dragging = true;
	}
	if (qg_mode == 1) q_y = q_y0 - dy > 0 ? q_y0 - dy : 0;
	else if (qg_mode == 2) {
		float o = qs_base + dx;
		qx_off = o > 0 ? 0 : o < -QW ? -QW : o;
		qx_goal = qx_off;
		if (abs(x - qs_lx) >= 2) qs_mv = osGetTime();   // still moving; when it stops, that's hesitation
		qs_v = qs_v * 0.5f + (x - qs_lx) * 0.5f;
		qs_lx = x;
	} else if (qg_mode == 4) {
		qu_dx = (float)dx;
		qs_v = qs_v * 0.5f + (x - qs_lx) * 0.5f;
		qs_lx = x;
	} else if (qg_mode == 3) {
		qr_fy = y;
		float py = y - 34 - 4 + q_y, best = 1e9f;
		int to = qr_from;
		for (int k = 0; k < q_view_n; k++) {   // the nearest place, by where each row sat before the lift
			float c = qid_y[k] + QR_H / 2, d = fabsf(py - c);
			if (d < best) { best = d; to = k; }
		}
		qr_to = to;
	}
}
static void queue_touch_end(void) {
	if (qg_mode == 2) {
		// Hesitation holds the tab open: a finger that stopped before it let go, a swipe that was short or slow. Only a swipe
		// that went far (past 62% of the screen) without pausing, or a real flick, carries through and removes.
		bool paused = osGetTime() - qs_mv > 220;
		bool far = -qx_off > QW * 0.62f, flick = qs_v < -11 && -qx_off > 100;
		if (!paused && (far || flick)) { qx_goal = -QW; qx_going = true; }
		else if (-qx_off > QS_OPEN * 0.3f) qx_goal = -QS_OPEN;
		else qx_goal = 0;
	} else if (qg_mode == 4) {
		// far enough (a quarter of the screen), or a quick flick: the removed row comes back
		if (qu_dx > QW * 0.25f || (qs_v > 7 && qu_dx > 30)) {
			int k = queue_restore();
			if (k >= 0 && k < 64) { qx_k = k; qx_off = -QW; qx_goal = 0; qx_h = 0; qx_going = false; qx_in = true; qe_ok = false; }
		}
	} else if (qg_mode == 3) {
		int from = qr_from, to = qr_to;
		qr_from = -1;
		qg_mode = 0;
		if (from >= 0 && from != to) { queue_move(from, to); qe_ok = false; }
	}
	qg_mode = 0;
}
// the identity places, kept for a reorder to measure against: taken when a touch begins on a handle
static void queue_places(void) {
	int n = q_view_n;
	float y = QH_H + QR_H, nq = 0;
	for (int k = 0; k < n; k++) if (q_viewq[k]) nq++;
	if (nq) y += QH_H;
	for (int k = 0; k < n; k++) {
		if (k == (int)nq && n > (int)nq) y += QH_H;
		qid_y[k] = y;
		y += QR_H;
	}
}

// a tap on the page (x, y on the touchscreen): the trash of an open row removes it, a tap elsewhere only shuts the
// tab; otherwise the song under it plays, if the 3DS holds the queue
static void queue_tap(int x, int y, const AudioInfo *ai) {
	if (qx_k >= 0 && !qx_going) {
		if (qrow_at(y) == qx_k && x >= QW + qx_off) { qx_goal = -QW; qx_going = true; return; }
		qx_goal = 0;
		return;
	}
	if (connect_driving()) { say("Spotify's queue: skip with R"); return; }
	float py = y - 34 - 4 + q_y;
	(void)ai;
	for (int i = 0; i < qrow_n; i++) {
		if (qrows[i].kind != 2 || py < qrows[i].y || py >= qrows[i].y + QR_H) continue;
		int k = qrows[i].k, t = -1;
		LightLock_Lock(&qlock);
		if (qn > k + 1) {
			int at = qpos + k + 1;
			if (at >= qn) at %= qn;
			qpos = at;
			qadd = qadd > k + 1 ? qadd - (k + 1) : 0;   // the queued songs it jumped over are spent
			t = q_play[qpos];
		}
		LightLock_Unlock(&qlock);
		if (t >= 0) audio_play(t);
		return;
	}
}
