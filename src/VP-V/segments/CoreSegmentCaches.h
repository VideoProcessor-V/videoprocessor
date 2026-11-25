// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// K#10 (GEM-175/177): per-host steady-state caches for CoreSegments.inl (declarations only; bodies in ExtensionHub.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Vpv
{
	namespace Segments
	{
		class CoreSegmentCaches
		{
		public:
			using Float32FboProbeFn = bool (*)(const void* gpu);
			using EnvProbeFn = bool (*)();

			static CoreSegmentCaches& Instance();
			void OnGpuCreated(const void* host, const void* gpu);
			bool OnGpuCreated(const void* host, const void* gpu, Float32FboProbeFn probe);
			void OnGpuReleased(const void* host);
			bool GpuSupportsFloat32Fbo(const void* host, const void* gpu, Float32FboProbeFn probe);
			bool RefreshTraceFlag(const void* host, EnvProbeFn probe);
			bool TraceHookListEnabled(const void* host, EnvProbeFn probe);
			void ResetForTesting();
			size_t HostCountForTesting() const;
			uint64_t GpuEpochForTesting(const void* host) const;

		private:
			mutable std::mutex mutex_;
			struct Entry
			{
				const void* host = nullptr;
				const void* gpu = nullptr;
				uint64_t gpuEpoch = 0;
				bool float32Known = false;
				bool float32Supported = false;
				bool traceKnown = false;
				bool traceEnabled = false;
			};
			Entry* FindLocked(const void* host);
			const Entry* FindLocked(const void* host) const;
			Entry& FindOrAddLocked(const void* host);
			std::vector<Entry> entries_;
		};
	}
}
