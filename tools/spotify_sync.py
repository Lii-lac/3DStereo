# /// script
# requires-python = ">=3.10"
# dependencies = ["mutagen>=1.47"]
# ///
"""Copy your Spotify playlists and Liked Songs onto Pocket's SD card, as playlists of your own files.

Nothing is streamed or downloaded: Spotify only says which songs are in which list, and each one is matched to a file
already in sdmc:/music by its tags (ISRC when both sides have it, then title, artist and length). A song you don't have
is written into the playlist as a comment, so the player skips it and you can see what's missing.

    uv run tools/spotify_sync.py                  # sync onto the card at S:
    uv run tools/spotify_sync.py --dry-run        # say what it would do, write nothing
    uv run tools/spotify_sync.py --music D:/music --token D:/3ds/player/spotify.json

Pair first with tools/spotify_pair.py. What the sync writes:
  - one <name>.m3u8 per playlist in sdmc:/music/playlists/, marked with a #SPOTIFY line so the next sync can update,
    rename or remove it. A playlist of yours with the same name is never touched; the Spotify one gets " (Spotify)".
  - Liked Songs.m3u8, synced both ways since the last run: a heart or unheart on Spotify reaches the card, and a like or
    unlike on the 3DS reaches Spotify. A song that's moved or renamed on the card isn't taken for an unlike, and if a lot
    of songs vanish at once it stops and asks for --force.
  - sdmc:/3ds/player/spotify-sync.json, the Liked Songs both sides agreed on last time.
"""
import argparse
import json
import re
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

AUDIO = {".flac", ".mp3", ".wav", ".ogg", ".oga", ".opus"}   # decoder.c's list
LIKED = "Liked Songs"
MARK = "#SPOTIFY:"
TOKEN_URL = "https://accounts.spotify.com/api/token"
API = "https://api.spotify.com/v1"


# ---- Spotify --------------------------------------------------------------------------------------------------------

class Spotify:
    """The few Web API calls the sync needs, with the token refreshed as it goes and 429s waited out."""

    def __init__(self, token_file: Path):
        self.file = token_file
        self.saved = json.loads(token_file.read_text())
        self.token = None

    def refresh(self) -> None:
        body = urllib.parse.urlencode({"grant_type": "refresh_token", "refresh_token": self.saved["refresh_token"],
                                       "client_id": self.saved["client_id"]}).encode()
        req = urllib.request.Request(TOKEN_URL, body, {"Content-Type": "application/x-www-form-urlencoded"})
        with urllib.request.urlopen(req, timeout=20) as r:
            tok = json.load(r)
        self.token = tok["access_token"]
        if tok.get("refresh_token") and tok["refresh_token"] != self.saved["refresh_token"]:   # Spotify rotated it
            self.saved["refresh_token"] = tok["refresh_token"]
            self.file.write_text(json.dumps(self.saved, indent=2))
        granted = set(tok.get("scope", "").split())
        need = {"playlist-read-private", "user-library-read"}
        if granted and not need <= granted:
            sys.exit(f"The saved pairing can't read {', '.join(sorted(need - granted))}: run tools/spotify_pair.py again.")
        self.scopes = granted

    def call(self, method: str, url: str, body=None, retried=False):
        if not self.token:
            self.refresh()
        if not url.startswith("http"):
            url = API + url
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(url, data, method=method, headers={"Authorization": f"Bearer {self.token}",
                                                                        "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read()
                return json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            if e.code == 401 and not retried:
                self.refresh()
                return self.call(method, url, body, True)
            if e.code == 429:
                time.sleep(int(e.headers.get("Retry-After", "2")) + 1)
                return self.call(method, url, body, retried)
            raise

    def pages(self, url: str):
        while url:
            page = self.call("GET", url)
            yield from page.get("items", [])
            url = page.get("next")

    def liked(self) -> list[dict]:
        return [it["track"] for it in self.pages("/me/tracks?limit=50") if it.get("track")]

    def playlists(self) -> list[dict]:
        return list(self.pages("/me/playlists?limit=50"))

    def playlist_tracks(self, pid: str) -> list[dict]:
        """A playlist's songs. Spotify moved these to /items in 2026, where each entry's song is under "item" (and
        "track" became a yes/no); the old /tracks shape is still read, in case an account gets the old API."""
        items = list(self.pages(f"/playlists/{pid}/items?limit=50"))
        out = []
        for it in items:
            t = it.get("item") if isinstance(it.get("item"), dict) else it.get("track") if isinstance(it.get("track"), dict) else None
            if t and t.get("type", "track") == "track":
                out.append(t)
        return out

    def me(self) -> dict:
        return self.call("GET", "/me")

    def find(self, song: "Song") -> str | None:
        """A Spotify track id for a song on the card: by ISRC if it has one, else by title and artist."""
        qs = [f"isrc:{song.isrc}"] if song.isrc else []
        if song.title and song.artists:
            qs.append(f'track:"{song.title}" artist:"{song.artists[0]}"')
        for q in qs:
            res = self.call("GET", "/search?type=track&limit=5&q=" + urllib.parse.quote(q))
            for t in res.get("tracks", {}).get("items", []):
                if song.isrc or matches(t, song):
                    return t["id"]
        return None

    # Spotify's 2026 API: saving and removing go through /me/library with track URIs; /me/tracks answers 403 to writes now
    def heart(self, ids: list[str]) -> None:
        for i in range(0, len(ids), 40):
            self.call("PUT", "/me/library?uris=" + ",".join(f"spotify:track:{x}" for x in ids[i:i + 40]))

    def unheart(self, ids: list[str]) -> None:
        for i in range(0, len(ids), 40):
            self.call("DELETE", "/me/library?uris=" + ",".join(f"spotify:track:{x}" for x in ids[i:i + 40]))


# ---- the card -------------------------------------------------------------------------------------------------------

@dataclass
class Song:
    path: Path             # on the PC
    rel: str               # from the playlists folder, the way the player writes it: "../Artist/Album/01 x.flac"
    title: str = ""
    artists: list[str] = field(default_factory=list)
    secs: float = 0
    isrc: str = ""


def split_artists(s: str) -> list[str]:
    return [a.strip() for a in re.split(r"\s*(?:,|;|/|&|\bfeat\.?|\bft\.?|\bfeaturing\b|\bx\b|\bwith\b)\s*", s, flags=re.I)
            if a.strip()]


def scan(music: Path) -> list[Song]:
    import mutagen
    songs = []
    for p in sorted(music.rglob("*")):
        if p.suffix.lower() not in AUDIO or not p.is_file() or any(part.startswith(".") for part in p.parts):
            continue
        rel = "../" + p.relative_to(music).as_posix()
        s = Song(p, rel)
        try:
            f = mutagen.File(p, easy=True)
        except Exception:
            f = None
        if f is not None:
            tags = f.tags or {}
            get = lambda k: (tags.get(k) or [""])[0] if hasattr(tags, "get") else ""
            s.title, s.isrc = get("title").strip(), get("isrc").strip().upper().replace("-", "")
            raw = list(tags.get("artist") or []) if hasattr(tags, "get") else []
            s.artists = raw + [a for v in raw for a in split_artists(v) if a != v]   # whole, then split: "Tyler, The Creator"
            s.secs = getattr(f.info, "length", 0) or 0
        if not s.title:   # no tags: "Artist - Title.flac", the way most rips are named
            stem = re.sub(r" \[[\w-]{11}\]$", "", p.stem)
            a, sep, t = stem.partition(" - ")
            s.title, s.artists = (t, [a] + split_artists(a)) if sep else (stem, s.artists)
        songs.append(s)
    return songs


# ---- matching -------------------------------------------------------------------------------------------------------

def norm(s: str) -> str:
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode().casefold().replace("&", " and ")
    return " ".join(re.sub(r"[^a-z0-9]+", " ", s).split())


def loose(title: str) -> str:
    """The title with the decorations off: (feat. X), [Remastered 2011], - Radio Edit."""
    t = re.sub(r"[\(\[][^\)\]]*[\)\]]", " ", title)
    t = re.sub(r"\s-\s.*$", "", t)
    t = re.sub(r"\b(feat|ft)\.?\s.*$", "", t, flags=re.I)
    return norm(t) or norm(title)


def artist_keys(names: list[str]) -> set[str]:
    return {norm(a) for a in names if norm(a)}


def matches(track: dict, song: Song, slack: float = 4) -> bool:
    if loose(track["name"]) != loose(song.title):
        return False
    if not artist_keys([a["name"] for a in track.get("artists", [])]) & artist_keys(song.artists):
        return False
    return not (song.secs and track.get("duration_ms")) or abs(song.secs - track["duration_ms"] / 1000) <= slack


class Index:
    def __init__(self, songs: list[Song]):
        self.songs = songs
        self.by_isrc = {s.isrc: s for s in songs if s.isrc}
        self.by_title: dict[str, list[Song]] = {}
        for s in songs:
            self.by_title.setdefault(loose(s.title), []).append(s)

    def match(self, t: dict) -> Song | None:
        isrc = (t.get("external_ids") or {}).get("isrc", "").upper().replace("-", "")
        if isrc and isrc in self.by_isrc:
            return self.by_isrc[isrc]
        want = artist_keys([a["name"] for a in t.get("artists", [])])
        cands = [s for s in self.by_title.get(loose(t.get("name", "")), []) if artist_keys(s.artists) & want or not s.artists]
        if not cands:
            return None
        ms = t.get("duration_ms") or 0
        exact = [s for s in cands if norm(s.title) == norm(t["name"])]
        pool = exact or cands
        if ms:   # several versions: the closest length, and none that's clearly a different cut
            pool = [s for s in pool if not s.secs or abs(s.secs - ms / 1000) <= 6] or ([] if all(s.secs for s in pool) else pool)
            pool.sort(key=lambda s: abs(s.secs - ms / 1000) if s.secs else 99)
        return pool[0] if pool else None


def label(t: dict) -> str:
    return f"{', '.join(a['name'] for a in t.get('artists', []))} - {t.get('name', '?')}"


# ---- playlists on the card ---------------------------------------------------------------------------------------

def safe_name(name: str) -> str:
    n = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(" .") or "Playlist"
    return n[:120]


def read_m3u(p: Path) -> tuple[str | None, list[str]]:
    """(the Spotify id it was synced from, if any; its song lines)."""
    sid, lines = None, []
    for ln in p.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        ln = ln.strip()
        if ln.startswith(MARK):
            sid = ln[len(MARK):].strip()
        elif ln and not ln.startswith("#"):
            lines.append(ln.replace("\\", "/"))
    return sid, lines


def m3u(sid: str | None, title: str, rels: list[str], missing: list[str]) -> str:
    out = ["#EXTM3U", f"#PLAYLIST:{title}"]
    if sid:
        out.append(MARK + sid)
    out += rels
    if missing:
        out.append(f"# not on the card ({len(missing)}):")
        out += [f"# {m}" for m in missing]
    return "\n".join(out) + "\n"


def write(p: Path, text: str, dry: bool) -> None:
    if dry:
        return
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")


# ---- the sync -------------------------------------------------------------------------------------------------------

def sync(sp, music: Path, state_file: Path, dry: bool, mine_only: bool, force: bool = False, say=print) -> dict:
    lists = music / "playlists"
    songs = scan(music)
    idx = Index(songs)
    say(f"{len(songs)} songs on the card.")
    me = sp.me()
    scopes = getattr(sp, "scopes", None)   # known once me() has refreshed the token
    if scopes and "user-library-modify" not in scopes:
        sys.exit("This pairing can only read your likes: run tools/spotify_pair.py again so the 3DS can change them.")
    state = json.loads(state_file.read_text()) if state_file.exists() else {}
    report = {"playlists": {}, "liked_added": [], "liked_removed": [], "liked_missing": [], "hearted": [], "unhearted": [],
              "not_on_spotify": [], "skipped": []}

    # playlists the last sync made, by Spotify id, and playlists that are yours, by name
    ours, theirs = {}, set()
    if lists.exists():
        for p in lists.glob("*.m3u*"):
            sid, _ = read_m3u(p)
            if sid:
                ours[sid] = p
            else:
                theirs.add(p.stem.casefold())

    seen, used = set(), {LIKED.casefold()} | theirs
    for pl in sp.playlists():
        if not pl:
            continue
        if mine_only and pl.get("owner", {}).get("id") != me.get("id"):
            continue
        pid, title = pl["id"], (pl.get("name") or "").strip() or "Untitled playlist"
        try:
            tracks = sp.playlist_tracks(pid)
        except urllib.error.HTTPError as e:
            if e.code in (403, 404):   # Spotify's mixes, and other people's playlists, are off limits to hobby apps
                report["skipped"].append(title)
                continue
            raise
        rels, missing = [], []
        for t in tracks:
            s = idx.match(t)
            (rels.append(s.rel) if s else missing.append(label(t)))
        name = safe_name(title)
        if name.casefold() in theirs or name.casefold() == LIKED.casefold():   # one of yours has the name
            name = safe_name(f"{title} (Spotify)")
        base, n = name, 2
        while name.casefold() in used:   # two Spotify playlists with one name
            name, n = f"{base} {n}", n + 1
        used.add(name.casefold())
        seen.add(pid)
        dest = lists / f"{name}.m3u8"
        old = ours.get(pid)
        if old and old != dest and not dry:
            old.unlink()   # renamed on Spotify
        write(dest, m3u(pid, title, rels, missing), dry)
        report["playlists"][name] = (len(rels), len(missing))
    for pid, p in ours.items():   # unfollowed or deleted on Spotify
        if pid not in seen and pid != "liked":
            report["playlists"][p.stem] = None
            if not dry:
                p.unlink()

    # Liked Songs, both ways. The snapshot in the state file is the list both sides agreed on last time, so a song that
    # left one side since then was unliked there, and a song new to one side was liked there.
    liked_p = lists / f"{LIKED}.m3u8"
    _, local = read_m3u(liked_p) if liked_p.exists() else (None, [])
    on_card = {s.rel.casefold(): s for s in songs}
    prev = {d["rel"].casefold(): d for d in state.get("liked", [])}
    sp_on_card = {}   # Spotify's likes that are on the card: rel.casefold() -> (rel, id), newest first, as Spotify lists them
    for t in sp.liked():
        s = idx.match(t)
        if s:
            sp_on_card.setdefault(s.rel.casefold(), (s.rel, t["id"]))
        else:
            report["liked_missing"].append(label(t))
    L = {}
    for r in local:
        L.setdefault(r.casefold(), r)

    # liked on the 3DS: heart it. Unliked on the 3DS (and the file's still there, so it wasn't just moved): unheart it.
    new_here = [L[k] for k in L if k not in prev and k not in sp_on_card and k in on_card]
    gone_here = [(prev[k]["rel"], sp_on_card[k][1]) for k in prev if k not in L and k in on_card and k in sp_on_card]
    # liked on Spotify: onto the card. Unliked on Spotify (it was there last time): off the card.
    new_there = [rel for k, (rel, _) in sp_on_card.items() if k not in L and k not in prev]
    gone_there = [L[k] for k in L if k in prev and prev[k].get("id") and k not in sp_on_card]
    if len(gone_here) > 10 and len(gone_here) > len(prev) / 4 and not force:
        sys.exit(f"{len(gone_here)} songs are gone from Liked Songs on the card since the last sync. If you really unliked "
                 "them all, run again with --force to unlike them on Spotify too.")

    ids = {k: i for k, (_, i) in sp_on_card.items()}
    hearts = []
    for rel in new_here:
        i = sp.find(on_card[rel.casefold()])
        if i:
            hearts.append(i)
            ids[rel.casefold()] = i
            report["hearted"].append(rel)
        else:
            report["not_on_spotify"].append(rel)
    if not dry:
        if hearts:
            sp.heart(hearts)
        if gone_here:
            sp.unheart([i for _, i in gone_here])
    report["unhearted"] = [r for r, _ in gone_here]
    report["liked_added"], report["liked_removed"] = new_there, gone_there

    drop = {r.casefold() for r in gone_there}
    final = new_there + [r for r in local if r.casefold() not in drop]   # newest on top, the way the 3DS keeps it
    write(liked_p, "#EXTM3U\n" + "".join(r + "\n" for r in final), dry)
    if not dry:
        snap, seen_k = [], set()
        for r in final:
            k = r.casefold()
            if k not in seen_k:
                seen_k.add(k)
                snap.append({"rel": r, "id": ids.get(k) or (prev.get(k) or {}).get("id")})
        state_file.parent.mkdir(parents=True, exist_ok=True)
        state_file.write_text(json.dumps({"liked": snap, "synced": time.strftime("%Y-%m-%d %H:%M")}, indent=1))
    return report


def show(report: dict, dry: bool, say=print) -> None:
    say("Would write:" if dry else "Synced:")
    for name, got in sorted(report["playlists"].items(), key=lambda kv: kv[0].casefold()):
        if got is None:
            say(f"  {name}: gone from Spotify, removed")
        else:
            have, miss = got
            say(f"  {name}: {have} on the card" + (f", {miss} missing" if miss else ""))
    parts = [f"{len(report[k])} {what}" for k, what in (("liked_added", "liked on Spotify, added"),
                                                         ("liked_removed", "unliked on Spotify, taken off"),
                                                         ("hearted", "liked on the 3DS, hearted on Spotify"),
                                                         ("unhearted", "unliked on the 3DS, unhearted on Spotify"),
                                                         ("liked_missing", "liked on Spotify but not on the card"),
                                                         ("not_on_spotify", "liked on the 3DS but not found on Spotify"))
             if report[k]]
    say("  Liked Songs: " + ("; ".join(parts) if parts else "nothing changed"))
    if report["skipped"]:
        say(f"  Skipped, Spotify won't share them with apps like this (its own mixes, and some other people's): "
            f"{', '.join(report['skipped'])}")


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # names with emoji in a Windows console
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--music", type=Path, default=Path("S:/music"), help="the card's music folder (default S:/music)")
    ap.add_argument("--token", type=Path, default=Path("S:/3ds/player/spotify.json"), help="what spotify_pair.py saved")
    ap.add_argument("--dry-run", action="store_true", help="write nothing, just say what would change")
    ap.add_argument("--force", action="store_true", help="go ahead even if it would unlike a lot of songs on Spotify")
    ap.add_argument("--mine", action="store_true", help="only playlists you own, not ones you follow")
    a = ap.parse_args()
    if not a.music.is_dir():
        sys.exit(f"No music folder at {a.music}: is the SD card in? (or pass --music)")
    if not a.token.exists():
        sys.exit(f"No pairing at {a.token}: run tools/spotify_pair.py first (or pass --token)")
    state = a.token.parent / "spotify-sync.json"
    show(sync(Spotify(a.token), a.music, state, a.dry_run, a.mine, a.force), a.dry_run)


if __name__ == "__main__":
    main()
