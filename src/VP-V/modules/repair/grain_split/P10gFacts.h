// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/SkeletonAcquire.h>

#include <memory>

namespace GrainSplit
{
	namespace LevelGpu
	{
		// Converts source analysis codes (native depth bits) to 8-bit codes: 255 / ((1 << depth) - 1).
		// POD2-H1 fix: 12-bit sources use 255/4095 (not 10-bit 255/1023), 8-bit sources use 255/255 = 1.0.
		inline double CodeToEightBitOf(int sourceBits)
		{
			const int bits = (sourceBits >= 1 && sourceBits <= 16) ? sourceBits : 10;
			return 255.0 / static_cast<double>((1u << bits) - 1u);
		}

		// Metadata carried on kP10gGrainLevel GPU AcquireResult via textureSet tag + keepAlive (same idiom as P3FieldFill.h).
		struct P10gFacts
		{
			int sourceBits = 10;
			int blockPx = 16;
			double codeToEightBit = 255.0 / 1023.0;
			std::shared_ptr<void> textureKeepAlive;
		};

		inline const void* P10gFactsTag()
		{
			static const char tag = 0;
			return &tag;
		}

		inline const P10gFacts* FactsOf(const VpSkeleton::AcquireResult& r)
		{
			return r.Ok() && r.gpu.textureSet == P10gFactsTag()
				? static_cast<const P10gFacts*>(r.gpu.keepAlive.get())
				: nullptr;
		}
	}
}
