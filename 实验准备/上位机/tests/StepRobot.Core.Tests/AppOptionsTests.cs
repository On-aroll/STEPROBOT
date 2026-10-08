using StepRobot.App;
using Xunit;

namespace StepRobot.Core.Tests;

public class AppOptionsTests
{
    [Fact]
    public void 默认只读本机且不允许运动()
    {
        var opt = AppOptions.Parse([]);
        Assert.True(opt.Native);
        Assert.Equal("127.0.0.1", opt.Ip);
        Assert.False(opt.AllowMotion);
    }

    [Theory]
    [InlineData("--seconds", "0")]
    [InlineData("--seconds", "abc")]
    [InlineData("--port", "65536")]
    [InlineData("--telemetry-ms", "-1")]
    [InlineData("--joints", "1,2,3")]
    [InlineData("--joints", "NaN,0,0,0,0,0")]
    [InlineData("--joints", "1e999,0,0,0,0,0")]
    [InlineData("--limits", "1,0;0,1;0,1;0,1;0,1;0,1")]
    [InlineData("--ip", "bad-address")]
    public void 无效参数在连接前拒绝(string name, string value)
        => Assert.Throws<ArgumentException>(() => AppOptions.Parse([name, value]));

    [Fact]
    public void 缺值不会自动改成示例目标()
        => Assert.Throws<ArgumentException>(() => AppOptions.Parse(["--joints"]));

    [Fact]
    public void 缺显式目标时不能上使能()
        => Assert.Throws<ArgumentException>(() => AppOptions.Parse(["--demo", "--allow-motion"]));

    [Fact]
    public void 原生桥接拒绝写入模式()
        => Assert.Throws<ArgumentException>(() => AppOptions.Parse(["--allow-motion"]));

    [Fact]
    public void 桩件与厂商DLL不能混选()
        => Assert.Throws<ArgumentException>(() => AppOptions.Parse(["--demo", "--dll", "other.dll"]));

    [Fact]
    public void 目标越界在连接前拒绝()
        => Assert.Throws<ArgumentException>(() => AppOptions.Parse(["--demo", "--allow-motion", "--allow-stub",
            "--joints", "200,0,0,0,0,0", "--limits", "-10,10;-10,10;-10,10;-10,10;-10,10;-10,10"]));

    [Fact]
    public void 明确的合法桩件运行参数可解析()
    {
        var opt = AppOptions.Parse(["--demo", "--allow-motion", "--allow-stub",
            "--joints", "1,2,3,4,5,6", "--limits", "-10,10;-10,10;-10,10;-10,10;-10,10;-10,10"]);
        Assert.True(opt.AllowMotion);
        Assert.Equal(6, opt.Joints!.Length);
        Assert.Equal(6, opt.Limits!.Length);
    }
}
