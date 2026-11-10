# NVIDIA NGX DLSS SDK (v310.7.0)

## 来源

从 https://github.com/NVIDIA/DLSS 下载 SDK Release 包，版本 310.7.0。

## 目录结构

```
third_party/ngx/
├── include/                    # 16 个头文件（已就位）
│   ├── nvsdk_ngx.h             # 主 API
│   ├── nvsdk_ngx_vk.h          # Vulkan API
│   ├── nvsdk_ngx_helpers.h     # Helper 函数
│   ├── nvsdk_ngx_helpers_vk.h  # Vulkan Helper
│   ├── nvsdk_ngx_defs*.h      # 定义（DLSS SR / RR）
│   ├── nvsdk_ngx_params*.h    # 参数结构
│   └── ...                     # 共 16 个
├── lib/                        # 导入库（已就位）
│   ├── nvsdk_ngx_d.lib         # Release 动态链接（对应 nvngx_dlss.dll）
│   └── nvsdk_ngx_d_dbg.lib     # Debug 动态链接（含断言）
└── README.md
```

## 运行时 DLL

运行时 DLL 已复制到 `Assets/Plugins/x86_64/`（开发版，含 debug overlay）：

| DLL | 用途 | 大小 |
|---|---|---|
| `nvngx_dlss.dll` | DLSS SR（超分） | 69 MB |
| `nvngx_dlssd.dll` | DLSS RR（Ray Reconstruction） | 41 MB |

生产构建时替换为 `lib/Windows_x86_64/rel/` 下的生产版 DLL。

## 库命名说明

- `_d` 后缀 = 动态链接（运行时加载 DLL）
- `_s` 后缀 = 静态链接（嵌入二进制）
- `_dbg` 后缀 = Debug 断言版

本项目使用 `_d` / `_d_dbg`（动态链接），插件运行时由 Unity 加载 `nvngx_dlss.dll`。
