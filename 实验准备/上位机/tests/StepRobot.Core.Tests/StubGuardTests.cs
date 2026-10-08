using StepRobot.Core;
using Xunit;

namespace StepRobot.Core.Tests;

public class StubGuardTests
{
    [Theory]
    [InlineData("V2.3-STUB", true)]
    [InlineData("v2.3-stub", true)]
    [InlineData("FAKE", true)]
    [InlineData("V2.3", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void 桩件识别(string? version, bool expected)
        => Assert.Equal(expected, StubGuard.LooksLikeStub(version));

    [Fact]
    public void 干跑时桩件放行()
    {
        // 桩件本来就是给干跑和单测用的，不能把自己锁死。
        Assert.Null(StubGuard.Reject(allowMotion: false, "V2.3-STUB", stubExplicitlyAllowed: false));
    }

    [Fact]
    public void 真机版本不受影响()
        => Assert.Null(StubGuard.Reject(true, "V2.3", false));

    [Fact]
    public void 授权运动又对着桩件_必须拒绝()
    {
        var verdict = StubGuard.Reject(true, "V2.3-STUB", stubExplicitlyAllowed: false);
        Assert.NotNull(verdict);
        Assert.StartsWith("拒绝", verdict);
    }

    [Fact]
    public void 显式加锁后降为警告()
    {
        var verdict = StubGuard.Reject(true, "V2.3-STUB", stubExplicitlyAllowed: true);
        Assert.NotNull(verdict);
        Assert.StartsWith("警告", verdict);
    }
}

public class SinkContractTests
{
    [Fact]
    public async Task 真跑时每步都有begin与end配对_供分析脚本对齐时间轴()
    {
        var link = new FakeRobotLink();
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(20));
        poller.Start();
        await Task.Delay(120);

        var sink = new MemorySink();
        var runner = new ExperimentRunner(link, poller,
            new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
            sink, new RunnerOptions { AllowMotion = true });

        var report = await runner.RunAsync(new Step[]
        {
            new SetRate(10),
            new Enable(),
            new Hold(TimeSpan.FromMilliseconds(350)),
            new MoveJ(new JointTarget(new double[] { 1, 2, 3, 4, 5, 6 })),
        });
        await poller.StopAsync();

        Assert.False(report.Aborted);
        var events = sink.Lines.Select(l => l.evt).ToList();
        Assert.Equal(4, events.Count(e => e == "step.begin"));
        Assert.Equal(4, events.Count(e => e == "step.end"));

        // begin 必须带上目标点位，否则分析脚本没法算指令与实测的跟踪误差。
        // （payload 是 Core 程序集里的匿名类型，只能反射读。）
        var begins = sink.Lines.Where(l => l.evt == "step.begin").Select(l => l.payload).ToList();
        Assert.Contains(begins, p =>
            p.GetType().GetProperty("target")?.GetValue(p) is double[] arr && arr.Length > 0);
    }
}
