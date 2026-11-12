import math
import struct
import wave
from pathlib import Path

import pytest

import otci


def make_wav(path: Path, freq: float, frames=2000, channels=1, width=2, rate=44100):
    path.parent.mkdir(parents=True, exist_ok=True)
    peak = (1 << (8 * width - 1)) - 1
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels); w.setsampwidth(width); w.setframerate(rate)
        buf = bytearray()
        for n in range(frames):
            v = int(peak * 0.5 * math.sin(2 * math.pi * freq * n / rate))
            for _ in range(channels):
                buf += v.to_bytes(width, "little", signed=True)
        w.writeframes(bytes(buf))


def pcm(wav_bytes: bytes) -> bytes:
    import io
    with wave.open(io.BytesIO(wav_bytes)) as w:
        return w.readframes(w.getnframes())


@pytest.fixture
def kit(tmp_path):
    root = tmp_path / "Kick"
    for velo, f in ((0, 100), (30, 200), (87, 300)):
        for rr in (1, 2):
            make_wav(root / "Kick" / f"{velo:03d}_{rr}.wav", f + rr, width=3 if velo == 87 else 2,
                     channels=2 if velo == 30 else 1)
    return root


def test_roundtrip_audio_is_lossless(kit, tmp_path):
    out = tmp_path / "k.otci"
    inst = otci.build_from_folder(kit, "Test Kick")
    otci.write_otci(out, inst)
    back, _ = otci.read_otci(out)
    assert len(back.waves) == 6
    for a, b in zip(inst.waves, back.waves):
        src = pcm(otci.decode_flac(a.flac))
        assert pcm(otci.decode_flac(b.flac)) == src
        assert (a.frames, a.channels, a.rate) == (b.frames, b.channels, b.rate)


def test_structure_roundtrip(kit, tmp_path):
    out = tmp_path / "k.otci"
    otci.write_otci(out, otci.build_from_folder(kit, "Test Kick", "Mine"))
    back, at = otci.read_otci(out)
    assert (back.name, back.library) == ("Test Kick", "Mine")
    art = back.articulations[0]
    assert [l.velo for l in art.layers] == [0, 30, 87]
    assert all(len(l.round_robins) == 2 for l in art.layers)
    assert at["trigger_instrument" + "Version"] == "1"


def test_header_layout(kit, tmp_path):
    out = tmp_path / "k.otci"
    otci.write_otci(out, otci.build_from_folder(kit))
    d = out.read_bytes()
    assert d[:25] == otci.MAGIC and d[25:0x40] == b"\0" * (0x40 - 25)
    (psize,) = struct.unpack_from("<Q", d, 0x40)
    assert d[0x48 + psize + 12 : 0x48 + psize + 14] == b"\x78\xda"  # zlib level 9


def test_pick_layer_velocity_and_gain():
    art = otci.Articulation("k", [otci.Layer(0, 0.1, 1.4), otci.Layer(30, 0.45, 1.26), otci.Layer(87, 0.63, 1.0)])
    assert otci.pick_layer(art, 1)[0].velo == 0
    assert otci.pick_layer(art, 30)[0].velo == 30
    assert otci.pick_layer(art, 127)[0].velo == 87
    lay, g = otci.pick_layer(art, 127)
    assert g == pytest.approx(1.0)
    assert otci.pick_layer(art, 87)[1] == pytest.approx(0.63)


def test_bad_filename_rejected(tmp_path):
    make_wav(tmp_path / "K" / "A" / "snare.wav", 100)
    with pytest.raises(ValueError, match="expected <velo>_<rr>.wav"):
        otci.build_from_folder(tmp_path / "K")


def test_cover_image_roundtrip(kit, tmp_path):
    png = b"\x89PNG\r\n\x1a\n" + b"fakeimagebytes"
    (kit / "cover.png").write_bytes(png)
    out = tmp_path / "c.otci"
    otci.write_otci(out, otci.build_from_folder(kit))
    back, _ = otci.read_otci(out)
    assert back.image == png
    assert len(back.waves) == 6
    # audio still intact when an image trails the payload
    assert pcm(otci.decode_flac(back.waves[-1].flac))
