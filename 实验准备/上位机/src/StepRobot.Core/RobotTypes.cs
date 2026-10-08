namespace StepRobot.Core;

public enum LinkHealth { Connected, Degraded, Disconnected }

public enum OperationMode { Unknown, Manual, ManualHigh, Automatic, AutomaticExternal }

/// 一次控制器状态快照。SDK 无回调，全部状态只能轮询得到，故快照是唯一的事实来源。
public sealed record RobotSnapshot(
    DateTimeOffset At,
    double[] JointDeg,
    double[]? CartPos,
    bool MotorsEnabled,
    OperationMode Mode,
    double OverridePercent,
    string ProjectName,
    string ProgramName,
    string ToolName,
    int CurrentLine,
    ulong[]? DiWords,
    string? LastMessage,
    LinkHealth Health)
{
    public static RobotSnapshot Empty(LinkHealth health) => new(
        DateTimeOffset.MinValue, Array.Empty<double>(), null, false,
        OperationMode.Unknown, 0, "", "", "", -1, null, null, health);
}

/// 绝对关节目标。单位与 SDK 一致：度。
public sealed record JointTarget(double[] Deg)
{
    public int Count => Deg.Length;
    public override string ToString() => "[" + string.Join(", ", Deg.Select(d => d.ToString("0.###"))) + "]";
}

public abstract record Step
{
    public abstract string Name { get; }
}

public sealed record SetRate(double Percent) : Step
{
    public override string Name => "SetRate";
}

public sealed record MoveJ(JointTarget Target, string Tool = "Default", string RefSys = "WORLD") : Step
{
    public override string Name => "MoveJ";
}

/// 上使能。SDK 的 EnableMotorCmd 是**切换式**的（同一接口上/下使能），
/// 因此必须作为独立步骤显式出现在序列里、可审计，不能藏在某个动作的副作用里。
public sealed record Enable : Step
{
    public override string Name => "Enable";
}

public sealed record WaitEnabled(TimeSpan Timeout) : Step
{
    public override string Name => "WaitEnabled";
}

public sealed record Hold(TimeSpan For) : Step
{
    public override string Name => "Hold";
}

public sealed record WaitFor(Func<RobotSnapshot, bool> Predicate, TimeSpan Timeout, string Because = "") : Step
{
    public override string Name => "WaitFor";
}

public sealed class UnsafeCommandException : Exception
{
    public IReadOnlyList<string> Violations { get; }

    public UnsafeCommandException(Step step, IReadOnlyList<string> violations)
        : base($"拒绝执行 {step.Name}：{string.Join("；", violations)}")
        => Violations = violations;
}
