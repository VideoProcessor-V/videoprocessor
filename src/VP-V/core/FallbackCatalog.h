#pragma once

#include <VP-V/core/FallbackStatus.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>

#include <cmath>
#include <cstdio>
#include <string>

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
}
