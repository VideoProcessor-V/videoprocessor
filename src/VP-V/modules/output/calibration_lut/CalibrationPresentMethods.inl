// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P11a-e (refs/110 s2.5): the calibration pattern present, moved VERBATIM out of LibplaceboVideoRenderer.cpp's Impl.
// An out-of-line member definition included ONCE by the renderer with the other module .inl files. The adapter it
// calls (calibrationHost) stays an Impl member: outputColour holds a reference into it (init order), so it cannot
// move into the last-declared anchor; the PlanPresent handler is a follow-on once Phase 3's R11 is on this base.
#pragma once

// Automatic calibration (docs/calibration/AUTO-CALIBRATION.md): one refresh of the current
// patch, presented by the calibration adapter (the caller holds renderMutex).
bool LibplaceboVideoRenderer::Impl::PresentCalibrationPattern()
{
	if (!d3d11 || !d3d11->gpu || !swapchain || outputResourcesRetired)
		return false;
	const CalibrationLutHost::PresentResult presented = calibrationHost.Present({ d3d11->gpu, vpOwnedSwapchain,
		swapchain, negotiatedSwapchainFormat, [this](ID3D11Texture2D* backbuffer, std::vector<uint32_t>& pixels,
			D3D11_TEXTURE2D_DESC& desc, std::string& error) { return ReadR10Texture(backbuffer, pixels, desc, error); } });
	if (presented.patchWindow.engaged) EngageFallback(presented.patchWindow.id.c_str(), presented.patchWindow.reason);
	return presented.presented;
}
