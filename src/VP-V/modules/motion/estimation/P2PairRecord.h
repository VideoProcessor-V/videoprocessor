// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// motion/estimation: the CPU FORM of the P2 vector pair (kP2VectorPair, AcquireForm::Cpu) = the pair's identity and the whole-frame
// motion the producer worked out for it (the engine global, forward [0] and backward [1], D1 format, plus the second layer).
// The flow arrays are NOT part of the form: they stay behind FlowOf / P2View (one copy, rule 1).
// ONE codec (EncodeP2PairRecord / DecodeP2PairRecord): the owner fill (P2CpuForm.cpp) writes it, every reader decodes it
// (CameraRecordPool.h CRD1 pattern: magic, exact size, refusal on anything else).
// Header-only; C++14; no dependencies on the producer or the skeleton.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace VpMotion
{
	constexpr uint32_t kP2PairRecordMagic = 0x31523250u;   // "P2R1" (little-endian bytes 'P' '2' 'R' '1')
	constexpr uint32_t kP2PairFlagGlobalSet = 1u;          // bit0: the global / second were worked out (FlowGlobals) and are meaningful
	constexpr uint32_t kP2PairFlagGlobalGeneral = 2u;      // bit1: voted with the general_rate_fixes flag (MotionAhead::Flow::globalGeneral)

	// fwd = [0], bwd = [1]; each (x, y, ambiguity, w) in the de-judder's D1 format (x, y in 1/8-size texels = one engine cell),
	// exactly MotionAhead::Flow::global / second.
	struct P2PairRecord
	{
		uint32_t magic = kP2PairRecordMagic;
		uint32_t flags = 0;
		uint64_t from = 0, to = 0;
		uint64_t generation = 0;   // the validity generation the pair was stored under
		float global[2][4] = {};
		float second[2][4] = {};
		bool GlobalSet() const { return (flags & kP2PairFlagGlobalSet) != 0; }
		bool GlobalGeneral() const { return (flags & kP2PairFlagGlobalGeneral) != 0; }
	};

	// The encoded size: 4 + 4 + 3 x 8 + 2 x (2 x 4 x 4) = 96 bytes.
	constexpr size_t kP2PairRecordBytes = 4 + 4 + 8 + 8 + 8 + 2 * 4 * sizeof(float) * 2;

	// Builds the record from the pair's identity and its Flow's global fields (arrays of [2][4], D1 format).
	inline P2PairRecord MakeP2PairRecord(uint64_t from, uint64_t to, uint64_t generation, const float (&global)[2][4],
		const float (&second)[2][4], bool globalSet, bool globalGeneral)
	{
		P2PairRecord r;
		r.flags = (globalSet ? kP2PairFlagGlobalSet : 0u) | (globalGeneral ? kP2PairFlagGlobalGeneral : 0u);
		r.from = from;
		r.to = to;
		r.generation = generation;
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
			{
				r.global[d][k] = global[d][k];
				r.second[d][k] = second[d][k];
			}
		return r;
	}

	namespace detail
	{
		template <class T> inline void PutP2(uint8_t* out, size_t& n, const T& v)
		{
			std::memcpy(out + n, &v, sizeof(T));
			n += sizeof(T);
		}
		template <class T> inline T GetP2(const uint8_t* in, size_t& n)
		{
			T v{};
			std::memcpy(&v, in + n, sizeof(T));
			n += sizeof(T);
			return v;
		}
	}

	// Returns the byte count written (kP2PairRecordBytes), 0 when `cap` is too small.
	inline size_t EncodeP2PairRecord(const P2PairRecord& r, uint8_t* out, size_t cap)
	{
		if (out == nullptr || cap < kP2PairRecordBytes)
			return 0;
		size_t n = 0;
		detail::PutP2(out, n, kP2PairRecordMagic);
		detail::PutP2(out, n, r.flags);
		detail::PutP2(out, n, r.from);
		detail::PutP2(out, n, r.to);
		detail::PutP2(out, n, r.generation);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				detail::PutP2(out, n, r.global[d][k]);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				detail::PutP2(out, n, r.second[d][k]);
		return n;
	}

	// False (and `out` untouched) when the size is not exactly kP2PairRecordBytes or the magic differs.
	inline bool DecodeP2PairRecord(const uint8_t* in, size_t bytes, P2PairRecord& out)
	{
		if (in == nullptr || bytes != kP2PairRecordBytes)
			return false;
		size_t n = 0;
		if (detail::GetP2<uint32_t>(in, n) != kP2PairRecordMagic)
			return false;
		P2PairRecord r;
		r.flags = detail::GetP2<uint32_t>(in, n);
		r.from = detail::GetP2<uint64_t>(in, n);
		r.to = detail::GetP2<uint64_t>(in, n);
		r.generation = detail::GetP2<uint64_t>(in, n);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				r.global[d][k] = detail::GetP2<float>(in, n);
		for (int d = 0; d < 2; ++d)
			for (int k = 0; k < 4; ++k)
				r.second[d][k] = detail::GetP2<float>(in, n);
		out = r;
		return true;
	}
}
