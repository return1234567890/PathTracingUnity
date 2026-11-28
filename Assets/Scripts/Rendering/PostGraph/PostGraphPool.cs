using System;
using System.Collections.Generic;
using UnityEngine;

namespace URPPT.Rendering
{
    /// <summary>
    /// 图内资源的持久池：同 desc 复用同一个 RenderTexture 对象，避免每帧 Create/Release
    /// 触发 VkImage churn（见 doc/bug追踪.md BUG-004）。
    /// v1 只在 Rebuild 时取/还，稳态不会分配。
    /// </summary>
    internal sealed class PostGraphPool : IDisposable
    {
        readonly Dictionary<(int, int, int, bool, int), Stack<RenderTexture>> _free =
            new Dictionary<(int, int, int, bool, int), Stack<RenderTexture>>();

        readonly List<RenderTexture> _all = new List<RenderTexture>();

        public RenderTexture Get(int width, int height, RgFormat format, bool randomWrite, FilterMode filter)
        {
            width = Mathf.Max(1, width);
            height = Mathf.Max(1, height);
            var rtFormat = ToRTFormat(format);
            var key = (width, height, (int)rtFormat, randomWrite, (int)filter);

            if (_free.TryGetValue(key, out var stack) && stack.Count > 0)
            {
                var cached = stack.Pop();
                if (cached != null)
                    return cached;
            }

            var rt = format == RgFormat.Depth32
                ? new RenderTexture(width, height, 24, RenderTextureFormat.Depth)
                : new RenderTexture(width, height, 0, rtFormat);

            rt.name = "PostGraph_" + format;
            rt.enableRandomWrite = randomWrite;
            rt.filterMode = filter;
            rt.wrapMode = TextureWrapMode.Clamp;
            rt.antiAliasing = 1;
            rt.useMipMap = false;
            rt.autoGenerateMips = false;
            rt.Create();

            _all.Add(rt);
            return rt;
        }

        public void Release(RenderTexture rt)
        {
            if (rt == null)
                return;

            var key = (rt.width, rt.height, (int)rt.format, rt.enableRandomWrite, (int)rt.filterMode);
            if (!_free.TryGetValue(key, out var stack))
            {
                stack = new Stack<RenderTexture>();
                _free[key] = stack;
            }
            stack.Push(rt);
        }

        public void Dispose()
        {
            _free.Clear();
            foreach (var rt in _all)
            {
                if (rt == null)
                    continue;
                rt.Release();
                if (Application.isPlaying)
                    UnityEngine.Object.Destroy(rt);
                else
                    UnityEngine.Object.DestroyImmediate(rt);
            }
            _all.Clear();
        }

        static RenderTextureFormat ToRTFormat(RgFormat format)
        {
            switch (format)
            {
                case RgFormat.ColorRGBAFloat: return RenderTextureFormat.ARGBFloat;
                case RgFormat.ColorRFloat:    return RenderTextureFormat.RFloat;
                case RgFormat.ColorRGFloat:   return RenderTextureFormat.RGFloat;
                case RgFormat.Depth32:        return RenderTextureFormat.Depth;
                default:                      return RenderTextureFormat.ARGBHalf;
            }
        }
    }
}
