// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10 (doc 59 R18): what input.capture keeps in the converted payload's `extension` slot. The slot is
// input.capture's: no other module writes it (a module that needs payload data of its own gets an interface, not
// this slot). Set on the conversion thread BEFORE the payload is published and never reassigned after.
//
// P10-3: the early upload's hand-off, so AfterPublish can run the upload after the notify without the conversion
// loop keeping its own copies. The remaining raw fields of the payload (rawFrame, rawRowBytes, rawBytes,
// rawEncoding, early, earlyHandoff, dvTunnelRaw, dvTunnelHighAligned) move here when the LibplaceboVideoRenderer.h
// zone (LVRH) is free; until then they stay in the host's payload, unchanged. C++14.

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
			std::shared_ptr<EarlyUploadHandoff> earlyHandoff;   // the same hand-off the payload holds
			std::shared_ptr<VideoFrame> earlyFrame;             // the raw frame the early upload reads
			size_t earlyRowBytes = 0;
			int earlyHeight = 0;
		};

		// The slot's content (null: none).
		inline const PayloadExtension* ExtensionOf(const std::shared_ptr<void>& slot)
		{
			return static_cast<const PayloadExtension*>(slot.get());
		}

		// The AfterPublish gate: the slot's content when it carries an early upload to run (a hand-off and its frame),
		// else null (no slot, or a payload published without the early upload): nothing is uploaded.
		inline const PayloadExtension* EarlyUploadDue(const std::shared_ptr<void>& slot)
		{
			const PayloadExtension* e = ExtensionOf(slot);
			return e && e->earlyHandoff && e->earlyFrame ? e : nullptr;
		}
	}
}
