#!/usr/bin/env python3
"""Generate runtime/host/fonts/*.pgf: fallback PSP system fonts made from Noto (SIL OFL 1.1).

A PSP game's sceLibFont reads Sony's PGF fonts from flash0:/font, which AnyPSP cannot ship. These
files are the same PGF format (revision 2, 4-bit glyphs), pre-rasterised from Noto with FreeType
(through Pillow) at the sizes the PSP's fonts have, so runtime/host/font.cpp reads them with the
same code as a real PGF. Real fonts in flash0/font still win.

Source fonts (downloaded by the user, checked against the SHA-256 below):
  notofonts/notofonts.github.io @ d3abdda968b38bb6eb0ec59f6f9f58b8c2139bca
      fonts/NotoSans/hinted/ttf/NotoSans-{Regular,Bold,Italic,BoldItalic}.ttf
      fonts/NotoSerif/hinted/ttf/NotoSerif-{Regular,Bold,Italic,BoldItalic}.ttf
  notofonts/noto-cjk @ f8d157532fbfaeda587e826d4cd5b21a49186f7c
      Sans/SubsetOTF/JP/NotoSansJP-Medium.otf, Sans/SubsetOTF/KR/NotoSansKR-Regular.otf

  python tools/gen-fallback-fonts.py --src <folder with the files above> --out runtime/host/fonts
"""

import argparse
import collections
import hashlib
import pathlib
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

SOURCES = {
    "NotoSans-Regular.ttf": "478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823",
    "NotoSans-Bold.ttf": "1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913",
    "NotoSans-Italic.ttf": "467e3f89eeca4108bb8710a2b9e0cf2281ac56d5b0609211a83776d0505eecb5",
    "NotoSans-BoldItalic.ttf": "1b602a9d6353be42c91df097a4857b69fa2696f26703d7a33b54a15d87c2622c",
    "NotoSerif-Regular.ttf": "19e72cd8d595fae5bd74a5206f5d938512e1183d4fed7abb1ec1be1d7efa5f88",
    "NotoSerif-Bold.ttf": "96656aa5cec8f1d6fd0e804c1fad397e1a1cfa082e6642124e0bda68cd8363ce",
    "NotoSerif-Italic.ttf": "749e80e313ef711f9373c6cce17c72297ef05490b3dcda7967d1d5d90bf1183f",
    "NotoSerif-BoldItalic.ttf": "c710c5b9cf354ae46e7a10472a08019b28220fafeb5887a482a76856f8f6fc0b",
    "NotoSansJP-Medium.otf": "f396a3b57256e4515be9cb41f7aac54766d654890082a9f1b5c2451b5c093d8a",
    "NotoSansKR-Regular.otf": "69975a0ac8472717870aefeab0a4d52739308d90856b9955313b2ad5e0148d68",
}

# PSP font sizes (26.6 points) at 128 dpi: 0x288 = 10.125 pt = 18 px, 0x1C0 = 7 pt = 12.44 px.
LARGE, SMALL, DPI = 0x288, 0x1C0, 0x2000


def latin():
    ranges = [(0x20, 0x7E), (0xA0, 0x17F), (0x2010, 0x2026), (0x2030, 0x203A)]
    extra = [0x192, 0x2C6, 0x2DC, 0x20AC, 0x2122]
    return sorted({c for a, b in ranges for c in range(a, b + 1)} | set(extra))


def encoded(codec, first, last):
    """Code points in [first, last] with their encoding in `codec`."""
    out = {}
    for c in range(first, last + 1):
        try:
            out[c] = chr(c).encode(codec)
        except UnicodeEncodeError:
            pass
    return out


def japanese():
    # Shift JIS up to the end of JIS X 0208 level 1 (0x9872): symbols, kana, Greek, Cyrillic and the 2965
    # common kanji. Level 2 kanji would double the file; they fall back to the alternate character.
    sjis = encoded("shift_jis", 0x20, 0xFFFF)
    return sorted(set(latin()) | {c for c, e in sjis.items() if len(e) == 1 or int.from_bytes(e, "big") <= 0x9872})


def korean():
    # The 2350 precomposed syllables of KS X 1001 (other syllables only encode as 8-byte jamo sequences).
    hangul = {c for c, e in encoded("euc_kr", 0xAC00, 0xD7A3).items() if len(e) == 2}
    return sorted(set(latin()) | hangul | set(range(0x3131, 0x318F)) | set(range(0x3000, 0x3004)))


# (output file, source font, PGF name, PGF type, size in 26.6 points, characters)
FONTS = [
    ("sans-regular.pgf", "NotoSans-Regular.ttf", "AnyPSP Sans", "Regular", LARGE, latin),
    ("sans-bold.pgf", "NotoSans-Bold.ttf", "AnyPSP Sans", "Bold", LARGE, latin),
    ("sans-italic.pgf", "NotoSans-Italic.ttf", "AnyPSP Sans", "Italic", LARGE, latin),
    ("sans-bold-italic.pgf", "NotoSans-BoldItalic.ttf", "AnyPSP Sans", "Bold Italic", LARGE, latin),
    ("serif-regular.pgf", "NotoSerif-Regular.ttf", "AnyPSP Serif", "Regular", LARGE, latin),
    ("serif-bold.pgf", "NotoSerif-Bold.ttf", "AnyPSP Serif", "Bold", LARGE, latin),
    ("serif-italic.pgf", "NotoSerif-Italic.ttf", "AnyPSP Serif", "Italic", LARGE, latin),
    ("serif-bold-italic.pgf", "NotoSerif-BoldItalic.ttf", "AnyPSP Serif", "Bold Italic", LARGE, latin),
    ("sans-regular-small.pgf", "NotoSans-Regular.ttf", "AnyPSP Sans", "Regular", SMALL, latin),
    ("sans-bold-small.pgf", "NotoSans-Bold.ttf", "AnyPSP Sans", "Bold", SMALL, latin),
    ("sans-italic-small.pgf", "NotoSans-Italic.ttf", "AnyPSP Sans", "Italic", SMALL, latin),
    ("sans-bold-italic-small.pgf", "NotoSans-BoldItalic.ttf", "AnyPSP Sans", "Bold Italic", SMALL, latin),
    ("serif-regular-small.pgf", "NotoSerif-Regular.ttf", "AnyPSP Serif", "Regular", SMALL, latin),
    ("serif-bold-small.pgf", "NotoSerif-Bold.ttf", "AnyPSP Serif", "Bold", SMALL, latin),
    ("serif-italic-small.pgf", "NotoSerif-Italic.ttf", "AnyPSP Serif", "Italic", SMALL, latin),
    ("serif-bold-italic-small.pgf", "NotoSerif-BoldItalic.ttf", "AnyPSP Serif", "Bold Italic", SMALL, latin),
    ("sans-jp.pgf", "NotoSansJP-Medium.otf", "AnyPSP Sans JP", "Medium", LARGE, japanese),
    ("sans-kr.pgf", "NotoSansKR-Regular.otf", "AnyPSP Sans KR", "Regular", LARGE, korean),
]


class Bits:
    """LSB-first bit stream, the order PGF files use."""

    def __init__(self):
        self.data = bytearray()
        self.length = 0

    def put(self, value, count):
        for i in range(count):
            if self.length % 8 == 0:
                self.data.append(0)
            if (value >> i) & 1:
                self.data[self.length // 8] |= 1 << (self.length % 8)
            self.length += 1

    def align(self, bits):
        while self.length % bits:
            self.put(0, 1)


def packed(values, bpe):
    b = Bits()
    for v in values:
        b.put(v, bpe)
    b.align(32)
    return bytes(b.data)


def rle(nibbles):
    """PGF glyph pixels: a control nibble below 8 repeats the next nibble control+1 times,
    8 or more is followed by 16-control literal nibbles."""
    out, i = [], 0
    while i < len(nibbles):
        run = 1
        while i + run < len(nibbles) and run < 8 and nibbles[i + run] == nibbles[i]:
            run += 1
        if run >= 2:
            out += [run - 1, nibbles[i]]
            i += run
            continue
        literal = []
        while i < len(nibbles) and len(literal) < 8:
            if i + 1 < len(nibbles) and nibbles[i + 1] == nibbles[i]:
                break
            literal.append(nibbles[i])
            i += 1
        out += [16 - len(literal)] + literal
    return out


def rasterise(font, code, em):
    """Bitmap (rows of 0-15), left and top bearing in pixels, and advance in 26.6."""
    pad = int(em * 2)
    canvas = Image.new("L", (pad * 3, pad * 3), 0)
    ImageDraw.Draw(canvas).text((pad, pad * 2), chr(code), fill=255, font=font, anchor="ls")
    advance = round(font.getlength(chr(code))) * 64
    box = canvas.getbbox()
    if box is None:
        return [], 0, 0, 0, 0, advance
    x0, y0, x1, y1 = box
    pixels = canvas.crop(box).load()
    w, h = x1 - x0, y1 - y0
    nibbles = [(pixels[x, y] + 8) // 17 for y in range(h) for x in range(w)]
    if not any(nibbles):
        return [], 0, 0, 0, 0, advance
    return nibbles, w, h, x0 - pad, pad * 2 - y0, advance


def build(source, name, kind, size, chars):
    em = size / 64 * (DPI / 64) / 72
    font = ImageFont.truetype(str(source), size=em)
    ascent, descent = font.getmetrics()
    line = round(em) * 64
    glyphs = {}
    notdef = rasterise(font, 0x10FFFD, em)  # private use: never in a Noto cmap, so FreeType draws .notdef
    for code in chars:
        nibbles, w, h, left, top, advance = rasterise(font, code, em)
        if code != 0x20 and (nibbles, w, h, left, top, advance) == notdef:
            continue
        if w > 127 or h > 127 or not -64 <= left < 64 or not -64 <= top < 64:
            sys.exit(f"{name}: U+{code:04X} does not fit the PGF glyph fields")
        metrics = [(w * 64, h * 64), (left * 64, left * 64 - advance // 2), (top * 64, (line - h * 64) // 2),
                   (advance, line)]
        glyphs[code] = (nibbles, w, h, left, top, metrics)

    tables = []
    for t in range(4):
        common = collections.Counter(g[5][t] for g in glyphs.values()).most_common(255)
        tables.append([pair for pair, _ in common])
    index = [{pair: i for i, pair in enumerate(table)} for table in tables]

    codes = sorted(glyphs)
    first, last = codes[0], codes[-1]
    glyph_data = bytearray()
    pointers = []
    for code in codes:
        nibbles, w, h, left, top, metrics = glyphs[code]
        flags = 1  # bitmap stored in rows
        for t in range(4):
            if metrics[t] in index[t]:
                flags |= 4 << t
        fields = [(w, 7), (h, 7), (left & 0x7F, 7), (top & 0x7F, 7), (flags, 6), (0, 2 + 2 + 3 + 9)]
        for t in range(4):
            if flags & (4 << t):
                fields.append((index[t][metrics[t]], 8))
            else:
                fields += [(metrics[t][0] & 0xFFFFFFFF, 32), (metrics[t][1] & 0xFFFFFFFF, 32)]
        fields += [(n, 4) for n in rle(nibbles)]
        b = Bits()
        b.put(min((14 + sum(count for _, count in fields) + 7) // 8, 0x3FFF), 14)
        for value, count in fields:
            b.put(value, count)
        b.align(32)
        pointers.append(len(glyph_data) // 4)
        glyph_data += b.data

    missing = len(pointers)
    map_bits = missing.bit_length()
    pointer_bits = max(1, (len(glyph_data) // 4).bit_length())
    position = {code: i for i, code in enumerate(codes)}
    char_map = [position.get(c, missing) for c in range(first, last + 1)]

    all_metrics = [g[5] for g in glyphs.values()]
    max_advance = (max(m[3][0] for m in all_metrics), max(m[3][1] for m in all_metrics))
    header = bytearray(392)
    struct.pack_into("<HH4siiiiii", header, 0, 0, 392, b"PGF0", 2, 6, len(char_map), len(pointers), map_bits,
                     pointer_bits)
    header[34] = 4
    struct.pack_into("<iiii", header, 36, size, size, DPI, DPI)
    header[53:53 + len(name)] = name.encode()
    header[117:117 + len(kind)] = kind.encode()
    struct.pack_into("<HH", header, 182, first, last)
    struct.pack_into("<10i", header, 212,
                     ascent * 64, -descent * 64,
                     max(m[1][0] for m in all_metrics), max(m[2][0] for m in all_metrics),
                     min(m[1][1] for m in all_metrics), max(m[2][1] for m in all_metrics),
                     max_advance[0], max_advance[1],
                     max(m[0][0] for m in all_metrics), max(m[0][1] for m in all_metrics))
    struct.pack_into("<HH", header, 252, max(g[1] for g in glyphs.values()), max(g[2] for g in glyphs.values()))
    struct.pack_into("<4B", header, 258, *(len(t) for t in tables))
    struct.pack_into("<ii", header, 364, 0, 8)

    out = bytearray(header)
    for table in tables:
        for a, b in table:
            out += struct.pack("<ii", a, b)
    out += packed(char_map, map_bits)
    out += packed(pointers, pointer_bits)
    out += glyph_data
    return bytes(out), len(codes)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    for file, digest in SOURCES.items():
        path = args.src / file
        if not path.is_file():
            sys.exit(f"missing {path}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            sys.exit(f"{path} does not match its pinned SHA-256")
    args.out.mkdir(parents=True, exist_ok=True)
    for file, source, name, kind, size, chars in FONTS:
        data, count = build(args.src / source, name, kind, size, chars())
        (args.out / file).write_bytes(data)
        print(f"{file}: {count} glyphs, {len(data)} bytes")


if __name__ == "__main__":
    main()
