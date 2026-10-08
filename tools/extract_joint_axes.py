# -*- coding: utf-8 -*-
"""从 STEP(AP214) 明文里提取圆柱面轴线，用于反推机器人关节轴线。

原理：关节的旋转轴 = 关节处轴承配合圆柱面的轴线。
STEP 中 CYLINDRICAL_SURFACE(id,'NONE',#axis2placement,radius)，
AXIS2_PLACEMENT_3D(id,'NONE',#location,#axis_dir,#ref_dir)，
再解到 CARTESIAN_POINT / DIRECTION。

用法：
  python extract_joint_axes.py <step> [--top N] [--radius-min R]
输出：按方向聚类的轴线统计 + 大半径（主轴承）轴线明细，JSON 落盘。
"""
import sys, re, json, math, collections, os

ENT_RE = re.compile(r'^#(\d+)\s*=\s*([A-Z0-9_]+)\s*\((.*)\)\s*;?\s*$')
NUM_RE = re.compile(r'-?\d+(?:\.\d+)?(?:[Ee][-+]?\d+)?')

def parse(path):
    ents = {}
    n = 0
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            n += 1
            if not line.startswith('#'):
                continue
            m = ENT_RE.match(line.rstrip('\n'))
            if m:
                ents[int(m.group(1))] = (m.group(2), m.group(3))
    return ents, n

def nums(s):
    return [float(x) for x in NUM_RE.findall(s)]

def get_point(ents, i):
    e = ents.get(i)
    if not e:
        return None
    v = nums(e[1])
    return v[:3] if len(v) >= 3 else None

def get_dir(ents, i):
    return get_point(ents, i)

def refs(s):
    return [int(x) for x in re.findall(r'#(\d+)', s)]

def main():
    path = sys.argv[1]
    top = 40
    rmin = 0.0
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
    if '--radius-min' in sys.argv:
        rmin = float(sys.argv[sys.argv.index('--radius-min') + 1])

    ents, nlines = parse(path)
    print("解析行数:", nlines, "实体数:", len(ents))

    cy = [(i, e) for i, e in ents.items() if e[0] == 'CYLINDRICAL_SURFACE']
    print("圆柱面:", len(cy))

    axes = []
    for i, (_, args) in cy:
        rs = refs(args)
        if len(rs) < 1:
            continue
        vals = nums(args)
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
        L = math.sqrt(sum(c * c for c in dr)) or 1.0
        d = tuple(round(c / L, 4) for c in dr)
        axes.append({"id": i, "r": radius, "loc": [round(c, 2) for c in loc], "dir": d})

    print("可解轴线的圆柱面:", len(axes))

    # 方向聚类（把反向归一到同一类）
    def canon(d):
        for c in d:
            if abs(c) > 1e-6:
                return tuple(round(x * (1 if c > 0 else -1), 3) for x in d)
        return d

    cnt = collections.Counter(canon(a["dir"]) for a in axes)
    print("\n=== 轴线方向分布（前 12）===")
    for d, c in cnt.most_common(12):
        print("  dir=%-28s 圆柱面数=%d" % (str(d), c))

    # 大半径圆柱面 = 关节主轴承孔
    big = sorted([a for a in axes if (a["r"] or 0) >= rmin], key=lambda a: -(a["r"] or 0))[:top]
    print("\n=== 半径最大的 %d 个圆柱面（疑似关节轴承孔）===" % top)
    for a in big:
        print("  r=%-8s dir=%-24s loc=%s" % (a["r"], str(a["dir"]), a["loc"]))

    out = os.path.join(os.path.dirname(os.path.abspath(path)), "joint_axes_probe.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"total_cyl": len(axes), "dir_hist": cnt.most_common(30), "top_by_radius": big}, f,
                  ensure_ascii=False, indent=2)
    print("\n已写出:", out)

if __name__ == "__main__":
    main()
