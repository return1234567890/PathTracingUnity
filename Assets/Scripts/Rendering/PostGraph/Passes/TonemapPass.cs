using UnityEngine;
using UnityEngine.Rendering;

namespace URPPT.Rendering
{
    /// <summary>
    /// 全屏 tonemap：线性 HDR → 显示参考（[0,1] 线性值）。
    /// 不做 sRGB 编码——交给 URP 的最终上屏（Linear 色彩空间）。
    /// 参数来自 PostGraphConfig（不序列化）。
    /// </summary>
    public sealed class TonemapPass : PostPass
    {
        public override string Name => "Tonemap";

        readonly Material _material;

        public TonemapPass(Material material)
        {
            _material = material;
        }

        public override void Declare(PostGraphBuilder g)
        {
            g.Read(Input);
            Output = g.Create(new RgTextureDesc
            {
                Name = "Tonemap.Out",
                Format = RgFormat.ColorRGBAHalf,
                Filter = FilterMode.Bilinear,
            });
        }

        public override void Render(PostPassContext ctx)
        {
            var src = ctx.Get(Input);
            var dst = ctx.Get(Output);
            if (src == null || dst == null || _material == null)
                return;

            _material.SetFloat("_Exposure", PostGraphConfig.TonemapExposure);
            _material.SetInt("_Mode", PostGraphConfig.TonemapMode);
            ctx.cmd.Blit(src, dst, _material, 0);
        }
    }
}
