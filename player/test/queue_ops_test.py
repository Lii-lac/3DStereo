# Host test of main.c's queue add/remove/move: python -I player/test/queue_ops_test.py player/source/main.c out.c, then gcc -fsanitize=address,undefined out.c && ./a.out (devkitarm image has gcc)
import re, subprocess, sys, os, textwrap
src = open(sys.argv[1], 'rb').read().decode('utf-8').replace('\r\n', '\n')
a = src.index('// ---- "queue this" (circle pad down)')
b = src.index('#include "lyricsview.inc"')
body = src[a:b]
i = body.index('static void queue_add')
j = body.index(chr(10) + '}' + chr(10), i) + 3
body = body[:j]
harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef int LightLock;
static void LightLock_Lock(LightLock *l) { (void)l; }
static void LightLock_Unlock(LightLock *l) { (void)l; }
static LightLock qlock;
static int *q_orig, *q_play, qn, qpos, qadd, repeat;
static bool shuffle;
static char src_name[320];
static int src_kind, src_id;
struct { int nt; } lib = {1000};
static bool driving;
static int last_up[4], last_nu, last_nq;
static char last_say[96];
static void say(const char *s) { snprintf(last_say, sizeof last_say, "%s", s); }
static void set_queue(const int *songs, int n, int start) { q_orig = realloc(q_orig, n * sizeof(int)); q_play = realloc(q_play, n * sizeof(int)); memcpy(q_orig, songs, n * sizeof(int)); memcpy(q_play, songs, n * sizeof(int)); qn = n; qpos = start; qadd = 0; }
static bool connect_driving(void) { return driving; }
static bool connect_queue_add(int t) { (void)t; return false; }
static int connect_queue_clear(void) { return 0; }
typedef unsigned long long u64;
static u64 fake_ms;
static u64 osGetTime(void) { return fake_ms; }
typedef struct { int at, val, left, right, n; } ConnectUndo;
static bool connect_queue_remove(int k, ConnectUndo *u) { (void)k; (void)u; return false; }
static int connect_queue_restore(const ConnectUndo *u) { (void)u; return -1; }
static void connect_queue_move(int a, int b) { (void)a; (void)b; }
static void connect_upcoming(const int *n, int nu, int nq) { memcpy(last_up, n, nu * sizeof(int)); last_nu = nu; last_nq = nq; }
''' + body + r'''
static void show(const char *t) { printf("%-28s qpos=%d qadd=%d qn=%d |", t, qpos, qadd, qn); for (int i = 0; i < qn; i++) printf(" %d", q_play[i]); printf("\n"); }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void sorted_same(void) {   // q_orig and q_play hold the same songs
	int a[64], b[64];
	memcpy(a, q_play, qn * sizeof(int)); memcpy(b, q_orig, qn * sizeof(int));
	for (int i = 0; i < qn; i++) for (int j = i + 1; j < qn; j++) { if (a[j] < a[i]) { int t = a[i]; a[i] = a[j]; a[j] = t; } if (b[j] < b[i]) { int t = b[i]; b[i] = b[j]; b[j] = t; } }
	CHECK(!memcmp(a, b, qn * sizeof(int)));
}
int main(void) {
	int base[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
	set_queue(base, 10, 2);
	show("start (playing 12)");
	int s1 = 100; queue_add(&s1, 1); show("add 100");
	CHECK(q_play[3] == 100 && qadd == 1 && qn == 11);
	int s2[2] = {101, 102}; queue_add(s2, 2); show("add 101 102");
	CHECK(q_play[4] == 101 && q_play[5] == 102 && qadd == 3 && qn == 13);
	CHECK(last_nu == 4 && last_nq == 3 && last_up[0] == 100);
	sorted_same();
	queue_remove(1); show("remove view 1 (101)");
	CHECK(qadd == 2 && qn == 12 && q_play[3] == 100 && q_play[4] == 102);
	sorted_same();
	queue_move(0, 4); show("move 100 to view 4");
	CHECK(q_play[3] == 102 && q_play[7] == 100 && qadd == 1);   // 100 left the queued block, 102 stays
	sorted_same();
	queue_move(5, 0); show("move view5 into the queue");
	CHECK(qadd == 2);
	sorted_same();
	queue_remove(0); queue_remove(0); show("remove two");
	CHECK(qadd == 0);
	sorted_same();
	/* the playing song is never removed, out of range does nothing */
	int before = qn; queue_remove(99); CHECK(qn == before);
	queue_move(0, 99); CHECK(qn == before);
	/* shuffle on: q_orig is untouched by a move, a remove takes one copy out of it */
	shuffle = true;
	int c1 = 200; queue_add(&c1, 1); show("shuffled add");
	sorted_same();
	queue_move(0, 3); show("shuffled move");
	sorted_same();
	queue_remove(0); show("shuffled remove"); sorted_same();
	/* repeat all: the view wraps past the end */
	shuffle = false; repeat = 1;
	set_queue(base, 10, 8);
	queue_remove(3); show("repeat-all remove across the end");
	CHECK(qn == 9);
	sorted_same();
	/* clear: the songs queued by hand go, the rest stays exactly as it was */
	shuffle = false; repeat = 0;
	set_queue(base, 10, 2);
	int q3[3] = {300, 301, 302}; queue_add(q3, 3);
	queue_clear(); show("clear");
	CHECK(qadd == 0 && qn == 10 && q_play[3] == 13);
	sorted_same();
	for (int i = 0; i < 10; i++) CHECK(q_play[i] == base[i]);
	queue_clear(); CHECK(qn == 10);   /* already empty: nothing happens */
	/* undo: the last removed song goes back where it was */
	shuffle = false; repeat = 0;
	set_queue(base, 10, 2);
	int u1[2] = {400, 401}; queue_add(u1, 2);
	int snap[16], sqn = qn, sqadd = qadd, sqpos = qpos;
	memcpy(snap, q_play, qn * sizeof(int));
	queue_remove(1); show("undo: remove queued 401");
	CHECK(qn == sqn - 1 && qadd == sqadd - 1 && queue_undo_ok());
	CHECK(queue_restore() == 1); show("undo: restored");
	CHECK(qn == sqn && qadd == sqadd && qpos == sqpos && !memcmp(q_play, snap, qn * sizeof(int)));
	sorted_same();
	CHECK(!queue_undo_ok() && queue_restore() == -1);   /* only once */
	queue_remove(5); show("undo: remove a plain one");
	CHECK(qadd == sqadd);
	CHECK(queue_restore() == 5); show("undo: restored");
	CHECK(qn == sqn && qadd == sqadd && !memcmp(q_play, snap, qn * sizeof(int)));
	sorted_same();
	queue_remove(0); show("undo: remove the first queued one");
	CHECK(queue_restore() == 0);
	CHECK(qn == sqn && qadd == sqadd && !memcmp(q_play, snap, qn * sizeof(int)));
	sorted_same();
	queue_remove(sqn - 4); show("undo: remove the last one");
	CHECK(queue_restore() == sqn - 4);
	CHECK(qn == sqn && !memcmp(q_play, snap, qn * sizeof(int)));
	sorted_same();
	/* it only goes back into the queue it left */
	queue_remove(3);
	int u2 = 500; queue_add(&u2, 1);
	CHECK(queue_restore() == -1);   /* the queue grew: not the same one */
	set_queue(base, 10, 2);
	queue_remove(3);
	queue_move(0, 4);
	CHECK(queue_restore() == -1);   /* neighbours changed */
	set_queue(base, 10, 2);
	queue_remove(3);
	fake_ms += 10500;
	CHECK(!queue_undo_ok() && queue_restore() == -1);   /* too late */
	/* repeat all: a removal past the end comes back past the end, and the playing song keeps its place */
	repeat = 1;
	set_queue(base, 10, 8);
	int sp = q_play[qpos];
	queue_remove(3); show("undo: repeat-all remove");
	int k = queue_restore(); show("undo: repeat-all restored");
	CHECK(k == 3 && qn == 10 && q_play[qpos] == sp);
	for (int i = 0; i < 10; i++) CHECK(q_play[i] == base[i]);
	sorted_same();
	/* shuffle: q_orig gets its copy back too */
	repeat = 0; shuffle = true;
	set_queue(base, 10, 2);
	queue_move(1, 5);
	memcpy(snap, q_play, 10 * sizeof(int));
	queue_remove(4); CHECK(queue_restore() == 4);
	CHECK(qn == 10 && !memcmp(q_play, snap, 10 * sizeof(int)));
	sorted_same();
	shuffle = false;
	printf(fails ? "FAILED %d\n" : "all ok\n", fails);
	return fails != 0;
}
'''
open(sys.argv[2], 'w', newline='\n').write(harness)
