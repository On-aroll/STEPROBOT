# -*- coding: utf-8 -*-
"""
环境冒烟测试：在 VS Code 里按 F5 或右上角▶运行本文件。
检查 Python 数值计算和工作区文件，不验证机械臂模型或 STEP Studio 运行状态。
必需依赖或数值自检失败时返回非零退出码；可选库缺失不算失败。
"""
import sys
import shutil
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
errors = []

print("=" * 60)
print("1) 解释器")
print("   python =", sys.executable)
print("   版本   =", sys.version.split()[0])
if (Path(sys.prefix) / "conda-meta").is_dir():
    print("   [OK] 用的是 conda 环境的解释器")
else:
    print("   [提示] 当前不是 conda 环境，以以下依赖检查结果为准")

print("\n2) 科学计算库（仿真主力，都应该在）")
for mod in ("numpy", "scipy", "matplotlib", "pandas", "sympy", "numba"):
    try:
        m = __import__(mod)
        print(f"   [OK]   {mod:11s} {getattr(m, '__version__', '?')}")
    except Exception as exc:
        errors.append(mod)
        print(f"   [失败] {mod}: {exc}")

print("\n3) 需要时才补的库（做熔池图像/深度学习才要）")
for mod in ("torch", "cv2", "PIL", "open3d"):
    try:
        m = __import__(mod)
        print(f"   [OK]   {mod:11s} {getattr(m, '__version__', '?')}")
    except Exception as exc:
        print(f"   [可选] {mod:11s} 不可用（{exc}），需要时再处理")

print("\n4) 外部工具链")
for tool in ("conda", "git", "cl", "g++", "cmake"):
    path = shutil.which(tool)
    print(f"   {tool:7s} : {path if path else '（未安装 / 不在 PATH）'}")
print("   提示：cl / cmake 不在普通终端 PATH，不等于未安装；C++ 请另跑 build_msvc.bat。")

print("\n5) 本工作区文件是否齐")
root = Path(__file__).resolve().parent
for name in ("docs/STEP机器人研究上手地图.md", "_extract/txt", "_extract/sdk", "research/cpp-starter/src/main.cpp"):
    p = root / name
    print(f"   [{'OK' if p.exists() else '缺'}]  {p}")
    if not p.exists():
        errors.append(name)

print("=" * 60)
print("正运动学最小的一个自检：两层连杆 L1=1, L2=1, q=(90°,0°) 应该在 (0,2)")
try:
    import numpy as np
    def fk2(th1, th2, L1=1.0, L2=1.0):
        c1, s1 = np.cos(th1), np.sin(th1)
        c12, s12 = np.cos(th1 + th2), np.sin(th1 + th2)
        return np.array([L1 * c1 + L2 * c12, L1 * s1 + L2 * s12])
    p = fk2(np.radians(90), 0.0)
    print("   算出来 =", np.round(p, 6), "  误差 =", np.abs(p - np.array([0, 2])).max())
    if not np.allclose(p, [0, 2]):
        raise ValueError("numpy 计算异常")
    print("   [OK] 二连杆数值示例通过（不是本项目六轴机械臂模型验证）")
except Exception as exc:
    errors.append("数值自检")
    print(f"   [失败] {exc}")

if errors:
    print("\n自检未通过：" + "、".join(errors))
else:
    print("\nPython 数值计算与所列工作区文件自检通过。")
print("STEP Studio 激活、虚拟控制器、机型与轨迹运行仍需单独验收。")
sys.exit(1 if errors else 0)
