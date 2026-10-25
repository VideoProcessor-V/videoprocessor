#pragma once

#include <VP-V/modules/drive/led_measure/LedLookahead.h>

#include <libplacebo/colorspace.h>
#include <libplacebo/shaders/colorspace.h>

#include <cstdint>

// Builds the lookahead's raw-to-output transfer from what the renderer
// actually uses for a frame: the source and target colour spaces and its
// colour-mapping parameters. Input transfer (PQ, HLG or the SDR gamma) to
// light, libplacebo's own tone-map curve (pl_tone_map_sample) between the
// nominal source and target luminance, then the target transfer
// (output_gamma). Gamut mapping, the target LUT and channel weights are
// ignored; the module's correction ratio absorbs them. It only reads the
// profile: nothing here sets or changes output_gamma.
namespace LedDrive
{
	RawTransfer BuildRawTransfer(const pl_color_space& source, const pl_color_space& target,
		const pl_color_map_params* colorMap);

	// Changes whenever BuildRawTransfer would build a different table.
	uint64_t RawTransferSignature(const pl_color_space& source, const pl_color_space& target,
		const pl_color_map_params* colorMap);

	// Automatic calibration phase 2 (HDR verify): libplacebo's tone curve for
	// this source / target / colour map, in nits, at `count` source levels
	// (evenly spaced in PQ from 0.005 nits to the source peak) - the same inference and
	// pl_tone_map_sample call as BuildRawTransfer.
	struct ToneCurveSamples
	{
		int count = 0;
		float sourceNits[64] = {};
		float outputNits[64] = {};
		float sourcePeak = 0.0f, sourceBlack = 0.0f, targetPeak = 0.0f, targetBlack = 0.0f;
		bool active = false;           // false: the mapping is a no-op
		const char* function = "";     // the function's name (static storage)
	};
	ToneCurveSamples SampleToneCurve(const pl_color_space& source, const pl_color_space& target,
		const pl_color_map_params* colorMap, int count = 64);
}
