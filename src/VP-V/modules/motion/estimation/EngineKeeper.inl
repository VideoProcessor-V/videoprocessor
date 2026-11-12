// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a-5 (doc 52 section 3.1 item e): the idle motion engine keeper - the warm-up estimate at a source's start and
// the DXGI video-memory budget reading behind MotionEngineIdle::Keeper. Moved VERBATIM out of
// LibplaceboVideoRenderer.cpp (only the signatures gained the Impl:: qualification; default arguments stay on the
// declarations in Impl). These are out-of-line member definitions of the renderer's Impl: this file is included ONCE,
// by VectorEngineEstimate.inl, which LibplaceboVideoRenderer.cpp includes right after the Impl struct. It is not a
// stand-alone header and no other translation unit may include it. Render thread (renderMutex held), as before.
// The engine state they use (dejudder, motionEngineIdle, motionEngineBudgetAdapter, motionEngineWarm*) lives in
// VpMotion::EstimateState (EstimateState.h) and is reached through the Impl aliases (HOOKS.md section 4).
#pragma once

#include <VP-V/core/VramUsers.h>

// H3 F4 follow-up: create the matcher, its optical-flow engine and textures, and the
// interpolator's planes and passes once, on A's own planes (A -> A); nothing they produce
// is used (no live field, no picture), and the history is forgotten.
void LibplaceboVideoRenderer::Impl::WarmIdleMotionEngine(const VideoFrameFormatterOutputContract& contract, bool lossless422, bool nativeRgb)
{
	// W2-C1 (H3Idle): a permanent bail (the source's format never takes the matcher) ends the warm-up for this source; a transient
	// one (textures or the render device not there yet) keeps it pending for the next frame. Every bail is logged (a transient
	// reason once per change).
	if (nativeRgb || dejudderRgbLuma)
	{
		motionEngineWarmPending = false;
		DebugLog::Log("Motion engine: idle warm-up skipped (%s)", nativeRgb ? "native RGB" : "RGB luma");
		return;
	}
	const char* transient = !d3d11 || !d3d11->gpu ? "no render device yet" : (!textures[0] || !textures[1]) ? "no source textures yet" : nullptr;
	if (transient)
	{
		if (vectorEngine.motionWarmSkipLogged != transient)
		{
			vectorEngine.motionWarmSkipLogged = transient;
			DebugLog::Log("Motion engine: idle warm-up skipped (%s; retried next frame)", transient);
		}
		return;
	}
	motionEngineWarmPending = false;
	vectorEngine.motionWarmSkipLogged = nullptr;
	++vectorEngine.WorkCounters().warmUps;
	Dejudder::Planes a;
	a.luma = textures[0];
	a.chroma = textures[1];
	a.chromaShiftX = 1;
	a.chromaShiftY = lossless422 ? 0 : 1;
	a.fullRange = contract.sampleRange == VideoFrameSampleRange::FULL;
	const auto started = std::chrono::steady_clock::now();
	vectorEstimate.motionEngineWarming = true;
	const bool estimated = RunDejudderEstimate(a, a, 0, 0);
	vectorEstimate.motionEngineWarming = false;
	dejudderLiveFieldFrom = 0;
	motionSegment.Retract(VpMotion::Direction::Forward);
	bool pictured = false;
	if (estimated && dejudder)
	{
		Interpolator::Motion motion;
		// P5c: the ONE allowed direct read of the matcher's outputs outside the P3 field view (the producer side, inside
		// motion\estimation: the keeper's own warm-up estimate, the segment just retracted, so the members are the answer).
		motion.field = dejudder->Field();
		motion.frameTrust = dejudder->FrameTrust();
		motion.subField = dejudder->SubField();
		motion.minTrustedShare = dejudder->GetSettings().minTrustedShare;
		if (!frameInterpolator)
			frameInterpolator = std::make_unique<Interpolator::FrameInterpolator>(log, d3d11->gpu);
		frameInterpolator->SetGrain(activeSettings.interpolationGrain);
		Dejudder::Planes scratch;
		pictured = frameInterpolator->Run(a, a, motion, 0.5f, activeSettings.interpolationFallback, scratch);
		vpPhasePlanesValid = false;
	}
	if (dejudder)
		dejudder->ForgetHistory();
	DebugLog::Log("Motion engine: loaded idle for hiccup repair at the source's start (%.1f ms; estimate %s, interpolator %s)",
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
		estimated ? "ok" : "failed", pictured ? "ok" : "not run");
}

// Bug hunt 3 F4: one DXGI video memory reading a second for the idle motion engine's
// low-memory fallback (engineHeld: its bytes are already in the usage).
void LibplaceboVideoRenderer::Impl::PollMotionEngineBudget(int width, int height, bool engineHeld)
{
	const double now = static_cast<double>(GetTickCount64()) / 1000.0;
	vectorEstimate.motionEngineIdle.NoteTime(now);
	if (!vectorEstimate.motionEngineIdle.BudgetDue(now))
		return;
	uint64_t usage = 0, budget = 0;
	const bool reported = QueryMotionEngineBudget(usage, budget);
	vectorEstimate.motionEngineIdle.NoteBudgetReported(reported);
	vectorEstimate.motionEngineIdle.NoteBudget(reported ? usage : 0, reported ? budget : 0,
		engineHeld ? 0 : MotionEngineIdle::IdleBytes(width, height), now);
	SyncFallback(FallbackCatalog::MotionEnginePressure(vectorEstimate.motionEngineIdle.Pressured()));   // SK-MV: dropping the standing demand is visible
	// W2-C1 (B9): the release is a hard engine state (VramRelease), and the one reading is shared with the skeleton's tier plan
	// (QueryCard reads it while fresh instead of querying DXGI again).
	vectorEngine.NoteVramRelease(vectorEstimate.motionEngineIdle.Pressured() && !engineHeld);   // a held matcher is not released
	DXGI_ADAPTER_DESC desc{};
	if (reported && motionEngineBudgetAdapter && SUCCEEDED(motionEngineBudgetAdapter->GetDesc(&desc)))
		VpSkeleton::VramUsers::Instance().NoteBudget((static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
			static_cast<uint64_t>(desc.AdapterLuid.LowPart), usage, budget, now);
}

bool LibplaceboVideoRenderer::Impl::QueryMotionEngineBudget(uint64_t& usage, uint64_t& budget)
{
	if (!d3d11 || !d3d11->gpu)
		return false;
	if (!motionEngineBudgetAdapter)
	{
		pl_d3d11 d3d = pl_d3d11_get(d3d11->gpu);
		CComPtr<IDXGIDevice> dxgiDevice;
		CComPtr<IDXGIAdapter> adapter;
		if (!d3d || !d3d->device || FAILED(d3d->device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
			FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->QueryInterface(IID_PPV_ARGS(&motionEngineBudgetAdapter))))
			return false;
	}
	// Local: a discrete card's own memory; on an integrated GPU Windows reports its shared budget here.
	DXGI_QUERY_VIDEO_MEMORY_INFO info{};
	if (FAILED(motionEngineBudgetAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || info.Budget == 0)
		return false;
	usage = info.CurrentUsage;
	budget = info.Budget;
	return true;
}
