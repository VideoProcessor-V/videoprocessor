// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Renderer-host shader, fallback, NLS, display-LUT reload, calibration-pattern present, and
// entrance-wiring helpers moved out of LibplaceboVideoRenderer.cpp::Impl and
// CalibrationPresentMethods.inl into a compiled VP-V unit.

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/FallbackStatus.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionMonitor.h>
#include <VP-V/modules/output/calibration_lut/CalibrationLutHost.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/repair/deband/VpDebander.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>
#include <VP-V/segments/EntranceSegment.h>
#include <microsoft_directshow/MadVRShaderLoader.h>
#include <vprenderer/NlsMaskMapping.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace RendererHost
	{
		void EnsureRebaseDispatch(pl_dispatch& rebaseDispatch, pl_log log, pl_d3d11 d3d11);

		void TagSlotHooks(const std::vector<const pl_hook*>& frameHooks,
			std::vector<std::pair<const pl_hook*, PipelineSlots::Id>>& frameSlotHooks,
			size_t start, PipelineSlots::Id slot);

		void EngageFallback(FallbackStatus::Registry& fallbacks, const char* id, const std::string& reason);

		void ReleaseFallback(FallbackStatus::Registry& fallbacks, const char* id);

		void SyncFallback(FallbackStatus::Registry& fallbacks, const FallbackCatalog::Decision& decision);

		CustomHookChain::Loader BuildGlslHookLoader(
			const pl_d3d11& d3d11,
			bool (*readUserShader)(const std::string&, std::string&, std::string&, std::string&));

		void MaybeReloadDisplayLut(CalibrationLutHost::Adapter& calibrationHost,
			const std::string& displayLutPath, bool calibrationLutEnabled,
			const std::function<void()>& reloadDisplayLut);

		bool PresentCalibrationPattern(CalibrationLutHost::Adapter& calibrationHost,
			pl_d3d11 d3d11, IDXGISwapChain1* vpOwnedSwapchain, pl_swapchain swapchain,
			DXGI_FORMAT negotiatedSwapchainFormat, bool outputResourcesRetired,
			const std::function<bool(ID3D11Texture2D*, std::vector<uint32_t>&, D3D11_TEXTURE2D_DESC&, std::string&)>& readR10,
			FallbackStatus::Registry& fallbacks);

		bool IsSigmaDeltaRule(const ConfiguredShaderRule& rule);

		std::vector<ConfiguredShaderRule> TemporalDitherSelection(
			const std::vector<ConfiguredShaderRule>& requested, const std::string& mode);

		struct CustomShaderStatusHost
		{
			CustomHookChain::Chain& customHooks;
			bool lowLatency = false;
			const VpvRenderer::Fields& activeSettings;
			const AdaptiveDownscale::Module& adaptiveDownscale;
			bool adaptiveNoSsimLogged = false;
			double bitrateWeightScale = 1.0;
			const AdaptiveDownscaleStage::Stage& adaptiveStage;
			const SourceResolutionMonitor::Monitor& sourceMonitor;
			const std::string& debandLogged;
			const VpDebander::Module& debander;
			bool detailPulseRuns = false;
			const std::string& detailPulseLogged;
			bool textureRestoreRuns = false;
			const std::string& textureRestoreLogged;
			std::mutex& shaderStatusMutex;
			std::vector<std::string>& activeCustomShaders;
		};

		void PublishCustomShaderStatus(CustomShaderStatusHost& h);

		NlsMaskMapping::Mapping KnownNlsMapping(const ConfiguredShaderRule& nlsRule,
			const std::map<std::string, std::string>& fixedParams,
			double stretchRatio, bool verticalWarp);

		void SetNlsSampling(const struct pl_hook* hook, bool catmull);
	}
}
