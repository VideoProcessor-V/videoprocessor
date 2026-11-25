// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// scaling.chroma_upscale: per-frame chroma-siting offset propagation for CfL_Prediction.glsl (GEM-066).
#include <pch.h>

#include <VP-V/modules/scaling/chroma_upscale/CflHookOffsets.h>
#include <VP-V/modules/output/dither/CustomHookParameters.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace ChromaUpscale
{
	namespace
	{
		bool IsCflPredictionFilename(const std::string& filename)
		{
			const size_t slash = filename.find_last_of("/\\");
			const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
			static const char kTarget[] = "cfl_prediction.glsl";
			constexpr size_t kTargetLen = sizeof(kTarget) - 1;
			if (filename.size() - start != kTargetLen)
				return false;
			for (size_t i = 0; i < kTargetLen; ++i)
			{
				const unsigned char c = static_cast<unsigned char>(filename[start + i]);
				if (static_cast<char>(std::tolower(c)) != kTarget[i])
					return false;
			}
			return true;
		}
	}

	CflOffsets CflOffsetsFromFrame(const pl_frame& frame)
	{
		if (frame.repr.sys == PL_COLOR_SYSTEM_RGB || frame.repr.sys == PL_COLOR_SYSTEM_XYZ)
			return CflOffsets();
		if (frame.num_planes < 2 || frame.num_planes > PL_MAX_PLANES)
			return CflOffsets();

		int lumaW = 0;
		int lumaH = 0;
		for (int i = 0; i < frame.num_planes; ++i)
		{
			const pl_plane& p = frame.planes[i];
			if (!p.texture) continue;
			const int comps = (std::min)(p.components, 4);
			for (int c = 0; c < comps; ++c)
			{
				if (p.component_mapping[c] == PL_CHANNEL_Y)
				{
					lumaW = p.texture->params.w;
					lumaH = p.texture->params.h;
					break;
				}
			}
			if (lumaW > 0 && lumaH > 0)
				break;
		}
		if (lumaW <= 0 || lumaH <= 0)
			return CflOffsets();

		for (int i = 0; i < frame.num_planes; ++i)
		{
			const pl_plane& p = frame.planes[i];
			if (!p.texture) continue;
			bool isChroma = false;
			const int comps = (std::min)(p.components, 4);
			for (int c = 0; c < comps; ++c)
			{
				const int cm = p.component_mapping[c];
				if (cm == PL_CHANNEL_CB || cm == PL_CHANNEL_CR)
				{
					isChroma = true;
					break;
				}
			}
			if (!isChroma) continue;
			const int chromaW = p.texture->params.w;
			const int chromaH = p.texture->params.h;
			if (chromaW <= 0 || chromaH <= 0 || chromaW > lumaW || chromaH > lumaH)
				continue;
			if (chromaW < lumaW || chromaH < lumaH)
			{
				const double sx = static_cast<double>(p.shift_x);
				const double sy = static_cast<double>(p.shift_y);
				if (!std::isfinite(sx) || !std::isfinite(sy))
					return CflOffsets();
				CflOffsets out;
				out.valid = true;
				out.x = (sx == 0.0) ? 0.0 : sx;
				out.y = (sy == 0.0) ? 0.0 : sy;
				return out;
			}
		}
		return CflOffsets();
	}

	bool RuleSetsCflParam(const std::vector<ConfiguredShaderRule>& rules, const char* paramName)
	{
		if (!paramName || paramName[0] == '\0')
			return false;
		for (const ConfiguredShaderRule& rule : rules)
		{
			if (rule.none || !IsCflPredictionFilename(rule.filename))
				continue;
			if (rule.parameters.find(paramName) != rule.parameters.end())
				return true;
		}
		return false;
	}

	void ApplyCflHookOffsets(const pl_frame& frame, const std::vector<const pl_hook*>& hooks,
		const std::vector<ConfiguredShaderRule>& rules, bool originalPathway)
	{
		if (originalPathway || hooks.empty())
			return;
		const CflOffsets off = CflOffsetsFromFrame(frame);
		if (!off.valid)
			return;
		const bool skipX = RuleSetsCflParam(rules, "chroma_offset_x");
		const bool skipY = RuleSetsCflParam(rules, "chroma_offset_y");
		if (skipX && skipY)
			return;
		for (const pl_hook* hook : hooks)
		{
			if (!skipX)
				CustomHookChain::SetHookParameter(hook, "chroma_offset_x", off.x);
			if (!skipY)
				CustomHookChain::SetHookParameter(hook, "chroma_offset_y", off.y);
		}
	}
}
