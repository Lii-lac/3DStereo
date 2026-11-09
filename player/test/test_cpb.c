// cpb.c on a PC: decodes test/out/cpb/transfer.bin (made by test_cpb.py with the real .proto
// files), prints what it read, and writes two states for test_cpb.py to decode.
#include "cpb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void save(const char *path, const Pb *b) {
	FILE *f = fopen(path, "wb");
	fwrite(b->d, 1, b->n, f);
	fclose(f);
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "player/test/out/cpb";
	char path[512];
	snprintf(path, sizeof path, "%s/transfer.bin", dir);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("no %s\n", path); return 1; }
	static uint8_t buf[1 << 16];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	static char pu[8][64], pi[8][64];
	Transfer t = {.page_uris = pu, .page_uids = pi, .npages_max = 8};
	bool ok = cpb_transfer(buf, n, &t);
	printf("transfer ok=%d ctx=%s track=%s uid=%s cur=%s ts=%lld pos=%lld paused=%d shuffle=%d repctx=%d origin=%u restr=%u pages=%d",
	       ok, t.ctx_uri, t.track_uri, t.track_uid, t.current_uid, (long long)t.timestamp, (long long)t.pos, t.paused,
	       t.shuffle, t.rep_ctx, (unsigned)t.norigin, (unsigned)t.nctx_restr, t.npage_tracks);
	for (int i = 0; i < t.npage_tracks; i++) printf(" [%s %s]", pu[i], pi[i]);
	printf("\n");

	uint8_t g[16];
	for (int i = 0; i < 16; i++) g[i] = 0x0b;
	char b62[23];
	gid_b62(g, 16, b62);
	uint8_t d64[64];
	long dn = b64_decode("aGVsbG8gd29ybGQ=", d64);
	printf("gid %s b64 %.*s\n", b62, (int)dn, d64);

	const char *uris[] = {"spotify:track:aaa", "spotify:track:bbb", "spotify:track:ccc"};
	const char *uids[] = {"u0", "u1", "u2"};
	bool q[] = {false, false, true};
	for (int paused = 0; paused < 2; paused++) {
		CState s = {.name = "Pocket", .device_id = "dev123", .client_id = "cid", .version = "pocket 41",
		            .active = true, .loaded = true, .paused = paused, .reason = REASON_PLAYER_STATE_CHANGED, .volume = 40000,
		            .now_ms = 1762370000123LL, .pos_ms = 61234, .dur_ms = 200000, .started_at = 1762369999000ULL,
		            .last_msg_id = 77, .last_from = "pcdevice", .ctx_uri = "spotify:album:xyz", .session_id = "sess",
		            .playback_id = "pbid", .uris = uris, .uids = uids, .queued = q, .n = 3, .index = 1,
		            .origin = t.origin, .norigin = t.norigin, .ctx_restr = t.ctx_restr, .nctx_restr = t.nctx_restr,
		            .shuffle = true, .rep_ctx = false, .rep_track = true, .next_max = 80, .prev_max = 10};
		Pb b = {0};
		cpb_state(&b, &s);
		snprintf(path, sizeof path, "%s/state%d.bin", dir, paused);
		save(path, &b);
		pb_free(&b);
	}
	Pb e = {0};
	CState idle = {.name = "Pocket", .device_id = "dev123", .client_id = "cid", .version = "pocket 41",
	               .reason = REASON_NEW_DEVICE, .now_ms = 5, .session_id = "sess"};
	cpb_state(&e, &idle);
	snprintf(path, sizeof path, "%s/state_idle.bin", dir);
	save(path, &e);
	pb_free(&e);
	return 0;
}
