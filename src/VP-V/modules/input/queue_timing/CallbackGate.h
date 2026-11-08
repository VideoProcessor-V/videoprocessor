// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// M2 P10-8 (fix C F2, doc 30 / doc 59 section 5.2 lifetime): the renderer's capture and file callbacks
// (OnVideoFrame, OnTimedVideoFrame) read the renderer's Impl outside every lock. Retire / FinalizeRetirementForShutdown
// / Build / the destructor reset the Impl after joining only the render and conversion threads, so a callback already
// past its stop check could run into a freed Impl. The gate closes that window: a callback holds a Use for its whole
// run; the owner calls CloseAndDrain() before it resets the Impl (new callbacks are refused, running ones finish) and
// Open() after it has built a new one.
//
// Correctness: Enter increments, THEN reads `closed`; CloseAndDrain sets `closed`, THEN waits for zero. With
// sequentially consistent atomics one of the two always sees the other (a callback either sees the gate closed and
// leaves, or CloseAndDrain sees it counted and waits for it). Lock-free; the wait yields and never holds a lock.
// A callback never calls CloseAndDrain (it would wait for itself): the owners run on the GUI, the retirement worker
// or the destructor's thread. C++14.

#include <atomic>
#include <thread>

namespace Vpv
{
	namespace Ingress
	{
		class CallbackGate
		{
		public:
			class Use
			{
			public:
				explicit Use(CallbackGate& gate) : gate_(gate)
				{
					gate_.active_.fetch_add(1);
					entered_ = !gate_.closed_.load();
					if (!entered_)
						gate_.active_.fetch_sub(1);
				}
				~Use()
				{
					if (entered_)
						gate_.active_.fetch_sub(1);
				}
				Use(const Use&) = delete;
				Use& operator=(const Use&) = delete;
				explicit operator bool() const { return entered_; }

			private:
				CallbackGate& gate_;
				bool entered_ = false;
			};

			// Refuses new callbacks and waits for the running ones to finish.
			void CloseAndDrain()
			{
				closed_.store(true);
				while (active_.load() != 0)
					std::this_thread::yield();
			}
			void Open() { closed_.store(false); }
			bool Closed() const { return closed_.load(); }
			int Active() const { return active_.load(); }

		private:
			std::atomic<int> active_{ 0 };
			std::atomic<bool> closed_{ false };
		};
	}
}
