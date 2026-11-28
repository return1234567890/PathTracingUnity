using System;
using System.Collections.Generic;
using UnityEngine;

namespace URPPT.Rendering
{
    internal sealed class RgResource
    {
        public int Id;
        public string Name;
        public RgTextureDesc Desc;        // 图内创建的资源
        public RenderTexture Imported;    // 导入的资源（只读）
        public bool External => Imported != null;
        public bool Written;
        public int WriteCount;
    }

    internal sealed class CompiledPlan
    {
        public RgResource[] Resources;
        public PostPass[] Passes;
        public RgTexture Present;
        public RenderTexture[] Textures;
    }

    /// <summary>
    /// 链的组装期 API：Import / Create / Read / Add / Present。
    /// 只在 Rebuild 时调用一次；每帧执行不会再走这里。
    /// </summary>
    public sealed class PostGraphBuilder
    {
        readonly int _outW;
        readonly int _outH;
        readonly List<RgResource> _resources = new List<RgResource>();
        readonly List<PostPass> _passes = new List<PostPass>();
        readonly List<string> _errors = new List<string>();
        readonly Dictionary<string, int> _names = new Dictionary<string, int>();

        PostPass _current;
        RgTexture _present;
        int _presentCount;

        internal PostGraphBuilder(int outW, int outH)
        {
            _outW = Mathf.Max(1, outW);
            _outH = Mathf.Max(1, outH);
        }

        /// <summary>导入外部 RT（只读）。</summary>
        public RgTexture Import(RenderTexture rt, string name)
        {
            if (rt == null)
            {
                _errors.Add($"Import '{name}': texture is null");
                return default;
            }
            if (!CheckName(name)) return default;

            var r = new RgResource
            {
                Id = _resources.Count + 1,
                Name = name,
                Imported = rt,
                Written = true,
            };
            _resources.Add(r);
            return new RgTexture(r.Id, 1);
        }

        /// <summary>创建一张图内资源（输出分辨率 / 比例 / 绝对尺寸）。</summary>
        public RgTexture Create(RgTextureDesc desc)
        {
            if (desc == null)
            {
                _errors.Add("Create: desc is null");
                return default;
            }
            if (!CheckName(desc.Name)) return default;

            var r = new RgResource
            {
                Id = _resources.Count + 1,
                Name = desc.Name,
                Desc = desc,
            };
            _resources.Add(r);
            return new RgTexture(r.Id, 1);
        }

        public void Read(RgTexture t)
        {
            if (_current == null)
            {
                _errors.Add("Read outside of a pass");
                return;
            }
            var r = Get(t);
            if (r == null)
            {
                _errors.Add($"Pass '{_current.Name}' reads invalid handle (id={t.Id})");
                return;
            }
            if (!r.External && !r.Written)
                _errors.Add($"Pass '{_current.Name}' reads '{r.Name}' before it is written");
        }

        /// <summary>原地写：Id 不变、Version+1。只允许图内资源。</summary>
        public RgTexture ReadWrite(RgTexture t)
        {
            var r = Get(t);
            if (r == null)
            {
                _errors.Add("ReadWrite: invalid handle");
                return t;
            }
            if (r.External)
            {
                _errors.Add($"ReadWrite: '{r.Name}' is imported and read-only");
                return t;
            }
            Read(t);
            r.Written = true;
            r.WriteCount++;
            var next = new RgTexture(t.Id, t.Version + 1);
            if (_current != null) _current.Output = next;
            return next;
        }

        /// <summary>注册一个 pass，返回它的输出句柄。Enabled=false 时直接返回输入。</summary>
        public RgTexture Add(PostPass pass)
        {
            if (pass == null)
            {
                _errors.Add("Add: pass is null");
                return default;
            }
            if (!pass.Enabled)
                return pass.Input;

            _current = pass;
            pass.Output = default;
            pass.Declare(this);
            _current = null;

            if (!pass.Output.IsValid)
            {
                _errors.Add($"Pass '{pass.Name}' declared no output");
            }
            else
            {
                var r = Get(pass.Output);
                if (r == null)
                {
                    _errors.Add($"Pass '{pass.Name}' wrote an invalid handle");
                }
                else
                {
                    r.Written = true;
                    r.WriteCount++;
                }
            }

            _passes.Add(pass);
            return pass.Output;
        }

        /// <summary>指定最终上屏的颜色，必须恰好一次。</summary>
        public void Present(RgTexture color)
        {
            _presentCount++;
            _present = color;
        }

        internal CompiledPlan Compile()
        {
            if (_presentCount != 1)
                _errors.Add($"Present must be called exactly once (got {_presentCount})");

            var present = Get(_present);
            if (present == null)
            {
                _errors.Add("Present: invalid color handle");
            }
            else if (!present.External && !present.Written)
            {
                _errors.Add($"Present: '{present.Name}' was never written");
            }

            var passNames = new HashSet<string>();
            foreach (var p in _passes)
            {
                if (!passNames.Add(p.Name))
                    _errors.Add($"Duplicate pass name '{p.Name}'");
            }

            if (_errors.Count > 0)
            {
                foreach (var e in _errors)
                    Debug.LogError("[PostGraph] " + e);
                return null;
            }

            return new CompiledPlan
            {
                Resources = _resources.ToArray(),
                Passes = _passes.ToArray(),
                Present = _present,
            };
        }

        bool CheckName(string name)
        {
            if (string.IsNullOrEmpty(name))
            {
                _errors.Add("Resource name is empty");
                return false;
            }
            if (_names.ContainsKey(name))
            {
                _errors.Add($"Duplicate resource name '{name}'");
                return false;
            }
            _names[name] = 1;
            return true;
        }

        RgResource Get(RgTexture t)
        {
            if (!t.IsValid || t.Id < 1 || t.Id > _resources.Count)
                return null;
            return _resources[t.Id - 1];
        }
    }
}
