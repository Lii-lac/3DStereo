# /// script
# requires-python = ">=3.10"
# dependencies = ["mutagen>=1.47", "imageio-ffmpeg>=0.5"]
# ///
"""Make your music card-ready: 24-bit (or 32-bit) FLAC and WAV become 16-bit, properly dithered, and everything else is
copied as it is. The folders, the file names, the tags and the cover art all stay the same.

Pocket sends the 3DS 16-bit audio whatever you give it, so a 24-bit file only takes about twice the room on the card and
then gets cut down while it plays. This does the cut once, better: TPDF dither with a gentle high-pass shape, at the
file's own sample rate (nothing is resampled; Pocket does that itself on Best). You won't hear the difference; the card
will notice it's half full.

    uv run tools/to16.py ~/Music/Mornye -o S:/music/Mornye       # a folder, straight onto the card
    uv run tools/to16.py "song.flac" "Album/" -o out/            # files and folders, into out/
    uv run tools/to16.py ~/Music/Mornye -o S:/music/Mornye -n    # say what it would do, write nothing
    uv run tools/to16.py ~/Music/Mornye -o S:/music/Mornye --new # only songs the card doesn't have yet

A folder you give keeps its own layout under -o; a file goes straight into -o. A file already done is skipped (a 16-bit
copy with the same length, tags and cover; a plain copy with the same size and date), so running it again only does
what's new or retagged. The originals are only ever read: it stops before writing anything if -o is inside a source
folder, if a copy would land on an original, or if two files would land on one name.

How: ffmpeg's aresample with dither_method=triangular_hp to s16, then every tag and picture copied back from the
original with mutagen, word for word (ffmpeg drops some, COMMENT among them). Each result is checked: 16-bit, the same
length to the sample, the same tags, the same pictures, before it's moved into place. ReplayGain tags stay right, since
dither doesn't change loudness. Not kept: a cue sheet embedded in a FLAC (one file for a whole album), which Pocket
doesn't read anyway.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from mutagen.flac import FLAC
from mutagen.wave import WAVE

AUDIO = {".flac", ".mp3", ".wav", ".ogg", ".oga", ".opus"}   # what Pocket plays
EXTRAS = {".jpg", ".jpeg", ".png", ".m3u8"}                   # covers and playlists beside the music, inside a folder


def ffmpeg() -> str:
    import imageio_ffmpeg   # brings its own ffmpeg, so nothing has to be installed

    return imageio_ffmpeg.get_ffmpeg_exe()


def bits(path: Path) -> int:
    """Bits per sample of a FLAC or WAV; 0 for anything else (it's copied as it is)."""
    ext = path.suffix.lower()
    try:
        if ext == ".flac":
            return FLAC(path).info.bits_per_sample
        if ext == ".wav":
            return WAVE(path).info.bits_per_sample
    except Exception:
        return 0
    return 0


def tags_of(path: Path) -> list[tuple[str, str]]:
    """The tags as (key, value) pairs, sorted, keys lowercased: what the check compares."""
    ext = path.suffix.lower()
    if ext == ".flac":
        f = FLAC(path)
        return sorted((k.lower(), v) for k, v in (f.tags or []))
    if ext == ".wav":
        f = WAVE(path)
        return sorted((k, str(v)) for k, v in (f.tags or {}).items())
    return []


def pictures_of(path: Path) -> list[bytes]:
    if path.suffix.lower() == ".flac":
        return [p.write() for p in FLAC(path).pictures]
    return []


def frames(path: Path) -> int:
    ext = path.suffix.lower()
    if ext == ".flac":
        return FLAC(path).info.total_samples
    if ext == ".wav":
        i = WAVE(path).info
        return round(i.length * i.sample_rate)
    return -1


def copy_tags(src: Path, dst: Path) -> None:
    """Every tag and picture of src onto dst, as they were."""
    if src.suffix.lower() == ".flac":
        a, b = FLAC(src), FLAC(dst)
        b.delete()           # ffmpeg's own (and its encoder line)
        b.clear_pictures()
        if a.tags is not None:
            if b.tags is None:
                b.add_tags()
            b.tags.extend(a.tags)          # in order, keys as they were written
            b.tags.vendor = a.tags.vendor
        for p in a.pictures:
            b.add_picture(p)
        b.save()
    elif src.suffix.lower() == ".wav":
        a = WAVE(src)       # its ID3 chunk; ffmpeg carries the INFO tags itself
        if a.tags:
            b = WAVE(dst)
            if b.tags is None:
                b.add_tags()
            else:
                b.tags.clear()
            for v in a.tags.values():
                b.tags.add(v)
            b.save(v2_version=3 if a.tags.version[1] == 3 else 4)


def same(src: Path, dst: Path) -> str | None:
    """None if dst is src made 16-bit with nothing lost; otherwise what's wrong."""
    if bits(dst) != 16:
        return f"{bits(dst)}-bit, not 16"
    if frames(dst) != frames(src):
        return f"{frames(dst)} samples, the original has {frames(src)}"
    if tags_of(dst) != tags_of(src):
        return "the tags differ"
    if pictures_of(dst) != pictures_of(src):
        return "the cover art differs"
    return None


@dataclass
class Job:
    src: Path
    dst: Path


def convert(job: Job, ff: str) -> str:
    """Dither one file into job.dst; returns what happened, for the list at the end."""
    src, dst = job.src, job.dst
    flac = src.suffix.lower() == ".flac"
    codec = ["-c:a", "flac", "-compression_level", "8"] if flac else ["-c:a", "pcm_s16le"]
    # FLAC's tags come back from the original with mutagen; a WAV's INFO tags only ffmpeg carries over
    meta = ["-map_metadata", "-1"] if flac else ["-map_metadata", "0"]
    tmp = None
    try:
        dst.parent.mkdir(parents=True, exist_ok=True)
        # beside the destination, then moved into place: a stopped run never leaves half a song with the real name
        fd, name = tempfile.mkstemp(suffix=src.suffix, dir=dst.parent, prefix=".to16-")
        os.close(fd)
        tmp = Path(name)
        r = subprocess.run(
            [ff, "-nostdin", "-v", "error", "-y", "-i", str(src), "-map", "0:a:0", *meta,
             "-af", "aresample=osf=s16:dither_method=triangular_hp", *codec, str(tmp)],
            capture_output=True, text=True)
        if r.returncode:
            err = r.stderr.strip().splitlines()
            return f"FAILED  {src.name}: {err[-1] if err else 'ffmpeg failed'}"
        copy_tags(src, tmp)
        why = same(src, tmp)
        if why:
            return f"FAILED  {src.name}: {why}"
        os.replace(tmp, dst)
        return f"16-bit  {src.name}  ({src.stat().st_size / 1e6:.0f} -> {dst.stat().st_size / 1e6:.0f} MB)"
    except Exception as e:   # a file that won't read, a full card: this one's left out, the rest go on
        return f"FAILED  {src.name}: {e}"
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)


def ok_copy(src: Path, dst: Path) -> bool:
    try:
        return same(src, dst) is None
    except Exception:   # a broken file on the card: made again
        return False


def same_date(a: Path, b: Path) -> bool:
    d = abs(a.stat().st_mtime - b.stat().st_mtime)
    return d <= 2 or abs(d - 3600) <= 2   # FAT keeps dates to 2 s, and Windows shifts its dates an hour with the clocks


def place(src: Path, dst: Path) -> None:
    """A plain copy, beside the destination first, so a pulled card never leaves half a song under the real name."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(suffix=src.suffix, dir=dst.parent, prefix=".to16-")
    os.close(fd)
    try:
        shutil.copy2(src, name)
        os.replace(name, dst)
    finally:
        Path(name).unlink(missing_ok=True)


def key(p: Path) -> str:
    return os.path.normcase(str(p.resolve()))   # one file, however it's spelled (Windows ignores case)


def plan(sources: list[Path], out: Path, only_new: bool = False) -> tuple[list[Job], list[Job], list[str]]:
    """(to dither, to copy, already there) for everything under the sources. Exits, writing nothing, if an original
    could be written over or two files would land on one name."""
    o = key(out)
    for s in sources:
        if s.is_dir() and (o == key(s) or o.startswith(key(s).rstrip(os.sep) + os.sep)):
            sys.exit(f"-o {out} is inside {s}: the copies would mix with the originals. Pick a folder outside it.")
        if key(s).startswith(o.rstrip(os.sep) + os.sep):   # -o Music for Music/Mornye: your own files sit beside it
            sys.exit(f"{s} is inside -o {out}: the copies would land among your own files. Pick a folder outside it.")
    originals = {key(f) for s in sources for f in ([s] if s.is_file() else s.rglob("*")) if f.is_file()}
    dither, copy, done = [], [], []
    taken: dict[str, Path] = {}
    for s in sources:
        files = [s] if s.is_file() else sorted(p for p in s.rglob("*") if p.is_file())
        for f in files:
            ext = f.suffix.lower()
            if ext not in AUDIO and not (s.is_dir() and ext in EXTRAS):
                continue
            if f.name.startswith(".to16-"):
                continue
            dst = out / (f.relative_to(s) if s.is_dir() else f.name)
            k = key(dst)
            if k in originals:
                sys.exit(f"{dst} is one of the originals: give an -o somewhere else")
            if k in taken:
                sys.exit(f"{taken[k]} and {f} would both become {dst}: give them separate -o folders")
            taken[k] = f
            if only_new and dst.exists():   # --new: whatever's there already stays as it is
                done.append(str(dst))
                continue
            if bits(f) > 16:
                if dst.exists() and ok_copy(f, dst):
                    done.append(str(dst))
                else:
                    dither.append(Job(f, dst))
            elif dst.exists() and dst.stat().st_size == f.stat().st_size and same_date(dst, f):
                done.append(str(dst))   # copy2 keeps the date, and a retag changes it
            else:
                copy.append(Job(f, dst))
    return dither, copy, done


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+", type=Path, help="files or folders of music")
    ap.add_argument("-o", "--out", type=Path, required=True, help="where the card-ready copies go")
    ap.add_argument("-n", "--dry-run", action="store_true", help="say what would happen, write nothing")
    ap.add_argument("--new", action="store_true", help="only songs not in -o yet; leave the rest alone")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1), help="files at once")
    a = ap.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):   # a name the console can't show is printed with a ?, not a crash
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(errors="replace")

    for s in a.sources:
        if not s.exists():
            sys.exit(f"{s}: not found")
    dither, copy, done = plan(a.sources, a.out, a.new)
    print(f"{len(dither)} to make 16-bit, {len(copy)} to copy as they are, {len(done)} already there")
    if a.dry_run:
        for j in dither:
            print(f"  16-bit  {j.src}  ->  {j.dst}")
        for j in copy:
            print(f"  copy    {j.src}  ->  {j.dst}")
        return 0

    ff = ffmpeg() if dither else ""
    failed = 0
    with ThreadPoolExecutor(a.jobs) as pool:
        for line in pool.map(lambda j: convert(j, ff), dither):
            print(" ", line, flush=True)
            failed += line.startswith("FAILED")
    for j in copy:
        try:
            place(j.src, j.dst)
            print(f"  copied  {j.src.name}", flush=True)
        except OSError as e:
            print(f"  FAILED  {j.src.name}: {e}", flush=True)
            failed += 1
    print(f"done: {len(dither) + len(copy) - failed} written" + (f", {failed} FAILED (left out, see above)" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
