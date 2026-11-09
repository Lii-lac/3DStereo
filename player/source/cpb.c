#include "cpb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- writing ------------------------------------------------------------------------------

void pb_free(Pb *b) { free(b->d); *b = (Pb){0}; }

static void put(Pb *b, const void *d, size_t n) {
	if (b->n + n > b->cap) {
		size_t c = b->cap ? b->cap * 2 : 256;
		while (c < b->n + n) c *= 2;
		uint8_t *g = realloc(b->d, c);
		if (!g) return;
		b->d = g;
		b->cap = c;
	}
	memcpy(b->d + b->n, d, n);
	b->n += n;
}

static void raw_varint(Pb *b, uint64_t v) {
	uint8_t t[10];
	int k = 0;
	do { t[k] = v & 0x7F; v >>= 7; if (v) t[k] |= 0x80; k++; } while (v);
	put(b, t, k);
}

static void key(Pb *b, int field, int wt) { raw_varint(b, ((uint64_t)field << 3) | wt); }

void pb_varint(Pb *b, int field, uint64_t v) { key(b, field, 0); raw_varint(b, v); }
void pb_bool(Pb *b, int field, bool v) { if (v) pb_varint(b, field, 1); }
void pb_bytes(Pb *b, int field, const void *d, size_t n) { key(b, field, 2); raw_varint(b, n); if (n) put(b, d, n); }
void pb_str(Pb *b, int field, const char *s) { if (s && *s) pb_bytes(b, field, s, strlen(s)); }
void pb_msg(Pb *b, int field, const Pb *sub) { pb_bytes(b, field, sub->d, sub->n); }

void pb_double(Pb *b, int field, double v) {
	uint64_t u;
	memcpy(&u, &v, 8);
	uint8_t t[8];
	for (int k = 0; k < 8; k++) t[k] = u >> (8 * k);   // little-endian, like the wire wants
	key(b, field, 1);
	put(b, t, 8);
}

void pb_map(Pb *b, int field, const char *k, const char *v) {
	Pb e = {0};
	pb_bytes(&e, 1, k, strlen(k));
	pb_bytes(&e, 2, v, strlen(v));
	pb_msg(b, field, &e);
	pb_free(&e);
}

// ---- reading ------------------------------------------------------------------------------

static bool get_varint(PbR *r, uint64_t *v) {
	*v = 0;
	for (int s = 0; s < 64 && r->p < r->end; s += 7) {
		uint8_t c = *r->p++;
		*v |= (uint64_t)(c & 0x7F) << s;
		if (!(c & 0x80)) return true;
	}
	return false;
}

bool pbr_next(PbR *r, int *field, int *wt, uint64_t *v, const uint8_t **data, size_t *len) {
	if (r->p >= r->end) return false;
	uint64_t k;
	if (!get_varint(r, &k)) return false;
	*field = (int)(k >> 3);
	*wt = (int)(k & 7);
	*v = 0; *data = NULL; *len = 0;
	switch (*wt) {
	case 0: return get_varint(r, v);
	case 1: case 5: {
		int n = *wt == 1 ? 8 : 4;
		if (r->end - r->p < n) return false;
		for (int i = 0; i < n; i++) *v |= (uint64_t)r->p[i] << (8 * i);
		r->p += n;
		return true;
	}
	case 2: {
		uint64_t n;
		if (!get_varint(r, &n) || n > (uint64_t)(r->end - r->p)) return false;
		*data = r->p; *len = (size_t)n;
		r->p += n;
		return true;
	}
	default: return false;   // groups: nothing Spotify sends us
	}
}

static void copy_str(char *out, int cap, const uint8_t *d, size_t n) {
	if (n >= (size_t)cap) n = cap - 1;
	memcpy(out, d, n);
	out[n] = 0;
}

// ---- base64, gids -------------------------------------------------------------------------

static int b64v(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+' || c == '-') return 62;
	if (c == '/' || c == '_') return 63;
	return -1;
}

long b64_decode(const char *in, uint8_t *out) {
	long n = 0;
	uint32_t acc = 0;
	int bits = 0;
	for (; *in && *in != '='; in++) {
		if (*in == '\n' || *in == '\r') continue;
		int v = b64v(*in);
		if (v < 0) return -1;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) { bits -= 8; out[n++] = (acc >> bits) & 0xFF; }
	}
	return n;
}

void gid_b62(const uint8_t *gid, size_t n, char out[23]) {
	static const char A[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	uint8_t x[16] = {0};
	if (n > 16) n = 16;
	memcpy(x + 16 - n, gid, n);
	for (int k = 21; k >= 0; k--) {   // the 128-bit number divided by 62, 22 times
		uint32_t rem = 0;
		for (int i = 0; i < 16; i++) {
			uint32_t cur = (rem << 8) | x[i];
			x[i] = cur / 62;
			rem = cur % 62;
		}
		out[k] = A[rem];
	}
	out[22] = 0;
}

// ---- the device's state -------------------------------------------------------------------

static void track(Pb *out, int field, const CState *s, int i) {
	Pb t = {0};
	pb_str(&t, 1, s->uris[i]);
	pb_str(&t, 2, s->uids[i]);
	pb_map(&t, 3, "context_uri", s->ctx_uri);
	pb_map(&t, 3, "entity_uri", s->ctx_uri);
	pb_str(&t, 6, s->queued && s->queued[i] ? "queue" : "context");
	pb_msg(out, field, &t);
	pb_free(&t);
}

void cpb_state(Pb *out, const CState *s) {
	Pb caps = {0}, info = {0}, ps = {0}, dev = {0}, sup = {0};
	// Capabilities: what librespot says, less volume (the 3DS's slider is a real slider)
	pb_bool(&caps, 2, true);    // can_be_player
	pb_bool(&caps, 5, true);    // gaia_eq_connect_id
	pb_bool(&caps, 7, true);    // is_observable
	pb_varint(&caps, 8, 64);    // volume_steps
	pb_str(&caps, 9, "audio/track");
	pb_str(&caps, 9, "audio/local");
	pb_bool(&caps, 10, true);   // command_acks
	pb_bool(&caps, 15, true);   // supports_playlist_v2
	pb_bool(&caps, 16, true);   // is_controllable
	pb_bool(&caps, 19, true);   // supports_transfer_command
	pb_bool(&caps, 20, true);   // supports_command_request
	pb_bool(&caps, 22, true);   // needs_full_player_state
	pb_bool(&caps, 23, true);   // supports_gzip_pushes
	pb_bool(&caps, 25, true);   // supports_set_options_command

	pb_bool(&info, 1, true);    // can_play
	pb_varint(&info, 2, s->volume);
	pb_str(&info, 3, s->name);
	pb_msg(&info, 4, &caps);
	pb_str(&info, 6, s->version);
	pb_varint(&info, 7, DEVICE_GAME_CONSOLE);
	pb_str(&info, 9, "3.2.6");  // spirc_version, as librespot
	pb_str(&info, 10, s->device_id);
	pb_str(&info, 13, s->client_id);

	pb_varint(&ps, 1, (uint64_t)s->now_ms);   // timestamp
	if (s->loaded && s->n > 0) {
		pb_str(&ps, 2, s->ctx_uri);
		char url[300];
		snprintf(url, sizeof url, "context://%s", s->ctx_uri);
		pb_str(&ps, 3, url);
		if (s->nctx_restr) pb_bytes(&ps, 4, s->ctx_restr, s->nctx_restr);
		else { Pb e = {0}; pb_msg(&ps, 4, &e); }
		if (s->norigin) pb_bytes(&ps, 5, s->origin, s->norigin);
		else { Pb e = {0}; pb_msg(&ps, 5, &e); }
		Pb ix = {0};
		pb_varint(&ix, 2, (uint64_t)s->index);
		pb_msg(&ps, 6, &ix);
		pb_free(&ix);
		track(&ps, 7, s, s->index);
		pb_str(&ps, 8, s->playback_id);
		pb_double(&ps, 9, s->paused ? 0.0 : 1.0);   // playback_speed
		pb_varint(&ps, 10, (uint64_t)(s->pos_ms > 0 ? s->pos_ms : 0));   // position_as_of_timestamp
		pb_varint(&ps, 11, (uint64_t)(s->dur_ms > 0 ? s->dur_ms : 0));
		// librespot's set_status: paused is playing + paused + buffering, all true, at speed 0;
		// otherwise the play button on a PC goes grey
		pb_bool(&ps, 12, true);
		pb_bool(&ps, 13, s->paused);
		pb_bool(&ps, 14, s->paused);
		pb_bool(&ps, 15, true);     // is_system_initiated
		Pb opt = {0};
		pb_bool(&opt, 1, s->shuffle);
		pb_bool(&opt, 2, s->rep_ctx);
		pb_bool(&opt, 3, s->rep_track);
		pb_msg(&ps, 16, &opt);
		pb_free(&opt);
		Pb rs = {0};   // what the other side's pause/play button goes by
		pb_str(&rs, s->paused ? 1 : 2, s->paused ? "not_playing" : "not_paused");
		pb_msg(&ps, 17, &rs);
		pb_free(&rs);
		pb_msg(&ps, 18, &sup);
		int lo = s->index - s->prev_max; if (lo < 0) lo = 0;
		for (int i = lo; i < s->index; i++) track(&ps, 19, s, i);
		int hi = s->index + 1 + s->next_max; if (hi > s->n) hi = s->n;
		for (int i = s->index + 1; i < hi; i++) track(&ps, 20, s, i);
		pb_str(&ps, 23, s->session_id);
		char rev[17];
		uint32_t h = 2166136261u;
		for (int i = s->index + 1; i < hi; i++) for (const char *c = s->uris[i]; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
		snprintf(rev, sizeof rev, "%08x%08x", (unsigned)h, (unsigned)(hi - s->index));
		pb_str(&ps, 24, rev);   // queue_revision: changes whenever what's next does
	} else {
		pb_bool(&ps, 15, true);
		pb_double(&ps, 9, 1.0);
		pb_str(&ps, 23, s->session_id);
	}

	pb_msg(&dev, 1, &info);
	pb_msg(&dev, 2, &ps);

	pb_msg(out, 2, &dev);
	pb_varint(out, 3, 2);       // member_type CONNECT_STATE
	pb_bool(out, 4, s->active);
	pb_varint(out, 5, (uint64_t)s->reason);
	pb_str(out, 7, s->last_from);
	if (s->last_msg_id) pb_varint(out, 8, s->last_msg_id);
	if (s->active && s->started_at) pb_varint(out, 9, s->started_at);
	pb_varint(out, 12, (uint64_t)s->now_ms);   // client_side_timestamp
	pb_free(&caps); pb_free(&info); pb_free(&ps); pb_free(&dev); pb_free(&sup);
}

// ---- what comes down ----------------------------------------------------------------------

static void ctx_track(const uint8_t *d, size_t n, char *uri, int ucap, char *uid, int icap) {
	PbR r = {d, d + n};
	int f, wt; uint64_t v; const uint8_t *p; size_t l;
	uri[0] = uid[0] = 0;
	char gid[23] = "";
	while (pbr_next(&r, &f, &wt, &v, &p, &l)) {
		if (f == 1 && wt == 2) copy_str(uri, ucap, p, l);
		else if (f == 2 && wt == 2) copy_str(uid, icap, p, l);
		else if (f == 3 && wt == 2 && l) gid_b62(p, l, gid);
	}
	if (!uri[0] && gid[0]) snprintf(uri, ucap, "spotify:track:%s", gid);
}

static void options(const uint8_t *d, size_t n, Transfer *t) {
	PbR r = {d, d + n};
	int f, wt; uint64_t v; const uint8_t *p; size_t l;
	while (pbr_next(&r, &f, &wt, &v, &p, &l)) {
		if (wt != 0) continue;
		if (f == 1) t->shuffle = v;
		else if (f == 2) t->rep_ctx = v;
		else if (f == 3) t->rep_track = v;
	}
}

static void context(const uint8_t *d, size_t n, Transfer *t) {
	PbR r = {d, d + n};
	int f, wt; uint64_t v; const uint8_t *p; size_t l;
	while (pbr_next(&r, &f, &wt, &v, &p, &l)) {
		if (wt != 2) continue;
		if (f == 1) copy_str(t->ctx_uri, sizeof t->ctx_uri, p, l);
		else if (f == 4) { t->ctx_restr = p; t->nctx_restr = l; }
		else if (f == 5) {   // a page: its tracks (field 4)
			PbR pr = {p, p + l};
			int pf, pw; uint64_t pv; const uint8_t *pp; size_t pl;
			while (pbr_next(&pr, &pf, &pw, &pv, &pp, &pl)) {
				if (pf != 4 || pw != 2) continue;
				if (t->page_uris && t->npage_tracks < t->npages_max) {
					ctx_track(pp, pl, t->page_uris[t->npage_tracks], 64, t->page_uids[t->npage_tracks], 64);
					t->npage_tracks++;
				}
			}
		}
	}
}

bool cpb_transfer(const uint8_t *d, size_t n, Transfer *t) {
	char (*pu)[64] = t->page_uris; char (*pi)[64] = t->page_uids; int pm = t->npages_max;
	memset(t, 0, sizeof *t);
	t->page_uris = pu; t->page_uids = pi; t->npages_max = pm;
	PbR r = {d, d + n};
	int f, wt; uint64_t v; const uint8_t *p; size_t l;
	bool any = false;
	while (pbr_next(&r, &f, &wt, &v, &p, &l)) {
		if (wt != 2) continue;
		any = true;
		if (f == 1) options(p, l, t);
		else if (f == 2) {   // Playback
			PbR q = {p, p + l};
			int qf, qw; uint64_t qv; const uint8_t *qp; size_t ql;
			while (pbr_next(&q, &qf, &qw, &qv, &qp, &ql)) {
				if (qf == 1 && qw == 0) t->timestamp = (int64_t)qv;
				else if (qf == 2 && qw == 0) t->pos = (int32_t)(uint32_t)qv;
				else if (qf == 4 && qw == 0) t->paused = qv;
				else if (qf == 5 && qw == 2) ctx_track(qp, ql, t->track_uri, sizeof t->track_uri, t->track_uid, sizeof t->track_uid);
			}
		} else if (f == 3) {   // Session
			PbR q = {p, p + l};
			int qf, qw; uint64_t qv; const uint8_t *qp; size_t ql;
			while (pbr_next(&q, &qf, &qw, &qv, &qp, &ql)) {
				if (qw != 2) continue;
				if (qf == 1) { t->origin = qp; t->norigin = ql; }
				else if (qf == 2) context(qp, ql, t);
				else if (qf == 3) copy_str(t->current_uid, sizeof t->current_uid, qp, ql);
			}
		}
	}
	return any && t->track_uri[0];
}

long cpb_volume(const uint8_t *d, size_t n) {
	PbR r = {d, d + n};
	int f, wt; uint64_t v; const uint8_t *p; size_t l;
	while (pbr_next(&r, &f, &wt, &v, &p, &l)) if (f == 1 && wt == 0) return (long)(v > 65535 ? 65535 : v);
	return -1;
}

bool cpb_cluster_active(const uint8_t *d, size_t n, char *out, int cap) {
	PbR r = {d, d + n};
	int f, wt; uint64_t v; const uint8_t *p; size_t l;
	out[0] = 0;
	while (pbr_next(&r, &f, &wt, &v, &p, &l)) {
		if (f != 1 || wt != 2) continue;
		PbR c = {p, p + l};
		int cf, cw; uint64_t cv; const uint8_t *cp; size_t cl;
		while (pbr_next(&c, &cf, &cw, &cv, &cp, &cl))
			if (cf == 2 && cw == 2) { copy_str(out, cap, cp, cl); return true; }
		return true;
	}
	return false;
}
