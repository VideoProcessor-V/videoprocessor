// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P10-8 (fix C F2): the capture / file callback gate. A callback that entered keeps the owner's CloseAndDrain
// waiting until it leaves; after CloseAndDrain no callback enters until Open. The stress test plays the renderer:
// a "capture thread" reads a heap object inside Uses while the owner closes, frees, rebuilds and reopens it
// (doc 59 section 5.3 CaptureExtensionLifetimeTests form, 1e5 iterations). CPU only.
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/queue_timing/CallbackGate.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(CallbackGateTests)
	{
	public:
		TEST_METHOD(ClosedGateRefusesAndOpenGateAdmits)
		{
			Vpv::Ingress::CallbackGate gate;
			{
				Vpv::Ingress::CallbackGate::Use use(gate);
				Assert::IsTrue(static_cast<bool>(use));
				Assert::AreEqual(1, gate.Active());
			}
			Assert::AreEqual(0, gate.Active());
			gate.CloseAndDrain();
			{
				Vpv::Ingress::CallbackGate::Use use(gate);
				Assert::IsFalse(static_cast<bool>(use));
				Assert::AreEqual(0, gate.Active(), L"a refused use leaves no count");
			}
			gate.Open();
			Vpv::Ingress::CallbackGate::Use use(gate);
			Assert::IsTrue(static_cast<bool>(use));
		}

		TEST_METHOD(DrainWaitsForTheCallbackInside)
		{
			Vpv::Ingress::CallbackGate gate;
			std::atomic<bool> inside{ false }, release{ false }, drained{ false };
			std::thread callback([&]
			{
				Vpv::Ingress::CallbackGate::Use use(gate);
				inside.store(true);
				while (!release.load())
					std::this_thread::yield();
			});
			while (!inside.load())
				std::this_thread::yield();
			std::thread owner([&] { gate.CloseAndDrain(); drained.store(true); });
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			Assert::IsFalse(drained.load(), L"the owner waits while a callback is inside");
			release.store(true);
			callback.join();
			owner.join();
			Assert::IsTrue(drained.load());
		}

		TEST_METHOD(OwnerFreeingTheStateNeverRacesACallback)
		{
			struct State { std::atomic<int> alive{ 0x5eed }; };
			Vpv::Ingress::CallbackGate gate;
			std::shared_ptr<State> holder = std::make_shared<State>();
			State* raw = holder.get();   // what the callback reads (no ownership, as m_impl-> today)
			std::atomic<State*> current{ raw };
			std::atomic<bool> stop{ false };
			std::atomic<int> bad{ 0 };
			std::thread capture([&]
			{
				while (!stop.load())
				{
					Vpv::Ingress::CallbackGate::Use use(gate);
					if (!use)
						continue;
					State* s = current.load();
					if (!s || s->alive.load() != 0x5eed)
						bad.fetch_add(1);
				}
			});
			for (int i = 0; i < 100000; ++i)
			{
				gate.CloseAndDrain();
				current.store(nullptr);
				holder->alive.store(0);   // "freed"
				holder = std::make_shared<State>();
				current.store(holder.get());
				gate.Open();
			}
			stop.store(true);
			capture.join();
			Assert::AreEqual(0, bad.load(), L"no callback ever saw a freed or missing state");
		}
	};
}
