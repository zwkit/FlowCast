"""Crop and nearest-neighbour zoom a PNG without Pillow.

Only exists because this box has no PIL and the window buttons are 30 px wide:
reviewing them means looking at them at 6x, and `--shot` renders at the desktop
DPI. Handles the subset GDI+ writes (8-bit RGB/RGBA, no interlace).

    python tools/png_zoom.py in.png out.png x y w h [scale] [--row a.png b.png ...]
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    idat = b""
    w = h = depth = color = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            w, h, depth, color, comp, filt, inter = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and color in (2, 6) and inter == 0, "unsupported PNG"
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    bpp = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, out


def write_png(path, w, h, bpp, pixels):
    ctype = 6 if bpp == 4 else 2
    stride = w * bpp
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(
            ">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def crop_zoom(src, x, y, cw, ch, scale):
    w, h, bpp, px = read_png(src)
    assert x + cw <= w and y + ch <= h, "crop outside the image"
    out = bytearray(cw * scale * ch * scale * bpp)
    stride = w * bpp
    for oy in range(ch * scale):
        sy = y + oy // scale
        rowbase = sy * stride
        obase = oy * cw * scale * bpp
        for ox in range(cw * scale):
            sx = x + ox // scale
            si = rowbase + sx * bpp
            di = obase + ox * bpp
            out[di:di + bpp] = px[si:si + bpp]
    return cw * scale, ch * scale, bpp, out


def main():
    args = [a for a in sys.argv[1:]]
    scale = 6
    row = None
    if "--row" in args:
        i = args.index("--row")
        row = args[i + 1:]
        args = args[:i]
    if row:
        out_path, x, y, cw, ch = args[0], *map(int, args[1:5])
        if len(args) > 5:
            scale = int(args[5])
        parts = [crop_zoom(p, x, y, cw, ch, scale) for p in row]
        bpp = parts[0][2]
        tw, th = parts[0][0], parts[0][1]
        total = tw * len(parts)
        merged = bytearray(total * th * bpp)
        for k, (pw, ph, pbpp, px) in enumerate(parts):
            for yy in range(th):
                src = yy * pw * pbpp
                dst = yy * total * bpp + k * tw * bpp
                merged[dst:dst + pw * pbpp] = px[src:src + pw * pbpp]
        write_png(out_path, total, th, bpp, merged)
        print("wrote %s (%dx%d)" % (out_path, total, th))
        return
    src, dst, x, y, cw, ch = args[0], args[1], *map(int, args[2:6])
    if len(args) > 6:
        scale = int(args[6])
    w, h, bpp, px = crop_zoom(src, x, y, cw, ch, scale)
    write_png(dst, w, h, bpp, px)
    print("wrote %s (%dx%d)" % (dst, w, h))


if __name__ == "__main__":
    main()
