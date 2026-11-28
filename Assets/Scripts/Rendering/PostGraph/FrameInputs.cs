using UnityEngine;

namespace URPPT.Rendering
{
    /// <summary>
    /// 前置段（PT + DLSS）交给后置段的只读输入。
    /// 约定：Color 稳态下是输出分辨率；DLSS 未就绪的 1~2 帧可能是渲染分辨率（按实际 width/height 处理）。
    /// GBuffer0/1 是渲染分辨率（DLSS 关闭时等于输出分辨率），只能被升采样/重建类 pass 消费。
    /// </summary>
    public struct FrameInputs
    {
        /// <summary>必填。DLSS 输出，或 DLSS 关闭/失败时的 PT 原图。</summary>
        public RenderTexture Color;

        /// <summary>可选，输出分辨率。现在恒为 null；将来 native 直接出深度时填这里。</summary>
        public RenderTexture Depth;

        /// <summary>可选，渲染分辨率：worldPos.xyz + 命中距离。</summary>
        public RenderTexture GBuffer0;

        /// <summary>可选，渲染分辨率：normal.xyz + roughness。</summary>
        public RenderTexture GBuffer1;

        public Matrix4x4 View;
        public Matrix4x4 Proj;
        public Matrix4x4 PrevViewProj;
        public Vector2 Jitter;
        public int FrameIndex;
    }
}
