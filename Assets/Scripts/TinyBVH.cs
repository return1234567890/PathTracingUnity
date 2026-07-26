using System;
using UnityEngine;

/// <summary>
/// tinybvh C# 封装，提供 BVH 构建与射线求交能力。
/// 使用示例：
///   var bvh = new TinyBVH();
///   bvh.Build(meshFilter.sharedMesh);
///   var hit = bvh.Raycast(origin, direction, 1000f);
///   bvh.Dispose();
/// </summary>
public class TinyBVH : IDisposable
{
    private IntPtr _handle;
    private bool   _disposed;

    public struct RayHit
    {
        public bool  hit;
        public int   triangleIndex;
        public float distance;
        public float u, v;
    }

    public TinyBVH()
    {
        _handle = TinyBVHNative.tinybvh_create();
    }

    /// <summary>从 Unity Mesh 构建 BVH</summary>
    public void Build(Mesh mesh)
    {
        Build(mesh.vertices, mesh.triangles);
    }

    /// <summary>从顶点 + 三角形索引构建 BVH</summary>
    public void Build(Vector3[] vertices, int[] triangles)
    {
        int triCount = triangles.Length / 3;
        // 每顶点 4 float (x,y,z,pad)，共 triCount*3 个顶点
        float[] data = new float[triCount * 3 * 4];
        for (int i = 0; i < triCount; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                Vector3 v = vertices[triangles[i * 3 + j]];
                int baseIdx = (i * 3 + j) * 4;
                data[baseIdx + 0] = v.x;
                data[baseIdx + 1] = v.y;
                data[baseIdx + 2] = v.z;
                data[baseIdx + 3] = 0f; // pad
            }
        }
        TinyBVHNative.tinybvh_build(_handle, data, triCount);
    }

    /// <summary>单条射线求交</summary>
    public RayHit Raycast(Vector3 origin, Vector3 direction, float maxDistance = 1e30f)
    {
        var r = TinyBVHNative.tinybvh_intersect(
            _handle,
            origin.x, origin.y, origin.z,
            direction.x, direction.y, direction.z,
            maxDistance);

        return new RayHit
        {
            hit           = r.hit != 0,
            triangleIndex = r.primIdx,
            distance      = r.t,
            u             = r.u,
            v             = r.v
        };
    }

    /// <summary>批量射线求交（减少跨边界调用开销）</summary>
    public RayHit[] RaycastBatch(Vector3[] origins, Vector3[] directions, float maxDistance = 1e30f)
    {
        int count = origins.Length;
        float[] oriFlat = new float[count * 3];
        float[] dirFlat = new float[count * 3];
        for (int i = 0; i < count; i++)
        {
            oriFlat[i * 3 + 0] = origins[i].x;
            oriFlat[i * 3 + 1] = origins[i].y;
            oriFlat[i * 3 + 2] = origins[i].z;
            dirFlat[i * 3 + 0] = directions[i].x;
            dirFlat[i * 3 + 1] = directions[i].y;
            dirFlat[i * 3 + 2] = directions[i].z;
        }

        var nativeResults = new TinyBVHNative.HitResult[count];
        TinyBVHNative.tinybvh_intersect_batch(
            _handle, oriFlat, dirFlat, nativeResults, count, maxDistance);

        var results = new RayHit[count];
        for (int i = 0; i < count; i++)
        {
            results[i] = new RayHit
            {
                hit           = nativeResults[i].hit != 0,
                triangleIndex = nativeResults[i].primIdx,
                distance      = nativeResults[i].t,
                u             = nativeResults[i].u,
                v             = nativeResults[i].v
            };
        }
        return results;
    }

    public void Dispose()
    {
        if (!_disposed && _handle != IntPtr.Zero)
        {
            TinyBVHNative.tinybvh_destroy(_handle);
            _handle   = IntPtr.Zero;
            _disposed = true;
        }
    }

    ~TinyBVH() { Dispose(); }
}
