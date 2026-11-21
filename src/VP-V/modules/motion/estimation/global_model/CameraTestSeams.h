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
	}
}
