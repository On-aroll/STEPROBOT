using StepRobot.Core;
using Xunit;

namespace StepRobot.Core.Tests;

public class ExperimentRunnerTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task 运动后缺失反馈或断连不能算到位(bool disconnect)
    {
        var link = new FakeRobotLink { MoveFeedbackOverride = Array.Empty<double>(), FailReadsAfterMove = disconnect };
        var (_, poller) = await Start(link);
        using (poller)
        {
            var runner = new ExperimentRunner(link, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink(), new RunnerOptions { AllowMotion = true, MoveTimeout = TimeSpan.FromMilliseconds(200), WaitPoll = TimeSpan.FromMilliseconds(20) });
            var report = await runner.RunAsync(Sequence());
            Assert.True(report.Aborted);
            Assert.Equal("timeout", report.Outcomes.Last().Status);
            Assert.Equal("JogExit", link.WriteCalls.Last());
        }
    }

    [Fact]
    public async Task 等待可取消且报告中止并退出点动()
    {
        var link = new FakeRobotLink { MoveArrives = false };
        var (_, poller) = await Start(link);
        using (poller)
        using (var cts = new CancellationTokenSource())
        {
            var runner = new ExperimentRunner(link, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink(), new RunnerOptions { AllowMotion = true });
            // WaitFor exercises cancellation inside WaitAsync, without relying on a move duration.
            var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var task = runner.RunAsync([new WaitFor(_ => { started.TrySetResult(); return false; }, TimeSpan.FromSeconds(30))], cts.Token);
            await started.Task.WaitAsync(TimeSpan.FromSeconds(2));
            cts.Cancel();
            var report = await task.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.True(report.Aborted);
            Assert.Equal("cancelled", report.Outcomes.Single().Status);
            Assert.Equal("JogExit", link.WriteCalls.Last());
        }
    }

    /// 起一个真正在跑的轮询器，再交给 runner —— 快照新鲜度是安全条件的一部分，
    /// 所以这里不用手工塞 Latest 的捷径。
    private static async Task<(FakeRobotLink link, StatePoller poller)> Start(
        FakeRobotLink link, double intervalMs = 20)
    {
        var poller = new StatePoller(link, TimeSpan.FromMilliseconds(intervalMs));
        poller.Start();
        await Task.Delay(120);
        return (link, poller);
    }

    private static Step[] Sequence() => new Step[]
    {
        new SetRate(10),
        new Enable(),
        new WaitEnabled(TimeSpan.FromSeconds(5)),
        new Hold(TimeSpan.FromMilliseconds(350)),
        new MoveJ(new JointTarget(new double[] { 9.02, -14.5, 30, 0, 90, -180 })),
    };

    [Fact]
    public async Task 干跑_一次写调用都不发出()
    {
        var link = new FakeRobotLink();
        var (l, poller) = await Start(link);
        using (poller)
        {
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink());          // AllowMotion 默认 false

            var report = await runner.RunAsync(Sequence());

            Assert.True(report.DryRun);
            Assert.Empty(l.WriteCalls);
            Assert.Equal(5, report.CountBy("dry-run"));
            Assert.True(l.ReadCount > 0);   // 只读轮询照常进行
        }
    }

    [Fact]
    public async Task 干跑时也照样预告安全违规()
    {
        var link = new FakeRobotLink();
        var (l, poller) = await Start(link);
        using (poller)
        {
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy()),   // 故意不给限位
                new MemorySink());

            var report = await runner.RunAsync(Sequence());
            Assert.Contains(report.Outcomes, o => o.Status == "dry-run" && o.Detail.Contains("软限位"));
        }
    }

    [Fact]
    public async Task 授权后按序执行并关闭点动通道()
    {
        var link = new FakeRobotLink();
        var (l, poller) = await Start(link);
        using (poller)
        {
            var sink = new MemorySink();
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                sink,
                new RunnerOptions { AllowMotion = true });

            var report = await runner.RunAsync(Sequence());

            Assert.False(report.Aborted, string.Join("; ", report.Outcomes.Select(o => $"{o.Step}: {o.Detail}")));
            Assert.Equal(5, report.CountBy("executed"));
            Assert.Equal("SetOverride(10)", l.WriteCalls[0]);
            Assert.Equal("ConfirmAllAlarms", l.WriteCalls[1]);
            Assert.Equal("ToggleMotorEnable", l.WriteCalls[2]);
            Assert.StartsWith("JogToJointAbsolute", l.WriteCalls[3]);
            Assert.Equal("JogExit", l.WriteCalls[^1]);
            Assert.True(l.State.MotorsEnabled);
            Assert.Equal(9.02, l.LastTarget!.Deg[0], 3);
        }
    }

    [Fact]
    public async Task 没有软限位_走到MoveJ就中止且不发运动()
    {
        var link = new FakeRobotLink();
        var (l, poller) = await Start(link);
        using (poller)
        {
            var runner = new ExperimentRunner(l, poller, new SafetyGuard(new SafetyPolicy()),
                new MemorySink(), new RunnerOptions { AllowMotion = true });

            var report = await runner.RunAsync(Sequence());

            Assert.True(report.Aborted);
            Assert.DoesNotContain(l.WriteCalls, c => c.StartsWith("JogToJointAbsolute"));
            var last = report.Outcomes.Last();
            Assert.Equal("rejected", last.Status);
            Assert.Contains("软限位", last.Detail);
        }
    }

    [Fact]
    public async Task 使能后不到300ms_运动被拦()
    {
        var link = new FakeRobotLink();
        var (l, poller) = await Start(link);
        using (poller)
        {
            // 同一条序列但去掉 Hold(350ms)，用来专门验证手册的死区要求。
            var steps = new Step[]
            {
                new SetRate(10), new Enable(),
                new MoveJ(new JointTarget(new double[] { 0, 0, 0, 0, 0, 0 })),
            };
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink(), new RunnerOptions { AllowMotion = true });

            var report = await runner.RunAsync(steps);

            Assert.True(report.Aborted);
            Assert.Contains("300ms", report.Outcomes.Last().Detail);
            Assert.DoesNotContain(l.WriteCalls, c => c.StartsWith("JogToJointAbsolute"));
        }
    }

    [Fact]
    public async Task 等不到到位_标记超时而不是无限等()
    {
        var link = new FakeRobotLink { MoveArrives = false };
        var (l, poller) = await Start(link);
        using (poller)
        {
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink(),
                new RunnerOptions
                {
                    AllowMotion = true,
                    MoveTimeout = TimeSpan.FromMilliseconds(400),
                    WaitPoll = TimeSpan.FromMilliseconds(50),
                });

            var report = await runner.RunAsync(new Step[]
            {
                new Enable(), new Hold(TimeSpan.FromMilliseconds(350)),
                new MoveJ(new JointTarget(new double[] { 50, 50, 50, 50, 50, 50 })),
            });

            Assert.True(report.Aborted);
            Assert.Equal("timeout", report.Outcomes.Last().Status);
            Assert.Contains("超时", report.Outcomes.Last().Detail);
            Assert.Equal(2, report.CountBy("executed"));   // 中止发生在失败这一步，不是整轮作废
        }
    }

    [Fact]
    public async Task 链路已断_直接拒绝运动()
    {
        var link = new FakeRobotLink(FakeRobotLink.Snap(enabled: true, health: LinkHealth.Disconnected));
        var (l, poller) = await Start(link);
        using (poller)
        {
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink(), new RunnerOptions { AllowMotion = true });

            var report = await runner.RunAsync(new Step[]
            {
                new MoveJ(new JointTarget(new double[] { 0, 0, 0, 0, 0, 0 })),
            });

            Assert.True(report.Aborted);
            Assert.DoesNotContain(l.WriteCalls, c => c.StartsWith("JogToJointAbsolute"));
        }
    }

    [Fact]
    public async Task 读状态抛异常_轮询降级_运动被拒()
    {
        var link = new FakeRobotLink { FailReads = 6 };
        var (l, poller) = await Start(link);
        using (poller)
        {
            Assert.True(poller.ConsecutiveFailures >= 1);
            var runner = new ExperimentRunner(l, poller,
                new SafetyGuard(new SafetyPolicy { JointLimitsDeg = FakeRobotLink.WideLimits() }),
                new MemorySink(), new RunnerOptions { AllowMotion = true });

            var report = await runner.RunAsync(new Step[]
            {
                new MoveJ(new JointTarget(new double[] { 0, 0, 0, 0, 0, 0 })),
            });
            Assert.True(report.Aborted);
        }
    }
}
