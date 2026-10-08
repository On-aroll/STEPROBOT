using StepRobot.Core;
using Xunit;

namespace StepRobot.Core.Tests;

public class SafetyGuardTests
{
    [Fact]
    public void 未知模式不能使能或运动()
    {
        var snapshot = FakeRobotLink.Snap(mode: OperationMode.Unknown);
        Assert.NotEmpty(Guard().Evaluate(new Enable(), snapshot, null));
    }

    [Theory]
    [InlineData(double.NaN)]
    [InlineData(double.PositiveInfinity)]
    [InlineData(double.NegativeInfinity)]
    public void 非有限数值不能绕过比较(double value)
    {
        var guard = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        Assert.NotEmpty(guard.Evaluate(new SetRate(value), Ok(), null));
        Assert.NotEmpty(guard.Evaluate(Move6(value), Ok(), TimeSpan.FromSeconds(1)));
        Assert.NotEmpty(guard.Evaluate(Move6(), Ok() with { OverridePercent = value }, TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public void 无效限位不能绕过比较()
    {
        var limits = FakeRobotLink.WideLimits();
        limits[0] = (double.NaN, 100);
        Assert.NotEmpty(Guard(new SafetyPolicy { JointLimitsDeg = limits }).Evaluate(Move6(), Ok(), TimeSpan.FromSeconds(1)));
    }

    private static SafetyGuard Guard(SafetyPolicy? p = null) => new(p ?? new SafetyPolicy());

    private static MoveJ Move(params double[] deg) => new(new JointTarget(deg));

    private static MoveJ Move6(double a1 = 0) =>
        new(new JointTarget(new[] { a1, 0, 0, 0, 0, 0 }));

    private static RobotSnapshot Ok() => FakeRobotLink.Snap(enabled: true);

    [Fact]
    public void 倍率超上限_被拒()
    {
        var bad = Guard().Evaluate(new SetRate(40), Ok(), null);
        Assert.Contains(bad, v => v.Contains("超过上限"));
    }

    [Fact]
    public void 倍率合规_放行()
        => Assert.Empty(Guard().Evaluate(new SetRate(10), Ok(), null));

    [Fact]
    public void 没配限位时_任何运动都被拒()
    {
        // 这条断言的是"默认不危险"：未填行程表就不许动，而不是"忘了配也能跑"。
        var bad = Guard().Evaluate(Move6(), Ok(), TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("未配置每轴软限位"));
    }

    [Fact]
    public void 配了限位_目标越界仍被拒()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits(6, 30) });
        var bad = g.Evaluate(Move6(120), Ok(), TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("A1 目标 120° 超出限位"));
    }

    [Fact]
    public void 配了限位_目标合规_放行()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        Assert.Empty(g.Evaluate(Move6(10), Ok(), TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public void 未使能_不许动()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var bad = g.Evaluate(Move6(), FakeRobotLink.Snap(enabled: false), TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("未使能"));
    }

    [Fact]
    public void 使能后不足300ms_不许动()
    {
        // 手册 P.13 明文要求，这条是照抄硬件约束。
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var bad = g.Evaluate(Move6(), Ok(), TimeSpan.FromMilliseconds(120));
        Assert.Contains(bad, v => v.Contains("300ms"));
    }

    [Fact]
    public void 本次会话没使能过_不许动()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var bad = g.Evaluate(Move6(), Ok(), null);
        Assert.Contains(bad, v => v.Contains("尚未使能"));
    }

    [Fact]
    public void 快照过期_不许动()
    {
        var stale = FakeRobotLink.Snap(enabled: true, at: DateTimeOffset.Now.AddSeconds(-30));
        var bad = Guard().Evaluate(Move6(), stale, TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("未更新"));
    }

    [Fact]
    public void 自动模式_不许上位机点动()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var auto = FakeRobotLink.Snap(enabled: true, mode: OperationMode.Automatic);
        var bad = g.Evaluate(Move6(), auto, TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("Automatic"));
    }

    [Fact]
    public void 链路断开_不许动()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var dead = FakeRobotLink.Snap(enabled: true, health: LinkHealth.Disconnected);
        var bad = g.Evaluate(Move6(), dead, TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("Disconnected"));
    }

    [Fact]
    public void 轮询连续失败_状态不可信_不许动()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var bad = g.Evaluate(Move6(), Ok(), TimeSpan.FromSeconds(1), pollFailures: 5);
        Assert.Contains(bad, v => v.Contains("轮询失败"));
    }

    [Fact]
    public void 控制盒倍率超限_即使本程序没设也不许动()
    {
        // 别人可能在示教器上把倍率拧到 80%，读回来的值必须拦住我们。
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() });
        var bad = g.Evaluate(Move6(), FakeRobotLink.Snap(enabled: true, overridePercent: 80), TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("当前倍率 80%"));
    }

    [Fact]
    public void 已使能时再Enable_会断使能_被拒()
    {
        // EnableMotorCmd 是切换式的，误调一次等于运行中切扭矩。
        var bad = Guard().Evaluate(new Enable(), FakeRobotLink.Snap(enabled: true), null);
        Assert.Contains(bad, v => v.Contains("断使能"));
    }

    [Fact]
    public void 未使能时Enable_放行()
        => Assert.Empty(Guard().Evaluate(new Enable(), FakeRobotLink.Snap(enabled: false), null));

    [Fact]
    public void 轴数与限位表不一致_被拒()
    {
        var g = Guard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits(6) });
        var bad = g.Evaluate(Move(1, 2, 3), Ok(), TimeSpan.FromSeconds(1));
        Assert.Contains(bad, v => v.Contains("3 轴"));
    }

    [Fact]
    public void AssertAllowed_违规直接抛异常并带全部理由()
    {
        var ex = Assert.Throws<UnsafeCommandException>(
            () => Guard().AssertAllowed(Move6(500), FakeRobotLink.Snap(enabled: false), null));
        Assert.True(ex.Violations.Count >= 2);
        Assert.Contains("MoveJ", ex.Message);
    }
}
