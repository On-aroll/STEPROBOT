namespace StepRobot.Core;

/// 软件层安全策略。
/// 存在的原因是硬事实：这套 SDK **没有任何急停 / 安全门 / 限速 / 限位接口**，
/// 硬件安全回路之外，上位机这一侧没人管速度、没人管越界 —— 只能自己管。
public sealed class SafetyPolicy
{
    /// 点动限速 250mm/s 是示教器侧规定；上位机这一侧用倍率兜底。
    public double MaxOverridePercent { get; init; } = 15.0;
    public double MinOverridePercent { get; init; } = 0.1;

    /// 每轴软限位（度）。**默认为空 = 拒绝一切运动**。
    /// 故意不给"看起来能用"的默认值：行程表必须按本机型号（如 SA1400）现场核对后填入，
    /// 猜一个数比留空更危险。
    public (double Min, double Max)[]? JointLimitsDeg { get; init; }

    public bool RequireManualMode { get; init; } = true;
    public bool RequireMotorsEnabled { get; init; } = true;

    /// 手册 P.13：使能与下一步之间必须 ≥300ms，少于这个时间无法执行下一步。
    public TimeSpan PostEnableDeadTime { get; init; } = TimeSpan.FromMilliseconds(300);

    /// 快照太旧说明轮询线程已经卡住，此时不能下发运动。
    public TimeSpan MaxSnapshotAge { get; init; } = TimeSpan.FromSeconds(2);
    public int MaxConsecutivePollFailures { get; init; } = 5;

    public static SafetyPolicy LabDefault() => new();
}

public sealed class SafetyGuard
{
    private readonly SafetyPolicy _policy;

    public SafetyGuard(SafetyPolicy policy) => _policy = policy;

    public SafetyPolicy Policy => _policy;

    /// 纯函数：给一个动作 + 当前快照 + 距上次使能的时间，返回全部违规项（空 = 放行）。
    public IReadOnlyList<string> Evaluate(
        Step step,
        RobotSnapshot snapshot,
        TimeSpan? sinceLastEnable,
        int pollFailures = 0)
    {
        var bad = new List<string>();

        switch (step)
        {
            case SetRate s:
                CheckLinkAndMode(snapshot, pollFailures, bad);
                if (!double.IsFinite(s.Percent)) bad.Add("倍率必须是有限数值");
                if (s.Percent > _policy.MaxOverridePercent)
                    bad.Add($"倍率 {s.Percent:0.#}% 超过上限 {_policy.MaxOverridePercent:0.#}%");
                if (s.Percent < _policy.MinOverridePercent)
                    bad.Add($"倍率 {s.Percent:0.###}% 低于下限 {_policy.MinOverridePercent}%");
                break;

            case MoveJ s:
                CheckLinkAndMode(snapshot, pollFailures, bad);
                CheckMotion(s, snapshot, sinceLastEnable, bad);
                break;

            case Enable:
                CheckLinkAndMode(snapshot, pollFailures, bad);
                if (snapshot.MotorsEnabled)
                    bad.Add("已经处于使能状态，再调一次 EnableMotorCmd 会**断使能**（该接口是切换式的）");
                if (snapshot.OverridePercent > _policy.MaxOverridePercent)
                    bad.Add($"使能前先把倍率降到 {_policy.MaxOverridePercent:0.#}% 以内（当前 {snapshot.OverridePercent:0.#}%）");
                if (!double.IsFinite(snapshot.OverridePercent)) bad.Add("控制器倍率无效");
                break;

            case WaitFor w:
                if (w.Timeout > TimeSpan.FromMinutes(5))
                    bad.Add($"等待 {w.Timeout.TotalSeconds:0}s 过长，实验步骤应可分段");
                break;
        }

        return bad;
    }

    private void CheckLinkAndMode(RobotSnapshot snap, int pollFailures, List<string> bad)
    {
        if (snap.Health != LinkHealth.Connected)
            bad.Add($"链路状态为 {snap.Health}");

        if (pollFailures >= _policy.MaxConsecutivePollFailures)
            bad.Add($"连续 {pollFailures} 次轮询失败，状态已不可信");

        if (snap.At == DateTimeOffset.MinValue)
            bad.Add("还没有任何状态快照，无法判断机器人当前位姿");
        else if (DateTimeOffset.Now - snap.At > _policy.MaxSnapshotAge)
            bad.Add($"状态快照已 {(DateTimeOffset.Now - snap.At).TotalSeconds:0.#}s 未更新（上限 {_policy.MaxSnapshotAge.TotalSeconds:0.#}s）");

        if (_policy.RequireManualMode &&
            snap.Mode is not (OperationMode.Manual or OperationMode.ManualHigh))
            bad.Add($"当前为 {snap.Mode} 模式，只允许在手动模式下操作");
    }

    private void CheckMotion(MoveJ step, RobotSnapshot snap, TimeSpan? sinceLastEnable, List<string> bad)
    {
        if (_policy.RequireMotorsEnabled && !snap.MotorsEnabled)
            bad.Add("机器人未使能");

        if (sinceLastEnable is null)
            bad.Add("本次会话尚未使能");
        else if (sinceLastEnable < _policy.PostEnableDeadTime)
            bad.Add($"距使能仅 {sinceLastEnable?.TotalMilliseconds:0}ms，需 ≥{_policy.PostEnableDeadTime.TotalMilliseconds:0}ms");

        if (snap.OverridePercent > _policy.MaxOverridePercent)
            bad.Add($"控制器当前倍率 {snap.OverridePercent:0.#}% 超过上限 {_policy.MaxOverridePercent:0.#}%");
        if (!double.IsFinite(snap.OverridePercent) || snap.OverridePercent < _policy.MinOverridePercent)
            bad.Add("控制器倍率无效或过低");
        if (step.Target.Count == 0 || step.Target.Deg.Any(v => !double.IsFinite(v)))
            bad.Add("关节目标必须是非空的有限数值");

        if (_policy.JointLimitsDeg is null)
        {
            bad.Add("未配置每轴软限位（JointLimitsDeg），按\"宁可不动也不乱动\"拒绝运动");
            return;
        }

        if (snap.JointDeg.Length != step.Target.Count || snap.JointDeg.Any(v => !double.IsFinite(v)))
            bad.Add("当前关节角无效或轴数不符");

        if (step.Target.Count != _policy.JointLimitsDeg.Length)
            bad.Add($"目标有 {step.Target.Count} 轴，策略定义了 {_policy.JointLimitsDeg.Length} 轴限位");

        for (int i = 0; i < Math.Min(step.Target.Count, _policy.JointLimitsDeg.Length); i++)
        {
            var (min, max) = _policy.JointLimitsDeg[i];
            if (!double.IsFinite(min) || !double.IsFinite(max) || min >= max)
                bad.Add($"A{i + 1} 限位无效");
            if (step.Target.Deg[i] < min || step.Target.Deg[i] > max)
                bad.Add($"A{i + 1} 目标 {step.Target.Deg[i]:0.##}° 超出限位 [{min:0.##}, {max:0.##}]°");
        }
    }

    public void AssertAllowed(Step step, RobotSnapshot snap, TimeSpan? sinceLastEnable, int pollFailures = 0)
    {
        var bad = Evaluate(step, snap, sinceLastEnable, pollFailures);
        if (bad.Count > 0) throw new UnsafeCommandException(step, bad);
    }
}
