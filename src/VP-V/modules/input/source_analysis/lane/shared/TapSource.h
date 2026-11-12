// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P10 D5 phase A (refs/118 s4): the lane tap-source decision. lane_tap_source = auto asks the P13 source ring for the queued
// frame's early-upload slot (the GPU tap, phase C); cpu forces the CPU tap (Original pathway always). The CPU tap is the
// per-frame fallback. EVERY reason a frame does not take the GPU source is a Cause, counted per cause (TapSource::Counters)
// and shown (Ctrl+I line, one-time notice, Config status, Ctrl+Alt+D dump); never silent (rule: no silent fallbacks).
//
// The format rule is NOT redone here: it is VpUnit::EngineLumaGpu::FormatCause (limited-range P010 / P210 / v210 / UYVY / HDYC),
// its causes map one to one. Decide() peeks the slot with EarlyUploadHandoff::PeekAttached (never waits, never abandons).
// Header-only; Decide() references EngineLumaGpu::FormatCause, so only a TU that calls it links EngineLumaGpu.cpp.

#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>
#include <VP-V/modules/input/capture/SourceRing.h>
#include <VP-V/modules/input/source_analysis/lane/shared/EngineLumaGpu.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace Vpv
{
	namespace TapSource
	{
		enum class Mode : uint8_t { Auto = 0, Cpu = 1 };

		enum class Cause : uint32_t
		{
			None = 0,            // the GPU source serves this frame
			SettingCpu,          // lane_tap_source = cpu (or the Original pathway)
			NoRawPayload,        // the queued frame carries no raw capture payload / hand-off
			NotInRing,           // the frame is not in the source ring (not listed this pass / another generation)
			UploadPending,       // the early upload has not attached yet (Pending / Uploading)
			NoEarlySlot,         // the early upload produced no slot (None / Abandoned)
			FormatRgb,
			FormatDolbyVision,
			FormatSdrTunnel,
			FullRange,
			FormatOther,
			SizeMismatch,        // the slot's row pitch or height cannot hold the tap's frame
			AcquireFailed,       // the render device could not open the slot (phase C)
			DispatchFailed,      // the unpack kernel refused (phase C)
			Count
		};

		inline const char* CauseName(Cause c)
		{
			switch (c)
			{
			case Cause::None: return "gpu";
			case Cause::SettingCpu: return "lane_tap_source cpu";
			case Cause::NoRawPayload: return "no raw payload";
			case Cause::NotInRing: return "not in source ring";
			case Cause::UploadPending: return "upload pending";
			case Cause::NoEarlySlot: return "no early slot";
			case Cause::FormatRgb: return "rgb";
			case Cause::FormatDolbyVision: return "dolby vision";
			case Cause::FormatSdrTunnel: return "sdr tunnel";
			case Cause::FullRange: return "full range";
			case Cause::FormatOther: return "format";
			case Cause::SizeMismatch: return "size mismatch";
			case Cause::AcquireFailed: return "acquire failed";
			case Cause::DispatchFailed: return "dispatch failed";
			default: return "?";
			}
		}

		inline Cause FromFormat(VpUnit::EngineLumaGpu::Cause c)
		{
			using E = VpUnit::EngineLumaGpu::Cause;
			switch (c)
			{
			case E::None: return Cause::None;
			case E::FormatRgb: return Cause::FormatRgb;
			case E::FormatDolbyVision: return Cause::FormatDolbyVision;
			case E::FormatSdrTunnel: return Cause::FormatSdrTunnel;
			case E::FullRange: return Cause::FullRange;
			case E::NoSlot: return Cause::NoEarlySlot;
			default: return Cause::FormatOther;
			}
		}

		// Per-cause counters (any thread). Count(None) counts a GPU-served frame.
		class Counters
		{
		public:
			Counters() { Reset(); }
			void Reset()
			{
				for (auto& c : counts) c.store(0, std::memory_order_relaxed);
				last.store(0, std::memory_order_relaxed);
			}
			void Count(Cause cause)
			{
				const uint32_t i = static_cast<uint32_t>(cause);
				if (i >= static_cast<uint32_t>(Cause::Count))
					return;
				counts[i].fetch_add(1, std::memory_order_relaxed);
				if (cause != Cause::None)
					last.store(i, std::memory_order_relaxed);
			}
			uint64_t Of(Cause cause) const
			{
				const uint32_t i = static_cast<uint32_t>(cause);
				return i < static_cast<uint32_t>(Cause::Count) ? counts[i].load(std::memory_order_relaxed) : 0;
			}
			uint64_t Fallbacks() const
			{
				uint64_t n = 0;
				for (uint32_t i = 1; i < static_cast<uint32_t>(Cause::Count); ++i)
					n += counts[i].load(std::memory_order_relaxed);
				return n;
			}
			Cause Last() const { return static_cast<Cause>(last.load(std::memory_order_relaxed)); }

		private:
			std::atomic<uint64_t> counts[static_cast<uint32_t>(Cause::Count)];
			std::atomic<uint32_t> last;
		};

		// The one-line text for the dump / status: "gpu 12, cpu 3 (last: upload pending)" plus every non-zero cause.
		inline std::string Describe(const Counters& c)
		{
			std::string s = "tap source: gpu " + std::to_string(c.Of(Cause::None)) + ", cpu " + std::to_string(c.Fallbacks());
			for (uint32_t i = 1; i < static_cast<uint32_t>(Cause::Count); ++i)
			{
				const uint64_t n = c.Of(static_cast<Cause>(i));
				if (n)
					s += "; " + std::string(CauseName(static_cast<Cause>(i))) + " " + std::to_string(n);
			}
			if (c.Fallbacks())
				s += " (last: " + std::string(CauseName(c.Last())) + ")";
			return s;
		}

		struct Pick
		{
			Capture::SourceRing::Entry entry;      // the ring entry (holds the hand-off)
			std::shared_ptr<void> slot;            // the attached early-upload slot (phase C acquires it on the render device)
			VpUnit::EngineLumaGpu::Packing packing = VpUnit::EngineLumaGpu::Packing::Planar16;
		};

		// One frame's decision. Cause::None: *pick is filled (the reader owns the copies; release on the render thread).
		// Order: setting, ring, hand-off, format, size, slot state. Never waits, never abandons.
		inline Cause Decide(Mode mode, const Capture::SourceRing& ring, uint64_t seq, uint64_t generation, bool fullRange,
			uint32_t width, uint32_t height, Pick& pick)
		{
			if (mode == Mode::Cpu)
				return Cause::SettingCpu;
			if (!ring.Find(seq, generation, pick.entry))
				return Cause::NotInRing;
			if (!pick.entry.earlyHandoff)
				return Cause::NoRawPayload;
			const auto encoding = static_cast<VideoFrameEncoding>(pick.entry.rawEncoding);
			const VpUnit::EngineLumaGpu::Cause format =
				VpUnit::EngineLumaGpu::FormatCause(encoding, fullRange, &pick.packing);
			if (format != VpUnit::EngineLumaGpu::Cause::None)
				return FromFormat(format);
			if (pick.entry.rawRowBytes < VpUnit::EngineLumaGpu::MinRowBytes(pick.packing, width) ||
				pick.entry.height < static_cast<int32_t>(height))
				return Cause::SizeMismatch;
			const auto handoff = std::static_pointer_cast<EarlyUploadHandoff>(pick.entry.earlyHandoff);
			EarlyUploadHandoff::State state = EarlyUploadHandoff::State::Pending;
			pick.slot = handoff->PeekAttached(&state);
			if (pick.slot)
				return Cause::None;
			if (state == EarlyUploadHandoff::State::Pending || state == EarlyUploadHandoff::State::Uploading)
				return Cause::UploadPending;
			return Cause::NoEarlySlot;
		}
	}
}
