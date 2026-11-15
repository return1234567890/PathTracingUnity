#pragma once
//
// vpt_trace.h — 卡死/挂起诊断用的独立日志 + 看门狗
//
// 为什么不用 Editor.log：
//   1. Unity 的 Editor.log 由编辑器自己缓冲，进程卡死时最后几行经常落不了盘；
//   2. 它混了引擎/NGX/其它插件的海量输出，真正的"最后一步"很难找。
// 这里另开一个文件，**每行立即 flush**，并记录「当前正在做什么」：
//   卡住时把文件末尾几行贴出来，就能直接看到是死在哪个 vk 调用/Unity 调用上。
//
// 另外起一个看门狗线程：主流程超过 kWarnMs 没有 Beat()，就往日志里写一条
// WATCHDOG（带当前阶段、卡了多久、帧号），所以哪怕编辑器完全无响应，
// 日志里也会有明确的"卡在哪一步"。
//
// 输出位置：优先 <项目根>/Logs/VPT-trace-native.log，失败则
//           %LOCALAPPDATA%/VPT/VPT-trace-native.log。
// 文件超过 64MB 时自动轮转为 .old（只保留上一份）。
//

#include <cstdint>

namespace VPTTrace
{
    /// 打开日志文件 + 启动看门狗（幂等；可从任意线程调用）
    void Init();

    /// 停止看门狗并关闭文件（卸载插件/销毁时调用）
    void Shutdown();

    /// 日志文件完整路径（未 Init 时返回空串 ""，不会是 nullptr）
    ///
    /// ⚠ 返回的指针**不保证长期有效**。它指向内部 std::string（g_Path）的缓冲区，
    ///   而 Shutdown() 之后的再次 Init() 会重新给 g_Path 赋值；若新路径更长，
    ///   std::string 会重新分配 → 旧缓冲区被释放，先前拿到的指针变成悬垂指针
    ///   （use-after-free：轻则打印出乱码路径，重则在 strlen/fprintf 里崩溃）。
    ///   注意 mutex 只在函数内部生效，**返回值出了锁就不再受任何保护**。
    ///
    /// 调用约定（必须遵守）：
    ///   · 在当前线程内「立即拷贝」使用，即 printf/memcpy/赋值给 std::string 这类一次性用法；
    ///   · 禁止缓存 const char*、禁止跨线程传递、禁止存进结构体或静态变量里长期持有。
    ///
    /// 现有调用点已按此约定核对过：
    ///   · plugin_main.cpp UnityPluginLoad：VPTTrace::Line("... trace=%s", Path()) 同表达式内用完；
    ///   · vpt_api.cpp VPT_GetTracePath：转给 C#，C# 侧 Marshal.PtrToStringAnsi 立即拷贝。
    ///
    /// 若将来确实需要「进程生命周期内有效」的指针，把实现改成固定大小的静态缓冲区
    ///   （如 static char g_PathBuf[MAX_PATH * 2]，Init 时 snprintf 写入、之后不再改动），
    ///   指针地址恒定、不会 realloc，即可安全对外承诺；代价是需要重新编译 DLL。
    const char* Path();

    /// 写一行日志（时间戳 + 线程 id + 帧号，立即 flush）。格式化同 printf。
    void Line(const char* fmt, ...);

    /// 设置「当前阶段」。stage 必须是静态字符串（只存指针，不拷贝）。
    /// 看门狗在检测到卡死时会把最后设置的 stage 打出来。
    void Stage(const char* stage);

    /// 喂狗：表示主流程还在推进。每个事件/每帧至少调用一次。
    void Beat();

    /// 标记"现在确实应该有帧在跑"（VPT_Initialize 之后为 true，VPT_Destroy 之后 false）。
    /// 看门狗只在 active 时才报警 —— 否则编辑器停留在 edit mode（没有 PT 帧）会被误报。
    void SetActive(bool active);

    /// 当前帧号（由 NextFrame 递增）
    uint32_t Frame();
    uint32_t NextFrame();
}
