# Host test of queueview.inc's swipe/reorder state machine with simulated touches: python -I player/test/queue_gesture_test.py player/source/queueview.inc out.c, then gcc -lm -fsanitize=address,undefined out.c && ./a.out
import sys
inc = open(sys.argv[1], 'rb').read().decode('utf-8').replace('\r\n', '\n')
def cut(a, b): return inc[inc.index(a):inc.index(b)]
p1 = cut('#define QW', 'static void queue_view_make')
p2 = cut('static void queue_reset', "// Spotify's icon")
p3 = cut('static float qrow_h', 'static void draw_qrow')
p4 = cut('static void queue_anim', 'static void draw_queue')
p5 = inc[inc.index('// ---- touches'):]
h = r'''
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
typedef int C3D_Tex;
typedef int LightLock;
typedef struct { int track; } AudioInfo;
static bool lyr_on;
static int qn = 20, qpos, qadd;
static LightLock qlock;
static void LightLock_Lock(LightLock *l) { (void)l; }
static void LightLock_Unlock(LightLock *l) { (void)l; }
static int q_play[64];
typedef unsigned long long u64;
typedef unsigned char u8;
static u64 fake_ms;
static u64 osGetTime(void) { return fake_ms; }
#define rgba(a, b) 0
#define rgb(a) 0
#define GREEN 0
static void say(const char *s) { printf("   say: %s\n", s); }
static bool connect_driving(void) { return false; }
static void audio_play(int t) { printf("   audio_play(%d)\n", t); }
static int removed = -9, mv_a = -9, mv_b = -9;
static void queue_remove(int k) { removed = k; }
static void queue_move(int a, int b) { mv_a = a; mv_b = b; }
static int restored, restore_k = 3;
static bool undo_ready;
static bool queue_undo_ok(void) { return undo_ready; }
static int queue_restore(void) { restored++; undo_ready = false; return restore_k; }
''' + p1 + p2 + p3 + p4 + p5 + r'''
static void frame(void) { queue_layout(); queue_anim(320, 34, 182); queue_layout(); for (int i = 0; i < qrow_n; i++) if (qrows[i].kind == 2) { int k = qrows[i].k; if (!qe_ok) qe[k] = qrows[i].y; else qe[k] += (qrows[i].y - qe[k]) * 0.4f; } qe_ok = true; }
static float row_screen_y(int k) { return 34 + 4 + qe[k] - q_y + 10; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void setup(void) {
	queue_reset();
	q_view_n = 8;
	for (int k = 0; k < 8; k++) { q_view[k] = 100 + k; q_viewq[k] = k < 2; }
	removed = mv_a = mv_b = -9;
	restored = 0; undo_ready = false;
	frame();
}
static int rest_ms;
static void drag(int x0, int y0, int x1, int y1, int steps) {
	bool dragging = false;
	queue_touch_begin(x0, y0);
	for (int s = 1; s <= steps; s++) {
		int x = x0 + (x1 - x0) * s / steps, y = y0 + (y1 - y0) * s / steps;
		fake_ms += 16;
		queue_touch_move(x, y, x0, y0, &dragging);
		frame();
	}
	fake_ms += rest_ms;
	if (qg_mode) queue_touch_end();
}
static void settle(int n) { for (int i = 0; i < n; i++) frame(); }
int main(void) {
	printf("hesitant swipe (-40 px) opens the tab\n");
	setup();
	int y = (int)row_screen_y(3);
	drag(250, y, 210, y, 8);
	settle(40);
	printf("   k=%d off=%.1f goal=%.1f going=%d removed=%d\n", qx_k, qx_off, qx_goal, qx_going, removed);
	CHECK(qx_k == 3 && qx_goal == -QS_OPEN && !qx_going && removed == -9);
	printf("tap on its trash removes\n");
	queue_touch_begin(300, y); queue_touch_end();
	queue_tap(300, y, NULL);
	settle(80);
	printf("   removed=%d qx_k=%d\n", removed, qx_k);
	CHECK(removed == 3 && qx_k == -1);
	printf("a slow middling swipe (-150 px, 47 percent) holds open, it does not remove\n");
	setup();
	y = (int)row_screen_y(4);
	drag(280, y, 130, y, 30);
	settle(80);
	printf("   removed=%d qx_k=%d goal=%.0f\n", removed, qx_k, qx_goal);
	CHECK(removed == -9 && qx_k == 4 && qx_goal == -QS_OPEN);
	printf("a far confident swipe (-220 px, 69 percent) removes\n");
	setup();
	y = (int)row_screen_y(4);
	drag(300, y, 80, y, 10);
	settle(120);
	printf("   removed=%d qx_k=%d\n", removed, qx_k);
	CHECK(removed == 4 && qx_k == -1);
	printf("far but the finger stops before letting go: holds open (nervous)\n");
	setup();
	y = (int)row_screen_y(4);
	rest_ms = 400;
	drag(300, y, 80, y, 10);
	rest_ms = 0;
	settle(120);
	printf("   removed=%d qx_k=%d goal=%.0f\n", removed, qx_k, qx_goal);
	CHECK(removed == -9 && qx_k == 4 && qx_goal == -QS_OPEN);
	printf("a real flick (-130 px, 2 frames) removes\n");
	setup();
	y = (int)row_screen_y(2);
	drag(270, y, 140, y, 2);
	settle(120);
	printf("   removed=%d qx_k=%d\n", removed, qx_k);
	CHECK(removed == 2);
	printf("a slow drag that stops short, then lets go, holds open\n");
	setup();
	y = (int)row_screen_y(6);
	rest_ms = 300;
	drag(260, y, 205, y, 12);
	rest_ms = 0;
	settle(60);
	printf("   removed=%d qx_k=%d goal=%.0f\n", removed, qx_k, qx_goal);
	CHECK(removed == -9 && qx_k == 6 && qx_goal == -QS_OPEN);
	printf("a tiny swipe springs shut\n");
	setup();
	y = (int)row_screen_y(5);
	drag(250, y, 240, y, 6);
	settle(60);
	printf("   qx_k=%d off=%.1f removed=%d\n", qx_k, qx_off, removed);
	CHECK(qx_k == -1 && removed == -9);
	printf("reorder from the handle\n");
	setup();
	y = (int)row_screen_y(1);
	drag(312, y, 312, y + 70, 10);
	printf("   move %d -> %d\n", mv_a, mv_b);
	CHECK(mv_a == 1 && mv_b > 1);
	printf("handle drag up\n");
	setup();
	y = (int)row_screen_y(6);
	drag(312, y, 312, y - 60, 10);
	printf("   move %d -> %d\n", mv_a, mv_b);
	CHECK(mv_a == 6 && mv_b < 6);
	printf("scroll by dragging a row body\n");
	setup();
	y = (int)row_screen_y(3);
	q_y = 0;
	drag(100, y, 100, y - 40, 8);
	printf("   q_y=%.1f removed=%d move=%d\n", q_y, removed, mv_a);
	CHECK(removed == -9 && mv_a == -9);
	printf("undo: a swipe right after a removal brings the row back in\n");
	setup();
	undo_ready = true;
	y = (int)row_screen_y(5);
	drag(60, y, 190, y, 8);
	CHECK(restored == 1 && qx_k == restore_k && qx_in && qx_h == 0);
	settle(4);
	CHECK(qx_k == restore_k && qx_h > 0.3f && qx_off < 0);
	settle(60);
	printf("   restored=%d qx_k=%d h=%.2f off=%.1f\n", restored, qx_k, qx_h, qx_off);
	CHECK(qx_k == -1 && qx_h == 1 && qx_off == 0 && removed == -9);
	printf("undo: nothing to undo, a swipe right does nothing\n");
	setup();
	y = (int)row_screen_y(5);
	drag(60, y, 190, y, 8);
	settle(20);
	CHECK(restored == 0 && qx_k == -1);
	printf("undo: too short a swipe right does nothing, and the undo stays\n");
	setup();
	undo_ready = true;
	y = (int)row_screen_y(5);
	drag(60, y, 100, y, 6);
	settle(20);
	CHECK(restored == 0 && qx_k == -1 && undo_ready);
	printf("undo: a quick flick right (60 px) counts\n");
	setup();
	undo_ready = true;
	y = (int)row_screen_y(2);
	drag(80, y, 140, y, 2);
	CHECK(restored == 1);
	printf("undo: a swipe right that starts on empty space (a header) works too\n");
	setup();
	undo_ready = true;
	drag(60, 40, 200, 40, 8);
	CHECK(restored == 1);
	printf("undo: scrolling still scrolls with an undo waiting\n");
	setup();
	undo_ready = true;
	y = (int)row_screen_y(3);
	drag(100, y, 100, y - 40, 8);
	CHECK(restored == 0 && undo_ready);
	printf("tap jumps\n");
	setup();
	y = (int)row_screen_y(3);
	queue_touch_begin(100, y); queue_touch_end(); queue_tap(100, y, NULL);
	printf(fails ? "FAILED %d\n" : "all ok\n", fails);
	return fails != 0;
}
'''
open(sys.argv[2], 'w', newline='\n').write(h)
