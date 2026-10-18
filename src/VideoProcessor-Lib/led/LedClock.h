#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// The LED drive's time source (flaky-test fix 2026-10-18): the module's and the
// RS-232 driver's threads read the time and wait for deadlines through it, so
// tests can drive time explicitly instead of depending on the wall clock.
// Production always uses SteadyClock(), which is exactly std::chrono::steady_clock
// and condition_variable::wait_until - behaviour is unchanged.
//
// I/O timeouts (a serial read's ack_timeout_ms) stay on the real clock: they bound
// how long a port read may block, not when the drive acts.
namespace LedDrive
{
	class Clock
	{
	public:
		using time_point = std::chrono::steady_clock::time_point;
		virtual ~Clock() = default;
		virtual time_point Now() const = 0;
		// cv.wait_until(lock, when, pred) on this clock: returns pred().
		virtual bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
			time_point when, const std::function<bool()>& pred) const = 0;
		// cv.wait_until(lock, when) on this clock: one wait that may also end on a
		// notification or spuriously; callers loop and re-check Now().
		virtual void WaitOnce(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
			time_point when) const = 0;
	};

	using ClockPtr = std::shared_ptr<const Clock>;

	class SteadyClockSource final : public Clock
	{
	public:
		time_point Now() const override { return std::chrono::steady_clock::now(); }
		bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
			time_point when, const std::function<bool()>& pred) const override
		{
			return cv.wait_until(lock, when, pred);
		}
		void WaitOnce(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
			time_point when) const override
		{
			cv.wait_until(lock, when);
		}
	};

	// The production clock (one shared instance).
	inline ClockPtr SteadyClock()
	{
		static const ClockPtr clock = std::make_shared<SteadyClockSource>();
		return clock;
	}

	// Tests and replays: time moves only when Advance()/AdvanceTo() is called.
	// Advance() wakes every thread waiting on this clock, and a waiter also
	// re-checks the time every 2 ms of real time (a backstop for a wake-up that
	// races its wait), so its outcome depends only on the manual time.
	// Settled(n) tells a test when n threads are parked on the clock having seen
	// its current time (ThreadSettled: one given thread) - nothing time-driven is
	// left to happen in them before the next advance.
	class ManualClock final : public Clock
	{
	public:
		// Starts well after the steady clock's epoch, so a default (zero) time_point
		// keeps meaning "never" (DriverStatus).
		ManualClock() : now(time_point(std::chrono::hours(1))) {}

		time_point Now() const override
		{
			std::lock_guard<std::mutex> guard(state);
			return now;
		}
		double NowMs() const
		{
			return std::chrono::duration<double, std::milli>(Now().time_since_epoch()).count();
		}
		bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
			time_point when, const std::function<bool()>& pred) const override
		{
			Waiter waiter{ &cv, std::this_thread::get_id() };
			bool result = true;
			for (;;)
			{
				if (pred()) break;
				if (!Park(waiter, when)) { result = pred(); break; }
				cv.wait_for(lock, std::chrono::milliseconds(2));
			}
			Leave(waiter);
			return result;
		}
		void WaitOnce(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
			time_point when) const override
		{
			Waiter waiter{ &cv, std::this_thread::get_id() };
			if (Park(waiter, when)) cv.wait_for(lock, std::chrono::milliseconds(2));
			Leave(waiter);
		}

		void Advance(double ms)
		{
			AdvanceTo(Now() + std::chrono::duration_cast<time_point::duration>(
				std::chrono::duration<double, std::milli>(ms)));
		}
		void AdvanceTo(time_point when)
		{
			std::lock_guard<std::mutex> guard(state);
			if (when <= now) return;
			now = when;
			++epoch;
			for (const Waiter* waiter : parked) waiter->cv->notify_all();
		}
		// True once `count` waiters are parked having seen the current time
		// (polled in real time, up to timeoutMs).
		bool Settled(int count, int timeoutMs = 10000) const
		{
			const auto start = std::chrono::steady_clock::now();
			const auto deadline = start + std::chrono::milliseconds(timeoutMs);
			for (;;)
			{
				if (SettledCount() >= count) return true;
				if (std::chrono::steady_clock::now() >= deadline) return false;
				// A short spin first: a sleep is a whole timer tick (~15 ms) on Windows.
				if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5))
					std::this_thread::yield();
				else
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		// Waiters parked having seen the current time, now (no waiting).
		int SettledCount() const
		{
			std::lock_guard<std::mutex> guard(state);
			int count = 0;
			for (const Waiter* waiter : parked)
				if (waiter->seenEpoch == epoch) ++count;
			return count;
		}
		// The given thread is parked having seen the current time.
		bool ThreadSettled(std::thread::id thread) const
		{
			std::lock_guard<std::mutex> guard(state);
			for (const Waiter* waiter : parked)
				if (waiter->thread == thread && waiter->seenEpoch == epoch) return true;
			return false;
		}

	private:
		struct Waiter
		{
			std::condition_variable* cv;
			std::thread::id thread;
			long long seenEpoch = -1;
			bool registered = false;
		};
		// False when `when` has come; otherwise records the waiter as parked.
		bool Park(Waiter& waiter, time_point when) const
		{
			std::lock_guard<std::mutex> guard(state);
			if (now >= when) return false;
			if (!waiter.registered) { parked.push_back(&waiter); waiter.registered = true; }
			waiter.seenEpoch = epoch;
			return true;
		}
		void Leave(Waiter& waiter) const
		{
			std::lock_guard<std::mutex> guard(state);
			if (!waiter.registered) return;
			parked.erase(std::find(parked.begin(), parked.end(), &waiter));
		}

		mutable std::mutex state;
		time_point now;
		long long epoch = 0;
		mutable std::vector<const Waiter*> parked;
	};
}
