using System;
using System.IO;
using System.Text;
using System.Threading;
using UnityEngine;

/// <summary>
/// 卡死诊断用日志（C# 侧）。
///
/// 与 native 侧的 VPT-trace-native.log 配合使用：
///   · 每行立即 flush，进程卡死时文件里保留的是"最后一步"
///   · 后台看门狗线程：主线程超过 6s 没有 Beat() 就写一条 WATCHDOG，
///     带上最后一次 Stage 名称 —— 卡住时直接看这一行就知道死在哪
///
/// 输出：&lt;项目根&gt;/Logs/VPT-trace-csharp.log
/// 关闭：删掉 PathTraceRendererFeature.Create 里的 VptTrace.Init() 调用即可（或改 enableTrace=false）。
/// </summary>
public static class VptTrace
{
    private const long WarnMs = 6000;
    private const long RepeatMs = 5000;

    private static readonly object _lock = new object();
    private static StreamWriter _writer;
    private static bool _inited;
    private static string _path = "";

    private static volatile string _stage = "not-started";
    private static long _lastBeatMs;
    private static long _lastWarnMs;
    private static int _frame;

    private static Thread _watchdog;
    private static volatile bool _stop;

    public static string Path => _path;

    // 用 Stopwatch 而不是 Environment.TickCount64：后者在 Unity 的 netstandard 配置下不可用
    private static readonly System.Diagnostics.Stopwatch _clock =
        System.Diagnostics.Stopwatch.StartNew();

    private static long NowMs() => _clock.ElapsedMilliseconds;

    public static void Init()
    {
        if (_inited) return;

        lock (_lock)
        {
            if (_inited) return;
            _inited = true;

            try
            {
                string dir = System.IO.Path.GetFullPath(
                    System.IO.Path.Combine(Application.dataPath, "..", "Logs"));
                Directory.CreateDirectory(dir);
                _path = System.IO.Path.Combine(dir, "VPT-trace-csharp.log");

                var stream = new FileStream(_path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                _writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };
            }
            catch (Exception e)
            {
                _writer = null;
                Debug.LogWarning("[VptTrace] 无法创建 C# 诊断日志：" + e.Message);
            }

            _lastBeatMs = NowMs();
            _frame = 0;   // 新会话从 0 开始，否则日志里的 [f 003263] 会接着上一局继续，容易误读
            Raw("===== C# trace session start  " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + " =====");
            Raw("trace file: " + _path);
        }

        _stop = false;
        if (_watchdog == null)
        {
            _watchdog = new Thread(WatchdogLoop)
            {
                IsBackground = true,
                Name = "VptTraceWatchdog"
            };
            _watchdog.Start();
        }
    }

    public static void Shutdown()
    {
        _stop = true;
        Line("VptTrace.Shutdown");

        // 等看门狗线程真的退出（它每 500ms 醒一次，最多等 1s）。
        // 两点必须注意：
        //   1) 必须先 Join 再拿 _lock —— 线程可能正卡在 Line() 里等这把锁，反过来会死锁；
        //   2) 结束后必须把引用清空，否则下次 Init() 看到 _watchdog != null 就不再启动新线程，
        //      域重载关闭（Enter Play Mode Options 里关掉 Reload Domain）时第二局 Play 的
        //      看门狗会静默死掉，卡死时也就不再有任何 WATCHDOG 记录。
        Thread watchdog = _watchdog;
        _watchdog = null;
        if (watchdog != null)
        {
            try { watchdog.Join(1000); } catch { }
        }

        lock (_lock)
        {
            if (_writer != null)
            {
                try { _writer.Flush(); _writer.Dispose(); } catch { }
                _writer = null;
            }
            _inited = false;
        }
    }

    /// <summary>写一行（时间戳 + 线程 + 帧号，立即 flush）。</summary>
    public static void Line(string msg)
    {
        string ts = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff");
        Raw("[" + ts + "][tid " + Thread.CurrentThread.ManagedThreadId.ToString("D3") +
            "][f " + _frame.ToString("D6") + "] " + msg);
    }

    /// <summary>设置当前阶段（请传字符串字面量，避免每帧分配）。</summary>
    public static void Stage(string stage) => _stage = stage;

    /// <summary>喂狗：表示主线程还在推进。</summary>
    public static void Beat()
    {
        _lastBeatMs = NowMs();
        _lastWarnMs = 0;
    }

    public static void NextFrame() => Interlocked.Increment(ref _frame);

    public static int Frame => _frame;

    private static void Raw(string line)
    {
        lock (_lock)
        {
            if (_writer == null) return;
            try { _writer.WriteLine(line); } catch { _writer = null; }
        }
    }

    private static void WatchdogLoop()
    {
        while (!_stop)
        {
            Thread.Sleep(500);

            long last = Interlocked.Read(ref _lastBeatMs);
            if (last == 0) continue;

            long idle = NowMs() - last;
            if (idle < WarnMs) continue;

            long lastWarn = Interlocked.Read(ref _lastWarnMs);
            long now = NowMs();
            if (lastWarn != 0 && now - lastWarn < RepeatMs) continue;
            Interlocked.Exchange(ref _lastWarnMs, now);

            Line("WATCHDOG: 主线程已 " + (idle / 1000.0).ToString("F1") +
                 " s 没有推进，最后阶段 = " + _stage);
        }
    }
}
