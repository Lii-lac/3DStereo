# /// script
# requires-python = ">=3.11"
# dependencies = ["protobuf>=7.35"]
# ///
"""cpb.c against the real .proto files (connect/proof/pb, compiled from librespot 99b6088).

    uv run player/test/test_cpb.py          # makes transfer.bin, builds and runs test_cpb.c in docker, checks
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "connect" / "proof" / "pb"))
import connect_pb2 as cpb          # noqa: E402
import transfer_state_pb2 as tpb   # noqa: E402

OUT = ROOT / "player" / "test" / "out" / "cpb"
OUT.mkdir(parents=True, exist_ok=True)

ts = tpb.TransferState()
ts.options.shuffling_context = True
ts.options.repeating_context = True
ts.playback.timestamp = 1762370000000
ts.playback.position_as_of_timestamp = 73605
ts.playback.is_paused = True
ts.playback.current_track.gid = bytes.fromhex("0b" * 16)
ts.playback.current_track.uid = "trackuid"
ts.current_session.play_origin.feature_identifier = "artist"
ts.current_session.play_origin.referrer_identifier = "your_library"
ts.current_session.context.uri = "spotify:album:1XJET4J6WgkMkalolZojgR"
ts.current_session.context.restrictions.disallow_toggling_shuffle_reasons.append("x")
pg = ts.current_session.context.pages.add()
pg.tracks.add(uri="spotify:track:one", uid="uid1")
pg.tracks.add(uri="spotify:track:two", uid="uid2")
ts.current_session.current_uid = "curuid"
(OUT / "transfer.bin").write_bytes(ts.SerializeToString())

cmd = ("gcc -O1 -Wall -Wextra -fsanitize=address,undefined -Iplayer/source player/test/test_cpb.c player/source/cpb.c "
       "-o /tmp/tcpb && /tmp/tcpb player/test/out/cpb")
r = subprocess.run(["docker", "run", "--rm", "-v", f"{ROOT}:/p", "-w", "/p", "gcc:14", "sh", "-c", cmd],
                   capture_output=True, text=True)
print(r.stdout, r.stderr, sep="")
if r.returncode:
    sys.exit("build/run failed")

fails = []


def check(what, got, want):
    if got != want:
        fails.append(f"{what}: got {got!r}, want {want!r}")


line = r.stdout.splitlines()[0]
for want in ["ok=1", "ctx=spotify:album:1XJET4J6WgkMkalolZojgR", "track=spotify:track:0kPXFaUSdc0QjS9ngeENib",
             "uid=trackuid", "cur=curuid", "ts=1762370000000", "pos=73605", "paused=1", "shuffle=1", "repctx=1",
             "pages=2", "[spotify:track:one uid1]", "[spotify:track:two uid2]"]:
    if want not in line:
        fails.append(f"transfer line lacks {want}")
check("gid/b64 line", r.stdout.splitlines()[1], "gid 0kPXFaUSdc0QjS9ngeENib b64 hello world")

for paused in (0, 1):
    req = cpb.PutStateRequest.FromString((OUT / f"state{paused}.bin").read_bytes())
    d, ps = req.device.device_info, req.device.player_state
    check("member_type", req.member_type, cpb.CONNECT_STATE)
    check("is_active", req.is_active, True)
    check("reason", req.put_state_reason, cpb.PLAYER_STATE_CHANGED)
    check("last id", req.last_command_message_id, 77)
    check("last from", req.last_command_sent_by_device_id, "pcdevice")
    check("started", req.started_playing_at, 1762369999000)
    check("client ts", req.client_side_timestamp, 1762370000123)
    check("name", d.name, "Pocket")
    check("device_type", d.device_type, 9)
    check("device_id", d.device_id, "dev123")
    check("can_be_player", d.capabilities.can_be_player, True)
    check("disable_volume", d.capabilities.disable_volume, False)
    check("volume", d.volume, 40000)
    check("supported_types", list(d.capabilities.supported_types), ["audio/track", "audio/local"])
    check("is_controllable", d.capabilities.is_controllable, True)
    check("track", ps.track.uri, "spotify:track:bbb")
    check("track uid", ps.track.uid, "u1")
    check("track ctx", dict(ps.track.metadata), {"context_uri": "spotify:album:xyz", "entity_uri": "spotify:album:xyz"})
    check("index", ps.index.track, 1)
    check("ctx url", ps.context_url, "context://spotify:album:xyz")
    check("pos", ps.position_as_of_timestamp, 61234)
    check("dur", ps.duration, 200000)
    check("ts", ps.timestamp, 1762370000123)
    check("is_playing", ps.is_playing, True)
    check("is_paused", ps.is_paused, bool(paused))
    check("is_buffering", ps.is_buffering, bool(paused))
    check("speed", ps.playback_speed, 0.0 if paused else 1.0)
    check("pause reasons", list(ps.restrictions.disallow_pausing_reasons), ["not_playing"] if paused else [])
    check("resume reasons", list(ps.restrictions.disallow_resuming_reasons), [] if paused else ["not_paused"])
    check("prev", [t.uri for t in ps.prev_tracks], ["spotify:track:aaa"])
    check("next", [(t.uri, t.provider) for t in ps.next_tracks], [("spotify:track:ccc", "queue")])
    check("origin", ps.play_origin.feature_identifier, "artist")
    check("origin ref", ps.play_origin.referrer_identifier, "your_library")
    check("ctx restr", list(ps.context_restrictions.disallow_toggling_shuffle_reasons), ["x"])
    check("shuffle", ps.options.shuffling_context, True)
    check("rep track", ps.options.repeating_track, True)
    check("session", ps.session_id, "sess")
    check("playback_id", ps.playback_id, "pbid")
    if not ps.queue_revision:
        fails.append("no queue_revision")

idle = cpb.PutStateRequest.FromString((OUT / "state_idle.bin").read_bytes())
check("idle reason", idle.put_state_reason, cpb.NEW_DEVICE)
check("idle active", idle.is_active, False)
check("idle track", idle.device.player_state.track.uri, "")

print("\n".join(fails) if fails else "test_cpb: all good")
sys.exit(1 if fails else 0)
