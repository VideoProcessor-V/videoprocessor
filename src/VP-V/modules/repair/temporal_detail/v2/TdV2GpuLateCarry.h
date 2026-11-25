// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Owner Decision D5 (2026-11-25):
// GPU-native Late-Frame Carry Controller (TdV2GpuLateCarryController) mirroring TdV2LateCarryController
// strictly on GPU `pl_tex` (ZERO full-frame CPU readbacks):
//   - Linear decay over kMaxLateCarryFrames (2) steps: miss 1 -> 2/3, miss 2 -> 1/3, miss 3+ -> expired (0 delta)
//   - GPU bicubic warp of previous delta & clean reference along backward carry motion + residual consistency check
//     + TD-U3 statistical bound |delta| <= kappa * sqrt(stepTotalVar)
//   - Explicit late-arrival guard: any stale result arriving with resultSeq <= lastPresentedSeq is strictly discarded,
//     counted (`lateArrivalsDiscarded`), and exposed visibly (NEVER overwrites a newer presented frame!)
//   - Visible FallbackCatalog::Decision + StandInRegistry telemetry (Rule 4: zero silent fallbacks).
//
// Header-only, C++14/17, no Windows headers.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/VpvGpuPass.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2BlendMath.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Gpu.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Scheduler.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2Shaders.h>
#include <VP-V/modules/repair/temporal_detail/v2/TdV2StandIns.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TdV2
{
namespace Gpu
{
	namespace LateCarryShaders
	{
		// Copy on-time delta & clean GPU textures into persistent carry snapshots and initialize valid mask = 1.0.
		constexpr const char* kPassLateCarrySnapshot = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;
float d = texelFetch(src_delta, p, 0).x;
float c = texelFetch(src_clean, p, 0).x;
bool bad = td_is_bad(d) || td_is_bad(c);
imageStore(dst_delta, p, vec4(bad ? 0.0 : d, 0.0, 0.0, 0.0));
imageStore(dst_clean, p, vec4(bad ? 0.0 : c, 0.0, 0.0, 0.0));
imageStore(dst_valid, p, vec4(bad ? 0.0 : 1.0, 0.0, 0.0, 0.0));
)GLSL";

		// Zero output delta texture cleanly on expired late frames (passthrough).
		constexpr const char* kPassLateCarryZero = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;
imageStore(out_delta, p, vec4(0.0));
)GLSL";

		// GPU warp + linear decay + residual consistency check + TD-U3 statistical bound in ONE compute dispatch.
		constexpr const char* kPassLateCarryWarpDecay = R"GLSL(
ivec2 p = ivec2(gl_GlobalInvocationID.xy);
if (p.x >= TD_W || p.y >= TD_H)
    return;

int ax0 = int(round(u_ax0));
int ay0 = int(round(u_ay0));
int ax1 = int(round(u_ax1));
int ay1 = int(round(u_ay1));
if (ax1 <= ax0 || ay1 <= ay0 || p.x < ax0 || p.x >= ax1 || p.y < ay0 || p.y >= ay1)
{
    imageStore(out_delta, p, vec4(0.0));
    imageStore(next_delta, p, vec4(0.0));
    imageStore(next_clean, p, vec4(texelFetch(cur_clean, p, 0).x, 0.0, 0.0, 0.0));
    imageStore(next_valid, p, vec4(0.0));
    return;
}

int cx = min(TD_CW - 1, p.x / TD_CELL_PX);
int cy = min(TD_CH - 1, p.y / TD_CELL_PX);
vec2 mv = texelFetch(motion_vec, ivec2(cx, cy), 0).xy;
vec2 mm = texelFetch(motion_meta, ivec2(cx, cy), 0).xy;
int mCls = int(round(mm.x));
float curC = texelFetch(cur_clean, p, 0).x;

// UPG-HIGH-3: Strictly zero delta on Overlay (3), DynamicTexture (2), invalid motion, or non-finite values!
if (mm.y < 0.5 || mCls == 2 || mCls == 3 || td_is_bad(mv.x) || td_is_bad(mv.y) || td_is_bad(curC))
{
    imageStore(out_delta, p, vec4(0.0));
    imageStore(next_delta, p, vec4(0.0));
    imageStore(next_clean, p, vec4(td_is_bad(curC) ? 0.0 : curC, 0.0, 0.0, 0.0));
    imageStore(next_valid, p, vec4(0.0));
    return;
}

vec2 sLocal = vec2(float(p.x - ax0), float(p.y - ay0)) + mv;
ivec2 sz = ivec2(ax1 - ax0, ay1 - ay0);
bool inBounds = (sLocal.x >= 0.0 && sLocal.y >= 0.0 && sLocal.x <= float(sz.x - 1) && sLocal.y <= float(sz.y - 1));
if (!inBounds)
{
    imageStore(out_delta, p, vec4(0.0));
    imageStore(next_delta, p, vec4(0.0));
    imageStore(next_clean, p, vec4(curC, 0.0, 0.0, 0.0));
    imageStore(next_valid, p, vec4(0.0));
    return;
}

// Bilinear sample of previous delta, clean reference, and propagated validity mask strictly inside active rect.
vec2 cl = clamp(sLocal, vec2(0.0), vec2(float(sz.x - 1), float(sz.y - 1)));
ivec2 i0 = ivec2(floor(cl));
ivec2 i1 = min(sz - ivec2(1), i0 + ivec2(1));
vec2 f = cl - vec2(i0);
ivec2 p00 = ivec2(ax0 + i0.x, ay0 + i0.y);
ivec2 p10 = ivec2(ax0 + i1.x, ay0 + i0.y);
ivec2 p01 = ivec2(ax0 + i0.x, ay0 + i1.y);
ivec2 p11 = ivec2(ax0 + i1.x, ay0 + i1.y);

float vMin = min(min(texelFetch(prev_valid, p00, 0).x, texelFetch(prev_valid, p10, 0).x),
                 min(texelFetch(prev_valid, p01, 0).x, texelFetch(prev_valid, p11, 0).x));
if (vMin < 0.5)
{
    imageStore(out_delta, p, vec4(0.0));
    imageStore(next_delta, p, vec4(0.0));
    imageStore(next_clean, p, vec4(curC, 0.0, 0.0, 0.0));
    imageStore(next_valid, p, vec4(0.0));
    return;
}

float d00 = texelFetch(prev_delta, p00, 0).x;
float d10 = texelFetch(prev_delta, p10, 0).x;
float d01 = texelFetch(prev_delta, p01, 0).x;
float d11 = texelFetch(prev_delta, p11, 0).x;
float wDelta = mix(mix(d00, d10, f.x), mix(d01, d11, f.x), f.y);

float c00 = texelFetch(prev_clean, p00, 0).x;
float c10 = texelFetch(prev_clean, p10, 0).x;
float c01 = texelFetch(prev_clean, p01, 0).x;
float c11 = texelFetch(prev_clean, p11, 0).x;
float wClean = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);

float bound = u_pixel_bound_kappa * sqrt(max(0.0, u_step_total_var));
float outD = 0.0;
float nextBaseD = 0.0;
float outV = 0.0;
if (!td_is_bad(wDelta) && !td_is_bad(wClean))
{
    // UPG-HIGH-3: Match CPU TdV2LateCarryController 1:1:
    //   1) Store UN-DECAYED warped base delta in next_delta so hop 1 applies 2/3 and hop 2 applies 1/3 (NEVER 2/9!).
    //   2) Opposite-sign rejection against desiredDir = wClean - curC.
    //   3) Convex-hull clamp |scaled| <= |desiredDir|.
    //   4) 3-sigma Huber pixel taper psi(r^2 / (9 * stepTotalVar)) + statistical kappa * sqrt(stepTotalVar) cap.
    nextBaseD = wDelta;
    float desiredDir = wClean - curC;
    float scaled = wDelta * u_decay_scale;
    if (scaled * desiredDir >= 0.0)
    {
        if (abs(scaled) > abs(desiredDir))
            scaled = desiredDir;
        if (u_step_total_var > 0.0)
        {
            float r2 = desiredDir * desiredDir;
            float knee = 9.0 * u_step_total_var;
            if (r2 > knee && r2 > TD_NUM_FLOOR)
                scaled *= sqrt(knee / r2);
        }
        outD = clamp(scaled, -bound, bound);
        outV = 1.0;
    }
}

imageStore(out_delta, p, vec4(outD, 0.0, 0.0, 0.0));
imageStore(next_delta, p, vec4(nextBaseD, 0.0, 0.0, 0.0));
imageStore(next_clean, p, vec4(td_is_bad(wClean) ? curC : wClean, 0.0, 0.0, 0.0));
imageStore(next_valid, p, vec4(outV, 0.0, 0.0, 0.0));
)GLSL";
	} // namespace LateCarryShaders

	struct GpuLateCarryInput
	{
		uint64_t seq = 0;
		pl_tex curCleanTex = nullptr;       // w x h R32F (sampleable)
		ActiveRect active;
		pl_tex motionVecTex = nullptr;      // Optional cw x ch RGBA32F (vx, vy, ...)
		pl_tex motionMetaTex = nullptr;     // Optional cw x ch RG32F/RGBA32F (cls, valid, ...)
		const CellMotionField* cpuMotion = nullptr; // Fallback if motionVecTex/motionMetaTex are null
		double stepTotalVar = 0.0;
		double pixelBoundKappa = kPixelBoundKappaDefault;
		int cellPx = kCanonicalBlockPx;
	};

	struct GpuLateCarryDecision
	{
		bool carried = false;
		bool expired = false;
		bool lateArrivalDiscarded = false;
		int lateStreak = 0;
		double decayScale = 0.0;
		std::string statusText;
		FallbackCatalog::Decision fallback{};
	};

	class TdV2GpuLateCarryController
	{
	public:
		TdV2GpuLateCarryController() = default;
		~TdV2GpuLateCarryController()
		{
			if (ownerGpu_ != nullptr)
				Reset(ownerGpu_);
		}
		TdV2GpuLateCarryController(const TdV2GpuLateCarryController&) = delete;
		TdV2GpuLateCarryController& operator=(const TdV2GpuLateCarryController&) = delete;

		// Exact VRAM budgeting for persistent ping-pong carry buffers (UPG-HIGH-3: zero unbudgeted VRAM!).
		static uint64_t EstimatedPersistentVramBytes(int w, int h, int cellPx = kCanonicalBlockPx)
		{
			if (w <= 0 || h <= 0)
				return 0;
			const int effCellPx = cellPx > 0 ? cellPx : kCanonicalBlockPx;
			const uint64_t px = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
			const uint64_t cw = static_cast<uint64_t>((w + effCellPx - 1) / effCellPx);
			const uint64_t ch = static_cast<uint64_t>((h + effCellPx - 1) / effCellPx);
			const uint64_t bR32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::R32F);
			const uint64_t bRGBA32 = VpSkeleton::BytesPerTexel(VpSkeleton::TexFormat::RGBA32F);
			// 2 ping-pong slots x (deltaTex_ + cleanTex_ + validTex_) R32F + motVecTex_ + motMetaTex_ RGBA32F.
			return 6u * px * bR32 + 2u * (cw * ch) * bRGBA32;
		}

		uint64_t AllocatedPersistentVramBytes() const
		{
			uint64_t total = 0;
			for (pl_tex t : {
				deltaTex_[0], deltaTex_[1], cleanTex_[0], cleanTex_[1],
				validTex_[0], validTex_[1], motVecTex_, motMetaTex_ })
			{
				total += VpSkeleton::TexBytesOf(t);
			}
			return total;
		}

		static VpSkeleton::VramUser MakeVramUser(int w, int h, int cellPx = kCanonicalBlockPx)
		{
			VpSkeleton::VramUser u;
			u.id = VpSkeleton::kUserTemporalDetail;
			u.name = VpSkeleton::VramUserName(VpSkeleton::kUserTemporalDetail);
			u.tier = VpSkeleton::Tier::T1;
			u.shedOrder = 0;
			u.lossless = false;
			const uint64_t bytes = EstimatedPersistentVramBytes(w, h, cellPx);
			u.levels.push_back(bytes);
			u.levelText.push_back(nullptr);
			if (bytes > 0)
			{
				u.levels.push_back(0);
				u.levelText.push_back("late carry off (GPU memory shed)");
			}
			return u;
		}

		void Reset(pl_gpu gpu = nullptr)
		{
			hasPrev_ = false;
			prevW_ = 0;
			prevH_ = 0;
			lateStreak_ = 0;
			expired_ = false;
			curSlot_ = 0;
			hasPresentedSeq_ = false;
			lastPresentedSeq_ = 0;
			pl_gpu effGpu = (gpu != nullptr) ? gpu : ownerGpu_;
			if (effGpu != nullptr)
			{
				for (pl_tex* t : {
					&deltaTex_[0], &deltaTex_[1], &cleanTex_[0], &cleanTex_[1],
					&validTex_[0], &validTex_[1], &motVecTex_, &motMetaTex_ })
				{
					if (*t != nullptr)
						pl_tex_destroy(effGpu, t);
				}
			}
			else
			{
				for (int i = 0; i < 2; ++i)
				{
					deltaTex_[i] = nullptr;
					cleanTex_[i] = nullptr;
					validTex_[i] = nullptr;
				}
				motVecTex_ = nullptr;
				motMetaTex_ = nullptr;
			}
			ownerGpu_ = nullptr;
		}

		void OnCutOrReset(pl_gpu gpu = nullptr, StandInRegistry* standIns = nullptr)
		{
			Reset(gpu);
			if (standIns != nullptr)
			{
				standIns->Clear(StandInKind::LateCarryActive);
				standIns->Clear(StandInKind::LateCarryExpired);
			}
		}

		// Explicit late-arrival guard (Owner D5):
		// Discards any stale GPU result arriving for a frame at or behind `lastPresentedSeq` so it can NEVER
		// overwrite a newer presented frame. Returns true if result is fresh and accepted, false if discarded.
		bool OnLateResultArrived(uint64_t resultSeq, uint64_t lastPresentedSeq, StandInRegistry* standIns = nullptr)
		{
			if (hasPresentedSeq_ && lastPresentedSeq > lastPresentedSeq_)
				lastPresentedSeq_ = lastPresentedSeq;
			const uint64_t watermark = hasPresentedSeq_ ? (std::max)(lastPresentedSeq_, lastPresentedSeq) : lastPresentedSeq;
			if (resultSeq <= watermark)
			{
				++lateArrivalsDiscarded_;
				lastDiscardStatus_ = "late arrival discarded (stale seq " + std::to_string(resultSeq) +
					" <= presented " + std::to_string(watermark) + ")";
				if (standIns != nullptr)
					standIns->Engage(StandInKind::LateCarryActive, lastDiscardStatus_.c_str());
				return false;
			}
			return true;
		}

		bool OnOnTimeFrameGpu(
			pl_gpu gpu,
			pl_dispatch dispatch,
			uint64_t seq,
			pl_tex curDeltaTex,
			pl_tex curCleanTex,
			StandInRegistry* standIns = nullptr)
		{
			if (!gpu || !dispatch || !curDeltaTex || !curCleanTex)
				return false;
			const int w = curCleanTex->params.w;
			const int h = curCleanTex->params.h;
			if (w <= 0 || h <= 0 || curDeltaTex->params.w != w || curDeltaTex->params.h != h)
				return false;

			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			if (!fmtR32)
				return false;
			if (!Detail::EnsureTex(gpu, deltaTex_[0], w, h, fmtR32, true, true, false, false) ||
			    !Detail::EnsureTex(gpu, cleanTex_[0], w, h, fmtR32, true, true, false, false) ||
			    !Detail::EnsureTex(gpu, validTex_[0], w, h, fmtR32, true, true, false, false))
			{
				return false;
			}
			ownerGpu_ = gpu;

			const int cw = (w + kCanonicalBlockPx - 1) / kCanonicalBlockPx;
			const int ch = (h + kCanonicalBlockPx - 1) / kCanonicalBlockPx;
			const std::string hdr = Detail::StaticGeomDefines(w, h, cw, ch, kCanonicalBlockPx) + Shaders::kCoreHelpers;
			pl_shader_desc d[5] = {
				VpvGpu::Sampled("src_delta", curDeltaTex),
				VpvGpu::Sampled("src_clean", curCleanTex),
				VpvGpu::Storage("dst_delta", deltaTex_[0]),
				VpvGpu::Storage("dst_clean", cleanTex_[0]),
				VpvGpu::Storage("dst_valid", validTex_[0])
			};
			if (!VpvGpu::Pass(dispatch, "td_v2_lc_snapshot", hdr, LateCarryShaders::kPassLateCarrySnapshot,
			                  d, 5, nullptr, 0, Detail::GroupsFor(w, 16), Detail::GroupsFor(h, 16), 0, 16, 16))
			{
				return false;
			}

			curSlot_ = 0;
			prevW_ = w;
			prevH_ = h;
			hasPrev_ = true;
			lateStreak_ = 0;
			expired_ = false;
			hasPresentedSeq_ = true;
			lastPresentedSeq_ = seq;
			if (standIns != nullptr)
			{
				standIns->Clear(StandInKind::LateCarryActive);
				standIns->Clear(StandInKind::LateCarryExpired);
			}
			return true;
		}

		GpuLateCarryDecision OnLateFrameGpu(
			pl_gpu gpu,
			pl_dispatch dispatch,
			const GpuLateCarryInput& in,
			pl_tex outDeltaTex,
			StandInRegistry* standIns = nullptr)
		{
			GpuLateCarryDecision dec;
			if (in.seq > lastPresentedSeq_ || !hasPresentedSeq_)
			{
				hasPresentedSeq_ = true;
				lastPresentedSeq_ = in.seq;
			}

			auto writeZeroOut = [&]()
			{
				if (!gpu || !dispatch || !outDeltaTex)
					return;
				const int w = outDeltaTex->params.w;
				const int h = outDeltaTex->params.h;
				if (w <= 0 || h <= 0)
					return;
				const int cw = (w + kCanonicalBlockPx - 1) / kCanonicalBlockPx;
				const int ch = (h + kCanonicalBlockPx - 1) / kCanonicalBlockPx;
				pl_shader_desc dz[1] = { VpvGpu::Storage("out_delta", outDeltaTex) };
				VpvGpu::Pass(dispatch, "td_v2_lc_zero", Detail::StaticGeomDefines(w, h, cw, ch, kCanonicalBlockPx),
				             LateCarryShaders::kPassLateCarryZero, dz, 1, nullptr, 0,
				             Detail::GroupsFor(w, 16), Detail::GroupsFor(h, 16), 0, 16, 16);
			};

			auto makeExpired = [&](const char* reason) -> GpuLateCarryDecision
			{
				expired_ = true;
				++lateExpiredFrames_;
				writeZeroOut();
				dec.carried = false;
				dec.expired = true;
				dec.lateStreak = lateStreak_;
				dec.decayScale = 0.0;
				dec.statusText = MetaOf(StandInKind::LateCarryExpired).statusText;
				dec.fallback = FallbackCatalog::Engaged("product.temporal_detail", dec.statusText);
				if (standIns != nullptr)
				{
					standIns->Clear(StandInKind::LateCarryActive);
					standIns->Engage(StandInKind::LateCarryExpired, reason);
				}
				return dec;
			};

			if (!gpu || !dispatch || !in.curCleanTex || !outDeltaTex ||
			    !std::isfinite(in.stepTotalVar) || in.stepTotalVar < 0.0)
			{
				if (standIns != nullptr)
					standIns->Engage(StandInKind::NonFiniteClosure, "TdV2GpuLateCarryController invalid args or stepTotalVar");
				++lateStreak_;
				return makeExpired("invalid args or stepTotalVar");
			}

			const int w = in.curCleanTex->params.w;
			const int h = in.curCleanTex->params.h;
			const int cellPx = in.cellPx > 0 ? in.cellPx : kCanonicalBlockPx;
			const int cw = (w + cellPx - 1) / cellPx;
			const int ch = (h + cellPx - 1) / cellPx;

			if (expired_)
			{
				++lateStreak_;
				return makeExpired("late carry latched expired (awaiting on-time frame)");
			}
			if (!hasPrev_ || prevW_ != w || prevH_ != h || outDeltaTex->params.w != w || outDeltaTex->params.h != h)
			{
				++lateStreak_;
				return makeExpired(!hasPrev_ ? "no stored delta (cut/reset)" : "geometry mismatch vs stored delta");
			}
			if (lateStreak_ >= kMaxLateCarryFrames)
			{
				++lateStreak_;
				return makeExpired("2-frame late carry cap reached");
			}

			const ActiveRect ar = in.active.Empty() ? ActiveRect::Full(w, h) : in.active.ClampedTo(w, h);
			if (ar.Empty())
			{
				++lateStreak_;
				return makeExpired("empty active rect");
			}

			const pl_fmt fmtR32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::R32F);
			const pl_fmt fmtRGBA32 = VpSkeleton::PlTexBackend::FindFormat(gpu, VpSkeleton::TexFormat::RGBA32F);
			if (!fmtR32 || !fmtRGBA32)
			{
				++lateStreak_;
				return makeExpired("FP32 formats unsupported");
			}

			pl_tex motVec = in.motionVecTex;
			pl_tex motMeta = in.motionMetaTex;
			if (!motVec || !motMeta)
			{
				if (in.cpuMotion == nullptr || !in.cpuMotion->IsWellFormed(cw, ch))
				{
					if (standIns != nullptr)
						standIns->Engage(StandInKind::MotionFieldStandIn, "TdV2GpuLateCarryController null/invalid carry motion");
					++lateStreak_;
					return makeExpired("carry motion unavailable");
				}
				if (!Detail::EnsureTex(gpu, motVecTex_, cw, ch, fmtRGBA32, true, false, true, false) ||
				    !Detail::EnsureTex(gpu, motMetaTex_, cw, ch, fmtRGBA32, true, false, true, false))
				{
					++lateStreak_;
					return makeExpired("motion upload alloc failed");
				}
				const size_t n = static_cast<size_t>(cw) * static_cast<size_t>(ch);
				std::vector<float> mvBuf(n * 4u, 0.0f);
				std::vector<float> mmBuf(n * 4u, 0.0f);
				for (size_t i = 0; i < n; ++i)
				{
					mvBuf[i * 4u + 0u] = static_cast<float>(in.cpuMotion->vx[i]);
					mvBuf[i * 4u + 1u] = static_cast<float>(in.cpuMotion->vy[i]);
					mmBuf[i * 4u + 0u] = static_cast<float>(static_cast<int>(in.cpuMotion->cls[i]));
					mmBuf[i * 4u + 1u] = in.cpuMotion->valid[i] ? 1.0f : 0.0f;
				}
				if (!Detail::UploadFloats(gpu, motVecTex_, mvBuf.data()) ||
				    !Detail::UploadFloats(gpu, motMetaTex_, mmBuf.data()))
				{
					++lateStreak_;
					return makeExpired("motion upload failed");
				}
				motVec = motVecTex_;
				motMeta = motMetaTex_;
			}

			const int nextSlot = 1 - curSlot_;
			if (!Detail::EnsureTex(gpu, deltaTex_[nextSlot], w, h, fmtR32, true, true, false, false) ||
			    !Detail::EnsureTex(gpu, cleanTex_[nextSlot], w, h, fmtR32, true, true, false, false) ||
			    !Detail::EnsureTex(gpu, validTex_[nextSlot], w, h, fmtR32, true, true, false, false))
			{
				++lateStreak_;
				return makeExpired("ping-pong carry alloc failed");
			}

			++lateStreak_;
			const double decay = 1.0 - static_cast<double>(lateStreak_) / static_cast<double>(kMaxLateCarryFrames + 1);
			const float uAx0 = static_cast<float>(ar.x0);
			const float uAy0 = static_cast<float>(ar.y0);
			const float uAx1 = static_cast<float>(ar.x1);
			const float uAy1 = static_cast<float>(ar.y1);
			const float uDecay = static_cast<float>(decay);
			const float uStepVar = static_cast<float>(in.stepTotalVar);
			const float uKappa = static_cast<float>(ClampPixelBoundKappa(in.pixelBoundKappa));

			const std::string hdr = Detail::StaticGeomDefines(w, h, cw, ch, cellPx) + Shaders::kCoreHelpers;
			pl_shader_desc d[10] = {
				VpvGpu::Sampled("cur_clean", in.curCleanTex),
				VpvGpu::Sampled("prev_delta", deltaTex_[curSlot_]),
				VpvGpu::Sampled("prev_clean", cleanTex_[curSlot_]),
				VpvGpu::Sampled("prev_valid", validTex_[curSlot_]),
				VpvGpu::Sampled("motion_vec", motVec),
				VpvGpu::Sampled("motion_meta", motMeta),
				VpvGpu::Storage("out_delta", outDeltaTex),
				VpvGpu::Storage("next_delta", deltaTex_[nextSlot]),
				VpvGpu::Storage("next_clean", cleanTex_[nextSlot]),
				VpvGpu::Storage("next_valid", validTex_[nextSlot])
			};
			pl_shader_var v[7] = {
				VpvGpu::Var("u_ax0", &uAx0),
				VpvGpu::Var("u_ay0", &uAy0),
				VpvGpu::Var("u_ax1", &uAx1),
				VpvGpu::Var("u_ay1", &uAy1),
				VpvGpu::Var("u_decay_scale", &uDecay),
				VpvGpu::Var("u_step_total_var", &uStepVar),
				VpvGpu::Var("u_pixel_bound_kappa", &uKappa)
			};
			if (!VpvGpu::Pass(dispatch, "td_v2_lc_warp_decay", hdr, LateCarryShaders::kPassLateCarryWarpDecay,
			                  d, 10, v, 7, Detail::GroupsFor(w, 16), Detail::GroupsFor(h, 16), 0, 16, 16))
			{
				return makeExpired("td_v2_lc_warp_decay dispatch failed");
			}

			curSlot_ = nextSlot;
			++lateCarriedFrames_;
			dec.carried = true;
			dec.expired = false;
			dec.lateStreak = lateStreak_;
			dec.decayScale = decay;
			dec.statusText = MetaOf(StandInKind::LateCarryActive).statusText;
			dec.fallback = FallbackCatalog::Engaged("product.temporal_detail", dec.statusText);
			if (standIns != nullptr)
			{
				standIns->Clear(StandInKind::LateCarryExpired);
				standIns->Engage(StandInKind::LateCarryActive);
			}
			return dec;
		}

		int LateStreak() const { return lateStreak_; }
		bool HasStoredDelta() const { return hasPrev_; }
		bool IsExpiredLatched() const { return expired_; }
		uint64_t LastPresentedSeq() const { return lastPresentedSeq_; }
		uint64_t LateCarriedFrames() const { return lateCarriedFrames_; }
		uint64_t LateExpiredFrames() const { return lateExpiredFrames_; }
		uint64_t LateArrivalsDiscarded() const { return lateArrivalsDiscarded_; }
		const std::string& LastDiscardStatus() const { return lastDiscardStatus_; }

	private:
		bool hasPrev_ = false;
		int prevW_ = 0;
		int prevH_ = 0;
		int lateStreak_ = 0;
		bool expired_ = false;
		int curSlot_ = 0;
		bool hasPresentedSeq_ = false;
		uint64_t lastPresentedSeq_ = 0;
		uint64_t lateCarriedFrames_ = 0;
		uint64_t lateExpiredFrames_ = 0;
		uint64_t lateArrivalsDiscarded_ = 0;
		std::string lastDiscardStatus_;

		pl_gpu ownerGpu_ = nullptr;
		pl_tex deltaTex_[2] = {};
		pl_tex cleanTex_[2] = {};
		pl_tex validTex_[2] = {};
		pl_tex motVecTex_ = nullptr;
		pl_tex motMetaTex_ = nullptr;
	};

} // namespace Gpu
} // namespace TdV2
