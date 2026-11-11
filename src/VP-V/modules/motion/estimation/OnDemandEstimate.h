// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// W2-C5b (doc 93 section 3; design prep refs\93-prep-C5b.md): the motion engine's ON-DEMAND estimate entry. When no pair was
// computed ahead (low latency: no look-ahead; a standard-mode miss; a hiccup gap pair), a consumer asks the ONE optical-flow
// session - the look-ahead producer's (MotionAheadProducer) - for a pair NOW. The producer's worker takes such a request
// before its queued look-ahead jobs, at its existing idle wait (its mutex + condition variable; no new thread, lock or
// session). The result is the producer's own Flow (MotionAheadProducer.h), so the matcher consumes it exactly like a pair
// computed ahead (SetExternalFlow). A request that misses its deadline is dropped before any GPU work (abandoned) and the
// consumer takes the matcher-only path, visibly (FallbackCatalog::NoPairCause::OnDemandLate).
//
// This header holds the request / result types and the request queue as plain logic (no D3D, no host types): the producer
// owns one queue under its mutex; OnDemandEstimateTests drive it directly and through the producer's test engine seam.
// C++14 (VideoProcessor-Lib and the Test project compile at C++14).
#pragma once

#include <AnalysisLumaSource.h>
#include <VP-V/modules/motion/estimation/VectorDemand.h>
#include <VP-V/modules/motion/estimation/VectorProducts.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

namespace VpMotion
{
	namespace OnDemand
	{
		using Clock = std::chrono::steady_clock;

		// One pair asked for: frames `from` -> `to` (to > from; to == from + 1 = an adjacent pair, else a gap pair) of the
		// continuity generation `generation`, with both frames' CPU luma (the queued frames' own pixels; the sources'
		// backingOwner keeps them alive while the request waits). chainOk: no break the sequence does not show lies between
		// the producer's last frame and `from` (false = never pair against the producer's held frame). budgetMs: how long
		// the asker can wait from now (derived per frame by the caller; <= 0 = no budget: the request is not issued).
		struct PairRequest
		{
			uint64_t from = 0, to = 0, generation = 0;
			AnalysisLumaSource lumaFrom, lumaTo;
			bool chainOk = true;
			double budgetMs = 0.0;
			VectorDemand::Consumer who = VectorDemand::Consumer::DejudderExtras;
			bool Adjacent() const { return to == from + 1; }
		};

		enum class Status : unsigned char
		{
			Pending,       // not answered yet (never returned by Get)
			Ready,         // computed for this request (flow set)
			FromSlot,      // the pair was already in the P2 slot (flow set; zero work)
			Late,          // the deadline passed or the asker gave up before the worker took it: no GPU work was done
			Refused,       // pushed out by newer requests (the queue holds kMaxPending) or not a pair (to <= from)
			Failed,        // the engine ran and failed (counted like a failed look-ahead job)
			Unavailable,   // no producer / not running / stopping / restarting (zero side effects)
			NoBudget,      // budgetMs <= 0: not issued
		};
		inline const char* StatusName(Status s)
		{
			switch (s)
			{
			case Status::Pending: return "pending";
			case Status::Ready: return "ready";
			case Status::FromSlot: return "from slot";
			case Status::Late: return "late";
			case Status::Refused: return "refused";
			case Status::Failed: return "failed";
			case Status::Unavailable: return "unavailable";
			case Status::NoBudget: return "no budget";
			}
			return "?";
		}

		struct PairResult
		{
			Status status = Status::Pending;
			MotionAhead::FlowPtr flow;   // set for Ready / FromSlot
			double queuedMs = 0.0;       // issue -> the worker took it
			double runMs = 0.0;          // the worker's time on it (luma, uploads, engine, readback, extras)
		};

		// The shared state of one request: the promise the worker fulfils once, and the asker's "gave up" flag.
		struct RequestState
		{
			std::promise<PairResult> promise;
			std::atomic<bool> abandoned{ false };
			std::atomic<bool> fulfilled{ false };
			std::mutex once;   // fulfils at most once (a promise throws on a second set_value)
			Clock::time_point issued, deadline;
		};

		// Fulfils the request once; later calls are no-ops. False = it was already answered.
		inline bool Fulfil(RequestState& state, PairResult result)
		{
			std::lock_guard<std::mutex> lock(state.once);
			if (state.fulfilled.load(std::memory_order_acquire))
				return false;
			state.promise.set_value(std::move(result));
			state.fulfilled.store(true, std::memory_order_release);
			return true;
		}

		// The asker's handle: wait with a deadline, give up (the worker then skips it before any GPU work).
		class PairFuture
		{
		public:
			PairFuture() {}
			// Takes the promise's future: construct ONCE per RequestState (copies of the handle share it).
			explicit PairFuture(std::shared_ptr<RequestState> state) : state_(std::move(state))
			{
				if (state_) future_ = state_->promise.get_future().share();
			}
			// An answered handle (FromSlot / Unavailable / NoBudget / Refused at issue).
			static PairFuture Answered(Status status, MotionAhead::FlowPtr flow = nullptr)
			{
				auto state = std::make_shared<RequestState>();
				state->issued = state->deadline = Clock::now();
				PairFuture f(state);
				PairResult r;
				r.status = status;
				r.flow = std::move(flow);
				Fulfil(*state, std::move(r));
				return f;
			}
			bool Valid() const { return state_ != nullptr && future_.valid(); }
			bool Ready() const { return Valid() && future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }
			// Waits up to `ms` (<= 0: no wait). False = not answered in time (the result is not touched).
			bool WaitFor(double ms) const
			{
				if (!Valid()) return false;
				if (ms <= 0.0) return Ready();
				return future_.wait_for(std::chrono::microseconds(static_cast<long long>(ms * 1000.0))) == std::future_status::ready;
			}
			// The answer (call after WaitFor / Ready returned true). An invalid handle answers Unavailable.
			PairResult Get() const
			{
				if (!Valid()) { PairResult r; r.status = Status::Unavailable; return r; }
				return future_.get();
			}
			// The asker gives up: a request the worker has not taken yet is skipped with no GPU work.
			void Abandon() const { if (state_) state_->abandoned.store(true, std::memory_order_release); }
			const std::shared_ptr<RequestState>& State() const { return state_; }

		private:
			std::shared_ptr<RequestState> state_;
			std::shared_future<PairResult> future_;
		};

		// One queued request (the worker's side).
		struct Pending
		{
			PairRequest request;
			std::shared_ptr<RequestState> state;
		};

		// The producer's priority queue of requests. NOT locked: the producer holds its own mutex around every call.
		class Queue
		{
		public:
			static constexpr size_t kMaxPending = 2;   // one for the frame being rendered, one for a gap fill beside it

			// Queues a request; when kMaxPending are already waiting, the OLDEST is answered Refused (counted, visible).
			PairFuture Push(PairRequest request, Clock::time_point now, unsigned long long* refused = nullptr)
			{
				auto state = std::make_shared<RequestState>();
				state->issued = now;
				const double budget = request.budgetMs > 0.0 ? request.budgetMs : 0.0;
				state->deadline = now + std::chrono::microseconds(static_cast<long long>(budget * 1000.0));
				PairFuture future(state);
				while (pending_.size() >= kMaxPending)
				{
					PairResult r;
					r.status = Status::Refused;
					Fulfil(*pending_.front().state, std::move(r));
					pending_.pop_front();
					if (refused) ++*refused;
				}
				Pending p;
				p.request = std::move(request);
				p.state = std::move(state);
				pending_.push_back(std::move(p));
				return future;
			}
			bool Empty() const { return pending_.empty(); }
			size_t Size() const { return pending_.size(); }
			// The oldest request, removed. False = none.
			bool Pop(Pending& out)
			{
				if (pending_.empty()) return false;
				out = std::move(pending_.front());
				pending_.pop_front();
				return true;
			}
			// Every waiting request answered `status` (Stop, a failure stop): never a broken promise.
			size_t FailAll(Status status)
			{
				const size_t n = pending_.size();
				for (auto& p : pending_)
				{
					PairResult r;
					r.status = status;
					Fulfil(*p.state, std::move(r));
				}
				pending_.clear();
				return n;
			}

		private:
			std::deque<Pending> pending_;
		};

		// How the worker runs a request against the producer's frame chain (the lumaTex ping-pong its look-ahead jobs use).
		enum class Mode : unsigned char
		{
			Chain,      // the chain holds `from` (the last request's or job's `to`): upload `to` only, one engine run (the steady case)
			Adopt,     // the chain is empty, broken, of another generation or behind `from`: upload both into the chain textures,
			            // then the chain holds `to` (so the next adjacent request is a Chain run)
			OffChain,   // the chain holds a frame AHEAD of `from` (the look-ahead walk runs ahead): upload both into separate request
			            // textures; the chain is untouched (the walk is never disturbed)
		};
		struct ChainState
		{
			bool havePrevious = false;
			uint64_t previousSequence = 0, previousGeneration = 0;
			uint64_t slotGeneration = 0;   // the P2 slot's generation (a request of another one resets the chain first, as a job does)
		};
		inline Mode ChooseMode(const ChainState& chain, const PairRequest& request)
		{
			const bool sameGeneration = chain.havePrevious && chain.previousGeneration == request.generation &&
				chain.slotGeneration == request.generation;
			if (!sameGeneration)
				return Mode::Adopt;
			if (request.chainOk && chain.previousSequence == request.from)
				return Mode::Chain;   // a gap pair too (temporal hints only for an adjacent one)
			if (chain.previousSequence > request.from)
				return Mode::OffChain;
			return Mode::Adopt;
		}
		inline const char* ModeName(Mode m)
		{
			return m == Mode::Chain ? "chain" : m == Mode::Adopt ? "adopt" : "off-chain";
		}

		inline double MsBetween(Clock::time_point a, Clock::time_point b)
		{
			return std::chrono::duration<double, std::milli>(b - a).count();
		}
	}
}
