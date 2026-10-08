# -*- coding: utf-8 -*-
"""取指定 HWND 的窗口矩形（左上右下 4 个 LONG）。"""
import ctypes
import sys
from ctypes import wintypes

user32 = ctypes.windll.user32


class RECT(ctypes.Structure):
    _fields_ = [('left', wintypes.LONG), ('top', wintypes.LONG),
                ('right', wintypes.LONG), ('bottom', wintypes.LONG)]


def main():
    hwnd = int(sys.argv[1])
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    print(f'left={r.left} top={r.top} right={r.right} bottom={r.bottom}')
    print(f'w={r.right - r.left}  h={r.bottom - r.top}')


if __name__ == '__main__':
    main()
