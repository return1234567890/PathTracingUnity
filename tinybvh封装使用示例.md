# tinybvh 封装使用示例

> 本文档说明如何使用 `TinyScene.cs` 获取路径追踪 Shader 所需的全部加速结构数据数组。

---

## 一、完整使用流程

```csharp
using System;
using System.Runtime.InteropServices;
using UnityEngine;

public class PathTracingSetup : MonoBehaviour
{
    private TinyScene _scene;

    void Start()
    {
        _scene = new TinyScene();

        // ════════ 1. 添加 BLAS ════════

        // 方式 A：Indexed mesh（推荐，支持共享顶点）
        Mesh meshA = GetComponent<MeshFilter>().mesh;
        int blas0 = _scene.AddBLASIndexed(
            meshA.vertices, meshA.triangles,
            vertBase: 0);  // 此 BLAS 顶点在全局顶点数组中从 0 开始

        // 方式 B：非 indexed（简单场景用，vertBase 默认 0）
        Mesh meshB = /* ... */;
        int blas1 = _scene.AddBLAS(
            meshB.vertices, meshB.triangles,
            vertBase: meshA.vertexCount);

        // ════════ 2. 添加实例 ════════
        int inst0 = _scene.AddInstance(blas0, transform,         materialID: 0);
        int inst1 = _scene.AddInstance(blas0, someOtherTransform, materialID: 1);
        int inst2 = _scene.AddInstance(blas1, anotherTransform,    materialID: 0);

        // ════════ 3. 构建 TLAS + 组装全局数据 ════════
        _scene.BuildTLAS();
        _scene.AssembleGlobals();

        // ════════ 4. 查询路径追踪所需数组 ════════

        // (1) TLAS 节点数组（BVHNode, 32 字节/节点）
        int tlasNodeCount;
        IntPtr tlasNodesPtr = _scene.GetTLASNodes(out tlasNodeCount);

        // (2) TLAS primIdx（实例索引映射）
        int tlasPrimCount;
        IntPtr tlasPrimIdxPtr = _scene.GetTLASPrimIdx(out tlasPrimCount);

        // (3) 全局 BLAS 节点数组（已重定位子节点索引）
        int blasNodeCount;
        IntPtr blasNodesPtr = _scene.GetGlobalBLASNodes(out blasNodeCount);

        // (4) 全局 BLAS primIdx（三角形索引映射）
        int blasPrimCount;
        IntPtr blasPrimIdxPtr = _scene.GetGlobalBLASPrimIdx(out blasPrimCount);

        // (5) BLAS 描述表
        int descCount;
        IntPtr descPtr = _scene.GetBLASDescriptors(out descCount);

        // (6) 实例表（materialID + blasID + transform + invTransform）
        int instCount;
        IntPtr instTablePtr = _scene.GetInstanceTable(out instCount);

        // ════════ 5. 拷贝数据到 ComputeBuffer 或托管数组 ════════

        // 方式 A：拷贝到 ComputeBuffer（GPU 路径追踪推荐）
        var tlasNodeBuffer = new ComputeBuffer(tlasNodeCount, 32);
        tlasNodeBuffer.SetBufferData(tlasNodesPtr, 0, 0, tlasNodeCount);

        var blasNodeBuffer = new ComputeBuffer(blasNodeCount, 32);
        blasNodeBuffer.SetBufferData(blasNodesPtr, 0, 0, blasNodeCount);

        var tlasPrimIdxBuffer = new ComputeBuffer(tlasPrimCount, 4);
        tlasPrimIdxBuffer.SetBufferData(tlasPrimIdxPtr, 0, 0, tlasPrimCount);

        var blasPrimIdxBuffer = new ComputeBuffer(blasPrimCount, 4);
        blasPrimIdxBuffer.SetBufferData(blasPrimIdxPtr, 0, 0, blasPrimCount);

        var descBuffer = new ComputeBuffer(descCount, 16);
        descBuffer.SetBufferData(descPtr, 0, 0, descCount);

        var instBuffer = new ComputeBuffer(instCount, 136);
        instBuffer.SetBufferData(instTablePtr, 0, 0, instCount);

        // 方式 B：拷贝到托管数组（CPU 路径追踪用）
        uint[] tlasPrimIdx = new uint[tlasPrimCount];
        Marshal.Copy(tlasPrimIdxPtr, (int[])(object)tlasPrimIdx, 0, tlasPrimCount);

        var descs = new TinyBVHNative.BLASDescriptor[descCount];
        for (int i = 0; i < descCount; i++)
        {
            descs[i] = Marshal.PtrToStructure<TinyBVHNative.BLASDescriptor>(
                descPtr + i * 16);
        }

        var instances = new TinyBVHNative.InstanceRecord[instCount];
        for (int i = 0; i < instCount; i++)
        {
            instances[i] = Marshal.PtrToStructure<TinyBVHNative.InstanceRecord>(
                instTablePtr + i * 136);
        }
    }

    void OnDestroy()
    {
        _scene?.Dispose();
    }
}
```

---

## 二、数据数组与 getter 对应关系

| # | 数组 | getter 方法 | 元素大小 | 数据来源 |
|---|---|---|---|---|
| 1 | TLAS 节点数组 | `GetTLASNodes` | 32B | `tlas.bvhNode` + `tlas.usedNodes` |
| 2 | TLAS primIdx | `GetTLASPrimIdx` | 4B | `tlas.primIdx` + `tlas.idxCount` |
| 3 | 全局 BLAS 节点数组（重定位） | `GetGlobalBLASNodes` | 32B | 各 `blas.bvhNode` 拼接 |
| 4 | 全局 BLAS primIdx | `GetGlobalBLASPrimIdx` | 4B | 各 `blas.primIdx` 拼接 |
| 5 | BLAS 描述表 | `GetBLASDescriptors` | 16B | 导出层组装 |
| 6 | 实例表 | `GetInstanceTable` | 136B | BLASInstance + materialID |
| 7 | 全局三角形数组 | 外部维护 | 12B | 与 `AddBLASIndexed` 的 triangles 一致 |
| 8 | 全局顶点数组 | 外部维护 | 结构体 | pos/normal/tangent/uv/color |
| 9 | 材质颜色数组 | 外部维护 | 自定义 | 按 materialID 索引 |

---

## 三、结构体布局

### BVHNode（32 字节，Wald 布局）

```
偏移 0-11:  aabbMin   (float3, 12B)
偏移 12-15: leftFirst (uint32, 4B)  内部节点=左子节点索引；叶子=primIdx起始
偏移 16-27: aabbMax   (float3, 12B)
偏移 28-31: triCount  (uint32, 4B)  内部节点=0；叶子>0
```

### BLASDescriptor（16 字节）

```csharp
public struct BLASDescriptor {
    public uint blasNodeBase;  // 全局 BLAS 节点数组中的基地址
    public uint primIdxBase;   // 全局 BLAS primIdx 数组中的基地址
    public uint triBase;       // 全局三角形数组中的基地址
    public uint vertBase;      // 全局顶点数组中的基地址
}
```

### InstanceRecord（136 字节）

```csharp
public struct InstanceRecord {
    public uint materialID;
    public uint blasID;
    public float[] transform;     // 行主序 4×4，16 个 float
    public float[] invTransform;  // 行主序 4×4，16 个 float
}
```

---

## 四、Shader 取值链路

```
TLAS 遍历
  → TLAS 叶子: leftFirst / triCount
  → TLAS primIdx[leftFirst .. leftFirst+triCount) → 实例ID
  → 实例表[实例ID] = { materialID, blasID, transform, invTransform }
  → BLAS 描述表[blasID] = { blasNodeBase, primIdxBase, triBase, vertBase }
  → 用 invTransform 把光线变换到实例局部空间
  → 从 blasNodeBase 处开始遍历全局 BLAS 节点数组
  → BLAS 叶子: leftFirst / triCount（leftFirst 已含 primIdxBase 偏移）
  → 全局 primIdx[leftFirst .. leftFirst+triCount) → 三角形局部索引 T
  → 全局三角形数组[T + triBase] = { v0, v1, v2 }
  → 全局顶点数组[v0/v1/v2 + vertBase] = pos / normal / tangent / uv / color
  → 用 transform 把命中点/法线变换回世界空间
  → 用 materialID 取材质颜色做 BRDF 着色
```

---

## 五、重定位规则

| 字段 | 位置 | 重定位方式 | 执行方 |
|---|---|---|---|
| 内部节点 `leftFirst` | 全局 BLAS 节点数组 | `+= blasNodeBase` | 导出层（AssembleGlobals） |
| 叶子节点 `leftFirst` | 全局 BLAS 节点数组 | `+= primIdxBase` | 导出层（AssembleGlobals） |
| primIdx 值 | 全局 BLAS primIdx 数组 | 不变，shader 侧 `+ triBase` | Shader |
| 三角形查找 | 外部数组 | `primIdx[k] + triBase` | Shader |
| 顶点查找 | 外部数组 | `v + vertBase` | Shader |

---

## 六、生命周期与调用顺序

```
AddBLAS / AddBLASIndexed     → globalsValid = false
UpdateBLAS                   → globalsValid = false
AddInstance / RemoveInstance → 实例表缓存失效
BuildTLAS                    → TLAS 节点/primIdx 指针更新
AssembleGlobals              → 全局 BLAS 数组刷新，globalsValid = true
```

**必须遵循的调用顺序：**

```
AddBLAS → AddInstance → BuildTLAS → AssembleGlobals → 查询数组 → 拷贝数据
```

任何 BLAS 或 Instance 变动后，需要重新执行 `BuildTLAS` + `AssembleGlobals` 再查询数组。

---

## 七、外部维护的数组

以下三个数组不由 tinybvh 导出，由 C# 侧自行拼接维护：

### 全局三角形数组

每元素 = `{v0, v1, v2}` 三个顶点索引。与传入 `AddBLASIndexed` 的 `triangles` 参数一致，按 `triBase` 偏移拼接各 mesh 的三角形索引。

### 全局顶点数组

结构体包含 `pos / normal / tangent / uv / color`，按 `vertBase` 偏移拼接各 mesh 的顶点数据。tinybvh 仅抽取 xyz 构建加速结构，完整顶点数据独立维护。

### 材质颜色数组

自定义结构，按 `materialID` 索引。当前阶段仅支持 albedo 颜色。
