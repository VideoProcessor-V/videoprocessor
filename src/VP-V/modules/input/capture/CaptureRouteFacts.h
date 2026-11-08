// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 (audit F3): the facts of the conversion route, read from the frame's state, the conversion override and the
// capture switches exactly as the pre-P10 rgbRaw / dvRaw / if-else terms read them (CaptureImpl.inl CaptureConvert
// calls this; CaptureRouteTableTests pins FactsOf + ChooseRoute against the old expressions on real states).
// `nativeRgbUpload` is the renderer's own IsNativeRgbUpload(encoding, override, dolbyVisionTunnel) (a renderer-local
// function), passed in. C++14.

#include <VP-V/modules/input/capture/CaptureRoutes.h>
#include <VP-V/modules/input/capture/GpuCaptureUnpack.h>
#include <VP-V/modules/input/capture/GpuRgbUnpack.h>
#include <VP-V/modules/input/signal/GpuDolbyVisionTunnelUnpack.h>
#include <VideoConversionOverride.h>
#include <VideoState.h>

namespace Vpv
{
	namespace Capture
	{
		// The renderer's capture switches (its atomics, loaded with acquire by the caller).
		struct RouteFlags
		{
			bool rgbWanted = false, rgbFailed = false;
			bool captureWanted = false, captureFailed = false;
			bool dvWanted = false, dvFailed = false;
		};

		inline RouteFacts FactsOf(const VideoState& state, VideoConversionOverride conversionOverride, bool nativeRgbUpload,
			const RouteFlags& flags)
		{
			RouteFacts facts;
			facts.dolbyVisionTunnel = state.dolbyVisionTunnel;
			facts.overrideNone = conversionOverride == VideoConversionOverride::VIDEOCONVERSION_NONE;
			facts.displayMode = state.displayMode != nullptr;
			facts.rgbWanted = flags.rgbWanted;
			facts.rgbFailed = flags.rgbFailed;
			facts.rgbSupports = GpuRgbUnpack::Supports(state.videoFrameEncoding);
			facts.twelveBitRgb = state.videoFrameEncoding == VideoFrameEncoding::R12B || state.videoFrameEncoding == VideoFrameEncoding::R12L;
			facts.widthMultipleOf8 = state.displayMode && state.displayMode->FrameWidth() % 8 == 0;
			facts.nativeRgbUpload = nativeRgbUpload;
			facts.captureWanted = flags.captureWanted;
			facts.captureFailed = flags.captureFailed;
			facts.captureSupports = GpuCaptureUnpack::Supports(state.videoFrameEncoding);
			facts.dvWanted = flags.dvWanted;
			facts.dvFailed = flags.dvFailed;
			facts.dvSupports = GpuDolbyVisionTunnelUnpack::Supports(state.videoFrameEncoding);
			return facts;
		}
	}
}
