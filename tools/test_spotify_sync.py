"""Tests for spotify_sync.py against a pretend Spotify and real tagged files made with ffmpeg.

    uv run --with mutagen --with imageio-ffmpeg --with pytest pytest tools/test_spotify_sync.py
"""
import json
import subprocess
import urllib.error
from pathlib import Path

import pytest

import spotify_sync as ss


def make(path: Path, title=None, artist=None, isrc=None, secs=3.0):
    import imageio_ffmpeg
    path.parent.mkdir(parents=True, exist_ok=True)
    meta = []
    for k, v in (("title", title), ("artist", artist), ("ISRC", isrc)):
        if v:
            meta += ["-metadata", f"{k}={v}"]
    subprocess.run([imageio_ffmpeg.get_ffmpeg_exe(), "-v", "error", "-y", "-f", "lavfi", "-i", f"sine=frequency=440:duration={secs}",
                    *meta, str(path)], check=True)


def track(name, artists, secs=3.0, isrc=None, tid=None):
    return {"type": "track", "name": name, "artists": [{"name": a} for a in artists], "duration_ms": int(secs * 1000),
            "external_ids": {"isrc": isrc} if isrc else {}, "id": tid or name.lower().replace(" ", "")}


class FakeSpotify:
    def __init__(self):
        self.lists, self.tracks, self.likes, self.catalog = [], {}, [], {}
        self.hearts, self.unhearts = [], []
        self.scopes = {"playlist-read-private", "user-library-read", "user-library-modify"}

    def me(self): return {"id": "alex"}
    def playlists(self): return self.lists
    def liked(self): return list(self.likes)

    def playlist_tracks(self, pid):
        if pid not in self.tracks:
            raise urllib.error.HTTPError("u", 404, "nope", {}, None)
        return self.tracks[pid]

    def find(self, song): return self.catalog.get(song.title)
    def heart(self, ids): self.hearts += ids
    def unheart(self, ids): self.unhearts += ids


@pytest.fixture
def card(tmp_path):
    m = tmp_path / "music"
    make(m / "Ravyn Lenae/Bird's Eye/01 Love Me Not.flac", "Love Me Not", "Ravyn Lenae", "USAT22400001", 3)
    make(m / "Tyler/Flower Boy/See You Again.mp3", "See You Again (feat. Kali Uchis)", "Tyler, The Creator", secs=4)
    make(m / "loose/Beabadoobee - Coffee.wav", secs=2)
    make(m / "Quadeca/Scrapyard/Way Too Much.opus", "Way Too Much", "Quadeca", secs=5)
    (m / "playlists").mkdir()
    (m / "playlists/Road Trip.m3u8").write_text("#EXTM3U\n../Quadeca/Scrapyard/Way Too Much.opus\n")
    (m / "playlists/Liked Songs.m3u8").write_text("#EXTM3U\n../Quadeca/Scrapyard/Way Too Much.opus\n")
    return m


def resolve(card: Path, m3u: Path) -> list[Path]:
    """The player's reading of a playlist: comments skipped, paths from the playlists folder."""
    out = []
    for ln in m3u.read_text(encoding="utf-8").splitlines():
        ln = ln.strip()
        if ln and not ln.startswith("#"):
            out.append((card / "playlists" / ln).resolve())
    return out


def test_scan_reads_tags_and_file_names(card):
    songs = {s.path.name: s for s in ss.scan(card)}
    assert songs["01 Love Me Not.flac"].isrc == "USAT22400001"
    assert "Tyler, The Creator" in songs["See You Again.mp3"].artists
    assert (songs["Beabadoobee - Coffee.wav"].title, songs["Beabadoobee - Coffee.wav"].artists[0]) == ("Coffee", "Beabadoobee")
    assert songs["Way Too Much.opus"].secs == pytest.approx(5, abs=.2)
    assert len(songs) == 4


def test_matching(card):
    idx = ss.Index(ss.scan(card))
    assert idx.match(track("Love Me Not", ["Ravyn Lenae"], 200, "USAT22400001")).title == "Love Me Not"   # ISRC wins over length
    assert idx.match(track("See You Again (feat. Kali Uchis)", ["Tyler, The Creator", "Kali Uchis"], 4)) is not None
    assert idx.match(track("Coffee", ["beabadoobee"], 2)) is not None
    assert idx.match(track("Coffee", ["Someone Else"], 2)) is None
    assert idx.match(track("Way Too Much", ["Quadeca"], 60)) is None   # a different cut


def test_playlists(card, tmp_path):
    sp, state = FakeSpotify(), tmp_path / "3ds/player/spotify-sync.json"
    sp.lists = [{"id": "p1", "name": "Road Trip", "owner": {"id": "alex"}},
                {"id": "p2", "name": "Late: Night?", "owner": {"id": "friend"}},
                {"id": "dw", "name": "Discover Weekly", "owner": {"id": "spotify"}}]
    sp.tracks = {"p1": [track("Love Me Not", ["Ravyn Lenae"], 3, "USAT22400001"), track("Nope", ["Nobody"])],
                 "p2": [track("Coffee", ["Beabadoobee"], 2)]}
    r = ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    lists = card / "playlists"
    assert (lists / "Road Trip.m3u8").read_text() == "#EXTM3U\n../Quadeca/Scrapyard/Way Too Much.opus\n"   # yours: untouched
    rt = lists / "Road Trip (Spotify).m3u8"
    assert [p.name for p in resolve(card, rt)] == ["01 Love Me Not.flac"] and all(p.exists() for p in resolve(card, rt))
    assert "# Nobody - Nope" in rt.read_text()
    assert (lists / "Late_ Night_.m3u8").exists() and r["skipped"] == ["Discover Weekly"]
    sp.lists = [{"id": "p1", "name": "Summer", "owner": {"id": "alex"}}]   # renamed one, unfollowed the other
    ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    assert not rt.exists() and (lists / "Summer.m3u8").exists() and not (lists / "Late_ Night_.m3u8").exists()
    assert (lists / "Road Trip.m3u8").exists()


LMN = track("Love Me Not", ["Ravyn Lenae"], 3, "USAT22400001", "lmn")
COF = track("Coffee", ["beabadoobee"], 2, tid="cof")
GHOST = track("Missing One", ["Ghost"], tid="ghost")


def liked(card):
    return [p.name for p in resolve(card, card / "playlists/Liked Songs.m3u8")]


def test_likes_both_ways(card, tmp_path):
    sp, state, lp = FakeSpotify(), tmp_path / "s.json", card / "playlists/Liked Songs.m3u8"
    sp.likes, sp.catalog = [LMN, COF, GHOST], {"Way Too Much": "wtm"}
    r = ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    # first sync: a merge. The 3DS's like goes up to Spotify, Spotify's come down on top.
    assert liked(card) == ["01 Love Me Not.flac", "Beabadoobee - Coffee.wav", "Way Too Much.opus"]   # newest on top, as on the 3DS
    assert sp.hearts == ["wtm"] and r["liked_missing"] == ["Ghost - Missing One"]
    sp.likes.append(track("Way Too Much", ["Quadeca"], 5, tid="wtm"))

    # nothing changed anywhere: nothing happens
    r = ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    assert not (r["hearted"] or r["unhearted"] or r["liked_added"] or r["liked_removed"])

    # unlike Coffee on the 3DS, unlike Love Me Not on Spotify
    lp.write_text(lp.read_text().replace("../loose/Beabadoobee - Coffee.wav\n", ""))
    sp.likes = [x for x in sp.likes if x["id"] != "lmn"]
    r = ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    assert sp.unhearts == ["cof"] and liked(card) == ["Way Too Much.opus"]
    assert r["liked_removed"] == ["../Ravyn Lenae/Bird's Eye/01 Love Me Not.flac"]

    # a song moved on the card isn't an unlike
    sp.likes = [x for x in sp.likes if x["id"] != "cof"]
    (card / "Quadeca/Scrapyard/Way Too Much.opus").rename(card / "Quadeca/Way Too Much.opus")
    lp.write_text("#EXTM3U\n")
    ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    assert sp.unhearts == ["cof"]


def test_mass_unlike_needs_force(card, tmp_path):
    for i in range(12):
        ss_path = card / f"many/Song {i}.flac"
        make(ss_path, f"Song {i}", "Band", secs=1)
    sp, state, lp = FakeSpotify(), tmp_path / "s.json", card / "playlists/Liked Songs.m3u8"
    sp.likes = [track(f"Song {i}", ["Band"], 1, tid=f"s{i}") for i in range(12)]
    ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    lp.write_text("#EXTM3U\n")   # the card's list got wiped somehow
    with pytest.raises(SystemExit):
        ss.sync(sp, card, state, dry=False, mine_only=False, say=lambda *_: None)
    assert sp.unhearts == []
    ss.sync(sp, card, state, dry=False, mine_only=False, force=True, say=lambda *_: None)
    assert len(sp.unhearts) == 12


def test_dry_run_changes_nothing(card, tmp_path):
    sp, state = FakeSpotify(), tmp_path / "s.json"
    sp.likes, sp.catalog = [LMN], {"Way Too Much": "wtm"}
    sp.lists, sp.tracks = [{"id": "p1", "name": "Mix", "owner": {"id": "alex"}}], {"p1": [LMN]}
    before = {p: p.read_text() for p in (card / "playlists").iterdir()}
    r = ss.sync(sp, card, state, dry=True, mine_only=False, say=lambda *_: None)
    assert r["hearted"] == ["../Quadeca/Scrapyard/Way Too Much.opus"] and r["liked_added"]
    assert sp.hearts == [] and not state.exists()
    assert {p: p.read_text() for p in (card / "playlists").iterdir()} == before
