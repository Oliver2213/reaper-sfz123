"""Writes a tiny SoundFont with audible, silent, empty, broken and key-limited presets.

    python3 tests/make_test_sf2.py out.sf2 [--all-silent]

Expected after loading: 3 silent presets hidden; "Sine" plays on all keys, "Narrow" only C4 to C5.
"""
import math
import struct
import sys


def chunk(cid, data):
    pad = b"\0" if len(data) % 2 else b""
    return cid + struct.pack("<I", len(data)) + data + pad


def lst(kind, *chunks):
    return chunk(b"LIST", kind + b"".join(chunks))


def name20(s):
    return s.encode()[:19].ljust(20, b"\0")


RATE = 44100
sine = [int(12000 * math.sin(2 * math.pi * 261.63 * i / RATE)) for i in range(RATE // 2)]
silence = [0] * (RATE // 2)
pad = [0] * 46

smpl = []
headers = []
for nm, data in (("sine", sine), ("silence", silence)):
    start = len(smpl)
    smpl += data
    end = len(smpl)
    smpl += pad
    headers.append((nm, start, end, start + 100, end - 100))

KEY_RANGE, INSTRUMENT, SAMPLE_ID, SAMPLE_MODES, ROOT_KEY = 43, 41, 53, 54, 58


def gen(op, amount):
    return struct.pack("<HH", op, amount & 0xFFFF)


def key_range(lo, hi):
    return lo | (hi << 8)


# instruments: list of zones, each a list of generators
instruments = [
    ("Sine", [[gen(SAMPLE_MODES, 1), gen(ROOT_KEY, 60), gen(SAMPLE_ID, 0)]]),
    ("Silent", [[gen(SAMPLE_MODES, 1), gen(SAMPLE_ID, 1)]]),
    ("Narrow", [[gen(KEY_RANGE, key_range(60, 72)), gen(SAMPLE_MODES, 1), gen(ROOT_KEY, 60), gen(SAMPLE_ID, 0)]]),
]

ALL_SILENT = "--all-silent" in sys.argv
presets = [] if ALL_SILENT else [("Sine", 0, [[gen(INSTRUMENT, 0)]])]
presets += [
    ("Silent", 1, [[gen(INSTRUMENT, 1)]]),
    ("Empty", 2, []),
    ("Broken", 4, [[gen(INSTRUMENT, 99)]]),
]
if not ALL_SILENT:
    presets.insert(3, ("Narrow", 3, [[gen(INSTRUMENT, 2)]]))


def build_bags(items, header_fmt):
    hdrs, bags, gens = b"", b"", b""
    gi = 0
    for header, zones in items:
        hdrs += header(len(bags) // 4)
        for zone in zones:
            bags += struct.pack("<HH", gi, 0)
            for g in zone:
                gens += g
                gi += 1
    return hdrs, bags, gens, gi


phdr, pbag, pgen, pgi = build_bags(
    [((lambda bi, n=n, p=p: name20(n) + struct.pack("<HHHIII", p, 0, bi, 0, 0, 0)), z) for n, p, z in presets], None)
phdr += name20("EOP") + struct.pack("<HHHIII", 0, 0, len(pbag) // 4, 0, 0, 0)
pbag += struct.pack("<HH", pgi, 0)
pgen += gen(0, 0)

inst, ibag, igen, igi = build_bags(
    [((lambda bi, n=n: name20(n) + struct.pack("<H", bi)), z) for n, z in instruments], None)
inst += name20("EOI") + struct.pack("<H", len(ibag) // 4)
ibag += struct.pack("<HH", igi, 0)
igen += gen(0, 0)

mod_terminal = b"\0" * 10
shdr = b""
for nm, start, end, ls, le in headers:
    shdr += name20(nm) + struct.pack("<IIIIIBbHH", start, end, ls, le, RATE, 60, 0, 0, 1)
shdr += name20("EOS") + b"\0" * 26

body = b"sfbk" + lst(b"INFO", chunk(b"ifil", struct.pack("<HH", 2, 1)), chunk(b"isng", b"EMU8000\0"), chunk(b"INAM", b"Silent Preset Test\0"))
body += lst(b"sdta", chunk(b"smpl", struct.pack("<%dh" % len(smpl), *smpl)))
body += lst(b"pdta", chunk(b"phdr", phdr), chunk(b"pbag", pbag), chunk(b"pmod", mod_terminal), chunk(b"pgen", pgen),
            chunk(b"inst", inst), chunk(b"ibag", ibag), chunk(b"imod", mod_terminal), chunk(b"igen", igen), chunk(b"shdr", shdr))

with open(sys.argv[1], "wb") as f:
    f.write(chunk(b"RIFF", body))
