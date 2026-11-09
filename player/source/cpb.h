// Spotify Connect's wire bits, with no 3DS in them (so test/test_cpb.c builds them on a PC):
// a protobuf writer and reader, base64, and the two messages that matter, the device's
// PutStateRequest going up and the TransferState coming down. Field numbers are librespot's
// protocol/proto (connect.proto, player.proto, transfer_state.proto), checked against it by
// test/test_cpb.py, which decodes our bytes with the real .proto files.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint8_t *d; size_t n, cap; } Pb;
void pb_free(Pb *b);
void pb_varint(Pb *b, int field, uint64_t v);       // written even when 0
void pb_bool(Pb *b, int field, bool v);             // only when true, like proto3
void pb_str(Pb *b, int field, const char *s);       // skipped when NULL or empty
void pb_bytes(Pb *b, int field, const void *d, size_t n);
void pb_msg(Pb *b, int field, const Pb *sub);       // written even when empty
void pb_double(Pb *b, int field, double v);
void pb_map(Pb *b, int field, const char *k, const char *v);   // one map<string,string> entry

// Reading: one field at a time. wt 0: *v; wt 2: *data/*len; wt 1/5: *v holds the raw bits.
typedef struct { const uint8_t *p, *end; } PbR;
bool pbr_next(PbR *r, int *field, int *wt, uint64_t *v, const uint8_t **data, size_t *len);

// base64 (standard alphabet, padding optional): the decoded length, or -1 if it isn't base64.
// out needs (strlen(in) / 4 + 1) * 3 bytes.
long b64_decode(const char *in, uint8_t *out);
// Spotify's 16-byte track gid as the 22-character id in its uris
void gid_b62(const uint8_t *gid, size_t n, char out[23]);

enum { REASON_NEW_DEVICE = 3, REASON_PLAYER_STATE_CHANGED = 4, REASON_VOLUME_CHANGED = 5, REASON_BECAME_INACTIVE = 7 };
enum { DEVICE_GAME_CONSOLE = 9 };

typedef struct {
	const char *name, *device_id, *client_id, *version;
	bool active, loaded, paused;
	int reason;
	uint32_t volume;            // 0..65535, Spotify's slider
	int64_t now_ms;             // Spotify's clock (unix ms), not the 3DS's
	int64_t pos_ms, dur_ms;     // where the song is at now_ms, and how long it is
	uint64_t started_at;        // when we became the active device, unix ms
	uint32_t last_msg_id;
	const char *last_from;      // the device that sent the last command
	const char *ctx_uri, *session_id, *playback_id;
	const char *const *uris, *const *uids;   // the context in play order, n of them
	const bool *queued;         // per song: put in the queue by hand (provider "queue")
	int n, index;
	const uint8_t *origin; size_t norigin;   // the PlayOrigin Spotify gave us, as it came
	const uint8_t *ctx_restr; size_t nctx_restr;
	bool shuffle, rep_ctx, rep_track;
	int next_max, prev_max;     // how many songs either side go up (librespot: 80 / 10)
} CState;
void cpb_state(Pb *out, const CState *s);

typedef struct {
	char ctx_uri[256];
	char track_uri[64], track_uid[64], current_uid[64];
	int64_t timestamp, pos;
	bool paused;
	bool shuffle, rep_ctx, rep_track;
	const uint8_t *origin; size_t norigin;     // pointers into the decoded message
	const uint8_t *ctx_restr; size_t nctx_restr;
	int npage_tracks;
	// the context's tracks the transfer carried, if any (usually a one-song slice)
	char (*page_uris)[64]; char (*page_uids)[64]; int npages_max;
} Transfer;
bool cpb_transfer(const uint8_t *d, size_t n, Transfer *t);

// ClusterUpdate: which device is active now ("" if none said)
bool cpb_cluster_active(const uint8_t *d, size_t n, char *out, int cap);

// SetVolumeCommand: the volume (0..65535), -1 if it isn't one
long cpb_volume(const uint8_t *d, size_t n);
