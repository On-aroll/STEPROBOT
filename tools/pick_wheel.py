# -*- coding: utf-8 -*-
"""从 PyPI JSON 里挑出最新版本的 py3-none-any wheel 地址。"""
import json
d = json.load(open(r'D:/Desktop/STEPROBOT/.workbuddy-ai/tmp/ft.json', encoding='utf-8'))
v = d['info']['version']
print('latest', v)
for f in d['releases'][v]:
    if f['filename'].endswith('.whl') and 'py3-none-any' in f['filename']:
        print(f['url'])
