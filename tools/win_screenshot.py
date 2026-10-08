# -*- coding: utf-8 -*-
"""截取指定 HWND 窗口的指定区域（屏幕坐标），保存为 PNG。
用法: python tools/win_screenshot.py <hwnd> <x> <y> <w> <h> <out.png> [zoom]
"""
import ctypes
import sys
import time

import screenshot as ss


def main():
    hwnd = int(sys.argv[1])
    x, y, w, h = (int(v) for v in sys.argv[2:6])
    out = sys.argv[6]
    zoom = int(sys.argv[7]) if len(sys.argv) > 7 else 1

    # 让窗口前置
    u = ctypes.windll.user32
    u.SetForegroundWindow(hwnd)
    time.sleep(0.4)

    # 用 WinAPI 直接拷窗位图到内存 DIB
    rect = ctypes.c_long()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rect),
                                       ctypes.byref(rect))
    # 退化：直接调 screenshot 库的全屏 grab（裁剪由 GetWindowRect 不易）
    # 简化方案：把 x y w h 当成相对窗口客户区，传给 screenshot 库
    cx = ss.user32.GetSystemMetrics(0)  # 屏幕宽
    print('screen size:', cx)
    # GetClientRect 给出客户区大小；位置用 GetWindowRect
    r = ctypes.c_long(); ct = ctypes.c_long(); cb = ctypes.c_long(); cr2 = ctypes.c_long()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(r), ctypes.byref(ct),
                                       ctypes.byref(cb), ctypes.byref(cr2))
    sx, sy, sw, sh = r.value, ct.value, cb.value - r.value, cr2.value - ct.value
    print('window rect:', sx, sy, sw, sh)
    w_full, h_full, bgra = ss.grab(sx + x, sy + y, w, h)
    rgb = ss.to_rgb(w_full, h_full, bgra)
    if zoom > 1:
        rgb = ss.zoom_rgb(rgb, w_full, h_full, zoom)
        w_full, h_full = w_full * zoom, h_full * zoom
    ss.write_png(out, w_full, h_full, rgb)
    print('saved', out, w_full, 'x', h_full)


if __name__ == '__main__':
    main()
