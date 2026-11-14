// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <VP-V/core/FallbackCatalog.h>

#include <string>

// S-12BIT (refs/119 #51): libplacebo latches renderer error flags (pl_render_errors.errors) and keeps
// rendering through a reduced, direct pass without telling anyone. This builds the visible entry from the
// flag word. The bit values mirror enum pl_render_error (the renderer static_asserts them against the real
// header); PL_RENDER_ERR_HOOKS is excluded: a disabled hook already has its own visible entry.
namespace LibplaceboErrors
{
	enum Bits : int
	{
		kFbo = 1 << 0, kSampling = 1 << 1, kDebanding = 1 << 2, kBlending = 1 << 3, kOverlay = 1 << 4,
		kPeakDetect = 1 << 5, kFilmGrain = 1 << 6, kFrameMixing = 1 << 7, kDeinterlacing = 1 << 8,
		kErrorDiffusion = 1 << 9, kHooks = 1 << 10, kContrastRecovery = 1 << 11, kBlur = 1 << 12,
	};

	inline int Reportable(int errors) { return errors & ~static_cast<int>(kHooks); }

	inline std::string Names(int errors)
	{
		static const struct { int bit; const char* text; } kTable[] = {
			{ kFbo, "intermediate pictures (FBO) in a reduced format" },
			{ kSampling, "scaling in a simpler filter" },
			{ kDebanding, "debanding off" },
			{ kBlending, "blending off" },
			{ kOverlay, "overlay off" },
			{ kPeakDetect, "HDR peak detection off" },
			{ kFilmGrain, "film grain off" },
			{ kFrameMixing, "frame mixing off" },
			{ kDeinterlacing, "deinterlacing off" },
			{ kErrorDiffusion, "error diffusion off" },
			{ kContrastRecovery, "contrast recovery off" },
			{ kBlur, "blur off" },
		};
		std::string out;
		for (const auto& e : kTable)
			if ((errors & e.bit) != 0)
			{
				if (!out.empty()) out += "; ";
				out += e.text;
			}
		return out;
	}

	inline FallbackCatalog::Decision Latched(int errors)
	{
		const int shown = Reportable(errors);
		if (shown == 0)
			return FallbackCatalog::Released("libplacebo errors");
		return FallbackCatalog::Engaged("libplacebo errors",
			"libplacebo latched an internal error and renders a reduced pass (clears on renderer reset): " + Names(shown));
	}

	// The Original pathway is Bill's own: nothing of VP's is reported there, and a stale entry is released.
	inline FallbackCatalog::Decision Latched(int errors, bool originalPathway)
	{
		return Latched(originalPathway ? 0 : errors);
	}
}
