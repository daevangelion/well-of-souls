#!/usr/bin/env python3
"""tools/oracle/bmp2png.py -- convert the oracle's screenshot BMPs to PNG.

The hook writes uncompressed 24-bit bottom-up BMPs of the 640x480 client area
(tools/oracle/hook/hook.c's dump_bmp) because that is the cheapest thing to
write from inside a DLL.  They are for a human to look at: deriving a click
coordinate means looking at the picture, and the read tool will not decode a
BMP.  This is a debugging aid, not part of the diff.

    bmp2png.py <in.bmp> <out.png>
"""
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise SystemExit("bmp2png: %s is not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    hdr = struct.unpack_from("<I", data, 14)[0]
    if hdr < 40:
        raise SystemExit("bmp2png: BITMAPCOREHEADER is not supported")
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if comp != 0:
        raise SystemExit("bmp2png: compressed BMP (comp=%d) is not supported" % comp)
    if bpp != 24:
        raise SystemExit("bmp2png: %d bpp is not supported" % bpp)
    stride = ((w * 3 + 3) // 4) * 4
    rows = []
    for y in range(abs(h)):
        # bottom-up unless the height is negative
        src = off + (y if h > 0 else (abs(h) - 1 - y)) * stride
        row = data[src:src + w * 3]
        rows.append(bytes(b for x in range(w) for b in row[x * 3:x * 3 + 3][::-1]))
    return w, abs(h), b"".join(rows)


def write_png(path, w, h, rgb):
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("bmp2png: PIL is not installed")
    Image.frombytes("RGB", (w, h), rgb).save(path)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    w, h, rgb = read_bmp(sys.argv[1])
    write_png(sys.argv[2], w, h, rgb)
    print("%s -> %s (%dx%d)" % (sys.argv[1], sys.argv[2], w, h))


if __name__ == "__main__":
    main()
