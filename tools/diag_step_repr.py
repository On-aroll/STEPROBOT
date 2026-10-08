# -*- coding: utf-8 -*-
"""诊断 STEP 里的表示类型，打通 pdef -> shape_rep -> brep 链。"""
import re
import sys
import collections

ENT = re.compile(r'^#(\d+)\s*=\s*([A-Z0-9_]+)\s*\((.*)\)\s*;?\s*$')
REF = re.compile(r'#(\d+)')

step = r'D:/Desktop/STEPROBOT/_extract/cad-original/SR175-3200 演示模型/SR175-3200本体演示模型.STEP'

c = collections.Counter()
sample = {}
ents = {}
with open(step, encoding='utf-8', errors='replace') as f:
    for line in f:
        if not line.startswith('#'):
            continue
        m = ENT.match(line.rstrip('\n'))
        if not m:
            continue
        eid, t, a = int(m.group(1)), m.group(2), m.group(3)
        ents[eid] = (t, a)
        if any(k in t for k in ('REPRESENTATION', 'PRODUCT_DEFINITION', 'SHAPE',
                                'NEXT_ASSEMBLY', 'BREP', 'SHELL')):
            c[t] += 1
            if t not in sample:
                sample[t] = line.strip()[:200]

for t, n in c.most_common(30):
    print(f"{n:6d}  {t}")
    if n <= 20:
        print("        ", sample[t])

# 看 brep 被谁引用
brep_ids = [e for e, (t, a) in ents.items() if t in ('MANIFOLD_SOLID_BREP', 'BREP_WITH_VOIDS')]
print(f"\nbrep ids (前5): {brep_ids[:5]}")
if brep_ids:
    b0 = brep_ids[0]
    tag = f"#{b0}"
    print(f"引用 {tag} 的实体：")
    n = 0
    for eid, (t, a) in ents.items():
        if tag in a:
            n += 1
            if n <= 6:
                print(f"   #{eid} = {t} ( {a[:140]} )")
    print(f"   共 {n} 个引用")
