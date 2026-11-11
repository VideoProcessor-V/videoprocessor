// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 (doc 59 R18): what input.capture keeps in the converted payload's `extension` slot. The slot is
// input.capture's: no other module writes it (a module that needs payload data of its own gets an interface, not
// this slot). Set on the conversion thread BEFORE the payload is published and never reassigned after.
//
// R18 (r2/p10): ALL of the capture-specific payload data lives here, so the host's ConvertedPayload keeps Bill's
// members (pixels, contract, dolby, conversionCpuMs, completedQpc) plus VP's captureEncoding and this one slot:
//   - the raw capture frame held for the render's GPU unpack (rawFrame, rawRowBytes, rawBytes, rawEncoding);
//   - the early upload (early: legacy, always null; earlyHandoff: the hand-off the conversion thread uploads into
//     after the publish; earlyHeight: the frame height that upload needs);
//   - the Dolby Vision tunnel route (dvTunnelRaw, dvTunnelHighAligned).
// A payload without a slot (the CPU formatter's, the libplacebo native-upload marker) is "not raw": every reader
// below answers as the former default members did (Raw() false, encoding UNKNOWN, no DV tunnel, Bytes = pixels).
//
// The raw luma this slot carries (the raw frame's luma samples, decoded by the GPU unpack) is the DECODE POINT that
// C9 (doc 93: one luma-decode point in source_analysis) shares: a later consumer of the picture's luma reads it from
// this payload's unpack, it does not decode the capture frame a second time. Design note only; no C9 code here.
// C++14.

#include <VideoFrameEncoding.h>

#include <cstddef>
#include <memory>

class VideoFrame;
class EarlyUploadHandoff;

namespace Vpv
{
	namespace Capture
	{
		struct PayloadExtension
		{
			// capture_path gpu / native_rgb_ingest / dv_tunnel_gpu_unpack: the raw capture frame itself (kept alive
			// here, its buffer released with it), unpacked on the GPU. Null: not a raw payload.
			std::shared_ptr<VideoFrame> rawFrame;
			size_t rawRowBytes = 0;
			size_t rawBytes = 0;
			VideoFrameEncoding rawEncoding = VideoFrameEncoding::UNKNOWN;
			// capture_upload: early (legacy, always null now): the conversion thread never sets it (it publishes
			// earlyHandoff and uploads after the publish); the render still reads it first, so a slot placed here
			// before publication would be used as before.
			std::shared_ptr<void> early;
			// capture_upload: early - the hand-off of the slot the conversion thread uploads AFTER it has published
			// the payload. The render reads it without a lock; the hand-off's own mutex guards its state. Null: the
			// render uploads.
			std::shared_ptr<EarlyUploadHandoff> earlyHandoff;
			int earlyHeight = 0;   // the frame height the early upload reads (AfterPublish)
			// dv_tunnel_gpu_unpack: the raw frame is a Dolby Vision tunnel picture (the payload's `dolby` holds its
			// checked DM); the render unpacks it with GpuDolbyVisionTunnelUnpack, RGB8 in the high (true) or low
			// eight bits of the r210 channels.
			bool dvTunnelRaw = false;
			bool dvTunnelHighAligned = false;
		};

		// The slot's content (null: none).
		inline const PayloadExtension* ExtensionOf(const std::shared_ptr<void>& slot)
		{
			return static_cast<const PayloadExtension*>(slot.get());
		}

		// The same, for a payload (any type with an `extension` slot and `pixels`).
		template <typename Payload>
		inline const PayloadExtension* CaptureOf(const Payload& p)
		{
			return ExtensionOf(p.extension);
		}

		// The slot's content when the payload holds a raw capture frame (the former Raw()), else null.
		template <typename Payload>
		inline const PayloadExtension* RawOf(const Payload& p)
		{
			const PayloadExtension* e = ExtensionOf(p.extension);
			return e && e->rawFrame ? e : nullptr;
		}

		template <typename Payload>
		inline bool IsRaw(const Payload& p) { return RawOf(p) != nullptr; }

		// The former Bytes(): the raw frame's size for a raw payload, else the formatted pixels'.
		template <typename Payload>
		inline size_t BytesOf(const Payload& p)
		{
			const PayloadExtension* e = RawOf(p);
			return e ? e->rawBytes : p.pixels.size();
		}

		// The raw frame's capture encoding (UNKNOWN: not raw).
		template <typename Payload>
		inline VideoFrameEncoding RawEncodingOf(const Payload& p)
		{
			const PayloadExtension* e = ExtensionOf(p.extension);
			return e ? e->rawEncoding : VideoFrameEncoding::UNKNOWN;
		}

		// Dolby Vision tunnel route (false: not the tunnel's raw route).
		template <typename Payload>
		inline bool DvTunnelRawOf(const Payload& p)
		{
			const PayloadExtension* e = ExtensionOf(p.extension);
			return e && e->dvTunnelRaw;
		}

		// The raw frame (null: not raw).
		template <typename Payload>
		inline VideoFrame* RawFrameOf(const Payload& p)
		{
			const PayloadExtension* e = ExtensionOf(p.extension);
			return e ? e->rawFrame.get() : nullptr;
		}

		// The legacy early slot or the early hand-off is present (the render's upload is "early" for this payload).
		template <typename Payload>
		inline bool HasEarlyUpload(const Payload& p)
		{
			const PayloadExtension* e = ExtensionOf(p.extension);
			return e && (e->early || e->earlyHandoff);
		}

		// The AfterPublish gate: the slot's content when it carries an early upload to run (a hand-off and the raw
		// frame it reads), else null (no slot, or a payload published without the early upload): nothing is uploaded.
		inline const PayloadExtension* EarlyUploadDue(const std::shared_ptr<void>& slot)
		{
			const PayloadExtension* e = ExtensionOf(slot);
			return e && e->earlyHandoff && e->rawFrame ? e : nullptr;
		}
	}
}
