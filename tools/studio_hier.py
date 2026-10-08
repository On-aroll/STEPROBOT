# -*- coding: utf-8 -*-
"""枚举 STEPStudio 主窗口的子窗口（原生 HWND 树），带文本与类名。
用法: python tools/studio_hier.py [hwnd] [最大层级]
"""
import ctypes
import sys
from ctypes import wintypes

user32 = ctypes.windll.user32
EnumChildProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

DEFAULT_HWND = 460840


def text_of(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    if not n:
        return ''
    b = ctypes.create_unicode_buffer(n + 1)
    user32.GetWindowTextW(hwnd, b, n + 1)
    return b.value


def cls_of(hwnd):
    b = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(hwnd, b, 64)
    return b.value


def walk(hwnd, depth, maxdepth, out):
    for h in children(hwnd):
        t = text_of(h)
        c = cls_of(h)
        vis = bool(user32.IsWindowVisible(h))
        if vis or t:
            out.append((depth, h, c, t))
        if depth < maxdepth:
            walk(h, depth + 1, maxdepth, out)


def children(hwnd):
    res = []

    def cb(h, lp):
        res.append(int(h))
        return True

    user32.EnumChildWindows(hwnd, EnumChildProc(cb), 0)
    return res


if __name__ == '__main__':
    hwnd = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_HWND
    md = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    out = []
    walk(hwnd, 0, md, out)
    print('children:', len(out))
    for d, h, c, t in out:
        if t:
            print('  ' * d + f'hwnd={h} class={c[:22]:22s} text={t[:46]!r}')
