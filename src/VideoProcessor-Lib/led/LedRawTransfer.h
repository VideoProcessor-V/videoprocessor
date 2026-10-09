#pragma once

#include "LedLookahead.h"

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
}
