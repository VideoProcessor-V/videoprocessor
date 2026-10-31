// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// capture_upload early: the hand-off of an early-uploaded slot between the
// conversion thread (which uploads AFTER it has published the payload, so the
// upload never delays the frame's readiness) and the render thread (which
// reads the slot, or takes the render upload when the upload has not started).
//
// Threading: every access to state and slot is under m. The conversion thread
// calls BeginUpload / Finish (through UploadScope); the render thread calls
// Take. The render waits only while an upload is actually running (Uploading),
// and an UploadScope guarantees Finish runs on every exit path, so a waiter is
// always released. A payload the render reaches before the upload has started
// is marked Abandoned: the conversion thread then skips its upload and the
// render uploads (the "late" fallback, counted and shown).
//
// Lifetime: the conversion thread holds its own shared_ptr to the handoff for
// the whole RunUpload call (the notify after the unlock in Finish relies on
// the object still being alive even if the render has dropped the payload).
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

class EarlyUploadHandoff
{
public:
	enum class State { Pending, Uploading, Attached, None, Abandoned };
	enum class Outcome
	{
		Early,            // the slot was attached before the render asked
		WaitedThenEarly,  // the upload was running; the render waited and got the slot
		WaitedThenNone,   // the upload was running; it produced no slot (no free slot / failed)
		Late,             // the upload had not started; abandoned now (once), the render uploads
		AbandonedAgain,   // a later draw of a payload abandoned earlier: the render uploads (not re-counted)
		NoSlot            // the upload ran and produced no slot
	};

	EarlyUploadHandoff() = default;
	EarlyUploadHandoff(const EarlyUploadHandoff&) = delete;
	EarlyUploadHandoff& operator=(const EarlyUploadHandoff&) = delete;

	// Conversion thread: true when this caller now owns the upload (and must
	// call Finish). False when the render already abandoned it.
	bool BeginUpload()
	{
		std::lock_guard<std::mutex> lock(m);
		if (state != State::Pending)
			return false;
		state = State::Uploading;
		return true;
	}

	// Conversion thread: the upload's result (null: no slot). When the state is
	// not Uploading the handle is dropped after the lock is released.
	void Finish(std::shared_ptr<void> s)
	{
		std::shared_ptr<void> dropped;   // declared before the lock: destroyed after it
		{
			std::lock_guard<std::mutex> lock(m);
			if (state == State::Uploading)
			{
				if (s)
				{
					slot = std::move(s);
					state = State::Attached;
				}
				else
					state = State::None;
			}
			else
				dropped = std::move(s);
		}
		cv.notify_all();
	}

	// Calls Finish(nullptr) on scope exit unless Finish was called through it.
	class UploadScope
	{
	public:
		explicit UploadScope(EarlyUploadHandoff& handoff) : h(handoff) {}
		UploadScope(const UploadScope&) = delete;
		UploadScope& operator=(const UploadScope&) = delete;
		~UploadScope()
		{
			if (!done)
				h.Finish(nullptr);
		}
		void Finish(std::shared_ptr<void> s)
		{
			done = true;
			h.Finish(std::move(s));
		}
	private:
		EarlyUploadHandoff& h;
		bool done = false;
	};

	// Conversion thread: runs `upload` (returns the slot handle, null for none)
	// when the render has not abandoned the payload; Finish runs on every path,
	// including an exception. Returns false when the upload was skipped.
	template <class F>
	bool RunUpload(F&& upload) noexcept
	{
		if (!BeginUpload())
			return false;
		UploadScope scope(*this);
		std::shared_ptr<void> s;
		try
		{
			s = upload();
		}
		catch (...)
		{
			s = nullptr;
		}
		scope.Finish(std::move(s));
		return true;
	}

	// Render thread: the slot (a copy; a payload may be drawn several times) or
	// null. Never blocks unless an upload is running. firstTake: true on the
	// payload's first Take only (count per payload, not per draw).
	std::shared_ptr<void> Take(Outcome* outcome, bool* firstTake = nullptr)
	{
		std::unique_lock<std::mutex> lock(m);
		if (firstTake)
			*firstTake = !taken;
		taken = true;
		bool waited = false;
		if (state == State::Pending)
		{
			state = State::Abandoned;
			if (outcome)
				*outcome = Outcome::Late;
			return nullptr;
		}
		if (state == State::Uploading)
		{
			waited = true;
			cv.wait(lock, [this] { return state == State::Attached || state == State::None; });
		}
		if (state == State::Attached)
		{
			if (outcome)
				*outcome = waited ? Outcome::WaitedThenEarly : Outcome::Early;
			return slot;
		}
		if (outcome)
			*outcome = waited ? Outcome::WaitedThenNone : (state == State::Abandoned ? Outcome::AbandonedAgain : Outcome::NoSlot);
		return nullptr;
	}

	State Peek() const
	{
		std::lock_guard<std::mutex> lock(m);
		return state;
	}

private:
	mutable std::mutex m;
	std::condition_variable cv;
	State state = State::Pending;
	std::shared_ptr<void> slot;
	bool taken = false;   // render thread, under m
};
