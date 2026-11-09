"""Tests for to16.py on real files made with ffmpeg: a 24-bit FLAC with tags (COMMENT too) and a cover, a 24-bit WAV,
a 16-bit FLAC and an MP3.

    uv run --with mutagen --with imageio-ffmpeg --with pytest pytest tools/test_to16.py
"""
import os
import subprocess
import time
from pathlib import Path

import imageio_ffmpeg
import pytest
from mutagen.flac import FLAC, Picture
from mutagen.id3 import TIT2
from mutagen.wave import WAVE

import to16

FF = imageio_ffmpeg.get_ffmpeg_exe()


def tone(path: Path, fmt: str, secs=2.0, rate=48000):
    path.parent.mkdir(parents=True, exist_ok=True)
    codec = {"s24flac": ["-c:a", "flac", "-sample_fmt", "s32"], "s16flac": ["-c:a", "flac", "-sample_fmt", "s16"],
             "s24wav": ["-c:a", "pcm_s24le"], "mp3": ["-c:a", "libmp3lame", "-b:a", "128k"]}[fmt]
    subprocess.run([FF, "-v", "error", "-y", "-f", "lavfi", "-i", f"sine=frequency=1000:sample_rate={rate}:duration={secs}",
                    "-ac", "2", *codec, str(path)], check=True)


def tag(path: Path):
    f = FLAC(path)
    f["TITLE"] = "Phobie"
    f["ARTIST"] = ["glaive", "ovine hall"]
    f["COMMENT"] = "the one ffmpeg drops"
    f["REPLAYGAIN_TRACK_GAIN"] = "-6.20 dB"
    p = Picture()
    p.type = 3
    p.mime = "image/png"
    p.data = b"\x89PNG\r\n\x1a\n" + b"x" * 200
    f.add_picture(p)
    f.save()


def test_converts_keeps_everything_and_skips_the_second_time(tmp_path):
    src = tmp_path / "Music" / "Mornye"
    tone(src / "a" / "hi.flac", "s24flac")
    tag(src / "a" / "hi.flac")
    tone(src / "wave.wav", "s24wav")
    tone(src / "cd.flac", "s16flac")
    tone(src / "lossy.mp3", "mp3")
    (src / "a" / "cover.jpg").write_bytes(b"jpg")
    (src / "notes.txt").write_text("not music")
    assert FLAC(src / "a" / "hi.flac").info.bits_per_sample == 24

    out = tmp_path / "card"
    assert to16.main([str(src), "-o", str(out)]) == 0

    hi, orig = out / "a" / "hi.flac", src / "a" / "hi.flac"
    f = FLAC(hi)
    assert f.info.bits_per_sample == 16
    assert f.info.sample_rate == 48000                       # not resampled
    assert f.info.total_samples == FLAC(orig).info.total_samples
    assert f["COMMENT"] == ["the one ffmpeg drops"]
    assert f["ARTIST"] == ["glaive", "ovine hall"]
    assert f["REPLAYGAIN_TRACK_GAIN"] == ["-6.20 dB"]
    assert [p.data for p in f.pictures] == [p.data for p in FLAC(orig).pictures]
    assert to16.bits(out / "wave.wav") == 16
    assert (out / "cd.flac").read_bytes() == (src / "cd.flac").read_bytes()      # 16-bit already: untouched
    assert (out / "lossy.mp3").read_bytes() == (src / "lossy.mp3").read_bytes()
    assert (out / "a" / "cover.jpg").exists()
    assert not (out / "notes.txt").exists()
    assert not list(out.rglob(".to16-*"))                    # no leftovers
    assert FLAC(orig).info.bits_per_sample == 24              # the original is only read

    dither, copy, done = to16.plan([src], out)               # again: nothing left to do
    assert not dither and not copy and len(done) == 5


def test_a_single_file_goes_straight_into_out(tmp_path):
    tone(tmp_path / "one.flac", "s24flac")
    assert to16.main([str(tmp_path / "one.flac"), "-o", str(tmp_path / "o")]) == 0
    assert to16.bits(tmp_path / "o" / "one.flac") == 16


def test_dry_run_writes_nothing(tmp_path):
    tone(tmp_path / "in" / "x.flac", "s24flac")
    assert to16.main([str(tmp_path / "in"), "-o", str(tmp_path / "o"), "-n"]) == 0
    assert not (tmp_path / "o").exists()


def test_two_sources_onto_one_name_stop_before_writing(tmp_path):
    tone(tmp_path / "CD1" / "01.flac", "s24flac")
    tone(tmp_path / "CD2" / "01.flac", "s24flac")
    with pytest.raises(SystemExit, match="would both become"):
        to16.main([str(tmp_path / "CD1"), str(tmp_path / "CD2"), "-o", str(tmp_path / "o")])
    assert not (tmp_path / "o").exists()


def test_never_writes_into_or_over_the_originals(tmp_path):
    music = tmp_path / "Music"
    tone(music / "Album" / "a.flac", "s24flac")
    tone(music / "a.flac", "s24flac")
    before = (music / "a.flac").read_bytes()
    with pytest.raises(SystemExit, match="inside"):
        to16.main([str(music), "-o", str(music / "out")])
    with pytest.raises(SystemExit, match="is inside -o"):
        to16.main([str(music / "Album"), "-o", str(music)])          # Album/a.flac would land on Music/a.flac
    with pytest.raises(SystemExit, match="is inside -o"):
        to16.main([str(music / "a.flac"), "-o", str(music)])          # a file onto itself
    assert (music / "a.flac").read_bytes() == before


def test_one_bad_file_doesnt_stop_the_rest(tmp_path, monkeypatch):
    tone(tmp_path / "in" / "bad.flac", "s24flac")
    tone(tmp_path / "in" / "good.flac", "s24flac")
    real = to16.copy_tags

    def flaky(src, dst):
        if src.name == "bad.flac":
            raise OSError("the card is full")
        real(src, dst)

    monkeypatch.setattr(to16, "copy_tags", flaky)
    assert to16.main([str(tmp_path / "in"), "-o", str(tmp_path / "o")]) == 1
    assert to16.bits(tmp_path / "o" / "good.flac") == 16
    assert not (tmp_path / "o" / "bad.flac").exists()
    assert not list((tmp_path / "o").glob(".to16-*"))


def test_wav_keeps_its_tags(tmp_path):
    w = tmp_path / "in" / "w.wav"
    tone(w, "s24wav")
    f = WAVE(w)
    f.add_tags()
    f.tags.add(TIT2(encoding=3, text="a wav title"))
    f.save()
    assert to16.main([str(tmp_path / "in"), "-o", str(tmp_path / "o")]) == 0
    out = WAVE(tmp_path / "o" / "w.wav")
    assert out.info.bits_per_sample == 16
    assert str(out.tags["TIT2"]) == "a wav title"


def test_a_retagged_copy_is_copied_again(tmp_path):
    tone(tmp_path / "in" / "cd.flac", "s16flac")
    assert to16.main([str(tmp_path / "in"), "-o", str(tmp_path / "o")]) == 0
    assert to16.plan([tmp_path / "in"], tmp_path / "o")[1] == []
    f = FLAC(tmp_path / "in" / "cd.flac")
    f["TITLE"] = "new"   # fits in the padding: the same size, a new date
    f.save()
    t = time.time() + 10
    os.utime(tmp_path / "in" / "cd.flac", (t, t))
    assert len(to16.plan([tmp_path / "in"], tmp_path / "o")[1]) == 1


def test_new_only_leaves_whats_there(tmp_path):
    tone(tmp_path / "in" / "old.flac", "s16flac")
    tone(tmp_path / "in" / "fresh.flac", "s24flac")
    (tmp_path / "o").mkdir()
    (tmp_path / "o" / "old.flac").write_bytes(b"the card's own copy")
    dither, copy, done = to16.plan([tmp_path / "in"], tmp_path / "o", only_new=True)
    assert [j.src.name for j in dither] == ["fresh.flac"] and not copy and len(done) == 1
