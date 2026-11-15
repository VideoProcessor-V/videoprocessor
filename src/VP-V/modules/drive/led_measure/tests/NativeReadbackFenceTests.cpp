// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// fx/dv-frame-quiet: the readiness and the deferral of LedDrive::NativeTexReadback.
//  - CPU only (no device): ReadyGate (what Poll does for a slot given the fence) and DeferredCopies
//    (copies recorded inside a frame and issued by the host's post-frame flush: order, disarmed
//    pass-through, one entry per slot, a destroyed owner is skipped, a copy issued by a later frame's
//    flush is counted as late).
//  - On the libplacebo WARP device (GpuWarp; VP_TEST_GPU=hardware for a real GPU): a deferred copy is
//    Pending (and nothing is mapped) until DeferredCopies::Flush, then reads back exactly; an unarmed
//    QueueDeferred issues at once; a readback without a fence is counted visibly
//    (FenceUnavailableReadbacks) and still works through the Map probe.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/drive/led_measure/NativeReadback.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace VideoProcessorTest
{
	namespace
	{
		using LedDrive::DeferredCopies;
		using LedDrive::ReadyGate;

		// A fake owner for the pure bookkeeping tests.
		struct Fake
		{
			std::shared_ptr<int> alive = std::make_shared<int>(0);
			std::vector<int> issued;
			static void Issue(void* self, int slot) { static_cast<Fake*>(self)->issued.push_back(slot); }
		};

		// Audit 7: the thread-local deferral never stays armed past a test (a failed assertion must not leak into the next test).
		struct DisarmGuard
		{
			~DisarmGuard() { DeferredCopies::ForThisThread().Flush(); }
		};

		std::vector<float> Known(int w, int h, int salt)
		{
			std::vector<float> v(static_cast<size_t>(w) * static_cast<size_t>(h));
			for (size_t i = 0; i < v.size(); ++i)
				v[i] = static_cast<float>(static_cast<int>((i * 41 + static_cast<size_t>(salt) * 13) % 991) - 300) * 0.25f;
			return v;
		}

		bool WaitReady(LedDrive::NativeTexReadback& rb, int slot)
		{
			for (int i = 0; i < 5000; ++i)
			{
				const LedDrive::ReadState state = rb.Poll(slot);
				if (state == LedDrive::ReadState::Ready) return true;
				if (state == LedDrive::ReadState::Failed) return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		}
	}

	TEST_CLASS(NativeReadbackFenceTests)
	{
	public:
		TEST_METHOD(AnUnissuedCopyIsPendingWhateverTheFenceSays)
		{
			Assert::IsTrue(ReadyGate::Decide(false, true, 5, 99) == ReadyGate::Step::NotIssued);
			Assert::IsTrue(ReadyGate::Decide(false, false, 0, 0) == ReadyGate::Step::NotIssued, L"also without a fence");
		}

		TEST_METHOD(AFenceBelowTheSignalKeepsTheMapOutOfIt)
		{
			Assert::IsTrue(ReadyGate::Decide(true, true, 7, 6) == ReadyGate::Step::FenceWaiting, L"completed 6 < signal 7");
			Assert::IsTrue(ReadyGate::Decide(true, true, 7, 0) == ReadyGate::Step::FenceWaiting);
			Assert::IsTrue(ReadyGate::Decide(true, true, 7, 7) == ReadyGate::Step::MapProbe, L"the fence reached the signal");
			Assert::IsTrue(ReadyGate::Decide(true, true, 7, 8) == ReadyGate::Step::MapProbe, L"a later value includes this one");
			// A removed device reads UINT64_MAX: the Map then reports the removal as Failed, visibly.
			Assert::IsTrue(ReadyGate::Decide(true, true, 7, ~0ull) == ReadyGate::Step::MapProbe);
		}

		TEST_METHOD(WithoutAFenceTheMapProbeAloneDecides)
		{
			Assert::IsTrue(ReadyGate::Decide(true, false, 0, 0) == ReadyGate::Step::MapProbe);
			Assert::IsTrue(ReadyGate::Decide(true, false, 9, 0) == ReadyGate::Step::MapProbe, L"no fence: the signal value means nothing");
		}

		TEST_METHOD(NotArmedMeansTheCallerIssuesTheCopyAtOnce)
		{
			DeferredCopies d;
			Fake f;
			Assert::IsFalse(d.Defer(f.alive, &f, 0, &Fake::Issue));
			Assert::AreEqual<size_t>(0, d.Pending());
			Assert::AreEqual<size_t>(0, d.Flush());
			Assert::IsTrue(f.issued.empty());
		}

		TEST_METHOD(ArmedCopiesAreIssuedInOrderByFlushAndFlushDisarms)
		{
			DeferredCopies d;
			Fake a, b;
			d.Arm();
			Assert::IsTrue(d.Armed());
			Assert::IsTrue(d.Defer(a.alive, &a, 2, &Fake::Issue));
			Assert::IsTrue(d.Defer(b.alive, &b, 0, &Fake::Issue));
			Assert::IsTrue(d.Defer(a.alive, &a, 1, &Fake::Issue));
			Assert::AreEqual<size_t>(3, d.Pending());
			Assert::IsTrue(a.issued.empty() && b.issued.empty(), L"nothing is issued before Flush");
			Assert::AreEqual<size_t>(3, d.Flush());
			Assert::IsFalse(d.Armed(), L"Flush disarms: the next frame arms again");
			Assert::AreEqual<size_t>(0, d.Pending());
			Assert::AreEqual<size_t>(2, a.issued.size());
			Assert::AreEqual(2, a.issued[0]);
			Assert::AreEqual(1, a.issued[1]);
			Assert::AreEqual<size_t>(1, b.issued.size());
			Assert::AreEqual<unsigned long long>(0, d.LateIssued(), L"all issued by the flush of their own frame");
			Assert::AreEqual<size_t>(0, d.Flush(), L"a second Flush has nothing to do");
		}

		TEST_METHOD(ASlotIsRecordedOnceAndADestroyedOwnerIsSkipped)
		{
			DeferredCopies d;
			Fake a;
			std::unique_ptr<Fake> gone(new Fake());
			d.Arm();
			Assert::IsTrue(d.Defer(a.alive, &a, 0, &Fake::Issue));
			Assert::IsTrue(d.Defer(a.alive, &a, 0, &Fake::Issue), L"deferred, but not recorded twice");
			Assert::IsTrue(d.Defer(gone->alive, gone.get(), 3, &Fake::Issue));
			Assert::AreEqual<size_t>(2, d.Pending());
			gone.reset();   // the owner (and its alive token) is destroyed before the flush
			Assert::AreEqual<size_t>(1, d.Flush(), L"the destroyed owner's copy is skipped, not issued into freed memory");
			Assert::AreEqual<size_t>(1, a.issued.size());
		}

		TEST_METHOD(ACopyIssuedByALaterFramesFlushIsCountedLate)
		{
			DeferredCopies d;
			Fake a;
			d.Arm();                                              // frame 1
			Assert::IsTrue(d.Defer(a.alive, &a, 0, &Fake::Issue));
			// frame 1 never reached its flush; frame 2 arms and records its own copy
			d.Arm();
			Fake b;
			Assert::IsTrue(d.Defer(b.alive, &b, 1, &Fake::Issue));
			Assert::AreEqual<size_t>(2, d.Flush());
			Assert::AreEqual<unsigned long long>(1, d.LateIssued(), L"only frame 1's copy is late");
			Assert::AreEqual<size_t>(1, a.issued.size());
			Assert::AreEqual<size_t>(1, b.issued.size());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(DeferredCopyIsPendingUntilTheFlushThenReadsBackExactly)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DeferredCopyIsPendingUntilTheFlushThenReadsBackExactly)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int w = 11, h = 4;
			const unsigned before = LedDrive::FenceUnavailableReadbacks().load();
			LedDrive::NativeTexReadback rb;
			Assert::IsTrue(rb.Create(gpu, 2, w, h, DXGI_FORMAT_R32_FLOAT), L"Create");
			// The visible record of a device without a fence: counted while the readback lives.
			Assert::AreEqual(before + (rb.FenceAvailable() ? 0u : 1u), LedDrive::FenceUnavailableReadbacks().load());
			Logger::WriteMessage(rb.FenceAvailable() ? "fence available on this device\n" : "NO fence on this device: the Map probe alone\n");
			ID3D11DeviceContext* ctx = nullptr;
			pl_d3d11_get(gpu)->device->GetImmediateContext(&ctx);
			Assert::IsNotNull(ctx);

			// Armed: QueueDeferred only records; nothing is on the GPU, so nothing can become ready.
			const std::vector<float> a = Known(w, h, 1);
			ctx->UpdateSubresource(rb.Texture(0), 0, nullptr, a.data(), static_cast<UINT>(w * sizeof(float)), 0);
			DeferredCopies& deferred = DeferredCopies::ForThisThread();
			DisarmGuard guard;
			deferred.Arm();
			rb.QueueDeferred(0);
			Assert::AreEqual<size_t>(1, deferred.Pending());
			ctx->Flush();
			for (int i = 0; i < 20; ++i)
			{
				Assert::IsTrue(rb.Poll(0) == LedDrive::ReadState::Pending, L"a copy that was not issued is never ready");
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			Assert::AreEqual<size_t>(1, deferred.Flush());
			Assert::IsTrue(WaitReady(rb, 0), L"after the flush the copy completes");
			std::vector<float> got(a.size(), -1.0f);
			Assert::IsTrue(rb.Read(0, got.data(), static_cast<size_t>(w) * sizeof(float), h));
			Assert::IsTrue(std::memcmp(a.data(), got.data(), a.size() * sizeof(float)) == 0, L"the deferred copy differs from the source");

			// Not armed (Flush disarmed): QueueDeferred is Queue, issued at once.
			const std::vector<float> b = Known(w, h, 2);
			ctx->UpdateSubresource(rb.Texture(1), 0, nullptr, b.data(), static_cast<UINT>(w * sizeof(float)), 0);
			rb.QueueDeferred(1);
			Assert::AreEqual<size_t>(0, deferred.Pending(), L"nothing recorded when the deferral is not armed");
			Assert::IsTrue(WaitReady(rb, 1), L"an unarmed QueueDeferred is issued at once");
			Assert::IsTrue(rb.Read(1, got.data(), static_cast<size_t>(w) * sizeof(float), h));
			Assert::IsTrue(std::memcmp(b.data(), got.data(), b.size() * sizeof(float)) == 0, L"the immediate copy differs from the source");

			// A slot queued again immediately while its deferred record is still waiting: the immediate copy wins,
			// the stale record does nothing (no second copy, no failure).
			const std::vector<float> c = Known(w, h, 3);
			ctx->UpdateSubresource(rb.Texture(0), 0, nullptr, c.data(), static_cast<UINT>(w * sizeof(float)), 0);
			deferred.Arm();
			rb.QueueDeferred(0);
			rb.Queue(0);
			deferred.Flush();
			Assert::IsTrue(WaitReady(rb, 0));
			Assert::IsTrue(rb.Read(0, got.data(), static_cast<size_t>(w) * sizeof(float), h));
			Assert::IsTrue(std::memcmp(c.data(), got.data(), c.size() * sizeof(float)) == 0, L"Queue over a deferred record");
			Assert::IsFalse(rb.Failed());

			ctx->Release();
			rb.Release(gpu);
			Assert::AreEqual(before, LedDrive::FenceUnavailableReadbacks().load(), L"the count goes back when the readback is released");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(AReleasedReadbackLeavesNoDanglingDeferredCopy)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AReleasedReadbackLeavesNoDanglingDeferredCopy)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			DeferredCopies& deferred = DeferredCopies::ForThisThread();
			DisarmGuard guard;
			{
				LedDrive::NativeTexReadback rb;
				Assert::IsTrue(rb.Create(gpu, 1, 4, 4, DXGI_FORMAT_R32_FLOAT));
				deferred.Arm();
				rb.QueueDeferred(0);
				Assert::AreEqual<size_t>(1, deferred.Pending());
				rb.Release(gpu);
			}   // destroyed with a record still waiting
			Assert::AreEqual<size_t>(0, deferred.Flush(), L"the record of a destroyed readback is skipped");
		}
	};
}
