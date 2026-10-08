using StepRobot.Core;

namespace StepRobot.Core.Tests;

/// IRobotLink 的内存替身 —— 单元测试用它，而不是桩件 DLL。
/// 抽象层的价值就在这里：业务逻辑可以不碰任何二进制就全部测完。
public sealed class FakeRobotLink : IRobotLink
{
    public List<string> WriteCalls { get; } = new();
    public int ReadCount { get; private set; }
    public RobotSnapshot State { get; set; }
    public bool Connected { get; set; } = true;
    public string SdkVersion => "FAKE";

    /// 连续 N 次让 ReadState 抛异常，模拟掉线。
    public int FailReads { get; set; }

    /// false 时点动不更新位置，用来测"等到位超时"。
    public bool MoveArrives { get; set; } = true;
    public double[]? MoveFeedbackOverride { get; set; }
    public bool FailReadsAfterMove { get; set; }

    public JointTarget? LastTarget { get; private set; }
    public double? LastOverride { get; private set; }

    public FakeRobotLink(RobotSnapshot? initial = null) => State = initial ?? Snap();

    public void Connect(string ip, int port) => WriteCalls.Add($"Connect({ip}:{port})");
    public void Close() => WriteCalls.Add("Close");
    public void Dispose() { }

    public RobotSnapshot ReadState()
    {
        if (FailReads > 0) { FailReads--; throw new RobotLinkException("模拟读取失败"); }
        ReadCount++;
        return State with { At = DateTimeOffset.Now };
    }

    public void ConfirmAllAlarms() => WriteCalls.Add("ConfirmAllAlarms");

    public void SetOverride(double percent)
    {
        WriteCalls.Add($"SetOverride({percent:0.##})");
        LastOverride = percent;
        State = State with { OverridePercent = percent };
    }

    public void ToggleMotorEnable()
    {
        WriteCalls.Add("ToggleMotorEnable");
        State = State with { MotorsEnabled = !State.MotorsEnabled };
    }

    public void JogToJointAbsolute(JointTarget target, string tool, string refSys)
    {
        WriteCalls.Add($"JogToJointAbsolute({target}, {tool}, {refSys})");
        LastTarget = target;
        if (MoveArrives) State = State with { JointDeg = target.Deg };
        if (MoveFeedbackOverride is not null) State = State with { JointDeg = MoveFeedbackOverride };
        if (FailReadsAfterMove) FailReads = int.MaxValue;
    }

    public void JogExit() => WriteCalls.Add("JogExit");

    public static RobotSnapshot Snap(
        bool enabled = false,
        double overridePercent = 15.0,
        LinkHealth health = LinkHealth.Connected,
        OperationMode mode = OperationMode.Manual,
        double[]? joints = null,
        DateTimeOffset? at = null) => new(
            at ?? DateTimeOffset.Now,
            joints ?? new double[] { 7.02, -12.5, 30, 0, 90, -180 },
            new double[] { 800, 10, 1027, 0, 90, -180 },
            enabled, mode, overridePercent,
            "project0", "program0", "Default", 0, null, null, health);

    /// 六轴 ±360°，只为了让"限位已配置"这个分支可测；真机必须按本体行程表填。
    public static (double Min, double Max)[] WideLimits(int n = 6, double half = 360)
        => Enumerable.Range(0, n).Select(_ => (-half, (double)half)).ToArray();
}
