#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// ABI 35 (VP Renderer DirectShow filter): the slot wait behind
// IVideoRenderer::WaitForFileFrameSlot / CancelFileFrameWait.
//
// A file source never drops: the filter's Receive waits here until the
// renderer's queue has room. The gate has its own mutex, so a waiter never
// holds the queue mutex while it sleeps; the room test (a callable) takes the
// queue mutex only for the moment it reads the queue. A sequence number closes
// the lost-wakeup window between that test and the sleep: Notify() after a
// frame leaves the queue always wakes a waiter that tested before it.
//
// Cancel() wakes every waiter with false and stays in force (sticky) until the
// next Arm(), which the renderer calls on the next timed frame. Pure standard
// library, C++14, no Windows types (unit tested).
class FileFrameSlotGate
{
public:
	// Win32 INFINITE convention: wait without a deadline.
	static constexpr uint32_t kInfinite = 0xFFFFFFFFu;

	// The next timed frame arrived: waits block again.
	void Arm()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_cancelled = false;
	}

	// Flush or stop: every wait returns false now and until the next Arm().
	void Cancel()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_cancelled = true;
			++m_sequence;
		}
		m_changed.notify_all();
	}

	// Room may have appeared (a frame left the queue, the queue was reset or
	// resized, the renderer started rendering).
	void Notify()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_sequence;
		}
		m_changed.notify_all();
	}

	bool Cancelled() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_cancelled;
	}

	// True as soon as hasRoom() reports room; false on timeout or cancel.
	// hasRoom is called without the gate's mutex held.
	template <class HasRoom>
	bool Wait(uint32_t timeoutMs, HasRoom hasRoom)
	{
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeoutMs == kInfinite ? 0 : timeoutMs);
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;)
		{
			if (m_cancelled)
				return false;
			const uint64_t seen = m_sequence;
			lock.unlock();
			const bool room = hasRoom();
			lock.lock();
			if (m_cancelled)
				return false;
			if (room)
				return true;
			const auto changed = [this, seen]() { return m_cancelled || m_sequence != seen; };
			if (timeoutMs == kInfinite)
				m_changed.wait(lock, changed);
			else if (!m_changed.wait_until(lock, deadline, changed))
				return false;   // timed out with no change
		}
	}

private:
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	uint64_t m_sequence = 0;
	bool m_cancelled = false;
};
