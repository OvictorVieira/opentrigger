#!/usr/bin/env python3
"""OTCI: Open Trigger Compressed Instrument. Prototype reader/writer/builder.

Container layout:

  0x00  64 bytes  magic "OPEN TRIGGER INSTRUMENT 1", zero padded
  0x40  u64 LE    payload_size
  0x48  payload   concatenated audio blocks (here: FLAC streams), addressed by
                  `dataN_offset1` (offset relative to 0x48) in the metadata
  0x48+payload_size
        12 bytes  trailer header: u32 version(=1), u32 reserved, u32 0
        zlib      metadata chunk: b"OTI1" + u32 LE xml_len + xml (UTF-8)

Metadata XML is ONE element with flat attributes:
  trigger_instrument trigger_instrumentVersion data_count instrument_name
  instrument_version library_name articulation_count
  img_offset1 img_size (optional cover art blob, after the last wave)
  dataN_offset1 wdNvol wdNstereo wdNframes wdNsamples wdNcodec wdNrate
  art_Idx_<a>artName art_Idx_<a>_rel_time_{minTop,maxTop,minBot,maxBot}
  art_Idx_<a>layers_count
  art_Idx_<a>_layer_Idx_<l>{velo,minVol,maxVol,multy_wave_count}
  art_Idx_<a>_layer_Idx_<l>multy_wave_Idx_<r>samples_count_in_mw
  art_Idx_<a>_layer_Idx_<l>multy_wave_Idx_<r>wave_Idx_<k>   -> wave index
"""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
import wave
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from xml.sax.saxutils import quoteattr

MAGIC = b"OPEN TRIGGER INSTRUMENT 1"
HEADER_SIZE = 0x48
META_TAG = b"OTI1"


@dataclass
class Wave:
    flac: bytes
    frames: int
    channels: int
    rate: int


@dataclass
class Layer:
    velo: int
    min_vol: float = 0.1
    max_vol: float = 1.0
    round_robins: list[list[int]] = field(default_factory=list)  # [[wave idx]]


@dataclass
class Articulation:
    name: str
    layers: list[Layer] = field(default_factory=list)
    rel_time: tuple[float, float, float, float] = (0.2, 1.3, 0.2, 0.4)


@dataclass
class Instrument:
    name: str
    library: str = "User"
    articulations: list[Articulation] = field(default_factory=list)
    waves: list[Wave] = field(default_factory=list)
    image: bytes = b""  # optional cover art (PNG/JPEG), stored after the waves in the payload


# ---------------------------------------------------------------- audio codec
def encode_flac(wav_path: Path) -> Wave:
    with wave.open(str(wav_path), "rb") as w:
        ch, rate, frames, width = w.getnchannels(), w.getframerate(), w.getnframes(), w.getsampwidth()
    if ch not in (1, 2):
        raise ValueError(f"{wav_path}: only mono/stereo supported, got {ch} channels")
    if width not in (2, 3):
        raise ValueError(f"{wav_path}: only 16/24-bit PCM supported")
    out = subprocess.run(
        ["flac", "-s", "-8", "-f", "-o", "-", str(wav_path)],
        check=True, capture_output=True,
    ).stdout
    return Wave(out, frames, ch, rate)


def decode_flac(blob: bytes) -> bytes:
    """Return a WAV file image."""
    return subprocess.run(
        ["flac", "-d", "-s", "-c", "-"], input=blob, check=True, capture_output=True
    ).stdout


# ------------------------------------------------------------------- writing
def to_xml(inst: Instrument, offsets: list[int], img_offset: int = 0) -> str:
    a: list[tuple[str, str]] = [
        ("trigger_instrumentVersion", "1"),
        ("data_count", str(len(inst.waves))),
    ]
    for i, o in enumerate(offsets):
        a.append((f"data{i}_offset1", str(o)))
    if inst.image:
        a += [("img_offset1", str(img_offset)), ("img_size", str(len(inst.image)))]
    a += [
        ("instrument_name", inst.name),
        ("instrument_version", "0"),
        ("library_name", inst.library),
        ("articulation_count", str(len(inst.articulations))),
    ]
    for i, w in enumerate(inst.waves):
        a += [
            (f"wd{i}vol", "1.0"),
            (f"wd{i}stereo", "1" if w.channels == 2 else "0"),
            (f"wd{i}frames", str(w.frames)),
            (f"wd{i}samples", str(w.frames * w.channels)),
            (f"wd{i}rate", str(w.rate)),
            (f"wd{i}codec", "flac"),
        ]
    for ai, art in enumerate(inst.articulations):
        p = f"art_Idx_{ai}"
        a.append((f"{p}artName", art.name))
        for key, val in zip(("minTop", "maxTop", "minBot", "maxBot"), art.rel_time):
            a.append((f"{p}_rel_time_{key}", repr(float(val))))
        a.append((f"{p}layers_count", str(len(art.layers))))
        for li, lay in enumerate(art.layers):
            q = f"{p}_layer_Idx_{li}"
            a += [
                (f"{q}velo", str(lay.velo)),
                (f"{q}minVol", repr(float(lay.min_vol))),
                (f"{q}maxVol", repr(float(lay.max_vol))),
                (f"{q}multy_wave_count", str(len(lay.round_robins))),
            ]
            for ri, rr in enumerate(lay.round_robins):
                r = f"{q}multy_wave_Idx_{ri}"
                a.append((f"{r}samples_count_in_mw", str(len(rr))))
                for ki, widx in enumerate(rr):
                    a.append((f"{r}wave_Idx_{ki}", str(widx)))
    return "<trigger_instrument " + " ".join(f"{k}={quoteattr(v)}" for k, v in a) + "/>"


def write_otci(path: Path, inst: Instrument) -> None:
    payload = bytearray()
    offsets = []
    for w in inst.waves:
        offsets.append(len(payload))
        payload += w.flac
    img_offset = len(payload)
    payload += inst.image
    xml = to_xml(inst, offsets, img_offset).encode()
    meta = zlib.compress(META_TAG + struct.pack("<I", len(xml)) + xml, 9)
    head = MAGIC.ljust(0x40, b"\0") + struct.pack("<Q", len(payload))
    trailer = struct.pack("<III", 1, 0, 0)
    Path(path).write_bytes(head + bytes(payload) + trailer + meta)


# ------------------------------------------------------------------- reading
def _parse_attrs(xml: str) -> dict[str, str]:
    import xml.etree.ElementTree as ET

    return dict(ET.fromstring(xml).attrib)


def read_metadata(path: Path) -> dict[str, str]:
    """Reads only the metadata of an OTCI file (no audio)."""
    d = Path(path).read_bytes()
    (psize,) = struct.unpack_from("<Q", d, 0x40)
    i = d.find(b"\x78", HEADER_SIZE + psize)
    blob = zlib.decompressobj().decompress(d[i:])
    tag, (n,) = blob[:4], struct.unpack_from("<I", blob, 4)
    if tag != META_TAG:
        raise ValueError(f"unknown metadata tag {tag!r}")
    return _parse_attrs(blob[8 : 8 + n].decode("utf-8", "replace"))


def read_otci(path: Path) -> tuple[Instrument, dict[str, str]]:
    d = Path(path).read_bytes()
    if not d.startswith(MAGIC):
        raise ValueError("not an OTCI file")
    at = read_metadata(path)
    n = int(at["data_count"])
    psize = struct.unpack_from("<Q", d, 0x40)[0]
    img_off = int(at["img_offset1"]) if "img_offset1" in at else psize
    offs = [int(at[f"data{i}_offset1"]) for i in range(n)] + [img_off]
    waves = [
        Wave(d[HEADER_SIZE + offs[i] : HEADER_SIZE + offs[i + 1]],
             int(at[f"wd{i}frames"]), 2 if at[f"wd{i}stereo"] == "1" else 1, int(at[f"wd{i}rate"]))
        for i in range(n)
    ]
    image = d[HEADER_SIZE + img_off : HEADER_SIZE + img_off + int(at["img_size"])] if "img_size" in at else b""
    inst = Instrument(at["instrument_name"], at["library_name"], [], waves, image)
    for ai in range(int(at["articulation_count"])):
        p = f"art_Idx_{ai}"
        art = Articulation(
            at[f"{p}artName"],
            rel_time=tuple(float(at[f"{p}_rel_time_{k}"]) for k in ("minTop", "maxTop", "minBot", "maxBot")),
        )
        for li in range(int(at[f"{p}layers_count"])):
            q = f"{p}_layer_Idx_{li}"
            lay = Layer(int(at[f"{q}velo"]), float(at[f"{q}minVol"]), float(at[f"{q}maxVol"]))
            for ri in range(int(at[f"{q}multy_wave_count"])):
                r = f"{q}multy_wave_Idx_{ri}"
                lay.round_robins.append(
                    [int(at[f"{r}wave_Idx_{k}"]) for k in range(int(at[f"{r}samples_count_in_mw"]))]
                )
            art.layers.append(lay)
        inst.articulations.append(art)
    return inst, at


# ----------------------------------------------------------- trigger logic
def pick_layer(art: Articulation, velocity: int) -> tuple[Layer, float]:
    """HYPOTHESIS: highest layer with velo <= velocity; gain lerp(minVol,maxVol)."""
    layers = sorted(art.layers, key=lambda l: l.velo)
    idx = max((i for i, l in enumerate(layers) if l.velo <= velocity), default=0)
    lay = layers[idx]
    top = layers[idx + 1].velo - 1 if idx + 1 < len(layers) else 127
    span = max(top - lay.velo, 1)
    t = min(max((velocity - lay.velo) / span, 0.0), 1.0)
    return lay, lay.min_vol + (lay.max_vol - lay.min_vol) * t


# ------------------------------------------------------------------ builder
_NAME = re.compile(r"^(?P<velo>\d{1,3})[_-](?P<rr>\d+)\.wav$", re.I)


def build_from_folder(root: Path, name: str | None = None, library: str = "User") -> Instrument:
    """root/<Articulation>/<velo>_<rr>.wav ; velo = layer lower bound 0..127, rr = 1..N."""
    root = Path(root)
    inst = Instrument(name or root.name, library)
    for adir in sorted(p for p in root.iterdir() if p.is_dir()):
        art = Articulation(adir.name)
        groups: dict[int, dict[int, int]] = {}
        for f in sorted(adir.glob("*.wav")):
            m = _NAME.match(f.name)
            if not m:
                raise ValueError(f"{f}: expected <velo>_<rr>.wav, e.g. 000_1.wav")
            inst.waves.append(encode_flac(f))
            groups.setdefault(int(m["velo"]), {})[int(m["rr"])] = len(inst.waves) - 1
        velos = sorted(groups)
        for n, v in enumerate(velos):
            lo = 0.1 if n == 0 else 0.5
            art.layers.append(Layer(v, lo, 1.0, [[groups[v][k]] for k in sorted(groups[v])]))
        inst.articulations.append(art)
    if not inst.articulations:
        raise ValueError(f"{root}: no articulation folders found")
    for cover in ("cover.png", "cover.jpg", "cover.jpeg"):
        if (root / cover).is_file():
            inst.image = (root / cover).read_bytes()
            break
    return inst


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="WAV folder -> .otci")
    b.add_argument("folder", type=Path); b.add_argument("out", type=Path)
    b.add_argument("--name"); b.add_argument("--library", default="User")
    i = sub.add_parser("inspect", help="print structure of an .otci (metadata only)")
    i.add_argument("file", type=Path)
    x = sub.add_parser("extract", help=".otci -> WAV files")
    x.add_argument("file", type=Path); x.add_argument("outdir", type=Path)
    args = ap.parse_args(argv)

    if args.cmd == "build":
        inst = build_from_folder(args.folder, args.name, args.library)
        write_otci(args.out, inst)
        print(f"{args.out}: {len(inst.waves)} waves, {len(inst.articulations)} articulations")
    elif args.cmd == "inspect":
        at = read_metadata(args.file)
        print({k: at[k] for k in ("instrument_name", "library_name", "data_count", "articulation_count") if k in at})
        for ai in range(int(at["articulation_count"])):
            p = f"art_Idx_{ai}"
            print(f" art {ai}: {at[p + 'artName']!r}")
            for li in range(int(at[f"{p}layers_count"])):
                q = f"{p}_layer_Idx_{li}"
                print(f"   layer {li}: velo>={at[q + 'velo']} vol {at[q + 'minVol'][:5]}..{at[q + 'maxVol'][:5]} rr={at[q + 'multy_wave_count']}")
    elif args.cmd == "extract":
        inst, _ = read_otci(args.file)
        args.outdir.mkdir(parents=True, exist_ok=True)
        for i, w in enumerate(inst.waves):
            (args.outdir / f"wave_{i:03d}.wav").write_bytes(decode_flac(w.flac))
        print(f"{len(inst.waves)} waves -> {args.outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
