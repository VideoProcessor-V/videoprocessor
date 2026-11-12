// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P11a-d (refs/110 s2.4): the LED / ambient / target-black share that sat in LibplaceboVideoRenderer.cpp's
// RenderLocked, out of Bill's class. Out-of-line member definitions of the renderer's Impl, included ONCE by
// LibplaceboVideoRenderer.cpp with the other module .inl files. Moved VERBATIM; only the moved state is
// name-qualified (`vpv.led.sdrLift.x`, state in LedSdrLiftState.h, reached through the S8 anchor `Vpv::HostState vpv`).
//   - IntentFrameBlackNits: the one call the frame makes (the old `IntentTargetBlackNits(sourceSequence)` line):
//     the target-black decision then the SDR lift that follows from it (the lift reads only values, never the
//     target, so the order against ApplyTargetLuminance / NoteVideoTarget is not observable in the image).
//   - UpdateAmbientSdrLift: the old "Hunt 3 F2" block, body unchanged.
//   - LedDisplayGamma: the old static, body unchanged.
// Render thread (RenderLocked, renderMutex held).
#pragma once

double LibplaceboVideoRenderer::Impl::IntentFrameBlackNits(uint64_t sourceSequence, bool inputIsSdr, const pl_frame& image)
{
	// Ambient adaptation (intent interpreter item 3, 2026-10-17): the black this frame
	// renders to; exactly sdr_black_nits while ambient_adapt is off or at strength 0.
	const double renderBlackNits = IntentTargetBlackNits(sourceSequence);
	UpdateAmbientSdrLift(renderBlackNits, inputIsSdr, image);
	return renderBlackNits;
}

void LibplaceboVideoRenderer::Impl::UpdateAmbientSdrLift(double renderBlackNits, bool inputIsSdr, const pl_frame& image)
{
	// Hunt 3 F2: for SDR input the target black above stays the SDR reference (sdr_black_nits
	// is not applied to SDR: the signal is display-referred, CONFIGURATION.html), so ambient
	// adaptation's rise of the black reaches the picture as the BT.1886 black lift it stands
	// for (LibplaceboRenderParameters::SdrAmbientLift, through the frame's colour adjustment).
	// Off, strength 0 or a dark room: renderBlackNits is the owner's black exactly and
	// nothing changes (bit-identical).
	{
		float contrast = 1.0f, brightness = 0.0f;
		vpv.led.sdrLift.ambientSdrLift = inputIsSdr && LibplaceboRenderParameters::SdrAmbientLift(sdrBlackNits, renderBlackNits,
			sdrTargetNits, image.color.transfer, contrast, brightness);
		vpv.led.sdrLift.ambientSdrContrast = vpv.led.sdrLift.ambientSdrLift ? contrast : 1.0f;
		vpv.led.sdrLift.ambientSdrBrightness = vpv.led.sdrLift.ambientSdrLift ? brightness : 0.0f;
		std::string state;
		char text[256];
		if (vpv.led.sdrLift.ambientSdrLift)
		{
			snprintf(text, sizeof(text), "Ambient adaptation (SDR input): BT.1886 black lift, contrast %.6f brightness %.6f "
				"(effective black %.5f nits over the owner's %.5f, white %.1f nits)", vpv.led.sdrLift.ambientSdrContrast, vpv.led.sdrLift.ambientSdrBrightness,
				renderBlackNits, sdrBlackNits, sdrTargetNits);
			state = text;
		}
		else if (inputIsSdr && renderBlackNits > sdrBlackNits)
		{
			snprintf(text, sizeof(text), "Ambient adaptation (SDR input): not applied (source transfer %s is not a power law)",
				pl_color_transfer_name(image.color.transfer));
			state = text;
		}
		else if (!vpv.led.sdrLift.ambientSdrLogged.empty())
			state = "Ambient adaptation (SDR input): no lift (the owner's black)";
		if (!state.empty() && state != vpv.led.sdrLift.ambientSdrLogged)
		{
			vpv.led.sdrLift.ambientSdrLogged = state;
			DebugLog::Log("%s", state.c_str());
		}
	}
}

// The gamma an SDR output transfer encodes for: the gamma boost's base
// (delta 4). Pure powers as given; BT.1886 as 2.4 and sRGB as 2.2; anything
// else (HDR) keeps 2.2, the value the boost has always used.
double LibplaceboVideoRenderer::Impl::LedDisplayGamma(enum pl_color_transfer transfer)
{
	switch (transfer)
	{
	case PL_COLOR_TRC_GAMMA18: return 1.8;
	case PL_COLOR_TRC_GAMMA20: return 2.0;
	case PL_COLOR_TRC_GAMMA22: return 2.2;
	case PL_COLOR_TRC_GAMMA24: return 2.4;
	case PL_COLOR_TRC_GAMMA26: return 2.6;
	case PL_COLOR_TRC_GAMMA28: return 2.8;
	case PL_COLOR_TRC_BT_1886: return 2.4;
	case PL_COLOR_TRC_SRGB: return 2.2;
	default: return 2.2;
	}
}
