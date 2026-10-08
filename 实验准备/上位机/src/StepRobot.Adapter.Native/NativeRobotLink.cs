using System.Runtime.InteropServices;
using System.Text;
using StepRobot.Core;

namespace StepRobot.Adapter.Native;

/// C# -> C ABI -> local vendor C++ SDK. This adapter intentionally exposes reads only.
public sealed class NativeRobotLink : IRobotLink
{
    private readonly object _gate = new();
    private IntPtr _handle;

    public NativeRobotLink()
    {
        if (!Environment.Is64BitProcess) throw new RobotLinkException("本机 C++ SDK 需要 x64 进程");
        try
        {
            if (Native.step_abi_version() != 1) throw new RobotLinkException("原生桥接版本不匹配，请重新构建");
            Check(Native.step_create(out _handle), "创建 SDK 客户端");
        }
        catch (DllNotFoundException ex)
        { throw new RobotLinkException("找不到 StepRobot.Native.dll 或 VC++ 运行库；请运行 build.ps1", inner: ex); }
    }

    public string SdkVersion { get { lock (_gate) return Text(0); } }
    public bool Connected { get { lock (_gate) return _handle != IntPtr.Zero && Native.step_connected(_handle) == 1; } }
    public int ConnectionStatus { get { lock (_gate) { RequireHandle(); return Native.step_connection_status(_handle); } } }

    public void Connect(string ip, int port)
    {
        if (port is < 1 or > 65535) throw new ArgumentOutOfRangeException(nameof(port));
        lock (_gate) { RequireHandle(); Check(Native.step_connect(_handle, ip, (ushort)port), $"连接 {ip}:{port}"); }
    }

    public RobotSnapshot ReadState()
    {
        lock (_gate)
        {
            RequireHandle();
            var data = new double[13];
            var flags = new int[4];
            Check(Native.step_read(_handle, data, data.Length, flags, flags.Length), "读取状态");
            if (data.Any(v => !double.IsFinite(v)) || flags[0] is not (0 or 1))
                throw new RobotLinkException("控制器返回无效数值或使能状态");
            var mode = flags[1] switch
            {
                0 => OperationMode.Manual, 1 => OperationMode.ManualHigh,
                2 => OperationMode.Automatic, 3 => OperationMode.AutomaticExternal,
                _ => OperationMode.Unknown
            };
            var project = Text(1); var program = Text(2); var tool = Text(3);
            var message = Text(4);
            return new RobotSnapshot(DateTimeOffset.Now, data[..6], data[6..12], flags[0] == 1,
                mode, data[12], project, program, tool, flags[2], null,
                flags[3] == 0 && message.Length == 0 ? null : $"{flags[3]}: {message}", LinkHealth.Connected);
        }
    }

    private string Text(int field)
    {
        RequireHandle();
        var buffer = new byte[1024];
        Check(Native.step_text(_handle, field, buffer, buffer.Length), "读取 SDK 文本");
        int end = Array.IndexOf(buffer, (byte)0);
        return Encoding.UTF8.GetString(buffer, 0, end < 0 ? buffer.Length : end);
    }
    private void RequireHandle() { if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(NativeRobotLink)); }
    private static void Check(int code, string operation)
    { if (code != 0) throw new RobotLinkException(operation + "失败", code); }
    private static void ReadOnly() => throw new RobotLinkException("C++ 桥接当前只提供只读采集，不提供运动或 IO 写入");
    public void ConfirmAllAlarms() => ReadOnly();
    public void SetOverride(double percent) => ReadOnly();
    public void ToggleMotorEnable() => ReadOnly();
    public void JogToJointAbsolute(JointTarget target, string tool, string refSys) => ReadOnly();
    public void JogExit() => ReadOnly();
    public void Close()
    {
        lock (_gate)
        {
            if (_handle == IntPtr.Zero) return;
            Native.step_destroy(_handle);
            _handle = IntPtr.Zero;
        }
    }
    public void Dispose() => Close();

    private static class Native
    {
        private const string Dll = "StepRobot.Native.dll";
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_abi_version();
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_create(out IntPtr handle);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern void step_destroy(IntPtr handle);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_connected(IntPtr handle);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_connection_status(IntPtr handle);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_connect(IntPtr handle, [MarshalAs(UnmanagedType.LPUTF8Str)] string ip, ushort port);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_read(IntPtr handle, [Out] double[] values, int count, [Out] int[] flags, int flagCount);
        [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] internal static extern int step_text(IntPtr handle, int field, [Out] byte[] buffer, int capacity);
    }
}
