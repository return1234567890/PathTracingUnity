using System;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace URPPT.Rendering
{
    /// <summary>
    /// 后置段的轻量 Pass 图（v1：Import → [Tonemap] → Present）。
    ///
    /// 职责：按注册顺序执行 pass、解析句柄、在 Rebuild 时分配/复用 RT、把最终颜色写到 cameraColor。
    /// 非职责：不管理前置段（PT/DLSS）的 RT、不重建 DLSS、不碰 native 资源。
    ///
    /// 生命周期：拓扑只在配置变化时重建（分辨率/输入格式/开关），每帧只执行。
    /// 参数来自 PostGraphConfig（不序列化到 Renderer 资产）。
    /// </summary>
    public sealed class PostGraph : IDisposable
    {
        public delegate RgTexture BuildFunc(PostGraphBuilder g, FrameInputs inputs);

        readonly BuildFunc _build;
        readonly PostGraphPool _pool = new PostGraphPool();

        CompiledPlan _plan;

        bool _hasKey;
        int _keyW, _keyH;
        int _keyColorW, _keyColorH, _keyColorFormat;
        bool _keyTonemap;

        public PostGraph(BuildFunc build)
        {
            _build = build ?? throw new ArgumentNullException(nameof(build));
        }

        /// <summary>
        /// 每帧调用：必要时 Rebuild，然后按顺序执行 pass，最后 Present。
        /// 返回 false 表示图不可用，调用方应退回旧的直接 Blit。
        /// </summary>
        public bool Execute(CommandBuffer cmd, ScriptableRenderContext context, Camera camera,
                            FrameInputs inputs, RTHandle cameraColor)
        {
            if (cmd == null || inputs.Color == null || cameraColor == null)
                return false;

            int w = camera != null ? camera.pixelWidth : inputs.Color.width;
            int h = camera != null ? camera.pixelHeight : inputs.Color.height;

            if (NeedsRebuild(w, h, inputs))
                Rebuild(w, h, inputs);

            if (_plan == null)
                return false;

            var ctx = new PostPassContext
            {
                graph = this,
                context = context,
                cmd = cmd,
                cameraColor = cameraColor,
                inputs = inputs,
                camera = camera,
            };

            var passes = _plan.Passes;
            for (int i = 0; i < passes.Length; i++)
                passes[i].Render(ctx);

            var final = GetTexture(_plan.Present);
            if (final == null)
                return false;

            cmd.Blit(final, cameraColor);
            return true;
        }

        internal RenderTexture GetTexture(RgTexture t)
        {
            if (_plan == null || !t.IsValid)
                return null;

            int index = t.Id - 1;
            if (index < 0 || index >= _plan.Textures.Length)
                return null;
            return _plan.Textures[index];
        }

        public string DumpChain()
        {
            if (_plan == null)
                return "[PostGraph] (not built)";

            var sb = new StringBuilder(512);
            sb.Append("[PostGraph] ").Append(_plan.Passes.Length).Append(" pass(es)\n");
            for (int i = 0; i < _plan.Resources.Length; i++)
            {
                var r = _plan.Resources[i];
                var tex = _plan.Textures != null && i < _plan.Textures.Length ? _plan.Textures[i] : null;
                sb.Append("  R").Append(r.Id).Append(' ').Append(r.Name)
                  .Append(r.External ? " [imported]" : " [created]")
                  .Append(tex != null ? $" {tex.width}x{tex.height} {tex.format}" : " (null)")
                  .Append('\n');
            }
            for (int i = 0; i < _plan.Passes.Length; i++)
                sb.Append("  P").Append(i).Append(' ').Append(_plan.Passes[i].Name).Append('\n');
            return sb.ToString();
        }

        public void Dispose()
        {
            ReleasePlan();
            _pool.Dispose();
        }

        // ── 内部 ─────────────────────────────────────────────

        bool NeedsRebuild(int w, int h, FrameInputs inputs)
        {
            if (!_hasKey)
                return true;
            if (_keyW != w || _keyH != h)
                return true;

            var color = inputs.Color;
            if (_keyColorW != color.width || _keyColorH != color.height ||
                _keyColorFormat != (int)color.format)
                return true;

            return _keyTonemap != PostGraphConfig.TonemapEnabled;
        }

        void Rebuild(int w, int h, FrameInputs inputs)
        {
            ReleasePlan();

            var builder = new PostGraphBuilder(w, h);
            try
            {
                _build(builder, inputs);
            }
            catch (Exception e)
            {
                Debug.LogError("[PostGraph] build exception: " + e);
                MarkKey(w, h, inputs);
                return;
            }

            var plan = builder.Compile();
            if (plan == null)
            {
                // Compile 已经逐条打过 LogError
                MarkKey(w, h, inputs);
                return;
            }

            var textures = new RenderTexture[plan.Resources.Length];
            for (int i = 0; i < plan.Resources.Length; i++)
            {
                var r = plan.Resources[i];
                if (r.External)
                {
                    textures[i] = r.Imported;
                    continue;
                }

                ResolveSize(r.Desc, w, h, out int rw, out int rh);
                textures[i] = _pool.Get(rw, rh, r.Desc.Format, r.Desc.RandomWrite, r.Desc.Filter);
            }

            plan.Textures = textures;
            _plan = plan;
            MarkKey(w, h, inputs);
        }

        void ReleasePlan()
        {
            if (_plan == null)
                return;

            var res = _plan.Resources;
            var tex = _plan.Textures;
            if (tex != null)
            {
                for (int i = 0; i < res.Length && i < tex.Length; i++)
                {
                    if (!res[i].External && tex[i] != null)
                        _pool.Release(tex[i]);
                }
            }

            _plan = null;
        }

        void MarkKey(int w, int h, FrameInputs inputs)
        {
            _hasKey = true;
            _keyW = w;
            _keyH = h;
            _keyColorW = inputs.Color.width;
            _keyColorH = inputs.Color.height;
            _keyColorFormat = (int)inputs.Color.format;
            _keyTonemap = PostGraphConfig.TonemapEnabled;
        }

        static void ResolveSize(RgTextureDesc desc, int outW, int outH, out int w, out int h)
        {
            switch (desc.Size)
            {
                case RgSize.Fraction:
                    w = Mathf.Max(1, Mathf.RoundToInt(outW * desc.Fraction));
                    h = Mathf.Max(1, Mathf.RoundToInt(outH * desc.Fraction));
                    break;
                case RgSize.Absolute:
                    w = Mathf.Max(1, desc.Width);
                    h = Mathf.Max(1, desc.Height);
                    break;
                default:
                    w = Mathf.Max(1, outW);
                    h = Mathf.Max(1, outH);
                    break;
            }
        }
    }
}
