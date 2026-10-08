namespace StepRobot.Core;

/// 厂商 SDK 的唯一抽象边界。
/// 换实现（反射适配层 → 真 DLL 编译期引用 → 单元测试替身）不需要改动 Core 的任何业务逻辑。
/// 所有方法均可能抛 RobotLinkException。
public interface IRobotLink : IDisposable
{
    string SdkVersion { get; }
    void Connect(string ip, int port);

    /// SDK 本身没有断开接口，实现只能尽力而为；调用后连接仍可能被服务端保留。
    void Close();

    bool Connected { get; }

    RobotSnapshot ReadState();
    void ConfirmAllAlarms();
    void SetOverride(double percent);

    /// 危险：使能是切换式的（同一接口上/下使能），且手册要求与后续动作间隔 ≥300ms。
    void ToggleMotorEnable();

    void JogToJointAbsolute(JointTarget target, string tool, string refSys);
    void JogExit();
}

public sealed class RobotLinkException : Exception
{
    public int? Code { get; }

    public RobotLinkException(string message, int? code = null, Exception? inner = null)
        : base(code is null ? message : $"{message}（返回码 {code}）", inner)
        => Code = code;
}
