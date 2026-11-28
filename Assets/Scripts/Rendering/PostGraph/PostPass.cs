using UnityEngine;
using UnityEngine.Rendering;

namespace URPPT.Rendering
{
    /// <summary>
    /// 一个后置 pass。只做两件事：
    ///   1) Declare：声明读写与输出（不录命令）；
    ///   2) Render：把命令录进 ctx.cmd（需要 DrawRenderers 的先 ctx.Flush()）。
    /// </summary>
    public abstract class PostPass
    {
        public abstract string Name { get; }
        public virtual bool Enabled => true;

        /// <summary>由 pipeline 赋值的颜色输入。</summary>
        public RgTexture Input;

        /// <summary>由 Declare 赋值的颜色输出。</summary>
        public RgTexture Output;

        public abstract void Declare(PostGraphBuilder g);

        public abstract void Render(PostPassContext ctx);
    }

    /// <summary>执行期上下文：解析句柄、录命令、必要时 flush。</summary>
    public struct PostPassContext
    {
        internal PostGraph graph;

        public ScriptableRenderContext context;
        public CommandBuffer cmd;
        public RTHandle cameraColor;
        public FrameInputs inputs;
        public Camera camera;

        public RenderTexture Get(RgTexture t) => graph.GetTexture(t);

        /// <summary>
        /// 把已录的命令立即交给 context 并清空 cmd。
        /// 任何要 context.DrawRenderers 的 pass 必须先调它；
        /// 必须在 ExecuteCommandBuffer 之后 Clear，否则下次执行会重复录过的命令。
        /// </summary>
        public void Flush()
        {
            context.ExecuteCommandBuffer(cmd);
            cmd.Clear();
        }
    }
}
