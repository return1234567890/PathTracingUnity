// vpt_trace.cpp — 卡死诊断日志 + 看门狗实现

#include "vpt_trace.h"

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <ctime>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
    std::mutex              g_FileMutex;
    FILE*                   g_File         = nullptr;
    std::string             g_Path;
    bool                    g_Inited       = false;

    std::atomic<uint32_t>   g_Frame        { 0 };
    std::atomic<uint64_t>   g_LastBeatMs   { 0 };
    std::atomic<uint64_t>   g_LastWarnMs   { 0 };
    std::atomic<const char*> g_Stage       { "not-started" };
    std::atomic<bool>       g_StopThread   { false };
    std::atomic<bool>       g_Active       { false };
    std::thread             g_Watchdog;

    constexpr uint64_t kWarnMs      = 6000;     // 超过 6s 没喂狗 → 认为卡住
    constexpr uint64_t kRepeatMs    = 5000;     // 卡住期间每 5s 再报一次
    constexpr uint64_t kMaxBytes    = 64ull * 1024 * 1024;

    uint64_t NowMs()
    {
        return GetTickCount64();
    }

    // 优先写到 <项目根>/Logs（Unity 的工作目录就是项目根），失败再退回 LOCALAPPDATA
    std::string ResolvePath()
    {
        char cwd[MAX_PATH] = { 0 };
        if (GetCurrentDirectoryA(MAX_PATH, cwd) > 0)
        {
            std::string logsDir = std::string(cwd) + "\\Logs";
            DWORD attrs = GetFileAttributesA(logsDir.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                return logsDir + "\\VPT-trace-native.log";
        }

        char appData[MAX_PATH] = { 0 };
        size_t n = 0;
        if (getenv_s(&n, appData, sizeof(appData), "LOCALAPPDATA") == 0 && appData[0])
        {
            std::string dir = std::string(appData) + "\\VPT";
            CreateDirectoryA(dir.c_str(), nullptr);
            return dir + "\\VPT-trace-native.log";
        }

        return "VPT-trace-native.log"; // 最后兜底：当前目录
    }

    void RotateIfNeeded(const std::string& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
            return;
        uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (size < kMaxBytes)
            return;

        std::string oldPath = path + ".old";
        DeleteFileA(oldPath.c_str());
        MoveFileA(path.c_str(), oldPath.c_str());
    }

    void FormatTimestamp(char* buf, size_t len)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    }

    void WatchdogLoop()
    {
        while (!g_StopThread.load(std::memory_order_relaxed))
        {
            Sleep(500);

            const uint64_t now = NowMs();
            const uint64_t last = g_LastBeatMs.load(std::memory_order_relaxed);
            if (last == 0)
                continue;

            // 只在"应该有帧在跑"时报警：edit mode 下没有 PT 帧，属正常静默
            if (!g_Active.load(std::memory_order_relaxed))
            {
                g_LastBeatMs.store(now, std::memory_order_relaxed);
                continue;
            }

            const uint64_t idle = now - last;
            if (idle < kWarnMs)
                continue;

            const uint64_t lastWarn = g_LastWarnMs.load(std::memory_order_relaxed);
            if (lastWarn != 0 && now - lastWarn < kRepeatMs)
                continue;
            g_LastWarnMs.store(now, std::memory_order_relaxed);

            const char* stage = g_Stage.load(std::memory_order_relaxed);
            VPTTrace::Line("WATCHDOG: 主流程已 %.1f s 没有推进，最后阶段 = %s（frame=%u）",
                           (double)idle / 1000.0,
                           stage ? stage : "?",
                           VPTTrace::Frame());
        }
    }
}

namespace VPTTrace
{
    const char* Path()
    {
        std::lock_guard<std::mutex> lock(g_FileMutex);
        return g_Inited ? g_Path.c_str() : "";
    }

    void Init()
    {
        bool firstTime = false;
        {
            std::lock_guard<std::mutex> lock(g_FileMutex);
            if (g_Inited)
                return;
            g_Inited = true;
            firstTime = true;

            g_Path = ResolvePath();
            RotateIfNeeded(g_Path);
            g_File = fopen(g_Path.c_str(), "ab");
            if (!g_File)
            {
                // 退到临时目录，至少还能留下痕迹
                char tmp[MAX_PATH] = { 0 };
                GetTempPathA(MAX_PATH, tmp);
                g_Path = std::string(tmp) + "VPT-trace-native.log";
                g_File = fopen(g_Path.c_str(), "ab");
            }
        }

        g_LastBeatMs.store(NowMs(), std::memory_order_relaxed);
        g_StopThread.store(false, std::memory_order_relaxed);

        if (firstTime)
        {
            char ts[64];
            FormatTimestamp(ts, sizeof(ts));
            Line("===== VPT trace session start  %s  pid=%lu =====",
                 ts, (unsigned long)GetCurrentProcessId());
            Line("trace file: %s", g_Path.c_str());
            // 让用户在 Editor.log 里也能直接看到日志位置（文件打不开时会退化为 stderr）
            fprintf(stderr, "[VPT] trace log: %s%s\n", g_Path.c_str(),
                    g_File ? "" : "  (OPEN FAILED -> falling back to stderr)");
            fflush(stderr);
        }

        if (!g_Watchdog.joinable())
            g_Watchdog = std::thread(WatchdogLoop);
    }

    void Shutdown()
    {
        g_StopThread.store(true, std::memory_order_relaxed);
        if (g_Watchdog.joinable())
            g_Watchdog.join();

        std::lock_guard<std::mutex> lock(g_FileMutex);
        if (g_File)
        {
            char ts[64];
            FormatTimestamp(ts, sizeof(ts));
            fprintf(g_File, "[%s][tid %5lu] ===== VPT trace session end =====\n",
                    ts, (unsigned long)GetCurrentThreadId());
            fflush(g_File);
            fclose(g_File);
            g_File = nullptr;
        }
        g_Inited = false;
    }

    void Line(const char* fmt, ...)
    {
        char ts[64];
        FormatTimestamp(ts, sizeof(ts));

        char msg[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_FileMutex);
        if (!g_File)
        {
            // 退路：文件打不开时走 stderr（Unity 会写进 Editor.log），至少不丢诊断信息
            fprintf(stderr, "[VPT][warn] %s\n", msg);
            fflush(stderr);
            return;
        }

        fprintf(g_File, "[%s][tid %5lu][f %6u] %s\n",
                ts, (unsigned long)GetCurrentThreadId(),
                Frame(), msg);
        fflush(g_File);      // 关键：卡死前必须已经落盘
    }

    void Stage(const char* stage)
    {
        g_Stage.store(stage ? stage : "?", std::memory_order_relaxed);
    }

    void Beat()
    {
        g_LastBeatMs.store(NowMs(), std::memory_order_relaxed);
        g_LastWarnMs.store(0, std::memory_order_relaxed);
    }

    void SetActive(bool active)
    {
        g_Active.store(active, std::memory_order_relaxed);
        if (active)
            Beat();
    }

    uint32_t Frame()
    {
        return g_Frame.load(std::memory_order_relaxed);
    }

    uint32_t NextFrame()
    {
        return g_Frame.fetch_add(1, std::memory_order_relaxed) + 1;
    }
}
