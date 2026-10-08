# -*- coding: utf-8 -*-
"""从 guoqing_kuaile_path.json 生成新时达示教器可直接粘贴的程序代码。
每个笔画(segment)起点用 PTP(快速移动/提笔)，笔画内用 LIN(直线插补写字)。
姿态统一 A=0, B=90, C=180。
"""
import json, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
with open(os.path.join(root, "reports", "guoqing_kuaile_path.json"), encoding="utf-8") as f:
    data = json.load(f)

A, B, C = 0.0, 90.0, 180.0

var_lines = []
prog_lines = []
idx = 0
for seg in data["segments"]:
    pts = seg["points"]
    for j, p in enumerate(pts):
        x, y, z = p[0], p[1], p[2]
        cp = f"cp{idx}"
        var_lines.append(f"CARTPOS {cp} := {{{x:.1f} , {y:.1f} , {z:.1f} , {A:.1f} , {B:.1f} , {C:.1f} , 0 , 0 , 0 , 0 , 0 , 0 , 0}}")
        if j == 0:
            # 笔画起点：PTP 快速移动（提笔）
            prog_lines.append(f"PTP({cp}, NULL, NULL, Default, WORLD);")
        else:
            prog_lines.append(f"LIN({cp}, NULL, NULL, Default, WORLD);")
        idx += 1

out_dir = os.path.join(root, "reports")
srd_path = os.path.join(out_dir, "guoqing_program.srd")
srp_path = os.path.join(out_dir, "guoqing_program.srp")

with open(srd_path, "w", encoding="utf-8") as f:
    f.write("\n".join(var_lines) + "\n")

with open(srp_path, "w", encoding="utf-8") as f:
    # 手册: Lin(pos) 即可，其他参数全选填；PTP/LIN 都只需位置变量
    lines = [f"LIN({cp});" for cp in [f"cp{i}" for i in range(idx)]]
    f.write("\n".join(lines) + "\n")

print(f"points: {idx}, segments: {len(data['segments'])}")
print(f"srd -> {srd_path}")
print(f"srp -> {srp_path}")
