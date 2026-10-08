using System.Diagnostics;

namespace StepRobot.Core;

public sealed class RunnerOptions
{
    /// 唯一的运动总闸。**默认 false = 干跑**：只做策略评估和日志，一次也不会碰 IRobotLink 的写接口。
    /// 现场必须显式传 --allow-motion 才可能让机器人动。
    public bool AllowMotion { get; init; }
    public double ArrivalToleranceDeg { get; init; } = 0.5;
    public TimeSpan MoveTimeout { get; init; } = TimeSpan.FromSeconds(30);
    public TimeSpan WaitPoll { get; init; } = TimeSpan.FromMilliseconds(100);
}

public sealed record StepOutcome(int Index, string Step, string Status, string Detail, double Ms);

public sealed record RunReport(bool DryRun, bool Aborted, IReadOnlyList<StepOutcome> Outcomes)
{
    public int CountBy(string status) => Outcomes.Count(o => o.Status == status);
}

/// 指令序列器：逐步「评估安全策略 → 执行 → 等到位」，全程落日志。
public sealed class ExperimentRunner
{
    private readonly IRobotLink _link;
    private readonly StatePoller _poller;
    private readonly SafetyGuard _guard;
    private readonly ISink _sink;
    private readonly RunnerOptions _opt;
    private readonly Stopwatch _sinceEnable = new();

    public ExperimentRunner(IRobotLink link, StatePoller poller, SafetyGuard guard, ISink sink, RunnerOptions? opt = null)
    {
        _link = link;
        _poller = poller;
        _guard = guard;
        _sink = sink;
        _opt = opt ?? new RunnerOptions();
    }

    public async Task<RunReport> RunAsync(IReadOnlyList<Step> steps, CancellationToken ct = default)
    {
        var outcomes = new List<StepOutcome>();
        var dryRun = !_opt.AllowMotion;
        var aborted = ct.IsCancellationRequested;

        _sink.Write("run.begin", new { dryRun, planned = steps.Count, policy = _guard.Policy.MaxOverridePercent });

        for (int i = 0; i < steps.Count && !ct.IsCancellationRequested; i++)
        {
            var step = steps[i];
            var snap = _poller.Latest;
            var sinceEnable = _sinceEnable.IsRunning ? _sinceEnable.Elapsed : (TimeSpan?)null;
            var violations = _guard.Evaluate(step, snap, sinceEnable, _poller.ConsecutiveFailures);

            if (violations.Count > 0)
            {
                // 干跑时违规只是"预告"；真跑时违规即中止，绝不带着已知违规继续往下发指令。
                var status = dryRun ? "dry-run" : "rejected";
                outcomes.Add(new StepOutcome(i, step.Name, status, string.Join("；", violations), 0));
                _sink.Write("step." + status, new { index = i, step = step.Name, violations });
                if (!dryRun) { aborted = true; break; }
                continue;
            }

            if (dryRun)
            {
                outcomes.Add(new StepOutcome(i, step.Name, "dry-run", "策略放行，但未加 --allow-motion 故不执行", 0));
                _sink.Write("step.dry-run", new { index = i, step = step.Name });
                continue;
            }

            var sw = Stopwatch.StartNew();
            _sink.Write("step.begin", new
            {
                index = i,
                step = step.Name,
                target = (step as MoveJ)?.Target.Deg,
                rate = (step as SetRate)?.Percent,
            });
            try
            {
                var (detail, ok) = await DispatchAsync(step, ct).ConfigureAwait(false);
                sw.Stop();
                var status = ok ? "executed" : "timeout";
                outcomes.Add(new StepOutcome(i, step.Name, status, detail, sw.Elapsed.TotalMilliseconds));
                _sink.Write("step.end", new { index = i, step = step.Name, detail, ok, ms = Math.Round(sw.Elapsed.TotalMilliseconds) });

                // 没到位 / 没等到条件就继续发下一条运动指令，等于在未知状态下盲动 —— 必须中止。
                if (!ok) { aborted = true; break; }
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested)
            {
                aborted = true;
                outcomes.Add(new StepOutcome(i, step.Name, "cancelled", "用户取消", sw.Elapsed.TotalMilliseconds));
                _sink.Write("step.cancelled", new { index = i, step = step.Name });
                break;
            }
            catch (Exception ex)
            {
                sw.Stop();
                aborted = true;
                outcomes.Add(new StepOutcome(i, step.Name, "failed", ex.Message, sw.Elapsed.TotalMilliseconds));
                _sink.Write("step.failed", new { index = i, step = step.Name, error = ex.Message });
                break;
            }
        }

        aborted |= ct.IsCancellationRequested;
        if (!dryRun)
        {
            // 手册要求点动通道用完必须显式关闭，否则下一次点动行为未定义。
            try { _link.JogExit(); } catch (Exception ex) { _sink.Write("jogexit.failed", new { error = ex.Message }); }
        }

        var report = new RunReport(dryRun, aborted, outcomes);
        _sink.Write("run.end", new
        {
            dryRun,
            aborted,
            executed = report.CountBy("executed"),
            rejected = report.CountBy("rejected"),
            failed = report.CountBy("failed"),
            dryRunSkipped = report.CountBy("dry-run"),
        });
        return report;
    }

    private async Task<(string Detail, bool Ok)> DispatchAsync(Step step, CancellationToken ct)
    {
        switch (step)
        {
            case SetRate s:
                _link.SetOverride(s.Percent);
                return ($"倍率 → {s.Percent:0.#}%", true);

            case Enable:
                _link.ConfirmAllAlarms();
                _link.ToggleMotorEnable();
                _sinceEnable.Restart();
                return ("已发送使能，计时 ≥300ms 死区", true);

            case WaitEnabled w:
                return await WaitAsync(s => s.MotorsEnabled, w.Timeout, "使能生效", ct).ConfigureAwait(false);

            case MoveJ s:
                long beforeMove = _poller.Samples;
                _link.JogToJointAbsolute(s.Target, s.Tool, s.RefSys);
                var (note, arrived) = await WaitAsync(
                    state => _poller.Samples > beforeMove && JointError(state.JointDeg, s.Target) <= _opt.ArrivalToleranceDeg,
                    _opt.MoveTimeout, "到位", ct).ConfigureAwait(false);
                return ($"{note}，最大关节误差 {JointError(_poller.Latest.JointDeg, s.Target):0.###}°", arrived);

            case Hold h:
                await Task.Delay(h.For, ct).ConfigureAwait(false);
                return ($"等待 {h.For.TotalMilliseconds:0}ms", true);

            case WaitFor w:
                return await WaitAsync(w.Predicate, w.Timeout, w.Because, ct).ConfigureAwait(false);

            default:
                return ($"未识别的步骤类型 {step.GetType().Name}，未执行", false);
        }
    }

    private async Task<(string Detail, bool Ok)> WaitAsync(Func<RobotSnapshot, bool> condition, TimeSpan timeout, string because, CancellationToken ct)
    {
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < timeout)
        {
            ct.ThrowIfCancellationRequested();
            var snap = _poller.Latest;
            if (snap.Health != LinkHealth.Connected || _poller.ConsecutiveFailures > 0 ||
                snap.At == DateTimeOffset.MinValue || DateTimeOffset.Now - snap.At > _guard.Policy.MaxSnapshotAge)
                return ($"{because}中止：状态失效或通信中断", false);
            if (condition(snap)) return ($"{because}达成（{sw.Elapsed.TotalSeconds:0.##}s）", true);
            await Task.Delay(_opt.WaitPoll, ct).ConfigureAwait(false);
        }
        return ($"{because}超时（>{timeout.TotalSeconds:0.#}s）", false);
    }

    private static double JointError(double[] current, JointTarget target)
    {
        if (current.Length == 0 || current.Length != target.Count ||
            current.Any(v => !double.IsFinite(v)) || target.Deg.Any(v => !double.IsFinite(v)))
            return double.PositiveInfinity;
        double worst = 0;
        for (int i = 0; i < Math.Min(current.Length, target.Count); i++)
            worst = Math.Max(worst, Math.Abs(current[i] - target.Deg[i]));
        return worst;
    }
}
