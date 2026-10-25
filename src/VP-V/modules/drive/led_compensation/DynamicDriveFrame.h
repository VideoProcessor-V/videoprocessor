#pragma once

// WP-15 (integration batch 1): the Dynamic Drive host adapter, render-thread side.
//
// The renderer's Impl owns one DynamicDriveFrame. It holds what the render path used to keep
// for the LED drive (the module pointer the outer host publishes, the measurement hook, the
// LED compensation hook chain, the hook parameters, the once-only log latches) and does the
// per-frame work that used to sit inline in the render function:
//
//   PrepareFrame     gain in: BeginFrame / BeginFrameAhead, the look-ahead's raw transfer,
//                    the measurement geometry (active picture, peak boxes, picture-only masks,
//                    grain map), then the measurement and compensation hooks with this
//                    frame's parameters, and (module findings 2026-10-24 item 1) whether the
//                    compensation runs this frame: compensate holds full drive while nothing
//                    compensates, visibly ("Fallbacks engaged: LED drive ...");
//   OnHookErrors     libplacebo's disabled hooks (the compensation chain, the measurement pass);
//   CollectMeasurements  stats out: finished readbacks to the module.
//
// The host keeps one call per hook point (docs/design/wp15-led-callsites.md). Pixel work stays
// in the S2 stages (LedMeasurementHook, "LED compensation.glsl"); nothing here touches pixels.
// Threading: render thread, except the DynamicDriveShared calls (Publish*, Module,
// WantedLookaheadFrames), which any thread may make (atomics, as before).
// VP-only (not upstreamable).

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/core/PipelineSlots.h>
#include <VP-V/modules/drive/led_compensation/DynamicDriveHost.h>
#include <VP-V/modules/drive/led_measure/LedMeasurementHook.h>
#include <vprenderer/CustomHookChain.h>
#include <vprenderer/HdrPeakAnalysisCrop.h>
#include <vprenderer/NlsMaskMapping.h>
#include <QueuedFrameStatistics.h>
#include <SubtitleBoxDetector.h>
#include <SubtitleCutPaste.h>

#include <libplacebo/renderer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace LedDrive
{
	class LedDriveModule;

	// The facts of one frame the per-frame work reads; filled by the host from its locals.
	struct DynamicDriveFrameInputs
	{
		const pl_frame* image = nullptr;           // the frame as rendered (colour, crop)
		const pl_frame* target = nullptr;          // this frame's target (colour, shown crop)
		const pl_frame* screen = nullptr;          // the whole output (the script's screenshot)
		const pl_render_params* params = nullptr;  // colour-map parameters
		const CustomHookChain::TemplateValues* hookTemplates = nullptr;
		const std::string* sourceRange = nullptr;  // "full" / "limited" / anything else = unknown
		// The gamma this frame's output is encoded for (the host's LedDisplayGamma of the target
		// transfer): the gamma boost's base (delta 4), not the projector's compensation_eotf.
		double displayGamma = 2.2;
		double captureRateHz = 0.0;                // measured (film rate under a pulldown lock)
		double nominalSourceRateHz = 0.0;
		// The render-ahead schedule's next vsync in QPC seconds, when render-ahead plans this
		// frame (renderAheadActive); the gain and drive then use the refresh it lands on.
		bool renderAheadActive = false;
		double renderAheadNextVsyncSeconds = 0.0;
		uint64_t sourceSequence = 0;
		const HdrPeakAnalysisCrop::TrustedPicture* trustedPicture = nullptr;
		// led_picture_only: the detected subtitle and a moved subtitle's places (source raster).
		const SubtitleBoxResult* subtitle = nullptr;
		int sourceWidth = 0;
		int sourceHeight = 0;
		SubtitleBoxPadding subtitlePadding{};
		bool subtitleComposed = false;
		const SubtitleCutPasteGeometry* relocatedSubtitle = nullptr;
		// NLS warps the picture before the scaler (masks follow its map when it is known).
		bool nlsSubmitted = false;
		std::function<NlsMaskMapping::Mapping()> nlsMapping;   // asked only for picture-only masks under NLS
		// Delta 8 R2: the adaptive downscaler's grain map when it was used this frame, else null.
		pl_tex grainMap = nullptr;
		float grainFloor = 0.0f;
		// Which LED slots the registry runs this frame.
		bool measureSlotRuns = false;
		bool compensationSlotRuns = false;
	};

	class DynamicDriveFrame : public DynamicDriveShared
	{
	public:
		// walkObserver: the Impl's statistics observer of the queued-frame walk (DynamicDriveShared);
		// fallbacks: the Impl's "Fallbacks engaged" registry; loader: the Impl's GLSL hook loader
		// (for "LED compensation.glsl").
		DynamicDriveFrame(std::shared_ptr<QueuedFrameStatistics::IObserver>& walkObserver,
			FallbackStatus::Registry& fallbacks, CustomHookChain::Loader loader);
		DynamicDriveFrame(const DynamicDriveFrame&) = delete;
		DynamicDriveFrame& operator=(const DynamicDriveFrame&) = delete;

		// Published by the outer host (any thread): see DynamicDriveShared.

		// --- render thread ---
		// The intent interpreter's LED limits; true when they reached a module (the caller then
		// records them as sent).
		bool SendIntentLimits(double shadowMinimumShare, double ambientBlackNits, double displayBlackNits);
		// Gain in and the S2 LED hooks for this frame, appended to `hooks`. tagSlot(start, id) is
		// the host's slot tagging (all hooks from `start` to the end belong to slot `id`).
		void PrepareFrame(const DynamicDriveFrameInputs& in, std::vector<const pl_hook*>& hooks,
			const std::function<void(size_t, PipelineSlots::Id)>& tagSlot);
		// libplacebo's disabled hooks after the render (pl_renderer_get_errors).
		void OnHookErrors(const uint64_t* disabledSignatures, int count);
		// Stats out: finished measurements (a few frames old; never waits).
		void CollectMeasurements();
		// The measurement pass (the de-judder extras leave it out).
		const pl_hook* MeasurementHookPointer() const { return measurement.Hook(); }
		// Renderer teardown / GPU reset: hooks released, the chain reconfigured on next use.
		void Release();

		// The picture-only masks (Bug hunt 2, led_picture_only): output-relative rectangles for
		// the GPU pass and source-raster rectangles for the look-ahead. Returns the count.
		static int PictureOnlyMasks(const DynamicDriveFrameInputs& in, MeasurementRect* masks,
			PixelRect* sourceMasks);
		// Module findings 2026-10-24 item 1: the reason the compensation cannot run this frame
		// (empty = it runs). collected: hooks the chain submitted; runtimeDisabled: libplacebo
		// disabled the chain's hook.
		static std::string CompensationUnavailable(bool slotRuns, size_t collected, bool runtimeDisabled);

		// Fallback ids this adapter engages in the host's registry.
		static constexpr const char* kMeasurementFallbackId = "LED measurement";
		static constexpr const char* kBoostFallbackId = "gamma boost";

	private:
		void Engage(const char* id, const std::string& reason);
		void ReleaseFallback(const char* id);
		void MergeModuleFallbacks(const std::shared_ptr<LedDriveModule>& led);

		FallbackStatus::Registry& fallbacks;
		MeasurementHook measurement;
		CustomHookChain::Chain compensation;
		bool compensationConfigured = false;
		bool compensationRuntimeDisabled = false;   // libplacebo disabled the chain's hook (until Release)
		uint64_t rawTransferSignature = 0;
		bool boostMissingLogged = false;
		bool measurementDisabledLogged = false;
		double lastLoggedBoostExponent = 1.0;
		int lastDropped = 0;
		int droppedGrowthFrames = 0;
	};
}
