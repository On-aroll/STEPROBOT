namespace StepRobot.Core;

/// 防呆：别让仿真桩件被当成真控制器使。
/// 桩件的 init 恒成功、状态恒健康、任何指令都返回 0 —— 拿它验证过"闸门有效"，
/// 也就会让人误以为真机验证过了。所以一旦要发写指令，先确认对面不是桩件。
public static class StubGuard
{
    public static bool LooksLikeStub(string? sdkVersion) =>
        !string.IsNullOrEmpty(sdkVersion)
        && (sdkVersion!.Contains("STUB", StringComparison.OrdinalIgnoreCase)
            || sdkVersion.Contains("FAKE", StringComparison.OrdinalIgnoreCase)
            || sdkVersion.Contains("MOCK", StringComparison.OrdinalIgnoreCase));

    /// 返回 null 表示放行，否则返回拒绝理由。
    public static string? Reject(bool allowMotion, string? sdkVersion, bool stubExplicitlyAllowed)
    {
        if (!allowMotion) return null;                        // 干跑不碰机器人，桩件随便用
        if (!LooksLikeStub(sdkVersion)) return null;
        if (stubExplicitlyAllowed) return "警告：已用 --allow-stub 强制对桩件下发写指令，结果不得作为真机依据。";
        return "拒绝：目标是仿真桩件（SDKVersion 含 STUB/FAKE/MOCK），它会对任何指令返回成功，" +
               "不能用来验证真机行为。确认无误可加 --allow-stub。";
    }
}
