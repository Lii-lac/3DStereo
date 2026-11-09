#include "spotmatch.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// U+00C0 to U+017F with the accents off, the way the PC tool's NFKD sees them
static const char *const FOLD[192] = {"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", "d", "n", "o", "o", "o", "o", "o", " ", "o", "u", "u", "u", "u", "y", "th", "ss", "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", "d", "n", "o", "o", "o", "o", "o", " ", "o", "u", "u", "u", "u", "y", "th", "y", "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d", "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g", "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i", "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l", "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o", "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s", "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s"};

static bool word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }
static bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

void sm_norm(const char *s, char *out, int cap) {
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		char one[2] = {0};
		const char *rep = one;
		int adv = 1;
		unsigned c = *p;
		if (c < 0x80) {
			if (isalnum(c)) one[0] = tolower(c);
			else rep = c == '&' ? " and " : " ";
		} else if ((c == 0xC3 || c == 0xC4 || c == 0xC5) && (p[1] & 0xC0) == 0x80) {
			unsigned cp = (c & 0x1F) << 6 | (p[1] & 0x3F);
			rep = cp >= 0xC0 && cp < 0x180 ? FOLD[cp - 0xC0] : " ";
			adv = 2;
		} else if (c == 0xC2 && (p[1] & 0xC0) == 0x80) {   // NBSP, quotes, the odd symbol
			rep = " ";
			adv = 2;
		} else if (c == 0xE2 && (p[1] == 0x80 || p[1] == 0x81) && (p[2] & 0xC0) == 0x80) {   // curly quotes, dashes
			rep = " ";
			adv = 3;
		} else one[0] = c;   // any other script stays as it is
		for (; *rep; rep++) {
			if (*rep == ' ') { if (n && out[n - 1] != ' ' && n < cap - 1) out[n++] = ' '; }
			else if (n < cap - 1) out[n++] = *rep;
		}
		for (int k = 0; k < adv && *p; k++) p++;
	}
	while (n && out[n - 1] == ' ') n--;
	out[n] = 0;
}

void sm_loose(const char *title, char *out, int cap) {
	char u[512];
	int n = 0;
	for (int i = 0; title[i] && n < (int)sizeof u - 1;) {   // (...) and [...] out
		if (title[i] == '(' || title[i] == '[') {
			int j = i + 1;
			while (title[j] && title[j] != ')' && title[j] != ']') j++;
			if (title[j]) { u[n++] = ' '; i = j + 1; continue; }
		}
		u[n++] = title[i++];
	}
	u[n] = 0;
	for (int i = 0; u[i]; i++)   // " - Radio Edit"
		if (space(u[i]) && u[i + 1] == '-' && space(u[i + 2])) { u[i] = 0; break; }
	for (int i = 0; u[i]; i++) {   // "feat. X", "ft X"
		if (i && word(u[i - 1])) continue;
		int k = !strncasecmp(u + i, "feat", 4) ? 4 : !strncasecmp(u + i, "ft", 2) ? 2 : 0;
		if (!k) continue;
		int j = i + k;
		if (space(u[j]) || (u[j] == '.' && space(u[j + 1]))) { u[i] = 0; break; }
	}
	sm_norm(u, out, cap);
	if (!out[0]) sm_norm(title, out, cap);
}

// ---- what each song on the card is matched by ---------------------------------------------

typedef struct { char *title, *loose, *exact, *artist, **keys; int nk; } Song;
static Song *S;
static int nS;
typedef struct { const char *key; int i; } Entry;
static Entry *E;

// a separator between artists at s: , ; / & or the words feat ft featuring with x
static int sep_at(const char *base, const char *s) {
	if (*s == ',' || *s == ';' || *s == '/' || *s == '&') return 1;
	if (s > base && word(s[-1])) return 0;
	static const char *W[] = {"featuring", "feat", "ft", "with", "x"};
	for (int k = 0; k < 5; k++) {
		int n = strlen(W[k]);
		if (strncasecmp(s, W[k], n)) continue;
		if (k == 1 || k == 2) { if (s[n] == '.') return n + 1; }
		if (!word(s[n])) return n;
	}
	return 0;
}

static void add_key(Song *g, const char *a, int len) {
	char raw[256], k[256];
	if (len > (int)sizeof raw - 1) len = sizeof raw - 1;
	memcpy(raw, a, len);
	raw[len] = 0;
	sm_norm(raw, k, sizeof k);
	if (!k[0]) return;
	for (int j = 0; j < g->nk; j++) if (!strcmp(g->keys[j], k)) return;
	g->keys = realloc(g->keys, (g->nk + 1) * sizeof(char *));
	g->keys[g->nk++] = strdup(k);
}

// "Tyler, The Creator" stays whole, and splits too: either half is a match
static void artist_keys(Song *g, const char *a) {
	add_key(g, a, strlen(a));
	const char *piece = a;
	for (const char *s = a; *s;) {
		int n = sep_at(a, s);
		if (n) {
			if (s > piece) add_key(g, piece, s - piece);
			s += n;
			piece = s;
		} else s++;
	}
	if (piece != a && *piece) add_key(g, piece, strlen(piece));
}

static int entry_cmp(const void *a, const void *b) {
	const Entry *x = a, *y = b;
	int c = strcmp(x->key, y->key);
	return c ? c : x->i - y->i;
}

// the name the library falls back to for an untagged file: "Artist - Title [videoid].mp3"
static void stem_of(const char *path, char *out, int cap) {
	const char *s = strrchr(path, '/');
	s = s ? s + 1 : path;
	const char *dot = strrchr(s, '.');
	int n = dot && dot > s ? dot - s : (int)strlen(s);
	if (n > 14 && s[n - 1] == ']' && s[n - 13] == '[' && s[n - 14] == ' ') n -= 14;
	if (n > cap - 1) n = cap - 1;
	memcpy(out, s, n);
	out[n] = 0;
}

void sm_build(void) {
	if (S) return;
	nS = lib.nt;
	S = calloc(nS ? nS : 1, sizeof *S);
	E = malloc((nS ? nS : 1) * sizeof *E);
	char buf[512], stem[512];
	for (int i = 0; i < nS; i++) {
		Track *t = &lib.t[i];
		Song *g = &S[i];
		const char *artist = strcmp(t->artist, "Unknown artist") ? t->artist : NULL;
		stem_of(t->path, stem, sizeof stem);
		const char *dash = strstr(stem, " - ");
		if (!strcmp(t->title, stem) && dash) {   // no title tag: the file name is "Artist - Title"
			g->title = strdup(dash + 3);
			g->artist = strndup(stem, dash - stem);
		} else {
			g->title = strdup(t->title);
			g->artist = artist ? strdup(artist) : NULL;
		}
		if (g->artist) artist_keys(g, g->artist);
		sm_loose(g->title, buf, sizeof buf);
		g->loose = strdup(buf);
		sm_norm(g->title, buf, sizeof buf);
		g->exact = strdup(buf);
		E[i] = (Entry){g->loose, i};
	}
	qsort(E, nS, sizeof *E, entry_cmp);
}

void sm_free(void) {
	for (int i = 0; i < nS && S; i++) {
		free(S[i].title); free(S[i].loose); free(S[i].exact); free(S[i].artist);
		for (int k = 0; k < S[i].nk; k++) free(S[i].keys[k]);
		free(S[i].keys);
	}
	free(S); free(E);
	S = NULL; E = NULL; nS = 0;
}

static bool overlap(const Song *g, char want[][128], int nw) {
	for (int a = 0; a < nw; a++)
		for (int k = 0; k < g->nk; k++) if (!strcmp(want[a], g->keys[k])) return true;
	return false;
}

static int want_keys(const SmTrack *t, char want[8][128]) {
	int nw = 0;
	for (int a = 0; a < t->na && a < 8; a++) {
		sm_norm(t->artists[a], want[nw], 128);
		if (want[nw][0]) nw++;
	}
	return nw;
}

int sm_match(const SmTrack *t) {
	if (!t->name || !nS) return -1;
	char lk[512], ex[512], want[8][128];
	sm_loose(t->name, lk, sizeof lk);
	sm_norm(t->name, ex, sizeof ex);
	int nw = want_keys(t, want);
	int lo = 0, hi = nS;   // the first entry with this loose title
	while (lo < hi) { int m = (lo + hi) / 2; if (strcmp(E[m].key, lk) < 0) lo = m + 1; else hi = m; }
	// the candidates: that title, and an artist in common (or no artist tag at all). If any has
	// the exact title, only those count; then the closest length, none more than 6 s off.
	int hi2 = lo;
	bool have_exact = false;
	for (; hi2 < nS && !strcmp(E[hi2].key, lk); hi2++) {
		const Song *g = &S[E[hi2].i];
		if ((!g->nk || overlap(g, want, nw)) && !strcmp(g->exact, ex)) have_exact = true;
	}
	int best = -1;
	long best_d = 0;
	for (int e = lo; e < hi2; e++) {
		const Song *g = &S[E[e].i];
		if (g->nk && !overlap(g, want, nw)) continue;
		if (have_exact && strcmp(g->exact, ex)) continue;
		u32 secs = lib.t[E[e].i].secs;
		long d = secs && t->ms ? labs((long)secs * 1000 - (long)t->ms) : 99000;
		if (secs && t->ms && d > 6000) continue;   // a different cut
		if (best < 0 || d < best_d) { best = E[e].i; best_d = d; }
	}
	return best;
}

bool sm_matches(const SmTrack *t, int i, int slack) {
	if (i < 0 || i >= nS || !t->name) return false;
	const Song *g = &S[i];
	char lk[512], want[8][128];
	sm_loose(t->name, lk, sizeof lk);
	if (strcmp(lk, g->loose)) return false;
	if (!overlap(g, want, want_keys(t, want))) return false;
	u32 secs = lib.t[i].secs;
	return !(secs && t->ms) || labs((long)secs * 1000 - (long)t->ms) <= slack * 1000L;
}

const char *sm_title(int i) { return i >= 0 && i < nS ? S[i].title : ""; }

bool sm_artist(int i, int which, char *out, int cap) {
	if (i < 0 || i >= nS || !S[i].artist) return false;
	const char *a = S[i].artist;
	int n = strlen(a);
	if (which)
		for (const char *s = a; *s; s++)
			if (sep_at(a, s)) { n = s - a; break; }
	while (n && space(a[n - 1])) n--;
	if (n <= 0) return false;
	if (n > cap - 1) n = cap - 1;
	memcpy(out, a, n);
	out[n] = 0;
	return true;
}
