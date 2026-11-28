namespace URPPT.Rendering
{
    /// <summary>
    /// 后置段参数。v1 **不序列化到 URP-PathTrace-Renderer.asset**——避免 Renderer 资产里堆参数。
    /// 默认值集中在这里；临时对比时改这些静态字段即可（改完重编译一次）。
    ///
    /// 以后如果要 Inspector 调参，建议单独建一个 ScriptableObject 资产（或独立的配置面板），
    /// 而不是把这些字段塞回 Renderer 资产。
    /// </summary>
    public static class PostGraphConfig
    {
        /// <summary>后置段总开关；false 时走旧的直接 Blit。</summary>
        public static bool Enabled = true;

        /// <summary>tonemap 开关；false 时链退化为一次拷贝，用于和旧路径 A/B。</summary>
        public static bool TonemapEnabled = true;

        /// <summary>线性域曝光倍率（在 tonemap 曲线之前乘上）。</summary>
        public static float TonemapExposure = 1.0f;

        /// <summary>0 = ACES，1 = Reinhard，2 = 直接截断（对照用）。</summary>
        public static int TonemapMode = 0;
    }
}
