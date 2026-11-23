// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// See RunningPathway.h / RunningPathwayLatch.h.
#include <pch.h>
#include <VP-V/core/RunningPathway.h>
#include <VP-V/core/RunningPathwayLatch.h>

#include <ConfigFile.h>
#include <ShortcutDefaults.h>   // OriginalPathwayForConfig: the pathway read as the renderer reads it

namespace Vpv
{
	namespace RunningPathway
	{
		namespace
		{
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
			bool Latched() { return Value().load(std::memory_order_relaxed) == kOriginal; }
		}

		// SHOULD-W4-1 (no silent fallback): nothing latched yet (before the profile runtime's first snapshot, or when it
		// failed to start) -> latch once from the start-up configuration file, the one VideoProcessorApp and the dialog
		// load (ConfigFile::Load's default, --config honoured), so an Original process is Bill's from its first read.
		bool Original()
		{
			if (Value().load(std::memory_order_relaxed) == kUnknown && !StartupTried().exchange(true))
				LatchFromConfigurationFile(ConfigFile::DEFAULT_FILENAME);
			return Latched();
		}

		bool Latch(bool original)
		{
			int unknown = kUnknown;
			Value().compare_exchange_strong(unknown, original ? kOriginal : kVpv);
			return Latched();
		}

		bool LatchFromConfigurationFile(const std::string& path)
		{
			if (Value().load() != kUnknown)
				return Latched();
			ConfigFile config;
			if (!config.Load(path))
				return Latched();
			return Latch(OriginalPathwayForConfig(
				[&config](const char* section, const char* key, std::string& out) { return config.TryGetString(section, key, out); }));
		}

		void SetForTesting(int state)
		{
			Value().store(state);
			StartupTried().store(false);
		}
	}
}
