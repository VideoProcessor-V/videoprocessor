// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// capture_upload early: the conversion/render hand-off (CPU only, no GPU).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(EarlyUploadHandoffTests)
	{
	public:
		using H = EarlyUploadHandoff;

		TEST_METHOD(AttachedBeforeTakeIsEarlyAndRepeatedTakesShareTheSlot)
		{
			H h;
			auto slot = std::make_shared<int>(7);
			Assert::IsTrue(h.BeginUpload());
			h.Finish(slot);
			H::Outcome o1{}, o2{};
			auto a = h.Take(&o1);
			auto b = h.Take(&o2);
			Assert::IsTrue(o1 == H::Outcome::Early);
			Assert::IsTrue(o2 == H::Outcome::Early);
			Assert::IsTrue(a == slot && b == slot);
			Assert::IsTrue(h.Peek() == H::State::Attached);
		}

		TEST_METHOD(TakeWhilePendingIsLateAndTheUploadIsSkipped)
		{
			H h;
			H::Outcome o{};
			Assert::IsTrue(h.Take(&o) == nullptr);
			Assert::IsTrue(o == H::Outcome::Late);
			Assert::IsTrue(h.Peek() == H::State::Abandoned);
			Assert::IsFalse(h.BeginUpload());
			Assert::IsTrue(h.Take(&o) == nullptr);
			Assert::IsTrue(o == H::Outcome::AbandonedAgain, L"a late payload is counted once");
		}

		TEST_METHOD(TakeWhileUploadingBlocksUntilFinishWithTheSlot)
		{
			H h;
			Assert::IsTrue(h.BeginUpload());
			auto slot = std::make_shared<int>(3);
			std::promise<void> entered;
			H::Outcome o{};
			std::shared_ptr<void> got;
			std::thread render([&]
			{
				entered.set_value();
				got = h.Take(&o);
			});
			Assert::IsTrue(entered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
			h.Finish(slot);
			render.join();
			Assert::IsTrue(o == H::Outcome::WaitedThenEarly || o == H::Outcome::Early);   // the render may reach Take after Finish
			Assert::IsTrue(got == slot);
		}

		TEST_METHOD(TakeWhileUploadingBlocksUntilFinishWithNoSlot)
		{
			H h;
			Assert::IsTrue(h.BeginUpload());
			std::promise<void> entered;
			H::Outcome o{};
			std::shared_ptr<void> got = std::make_shared<int>(1);
			std::thread render([&]
			{
				entered.set_value();
				got = h.Take(&o);
			});
			Assert::IsTrue(entered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
			h.Finish(nullptr);
			render.join();
			Assert::IsTrue(o == H::Outcome::WaitedThenNone || o == H::Outcome::NoSlot);
			Assert::IsTrue(got == nullptr);
			Assert::IsTrue(h.Peek() == H::State::None);
		}

		TEST_METHOD(UploadScopeWithoutFinishReleasesAWaiterWithNone)
		{
			H h;
			Assert::IsTrue(h.BeginUpload());
			std::promise<void> entered;
			H::Outcome o{};
			std::shared_ptr<void> got = std::make_shared<int>(1);
			std::thread render([&]
			{
				entered.set_value();
				got = h.Take(&o);
			});
			Assert::IsTrue(entered.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready);
			try
			{
				H::UploadScope scope(h);
				throw std::runtime_error("upload failed");
			}
			catch (const std::exception&)
			{
			}
			render.join();
			Assert::IsTrue(got == nullptr);
			Assert::IsTrue(h.Peek() == H::State::None);
		}

		TEST_METHOD(FinishWhenNotUploadingDropsTheHandleOutsideTheLock)
		{
			// Abandoned by the render: the late handle is released, and its deleter
			// can take the handoff's own lock (Peek) without deadlocking.
			H h;
			H::Outcome o{};
			h.Take(&o);   // Pending -> Abandoned
			std::atomic<int> released{ 0 };
			H::State seen = H::State::Pending;
			std::shared_ptr<void> handle(new int(5), [&](void* p)
			{
				seen = h.Peek();   // would deadlock if destroyed under the lock
				delete static_cast<int*>(p);
				++released;
			});
			h.Finish(std::move(handle));
			Assert::AreEqual(1, released.load());
			Assert::IsTrue(seen == H::State::Abandoned);
			Assert::IsTrue(h.Peek() == H::State::Abandoned);
		}

		TEST_METHOD(RunUploadAttachesTheSlot)
		{
			H h;
			auto slot = std::make_shared<int>(9);
			Assert::IsTrue(h.RunUpload([&] { return std::shared_ptr<void>(slot); }));
			H::Outcome o{};
			Assert::IsTrue(h.Take(&o) == slot);
			Assert::IsTrue(o == H::Outcome::Early);
		}

		TEST_METHOD(RunUploadAfterTakeWhilePendingIsSkipped)
		{
			H h;
			H::Outcome o{};
			h.Take(&o);
			bool called = false;
			Assert::IsFalse(h.RunUpload([&] { called = true; return std::shared_ptr<void>(); }));
			Assert::IsFalse(called);
		}

		TEST_METHOD(RunUploadWithAThrowingLambdaLeavesNone)
		{
			H h;
			Assert::IsTrue(h.RunUpload([&]() -> std::shared_ptr<void> { throw std::runtime_error("x"); }));
			Assert::IsTrue(h.Peek() == H::State::None);
			H::Outcome o{};
			Assert::IsTrue(h.Take(&o) == nullptr);
			Assert::IsTrue(o == H::Outcome::NoSlot);
		}

		TEST_METHOD(RepeatedTakesAfterAbandonCountLateOnceAndFirstTakeOnce)
		{
			H h;
			H::Outcome o{};
			bool first = false;
			h.Take(&o, &first);
			Assert::IsTrue(o == H::Outcome::Late && first);
			for (int i = 0; i < 5; ++i)
			{
				Assert::IsTrue(h.Take(&o, &first) == nullptr);
				Assert::IsTrue(o == H::Outcome::AbandonedAgain && !first);
			}
			bool called = false;
			Assert::IsFalse(h.RunUpload([&] { called = true; return std::shared_ptr<void>(); }));
			Assert::IsFalse(called, L"no upload after the render abandoned it");
			H a;
			auto slot = std::make_shared<int>(2);
			a.RunUpload([&] { return std::shared_ptr<void>(slot); });
			a.Take(&o, &first);
			Assert::IsTrue(o == H::Outcome::Early && first);
			a.Take(&o, &first);
			Assert::IsTrue(o == H::Outcome::Early && !first);
		}

		TEST_METHOD(StressRunUploadVersusTakeVersusDestruction)
		{
			// Thousands of payloads: the conversion thread runs the upload while the render takes,
			// each side holding its own shared_ptr, the last holder destroying the handoff. Every
			// slot is released exactly once, no upload runs after an abandon, and a Take that
			// returns a slot only ever sees an attached one.
			const int n = 4000;
			std::atomic<int> made{ 0 }, released{ 0 }, uploadsAfterAbandon{ 0 }, bad{ 0 };
			std::vector<std::shared_ptr<H>> handoffs(n);
			for (auto& p : handoffs)
				p = std::make_shared<H>();
			std::vector<std::shared_ptr<H>> forRender = handoffs, forConversion = handoffs;
			handoffs.clear();
			std::thread conversion([&]
			{
				for (int i = 0; i < n; ++i)
				{
					std::shared_ptr<H> mine = std::move(forConversion[i]);
					mine->RunUpload([&]() -> std::shared_ptr<void>
					{
						if (mine->Peek() != H::State::Uploading)
							++uploadsAfterAbandon;
						if (i % 7 == 3)
							return nullptr;   // no free slot
						if (i % 11 == 5)
							throw std::runtime_error("upload failed");
						++made;
						return std::shared_ptr<void>(new int(i), [&](void* p) { delete static_cast<int*>(p); ++released; });
					});
				}
			});
			std::thread render([&]
			{
				for (int i = 0; i < n; ++i)
				{
					std::shared_ptr<H> mine = std::move(forRender[i]);
					if (i % 3 == 0)
						std::this_thread::yield();
					H::Outcome o{};
					for (int draw = 0; draw < 2; ++draw)
					{
						auto s = mine->Take(&o);
						if (s && !(o == H::Outcome::Early || o == H::Outcome::WaitedThenEarly))
							++bad;
						if (!s && (o == H::Outcome::Early || o == H::Outcome::WaitedThenEarly))
							++bad;
					}
				}
			});
			conversion.join();
			render.join();
			forRender.clear();
			forConversion.clear();
			Assert::AreEqual(0, uploadsAfterAbandon.load());
			Assert::AreEqual(0, bad.load());
			Assert::AreEqual(made.load(), released.load(), L"every slot released exactly once");
		}
	};
}
