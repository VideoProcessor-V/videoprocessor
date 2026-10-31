// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// capture_upload early: the conversion/render hand-off (CPU only, no GPU).
#include "pch.h"
#include <VP-V/modules/input/capture/EarlyUploadHandoff.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

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
			Assert::IsTrue(o == H::Outcome::Late);
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
	};
}
