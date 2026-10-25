#pragma once

#include <DebugLog.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// VP Renderer filter (2026-10-17): MPC-HC / MPC-BE rebuild the filter graph
// for every file, so the renderer - and with it the LED drive - is torn down
// and made again between files. Stopping the drive restores full light, which
// would flash the projector to full brightness at every file change. In the
// filter process the outgoing renderer parks its running drive here instead
// (file_led_holdover_ms); the next renderer adopts it when its LED settings are
// the same, carrying on from the current LED level. When no renderer adopts it
// in time, or the settings differ, the parked drive is stopped through its
// normal shutdown path (LedDriveModule::Stop: LDMM 0, LPW1 to the ceiling,
// CNTR 50), so CNTR is never left above need for longer than the hold-over.
//
// VideoProcessor.exe never parks (its renderer only parks for source=file), so
// its LED drive is unchanged. The drive keeps the port - and the LED drive
// mutex (LedDriveArbiter.h) - while parked.
namespace LedDrive
{
	// The parking slot alone: no threads, explicit time (tests).
	template <typename Drive>
	class HoldoverSlot
	{
	public:
		using Ptr = std::shared_ptr<Drive>;

		// Parks `drive` until nowMs + holdMs. Returns the drive parked before
		// (the caller stops it), if any.
		Ptr Park(Ptr drive, const std::string& signature, uint64_t nowMs, uint32_t holdMs)
		{
			Ptr previous = std::move(parked);
			parked = std::move(drive);
			parkedSignature = signature;
			deadlineMs = nowMs + holdMs;
			return previous;
		}

		// The parked drive when the settings match and its time is not up; the
		// slot is then empty. Otherwise null, and `stale` receives a parked drive
		// that can no longer be adopted (the caller stops it).
		Ptr Adopt(const std::string& signature, uint64_t nowMs, Ptr& stale)
		{
			stale.reset();
			if (!parked)
				return Ptr();
			if (nowMs >= deadlineMs || signature != parkedSignature)
			{
				stale = Take();
				return Ptr();
			}
			return Take();
		}

		// The parked drive once its time is up (the caller stops it).
		Ptr Expire(uint64_t nowMs)
		{
			return parked && nowMs >= deadlineMs ? Take() : Ptr();
		}

		Ptr Take()
		{
			Ptr drive = std::move(parked);
			parked.reset();
			parkedSignature.clear();
			deadlineMs = 0;
			return drive;
		}

		bool Parked() const { return parked != nullptr; }
		uint64_t DeadlineMs() const { return deadlineMs; }

	private:
		Ptr parked;
		std::string parkedSignature;
		uint64_t deadlineMs = 0;
	};

	// The process-wide holder: the slot, plus a thread that stops a parked
	// drive at its deadline. `stop` stops a drive (restoring full light); it is
	// always called without the holder's lock. Tests pass a fake clock and
	// runThread false and call Poll().
	template <typename Drive>
	class Holdover
	{
	public:
		using Ptr = std::shared_ptr<Drive>;
		using Clock = std::function<uint64_t()>;
		using StopDrive = std::function<void(const Ptr&)>;

		Holdover(Clock clockIn, StopDrive stopIn, bool runThread = true)
			: clock(std::move(clockIn)), stop(std::move(stopIn)), threaded(runThread)
		{
		}

		~Holdover()
		{
			{
				std::lock_guard<std::mutex> guard(mutex);
				ending = true;
				wake.notify_all();
			}
			if (thread.joinable())
				thread.join();
			StopDriveNow(slot.Take(), "the holder ended");
		}

		Holdover(const Holdover&) = delete;
		Holdover& operator=(const Holdover&) = delete;

		// The outgoing renderer: keeps `drive` running for holdMs (0 = stops it now).
		void Park(Ptr drive, const std::string& signature, uint32_t holdMs)
		{
			if (!drive)
				return;
			if (holdMs == 0)
			{
				StopDriveNow(drive, "file_led_holdover_ms is 0");
				return;
			}
			Ptr previous;
			{
				std::lock_guard<std::mutex> guard(mutex);
				previous = slot.Park(std::move(drive), signature, clock(), holdMs);
				EnsureThreadLocked();
				wake.notify_all();
			}
			DebugLog::Log("LED drive: renderer gone; holding the LED drive for %u ms for the next file", holdMs);
			StopDriveNow(previous, "a newer drive was parked");
		}

		// The incoming renderer: the parked drive when its settings match.
		Ptr Adopt(const std::string& signature)
		{
			Ptr stale;
			Ptr adopted;
			bool wasParked = false;
			{
				std::lock_guard<std::mutex> guard(mutex);
				wasParked = slot.Parked();
				adopted = slot.Adopt(signature, clock(), stale);
				wake.notify_all();
			}
			if (adopted)
				DebugLog::Log("LED drive: the new renderer adopted the held LED drive (no return to full light)");
			else if (wasParked)
				StopDriveNow(stale, "the new renderer's LED settings differ or the hold-over had ended");
			return adopted;
		}

		// Stops a parked drive at once (the filter is going away). True when one was parked.
		bool EndNow(const char* reason)
		{
			Ptr drive;
			{
				std::lock_guard<std::mutex> guard(mutex);
				drive = slot.Take();
			}
			const bool had = drive != nullptr;
			StopDriveNow(drive, reason);
			return had;
		}

		// Stops the parked drive if its time is up (the thread calls this; tests call it directly).
		bool Poll()
		{
			Ptr expired;
			{
				std::lock_guard<std::mutex> guard(mutex);
				expired = slot.Expire(clock());
			}
			const bool had = expired != nullptr;
			StopDriveNow(expired, "no new renderer arrived in time");
			return had;
		}

		bool Parked() const
		{
			std::lock_guard<std::mutex> guard(mutex);
			return slot.Parked();
		}

	private:
		void StopDriveNow(const Ptr& drive, const char* reason)
		{
			if (!drive)
				return;
			DebugLog::Log("LED drive: ending the held LED drive (%s); restoring full light", reason ? reason : "");
			if (stop)
				stop(drive);
		}

		void EnsureThreadLocked()
		{
			if (!threaded || thread.joinable())
				return;
			thread = std::thread([this] { Run(); });
		}

		void Run()
		{
			std::unique_lock<std::mutex> guard(mutex);
			while (!ending)
			{
				if (!slot.Parked())
				{
					wake.wait(guard);
					continue;
				}
				const uint64_t now = clock();
				const uint64_t deadline = slot.DeadlineMs();
				if (now < deadline)
				{
					wake.wait_for(guard, std::chrono::milliseconds(deadline - now));
					continue;
				}
				Ptr expired = slot.Take();
				guard.unlock();
				StopDriveNow(expired, "no new renderer arrived in time");
				guard.lock();
			}
		}

		Clock clock;
		StopDrive stop;
		const bool threaded;
		mutable std::mutex mutex;
		std::condition_variable wake;
		std::thread thread;
		bool ending = false;
		HoldoverSlot<Drive> slot;
	};

	class LedDriveModule;

	// The process-wide holder for the LED drive module (one per module that
	// links Lib: the VP Renderer DLL owns the drive). Never destroyed, so no
	// thread is joined or projector command sent while the process unloads;
	// the filter calls EndHeldLedDrive() when it is going away.
	Holdover<LedDriveModule>& ProcessLedDriveHoldover();
	bool EndHeldLedDrive(const char* reason);
}
