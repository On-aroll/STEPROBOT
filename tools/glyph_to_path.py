# -*- coding: utf-8 -*-
"""从 Windows 字体提取"国庆快乐"的笔画轮廓，展平成折线，映射为机器人笛卡尔目标点。

输出: reports/guoqing_kuaile_path.json
  {"segments": [ {"pen": bool, "points": [[X,Y,Z],...]} ], "n_points": N}
坐标系：X 固定(写字平面到机器人的距离)，Y=字宽方向，Z=字高方向。
"""
import json
import os
import sys

from fontTools.ttLib import TTCollection
from fontTools.pens.recordingPen import RecordingPen

TEXT = "国庆快乐"
FONT = r'C:/Windows/Fonts/msyh.ttc'

# --- 版面参数（毫米）---
CHAR_SIZE = 260.0     # 单字高度
GAP = 60.0            # 字间距
PLANE_X = 1500.0      # 写字平面：到机器人基座的水平距离
CENTER_Z = 1200.0     # 文字中心高度

# 采样精度：贝塞尔每个象限最多细分多少段
CURVE_TOL = 2.0       # mm，弦高误差


def flatten_quad(p0, p1, p2, tol=CURVE_TOL):
    """把二次贝塞尔 p0-p1-p2 展平成点列（不含起点，含终点）。"""
    # 用控制多边形长度与弦长之比估计细分段数
    chord = ((p2[0]-p0[0])**2 + (p2[1]-p0[1])**2) ** 0.5
    poly = ((p1[0]-p0[0])**2 + (p1[1]-p0[1])**2) ** 0.5 + \
           ((p2[0]-p1[0])**2 + (p2[1]-p1[1])**2) ** 0.5
    n = 2
    if chord > 0:
        n = int(max(2, min(24, poly / max(chord, 1e-6) * 4, chord / tol)))
    pts = []
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        x = mt*mt*p0[0] + 2*mt*t*p1[0] + t*t*p2[0]
        y = mt*mt*p0[1] + 2*mt*t*p1[1] + t*t*p2[1]
        pts.append((x, y))
    return pts


def glyph_contours(gs, gname):
    """返回该字形的轮廓列表，每条轮廓是 [(x,y),...]（字体单位，y 向上）。"""
    pen = RecordingPen()
    gs[gname].draw(pen)
    contours, cur, cur_pt = [], [], (0.0, 0.0)
    for op, args in pen.value:
        if op == 'moveTo':
            if len(cur) > 1:
                contours.append(cur)
            cur = [args[0]]
            cur_pt = args[0]
        elif op == 'lineTo':
            cur.append(args[0]); cur_pt = args[0]
        elif op == 'qCurveTo':
            # TrueType 二次贝塞尔：可能一串 off-curve + 一个 on-curve
            pts = list(args)
            if len(pts) == 1:            # 全 off-curve：隐式闭合到起点
                pts = pts + [cur[0] if contours or cur else cur_pt]
            p0 = cur_pt
            for k in range(len(pts) - 1):
                c = pts[k]
                nxt = pts[k+1]
                cur.extend(flatten_quad(p0, c, nxt))
                p0 = nxt
            cur.append(pts[-1]); cur_pt = pts[-1]
        elif op == 'curveTo':            # CFF 三次贝塞尔
            p0 = cur_pt
            c1, c2, p2 = args
            # 三次转二次近似：细分直接用三次公式
            n = 8
            for i in range(1, n+1):
                t = i/n; mt = 1-t
                x = mt**3*p0[0] + 3*mt*mt*t*c1[0] + 3*mt*t*t*c2[0] + t**3*p2[0]
                y = mt**3*p0[1] + 3*mt*mt*t*c1[1] + 3*mt*t*t*c2[1] + t**3*p2[1]
                cur.append((x, y))
            cur_pt = p2
        elif op == 'closePath':
            if len(cur) > 1:
                contours.append(cur)
            cur, cur_pt = [], cur_pt
    if len(cur) > 1:
        contours.append(cur)
    return contours


def main():
    tc = TTCollection(FONT)
    font = tc.fonts[0]
    cmap = font.getBestCmap()
    gs = font.getGlyphSet()
    upm = font['head'].unitsPerEm
    scale = CHAR_SIZE / upm

    segments = []
    pen_x = 0.0
    for ch in TEXT:
        gname = cmap.get(ord(ch))
        if gname is None:
            pen_x += CHAR_SIZE + GAP
            continue
        g = gs[gname]
        adv = g.width * scale
        for contour in glyph_contours(gs, gname):
            pts = [((pen_x + x*scale) - (len(TEXT)*(CHAR_SIZE+GAP)-GAP)/2.0,
                    y*scale + CHAR_SIZE/2.0)
                   for (x, y) in contour]
            # 映射到机器人坐标：字面 x -> 机器人 Y（镜像，使正对字面时从左往右读）
            #                字面 y -> 机器人 Z
            step = max(1, len(pts) // 40)
            kept = pts[::step]
            if kept[-1] != pts[-1]:
                kept.append(pts[-1])
            seg = {"pen": True,
                   "points": [[PLANE_X, round(-g[0], 2), round(g[1] + CENTER_Z, 2)]
                              for g in kept]}
            segments.append(seg)
        pen_x += CHAR_SIZE + GAP

    total = sum(len(s["points"]) for s in segments)
    out = {"text": TEXT, "plane_x": PLANE_X, "center_z": CENTER_Z,
           "char_size": CHAR_SIZE, "n_segments": len(segments),
           "n_points": total, "segments": segments}
    dst = r'D:/Desktop/STEPROBOT/reports/guoqing_kuaile_path.json'
    with open(dst, 'w', encoding='utf-8') as f:
        json.dump(out, f, ensure_ascii=False)
    print("segments:", len(segments), " points:", total)
    print("wrote", dst)

    # ---- 生成 C++ 头文件（供 write_guonian.exe 使用）----
    hdr = r'D:/Desktop/STEPROBOT/research/cpp-starter/src/write_path.h'
    lines = ['// 自动生成：tools/glyph_to_path.py', '#pragma once', '#include <vector>', '',
             'struct WPt { double x, y, z; bool pen; };', '',
             'static const WPt WRITE_PATH[] = {']
    for si, seg in enumerate(segments):
        for pi, p in enumerate(seg["points"]):
            pen = 'true' if (pi > 0) else ('true' if si > 0 else 'false')
            lines.append('  {%.1f, %.1f, %.1f, %s},' % (p[0], p[1], p[2], pen))
    lines.append('};')
    lines.append('static const int WRITE_PATH_N = %d;' % total)
    lines.append('static const double WRITE_ORI[3] = {0.0, 90.0, 180.0};')
    open(hdr, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('wrote', hdr)


if __name__ == '__main__':
    main()