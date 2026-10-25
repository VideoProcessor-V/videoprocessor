#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

// Efficiency audit 2026-10-24 #3: capture_upload auto = early upload in EVERY mode (it was early
// only in the low-latency path), so the normal film / de-judder mode no longer copies and
// transfers each raw frame on the render thread (~5.4 ms per 4K 12-bit frame, EarlyUpload.h), and
// with native RGB + de-judder both unpacks read the same uploaded slot.
//
// Byte-identical: the same raw bytes reach the same GPU unpack, after a fence (EarlyUpload's
// anti-tearing contract); only the thread and device that copy them change.
//
// VRAM guard (the 4 GB laptop): the pool is sized by need - the slots the queue needs (frames in
// flight plus the margin, AlphaQueuePolicy::EarlyUploadSlots) x one raw frame - and opened only
// when that fits in the free budget with a reserve left over. Otherwise the render upload is kept
// and the reason is VISIBLE (owner rule vp-no-silent-fallbacks), through FallbackCatalog's
// "early upload" entry: "early upload (the render upload (video memory short, needs N MB, M MB
// free))". A frame size not known yet (0) decides nothing: no pool, no entry, asked again
// (audit 2026-10-25: 0 used to mean "needs nothing" and bypassed the guard). An explicit "early" setting is
// guarded the same way (it cannot work without the memory). "render" is never a fallback.
//
// Pure, CPU-tested; the renderer supplies the DXGI budget reading and the frame size.
namespace EarlyUploadPolicy
{
	enum class Mode { Render, Early, Auto };
	enum class Reason { Active, SettingRender, NoDevice, NoBudget, MemoryShort, OpenFailed, FrameSizeUnknown };

	struct Input
	{
		Mode mode = Mode::Auto;
		bool device = true;              // a D3D11 render device (the second device needs one)
		int slots = 0;                   // the pool the queue needs (AlphaQueuePolicy::EarlyUploadSlots)
		size_t frameBytes = 0;           // one raw frame (rowBytes x height); 0 = not known yet
		uint64_t budgetBytes = 0;        // DXGI local budget (0 = not reported)
		uint64_t usageBytes = 0;         // DXGI local current usage
		uint64_t poolBytesHeld = 0;      // what an open pool already holds (counted in usage)
		bool openFailed = false;         // EarlyUpload::Open failed this session
	};

	// The reserve kept free after the pool: a quarter of the budget, at least 256 MB (the
	// renderer's own textures grow with features; MotionEngineIdle presses at 95 %).
	constexpr double kReserveShare = 0.25;
	constexpr uint64_t kReserveMinBytes = uint64_t(256) << 20;

	struct Decision
	{
		bool early = false;
		Reason reason = Reason::SettingRender;
		uint64_t needBytes = 0;   // the pool by need
		uint64_t freeBytes = 0;   // budget - usage - reserve (+ the pool's own bytes when open)
		// Why the render upload is kept, for FallbackCatalog::EarlyUpload's error text (empty: no
		// fallback - early, the "render" setting, or a frame size not known yet).
		std::string FallbackReason() const
		{
			char text[160];
			switch (reason)
			{
			case Reason::MemoryShort:
				std::snprintf(text, sizeof(text), "video memory short, needs %llu MB, %llu MB free",
					static_cast<unsigned long long>(needBytes >> 20), static_cast<unsigned long long>(freeBytes >> 20));
				return text;
			case Reason::NoDevice: return "no D3D11 device";
			case Reason::OpenFailed: return "the second upload device could not be opened";
			default: return std::string();
			}
		}
	};

	inline Decision Decide(const Input& in)
	{
		Decision d;
		if (in.mode == Mode::Render) { d.reason = Reason::SettingRender; return d; }
		if (!in.device) { d.reason = Reason::NoDevice; return d; }
		if (in.openFailed) { d.reason = Reason::OpenFailed; return d; }
		if (in.frameBytes == 0) { d.reason = Reason::FrameSizeUnknown; return d; }
		d.needBytes = static_cast<uint64_t>(in.slots > 0 ? in.slots : 0) * static_cast<uint64_t>(in.frameBytes);
		if (in.budgetBytes == 0)
		{
			// Not reported: EarlyUpload::Open's own byte cap (1 GB) still bounds the pool.
			d.early = true;
			d.reason = Reason::NoBudget;
			return d;
		}
		const uint64_t reserve = (std::max)(kReserveMinBytes, static_cast<uint64_t>(kReserveShare * static_cast<double>(in.budgetBytes)));
		const uint64_t used = in.usageBytes > in.poolBytesHeld ? in.usageBytes - in.poolBytesHeld : 0;
		d.freeBytes = in.budgetBytes > used + reserve ? in.budgetBytes - used - reserve : 0;
		if (d.needBytes > d.freeBytes) { d.reason = Reason::MemoryShort; return d; }
		d.early = true;
		d.reason = Reason::Active;
		return d;
	}
}
