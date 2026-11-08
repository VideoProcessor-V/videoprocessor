// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-2: the input.capture hub module routes each hook to the renderer registered under the hook's host, and to
// nobody else; with no renderer registered it leaves the host's own code to run. CPU only, fake hosts.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/capture/CaptureExtension.h>
#include <VP-V/modules/input/capture/CaptureHost.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		namespace RE = RendererExtension;

		struct Counts
		{
			int settings = 0, release = 0, convert = 0, publish = 0;
			std::vector<FallbackCatalog::Decision> fallbacks;
		};

		class FakeHost : public Vpv::Capture::ICaptureHost
		{
		public:
			FakeHost(Counts& c, bool handles) : c_(c), handles_(handles) {}
			void ApplySettings() override { ++c_.settings; }
			void ReleaseGpu() override { ++c_.release; }
			bool Convert(RE::ConvertContext&) override { ++c_.convert; return handles_; }
			void AfterPublish(RE::PublishView&) override { ++c_.publish; }
			void SyncFallback(const FallbackCatalog::Decision& d) override { c_.fallbacks.push_back(d); }

		private:
			Counts& c_;
			bool handles_;
		};

		RE::FrameContext ContextFor(const void* host)
		{
			RE::FrameContext c;
			c.host = host;
			return c;
		}
	}

	TEST_CLASS(CaptureExtensionTests)
	{
	public:
		TEST_METHOD(NoRegisteredRendererLeavesTheHostCode)
		{
			Vpv::Capture::CaptureExtension module;
			int key = 0;
			RE::FrameContext c = ContextFor(&key);
			RE::ConvertContext cc;
			Assert::IsFalse(module.OnConvertedFrame(c, cc), L"not handled: the host's formatter runs");
			RE::PublishView pv;
			module.OnAfterPublish(c, pv);   // nothing to reach, no crash
			RE::LifecycleFacts f;
			f.host = &key;
			f.kind = RE::Lifecycle::GpuReleased;
			module.OnLifecycle(f);
		}

		TEST_METHOD(EachHookReachesOnlyItsOwnRenderer)
		{
			Vpv::Capture::CaptureExtension module;
			int keyA = 0, keyB = 0;
			Counts a, b;
			const size_t before = Vpv::Capture::BoundHosts();
			{
				Vpv::Capture::HostBinding bindA(&keyA, std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(a, true)));
				Vpv::Capture::HostBinding bindB(&keyB, std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(b, false)));
				Assert::IsTrue(bindA.Registered() && bindB.Registered());
				Assert::AreEqual(before + 2, Vpv::Capture::BoundHosts());

				RE::FrameContext cA = ContextFor(&keyA), cB = ContextFor(&keyB);
				RE::ConvertContext cc;
				Assert::IsTrue(module.OnConvertedFrame(cA, cc), L"A's route handled");
				Assert::IsFalse(module.OnConvertedFrame(cB, cc), L"B's route declined: B's host formatter runs");
				RE::PublishView pv;
				module.OnAfterPublish(cA, pv);

				RE::LifecycleFacts f;
				f.host = &keyB;
				f.kind = RE::Lifecycle::SettingsApplied;
				module.OnLifecycle(f);
				f.kind = RE::Lifecycle::GpuReleased;
				module.OnLifecycle(f);
				f.kind = RE::Lifecycle::GpuCreated;   // not served
				module.OnLifecycle(f);

				Assert::AreEqual(1, a.convert); Assert::AreEqual(1, a.publish); Assert::AreEqual(0, a.settings); Assert::AreEqual(0, a.release);
				Assert::AreEqual(1, b.convert); Assert::AreEqual(0, b.publish); Assert::AreEqual(1, b.settings); Assert::AreEqual(1, b.release);
			}
			Assert::AreEqual(before, Vpv::Capture::BoundHosts(), L"both bindings removed with their members");
			RE::FrameContext cA = ContextFor(&keyA);
			RE::ConvertContext cc;
			Assert::IsFalse(module.OnConvertedFrame(cA, cc), L"a removed renderer is never reached again");
			Assert::AreEqual(1, a.convert);
		}

		TEST_METHOD(ARetiredRendererNeverReachesItsReplacement)
		{
			// The replacement may register while the retiring renderer's binding is still alive (another key):
			// each keeps its own state; removing the old one leaves the new one reachable.
			Vpv::Capture::CaptureExtension module;
			int oldKey = 0, newKey = 0;
			Counts o, n;
			std::unique_ptr<Vpv::Capture::HostBinding> oldBind(new Vpv::Capture::HostBinding(&oldKey,
				std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(o, true))));
			Vpv::Capture::HostBinding newBind(&newKey, std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(n, true)));
			oldBind.reset();
			RE::LifecycleFacts f;
			f.host = &oldKey;
			f.kind = RE::Lifecycle::GpuReleased;
			module.OnLifecycle(f);
			Assert::AreEqual(0, n.release, L"the old renderer's release does not reach the new one");
			RE::FrameContext c = ContextFor(&newKey);
			RE::ConvertContext cc;
			Assert::IsTrue(module.OnConvertedFrame(c, cc));
			Assert::AreEqual(1, n.convert);
		}

		TEST_METHOD(AFullBindingTableEngagesTheVisibleFallback)
		{
			// No silent fallbacks: a renderer that finds no free slot raises FallbackCatalog::CaptureUnbound on its own
			// "Fallbacks engaged"; the registered ones raise nothing.
			std::vector<std::unique_ptr<Vpv::Capture::HostBinding>> bindings;
			std::vector<Counts> counts(64);
			std::vector<int> keys(64);
			size_t failed = counts.size();
			for (size_t i = 0; i < counts.size(); ++i)
			{
				bindings.emplace_back(new Vpv::Capture::HostBinding(&keys[i],
					std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(counts[i], true))));
				if (!bindings.back()->Registered())
				{
					failed = i;
					break;
				}
			}
			Assert::IsTrue(failed < counts.size(), L"the table is bounded");
			for (size_t i = 0; i < failed; ++i)
				Assert::IsTrue(counts[i].fallbacks.empty(), L"registered: no entry");
			Assert::AreEqual<size_t>(1, counts[failed].fallbacks.size());
			Assert::IsTrue(counts[failed].fallbacks[0].engaged);
			Assert::AreEqual(std::string("capture extension"), counts[failed].fallbacks[0].id);
			bindings.clear();
			int again = 0;
			Counts c;
			Vpv::Capture::HostBinding fresh(&again, std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(c, true)));
			Assert::IsTrue(fresh.Registered(), L"slots come back with their bindings");
		}

		TEST_METHOD(ConversionThreadDeliveringWhileBindingsComeAndGo)
		{
			// The conversion thread looks its renderer up on every frame while other renderers' bindings are
			// created and removed (doc 59 section 5.3 lifetime test, CPU form).
			Vpv::Capture::CaptureExtension module;
			int key = 0;
			Counts c;
			Vpv::Capture::HostBinding bind(&key, std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(c, true)));
			std::atomic<bool> stop{ false };
			std::atomic<int> misses{ 0 };
			std::thread conversion([&]
			{
				RE::FrameContext ctx = ContextFor(&key);
				RE::ConvertContext cc;
				while (!stop.load())
					if (!module.OnConvertedFrame(ctx, cc))
						misses.fetch_add(1);
			});
			std::vector<int> others(8);
			for (int i = 0; i < 20000; ++i)
			{
				Counts unused;
				Vpv::Capture::HostBinding transient(&others[i % others.size()],
					std::unique_ptr<Vpv::Capture::ICaptureHost>(new FakeHost(unused, false)));
			}
			stop.store(true);
			conversion.join();
			Assert::AreEqual(0, misses.load(), L"the live renderer is found on every frame");
			Assert::IsTrue(c.convert > 0);
		}
	};
}
