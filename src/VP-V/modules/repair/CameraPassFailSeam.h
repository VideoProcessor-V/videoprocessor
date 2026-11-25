// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Test-only pass-failure seam for repair/camera consumers (AB2#2; mirrors global_model/CameraTestSeams.h).
// Inert in production: unarmed, Take(site) executes a single relaxed atomic load reading <= 0 and
// returns false (zero atomic RMW, zero locks, zero behaviour change). Tests arm a site via the RAII
// ScopedFail guard so armed state can never leak across tests even on assertion failure.
// Pure C++14, camera-type-free, no Windows headers.

#include <atomic>

namespace VpRepair
{
	namespace CameraPassFailSeam
	{
		enum class Site : int
		{
			GrainMap = 0,
			TextureStats,
			TextureRef,
			VectorField,
			Debander,
			Count
		};

		inline std::atomic<int>& Armed(Site s)
		{
			static std::atomic<int> armed[static_cast<int>(Site::Count)];
			return armed[static_cast<int>(s)];
		}

		inline void FailNext(Site s, int count = 1)
		{
			Armed(s).store(count, std::memory_order_release);
		}

		inline bool Take(Site s)
		{
			std::atomic<int>& a = Armed(s);
			int n = a.load(std::memory_order_relaxed);
			while (n > 0)
				if (a.compare_exchange_weak(n, n - 1, std::memory_order_acq_rel, std::memory_order_relaxed))
					return true;
			return false;
		}

		class ScopedFail
		{
		public:
			explicit ScopedFail(Site site, int count = 1)
				: m_site(site)
				, m_prev(Armed(site).exchange(count, std::memory_order_acq_rel))
			{
			}
			~ScopedFail()
			{
				Armed(m_site).store(m_prev, std::memory_order_release);
			}
			ScopedFail(const ScopedFail&) = delete;
			ScopedFail& operator=(const ScopedFail&) = delete;

		private:
			Site m_site;
			int m_prev = 0;
		};
	}
}
