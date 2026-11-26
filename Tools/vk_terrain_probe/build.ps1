# 构建并运行 vk_terrain_probe（Windows / VS2022 / Vulkan SDK）
# 用法： pwsh -ExecutionPolicy Bypass -File .\build.ps1 [-VulkanSdk C:\VulkanSDK\1.4.313.2]
param(
    [string]$VulkanSdk = $env:VULKAN_SDK
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

if (-not $VulkanSdk -or -not (Test-Path $VulkanSdk)) {
    $candidate = Get-ChildItem "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $candidate) { throw "未找到 Vulkan SDK，请用 -VulkanSdk 指定" }
    $VulkanSdk = $candidate.FullName
}
Write-Host "Vulkan SDK: $VulkanSdk"

$vcvars = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars64.bat"
}
if (-not (Test-Path $vcvars)) { throw "未找到 vcvars64.bat，请在 VS2022 开发者命令行里运行" }

$glslang = Join-Path $VulkanSdk "Bin\glslangValidator.exe"
& $glslang -V --target-env vulkan1.2 probe.comp -o probe.spv
if ($LASTEXITCODE -ne 0) { throw "glslangValidator 失败" }

$cl = @"
call "$vcvars" >nul 2>&1 && cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 probe.cpp /I "$VulkanSdk\Include" /Fe:probe.exe /link /LIBPATH:"$VulkanSdk\Lib" vulkan-1.lib && cl /nologo /utf-8 /std:c++17 /EHsc /O2 sz.cpp /I "$VulkanSdk\Include" /Fe:sz.exe /link /LIBPATH:"$VulkanSdk\Lib" vulkan-1.lib
"@
cmd /c $cl
if ($LASTEXITCODE -ne 0) { throw "MSVC 编译失败" }

Write-Host "`n=== probe ===" -ForegroundColor Cyan
& .\probe.exe
Write-Host "`n=== AS sizes ===" -ForegroundColor Cyan
& .\sz.exe
