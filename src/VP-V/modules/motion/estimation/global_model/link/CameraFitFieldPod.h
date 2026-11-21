// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (CP-A1-D4-CAMLABEL-FIELDCOPY): the byte form of the P7 fit field as it is stored in the declared lane product
// p7f_camera_fit_field. The PRODUCT SLOT is the only storage of the field the labels read: the worker encodes the fit's forward
// field into the slot's CPU buffer once (before it publishes P7), P8 decodes it from the same slot under the slot's stamp. What P8
// reads is the grid, the block size and the forward vector / trust planes (nothing else of the field is stored).
// Pure, header-only, C++14.
#pragma once

#include "FieldCopy.h"

#include <cstdint>
#include <cstring>

namespace CameraLink
{
	constexpr uint32_t kFitFieldPodMagic = 0x46464C31u;   // "FFL1"
	constexpr size_t kFitFieldPodHeaderBytes = 4 + 4 + 4 + 8 + 4;

	// The bytes needed for a field with `blocks` blocks (three float planes with trust, two without).
	inline size_t FitFieldPodBytes(size_t blocks, bool trust) { return kFitFieldPodHeaderBytes + blocks * sizeof(float) * (trust ? 3u : 2u); }

	// The largest pod a fit can produce for a source of w x h px: the engine's native cell grid, cells of >= 8 source px per axis (a cell is
	// 4 engine px of a luma at most half the source size), with trust. The declared slot size of p7f_camera_fit_field must cover this.
	inline size_t WorstCaseFitFieldPodBytes(uint32_t w, uint32_t h)
	{
		const size_t cw = (static_cast<size_t>(w) + 7) / 8, ch = (static_cast<size_t>(h) + 7) / 8;
		return FitFieldPodBytes(cw * ch, true);
	}

	// Encodes the forward field. Returns the byte count; 0 = invalid field or the buffer is too small (the product is then NOT published).
	inline size_t EncodeFitFieldPod(const FieldCopy& f, uint8_t* out, size_t cap)
	{
		if (!out || !f.Valid() || f.fwdY.size() != f.Blocks())
			return 0;
		const bool trust = f.fwdTrust.size() == f.Blocks();
		const size_t blocks = f.Blocks();
		const size_t need = FitFieldPodBytes(blocks, trust);
		if (need > cap)
			return 0;
		size_t n = 0;
		const auto put = [&](const void* p, size_t bytes) { std::memcpy(out + n, p, bytes); n += bytes; };
		const uint32_t magic = kFitFieldPodMagic;
		const int32_t gw = f.gridW, gh = f.gridH;
		const double bs = f.blockSize;
		const uint32_t hasTrust = trust ? 1u : 0u;
		put(&magic, 4); put(&gw, 4); put(&gh, 4); put(&bs, 8); put(&hasTrust, 4);
		put(f.fwdX.data(), blocks * sizeof(float));
		put(f.fwdY.data(), blocks * sizeof(float));
		if (trust)
			put(f.fwdTrust.data(), blocks * sizeof(float));
		return n;
	}

	// Decodes into `out` (the backward planes and the provenance stay empty). False on a bad magic or a short buffer (out untouched). Backward planes are not touched (the caller's reused buffer never has any).
	inline bool DecodeFitFieldPod(const uint8_t* in, size_t bytes, FieldCopy& out)
	{
		if (!in || bytes < kFitFieldPodHeaderBytes)
			return false;
		uint32_t magic = 0, hasTrust = 0;
		int32_t gw = 0, gh = 0;
		double bs = 0;
		size_t n = 0;
		const auto get = [&](void* p, size_t b) { std::memcpy(p, in + n, b); n += b; };
		get(&magic, 4); get(&gw, 4); get(&gh, 4); get(&bs, 8); get(&hasTrust, 4);
		if (magic != kFitFieldPodMagic || gw <= 0 || gh <= 0 || !(bs > 0))
			return false;
		const size_t blocks = static_cast<size_t>(gw) * static_cast<size_t>(gh);
		if (blocks > bytes || FitFieldPodBytes(blocks, hasTrust != 0) > bytes)
			return false;
		// Decoded IN PLACE into `out` (its vectors keep their capacity: a reused buffer allocates nothing at steady state).
		out.gridW = gw;
		out.gridH = gh;
		out.blockSize = bs;
		out.fwdX.resize(blocks);
		out.fwdY.resize(blocks);
		get(out.fwdX.data(), blocks * sizeof(float));
		get(out.fwdY.data(), blocks * sizeof(float));
		if (hasTrust)
		{
			out.fwdTrust.resize(blocks);
			get(out.fwdTrust.data(), blocks * sizeof(float));
		}
		else
			out.fwdTrust.clear();
		return true;
	}
}
