# -*- coding: utf-8 -*-
"""下载并安装 pywinauto（含 comtypes/six 依赖），全部走 curl 续传。"""
import json
import os
import subprocess
import sys

TMP = r'D:/Desktop/STEPROBOT/.workbuddy-ai/tmp'
VENV_PY = r'C:/Users/BonVoyage/.workbuddy-ai/binaries/python/envs/pdf/Scripts/python.exe'
PKGS = ['pywinauto']


def pypi_json(name):
    dst = os.path.join(TMP, name + '.json')
    url = 'https://pypi.org/pypi/%s/json' % name
    for i in range(12):
        subprocess.run(['curl', '-sL', '-C', '-', '--max-time', '60',
                        '-o', dst, url], check=False)
        try:
            d = json.load(open(dst, encoding='utf-8'))
            return d
        except Exception:
            continue
    raise RuntimeError('json download failed: ' + name)


def latest_wheels(d, v):
    out = []
    for f in d['releases'][v]:
        n = f['filename']
        if n.endswith('.whl') and ('py3-none-any' in n or 'abi3-none-any' in n
                                   or 'cp313-cp313-win_amd64' in n
                                   or 'cp3-abi3-win_amd64' in n):
            out.append((n, f['url'], f['size']))
    return out


def fetch(url, dst, want):
    for i in range(15):
        if os.path.exists(dst) and os.path.getsize(dst) >= want:
            return
        subprocess.run(['curl', '-sL', '-C', '-', '--max-time', '90',
                        '-o', dst, url], check=False)
    if not os.path.exists(dst) or os.path.getsize(dst) < want * 0.95:
        raise RuntimeError('download failed: ' + url)


def main():
    os.makedirs(TMP, exist_ok=True)
    wheels = []
    for name in PKGS:
        d = pypi_json(name)
        v = d['info']['version']
        print(name, 'latest', v)
        ws = latest_wheels(d, v)
        if not ws:
            raise RuntimeError('no wheel for ' + name)
        for n, u, sz in ws:
            dst = os.path.join(TMP, n)
            print('  fetch', n, sz, 'bytes')
            fetch(u, dst, sz)
            wheels.append(dst)
        # 依赖
        req = d['info'].get('requires_dist') or []
        for r in req:
            base = r.split(';')[0].split('==')[0].split('>=')[0].split('<')[0].strip()
            if base and base.lower() not in ('python',) and 'extra' not in r:
                PKGS.append(base)
    # 安装
    for w in wheels:
        print('pip install', os.path.basename(w))
        subprocess.run([VENV_PY, '-m', 'pip', 'install', '--disable-pip-version-check',
                        '--no-input', '-q', w], check=False)
    r = subprocess.run([VENV_PY, '-c', 'import pywinauto; print("pywinauto OK", pywinauto.__version__)'],
                       capture_output=True, text=True)
    print(r.stdout.strip(), r.stderr.strip()[:300])


if __name__ == '__main__':
    main()