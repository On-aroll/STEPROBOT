using StepRobot.Core;
using Xunit;

namespace StepRobot.Core.Tests;

public class StatePollerTests
{
    /// 等事件，不等时间。固定 sleep 在负载高的机器上会随机翻脸。
    private static async Task OrTimeout(Task task, int ms = 3000)
    {
        if (await Task.WhenAny(task, Task.Delay(ms)) != task)
            throw new TimeoutException("等待轮询事件超时");
        await task;
    }

    [Fact]
    public async Task 持续采样并推进计数()
    {
        var link = new FakeRobotLink();
        var fifth = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(20));
        poller.OnSample = _ => { if (poller.Samples >= 5) fifth.TrySetResult(); };
        poller.Start();

        await OrTimeout(fifth.Task);
        await poller.StopAsync();

        Assert.True(poller.Samples >= 5);
        Assert.Equal(LinkHealth.Connected, poller.Latest.Health);
        Assert.Equal(0, poller.ConsecutiveFailures);
    }

    [Fact]
    public async Task 读失败时快照降级并保留最后一次好数据()
    {
        var link = new FakeRobotLink();
        var failed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(20))
        {
            OnFailure = (_, _) => failed.TrySetResult()
        };
        poller.Start();
        await Task.Delay(120);                       // 先攒够成功样本
        var goodJoints = poller.Latest.JointDeg;
        Assert.NotEmpty(goodJoints);

        link.FailReads = int.MaxValue;               // 持续失败，别在断言前恢复
        await OrTimeout(failed.Task);

        Assert.Equal(LinkHealth.Degraded, poller.Latest.Health);
        Assert.Equal(goodJoints, poller.Latest.JointDeg);   // 降级 ≠ 丢数据
        Assert.True(poller.ConsecutiveFailures >= 1);
        await poller.StopAsync();
    }

    [Fact]
    public async Task 连续失败五次算彻底断开()
    {
        var link = new FakeRobotLink { FailReads = int.MaxValue };
        var fifth = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(10))
        {
            OnFailure = (_, n) => { if (n >= 5) fifth.TrySetResult(); }
        };
        poller.Start();

        await OrTimeout(fifth.Task);
        Assert.Equal(LinkHealth.Disconnected, poller.Latest.Health);
        Assert.Equal(0, poller.Samples);
        await poller.StopAsync();
    }

    [Fact]
    public async Task 从失败中恢复后计数归零且状态回到已连接()
    {
        var link = new FakeRobotLink { FailReads = 3 };
        var recovered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(20));
        poller.OnSample = _ => { if (poller.ConsecutiveFailures == 0) recovered.TrySetResult(); };
        poller.Start();

        await OrTimeout(recovered.Task);
        Assert.Equal(LinkHealth.Connected, poller.Latest.Health);
        Assert.Equal(0, poller.ConsecutiveFailures);
        await poller.StopAsync();
    }

    [Fact]
    public async Task Stop之后不再增长()
    {
        var link = new FakeRobotLink();
        var first = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(20));
        poller.OnSample = _ => first.TrySetResult();
        poller.Start();

        await OrTimeout(first.Task);
        await poller.StopAsync();
        var frozen = poller.Samples;
        await Task.Delay(150);
        Assert.Equal(frozen, poller.Samples);
    }
}
