// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K5-1 (C1, F6): camera-compensated temporal smoothing of the per-block grain statistic. ONE unit: the CPU reference below and
// the GLSL text of the same rule (GlslText), so the two cannot drift apart.
//
// K5-0 inventory (2026-11-07, branch r1cam/units, base fork/integ/batch1):
//   * Today's smoothing site is the grain-map pass of the adaptive scaling stage: scaling/output/AdaptiveDownscale.cpp, MAP_BODY
//     (the 8-3 per-block map, one dispatch per SOURCE frame, 16x16 blocks, ping-pong textures map_in / map_out; host side
//     AdaptiveDownscale::Update "grain map"; alpha = 1 - exp(-frameSeconds*1000/grainMapSmoothMs)). Per block:
//       trusted  (frame trust ok and field.z > 0.5): previous value fetched NEAREST at floor((c - f.xy)/16), clamped to the grid,
//                  out = reset || !prev.valid ? target : mix(prev, target, alpha), valid = 1;
//       untrusted: previous value fetched at the SAME block (c), out = reset ? 1 : prev, valid = 0 (no new measurement).
//     That untrusted carry-forward at the same cell is what mixes regions on a pan.
//   * The grain split unit's version is GrainSplit::kUnitVersion (GrainSplit.h, currently 2; the catalog expects it as
//     kExpectedGrainSplitUnit). This unit does not change the split law, so that version is NOT bumped here; the refinement is
//     a separate VARIANT stamp (kRefineUnitVersion, VariantStamp) so a reader can tell which variant produced a map.
//   * Gap order: the P10 lane runs in the gap before the seal; P8 comes from the camera lane (K2). Wiring is K5-2 (waits for
//     P4/SA2); this header has no call site.
//
// What it changes: WHERE the previous statistic is fetched, for camera blocks the P8 accessor lets through (policy enabled, weight
// above 0, i.e. confidence above the policy floor): at the camera model position (accessor blend of today's vector and the model
// vector) instead of the measured vector (trusted blocks) or the same cell (untrusted blocks, which now carry their statistic
// along with the camera). Object blocks, Unknown/Overlay blocks, blocks below the trust floor and every block when P8 is absent or
// late keep today's rule exactly. It touches smoothing parameters only: no grain layer, no split input, no re-add; the grain
// statistics of the picture are preserved by construction. A measured change means the split leaks: fix the split, not the gate.
//
// Absent / late P8: the whole frame takes today's rule (the UNCOMPENSATED variant) and the result says so (Variant, cameraLate)
// so the host can show "grain: camera late". Never waits.
//
// Pure, deterministic, C++14, header-only, no Windows headers. The CPU rule uses CameraModel::FieldVector, whose GLSL port
// (camera_field_vector) the shader text calls, so the position logic has one mirror.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessorGlsl.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace GrainCameraRefine
{
	constexpr uint32_t kRefineUnitVersion = 1;
	// The split unit's version this variant was built against (the law it leaves untouched).
	constexpr uint32_t kGrainSplitUnitVersion = GrainSplit::kUnitVersion;

	enum class Variant : uint8_t { Uncompensated = 0, Camera = 1 };

	// Variant stamp: split version in the high half, refine version and flag below. Distinct for every (version, variant).
	inline uint32_t VariantStamp(Variant v)
	{
		return (kGrainSplitUnitVersion << 16) | (kRefineUnitVersion << 1) | (v == Variant::Camera ? 1u : 0u);
	}

	// The per-block map (value 0..1 as today's map; valid mirrors today's .g: 1 = a measurement went into it).
	struct Grid
	{
		int w = 0, h = 0;
		std::vector<float> value;
		std::vector<uint8_t> valid;
		void Resize(int gw, int gh, float v = 1.0f)
		{
			w = gw;
			h = gh;
			value.assign(static_cast<size_t>(gw) * static_cast<size_t>(gh), v);
			valid.assign(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
		}
		size_t Index(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x); }
	};

	// This frame's per-block inputs (what the map pass has today).
	struct BlockInputs
	{
		std::vector<float> target;          // the ramped instantaneous value (today's `target`)
		std::vector<uint8_t> trusted;       // today's `trusted` (frame trust ok and field.z > 0.5)
		std::vector<float> vx, vy;          // today's vector f.xy (source px): previous position = centre - v
	};

	struct Settings
	{
		double blockSize = 16.0;            // source px per block (today: 16)
		float alpha = 1.0f;                 // today's smooth_alpha (1 = no smoothing)
		bool reset = false;                 // today's cut / first-map flag
	};

	struct FrameResult
	{
		Variant variant = Variant::Uncompensated;
		bool cameraWanted = false;           // the consumer switch (policy.enabled) is on
		bool cameraLate = false;             // wanted, but P8 absent or late for this frame: uncompensated, show "grain: camera late"
		uint32_t cameraBlocks = 0;           // blocks that fetched at the camera position
		uint32_t carriedBlocks = 0;          // of those, untrusted blocks that carried their statistic along
		uint32_t stamp = VariantStamp(Variant::Uncompensated);
	};

	// ---- today's rule (mirror of MAP_BODY) ----------------------------------------------------------------------------------

	inline int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

	// The previous texel for a block: nearest, clamped to the grid (floor of centre - vector over the block size).
	inline size_t PrevIndex(const Grid& g, int bx, int by, double blockSize, double vx, double vy)
	{
		const double cx = (bx + 0.5) * blockSize - vx, cy = (by + 0.5) * blockSize - vy;
		const int px = ClampInt(static_cast<int>(std::floor(cx / blockSize)), 0, g.w - 1);
		const int py = ClampInt(static_cast<int>(std::floor(cy / blockSize)), 0, g.h - 1);
		return g.Index(px, py);
	}

	// One block of today's map: the same arithmetic as MAP_BODY. `prevIndex` is the previous texel (placed by the caller from the
	// vector; an untrusted block reads its own cell under today's rule).
	inline void SmoothBlock(const Grid& prev, size_t prevIndex, bool trusted, float target, float alpha, bool reset,
		float& outValue, uint8_t& outValid)
	{
		const float pv = prev.value[prevIndex];
		const bool pvalid = prev.valid[prevIndex] != 0;
		if (!trusted)
		{
			outValue = reset ? 1.0f : pv;
			outValid = 0;
		}
		else if (reset || !pvalid)
		{
			outValue = target;
			outValid = 1;
		}
		else
		{
			outValue = pv + (target - pv) * alpha;   // mix(prev, target, alpha)
			outValid = 1;
		}
	}

	// ---- the frame -----------------------------------------------------------------------------------------------------------

	// One source frame. `record` null (or p8Available false) = P8 absent or late: the uncompensated variant. `policy` is the
	// consumer's (GrainMap preset + its `enabled`), `state` its BlendState already updated for this frame. `prev` and `out` are
	// distinct grids. `out` always receives a complete map.
	inline FrameResult RefineFrame(const CameraModel::Policy& policy, const CameraModel::BlendState& state,
		const CameraModel::CameraMotion* record, bool p8Available, const Grid& prev, const BlockInputs& in,
		const Settings& s, Grid& out)
	{
		FrameResult r;
		r.cameraWanted = policy.enabled;
		const bool usable = policy.enabled && p8Available && CameraModel::ModelUsable(policy, record);
		r.cameraLate = policy.enabled && !usable;
		out.Resize(prev.w, prev.h);
		const size_t n = static_cast<size_t>(prev.w) * static_cast<size_t>(prev.h);
		const bool shapeOk = in.target.size() >= n && in.trusted.size() >= n && in.vx.size() >= n && in.vy.size() >= n &&
			prev.value.size() >= n && prev.valid.size() >= n;
		if (!shapeOk)
			return r;
		for (int by = 0; by < prev.h; ++by)
		{
			for (int bx = 0; bx < prev.w; ++bx)
			{
				const size_t i = prev.Index(bx, by);
				const bool trusted = in.trusted[i] != 0;
				// Today's vector (an untrusted block reads its own cell: vector 0).
				float tx = trusted ? in.vx[i] : 0.0f, ty = trusted ? in.vy[i] : 0.0f;
				bool camera = false;
				if (usable)
				{
					float ox = tx, oy = ty;
					const CameraModel::Choice c = CameraModel::FieldVector(policy, state, record, bx, by, false, tx, ty, ox, oy);
					if (c == CameraModel::Choice::Model || c == CameraModel::Choice::Blend)
					{
						camera = true;
						tx = ox;
						ty = oy;
					}
				}
				const size_t pi = PrevIndex(prev, bx, by, s.blockSize, tx, ty);
				SmoothBlock(prev, pi, trusted, in.target[i], s.alpha, s.reset, out.value[i], out.valid[i]);
				if (camera)
				{
					++r.cameraBlocks;
					if (!trusted)
						++r.carriedBlocks;
				}
			}
		}
		r.variant = r.cameraBlocks > 0 ? Variant::Camera : Variant::Uncompensated;
		r.stamp = VariantStamp(r.variant);
		return r;
	}

	// ---- the shader text -------------------------------------------------------------------------------------------------------

	// GLSL of the previous-position rule. Emitted ONLY when the consumer switch is on (a consumer that is off gives no define and
	// no text, so its shader and cache key are unchanged); it needs CameraFieldVectorGlsl() and the six accessor uniforms in the
	// same shader (CameraConsumerBinding). `todays` is today's vector (zero for an untrusted block). Returns the position, in the
	// same pixel space as `c`, at which the previous statistic is fetched; `compensated` says the camera rule applied.
	inline std::string GlslText(bool consumerEnabled)
	{
		if (!consumerEnabled)
			return std::string();
		std::string t;
		t += "#define VPU_GRAIN_CAMERA_REFINE_VERSION " + std::to_string(kRefineUnitVersion) + "\n";
		t += R"GLSL(
vec2 gcr_prev_centre(ivec2 b, vec2 c, vec2 todays, out bool compensated)
{
    vec2 o;
    int choice = camera_field_vector(b, false, todays, o);
    compensated = (choice == 1 || choice == 2);
    return c - (compensated ? o : todays);
}
)GLSL";
		return t;
	}
}
