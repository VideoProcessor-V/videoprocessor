// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P5b-3 (doc 59 section 2.4 risk 3): UpdateDejudderEstimate's 15 positional arguments as one struct, in the
// same order and with the same values (the renderer's forwarder fills it from its arguments). References: valid
// for the call only. Payload / Contract are the renderer's QueuedFrame::ConvertedPayload and
// VideoFrameFormatterOutputContract (a template, so this header needs neither). `flipped` is unused (as before).

#include <cstddef>
#include <cstdint>
#include <memory>

namespace VpMotion
{
	template<class Payload, class Contract>
	struct DejudderEstimateCallT
	{
		const std::shared_ptr<const Payload>& next;
		const std::shared_ptr<const Payload>& current;
		const Contract& contract;
		int width;
		int height;
		size_t rowBytes;
		int chromaHeight;
		bool lossless422;
		bool flipped;
		uint64_t sourceSequence;
		bool cadenceRepeat;
		bool nativeRgb;
		bool interlaced;
		double sourceHz;
		uint64_t nextSourceSequence;
	};
}
