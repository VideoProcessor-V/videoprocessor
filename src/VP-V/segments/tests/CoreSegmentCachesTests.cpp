// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// K#10 (GEM-175/177): CoreSegmentCaches per-session trace flag cache and per-GPU-lifecycle
// rgba32f FBO support cache. Covers CPU caching/invalidation, same-address device reuse,
// ExtensionHub lifecycle wiring, and real D3D11 WARP pl_gpu probe parity across recreate.

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/segments/CoreSegmentCaches.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/gpu.h>
#include <libplacebo/log.h>

#include <atomic>
#include <cstdint>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RendererExtension;
		using Vpv::Segments::CoreSegmentCaches;

		// RAII save/restore guard for an environment variable so zero env state leaks across tests.
		struct EnvVarGuard
		{
			std::string name;
			bool hadValue = false;
			std::string savedValue;

			explicit EnvVarGuard(const char* varName) : name(varName ? varName : "")
			{
				char buf[256] = {};
				const DWORD n = GetEnvironmentVariableA(name.c_str(), buf, static_cast<DWORD>(sizeof(buf)));
				if (n > 0 && n < sizeof(buf))
				{
					hadValue = true;
					savedValue.assign(buf, n);
				}
				else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND && n >= sizeof(buf))
				{
					std::string big(n + 1, '\0');
					const DWORD n2 = GetEnvironmentVariableA(name.c_str(), &big[0], n + 1);
					if (n2 > 0)
					{
						hadValue = true;
						savedValue.assign(big.data(), n2);
					}
				}
			}

			~EnvVarGuard()
			{
				if (hadValue)
					SetEnvironmentVariableA(name.c_str(), savedValue.c_str());
				else
					SetEnvironmentVariableA(name.c_str(), nullptr);
			}

			void Set(const char* value) const
			{
				SetEnvironmentVariableA(name.c_str(), value);
			}
		};

		// RAII reset scope for CoreSegmentCaches singleton state (entry + exit).
		struct CachesResetGuard
		{
			CachesResetGuard() { CoreSegmentCaches::Instance().ResetForTesting(); }
			~CachesResetGuard() { CoreSegmentCaches::Instance().ResetForTesting(); }
		};

		// RAII reset scope for ExtensionHub modules/hosts (entry + exit) so stack-allocated fake
		// GPU tags are never fanned out to leftover GPU-aware modules from earlier tests.
		struct HubModulesResetGuard
		{
			HubModulesResetGuard() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
			~HubModulesResetGuard() { Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting(); }
		};

		// RAII scope ensuring a host notified via ExtensionHub::OnLifecycle(GpuCreated) is cleanly released.
		struct HubHostReleaseGuard
		{
			const void* host = nullptr;
			explicit HubHostReleaseGuard(const void* h) : host(h) {}
			~HubHostReleaseGuard()
			{
				if (host)
				{
					RE::LifecycleFacts f;
					f.kind = RE::Lifecycle::GpuReleased;
					f.host = host;
					Vpv::Hub::ExtensionHub::Instance().OnLifecycle(f);
				}
			}
		};

		// RAII scope for a real libplacebo D3D11 WARP device.
		struct WarpPlGpuScope
		{
			pl_log log = nullptr;
			pl_d3d11 d3d11 = nullptr;

			bool Create()
			{
				Destroy();
				pl_log_params lp{};
				lp.log_level = PL_LOG_NONE;
				log = pl_log_create(PL_API_VER, &lp);
				if (!log)
					return false;
				const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams();
				d3d11 = pl_d3d11_create(log, &params);
				return d3d11 != nullptr && d3d11->gpu != nullptr;
			}

			void Destroy()
			{
				if (d3d11)
					pl_d3d11_destroy(&d3d11);
				if (log)
					pl_log_destroy(&log);
			}

			pl_gpu Gpu() const { return d3d11 ? d3d11->gpu : nullptr; }

			~WarpPlGpuScope() { Destroy(); }
		};

		std::atomic<int> g_traceProbeCalls{ 0 };
		std::atomic<int> g_f32ProbeCalls{ 0 };
		std::atomic<bool> g_f32ProbeResult{ false };
		std::atomic<const void*> g_lastProbedGpu{ nullptr };
		std::atomic<int> g_warpProbeCalls{ 0 };

		bool ReadTraceEnvRaw()
		{
			char f[4] = {};
			return GetEnvironmentVariableA("VP_HOOK_LIST_TRACE", f, static_cast<DWORD>(sizeof(f))) != 0 && f[0] == '1';
		}

		bool CountingTraceEnvProbe()
		{
			g_traceProbeCalls.fetch_add(1, std::memory_order_relaxed);
			return ReadTraceEnvRaw();
		}

		bool CountingMockFloat32Probe(const void* gpu)
		{
			g_f32ProbeCalls.fetch_add(1, std::memory_order_relaxed);
			g_lastProbedGpu.store(gpu, std::memory_order_relaxed);
			return g_f32ProbeResult.load(std::memory_order_relaxed);
		}

		bool DirectFloat32FboQuery(pl_gpu gpu)
		{
			if (!gpu)
				return false;
			const pl_fmt f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 0,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE));
			return f != nullptr && f->component_depth[0] >= 32;
		}

		bool CountingWarpFloat32Probe(const void* g)
		{
			g_warpProbeCalls.fetch_add(1, std::memory_order_relaxed);
			return DirectFloat32FboQuery(static_cast<pl_gpu>(g));
		}
	}

	TEST_CLASS(CoreSegmentCachesTests)
	{
	public:
		TEST_METHOD(TraceFlagCachesAcrossFramesAndRefreshesOnSession)
		{
			EnvVarGuard envGuard("VP_HOOK_LIST_TRACE");
			CachesResetGuard cacheGuard;
			g_traceProbeCalls.store(0, std::memory_order_relaxed);

			CoreSegmentCaches& caches = CoreSegmentCaches::Instance();
			const int hostTag = 0;
			const void* const host = &hostTag;

			envGuard.Set("0");
			Assert::IsFalse(caches.RefreshTraceFlag(host, &CountingTraceEnvProbe), L"initial session refresh reads VP_HOOK_LIST_TRACE=0");
			Assert::AreEqual(1, g_traceProbeCalls.load(std::memory_order_relaxed));

			// Steady-state frame queries hit the per-host cache with zero additional env probes.
			for (int frame = 0; frame < 5; ++frame)
				Assert::IsFalse(caches.TraceHookListEnabled(host, &CountingTraceEnvProbe));
			Assert::AreEqual(1, g_traceProbeCalls.load(std::memory_order_relaxed), L"cached queries must not re-probe env");

			// Mutate process environment mid-session.
			envGuard.Set("1");

			// In-test negative control on the exact same process env: direct uncached env read sees "1" immediately.
			Assert::IsTrue(ReadTraceEnvRaw(), L"negative control: direct uncached env read observes mid-session mutation to 1");

			// Cached query still returns false with 0 additional env-probe invocations.
			for (int frame = 0; frame < 4; ++frame)
				Assert::IsFalse(caches.TraceHookListEnabled(host, &CountingTraceEnvProbe),
					L"mid-session env mutation must not bypass session cache");
			Assert::AreEqual(1, g_traceProbeCalls.load(std::memory_order_relaxed),
				L"cached steady-state path must incur 0 env probes after mutation");

			// Simulate new renderer session / UpdatePipelineSlots calling RefreshTraceFlag.
			Assert::IsTrue(caches.RefreshTraceFlag(host, &CountingTraceEnvProbe),
				L"RefreshTraceFlag re-probes env for the new session");
			Assert::AreEqual(2, g_traceProbeCalls.load(std::memory_order_relaxed));
			Assert::IsTrue(caches.TraceHookListEnabled(host, &CountingTraceEnvProbe));
			Assert::AreEqual(2, g_traceProbeCalls.load(std::memory_order_relaxed));

			// Clear env mid-session: direct uncached read sees false (negative control), cached query stays true.
			envGuard.Set(nullptr);
			Assert::IsFalse(ReadTraceEnvRaw(), L"negative control: uncached env read observes cleared variable");
			Assert::IsTrue(caches.TraceHookListEnabled(host, &CountingTraceEnvProbe),
				L"cached true stays pinned until next RefreshTraceFlag or OnGpuReleased");
			Assert::AreEqual(2, g_traceProbeCalls.load(std::memory_order_relaxed));

			// OnGpuReleased drops the host's cache entry so a subsequent query re-probes and observes cleared env.
			caches.OnGpuReleased(host);
			Assert::AreEqual(size_t(0), caches.HostCountForTesting());
			Assert::IsFalse(caches.TraceHookListEnabled(host, &CountingTraceEnvProbe));
			Assert::AreEqual(3, g_traceProbeCalls.load(std::memory_order_relaxed));

			// Null host bypasses cache directly without creating a table entry.
			caches.ResetForTesting();
			envGuard.Set("1");
			Assert::IsTrue(caches.TraceHookListEnabled(nullptr, &CountingTraceEnvProbe));
			Assert::AreEqual(4, g_traceProbeCalls.load(std::memory_order_relaxed));
			Assert::AreEqual(size_t(0), caches.HostCountForTesting());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(GpuSupportsFloat32FboCachesPerLifecycleAndResetsOnRecreateAndSameAddressReuse)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuSupportsFloat32FboCachesPerLifecycleAndResetsOnRecreateAndSameAddressReuse)
		{
			CachesResetGuard cacheGuard;
			g_f32ProbeCalls.store(0, std::memory_order_relaxed);
			g_lastProbedGpu.store(nullptr, std::memory_order_relaxed);

			CoreSegmentCaches& caches = CoreSegmentCaches::Instance();
			const int hostTag = 0;
			const int gpuTagA = 1;
			const void* const host = &hostTag;
			const void* const gpuA = &gpuTagA;

			g_f32ProbeResult.store(true, std::memory_order_relaxed);
			caches.OnGpuCreated(host, gpuA);
			Assert::AreEqual(uint64_t(1), caches.GpuEpochForTesting(host));

			// 5 queries on the same (host, gpuA) lifecycle invoke the probe exactly once.
			for (int i = 0; i < 5; ++i)
				Assert::IsTrue(caches.GpuSupportsFloat32Fbo(host, gpuA, &CountingMockFloat32Probe));
			Assert::AreEqual(1, g_f32ProbeCalls.load(std::memory_order_relaxed),
				L"Float32 FBO probe must run exactly once per GPU lifecycle");
			Assert::IsTrue(g_lastProbedGpu.load(std::memory_order_relaxed) == gpuA);

			// In-test negative control: flip probe result mid-lifecycle and query uncached null-host path
			// across 5 frames on the exact same gpuA -> probe runs all 5 times and observes false, while
			// cached (host, gpuA) continues returning true with 0 extra probe calls.
			g_f32ProbeResult.store(false, std::memory_order_relaxed);
			for (int i = 0; i < 5; ++i)
				Assert::IsFalse(caches.GpuSupportsFloat32Fbo(nullptr, gpuA, &CountingMockFloat32Probe),
					L"negative control: uncached null-host path must re-probe and observe mid-lifecycle false");
			Assert::AreEqual(6, g_f32ProbeCalls.load(std::memory_order_relaxed),
				L"negative control: uncached null-host path must invoke probe on every frame (1 cached + 5 uncached = 6)");
			Assert::IsTrue(caches.GpuSupportsFloat32Fbo(host, gpuA, &CountingMockFloat32Probe),
				L"cached (host, gpuA) entry must still return true after mid-lifecycle probe flip");
			Assert::AreEqual(6, g_f32ProbeCalls.load(std::memory_order_relaxed),
				L"cached (host, gpuA) query must incur 0 additional probe calls");
			g_f32ProbeResult.store(true, std::memory_order_relaxed);

			// gpu == nullptr returns false without invoking probe and without poisoning cached gpuA state.
			Assert::IsFalse(caches.GpuSupportsFloat32Fbo(host, nullptr, &CountingMockFloat32Probe));
			Assert::AreEqual(6, g_f32ProbeCalls.load(std::memory_order_relaxed));
			Assert::IsTrue(caches.GpuSupportsFloat32Fbo(host, gpuA, &CountingMockFloat32Probe),
				L"nullptr query must not poison cached gpuA entry");
			Assert::AreEqual(6, g_f32ProbeCalls.load(std::memory_order_relaxed));

			// Destroy and recreate device at the EXACT SAME pointer address gpuA with flipped capability (true -> false).
			caches.OnGpuReleased(host);
			Assert::AreEqual(size_t(0), caches.HostCountForTesting());
			Assert::AreEqual(uint64_t(0), caches.GpuEpochForTesting(host));

			g_f32ProbeResult.store(false, std::memory_order_relaxed);
			caches.OnGpuCreated(host, gpuA);
			Assert::IsFalse(caches.GpuSupportsFloat32Fbo(host, gpuA, &CountingMockFloat32Probe),
				L"recreated device at same pointer address must re-probe and never leak stale true");
			Assert::AreEqual(7, g_f32ProbeCalls.load(std::memory_order_relaxed));

			// In-place OnGpuCreated without OnGpuReleased (same address) also bumps epoch and invalidates cache.
			g_f32ProbeResult.store(true, std::memory_order_relaxed);
			const uint64_t epochBefore = caches.GpuEpochForTesting(host);
			caches.OnGpuCreated(host, gpuA);
			Assert::IsTrue(caches.GpuEpochForTesting(host) > epochBefore);
			Assert::IsTrue(caches.GpuSupportsFloat32Fbo(host, gpuA, &CountingMockFloat32Probe),
				L"epoch bump on same-address OnGpuCreated must re-probe");
			Assert::AreEqual(8, g_f32ProbeCalls.load(std::memory_order_relaxed));

			// End-to-end ExtensionHub::OnLifecycle driving CoreSegmentCaches::Instance() (GpuCreated -> GpuReleased -> GpuCreated).
			// Reset ExtensionHub modules on entry and exit so &gpuTagB (stack tag) is never fanned out to leftover GPU-aware modules.
			{
				HubModulesResetGuard hubModulesGuard;
				const int hubHostTag = 2;
				const int gpuTagB = 3;
				const void* const hubHost = &hubHostTag;
				const void* const gpuB = &gpuTagB;
				HubHostReleaseGuard hubGuard(hubHost);

				g_f32ProbeResult.store(true, std::memory_order_relaxed);
				RE::LifecycleFacts created;
				created.kind = RE::Lifecycle::GpuCreated;
				created.host = hubHost;
				created.gpu = reinterpret_cast<pl_gpu>(const_cast<void*>(gpuB));
				Vpv::Hub::ExtensionHub::Instance().OnLifecycle(created);

				Assert::IsTrue(caches.GpuSupportsFloat32Fbo(hubHost, gpuB, &CountingMockFloat32Probe));
				Assert::AreEqual(9, g_f32ProbeCalls.load(std::memory_order_relaxed));
				g_f32ProbeResult.store(false, std::memory_order_relaxed);
				Assert::IsTrue(caches.GpuSupportsFloat32Fbo(hubHost, gpuB, &CountingMockFloat32Probe),
					L"cached true holds while ExtensionHub GPU lifecycle is active");
				Assert::AreEqual(9, g_f32ProbeCalls.load(std::memory_order_relaxed));

				RE::LifecycleFacts released;
				released.kind = RE::Lifecycle::GpuReleased;
				released.host = hubHost;
				Vpv::Hub::ExtensionHub::Instance().OnLifecycle(released);
				Assert::AreEqual(uint64_t(0), caches.GpuEpochForTesting(hubHost));

				Vpv::Hub::ExtensionHub::Instance().OnLifecycle(created);
				Assert::IsFalse(caches.GpuSupportsFloat32Fbo(hubHost, gpuB, &CountingMockFloat32Probe),
					L"ExtensionHub GpuReleased + GpuCreated must reset CoreSegmentCaches and re-probe");
				Assert::AreEqual(10, g_f32ProbeCalls.load(std::memory_order_relaxed));
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(WarpGpuFloat32FboMatchesDirectProbeAcrossDeviceRecreate)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(WarpGpuFloat32FboMatchesDirectProbeAcrossDeviceRecreate)
		{
			CachesResetGuard cacheGuard;
			g_warpProbeCalls.store(0, std::memory_order_relaxed);

			CoreSegmentCaches& caches = CoreSegmentCaches::Instance();
			const int hostTag = 0;
			const void* const host = &hostTag;

			WarpPlGpuScope dev;
			Assert::IsTrue(dev.Create(), L"Could not create D3D11 WARP libplacebo GPU (first lifecycle)");
			pl_gpu const gpu1 = dev.Gpu();
			const bool expected1 = DirectFloat32FboQuery(gpu1);

			caches.OnGpuCreated(host, gpu1);
			for (int frame = 0; frame < 4; ++frame)
			{
				const bool got = caches.GpuSupportsFloat32Fbo(host, gpu1, &CountingWarpFloat32Probe);
				Assert::AreEqual(expected1, got, L"cached WARP float32 FBO capability must match direct pl_find_fmt");
			}
			Assert::AreEqual(1, g_warpProbeCalls.load(std::memory_order_relaxed),
				L"WARP float32 FBO probe must execute exactly once across 4 frames");

			// In-test negative control: uncached null-host bypass through CoreSegmentCaches across 4 frames on the exact same gpu1 runs 4 probes.
			for (int frame = 0; frame < 4; ++frame)
			{
				const bool got = caches.GpuSupportsFloat32Fbo(nullptr, gpu1, &CountingWarpFloat32Probe);
				Assert::AreEqual(expected1, got, L"negative control: uncached null-host bypass must match direct pl_find_fmt");
			}
			Assert::AreEqual(5, g_warpProbeCalls.load(std::memory_order_relaxed),
				L"negative control: uncached null-host bypass runs probe on every frame (1 cached + 4 uncached = 5)");

			// Destroy and cleanly recreate the WARP device with OnGpuReleased -> OnGpuCreated.
			caches.OnGpuReleased(host);
			dev.Destroy();

			Assert::IsTrue(dev.Create(), L"Could not recreate D3D11 WARP libplacebo GPU (second lifecycle)");
			pl_gpu const gpu2 = dev.Gpu();
			const bool expected2 = DirectFloat32FboQuery(gpu2);

			caches.OnGpuCreated(host, gpu2);
			for (int frame = 0; frame < 4; ++frame)
			{
				const bool got = caches.GpuSupportsFloat32Fbo(host, gpu2, &CountingWarpFloat32Probe);
				Assert::AreEqual(expected2, got, L"recreated WARP device must match fresh direct pl_find_fmt");
			}
			Assert::AreEqual(6, g_warpProbeCalls.load(std::memory_order_relaxed),
				L"recreated WARP device must re-probe exactly once (1 cached + 4 uncached + 1 recreated = 6)");

			caches.OnGpuReleased(host);
			dev.Destroy();
		}
	};
}
