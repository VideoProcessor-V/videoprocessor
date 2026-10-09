// Built without the precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "LedRawTransfer.h"

#include <libplacebo/tone_mapping.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace LedDrive
{
	namespace
	{
		double GammaOf(pl_color_transfer transfer)
		{
			switch (transfer)
			{
			case PL_COLOR_TRC_LINEAR: return 1.0;
			case PL_COLOR_TRC_GAMMA18:
			case PL_COLOR_TRC_PRO_PHOTO: return 1.8;
			case PL_COLOR_TRC_GAMMA20: return 2.0;
			case PL_COLOR_TRC_GAMMA22: return 2.2;
			case PL_COLOR_TRC_GAMMA26:
			case PL_COLOR_TRC_ST428: return 2.6;
			case PL_COLOR_TRC_GAMMA28: return 2.8;
			default: return 2.4; // BT.1886 and anything unlisted
			}
		}

		// SDR signal (0..1) to nits between the space's black and white, as
		// libplacebo does: BT.1886 with its black offset, the others scaled
		// from black to white.
		double SdrToNits(pl_color_transfer transfer, double x, double black, double white)
		{
			if (transfer == PL_COLOR_TRC_BT_1886)
			{
				const double lw = std::pow(white, 1.0 / 2.4), lb = std::pow(std::max(black, 0.0), 1.0 / 2.4);
				const double a = std::pow(lw - lb, 2.4), b = lb / (lw - lb);
				return a * std::pow(std::max(x + b, 0.0), 2.4);
			}
			const double relative = transfer == PL_COLOR_TRC_SRGB ?
				(x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4)) :
				std::pow(x, GammaOf(transfer));
			return black + (white - black) * relative;
		}

		// Nits back to the target signal, the inverse of the above.
		double NitsToSignal(pl_color_transfer transfer, double nits, double black, double white)
		{
			if (transfer == PL_COLOR_TRC_BT_1886)
			{
				const double lw = std::pow(white, 1.0 / 2.4), lb = std::pow(std::max(black, 0.0), 1.0 / 2.4);
				const double a = std::pow(lw - lb, 2.4), b = lb / (lw - lb);
				return std::max(0.0, std::min(std::pow(std::max(nits, 0.0) / a, 1.0 / 2.4) - b, 1.0));
			}
			const double v = std::max(0.0, std::min((nits - black) / std::max(white - black, 1e-9), 1.0));
			if (transfer == PL_COLOR_TRC_SRGB)
				return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
			return std::pow(v, 1.0 / GammaOf(transfer));
		}

		double PqToNits(double x)
		{
			const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
			const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
			const double n = std::pow(std::max(x, 0.0), 1.0 / m2);
			return 10000.0 * std::pow(std::max(n - c1, 0.0) / (c2 - c3 * n), 1.0 / m1);
		}

		double HlgToNits(double x)
		{
			const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
			const double scene = x <= 0.5 ? x * x / 3.0 : (std::exp((x - c) / a) + b) / 12.0;
			return 1000.0 * std::pow(scene, 1.2);
		}

		void Infer(const pl_color_space& sourceIn, const pl_color_space& targetIn,
			pl_color_space& source, pl_color_space& target, float range[4])
		{
			source = sourceIn;
			target = targetIn;
			pl_color_space_infer_map(&source, &target);
			pl_nominal_luma_params luma{};
			luma.metadata = PL_HDR_METADATA_ANY;
			luma.scaling = PL_HDR_NITS;
			luma.color = &source;
			luma.out_min = &range[0];
			luma.out_max = &range[1];
			pl_color_space_nominal_luma_ex(&luma);
			luma.color = &target;
			luma.out_min = &range[2];
			luma.out_max = &range[3];
			pl_color_space_nominal_luma_ex(&luma);
		}

		void Hash(uint64_t& hash, const void* data, size_t size)
		{
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		}
	}

	RawTransfer BuildRawTransfer(const pl_color_space& sourceIn, const pl_color_space& targetIn,
		const pl_color_map_params* colorMap)
	{
		pl_color_space source{}, target{};
		float range[4] = {};
		Infer(sourceIn, targetIn, source, target, range);
		pl_tone_map_params toneMap{};
		toneMap.function = colorMap && colorMap->tone_mapping_function ?
			colorMap->tone_mapping_function : &pl_tone_map_spline;
		toneMap.constants = colorMap ? colorMap->tone_constants : pl_color_map_default_params.tone_constants;
		toneMap.input_scaling = PL_HDR_NITS;
		toneMap.output_scaling = PL_HDR_NITS;
		toneMap.input_min = range[0];
		toneMap.input_max = range[1];
		toneMap.output_min = range[2];
		toneMap.output_max = range[3];
		toneMap.hdr = source.hdr;
		pl_tone_map_params_infer(&toneMap);
		const bool noop = pl_tone_map_params_noop(&toneMap);

		RawTransfer transfer(RAW_TRANSFER_SIZE);
		for (int i = 0; i < RAW_TRANSFER_SIZE; ++i)
		{
			const double x = static_cast<double>(i) / (RAW_TRANSFER_SIZE - 1);
			double nits;
			if (source.transfer == PL_COLOR_TRC_PQ) nits = PqToNits(x);
			else if (source.transfer == PL_COLOR_TRC_HLG) nits = HlgToNits(x);
			else nits = SdrToNits(source.transfer, x, range[0], range[1]);
			const double mapped = noop ? nits : pl_tone_map_sample(static_cast<float>(nits), &toneMap);
			transfer[i] = static_cast<float>(NitsToSignal(target.transfer, mapped, range[2], range[3]));
		}
		return transfer;
	}

	uint64_t RawTransferSignature(const pl_color_space& sourceIn, const pl_color_space& targetIn,
		const pl_color_map_params* colorMap)
	{
		pl_color_space source{}, target{};
		float range[4] = {};
		Infer(sourceIn, targetIn, source, target, range);
		uint64_t hash = 14695981039346656037ull;
		const int transfers[2] = { static_cast<int>(source.transfer), static_cast<int>(target.transfer) };
		Hash(hash, transfers, sizeof(transfers));
		Hash(hash, range, sizeof(range));
		const void* function = colorMap ? colorMap->tone_mapping_function : nullptr;
		Hash(hash, &function, sizeof(function));
		if (colorMap) Hash(hash, &colorMap->tone_constants, sizeof(colorMap->tone_constants));
		return hash;
	}
}
