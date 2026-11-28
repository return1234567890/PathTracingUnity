using UnityEngine;

namespace URPPT.Rendering
{
    /// <summary>
    /// 后置 Pass 图中的资源句柄。Id 指向逻辑资源槽，Version 是写入次数（v1 里基本恒为 1）。
    /// 句柄是值类型：pass 之间靠它显式传递，不靠"字段里塞最新的 RT"这种口头约定。
    /// </summary>
    public readonly struct RgTexture
    {
        public readonly int Id;
        public readonly int Version;

        public RgTexture(int id, int version)
        {
            Id = id;
            Version = version;
        }

        public bool IsValid => Id != 0;
    }

    public enum RgSize
    {
        /// <summary>相机输出分辨率。</summary>
        Output = 0,
        /// <summary>输出分辨率的某个比例（bloom mip 链用）。</summary>
        Fraction = 1,
        /// <summary>显式宽高。</summary>
        Absolute = 2,
    }

    public enum RgFormat
    {
        ColorRGBAHalf = 0,
        ColorRGBAFloat = 1,
        ColorRFloat = 2,
        ColorRGFloat = 3,
        Depth32 = 4,
    }

    /// <summary>图内创建资源时的描述。导入资源不需要它（直接带 RT 进来）。</summary>
    public sealed class RgTextureDesc
    {
        public string Name = "RgTexture";
        public RgSize Size = RgSize.Output;
        public float Fraction = 1f;
        public int Width;
        public int Height;
        public RgFormat Format = RgFormat.ColorRGBAHalf;
        public bool RandomWrite;
        public FilterMode Filter = FilterMode.Point;
    }
}
