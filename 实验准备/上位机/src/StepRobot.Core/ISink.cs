using System.Text.Json;

namespace StepRobot.Core;

public interface ISink
{
    void Write(string evt, object payload);
}

/// 每行一个 JSON 对象的实验记录。选 JSONL 而不是 CSV：一条记录里既有标量也有
/// 关节数组，CSV 表达不了；后续用 pandas 一次 read_json(lines=True) 就能读。
public sealed class JsonlSink : ISink, IDisposable
{
    private readonly StreamWriter _writer;
    private readonly object _gate = new();
    private readonly ISink? _tee;

    private static readonly JsonSerializerOptions Opts = new()
    {
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
    };

    public JsonlSink(string path, ISink? tee = null)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        _writer = new StreamWriter(path, append: true) { AutoFlush = true };
        _tee = tee;
    }

    public static JsonlSink Null { get; } = new JsonlSink(
        Path.Combine(Path.GetTempPath(), "steprobot-null-" + Guid.NewGuid().ToString("N") + ".jsonl"));

    public void Write(string evt, object payload)
    {
        var line = JsonSerializer.Serialize(
            new { ts = DateTimeOffset.Now, evt, payload }, Opts);
        lock (_gate) _writer.WriteLine(line);
        _tee?.Write(evt, payload);
    }

    public void Dispose()
    {
        lock (_gate) _writer.Dispose();
    }
}

/// 控制台输出，用于现场盯着看。
public sealed class ConsoleSink : ISink
{
    public void Write(string evt, object payload)
        => Console.WriteLine($"  · {evt,-16} {payload}");
}

/// 测试用：把事件留在内存里断言。
public sealed class MemorySink : ISink
{
    private readonly List<(string evt, object payload)> _lines = new();
    public IReadOnlyList<(string evt, object payload)> Lines => _lines;
    public int CountOf(string evt) => _lines.Count(l => l.evt == evt);
    public void Write(string evt, object payload) => _lines.Add((evt, payload));
}
