#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ARM → Unity MetallicSmoothness 纹理转换工具

输入: ARM 纹理（glTF/ORM 标准布局: R=AO, G=Roughness, B=Metallic）
输出: Unity MetallicSmoothness 布局 (R=Metallic, A=Smoothness=1-Roughness, G/B=0)
      保存到输入纹理同目录下，命名为 <输入纹理名>_ms.png

通道映射（字节级直拷，不做色彩空间转换，ARM 按约定为 linear 数据）:
    输出 R  = 输入 B  (Metallic)
    输出 A  = 255 - 输入 G  (Smoothness = 1 - Roughness)
    输出 GB = 0

用法:
    python arm_to_unity_ms.py <纹理或目录> [更多路径...]
    python arm_to_unity_ms.py Models/room/textures/ARM_wood.png
    python arm_to_unity_ms.py Models/room/textures/          # 递归批量处理目录

Unity 导入设置提示（很重要）:
    - sRGB (Color Texture) 必须关闭（linear 数据）
    - Alpha Is Transparency 关闭（A 是数据通道，不是透明度）
"""

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("错误: 缺少 Pillow，请先安装: pip install Pillow", file=sys.stderr)
    sys.exit(1)

MS_SUFFIX = "_ms"          # 输出文件名后缀（输出固定为 .png）
SUPPORTED_EXTS = {".png", ".tga", ".jpg", ".jpeg", ".bmp"}


def convert(arm_path: Path) -> Path:
    """将单张 ARM 纹理转换为 Unity MetallicSmoothness 布局，返回输出路径。"""
    img = Image.open(arm_path)
    # ARM 布局只关心 RGB，丢弃可能存在的 alpha/额外通道
    rgb = img.convert("RGB")
    r, g, b = rgb.split()
    # r=AO(丢弃), g=Roughness, b=Metallic
    smoothness = g.point(lambda v: 255 - v)          # A = 1 - roughness
    zero = Image.new("L", rgb.size, 0)
    out = Image.merge("RGBA", (b, zero, zero, smoothness))

    out_path = arm_path.with_name(arm_path.stem + MS_SUFFIX + ".png")
    out.save(out_path)
    return out_path


def collect_inputs(paths) -> list:
    """展开命令行参数为待处理文件列表（支持文件与目录，目录递归；去重；跳过输出文件）。"""
    files, seen = [], set()
    for p in map(Path, paths):
        if p.is_dir():
            candidates = sorted(f for f in p.rglob("*")
                                if f.is_file() and f.suffix.lower() in SUPPORTED_EXTS)
        elif p.is_file():
            candidates = [p]
        else:
            print(f"[警告] 路径不存在: {p}", file=sys.stderr)
            continue
        for f in candidates:
            if f.suffix.lower() not in SUPPORTED_EXTS:
                print(f"[警告] 跳过不支持的格式: {f}", file=sys.stderr)
                continue
            if f.stem.endswith(MS_SUFFIX):           # 跳过本脚本自身的输出，避免重复转换
                continue
            key = f.resolve()
            if key in seen:
                continue
            seen.add(key)
            files.append(f)
    return files


def main() -> None:
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return

    files = collect_inputs(args)
    if not files:
        print("未找到任何待处理的纹理文件", file=sys.stderr)
        sys.exit(1)

    ok = fail = 0
    for f in files:
        try:
            out = convert(f)
            print(f"[OK] {f.name} -> {out.name}")
            ok += 1
        except Exception as e:
            print(f"[失败] {f}: {e}", file=sys.stderr)
            fail += 1

    print(f"完成: {ok} 成功, {fail} 失败")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
