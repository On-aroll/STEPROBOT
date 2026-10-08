# -*- coding: utf-8 -*-
"""从二进制里粗提可读字符串（ASCII + UTF-16LE），用于辨别安装包身份。"""
import sys, re

def strings(path, minlen=6, limit=4000):
    ascii_re = re.compile(rb'[ -~]{%d,}' % minlen)
    out = []
    with open(path, 'rb') as f:
        data = f.read()
    for m in ascii_re.finditer(data):
        out.append(m.group().decode('latin-1'))
        if len(out) >= limit:
            break
    # UTF-16LE
    u16 = re.compile((r'(?:[ -~]\x00){%d,}' % minlen).encode())
    for m in u16.finditer(data):
        out.append(m.group().decode('utf-16-le', 'ignore'))
        if len(out) >= limit * 2:
            break
    return out

if __name__ == "__main__":
    path = sys.argv[1]
    pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    ss = strings(path)
    seen = set()
    n = 0
    for s in ss:
        if pat and not pat.search(s):
            continue
        if s in seen:
            continue
        seen.add(s)
        print(s)
        n += 1
        if n >= 60:
            break
