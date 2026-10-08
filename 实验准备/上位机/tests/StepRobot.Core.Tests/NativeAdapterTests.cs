using StepRobot.Adapter.Native;
using StepRobot.Core;
using Xunit;

namespace StepRobot.Core.Tests;

// ABI/dependency checks only. These tests never open a network connection.
public class NativeAdapterTests
{
    [Fact]
    public void 厂商库可加载且未连接不会返回伪造状态()
    {
        using var link = new NativeRobotLink();
        Assert.False(string.IsNullOrWhiteSpace(link.SdkVersion));
        Assert.False(StubGuard.LooksLikeStub(link.SdkVersion));
        Assert.False(link.Connected);
        Assert.Throws<RobotLinkException>(() => link.ReadState());
        link.Close();
        link.Close();
        Assert.False(link.Connected);
    }

    [Fact]
    public void 只读桥接拒绝所有写接口()
    {
        using var link = new NativeRobotLink();
        Assert.Throws<RobotLinkException>(() => link.ToggleMotorEnable());
        Assert.Throws<RobotLinkException>(() => link.SetOverride(10));
        Assert.Throws<RobotLinkException>(() => link.ConfirmAllAlarms());
        Assert.Throws<RobotLinkException>(() => link.JogToJointAbsolute(new JointTarget(new double[6]), "Default", "WORLD"));
        Assert.Throws<RobotLinkException>(() => link.JogExit());
    }
}
