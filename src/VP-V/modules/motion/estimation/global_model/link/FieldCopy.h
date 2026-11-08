// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1): the CPU copy of one verified P3 field, in SOURCE pixels, as the fit and the guidance plan read it.
// Pure, header-only, C++14, no Windows, no GPU.
#pragma once

#include <VP-V/core/Indicator.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace CameraLink
{
	// F2 provenance: where a copy came from. The readback ring stamps it; the camera fit refuses Live (the GUIDED live
	// matcher field, which the camera's own guidance shaped) at CameraLinkCore::SubmitField. Synthetic = built by hand (tests,
	// the accuracy harness, a dump replay). Ahead = the ahead fit's input (AheadFlow.h), accepted like Seed.
	enum class FieldOrigin
	{
		Synthetic,
		Live,    // IVectorEngine::FieldOf through FieldReadbackRing::Request(const VerifiedField&): never fitted
		Seed,    // IVectorEngine::SeedFieldOf through FieldReadbackRing::Request(const SeedField&)
		Ahead,   // camera ahead fit: IVectorEngine::FlowOf (the producer's UNGUIDED NVOF pair) through CopyAhead -> AheadToSourceGrid
	};

	struct FieldCopy
	{
		enum { kSourceBlockPx = 16 };               // the camera fit's source grid (the live field's 16-px blocks)
		uint64_t seq = 0, pairFrom = 0, pairTo = 0;
		int gridW = 0, gridH = 0;
		double blockSize = 0;                       // source px per block
		std::vector<float> fwdX, fwdY, fwdTrust;    // source px, gridW*gridH
		std::vector<float> bwdX, bwdY, bwdTrust;    // empty when no backward field
		VpIndicator::Stamp stamp;                   // the stamp the field was computed under
		FieldOrigin origin = FieldOrigin::Synthetic;
		// G-A3: the P2 provenance of an Ahead field (the FlowOf pair = pairFrom -> pairTo, served under the engine's vectors-valid epoch
		// p2Epoch, IVectorEngine::FieldEra). Checked at claim, seal and every consumer read: another epoch = refused, counted, shown.
		bool haveP2 = false;
		uint64_t p2Epoch = 0;
		bool HaveBackward() const { return !bwdX.empty(); }
		size_t Blocks() const { return static_cast<size_t>(gridW > 0 ? gridW : 0) * static_cast<size_t>(gridH > 0 ? gridH : 0); }
		bool Valid() const { return gridW > 0 && gridH > 0 && blockSize > 0 && fwdX.size() == Blocks(); }
	};

	// Unpack an rgba32f texel array (vx, vy, trust, unused per block; gridW*gridH*4 floats) into the planes, scaling the
	// vectors by `scaleToSourcePx`. Trust is copied as is. Deterministic, order independent.
	inline void UnpackRgba(const float* rgba, size_t blocks, float scaleToSourcePx,
		std::vector<float>& x, std::vector<float>& y, std::vector<float>& trust)
	{
		x.resize(blocks);
		y.resize(blocks);
		trust.resize(blocks);
		for (size_t i = 0; i < blocks; ++i)
		{
			x[i] = rgba[i * 4 + 0] * scaleToSourcePx;
			y[i] = rgba[i * 4 + 1] * scaleToSourcePx;
			trust[i] = rgba[i * 4 + 2];
		}
	}

	// F2: the optical-flow seed's layout (IVectorEngine::SeedField, as plain values).
	struct SeedLayout
	{
		int gridW = 0, gridH = 0;               // seed texels
		int per = 0;                            // 16-px source blocks per seed texel and axis (2^halvings)
		float scaleToSourcePx = 0.0f;           // seed px -> source px
		int sourceGridW = 0, sourceGridH = 0;   // the 16-px source-block grid of the result
		float sadTrustMax = 0.0f;               // the live SAD limit (taper_sad with block_taper, else match_tolerance)
		float weightPenalty = 0.0f;             // the live engine-weight penalty (2 x sadTrustMax with trust weights, else 0)
		float consistencyPx = 0.0f;             // forward/backward agreement, seed px
	};

	// F2: the seed (rgba per texel: vx, vy in seed px, SAD, engine weight; forward and backward planes, gridW*gridH*4 floats
	// each) onto the 16-px source-block grid, forward only (the fit reads no backward field, as with the live field). Each
	// source block takes the seed texel its centre lies in (block t -> texel t / per, clamped). Trust (1/0) is the live
	// consistency pass's rule on the seed: forward and backward agree within consistencyPx (the backward texel where the
	// block's centre lands, seed px of 8-px blocks), and SAD + (1 - weight) x weightPenalty <= sadTrustMax (a reach
	// sentinel SAD of 1e4 fails); a non-finite texel is untrusted with zero motion. Deterministic, order independent.
	inline bool SeedToSourceGrid(const float* fwd, const float* bwd, const SeedLayout& l, FieldCopy& out)
	{
		if (!fwd || !bwd || l.gridW <= 0 || l.gridH <= 0 || l.per <= 0 || l.sourceGridW <= 0 || l.sourceGridH <= 0 ||
			(l.sourceGridW + l.per - 1) / l.per != l.gridW || (l.sourceGridH + l.per - 1) / l.per != l.gridH ||
			!(l.scaleToSourcePx > 0.0f) || !(l.sadTrustMax > 0.0f) || !(l.consistencyPx > 0.0f) || !(l.weightPenalty >= 0.0f))
			return false;
		out.gridW = l.sourceGridW;
		out.gridH = l.sourceGridH;
		out.blockSize = 16.0;
		out.origin = FieldOrigin::Seed;
		const size_t blocks = static_cast<size_t>(l.sourceGridW) * static_cast<size_t>(l.sourceGridH);
		out.fwdX.assign(blocks, 0.0f);
		out.fwdY.assign(blocks, 0.0f);
		out.fwdTrust.assign(blocks, 0.0f);
		out.bwdX.clear();
		out.bwdY.clear();
		out.bwdTrust.clear();
		const float penalty = l.weightPenalty;
		for (int ty = 0; ty < l.sourceGridH; ++ty)
		{
			const int sy = (std::min)(ty / l.per, l.gridH - 1);
			for (int tx = 0; tx < l.sourceGridW; ++tx)
			{
				const int sx = (std::min)(tx / l.per, l.gridW - 1);
				const float* f = fwd + (static_cast<size_t>(sy) * static_cast<size_t>(l.gridW) + static_cast<size_t>(sx)) * 4;
				const size_t o = static_cast<size_t>(ty) * static_cast<size_t>(l.sourceGridW) + static_cast<size_t>(tx);
				if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2]) || !std::isfinite(f[3]))
					continue;   // zero motion, untrusted
				out.fwdX[o] = f[0] * l.scaleToSourcePx;
				out.fwdY[o] = f[1] * l.scaleToSourcePx;
				const float lx = static_cast<float>(sx * 8 + 4) + f[0], ly = static_cast<float>(sy * 8 + 4) + f[1];
				int bx = static_cast<int>(std::floor(lx / 8.0f)), by = static_cast<int>(std::floor(ly / 8.0f));
				bx = bx < 0 ? 0 : bx >= l.gridW ? l.gridW - 1 : bx;
				by = by < 0 ? 0 : by >= l.gridH ? l.gridH - 1 : by;
				const float* g = bwd + (static_cast<size_t>(by) * static_cast<size_t>(l.gridW) + static_cast<size_t>(bx)) * 4;
				if (!std::isfinite(g[0]) || !std::isfinite(g[1]))
					continue;
				const float ex = f[0] + g[0], ey = f[1] + g[1];
				const bool consistent = std::sqrt(ex * ex + ey * ey) <= l.consistencyPx;
				const float w = f[3] < 0.0f ? 0.0f : f[3] > 1.0f ? 1.0f : f[3];
				const bool good = f[2] + (1.0f - w) * penalty <= l.sadTrustMax;
				out.fwdTrust[o] = consistent && good ? 1.0f : 0.0f;
			}
		}
		return true;
	}
}
