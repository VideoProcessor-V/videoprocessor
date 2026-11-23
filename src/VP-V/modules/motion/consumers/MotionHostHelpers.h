// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Renderer-host motion, de-judder, resync, and sigma-delta vector-source helpers moved out of
// LibplaceboVideoRenderer.cpp::Impl and DitherVectorSourceHost.inl into a compiled VP-V unit.

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SlotRegistry.h>
#include <VP-V/modules/control/config/VpvRendererFields.h>
#include <VP-V/modules/input/signal/ResyncExtension.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include <VP-V/modules/motion/consumers/DejudderAhead.h>
#include <VP-V/modules/motion/consumers/DejudderCameraBindings.h>
#include <VP-V/modules/motion/consumers/DejudderExtrasPresent.h>
#include <VP-V/modules/motion/consumers/IntentMotionState.h>
#include <VP-V/modules/motion/consumers/RenderedWarp.h>
#include <VP-V/modules/motion/estimation/MotionValidity.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/matcher/DejudderWarp.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/repair/deband/VpDebander.h>
#include <VP-V/modules/repair/texture_restore/DetailPulse.h>
#include <VP-V/modules/repair/texture_restore/TextureRestore.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscale.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/renderer.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace Vpv
{
	namespace MotionHost
	{
		bool MotionVectorsRun(const PipelineSlots::Registry& pipelineSlots);

		bool DitherVectorsSupported(const PipelineSlots::Registry& pipelineSlots);

		struct VectorDemandHost
		{
			const VpvRenderer::Fields& activeSettings;
			const PipelineSlots::Registry& pipelineSlots;
			bool lowLatency = false;
			int queueAheadCapacity = 0;
			bool smoothMotionEngine = false;
			bool temporalActive = false;
			bool temporalMixing = false;
			bool sigmaDeltaRanLast = false;
			bool ditherVectorsSupported = false;
			bool adaptiveGrainMapWanted = false;
			bool entranceOnce = false;
			bool motionIntentActive = false;
		};

		VectorDemand::Input BuildVectorDemandInput(const VectorDemandHost& h, bool interlaced);

		bool SameGeometry(const DejudderRendered::Geometry& a, const DejudderRendered::Geometry& b);

		float AdaptiveFallbackFramePeriod(double customHookRefreshHz);

		::Dejudder::Settings RenderedWarpSettings(const ::Dejudder::MotionWarp& dejudder, int outputHeight);

		double SourceRateHz(double pulldownFilmHz, double displayModeRefreshHz);

		void RequestMotionInvalidate(MotionValidity::State& motionValidity,
			VpFrameAnalysis::Host& frameAnalysis, MotionValidity::Cause cause);

		void ApplyResyncLibplaceboFlush(VpResync::Shadow& resync, VpResync::Drive& resyncDrive,
			pl_renderer renderer, bool originalPathway, unsigned long long& flushCount,
			uint64_t sourceSequence, const std::function<void()>& clearTemporal);

		struct MotionInvalidateHost
		{
			bool originalPathway = false;
			VpResync::Shadow& resync;
			VpMotion::VectorEngine& vectorEngine;
			std::unique_ptr<::Dejudder::MotionWarp>& dejudder;
			::Dejudder::AheadSchedule& dejudderAhead;
			Dejudder::ExtrasState& dejudderExtras;
			uint64_t& dejudderEstimatedSequence;
			uint64_t& dejudderEstimatedEpoch;
			uint64_t& dejudderLiveFieldFrom;
			bool& motionEngineWarmPending;
			AdaptiveDownscale::Module& adaptiveDownscale;
			VpDebander::Module& debander;
			DetailPulse::Module& detailPulse;
			TextureRestore::Module& textureRestore;
			std::function<void()> endSnapshotLease;
			std::function<void()> releaseDejudder;
			std::function<void()> clearAheadPayload;
		};

		void ApplyMotionInvalidate(MotionInvalidateHost& h);

		struct DitherVectorsHost
		{
			const void* host = nullptr;
			pl_log log = nullptr;
			pl_d3d11 d3d11 = nullptr;
			const VpvRenderer::Fields& activeSettings;
			const PipelineSlots::Registry& pipelineSlots;
			CustomHookChain::Chain& customHooks;
			const VectorDemand::Decision& vectorDemand;
			VpMotion::VectorEngine& vectorEngine;
			MotionValidity::State& motionValidity;
			Dejudder::ExtrasState& dejudderExtras;
			std::unique_ptr<::Dejudder::MotionWarp>& dejudder;
			uint64_t dejudderEstimatedEpoch = 0;
			bool interpolating = false;
			bool vpTimelineActive = false;
			std::function<bool(uint64_t)> liveFieldFor;
			std::function<void(const FallbackCatalog::Decision&)> syncFallback;
		};

		void BuildSigmaDeltaVectorsWithoutExtras(DitherVectorsHost& h, uint64_t sourceSequence, uint64_t nextSourceSequence);

		void PrepareDejudderCamera(Dejudder::CameraBindings& bindings, const void* host, uint64_t sourceSequence, float phaseT);

		void AppendDejudderStatusExtras(std::string& text, bool pulldown, uint32_t pulldownCadence, double pulldownFilmHz,
			unsigned long long pulldownSkipped, bool occlusionMask, uint64_t maskUsedTick, uint64_t now,
			const std::string& maskState, const Dejudder::CameraBindings& cameraBindings);
	}
}
