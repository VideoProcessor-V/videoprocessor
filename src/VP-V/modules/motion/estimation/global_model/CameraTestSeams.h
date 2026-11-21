// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Test-only seams for the global camera model and link subsystems (Package D; mirrors repair/CameraPassFailSeam.h).
// Every seam is inert in production: unarmed, each site executes at most one relaxed atomic load (zero behaviour
// change, zero atomic writes, zero locks). Tests arm a seam through an RAII Scoped* guard so armed state can never
// leak across tests even on assertion failure. Pure C++14, camera-type-free, no Windows headers.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace CameraModel
{
	namespace CameraTestSeams
	{
		// ---- Seam 1: CameraLabelFill::ReadFitField pre-reread hook -------------------------------------------------------
		// Fires right after DecodeFitFieldPod succeeds and before ReadFitField re-reads the fit-field slot and P7 stamp
		// (CameraLabelFill.cpp). Lets a unit test deterministically recycle or stamp-drift the slot mid-read.
		using ReadFitFieldHookFn = void(*)(uint64_t seq, void* ctx);

		struct ReadFitFieldHookSlot
		{
			ReadFitFieldHookFn fn = nullptr;
			void* ctx = nullptr;
		};

		inline std::atomic<const ReadFitFieldHookSlot*>& ReadFitFieldHookPtr()
		{
			static std::atomic<const ReadFitFieldHookSlot*> ptr{ nullptr };
			return ptr;
		}

		inline void OnReadFitFieldBeforeReread(uint64_t seq)
		{
			if (const ReadFitFieldHookSlot* s = ReadFitFieldHookPtr().load(std::memory_order_relaxed))
			{
				std::atomic_thread_fence(std::memory_order_acquire);
				if (s->fn)
					s->fn(seq, s->ctx);
			}
		}

		class ScopedReadFitFieldHook
		{
		public:
			ScopedReadFitFieldHook(ReadFitFieldHookFn fn, void* ctx)
				: m_slot{ fn, ctx }
				, m_prev(ReadFitFieldHookPtr().exchange(&m_slot, std::memory_order_acq_rel))
			{
			}
			~ScopedReadFitFieldHook()
			{
				ReadFitFieldHookPtr().store(m_prev, std::memory_order_release);
			}
			ScopedReadFitFieldHook(const ScopedReadFitFieldHook&) = delete;
			ScopedReadFitFieldHook& operator=(const ScopedReadFitFieldHook&) = delete;

		private:
			ReadFitFieldHookSlot m_slot;
			const ReadFitFieldHookSlot* m_prev = nullptr;
		};

		// ---- Seam 2: CameraLane::Build fit-field slot-size override ------------------------------------------------------
		// 0 = unarmed (use the declared Node::bufferBytes unchanged). Non-zero = test-only override for the fit-field slot
		// size (bytes) seen by CameraLane::Build, allowing deterministic testing of the WorstCaseFitFieldPodBytes startup
		// check and exact-boundary slot sizing (CameraLane.cpp).
		inline std::atomic<size_t>& FitFieldSlotBytesOverride()
		{
			static std::atomic<size_t> bytes{ 0 };
			return bytes;
		}

		inline size_t TakeFitFieldSlotBytesOverride()
		{
			return FitFieldSlotBytesOverride().load(std::memory_order_relaxed);
		}

		class ScopedFitFieldSlotBytesOverride
		{
		public:
			explicit ScopedFitFieldSlotBytesOverride(size_t bytes)
				: m_prev(FitFieldSlotBytesOverride().exchange(bytes, std::memory_order_acq_rel))
			{
			}
			~ScopedFitFieldSlotBytesOverride()
			{
				FitFieldSlotBytesOverride().store(m_prev, std::memory_order_release);
			}
			ScopedFitFieldSlotBytesOverride(const ScopedFitFieldSlotBytesOverride&) = delete;
			ScopedFitFieldSlotBytesOverride& operator=(const ScopedFitFieldSlotBytesOverride&) = delete;

		private:
			size_t m_prev = 0;
		};
	}
}
