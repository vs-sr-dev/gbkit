"""PPM/PGM frames to PNG (no dependencies), optionally tiled into a sheet.

    python tools/ppm2png.py OUT.png A.ppm [B.ppm ...] [--cols 3] [--scale 1]
"""
import struct
import sys
import zlib


def read_pnm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    i += 1
    kind, w, h = parts[0], int(parts[1]), int(parts[2])
    px = d[i:]
    if kind == b'P5':
        px = bytes(b for g in px[:w * h] for b in (g, g, g))
    return w, h, px[:w * h * 3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main(argv):
    cols, scale, files = 3, 1, []
    it = iter(argv[1:])
    out = argv[0]
    for a in it:
        if a == '--cols':
            cols = int(next(it))
        elif a == '--scale':
            scale = int(next(it))
        else:
            files.append(a)
    imgs = [read_pnm(f) for f in files]
    w = max(i[0] for i in imgs)
    h = max(i[1] for i in imgs)
    cols = min(cols, len(imgs))
    rows = (len(imgs) + cols - 1) // cols
    W, H = cols * (w + 4), rows * (h + 4)
    sheet = bytearray(W * H * 3)
    for k, (iw, ih, px) in enumerate(imgs):
        ox, oy = (k % cols) * (w + 4), (k // cols) * (h + 4)
        for y in range(ih):
            o = ((oy + y) * W + ox) * 3
            sheet[o:o + iw * 3] = px[y * iw * 3:(y + 1) * iw * 3]
    if scale > 1:
        big = bytearray()
        for y in range(H):
            row = bytes(b for x in range(W) for b in sheet[(y * W + x) * 3:(y * W + x) * 3 + 3] * scale)
            big += row * scale
        sheet, W, H = big, W * scale, H * scale
    write_png(out, W, H, bytes(sheet))


if __name__ == '__main__':
    main(sys.argv[1:])
