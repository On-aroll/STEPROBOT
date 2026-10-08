# -*- coding: utf-8 -*-
"""打印某个 PyPI 包的最新兼容 wheel 下载地址。"""
import json, sys, urllib.request
name = sys.argv[1] if len(sys.argv) > 1 else 'fonttools'
d = json.load(urllib.request.urlopen('https://pypi.org/pypi/%s/json' % name, timeout=30))
v = d['info']['version']
print('latest', v)
for f in d['releases'][v]:
    if f['filename'].endswith('.whl'):
        print(f['url'])
