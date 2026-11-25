# Upstream Proposal: 2-Pass Separable Catmull-Rom Sampling for `NLSPlus.glsl`

- **Tracking:** Package `Q#12` (`GEM-079` — `NLS upstream`, doc-only proposal)
- **Target upstream file:** `shaders/NLSPlus.glsl` (Bill Slack, `Copyright (C) 2026 Bill Slack`, `SPDX-License-Identifier: GPL-3.0-or-later`)
- **Related divergence entry:** `DIV-D-03` in `docs/design/upstream-divergence-register.md` (`sampling` dynamic parameter)

---

## 1. Scope Statement

**Upstream proposal ONLY — ZERO edits to Bill's `shaders/NLSPlus.glsl` (or any file in VP's code/shader/test tree).**

`shaders/NLSPlus.glsl` and `shaders/NLS.glsl` are Bill-owned upstream files (Rule 5 boundary; see `docs/design/migration-m2-plan.md`). This document records the problem analysis, mathematical separability derivation, fp16/fp32 rounding error bounds, separability failure boundaries, and parity verification protocol for an **upstream PR / proposal** to Bill Slack's repository. No production code, shader, or test file in the VP-V worktree is modified by this package.

---

## 2. Exact Problem Summary

### 2.1 Where the 2D `N×N` Loop Lives (`[S]:331` / `GEM-079`)

VP's renderer profile setting `nls_sampling` (`catmull` default, `bilinear` legacy; wired via `SetNlsSampling()` in `src/VP-V/modules/control/skeleton/RendererHostHelpers.cpp:181-190`, called from `LibplaceboVideoRenderer.cpp:11070, 11104`) sets the dynamic parameter `sampling` in `shaders/NLSPlus.glsl:27-32`:

- **`sampling < 0.5` (`nls_sampling = bilinear`):** Executes Bill's original single hardware bilinear fetch `HOOKED_tex(sample_pos)` (`shaders/NLSPlus.glsl:191-192`). Unaffected by this proposal.
- **`sampling >= 0.5` (`nls_sampling = catmull`):** Calls `nls_sample(sample_pos, slope, bvec2(true, true))` (`shaders/NLSPlus.glsl:206`), which evaluates a **single-pass 2D nested loop** over up to `NLS_MAX_TAPS × NLS_MAX_TAPS` texels (`shaders/NLSPlus.glsl:101, 105-144`):

```glsl
// shaders/NLSPlus.glsl:101, 124-143
#define NLS_MAX_TAPS 9
...
    for (int j = 0; j < NLS_MAX_TAPS; j++) {
        if (j >= ny)
            break;
        int y = lo.y + j;
        float wy = warped.y ? nls_catmull((float(y) - c.y) / sc.y) : 1.0;
        vec4 row = vec4(0.0);
        float rsum = 0.0;
        for (int i = 0; i < NLS_MAX_TAPS; i++) {
            if (i >= nx)
                break;
            int x = lo.x + i;
            float wx = warped.x ? nls_catmull((float(x) - c.x) / sc.x) : 1.0;
            row += wx * texelFetch(HOOKED_raw, clamp(ivec2(x, y), ivec2(0), hi), 0);
            rsum += wx;
        }
        acc += wy * row;
        wsum += wy * rsum;
    }
    return HOOKED_mul * (acc / wsum);
```

Because Catmull-Rom (`nls_catmull`, lines `90-98`) has support $|x| < 2$ and the per-axis minification widening factor is clamped to $s_c = \operatorname{clamp}(\text{slope}, 1.0, 2.0)$ (line `109`), each warped axis spans half-width $R = 2 s_c \in [2, 4]$ texels, giving $n \in [3, 9]$ taps per axis (`nx`, `ny` at lines `115-121`):

| Regime in `NLSPlus.glsl` (`sampling >= 0.5`) | `nx × ny` (1-pass 2D) | `nx + ny` (2-pass separable) | Tap reduction |
|---|---:|---:|---:|
| Both axes at `sc = 1.0` (fractional offset) | $4 \times 4 = 16$ | $4 + 4 = 8$ | $2.0\times$ |
| Typical balanced stretch (`sc = (1.5, 1.0)`) | $6 \times 4 = 24$ | $6 + 4 = 10$ | $2.4\times$ |
| Balanced stretch near edges where complementary axis also minifies (`sc = (1.35, 1.35)`) | $6 \times 6 = 36$ | $6 + 6 = 12$ | $3.0\times$ |
| Worst-case widened both axes (`sc = (2.0, 2.0)`) | $9 \times 9 = 81$ | $9 + 9 = 18$ | $4.5\times$ |
| Degenerate 1-axis (`axis_balance == 0`, currently still passes `bvec2(true, true)` at line `206`) | $(4\text{–}9) \times (3\text{–}4) = 12\text{–}36$ | $4\text{–}9$ (1 pass if identity axis gated) | $3.0\text{–}4.0\times$ |

At 3840×2160 (`~8.29M` pixels), $24\text{–}81$ `texelFetch` calls per pixel equals **199M–672M fetches per frame** inside a single `MAIN` hook pass.

Note also that **both** axes in `NLSPlus.glsl` can minify (`slope > 1.0`) in different regions of the frame:
- On the primary axis (`scale > 1.0`), `slope > 1.0` at the center (`radius <= center_protection` or small `r`).
- On the complementary axis (`scale < 1.0`), the derivative at the edge exceeds $1.0$: for `geometry == 0` (power curve, lines `46-47`), $f'(1) = 1 + (c - 1)(1 - s) > 1$ (e.g., at `ratio = 1.5`, `balance = 0.5`, `max_center_zoom = 1.25`, `curve = 2.9`: $s \approx 0.8165 \implies f'(1) \approx 1.349$); for `geometry == 1` (protected center, lines `65-66`), `edge_slope = max(0.20, 1.0 - 0.5 * (scale - 1.0) * curve)` similarly exceeds $1.0$ when `scale < 1.0` ($\approx 1.266$).

### 2.2 Interaction with VP's `NlsMaskMapping`

VP-V's `NlsMaskMapping` (`src/VP-V/modules/scaling/output/NlsMaskMapping.h`) mirrors `NLSPlus.glsl`'s exact per-axis coordinate map (`xScale`, `yScale`, `curve`, `geometry`, `xProtect`, `yProtect`) so downstream VP-V stages (occlusion mask, adaptive downscale, dynamic drive) track source pixels through NLS. Because a 2-pass separable split preserves the **identical** coordinate map $(u(x), v(y))$, `NlsMaskMapping` requires **zero changes**.

### 2.3 When `NLSPlus.glsl` Is Separable-Eligible vs. Non-Separable

| Path / Warp Class | Separable-Eligible? | Reason |
|---|---|---|
| `NLSPlus.glsl`, `sampling < 0.5` (`bilinear`) | N/A (already 1 fetch) | Hardware bilinear lookup `HOOKED_tex(sample_pos)` (line `192`). |
| `NLS.glsl`, `sampling >= 0.5` (1-axis NLS) | **Do not split** | `NLS.glsl:180-181` already passes `bvec2(!vertical_warp, vertical_warp)`, so the unwarped axis executes 1 tap ($O(R)$ in 1 pass). Adding a 2nd pass would only add full-frame intermediate bandwidth. |
| `NLSPlus.glsl`, `balance == 0` or `strength == 0` or `ratio == 1.0` | **Single-pass 1-axis gate** | One or both axes have `scale == 1.0` (identity map). Passing `bvec2(abs(x_scale - 1.0) > 1e-6, abs(y_scale - 1.0) > 1e-6)` (and skipping Pass 2 via `//!WHEN`) avoids both the redundant 3–4 taps on the identity axis and any intermediate pass. |
| `NLSPlus.glsl`, current 2-axis balanced map (`balance > 0`, `ratio > 1.0`, `sampling >= 0.5`) | **Yes — exact tensor-product kernel** | `sample_pos.x` and `slope.x` depend *only* on `HOOKED_pos.x`; `sample_pos.y` and `slope.y` depend *only* on `HOOKED_pos.y` (`NLSPlus.glsl:186-205`, line `195`: *"The map is separable, so the two slopes are the whole local scale"*). |
| Coupled / non-separable 2D warps ($\partial u/\partial y \ne 0$ or $\partial v/\partial x \ne 0$: radial/elliptical NLS, barrel/pincushion, corner-coupled protection $p_x(y)$, anisotropic rotated EWA) | **No — strictly non-separable** | Off-diagonal Jacobian terms couple the axes; a 2-pass axis-aligned factorization introduces geometric shear/aliasing error and cannot reproduce the 2D footprint (see §3.4). |

---

## 3. Mathematical Separability Derivation & Rounding Error Bound

### 3.1 Algebraic Equivalence: 1-Pass 2D $O(R^2)$ vs. 2-Pass Separable $O(2R)$

Let the source texture `HOOKED_raw` have dimensions $W \times H$ (`HOOKED_size`), normalization multiplier $M = \text{HOOKED\_mul}$, and output pixel grid $(x_{\text{out}}, y_{\text{out}}) \in \{0, \dots, W-1\} \times \{0, \dots, H-1\}$ (since `//!HOOK MAIN` has no `//!WIDTH`/`//!HEIGHT` resize; linear aspect scaling occurs downstream in libplacebo's scaler).

Define the per-axis mapped texel coordinates and clamped local slopes from `shaders/NLSPlus.glsl:108-109, 186-205`:

$$c_x(x_{\text{out}}) = u(x_{\text{out}})\, W - \tfrac{1}{2}, \qquad s_x(x_{\text{out}}) = \operatorname{clamp}\!\left(\left|\frac{u(x_{\text{out}} + \tfrac{1}{2}\text{pt}_x) - u(x_{\text{out}} - \tfrac{1}{2}\text{pt}_x)}{\text{pt}_x}\right|,\, 1,\, 2\right)$$

$$c_y(y_{\text{out}}) = v(y_{\text{out}})\, H - \tfrac{1}{2}, \qquad s_y(y_{\text{out}}) = \operatorname{clamp}\!\left(\left|\frac{v(y_{\text{out}} + \tfrac{1}{2}\text{pt}_y) - v(y_{\text{out}} - \tfrac{1}{2}\text{pt}_y)}{\text{pt}_y}\right|,\, 1,\, 2\right)$$

With Catmull-Rom weights $w_x(x_i; x_{\text{out}}) = K\!\left(\frac{x_i - c_x(x_{\text{out}})}{s_x(x_{\text{out}})}\right)$ and $w_y(y_j; y_{\text{out}}) = K\!\left(\frac{y_j - c_y(y_{\text{out}})}{s_y(y_{\text{out}})}\right)$, and clamped indices $\tilde{x}_i = \operatorname{clamp}(x_i, 0, W-1)$, $\tilde{y}_j = \operatorname{clamp}(y_j, 0, H-1)$, the existing 1-pass 2D loop (`shaders/NLSPlus.glsl:124-143`) evaluates:

$$I_{\text{out}}^{(1\text{-pass})}(x_{\text{out}}, y_{\text{out}}) = M \cdot \frac{\sum_{j=0}^{n_y-1} w_y(y_j; y_{\text{out}}) \left(\sum_{i=0}^{n_x-1} w_x(x_i; x_{\text{out}})\, I_{\text{src}}(\tilde{x}_i, \tilde{y}_j)\right)}{\sum_{j=0}^{n_y-1} w_y(y_j; y_{\text{out}}) \left(\sum_{i=0}^{n_x-1} w_x(x_i; x_{\text{out}})\right)}$$

Because $x_i$, $n_x$, and $w_x(x_i; x_{\text{out}})$ are **independent of $y_j$ and $y_{\text{out}}$**, the row weight sum $W_x(x_{\text{out}}) = \sum_{i=0}^{n_x-1} w_x(x_i; x_{\text{out}})$ (`rsum` at line `137`) is identical for every $j$ and factors out of the $j$-summation cleanly:

- **Pass 1 (Horizontal warp at source resolution $W \times H$, saved to unclamped float `//!SAVE` intermediate $T$):**
  $$T(x_{\text{out}}, y) = M \cdot \frac{\sum_{i=0}^{n_x(x_{\text{out}})-1} w_x(x_i; x_{\text{out}})\, I_{\text{src}}(\tilde{x}_i, y)}{W_x(x_{\text{out}})}, \qquad y \in \{0, \dots, H-1\}$$
  *(Note: $M = \text{HOOKED\_mul}$ is applied once in Pass 1 so $T$ is in normalized units; Pass 2 multiplies by `PASS1_mul`, which is $1.0$ for float intermediates.)*

- **Pass 2 (Vertical warp reading $T$ at fixed column $x_{\text{out}}$):**
  $$I_{\text{out}}^{(2\text{-pass})}(x_{\text{out}}, y_{\text{out}}) = \frac{\sum_{j=0}^{n_y(y_{\text{out}})-1} w_y(y_j; y_{\text{out}})\, T(x_{\text{out}}, \tilde{y}_j)}{W_y(y_{\text{out}})}, \qquad W_y(y_{\text{out}}) = \sum_{j=0}^{n_y-1} w_y(y_j; y_{\text{out}})$$

Substituting $T(x_{\text{out}}, \tilde{y}_j)$ into Pass 2 gives:

$$I_{\text{out}}^{(2\text{-pass})}(x_{\text{out}}, y_{\text{out}}) = M \cdot \frac{\sum_{j=0}^{n_y-1} w_y(y_j; y_{\text{out}}) \sum_{i=0}^{n_x-1} w_x(x_i; x_{\text{out}})\, I_{\text{src}}(\tilde{x}_i, \tilde{y}_j)}{W_y(y_{\text{out}}) \cdot W_x(x_{\text{out}})} = I_{\text{out}}^{(1\text{-pass})}(x_{\text{out}}, y_{\text{out}})$$

In exact real arithmetic:
1. **The two operators are algebraically identical** (including boundary clamping, since Pass 1 clamps $\tilde{x}_i \in [0, W-1]$ for every source row $y \in [0, H-1]$ and Pass 2 clamps $\tilde{y}_j \in [0, H-1]$ on the $W \times H$ intermediate).
2. **Complexity drops from $O(R_x R_y)$ to $O(R_x + R_y)$** ($n_x n_y \le 81$ fetches/pixel $\to$ $n_x + n_y \le 18$ fetches/pixel), trading inner-loop texture fetches for one $W \times H$ intermediate write + read.
3. **Pass order (`H → V` vs `V → H`) is algebraically commutative**; `H → V` is preferred for horizontal cache-line locality in Pass 1.

### 3.2 Catmull-Rom Sidelobe Range & Lebesgue Constant (Why Signed Float Is Mandatory)

From `nls_catmull(x)` (`shaders/NLSPlus.glsl:90-98`):

$$K(x) = \begin{cases} 1 - \tfrac{5}{2}x^2 + \tfrac{3}{2}x^3, & 0 \le x < 1 \\ 2 - 4x + \tfrac{5}{2}x^2 - \tfrac{1}{2}x^3 = -\tfrac{1}{2}(x-1)(x-2)^2, & 1 \le x < 2 \\ 0, & x \ge 2 \end{cases}$$

- **Negative sidelobe extremum:** $K'(x) = 0$ on $(1, 2)$ at $x = \frac{4}{3}$, giving $K\!\left(\frac{4}{3}\right) = -\frac{2}{27} \approx -0.074074$.
- **Discrete worst-case undershoot / overshoot and 1D Lebesgue constant $\Lambda_1$:** For $s_c = 1.0$ at fractional offset $f \in [0, 1)$, the two negative taps sum to $-\frac{1}{2} f(1 - f)$, minimized at $f = \frac{1}{2}$:
  $$w_{\text{neg,sum}}\!\left(\tfrac{1}{2}\right) = -\frac{1}{8} = -0.125, \qquad w_{\text{pos,sum}}\!\left(\tfrac{1}{2}\right) = \frac{9}{8} = 1.125$$
  $$\Lambda_1 = \max_{s_c \in [1, 2],\, f} \frac{\sum_i |w_i|}{\sum_i w_i} = \frac{9}{8} + \frac{1}{8} = \frac{5}{4} = 1.25$$
  *(For reference, in the widened continuous limit $s_c \to \infty$, $\int_{-2}^{2} K(x)\,dx = 1$ and $\int_{-2}^{2} |K(x)|\,dx = \frac{7}{6} \approx 1.1667$, so $\Lambda_1 = 1.25$ is the exact discrete upper bound across all $s_c \in [1, 2]$. In 2D, $\Lambda_1^2 \le \frac{25}{16} = 1.5625$. Similarly, the 1D $L^2$ white-noise gain at $s_c = 1, f = \frac{1}{2}$ is $\|w\|_2 = \frac{\sqrt{164}}{16} \approx 0.8004$, matching `shaders/NLSPlus.glsl:87`.)*

**Consequence:** For a $[0, 1]$ input step, Pass 1 produces intermediate values in **$[-0.125,\, 1.125]$** (and slightly wider for BTB/WTW extended-range signals). An unsigned normalized intermediate (`rgba8`, `rgba16` UNORM) would clamp $[-0.125, 0)$ and $(1, 1.125]$ before Pass 2, introducing up to **$0.125$** absolute error (**$128\text{ LSB}_{10}$ / $512\text{ LSB}_{12}$**) on sharp edges. **An unclamped signed floating-point intermediate is strictly required.**

### 3.3 Exact Intermediate Precision & FP16 vs. FP32 Rounding Error Bound

Let the quantized intermediate stored after Pass 1 be $\hat{T}(x, y) = T_{\text{exact}}(x, y) + \delta_1(x, y) + \epsilon_{\text{store}}(x, y)$, where:
- $\delta_1$ is Pass 1's fp32 accumulation error ($n_x \le 9$ taps, binary32 unit roundoff $u_{32} = 2^{-24} \approx 5.960 \times 10^{-8}$),
- $|\epsilon_{\text{store}}(x, y)| \le \frac{1}{2}\operatorname{ulp}_{\text{fmt}}(T(x, y))$ is the format rounding error on storing $T$.

By linearity of Pass 2, intermediate storage error propagates into the final output with amplification bounded by the vertical 1D Lebesgue constant $\Lambda_y \le \frac{5}{4} = 1.25$:

$$\left|\Delta I_{\text{store}}\right| \le \Lambda_y \cdot \frac{1}{2}\operatorname{ulp}_{\text{fmt}}(T_{\max}) \le 1.25 \cdot \frac{1}{2}\operatorname{ulp}_{\text{fmt}}(T_{\max})$$

Crucially, because Catmull-Rom overshoots into **$[1.0, 1.125]$** on bright edges, the floating-point binary exponent increments by $+1$ relative to $[0.5, 1.0)$, **doubling** $\operatorname{ulp}_{\text{fmt}}$!

Using exact IEEE-754 significand widths ($p_{16} = 11$ bits, $p_{32} = 24$ bits) — where a normal binade $[2^e, 2^{e+1})$ has $\operatorname{ulp}_p = 2^{e-(p-1)}$ and $\frac{1}{2}\operatorname{ulp}_p = 2^{e-p}$ — and full-range display code LSBs:
- **10-bit full range:** $\Delta_{10,\text{full}} = \frac{1}{2^{10}-1} = \frac{1}{1023} \approx 9.775 \times 10^{-4}$ *(limited range $64\dots 940$: $\Delta_{10,\text{lim}} = \frac{1}{876} \approx 1.142 \times 10^{-3}$)*
- **12-bit full range:** $\Delta_{12,\text{full}} = \frac{1}{2^{12}-1} = \frac{1}{4095} \approx 2.442 \times 10^{-4}$ *(limited range $256\dots 3760$: $\Delta_{12,\text{lim}} = \frac{1}{3504} \approx 2.854 \times 10^{-4}$)*

Combining intermediate storage rounding ($\Lambda_y \cdot \frac{1}{2}\operatorname{ulp}_{\text{fmt}}$) with the worst-case 1-pass vs. 2-pass fp32 summation/normalization reassociation bound $|\delta_{\text{fp32}}| \le (n_x + n_y + 2)\Lambda_1^2 u_{32} \le 20 \times 1.5625 \times 2^{-24} \approx 1.86 \times 10^{-6}$ (and intermediate-storage RMS rounding noise $\sigma_{\text{store}} = \|w_y\|_2 \cdot \operatorname{ulp}_{\text{fmt}}/\sqrt{12}$ with $\|w_y\|_2 \in [0.8004, 1.0]$):

| Intermediate Format | Signal Band of $T$ | $\frac{1}{2}\operatorname{ulp}_{\text{fmt}}$ | Worst-Case $|\Delta I_{\text{out}}|$ (vs. 1-pass fp32) | Error in **10-bit full LSB** ($\Delta_{10}$) | Error in **12-bit full LSB** ($\Delta_{12}$) | Storage RMS Error ($\Delta_{10}$ / $\Delta_{12}$) |
|---|---|---:|---:|---:|---:|---:|
| **`rgba32f` (fp32)** | $[0.5, 1.0)$ | $2^{-25} \approx 2.98 \times 10^{-8}$ | $\le 1.90 \times 10^{-6}$ | **$0.0019\text{ LSB}$** | **$0.0078\text{ LSB}$** | $< 0.0001$ / $< 0.0001\text{ LSB}$ |
| **`rgba32f` (fp32)** | $[1.0, 1.125]$ (overshoot) | $2^{-24} \approx 5.96 \times 10^{-8}$ | $\le 1.94 \times 10^{-6}$ | **$0.0020\text{ LSB}$** | **$0.0079\text{ LSB}$** | $< 0.0001$ / $< 0.0002\text{ LSB}$ |
| **`rgba16f` (fp16)** | $[0.5, 1.0)$ | $2^{-12} \approx 2.44 \times 10^{-4}$ | $\le 3.07 \times 10^{-4}$ | **$0.31\text{ LSB}$** | **$1.26\text{ LSB}$** | $0.115\text{–}0.144$ / $0.46\text{–}0.58\text{ LSB}$ |
| **`rgba16f` (fp16)** | $[1.0, 1.125]$ (overshoot) | $2^{-11} \approx 4.88 \times 10^{-4}$ | $\le 6.12 \times 10^{-4}$ | **$0.63\text{ LSB}$** | **$2.51\text{ LSB}$** | $0.23\text{–}0.29$ / $0.92\text{–}1.15\text{ LSB}$ |
| `rgba16` (UNORM, invalid) | $[-0.125, 1.125]$ | clamps at $0, 1$ | $\le 1.25 \times 10^{-1}$ | **$128\text{ LSB}$** | **$512\text{ LSB}$** | — (breaks linearity) |

*(For limited-range 10-bit/12-bit, divide the full-range LSB counts by $1023/876 \approx 1.168$ (10-bit) and $4095/3504 \approx 1.169$ (12-bit): `rgba16f` still reaches $0.54\text{ LSB}_{10,\text{lim}}$ and $2.15\text{ LSB}_{12,\text{lim}}$ on overshoot edges — and $1.08\text{ LSB}_{12,\text{lim}}$ even on $[0.5, 1.0)$ — while `rgba32f` stays below $0.007\text{ LSB}_{12,\text{lim}}$.)*

**Precision Conclusion & libplacebo `//!SAVE` Note:**
1. **`rgba32f` achieves strict sub-LSB parity** ($\le 0.0020\text{ LSB}_{10}$, $\le 0.0079\text{ LSB}_{12}$, over two orders of magnitude below 1 LSB).
2. **`rgba16f` does NOT guarantee sub-LSB parity** at either 10-bit (exceeds $0.5\text{ LSB}_{10}$ on bright-edge ringing up to $0.63\text{ LSB}_{10}$, $0.54\text{ LSB}_{10,\text{lim}}$) or 12-bit (exceeds $1\text{ LSB}_{12}$ across both $[0.5, 1.0)$ and $[1.0, 1.125]$ at $1.26\text{–}2.51\text{ LSB}_{12}$, $1.08\text{–}2.15\text{ LSB}_{12,\text{lim}}$).
3. **Important libplacebo constraint:** In mpv/libplacebo user shaders, `//!SAVE` intermediate passes inherit the renderer's FBO format (`//!FORMAT` applies only to `//!TEXTURE`/`//!STORAGE` declarations, not `//!SAVE` passes). VP-V defaults to `fbo_float32` (`rgba32f`, `LibplaceboVideoRenderer.cpp:5485`), whereas stock mpv/libplacebo configurations often default to `rgba16f`. An upstream PR should therefore **document the `rgba32f` FBO requirement explicitly** and never silently claim bit/sub-LSB parity when running on an `rgba16f` FBO (Rule 4).

### 3.4 Where Non-Linear Horizontal/Vertical Warp Curvature Breaks Strict Separability

Strict 2-pass separability is **not** a universal property of arbitrary non-linear warps. Upstream maintainers must enforce the following exact mathematical boundaries:

1. **Coupled 2D warps ($\partial u/\partial y \ne 0$ or $\partial v/\partial x \ne 0$):**
   If horizontal mapping ever depends on $y$ or vertical mapping depends on $x$ — for example, radial/elliptical NLS, barrel/pincushion lens correction, or corner-coupled center protection $p_x(y)$ / $p_y(x)$ — the local Jacobian
   $$J(x, y) = \begin{pmatrix} \partial u/\partial x & \partial u/\partial y \\ \partial v/\partial x & \partial v/\partial y \end{pmatrix}$$
   is **non-diagonal**. The ideal prefilter footprint in source space is sheared/rotated (anisotropic elliptical EWA), not an axis-aligned rectangle. Even a 2-pass scanline-shear decomposition bends straight rows into curved paths in Pass 1, causing Pass 2 to convolve across *mismatched* horizontal displacements $u(x_{\text{out}}, y_j) \ne u(x_{\text{out}}, y_{\text{out}})$, introducing cross-axis curvature/shear error $O(|\partial u/\partial y|\cdot s_y)$ and foldover artifacts if $\partial u/\partial x \le 0$. **Such warps must remain 2D single-pass.**
2. **Non-linear curvature ($u''(x) \ne 0$, $v''(y) \ne 0$) and grid-consistency invariants:**
   Even for the current axis-independent map $(u(x), v(y))$, the warp has nonzero second derivatives $u''(x), v''(y) \ne 0$ (strongest near the `center_protection` quintic Hermite transition in `geometry == 1` and near $|r| \to 1$ at `curve = 2.9` in `geometry == 0`). Across a $2 s_x$-texel support window, the true local Jacobian changes by $\Delta u' \approx |u''(x)| \cdot 2 s_x \cdot \text{pt}_x$.
   - The current `NLSPlus.glsl` already makes a **piecewise-constant local-slope approximation** (`slope` via $\pm 0.5\,\text{pt}$ central difference, lines `196-205`), which is shared identically by both 1-pass and 2-pass forms **if and only if** Pass 1 preserves the exact unwarped source grid along the second axis (same $W \times H$ resolution, no intermediate resize/crop, same `HOOKED_pt` finite-difference `slope`).
   - **Separability breaks** if an upstream refactor tries to fuse Pass 1 with output scaling (non-uniform or resized intermediate spacing on the second axis) or adapts one axis's kernel width as a function of the other axis's local curvature/slope (e.g., isotropic radial cutoff $s = \max(s_x(x), s_y(y))$ or $s = \sqrt{s_x(x)^2 + s_y(y)^2}$, which couples $w_x$ to $y$ and $w_y$ to $x$).

---

## 4. Concrete Upstream Parity Verification Method

Before merging a 2-pass separable refactor upstream into `shaders/NLSPlus.glsl`, the upstream author should validate parity on synthetic physics patterns first (Rule 3) in both deterministic WARP software rendering and real GPU hardware (`rgba32f` FBO), comparing the proposed 2-pass shader against the unmodified 1-pass `NLSPlus.glsl` as the exact reference baseline on identical `float32` input textures and `rgba32f` output FBOs.

### 4.1 Synthetic Test Patterns (Full `float32` Source Inputs)

1. **2D Radial Zone Plate / Chirp** (sweeping spatial frequency from DC to Nyquist along horizontal, vertical, and $45^\circ$ diagonal axes): stresses both primary-axis center minification and complementary-axis edge minification simultaneously, exposing any aliasing, phase shift, or tap-window mismatch.
2. **High-Contrast Step Edges (Horizontal, Vertical, and $45^\circ$ Diagonal)** spanning $[0, 1]$ and extended-range $[-0.05, 1.05]$: triggers Catmull-Rom's maximal $-0.125$ undershoot and $+1.125$ overshoot in Pass 1, directly verifying signed unclamped intermediate preservation and exposing exponent-step rounding in $[1.0, 1.125]$.
3. **Smooth 10-bit & 12-bit Linear Ramps** across $[0, 1]$ (with dense coverage of $[0.5, 1.0)$ where fp16 ULP is maximal): exposes sub-LSB quantization staircasing and rounding bias.
4. **1-Pixel Impulse, Border-Touch Frame, and White-Noise Grain Field:** verifies exact interpolation at integer centers, boundary clamping parity at $x \in \{0, W-1\}$ and $y \in \{0, H-1\}$, and Catmull-Rom white-grain RMS retention ($\ge 0.8\times$ per `shaders/NLSPlus.glsl:87`).

### 4.2 Parameter Matrix

Cover all shader branches (`shaders/NLSPlus.glsl:38-69, 151-206`):
- `geometry` $\in \{0, 1\}$ (`curve` $\in \{2.0, 2.9\}$, `horizontal_center_protection = 0.35`, `vertical_center_protection = 0.25`).
- `stretch_ratio` $\in \{1.0, 1.32, 1.5\}$, `warp_axis` $\in \{0.0, 1.0\}$.
- `axis_balance` $\in \{0.0, 0.25, 0.5\}$, `strength` $\in \{0.0, 0.8, 1.0\}$, `max_center_zoom` $\in \{1.08, 1.25\}$ (including cases where the complementary axis reaches `slope > 1.0` near the edges).
- `sampling` $\in \{1.0, 0.0\}$ (confirming `sampling = 0.0` remains byte-identical to bilinear).

### 4.3 Mandatory Negative Controls

To prove the verification harness catches real defects rather than passing vacuously:
1. **FP16 intermediate control:** Force the `//!SAVE` intermediate FBO to `rgba16f` on the step + ramp patterns and verify max absolute error rises into the predicted $[2.44 \times 10^{-4},\, 6.12 \times 10^{-4}]$ band (up to $0.31\text{ LSB}_{10}$ / $1.26\text{ LSB}_{12}$ on $[0.5, 1.0)$ ramps and exceeding $0.5\text{ LSB}_{10}$ on bright-edge ringing up to $0.63\text{ LSB}_{10}$ / $2.51\text{ LSB}_{12}$, failing sub-LSB 12-bit parity across both $[0.5, 1.0)$ and $[1.0, 1.125]$ and failing the strict `rgba32f` gate).
2. **Clamped intermediate control:** Clamp Pass 1 output to $[0, 1]$ on the step pattern and verify error spikes to $\approx 0.125$ ($\approx 128\text{ LSB}_{10}$), failing the gate.
3. **Bilinear baseline control:** Run `sampling = 0.0` on the white-noise grain field and verify grain RMS drops to $\approx 0.5\times$ at half-pixel offsets, confirming the harness distinguishes Catmull-Rom ($\ge 0.8\times$) from bilinear.

### 4.4 Execution Targets & Derived Pass Criteria

- **Execution environments:**
  1. **WARP software rasterizer (`rgba32f` FBO)** — deterministic CI regression gate (mirroring VP's `TargetLutGpuFixture` / `BundledNlsHooksLogCleanly` pattern in `WholeHookChainHookTests.cpp:667-690`), asserting zero libplacebo warnings/errors.
  2. **Hardware GPU (`rgba32f` FBO, 3840×2160)** — parity check plus GPU timestamp benchmark verifying a net frame-time reduction after accounting for the added full-resolution `rgba32f` intermediate write + read bandwidth (checking both narrow $4 \times 4$ and wide $6 \times 6\text{–}9 \times 9$ regimes).
- **Derived parity pass criteria (on `rgba32f`, from §3.3):**
  - Max absolute error across all pixels and RGB channels: $|\Delta|_\infty \le 1.94 \times 10^{-6}$ ($\le 0.0020\text{ LSB}_{10}$, $\le 0.0079\text{ LSB}_{12}$).
  - Channel RMSE: $\le 4.2 \times 10^{-7}$ (quadrature of fp32 reassociation RMS $\sqrt{20}\,\Lambda_1^2 u_{32} \approx 4.17 \times 10^{-7}$ and `rgba32f` storage RMS $\le 3.44 \times 10^{-8}$).
  - Quantized 10-bit and 12-bit code parity: **0 code differences** on all synthetic pixels outside a $1.94 \times 10^{-6}$ neighborhood of a half-integer quantization boundary, and at most $1$ code on boundary ties.

---

## 5. Non-Goals

- **Zero changes inside VP's tree:** No edits to `shaders/NLSPlus.glsl`, `shaders/NLS.glsl`, `src/VP-V/`, `src/VideoProcessor-Lib/`, or any test file. This document is for **upstream submission only**.
- **No change to the NLS+ coordinate mapping:** `nls_plus_map_radius()`, `nls_plus_map_coordinate()`, parameter semantics, and `NlsMaskMapping.h` remain untouched.
- **No splitting of single-axis `shaders/NLS.glsl`:** Already $O(R)$ in a single pass via `bvec2(!vertical_warp, vertical_warp)`.
- **No modification to Bill's bilinear path (`sampling = 0`)** and no policy decision on `DIV-D-03`'s default `nls_sampling` value.
- **No support for non-separable 2D warps** (radial/barrel/corner-coupled geometries), which are mathematically outside the separable tensor-product class.
