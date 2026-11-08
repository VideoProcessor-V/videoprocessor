// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 (doc 59 section 5): the conversion thread's capture route, as a pure decision over plain facts.
// The renderer's ConversionLoop chose it inline (capture_path gpu, native_rgb_ingest, dv_tunnel_gpu_unpack,
// libplacebo's native RGB upload); CaptureConvert (CaptureImpl.inl) now asks this function
// (its facts from CaptureRouteFacts.h FactsOf). The facts are
// exactly the terms of the old expressions, evaluated by the caller in the same way; the order of the old
// if / else-if chain is kept (native RGB upload, then the raw routes, then the Dolby Vision analyse, then the
// CPU formatter). CaptureRouteTableTests pins the function against a verbatim copy of the old expressions
// over every combination of the facts. C++14, header-only, no Windows headers.

namespace Vpv
{
	namespace Capture
	{
		enum class ConvertRoute
		{
			CpuFormatter,      // the host's own formatter (Bill's path)
			NativeRgbEmpty,    // libplacebo's native RGB upload: an empty payload, the render reads the frame
			RawCapture,        // capture_path gpu: V210 / UYVY / HDYC on raw, GpuCaptureUnpack in the render
			RawRgb,            // native_rgb_ingest: r210 / R10b / R10l / R12B / R12L (and 8-bit RGB) on raw
			DvAnalyse          // dv_tunnel_gpu_unpack: the DV formatter checks the DM packet, the picture on raw
		};

		struct RouteFacts
		{
			bool dolbyVisionTunnel = false;      // state->dolbyVisionTunnel
			bool overrideNone = true;            // the conversion override is VIDEOCONVERSION_NONE
			bool displayMode = false;            // state->displayMode is set
			// native_rgb_ingest
			bool rgbWanted = false, rgbFailed = false;
			bool rgbSupports = false;            // GpuRgbUnpack::Supports(encoding)
			bool twelveBitRgb = false;           // encoding is R12B or R12L
			bool widthMultipleOf8 = false;       // displayMode->FrameWidth() % 8 == 0 (read only with a display mode)
			// libplacebo's native RGB upload: IsNativeRgbUpload(encoding, override, dolbyVisionTunnel)
			bool nativeRgbUpload = false;
			// capture_path gpu
			bool captureWanted = false, captureFailed = false;
			bool captureSupports = false;        // GpuCaptureUnpack::Supports(encoding)
			// dv_tunnel_gpu_unpack
			bool dvWanted = false, dvFailed = false;
			bool dvSupports = false;             // GpuDolbyVisionTunnelUnpack::Supports(encoding)
		};

		// The old rgbRaw term (native_rgb_ingest): never on the Dolby Vision tunnel (its r210 words carry packed
		// 12-bit IPT and the DM packet, not RGB); R12B / R12L only at a width that is a multiple of 8.
		inline bool RgbRaw(const RouteFacts& f)
		{
			return !f.dolbyVisionTunnel && f.rgbWanted && !f.rgbFailed && f.overrideNone && f.rgbSupports && f.displayMode &&
				(!f.twelveBitRgb || f.widthMultipleOf8);
		}

		// The old dvRaw term (dv_tunnel_gpu_unpack).
		inline bool DvRaw(const RouteFacts& f)
		{
			return f.dolbyVisionTunnel && f.dvWanted && !f.dvFailed && f.overrideNone && f.dvSupports && f.displayMode;
		}

		inline ConvertRoute ChooseRoute(const RouteFacts& f)
		{
			const bool rgbRaw = RgbRaw(f);
			if (!rgbRaw && f.nativeRgbUpload)
				return ConvertRoute::NativeRgbEmpty;
			if (rgbRaw || (f.captureWanted && !f.captureFailed && f.overrideNone && f.captureSupports))
				return rgbRaw ? ConvertRoute::RawRgb : ConvertRoute::RawCapture;
			if (DvRaw(f))
				return ConvertRoute::DvAnalyse;
			return ConvertRoute::CpuFormatter;
		}
	}
}
