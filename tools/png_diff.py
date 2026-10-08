# -*- coding: utf-8 -*-
"""纯 Python PNG 解码 + 两图差异分析（无需 Pillow）。
只支持本工具 screenshot.py 生成的格式（8bit RGB，逐行 filter）。
"""
import struct, sys, zlib

def decode(path):
    d = open(path, 'rb').read()
    pos, idat, w, h, bd, ct = 8, b'', 0, 0, 0, 0
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos+4])[0]
        tag = d[pos+4:pos+8]
        data = d[pos+8:pos+8+ln]
        if tag == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', data[:10])
        elif tag == b'IDAT':
            idat += data
        elif tag == b'IEND':
            break
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 3
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        if f == 1:
            for x in range(3, stride):
                line[x] = (line[x] + line[x-3]) & 0xFF
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:
            for x in range(stride):
                a = line[x-3] if x >= 3 else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            for x in range(stride):
                a = line[x-3] if x >= 3 else 0
                b = prev[x]
                c = prev[x-3] if x >= 3 else 0
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, out


def diff(pa, pb, thresh=24):
    wa, ha, A = decode(pa)
    wb, hb, B = decode(pb)
    if (wa, ha) != (wb, hb):
        print("size differ", wa, ha, wb, hb); return
    n = 0
    minx, miny, maxx, maxy = wa, ha, -1, -1
    stride = wa * 3
    for y in range(ha):
        base = y * stride
        for x in range(wa):
            i = base + x*3
            if (abs(A[i]-B[i]) > thresh or abs(A[i+1]-B[i+1]) > thresh
                    or abs(A[i+2]-B[i+2]) > thresh):
                n += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    total = wa * ha
    print(f"图像 {wa}x{ha}  总像素 {total}")
    print(f"差异像素: {n}  ({100.0*n/total:.2f}%)")
    if n:
        print(f"差异区域: x[{minx},{maxx}]  y[{miny},{maxy}]  "
              f"(宽 {maxx-minx+1} x 高 {maxy-miny+1})")


if __name__ == "__main__":
    diff(sys.argv[1], sys.argv[2])