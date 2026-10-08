# -*- coding: utf-8 -*-
"""把 STEP 里的圆柱面轴线按零件归属分组，筛出 6 条关节轴。

用法:
  python tools/assign_axes_to_parts.py <step> <mesh_mapping.json> [--output DIR]

输入 mesh_mapping.json 是 _extract/mesh/mapping.json 的格式:
  [{"short": "J0", "bbox": [xmin,ymin,zmin,xmax,ymax,zmax]}, ...]

输出:
  - parts_axes.json: 每个零件的圆柱面统计 + 主轴方向
  - joints_axes.json: 仅 J0..J6 的轴线方向与代表点
"""
import json
import re
import sys
import math
import os

ENTITY_RE = re.compile(r'^#(\d+)\s*=\s*([A-Z0-9_]+)\s*\((.*)\)\s*;?\s*$')
NUM_RE = re.compile(r'-?\d+(?:\.\d+)?(?:[Ee][-+]?\d+)?')
REF_RE = re.compile(r'#(\d+)')


def parse_step(path):
    ents = {}
    n = 0
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            n += 1
            if not line.startswith('#'):
                continue
            m = ENTITY_RE.match(line.rstrip('\n'))
            if m:
                ents[int(m.group(1))] = (m.group(2), m.group(3))
    return ents, n


def get_point(ents, i):
    e = ents.get(i)
    if not e or e[0] != 'CARTESIAN_POINT':
        return None
    v = [float(x) for x in NUM_RE.findall(e[1])]
    return v[:3] if len(v) >= 3 else None


def get_dir(ents, i):
    e = ents.get(i)
    if not e or e[0] != 'DIRECTION':
        return None
    v = [float(x) for x in NUM_RE.findall(e[1])]
    return v[:3] if len(v) >= 3 else None


def refs(s):
    return [int(x) for x in REF_RE.findall(s)]


def canon_dir(d):
    """归一化方向向量，反向归一。"""
    L = math.sqrt(sum(c * c for c in d)) or 1.0
    n = [c / L for c in d]
    for c in n:
        if abs(c) > 1e-6:
            return tuple(round(x * (1 if c > 0 else -1), 3) for x in n)
    return tuple(round(x, 3) for x in n)


def point_in_bbox(p, b):
    return (b[0] - 1 <= p[0] <= b[3] + 1 and
            b[1] - 1 <= p[1] <= b[4] + 1 and
            b[2] - 1 <= p[2] <= b[5] + 1)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    step = sys.argv[1]
    map_path = sys.argv[2]
    out_dir = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else '.'
    if '--output' in sys.argv:
        out_dir = sys.argv[sys.argv.index('--output') + 1]

    print(f"parsing step: {step}")
    ents, n = parse_step(step)
    print(f"  parsed {n} lines, {len(ents)} entities")

    parts = json.load(open(map_path, encoding='utf-8'))
    print(f"  loaded {len(parts)} parts from mapping")

    # 解析所有 CYLINDRICAL_SURFACE
    cyls = []
    cy_type = 'CYLINDRICAL_SURFACE'
    for eid, (etype, args) in ents.items():
        if etype != cy_type:
            continue
        rs = refs(args)
        if not rs:
            continue
        vals = [float(x) for x in NUM_RE.findall(args)]
        radius = vals[-1] if vals else None
        ap = ents.get(rs[0])
        if not ap or ap[0] != 'AXIS2_PLACEMENT_3D':
            continue
        ar = refs(ap[1])
        if len(ar) < 2:
            continue
        loc = get_point(ents, ar[0])
        dr = get_dir(ents, ar[1])
        if loc is None or dr is None:
            continue
        cyls.append({"id": eid, "r": radius, "loc": loc, "dir": canon_dir(dr)})
    print(f"  resolved {len(cyls)} cylinders")

    # 给每个圆柱面分配零件：找包含其轴点且体积最小的零件
    def vol(b):
        return max(0.0, (b[3] - b[0]) * (b[4] - b[1]) * (b[5] - b[2]))

    for c in cyls:
        c["part"] = None
        c["part_volume"] = None
        candidates = []
        for p in parts:
            if point_in_bbox(c["loc"], p["bbox"]):
                candidates.append((vol(p["bbox"]), p["short"]))
        if candidates:
            candidates.sort()
            c["part"] = candidates[0][1]
            c["part_volume"] = candidates[0][0]
        else:
            # 最近的 bbox（中心距）
            best = None; bestd = 1e30
            for p in parts:
                cx = (p["bbox"][0] + p["bbox"][3]) / 2
                cy = (p["bbox"][1] + p["bbox"][4]) / 2
                cz = (p["bbox"][2] + p["bbox"][5]) / 2
                d = (cx - c["loc"][0]) ** 2 + (cy - c["loc"][1]) ** 2 + (cz - c["loc"][2]) ** 2
                if d < bestd:
                    bestd = d; best = p["short"]
            c["part"] = best + " (nearest)"
            c["part_volume"] = None

    # 统计
    by_part = {}
    for c in cyls:
        by_part.setdefault(c["part"], []).append(c)

    parts_axes = {}
    for pname, items in by_part.items():
        cnt = collections.Counter(c["dir"] for c in items)
        # 加权：larger radius counts more
        weighted = collections.Counter()
        for c in items:
            weighted[c["dir"]] += max(1, c["r"] or 1)
        parts_axes[pname] = {
            "n_cyl": len(items),
            "n_distinct_dirs": len(cnt),
            "raw_top_dirs": cnt.most_common(5),
            "weighted_top_dirs": weighted.most_common(5),
        }
        # dominant axis
        dom = weighted.most_common(1)[0][0]
        # 该方向的最大半径
        maxr = max((c["r"] or 0 for c in items if c["dir"] == dom), default=0)
        parts_axes[pname]["dominant_dir"] = list(dom)
        parts_axes[pname]["dominant_max_radius"] = maxr

    # 仅 J0..J6
    joints_axes = {}
    for p in parts:
        if re.match(r'^J\d', p["short"]):
            joints_axes[p["short"]] = parts_axes.get(p["short"], {
                "n_cyl": 0, "dominant_dir": None, "dominant_max_radius": None
            })

    os.makedirs(out_dir, exist_ok=True)
    pa_path = os.path.join(out_dir, "parts_axes.json")
    ja_path = os.path.join(out_dir, "joints_axes.json")
    with open(pa_path, "w", encoding="utf-8") as f:
        json.dump(parts_axes, f, ensure_ascii=False, indent=2)
    with open(ja_path, "w", encoding="utf-8") as f:
        json.dump(joints_axes, f, ensure_ascii=False, indent=2)

    print(f"\n=== J0..J6 主轴 ===")
    for jn in sorted(joints_axes):
        info = joints_axes[jn]
        d = info.get("dominant_dir")
        r = info.get("dominant_max_radius")
        n = info.get("n_cyl", 0)
        print(f"  {jn}: n_cyl={n:4d}  dominant_dir={d}  max_r={r:.1f}")
    print(f"\n写出: {pa_path}")
    print(f"写出: {ja_path}")


if __name__ == "__main__":
    import collections
    main()