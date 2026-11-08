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
