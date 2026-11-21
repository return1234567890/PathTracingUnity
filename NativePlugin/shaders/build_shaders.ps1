# ============================================================
#  URPPT — 着色器构建（Ray Tracing Pipeline / Slang）
#
#  把 pt.slang 编译成 SPIR-V，再转成 C++ 内嵌头文件：
#      shaders/pt.slang + pt_common.slang
#        → shaders/pt.spv            (由 spv_to_header.ps1 读入)
#        → src/pathtrace_spv.h       (g_PathTraceSpv[] / g_PathTraceSpv_size)
#
#  用法（在 NativePlugin 目录）：
#      pwsh -File shaders\build_shaders.ps1
#      pwsh -File shaders\build_shaders.ps1 -SkipHeader      # 只出 .spv
#
#  之后照常重建插件：cmake --build build --config Release
#
#  ── 工具链版本（固定，避免 slangc 漂移导致 SPIR-V 变化、难以二分）──
#      slangc : Vulkan SDK 1.4.313.2 自带（Slang 2025.x）
#               C:\VulkanSDK\1.4.313.2\Bin\slangc.exe
#      target : SPIR-V 1.4（sm_6_6+spirv_1_4）
#               1.1 设备靠 VK_KHR_spirv_1_4（拦截器已注入）；1.2/1.3 核心即支持 1.4。
#               不要改成 1.5/1.6：Unity 的实例可能只到 1.1，模块会被拒。
#
#  ── 参数说明（改之前先看方案 P1）──
#      -fvk-use-entrypoint-name  必须。否则 7 个入口名被修饰，pName 找不到，
#                                vkCreateRayTracingPipelinesKHR 链接失败。
#      -O0                       本工程约定让驱动优化。
#      -emit-spirv-directly      跳过中间 GLSL 路径。
#      -I                        让 pt.slang 里的 #include "pt_common.slang" 生效。
#      ✗ 不要加 -force-glsl-scalar-layout：会改掉 208B UBO / 80B 顶点等布局。
#
#  注意：skinning.comp.glsl 是独立的 GLSL compute（push constant + BDA），
#        与本脚本无关，仍用 glslangValidator 单独编译，本次不做改动。
# ============================================================

param(
    [string]$Slangc  = "",
    [switch]$SkipHeader
)

$ErrorActionPreference = "Stop"
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$root   = Split-Path -Parent $here            # NativePlugin/
$src    = Join-Path $root "src"

if (-not $Slangc) {
    if ($env:VULKAN_SDK) {
        $Slangc = Join-Path $env:VULKAN_SDK "Bin\slangc.exe"
    } else {
        $Slangc = "slangc"
    }
}
if (-not (Test-Path $Slangc)) {
    throw "slangc not found: $Slangc  (set VULKAN_SDK or pass -Slangc)"
}

$slangVersion = "unknown"
$prevEa = $ErrorActionPreference
$ErrorActionPreference = "Continue"   # slangc -v 写 stderr 且返回 1，别让它中断脚本
try {
    $raw = (& "$Slangc" -v 2>&1 | Out-String)
    $m = [regex]::Match($raw, '\b\d{9,}\b')   # slangc -v 只输出构建时间戳
    if ($m.Success) { $slangVersion = $m.Value }
} catch { }
$ErrorActionPreference = $prevEa
Write-Host "[build_shaders] slangc : $Slangc (build $slangVersion)"
Write-Host "[build_shaders] root   : $root"

$spv  = Join-Path $here "pt.spv"
$hdr  = Join-Path $src  "pathtrace_spv.h"

# ── 1. Slang → SPIR-V（单模块，7 个 RT 入口共用一份）──────────────
& "$Slangc" (Join-Path $here "pt.slang") `
    -target spirv `
    -profile sm_6_6+spirv_1_4 `
    -fvk-use-entrypoint-name `
    -O0 `
    -emit-spirv-directly `
    -I "$here" `
    -o "$spv"
if ($LASTEXITCODE -ne 0) { throw "slangc failed with exit code $LASTEXITCODE" }

# ── 2. 自检：入口名 + 描述符绑定（G1 的第一道关，不需要开 Unity）──
$dis = Join-Path (Split-Path -Parent $Slangc) "spirv-dis.exe"
if (Test-Path $dis) {
    $entryPoints = & "$dis" $spv | Select-String "OpEntryPoint" |
        ForEach-Object { if ($_ -match 'OpEntryPoint \w+ %(\w+) "([^"]+)"') { $matches[2] } }
    $expected = @("rgenMain", "rmissMain", "rmissShadow", "rchitMain", "rahitMain", "rchitShadow", "rahitShadow")
    $missing  = $expected | Where-Object { $entryPoints -notcontains $_ }
    if ($missing) {
        throw "missing entry point(s) in SPIR-V: $($missing -join ', '). Did you drop -fvk-use-entrypoint-name?"
    }
    Write-Host "[build_shaders] entry points OK: $($entryPoints -join ', ')"
} else {
    Write-Warning "spirv-dis.exe not found next to slangc; skipping entry-point self-check"
}

$spvLen = (Get-Item $spv).Length
Write-Host "[build_shaders] SPIR-V : $spv ($spvLen bytes)"

# ── 3. SPIR-V → C++ 头（脚本本身不改，仍是一个数组）──────────────
if (-not $SkipHeader) {
    & (Join-Path $here "spv_to_header.ps1") -spvPath $spv -headerPath $hdr -varName "g_PathTraceSpv"
    if ($LASTEXITCODE -ne 0) { throw "spv_to_header.ps1 failed" }
    Write-Host "[build_shaders] header : $hdr"
}

Write-Host "[build_shaders] done. Rebuild the plugin: cmake --build build --config Release"
