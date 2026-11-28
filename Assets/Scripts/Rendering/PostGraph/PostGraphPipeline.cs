using UnityEngine;

namespace URPPT.Rendering
{
    /// <summary>
    /// v1 的链：Import(Color) → [Tonemap] → Present。
    /// 以后加 pass 只改这里（顺序就是代码顺序），不要动 PostGraph 本体。
    /// 开关在 PostGraphConfig（不序列化）。
    /// </summary>
    public static class PostGraphPipeline
    {
        public static RgTexture Build(PostGraphBuilder g, FrameInputs inputs, Material tonemapMaterial)
        {
            RgTexture color = g.Import(inputs.Color, "Input.Color");

            if (PostGraphConfig.TonemapEnabled && tonemapMaterial != null)
            {
                color = g.Add(new TonemapPass(tonemapMaterial)
                {
                    Input = color,
                });
            }

            // 以后在这里按顺序插入：Bloom / FXAA / ForwardOverlay / OverlayUI ...

            g.Present(color);
            return color;
        }
    }
}
