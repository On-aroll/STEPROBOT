# -*- coding: utf-8 -*-
"""按 STEP 拓扑把圆柱面轴线归到零件（bbox 法的修正版，v2）。

链路:
  CYLINDRICAL_SURFACE --ADVANCED_FACE--> FACE --CLOSED_SHELL--> MANIFOLD_SOLID_BREP
    --ADVANCED_BREP_SHAPE_REPRESENTATION--> (经 SHAPE_REPRESENTATION_RELATIONSHIP 等价)
    <--SHAPE_DEFINITION_REPRESENTATION-- PRODUCT_DEFINITION_SHAPE --> PRODUCT_DEFINITION
    --FORMATION--> PRODUCT 名

用法:  python tools/axes_by_topology.py <step> [--out DIR]
输出:  <out>/axes_by_topology.json
"""
import json
import os
import re
import sys
import math
from collections import Counter, defaultdict

ENT = re.compile(r'^#(\d+)\s*=\s*([A-Z0-9_]+)\s*\((.*)\)\s*;?\s*$')
NUM = re.compile(r'-?\d+(?:\.\d+)?(?:[Ee][-+]?\d+)?')
REF = re.compile(r'#(\d+)')
FACE_LINE = re.compile(
    r"^#(\d+)\s*=\s*ADVANCED_FACE\s*\(\s*'[^']*'\s*,\s*\(([^)]*)\)\s*,\s*#(\d+)\s*,")


def parse(path):
    ents = {}
    with open(path, encoding='gbk', errors='replace') as f:
        for line in f:
            if not line.startswith('#'):
                continue
            m = ENT.match(line.rstrip('\n'))
            if m:
                ents[int(m.group(1))] = (m.group(2), m.group(3))
    return ents


def refs(s):
    return [int(x) for x in REF.findall(s)]


def nums(s):
    return [float(x) for x in NUM.findall(s)]


def canon(d):
    L = math.sqrt(sum(c * c for c in d)) or 1.0
    n = [c / L for c in d]
    for c in n:
        if abs(c) > 1e-6:
            return tuple(round(x * (1 if c > 0 else -1), 3) for x in n)
    return tuple(round(x, 3) for x in n)


def main():
    step = sys.argv[1]
    outdir = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else '.'
    NAME_PREFIX = "SR175-3200本体演示模型"

    ents = parse(step)
    print(f"parsed {len(ents)} entities")

    # ---- face -> surface ----
    face_to_surface = {}
    with open(step, encoding='gbk', errors='replace') as f:
        for line in f:
            if 'ADVANCED_FACE' not in line:
                continue
            m = FACE_LINE.match(line.rstrip('\n'))
            if m:
                face_to_surface[int(m.group(1))] = int(m.group(3))
    print(f"  faces: {len(face_to_surface)}")

    # ---- shell / brep ----
    shell_faces, brep_shell = {}, {}
    for eid, (t, a) in ents.items():
        if t in ('CLOSED_SHELL', 'OPEN_SHELL'):
            shell_faces[eid] = refs(a)
        elif t in ('MANIFOLD_SOLID_BREP', 'BREP_WITH_VOIDS'):
            r = refs(a)
            if r:
                brep_shell[eid] = r[0]
    face_to_brep = {}
    for bid, sid in brep_shell.items():
        for fid in shell_faces.get(sid, []):
            face_to_brep[fid] = bid
    print(f"  shells={len(shell_faces)} breps={len(brep_shell)} face->brep={len(face_to_brep)}")

    # ---- product 链 ----
    pname, form_of, def_form = {}, {}, {}
    for eid, (t, a) in ents.items():
        if t == 'PRODUCT':
            m = re.match(r"\s*'([^']*)'", a)
            if m:
                pname[eid] = m.group(1)
        elif t.startswith('PRODUCT_DEFINITION_FORMATION'):
            r = refs(a)
            if r:
                form_of[eid] = r[0]
        elif t == 'PRODUCT_DEFINITION':
            r = refs(a)
            if r:
                def_form[eid] = r[0]
    def_product = {d: form_of.get(f, -1) for d, f in def_form.items()}

    # ---- PDS / SDR ----
    pds_char = {}
    for eid, (t, a) in ents.items():
        if t == 'PRODUCT_DEFINITION_SHAPE':
            r = refs(a)
            if r:
                pds_char[eid] = r[0]
    sr_to_pdef = defaultdict(set)
    for eid, (t, a) in ents.items():
        if t == 'SHAPE_DEFINITION_REPRESENTATION':
            r = refs(a)
            if len(r) >= 2 and r[0] in pds_char:
                sr_to_pdef[r[1]].add(pds_char[r[0]])

    # ---- rep 等价（SHAPE_REPRESENTATION_RELATIONSHIP）----
    equiv = defaultdict(set)
    for eid, (t, a) in ents.items():
        if t == 'SHAPE_REPRESENTATION_RELATIONSHIP':
            r = refs(a)
            if len(r) >= 2:
                equiv[r[0]].add(r[1])
                equiv[r[1]].add(r[0])

    def resolve_rep(sr, seen=None):
        """沿等价关系找出带 brep 的 shape_rep。"""
        seen = seen or set()
        if sr in seen:
            return []
        seen.add(sr)
        found = list(sr_breps_direct.get(sr, []))
        for e in equiv.get(sr, ()):
            found += resolve_rep(e, seen)
        return found

    # ---- shape_rep -> breps（直接 item）----
    sr_breps_direct = defaultdict(list)
    SR_TYPES = {'SHAPE_REPRESENTATION', 'ADVANCED_BREP_SHAPE_REPRESENTATION',
                'MANIFOLD_SURFACE_SHAPE_REPRESENTATION'}
    item_re = re.compile(r"\s*'[^']*'\s*,\s*\((.*?)\)\s*,\s*#")
    for eid, (t, a) in ents.items():
        if t in SR_TYPES:
            m = item_re.match(a)
            if m:
                for it in refs(m.group(1)):
                    if it in brep_shell:
                        sr_breps_direct[eid].append(it)

    # ---- brep -> part name ----
    brep_part = {}
    for sr, bs in sr_breps_direct.items():
        pdefs = set(sr_to_pdef.get(sr, set()))
        for e in equiv.get(sr, ()):  # 等价 rep 也可能被 SDR 指到
            pdefs |= sr_to_pdef.get(e, set())
        if not pdefs:
            continue
        names = sorted({pname.get(def_product.get(p, -1), '?') for p in pdefs})
        for b in bs:
            brep_part.setdefault(b, names[0] if names else '?')
    print(f"  brep->part: {len(brep_part)} / {len(brep_shell)}")

    # ---- cylinder -> part ----
    by_part = defaultdict(list)
    n_assigned = 0
    surf_to_brep = defaultdict(list)
    for fid, surf in face_to_surface.items():
        if fid in face_to_brep:
            surf_to_brep[surf].append(face_to_brep[fid])

    for eid, (t, a) in ents.items():
        if t != 'CYLINDRICAL_SURFACE':
            continue
        bs = surf_to_brep.get(eid)
        if not bs:
            continue
        part = None
        for b in bs:
            p = brep_part.get(b)
            if p:
                part = p
                break
        if not part:
            continue
        n_assigned += 1
        radius = nums(a)[-1] if nums(a) else None
        rs = refs(a)
        loc = dr = None
        if rs:
            ap = ents.get(rs[0])
            if ap and ap[0] == 'AXIS2_PLACEMENT_3D':
                ar = refs(ap[1])
                if len(ar) >= 2:
                    lp, dp = ents.get(ar[0]), ents.get(ar[1])
                    if lp and dp:
                        loc = nums(lp[1])[:3]
                        dr = nums(dp[1])[:3]
        short = part.replace(NAME_PREFIX, '').lstrip('-') or part
        by_part[short].append({"id": eid, "r": radius,
                               "loc": [round(v, 2) for v in (loc or [])],
                               "dir": canon(dr) if dr else None})

    total_cyl = sum(1 for e in ents.values() if e[0] == 'CYLINDRICAL_SURFACE')
    print(f"  cylinders assigned: {n_assigned} / {total_cyl}")

    summary = {}
    for pname_, items in sorted(by_part.items()):
        cnt = Counter(c["dir"] for c in items if c["dir"])
        w = Counter()
        for c in items:
            if c["dir"]:
                w[c["dir"]] += max(1.0, c["r"] or 1.0)
        dom, score = w.most_common(1)[0] if w else ((None, 0),)
        maxr = max((c["r"] or 0 for c in items if c["dir"] == dom), default=0)
        summary[pname_] = {
            "n_cyl": len(items),
            "distinct_dirs": len(cnt),
            "top_dirs": [[list(d), n] for d, n in cnt.most_common(4)],
            "weighted_top": [[list(d), round(v, 1)] for d, v in w.most_common(4)],
            "dominant_dir": list(dom) if dom else None,
            "dominant_max_radius": round(maxr, 2),
            "weight_score": round(score, 1),
        }

    os.makedirs(outdir, exist_ok=True)
    fp = os.path.join(outdir, "axes_by_topology.json")
    with open(fp, 'w', encoding='utf-8') as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)
    # 大轴承直线明细（r>=150），用于做运动学链匹配
    big = {p: [c for c in items if (c["r"] or 0) >= 150]
           for p, items in by_part.items()}
    big = {p: v for p, v in big.items() if v}
    fp2 = os.path.join(outdir, "axes_lines.json")
    with open(fp2, 'w', encoding='utf-8') as f:
        json.dump(big, f, ensure_ascii=False, indent=2)
    print(f"\nwrote {fp}")
    print(f"wrote {fp2}")
    for k in sorted(summary):
        s = summary[k]
        print(f"  {k:10s} n={s['n_cyl']:4d} dirs={s['distinct_dirs']:3d} "
              f"dominant={s['dominant_dir']} max_r={s['dominant_max_radius']}")


if __name__ == "__main__":
    main()