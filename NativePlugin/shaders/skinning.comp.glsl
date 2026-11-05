#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// 骨骼动画蒙皮 compute shader（阶段2，对齐参考项目 skinning.comp.slang）
//
// 每顶点最多 4 关节加权（位置/法线/切线）。从 bind-pose base 副本读顶点，
// 把变形后的 pos/normal/tangent 就地写入统一顶点缓冲的 VPT_Vertex 区段——
// UV/颜色不随骨骼变形，统一缓冲中保持原值不动（只写 3 个字段，无读改写）。
//
// 无 descriptor set：所有缓冲地址经 push constant 的 BDA 指针传入。
// 关节矩阵按 4 个 vec4「行」存储（C# 侧 Unity Matrix4x4 行主序展开），
// 变换即 v' = (dot(row0, p), dot(row1, p), dot(row2, p))。

// 80B 交错顶点（与 vpt_types.h VPT_Vertex 一致；std430 下 vec3 对齐 16，
// 恰好复现 C++ #pragma pack(1) 的 80B 布局，数组步长 80）
struct VptVertex
{
    vec3  pos;
    float pad0;
    vec3  normal;
    float pad1;
    vec4  tangent;   // xyz + sign
    vec2  uv;
    vec2  pad2;
    vec4  color;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VertexBuffer  { VptVertex v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) writeonly buffer OutVertexBuf { VptVertex v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer WeightBuffer { vec4  w[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointIdxBuf  { ivec4 j[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer JointMatBuf { vec4  rows[]; };

layout(push_constant, std430) uniform PushConstants
{
    uint64_t baseVertexAddr;   // bind-pose 顶点副本（蒙皮只读，避免在变形数据上反复蒙皮）
    uint64_t outVertexAddr;   // 统一顶点缓冲中该 mesh 区段的首地址
    uint64_t weightsAddr;
    uint64_t jointsAddr;
    uint64_t jointMatsAddr;   // 关节矩阵（本帧环形槽位）
    uint     vertexCount;
    uint     numJoints;
} pc;

layout(local_size_x = 256) in;

void main()
{
    uint vi = gl_GlobalInvocationID.x;
    if (vi >= pc.vertexCount)
        return;

    VertexBuffer baseVerts = VertexBuffer(pc.baseVertexAddr);
    OutVertexBuf outVerts  = OutVertexBuf(pc.outVertexAddr);
    WeightBuffer weights   = WeightBuffer(pc.weightsAddr);
    JointIdxBuf  joints    = JointIdxBuf(pc.jointsAddr);
    JointMatBuf  mats      = JointMatBuf(pc.jointMatsAddr);

    vec4  w = weights.w[vi];
    ivec4 j = joints.j[vi];

    VptVertex base = baseVerts.v[vi];

    vec3  skinnedPos = vec3(0.0);
    vec3  skinnedNrm = vec3(0.0);
    vec3  skinnedTan = vec3(0.0);
    float weightSum  = 0.0;

    for (int i = 0; i < 4; ++i)
    {
        float jw = w[i];
        if (jw <= 0.0)
            continue;
        int ji = j[i];
        if (ji < 0 || uint(ji) >= pc.numJoints)
            continue;

        vec4 row0 = mats.rows[ji * 4 + 0];
        vec4 row1 = mats.rows[ji * 4 + 1];
        vec4 row2 = mats.rows[ji * 4 + 2];

        vec4 p4 = vec4(base.pos, 1.0);
        skinnedPos += jw * vec3(dot(row0, p4), dot(row1, p4), dot(row2, p4));

        vec3 n = base.normal;
        skinnedNrm += jw * vec3(dot(row0.xyz, n), dot(row1.xyz, n), dot(row2.xyz, n));

        vec3 t = base.tangent.xyz;
        skinnedTan += jw * vec3(dot(row0.xyz, t), dot(row1.xyz, t), dot(row2.xyz, t));

        weightSum += jw;
    }

    if (weightSum > 0.0)
    {
        outVerts.v[vi].pos    = skinnedPos;
        outVerts.v[vi].normal = normalize(skinnedNrm);
        outVerts.v[vi].tangent = vec4(normalize(skinnedTan), base.tangent.w);
    }
    // 权重全零的顶点（不受骨骼影响）保持统一顶点缓冲中的 bind-pose 原值不动
}
