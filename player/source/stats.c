#include "stats.h"
#include "library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR   "sdmc:/3ds/player"
#define STATS DIR "/stats"

static u32 *plays;
static float *secs;
static char *gone;          // lines for songs not on the card right now, kept word for word
static size_t gone_n;
static u32 tot_plays;
static double tot_secs;
static int cur_t = -1;
static double last_pos, heard;
static bool counted, changed, unsaved;

void stats_load(void) {
	plays = calloc(lib.nt + 1, sizeof *plays);
	secs = calloc(lib.nt + 1, sizeof *secs);
	FILE *f = fopen(STATS, "r");
	if (!f) return;
	char l[1100];
	if (fgets(l, sizeof l, f) && !strcmp(l, "pocket-stats 1\n"))
		while (fgets(l, sizeof l, f)) {
			unsigned long p;
			double s;
			int at;
			if (sscanf(l, "%lu %lf %n", &p, &s, &at) != 2) continue;
			l[strcspn(l, "\r\n")] = 0;
			tot_plays += p;
			tot_secs += s;
			int i = library_find(l + at);
			if (i >= 0) { plays[i] += p; secs[i] += s; continue; }
			size_t n = strlen(l);
			char *g = realloc(gone, gone_n + n + 2);
			if (!g) continue;
			gone = g;
			memcpy(gone + gone_n, l, n);
			gone[gone_n + n] = '\n';
			gone_n += n + 1;
			gone[gone_n] = 0;
		}
	fclose(f);
}

static void save(void) {
	size_t cap = 32 + gone_n;
	for (int i = 0; i < lib.nt; i++) if (plays[i] || secs[i] >= 1) cap += strlen(lib.t[i].path) + 32;
	char *t = malloc(cap);
	if (!t) return;
	size_t n = sprintf(t, "pocket-stats 1\n");
	for (int i = 0; i < lib.nt; i++)
		if (plays[i] || secs[i] >= 1) n += sprintf(t + n, "%lu %.0f %s\n", (unsigned long)plays[i], secs[i], lib.t[i].path);
	if (gone_n) memcpy(t + n, gone, gone_n + 1);
	bg_save(STATS, DIR, t);
	unsaved = false;
}

void stats_heard(int t, double pos, bool playing) {
	if (!plays) return;
	// a new song, or this one again from the top: a fresh listen
	if (t != cur_t || (pos < last_pos - 2 && pos < 2)) { cur_t = t; heard = 0; counted = false; last_pos = pos; return; }
	double d = pos - last_pos;
	last_pos = pos;
	if (t < 0 || t >= lib.nt || !playing || d <= 0 || d > 1) return;   // paused, or a seek: not heard
	heard += d;
	secs[t] += d;
	tot_secs += d;
	unsaved = true;
	double need = lib.t[t].secs && lib.t[t].secs < 60 ? lib.t[t].secs / 2.0 : 30;
	if (!counted && heard >= need) {
		counted = true;
		plays[t]++;
		tot_plays++;
		changed = true;
		save();
	}
}

u32 stats_plays(int t) { return plays && t >= 0 && t < lib.nt ? plays[t] : 0; }
float stats_secs(int t) { return secs && t >= 0 && t < lib.nt ? secs[t] : 0; }
u32 stats_total_plays(void) { return tot_plays; }
double stats_total_secs(void) { return tot_secs; }
bool stats_changed(void) { bool c = changed; changed = false; return c; }
void stats_flush(void) { if (unsaved && plays) save(); }
