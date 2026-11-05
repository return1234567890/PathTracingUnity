using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

/// <summary>
/// jugong 蒙皮动画模型自动导入器（一次性工具）。
/// 编辑器加载/重编译后自动执行；也可通过菜单 Tools → Jugong 手动触发。
/// 流程：
///   1. 配置 ModelImporter：Generic 骨骼 + 循环动画 + 可读网格（供 BakeMesh 路径追踪）
///   2. 修正纹理导入设置（normal → NormalMap，metallic/roughness → 线性）
///   3. 合成 URP MetallicSmoothness 蒙版（R = metallic，A = 1 - roughness）
///   4. 生成 URP Lit 材质与 AnimatorController（单状态循环播放）
///   5. 实例化到当前打开场景，归一化身高至 1.7m 并落地，保存场景
/// </summary>
public static class JugongAutoImport
{
    const string ModelDir = "Assets/Models/jugong";
    const string FbxPath = ModelDir + "/tripo_convert_7f7027e8-304e-48f0-ac7c-5896bd952b4c.fbx";
    const string MatPath = ModelDir + "/jugong_mat.mat";
    const string MaskPath = ModelDir + "/jugong_metallicSmoothness.png";
    const string ControllerPath = ModelDir + "/jugong.controller";
    const string SceneObjectName = "jugong";
    const float TargetHeight = 1.7f;
    const float FloorTopY = -0.5f; // 场景地板 Cube（y=-1，scale.y=1）顶面

    static int _retries;

    [InitializeOnLoadMethod]
    static void AutoRun()
    {
        _retries = 0;
        EditorApplication.delayCall += Run;
        EditorApplication.delayCall += FixAvatar;
    }

    [MenuItem("Tools/Jugong/导入到场景")]
    static void MenuRun() => Run();

    [MenuItem("Tools/Jugong/修复 Avatar")]
    static void MenuFixAvatar() => FixAvatar();

    [MenuItem("Tools/Jugong/删除并重新导入")]
    static void MenuRerun()
    {
        var existing = GameObject.Find(SceneObjectName);
        if (existing != null) Object.DestroyImmediate(existing);
        Run();
    }

    static void Run()
    {
        if (!File.Exists(FbxPath))
        {
            Debug.LogWarning($"[JugongAutoImport] 找不到 FBX：{FbxPath}");
            return;
        }
        if (GameObject.Find(SceneObjectName) != null)
        {
            Debug.Log("[JugongAutoImport] 场景中已存在 jugong，跳过。");
            return;
        }
        if (EditorApplication.isPlayingOrWillChangePlaymode)
        {
            EditorApplication.delayCall += Run;
            return;
        }

        // FBX 可能尚未完成首次导入，延迟重试
        var importer = (ModelImporter)AssetImporter.GetAtPath(FbxPath);
        if (importer == null)
        {
            if (++_retries < 120)
            {
                EditorApplication.delayCall += Run;
                return;
            }
            Debug.LogWarning("[JugongAutoImport] FBX 导入器不可用，请手动执行 Tools → Jugong → 导入到场景。");
            return;
        }

        ConfigureModelImporter(importer);
        ConfigureTextureImporters();
        var mask = CreateMetallicSmoothnessMask();
        var material = CreateMaterial(mask);
        var clip = AssetDatabase.LoadAllAssetsAtPath(FbxPath).OfType<AnimationClip>().FirstOrDefault();
        var controller = CreateAnimatorController(clip);
        var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(FbxPath);
        if (prefab == null)
        {
            Debug.LogWarning("[JugongAutoImport] 无法加载 FBX 根对象。");
            return;
        }

        // ── 实例化到当前打开的场景 ──
        Scene scene = SceneManager.GetActiveScene();
        if (!scene.IsValid())
        {
            Debug.LogWarning("[JugongAutoImport] 没有打开的场景。");
            return;
        }

        var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab, scene);
        if (go == null) go = Object.Instantiate(prefab);
        go.name = SceneObjectName;

        // 以原始单位重置变换后测量包围盒
        go.transform.SetPositionAndRotation(Vector3.zero, Quaternion.identity);
        go.transform.localScale = Vector3.one;
        bool hasBounds = CalculateWorldBounds(go, out Bounds bounds);

        // 材质（覆盖所有子渲染器）
        if (material != null)
        {
            foreach (var smr in go.GetComponentsInChildren<SkinnedMeshRenderer>(true))
                smr.sharedMaterials = Enumerable.Repeat(material, smr.sharedMaterials.Length).ToArray();
            foreach (var mf in go.GetComponentsInChildren<MeshFilter>(true))
            {
                var mr = mf.GetComponent<MeshRenderer>();
                if (mr != null) mr.sharedMaterials = Enumerable.Repeat(material, mr.sharedMaterials.Length).ToArray();
            }
        }

        // 归一化身高并落地，面向相机（相机位于 -Z 朝 +Z 看）
        float scaleFactor = 1f;
        if (hasBounds && bounds.size.y > 1e-5f)
        {
            scaleFactor = TargetHeight / bounds.size.y;
            go.transform.localScale = Vector3.one * scaleFactor;
            go.transform.SetPositionAndRotation(
                new Vector3(0f, FloorTopY - bounds.min.y * scaleFactor, 2f),
                Quaternion.Euler(0f, 180f, 0f));
        }
        else
        {
            go.transform.SetPositionAndRotation(new Vector3(0f, FloorTopY, 2f), Quaternion.Euler(0f, 180f, 0f));
        }

        // Animator + 循环动画
        if (controller != null)
        {
            var animator = go.GetComponent<Animator>();
            if (animator == null) animator = go.AddComponent<Animator>();
            animator.runtimeAnimatorController = controller;
            animator.applyRootMotion = false;
        }

        Undo.RegisterCreatedObjectUndo(go, "Import jugong");
        EditorSceneManager.MarkSceneDirty(scene);
        EditorSceneManager.SaveScene(scene);
        Selection.activeGameObject = go;

        Debug.Log(
            $"[JugongAutoImport] 导入完成：动画剪辑={clip?.name ?? "无"}, " +
            $"原始身高={(hasBounds ? bounds.size.y.ToString("F2") + "m" : "未知")}，" +
            $"缩放={scaleFactor:F3}，材质={(material != null ? MatPath : "FBX 默认")}，" +
            $"蒙版={(mask != null ? MaskPath : "无")}，场景已保存。");
    }

    // ── Avatar 修复：FBX 生成的 Avatar 子资产 → 场景 Animator ──
    /// <summary>
    /// Generic 动画必须有 Avatar 才能把剪辑曲线绑定到骨骼层级，否则光栅/路径追踪均不播放。
    /// 注意：不能信任磁盘上的 meta（编辑器开着时外部改 meta，导入器内存状态可能是旧值，
    /// SaveAndReimport 会用内存状态重导并把 meta 回写为旧值）——这里强制在代码里设置
    /// avatarSetup=CreateFromThisModel 再重导入，确保 Avatar 子资产一定生成。
    /// 每个分支均有日志，便于定位失败原因。
    /// </summary>
    static void FixAvatar()
    {
        if (!File.Exists(FbxPath))
        {
            Debug.LogWarning($"[JugongAutoImport] FixAvatar: 找不到 FBX：{FbxPath}");
            return;
        }
        if (EditorApplication.isPlayingOrWillChangePlaymode)
        {
            // play mode 下赋值不会持久化，退出后重试
            EditorApplication.delayCall += FixAvatar;
            return;
        }

        var importer = (ModelImporter)AssetImporter.GetAtPath(FbxPath);
        if (importer == null)
        {
            Debug.LogWarning("[JugongAutoImport] FixAvatar: ModelImporter 不可用。");
            return;
        }

        // 强制 Generic + CreateFromThisModel（不信任磁盘 meta，原因见注释）
        if (importer.animationType != ModelImporterAnimationType.Generic)
            importer.animationType = ModelImporterAnimationType.Generic;
        if (importer.avatarSetup != ModelImporterAvatarSetup.CreateFromThisModel)
        {
            Debug.Log("[JugongAutoImport] FixAvatar: avatarSetup != CreateFromThisModel，强制重导入生成 Avatar...");
            importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
            importer.SaveAndReimport();
        }

        // 查找 FBX 内的 Avatar 子资产；仍无则再强制重导入一次
        var avatar = AssetDatabase.LoadAllAssetsAtPath(FbxPath).OfType<Avatar>().FirstOrDefault();
        if (avatar == null)
        {
            Debug.Log("[JugongAutoImport] FixAvatar: 首次查找无 Avatar，再次强制重导入...");
            importer.avatarSetup = ModelImporterAvatarSetup.CreateFromThisModel;
            importer.SaveAndReimport();
            avatar = AssetDatabase.LoadAllAssetsAtPath(FbxPath).OfType<Avatar>().FirstOrDefault();
        }
        if (avatar == null)
        {
            Debug.LogWarning($"[JugongAutoImport] FixAvatar: 重导入后仍无 Avatar 子资产，" +
                              $"FBX 骨架可能无法生成 Generic Avatar，请在 FBX Rig 页签手动确认：{FbxPath}");
            return;
        }
        Debug.Log($"[JugongAutoImport] FixAvatar: 已找到 Avatar '{avatar.name}'。");

        // 只修复使用 jugong.controller 且 Avatar 为空的 Animator（不碰场景其它角色）
        int matched = 0, skipHasAvatar = 0, skipNoCtrl = 0, skipOtherCtrl = 0;
        bool dirty = false;
        foreach (var animator in Object.FindObjectsOfType<Animator>())
        {
            if (animator.runtimeAnimatorController == null) { skipNoCtrl++; continue; }
            if (animator.avatar != null) { skipHasAvatar++; continue; }
            string ctrlPath = AssetDatabase.GetAssetPath(animator.runtimeAnimatorController);
            if (ctrlPath != ControllerPath) { skipOtherCtrl++; continue; }

            animator.avatar = avatar;
            matched++;
            dirty = true;
            Debug.Log($"[JugongAutoImport] FixAvatar: 已为 '{animator.name}' 赋值 Avatar '{avatar.name}'。", animator.gameObject);
        }

        Debug.Log($"[JugongAutoImport] FixAvatar 完成：赋值={matched}，已有Avatar跳过={skipHasAvatar}，" +
                  $"无Controller跳过={skipNoCtrl}，其他Controller跳过={skipOtherCtrl}。");

        if (dirty)
        {
            Scene scene = SceneManager.GetActiveScene();
            EditorSceneManager.MarkSceneDirty(scene);
            EditorSceneManager.SaveScene(scene);
            Debug.Log("[JugongAutoImport] FixAvatar: 场景已保存。");
        }
    }

    // ── 1. ModelImporter：Generic 骨骼 + 循环动画 + 可读网格 ──
    static void ConfigureModelImporter(ModelImporter importer)
    {
        bool dirty = false;
        if (importer.animationType != ModelImporterAnimationType.Generic)
        {
            importer.animationType = ModelImporterAnimationType.Generic;
            dirty = true;
        }
        if (!importer.isReadable) { importer.isReadable = true; dirty = true; }
        if (importer.importCameras) { importer.importCameras = false; dirty = true; }
        if (importer.importLights) { importer.importLights = false; dirty = true; }
        if (importer.optimizeGameObjects) { importer.optimizeGameObjects = false; dirty = true; }

        var defaults = importer.defaultClipAnimations;
        if (defaults != null && defaults.Length > 0)
        {
            var current = importer.clipAnimations;
            bool needs = current == null || current.Length != defaults.Length;
            if (!needs)
                for (int i = 0; i < current.Length; i++)
                    if (!current[i].loopTime) { needs = true; break; }
            if (needs)
            {
                for (int i = 0; i < defaults.Length; i++) defaults[i].loopTime = true;
                importer.clipAnimations = defaults;
                dirty = true;
            }
        }

        if (dirty) importer.SaveAndReimport();
    }

    // ── 2. 纹理导入设置：normal → NormalMap，metallic/roughness → 线性 ──
    static void ConfigureTextureImporters()
    {
        foreach (var guid in AssetDatabase.FindAssets("t:Texture2D", new[] { ModelDir }))
        {
            string path = AssetDatabase.GUIDToAssetPath(guid);
            string name = Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
            var imp = (TextureImporter)AssetImporter.GetAtPath(path);
            if (imp == null) continue;

            bool dirty = false;
            if (name.Contains("normal") && imp.textureType != TextureImporterType.NormalMap)
            {
                imp.textureType = TextureImporterType.NormalMap;
                dirty = true;
            }
            if ((name.Contains("metallic") || name.Contains("roughness")) && imp.sRGBTexture)
            {
                imp.sRGBTexture = false;
                dirty = true;
            }
            if (dirty) imp.SaveAndReimport();
        }
    }

    // ── 3. 合成 URP MetallicSmoothness 蒙版（R = metallic，A = 1 - roughness）──
    static Texture2D CreateMetallicSmoothnessMask()
    {
        var existing = AssetDatabase.LoadAssetAtPath<Texture2D>(MaskPath);
        if (existing != null) return existing;

        string metalPath = FindTexturePath("metallic");
        string roughPath = FindTexturePath("roughness");
        if (metalPath == null || roughPath == null)
        {
            Debug.LogWarning("[JugongAutoImport] 未找到 metallic/roughness 纹理，跳过蒙版合成。");
            return null;
        }

        var metal = LoadReadable(metalPath);
        var rough = LoadReadable(roughPath);
        if (metal == null || rough == null) return null;

        int w = metal.width;
        int h = metal.height;
        var mask = new Texture2D(w, h, TextureFormat.RGBA32, false, true);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                // roughness 分辨率可能不同，按 UV 比例采样
                int rx = rough.width > 1 ? (x * (rough.width - 1)) / (w - 1) : 0;
                int ry = rough.height > 1 ? (y * (rough.height - 1)) / (h - 1) : 0;
                float metallic = metal.GetPixel(x, y).r;
                float smoothness = 1f - rough.GetPixel(rx, ry).r;
                mask.SetPixel(x, y, new Color(metallic, 0f, 0f, smoothness));
            }
        }
        File.WriteAllBytes(MaskPath, mask.EncodeToPNG());
        Object.DestroyImmediate(mask);
        AssetDatabase.ImportAsset(MaskPath);

        var maskImp = (TextureImporter)AssetImporter.GetAtPath(MaskPath);
        if (maskImp != null)
        {
            maskImp.sRGBTexture = false;
            maskImp.SaveAndReimport();
        }
        return AssetDatabase.LoadAssetAtPath<Texture2D>(MaskPath);
    }

    static string FindTexturePath(string keyword)
    {
        return AssetDatabase.FindAssets("t:Texture2D", new[] { ModelDir })
            .Select(AssetDatabase.GUIDToAssetPath)
            .FirstOrDefault(p => Path.GetFileNameWithoutExtension(p).ToLowerInvariant().Contains(keyword));
    }

    static Texture2D LoadReadable(string path)
    {
        var imp = (TextureImporter)AssetImporter.GetAtPath(path);
        if (imp != null && !imp.isReadable)
        {
            imp.isReadable = true;
            imp.SaveAndReimport();
        }
        return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
    }

    // ── 4. URP Lit 材质 + AnimatorController ──
    static Material CreateMaterial(Texture2D mask)
    {
        var existing = AssetDatabase.LoadAssetAtPath<Material>(MatPath);
        if (existing != null) return existing;

        var shader = Shader.Find("Universal Render Pipeline/Lit");
        if (shader == null)
        {
            Debug.LogWarning("[JugongAutoImport] 未找到 URP Lit shader。");
            return null;
        }

        var mat = new Material(shader);
        var baseTex = LoadTexture("online.png");
        var normalTex = LoadTexture("normal");

        if (baseTex != null)
            mat.SetTexture("_BaseMap", baseTex);
        if (normalTex != null)
        {
            mat.SetTexture("_BumpMap", normalTex);
            mat.SetFloat("_BumpScale", 1f);
            mat.EnableKeyword("_NORMALMAP");
        }
        if (mask != null)
        {
            mat.SetTexture("_MetallicGlossMap", mask);
            mat.SetFloat("_Metallic", 1f);
            mat.SetFloat("_Smoothness", 1f);
            mat.EnableKeyword("_METALLICSPECGLOSSMAP");
        }

        AssetDatabase.CreateAsset(mat, MatPath);
        return mat;
    }

    static Texture2D LoadTexture(string nameSuffix)
    {
        return AssetDatabase.FindAssets("t:Texture2D", new[] { ModelDir })
            .Select(AssetDatabase.GUIDToAssetPath)
            .Where(p => Path.GetFileName(p).ToLowerInvariant().EndsWith(nameSuffix))
            .Where(p => !p.EndsWith(MaskPath))
            .Select(AssetDatabase.LoadAssetAtPath<Texture2D>)
            .FirstOrDefault(t => t != null);
    }

    static AnimatorController CreateAnimatorController(AnimationClip clip)
    {
        if (clip == null) return null;

        var existing = AssetDatabase.LoadAssetAtPath<AnimatorController>(ControllerPath);
        if (existing != null) return existing;

        var controller = AnimatorController.CreateAnimatorControllerAtPath(ControllerPath);
        var rootSM = controller.layers[0].stateMachine;
        // 注意：带位置参数的 AddState 重载直接返回 AnimatorState（而非 ChildAnimatorState）
        var state = rootSM.AddState(clip.name, new Vector3(250f, 120f, 0f));
        state.motion = clip;
        rootSM.defaultState = state;
        AssetDatabase.SaveAssets();
        return controller;
    }

    // ── 5. 包围盒计算（根变换重置后的世界空间）──
    static bool CalculateWorldBounds(GameObject root, out Bounds bounds)
    {
        bounds = default;
        bool has = false;

        foreach (var smr in root.GetComponentsInChildren<SkinnedMeshRenderer>(true))
        {
            if (smr.sharedMesh == null) continue;
            Encapsulate(ref bounds, ref has, smr.transform.localToWorldMatrix, smr.sharedMesh.bounds);
        }
        foreach (var mf in root.GetComponentsInChildren<MeshFilter>(true))
        {
            if (mf.sharedMesh == null) continue;
            Encapsulate(ref bounds, ref has, mf.transform.localToWorldMatrix, mf.sharedMesh.bounds);
        }
        return has;
    }

    static void Encapsulate(ref Bounds bounds, ref bool has, Matrix4x4 matrix, Bounds local)
    {
        var corners = new Vector3[8];
        for (int i = 0; i < 8; i++)
        {
            Vector3 ext = local.extents;
            corners[i] = matrix.MultiplyPoint3x4(local.center + new Vector3(
                (i & 1) == 0 ? -ext.x : ext.x,
                (i & 2) == 0 ? -ext.y : ext.y,
                (i & 4) == 0 ? -ext.z : ext.z));
        }
        var min = Vector3.positiveInfinity;
        var max = Vector3.negativeInfinity;
        foreach (var c in corners) { min = Vector3.Min(min, c); max = Vector3.Max(max, c); }
        var world = new Bounds((min + max) * 0.5f, max - min);
        if (!has) { bounds = world; has = true; }
        else bounds.Encapsulate(world);
    }
}
