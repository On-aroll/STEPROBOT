# `_extract/mesh/` —— SR175-3200 三维网格（STEP 转换产物）

**一句话：自建仿真/动画需要的真机三维模型，本项目是有的，而且已经在本机 Linux 上转成了可直接渲染的网格。**

生成时间：2026-10-06 22:26–22:39
生成环境：本机 Ubuntu（Linux），无 Windows、无 SolidWorks、无 FreeCAD

---

## 1. 结论先说

| 问题 | 答案 |
|---|---|
| 真机三维模型有没有？ | **有**。源文件在 `_extract/cad-original/SR175-3200 演示模型/` |
| 能不能用？ | **能**。已转成 14 个按关节命名的 STL，共 **322,043 个三角面**，约 16 MB |
| 需要 Windows 吗？ | **不需要**。全流程在本机 Linux 上跑通，耗时 7.3 秒 |
| 还缺什么？ | 不是几何，是**关节轴线与坐标系对齐**（见 §5） |

---

## 2. 源文件盘点

`_extract/cad-original/SR175-3200 演示模型/`（16 个文件，102 MB）：

| 文件 | 大小 | 可用性 |
|---|---|---|
| `SR175-3200本体演示模型.STEP` | 73 MB | ✅ **主力**。STEP AP214 明文 ASCII，SolidWorks 2012 / SwSTEP 2.0 导出 |
| `SR175-3200本体演示模型.SLDASM` | 6.7 MB | ❌ SolidWorks 原生装配，无解析器 |
| `SR175-3200本体演示模型-J0..J6*.SLDPRT` | 共 11 个 | ❌ SolidWorks 原生零件（OLE2 复合文档），无解析器 |
| `SR175-3200本体演示模型3D包络.SLDPRT` | 0.7 MB | ❌ 同上 |
| `SR175-3200本体演示模型cable1/Cable2.SLDPRT` | 共 3.0 MB | ❌ 同上 |

**关键点：STEP 总装里已经包含了全部零件，并且零件名完整保留。** 所以那 14 个 `.SLDPRT` 打不开并不影响——
STEP 是 SolidWorks 自己导出的，内容等价。

STEP 内部结构（实测统计）：

```
MANIFOLD_SOLID_BREP              29
CLOSED_SHELL                    152
ADVANCED_FACE                10,946
CARTESIAN_POINT             309,198
PRODUCT                          15   ← 1 个总装 + 14 个零件
NEXT_ASSEMBLY_USAGE_OCCURRENCE    14   ← 14 个装配实例
```

---

## 3. 转换结果

### 3.1 零件清单（`mapping.json` 为准）

| NAUO | 零件 | 三角面 | 包围盒 X / Y / Z（mm） |
|---|---|---|---|
| NAUO1 | **J1** | 45,623 | [-153.7, 446.8] [-573.3, 80.0] [-558.0, 204.1] |
| NAUO2 | **J5** | 15,360 | [68.2, 286.5] [1416.0, 1646.2] [-2098.3, -1775.2] |
| NAUO3 | **J4** | 14,808 | [-5.9, 396.1] [1397.8, 1658.8] [-2033.4, -1417.3] |
| NAUO4 | **J6** | 10,522 | [89.5, 291.7] [1426.3, 1628.0] [-2127.0, -2090.8] |
| NAUO5 | **J0** | 91,841 | [-120.9, 502.2] [-771.2, -464.6] [-401.4, 559.2] |
| NAUO6 | **J3-3** | 31,531 | [-16.7, 337.5] [1380.5, 1678.2] [-1476.4, -984.5] |
| NAUO7 | **J3-1** | 17,058 | [-149.1, 345.2] [1013.7, 1683.9] [-680.4, -210.3] |
| NAUO8 | **J2** | 6,208 | [188.1, 400.2] [-287.1, 1406.8] [-600.0, -174.7] |
| NAUO9 | **J3-2** | 1,272 | [44.2, 337.1] [1380.9, 1673.8] [-987.9, -677.4] |
| NAUO10 | **J2-1** | 1,156 | [387.2, 512.1] [28.0, 675.6] [-482.2, -315.3] |
| NAUO11 | **J2-2** | 3,354 | [396.4, 502.9] [-428.5, 182.8] [-527.4, -369.2] |
| NAUO12 | Cable2 | 844 | [494.4, 596.9] [141.7, 1112.9] [-445.1, -334.6] |
| NAUO13 | 3D包络 | 54,354 | [-3889.1, 4271.1] [-3863.0, 3647.2] [-4140.2, 4020.4] |
| NAUO14 | cable1 | 28,112 | [13.4, 159.7] [-767.0, -591.4] [537.5, 878.4] |
| | **合计** | **322,043** | |

> `3D包络` 是**工作空间包络体**（约 8 m 立方），不是机器人本体。做动画时应当排除或单独显示。

### 3.2 目录内容

```
_extract/mesh/
├── README.md        本文件
├── mapping.json     零件名 / 三角面数 / 包围盒 / 对应 STL 文件
├── preview.png      两视图渲染预览（侧视 Y-Z、前视 X-Z）+ 关节图例
├── parts/           14 个按零件命名的 STL ← 动画直接用这个
│   ├── J0.stl  J1.stl  J2.stl  J2-1.stl  J2-2.stl
│   ├── J3-1.stl  J3-2.stl  J3-3.stl
│   ├── J4.stl  J5.stl  J6.stl
│   ├── cable1.stl  Cable2.stl  3D包络.stl
└── solids/          36 个按实体拆的 STL（中间产物，按 STEP 内部实体顺序编号）
```

- **`parts/`** 是给动画用的：一个关节连杆一个文件。
- **`solids/`** 是中间产物：按 OCC 遍历顺序拆的 36 个实体，文件名 `solid_01.stl` … `solid_36.stl`。
  保留它是为了核对——36 个实体正好分到 14 个零件，数目对得上。

### 3.3 网格参数

- 三角化方式：**OpenCASCADE `BRepMesh_IncrementalMesh`**，线性偏差 1.5 mm，角偏差 0.5 rad，并行开启。
- 输出：二进制 STL，法向按右手定则从顶点顺序重算。
- 坐标系：**STEP 原始装配坐标系**，单位 mm，未做任何平移/旋转/单位换算。

---

## 4. 怎么复现

```bash
# 依赖（已装在托管 venv 里）
export UV_CACHE_DIR=/home/bonvoyage/.workbuddy-ai/binaries/uv-cache
/home/bonvoyage/.workbuddy-ai/binaries/python/versions/3.13.12/bin/python3 \
    -m venv /home/bonvoyage/.workbuddy-ai/binaries/python/envs/cad
uv pip install --python /home/bonvoyage/.workbuddy-ai/binaries/python/envs/cad/bin/python \
    cadquery-ocp matplotlib

# 转换（本仓库根目录执行）
/home/bonvoyage/.workbuddy-ai/binaries/python/envs/cad/bin/python tools/step2stl.py \
    "_extract/cad-original/SR175-3200 演示模型/SR175-3200本体演示模型.STEP" \
    _extract/mesh/parts
```

**复现验证记录（2026-10-06）**：用 `tools/step2stl.py` 重新跑一遍到临时目录，
14 个 STL 与 `parts/` 里现有文件**逐字节一致**（`cmp -s` 全部通过），总三角面数同为 322,043。

---

## 5. 还缺什么（重要，别当成已经能动了）

几何这一块**已经补齐**。但要让"仿真画面"真的动起来，还差**运动学**这一层：

| 缺项 | 说明 | 影响 |
|---|---|---|
| **关节轴线位置与方向** | STEP 只给了零件在**某个固定演示姿态**下的形状。每个关节的旋转轴在哪、朝哪个方向，要从 CAD 的装配配合里提取，或从连杆几何反推。 | 没有它无法让零件绕正确轴线转 |
| **装配坐标系 ↔ 控制坐标系的对齐** | 当前坐标是 SolidWorks 装配系（实测：本体主要延展在 Y ∈ [-771, 1684]、Z ∈ [-2127, 878]），与机器人基坐标系不是同一个。 | 需要一组平移+旋转把两者对齐 |
| **连杆参数口径** | `RobotParameter.xml` 有 6 个距离值，但 6 个 `SRRot*` 全为 0，命名非标准 DH。 | 决定 FK 怎么算 |
| **关节限位** | `SystemConfig.xml` 的 `LowerLimit`/`UpperLimit` 全为 0。 | 决定动画里能不能越界 |
| **工具 TCP** | `ToolCalibration.xml` 是全零模板。 | 末端姿态没有参考点 |

**一句话**：三维模型从"没有"变成"有"，卡点从"几何"移到了"运动学参数"。

---

## 6. 工具链踩坑记录（下次直接用）

1. **`gmsh` 干不了这件事。**
   它能把 STEP 读进来（36 实体 / 10,946 面，47 秒），但 2D 网格器在这份 CAD 上直接报
   `The 1D mesh seems not to be forming a closed loop`；开 OCC 修复选项、换 Delaunay 算法都无效。
   而且内存占用远高于 OCC——本机可用内存只有 3.0 GiB 时，粗网格设置下也会被 OOM 杀掉。
   → **CAD 转网格请用 OpenCASCADE 的增量网格化，不要用 gmsh。**

2. **`STEPControl_Reader` 与 `STEPCAFControl_Reader` 是两个独立世界。**
   对前者读出的形体做 `BRepMesh_IncrementalMesh`，不会让后者文档里的形体拿到三角化。
   要按零件分组导出，必须**对 XCAF 里的每个形体单独网格化**。

3. **零件名要三级解引用。**
   装配实例标签上只有 `NAUO1`…`NAUO14` 这种自动名。真实名字在**被引用的原始标签**上，
   要 `GetReferredShape` 解引用才能拿到。

4. **STEP 里的中文是 GBK，OCC 会读成乱码。**
   实测码点：`SR175-3200` 后面跟着 `0xB1 0xBE 0x365 0x49D 0x2BE 0x123 0x44D` ——
   后 5 个码点已经超过 `0xFF`，`latin-1` 回环解不出来。
   → **绕过 OCC，直接按 GBK 读 STEP 原文**，正则解析
   `PRODUCT` / `PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE` / `PRODUCT_DEFINITION`
   / `NEXT_ASSEMBLY_USAGE_OCCURRENCE` 四级关系。
   注意 FORMATION 的实体名带 `_WITH_SPECIFIED_SOURCE` 后缀，写 `PRODUCT_DEFINITION_FORMATION \(` 匹配不到。

5. **OCP 的 API 名称与老版 pythonocc 不同**（OCP 8.0.1.1）：
   - `TopoDS.Solid_s` ❌ → `TopoDS.Solid` ✅
   - `TDF_LabelSequence` ❌ → `OCP.collections.Sequence_TDF_Label` ✅
   - `Bnd_Box.Get()` 返回未注册的 struct ❌ → `CornerMin()` / `CornerMax()` ✅
   - `XCAFDoc_ShapeTool.GetFreeShapes_s` ❌ → `GetFreeShapes` ✅

6. **本机 `/tmp` 是只有 10 MB 的 tmpfs。**
   往 `/tmp` 写 16 MB 的 STL 会 `No space left on device`。中间产物一律写到项目盘。

7. **沙箱的 `os.remove` 走回收站机制，在无 `HOME`/`XDG_DATA_HOME` 时会失败。**
   Python 里清目录用 `shutil` 会报 `Neither the XDG_DATA_HOME nor the HOME environment variable was found`；
   用 shell 的 `rm` 反而正常。

---

## 7. 相关文档

- `../../docs/仿真缺项分析-2026-10-06.md` §11 —— 本节结论在缺口分析里的位置
- `../../docs/整理记录-2026-10-06.md` —— 项目资料整理记录
- `../../tools/step2stl.py` —— 本目录的生成脚本（带注释的坑位说明）
