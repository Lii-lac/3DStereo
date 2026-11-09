// An artist page from the card, the way a tap after a restart gets it (build 36): with nothing in
// memory, do_artist has to hand over the page AND its photo in the same publish, so the main
// thread never draws the colour placeholder. Uses glaive's real page and pictures copied off the
// card (player/test/data/card). From the project root:
//   docker run --rm -v "$(pwd -W):/src" -w /src ubuntu:24.04 sh -c "apt-get -qq update && apt-get -qq install -y
//     gcc libjansson-dev >/dev/null && sh player/test/test_pagefast.sh"
#include "../source/meta.c"
#include <stdarg.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"

bool net_up(char *e, int c) { (void)e; (void)c; return false; }
bool net_wifi(void) { return false; }   // offline: everything has to come from the card
CURL *net_handle(void) { return NULL; }
long net_req(CURL *h, const char *m, const char *u, const char *f, const char *a, Buf *o, long *r, volatile bool *q, CURLcode *c) {
	(void)h; (void)m; (void)f; (void)a; (void)o; (void)r; (void)q; (void)c;
	printf("  NETWORK ASKED: %s\n", u);
	return -1;
}
bool spot_token(CURL *h, const char *s, char *o, int c, char *w, int wc, volatile bool *q) {
	(void)h; (void)s; (void)o; (void)c; (void)w; (void)wc; (void)q;
	return false;
}
bool spot_paired(void) { return false; }
int audio_make_room(int max_ms) { (void)max_ms; return 0; }
void crumb(const char *fmt, ...) { va_list a; va_start(a, fmt); fputs("  crumb: ", stdout); vprintf(fmt, a); va_end(a); putchar('\n'); }

Library lib;
static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)

int main(void) {
	static Track T[1] = {{.path = "sdmc:/music/a.flac", .title = "the prom", .artist = "glaive", .album = "the prom", .secs = 132}};
	static int s0[] = {0};
	static Group AL[1], AR[1];
	AL[0] = (Group){.name = "the prom", .artist = "glaive", .songs = s0, .n = 1};
	AR[0] = (Group){.name = "glaive", .songs = s0, .n = 1};
	lib = (Library){.t = T, .nt = 1, .albums = AL, .nal = 1, .artists = AR, .nar = 1};
	sm_build();

	// a tap on glaive, nothing in memory (a fresh launch): build 36 answers it from the card inside
	// the call, on the main thread, before the worker is even woken
	meta_artist_want("glaive", NULL);
	LightLock_Lock(&lk);
	int g = a_gen;
	LightLock_Unlock(&lk);
	CHECK(a_state == MS_READY && a_ready, "page ready in the tap's own call, state %d (%s)", a_state, a_why);
	CHECK(a_done_gen == g, "a fresh page leaves the worker nothing but pictures");
	if (a_ready) printf("  %s: %ld monthly listeners, %d popular\n", a_ready->name, (long)a_ready->listeners, a_ready->ntop);
	CHECK(a_ready && a_ready->listeners > 0, "monthly listeners from the card");
	bool head = false;
	for (int k = 0; k < nout; k++)
		if (outq[k].slot == 0 && outq[k].gen == g && outq[k].w == MI_HEAD_W && outq[k].h == MI_HEAD_H && outq[k].px) head = true;
	CHECK(head, "the photo went out with the page (%d pictures with it)", nout);
	printf("  %d pictures out with the page\n", nout);
	if (a_ready) printf("  colour %06lx\n", (unsigned long)a_ready->color);
	CHECK(a_ready && a_ready->color, "the page's own colour from the card");
	int more = 0;
	while (do_image()) more++;   // the worker's part: the rest of the pictures (offline: none come)
	printf("  the worker went through %d more picture jobs\n", more);
	printf(bad ? "%d FAILED\n" : "all good\n", bad);
	return bad != 0;
}
