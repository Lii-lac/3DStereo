// decoder.c reading through its own 64 KB-buffered files against the decoder before it
// (decoder.c.pre28, which let each library open the file), on real files of every format: the
// samples must come out identical, from the top and after a seek. test_decoder.sh builds it.
#include "decoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Decoder *old_dec_open(const char *path, DecInfo *info);
void old_dec_close(Decoder *d);
int old_dec_read(Decoder *d, float *out, int frames);
bool old_dec_seek(Decoder *d, u64 frame);

static int check(const char *path) {
	DecInfo a, b;
	Decoder *x = old_dec_open(path, &a), *y = dec_open(path, &b);
	if (!x || !y) { printf("%s: open %d %d\n", path, !!x, !!y); return !!x != !!y; }
	int bad = memcmp(&a, &b, sizeof a) != 0;
	static float p[4096 * 8], q[4096 * 8];
	long frames = 0;
	for (int pass = 0; pass < 2 && !bad; pass++) {
		if (pass) {   // a seek into the middle, then on to the end again
			u64 to = frames / 2;
			bad |= old_dec_seek(x, to) != dec_seek(y, to);
		}
		for (;;) {
			int m = old_dec_read(x, p, 4096), n = dec_read(y, q, 4096);
			if (m != n || memcmp(p, q, sizeof(float) * m * a.channels)) { bad = 1; break; }
			if (!m) break;
			if (!pass) frames += m;
		}
	}
	old_dec_close(x);
	dec_close(y);
	printf("%s: %ld frames, %s\n", path, frames, bad ? "DIFFERENT" : "same");
	return bad;
}
int main(int argc, char **argv) {
	int bad = 0;
	for (int k = 1; k < argc; k++) bad += check(argv[k]);
	printf("%d files, %d different\n", argc - 1, bad);
	return bad != 0;
}
