"""Turns concatenated binary PPMs (stdin) into a PNG or animated PNG: python3 tools/apng.py out.png [delay_ms] < frames.ppm"""
import struct
import sys
import zlib


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def read_ppms(data):
    frames, i = [], 0
    while i < len(data):
        header_end = data.index(b"\n", i)
        _, w, h, _ = data[i:header_end].split()
        w, h = int(w), int(h)
        start = header_end + 1
        frames.append((w, h, data[start:start + w * h * 3]))
        i = start + w * h * 3
    return frames


def main():
    out, delay = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 80
    frames = read_ppms(sys.stdin.buffer.read())
    w, h, _ = frames[0]
    png = [b"\x89PNG\r\n\x1a\n", chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))]
    if len(frames) > 1:
        png.append(chunk(b"acTL", struct.pack(">II", len(frames), 0)))
    seq = 0
    for n, (_, _, rgb) in enumerate(frames):
        raw = zlib.compress(b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)), 9)
        if len(frames) > 1:
            png.append(chunk(b"fcTL", struct.pack(">IIIIIHHBB", seq, w, h, 0, 0, delay, 1000, 0, 0)))
            seq += 1
        if n == 0:
            png.append(chunk(b"IDAT", raw))
        else:
            png.append(chunk(b"fdAT", struct.pack(">I", seq) + raw))
            seq += 1
    png.append(chunk(b"IEND", b""))
    with open(out, "wb") as f:
        f.write(b"".join(png))


main()
