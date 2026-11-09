// spotmatch.c on a PC, against the PC tool's answers (cases.py writes them):
//   uv run player/test/cases.py S:/music player/test/out
//   docker run --rm -v "$(pwd -W):/src" -w /src devkitpro/devkitarm sh -c
//     "gcc -O1 -Wall -Iplayer/test/fake -Iplayer/source player/test/test_spotmatch.c player/source/spotmatch.c -o /tmp/t && /tmp/t player/test/out"
#include "spotmatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Library lib;

static char *field(char **s, char sep) {
	char *f = *s, *e = strchr(f, sep);
	if (e) { *e = 0; *s = e + 1; } else *s = f + strlen(f);
	return f;
}

static FILE *open_in(const char *dir, const char *name) {
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "r");
	if (!f) { perror(p); exit(2); }
	return f;
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "player/test/out";
	char line[4096];
	int bad = 0, n = 0;

	FILE *f = open_in(dir, "strings.tsv");
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *s = line, *in = field(&s, '\t'), *pn = field(&s, '\t'), *pl = field(&s, '\t');
		// on purpose, C reads a curly apostrophe as the straight one (the PC tool drops it, so
		// "Don’t" and "Don't" never meet there), and keeps Ø as o
		if (strstr(in, "\xE2\x80\x98") || strstr(in, "\xE2\x80\x99") || strstr(in, "\xC3\x98") || strstr(in, "\xC3\xB8")) continue;
		char cn[512], cl[512];
		sm_norm(in, cn, sizeof cn);
		sm_loose(in, cl, sizeof cl);
		n++;
		if (strcmp(cn, pn)) { bad++; printf("norm  [%s]: C [%s] py [%s]\n", in, cn, pn); }
		if (strcmp(cl, pl)) { bad++; printf("loose [%s]: C [%s] py [%s]\n", in, cl, pl); }
	}
	fclose(f);
	printf("%d strings\n", n);

	f = open_in(dir, "lib.tsv");
	int cap = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (lib.nt == cap) { cap = cap ? cap * 2 : 64; lib.t = realloc(lib.t, cap * sizeof(Track)); }
		Track *t = &lib.t[lib.nt++];
		memset(t, 0, sizeof *t);
		char *s = line;
		t->path = strdup(field(&s, '\t'));
		t->title = strdup(field(&s, '\t'));
		t->artist = strdup(field(&s, '\t'));
		t->secs = atoi(field(&s, '\t'));
	}
	fclose(f);
	sm_build();

	f = open_in(dir, "queries.tsv");
	int nq = 0, hits = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *s = line, *name = field(&s, '\t'), *arts = field(&s, '\t');
		SmTrack q = {.name = name};
		q.ms = atoi(field(&s, '\t'));
		char *want = field(&s, '\t');
		while (*arts && q.na < 8) q.artists[q.na++] = field(&arts, '\x1f');
		int i = sm_match(&q);
		char got[1024] = "-";
		if (i >= 0) snprintf(got, sizeof got, "../%s", lib.t[i].path + strlen("sdmc:/music/"));
		nq++;
		hits += i >= 0;
		if (strcmp(got, want)) { bad++; printf("match [%s] by [%s] %u ms: C %s, py %s\n", name, q.artists[0], q.ms, got, want); }
		if (i >= 0 && !strcmp(got, want) && q.ms && strcmp(lib.t[i].artist, "Unknown artist") && !sm_matches(&q, i, 6))
			printf("note: sm_matches disagrees on [%s]\n", name);
	}
	fclose(f);
	printf("%d queries, %d matched, %d differences\n", nq, hits, bad);
	return bad != 0;
}
