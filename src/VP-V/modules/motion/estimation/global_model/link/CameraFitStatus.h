// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K1-6): the visible state of the fit runner. Atomic counters (any thread writes), the FallbackCatalog
// ids camera.no_model.{late,ambiguous,timing,readback,slip,depth0}, and the status lines. Never log-only: CollectStatus
// of the link returns the lines; the host shows them with the other fallbacks.
// "Ambiguous" engages after 0.5 s of consecutive Ambiguous records, measured in the records' own pairSeconds (never a
// frame count), and releases automatically on the first record that is not Ambiguous.
// Pure, header-only, C++14, deterministic.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/PhotometricWiring.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace CameraLink
{
	constexpr const char* kIdNoModelLate = "camera.no_model.late";
	constexpr const char* kIdNoModelAmbiguous = "camera.no_model.ambiguous";
	constexpr const char* kIdNoModelTiming = "camera.no_model.timing";
	constexpr const char* kIdNoModelReadback = "camera.no_model.readback";
	constexpr const char* kIdNoModelSlip = "camera.no_model.slip";
	constexpr const char* kIdNoModelDepth0 = "camera.no_model.depth0";
	constexpr const char* kIdNoModelQueue = "camera.no_model.queue";
	// R3 (photometric refinement): engaged while it never ran on a fitted pair (no luma / resolution rule / degenerate input), and while any
	// PhotometricRefineGpu failure counter is non-zero. Both reach the Ctrl+I lines, the one-time notice, Config status and Ctrl+Alt+D.
	constexpr const char* kIdPhotometricNotRunning = "camera.photometric.not_running";
	constexpr const char* kIdPhotometricGpuFailed = "camera.photometric.gpu_failed";

	constexpr double kAmbiguousEngageSeconds = 0.5;

	// F2: the camera fit reads only the optical-flow seed (IVectorEngine::SeedFieldOf); without one there is no fit (never the
	// guided live field instead). Shown as the camera_fit fallback entry ("camera_fit: " + this).
	constexpr const char* kSeedAbsentText = "camera fit needs the optical-flow seed (engine off or matcher-only)";
	// ... and when the live pair had no seed because its searches were skipped (camera guidance: a cut / flash): that pair only.
	constexpr const char* kSeedSkippedText = "camera fit skipped this pair (cut: no seed)";

	struct FitCounters
	{
		std::atomic<uint64_t> submitted{ 0 };        // jobs accepted by the worker queue
		std::atomic<uint64_t> queueFull{ 0 };        // jobs dropped: the bounded queue was full (visible)
		std::atomic<uint64_t> fitted{ 0 };
		std::atomic<uint64_t> published{ 0 };
		std::atomic<uint64_t> late{ 0 };             // fitted, not published (seal first / claim stale / not writable / no inputs)
		std::atomic<uint64_t> notFittedSlip{ 0 };
		std::atomic<uint64_t> timingUnknown{ 0 };    // pairSeconds 0 -> NoInput
		std::atomic<uint64_t> readbackDropped{ 0 };  // ring full or copy failed (no field for the pair)
		std::atomic<uint64_t> depth0{ 0 };           // seals with no lane depth (no look-ahead): P7 had no chance
		std::atomic<uint64_t> sealedOk{ 0 };
		std::atomic<uint64_t> sealedLate{ 0 };       // frame sealed without a published P7
		std::atomic<uint64_t> unusable{ 0 };         // published records outside the usable set
		std::atomic<uint64_t> resets{ 0 };           // fitter resets consumed by the worker
		std::atomic<uint64_t> relatches{ 0 };
		std::atomic<uint64_t> staleLane{ 0 };        // posts REFUSED: the job's lane generation is not the worker's (the lane graph was rebuilt)
		std::atomic<uint64_t> rebound{ 0 };          // times the worker took a new lane lease (a rebuild was noticed)
		std::atomic<uint64_t> fieldUndersized{ 0 };  // fit field larger than its product's size (gs.dims): NOT published, P8 gets no labels for that frame
		std::atomic<uint64_t> fieldNotPublished{ 0 };// fit field product not published for another reason (no storage / claim / pins / rebuild)
		std::atomic<uint64_t> guidedRefused{ 0 };    // F2: copies of the guided live field refused by SubmitField (never fitted)
		std::atomic<uint64_t> seedAbsent{ 0 };       // F2: engine pairs without an optical-flow seed (no fit; camera_fit fallback)
		// AHEAD FIT (camera_ahead_fit; all stay 0 when it is off). On time = P7 sealed with the frame in the on-time target;
		// bridged = sealed by the lag bridge (N + kSealLag) instead: an ahead fit that missed its frame's seal, or a frame fitted from the
		// seed because no ahead pair existed.
		std::atomic<uint64_t> aheadSubmitted{ 0 };   // fits submitted from the ahead pair (FlowOf)
		std::atomic<uint64_t> aheadOnTime{ 0 };      // ahead fits published into the on-time target before their frame's seal
		std::atomic<uint64_t> aheadBridged{ 0 };     // ahead fits published into the lag bridge (missed the on-time seal, or no target)
		std::atomic<uint64_t> aheadLate{ 0 };        // ahead fits published nowhere (late even for the bridge, or the park overflowed)
		std::atomic<uint64_t> aheadSeedFallback{ 0 };   // frames fitted from the seed while the ahead fit is on (no ahead pair)
		std::atomic<uint64_t> aheadNoPair{ 0 };      // FlowOf had no pair for a frame that needed one
		std::atomic<uint64_t> aheadIncomplete{ 0 };  // the pair had no backward vectors or costs (the engine trust cannot be computed)
		std::atomic<uint64_t> aheadLayout{ 0 };      // the pair's layout did not cover the camera's source grid
		std::atomic<uint64_t> onTimeSealed{ 0 };     // frames whose P7 the on-time seal accepted (render thread)
		std::atomic<uint64_t> onTimeRejected{ 0 };   // on-time publishes the seal rejected (a resync after the publish): no P7 for the frame
		std::atomic<uint64_t> staleAtUse{ 0 };       // records refused at consumption: the frame's stamp moved after the publish
		std::atomic<uint64_t> p2RefusedClaim{ 0 };   // G-A3: ahead fits refused at the claim (the P2 epoch moved after FlowOf)
		std::atomic<uint64_t> p2RefusedSeal{ 0 };    // ... rejected at the on-time seal
		std::atomic<uint64_t> p2RefusedUse{ 0 };     // ... refused at a consumer read
		std::atomic<uint64_t> aheadEpochRace{ 0 };   // G-A3: FlowOf answered while the vectors-valid epoch moved (refused at the copy)
		// R3 (photometric refinement of the chosen model; CPU reference). One counter per outcome: nothing is skipped silently.
		std::atomic<uint64_t> photoRefined{ 0 }, photoFellBack{ 0 }, photoSkippedNoLuma{ 0 }, photoSkippedResolution{ 0 }, photoSkippedOther{ 0 };
		// PhotometricRefineGpu failure counters (Unavailable / Limit / Ensure / Readback). The GPU form is not driven by the camera worker
		// (it owns no device), so they read 0 and the status says "cpu reference"; a device-owning driver adds into them.
		std::atomic<uint64_t> photoGpuUnavailable{ 0 }, photoGpuLimit{ 0 }, photoGpuEnsure{ 0 }, photoGpuReadback{ 0 };
		void NotePhotometric(CameraModel::PhotometricOutcome o)
		{
			using O = CameraModel::PhotometricOutcome;
			switch (o)
			{
			case O::Refined: photoRefined.fetch_add(1, std::memory_order_relaxed); break;
			case O::FellBack: photoFellBack.fetch_add(1, std::memory_order_relaxed); break;
			case O::SkippedNoLuma: photoSkippedNoLuma.fetch_add(1, std::memory_order_relaxed); break;
			case O::SkippedResolution: photoSkippedResolution.fetch_add(1, std::memory_order_relaxed); break;
			case O::SkippedOther: photoSkippedOther.fetch_add(1, std::memory_order_relaxed); break;
			default: break;
			}
		}
	};

	// AHEAD FIT: the visible state (Ctrl+I line, Config status, Ctrl+Alt+D). `target` says where an on-time result can go ("" = none).
	inline std::string AheadStatusLine(const FitCounters& c, const std::string& target)
	{
		char b[800];
		std::snprintf(b, sizeof(b), "camera ahead fit: %llu on time, %llu bridged (%llu ahead fits late for the on-time seal, %llu from the seed: no ahead pair), "
			"%llu late, %llu rejected after the publish (resync), %llu refused at use; no pair %llu, incomplete %llu, layout %llu; "
			"P2 provenance refused at copy / claim / seal / use %llu / %llu / %llu / %llu; on-time target: %s",
			(unsigned long long)c.onTimeSealed.load(), (unsigned long long)(c.aheadBridged.load() + c.aheadSeedFallback.load()),
			(unsigned long long)c.aheadBridged.load(), (unsigned long long)c.aheadSeedFallback.load(), (unsigned long long)c.aheadLate.load(),
			(unsigned long long)c.onTimeRejected.load(), (unsigned long long)c.staleAtUse.load(), (unsigned long long)c.aheadNoPair.load(),
			(unsigned long long)c.aheadIncomplete.load(), (unsigned long long)c.aheadLayout.load(), (unsigned long long)c.aheadEpochRace.load(),
			(unsigned long long)c.p2RefusedClaim.load(),
			(unsigned long long)c.p2RefusedSeal.load(), (unsigned long long)c.p2RefusedUse.load(), target.empty() ? "none" : target.c_str());
		return b;
	}

	// Counts consecutive Ambiguous measured seconds.
	class AmbiguousTracker
	{
	public:
		// Returns true while engaged.
		bool Observe(CameraModel::Reason reason, double pairSeconds)
		{
			if (reason == CameraModel::Reason::Ambiguous)
			{
				m_seconds += pairSeconds > 0 ? pairSeconds : 0;
				if (m_seconds >= kAmbiguousEngageSeconds)
					m_engaged = true;
			}
			else
			{
				m_seconds = 0;
				m_engaged = false;
			}
			return m_engaged;
		}
		bool Engaged() const { return m_engaged; }
		double Seconds() const { return m_seconds; }
		void Reset() { m_seconds = 0; m_engaged = false; }

	private:
		double m_seconds = 0;
		bool m_engaged = false;
	};

	// The newest fitted record, for the status line (K3). Written by the camera worker under the link's status mutex.
	struct LastFitSummary
	{
		bool have = false;
		CameraModel::ModelType type = CameraModel::ModelType::None;
		double confidence = 0;
		double fitMs = 0;               // the fit's own cost (CameraMotion::costMicros)
		CameraModel::Reason reason = CameraModel::Reason::NoInput;
		uint8_t verdictFlags = 0;
		bool published = false;         // the record reached its P7 slot before the seal
	};

	// The facts the decisions are built from (a snapshot).
	struct StatusFacts
	{
		bool graphHasP7 = false;
		bool laneDepthZero = false;
		bool lateNow = false;           // the last sealed frame had no published P7
		bool timingUnknownNow = false;
		bool readbackFailedNow = false;
		bool slipNow = false;
		bool queueFullNow = false;      // the bounded job queue dropped a pair
		bool ambiguousNow = false;
		double ambiguousShare = 0;      // share of recent records Ambiguous (for the line)
	};

	inline std::vector<FallbackCatalog::Decision> BuildDecisions(const StatusFacts& f)
	{
		using namespace FallbackCatalog;
		std::vector<Decision> d;
		auto add = [&](const char* id, bool on, const char* why) { d.push_back(on && f.graphHasP7 ? Engaged(id, why) : Released(id)); };
		add(kIdNoModelLate, f.lateNow, "camera: no model (late): the fit was not published before the frame; consumers keep their measured values");
		char amb[128];
		std::snprintf(amb, sizeof(amb), "camera: no dominant motion (ambiguous), model not used, %.0f%% of frames", f.ambiguousShare * 100.0);
		add(kIdNoModelAmbiguous, f.ambiguousNow, amb);
		add(kIdNoModelTiming, f.timingUnknownNow, "camera: no model (timing unknown): the pair duration could not be derived");
		add(kIdNoModelReadback, f.readbackFailedNow, "camera: no model (readback): the field copy was not ready");
		add(kIdNoModelSlip, f.slipNow, "camera: no model (slip): the pair spans a drop, repeat or resync");
		add(kIdNoModelQueue, f.queueFullNow, "camera: no model (queue): the fit queue was full and a pair was dropped; the next fit restarts its history");
		add(kIdNoModelDepth0, f.laneDepthZero, "camera: no model (no look-ahead depth): the fit cannot run ahead");
		return d;
	}

	inline unsigned long long PhotometricGpuFailures(const FitCounters& c)
	{
		return c.photoGpuUnavailable.load() + c.photoGpuLimit.load() + c.photoGpuEnsure.load() + c.photoGpuReadback.load();
	}
	// R3 decisions from the counters (cumulative: "never ran" = pairs fitted, none refined or verified-and-fallen-back).
	inline std::vector<FallbackCatalog::Decision> PhotometricDecisions(const FitCounters& c, bool graphHasP7)
	{
		using namespace FallbackCatalog;
		std::vector<Decision> d;
		const unsigned long long ran = c.photoRefined.load() + c.photoFellBack.load();
		const unsigned long long skipped = c.photoSkippedNoLuma.load() + c.photoSkippedResolution.load() + c.photoSkippedOther.load();
		const bool notRunning = graphHasP7 && ran == 0 && skipped > 0;
		d.push_back(notRunning ? Engaged(kIdPhotometricNotRunning,
			"camera: photometric refine not running (no luma pair, the resolution rule, or a degenerate input): the vector model is used unrefined")
			: Released(kIdPhotometricNotRunning));
		char b[200];
		std::snprintf(b, sizeof(b), "camera: photometric refine GPU failures: %llu unavailable, %llu limit, %llu ensure, %llu readback",
			(unsigned long long)c.photoGpuUnavailable.load(), (unsigned long long)c.photoGpuLimit.load(),
			(unsigned long long)c.photoGpuEnsure.load(), (unsigned long long)c.photoGpuReadback.load());
		d.push_back(graphHasP7 && PhotometricGpuFailures(c) > 0 ? Engaged(kIdPhotometricGpuFailed, b) : Released(kIdPhotometricGpuFailed));
		return d;
	}
	// The R3 status line (always shown while the camera link is active): outcomes and the GPU failure counters; the path is the CPU reference.
	inline std::string PhotometricStatusLine(const FitCounters& c)
	{
		char b[400];
		std::snprintf(b, sizeof(b), "camera photometric refine (cpu reference): %llu refined, %llu fell back, %llu no luma, %llu resolution rule, %llu other; gpu failures %llu unavailable, %llu limit, %llu ensure, %llu readback",
			(unsigned long long)c.photoRefined.load(), (unsigned long long)c.photoFellBack.load(), (unsigned long long)c.photoSkippedNoLuma.load(),
			(unsigned long long)c.photoSkippedResolution.load(), (unsigned long long)c.photoSkippedOther.load(),
			(unsigned long long)c.photoGpuUnavailable.load(), (unsigned long long)c.photoGpuLimit.load(),
			(unsigned long long)c.photoGpuEnsure.load(), (unsigned long long)c.photoGpuReadback.load());
		return b;
	}

	// "camera: fits 120, published 118, late 2, unusable 3, slip 0, queue full 0"
	inline std::string StatusLine(const FitCounters& c)
	{
		char b[640];
		std::snprintf(b, sizeof(b), "camera fit: %llu fitted, %llu published, %llu late, %llu not used, %llu slip, %llu timing unknown, %llu queue full, %llu readback dropped, %llu stale lane (refused after a graph rebuild), %llu lane rebinds, %llu fit field too big for its product (no labels for those frames), %llu fit field not published",
			(unsigned long long)c.fitted.load(), (unsigned long long)c.published.load(), (unsigned long long)c.late.load(),
			(unsigned long long)c.unusable.load(), (unsigned long long)c.notFittedSlip.load(),
			(unsigned long long)c.timingUnknown.load(), (unsigned long long)c.queueFull.load(),
			(unsigned long long)c.readbackDropped.load(), (unsigned long long)c.staleLane.load(), (unsigned long long)c.rebound.load(),
			(unsigned long long)c.fieldUndersized.load(), (unsigned long long)c.fieldNotPublished.load());
		return b;
	}
}
