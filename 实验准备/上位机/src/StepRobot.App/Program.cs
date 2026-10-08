using System.Globalization;
using StepRobot.App;
using StepRobot.Adapter.Native;
using StepRobot.Adapter.Reflective;
using StepRobot.Core;

Console.OutputEncoding = System.Text.Encoding.UTF8;
AppOptions opt;
try { opt = AppOptions.Parse(args); }
catch (Exception ex) { Console.Error.WriteLine("参数错误：" + ex.Message); return 2; }
if (opt.Help || args.Length == 0) { Console.WriteLine(AppOptions.Usage); return 0; }

using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };
var ct = cancellation.Token;
IRobotLink link;
try
{
    link = opt.Native ? new NativeRobotLink() : new ReflectiveRobotLink(
        opt.Demo ? Path.Combine(AppContext.BaseDirectory, "test-fixtures", "Robot-SDK-CSharp.dll") : opt.Dll!, opt.TypeName);
}
catch (Exception ex) { Console.Error.WriteLine("适配层初始化失败：" + ex.Message); return 2; }

using (link)
{
    string backend = opt.Native ? "vendor-cpp-readonly" : opt.Demo ? "test-stub" : "vendor-csharp";
    Console.WriteLine($"STEP C# 上位机 | 后端 {backend} | .NET {Environment.Version}");
    Console.WriteLine($"SDK：{link.SdkVersion}");
    if (opt.Diagnose)
    {
        Console.WriteLine("本地 SDK 加载成功；未发起网络连接，未验证控制器接口。");
        return 0;
    }
    Console.WriteLine(opt.Demo ? "数据源：软件测试桩件（不连接 Studio，不代表六轴物理仿真）" : $"连接目标：{opt.Ip}:{opt.Port}");
    try { link.Connect(opt.Ip, opt.Port); }
    catch (Exception ex)
    {
        Console.Error.WriteLine("连接失败：" + ex.Message);
        Console.Error.WriteLine("请核对虚拟控制器是否显示运行中、绑定地址和 30312 端口；Studio 主窗口打开不等于控制器已启动。");
        return 3;
    }
    var verdict = StubGuard.Reject(opt.AllowMotion, link.SdkVersion, opt.AllowStub);
    if (link is NativeRobotLink native) Console.WriteLine($"C++ SDK ConnectStatus 原始返回值：{native.ConnectionStatus}");
    if (verdict is not null)
    {
        Console.WriteLine(verdict);
        if (verdict.StartsWith("拒绝")) return 5;
    }
    try
    {
        using var sink = new JsonlSink(opt.OutPath);
        sink.Write("session", new
        {
            backend, requestedRobotModel = "SR175/3200", dataKind = StubGuard.LooksLikeStub(link.SdkVersion) ? "software-test" : "controller-unvalidated",
            ip = opt.Demo ? null : opt.Ip, port = opt.Demo ? (int?)null : opt.Port,
            dryRun = !opt.AllowMotion, sdkVersion = link.SdkVersion, telemetryMs = opt.TelemetryMs,
            captureOnly = opt.Native || opt.CaptureOnly,
            limits = opt.Limits is null ? null : string.Join(";", opt.Limits.Select(l =>
                l.Min.ToString(CultureInfo.InvariantCulture) + "," + l.Max.ToString(CultureInfo.InvariantCulture))),
            commandedJoints = opt.Joints, started = DateTimeOffset.Now
        });
        using var poller = new StatePoller(link, TimeSpan.FromMilliseconds(50));
        long lastTelemetry = long.MinValue;
        poller.OnSample = s =>
        {
            long now = Environment.TickCount64;
            if (lastTelemetry != long.MinValue && now - lastTelemetry < opt.TelemetryMs) return;
            lastTelemetry = now;
            sink.Write("state", new { at = s.At, j = s.JointDeg, c = s.CartPos, en = s.MotorsEnabled,
                ovl = s.OverridePercent, mode = s.Mode.ToString(), line = s.CurrentLine, health = s.Health.ToString(),
                project = s.ProjectName, program = s.ProgramName, tool = s.ToolName, message = s.LastMessage });
        };
        poller.OnFailure = (ex, n) => sink.Write("poll.fail", new { n, error = ex.Message });
        poller.Start();
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(opt.Seconds), ct);
            var snap = poller.Latest;
            Console.WriteLine($"采集：{poller.Samples} 次；连续失败：{poller.ConsecutiveFailures}；状态：{snap.Health}");
            Console.WriteLine("关节角 [deg]：" + string.Join(", ", snap.JointDeg.Select(v => v.ToString("0.###", CultureInfo.InvariantCulture))));
            Console.WriteLine("WORLD XYZABC：" + (snap.CartPos is null ? "未提供" : string.Join(", ", snap.CartPos.Select(v => v.ToString("0.###", CultureInfo.InvariantCulture)))));
            Console.WriteLine($"使能：{snap.MotorsEnabled}；模式：{snap.Mode}；倍率：{snap.OverridePercent:0.#}%");
            Console.WriteLine($"工程：{snap.ProjectName}/{snap.ProgramName}；消息：{snap.LastMessage ?? "无"}");
            if (poller.Samples == 0 || snap.Health != LinkHealth.Connected || poller.ConsecutiveFailures > 0)
            {
                sink.Write("run.end", new { dryRun = true, aborted = true, failed = 1, reason = "无有效状态或链路异常" });
                return 1;
            }
            if (opt.Native || opt.CaptureOnly)
            {
                sink.Write("run.end", new { dryRun = true, aborted = false, failed = 0, captureOnly = true });
                return 0;
            }
            if (opt.AllowMotion)
            {
                Console.WriteLine("即将执行明确指定的运动序列；5 秒内可按 Ctrl+C 取消。");
                await Task.Delay(5000, ct);
            }
            var target = opt.Joints ?? snap.JointDeg;
            Step[] steps = [new SetRate(10), new Enable(), new WaitEnabled(TimeSpan.FromSeconds(5)),
                new Hold(TimeSpan.FromMilliseconds(400)), new MoveJ(new JointTarget(target))];
            var runner = new ExperimentRunner(link, poller, new SafetyGuard(new SafetyPolicy { JointLimitsDeg = opt.Limits }),
                opt.Verbose ? new FanOutSink(sink, new ConsoleSink()) : sink, new RunnerOptions { AllowMotion = opt.AllowMotion });
            var report = await runner.RunAsync(steps, ct);
            await Task.Delay(Math.Max(opt.TelemetryMs * 2, 200), ct);
            foreach (var result in report.Outcomes) Console.WriteLine($"[{result.Step}] {result.Status}: {result.Detail}");
            if (ct.IsCancellationRequested) return 130;
            return report.Aborted ? 1 : 0;
        }
        catch (OperationCanceledException)
        { sink.Write("run.end", new { dryRun = !opt.AllowMotion, aborted = true, cancelled = true }); return 130; }
        finally
        {
            await poller.StopAsync();
            Console.WriteLine("日志：" + Path.GetFullPath(opt.OutPath));
        }
    }
    catch (Exception ex) { Console.Error.WriteLine("运行失败：" + ex.Message); return 1; }
}

internal sealed class FanOutSink(params ISink[] sinks) : ISink
{
    public void Write(string evt, object payload) { foreach (var sink in sinks) sink.Write(evt, payload); }
}
