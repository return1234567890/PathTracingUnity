// ══════════════════════════════════════════════════════════════
//  VulkanPathTracePlugin — pt.slang
//
//  Ray Tracing Pipeline 版全场景路径追踪：
//    raygen        1 个：每像素 / 每 sample / 每弹射的迭代循环 + 全部着色
//    miss          2 个：天空/环境（rmissMain）、空体穿透（rmissShadow）
//    closest-hit   2 个：主路径只回写命中记录（rchitMain）；阴影被挡置 0（rchitShadow）
//    any-hit       2 个：镂空 alpha cutout（rahitMain）；阴影逐层 Beer + 透射（rahitShadow）
//
//  SBT：maxPipelineRayRecursionDepth = 1（阴影光线从 raygen 发出，不在 CH 里递归）
//       raygen / miss0 / miss1 + hit group 0（CH+AH）/ hit group 1（CH+AH）
//       TraceRay 的 sbtOffset 直接选 group：主路径 0 / 阴影 1
//
//  ｜编译（由 build_shaders.ps1 执行，版本与参数都固定在那里）：
//    slangc pt.slang -target spirv -profile sm_6_6+spirv_1_4 -fvk-use-entrypoint-name
//           -O0 -emit-spirv-directly -o pt.spv
//  ⚠ 必须 -fvk-use-entrypoint-name，否则 7 个入口名被修饰，pName 找不到。
//  ⚠ 不要加 -force-glsl-scalar-layout：会改掉 208B UBO 与 SSBO 的布局。
// ══════════════════════════════════════════════════════════════

#include "pt_common.slang"

// ══════════════════════════════════════════════════════════════
//  G-buffer 写出（Y flip：RA 的 y=0 在顶部，Unity RenderTexture 是自底向上）
// ══════════════════════════════════════════════════════════════

uint2 storeCoordFromPixel(uint2 pixel, uint h)
{
    return uint2(pixel.x, h - 1u - pixel.y);
}

// ══════════════════════════════════════════════════════════════
//  Ray generation — 主循环
// ══════════════════════════════════════════════════════════════

[shader("raygeneration")]
void rgenMain()
{
    uint2 pixel = DispatchRaysIndex().xy;
    // 分辨率只从输出图尺寸取（cam.posFov.w 是 fov，不是宽度）
    uint w = 0u, h = 0u;
    outputImg.GetDimensions(w, h);
    if (pixel.x >= w || pixel.y >= h) return;

    // ── 1. Camera ray generation (with subpixel jitter for DLSS) ──
    // jitterNearFar.xy = Halton jitter in render pixel space [-0.5, 0.5]
    float2 jitter = cam.jitterNearFar.xy;
    float2 uv     = (float2(pixel) + 0.5f + jitter) / float2(w, h);
    float2 ndc    = uv * 2.0f - 1.0f;
    ndc.y         = -ndc.y;

    // row_major 的 UBO 矩阵 → mul(M, v) 等价于 GLSL 的 M * v
    float4 target    = mul(cam.projInv, float4(ndc, 1.0f, 1.0f));
    float3 direction = normalize(mul(cam.viewInv, float4(target.xyz, 0.0f)).xyz);
    float3 origin    = cam.posFov.xyz;

    float nearPlane  = cam.jitterNearFar.z;
    float farPlane   = cam.jitterNearFar.w;
    int   maxDepth   = cam.frameMaxDepth.y;
    uint  frameCount = uint(cam.frameMaxDepth.x);
    uint  spp        = max(cam.samplesPerPixel, 1u);
    float3 ambient   = cam.ambientPad.xyz;
    // HDR 未上传时（占位图是 1×1 黑色）走常数 ambient 回退
    bool  hasEnv     = (cam.frameMaxDepth.z != 0);
    uint  lightCount = cam.lightCount;

    float3 finalRadiance = float3(0.0f);

    // ── DLSS RR specular guide：先把本像素写成「无镜面信息」──
    // 下面只有在 depth 0 真的选中镜面反射 lobe 时才会覆盖它。
    // 写 0 而不是留旧值：guide 每帧被 RR 消费，残留上一帧的距离会让 RR 算错虚拟命中点。
    {
        uint2 guideCoord = storeCoordFromPixel(pixel, h);
        specGuideImg[guideCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    for (uint s = 0u; s < spp; s++)
    {
        float3 rayOrigin    = origin;
        float3 rayDir       = direction;
        uint   seed         = xxhash32(uint3(pixel, frameCount * spp + s));
        float3 radiance     = float3(0.0f);
        float3 throughput   = float3(1.0f);

        // ── Volume 介质状态（每个 sample 独立）──
        // 单介质假设：只支持一层玻璃（教堂窗）；嵌套玻璃/玻璃内水不支持。
        bool   isInsideVolume = false;
        float3 volumeSigma    = float3(0.0f);

        // ── RR specular guide 状态（每个 sample 独立）──
        // depth 0 选中【镜面反射】lobe 时置位并把方向存下来；
        // depth 1 拿到这条射线的命中距离后，两者一起写进 specGuideImg。
        bool   specPending = false;
        float3 specDir     = float3(0.0f);

        for (int depth = 0; depth < maxDepth; depth++)
        {
            // ── Trace primary ray ──
            // tmin：只有相机首段用近裁剪面；后续 bounce 必须从 0 起，
            // 否则 < nearPlane(0.3m) 的几何会被跳过——12mm 玻璃的后表面永远命中不到。
            float tmin = (depth == 0) ? nearPlane : 0.0f;

            RayDesc ray;
            ray.Origin    = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin      = tmin;
            ray.TMax      = farPlane;

            // 不加 RAY_FLAG_FORCE_OPAQUE：镂空材质必须让 any-hit（rahitMain）参与，
            // 否则镂空变实心。不透明实例仍由 TLAS 的实例级 FORCE_OPAQUE 直接 commit，
            // 不产生 any-hit 调用 —— 与旧 ray query 版（不加 Opaque、靠实例标记）一致。
            HitPayload hitPayload;
            hitPayload.hitT = INFINITE_T;
            TraceRay(tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, hitPayload);

            bool committed = (hitPayload.hitT != INFINITE_T);

            // ── DLSS RR specular guide 写出 ──
            // 走到这里时 depth>=1，说明这一跳的射线方向是 depth-1 末尾选出来的。
            // 若那次选中的是镜面反射 lobe，hitT 就是「从主表面沿镜面方向到命中点」的距离。
            //   · 只在 s==0 写（与 G-buffer 一致）：spp>1 时 guide 只反映第 0 个 sample；
            //   · miss 写 0：没有可连接的几何，让 RR 退回主表面重投影。
            // ⚠ maxDepth==1 时循环到不了 depth 1 → guide 恒为 0（等于没接）。
            if (depth == 1 && s == 0u && specPending)
            {
                uint2 guideCoord = storeCoordFromPixel(pixel, h);
                specGuideImg[guideCoord] = float4(specDir, committed ? hitPayload.hitT : 0.0f);
            }

            // ── Miss: HDR 环境贴图查找（或常数 ambient fallback）──
            if (!committed)
            {
                if (depth == 0 && s == 0u)
                {
                    uint2 storeCoord = storeCoordFromPixel(pixel, h);
                    gbufferPosDepth[storeCoord]    = float4(0.0f, 0.0f, 0.0f, 0.0f);
                    gbufferNormalRough[storeCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
                    // sky albedo 填 0.5（RR Guide 明确建议，常见错误是 sky 未清理）
                    diffuseAlbedoImg[storeCoord]   = float4(0.5f, 0.5f, 0.5f, 1.0f);
                    specularAlbedoImg[storeCoord]  = float4(0.5f, 0.5f, 0.5f, 1.0f);
                }
                // HDR env lookup via sphericalUV；HDR 未上传时回退常数 ambient
                float3 envColor = hasEnv
                    ? envHdrTex.SampleLevel(sphericalUV(rayDir), 0.0f).rgb * cam.envIntensity
                    : ambient;
                radiance += throughput * envColor;
                break;
            }

            // ── Hit: reconstruct attributes（raygen 侧回读 SSBO）──
            HitInfo hit      = reconstructHitInfo(hitPayload);
            float3  N        = hit.normal;
            float3  Ngeo     = hit.geoNormal;
            float3  P        = hit.worldPos;
            VPT_Material mat = materials[hit.materialID];

            bool frontFace = dot(rayDir, N) < 0.0f;
            if (!frontFace)
            {
                N    = -N;
                Ngeo = -Ngeo;
            }

            // ── Texture sampling ──
            float2 finalUV = hit.uv * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;

            // Sample textures and overwrite mat fields so BSDF functions use textured values
            float3 emission = mat.emissionColor.rgb;

            // ★ URP Lit 语义：_BaseColor * _BaseMap，RGBA 全乘。
            // alpha 必须一起乘进来，否则 mat.albedo.a 恒为 _BaseColor.a(=1)，
            // 后面的 transmission = 1 - albedo.a 会恒为 0（不透明）。
            float4 baseCol = sampleBaseColor(mat, finalUV);
            mat.albedo     = baseCol;

            if ((mat.flags & MAT_HAS_MR) != 0u && mat.metalRoughTexID != TEX_NONE)
            {
                float4 mr    = metallicRoughTex.SampleLevel(float3(finalUV, float(mat.metalRoughTexID)), 0.0f);
                mat.metallic = mr.r;
                mat.roughness = 1.0f - mr.a; // Unity smoothness → roughness
            }

            if ((mat.flags & MAT_HAS_EMISS) != 0u && mat.emissiveTexID != TEX_NONE)
                emission *= emissiveTex.SampleLevel(float3(finalUV, float(mat.emissiveTexID)), 0.0f).rgb;

            // Tangent to world space
            float3 T              = hit.tangent.xyz;
            float  bitangentSign  = hit.tangent.w >= 0.0f ? 1.0f : -1.0f;

            // Normal map perturbation
            if ((mat.flags & MAT_HAS_NORMAL) != 0u && mat.normalTexID != TEX_NONE)
            {
                float3 tangentNormal = normalTex.SampleLevel(float3(finalUV, float(mat.normalTexID)), 0.0f).rgb * 2.0f - 1.0f;
                float3 B             = cross(N, T) * bitangentSign;
                N = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);
            }

            float3 N_flat = N; // flat normal for visibility checks

            // ★ Transparent: override transmission from base color alpha
            bool isTransparent = ((mat.flags & MAT_IS_TRANSPARENT) != 0u);
            if (isTransparent)
                mat.transmission = 1.0f - mat.albedo.a;

            // ── Volume：上一表面 → 当前命中的 segment 介质吸收（Beer-Lambert）──
            // 必须在任何着色贡献之前乘到 throughput；体积颜色只在这里乘一次，
            // 表面 tint 已中性化（见 btdfSample / btdfEval）。
            if (isInsideVolume)
                throughput *= exp(-hit.hitT * volumeSigma);

            // G-buffer output (first hit, first sample)
            if (depth == 0 && s == 0u)
            {
                uint2 storeCoord = storeCoordFromPixel(pixel, h);
                gbufferPosDepth[storeCoord]    = float4(P, hit.hitT);
                gbufferNormalRough[storeCoord] = float4(Ngeo, mat.roughness);
                // DLSS RR albedo 分解（RR Guide 3.4）：
                //   Diffuse  = albedo * (1 - metallic)
                //   Specular = EnvBRDFApprox(F0, roughness^2, NdotV)，N 为扰动后 shading normal
                float ndv = max(dot(N, -rayDir), 0.0f);
                float3 F0 = lerp(float3(0.04f), mat.albedo.rgb, mat.metallic);
                diffuseAlbedoImg[storeCoord]  = float4(mat.albedo.rgb * (1.0f - mat.metallic), 1.0f);
                specularAlbedoImg[storeCoord] = float4(EnvBRDFApprox(F0, mat.roughness * mat.roughness, ndv), 1.0f);
            }

            // ── Emissive contribution ──
            if ((mat.flags & MAT_HAS_EMISS) != 0u)
            {
                radiance += throughput * emission;
            }

            float3 V     = -rayDir;
            float  NdotV = max(dot(N, V), 0.0f);

            // Prepare ONB for BTDF (same as sampleBSDF internal)
            float3 T_world = T;
            float3 B_world = cross(N, T_world) * bitangentSign;

            // ── NEE: one-sample MIS（灯 50% / HDR 环境 50%）──
            // 注入逐帧蒙特卡洛噪声使 RR 恢复强累积，消除亮区抖动。
            bool sampleLights = (rand(seed) <= 0.5f);

            if (sampleLights && lightCount > 0u)
            {
                // ── 灯采样分支（原逻辑，加 MIS 权重 ×2）──
                uint li = uint(rand(seed) * float(lightCount)) % lightCount;
                VPT_Light L = lights[li];
                float3 toL;
                float  dist;
                float3 Lcolor;
                sampleLightContribution(L, P, toL, dist, Lcolor);
                float NdotL = dot(N, toL);

                // ★ Transparent: allow backside lighting via BTDF
                bool   lightValid = false;
                float3 bsdf_nee   = float3(0.0f);

                if (isTransparent && NdotL <= 0.0f)
                {
                    float absNdotL = abs(NdotL);
                    bsdf_nee = btdfEval(mat, N, T_world, B_world, V, toL, NdotV, absNdotL, isInsideVolume);
                    lightValid = (dot(N_flat, toL) < 0.0f || NdotL > 0.0f);
                    NdotL = absNdotL;
                }
                else if (NdotL > 0.0f)
                {
                    bsdf_nee = evaluateBSDF(mat, N, toL, V, NdotL, NdotV);
                    lightValid = (dot(N_flat, toL) > 0.0f);
                }

                if (lightValid)
                {
                    float3 shadowOrigin = P + Ngeo * surfaceOffset(mat);
                    // ★ transparent-aware shadows（group 1）
                    float3 shadowTrans = traceShadowTransmission(shadowOrigin, toL, dist,
                                                                 isInsideVolume, volumeSigma);
                    // ×lightCount 补偿单灯选取；×2.0 = MIS 权重（1/0.5）
                    radiance += throughput * bsdf_nee * NdotL * Lcolor * shadowTrans
                              * float(lightCount) * 2.0f;
                }
            }
            else if (hasEnv)
            {
                // ── HDR 环境光分支：importance 采样（alias method）──
                float3 envRand   = float3(rand(seed), rand(seed), rand(seed));
                float3 toL;
                float4 envSample = environmentSample(envHdrTex, envRand, toL);
                float3 envRadiance = envSample.rgb;
                float  envPdf      = max(envSample.w, 1e-6f);
                float  NdotL       = dot(N, toL);

                bool   lightValid = false;
                float3 bsdf_nee   = float3(0.0f);

                if (isTransparent && NdotL <= 0.0f)
                {
                    float absNdotL = abs(NdotL);
                    bsdf_nee = btdfEval(mat, N, T_world, B_world, V, toL, NdotV, absNdotL, isInsideVolume);
                    lightValid = (dot(N_flat, toL) < 0.0f || NdotL > 0.0f);
                    NdotL = absNdotL;
                }
                else if (NdotL > 0.0f)
                {
                    bsdf_nee = evaluateBSDF(mat, N, toL, V, NdotL, NdotV);
                    lightValid = (dot(N_flat, toL) > 0.0f);
                }

                if (lightValid)
                {
                    float3 shadowOrigin = P + Ngeo * surfaceOffset(mat);
                    // 环境在无穷远；遮挡=环境遮蔽（AO）/半影
                    float3 shadowTrans = traceShadowTransmission(shadowOrigin, toL, 1e34f,
                                                                 isInsideVolume, volumeSigma);
                    // ×2.0 = MIS coin weight（1/0.5）；radiance/pdf 为 importance 采样估计
                    radiance += throughput * bsdf_nee * NdotL * (envRadiance / envPdf)
                              * shadowTrans * 2.0f;
                }
            }

            // ── BSDF sampling for indirect bounce ──
            float3 newDir;
            float  pdf;
            float3 bsdf;
            // 本次选中的 lobe；默认漫反射（透射分支不会覆盖它，因此不会误写 guide）
            uint   bsdfLobe = LOBE_DIFFUSE_REFLECTION;

            if (isTransparent)
            {
                int lobe = findLobe(mat, dot(N, V), rand(seed));
                if (lobe == LOBE_SPECULAR_TRANSMISSION || lobe == LOBE_DIFFUSE_TRANSMISSION)
                {
                    if (lobe == LOBE_SPECULAR_TRANSMISSION)
                    {
                        btdfSample(mat, N, T_world, B_world, V, seed, isInsideVolume, newDir, pdf, bsdf);
                    }
                    else
                    {
                        // Diffuse transmission: cosine hemisphere on back side
                        newDir = cosineSampleHemisphere(float2(rand(seed), rand(seed)), T_world, B_world, -N);
                        pdf    = abs(dot(newDir, N)) / PI;
                        bsdf   = mat.albedo.rgb * mat.diffuseTransmission;
                    }
                }
                else
                {
                    bsdf = sampleBSDF(mat, N, V, seed, newDir, pdf, bsdfLobe);
                }
            }
            else
            {
                bsdf = sampleBSDF(mat, N, V, seed, newDir, pdf, bsdfLobe);
            }

            // ── RR specular guide：depth 0 的镜面反射 lobe → 下一跳的距离就是 guide ──
            // 透射材质（玻璃）整体排除：折射方向不是「镜面反射方向」，喂给 RR 会被
            // 当成镜面反射去反推虚拟点 → 反射错位。这类像素保持 0（= 退回主表面重投影）。
            if (depth == 0 && !isTransparent && bsdfLobe == LOBE_SPECULAR_REFLECTION)
            {
                specPending = true;
                specDir     = newDir;
            }

            float NdotL_new = max(dot(N, newDir), 0.0f);
            if (isTransparent) NdotL_new = abs(dot(N, newDir));
            if (pdf <= 0.0f || NdotL_new <= 0.0f) break;
            if (!isTransparent && dot(N_flat, newDir) <= 0.0f) break;
            throughput *= bsdf * NdotL_new / pdf;

            // ── Ray offset: transmission vs reflection ──
            bool isTransmission = (dot(newDir, Ngeo) < 0.0f);

            // ── Volume 边界状态机：进入时捕获 σt，离开时清空 ──
            // 单介质假设：只要还是「体积材质 + 透射」，就按进入/离开翻转。反射不改状态。
            // 体积色取命中点逐像素 baseColor（彩色玻璃马赛克逐格着色）。
            if (isTransmission && materialIsVolume(mat))
            {
                if (!isInsideVolume)
                {
                    volumeSigma    = volumeSigmaT(mat, baseCol.rgb);
                    isInsideVolume = true;
                }
                else
                {
                    isInsideVolume = false;
                    volumeSigma    = float3(0.0f);
                }
            }

            float eps = surfaceOffset(mat);
            if (isTransmission)
                rayOrigin = P - Ngeo * eps;
            else
                rayOrigin = P + Ngeo * eps;
            rayDir = newDir;

            // ── Russian roulette (depth > 3) ──
            if (depth > 3)
            {
                float pcont = min(max(max(throughput.x, throughput.y), throughput.z) + 0.001f, 0.95f);
                if (rand(seed) >= pcont) break;
                throughput /= pcont;
            }
        }

        finalRadiance += radiance;
    }

    finalRadiance /= float(spp);

    // Y flip: raygen pixel.y=0 is top, Unity RenderTexture displays bottom-up
    outputImg[storeCoordFromPixel(pixel, h)] = float4(finalRadiance, 1.0f);
}

// ══════════════════════════════════════════════════════════════
//  Miss
// ══════════════════════════════════════════════════════════════

[shader("miss")]
void rmissMain(inout HitPayload payload)
{
    payload.hitT = INFINITE_T;
}

[shader("miss")]
void rmissShadow(inout ShadowPayload payload)
{
    // 空体：保留 any-hit 累积的透射率，不清零
}

// ══════════════════════════════════════════════════════════════
//  Closest hit
// ══════════════════════════════════════════════════════════════

// 主路径：只回写命中记录，材质/BSDF/纹理全部留在 raygen（延迟着色）
[shader("closesthit")]
void rchitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.hitT           = RayTCurrent();
    payload.instanceIndex  = InstanceIndex();
    payload.primitiveIndex = PrimitiveIndex();
    payload.bary           = attr.barycentrics;
}

// 阴影：走到这里说明遍历结束于一个被接受的命中
//   · 不透明实例（实例级 FORCE_OPAQUE，根本没进 rahitShadow）→ 完全遮挡
//   · rahitShadow 已把 totalTransmission 置 0 后 AcceptHitAndEndSearch → 保持 0
//   · rahitShadow 因「层数耗尽」接受（acceptMode == 2）→ 保留已累积值（偏亮，沿用旧行为）
[shader("closesthit")]
void rchitShadow(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    if (payload.acceptMode != 2u)
        payload.totalTransmission = float3(0.0f);
}

// ══════════════════════════════════════════════════════════════
//  Any hit
// ══════════════════════════════════════════════════════════════

// 主路径镂空：alpha < cutoff 的微面直接 IgnoreHit（洞），其余正常返回 = 接受该命中。
// 注意：不透明实例带 FORCE_OPAQUE，不会进这里；透明实例（玻璃）也不带，会以
// 「接受 + 继续找更近命中」的方式参与最近命中竞争，与旧 candidate+Confirm 等价。
[shader("anyhit")]
void rahitMain(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instanceIndex = InstanceIndex();
    InstanceInfo inst  = instances[instanceIndex];
    VPT_Material mat   = materials[inst.materialID];

    if ((mat.flags & MAT_ALPHA_TEST) != 0u)
    {
        float alpha = sampleHitAlpha(attr.barycentrics, inst, mat, PrimitiveIndex());
        if (alpha < mat.alphaCutoff)
            IgnoreHit();
    }
}

// 阴影透射：逐层 Beer 吸收 + 透射材质透过率。
//   · 不透明实例带 FORCE_OPAQUE，不会进这里（由 rchitShadow 置 0）；
//   · 镂空洞 → IgnoreHit（不更新 prevT，也不做段吸收）；
//   · 实心不透明（异常防御）→ 置 0 后 AcceptHitAndEndSearch；
//   · 透射 → 累积 layerT，IgnoreHit 继续找下一层；
//   · 层数 / 透射率耗尽 → AcceptHitAndEndSearch（区别见 acceptMode）。
[shader("anyhit")]
void rahitShadow(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint   instanceIndex  = InstanceIndex();
    uint   primitiveIndex = PrimitiveIndex();
    float  hitT           = RayTCurrent();

    InstanceInfo inst = instances[instanceIndex];
    VPT_Material mat  = materials[inst.materialID];

    const bool isAlphaTest   = (mat.flags & MAT_ALPHA_TEST) != 0u;
    const bool isTransparent = (mat.flags & MAT_IS_TRANSPARENT) != 0u;

    // 层数上限：防止病态场景（上百层玻璃）无底洞；按旧行为返回已累积值（偏亮）
    payload.layerCount++;
    if (payload.layerCount > MAX_SHADOW_LAYERS)
    {
        payload.acceptMode = 2u;
        AcceptHitAndEndSearch();
        return;
    }

    // ── 非透射且非镂空：完全遮挡 ──
    // 正常情况下到不了这里（不透明实例被硬件 commit），留着是防御
    if (!isAlphaTest && !isTransparent)
    {
        payload.totalTransmission = float3(0.0f);
        AcceptHitAndEndSearch();
        return;
    }

    uint i0 = indices[inst.indexOffset + primitiveIndex * 3 + 0];
    uint i1 = indices[inst.indexOffset + primitiveIndex * 3 + 1];
    uint i2 = indices[inst.indexOffset + primitiveIndex * 3 + 2];
    VPT_Vertex v0 = vertices[inst.vertexOffset + i0];
    VPT_Vertex v1 = vertices[inst.vertexOffset + i1];
    VPT_Vertex v2 = vertices[inst.vertexOffset + i2];
    float3 Ngeo = normalize(transformDir(inst,
        normalize(cross(v1.pos - v0.pos, v2.pos - v0.pos))));

    // ── 采样 baseColor：alpha 同时用于镂空判定与透射率 ──
    float2 bary  = attr.barycentrics;
    float2 hitUV = v0.uv * (1.0f - bary.x - bary.y) + v1.uv * bary.x + v2.uv * bary.y;
    float2 uvT   = hitUV * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;
    float4 base  = sampleBaseColor(mat, uvT);

    // ── Alpha cutout：洞 → 丢弃该层；实心 → 遮挡 ──
    // 洞不是介质边界，不能更新 prevT / 做段吸收，否则玻璃内阴影会被切成错误的段。
    if (isAlphaTest && base.a < mat.alphaCutoff)
    {
        IgnoreHit();
        return;
    }
    if (!isTransparent)
    {
        payload.totalTransmission = float3(0.0f);
        AcceptHitAndEndSearch();
        return;
    }

    // ── 介质段吸收：上一表面 → 当前命中（Beer-Lambert）──
    // abs()：any-hit 不保证按 t 排序，不能让乱序把段长算成负数/0。
    bool inside = (payload.isInside != 0u);
    if (inside)
        payload.totalTransmission *= exp(-abs(hitT - payload.prevHitT) * payload.extinction);
    payload.prevHitT = hitT;

    float trans = 1.0f - base.a;
    if (trans <= MIN_TRANSMISSION)
    {
        payload.totalTransmission = float3(0.0f);
        AcceptHitAndEndSearch();
        return;
    }

    bool isVolume = materialIsVolume(mat);

    // ── Surface-only transmission（体积材质不再乘颜色）──
    float3 layerT = float3(trans);                                              // Factor A
    float  cosTheta = abs(dot(WorldRayDirection(), Ngeo));                      // Factor B
    float  fresnel  = schlickFresnelScalar(mat.ior, cosTheta);
    layerT *= (1.0f - fresnel);
    if (!isVolume)                                                             // Factor C
        layerT *= base.rgb;

    // Factor D: 体积边界状态切换（进入时捕获 σt，离开时清空）
    if (isVolume)
    {
        if (!inside)
        {
            payload.extinction = volumeSigmaT(mat, base.rgb);
            payload.isInside   = 1u;
        }
        else
        {
            payload.isInside   = 0u;
            payload.extinction = float3(0.0f);
        }
    }

    // Factor E: roughness/metallic attenuation
    layerT *= (1.0f - mat.metallic);
    float roughnessEffect = 1.0f - (mat.roughness * mat.roughness);
    layerT *= lerp(0.65f, 1.0f, roughnessEffect);

    payload.totalTransmission *= layerT;

    // Early termination if too low
    float maxComp = max(max(payload.totalTransmission.r, payload.totalTransmission.g), payload.totalTransmission.b);
    if (maxComp <= MIN_TRANSMISSION)
    {
        payload.totalTransmission = float3(0.0f);
        AcceptHitAndEndSearch();
        return;
    }

    // 不 confirm → 该层被丢弃，遍历自动继续到下一层
    IgnoreHit();
}
