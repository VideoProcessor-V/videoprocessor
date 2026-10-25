#pragma once

#include "NetPackage.h"

#include <array>
#include <string>
#include <vector>

// Fused compute layers for the exported conv passes (efficiency batch, coordinator 2026-10-24).
//
// Why: the exporter writes one fragment pass per GROUP of 4 output channels, and every such pass
// re-reads all input groups at 9 taps. For D (24 channels = 6 groups) one layer is 6 passes x 6
// inputs x 9 taps = 324 texture fetches per pixel. D measured 3.7 ms and D_small 2.47 ms at
// 2560x1600 on the 3090: far from the 44% D_small should save on arithmetic alone, so both
// traffic and arithmetic matter.
//
// What: ExtractConvLayers recognises the exporter's conv-group passes (fragment, or tile-gated
// compute) and regroups them into layers, keeping the weights. FusedLayerGlsl writes ONE compute
// pass per layer from the SAME weights:
//   - the workgroup loads its input tile + a 1-pixel halo into shared memory once (mirrored at
//     the picture edges like the exporter's mi()); fp32, or fp16 storage (packHalf2x16);
//   - Direct: each thread computes ALL output groups of its pixel (accumulators in registers, each
//     tap read once);
//   - Winograd F(2x2,3x3): each thread computes a 2x2 block from a 4x4 patch,
//     Y = A^T [(G g G^T) . (B^T d B)] A, with the weight transform done here (the literals in the
//     shader are U = G g G^T), for 16 multiplies per 4 outputs instead of 36;
//   - fp32 accumulation always; bias, ReLU, the identity skip and the tile gate are kept.
// Equivalence is checked against the CPU reference (PlNetRunnerGpuTests). C++14, no libplacebo.
namespace VpNets
{
	struct ConvLayer
	{
		std::string hook, width, height;          // the first pass's (all passes of a layer agree)
		std::vector<std::string> inputs;          // input group textures, in order
		std::vector<std::string> outputs;         // SAVE name per output group
		std::vector<std::array<float, 4>> bias;   // per output group
		std::vector<std::string> biasText;        // the bias literal as written (exact)
		// Per (output group o, input group g, tap k = dy * 3 + dx): the GLSL mat4 literal as
		// written ("" = absent / zero) and its 16 floats (column-major: column c = the 4 output
		// channels' weights for input channel c).
		std::vector<std::string> matText;
		std::vector<std::array<float, 16>> mat;
		bool relu = false, skip = false;          // skip: output group o adds input group o
		std::string gate;                         // tile gate texture ("" = none)
		int gateTile = 0;
		std::vector<size_t> passes;               // the exported passes this layer replaces
		size_t Index(size_t o, size_t g, int k) const { return (o * inputs.size() + g) * 9 + static_cast<size_t>(k); }
	};

	// The fusable conv layers of a package, in pass order. Passes that are not plain conv groups
	// (features, pools, A's conditioned first layer, the bounded outputs) stay exported.
	std::vector<ConvLayer> ExtractConvLayers(const NetPackage& pkg);

	enum class FusedKind { Direct, Winograd };

	struct FusedShader
	{
		std::string glsl;          // header text: helpers, shared tile, hook()
		int groupX = 0, groupY = 0;
		int tileX = 0, tileY = 0;  // output pixels per workgroup
		size_t sharedBytes = 0;
	};

	// Descriptor names the generated code uses: NFI<g>_raw (sampled inputs), NFO<o> (storage
	// outputs), NFG_raw (the gate), and the variable nf_size (ivec2 output size).
	FusedShader FusedLayerGlsl(const ConvLayer& layer, FusedKind kind, bool fp16Shared);
}
