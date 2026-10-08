# -*- coding: utf-8 -*-
"""把"国庆快乐"字形轮廓导出为 DXF（R12，LINE 实体），供 STEPStudio 路径功能导入。

坐标：DXF X = 字宽方向（左->右），DXF Y = 字高方向（下->上），Z=0。
用法: python tools/glyph_to_dxf.py [输出.dxf]
"""
import json
import sys

CENTER_Z = 1200.0   # 与 glyph_to_path.py 一致（用于还原字形坐标）


def main():
    src = r'D:/Desktop/STEPROBOT/reports/guoqing_kuaile_path.json'
    dst = sys.argv[1] if len(sys.argv) > 1 else \
        r'D:/Desktop/STEPROBOT/reports/guoqing_kuaile.dxf'
    d = json.load(open(src, encoding='utf-8'))

    out = []
    out += ['0', 'SECTION', '2', 'HEADER', '9', '$ACADVER', '1', 'AC1009',
            '9', '$INSBASE', '10', '0.0', '20', '0.0', '30', '0.0',
            '0', 'ENDSEC']
    out += ['0', 'SECTION', '2', 'TABLES',
            '0', 'TABLE', '2', 'LAYER', '70', '1',
            '0', 'LAYER', '2', 'GUOQING', '70', '0', '62', '2', '6', 'CONTINUOUS',
            '0', 'ENDTAB', '0', 'ENDSEC']
    out += ['0', 'SECTION', '2', 'ENTITIES']

    n = 0
    for seg in d['segments']:
        pts = seg['points']
        # 机器人坐标 -> 字形坐标（X=字宽左->右, Y=字高下->上）
        poly = [(-p[1], p[2] - CENTER_Z) for p in pts]
        for (x1, y1), (x2, y2) in zip(poly, poly[1:]):
            out += ['0', 'LINE', '8', 'GUOQING',
                    '10', '%.3f' % x1, '20', '%.3f' % y1, '30', '0.0',
                    '11', '%.3f' % x2, '21', '%.3f' % y2, '31', '0.0']
            n += 1

    out += ['0', 'ENDSEC', '0', 'EOF']
    with open(dst, 'w', encoding='ascii') as f:
        f.write('\n'.join(out) + '\n')
    print('DXF:', dst)
    print('LINE 实体:', n, ' (来自 %d 段轮廓)' % len(d['segments']))


if __name__ == '__main__':
    main()