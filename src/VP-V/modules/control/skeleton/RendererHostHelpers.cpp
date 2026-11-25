// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include <pch.h>
#include "RendererHostHelpers.h"

#include <DebugLog.h>
#include <VP-V/core/GpuAttributionPl.h>
#include <VP-V/modules/output/presentation/RepeatReuseGlue.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace Vpv
{
	namespace RendererHost
	{
		void EnsureRebaseDispatch(pl_dispatch& rebaseDispatch, pl_log log, pl_d3d11 d3d11)
		{
			if (!rebaseDispatch)
			{
				rebaseDispatch = pl_dispatch_create(log, d3d11->gpu);
				GpuAttribution::Watch(rebaseDispatch, "rebase");
			}
		}

		void TagSlotHooks(const std::vector<const pl_hook*>& frameHooks,
			std::vector<std::pair<const pl_hook*, PipelineSlots::Id>>& frameSlotHooks,
			size_t start, PipelineSlots::Id slot)
		{
			for (size_t i = start; i < frameHooks.size(); ++i)
				frameSlotHooks.emplace_back(frameHooks[i], slot);
		}

		void EngageFallback(FallbackStatus::Registry& fallbacks, const char* id, const std::string& reason)
		{
			if (fallbacks.Engage(id, reason))
				DebugLog::Log("%s", fallbacks.Heading().c_str());
		}

		void ReleaseFallback(FallbackStatus::Registry& fallbacks, const char* id)
		{
			if (fallbacks.Release(id))
				DebugLog::Log("%s", fallbacks.Heading().c_str());
		}

		void SyncFallback(FallbackStatus::Registry& fallbacks, const FallbackCatalog::Decision& decision)
		{
			if (FallbackCatalog::Sync(fallbacks, decision))
				DebugLog::Log("%s", fallbacks.Heading().c_str());
		}

		CustomHookChain::Loader BuildGlslHookLoader(
			const pl_d3d11& d3d11,
			bool (*readUserShader)(const std::string&, std::string&, std::string&, std::string&))
		{
			return {
				[readUserShader](const std::string& filename, std::string& source, std::string& reason)
				{
					std::string resolvedPath;
					return readUserShader ? readUserShader(filename, source, resolvedPath, reason) : false;
				},
				[&d3d11](const std::string& source) -> const pl_hook*
				{
					const pl_hook* hook = d3d11 ? pl_mpv_user_shader_parse(d3d11->gpu, source.data(), source.size()) : nullptr;
					VpRepeatReuse::TemporalShaderRegistry::Global().Note(hook, source.data(), source.size());   // WP-24 M2
					return hook;
				},
				[](const pl_hook*& hook) { pl_mpv_user_shader_destroy(&hook); } };
		}

		void MaybeReloadDisplayLut(CalibrationLutHost::Adapter& calibrationHost,
			const std::string& displayLutPath, bool calibrationLutEnabled,
			const std::function<void()>& reloadDisplayLut)
		{
			if (calibrationHost.LutReloadDue(displayLutPath, calibrationLutEnabled, GetTickCount64()) && reloadDisplayLut)
				reloadDisplayLut();
		}

		bool PresentCalibrationPattern(CalibrationLutHost::Adapter& calibrationHost,
			pl_d3d11 d3d11, IDXGISwapChain1* vpOwnedSwapchain, pl_swapchain swapchain,
			DXGI_FORMAT negotiatedSwapchainFormat, bool outputResourcesRetired,
			const std::function<bool(ID3D11Texture2D*, std::vector<uint32_t>&, D3D11_TEXTURE2D_DESC&, std::string&)>& readR10,
			FallbackStatus::Registry& fallbacks)
		{
			if (!d3d11 || !d3d11->gpu || !swapchain || outputResourcesRetired)
				return false;
			const CalibrationLutHost::PresentResult presented = calibrationHost.Present({ d3d11->gpu, vpOwnedSwapchain,
				swapchain, negotiatedSwapchainFormat, readR10 });
			if (presented.patchWindow.engaged)
				EngageFallback(fallbacks, presented.patchWindow.id.c_str(), presented.patchWindow.reason);
			return presented.presented;
		}

		bool IsSigmaDeltaRule(const ConfiguredShaderRule& rule)
		{
			std::string f = rule.filename;
			std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return f.find("sigma-delta") != std::string::npos || f.find("sigma_delta") != std::string::npos;
		}

		std::vector<ConfiguredShaderRule> TemporalDitherSelection(
			const std::vector<ConfiguredShaderRule>& requested, const std::string& mode)
		{
			std::vector<ConfiguredShaderRule> out;
			bool have = false;
			for (const ConfiguredShaderRule& rule : requested)
			{
				if (IsSigmaDeltaRule(rule))
				{
					if (mode == "off" || mode == "libplacebo") continue;
					have = true;
				}
				out.push_back(rule);
			}
			if (mode == "sigma_delta" && !have)
			{
				ConfiguredShaderRule rule;
				rule.name = "vp.temporal_dither";
				rule.label = "Temporal sigma-delta (VP)";
				rule.filename = "Temporal sigma-delta.glsl";
				rule.stageOrder = 90;
				out.push_back(rule);
			}
			return out;
		}

		void PublishCustomShaderStatus(CustomShaderStatusHost& h)
		{
			auto descriptions = h.customHooks.ActiveDescriptions(h.lowLatency);
			// Stage 8-2: the blend's own line (what it is doing on this disc).
			if (h.activeSettings.adaptiveDownscale)
			{
				char line[160];
				const AdaptiveDownscale::Stats st = h.adaptiveDownscale.LastStats();
				if (h.adaptiveNoSsimLogged)
					std::snprintf(line, sizeof(line), "Adaptive downscale: needs SSimDownscaler");
				else if (!h.adaptiveDownscale.Active())
					std::snprintf(line, sizeof(line), "Adaptive downscale: idle (not downscaling)");
				else if (!st.valid)
					std::snprintf(line, sizeof(line), "Adaptive downscale: measuring");
				else
					std::snprintf(line, sizeof(line), "Adaptive downscale: grain %.2f, w %.2f%s%s", st.grainScale, st.meanW,
						h.adaptiveDownscale.LastMapState() == AdaptiveDownscale::MapState::Used ? ", grain map" :
						h.activeSettings.adaptive.grainMap ? ", no vectors (per-frame grain)" : "",
						h.bitrateWeightScale < 1.0 ? (", low bitrate x" + std::to_string(h.bitrateWeightScale).substr(0, 4)).c_str() : "");
				descriptions.push_back(line);
			}
			// Stage 8b's form and the motion-adapted blend (VP-V scaling.output: its own lines).
			for (const std::string& stageLine : h.adaptiveStage.StatusLines())
				descriptions.push_back(stageLine);
			// Stage 12: the source's effective resolution, bitrate quality and bit depth.
			descriptions.push_back(SourceResolutionMonitor::Describe(h.sourceMonitor.Read(), h.sourceMonitor.AutoRatio(),
				h.activeSettings.sourceResolution == "auto"));
			if (h.activeSettings.debandVp != "off" && !h.debandLogged.empty())
			{
				std::string line = h.debandLogged;
				if (h.debander.HeldFrames() > 0)
					line += "; held along the vectors (" + std::to_string(h.debander.HeldFrames()) + " frames)";
				descriptions.push_back(line);
			}
			// detail_pulse_stabilise / texture_restore: one line each while they run.
			if (h.detailPulseRuns && !h.detailPulseLogged.empty())
				descriptions.push_back(h.detailPulseLogged);
			if (h.textureRestoreRuns && !h.textureRestoreLogged.empty())
				descriptions.push_back(h.textureRestoreLogged);
			std::lock_guard<std::mutex> guard(h.shaderStatusMutex);
			h.activeCustomShaders = descriptions;
		}

		NlsMaskMapping::Mapping KnownNlsMapping(const ConfiguredShaderRule& nlsRule,
			const std::map<std::string, std::string>& fixedParams,
			double stretchRatio, bool verticalWarp)
		{
			const std::string file = nlsRule.filename;
			const size_t slash = file.find_last_of("\\/");
			const std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
			const bool plus = _stricmp(name.c_str(), "NLSPlus.glsl") == 0;
			if (!plus && _stricmp(name.c_str(), "NLS.glsl") != 0)
				return NlsMaskMapping::Mapping();
			return NlsMaskMapping::FromParameters(fixedParams, stretchRatio, verticalWarp, plus);
		}

		void SetNlsSampling(const struct pl_hook* hook, bool catmull)
		{
			for (int index = 0; hook && index < hook->num_parameters; ++index)
			{
				const struct pl_hook_par& parameter = hook->parameters[index];
				if (parameter.name && parameter.data && parameter.type == PL_VAR_FLOAT &&
					std::strcmp(parameter.name, "sampling") == 0)
					parameter.data->f = catmull ? 1.0f : 0.0f;
			}
		}
	}
}
