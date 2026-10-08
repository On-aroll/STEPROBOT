# -*- coding: utf-8 -*-
"""零依赖 Windows 交互工具：枚举窗口、按标题关闭、点击、杀进程。
无需 Add-Type / Pillow。
"""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

WM_CLOSE = 0x0010
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202

EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
EnumChildProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def list_top_windows(only_visible=True):
    out = []
    def cb(hwnd, lParam):
        if only_visible and not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        out.append({"hwnd": int(hwnd), "title": buf.value, "class": cls.value,
                    "visible": bool(user32.IsWindowVisible(hwnd))})
        return True
    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return out


def list_child_windows(parent_hwnd):
    out = []
    def cb(hwnd, lParam):
        length = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        out.append({"hwnd": int(hwnd), "title": buf.value, "class": cls.value})
        return True
    user32.EnumChildWindows(parent_hwnd, EnumChildProc(cb), 0)
    return out


def close_by_title(substr):
    substr = substr.lower()
    hits = []
    for w in list_top_windows(only_visible=True):
        if substr in w["title"].lower():
            user32.PostMessageW(w["hwnd"], WM_CLOSE, 0, 0)
            hits.append(w)
    return hits


def kill_pid(pid):
    h = kernel32.OpenProcess(0x0001 | 0x0010, False, pid)  # PROCESS_TERMINATE
    if not h:
        return False, f"OpenProcess failed: {ctypes.get_last_error()}"
    ok = bool(kernel32.TerminateProcess(h, 0))
    kernel32.CloseHandle(h)
    return ok, None


def kill_by_exe(name_substr):
    """Snapshot the PIDs whose image name contains the substring (case-insensitive)."""
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq *", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True)
    pids = []
    for line in out.stdout.splitlines():
        parts = line.strip().strip('"').split('","')
        if len(parts) >= 2 and name_substr.lower() in parts[0].lower():
            try:
                pids.append((parts[0], int(parts[1])))
            except ValueError:
                pass
    return pids


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def click_at(x, y, button="left"):
    MoveCursor(x, y)
    time.sleep(0.04)
    if button == "left":
        down, up = WM_LBUTTONDOWN, WM_LBUTTONUP
    elif button == "right":
        down, up = 0x0204, 0x0205
    else:
        raise ValueError(button)
    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                    ("mouseData", ctypes.c_ulong), ("dwFlags", ctypes.c_ulong),
                    ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]
    class INPUT(ctypes.Structure):
        _fields_ = [("type", ctypes.c_ulong), ("ii", MOUSEINPUT)]
    inp = INPUT(0, MOUSEINPUT(0, 0, 0, down, 0, None))
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    time.sleep(0.05)
    inp = INPUT(0, MOUSEINPUT(0, 0, 0, up, 0, None))
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def MoveCursor(x, y):
    pt = POINT(x, y)
    user32.SetCursorPos(pt.x, pt.y)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        import json
        print(json.dumps(list_top_windows(only_visible=False), ensure_ascii=False, indent=2))
    elif cmd == "close":
        hits = close_by_title(sys.argv[2])
        print("closed:", len(hits))
    elif cmd == "kill":
        import os
        pids = kill_by_exe(sys.argv[2])
        for name, pid in pids:
            ok, err = kill_pid(pid)
            print(f"{name} pid={pid} ok={ok}{'' if ok else ' err='+str(err)}")
    elif cmd == "click":
        click_at(int(sys.argv[2]), int(sys.argv[3]),
                  sys.argv[4] if len(sys.argv) > 4 else "left")
        print("clicked")