// STEP 机器人 SDK 链路自检程序（只读 · 不使能 · 不动）
//
// 用途：阶段 1「现场链路联调」第 8 步。连接控制器 SDK 端口，读取一次全部只读状态，
//       然后按周期轮询若干分钟，验证链路稳定性并打印状态变化。
//
// 安全设计（重要）：
//   本程序**不在编译期引用** Robot-SDK-CSharp.dll，而是在运行时反射加载，且只调用
//   「名称以 get 开头 + 无参数 + 非 void 返回」的接口，再用一份窄排除表滤掉
//   Cmd / Modify / Set / Enforce / Simulation / Load / Kill / Send 等动作词。
//   本 SDK 中所有会改变机器人状态的方法要么带参数（运动目标、端口、变量名），
//   要么以 Cmd 结尾，因此这道闸是可靠的：EnableMotorCmd、SetModeCmd、JogTo*、
//   AllAlarmConfirmCmd、SDKSimulateProgramLoad 全部落在闸外。
//   启动时会打印"检测到但已拒绝调用"的接口清单，便于人工复核。
//
//   排除表里**不能**放 Enable / Jog / Run / Stop / Alarm，否则会误杀
//   getMotorEnableState()、getJogRefSys()、getProgramState() 等纯状态读取。
//
// 两种用法：
//   dotnet run -- --dump     离线导出 DLL 真实接口签名，不连网络、不调 init
//   dotnet run -- --ip ...   到现场验证链路并只读轮询
//
// 退出码：0 链路稳定 / 1 有调用异常 / 2 缺 DLL 或找不到类型 / 3 网络不通 / 4 init 失败

using System.Net.Sockets;
using System.Reflection;
using System.Text;

var ip = "192.168.39.220";
var port = 30312;
var seconds = 120;
var intervalMs = 200;
var dllPath = "";

for (int i = 0; i < args.Length - 1; i++)
{
    switch (args[i])
    {
        case "--ip": ip = args[++i]; break;
        case "--port": port = int.Parse(args[++i]); break;
        case "--seconds": seconds = int.Parse(args[++i]); break;
        case "--interval": intervalMs = int.Parse(args[++i]); break;
        case "--dll": dllPath = args[++i]; break;
    }
}

Console.OutputEncoding = Encoding.UTF8;
try { Encoding.RegisterProvider(CodePagesEncodingProvider.Instance); } catch { /* 平台不支持则跳过转码 */ }

Log("STEP 机器人 SDK 链路自检（只读模式）");
Log($"目标 {ip}:{port} ｜ 轮询 {seconds}s ｜ 周期 {intervalMs}ms");
Log("本程序不会调用任何使能、点动、运动或程序控制接口。");
Console.WriteLine();

// ---------- 步骤 1：加载 SDK 程序集 ----------
// 放在最前：本地文件检查比 3 秒网络超时更快失败，且让 --dump 模式完全离线可用。
Log("[1/4] 加载 Robot-SDK-CSharp.dll");
var candidates = new List<string>();
if (!string.IsNullOrWhiteSpace(dllPath)) candidates.Add(dllPath);
var asmDir = Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location) ?? ".";
candidates.Add(Path.Combine(AppContext.BaseDirectory, "libs", "Robot-SDK-CSharp.dll"));
candidates.Add(Path.Combine(asmDir, "libs", "Robot-SDK-CSharp.dll"));
candidates.Add(Path.Combine(asmDir, "Robot-SDK-CSharp.dll"));
candidates.Add(Path.Combine(Directory.GetCurrentDirectory(), "libs", "Robot-SDK-CSharp.dll"));

string? found = candidates.FirstOrDefault(File.Exists);
if (found == null)
{
    Fail("未找到 Robot-SDK-CSharp.dll。\n" +
         "      该文件本地没有、厂商公开下载页（stepelectric.com/Download/56.html）上也只有功能类 PDF，" +
         "需向厂商索取：robot@steprobots.com / 021-69926073。请索要配套 SDK 文档 V2.3 的版本，" +
         "以及 C# 示例工程 testSDKCSharp。\n" +
         "      拿到后放入本目录 libs\\ 下重跑即可（本工程无需重新编译，反射加载）。");
    return 2;
}
Assembly asm;
try { asm = Assembly.LoadFrom(found); }
catch (BadImageFormatException ex)
{
    Fail($"加载失败：{ex.Message}\n" +
         "      典型原因是 DLL 与进程位数不匹配。SDK 库以 Release x86/x64 编译，" +
         "请确认本工程 RuntimeIdentifier/Platform 为 x64（本仓库 csproj 已设）。");
    return 2;
}
Log($"      已加载 {found}");
Log($"      程序集 {asm.GetName().Name} {asm.GetName().Version}");

Type? clientType = asm.GetType("StepRobotSDKCSharp.RobotComClientCSharp");
if (clientType == null)
{
    // GetTypes() 在厂商 DLL 依赖解析不全时会抛 ReflectionTypeLoadException，
    // 这里退回到"能加载多少算多少"的类型集合，保证探测不崩。
    Type[] allTypes;
    try { allTypes = asm.GetTypes(); }
    catch (ReflectionTypeLoadException rtle)
    {
        allTypes = rtle.Types.Where(t => t != null).ToArray()!;
        Warn($"部分类型无法解析，已按 {allTypes.Length} 个可用类型继续探测。");
    }
    clientType = allTypes
        .FirstOrDefault(t => t.GetMethod("init", BindingFlags.Public | BindingFlags.Instance) != null);
    if (clientType == null)
    {
        Fail("程序集里找不到带 init(...) 的客户端类；厂商可能改了命名空间。请运行本目录的 --dump 模式人工确认类型名。");
        return 2;
    }
    Warn($"手册中的 StepRobotSDKCSharp.RobotComClientCSharp 未找到，改用探测到的类型 {clientType.FullName}。");
}
object client = Activator.CreateInstance(clientType)!;
Log($"      客户端实例 {clientType.FullName}");
Console.WriteLine();

// --dump：只导出真实接口签名后就退出，不调用 init，也不碰任何命令接口。
// 用途：拿到 DLL 后在办公室里就能核对签名，无需到现场、无需机器人上电。
if (args.Contains("--dump"))
{
    Log("接口清单模式（--dump）：以下签名可直接对照《SDK使用说明 C# V2.3》");
    Console.WriteLine();
    foreach (var m in clientType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
             .Where(m => m.DeclaringType != typeof(object))
             .OrderBy(m => m.Name, StringComparer.Ordinal))
    {
        var ps = string.Join(", ", m.GetParameters().Select(p => $"{p.ParameterType.Name} {p.Name}"));
        var readOnlyGetter = m.Name.StartsWith("get", StringComparison.Ordinal)
                          && m.GetParameters().Length == 0
                          && m.ReturnType != typeof(void);
        var tag = (readOnlyGetter || !IsWriteApi(m.Name)) ? "" : "  <= 命令/写操作，本工具永不调用";
        Console.WriteLine($"  {(readOnlyGetter ? "读" : "  ")} {m.ReturnType.Name,-15} {m.Name}({ps}){tag}");
    }
    return 0;
}

static bool IsWriteApi(string name) => new[]
{
    "Enable","Set","Jog","Cmd","Modify","Send","Load","Kill","Run","Stop","Reset","Clear",
    "Enforce","Simulation","Alarm","Delete","Abort","Start","Exit","Weld","Request"
}.Any(f => name.Contains(f, StringComparison.OrdinalIgnoreCase));

// ---------- 步骤 2：先验网络，把"网络不通"和"SDK 故障"区分开 ----------
Log("[2/4] 端口连通性预检");
using (var tcp = new TcpClient())
{
    var connectTask = tcp.ConnectAsync(ip, port);
    var finished = await Task.WhenAny(connectTask, Task.Delay(3000));
    if (finished != connectTask)
    {
        Fail($"3 秒内无法连上 {ip}:{port}。请依次检查：PC 是否在 {ip[..ip.LastIndexOf('.')]}.x 网段、" +
             "网线是否插在控制器调试网口、控制器是否上电、PC 防火墙是否放行、" +
             "以及 30312 是否被 SPad 示教器占用（该端口同时只允许一个客户端）。");
        return 3;
    }
    if (!tcp.Connected)
    {
        Fail($"连接 {ip}:{port} 失败（TCP 握手未建立）。若示教器已连接控制器，请先在示教器上断开。");
        return 3;
    }
}
Log("      TCP 可达。");
Log("      注意：预检连接已立即关闭；SDK 服务端可能因这次短连接而占用会话，" +
    "若下一步 init 返回 -1/-2，等 2 秒重试一次。");
Console.WriteLine();

// ---------- 步骤 3：init ----------
Log("[3/4] 建立 SDK 会话 client.init(ip, port)");
int result = 0;
var initMethod = FindOverload(clientType, "init", typeof(string), typeof(int))
              ?? FindOverload(clientType, "init", typeof(string), typeof(ushort))
              ?? FindOverload(clientType, "init", typeof(string), typeof(uint))
              ?? throw new InvalidOperationException("init(string, port) 未找到，实际签名可能不同。");
object portArg = initMethod.GetParameters()[1].ParameterType == typeof(string)
    ? port.ToString()
    : Convert.ChangeType(port, initMethod.GetParameters()[1].ParameterType);

for (int attempt = 1; attempt <= 2; attempt++)
{
    try { result = Convert.ToInt32(initMethod.Invoke(client, new[] { ip, portArg })); }
    catch (TargetInvocationException tie) { result = -99; Warn($"init 抛异常：{tie.InnerException?.Message}"); }
    if (result == 0) break;
    if (attempt < 2) { Warn($"init 返回 {result}（{Meaning(result)}），2 秒后重试"); Thread.Sleep(2000); }
}

if (result != 0)
{
    Fail($"init 最终返回 {result}：{Meaning(result)}\n" +
         "      -1 建立失败 → 多为示教器占用 30312 或网段不对；-2 数据收发异常 → 控制器固件版本低于 SDK 要求；" +
         "-3 通讯异常 → 中途掉线。SDK 无 Disconnect 接口，掉线只能重新 init。");
    return 4;
}
Log("      init 成功。");

string? ver = SafeInvoke(clientType, client, "SDKVersion") as string;
Log($"      SDK 版本：{ver ?? "(无 SDKVersion 接口)"}");
Console.WriteLine();

// ---------- 步骤 4：只读快照 + 轮询 ----------
Log("[4/4] 只读状态采集");

// 安全依据：本 SDK 里所有会改变机器人状态的方法要么带参数（运动目标、端口号、变量名），
// 要么以 Cmd 结尾（EnableMotorCmd / AllAlarmConfirmCmd / SDKSimulateProgramLoad /
// JogToPositionExitCmd）。因此「名称以 get 开头 + 无参数 + 非 void」本身就是一道可靠的闸。
//
// getterExclusions 只收录「绝不会出现在纯状态读取名里的动作动词」。
// 注意不能收录 Enable / Jog / Run / Stop / Alarm —— 否则会误杀
// getMotorEnableState()、getJogRefSys()、getProgramState()、getMessageData()
// 这些阶段 1 最需要读的接口（桩件冒烟测试实测过这个坑）。
var getterExclusions = new[]
{
    "Cmd","Modify","Set","Enforce","Simulation","Delete","Clear","Send","Write",
    "Reset","Abort","Load","Kill","Store","Save","Open","Close"
};

// 仅用于打印"检测到但已拒绝调用"清单与 --dump 标注，信息性用途。
var writeVerbs = new[]
{
    "Enable","Set","Jog","Cmd","Modify","Send","Load","Kill","Run","Stop","Reset","Clear",
    "Enforce","Simulation","Alarm","Delete","Abort","Start","Exit","Weld","Request",
    "Store","Save","Open","Close"
};

var safe = clientType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
    .Where(m => m.Name.StartsWith("get", StringComparison.Ordinal))
    .Where(m => m.GetParameters().Length == 0)
    .Where(m => m.ReturnType != typeof(void))
    .Where(m => !getterExclusions.Any(f => m.Name.Contains(f, StringComparison.OrdinalIgnoreCase)))
    .OrderBy(m => m.Name, StringComparer.Ordinal)
    .ToList();

var refused = clientType.GetMethods(BindingFlags.Public | BindingFlags.Instance)
    .Where(m => m.DeclaringType != typeof(object))
    .Where(m => !safe.Contains(m))
    .Where(m => writeVerbs.Any(f => m.Name.Contains(f, StringComparison.OrdinalIgnoreCase)))
    .Select(m => m.Name)
    .Distinct()
    .OrderBy(n => n, StringComparer.Ordinal)
    .ToList();

Log($"      白名单内只读接口 {safe.Count} 个，将全部调用一次并持续轮询。");
Log($"      检测到但**已拒绝调用**的接口 {refused.Count} 个：");
Console.WriteLine("        " + string.Join(", ", refused));
Console.WriteLine();

var lastValues = new Dictionary<string, string>();
int loops = 0, errors = 0;
var sw = System.Diagnostics.Stopwatch.StartNew();

while (sw.Elapsed.TotalSeconds < seconds)
{
    loops++;
    foreach (var m in safe)
    {
        object? val;
        try { val = m.Invoke(client, Array.Empty<object>()); }
        catch (Exception ex)
        {
            errors++;
            Warn($"调用 {m.Name}() 失败：{Root(ex)?.Message}");
            continue;
        }
        var text = Render(val);
        if (loops == 1)
        {
            Console.WriteLine($"  {m.Name,-28} : {text}");
            lastValues[m.Name] = text;
        }
        else if (lastValues.TryGetValue(m.Name, out var prev) && prev != text)
        {
            Console.WriteLine($"  [{sw.Elapsed.TotalSeconds,6:F1}s] {m.Name,-26} {prev}  ->  {text}");
            lastValues[m.Name] = text;
        }
    }

    if (loops == 1)
    {
        Console.WriteLine();
        Log($"      首轮快照完成（{sw.Elapsed.TotalMilliseconds:F0}ms）。以下为变化事件流，Ctrl+C 结束。");
        Console.WriteLine();
    }
    Thread.Sleep(intervalMs);
}

Console.WriteLine();
Log($"结束：{loops} 轮轮询，{errors} 次调用异常，持续 {sw.Elapsed.TotalSeconds:F0}s。");
if (errors == 0)
    Log("判定：链路稳定。可以进入阶段 2（低速使能与点动）—— 但那一步必须现场清场、速度 15%、并人工确认急停可用。");
else
    Log("判定：存在调用异常。请把上面的异常行截图给厂商核对接口签名与固件版本，不要贸然进入运动阶段。");
return errors == 0 ? 0 : 1;

// ---------- 辅助函数 ----------
static MethodInfo? FindOverload(Type t, string name, params Type[] paramTypes)
    => t.GetMethod(name, BindingFlags.Public | BindingFlags.Instance, null, paramTypes, null);

static object? SafeInvoke(Type t, object o, string name)
{
    var m = t.GetMethod(name, BindingFlags.Public | BindingFlags.Instance, null, Type.EmptyTypes, null);
    if (m == null || m.ReturnType == typeof(void)) return null;
    try { return m.Invoke(o, Array.Empty<object>()); } catch { return null; }
}

static Exception? Root(Exception ex) => ex is TargetInvocationException tie && tie.InnerException != null ? Root(tie.InnerException) : ex;

static string Meaning(int code) => code switch
{
    0 => "正常/执行中",
    -1 => "建立失败",
    -2 => "数据收发异常",
    -3 => "通讯断开",
    -99 => "调用抛异常",
    _ => "未知"
};

// 结构体字段用反射打印；字符串按手册要求尝试 GBK/UTF8 纠偏
static string Render(object? value, int depth = 0)
{
    if (value == null) return "null";
    var type = value.GetType();

    if (type.IsPrimitive || type == typeof(decimal) || type.IsEnum) return Convert.ToString(value) ?? "";
    if (type == typeof(string)) return FixText((string)value);
    if (value is Array arr)
    {
        if (arr.Rank > 1) return "[" + string.Join(",", Cast(arr).Select(x => Render(x, depth + 1))) + "]";
        var flat = Cast(arr).Select(x => Render(x, depth + 1)).ToList();
        return flat.Count <= 12
            ? "[" + string.Join(",", flat) + "]"
            : "[" + string.Join(",", flat.Take(12)) + $", …共{flat.Count}]";
    }
    if (type.IsValueType && depth < 2 && !type.IsPrimitive)
    {
        var parts = type.GetFields(BindingFlags.Public | BindingFlags.Instance)
            .Select(f => $"{f.Name}={Render(f.GetValue(value), depth + 1)}");
        var inner = string.Join(" ", parts);
        return string.IsNullOrWhiteSpace(inner) ? Convert.ToString(value) ?? "" : inner;
    }
    return Convert.ToString(value) ?? "";
}

static IEnumerable<object?> Cast(Array a)
{
    foreach (var x in a) yield return x;
}

// 手册 P.附录：中文消息须转码，否则乱码。C# 手册基于 .NET Framework（ANSI=GBK），
// .NET 7 下 Encoding.Default 已是 UTF8，故这里两种编码都试并取可打印度更高者。
static string FixText(string s)
{
    if (string.IsNullOrEmpty(s)) return "\"\"";
    if (s.Any(c => c > 0x2E7F && c < 0xA000)) return "\"" + s + "\""; // 已有中文，认为正常
    try
    {
        var gbk = Encoding.GetEncoding(936);
        var re = gbk.GetString(Encoding.UTF8.GetBytes(s));
        if (re.Any(c => c >= 0x4E00 && c <= 0x9FFF)) return "\"" + re + "\" (由 GBK 纠偏)";
    }
    catch { }
    return "\"" + s + "\"";
}

static void Log(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {msg}");
static void Warn(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] ! {msg}");
static void Fail(string msg)
{
    Console.ForegroundColor = ConsoleColor.Red;
    Console.Error.WriteLine($"[{DateTime.Now:HH:mm:ss}] X {msg}");
    Console.ResetColor();
}
