#version 460
#extension GL_EXT_ray_query : require

// ══════════════════════════════════════════════════════════════
//  VulkanPathTracePlugin — Path Tracing Compute Shader
//  Multi-bounce PBR with NEE, GGX BSDF, Russian roulette.
//  Uses VK_KHR_ray_query for hardware-accelerated ray traversal.
//  Phase 3: opaque-only, no texture arrays (uniform material values).
// ══════════════════════════════════════════════════════════════

#define PI 3.14159265359
#define MISS_INDEX 0xFFFFFFFFu
#define EPS_OFFSET 5e-3

#define MAT_HAS_BASE       1u
#define MAT_HAS_MR         2u
#define MAT_HAS_NORMAL     4u
#define MAT_HAS_EMISS      8u
#define MAT_IS_TRANSPARENT 16u
#define TEX_NONE       0xFFFFFFFFu

// ── Descriptor Set 0 ──────────────────────────────────────────
layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, rgba16f)  writeonly uniform image2D outputImg;
layout(set = 0, binding = 2, rgba32f)  writeonly uniform image2D gbufferPosDepth;
layout(set = 0, binding = 3, rgba16f)  writeonly uniform image2D gbufferNormalRough;

layout(set = 0, binding = 4, std140, row_major) uniform CameraUBO {
    mat4 viewInv;        // camera-to-world
    mat4 projInv;        // inverse projection
    vec4 posFov;         // xyz = camera position, w = fov
    vec4 jitterNearFar;  // xy = jitter, z = near plane, w = far plane
    ivec4 frameMaxDepth; // x = frameCount, y = maxDepth
    vec4 ambientPad;     // xyz = ambient color, w = pad
    uint lightCount;
    uint samplesPerPixel;
    uint _uboPad0;
    uint _uboPad1;
} cam;

// ── SSBOs ─────────────────────────────────────────────────────
struct VPT_Vertex {
    vec3 pos;       float pad0;
    vec3 normal;    float pad1;
    vec4 tangent;
    vec2 uv;        vec2 pad2;
    vec4 color;
};

struct VPT_Material {
    vec4  albedo;
    float metallic;
    float roughness;
    float bumpScale;
    uint  flags;
    vec4  emissionColor;
    uint  baseTexID;
    uint  metalRoughTexID;
    uint  normalTexID;
    uint  emissiveTexID;
    vec4  uvScaleOffset;
    float transmission;
    float ior;
    float diffuseTransmission;
    uint  thinWalled;
};

struct VPT_Light {
    vec3  positionOrDir;
    float range;
    vec3  color;
    uint  type;
};

struct InstanceInfo {
    vec4 transformRow0;
    vec4 transformRow1;
    vec4 transformRow2;
    uint vertexOffset;
    uint indexOffset;
    uint materialID;
    uint pad;
};

layout(set = 0, binding = 5, std430) readonly buffer VertexBuffer {
    VPT_Vertex vertices[];
};
layout(set = 0, binding = 6, std430) readonly buffer IndexBuffer {
    uint indices[];
};
layout(set = 0, binding = 7, std430) readonly buffer InstanceInfoBuffer {
    InstanceInfo instances[];
};
layout(set = 0, binding = 8, std430) readonly buffer MaterialBuffer {
    VPT_Material materials[];
};
layout(set = 0, binding = 9, std430) readonly buffer LightBuffer {
    VPT_Light lights[];
};

// ── Texture Arrays (Phase 4) ───────────────────────────────
layout(set = 0, binding = 10) uniform sampler2DArray baseColorTex;
layout(set = 0, binding = 11) uniform sampler2DArray metallicRoughTex;
layout(set = 0, binding = 12) uniform sampler2DArray normalTex;
layout(set = 0, binding = 13) uniform sampler2DArray emissiveTex;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// ══════════════════════════════════════════════════════════════
//  PRNG
// ══════════════════════════════════════════════════════════════

uint xxhash32(uvec3 p)
{
    const uint P1 = 0x9E3779B1u, P2 = 0x85EBCA77u, P3 = 0xC2B2AE3Du, P5 = 0x165667B1u;
    uint h = P5 + (p.x + p.y) * P1;
    h ^= p.z; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    h ^= p.x; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    h ^= p.y; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}

float rand(inout uint seed)
{
    seed = seed * 1664525u + 1013904223u;
    return float(seed >> 8) * (1.0 / 16777216.0);
}

// ══════════════════════════════════════════════════════════════
//  Geometry helpers
// ══════════════════════════════════════════════════════════════

void createONB(vec3 N, out vec3 T, out vec3 B)
{
    vec3 up = abs(N.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}

vec3 cosineSampleHemisphere(vec2 u, vec3 T, vec3 B, vec3 N)
{
    float phi = 2.0 * PI * u.x;
    float r = sqrt(u.y);
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u.y));
    return normalize(x * T + y * B + z * N);
}

// Transform a direction from object space to world space using InstanceInfo 3x4 row-major matrix
vec3 transformDir(InstanceInfo info, vec3 dir)
{
    return vec3(
        dot(info.transformRow0.xyz, dir),
        dot(info.transformRow1.xyz, dir),
        dot(info.transformRow2.xyz, dir)
    );
}

// ══════════════════════════════════════════════════════════════
//  GGX BSDF
// ══════════════════════════════════════════════════════════════

float D_GGX(float NdotH, float a2)
{
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_SmithGGX(float NdotV, float NdotL, float a2)
{
    float g1v = 2.0 * NdotV / (NdotV + sqrt(a2 + (1.0 - a2) * NdotV * NdotV));
    float g1l = 2.0 * NdotL / (NdotL + sqrt(a2 + (1.0 - a2) * NdotL * NdotL));
    return g1v * g1l;
}

vec3 F_Schlick(vec3 F0, float cosTheta)
{
    return F0 + (1.0 - F0) * pow(1.0 - max(cosTheta, 0.0), 5.0);
}

vec3 specularF0(vec3 albedo, float metallic)
{
    return mix(vec3(0.04), albedo, metallic);
}

// Evaluate BSDF for NEE (returns BSDF value, no NdotL factor)
vec3 evaluateBSDF(VPT_Material mat, vec3 N, vec3 L, vec3 V, float NdotL, float NdotV)
{
    vec3 albedo = mat.albedo.rgb;
    float metallic = mat.metallic;
    float roughness = mat.roughness;
    vec3 F0 = specularF0(albedo, metallic);

    vec3 diffuse = (1.0 - metallic) * albedo / PI;
    diffuse *= (1.0 - mat.transmission);  // transparent: reduce reflect energy

    vec3 H = normalize(L + V);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float D = D_GGX(NdotH, a2);
    float G = G_SmithGGX(NdotV, NdotL, a2);
    vec3 F = F_Schlick(F0, VdotH);
    vec3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-6);

    return diffuse + specular;
}

// Sample BSDF for indirect bounce (returns BSDF value and pdf)
vec3 sampleBSDF(VPT_Material mat, vec3 N, vec3 V,
               inout uint seed, out vec3 wi, out float pdf)
{
    vec3 albedo = mat.albedo.rgb;
    float metallic = mat.metallic;
    float roughness = mat.roughness;
    vec3 F0 = specularF0(albedo, metallic);
    float a = roughness * roughness;
    float a2 = a * a;

    float pd = 1.0 - metallic;
    float r = rand(seed);

    vec3 T, B;
    createONB(N, T, B);

    float NdotV = max(dot(N, V), 0.0);

    if (r < pd)
    {
        wi = cosineSampleHemisphere(vec2(rand(seed), rand(seed)), T, B, N);
    }
    else
    {
        float r1 = rand(seed), r2 = rand(seed);
        float phi = 2.0 * PI * r1;
        float cosTheta = sqrt(max(0.0, (1.0 - r2) / (1.0 + (a2 - 1.0) * r2)));
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        vec3 H = normalize(sinTheta * cos(phi) * T
                         + sinTheta * sin(phi) * B
                         + cosTheta * N);
        wi = reflect(-V, H);
    }

    float NdotL = max(dot(N, wi), 0.0);
    if (NdotL <= 0.0) { pdf = 0.0; return vec3(0.0); }

    vec3 H = normalize(wi + V);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float D = D_GGX(NdotH, a2);
    float G = G_SmithGGX(NdotV, NdotL, a2);
    vec3 F = F_Schlick(F0, VdotH);
    vec3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-6);
    vec3 diffuse = (1.0 - metallic) * albedo / PI;
    diffuse *= (1.0 - mat.transmission);  // transparent: reduce reflect energy
    vec3 bsdf = diffuse + specular;

    float pdf_diffuse = NdotL / PI;
    float pdf_specular = (VdotH > 0.0) ? (D * NdotH / (4.0 * VdotH)) : 0.0;
    pdf = pd * pdf_diffuse + (1.0 - pd) * pdf_specular;

    return bsdf;
}

// ══════════════════════════════════════════════════════════════
//  Transparent BSDF: lobe weights / BTDF / transparent shadow
// ══════════════════════════════════════════════════════════════

#define LOBE_DIFFUSE_REFLECTION    0
#define LOBE_SPECULAR_REFLECTION   1
#define LOBE_SPECULAR_TRANSMISSION 2
#define LOBE_DIFFUSE_TRANSMISSION  3
#define LOBE_COUNT                 4

#define MIN_TRANSMISSION 0.01
#define MAX_SHADOW_LAYERS 16

// Schlick Fresnel (scalar, for lobe weight allocation)
float schlickFresnelScalar(float ior, float cosTheta)
{
    float r0 = (1.0 - ior) / (1.0 + ior);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - max(cosTheta, 0.0), 5.0);
}

// Compute 4-lobe weights: x=diffuse_refl, y=spec_refl, z=spec_trans, w=diff_trans
vec4 computeLobeWeights(VPT_Material mat, float VdotN)
{
    float fr = schlickFresnelScalar(mat.ior, abs(VdotN));
    float weightBase = 1.0;
    vec4 w = vec4(0.0);

    // Layer 1: metallic reflection
    w.y = weightBase * mat.metallic;
    weightBase *= (1.0 - mat.metallic);

    // Layer 2: dielectric specular (Fresnel)
    w.y += weightBase * fr;
    weightBase *= (1.0 - fr);

    // Layer 3: specular transmission
    w.z = weightBase * mat.transmission;

    // Layer 4: remaining split between diffuse refl and diffuse trans
    float remain = weightBase * (1.0 - mat.transmission);
    w.w = remain * mat.diffuseTransmission;
    w.x = remain * (1.0 - mat.diffuseTransmission);

    return w;
}

// Randomly select a lobe based on weights
int findLobe(VPT_Material mat, float VdotN, float rndVal)
{
    vec4 w = computeLobeWeights(mat, VdotN);
    float weight = 0.0;
    for (int i = LOBE_COUNT - 1; i > 0; i--)
    {
        weight += w[i];
        if (rndVal < weight) return i;
    }
    return LOBE_DIFFUSE_REFLECTION;
}

// Snell refraction; returns refracted dir, TIR returns vec3(0)
vec3 refractDir(vec3 I, vec3 N, float eta, float cosI, out bool tir)
{
    tir = false;
    float k = 1.0 - eta * eta * (1.0 - cosI * cosI);
    if (k < 0.0) { tir = true; return vec3(0.0); }
    return eta * I + (eta * cosI - sqrt(k)) * N;
}

// ── BTDF sampling (specular transmission) ──
// Outputs raw BSDF and pdf for the transmitted lobe.
void btdfSample(VPT_Material mat, vec3 N, vec3 T, vec3 B,
               vec3 k1, inout uint seed,
               out vec3 k2, out float pdf, out vec3 bsdf)
{
    bsdf = vec3(0.0);
    pdf = 0.0;
    k2 = vec3(0.0);

    bool thinWalled = (mat.thinWalled != 0u);
    float ior1 = 1.0;
    float ior2 = mat.ior;
    float VdotN = dot(k1, N);
    bool entering = (VdotN > 0.0);
    float eta = entering ? (ior1 / ior2) : (ior2 / ior1);

    float nk1 = abs(VdotN);

    // VNDF sample half-vector H
    float a = mat.roughness * mat.roughness;
    float a2 = a * a;
    float r1 = rand(seed), r2 = rand(seed);
    float phi = 2.0 * PI * r1;
    float cosThetaH = sqrt(max(0.0, (1.0 - r2) / (1.0 + (a2 - 1.0) * r2)));
    float sinThetaH = sqrt(max(0.0, 1.0 - cosThetaH * cosThetaH));
    vec3 h0 = normalize(vec3(sinThetaH * cos(phi), sinThetaH * sin(phi), cosThetaH));
    vec3 H = h0.x * T + h0.y * B + h0.z * N;

    float k1h = dot(k1, H);
    if (k1h <= 0.0) return;

    bool tir = false;
    if (thinWalled)
    {
        k2 = reflect(-k1, H);
        k2 = normalize(k2 - 2.0 * N * dot(k2, N));
    }
    else
    {
        vec3 refN = entering ? H : -H;
        k2 = refractDir(-k1, refN, eta, k1h, tir);
    }

    if (tir)
        k2 = reflect(-k1, H);

    float nk2 = abs(dot(k2, N));
    if (nk2 <= 1e-6 || isnan(k2.x)) return;

    float fresnel = schlickFresnelScalar(mat.ior, k1h);
    float transProb = 1.0 - fresnel;

    float G = G_SmithGGX(nk1, nk2, a2);
    float D = D_GGX(max(dot(N, H), 0.0), a2);

    if (!thinWalled && !tir)
    {
        float k2h = abs(dot(k2, H));
        float tmp = k1h * ior1 - k2h * ior2;
        if (abs(tmp) > 1e-6)
            pdf = D * max(dot(N,H),0.0) * G * k1h * k2h / (nk1 * max(dot(N,H),1e-6) * tmp * tmp);
    }
    else
    {
        pdf = D * max(dot(N,H),0.0) * G * 0.25 / (nk1 * max(dot(N,H),1e-6));
    }

    if (pdf <= 1e-6) return;

    if (thinWalled || tir)
        bsdf = mat.albedo.rgb * G * pdf;
    else
        bsdf = mat.albedo.rgb * G * transProb * pdf;
}

// ── BTDF evaluation (for NEE) ──
vec3 btdfEval(VPT_Material mat, vec3 N, vec3 T, vec3 B,
             vec3 k1, vec3 k2, float NdotV, float NdotL)
{
    bool thinWalled = (mat.thinWalled != 0u);
    float ior1 = 1.0, ior2 = mat.ior;

    bool backside = (dot(k2, N) < 0.0);
    if (!backside) return vec3(0.0);

    vec3 H = normalize(k1 + k2);
    if (thinWalled) H = normalize(k1 - k2);

    float NdotH = max(dot(N, H), 0.0);
    float k1h = max(dot(k1, H), 0.0);
    float k2h = abs(dot(k2, H));
    if (NdotH <= 0.0 || k1h <= 0.0) return vec3(0.0);

    float a = mat.roughness * mat.roughness;
    float a2 = a * a;
    float D = D_GGX(NdotH, a2);
    float G = G_SmithGGX(NdotV, NdotL, a2);
    float fresnel = schlickFresnelScalar(mat.ior, k1h);

    if (!thinWalled)
    {
        float tmp = k1h * ior1 - k2h * ior2;
        if (abs(tmp) < 1e-6) return vec3(0.0);
        float b = (1.0 - fresnel) * G * D * k1h * k2h / (NdotV * NdotH * tmp * tmp);
        return mat.albedo.rgb * b;
    }
    else
    {
        float b = (1.0 - fresnel) * G * D * 0.25 / (NdotV * NdotH);
        return mat.albedo.rgb * b;
    }
}

// ── Transparent shadow traversal: accumulate transmission through all layers ──
// Returns rgb transmission: (1,1,1)=fully transparent, (0,0,0)=fully occluded
vec3 traceShadowTransmission(vec3 origin, vec3 dir, float maxT)
{
    vec3 totalTransmission = vec3(1.0);
    bool isInside = false;

    for (int iter = 0; iter < MAX_SHADOW_LAYERS; iter++)
    {
        // ── 1. Cast closest-hit ray via rayQuery ──
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, tlas,
            gl_RayFlagsOpaqueEXT, 0xFF,
            origin, 0.0, dir, maxT);
        while (rayQueryProceedEXT(rq)) {}

        bool committed = rayQueryGetIntersectionTypeEXT(rq, true)
                       != gl_RayQueryCommittedIntersectionNoneEXT;

        // ── 2. Miss: ray reached light, return accumulated transmission ──
        if (!committed)
            return totalTransmission;

        // ── 3. Get hit material and geometry ──
        uint instanceIndex = rayQueryGetIntersectionInstanceIdEXT(rq, true);
        uint primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
        float hitT = rayQueryGetIntersectionTEXT(rq, true);

        InstanceInfo inst = instances[instanceIndex];
        VPT_Material mat = materials[inst.materialID];

        uint i0 = indices[inst.indexOffset + primitiveIndex * 3 + 0];
        uint i1 = indices[inst.indexOffset + primitiveIndex * 3 + 1];
        uint i2 = indices[inst.indexOffset + primitiveIndex * 3 + 2];
        VPT_Vertex v0 = vertices[inst.vertexOffset + i0];
        VPT_Vertex v1 = vertices[inst.vertexOffset + i1];
        VPT_Vertex v2 = vertices[inst.vertexOffset + i2];
        vec3 Ngeo = normalize(transformDir(inst,
            normalize(cross(v1.pos - v0.pos, v2.pos - v0.pos))));

        vec3 P = origin + dir * hitT;

        // ── 4. Opaque material: fully occluded, terminate ──
        if ((mat.flags & MAT_IS_TRANSPARENT) == 0u || mat.transmission <= MIN_TRANSMISSION)
            return vec3(0.0);

        // ── 5. Compute per-layer transmission (5 factors) ──

        // Factor A: base transmission
        vec3 layerT = vec3(mat.transmission);

        // Factor B: Fresnel reflection loss
        float cosTheta = abs(dot(dir, Ngeo));
        float fresnel = schlickFresnelScalar(mat.ior, cosTheta);
        layerT *= (1.0 - fresnel);

        // Factor C: color tint
        layerT *= mat.albedo.rgb;

        // Factor D: volume absorption (Beer's law) — only when propagating inside
        if (mat.thinWalled == 0u && isInside)
        {
            vec3 absorbColor = max(mat.albedo.rgb, vec3(0.001));
            vec3 absCoeff = -log(absorbColor) / max(mat.ior * 2.0, 0.1);
            layerT *= exp(-hitT * absCoeff);
            isInside = !isInside;
        }
        else if (mat.thinWalled == 0u)
        {
            isInside = !isInside;
        }

        // Factor E: roughness/metallic attenuation
        layerT *= (1.0 - mat.metallic);
        float roughnessEffect = 1.0 - (mat.roughness * mat.roughness);
        layerT *= mix(0.65, 1.0, roughnessEffect);

        // ── 6. Accumulate transmission ──
        totalTransmission *= layerT;

        // ── 7. Early termination if too low ──
        float maxComp = max(max(totalTransmission.r, totalTransmission.g), totalTransmission.b);
        if (maxComp <= MIN_TRANSMISSION)
            return vec3(0.0);

        // ── 8. Continue from hit point ──
        float facing = dot(dir, Ngeo);
        origin = P + Ngeo * sign(facing) * EPS_OFFSET;
        maxT -= hitT;
        if (maxT <= EPS_OFFSET) break;
    }

    return totalTransmission;
}

// ══════════════════════════════════════════════════════════════
//  Light sampling
// ══════════════════════════════════════════════════════════════

void sampleLightContribution(VPT_Light L, vec3 P,
                             out vec3 toL, out float dist, out vec3 Lcolor)
{
    if (L.type == 1u) // directional
    {
        toL = L.positionOrDir;
        dist = 1e34;
        Lcolor = L.color;
    }
    else // point
    {
        vec3 d = L.positionOrDir - P;
        float dist2 = dot(d, d);
        dist = sqrt(dist2);
        toL = d / max(dist, 1e-6);
        float atten = 1.0;
        if (L.range < 1e33)
        {
            float rr = dist / L.range;
            atten = max(0.0, 1.0 - rr * rr);
        }
        Lcolor = L.color * atten / max(dist2, 1e-6);
    }
}

// ══════════════════════════════════════════════════════════════
//  Shadow ray via rayQuery (opaque, terminate-on-first-hit)
// ══════════════════════════════════════════════════════════════

bool testShadow(vec3 origin, vec3 dir, float maxT)
{
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0xFF, origin, 0.0, dir, maxT);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT;
}

// ══════════════════════════════════════════════════════════════
//  Hit point attribute reconstruction from SSBOs
// ══════════════════════════════════════════════════════════════

struct HitInfo {
    vec3  worldPos;
    vec3  normal;       // interpolated world normal
    vec3  geoNormal;   // geometry face normal (world)
    vec2  uv;
    vec4  tangent;     // world-space tangent
    uint  materialID;
    float hitT;
    bool  hasHit;
};

HitInfo getHitInfo(rayQueryEXT rq)
{
    HitInfo info;
    info.hasHit = false;

    uint instanceIndex = rayQueryGetIntersectionInstanceIdEXT(rq, true);
    uint primitiveIndex = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
    vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    float t = rayQueryGetIntersectionTEXT(rq, true);

    InstanceInfo inst = instances[instanceIndex];
    info.materialID = inst.materialID;
    info.hitT = t;

    uint i0 = indices[inst.indexOffset + primitiveIndex * 3 + 0];
    uint i1 = indices[inst.indexOffset + primitiveIndex * 3 + 1];
    uint i2 = indices[inst.indexOffset + primitiveIndex * 3 + 2];

    VPT_Vertex v0 = vertices[inst.vertexOffset + i0];
    VPT_Vertex v1 = vertices[inst.vertexOffset + i1];
    VPT_Vertex v2 = vertices[inst.vertexOffset + i2];

    float w0 = 1.0 - bary.x - bary.y;
    float w1 = bary.x;
    float w2 = bary.y;

    // Interpolate object-space attributes
    vec3 objPos    = v0.pos * w0 + v1.pos * w1 + v2.pos * w2;
    vec3 objNormal = v0.normal * w0 + v1.normal * w1 + v2.normal * w2;
    vec2 uv        = v0.uv * w0 + v1.uv * w1 + v2.uv * w2;
    vec4 objTang   = v0.tangent * w0 + v1.tangent * w1 + v2.tangent * w2;

    // Transform to world space
    info.worldPos  = vec3(
        dot(inst.transformRow0.xyz, objPos) + inst.transformRow0.w,
        dot(inst.transformRow1.xyz, objPos) + inst.transformRow1.w,
        dot(inst.transformRow2.xyz, objPos) + inst.transformRow2.w
    );
    info.normal    = normalize(transformDir(inst, objNormal));
    info.geoNormal = normalize(transformDir(inst,
        normalize(cross(v1.pos - v0.pos, v2.pos - v0.pos))));
    info.uv        = uv;
    info.tangent   = vec4(normalize(transformDir(inst, objTang.xyz)), objTang.w);
    info.hasHit    = true;

    return info;
}

// ══════════════════════════════════════════════════════════════
//  Main kernel
// ══════════════════════════════════════════════════════════════

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    uint width  = uint(cam.posFov.w > 0.0 ? cam.posFov.w : 0.0); // placeholder, use actual width
    // We store resolution in jitterNearFar? No. Let's use a simpler approach:
    // The actual width/height come from the image extents.
    ivec2 imgSize = imageSize(outputImg);
    if (pixel.x >= uint(imgSize.x) || pixel.y >= uint(imgSize.y)) return;
    uint w = uint(imgSize.x);
    uint h = uint(imgSize.y);

    // ── 1. Camera ray generation ──
    vec2 uv = (vec2(pixel) + 0.5) / vec2(w, h);
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;

    vec4 target = cam.projInv * vec4(ndc, 1.0, 1.0);
    vec3 direction = normalize((cam.viewInv * vec4(target.xyz, 0.0)).xyz);
    vec3 origin = cam.posFov.xyz;

    float nearPlane = cam.jitterNearFar.z;
    float farPlane  = cam.jitterNearFar.w;
    int   maxDepth  = cam.frameMaxDepth.y;
    uint  frameCount = uint(cam.frameMaxDepth.x);
    uint  spp        = max(cam.samplesPerPixel, 1u);
    vec3  ambient    = cam.ambientPad.xyz;
    uint  lightCount = cam.lightCount;

    vec3 finalRadiance = vec3(0.0);

    for (uint s = 0u; s < spp; s++)
    {
        vec3 rayOrigin = origin;
        vec3 rayDir = direction;
        uint seed = xxhash32(uvec3(pixel.x, pixel.y, frameCount * spp + s));
        vec3 radiance = vec3(0.0);
        vec3 throughput = vec3(1.0);

        for (int depth = 0; depth < maxDepth; depth++)
        {
            // ── Trace primary ray ──
            rayQueryEXT rq;
            rayQueryInitializeEXT(rq, tlas,
                gl_RayFlagsOpaqueEXT, 0xFF,
                rayOrigin, nearPlane,
                rayDir, farPlane);
            while (rayQueryProceedEXT(rq)) {}

            bool committed = rayQueryGetIntersectionTypeEXT(rq, true)
                           != gl_RayQueryCommittedIntersectionNoneEXT;

            // ── Miss: ambient color ──
            if (!committed)
            {
                if (depth == 0 && s == 0u)
                {
                    imageStore(gbufferPosDepth, ivec2(pixel.x, int(h) - 1 - int(pixel.y)), vec4(0.0));
                    imageStore(gbufferNormalRough, ivec2(pixel.x, int(h) - 1 - int(pixel.y)), vec4(0.0));
                }
                radiance += throughput * ambient;
                break;
            }

            // ── Hit: reconstruct attributes ──
            HitInfo hit = getHitInfo(rq);
            vec3 N = hit.normal;
            vec3 Ngeo = hit.geoNormal;
            vec3 P = hit.worldPos;
            VPT_Material mat = materials[hit.materialID];
            vec2 hitUV = hit.uv;

            bool frontFace = dot(rayDir, N) < 0.0;
            if (!frontFace)
            {
                N = -N;
                Ngeo = -Ngeo;
            }

            // ── Texture sampling (Phase 4) ──
            vec2 finalUV = hit.uv * mat.uvScaleOffset.xy + mat.uvScaleOffset.zw;

            // Sample textures and overwrite mat fields so BSDF functions use textured values
            vec3 emission = mat.emissionColor.rgb;

            if ((mat.flags & MAT_HAS_BASE) != 0u && mat.baseTexID != TEX_NONE)
                mat.albedo.rgb *= texture(baseColorTex, vec3(finalUV, float(mat.baseTexID))).rgb;

            if ((mat.flags & MAT_HAS_MR) != 0u && mat.metalRoughTexID != TEX_NONE)
            {
                vec4 mr = texture(metallicRoughTex, vec3(finalUV, float(mat.metalRoughTexID)));
                mat.metallic = mr.r;
                mat.roughness = 1.0 - mr.a; // Unity smoothness → roughness
            }

            if ((mat.flags & MAT_HAS_EMISS) != 0u && mat.emissiveTexID != TEX_NONE)
                emission *= texture(emissiveTex, vec3(finalUV, float(mat.emissiveTexID))).rgb;

            // Tangent to world space
            vec3 T = hit.tangent.xyz;
            float bitangentSign = hit.tangent.w >= 0.0 ? 1.0 : -1.0;

            // Normal map perturbation
            if ((mat.flags & MAT_HAS_NORMAL) != 0u && mat.normalTexID != TEX_NONE)
            {
                vec3 tangentNormal = texture(normalTex, vec3(finalUV, float(mat.normalTexID))).rgb * 2.0 - 1.0;
                vec3 B = cross(N, T) * bitangentSign;
                N = normalize(tangentNormal.x * T + tangentNormal.y * B + tangentNormal.z * N);
            }

            vec3 N_flat = N; // flat normal for visibility checks

            // ★ Transparent: override transmission from base color alpha
            bool isTransparent = ((mat.flags & MAT_IS_TRANSPARENT) != 0u);
            if (isTransparent)
                mat.transmission = 1.0 - mat.albedo.a;

            // G-buffer output (first hit, first sample)
            if (depth == 0 && s == 0u)
            {
                imageStore(gbufferPosDepth, ivec2(pixel.x, int(h) - 1 - int(pixel.y)),
                           vec4(P, hit.hitT));
                imageStore(gbufferNormalRough, ivec2(pixel.x, int(h) - 1 - int(pixel.y)),
                           vec4(Ngeo, mat.roughness));
            }

            // ── Emissive contribution ──
            if ((mat.flags & MAT_HAS_EMISS) != 0u)
            {
                radiance += throughput * emission;
            }

            vec3 V = -rayDir;
            float NdotV = max(dot(N, V), 0.0);

            // Prepare ONB for BTDF (same as sampleBSDF internal)
            vec3 T_world = T;
            vec3 B_world = cross(N, T_world) * bitangentSign;

            // ── NEE: random single light sampling ──
            if (lightCount > 0u)
            {
                uint li = uint(rand(seed) * float(lightCount)) % lightCount;
                VPT_Light L = lights[li];
                vec3 toL; float dist; vec3 Lcolor;
                sampleLightContribution(L, P, toL, dist, Lcolor);
                float NdotL = dot(N, toL);

                // ★ Transparent: allow backside lighting via BTDF
                bool lightValid = false;
                vec3 bsdf_nee = vec3(0.0);

                if (isTransparent && NdotL <= 0.0)
                {
                    float absNdotL = abs(NdotL);
                    bsdf_nee = btdfEval(mat, N, T_world, B_world, V, toL, NdotV, absNdotL);
                    lightValid = (dot(N_flat, toL) < 0.0 || NdotL > 0.0);
                    NdotL = absNdotL;
                }
                else if (NdotL > 0.0)
                {
                    bsdf_nee = evaluateBSDF(mat, N, toL, V, NdotL, NdotV);
                    lightValid = (dot(N_flat, toL) > 0.0);
                }

                if (lightValid)
                {
                    vec3 shadowOrigin = P + Ngeo * EPS_OFFSET;
                    // ★ Use traceShadowTransmission for transparent-aware shadows
                    vec3 shadowTrans = traceShadowTransmission(shadowOrigin, toL, dist);
                    radiance += throughput * bsdf_nee * NdotL * Lcolor * shadowTrans
                             * float(lightCount);
                }
            }

            // ── BSDF sampling for indirect bounce ──
            vec3 newDir;
            float pdf;
            vec3 bsdf;

            if (isTransparent)
            {
                int lobe = findLobe(mat, dot(N, V), rand(seed));
                if (lobe == LOBE_SPECULAR_TRANSMISSION || lobe == LOBE_DIFFUSE_TRANSMISSION)
                {
                    if (lobe == LOBE_SPECULAR_TRANSMISSION)
                        btdfSample(mat, N, T_world, B_world, V, seed, newDir, pdf, bsdf);
                    else
                    {
                        // Diffuse transmission: cosine hemisphere on back side
                        newDir = cosineSampleHemisphere(vec2(rand(seed), rand(seed)), T_world, B_world, -N);
                        pdf = abs(dot(newDir, N)) / PI;
                        bsdf = mat.albedo.rgb * mat.diffuseTransmission;
                    }
                }
                else
                {
                    bsdf = sampleBSDF(mat, N, V, seed, newDir, pdf);
                }
            }
            else
            {
                bsdf = sampleBSDF(mat, N, V, seed, newDir, pdf);
            }

            float NdotL_new = max(dot(N, newDir), 0.0);
            if (isTransparent) NdotL_new = abs(dot(N, newDir));
            if (pdf <= 0.0 || NdotL_new <= 0.0) break;
            if (!isTransparent && dot(N_flat, newDir) <= 0.0) break;
            throughput *= bsdf * NdotL_new / pdf;

            // ── Ray offset: transmission vs reflection ──
            bool isTransmission = (dot(newDir, Ngeo) < 0.0);
            if (isTransmission)
                rayOrigin = P - Ngeo * EPS_OFFSET;
            else
                rayOrigin = P + Ngeo * EPS_OFFSET;
            rayDir = newDir;

            // ── Russian roulette (depth > 3) ──
            if (depth > 3)
            {
                float pcont = min(max(max(throughput.x, throughput.y), throughput.z) + 0.001, 0.95);
                if (rand(seed) >= pcont) break;
                throughput /= pcont;
            }
        }

        finalRadiance += radiance;
    }

    finalRadiance /= float(spp);

    // Y flip: compute shader pixel.y=0 is top, Unity RenderTexture displays bottom-up
    ivec2 storeCoord = ivec2(pixel.x, int(h) - 1 - int(pixel.y));
    imageStore(outputImg, storeCoord, vec4(finalRadiance, 1.0));
}
