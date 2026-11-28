Shader "Hidden/URPPT/PostTonemap"
{
    Properties
    {
        _MainTex ("Source", 2D) = "white" {}
    }

    SubShader
    {
        Cull Off ZWrite Off ZTest Always

        Pass
        {
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment frag
            #pragma target 3.0
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float _Exposure;
            float _Mode;   // 0 = ACES, 1 = Reinhard, 2 = clamp

            float3 TonemapACES(float3 x)
            {
                const float a = 2.51;
                const float b = 0.03;
                const float c = 2.43;
                const float d = 0.59;
                const float e = 0.14;
                return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
            }

            float3 TonemapReinhard(float3 x)
            {
                return x / (1.0 + x);
            }

            fixed4 frag (v2f_img i) : SV_Target
            {
                float3 c = tex2D(_MainTex, i.uv).rgb;
                c = max(c, 0.0) * _Exposure;

                if (_Mode < 0.5)      c = TonemapACES(c);
                else if (_Mode < 1.5) c = TonemapReinhard(c);
                else                  c = saturate(c);

                return fixed4(c, 1.0);
            }
            ENDCG
        }
    }

    Fallback Off
}
