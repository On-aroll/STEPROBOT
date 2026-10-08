#!/usr/bin/env python3
"""STEP 装配体 -> 按零件命名的 STL 网格（Linux 侧工具）

用途：把 SR175-3200 的 SolidWorks 导出的 .STEP 总装，转成每个零件一个 .stl，
并保留零件的原始名称（J0..J6 / cable1 / Cable2 / 3D包络）。

背景与实测数据见 ../_extract/mesh/README.md。

依赖（已装在托管 venv 里）：
    /home/bonvoyage/.workbuddy-ai/binaries/python/envs/cad

    export UV_CACHE_DIR=/home/bonvoyage/.workbuddy-ai/binaries/uv-cache
    uv pip install --python <venv>/bin/python cadquery-ocp

用法：
    <venv>/bin/python tools/step2stl.py <输入.step> <输出目录>

实现要点（都是踩过的坑）：
  1. 用 STEPCAFControl_Reader 而不是 STEPControl_Reader，才能拿到零件名。
  2. 零件名不在装配实例标签上，要先 GetReferredShape 解引用到原始标签。
  3. OCC 把 GBK 字节按单字节读进来，名字会乱码。正确解法是直接按 GBK 读
     STEP 原文，自己解析 PRODUCT / PRODUCT_DEFINITION_FORMATION /
     PRODUCT_DEFINITION / NEXT_ASSEMBLY_USAGE_OCCURRENCE 四级关系。
     注意 FORMATION 的实体名是 PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE。
  4. 必须对 XCAF 里的形体逐个调用 BRepMesh_IncrementalMesh；对
     STEPControl_Reader 读出的另一个对象做网格化是没用的，三角化不会共享。
  5. Bnd_Box.Get() 返回未注册的 struct，要用 CornerMin()/CornerMax()。
  6. 不要用 gmsh 做这件事：它的 2D 网格器在这份 CAD 上会报
     "The 1D mesh seems not to be forming a closed loop"，且内存占用远高于 OCC。
"""

import os
import re
import sys
import json
import time
import struct

from OCP.BRep import BRep_Tool
from OCP.BRepBndLib import BRepBndLib
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.Bnd import Bnd_Box
from OCP.STEPCAFControl import STEPCAFControl_Reader
from OCP.TCollection import TCollection_ExtendedString
from OCP.TDF import TDF_ChildIterator
from OCP.TDocStd import TDocStd_Document
from OCP.TopAbs import TopAbs_FACE
from OCP.TopExp import TopExp_Explorer
from OCP.TopLoc import TopLoc_Location
from OCP.TopoDS import TopoDS
from OCP.XCAFApp import XCAFApp_Application
from OCP.XCAFDoc import XCAFDoc_DocumentTool
from OCP.collections import Sequence_TDF_Label

# 从 STEP 里剥掉的产品名前缀，让输出文件名短一些
NAME_PREFIX = "SR175-3200本体演示模型"


def parse_names(step_path):
    """从 STEP 原文解析 NAUO1..NAUOn -> 零件名。STEP 里的中文是 GBK。"""
    txt = open(step_path, "rb").read().decode("gbk", errors="replace")
    prod = {int(m.group(1)): m.group(2)
            for m in re.finditer(r"#(\d+) = PRODUCT \( '([^']*)'", txt)}
    formation = {int(m.group(1)): int(m.group(2)) for m in re.finditer(
        r"#(\d+) = PRODUCT_DEFINITION_FORMATION\w* \( '[^']*',\s*'[^']*',\s*#(\d+),", txt)}
    definition = {int(m.group(1)): int(m.group(2)) for m in re.finditer(
        r"#(\d+) = PRODUCT_DEFINITION \( '[^']*',\s*'[^']*',\s*#(\d+),", txt)}
    nauo = [(m.group(1), int(m.group(2))) for m in re.finditer(
        r"NEXT_ASSEMBLY_USAGE_OCCURRENCE \( '([^']*)',[^)]*?#\d+, #(\d+),", txt)]
    return {nm: prod.get(formation.get(definition.get(child, -1), -1), "?")
            for nm, child in nauo}


def triangles_of(shape):
    """把已三角化的形体展开成三角面列表（世界坐标）。"""
    out = []
    ex = TopExp_Explorer(shape, TopAbs_FACE)
    while ex.More():
        face = TopoDS.Face(ex.Current())
        loc = TopLoc_Location()
        tri = BRep_Tool.Triangulation_s(face, loc)
        if tri is not None:
            trsf = loc.Transformation()
            for i in range(1, tri.NbTriangles() + 1):
                t = tri.Triangle(i)
                try:
                    a, b, c = t.Get()
                except Exception:
                    a, b, c = t.Value(1), t.Value(2), t.Value(3)
                out.append([tri.Node(n).Transformed(trsf) for n in (a, b, c)])
        ex.Next()
    return out


def write_binary_stl(tris, path):
    with open(path, "wb") as f:
        f.write(b"\0" * 80)
        f.write(struct.pack("<I", len(tris)))
        for a, b, c in tris:
            u = (b.X() - a.X(), b.Y() - a.Y(), b.Z() - a.Z())
            v = (c.X() - a.X(), c.Y() - a.Y(), c.Z() - a.Z())
            nx = u[1] * v[2] - u[2] * v[1]
            ny = u[2] * v[0] - u[0] * v[2]
            nz = u[0] * v[1] - u[1] * v[0]
            n = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
            f.write(struct.pack("<3f", nx / n, ny / n, nz / n))
            for p in (a, b, c):
                f.write(struct.pack("<3f", p.X(), p.Y(), p.Z()))
            f.write(struct.pack("<H", 0))


def get_shape(shape_tool, label):
    for fn in ("GetShape_s", "GetShape"):
        try:
            sh = getattr(shape_tool, fn)(label)
            if sh is not None and not sh.IsNull():
                return sh
        except Exception:
            pass
    return None


def convert(step_path, out_dir, deflection=1.5):
    os.makedirs(out_dir, exist_ok=True)
    names = parse_names(step_path)

    app = XCAFApp_Application.GetApplication_s()
    doc = TDocStd_Document(TCollection_ExtendedString("M"))
    app.NewDocument(TCollection_ExtendedString("M"), doc)
    reader = STEPCAFControl_Reader()
    reader.SetNameMode(True)
    reader.ReadFile(step_path)
    reader.Transfer(doc)
    tool = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
    roots = Sequence_TDF_Label()
    tool.GetFreeShapes(roots)

    mapping = []
    it = TDF_ChildIterator(roots.Value(1), False)
    i, t0 = 0, time.time()
    while it.More():
        i += 1
        shape = get_shape(tool, it.Value())
        key = "NAUO%d" % i
        name = names.get(key, key)
        short = name.replace(NAME_PREFIX, "").lstrip("-") or key
        if shape is None:
            print("  %-8s %s  无几何" % (key, name))
            it.Next()
            continue

        BRepMesh_IncrementalMesh(shape, deflection, False, 0.5, True)
        tris = triangles_of(shape)

        bb = Bnd_Box()
        BRepBndLib.Add_s(shape, bb)
        lo, hi = bb.CornerMin(), bb.CornerMax()
        bbox = [round(v, 1) for v in (lo.X(), lo.Y(), lo.Z(), hi.X(), hi.Y(), hi.Z())]

        fname = short + ".stl"
        if tris:
            write_binary_stl(tris, os.path.join(out_dir, fname))
        mapping.append({"nauo": key, "name": name, "short": short,
                        "triangles": len(tris),
                        "file": fname if tris else None, "bbox": bbox})
        print("  %-8s %-10s 三角=%7d  bbox=%s" % (key, short, len(tris), bbox))
        it.Next()

    with open(os.path.join(out_dir, "mapping.json"), "w") as f:
        json.dump(mapping, f, ensure_ascii=False, indent=2)
    print("\n耗时 %.1fs  合计 %d 三角面  %d 个 STL"
          % (time.time() - t0, sum(m["triangles"] for m in mapping),
             len([m for m in mapping if m["file"]])))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
