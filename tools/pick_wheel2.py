# -*- coding: utf-8 -*-
"""从 PyPI JSON（已下载）里挑出最新兼容 wheel 地址。
用法: python pick_wheel2.py <json文件> [包名]
"""
import json
import sys

path = sys.argv[1]
name = sys.argv[2] if len(sys.argv) > 2 else 'package'
d = json.load(open(path, encoding='utf-8'))
v = d['info']['version']
print('latest', v)
found = False
for f in d['releases'][v]:
    n = f['filename']
    if n.endswith('.whl') and ('py3-none-any' in n or 'abi3-none-any' in n
                               or 'cp313-cp313-win_amd64' in n
                               or 'cp3-abi3-win_amd64' in n):
        print(f['url'], f['size'])
        found = True
if not found:
    print('NO_WHEEL')
