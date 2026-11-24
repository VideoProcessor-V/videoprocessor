// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// See RunningPathway.h / RunningPathwayLatch.h.
#include <pch.h>
#include <VP-V/core/RunningPathway.h>
#include <VP-V/core/RunningPathwayLatch.h>

#include <ConfigFile.h>
#include <ShortcutDefaults.h>   // OriginalPathwayForConfig: the pathway read as the renderer reads it

#include <mutex>

namespace Vpv
{
	namespace RunningPathway
	{
		namespace
		{
			std::mutex& StartupMutex()
			{
				static std::mutex mutex;
				return mutex;
			}

			std::atomic<bool>& StartupTried()
			{
				static std::atomic<bool> tried{ false };
				return tried;
			}
		}

		std::atomic<int>& Value()
		{
			static std::atomic<int> value{ kUnknown };
			return value;
		}

		namespace
		{
			bool Latched() { return Value().load(std::memory_order_acquire) == kOriginal; }
		}

		// SHOULD-W4-1 (no silent fallback): nothing latched yet (before the profile runtime's first snapshot, or when it
		// failed to start) -> latch once from the start-up configuration file, the one VideoProcessorApp and the dialog
		// load (ConfigFile::Load's default, --config honoured), so an Original process is Bill's from its first read.
		bool Original()
		{
			const int current = Value().load(std::memory_order_acquire);
			if (current != kUnknown)
				return current == kOriginal;
			if (StartupTried().load(std::memory_order_acquire))
				return Latched();

			std::lock_guard<std::mutex> lock(StartupMutex());
			if (Value().load(std::memory_order_acquire) == kUnknown &&
				!StartupTried().load(std::memory_order_acquire))
			{
				LatchFromConfigurationFile(ConfigFile::DEFAULT_FILENAME);
				StartupTried().store(true, std::memory_order_release);
			}
			return Latched();
		}

		bool Latch(bool original)
		{
			int unknown = kUnknown;
			Value().compare_exchange_strong(
				unknown,
				original ? kOriginal : kVpv,
				std::memory_order_acq_rel,
				std::memory_order_acquire);
			return Latched();
		}

		bool LatchFromConfigurationFile(const std::string& path)
		{
			if (Value().load(std::memory_order_acquire) != kUnknown)
				return Latched();
			ConfigFile config;
			if (!config.Load(path))
				return Latched();
			return Latch(OriginalPathwayForConfig(
				[&config](const char* section, const char* key, std::string& out) { return config.TryGetString(section, key, out); }));
		}

		void SetForTesting(int state)
		{
			std::lock_guard<std::mutex> lock(StartupMutex());
			StartupTried().store(false, std::memory_order_release);
			Value().store(state, std::memory_order_release);
		}
	}
}
