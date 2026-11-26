#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
体积雾数学探针（对应 doc/PT体积雾接入可行性调研.md §5.2 / §5.3 / §5.7 / §10.0）

不依赖 Unity / Vulkan，纯 CPU 验证四件事：
  1) 指数高度雾的解析光学深度 tau(L) 与高密度数值积分一致；
  2) 自由程反演采样（闭式、无拒绝）的「无事件概率」等于 T(L)，且散射积分估计无偏；
  3) 透射率记账：主路径「命中表面权重=1」无偏，若再乘一次 T(L) 会引入 T^2 偏差；
  4) Henyey-Greenstein 相函数归一化与采样期望。

约定：世界坐标 (x, y, z)，y 为高度；密度 beta(y) = beta0 * exp(-(y-y0)/H)。
注意：所有测试段都留在 y >= Y0 之上，否则会撞上「地面钳断」（见文档 §5.2）。

用法:  python Tools/fog_math_probe.py
输出同时写 Tools/fog_math_probe_results.txt（UTF-8）。
"""
import math
import os
import numpy as np

rng = np.random.default_rng(20261121)

BETA0 = 0.03     # 基准高度处的消光, 1/m（轻雾，能见度约 200 m）
H = 8.0          # 高度衰减尺度, m
Y0 = 0.0         # 基准高度, m
EPS_DY = 1e-6
OUT = []


def log(s=""):
    print(s)
    OUT.append(s)


def ok(name, cond, detail):
    tag = "PASS" if cond else "FAIL"
    if not cond:
        FAILS.append(name)
    log(f"  [{tag}] {name}: {detail}")


FAILS = []


def dens(y):
    """消光系数沿高度分布 beta(y)"""
    return BETA0 * np.exp(-np.clip(y - Y0, 0.0, None) / H)


def tau_analytic(a, d, L):
    """解析光学深度：|dy|>eps 用闭式，否则退化为常数密度（y = index 1）"""
    dy = d[1]
    dh = a[1] - Y0
    base = BETA0 * math.exp(-dh / H)
    if abs(dy) < EPS_DY:
        return base * L
    return base * (H / dy) * (1.0 - math.exp(-dy * L / H))


def tau_numeric(a, d, L, n=40000):
    s = np.linspace(0.0, L, n)
    y = a[1] + s * d[1]
    return float(np.trapezoid(dens(y), s))


def sample_free_flight(a, d, Lmax):
    """闭式反演自由程；返回 (t, hit) —— hit=False 表示段内无散射"""
    total = tau_analytic(a, d, Lmax)
    if total <= 1e-12:
        return None, False
    u = rng.random()
    tau_star = -math.log(1.0 - u)
    if tau_star >= total:
        return None, False
    dy = d[1]
    dh = a[1] - Y0
    base = BETA0 * math.exp(-dh / H)
    if abs(dy) < EPS_DY:
        t = tau_star / base
    else:
        A = base * H / dy                      # 渐近上限（带符号）
        t = -(H / dy) * math.log(1.0 - tau_star / A)
    return (t, True) if t < Lmax else (None, False)


def hg_phase(g, cos_theta):
    denom = 1.0 + g * g - 2.0 * g * cos_theta
    return (1.0 - g * g) / (4.0 * math.pi * denom ** 1.5)


log("=" * 74)
log("1) 解析 tau(L) vs 数值积分（H=%.1f m, beta0=%.3f /m）" % (H, BETA0))
log("=" * 74)

cases = {
    # (起点, 方向, 段长列表)
    "向上 dy=+0.80": (np.array([0.0, 1.0, 0.0]), np.array([0.0, 0.8, 0.6]), [0.5, 5.0, 50.0, 400.0]),
    "向下 dy=-0.60": (np.array([0.0, 12.0, 0.0]), np.array([0.0, -0.6, 0.8]), [0.5, 5.0, 15.0, 19.9]),
    "水平 dy= 0.00": (np.array([0.0, 3.0, 0.0]), np.array([0.6, 0.0, 0.8]), [0.5, 5.0, 50.0, 400.0]),
    "掠射 dy=+0.01": (np.array([0.0, 2.0, 0.0]), np.array([0.0, 0.01, 0.99995]), [0.5, 5.0, 50.0, 400.0]),
}

for name, (a, d, lengths) in cases.items():
    for L in lengths:
        ta = tau_analytic(a, d, L)
        tn = tau_numeric(a, d, L)
        rel = abs(ta - tn) / max(tn, 1e-12)
        ok(f"{name} L={L:g}m", rel < 1e-5,
           f"analytic={ta:.6f}  numeric={tn:.6f}  rel_err={rel:.2e}  T={math.exp(-ta):.4f}")

log()
log("=" * 74)
log("2) 自由程反演：无事件概率应等于 T(L)；散射项估计应无偏")
log("=" * 74)

N = 400_000
a = np.array([0.0, 1.0, 0.0])
d = np.array([0.0, 0.6, 0.8])
L = 30.0
T_exact = math.exp(-tau_analytic(a, d, L))

n_hit = 0
for _ in range(N):
    t, hit = sample_free_flight(a, d, L)
    if hit:
        n_hit += 1
p_nohit = 1.0 - n_hit / N
ok("无事件概率 == T(L)", abs(p_nohit - T_exact) < 4e-3,
   f"MC={p_nohit:.4f}  T(L)={T_exact:.4f}  偏差={abs(p_nohit - T_exact):.2e} (N={N})")

albedo = 0.6
true_scatter = albedo * (1.0 - T_exact)
acc = 0.0
for _ in range(N):
    t, hit = sample_free_flight(a, d, L)
    if hit:
        acc += albedo          # 权重 = sigma_s/beta，与 t 无关
mc_scatter = acc / N
ok("散射项估计无偏", abs(mc_scatter - true_scatter) < 4e-3,
   f"MC={mc_scatter:.4f}  analytic={true_scatter:.4f}  偏差={abs(mc_scatter-true_scatter):.2e}")

S = 3.0
surf_correct = (1.0 - n_hit / N) * S
surf_wrong = (1.0 - n_hit / N) * S * T_exact
ok("表面项权重=1 无偏", abs(surf_correct - T_exact * S) < 1.5e-2,
   f"MC={surf_correct:.4f}  T*S={T_exact*S:.4f}")
ok("表面项再乘 T 会偏暗（应被检出）", surf_wrong < 0.9 * T_exact * S,
   f"误乘后={surf_wrong:.4f}，相对真值 {surf_wrong/(T_exact*S)*100:.1f}%（=T，即 T^2 偏差）")

log()
log("=" * 74)
log("3) 相函数归一化 ∫p dω = 1 与采样一致性")
log("=" * 74)

for g in (-0.6, 0.0, 0.5, 0.85):
    mu = np.linspace(-1.0, 1.0, 400001)
    integ = np.trapezoid(np.array([hg_phase(g, m) for m in mu]), mu) * 2.0 * math.pi
    ok(f"HG 归一化 g={g:+.2f}", abs(integ - 1.0) < 2e-3, f"integral={integ:.6f}")

g = 0.5
us = rng.random(200_000)
cos_s = (1.0 + g * g - ((1.0 - g * g) / (1.0 - g + 2.0 * g * us)) ** 2) / (2.0 * g)
emp = float(np.mean(cos_s))
ok("HG 采样 E[cosθ]=g", abs(emp - g) < 8e-3, f"E[cos]={emp:.4f}  g={g}")

log()
log("=" * 74)
if FAILS:
    log(f"结果: {len(FAILS)} 项 FAIL -> {FAILS}")
    status = 1
else:
    log("结果: 全部 PASS")
    status = 0
log("=" * 74)

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fog_math_probe_results.txt")
with open(out_path, "w", encoding="utf-8") as f:
    f.write("\n".join(OUT) + "\n")
raise SystemExit(status)
