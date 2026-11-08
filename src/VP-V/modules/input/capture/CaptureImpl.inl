// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10 (doc 59 section 5.1): the capture code of the renderer, moved VERBATIM out of LibplaceboVideoRenderer.cpp
// into out-of-line member definitions of its Impl. LibplaceboVideoRenderer.cpp includes this file ONCE, right after
// the Impl struct (beside VectorEngineEstimate.inl); no other translation unit may include it. The hub module
// (CaptureExtension) reaches these members through the adapter of CaptureHost.h, registered by the Impl's last
// member (captureBinding). Changes inside the moved bodies are listed per function.
#pragma once

#include <VP-V/modules/input/capture/CapturePayload.h>
#include <VP-V/modules/input/capture/CaptureRoutes.h>

// Item f (P10-2): the live capture switches, from the applied settings. Was two copies of these stores in the
// renderer (Initialize after UpdatePipelineSlots, and the live apply after it); now raised by UpdatePipelineSlots'
// SettingsApplied, where `activeSettings` is already the set both sites applied (`activeSettings = settings` just
// before). Only change: `settings.` -> `activeSettings.`. Settings thread (Initialize / live apply), release stores
// read by the conversion and render threads, as before.
void LibplaceboVideoRenderer::Impl::CaptureApplySettings()
{
	captureGpuWanted.store(activeSettings.capturePath == "gpu", std::memory_order_release);
	rgbGpuWanted.store(activeSettings.nativeRgbIngest, std::memory_order_release);
	dvGpuWanted.store(activeSettings.dvTunnelGpuUnpack, std::memory_order_release);
	earlyWanted.store(activeSettings.captureUpload == "early", std::memory_order_release);
	earlyAuto.store(activeSettings.captureUpload == "auto", std::memory_order_release);   // live too (audit 2026-10-16)
	earlyEfficiency.store(activeSettings.efficiencyFixes, std::memory_order_release);   // modularity 2026-10-25
}

// Item d (P10-2): the capture unpackers, the RGB luma and the early-upload pool at the renderer's GPU release.
// Moved verbatim from the Retire path's `if (d3d11)` block; it now runs at the GpuReleased notification of the
// same path (after the other VP releases there, still before pl_d3d11_destroy). Render thread (renderMutex on the
// Retire path), as before.
void LibplaceboVideoRenderer::Impl::CaptureReleaseGpu()
{
	if (!d3d11)
		return;
				captureUnpack.reset();   // its textures and dispatch, before the device
				rgbUnpack.reset();       // the same for the RGB unpack, and its planes
				dvUnpack.Reset();        // and the Dolby Vision tunnel unpack
				dvGpuFailed.store(false, std::memory_order_release);   // a new device may unpack again
				ReleaseFallback("Dolby Vision GPU unpack");
				pl_tex_destroy(d3d11->gpu, &rgbIngestLuma);
				if (earlyUpload->IsOpen())
				{
					const EarlyUpload::Stats es = earlyUpload->GetStats();
					DebugLog::Log("Early upload: closed (%llu frames read early, %llu took the render upload (%llu before the early upload started), %llu waited for a running upload; %llu uploads, %llu with no free slot (held %llu, GPU release pending %llu), %llu failed)",
						static_cast<unsigned long long>(earlyUsed), static_cast<unsigned long long>(earlyFallback),
						static_cast<unsigned long long>(earlyLate), static_cast<unsigned long long>(earlyWaited),
						static_cast<unsigned long long>(es.uploads), static_cast<unsigned long long>(es.noFreeSlot),
						static_cast<unsigned long long>(es.noFreeHeld), static_cast<unsigned long long>(es.noFreeGpu),
						static_cast<unsigned long long>(es.failures));
				}
				earlyUpload->Close();    // the second device and the shared textures
}


// Items h (P10-3): the conversion thread's capture routes, moved verbatim out of ConversionLoop's try block. Changes:
// - the route is Vpv::Capture::ChooseRoute over the same terms (CaptureRoutes.h; CaptureRouteTableTests pins it
//   against a copy of the old rgbRaw / dvRaw / if-else chain over every combination);
// - `m_impl->` dropped (these are Impl members now); the renderer's own members through `host`
//   (m_videoConversionOverride, AcquireConversionPayload); the loop's locals through the context;
// - makeRaw keeps the early hand-off in the payload's R18 slot (Vpv::Capture::PayloadExtension) instead of the loop's
//   four locals (earlyHandoff, earlyFrame, earlyRowBytes, earlyHeight), for CaptureAfterPublish;
// - a throw inside a route is caught here and its text handed back through *cc.error (the hub contains throws, and
//   a contained throw would otherwise fall back to the host formatter): the loop rethrows it, so the frame fails with
//   the same text exactly as before.
// Returns false only for the host formatter's route: the loop then runs Bill's formatter block unchanged.
// Conversion thread, no queue lock (formatterMutex inside the Dolby Vision analyse, as before).
bool LibplaceboVideoRenderer::Impl::CaptureConvert(RendererExtension::ConvertContext& cc)
{
	if (!cc.frame || !cc.state || !cc.payload || !cc.hostObject || !cc.error)
		return false;
	LibplaceboVideoRenderer* const host = static_cast<LibplaceboVideoRenderer*>(cc.hostObject);
	VideoFrame& frame = *const_cast<VideoFrame*>(cc.frame);   // the loop's own copy (the AddRef / copy below)
	VideoStateComPtr& state = *static_cast<VideoStateComPtr*>(const_cast<void*>(cc.state));   // the loop's local
	std::shared_ptr<QueuedFrame::ConvertedPayload>& converted = *static_cast<std::shared_ptr<QueuedFrame::ConvertedPayload>*>(cc.payload);
	const VideoConversionOverride m_videoConversionOverride = host->m_videoConversionOverride;   // the old expressions' name
	// capture_path: gpu (owner 2026-10-13): V210 / UYVY / HDYC go on raw
	// (the render unpacks them on the GPU); anything else, an override,
	// or a failed unpack earlier this session: the CPU formatter.
	// native_rgb_ingest: R12B / R12L / r210 go on raw (native RGB in the
	// render); the contract is the CPU formatter's (the unpack's luma
	// equals its Y plane).
	// Never on the Dolby Vision tunnel: its r210 words carry packed
	// 12-bit IPT and the DM packet, not RGB (the render excludes it too,
	// so a raw RGB payload there had no metadata and showed black).
	// dv_tunnel_gpu_unpack: the DV formatter finds and checks the DM
	// packet (its pixels only); the picture goes on raw and the render
	// unpacks it on the GPU. Off, an override or a failed unpack this
	// session: the CPU unpack.
	// libplacebo's native upload (ARGB / BGRA with native_rgb_ingest off):
	// no conversion. The frame is marked prepared with an empty payload
	// (the queue waits for one now that these encodings have a
	// formatter; the native upload reads the frame itself).
	Vpv::Capture::RouteFacts facts;
	facts.dolbyVisionTunnel = state->dolbyVisionTunnel;
	facts.overrideNone = m_videoConversionOverride == VideoConversionOverride::VIDEOCONVERSION_NONE;
	facts.displayMode = state->displayMode != nullptr;
	facts.rgbWanted = rgbGpuWanted.load(std::memory_order_acquire);
	facts.rgbFailed = rgbGpuFailed.load(std::memory_order_acquire);
	facts.rgbSupports = GpuRgbUnpack::Supports(state->videoFrameEncoding);
	facts.twelveBitRgb = state->videoFrameEncoding == VideoFrameEncoding::R12B || state->videoFrameEncoding == VideoFrameEncoding::R12L;
	facts.widthMultipleOf8 = state->displayMode && state->displayMode->FrameWidth() % 8 == 0;
	facts.nativeRgbUpload = IsNativeRgbUpload(state->videoFrameEncoding, m_videoConversionOverride, state->dolbyVisionTunnel);
	facts.captureWanted = captureGpuWanted.load(std::memory_order_acquire);
	facts.captureFailed = captureGpuFailed.load(std::memory_order_acquire);
	facts.captureSupports = GpuCaptureUnpack::Supports(state->videoFrameEncoding);
	facts.dvWanted = dvGpuWanted.load(std::memory_order_acquire);
	facts.dvFailed = dvGpuFailed.load(std::memory_order_acquire);
	facts.dvSupports = GpuDolbyVisionTunnelUnpack::Supports(state->videoFrameEncoding);
	const Vpv::Capture::ConvertRoute route = Vpv::Capture::ChooseRoute(facts);
	if (route == Vpv::Capture::ConvertRoute::CpuFormatter)
		return false;
	try
	{
			const bool rgbRaw = route == Vpv::Capture::ConvertRoute::RawRgb;
			// The raw frame held for the render's GPU unpack (capture_path gpu,
			// native_rgb_ingest, dv_tunnel_gpu_unpack).
			auto makeRaw = [&]()
			{
				auto raw = std::make_shared<QueuedFrame::ConvertedPayload>();
				frame.SourceBufferAddRef();   // released with the payload
				// Capture buffers held by raw payloads (owner 2026-10-15): the most
				// at once is logged when it rises, to check against the card's pool.
				const int held = ++*rawBuffersHeld;
				int peak = rawBuffersPeak.load(std::memory_order_relaxed);
				while (held > peak && !rawBuffersPeak.compare_exchange_weak(peak, held))
				{
				}
				if (held > peak)
					DebugLog::Log("Capture raw route: up to %d capture buffers held at once (queue, next / ahead frames)", held);
				std::shared_ptr<std::atomic<int>> heldCounter = rawBuffersHeld;
				raw->rawFrame = std::shared_ptr<VideoFrame>(new VideoFrame(frame), [heldCounter](VideoFrame* f)
				{
					f->SourceBufferRelease();
					delete f;
					--*heldCounter;
				});
				raw->rawRowBytes = static_cast<size_t>(state->BytesPerRow());
				raw->rawBytes = static_cast<size_t>(state->BytesPerFrame());
				raw->rawEncoding = state->videoFrameEncoding;
				// capture_upload: early - the upload runs after the payload is
				// published (EarlyUploadHandoff::RunUpload, CaptureAfterPublish); the frame is
				// held here so the upload can read it.
				if (EarlyActive() && earlyUpload->IsOpen() && state->displayMode)
				{
					raw->earlyHandoff = std::make_shared<EarlyUploadHandoff>();
					auto early = std::make_shared<Vpv::Capture::PayloadExtension>();
					early->earlyHandoff = raw->earlyHandoff;
					early->earlyFrame = raw->rawFrame;
					early->earlyRowBytes = raw->rawRowBytes;
					early->earlyHeight = static_cast<int>(state->displayMode->FrameHeight());
					raw->extension = std::move(early);
				}
				return raw;
			};
			if (route == Vpv::Capture::ConvertRoute::NativeRgbEmpty)
			{
				converted = std::make_shared<QueuedFrame::ConvertedPayload>();
				converted->contract.sampleRange = VideoFrameSampleRange::FULL;
				converted->contract.colorDepth = 8;
				converted->contract.bitShift = 0;
			}
			else if (route == Vpv::Capture::ConvertRoute::RawRgb || route == Vpv::Capture::ConvertRoute::RawCapture)
			{
				auto raw = makeRaw();
				if (rgbRaw)
				{
					raw->contract.sampleRange = GpuRgbUnpack::FullRange(state->videoFrameEncoding) ?
						VideoFrameSampleRange::FULL : VideoFrameSampleRange::LIMITED;
					raw->contract.colorDepth = 10;
					raw->contract.bitShift = 6;
				}
				else
					raw->contract = GpuCaptureUnpack::Contract(state->videoFrameEncoding);
				converted = raw;
			}
			else   // ConvertRoute::DvAnalyse
			{
				CRGBDolbyVisionTunnelFormatter::FrameResult result = CRGBDolbyVisionTunnelFormatter::FrameResult::REJECTED;
				bool highAligned = false;
				LONG byteCount = 0;
				VideoFrameFormatterOutputContract contract;
				DolbyVisionTunnelFrameMetadata dolby;
				{
					std::lock_guard<std::mutex> formatterGuard(formatterMutex);
					if (!formatterState ||
						formatterState->colorspace != state->colorspace)
					{
						formatter->OnVideoState(state);
						formatterState = state;
					}
					auto* dv = dynamic_cast<CRGBDolbyVisionTunnelFormatter*>(formatter.get());
					if (!dv)
						throw std::runtime_error("Dolby Vision tunnel formatter unavailable");
					byteCount = dv->GetOutFrameSize();
					if (byteCount <= 0)
						throw std::runtime_error("formatter returned an empty frame size");
					result = dv->AnalyzeFrame(frame);
					if (result == CRGBDolbyVisionTunnelFormatter::FrameResult::REJECTED)
						throw std::runtime_error("frame conversion failed");
					highAligned = dv->PlanesHighAligned();
					contract = dv->GetOutputContract();
					dolby = dv->GetDolbyVisionTunnelMetadata();
				}
				if (result == CRGBDolbyVisionTunnelFormatter::FrameResult::PICTURE)
				{
					auto raw = makeRaw();
					raw->dvTunnelRaw = true;
					raw->dvTunnelHighAligned = highAligned;
					converted = raw;
				}
				else
				{
					// No lock / no valid DM: the CPU path's zero planes (the render
					// presents black or the SDR RGB passthrough; never these bytes).
					converted = host->AcquireConversionPayload();
					converted->pixels.assign(static_cast<size_t>(byteCount), 0);
				}
				converted->contract = contract;
				converted->dolby = dolby;
			}
	}
	catch (const std::exception& e)
	{
		*cc.error = e.what();
	}
	catch (...)
	{
		*cc.error = "unknown exception";
	}
	return true;
}

// Item i (P10-3): capture_upload: early - after the notify, so readiness waiters wake first;
// the render takes the slot (or abandons it and uploads itself). Moved verbatim from ConversionLoop; the loop's
// four locals are the payload's R18 slot now. Conversion thread, no lock (the hand-off's own mutex), as before.
void LibplaceboVideoRenderer::Impl::CaptureAfterPublish(RendererExtension::PublishView& pv)
{
	const Vpv::Capture::PayloadExtension* published = Vpv::Capture::ExtensionOf(pv.extension);
	if (!published || !published->earlyHandoff || !published->earlyFrame)
		return;
	const std::shared_ptr<EarlyUploadHandoff>& earlyHandoff = published->earlyHandoff;
	const std::shared_ptr<VideoFrame>& earlyFrame = published->earlyFrame;
	const size_t earlyRowBytes = published->earlyRowBytes;
	const int earlyHeight = published->earlyHeight;
		earlyHandoff->RunUpload([&] { return EarlyActive() && earlyUpload->IsOpen() ?
			std::shared_ptr<void>(earlyUpload->Upload(earlyFrame->GetData(), earlyRowBytes, earlyHeight)) : std::shared_ptr<void>(); });
}

// Items a and b (P10-4a): the early upload (EarlyRequested, EarlyActive, SyncEarlyUpload, EarlyTexture,
// BookUploadTimers) and the unpack of raw payloads (UnpackRgbPayload, UnpackDolbyVisionOnCpu, UnpackDolbyVisionPayload,
// UnpackRawPayload), moved VERBATIM out of the Impl struct (one indentation level less; default arguments stay on the
// declarations in the Impl). The data members they use and the EarlyTake type stay in the Impl until the R18 / LVRH
// step. Callers are unchanged: the render upload branches (item g, still in the renderer) and P5a's de-judder
// next / ahead upload (UnpackRawPayload with sessionFallback false; VectorEngineEstimate.inl). Threads as before:
// render thread (renderMutex) for all; EarlyRequested / EarlyActive also on the conversion thread (atomics only).

bool LibplaceboVideoRenderer::Impl::EarlyRequested() const
{
	return EarlyUploadPolicy::Requested(earlyWanted.load(std::memory_order_acquire) ? EarlyUploadPolicy::Mode::Early :
		earlyAuto.load(std::memory_order_acquire) ? EarlyUploadPolicy::Mode::Auto : EarlyUploadPolicy::Mode::Render,
		earlyEfficiency.load(std::memory_order_acquire), lowLatencyQueueActive.load(std::memory_order_acquire));
}

bool LibplaceboVideoRenderer::Impl::EarlyActive() const
{
	return EarlyRequested() && !earlyGuardBlocked.load(std::memory_order_acquire);
}

void LibplaceboVideoRenderer::Impl::SyncEarlyUpload(size_t frameBytes)
{
	const bool requested = EarlyRequested() && d3d11;
	if (!requested)
	{
		if (earlyUpload->IsOpen())
		{
			earlyUpload->Close();
			DebugLog::Log("Early upload: off (%llu frames read early, %llu took the render upload (%llu before the early upload started), %llu waited for a running upload)",
				static_cast<unsigned long long>(earlyUsed), static_cast<unsigned long long>(earlyFallback),
				static_cast<unsigned long long>(earlyLate), static_cast<unsigned long long>(earlyWaited));
		}
		earlyOpenFailed = false;
		earlyGuardChecked = false;
		earlyGuardBlocked.store(false, std::memory_order_release);
		SyncFallback(FallbackCatalog::EarlyUpload(false, false, std::string()));
		earlyRenderUploads = FallbackCatalog::EarlyUploadRenderUploads();
		SyncFallback(earlyRenderUploads.last);
		return;
	}
	SyncFallback(earlyRenderUploads.Current(GetTickCount64()));   // ages out with no eligible payloads
	if (earlyUpload->IsOpen())
		return;
	const SteadyClock::time_point now = SteadyClock::now();
	if (earlyGuardChecked && now - earlyGuardCheckedAt < std::chrono::seconds(5))
		return;
	earlyGuardChecked = true;
	earlyGuardCheckedAt = now;
	// Frames in flight (queue, de-judder next / ahead, the one being
	// filled, a cadence repeat) plus a margin of 3 (owner 2026-10-15);
	// capped at 1 GB and a quarter of the card's memory. The queue part
	// follows the queue target (hunt 2 13-timing L4).
	const int slots = AlphaQueuePolicy::EarlyUploadSlots(earlyUploadQueueTarget.load(std::memory_order_acquire));
	EarlyUploadPolicy::Input guard;
	guard.mode = earlyWanted.load(std::memory_order_acquire) ? EarlyUploadPolicy::Mode::Early : EarlyUploadPolicy::Mode::Auto;
	guard.device = d3d11 && d3d11->device;
	guard.slots = slots;
	guard.frameBytes = frameBytes;
	uint64_t usage = 0, budget = 0;
	if (QueryMotionEngineBudget(usage, budget))
	{
		guard.usageBytes = usage;
		guard.budgetBytes = budget;
	}
	guard.openFailed = earlyOpenFailed;
	guard.guard = earlyEfficiency.load(std::memory_order_acquire);
	const EarlyUploadPolicy::Decision decision = EarlyUploadPolicy::Decide(guard);
	if (decision.reason == EarlyUploadPolicy::Reason::FrameSizeUnknown)
	{
		earlyGuardChecked = false;   // not known yet: decide on the next raw frame (no pool, no entry)
		return;
	}
	earlyGuardBlocked.store(!decision.early, std::memory_order_release);
	if (!decision.early)
	{
		const std::string why = decision.FallbackReason();
		if (!why.empty())
			SyncFallback(FallbackCatalog::EarlyUpload(true, false, why));
		return;
	}
	if (earlyUpload->Open(d3d11->device, d3d11->gpu, slots))
	{
		DebugLog::Log("Early upload: on (%d slots, up to %zu MB; frames uploaded as they arrive on a second device)", slots,
			earlyUpload->CapBytes() >> 20);
		SyncFallback(FallbackCatalog::EarlyUpload(true, true, std::string()));
	}
	else
	{
		earlyOpenFailed = true;
		earlyGuardBlocked.store(true, std::memory_order_release);
		DebugLog::Log("Early upload: not available (%s); the render upload", earlyUpload->LastError().c_str());
		SyncFallback(FallbackCatalog::EarlyUpload(true, false, earlyUpload->LastError()));
	}
}

pl_tex LibplaceboVideoRenderer::Impl::EarlyTexture(const QueuedFrame::ConvertedPayload& payload, EarlyUpload::Slot*& slot, EarlyTake* take)
{
	std::shared_ptr<void> handle = payload.early;
	EarlyUploadHandoff::Outcome outcome = EarlyUploadHandoff::Outcome::NoSlot;
	bool first = false;
	const double takeStart = PassTimerHook::Hook::NowMs();
	if (!handle && payload.earlyHandoff)
		handle = payload.earlyHandoff->Take(&outcome, &first);   // the slot stays alive through the handoff the payload holds
	const bool waited = outcome == EarlyUploadHandoff::Outcome::WaitedThenEarly || outcome == EarlyUploadHandoff::Outcome::WaitedThenNone;
	slot = handle ? static_cast<EarlyUpload::Slot*>(handle.get()) : nullptr;
	pl_tex tex = slot && earlyUpload->IsOpen() ? earlyUpload->Acquire(*slot) : nullptr;
	if (!tex)
		slot = nullptr;
	++(tex ? earlyUsed : earlyFallback);
	const bool late = outcome == EarlyUploadHandoff::Outcome::Late;   // once per payload (Pending -> Abandoned)
	earlyLate += late ? 1 : 0;
	earlyWaited += waited ? 1 : 0;
	if (first)   // once per early-eligible payload: the visible share of render uploads
		SyncFallback(earlyRenderUploads.Note(GetTickCount64(), late, !tex && !late, waited));
	if (take)
	{
		take->late = late || outcome == EarlyUploadHandoff::Outcome::AbandonedAgain;
		take->waitedMs = waited ? PassTimerHook::Hook::NowMs() - takeStart : 0.0;
	}
	return tex;
}

void LibplaceboVideoRenderer::Impl::BookUploadTimers(pl_tex early, const EarlyTake& take, double startMs)
{
	if (take.waitedMs > 0.0)
		passTimerHook.AddCpu("upload: waited for the conversion-thread upload", PassTimers::Kind::CpuWait, take.waitedMs);
	passTimerHook.AddCpu(early ? "upload: unpack submit (early slot)" : take.late ?
		"upload: CPU copy + unpack submit (render thread, early upload not started)" : "upload: CPU copy + unpack submit (render thread)",
		PassTimers::Kind::CpuWork, PassTimerHook::Hook::NowMs() - startMs - take.waitedMs);
}

bool LibplaceboVideoRenderer::Impl::UnpackRgbPayload(const QueuedFrame::ConvertedPayload& payload, int width, int height, bool bt2020, pl_tex& rgb, pl_tex& luma)
{
	if (!payload.Raw() || !d3d11)
		return false;
	if (!rgbUnpack)
		rgbUnpack = std::make_unique<GpuRgbUnpack>(log, d3d11->gpu);
	SyncEarlyUpload(payload.rawBytes);
	EarlyUpload::Slot* slot = nullptr;
	const double uploadCpuStart = PassTimerHook::Hook::NowMs();   // efficiency timers T4
	EarlyTake take;
	const pl_tex early = EarlyTexture(payload, slot, &take);
	bool unpacked = rgbUnpack->Unpack(payload.rawEncoding, bt2020, payload.rawFrame->GetData(), payload.rawRowBytes, width, height,
		rgb, luma, false, early);
	if (slot)
		earlyUpload->Used(*slot);
	BookUploadTimers(early, take, uploadCpuStart);
	if (!unpacked && early)
		unpacked = rgbUnpack->Unpack(payload.rawEncoding, bt2020, payload.rawFrame->GetData(), payload.rawRowBytes, width, height, rgb, luma);
	if (unpacked)
	{
		if (!rgbGpuLogged)
		{
			const VideoFrameEncoding e = payload.rawEncoding;
			DebugLog::Log("Native RGB ingest: %s unpacked on the GPU to RGB 4:4:4 (%dx%d, no CPU conversion)",
				e == VideoFrameEncoding::R210 ? "r210" : e == VideoFrameEncoding::R12B ? "R12B" :
				e == VideoFrameEncoding::R12L ? "R12L" : e == VideoFrameEncoding::R10b ? "R10b" :
				e == VideoFrameEncoding::R10l ? "R10l" : e == VideoFrameEncoding::ARGB_8BIT ? "ARGB" : "BGRA", width, height);
			rgbGpuLogged = true;
		}
		return true;
	}
	if (!rgbGpuFailed.exchange(true))
	{
		DebugLog::Log("Native RGB ingest: the GPU unpack failed (%s); the CPU conversion for the rest of the session",
			rgbUnpack->Error().c_str());
		EngageFallback("native RGB GPU unpack", "CPU conversion to 10-bit 4:2:0 for the session: " + rgbUnpack->Error());
	}
	return false;
}

bool LibplaceboVideoRenderer::Impl::UnpackDolbyVisionOnCpu(const QueuedFrame::ConvertedPayload& payload, int width, int height, std::vector<BYTE>& pixels)
{
	if (!payload.Raw() || !payload.dvTunnelRaw || width <= 0 || height <= 0 ||
		payload.rawRowBytes < static_cast<size_t>(width) * 4 ||
		payload.rawBytes < payload.rawRowBytes * static_cast<size_t>(height))
		return false;
	std::lock_guard<std::mutex> formatterGuard(formatterMutex);
	auto* dv = dynamic_cast<CRGBDolbyVisionTunnelFormatter*>(formatter.get());
	const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
	if (!dv || !formatterState || !formatterState->displayMode ||
		formatterState->videoFrameEncoding != payload.rawEncoding ||
		static_cast<size_t>(dv->GetOutFrameSize()) != bytes ||
		static_cast<size_t>(formatterState->BytesPerRow()) != payload.rawRowBytes)
		return false;
	pixels.resize(bytes);
	return dv->UnpackPicture(payload.rawFrame->GetData(), pixels.data(), payload.dvTunnelHighAligned);
}

bool LibplaceboVideoRenderer::Impl::UnpackDolbyVisionPayload(const QueuedFrame::ConvertedPayload& payload, int width, int height, pl_tex (&planes)[2],
	bool sessionFallback)
{
	// After a failure this session the payloads still in flight go straight
	// to the CPU fallback (no GPU retry per frame, no burst of late frames).
	if (dvGpuFailed.load(std::memory_order_acquire))
		return false;
	GpuDolbyVisionTunnelUnpack& unpack = dvUnpack.Unpacker(log, d3d11->gpu);
	SyncEarlyUpload(payload.rawBytes);
	EarlyUpload::Slot* slot = nullptr;
	const double uploadCpuStart = PassTimerHook::Hook::NowMs();   // efficiency timers T4
	EarlyTake take;
	const pl_tex early = EarlyTexture(payload, slot, &take);
	bool unpacked = unpack.Unpack(payload.rawEncoding, payload.rawFrame->GetData(), payload.rawRowBytes, width, height,
		payload.dvTunnelHighAligned, planes, early);
	if (slot)
		earlyUpload->Used(*slot);
	BookUploadTimers(early, take, uploadCpuStart);
	if (!unpacked && early)   // the early texture refused: the frame itself
		unpacked = unpack.Unpack(payload.rawEncoding, payload.rawFrame->GetData(), payload.rawRowBytes, width, height,
			payload.dvTunnelHighAligned, planes);
	return dvUnpack.Finish(unpacked, width, height, payload.dvTunnelHighAligned, sessionFallback, dvGpuFailed,
		[this](const char* id, const std::string& reason) { EngageFallback(id, reason); });
}

bool LibplaceboVideoRenderer::Impl::UnpackRawPayload(const QueuedFrame::ConvertedPayload& payload, int width, int height, pl_tex (&planes)[2],
	bool sessionFallback)
{
	if (!payload.Raw() || !d3d11)
		return false;
	if (payload.dvTunnelRaw)
		return UnpackDolbyVisionPayload(payload, width, height, planes, sessionFallback);
	if (!captureUnpack)
		captureUnpack = std::make_unique<GpuCaptureUnpack>(log, d3d11->gpu);
	SyncEarlyUpload(payload.rawBytes);
	EarlyUpload::Slot* slot = nullptr;
	const double uploadCpuStart = PassTimerHook::Hook::NowMs();   // efficiency timers T4
	EarlyTake take;
	const pl_tex early = EarlyTexture(payload, slot, &take);
	bool unpacked = captureUnpack->Unpack(payload.rawEncoding, payload.rawFrame->GetData(), payload.rawRowBytes, width, height,
		planes, early);
	if (slot)
		earlyUpload->Used(*slot);
	BookUploadTimers(early, take, uploadCpuStart);
	if (!unpacked && early)   // the early texture refused: the frame itself
		unpacked = captureUnpack->Unpack(payload.rawEncoding, payload.rawFrame->GetData(), payload.rawRowBytes, width, height, planes);
	if (unpacked)
	{
		if (!captureGpuLogged)
		{
			DebugLog::Log("Capture path gpu: %s unpacked on the GPU (%dx%d, no CPU conversion)",
				payload.rawEncoding == VideoFrameEncoding::V210 ? "V210" : "UYVY/HDYC", width, height);
			captureGpuLogged = true;
		}
		return true;
	}
	// The de-judder's next / ahead frame: that frame only (the de-judder
	// holds once); the picture's own frames decide the session.
	if (!sessionFallback)
	{
		DebugLog::Log("Capture path gpu: a de-judder frame's GPU unpack failed (%s); that frame skipped",
			captureUnpack->Error().c_str());
		return false;
	}
	if (!captureGpuFailed.exchange(true))
	{
		DebugLog::Log("Capture path gpu: the GPU unpack failed (%s); the CPU formatters for the rest of the session",
			captureUnpack->Error().c_str());
		// F-3 (m2-dv findings): the CPU formatters keep P210 4:2:2 for V210 / UYVY / HDYC unless the
		// P010 conversion is forced, so the entry no longer claims 4:2:0.
		EngageFallback("capture GPU unpack", "CPU formatters for the session: " + captureUnpack->Error());
	}
	return false;
}

