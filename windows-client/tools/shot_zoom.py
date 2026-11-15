#!/usr/bin/env python3
"""Crop / zoom / stack PNG screenshots with the standard library only.

Pillow is not installed on the build machine and shipping a dependency just to
look at a screenshot is absurd, so this reads and writes plain 8-bit RGB/RGBA
PNGs itself (GDI+ output is always non-interlaced, filter 0-4, 8 bpc).

    python tools/shot_zoom.py in.png -o out.png --crop 20,44,260,40 --zoom 6
    python tools/shot_zoom.py a.png b.png -o stack.png --crop 20,44,260,40 --zoom 4

Several inputs are stacked vertically with a 1 px separator, which is how two
text renderings get compared side by side without a GUI.
"""
import argparse
import struct
import sys
import zlib


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos = 8
    idat = bytearray()
    w = h = depth = ctype = 0
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace:
                raise SystemExit("only 8-bit non-interlaced PNG is supported")
        elif ctag == b"IDAT":
            idat += body
        elif ctag == b"IEND":
            break
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = line[i - nch] if i >= nch else 0
                upleft = prev[i - nch] if i >= nch else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if nch == 3:  # normalise to RGBA so stacking can blend alpha
        rgba = bytearray(w * h * 4)
        for i in range(w * h):
            rgba[i * 4:i * 4 + 3] = out[i * 3:i * 3 + 3]
            rgba[i * 4 + 3] = 255
        return w, h, rgba
    return w, h, out


def write_png(path, w, h, rgba):
    stride = w * 4
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def crop(w, h, rgba, box):
    x, y, cw, ch = box
    cw = min(cw, w - x)
    ch = min(ch, h - y)
    out = bytearray(cw * ch * 4)
    for row in range(ch):
        src = ((y + row) * w + x) * 4
        out[row * cw * 4:(row + 1) * cw * 4] = rgba[src:src + cw * 4]
    return cw, ch, out


def zoom(w, h, rgba, z):
    out = bytearray(w * z * h * z * 4)
    for row in range(h * z):
        src_row = (row // z) * w
        dst = row * w * z * 4
        for col in range(w):
            px = rgba[(src_row + col) * 4:(src_row + col) * 4 + 4]
            for k in range(z):
                o = dst + (col * z + k) * 4
                out[o:o + 4] = px
    return w * z, h * z, out


def stack(images, gap=1):
    w = max(i[0] for i in images)
    h = sum(i[1] for i in images) + gap * (len(images) - 1)
    out = bytearray(w * h * 4)
    for i in range(w * h):
        out[i * 4 + 3] = 255
    y = 0
    for cw, ch, pix in images:
        for row in range(ch):
            dst = ((y + row) * w) * 4
            out[dst:dst + cw * 4] = pix[row * cw * 4:(row + 1) * cw * 4]
        y += ch + gap
    return w, h, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--crop", help="x,y,w,h")
    ap.add_argument("--zoom", type=int, default=1)
    args = ap.parse_args()

    box = tuple(int(v) for v in args.crop.split(",")) if args.crop else None
    pieces = []
    for path in args.inputs:
        w, h, rgba = read_png(path)
        if box:
            w, h, rgba = crop(w, h, rgba, box)
        if args.zoom > 1:
            w, h, rgba = zoom(w, h, rgba, args.zoom)
        pieces.append((w, h, rgba))
        print(f"{path}: {w}x{h} -> stacked")
    w, h, rgba = stack(pieces)
    write_png(args.out, w, h, rgba)
    print(f"wrote {args.out} ({w}x{h})")


if __name__ == "__main__":
    sys.exit(main())
