using System.Globalization;
using System.Net;

namespace StepRobot.App;

public sealed class AppOptions
{
    public string? Dll { get; private set; }
    public string Ip { get; private set; } = "127.0.0.1";
    public int Port { get; private set; } = 30312;
    public string? TypeName { get; private set; }
    public string OutPath { get; private set; } = $"logs/run-{DateTime.Now:yyyyMMdd-HHmmss-fff}-{Guid.NewGuid():N}.jsonl";
    public int Seconds { get; private set; } = 3;
    public int TelemetryMs { get; private set; } = 200;
    public bool Demo { get; private set; }
    public bool Diagnose { get; private set; }
    public bool CaptureOnly { get; private set; }
    public bool AllowMotion { get; private set; }
    public bool AllowStub { get; private set; }
    public bool Verbose { get; private set; }
    public bool Help { get; private set; }
    public double[]? Joints { get; private set; }
    public (double Min, double Max)[]? Limits { get; private set; }
    public bool Native => !Demo && Dll is null;

    public static AppOptions Parse(string[] args)
    {
        var opt = new AppOptions();
        var seen = new HashSet<string>();
        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            if (!seen.Add(arg)) throw new ArgumentException($"参数重复：{arg}");
            string Next()
            {
                if (++i >= args.Length || args[i].StartsWith("--")) throw new ArgumentException($"{arg} 缺少参数值");
                return args[i];
            }
            switch (arg)
            {
                case "--help": case "-h": opt.Help = true; break;
                case "--demo": opt.Demo = true; break;
                case "--diagnose": opt.Diagnose = true; break;
                case "--capture-only": opt.CaptureOnly = true; break;
                case "--allow-motion": opt.AllowMotion = true; break;
                case "--allow-stub": opt.AllowStub = true; break;
                case "--verbose": opt.Verbose = true; break;
                case "--dll": opt.Dll = Path.GetFullPath(Next()); break;
                case "--ip": opt.Ip = Next(); break;
                case "--port": opt.Port = Integer(Next(), 1, 65535, arg); break;
                case "--type": opt.TypeName = Next(); break;
                case "--out": opt.OutPath = Path.GetFullPath(Next()); break;
                case "--seconds": opt.Seconds = Integer(Next(), 1, 86400, arg); break;
                case "--telemetry-ms": opt.TelemetryMs = Integer(Next(), 50, 60000, arg); break;
                case "--joints": opt.Joints = Vector(Next(), 6, arg); break;
                case "--limits":
                    var rows = Next().Split(';');
                    if (rows.Length != 6) throw new ArgumentException("--limits 需要六组 min,max，以分号分隔");
                    opt.Limits = rows.Select(row =>
                    {
                        var pair = Vector(row, 2, "--limits");
                        if (pair[0] >= pair[1]) throw new ArgumentException("限位下界必须小于上界");
                        return (pair[0], pair[1]);
                    }).ToArray();
                    break;
                default: throw new ArgumentException($"未知参数 {arg}，使用 --help 查看用法");
            }
        }
        if (opt.Help) return opt;
        if (!IPAddress.TryParse(opt.Ip, out var address) || address.AddressFamily != System.Net.Sockets.AddressFamily.InterNetwork)
            throw new ArgumentException("--ip 需要 IPv4 地址，例如 127.0.0.1");
        if (opt.Demo && opt.Dll is not null) throw new ArgumentException("--demo 与 --dll 不能同时使用");
        if (opt.Native && opt.AllowMotion) throw new ArgumentException("默认 C++ 桥接仅支持只读，不能使用 --allow-motion");
        if (opt.CaptureOnly && opt.AllowMotion) throw new ArgumentException("--capture-only 与 --allow-motion 不能同时使用");
        if (opt.AllowMotion && (opt.Limits is null || opt.Joints is null))
            throw new ArgumentException("运动模式必须明确提供 --limits 六轴限位和 --joints 六轴目标，不自动猜测目标");
        if (opt.Joints is not null && opt.Limits is not null)
            for (int i = 0; i < 6; i++)
                if (opt.Joints[i] < opt.Limits[i].Min || opt.Joints[i] > opt.Limits[i].Max)
                    throw new ArgumentException($"A{i + 1} 目标超出所给限位（尚未连接或发出命令）");
        return opt;
    }

    private static int Integer(string raw, int min, int max, string name)
    {
        if (!int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var v) || v < min || v > max)
            throw new ArgumentException($"{name} 必须为 {min}～{max} 的整数");
        return v;
    }
    private static double[] Vector(string raw, int count, string name)
    {
        var parts = raw.Split(',');
        if (parts.Length != count) throw new ArgumentException($"{name} 需要 {count} 个数值");
        return parts.Select(p => double.TryParse(p, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) && double.IsFinite(v)
            ? v : throw new ArgumentException($"{name} 包含无效数值：{p}")).ToArray();
    }

    public const string Usage = """
STEP Robot Host | SR175/3200 | 金属 3D 打印实验准备

  StepRobot.App.exe --demo                         软件测试桩件演示，无网络连接
  StepRobot.App.exe --diagnose                     本机 C++ 桥接加载和 SDK 版本检查
  StepRobot.App.exe --ip 127.0.0.1 --seconds 10     从虚拟控制器只读采集
  StepRobot.App.exe --dll <厂商C# DLL> --capture-only --ip <IP>

  --out <文件>          JSONL 日志（默认每次使用独立文件）
  --telemetry-ms <毫秒>  遥测落盘间隔，默认 200，最小 50
  --seconds <秒>        初始采集时间，默认 3
  --verbose             显示步骤日志
  --type <完整类名>      厂商 C# 客户端类型（可选）

  原有 C# DLL 运动序列保留为显式模式：
  --allow-motion --limits "min,max;...六组" --joints "q1,q2,q3,q4,q5,q6"
  测试桩件执行还需 --allow-stub；桩件不模拟真实机械臂或打印过程。
  C++ 桥接没有运动/IO 写接口。默认地址为本机，不会自动连接真机。
  退出码：0 成功；1 采集/步骤失败；2 参数/依赖错误；3 连接失败；5 桩件拦截；130 取消。
""";
}
