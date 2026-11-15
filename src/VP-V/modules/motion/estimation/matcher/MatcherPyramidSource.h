// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// L-B4 (BBP-B commit B3): the matcher's READ seam for the shared luma pyramid (kP1HalfLuma, GPU form, variant
// MatcherPyramidKey(level); owner input/source_analysis/lane/P1GpuFill.cpp). The matcher (DejudderWarp) knows no skeleton:
// the engine hands it this function, which asks the memo. Levels count halvings of the source (1 half, 3 = 1/8, 5 = 1/32;
// the lowRes inner 2, 4, 6). `lumaTex` is the pl_tex the matcher itself would downsample (its source luma): the engine
// serves the product only when that is the shared P13 luma plane (the named exception: any other luma = own build).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace VpMotion
{
	namespace MatcherPyramid
	{
		struct Level
		{
			// NotConfigured: no skeleton memo for this frame (the skeleton is off): the matcher builds its own, as before, and
			// shows nothing (there is no product to fall back from). Unavailable: the product was asked and not served: the matcher
			// builds its own and the row product.matcher_pyramid is engaged with `cause`. Product: `tex` (a pl_tex) is the level.
			enum class State : uint8_t { NotConfigured, Product, Unavailable } state = State::NotConfigured;
			void* tex = nullptr;
			std::shared_ptr<const void> pin;   // keeps the texture alive; held by the matcher until its next Estimate
			std::string cause;
		};
		typedef std::function<Level(uint64_t seq, int level, const void* lumaTex)> Source;
	}
}
