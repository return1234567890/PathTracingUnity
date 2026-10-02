# tinybvh 能力与项目需求调研

> 阶段约束：仅实现遍历 + 单材质 BRDF 着色的路径追踪主链路。
> 暂不涉及：光源数组、背景环境光、normalMatrix 约定、多材质网格（每三角形材质索引）、纹理。

---

## 一、Shader 输入数据布局（最终版）

### 1. 数据数组清单

| 数组 | 内容 | 来源 |
|---|---|---|
| TLAS 节点数组 | `BVHNode`（32B/节点，Wald 布局） | tinybvh `tlas.bvhNode` |
| TLAS primIdx | 实例索引（叶子→实例ID映射） | tinybvh `tlas.primIdx`，**必须导出** |
| BLAS 节点数组（全局拼接） | `BVHNode` | 各 `blas.bvhNode`，子节点索引需重定位 |
| BLAS primIdx（全局拼接） | 三角形索引（叶子→原始三角形索引） | 各 `blas.primIdx`，值需 +triBase 重定位 |
| 全局三角形数组 | 每元素 = `{v0, v1, v2}` 顶点索引 | 外部维护，须保持原始三角形顺序 |
| 全局顶点数组 | 结构体(pos/normal/tangent/uv/color) | 外部维护拼接，与 tinybvh 解耦 |
| 材质颜色数组 | color | 自定义 |
| BLAS 描述表（按 blasID 索引） | `{blasNodeBase, primIdxBase, triBase, vertBase}` | 导出层组装 |
| 实例表（按实例ID索引） | `{materialID, blasID, transform, invTransform}` | `BLASInstance` + 自定义 materialID |

### 2. 取值链路

```
TLAS 叶子
  → [leftFirst, leftFirst+triCount) in TLAS primIdx
  → primIdx[k] = 实例ID
  → 实例表[实例ID] = {materialID, blasID, transform, invTransform}
  → BLAS 描述表[blasID] = {blasNodeBase, primIdxBase, triBase, vertBase}
  → 用 invTransform 把光线变换到实例局部空间
  → 在 blasNodeBase 处遍历 BLAS
  → BLAS 叶子
     → [leftFirst, leftFirst+triCount) in BLAS primIdx（primIdxBase 处）
     → primIdx[k] = 原始三角形索引 T
     → 三角形数组[T + triBase] = {v0, v1, v2}
     → 全局顶点数组[v + vertBase] = pos/normal/tangent/uv/color
  → 用 transform 把命中点/法线变换回世界空间
  → 用 materialID 取材质颜色做 BRDF 着色
```

---

## 二、关键可行性结论

### 1. primIdx 能否导出 —— 能，且必须

tinybvh 在 `Build` 完成后，`uint32_t* primIdx`（长度 = `idxCount`，普通 BVH 下 = `triCount`）就是"叶子槽 → 原始索引"的重排映射：
- 构造时初始化为 `primIdx[i] = i`，随后被 SAH 重排（`tiny_bvh.h` L2240 / L2359 / TLAS 在 L2427）。
- **对 BLAS**：存"叶子槽 → 原始三角形索引"。shader 里 BLAS 叶子给出 `[leftFirst, leftFirst+triCount)` 这段 primIdx 范围，逐个取出才是真实三角形索引。不导出 primIdx，叶子的三角形索引就是错的。
- **对 TLAS**：存实例索引，同理必须导出。
- 拼接成全局数组时，primIdx 里的值也要加对应 BLAS 的三角形基地址做重定位（或在 shader 里加基地址）。

### 2. 三角形数组 = 每元素 3 个顶点索引 —— 支持

这是 indexed mesh 表示，tinybvh 原生支持重载：
```
Build(const bvhvec4* vertices, const uint32_t* indices, const uint32_t primCount)
```
内部用 `vertIdx` 存这 3 个顶点索引，用 `primIdx` 存"叶子槽 → 原始三角形索引"的重排映射。

**唯一约束**：全局三角形数组必须保持 tinybvh `Build` 时的**原始三角形顺序**（第 T 个三角形对应传给 Build 的第 T 个 primitive），这样 primIdx 取出的 T 才能正确索引。只要导出时不重排三角形数组，就成立。

### 3. 全局顶点数组外部维护 —— 无问题

tinybvh `Build` 只从顶点结构里抽 xyz 拼 `bvhvec4` 喂进去，完整顶点结构数组独立维护、独立喂 shader，两边解耦。拼接时给每个 BLAS 一个 `vertBase`（顶点基地址）即可。

### 4. 拆 BLAS 描述表 + 瘦身实例表 —— 推荐

与 tinybvh 自身的 `instList + blasList[blasIdx]` 两层结构天然契合：

**BLAS 描述表**（按 blasID 索引）：
```c
struct BLASDescriptor {
    uint32_t blasNodeBase;   // 该BLAS节点在全局BLAS节点数组中的基地址
    uint32_t primIdxBase;    // 该BLAS的primIdx在全局primIdx数组中的基地址
    uint32_t triBase;        // 该BLAS三角形在全局三角形数组中的基地址
    uint32_t vertBase;       // 该BLAS顶点在全局顶点数组中的基地址
};
```

**实例表**（按实例ID索引）：
```c
struct InstanceRecord {
    uint32_t materialID;
    uint32_t blasID;
    float    transform[16];    // 行主序 4×4
    float    invTransform[16]; // 行主序 4×4
};
```

> tinybvh 原生 `BLASInstance` 还带 `aabbMin/aabbMax/mask/dummy`：
> - AABB 只在 TLAS **构建时**用（`inst.Update(blas)` 算世界包围盒），shader 运行时遍历节点不需要 AABB。
> - `mask`（射线遮罩）当前不考虑，可丢；将来做双面/穿透再说。

---

## 三、需要特别注意的工程要点

### 1. BLAS 节点拼接时的子节点索引重定位

每个 BLAS 的节点内部 `leftFirst` 指向子节点时是**相对本 BLAS 的局部索引**。拼成全局一维数组后，必须加该 BLAS 的 `blasNodeBase` 做重定位，否则子节点指向错位。

### 2. 基地址语义统一

全链路明确基地址为**元素偏移**（而非字节偏移），shader 内所有 `+base` 运算保持一致。

### 3. primIdx 的两层映射

- 叶子在 primIdx 数组里是一段连续范围 `[leftFirst, leftFirst+triCount)`；
- 每条 primIdx 元素才是真实索引（三角形索引或实例索引）；
- 真实索引再 + 对应基地址定位到全局数据。

### 4. 当前约束下的局限

- 没有光源数组 + 没有背景环境光：场景须有其他发光来源（如某材质颜色当 emission），否则画面全黑。属 scope 取舍，非数据缺失。
- 实例级单一材质索引：不支持一个 mesh 内多材质（多 sub-mesh）。当前靠"每个 BLAS 单材质"或"实例级材质"约束规避。

---

## 四、tinybvh 相关结构体参考

### BVHNode（Wald 32 字节布局，`tiny_bvh.h` L885）
```c
struct BVHNode {
    bvhvec3 aabbMin; uint32_t leftFirst; // 内部节点=左子节点索引；叶子=primIdx起始
    bvhvec3 aabbMax; uint32_t triCount; // 内部节点=0；叶子>0
    bool isLeaf() const { return triCount > 0; }
};
```

### BLASInstance（`tiny_bvh.h` L1508）
```c
class BLASInstance {
    bvhmat4 transform;       // 行主序 4×4
    bvhmat4 invTransform;    // 行主序 4×4
    bvhvec3 aabbMin;
    uint32_t blasIdx;
    bvhvec3 aabbMax;
    uint32_t mask;
    uint32_t dummy[8];       // 填充到 64 字节
};
```

### BVH 关键成员（`tiny_bvh.h` L986-994）
```c
bvhvec4slice verts;     // 输入图元数组：每三角形 3 个 bvhvec4（仅 xyz）
uint32_t* vertIdx;      // 顶点索引，仅 indexed build 使用
uint32_t* primIdx;      // 图元索引数组（叶子→原始索引映射）
BLASInstance* instList; // TLAS 实例数组
BVHNode* bvhNode;       // 节点池，根恒在 index 0
uint32_t usedNodes;
uint32_t triCount;
uint32_t idxCount;      // 可超过 triCount（SBVH）
```

### TLAS 遍历逻辑（`tiny_bvh.h` L3669）
```
const BVHBase* blas = blasList[inst.blasIdx];
tmpRay.O = tinybvh_transform_point(ray.O, inst.invTransform);
tmpRay.D = tinybvh_transform_vector(ray.D, inst.invTransform);
// 用变换后的光线遍历 BLAS
```
