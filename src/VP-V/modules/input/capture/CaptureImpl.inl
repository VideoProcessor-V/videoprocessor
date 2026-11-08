// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10 (doc 59 section 5.1): the capture code of the renderer, moved VERBATIM out of LibplaceboVideoRenderer.cpp
// into out-of-line member definitions of its Impl. LibplaceboVideoRenderer.cpp includes this file ONCE, right after
// the Impl struct (beside VectorEngineEstimate.inl); no other translation unit may include it. The hub module
// (CaptureExtension) reaches these members through the adapter of CaptureHost.h, registered by the Impl's last
// member (captureBinding). Changes inside the moved bodies are listed per function.
#pragma once

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

// R16 (P10-3): the capture route of one frame. P10-2 skeleton: not handled (the host's code runs).
bool LibplaceboVideoRenderer::Impl::CaptureConvert(RendererExtension::ConvertContext& cc)
{
	(void)cc;
	return false;
}

// R16 AfterPublish (P10-3). P10-2 skeleton: nothing.
void LibplaceboVideoRenderer::Impl::CaptureAfterPublish(RendererExtension::PublishView& pv)
{
	(void)pv;
}
