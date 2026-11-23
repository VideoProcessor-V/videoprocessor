// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K5b (C1b, A2): camera-aligned residual history for zooms, rotations and pans.
// ONE unit: the CPU reference below and the GLSL text (GlslText), so the two cannot drift apart.
//
// K5b-0 inventory (base 69c96794a):
//   * No standalone P11 (kP11RegionBlockStats) residual-stabiliser site exists yet at 69c96794a
//     (detail_pulse_stabilise has no vector-warp site; P11 has no residual consumer).
//   * The scaling residual stabiliser (ssim_residual_stabilise / cnn_residual_stabilise) lives in
//     scaling/output/AdaptiveDownscale.cpp, which fetches the previous residual using a single block-constant
//     translation vector (field.xy). On a zoom or rotation, a block-constant vector drifts across the 16x16
//     block; this unit aligns each pixel of a P8 Camera block using the EXACT inverse 3x3 camera model
//     H^{-1}(p_cur) (blended with the measured vector by the accessor's rate-limited weight), while Object,
//     Overlay, Unknown and untrusted blocks keep the measured alignment unchanged.
//   * Wiring into AdaptiveDownscale.cpp / the future P11 stabiliser waits on merging r2/ce-ka's K5-2
//     AdaptiveDownscale.cpp edits (or landing the P11 stabiliser site); see reviews/2026-11-21_coding-batch-2-findings.md.
//
// Default OFF (policy.enabled == false): emits NO GLSL define or text (shader text and cache key unchanged)
// and produces byte-identical output to uncompensated residual stabilisation.
// Absent / late / degenerate P8: Variant::Uncompensated, cameraLate = true, byte-identical to uncompensated
// alignment, visible fallback text "residual stabilise: camera late".
//
// Pure, deterministic, header-only, C++14, no Windows or GPU headers.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ResidualCameraAlign
{
	constexpr uint32_t kUnitVersion = 1;
	constexpr const char* kCameraLateText = "residual stabilise: camera late";

	enum class Variant : uint8_t { Uncompensated = 0, Camera = 1 };

	struct InvHomography
	{
		bool valid = false;
		double h[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // inverse of CameraModel::Params::h (frame N -> frame N-1)
	};

	// Exact 3x3 inverse of a forward camera model (N-1 -> N) to map a pixel in frame N back to frame N-1.
	// Returns valid = false when singular or non-finite.
	inline InvHomography InvertModel(const CameraModel::Params& p)
	{
		InvHomography inv;
		if (p.type == CameraModel::ModelType::None)
			return inv;
		for (int i = 0; i < 9; ++i)
			if (!CameraModel::IsFiniteBits(p.h[i]))
				return inv;
		const double* m = p.h;
		const double c00 = m[4] * m[8] - m[5] * m[7];
		const double c01 = m[5] * m[6] - m[3] * m[8];
		const double c02 = m[3] * m[7] - m[4] * m[6];
		const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
		if (!CameraModel::IsFiniteBits(det) || std::fabs(det) < 1e-12)
			return inv;
		const double id = 1.0 / det;
		inv.h[0] = c00 * id;
		inv.h[1] = (m[2] * m[7] - m[1] * m[8]) * id;
		inv.h[2] = (m[1] * m[5] - m[2] * m[4]) * id;
		inv.h[3] = c01 * id;
		inv.h[4] = (m[0] * m[8] - m[2] * m[6]) * id;
		inv.h[5] = (m[2] * m[3] - m[0] * m[5]) * id;
		inv.h[6] = c02 * id;
		inv.h[7] = (m[1] * m[6] - m[0] * m[7]) * id;
		inv.h[8] = (m[0] * m[4] - m[1] * m[3]) * id;
		for (int i = 0; i < 9; ++i)
			if (!CameraModel::IsFiniteBits(inv.h[i]))
				return InvHomography();
		inv.valid = true;
		return inv;
	}

	// Maps a frame-space pixel centre (px, py) in frame N to its exact source position (prevX, prevY) in frame N-1
	// using the inverse camera homography in picture-centred coordinates. False if at/behind horizon or non-finite.
	inline bool PrevPixelExact(const InvHomography& inv, const CameraModel::Rect& pic, double px, double py,
		double& prevX, double& prevY)
	{
		if (!inv.valid || pic.Empty())
			return false;
		const double cx = pic.CentreX(), cy = pic.CentreY();
		const double x = px - cx, y = py - cy;
		const double w = inv.h[6] * x + inv.h[7] * y + inv.h[8];
		if (!(w > 1e-9) || !CameraModel::IsFiniteBits(w))
			return false;
		const double ox = (inv.h[0] * x + inv.h[1] * y + inv.h[2]) / w;
		const double oy = (inv.h[3] * x + inv.h[4] * y + inv.h[5]) / w;
		if (!CameraModel::IsFiniteBits(ox) || !CameraModel::IsFiniteBits(oy))
			return false;
		prevX = ox + cx;
		prevY = oy + cy;
		return true;
	}

	struct Plane
	{
		int w = 0, h = 0;
		std::vector<float> data;
		void Resize(int pw, int ph, float fill = 0.0f)
		{
			w = pw;
			h = ph;
			data.assign(static_cast<size_t>(pw > 0 ? pw : 0) * static_cast<size_t>(ph > 0 ? ph : 0), fill);
		}
		float At(int x, int y) const
		{
			const int cx = x < 0 ? 0 : (x >= w ? w - 1 : x);
			const int cy = y < 0 ? 0 : (y >= h ? h - 1 : y);
			return data[static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx)];
		}
	};

	struct BlockField
	{
		int gridW = 0, gridH = 0;
		double blockSize = 16.0;
		std::vector<float> vx, vy;       // measured forward vector per block (source px)
		std::vector<uint8_t> trusted;    // 1 = trusted block, 0 = untrusted (falls back to zero measured vector)
	};

	struct Settings
	{
		float alpha = 0.35f;             // one-pole blend towards current residual (0 = hold previous, 1 = current only)
		bool grainGuardNearest = true;   // residual_stabilise_grain_guard: nearest texel (true) vs bilinear (false)
		bool reset = false;              // cut / first frame: pass current residual unchanged
	};

	struct FrameResult
	{
		Variant variant = Variant::Uncompensated;
		bool cameraWanted = false;
		bool cameraLate = false;
		bool badShape = false;
		uint64_t cameraPixels = 0;
		uint64_t measuredPixels = 0;
	};

	namespace detail
	{
		inline float SampleNearest(const Plane& p, double fx, double fy)
		{
			const int ix = static_cast<int>(std::floor(fx));
			const int iy = static_cast<int>(std::floor(fy));
			return p.At(ix, iy);
		}

		inline float SampleBilinear(const Plane& p, double fx, double fy)
		{
			const double sx = fx - 0.5, sy = fy - 0.5;
			const int x0 = static_cast<int>(std::floor(sx));
			const int y0 = static_cast<int>(std::floor(sy));
			const double tx = sx - static_cast<double>(x0);
			const double ty = sy - static_cast<double>(y0);
			const float a = p.At(x0, y0), b = p.At(x0 + 1, y0);
			const float c = p.At(x0, y0 + 1), d = p.At(x0 + 1, y0 + 1);
			const double top = a + (b - a) * tx;
			const double bot = c + (d - c) * tx;
			return static_cast<float>(top + (bot - top) * ty);
		}
	}

	// CPU reference mirror of camera-aligned residual stabilisation.
	// `prev` = previous frame's residual plane, `cur` = this frame's raw residual plane, `out` = stabilised residual.
	// Policy preset: CameraModel::PolicyFor(CameraModel::Consumer::ResidualStabilise) with caller's `enabled`.
	// When policy.enabled is false or P8 is absent / late / degenerate, `out` is byte-identical to uncompensated
	// measured-vector residual alignment.
	inline FrameResult AlignResidualFrame(const CameraModel::Policy& policy, const CameraModel::BlendState& state,
		const CameraModel::CameraMotion* record, bool p8Available, const Plane& prev, const Plane& cur,
		const BlockField& field, const Settings& s, Plane& out)
	{
		FrameResult r;
		r.cameraWanted = policy.enabled;
		if (prev.w <= 0 || prev.h <= 0 || prev.w != cur.w || prev.h != cur.h ||
			prev.data.size() < static_cast<size_t>(prev.w) * static_cast<size_t>(prev.h) ||
			cur.data.size() < static_cast<size_t>(cur.w) * static_cast<size_t>(cur.h) ||
			field.gridW <= 0 || field.gridH <= 0 || !(field.blockSize > 0.0))
		{
			r.badShape = true;
			out = cur;
			return r;
		}
		const size_t nBlocks = static_cast<size_t>(field.gridW) * static_cast<size_t>(field.gridH);
		if (field.vx.size() < nBlocks || field.vy.size() < nBlocks || field.trusted.size() < nBlocks)
		{
			r.badShape = true;
			out = cur;
			return r;
		}

		const bool gridOk = record && record->blocks && record->blocks->gridW == field.gridW &&
			record->blocks->gridH == field.gridH && record->blocks->blockSize == field.blockSize;
		const CameraModel::Params activeParams = record ? (record->smoothed ? record->smoothedParams : record->params) : CameraModel::Params();
		const InvHomography inv = (gridOk && CameraModel::ModelUsable(policy, record)) ? InvertModel(activeParams) : InvHomography();
		const double weight = state.Weight();
		const bool usable = policy.enabled && p8Available && gridOk && inv.valid && weight > 0.0;
		r.cameraLate = policy.enabled && !usable;

		out.Resize(cur.w, cur.h);
		if (s.reset)
		{
			out.data = cur.data;
			return r;
		}

		const uint8_t* labels = usable ? record->blocks->label.data() : nullptr;
		const size_t labelCount = usable ? record->blocks->label.size() : 0;
		const CameraModel::Rect pic = usable ? record->picture : CameraModel::Rect{ 0, 0, cur.w, cur.h };

		for (int py = 0; py < cur.h; ++py)
		{
			const int by = (std::min)(field.gridH - 1, static_cast<int>(static_cast<double>(py) / field.blockSize));
			for (int px = 0; px < cur.w; ++px)
			{
				const int bx = (std::min)(field.gridW - 1, static_cast<int>(static_cast<double>(px) / field.blockSize));
				const size_t bi = static_cast<size_t>(by) * static_cast<size_t>(field.gridW) + static_cast<size_t>(bx);
				const size_t pi = static_cast<size_t>(py) * static_cast<size_t>(cur.w) + static_cast<size_t>(px);
				const bool trusted = field.trusted[bi] != 0;
				const double fx = static_cast<double>(px) + 0.5;
				const double fy = static_cast<double>(py) + 0.5;
				// Uncompensated previous position: block-constant measured vector for trusted blocks, own pixel if untrusted.
				double prevX = trusted ? fx - static_cast<double>(field.vx[bi]) : fx;
				double prevY = trusted ? fy - static_cast<double>(field.vy[bi]) : fy;

				bool usedCamera = false;
				if (usable && bi < labelCount && labels[bi] == static_cast<uint8_t>(CameraModel::Label::Camera))
				{
					double camPrevX = prevX, camPrevY = prevY;
					if (PrevPixelExact(inv, pic, fx, fy, camPrevX, camPrevY))
					{
						prevX = (1.0 - weight) * prevX + weight * camPrevX;
						prevY = (1.0 - weight) * prevY + weight * camPrevY;
						usedCamera = true;
					}
				}
				if (usedCamera)
					++r.cameraPixels;
				else
					++r.measuredPixels;

				const float pv = s.grainGuardNearest ? detail::SampleNearest(prev, prevX, prevY)
					: detail::SampleBilinear(prev, prevX, prevY);
				out.data[pi] = pv * (1.0f - s.alpha) + cur.data[pi] * s.alpha;
			}
		}
		r.variant = r.cameraPixels > 0 ? Variant::Camera : Variant::Uncompensated;
		return r;
	}

	// GLSL mirror of PrevPixelExact + accessor blend. Emitted ONLY when consumerEnabled is true
	// (off = empty string -> identical shader text and cache key).
	inline std::string GlslText(bool consumerEnabled)
	{
		if (!consumerEnabled)
			return std::string();
		std::string t;
		t += "#define VPU_RESIDUAL_CAMERA_ALIGN_VERSION " + std::to_string(kUnitVersion) + "\n";
		t += R"GLSL(
vec2 rca_prev_pixel(vec2 p, vec2 pic_centre, vec2 measured_prev, bool is_camera_block, float cam_weight, mat3 rca_inv_h, out bool used_camera)
{
    used_camera = false;
    if (!is_camera_block || !(cam_weight > 0.0))
        return measured_prev;
    vec2 rel = p - pic_centre;
    vec3 q = rca_inv_h * vec3(rel, 1.0);
    if (!(q.z > 1e-9))
        return measured_prev;
    vec2 cam_prev = (q.xy / q.z) + pic_centre;
    used_camera = true;
    return mix(measured_prev, cam_prev, cam_weight);
}
)GLSL";
		return t;
	}
}
