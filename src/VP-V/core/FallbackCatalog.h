#pragma once

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// The "Fallbacks engaged" entries of the renderer's automatic downgrades, built in ONE place
// (module findings 2026-10-24, the silent-fallbacks batch; ADR-0001 no silent fallbacks).
//
// Why: the review of the blocked modules found the sigma-delta drop, BTB/WTW clipping, the
// debander failing, the optical-flow engine failing, open-loop presentation, a failed refresh
// switch and more reaching only the log. The rule "host = call sites only" keeps the renderer to
// one line per site: it passes the facts it already has to a builder here and hands the result
// to Sync(), which engages or releases the entry. The wording, the stable ids and the decision
// of WHEN an entry is engaged live here and are CPU-tested (src/VP-V/tests/FallbackCatalogTests.cpp).
//
// Each builder returns a Decision for exactly one id: engaged with a reason, or released. Ids are
// stable short names (the GUI's one-time notice keys on them); reasons say what runs instead.
// Header-only, pure, C++14 (no Windows, no clock): shared by the renderer DLL, the EXE and tests.
namespace FallbackCatalog
{
	struct Decision
	{
		std::string id;
		bool engaged = false;
		std::string reason;
	};

	inline Decision Released(const char* id) { Decision d; d.id = id; return d; }
	inline Decision Engaged(const char* id, const std::string& reason)
	{
		Decision d;
		d.id = id;
		d.engaged = true;
		d.reason = reason;
		return d;
	}

	// Applies a decision; true when the heading changed (the caller logs the new heading once).
	inline bool Sync(FallbackStatus::Registry& registry, const Decision& decision)
	{
		return decision.engaged ? registry.Engage(decision.id, decision.reason) : registry.Release(decision.id);
	}

	// ---- output.dither: dither need (dither_need = auto) ----------------------------------------
	// The per-region extra dither amplitude was asked for but an input is missing (grain split, region
	// precursors, light-level LUT, supported domain): the sigma-delta runs at its base amplitude.
	// `missing` is DitherNeed::MissingText; empty = nothing missing.
	inline Decision DitherNeed(bool requested, const std::string& missing)
	{
		if (!requested || missing.empty())
			return Released("dither need");
		return Engaged("dither need", "Dither need: unavailable (" + missing + "); sigma-delta at its base amplitude");
	}

	// ---- output.dither --------------------------------------------------------------------------
	// The sigma-delta was asked for (temporal_dither = sigma_delta, or the shader is in the list)
	// but does not run: libplacebo's own dither quantises the output instead.
	enum class DitherDrop
	{
		None,                // the sigma-delta runs
		UnsupportedDomain,   // the target is not an 8- or 10-bit code domain (hook removed)
		ShaderNotLoaded,     // the shader file did not load or parse
		DisabledByLibplacebo // libplacebo disabled the hook at runtime
	};
	inline Decision Dither(bool requested, DitherDrop drop, const std::string& detail = std::string())
	{
		if (!requested || drop == DitherDrop::None)
			return Released("dither");
		const char* why = drop == DitherDrop::UnsupportedDomain ? "not an 8- or 10-bit code domain" :
			drop == DitherDrop::ShaderNotLoaded ? "the shader did not load" : "disabled by libplacebo";
		return Engaged("dither", std::string("libplacebo dither; sigma-delta dropped: ") + why +
			(detail.empty() ? std::string() : ": " + detail));
	}

	// ---- colour.extended_range (ADR-0002) -------------------------------------------------------
	enum class BtbWtwClip
	{
		None,           // BTB and WTW both carried
		HookFailed,     // libplacebo disabled the carry hook (latched for the session)
		FrameMixing,    // a late restore cannot follow libplacebo's frame mix (ForTemporal)
		MixClipsBtb     // libplacebo smooth motion: mixed refreshes clip BTB (WTW kept)
	};
	inline Decision BtbWtw(bool preserveRequested, BtbWtwClip clip, const std::string& detail = std::string())
	{
		if (!preserveRequested || clip == BtbWtwClip::None)
			return Released("BTB/WTW");
		const char* why = clip == BtbWtwClip::HookFailed ? "clipped: the carry hook was disabled by libplacebo" :
			clip == BtbWtwClip::FrameMixing ? "clipped: libplacebo's frame mix" :
			"BTB clipped on mixed refreshes (libplacebo smooth motion; VP's motion engine keeps both)";
		return Engaged("BTB/WTW", std::string(why) + (detail.empty() ? std::string() : " (" + detail + ")"));
	}

	// ---- scaling.rebase -------------------------------------------------------------------------
	// Not a fallback: the factors every downstream stage uses (debander field / radius scale,
	// detail pulse, texture restore, adaptive upscale). A re-base that does not run (slot off in
	// pipeline_slots_off, or the stage paused) leaves the planes native, so the factors are 1.
	struct RebaseFactors { double x = 1.0; double y = 1.0; };
	inline RebaseFactors EffectiveRebaseFactors(bool rebaseRuns, double factorX, double factorY)
	{
		RebaseFactors f;
		if (rebaseRuns)
		{
			f.x = factorX > 0.0 ? factorX : 1.0;
			f.y = factorY > 0.0 ? factorY : 1.0;
		}
		return f;
	}
	// A failed re-base pauses for `pauseSeconds` (native frames); released when a pass succeeds.
	inline Decision RebasePaused(bool paused, double pauseSeconds, const std::string& error)
	{
		if (!paused)
			return Released("re-base");
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "%.0f s", pauseSeconds);
		return Engaged("re-base", std::string("paused after a failure: native frames for ") + buffer +
			(error.empty() ? std::string() : " (" + error + ")"));
	}
	// A failed re-base on the EXTRA path (the de-judder's extra pictures): the extra shows the native
	// frame. Same id as RebasePaused ("re-base"): the renderer's next RebasePaused sync (the failure
	// count moved, so a pause follows) takes the entry over and releases it with the pause.
	inline Decision RebaseExtraNative(const std::string& error)
	{
		return Engaged("re-base", std::string("extra picture: native frame") +
			(error.empty() ? std::string() : " (" + error + ")"));
	}
	// Detail pulse stabilise / texture restore: a failed pass renders the frame without it.
	inline Decision DetailPulseFailed(bool failed, const std::string& error)
	{
		return failed ? Engaged("detail pulse", "not applied" + (error.empty() ? std::string() : " (" + error + ")") +
			"; the frame is rendered without it") : Released("detail pulse");
	}
	inline Decision TextureRestoreFailed(bool failed, const std::string& error)
	{
		return failed ? Engaged("texture restore", "not applied" + (error.empty() ? std::string() : " (" + error + ")") +
			"; the frame is rendered without it") : Released("texture restore");
	}

	// Custom GLSL hooks that did not load (read, template, libplacebo rejection, no known
	// //!HOOK stage) or that libplacebo disabled at runtime; the picture renders without them.
	// Quantiser failures are reported by the dither entry instead. Lists at most 3 names.
	inline Decision CustomHooksNotLoaded(const std::vector<std::string>& names)
	{
		if (names.empty()) return Released("custom hooks");
		constexpr size_t kListed = 3;
		std::string list;
		for (size_t i = 0; i < names.size() && i < kListed; ++i)
			list += (i ? ", " : "") + names[i];
		if (names.size() > kListed)
			list += " +" + std::to_string(names.size() - kListed) + " more";
		return Engaged("custom hooks", "not loaded: " + list + " (the picture renders without them)");
	}

	// ---- repair.deband --------------------------------------------------------------------------
	// libplacebo's automatic deband yields to VP's debander (deband_vp_exclusive). Before this, a
	// FAILED VP debander left the picture with no deband at all; now libplacebo's deband stands in
	// for it, visibly. Only a failure is a fallback:
	//  - the repair.deband slot switched off (pipeline_slots_off, the Original pathway) is a
	//    choice: no substitute, no entry; libplacebo's deband follows the user's own settings as
	//    before (still yielded when deband_vp_exclusive says so);
	//  - Dolby Vision tunnel pictures: no substitute (feat/dv-l8 runs neither debander on them).
	struct DebandPlan
	{
		bool libplaceboYields = false;   // keep libplacebo's deband off for this frame (as configured)
		Decision fallback;
	};
	// vpRequested: libplacebo yields to VP by the settings (LibplaceboDebandYieldsToVp).
	inline DebandPlan PlanDeband(bool vpRequested, bool slotRuns, bool vpFailed, bool dvTunnelPicture,
		const std::string& error = std::string())
	{
		DebandPlan plan;
		plan.libplaceboYields = vpRequested;
		plan.fallback = Released("deband");
		if (vpRequested && slotRuns && vpFailed && !dvTunnelPicture)
		{
			plan.libplaceboYields = false;
			plan.fallback = Engaged("deband", "libplacebo deband (VP debander failed" +
				(error.empty() ? std::string() : ": " + error) + ")");
		}
		return plan;
	}

	// ---- motion.estimation ----------------------------------------------------------------------
	inline Decision Motion(bool opticalFlowRequested, bool engineFailed, bool aheadProducerStopped,
		const std::string& error = std::string())
	{
		if (!opticalFlowRequested || (!engineFailed && !aheadProducerStopped))
			return Released("motion");
		std::string reason = engineFailed ? "matcher only (optical flow failed" : "no optical-flow look-ahead (producer stopped";
		return Engaged("motion", reason + (error.empty() ? std::string() : ": " + error) + ")");
	}

	// ---- motion.estimation: the vector producer's restarts (bug hunt 35 #1) -----------------------
	// The look-ahead producer stopped after three failed jobs in a row and restarts itself with a
	// back-off (MotionAhead::ProducerRecovery). Engaged while it is wanted and waiting to restart,
	// for every vector consumer (the "motion" entry above covers only the de-judder's view).
	inline Decision VectorProducer(bool wanted, bool recovering, unsigned stops, const std::string& error = std::string())
	{
		if (!wanted || !recovering)
			return Released("vector producer");
		return Engaged("vector producer", "render-time motion estimate; the look-ahead stopped" +
			(error.empty() ? std::string() : ": " + error) + "; restarting with back-off (stop " + std::to_string(stops) + ")");
	}

	// ---- look-ahead workers (bug hunt 31 H3) ----------------------------------------------------
	// A look-ahead batch threw (the motion-ahead hand-off, the source-resolution look-ahead): its
	// frames went without that work. Engaged at the failure with the exception text, released after
	// `releaseAfterGood` batches in a row without one (the next batches retry on their own).
	struct LookAheadFailures
	{
		const char* id = "look-ahead";
		const char* without = "frames skipped";
		unsigned long long failures = 0;
		unsigned goodSince = 0;
		bool engaged = false;
		std::string what;
		LookAheadFailures(const char* idIn, const char* withoutIn) : id(idIn), without(withoutIn) {}
		Decision Note(bool failed, const std::string& whatIn = std::string(), unsigned releaseAfterGood = 8)
		{
			if (failed)
			{
				++failures;
				goodSince = 0;
				engaged = true;
				what = whatIn;
			}
			else if (engaged && ++goodSince >= releaseAfterGood)
				engaged = false;
			if (!engaged)
				return Released(id);
			return Engaged(id, std::string(without) + " (worker exception" + (what.empty() ? std::string() : ": " + what) +
				"); retrying each batch");
		}
	};
	inline LookAheadFailures MotionAheadWorker() { return LookAheadFailures("motion look-ahead", "no optical-flow look-ahead for some frames"); }
	inline LookAheadFailures SourceResolutionWorker() { return LookAheadFailures("source resolution look-ahead", "resolution judged on arrival only"); }

	// ---- control.diagnostics: diagnostic overrides (bug hunt 39 #6) -----------------------------
	// Test / debug switches set in the environment outside a test harness
	// (VpDiagnostics::OverridesToShow). Engaged while any is set; the names only, never values.
	inline Decision DiagnosticOverrides(const std::vector<std::string>& names)
	{
		if (names.empty())
			return Released("diagnostic override");
		std::string list;
		for (size_t i = 0; i < names.size(); ++i)
			list += (i ? ", " : "") + names[i];
		return Engaged("diagnostic override", "active: " + list);
	}

	// ---- motion.consumers -----------------------------------------------------------------------
	// A picture between frames could not be made on the GPU (the de-judder warp, the rendered warp
	// or the interpolation failed): the source frame is shown instead (the de-judder holds). Engaged
	// while such failures are recent (one id, so the heading changes only when it engages/releases),
	// released after holdMs without one (group D audit D3, owner rule vp-no-silent-fallbacks).
	inline bool IsMotionConsumerGpuFailure(const char* failure)
	{
		if (!failure) return false;
		const std::string f(failure);
		return f == "warp failed" || f == "rendered warp failed" || f == "interpolation failed";
	}
	struct MotionConsumerFailures
	{
		unsigned long long count = 0;
		unsigned long long lastFailureMs = 0;
		std::string what;
		// Every attempt (extras run, picture between frames): `failure` null or a non-GPU reason
		// (too late, no backbuffer: timing, not a failed pass) only re-evaluates the hold.
		Decision Note(unsigned long long nowMs, const char* failure, unsigned long long holdMs = 5000)
		{
			if (IsMotionConsumerGpuFailure(failure))
			{
				++count;
				lastFailureMs = nowMs;
				what = failure;
			}
			return Current(nowMs, holdMs);
		}
		Decision Current(unsigned long long nowMs, unsigned long long holdMs = 5000) const
		{
			if (count == 0 || nowMs - lastFailureMs >= holdMs)
				return Released("motion consumers");
			return Engaged("motion consumers", what + ": the source frame is shown instead");
		}
	};

	// ---- output.presentation --------------------------------------------------------------------
	// Without DXGI frame statistics render-ahead runs open loop on an estimated rate, and with
	// render-ahead off the de-judder holds every frame (judder) until a display rate is known.
	inline Decision Presentation(bool openLoop, bool dejudderHeldNoDisplayRate, const std::string& detail = std::string())
	{
		if (!openLoop && !dejudderHeldNoDisplayRate)
			return Released("presentation");
		std::string reason = openLoop ? "open loop: no display statistics, estimated rate" :
			"de-judder held: display rate not known";
		return Engaged("presentation", reason + (detail.empty() ? std::string() : " (" + detail + ")"));
	}

	// ---- output.presentation: the display refresh switch ----------------------------------------
	// A switch was wanted for the input rate but the display kept another rate.
	inline Decision RefreshRate(bool switchWanted, double inputHz, double keptHz, const std::string& why)
	{
		if (!switchWanted || !(inputHz > 0.0) || !(keptHz > 0.0))
			return Released("refresh rate");
		char buffer[96];
		std::snprintf(buffer, sizeof(buffer), "kept %.3f Hz for %.3f Hz input", keptHz, inputHz);
		return Engaged("refresh rate", std::string(buffer) + (why.empty() ? std::string() : ": " + why));
	}

	// ---- the handed-off log-only sites (module findings 2026-10-24, part 1 item 2) ---------------
	inline Decision CalibrationLutDropped(bool dropped, const std::string& error)
	{
		return dropped ? Engaged("calibration LUT", "dropped after a render error" +
			(error.empty() ? std::string() : ": " + error) + "; no projector LUT for this renderer") :
			Released("calibration LUT");
	}
	inline Decision CalibrationPatchFullField(bool fullField, const std::string& why)
	{
		return fullField ? Engaged("calibration patch", "full field (window unavailable" +
			(why.empty() ? std::string() : ": " + why) + "); LCD/OLED readings are affected") :
			Released("calibration patch");
	}
	inline Decision OutputRangeComposed(bool composed, const std::string& reason)
	{
		return composed ? Engaged("output range", "AUTO Limited unavailable: composed Full/sRGB swapchain" +
			(reason.empty() ? std::string() : " (" + reason + ")")) : Released("output range");
	}
	inline Decision OutputRequestRejected(bool rejected, const std::string& requested, const std::string& reason)
	{
		return rejected ? Engaged("output request", (requested.empty() ? std::string("the request") : requested) +
			" rejected: Full/sRGB" + (reason.empty() ? std::string() : " (" + reason + ")")) : Released("output request");
	}
	inline Decision GeometrySafeFit(bool safeFit)
	{
		return safeFit ? Engaged("geometry", "NLS safe fit (hook failure)") : Released("geometry");
	}
	enum class SubtitleLoss { None, CueDropped, JobsDropped, WorkerError };
	inline Decision SubtitleDetection(SubtitleLoss loss)
	{
		switch (loss)
		{
		case SubtitleLoss::CueDropped: return Engaged("subtitle detection", "worker late: cue dropped");
		case SubtitleLoss::JobsDropped: return Engaged("subtitle detection", "worker behind: measurement jobs dropped");
		case SubtitleLoss::WorkerError: return Engaged("subtitle detection", "worker error: measurement skipped");
		default: return Released("subtitle detection");
		}
	}
	inline Decision SdrToneShape(bool requested, bool applied, const std::string& why)
	{
		return requested && !applied ? Engaged("SDR tone shape", "not applied" +
			(why.empty() ? std::string() : " (" + why + ")")) : Released("SDR tone shape");
	}
	// Adaptive downscale: GPU failures leave plain SSIM (w = 1); a missing SSimDownscaler leaves
	// libplacebo's downscaler.
	inline Decision OutputScaling(bool adaptiveRequested, unsigned long long newFailures, bool ssimShaderMissing)
	{
		if (!adaptiveRequested || (newFailures == 0 && !ssimShaderMissing))
			return Released("output scaling");
		return Engaged("output scaling", ssimShaderMissing ?
			"libplacebo downscaler (SSimDownscaler not in the shader list)" :
			"plain SSIM (adaptive downscale failed on the GPU)");
	}
	inline Decision EarlyUpload(bool requested, bool open, const std::string& error)
	{
		return requested && !open ? Engaged("early upload", "the render upload" +
			(error.empty() ? std::string() : " (" + error + ")")) : Released("early upload");
	}

	// The early upload is open but frames still take the render upload: the render reached the
	// payload before the conversion thread started its upload (late), or the slot was missing /
	// refused (no free slot, failed). Noted once per early-eligible payload (its first draw) and
	// evaluated per window of windowMs: engaged while a window's render-upload share is above
	// kShare with at least kMinFrames such frames; released after a window at or below it, or
	// when no eligible payload arrived for two windows. Waits on a running upload are reported.
	struct EarlyUploadRenderUploads
	{
		static constexpr double kShare = 0.05;
		static constexpr unsigned long long kMinFrames = 2;
		unsigned long long windowStartMs = 0, lastNoteMs = 0;
		unsigned long long frames = 0, late = 0, noSlot = 0, waited = 0;   // the current window
		bool started = false;
		Decision last = Released("early upload frames");
		Decision Note(unsigned long long nowMs, bool isLate, bool isNoSlot, bool isWaited, unsigned long long windowMs = 2000)
		{
			if (!started)
			{
				started = true;
				windowStartMs = nowMs;
			}
			lastNoteMs = nowMs;
			++frames;
			late += isLate ? 1 : 0;
			noSlot += isNoSlot ? 1 : 0;
			waited += isWaited ? 1 : 0;
			if (nowMs - windowStartMs >= windowMs)
			{
				const unsigned long long render = late + noSlot;
				if (render >= kMinFrames && static_cast<double>(render) > kShare * static_cast<double>(frames))
				{
					char text[200];
					std::snprintf(text, sizeof(text), "%llu of %llu frames took the render upload (%llu before the early upload "
						"started, %llu no slot), %llu waited for a running upload", render, frames, late, noSlot, waited);
					last = Engaged("early upload frames", text);
				}
				else
					last = Released("early upload frames");
				windowStartMs = nowMs;
				frames = late = noSlot = waited = 0;
			}
			return last;
		}
		// No eligible payload for two windows (another route, paused, pool closed): released.
		Decision Current(unsigned long long nowMs, unsigned long long windowMs = 2000)
		{
			if (started && nowMs - lastNoteMs >= 2 * windowMs)
				*this = EarlyUploadRenderUploads();
			return last;
		}
	};
	// ---- motion.estimation: Vector engine on demand ---------------------------------------------
	// Engaged while a wanted consumer goes without vectors; the reason names each and why.
	inline Decision VectorEngine(const VectorDemand::Decision& demand)
	{
		if (demand.unserved == 0)
			return Released("vector_engine");
		return Engaged("vector_engine", VectorDemand::UnservedText(demand));
	}
	inline Decision VectorEngineProducer(bool sizeSkipSustained)
	{
		return sizeSkipSustained ? Engaged("vector_engine_producer",
			"Vector engine look-ahead stores nothing for this size (render-time estimate runs)") :
			Released("vector_engine_producer");
	}
	inline Decision VectorEngineGlobal(bool sustainedMiss)
	{
		return sustainedMiss ? Engaged("vector_engine_global",
			"source monitor fast-pan / intent without whole-frame motion (vector engine pair missed)") :
			Released("vector_engine_global");
	}
	// ---- core: the shared seal (FrameSeal / AnalysisLane external-seal mode) ----------------------
	// Engaged when the one-seal-owner wiring is wrong: a frame reached the lane without the shared seal
	// (it is NOT sealed twice; its analysis falls back to the in-render form), or a provider was added
	// after the first frame (its readers are not registered on the current era until the next one).
	inline Decision ResyncSeal(unsigned long long frameMisses, unsigned lateProviders, unsigned rejectedReaders = 0)
	{
		if (frameMisses == 0 && lateProviders == 0 && rejectedReaders == 0)
			return Released("resync_seal");
		std::string reason;
		if (frameMisses != 0)
			reason += std::to_string(frameMisses) + " frames reached the analysis lane without the shared seal";
		if (lateProviders != 0)
			reason += std::string(reason.empty() ? "" : "; ") + std::to_string(lateProviders) +
				" resync reader provider(s) added after the first frame (not registered on this era)";
		if (rejectedReaders != 0)
			reason += std::string(reason.empty() ? "" : "; ") + std::to_string(rejectedReaders) +
				" resync reader(s) refused by the Indicator (registered after its first frame: they never act)";
		return Engaged("resync_seal", reason);
	}

	// ---- input.signal: resync storm hold (G8 / D4) ----------------------------------------------
	// Engaged while the resync storm hold is in force (On mode only): more than 3 FULL resyncs in
	// 10 s hold lane T0, native re-base and the vector engine at safe settings.
	inline Decision ResyncStorm(bool held, int fullCount)
	{
		if (!held)
			return Released("resync_storm");
		return Engaged("resync_storm", "Input unstable: " + std::to_string(fullCount < 0 ? 0 : fullCount) +
			" resyncs in 10 s - processing held at safe settings");
	}

	// ---- skeleton analysis lane -----------------------------------------------------------------
	// The regulator reduced the lane's work (dropped products, forced live fills, T0 in the render,
	// pinned at the minimum, the optical-flow input reduced by `ofaStep` steps: 0 = full), or the lane is
	// not available for the source stats. The lists hold plain-language product names (the host maps ids
	// to names).
	inline std::string JoinNames(const std::vector<std::string>& names)
	{
		std::string out;
		for (size_t i = 0; i < names.size(); ++i)
			out += (i ? ", " : "") + names[i];
		return out;
	}
	inline Decision AnalysisLane(const std::vector<std::string>& dropped, const std::vector<std::string>& forcedLive,
		bool pinnedAtMinimum, bool t0InRender, unsigned ofaStep = 0)
	{
		if (dropped.empty() && forcedLive.empty() && !pinnedAtMinimum && !t0InRender && ofaStep == 0)
			return Released("analysis_lane");
		std::string reason;
		auto add = [&reason](const std::string& part) { reason += (reason.empty() ? std::string() : "; ") + part; };
		if (!dropped.empty())
			add("reduced: " + JoinNames(dropped) + " off (dropped)");
		if (!forcedLive.empty())
			add("forced live: " + JoinNames(forcedLive));
		if (t0InRender)
			add("core analysis runs in the render");
		if (ofaStep != 0)
			add("vector engine input reduced (step " + std::to_string(ofaStep) + ")");
		if (pinnedAtMinimum)
			add("analysis at minimum");
		return Engaged("analysis_lane", reason);
	}
	inline Decision AnalysisLaneDeterminism(unsigned long long missedBounds)
	{
		if (missedBounds == 0)
			return Released("analysis_lane_determinism");
		return Engaged("analysis_lane_determinism", "deterministic playback: " + std::to_string(missedBounds) +
			" frames missed the bounded wait (vector engine fallback used; this run is not reproducible)");
	}
	inline Decision SourceStatsWalk(bool laneRequested, bool laneUnavailable)
	{
		return laneRequested && laneUnavailable ? Engaged("source_stats_walk",
			"active-picture walk uses Bill's original walk (lane source stats unavailable)") :
			Released("source_stats_walk");
	}

	// ---- skeleton activation (modules/control/skeleton, modules/motion/estimation/global_model/link) ----
	// analysis_lane was asked for but the lane cannot run (yet): the reason says what it waits for
	// (resync_event=on for the one shared seal, frame_analysis_store=on for the source frames, ...).
	inline Decision SkeletonLane(const std::string& waiting)
	{
		return waiting.empty() ? Released("skeleton_lane") : Engaged("skeleton_lane", waiting);
	}
	// Products the lane's graph holds that nothing can make yet (no producer, no live fill): they are declined,
	// their consumers keep their own computation.
	inline Decision SkeletonProductsDeclined(const std::string& products, const std::string& why)
	{
		return products.empty() ? Released("skeleton_products") : Engaged("skeleton_products", products + " declined: " + why);
	}
	// A [vpv.skeleton] key that could not be read (kept at its default).
	inline Decision SkeletonKeys(const std::string& problems)
	{
		return problems.empty() ? Released("skeleton_settings") : Engaged("skeleton_settings", problems);
	}
	// camera_link = report was asked for but the camera link cannot report (no store, no seal, ...).
	inline Decision CameraLinkReport(const std::string& problem)
	{
		return problem.empty() ? Released("camera_link") : Engaged("camera_link", problem);
	}

	// ---- product graph (core/ProductCatalog.h) ----------------------------------------------------
	// Every consumer of a shared product states what it does when the product is missing, stale, shed or its unit
	// version differs from the one the reader expects. These are the ids the catalog rows name; the GUI's one-time
	// notice keys on them like any other entry. `text` says what runs instead.
	struct ProductEntry
	{
		const char* id;
		const char* text;
	};
	inline const ProductEntry* ProductEntries(size_t* count)
	{
		static const ProductEntry kEntries[] = {
			{ "product.unit_version", "a shared product was made by another unit version: the reader runs its own reduced form" },
			{ "product.detail_pulse", "detail pulse measures its own bands and tiles (shared split / block statistics unavailable)" },
			{ "product.texture_restore", "texture restore measures its own bands and noise (shared split / block statistics unavailable)" },
			{ "product.debander", "debander uses its own half split and tensor (shared split / block statistics unavailable)" },
			{ "product.adaptive_downscale", "adaptive downscale computes its own edge, grain and cut terms (shared products unavailable)" },
			{ "product.sigma_delta", "output dither runs its own vector search (shared analysis field unavailable)" },
			{ "product.led_lookahead", "LED look-ahead measures the queued frames itself (shared source statistics unavailable)" },
			{ "product.hdr_inference", "HDR inference reads its own grid (shared source statistics unavailable)" },
			{ "product.source_resolution", "source-resolution monitor judges its own cuts (shared source statistics unavailable)" },
			{ "product.source_id", "source identification reads the monitor snapshot only (shared tiles unavailable)" },
			{ "product.source_quality", "source quality consumers use the classical bitrate quality (source identity record unavailable, stale or not yet latched)" },
			{ "product.interpolator", "interpolator builds its own grain bands and uses the render-time field (shared products unavailable)" },
			{ "product.temporal_detail", "temporal detail runs a lower memory tier or its own split (shared split / store unavailable)" },
			{ "product.chroma_cfl", "chroma upscale runs a lower tier without the grain guard (chroma split unavailable)" },
			{ "product.entrance", "entrance stages run in the render with no work done ahead (entrance output unavailable)" },
			{ "product.motion_warp", "motion warp builds its own half-resolution pyramids (shared half luma unavailable)" },
			{ "product.nvof_producer", "optical-flow producer builds its own 8-bit half luma (shared half luma unavailable)" },
			{ "product.region_evidence", "regional repairs use their own local statistics (shared split / light level unavailable)" },
			{ "product.contrast_recovery", "contrast recovery uses the classical bitrate quality for its need test (source identity unavailable)" },
			{ "product.dither_need", "dither need uses the classical bitrate quality (source identity unavailable)" },
			{ "product.light_level", "light-level users use the global level only (per-block light level unavailable)" },
			{ "product.vector_demand", "vector consumer runs without vectors (the vector engine product is unavailable)" },
			{ "product.camera", "camera consumer runs causal / without the camera model (camera products unavailable or short queue)" },
		};
		if (count)
			*count = sizeof(kEntries) / sizeof(kEntries[0]);
		return kEntries;
	}
	inline const ProductEntry* FindProductEntry(const char* id)
	{
		size_t n = 0;
		const ProductEntry* e = ProductEntries(&n);
		for (size_t i = 0; i < n; ++i)
			if (std::string(e[i].id) == id)
				return &e[i];
		return nullptr;
	}
	inline Decision ProductFallback(const char* id, const std::string& why)
	{
		const ProductEntry* e = FindProductEntry(id);
		return Engaged(id, (e ? std::string(e->text) : std::string("reduced form")) + ": " + why);
	}
}
