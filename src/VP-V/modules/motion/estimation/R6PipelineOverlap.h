// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// R6 (0a) 4K producer throughput: bounded single-thread asynchronous extras worker that overlaps
// CPU FlowGlobals + FlowExtras + P2Slot publish of pair N with LumaAtTarget(N+1) + NVOF RunPair(N+1)
// on the MotionAhead::Producer thread.
//
// Threading & ordering contract (Opus 2A reviewable):
//   * Single FIFO worker thread: started ONLY when opt-in R6 async extras is enabled (default OFF).
//   * Queue capacity is derived from the caller's pending-job capacity (never a fixed rate literal).
//   * If the queue is full (worker fell behind at 4K), Submit() runs the job synchronously on the
//     calling thread AFTER draining nothing? Wait: to preserve strict FIFO publish order into P2Slot,
//     when full we flush the queue or wait briefly? Even better: never publish out of order --
//     when queue >= capacity, count `syncFallback`, call `Flush()` to preserve strict FIFO order,
//     and execute the job synchronously (or enqueue cleanly once drained). No pair is EVER dropped
//     and publish order is ALWAYS strictly monotonic!
//   * Flush() / Stop() drains all queued pairs before returning (used on generation changes and Stop()).
//   * Every fallback to synchronous execution is counted visibly in R6ExtrasStats.
//
// Pure C++14, deterministic (output is bit-identical to synchronous FlowGlobals + FlowExtras).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace MotionAhead
{
	namespace R6
	{
		struct ExtrasStats
		{
			uint64_t asyncCompleted = 0;
			uint64_t syncFallbacks = 0;    // queue full or worker not running -> ran synchronously in FIFO order
			uint64_t flushes = 0;
			bool running = false;
			size_t capacity = 0;
		};

		// Bounded single-thread FIFO worker for overlapping CPU post-readback extras with the next NVOF pair.
		class ExtrasWorker
		{
		public:
			ExtrasWorker() = default;
			~ExtrasWorker() { Stop(); }
			ExtrasWorker(const ExtrasWorker&) = delete;
			ExtrasWorker& operator=(const ExtrasWorker&) = delete;

			bool Start(size_t queueCapacity)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_running)
					return false;
				m_capacity = queueCapacity < 1 ? 1 : queueCapacity;
				m_stop = false;
				m_running = true;
				m_thread = std::thread([this]() { Loop(); });
				return true;
			}

			void SetCapacity(size_t queueCapacity)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_capacity = queueCapacity < 1 ? 1 : queueCapacity;
			}

			void Stop()
			{
				Flush();
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					if (!m_running)
						return;
					m_stop = true;
				}
				m_cv.notify_all();
				if (m_thread.joinable())
					m_thread.join();
				std::lock_guard<std::mutex> lock(m_mutex);
				m_running = false;
			}

			bool Running() const
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				return m_running;
			}

			// Waits until every previously submitted job has completed (preserves strict FIFO ordering on
			// generation changes, shutdown, or sync fallback).
			void Flush()
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				if (!m_running)
					return;
				++m_flushes;
				m_idleCv.wait(lock, [this]() { return (m_queue.empty() && !m_busy) || !m_running; });
			}

			// Enqueues `fn` to run on the extras thread. If the worker is stopped or the bounded queue is full,
			// flushes earlier queued jobs first (preserving strict FIFO publish order), counts the fallback visibly,
			// and runs `fn` synchronously on the calling thread. Returns true when queued asynchronously.
			bool Submit(std::function<void()> fn)
			{
				if (!fn)
					return false;
				bool runSync = false;
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					if (!m_running || m_stop)
					{
						++m_syncFallbacks;
						runSync = true;
					}
					else if (m_queue.size() >= m_capacity)
					{
						// Bounded queue full: drain in-flight jobs to preserve strict FIFO order, then run sync visibly.
						++m_syncFallbacks;
						++m_flushes;
						m_idleCv.wait(lock, [this]() { return (m_queue.empty() && !m_busy) || !m_running; });
						runSync = true;
					}
					else
					{
						m_queue.push_back(std::move(fn));
					}
				}
				if (runSync)
				{
					fn();
					return false;
				}
				m_cv.notify_one();
				return true;
			}

			ExtrasStats Stats() const
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				ExtrasStats s;
				s.asyncCompleted = m_asyncCompleted;
				s.syncFallbacks = m_syncFallbacks;
				s.flushes = m_flushes;
				s.running = m_running;
				s.capacity = m_capacity;
				return s;
			}

			static std::string StatusLine(const ExtrasStats& s)
			{
				if (!s.running && s.asyncCompleted == 0 && s.syncFallbacks == 0)
					return "motion ahead R6 async extras: off (default synchronous path)";
				char b[200];
				std::snprintf(b, sizeof(b), "motion ahead R6 async extras (%s, cap %zu): %llu overlapped, %llu sync fallback, %llu flushes",
					s.running ? "on" : "stopped", s.capacity,
					static_cast<unsigned long long>(s.asyncCompleted),
					static_cast<unsigned long long>(s.syncFallbacks),
					static_cast<unsigned long long>(s.flushes));
				return b;
			}

		private:
			void Loop()
			{
				for (;;)
				{
					std::function<void()> fn;
					{
						std::unique_lock<std::mutex> lock(m_mutex);
						m_busy = false;
						m_idleCv.notify_all();
						m_cv.wait(lock, [this]() { return m_stop || !m_queue.empty(); });
						if (m_stop && m_queue.empty())
							return;
						fn = std::move(m_queue.front());
						m_queue.pop_front();
						m_busy = true;
					}
					fn();
					std::lock_guard<std::mutex> lock(m_mutex);
					++m_asyncCompleted;
				}
			}

			mutable std::mutex m_mutex;
			std::condition_variable m_cv, m_idleCv;
			std::deque<std::function<void()>> m_queue;
			size_t m_capacity = 4;
			bool m_running = false;
			bool m_stop = false;
			bool m_busy = false;
			uint64_t m_asyncCompleted = 0;
			uint64_t m_syncFallbacks = 0;
			uint64_t m_flushes = 0;
			std::thread m_thread;
		};
	}
}
