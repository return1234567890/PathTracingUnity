using UnityEngine;
using UnityEngine.Rendering.Universal;

/// <summary>
/// Play 模式专用：把 Game 相机切换为「仅路径追踪」配置，抑制那些必然会被
/// PathTraceRendererFeature 整屏覆盖掉的 URP 光栅 / 阴影 / 后处理开销。
///
/// 为什么不直接在 Renderer / URP 资产上改：
///   资产字段是全局且落盘的 —— 会同时影响 Edit 模式和 Scene View，还会把 .asset 写脏。
///   本类改成 pure per-camera 的运行时开关。
///
/// 为什么不会污染工程：
///   1. 本类不含 [ExecuteAlways]，编辑期不会执行任何逻辑；
///   2. 只修改【场景内组件的序列化字段】，退出 Play 时 Unity 按场景快照自动还原，
///      不触碰任何 .asset；
///   3. Play 中取消勾选本组件会立即还原，方便做 A/B 性能对比。
///
/// 依赖：URP 资产（URP-HighFidelity.asset）的 Renderer 列表里必须存在一个只含
///       PathTraceRendererFeature 的 Renderer。
///       本工程：index 0 = URP-HighFidelity-Renderer（默认，含 SSAO + PT）
///               index 1 = URP-PathTrace-Renderer  （仅 PT，无 SSAO、无光栅 mask）
/// </summary>
[RequireComponent(typeof(Camera))]
[DisallowMultipleComponent]
[AddComponentMenu("Rendering/Path Trace Camera Gate")]
public sealed class PathTraceCameraGate : MonoBehaviour
{
    [Header("Renderer 索引（对应 URP 资产的 m_RendererDataList）")]
    [Tooltip("PT 专用 Renderer 的索引。\n" +
             "本工程：0 = URP-HighFidelity-Renderer（默认，含 SSAO），\n" +
             "        1 = URP-PathTrace-Renderer（仅 PT feature、Opaque/Transparent LayerMask 均为 Nothing）。")]
    public int ptRendererIndex = 1;

    [Tooltip("停用本组件时恢复到的 Renderer 索引。\n" +
             "-1 表示「默认 Renderer」——URP 会把它解释成 m_DefaultRendererIndex。")]
    public int restoreRendererIndex = -1;

    [Header("抑制项（这些内容都会被 PT 结果整屏覆盖）")]
    [Tooltip("清空相机 cullingMask：Opaque / Transparent 不再绘制。\n" +
             "剔除结果为空后，主光与附加光阴影的 GetShadowCasterBounds() 也会失败，\n" +
             "4096 级联阴影图随之不再渲染。")]
    public bool suppressOpaqueAndTransparent = true;

    [Tooltip("关闭本相机阴影（UniversalAdditionalCameraData.renderShadows = false）。\n" +
             "URP 内部会把 maxShadowDistance 置 0，并在剔除阶段去掉 CullingOptions.ShadowCasters。\n" +
             "这是 per-camera 开关，Scene View 不受影响。")]
    public bool suppressShadows = true;

    [Tooltip("关闭本相机后处理（Bloom / Tonemapping 等）。")]
    public bool suppressPostProcessing = true;

    [Tooltip("clearFlags 改为 SolidColor，省掉天空盒的全屏绘制。")]
    public bool suppressSkybox = true;

    [Header("安全")]
    [Tooltip("场景中不存在 ModelManager 时不做任何抑制，避免 PT 不运行导致 Game 视图全黑。")]
    public bool requireModelManager = true;

    // ── 快照（OnDisable 还原用）──
    private Camera _cam;
    private UniversalAdditionalCameraData _camData;
    private bool _applied;
    private int _savedCullingMask;
    private CameraClearFlags _savedClearFlags;
    private bool _savedRenderShadows;
    private bool _savedRenderPostProcessing;

    private void OnEnable()
    {
        // 没有 [ExecuteAlways] → 编辑期根本不会执行到这里；这层判断只是双保险
        if (!Application.isPlaying)
            return;

        if (requireModelManager && !HasModelManager())
        {
            Debug.LogWarning("[PathTraceGate] 场景里没有 ModelManager，跳过 URP 抑制" +
                             "（否则 PT 不会运行，Game 视图会全黑）。", this);
            return;
        }

        _cam = GetComponent<Camera>();
        if (_cam == null)
            return;

        _camData = _cam.GetUniversalAdditionalCameraData();
        if (_camData == null)
            return;

        // 1) 记录原始值（OnDisable 还原）
        _savedCullingMask          = _cam.cullingMask;
        _savedClearFlags           = _cam.clearFlags;
        _savedRenderShadows        = _camData.renderShadows;
        _savedRenderPostProcessing = _camData.renderPostProcessing;

        // 2) 应用抑制
        if (suppressOpaqueAndTransparent) _cam.cullingMask = 0;
        if (suppressSkybox)               _cam.clearFlags = CameraClearFlags.SolidColor;
        if (suppressShadows)              _camData.renderShadows = false;
        if (suppressPostProcessing)       _camData.renderPostProcessing = false;

        // 3) 切到只含 PT feature 的 Renderer：
        //    这一步同时去掉 SSAO，以及它 ConfigureInput(Normal) 触发的全分辨率 DepthNormals Prepass
        _camData.SetRenderer(ptRendererIndex);

        _applied = true;

        Debug.Log($"[PathTraceGate] '{_cam.name}' → Renderer #{ptRendererIndex}，" +
                  $"cullingMask=0x{_cam.cullingMask:X}，" +
                  $"renderShadows={_camData.renderShadows}，" +
                  $"postProcess={_camData.renderPostProcessing}。退出 Play 自动还原。", this);
    }

    private void OnDisable()
    {
        if (!_applied)
            return;
        _applied = false;

        // Unity 的 null 检查对已销毁对象返回 true，可安全跳过
        if (_cam != null)
        {
            _cam.cullingMask = _savedCullingMask;
            _cam.clearFlags  = _savedClearFlags;
        }

        if (_camData != null)
        {
            _camData.renderShadows        = _savedRenderShadows;
            _camData.renderPostProcessing = _savedRenderPostProcessing;
            _camData.SetRenderer(restoreRendererIndex);
        }
    }

    /// <summary>场景里是否存在 ModelManager（Awake 顺序不确定，故不只看单例）。</summary>
    private static bool HasModelManager()
    {
        if (ModelManager.Instance != null)
            return true;

        return FindObjectOfType<ModelManager>(true) != null;
    }
}
