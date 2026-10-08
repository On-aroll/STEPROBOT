# -*- coding: utf-8 -*-
"""生成镜像版"国庆快乐"程序，Y取反。"""
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
        x, y, z = p[0], -p[1], p[2]  # Y 取反镜像
        cp = f"cp{idx}"
        var_lines.append(f"CARTPOS {cp} := {{{x:.1f} , {y:.1f} , {z:.1f} , {A:.1f} , {B:.1f} , {C:.1f} , 0 , 0 , 0 , 0 , 0 , 0 , 0}}")
        prog_lines.append(f"Lin({cp},NULL,NULL,NULL,Default,WORLD);")
        idx += 1

active_dir = r"D:\Desktop\STEPROBOT\STEPStudio 4.7.0\bin\test\工作站\SRSystemFile\UserPrograms\project1.sr"

# 写 .srd (LF 换行)
srd_text = "\n".join(var_lines) + "\n"
with open(os.path.join(active_dir, "program0.srd"), "w", newline="\n", encoding="utf-8") as f:
    f.write(srd_text)

# 写 .srp (LF 换行)
srp_text = "\n".join(prog_lines) + "\n"
with open(os.path.join(active_dir, "program0.srp"), "w", newline="\n", encoding="utf-8") as f:
    f.write(srp_text)

print(f"points: {idx}, segments: {len(data['segments'])}")
print(f"written to: {active_dir}")
