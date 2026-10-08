using System.Reflection;
using System.Text;
using StepRobot.Core;

namespace StepRobot.Adapter.Reflective;

/// 厂商 SDK 的反射适配层 —— 整个方案里**唯一**与 `Robot-SDK-CSharp.dll` 有具体符号耦合的地方。
///
/// 为什么走反射而不是 ProjectReference：真 DLL 现在还没拿到手，而厂商公开渠道没有它。
/// 走反射就能先拿桩件把上层逻辑全部跑通，真货到了只改这一个文件（或换成编译期引用版）。
///
/// 已知的不确定点全部集中在本文件的字段名/枚举名解析上，且都有兜底与日志：
///   · 结构体字段名（手册只给 C++ 布局，C# 侧未逐字确认）
///   · 单位（毫米/度只是从示例数值推断）
///   · getMotorEnableState 的返回值语义
public sealed class ReflectiveRobotLink : IRobotLink
{
    private readonly Assembly _asm;
    private readonly Type _clientType;
    private readonly object _client;
    private readonly Dictionary<string, MethodInfo?> _cache = new();
    private bool _connected;

    public const string DefaultTypeName = "StepRobotSDKCSharp.RobotComClientCSharp";

    public ReflectiveRobotLink(string dllPath, string? typeName = null)
    {
        if (!File.Exists(dllPath))
            throw new FileNotFoundException("找不到 SDK 程序集：" + dllPath, dllPath);

        try { _asm = Assembly.LoadFrom(dllPath); }
        catch (BadImageFormatException ex)
        {
            throw new RobotLinkException(
                $"加载 {dllPath} 失败：进程与 DLL 位数不匹配（SDK 库为 Release x86/x64 编译）。{ex.Message}", inner: ex);
        }

        _clientType = Resolve(dllPath, typeName);
        _client = Activator.CreateInstance(_clientType)
                  ?? throw new RobotLinkException($"{_clientType.FullName} 无法实例化（需要无参构造函数）");
    }

    private Type Resolve(string dllPath, string? typeName)
    {
        if (typeName is not null)
            return _asm.GetType(typeName, throwOnError: true)!;

        var byManualName = _asm.GetType(DefaultTypeName);
        if (byManualName is not null) return byManualName;

        Type[] types;
        try { types = _asm.GetTypes(); }
        catch (ReflectionTypeLoadException rtle) { types = rtle.Types.Where(t => t != null).ToArray()!; }

        return types.FirstOrDefault(t => t.GetMethod("init", BindingFlags.Public | BindingFlags.Instance) is not null)
            ?? throw new RobotLinkException($"{dllPath} 里找不到带 init(...) 的客户端类，请用 --type 指定完整类型名");
    }

    private MethodInfo? M(string name)
    {
        if (_cache.TryGetValue(name, out var hit)) return hit;
        hit = _clientType.GetMethod(name, BindingFlags.Public | BindingFlags.Instance, null, Type.EmptyTypes, null)
              ?? _clientType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
                  .FirstOrDefault(m => string.Equals(m.Name, name, StringComparison.OrdinalIgnoreCase));
        _cache[name] = hit;
        return hit;
    }

    private static object Coerce(ParameterInfo p, object value)
    {
        var t = p.ParameterType;
        if (t.IsAssignableFrom(value.GetType())) return value;
        if (t.IsEnum && value is string enumName)
            return Enum.Parse(t, enumName, ignoreCase: true);
        if (t.IsEnum && value is IConvertible)
            return Enum.ToObject(t, Convert.ToInt64(value));
        return Convert.ChangeType(value, Nullable.GetUnderlyingType(t) ?? t);
    }

    private int CallChecked(string name, params object?[] args)
    {
        var m = M(name) ?? throw new RobotLinkException($"接口 {name} 在此 SDK 版本中不存在");
        var ps = m.GetParameters();
        for (int i = 0; i < args.Length && i < ps.Length; i++)
            args[i] = args[i] is null ? null : Coerce(ps[i], args[i]!);

        object? raw;
        try { raw = m.Invoke(_client, args); }
        catch (TargetInvocationException tie) { throw new RobotLinkException($"{name}() 抛出", inner: Root(tie)); }

        if (raw is null) return 0;
        if (m.ReturnType == typeof(void)) return 0;
        return Convert.ToInt32(raw);
    }

    private static Exception? Root(Exception ex) =>
        ex is TargetInvocationException { InnerException: not null } tie ? Root(tie.InnerException!) : ex;

    public string SdkVersion
    {
        get
        {
            try { return (M("SDKVersion")?.Invoke(_client, null) as string) ?? "(无 SDKVersion 接口)"; }
            catch (Exception ex) { return "(读取失败: " + Root(ex)?.Message + ")"; }
        }
    }

    public bool Connected => _connected;

    public void Connect(string ip, int port)
    {
        var m = _clientType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
            .FirstOrDefault(x => x.Name.Equals("init", StringComparison.OrdinalIgnoreCase)
                              && x.GetParameters().Length == 2)
            ?? throw new RobotLinkException("找不到 init(ip, port)");

        var ps = m.GetParameters();
        object[] args = { Coerce(ps[0], ip), Coerce(ps[1], port) };
        int r;
        try { r = Convert.ToInt32(m.Invoke(_client, args) ?? 0); }
        catch (TargetInvocationException tie) { throw new RobotLinkException("init() 抛出", inner: Root(tie)); }

        // 0 正常 / -1 建立失败 / -2 数据收发异常 / -3 通讯异常（手册 2.1.3）
        if (r != 0)
            throw new RobotLinkException($"连接 {ip}:{port} 失败", r);

        _connected = true;
    }

    public void Close()
    {
        _connected = false;
        // 手册通篇没有 Disconnect/Close 接口，C++ 侧才有 close()。这里尽力而为。
        try { M("close")?.Invoke(_client, null); } catch { /* 没有就算了 */ }
    }

    public void Dispose() => Close();

    public RobotSnapshot ReadState()
    {
        if (!_connected) throw new RobotLinkException("尚未连接");

        var joints = ReadJoints();
        return new RobotSnapshot(
            At: DateTimeOffset.Now,
            JointDeg: joints,
            CartPos: ReadCart(),
            MotorsEnabled: ReadMotorEnable(),
            Mode: ReadEnum("getOperationMode", OperationMode.Unknown),
            OverridePercent: ReadDouble("getOverride"),
            ProjectName: ReadString("getProjectName"),
            ProgramName: ReadString("getProgramName"),
            ToolName: ReadString("getToolName"),
            CurrentLine: (int)ReadDouble("getCurrentLine"),
            DiWords: ReadDi(),
            LastMessage: ReadMessage(),
            Health: LinkHealth.Connected);
    }

    private object? Get(string name)
    {
        var m = M(name);
        if (m is null) return null;
        try { return m.Invoke(_client, null); }
        catch (TargetInvocationException tie) { throw new RobotLinkException($"{name}() 抛出", inner: Root(tie)); }
    }

    private double ReadDouble(string name) => Get(name) is { } v ? Convert.ToDouble(v) : 0;

    private string ReadString(string name)
    {
        var m = M(name);
        if (m is null) return "";
        var v = Get(name) as string;
        if (string.IsNullOrEmpty(v)) return v ?? "";
        return FixMojibake(v);
    }

    /// 0=未使能 1=已使能（手册 2.2.10）。负值按通讯异常处理，不当成"未使能"静默吞掉。
    private bool ReadMotorEnable()
    {
        var raw = Get("getMotorEnableState");
        if (raw is null) throw new RobotLinkException("没有 getMotorEnableState 接口");
        var v = Convert.ToInt32(raw);
        if (v < 0) throw new RobotLinkException("读取使能状态返回异常码", v);
        return v >= 1;
    }

    /// 按**名字**映射，不能按序号：Core 的 OperationMode 里 Unknown=0，
    /// 而 SDK 的 OPERATIONMODE 里 eManual=0 —— 序号转换会把"手动"悄悄读成"未知"。
    private T ReadEnum<T>(string getter, T fallback) where T : struct
    {
        var raw = Get(getter);
        if (raw is null) return fallback;

        var text = raw.ToString() ?? "";
        // Enum.TryParse accepts numeric strings too; vendor ordinal 0 is Manual, not Core.Unknown.
        if (!int.TryParse(text, out _) && Enum.TryParse<T>(text, true, out var exact)) return exact;
        if (text.Length > 1 && (text[0] == 'e' || text[0] == 'E')
            && Enum.TryParse<T>(text[1..], true, out var stripped)) return stripped;

        if (raw is IConvertible && int.TryParse(text, out var ordinal))
        {
            // 枚举以整数形式返回时按厂商头文件里的定义手工对齐（OPERATIONMODE: eManual=0…）
            var name = typeof(T).Name switch
            {
                nameof(OperationMode) => ordinal switch
                {
                    0 => "Manual", 1 => "ManualHigh", 2 => "Automatic", 3 => "AutomaticExternal", _ => null
                },
                _ => null
            };
            if (name is not null && Enum.TryParse<T>(name, out var byOrdinal)) return byOrdinal;
        }

        return fallback;   // 认不出来就是认不出来，绝不给一个"碰巧对上"的值
    }

    private static double[]? FirstDoubleArray(object? box)
    {
        if (box is null) return null;
        var t = box.GetType();
        if (t.IsArray && t.GetElementType() == typeof(double)) return (double[])box;
        foreach (var f in t.GetFields(BindingFlags.Public | BindingFlags.Instance))
            if (f.FieldType == typeof(double[]))
                return f.GetValue(box) as double[];
        return null;
    }

    private double[] ReadJoints()
    {
        // AXISPOS { JointsPos m_Joint; AuxPos m_AuxJoint }，而 JointsPos 内部是 double[] m_jointData
        var axis = Get("getAxisPos");
        if (axis is null) return Array.Empty<double>();

        foreach (var fieldName in new[] { "m_Joint", "m_joint", "m_Joints" })
        {
            var f = axis.GetType().GetField(fieldName, BindingFlags.Public | BindingFlags.Instance);
            if (f is null) continue;
            var arr = FirstDoubleArray(f.GetValue(axis));
            if (arr is { Length: > 0 }) return arr;
        }
        return FirstDoubleArray(axis) ?? Array.Empty<double>();
    }

    private double[]? ReadCart()
    {
        var cart = Get("getCartPosWorld");
        if (cart is null) return null;
        var arr = FirstDoubleArray(cart);
        if (arr is { Length: > 0 }) return arr;

        foreach (var f in cart.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance))
        {
            var nested = FirstDoubleArray(f.GetValue(cart));
            if (nested is { Length: > 0 }) return nested;
        }
        return null;
    }

    private ulong[]? ReadDi()
    {
        var di = Get("getRobotDI");
        if (di is null) return null;
        var field = di.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance)
            .FirstOrDefault(f => f.FieldType.IsArray);
        if (field?.GetValue(di) is not Array arr || arr.Length == 0) return null;
        var result = new ulong[arr.Length];
        for (int i = 0; i < arr.Length; i++) result[i] = Convert.ToUInt64(arr.GetValue(i));
        return result;
    }

    private string? ReadMessage()
    {
        var msg = Get("getMessageData");
        if (msg is null) return null;
        var text = msg.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance)
            .FirstOrDefault(f => f.FieldType == typeof(string))
            ?.GetValue(msg) as string;
        if (string.IsNullOrEmpty(text)) return null;
        var id = msg.GetType().GetFields(BindingFlags.Public | BindingFlags.Instance)
            .FirstOrDefault(f => f.Name.Contains("MessageID", StringComparison.OrdinalIgnoreCase))
            ?.GetValue(msg);
        return id is null ? FixMojibake(text) : $"{id}: {FixMojibake(text)}";
    }

    // ---- 写操作：全部集中在这里，Core 只会通过 IRobotLink 调到这些名字 ----

    public void ConfirmAllAlarms() => Check(CallChecked("AllAlarmConfirmCmd"), "AllAlarmConfirmCmd");
    public void JogExit() => Check(CallChecked("JogToPositionExitCmd"), "JogToPositionExitCmd");

    public void SetOverride(double percent)
    {
        var m = M("OverrideSetCmd") ?? throw new RobotLinkException("没有 OverrideSetCmd 接口");
        Check(CallChecked("OverrideSetCmd", percent), "OverrideSetCmd");
    }

    /// EnableMotorCmd 是**切换式**的：已使能时再调一次就是断使能。调用方必须知道这一点。
    public void ToggleMotorEnable() => Check(CallChecked("EnableMotorCmd"), "EnableMotorCmd");

    public void JogToJointAbsolute(JointTarget target, string tool, string refSys)
    {
        var m = _clientType.GetMethod("JogToRobotAxisposCmd", BindingFlags.Public | BindingFlags.Instance)
                ?? throw new RobotLinkException("没有 JogToRobotAxisposCmd 接口");
        var ps = m.GetParameters();
        if (ps.Length < 4) throw new RobotLinkException("JogToRobotAxisposCmd 参数个数与手册不符");

        // 第 1 参是 JogPositionType 枚举，PTP=0；第 2 参是位置结构体，需运行时构造并填 double[] 字段
        object typeArg = BuildFirstEnum(ps[0]);
        object posArg = BuildPosStruct(ps[1].ParameterType, target.Deg);

        Check(CallCheckedRaw(m, new object?[]
        {
            typeArg, posArg, Coerce(ps[2], tool), Coerce(ps[3], refSys)
        }), "JogToRobotAxisposCmd");
    }

    private static object BuildFirstEnum(ParameterInfo p)
    {
        var t = p.ParameterType;
        if (!t.IsEnum) return Activator.CreateInstance(t) ?? 0;
        foreach (var want in new[] { "ePTP", "PTP", "0" })
            if (Enum.TryParse(t, want, out var v)) return v;
        return Enum.GetValues(t).GetValue(0)!;
    }

    private static object BuildPosStruct(Type posType, double[] deg)
    {
        var box = Activator.CreateInstance(posType)
                  ?? throw new RobotLinkException($"无法构造 {posType.Name}");

        var arrField = posType.GetFields(BindingFlags.Public | BindingFlags.Instance)
            .FirstOrDefault(f => f.FieldType == typeof(double[]))
            ?? throw new RobotLinkException($"{posType.Name} 里找不到 double[] 字段，需按真 DLL 调整");

        var data = new double[Math.Max(deg.Length, 6)];
        Array.Copy(deg, data, deg.Length);
        arrField.SetValue(box, data);

        // C++ 侧 CartPos/RobotCartPos 带 m_Mode，默认 0；关节结构体没有该字段，找不到就跳过
        return box;
    }

    private int CallCheckedRaw(MethodInfo m, object?[] args)
    {
        object? raw;
        try { raw = m.Invoke(_client, args); }
        catch (TargetInvocationException tie) { throw new RobotLinkException($"{m.Name}() 抛出", inner: Root(tie)); }
        return raw is null || m.ReturnType == typeof(void) ? 0 : Convert.ToInt32(raw);
    }

    private static void Check(int code, string what)
    {
        if (code != 0)
            throw new RobotLinkException($"{what} 未成功", code);
    }

    /// 手册附录：中文字符串需转码否则乱码。.NET Framework 下 Encoding.Default 是 GBK，
    /// .NET 8 下已是 UTF8，所以这里两种方向都试，取含CJK字符的那个。
    private static string FixMojibake(string s)
    {
        if (s.Length == 0 || s.Any(c => c >= 0x4E00 && c <= 0x9FFF)) return s;
        try
        {
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            var re = Encoding.GetEncoding(936).GetString(Encoding.UTF8.GetBytes(s));
            if (re.Any(c => c >= 0x4E00 && c <= 0x9FFF)) return re;
        }
        catch { /* 编码页不可用就原样返回 */ }
        return s;
    }
}
