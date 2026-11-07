#!/usr/bin/env python3
"""Turn RGB565 frames into PNGs with a round mask, plus one contact sheet."""
import pathlib, struct, sys, zlib

W = H = 466
PAD = 26
BEZEL = (22, 24, 27)


def png(path, w, h, rgb):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    pathlib.Path(path).write_bytes(data)


def frame(path):
    raw = pathlib.Path(path).read_bytes()
    out = bytearray(W * H * 3)
    for y in range(H):
        for x in range(W):
            i = (y * W + x) * 2
            v = raw[i] | raw[i + 1] << 8
            r = (v >> 11) * 255 // 31
            g = ((v >> 5) & 63) * 255 // 63
            b = (v & 31) * 255 // 31
            dx, dy = x - 232.5, y - 232.5
            if dx * dx + dy * dy > 233 * 233:
                r, g, b = BEZEL
            out[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes((r, g, b))
    return bytes(out)


def main(src, dst):
    files = sorted(pathlib.Path(src).glob("*.rgb565"))
    frames = [frame(f) for f in files]
    for f, data in zip(files, frames):
        png(pathlib.Path(dst) / (f.stem + ".png"), W, H, data)
    cols = 3
    rows = (len(frames) + cols - 1) // cols
    sw, sh = cols * W + (cols + 1) * PAD, rows * H + (rows + 1) * PAD
    sheet = bytearray(bytes(BEZEL) * (sw * sh))
    for n, data in enumerate(frames):
        ox = PAD + (n % cols) * (W + PAD)
        oy = PAD + (n // cols) * (H + PAD)
        for y in range(H):
            sheet[(oy + y) * sw * 3 + ox * 3:(oy + y) * sw * 3 + (ox + W) * 3] = data[y * W * 3:(y + 1) * W * 3]
    png(pathlib.Path(dst) / "ui_preview.png", sw, sh, bytes(sheet))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
