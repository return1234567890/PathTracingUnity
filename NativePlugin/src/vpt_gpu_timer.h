#pragma once
//
// vpt_gpu_timer.h — GPU 分段计时（vkCmdWriteTimestamp）
//
// 背景：路径追踪的成本几乎全在 GPU 上，Unity Profiler 只能看到 CPU 侧提交，
//       所以这里直接在每个阶段前后写 timestamp，量 GPU 时间。
//
// 一帧拆成 7 段（索引与 C# NativeBridge.GpuStage 一一对应）：
//   0 skin       开始 → GPU 蒙皮 dispatch
//   1 refit      蒙皮 → BLAS refit
//   2 tlas       refit → TLAS 重建
//   3 pathtrace  TLAS → 路径追踪 dispatch 结束
//   4 motion     PT 结束 → DLSS 回调开始（覆盖 C# 侧 MotionVector compute）
//   5 dlss       DLSS 回调内：Evaluate 前后
//   6 total      开始 → 最后一个标记
//
// 打点位置：
//   VPTDispatch::RenderCallback  → BeginFrame + Mark x4
//   VPT_DLSS_RenderCallback      → Mark x2
//
// 读回是异步的：GPU 结果要 1~2 帧后才可用，Poll() 用 availability 位轮询，
// 不会阻塞（不能用 VK_QUERY_RESULT_WAIT_BIT，那会把 CPU 卡住，反而污染测量）。
//

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace VPTGpuTimer
{
enum Stage : int32_t
{
    STAGE_SKIN = 0,
    STAGE_REFIT,
    STAGE_TLAS,
    STAGE_PATHTRACE,
    STAGE_MOTION,
    STAGE_DLSS,
    STAGE_TOTAL,
    STAGE_COUNT
};

inline constexpr uint32_t kBlocks = 4;   // 环形块，避免和飞行中的帧抢同一个 query
inline constexpr uint32_t kMarks  = 8;   // 每帧最多 8 个标记（T0..T6 + 余量）
inline constexpr uint32_t kPoolSize = kBlocks * kMarks;

struct State
{
    VkDevice    device   = VK_NULL_HANDLE;
    VkQueryPool pool     = VK_NULL_HANDLE;
    float       tickToMs = 0.0f;         // timestampPeriod(ns) → ms
    bool        enabled  = false;
    bool        frameOpen = false;       // BeginFrame 之后、本帧标记还没写完

    uint32_t block = 0;
    uint32_t slot  = 0;
    uint32_t markCount[kBlocks] = {};

    // 最近一次成功读回的结果
    bool     hasNew     = false;
    uint32_t frameId    = 0;
    int32_t  stageCount = 0;
    float    ms[STAGE_COUNT] = {};
};

inline State& S()
{
    static State s;
    return s;
}

// ── 初始化 ────────────────────────────────────────────────
inline bool Init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex)
{
    State& s = S();
    if (s.pool != VK_NULL_HANDLE)
        return s.enabled;
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
        return false;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.limits.timestampPeriod <= 0.0f)
    {
        fprintf(stderr, "[VPT-GPU] timestampPeriod = 0, GPU 计时不可用\n");
        return false;
    }

    // timestampValidBits 必须非 0，否则写进去全是 0
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, nullptr);
    uint32_t validBits = 0;
    if (qCount > 0 && queueFamilyIndex < qCount)
    {
        VkQueueFamilyProperties* all = new VkQueueFamilyProperties[qCount];
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, all);
        validBits = all[queueFamilyIndex].timestampValidBits;
        delete[] all;
    }
    if (validBits == 0)
    {
        fprintf(stderr, "[VPT-GPU] queue family %u 不支持 timestamp，GPU 计时不可用\n", queueFamilyIndex);
        return false;
    }

    VkQueryPoolCreateInfo ci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    ci.queryType  = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = kPoolSize;
    if (vkCreateQueryPool(device, &ci, nullptr, &s.pool) != VK_SUCCESS)
    {
        fprintf(stderr, "[VPT-GPU] vkCreateQueryPool 失败\n");
        s.pool = VK_NULL_HANDLE;
        return false;
    }

    s.device     = device;
    s.tickToMs   = props.limits.timestampPeriod / 1000000.0f;
    s.block      = 0;
    s.slot       = 0;
    s.frameOpen  = false;
    s.enabled    = true;
    memset(s.markCount, 0, sizeof(s.markCount));
    fprintf(stderr, "[VPT-GPU] GPU 计时已启用（timestampPeriod = %.3f ns）\n",
            props.limits.timestampPeriod);
    return true;
}

inline void Shutdown()
{
    State& s = S();
    if (s.pool != VK_NULL_HANDLE && s.device != VK_NULL_HANDLE)
        vkDestroyQueryPool(s.device, s.pool, nullptr);
    s.pool      = VK_NULL_HANDLE;
    s.device    = VK_NULL_HANDLE;
    s.enabled   = false;
    s.frameOpen = false;
    s.hasNew    = false;
    memset(s.markCount, 0, sizeof(s.markCount));
}

inline void SetEnabled(bool on)
{
    State& s = S();
    if (s.pool == VK_NULL_HANDLE) return;   // 不支持就忽略
    s.enabled   = on;
    s.frameOpen = false;
    s.hasNew    = false;
    memset(s.markCount, 0, sizeof(s.markCount));
}

// ── 内部：写一个 timestamp ────────────────────────────────
inline void Write(VkCommandBuffer cmd)
{
    State& s = S();
    if (s.slot >= kMarks) return;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        s.pool, s.block * kMarks + s.slot);
    s.slot++;
    s.markCount[s.block] = s.slot;
}

// ── 取回已完成块的结果（非阻塞）───────────────────────────
inline void Poll()
{
    State& s = S();
    if (!s.enabled) return;

    for (uint32_t b = 0; b < kBlocks; ++b)
    {
        if (b == s.block) continue;            // 正在写的块跳过
        const uint32_t count = s.markCount[b];
        if (count < 2) continue;

        const uint32_t stride = sizeof(uint64_t) * 2;   // 64bit 值 + 32bit availability
        uint64_t data[kMarks * 2] = {};
        VkResult r = vkGetQueryPoolResults(
            s.device, s.pool, b * kMarks, count,
            (size_t)count * stride, data, stride,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r != VK_SUCCESS)
            continue;                          // VK_NOT_READY：下一帧再试

        bool ready = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            if ((uint32_t)(data[i * 2 + 1] & 0xFFFFFFFFu) == 0u) { ready = false; break; }
        }
        if (!ready) continue;

        uint64_t t[kMarks] = {};
        for (uint32_t i = 0; i < count; ++i)
            t[i] = data[i * 2];

        float ms[STAGE_COUNT] = {};
        // t0..t6: 开始 / skin / refit / tlas / pt / dlss_in / dlss_out
        if (count >= 2) ms[STAGE_SKIN]      = (float)(t[1] - t[0]) * s.tickToMs;
        if (count >= 3) ms[STAGE_REFIT]     = (float)(t[2] - t[1]) * s.tickToMs;
        if (count >= 4) ms[STAGE_TLAS]      = (float)(t[3] - t[2]) * s.tickToMs;
        if (count >= 5) ms[STAGE_PATHTRACE] = (float)(t[4] - t[3]) * s.tickToMs;
        if (count >= 6) ms[STAGE_MOTION]    = (float)(t[5] - t[4]) * s.tickToMs;
        if (count >= 7) ms[STAGE_DLSS]      = (float)(t[6] - t[5]) * s.tickToMs;
        ms[STAGE_TOTAL] = (float)(t[count - 1] - t[0]) * s.tickToMs;

        memcpy(s.ms, ms, sizeof(ms));
        s.stageCount = STAGE_COUNT;
        s.frameId++;
        s.hasNew = true;
        s.markCount[b] = 0;                    // 读完即释放该块
    }
}

// ── 每帧开始：换块 + 清 query + 打 T0 ─────────────────────
inline void BeginFrame(VkCommandBuffer cmd)
{
    State& s = S();
    if (!s.enabled || cmd == VK_NULL_HANDLE) return;

    Poll();                                    // 顺手回收 2 帧前的结果

    s.block = (s.block + 1) % kBlocks;
    s.slot  = 0;
    s.markCount[s.block] = 0;
    s.frameOpen = true;

    vkCmdResetQueryPool(cmd, s.pool, s.block * kMarks, kMarks);
    Write(cmd);                                // T0
}

// ── 阶段结束：依次打点（skin / refit / tlas / pt / dlss_in / dlss_out）──
inline void Mark(VkCommandBuffer cmd)
{
    State& s = S();
    if (!s.enabled || !s.frameOpen || cmd == VK_NULL_HANDLE) return;
    Write(cmd);
}

// ── 取结果（供 C# 轮询）──────────────────────────────────
inline bool GetStats(float* outMs, int32_t maxCount, uint32_t* outFrameId)
{
    State& s = S();
    if (!s.hasNew || outMs == nullptr) return false;

    const int32_t n = (maxCount < STAGE_COUNT) ? maxCount : (int32_t)STAGE_COUNT;
    for (int32_t i = 0; i < n; ++i)
        outMs[i] = s.ms[i];
    if (outFrameId) *outFrameId = s.frameId;
    s.hasNew = false;
    return true;
}

} // namespace VPTGpuTimer
