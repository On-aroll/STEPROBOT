namespace StepRobot.Core;

/// 状态轮询线程。SDK 没有任何回调/事件机制，"实时状态"只能自己周期性去取，
/// 所以这一层是必需件而不是优化项。
public sealed class StatePoller : IDisposable
{
    private readonly IRobotLink _link;
    private readonly TimeSpan _interval;
    private readonly CancellationTokenSource _cts = new();
    private Task? _loop;
    private readonly object _gate = new();
    private int _failures;
    private bool _disposed;

    public StatePoller(IRobotLink link, TimeSpan? interval = null)
    {
        _link = link;
        _interval = interval ?? TimeSpan.FromMilliseconds(50);
    }

    public RobotSnapshot Latest { get; private set; } = RobotSnapshot.Empty(LinkHealth.Disconnected);
    public int ConsecutiveFailures { get; private set; }
    public long Samples { get; private set; }
    public Action<RobotSnapshot>? OnSample { get; set; }
    public Action<Exception, int>? OnFailure { get; set; }

    public void Start()
    {
        if (_loop is not null) throw new InvalidOperationException("已启动");
        _loop = Task.Run(() => LoopAsync(_cts.Token));
    }

    public async Task StopAsync()
    {
        if (_disposed) return;
        _cts.Cancel();
        if (_loop is not null) await _loop.ConfigureAwait(false);
    }

    private async Task LoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            RobotSnapshot snap;
            try
            {
                snap = _link.ReadState();
                lock (_gate) { _failures = 0; }
                ConsecutiveFailures = 0;
            }
            catch (Exception ex)
            {
                int n;
                lock (_gate) { n = ++_failures; }
                ConsecutiveFailures = n;
                var health = _link.Connected && n < 5 ? LinkHealth.Degraded : LinkHealth.Disconnected;
                Latest = health == LinkHealth.Degraded && Latest.At != DateTimeOffset.MinValue
                    ? Latest with { Health = health }
                    : RobotSnapshot.Empty(health);
                OnFailure?.Invoke(ex, n);
                try { await Task.Delay(_interval, ct).ConfigureAwait(false); } catch { break; }
                continue;
            }

            Latest = snap;
            Samples++;
            OnSample?.Invoke(snap);

            try { await Task.Delay(_interval, ct).ConfigureAwait(false); }
            catch (OperationCanceledException) { break; }
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        try { StopAsync().GetAwaiter().GetResult(); }
        finally { _cts.Dispose(); _disposed = true; }
    }
}
