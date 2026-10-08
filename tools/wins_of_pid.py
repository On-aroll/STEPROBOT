# -*- coding: utf-8 -*-
"""列出某个 PID 拥有的顶层窗口。
用法: python tools/wins_of_pid.py <pid> [<pid2> ...]
"""
import ctypes
import sys
from ctypes import wintypes

user32 = ctypes.windll.user32
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def wins_of(pid):
    res = []

    def cb(hwnd, lp):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            b = ctypes.create_unicode_buffer(128)
            user32.GetWindowTextW(hwnd, b, 128)
            c = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, c, 64)
            res.append((int(hwnd), b.value, c.value, bool(user32.IsWindowVisible(hwnd))))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return res


if __name__ == '__main__':
    for arg in sys.argv[1:]:
        pid = int(arg)
        ws = wins_of(pid)
        print('PID %d -> %d windows' % (pid, len(ws)))
        for hwnd, title, cls, vis in ws[:12]:
            print('  hwnd=%d visible=%s class=%s title=%r' % (hwnd, vis, cls, title))
