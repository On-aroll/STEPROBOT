using StepRobot.Adapter.Reflective;
using StepRobot.Core;
using Xunit;

namespace StepRobot.Core.Tests;

/// 反射适配层 vs 桩件 DLL —— 这就是"适配层对着假 DLL 跑通"的那条证据链。
/// 桩件的方法名、类名、结构体字段名全部照抄《SDK使用说明 C# V2.3》，
/// 所以这里通过，意味着真 DLL 到位后需要改的只剩签名细节，而不是架构。
public class ReflectiveAdapterFixtureTests : IDisposable
{
    [Fact]
    public void 数字零按厂商定义解析为手动而非未知()
    {
        using var link = new ReflectiveRobotLink(typeof(NumericModeClient).Assembly.Location, typeof(NumericModeClient).FullName);
        link.Connect("127.0.0.1", 30312);
        Assert.Equal(OperationMode.Manual, link.ReadState().Mode);
    }
    private readonly string _fixture;
    private readonly ReflectiveRobotLink _link;

    public ReflectiveAdapterFixtureTests()
    {
        _fixture = LocateFixture();
        _link = new ReflectiveRobotLink(_fixture);
        _link.Connect("192.168.39.220", 30312);
    }

    private static string LocateFixture()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            var candidate = Path.Combine(dir.FullName, "test-fixtures", "Robot-SDK-CSharp.dll");
            if (File.Exists(candidate)) return candidate;
            dir = dir.Parent;
        }
        throw new FileNotFoundException(
            "没找到桩件夹具 test-fixtures\\Robot-SDK-CSharp.dll。" +
            "构建它：dotnet build ../../SdkLinkCheck/__test__/FakeSdk.csproj，" +
            "再把输出 dll 复制进 上位机/test-fixtures/（见 README）。");
    }

    public void Dispose() => _link.Dispose();

    [Fact]
    public void SDK版本可读()
        => Assert.Equal("V2.3-STUB", _link.SdkVersion);

    [Fact]
    public void 关节角字段解析正确()
    {
        var s = _link.ReadState();
        Assert.Equal(6, s.JointDeg.Length);
        Assert.Equal(7.02, s.JointDeg[0], 2);
        Assert.Equal(-180, s.JointDeg[5], 2);
    }

    [Fact]
    public void 使能状态解析正确_且按手册语义解读()
    {
        // 桩件初始 0 = 未使能。适配层必须把它读成 false，而不是当异常码吞掉。
        Assert.False(_link.ReadState().MotorsEnabled);
    }

    [Fact]
    public void 模式枚举按名字映射()
        => Assert.Equal(OperationMode.Manual, _link.ReadState().Mode);

    [Fact]
    public void 名称与倍率字段解析正确()
    {
        var s = _link.ReadState();
        Assert.Equal("project0", s.ProjectName);
        Assert.Equal("program0", s.ProgramName);
        Assert.Equal("Default", s.ToolName);
        Assert.Equal(15.0, s.OverridePercent, 1);
        Assert.Equal(LinkHealth.Connected, s.Health);
    }

    [Fact]
    public void 报警消息带编号回传()
        => Assert.Contains("4001", _link.ReadState().LastMessage);

    [Fact]
    public void 写倍率后能读回来()
    {
        _link.SetOverride(8.5);
        Assert.Equal(8.5, _link.ReadState().OverridePercent, 2);
    }

    [Fact]
    public void 使能是切换式的_调两次回到原状()
    {
        _link.ToggleMotorEnable();
        Assert.True(_link.ReadState().MotorsEnabled);
        _link.ToggleMotorEnable();
        Assert.False(_link.ReadState().MotorsEnabled);
    }

    [Fact]
    public void 点动结构体由反射构造并真的送达SDK()
    {
        var target = new JointTarget(new double[] { 12.5, -20.25, 33, 44, 55, -66 });
        _link.JogToJointAbsolute(target, "Default", "WORLD");

        var back = _link.ReadState().JointDeg;
        Assert.Equal(12.5, back[0], 2);
        Assert.Equal(-66, back[5], 2);
    }

    [Fact]
    public void 清报警与退出点动通道不抛()
    {
        _link.ConfirmAllAlarms();
        _link.JogExit();
    }

    [Fact]
    public void 未连接时拒绝读状态()
    {
        var fresh = new ReflectiveRobotLink(_fixture);
        fresh.Dispose();      // Dispose 里调 Close
        Assert.Throws<RobotLinkException>(() => fresh.ReadState());
    }

    [Fact]
    public void DLL路径写错时报错说人话()
    {
        var ex = Assert.Throws<FileNotFoundException>(
            () => new ReflectiveRobotLink(@"D:\不存在的\Robot-SDK-CSharp.dll"));
        Assert.Contains("找不到 SDK 程序集", ex.Message);
    }
}

// In-process fixture: no sockets and no writable SDK methods.
public sealed class NumericModeClient
{
    public int init(string ip, int port) => 0;
    public int getMotorEnableState() => 0;
    public int getOperationMode() => 0;
}
