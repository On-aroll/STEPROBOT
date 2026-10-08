#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""STEP 机器人实验日志分析：JSONL → 汇总表 + 遥测 CSV + 轨迹图。

日志由 src/StepRobot.App 产生，事件类型：
    session / state / run.begin / step.begin / step.end / step.dry-run /
    step.rejected / step.failed / poll.fail / run.end

用法：
    py -3 tools/analyze_run.py logs/exp-001.jsonl
    py -3 tools/analyze_run.py logs/*.jsonl --out-dir reports --no-png

退出码：0 全部步骤达成；1 有步骤被拒 / 失败 / 超时；2 输入或依赖问题。
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import sys
from pathlib import Path

try:
    import pandas as pd
    import matplotlib

    matplotlib.use("Agg")  # 无界面环境也要能出图
    import matplotlib.pyplot as plt
except ImportError as ex:  # pragma: no cover
    print(f"缺少依赖：{ex}\n先执行： py -3 -m pip install pandas matplotlib", file=sys.stderr)
    sys.exit(2)

AXES = ["A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8"]
STUB_TOKENS = ("STUB", "FAKE", "MOCK")


def _force_utf8() -> None:
    """Windows 控制台默认 GBK，打印中文箭头会抛 UnicodeEncodeError。
    重定向时 stream 可能没有 reconfigure，所以包一层 try。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


_force_utf8()


def _clean(value):
    """pandas 会把缺失键填成 NaN，而 float('nan') 是"真值"——
    `row.get('detail') or row.get('error')` 这种写法会被它骗过。"""
    if value is None:
        return None
    if isinstance(value, float) and math.isnan(value):
        return None
    try:
        if pd.isna(value):
            return None
    except (TypeError, ValueError):
        pass
    return value


def _as_list(value):
    """把 payload 里的数组统一成 list；None / NaN / 空 / 标量都返回 None。
    注意不能直接 `if not value` —— numpy 数组会抛 truth-value 歧义。"""
    value = _clean(value)
    if value is None:
        return None
    try:
        seq = list(value)
    except TypeError:
        return None
    return seq or None


def _seconds(series: pd.Series, t0) -> pd.Series:
    return (pd.to_datetime(series, utc=True, errors="coerce") - t0).dt.total_seconds()


def load(path: Path):
    """读一个 JSONL，返回 (遥测, 步骤, 异常, 会话配置)。

    时间基准必须是**全局同一个 t0**：遥测从轮询线程启动就开始记，
    步骤事件要等采集窗口跑完才出现，两者若各按各的最早时间归零，
    两条时间轴会差出整个采集窗口那么长，"取运动之后第一个遥测点"就会取到运动之前的值。
    """
    states, steps, faults = [], [], []
    session: dict = {}
    all_ts: list[str] = []

    with path.open(encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError:
                print(f"  ! {path.name}:{lineno} 不是合法 JSON，已跳过", file=sys.stderr)
                continue

            evt, ts, p = rec.get("evt"), rec.get("ts"), dict(rec.get("payload") or {})
            if ts:
                all_ts.append(ts)
            if evt == "session":
                p["ts"] = ts
                session = p
            elif evt == "state":
                p["ts"] = ts
                states.append(p)
            elif evt and evt.startswith("step."):
                p["evt"] = evt
                p["ts"] = ts
                steps.append(p)
            elif evt in ("poll.fail", "run.begin", "run.end"):
                p["evt"] = evt
                p["ts"] = ts
                faults.append(p)

    t0 = pd.to_datetime(pd.Series(all_ts), utc=True, errors="coerce").min()

    def frame(rows):
        if not rows:
            return pd.DataFrame()
        df = pd.DataFrame(rows)
        df["t"] = _seconds(df["ts"], t0)
        return df

    st = frame(states)
    steps_df = frame(steps)
    faults_df = frame(faults)

    if not st.empty and "j" in st.columns:
        joints = pd.DataFrame([j or [] for j in st["j"]])
        for i, name in enumerate(AXES):
            st[name] = joints[i] if i in joints.columns else float("nan")
        st = st.sort_values("t").reset_index(drop=True)

    for df in (steps_df, faults_df):
        if not df.empty:
            df.sort_values("t", inplace=True)
            df.reset_index(drop=True, inplace=True)

    return st, steps_df, faults_df, session


def terminal_joint_error(st: pd.DataFrame, target, end_ts: float | None):
    """MoveJ 终点关节误差，不是整段轨迹跟踪误差或 TCP 空间误差。"""
    target = _as_list(target)
    if target is None or st.empty or end_ts is None or math.isnan(end_ts):
        return None
    later = st[st["t"] >= end_ts]
    if later.empty:
        return None
    row = later.iloc[0]
    if len(target) != 6 or row.get("health") != "Connected":
        return None
    if any(a not in row.index or not math.isfinite(float(row[a])) for a in AXES[:6]):
        return None
    if any(not math.isfinite(float(v)) for v in target):
        return None
    return max(abs(float(row[a]) - float(v)) for a, v in zip(AXES[:6], target))


def analyse(path: Path, out_dir: Path, make_png: bool) -> tuple[bool, list[str]]:
    reasons: list[str] = []
    st, steps, faults, session = load(path)
    run = path.stem
    print(f"\n{'=' * 72}\n运行 {run}    （{path}）")

    ver = str(session.get("sdkVersion") or "?")
    stub = any(tok in ver.upper() for tok in STUB_TOKENS)
    flag = "   [!] 仿真桩件数据，不得作为真机结论" if stub else ""
    limits = _clean(session.get("limits"))
    print(f"  SDK {ver}   干跑={session.get('dryRun')}   "
          f"限位={limits if limits else '未配置'}{flag}")

    if st.empty:
        reasons.append("无遥测样本")
        print("  没有遥测样本（跑时加 --telemetry-ms 100 才会记录状态序列）")
    else:
        span = st["t"].max() - st["t"].min()
        print(f"  遥测 {len(st)} 点 / {span:.1f}s"
              f"（平均间隔 {span / max(len(st) - 1, 1) * 1000:.0f}ms，"
              f"最大空档 {st['t'].diff().max() * 1000:.0f}ms）")
        for a in AXES[:6]:
            if a in st and st[a].notna().any():
                print(f"    {a}: 起 {st[a].iloc[0]:8.2f}  止 {st[a].iloc[-1]:8.2f}  "
                      f"范围 [{st[a].min():8.2f}, {st[a].max():8.2f}]  "
                      f"行程 {st[a].max() - st[a].min():6.2f}")

    begins = steps[steps["evt"] == "step.begin"] if not steps.empty else pd.DataFrame()
    dry_session = bool(_clean(session.get("dryRun")) or False)

    ok_all = True
    if steps.empty:
        if session.get("captureOnly"):
            print("  只读采集会话，无运动步骤")
        else:
            reasons.append("无步骤事件（日志被截断？）")
            print("  没有任何步骤事件")
            ok_all = False
    else:
        print("  步骤时间线：")
        for _, e in steps.iterrows():
            if e["evt"] == "step.begin":
                continue
            idx = _clean(e.get("index"))
            b = begins[begins["index"] == idx] if idx is not None and "index" in begins else pd.DataFrame()
            name = _clean(e.get("step")) or (b.iloc[0]["step"] if len(b) else "?")
            begin_t = float(b.iloc[0]["t"]) if len(b) else None
            latency = (float(e["t"]) - begin_t) if begin_t is not None else float("nan")

            status = e["evt"].replace("step.", "")
            label = f"[{idx}] {name}"
            if status == "end":
                ok = _clean(e.get("ok"))
                if ok is not None:
                    ok_all = ok_all and bool(ok)
                    if not ok:
                        reasons.append(f"{label} 未达成（超时）")
            elif not dry_session:
                # 干跑下 dry-run / rejected 都是预期结果，不能算失败
                ok_all = False
                reasons.append(f"{label} {status}")

            detail = (_clean(e.get("detail")) or _clean(e.get("error"))
                      or _clean(e.get("violations")) or "")
            extra = ""
            if status == "end" and name == "MoveJ":
                err = terminal_joint_error(st, (b.iloc[0].get("target") if len(b) else None), float(e["t"]))
                extra = (f"  终点关节误差 {err:.3f}°" if err is not None
                         else "  终点关节误差 无法计算（缺少有效的六轴反馈）")
            lat = f"{latency:5.2f}s" if not math.isnan(latency) else "   —  "
            print(f"    [{idx}] {str(name):<11} {e['t']:6.2f}s  耗时 {lat}  {status:<9} {str(detail)[:44]}{extra}")

    pf = faults[faults["evt"] == "poll.fail"] if not faults.empty else pd.DataFrame()
    if not pf.empty:
        ok_all = False
        reasons.append(f"轮询失败 {len(pf)} 次")
        print(f"  ! 轮询失败 {len(pf)} 次，首条：{_clean(pf.iloc[0].get('error'))}")

    re_ = faults[faults["evt"] == "run.end"] if not faults.empty else pd.DataFrame()
    if re_.empty:
        reasons.append("缺少 run.end，采集可能未正常结束")
        ok_all = False
    if not re_.empty:
        p = re_.iloc[0]

        def n(key):
            v = _clean(p.get(key))
            return "—" if v is None else int(v)

        aborted = bool(_clean(p.get("aborted")) or False)
        print(f"  收尾：执行 {n('executed')} / 拒绝 {n('rejected')} / "
              f"失败 {n('failed')} / 跳过 {n('dryRunSkipped')}  中止={aborted}")
        if aborted and not dry_session:
            reasons.append("整轮中止")
        ok_all = ok_all and not aborted

    out_dir.mkdir(parents=True, exist_ok=True)
    if not st.empty:
        csv = out_dir / f"{run}_telemetry.csv"
        cols = ["t"] + AXES[:6] + ["ovl", "en", "mode", "line", "health"]
        st[[c for c in cols if c in st.columns]].to_csv(csv, index=False, encoding="utf-8-sig")
        print(f"  → {csv}")

    if make_png and not st.empty:
        png = out_dir / f"{run}_traces.png"
        fig, axs = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
        for a in AXES[:6]:
            if a in st:
                axs[0].plot(st["t"], st[a], lw=1.4, label=a)
        # 图内标签用英文：matplotlib 默认字体没有 CJK 字形，中文会画成方块
        axs[0].set_ylabel("joint angle (deg)")
        axs[0].legend(ncol=6, fontsize=8)
        for _, b in (begins.iterrows() if not begins.empty else []):
            if _as_list(b.get("target")):
                axs[0].axvline(float(b["t"]), ls="--", lw=0.9, color="crimson")
                axs[0].annotate("cmd", (float(b["t"]), axs[0].get_ylim()[1]),
                                fontsize=7, color="crimson", ha="right")
        if "ovl" in st:
            axs[1].plot(st["t"], st["ovl"], lw=1.2, color="tab:green")
        axs[1].set_ylabel("override (%)")
        if "en" in st:
            axs[2].step(st["t"], st["en"].astype(int), where="post", color="tab:blue")
        axs[2].set_ylabel("servo enabled")
        axs[2].set_xlabel("time (s) — aligned to one global t0")
        fig.suptitle(f"{run}   SDK={ver}" + ("   [STUB - not real data]" if stub else ""), fontsize=11)
        fig.tight_layout()
        fig.savefig(png, dpi=110)
        plt.close(fig)
        print(f"  → {png}")

    return ok_all, reasons


def main() -> int:
    ap = argparse.ArgumentParser(description="分析 STEP 机器人上位机 JSONL 实验日志")
    ap.add_argument("logs", nargs="+", help="一个或多个 .jsonl（支持通配符）")
    ap.add_argument("--out-dir", default="reports", help="CSV / PNG 输出目录")
    ap.add_argument("--no-png", action="store_true", help="只出表不出图")
    args = ap.parse_args()

    files: list[Path] = []
    resolved: set[str] = set()
    for pat in args.logs:
        wildcarded = any(ch in pat for ch in "*?[")
        for h in (sorted(glob.glob(pat)) if wildcarded else [pat]):
            p = Path(h)
            if p.exists():
                files.append(p)
                resolved.add(str(p.resolve()))
    # 只报"给了确切路径却不存在"；Path 会把 / 改写成 \，必须按绝对路径比
    missing = [m for m in args.logs
               if not any(ch in m for ch in "*?[") and str(Path(m).resolve()) not in resolved]
    for m in sorted(missing):
        print(f"! 找不到 {m}", file=sys.stderr)
    if not files:
        return 2

    out_dir = Path(args.out_dir)
    verdicts = [analyse(f, out_dir, not args.no_png) for f in files]  # 不用 all(生成器)：会短路漏掉后续文件

    bad = [(f.stem, rs) for f, (ok, rs) in zip(files, verdicts) if not ok]
    print(f"\n{'=' * 72}")
    print(f"结论：{len(files)} 个日志全部干净" if not bad else f"结论：{len(files)} 个日志中 {len(bad)} 个有问题")
    for name, rs in bad:
        print(f"  [!] {name}: {'；'.join(rs) if rs else '未达成'}")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
