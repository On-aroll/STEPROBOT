# -*- coding: utf-8 -*-
"""零依赖截屏：ctypes 调 GDI 抓屏 -> 手写 PNG（zlib）。无需 Pillow / Add-Type。

用法: python screenshot.py <out.png> [x y w h]
"""
import ctypes
import struct
import sys
import zlib
from ctypes import wintypes

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
except Exception:
    try:
        user32.SetProcessDPIAware()
    except Exception:
        pass


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def grab(x=0, y=0, w=None, h=None):
    if w is None:
        w = user32.GetSystemMetrics(78) or user32.GetSystemMetrics(0)
    if h is None:
        h = user32.GetSystemMetrics(79) or user32.GetSystemMetrics(1)
    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(memdc, bmp)
    SRCCOPY = 0x00CC0020
    gdi32.BitBlt(memdc, 0, 0, w, h, hdc, x, y, SRCCOPY)

    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h  # top-down
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    bi.bmiHeader.biCompression = 0  # BI_RGB

    buf = ctypes.create_string_buffer(w * h * 4)
    got = gdi32.GetDIBits(memdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(0, hdc)
    if got == 0:
        raise RuntimeError("GetDIBits failed")
    return w, h, buf.raw


def to_rgb(w, h, bgra):
    mv = memoryview(bgra)
    rgb = bytearray(w * h * 3)
    rgb[0::3] = mv[2::4]  # R
    rgb[1::3] = mv[1::4]  # G
    rgb[2::3] = mv[0::4]  # B
    return rgb


def write_png(path, w, h, rgb):
    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        out += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return out

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    out = sys.argv[1]
    box = [int(v) for v in sys.argv[2:6]] if len(sys.argv) >= 6 else []
    # 可选放大倍数：第 7 个参数（仅裁剪时有效）
    zoom = int(sys.argv[6]) if len(sys.argv) >= 7 else 1
    w, h, bgra = grab(*box) if box else grab()
    rgb = to_rgb(w, h, bgra)
    if zoom > 1 and box:
        rgb = zoom_rgb(rgb, w, h, zoom)
        w, h = w * zoom, h * zoom
    write_png(out, w, h, rgb)
    print("saved", out, w, "x", h)


def zoom_rgb(rgb, w, h, z):
    """最近邻放大 z 倍，便于阅读细小的界面文字。"""
    out = bytearray(w * z * h * z * 3)
    stride = w * 3
    for y in range(h * z):
        sy = y // z
        src = sy * stride
        dst = y * w * z * 3
        row = rgb[src:src + stride]
        for x in range(w):
            out[dst + x * 3:dst + x * 3 + 3] = row[x * 3:x * 3 + 3]
            if z > 1:  # 复制同一行的后续像素
                pass
        # 水平复制：把已填的 w 个像素重复 z 次
        for k in range(1, z):
            out[dst + k * w * 3: dst + (k + 1) * w * 3] = out[dst:dst + w * 3]
    return out


if __name__ == "__main__":
    main()
