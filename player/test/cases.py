# /// script
# requires-python = ">=3.10"
# dependencies = ["mutagen>=1.47"]
# ///
"""Cases for test_spotmatch.c: the card's songs as the 3DS library sees them, and what the PC tool's
matcher (tools/spotify_sync.py) answers for Spotify-shaped songs made from them, so the C port can
be held to the same answers.

    uv run player/test/cases.py S:/music player/test/out
"""
import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import spotify_sync as ss  # noqa: E402

music, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)


def latin(s: str) -> bool:   # where the two norms agree: Python drops other scripts, C keeps them
    return all(ord(c) < 0x180 or 0x2000 <= ord(c) < 0x2070 for c in s)


def stem_of(p: Path) -> str:
    return re.sub(r" \[[\w-]{11}\]$", "", p.stem)


# the library the way library.c builds it: the title tag or the file's name, the first artist tag
# or "Unknown artist"; whole seconds
lib = []
for s in ss.scan(music):
    import mutagen
    try:
        f = mutagen.File(s.path, easy=True)
        tags = (f.tags or {}) if f else {}
        title = (tags.get("title") or [""])[0].strip() if hasattr(tags, "get") else ""
        artist = (tags.get("artist") or [""])[0].strip() if hasattr(tags, "get") else ""
    except Exception:
        title = artist = ""
    lib.append(("sdmc:/music/" + s.path.relative_to(music).as_posix(), title or stem_of(s.path), artist or "Unknown artist", int(s.secs)))

# the same songs for the Python matcher, read by the rule sm_build uses
songs = []
for path, title, artist, secs in lib:
    stem = stem_of(Path(path))
    if title == stem and " - " in stem:
        a, _, t = stem.partition(" - ")
        arts = [a] + [x for x in ss.split_artists(a) if x != a]
    else:
        t, arts = title, ([artist] + [x for x in ss.split_artists(artist) if x != artist]) if artist != "Unknown artist" else []
    songs.append(ss.Song(Path(path), "../" + path[len("sdmc:/music/"):], t, arts, secs))
idx = ss.Index(songs)

with open(out / "lib.tsv", "w", encoding="utf-8", newline="\n") as f:
    for row in lib:
        f.write("\t".join(map(str, row)) + "\n")

rng = random.Random(7)
queries = []
for s in songs:
    pieces = ss.split_artists(s.artists[0]) if s.artists else ["Nobody"]
    ms = s.secs * 1000 + 400
    queries += [(s.title, pieces, ms), (s.title + " (feat. Someone Else)", pieces + ["Someone Else"], ms),
                (s.title + " - 2011 Remaster", pieces, ms), (s.title, pieces, ms + 20000), (s.title, ["Nobody At All"], ms),
                (s.title.upper(), pieces[:1], 0)]
for t, a, ms in [("Love Me Not", ["Ravyn Lenae"], 3000), ("See You Again (feat. Kali Uchis)", ["Tyler, The Creator", "Kali Uchis"], 4000),
                 ("Coffee", ["beabadoobee"], 2000)]:
    queries.append((t, a, ms))
with open(out / "queries.tsv", "w", encoding="utf-8", newline="\n") as f:
    n = 0
    for name, arts, ms in queries:
        if not latin(name) or not all(latin(a) for a in arts):
            continue
        hit = idx.match({"name": name, "artists": [{"name": a} for a in arts], "duration_ms": ms})
        f.write("\t".join([name, "\x1f".join(arts), str(ms), hit.rel if hit else "-"]) + "\n")
        n += 1
print(f"{len(lib)} songs, {n} queries")

with open(out / "strings.tsv", "w", encoding="utf-8", newline="\n") as f:
    extra = ["Beyoncé & JAY-Z", "Don't Stop  Me (Remastered 2011) [Live]", "Song - Radio Edit", "Tune ft. Guy", "Left Hand",
             "Featherweight", "Øystein", "naïve café", "(feat. nobody)", "Hello [x", "Raw ft.Guy", "a feat. b (c)"]
    for t in [x[1] for x in lib] + extra:
        if latin(t):
            f.write("\t".join([t, ss.norm(t), ss.loose(t)]) + "\n")
