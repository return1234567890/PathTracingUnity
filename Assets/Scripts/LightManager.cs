using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;

/// <summary>
/// 光源数据结构（32B，与 HLSL PathLight 严格对齐）。
/// </summary>
[StructLayout(LayoutKind.Sequential)]
public struct PathLight
{
    public Vector3 positionOrDir; // point=世界坐标; directional=指向光源方向
    public float   range;          // point=衰减范围; directional=1e34f
    public Vector3 color;          // rgb（intensity 已乘入）
    public uint    type;           // 0=point, 1=directional
}

/// <summary>
/// 场景光源收集管理器。
/// 在游戏开始时收集场景中的 Light 组件，组装为 32B 对齐的 PathLight 数组，
/// 供 RenderBufferManager 上传至 GPU 供 PathTrace.compute 使用。
///
/// 光源类型映射：
///   LightType.Point       → type 0（点光源，带平方反比衰减）
///   LightType.Directional → type 1（方向光，无衰减）
///   其余类型暂不采样，但会以 point 形式记录（避免丢失颜色）。
/// </summary>
public class LightManager : MonoBehaviour
{
    [Tooltip("是否在收集完成后打印光源统计信息")]
    public bool logBuildStats = true;

    /// <summary>全局单例，便于路径追踪器访问。</summary>
    public static LightManager Instance { get; private set; }

    /// <summary>收集到的光源数组（按实例索引）。</summary>
    public PathLight[] Lights { get; private set; }

    private void Awake()
    {
        if (Instance != null && Instance != this)
        {
            Debug.LogWarning("[LightManager] 已存在实例，销毁重复实例。", this);
            Destroy(this);
            return;
        }
        Instance = this;
    }

    private void Start()
    {
        CollectLights();
    }

    private void OnDestroy()
    {
        if (Instance == this)
            Instance = null;
    }

    /// <summary>
    /// 收集场景中的 Light 组件，组装为 PathLight 数组。
    /// 在 Start 中自动调用；场景光源变更后可手动调用重建。
    /// </summary>
    public void CollectLights()
    {
        var list = new List<PathLight>();

        Light[] lights = FindObjectsOfType<Light>();
        foreach (Light l in lights)
        {
            if (l == null) continue;
            if (!l.enabled || !l.gameObject.activeInHierarchy) continue;

            PathLight pl;
            pl.color = new Vector3(
                l.color.r * l.intensity,
                l.color.g * l.intensity,
                l.color.b * l.intensity);
            pl.type = 0;
            pl.positionOrDir = Vector3.zero;
            pl.range = 1e34f;

            if (l.type == LightType.Directional)
            {
                pl.type = 1;
                // 指向光源的方向（用于采样时取 toL = positionOrDir）
                pl.positionOrDir = -l.transform.forward;
            }
            else
            {
                // Point / Spot / Area 等统一按点光源处理
                pl.type = 0;
                pl.positionOrDir = l.transform.position;
                pl.range = l.range > 0 ? l.range : 1e34f;
            }

            list.Add(pl);
        }

        // 无光源时生成 1 条 fallback 常量环境光，避免全黑
        if (list.Count == 0)
        {
            list.Add(new PathLight
            {
                positionOrDir = new Vector3(0f, 1f, 0f),
                range = 1e34f,
                color = new Vector3(0.1f, 0.1f, 0.1f),
                type = 1
            });
        }

        Lights = list.ToArray();

        if (logBuildStats)
        {
            int point = 0, dir = 0;
            foreach (var pl in Lights)
            {
                if (pl.type == 1) dir++;
                else point++;
            }
            Debug.Log(
                $"[LightManager] 收集完成：光源总数={Lights.Length}（方向光={dir}, 点光源={point}）");
        }
    }
}
