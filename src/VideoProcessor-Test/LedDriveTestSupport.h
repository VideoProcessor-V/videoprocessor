#pragma once

// Test doubles shared by the LED drive tests: a stand-in FL35 behind a fake
// serial port.

#include <VP-V/modules/drive/projector_control/LedClock.h>
#include <led/LedDriveModule.h>
#include <VP-V/modules/drive/projector_control/SerialPort.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	// A stand-in FL35 behind a fake serial port. It answers ":MNEM..." commands
	// framed with acceptedSuffix with "%001 MNEM 000nnn" replies.
	struct FakeProjector
	{
		std::mutex mutex;
		std::condition_variable changed;
		std::vector<std::string> writes;
		std::vector<std::chrono::steady_clock::time_point> writeTimes;
		std::deque<std::string> replies;
		std::string acceptedSuffix = "\r";
		bool silent = false;
		bool holdReplies = false;
		bool openFails = false;
		// Writes fail (an unplugged adapter): nothing reaches the projector.
		bool writeFails = false;
		// audit 2026-10-16: a read fails and closes the port (as
		// Win32SerialPort::ReadLine does on a ReadFile failure).
		bool readCloses = false;
		int opens = 0;
		int light = 120;
		int contrast = 74;
		int minimum = 60;
		int maximum = 255;
		// Flaky-test fix 2026-10-18: the clock the write times are taken from (the
		// module's manual clock in the deterministic tests; the steady clock if unset).
		std::shared_ptr<const LedDrive::Clock> clock;

		static std::string Reply(const char* mnemonic, int value)
		{
			char text[32];
			sprintf_s(text, "%%001 %s %06d", mnemonic, value);
			return text;
		}

		void Receive(const std::string& bytes)
		{
			std::lock_guard<std::mutex> guard(mutex);
			writes.push_back(bytes);
			writeTimes.push_back(clock ? clock->Now() : std::chrono::steady_clock::now());
			changed.notify_all();
			if (silent || bytes.size() < acceptedSuffix.size() ||
				bytes.compare(bytes.size() - acceptedSuffix.size(), acceptedSuffix.size(), acceptedSuffix) != 0)
				return;
			const std::string body = bytes.substr(0, bytes.size() - acceptedSuffix.size());
			// "\r" framing must not also accept the literal "cr" variant.
			if (acceptedSuffix == "\r" && body.size() >= 2 && body.compare(body.size() - 2, 2, "cr") == 0)
				return;
			if (body == ":ECHO1") replies.push_back(Reply("ECHO", 1));
			else if (body == ":LPW1?") replies.push_back(Reply("LPW1", light));
			else if (body == ":LPW1 ?N") replies.push_back(Reply("LPW1", minimum));
			else if (body == ":LPW1 ?M") replies.push_back(Reply("LPW1", maximum));
			else if (body == ":CNTR?") replies.push_back(Reply("CNTR", contrast));
			else if (body.rfind(":LPW1", 0) == 0) { light = std::stoi(body.substr(5)); replies.push_back(Reply("LPW1", light)); }
			else if (body.rfind(":CNTR", 0) == 0) { contrast = std::stoi(body.substr(5)); replies.push_back(Reply("CNTR", contrast)); }
			// Any other ":MNEMvalue" (LDMM, LDCR, ...) is acknowledged with its value.
			else if (body.size() > 5 && body[0] == ':' && body.find('?') == std::string::npos &&
				std::isdigit(static_cast<unsigned char>(body[5])))
				replies.push_back(Reply(body.substr(1, 4).c_str(), std::stoi(body.substr(5))));
			changed.notify_all();
		}

		std::vector<std::string> Writes()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return writes;
		}

		std::vector<std::chrono::steady_clock::time_point> WriteTimes()
		{
			std::lock_guard<std::mutex> guard(mutex);
			return writeTimes;
		}

		bool WaitForWrites(size_t count, int timeoutMs = 2000)
		{
			std::unique_lock<std::mutex> lock(mutex);
			return changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return writes.size() >= count; });
		}

		void Release()
		{
			std::lock_guard<std::mutex> guard(mutex);
			holdReplies = false;
			changed.notify_all();
		}
	};

	class FakePort : public LedDrive::ISerialPort
	{
	public:
		explicit FakePort(std::shared_ptr<FakeProjector> projectorIn) : projector(std::move(projectorIn)) {}
		bool Open(const std::string&, int, std::string& error) override
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			++projector->opens;
			if (projector->openFails) { error = "cannot open COM9"; return false; }
			open = true;
			return true;
		}
		void Close() override { open = false; }
		bool IsOpen() const override { return open; }
		void DiscardInput() override
		{
			std::lock_guard<std::mutex> guard(projector->mutex);
			projector->replies.clear();
		}
		bool Write(const std::string& bytes, std::string& error) override
		{
			{
				std::lock_guard<std::mutex> guard(projector->mutex);
				if (projector->writeFails) { error = "write failed"; return false; }
			}
			projector->Receive(bytes);
			return true;
		}
		bool ReadLine(int timeoutMs, std::string& line) override
		{
			// audit 2026-10-16: like Win32SerialPort - a closed port fails at
			// once, and readCloses closes it on the read.
			if (!open) return false;
			std::unique_lock<std::mutex> lock(projector->mutex);
			if (projector->readCloses)
			{
				open = false;
				return false;
			}
			if (!projector->changed.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[&] { return !projector->holdReplies && !projector->replies.empty(); }))
				return false;
			line = projector->replies.front();
			projector->replies.pop_front();
			return true;
		}

	private:
		std::shared_ptr<FakeProjector> projector;
		bool open = false;
	};


	// Flaky-test fix 2026-10-18: the LED drive tests drive time explicitly. The
	// module and its driver run on this manual clock; the test moves it (frame by
	// frame, or in small steps that let the module's thread settle after each), so
	// no outcome depends on how fast the machine runs the threads. Real time is
	// only waited for things that need no time to pass (a port write and its echo),
	// with generous bounds.
	struct TestClock
	{
		std::shared_ptr<LedDrive::ManualClock> clock = std::make_shared<LedDrive::ManualClock>();
		const LedDrive::Clock::time_point origin = clock->Now();

		// The clock keeps whole nanoseconds: times compare within a microsecond.
		static constexpr double TOLERANCE_MS = 1e-3;

		operator LedDrive::ClockPtr() const { return clock; }
		// Milliseconds since the clock was made (the module's own time when the
		// module was made with it at once).
		double Ms() const { return std::chrono::duration<double, std::milli>(clock->Now() - origin).count(); }
		// Moves to `ms` after the origin (never backwards).
		void To(double ms)
		{
			clock->AdvanceTo(origin + std::chrono::duration_cast<LedDrive::Clock::time_point::duration>(
				std::chrono::duration<double, std::milli>(ms)));
		}
		// Optional: the driver has nothing to send (no target waiting, nothing in
		// flight). A driver waiting out a command gap or sequence step on the clock
		// counts as settled too (one more parked waiter).
		std::function<bool()> driverIdle;
		// `waiters` threads (the module's thread: 1) are parked on the clock having
		// seen its current time, and the driver has settled (driverIdle).
		bool Settle(int waiters = 1, int timeoutMs = 10000) const
		{
			const auto start = std::chrono::steady_clock::now();
			const auto deadline = start + std::chrono::milliseconds(timeoutMs);
			for (;;)
			{
				const int settled = clock->SettledCount();
				if (settled >= waiters && (!driverIdle || settled > waiters || driverIdle()))
				{
					// Re-checked once, so a driver that went idle while a waiter left is seen.
					if (clock->SettledCount() >= waiters) return true;
				}
				if (std::chrono::steady_clock::now() >= deadline)
				{
					char text[160];
					sprintf_s(text, "TestClock: not settled at %.3f ms (%d of %d waiters parked, driver %s)",
						Ms(), clock->SettledCount(), waiters, !driverIdle ? "-" : driverIdle() ? "idle" : "busy");
					Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(text);
					return false;
				}
				// A short spin first: a sleep is a whole timer tick (~15 ms) on Windows.
				if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5))
					std::this_thread::yield();
				else
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		// Advances `ms` in steps of stepMs, letting `waiters` threads settle after each.
		bool Run(double ms, double stepMs = 10.0, int waiters = 1)
		{
			const double end = Ms() + ms;
			if (!Settle(waiters)) return false;
			while (Ms() < end - TOLERANCE_MS)
			{
				To(std::min(end, Ms() + stepMs));
				if (!Settle(waiters)) return false;
			}
			return true;
		}
		// Runs `call` (a blocking driver call that waits out a gap on this clock) on
		// its own thread while the clock advances in steps of stepMs; each step waits
		// until that thread is parked on the clock again (or the call is done), with
		// `others` more waiters parked too (the driver's thread in a gap).
		// `call` must not assert (another thread); it records what it needs.
		bool During(const std::function<void()>& call, double stepMs = 10.0, int others = 0)
		{
			std::atomic<bool> done{ false };
			std::thread caller([&] { call(); done.store(true); });
			const std::thread::id callerId = caller.get_id();
			bool settled = true;
			while (!done.load())
			{
				const auto start = std::chrono::steady_clock::now();
				while (!done.load() && !(clock->ThreadSettled(callerId) && clock->SettledCount() >= 1 + others))
				{
					const auto waited = std::chrono::steady_clock::now() - start;
					if (waited > std::chrono::seconds(10)) { settled = false; break; }
					if (waited < std::chrono::milliseconds(5)) std::this_thread::yield();
					else std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				if (done.load()) break;
				To(Ms() + stepMs);
			}
			caller.join();
			return settled;
		}
		// Advances in steps until `condition` holds (true) or `maxMs` has passed (false).
		bool RunUntil(const std::function<bool()>& condition, double maxMs = 5000.0, double stepMs = 10.0,
			int waiters = 1)
		{
			const double end = Ms() + maxMs;
			if (!Settle(waiters)) return false;
			for (;;)
			{
				if (condition()) return true;
				if (Ms() >= end - TOLERANCE_MS) return false;
				To(std::min(end, Ms() + stepMs));
				if (!Settle(waiters)) return false;
			}
		}
	};

	// Flaky-test fix 2026-10-18: a module on the test's manual clock. The fake
	// projector stamps its writes on the same clock, and the clock's settling
	// waits for the driver to have nothing in flight (or be parked in a gap).
	inline void Bind(TestClock& clock, LedDrive::LedDriveModule& module, const std::shared_ptr<FakeProjector>& projector)
	{
		projector->clock = clock.clock;
		clock.driverIdle = [&module]
		{
			const LedDrive::DriverStatus status = module.ProjectorStatus();
			return !status.busy && !status.contrastBusy && !status.sequenceBusy;
		};
	}

	// One output frame at timeMs on the module's clock (moved there first).
	inline LedDrive::CompensationGain BeginFrameOn(TestClock& clock, LedDrive::LedDriveModule& module,
		uint64_t sequence, double timeMs, double sourceFps = 0.0)
	{
		clock.To(timeMs);
		return module.BeginFrameAt(sequence, timeMs, sourceFps);
	}
}
