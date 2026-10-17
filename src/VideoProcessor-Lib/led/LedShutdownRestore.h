#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <thread>

// Integration 2026-10-17: the forced shutdown (a renderer whose retirement is
// blocked is leaked at the deadline) must never leave the projector dimmed or
// CNTR high. The LED drive is stopped on its own thread - stopping restores
// full light (LDMM 0 when a dim mode was used, LPW1 to the ceiling, CNTR 50
// when contrast was driven or last sent at anything else) - and the caller
// waits at most `timeoutMs` (the blocked renderer may hold its locks). A
// timed-out stop keeps running detached; the process exit ends it.
namespace LedDrive
{
	enum class BoundedRestoreResult { Restored, NotConfirmed, Failed, TimedOut };

	inline const char* Name(BoundedRestoreResult result)
	{
		switch (result)
		{
		case BoundedRestoreResult::Restored: return "restored";
		case BoundedRestoreResult::NotConfirmed: return "not confirmed";
		case BoundedRestoreResult::Failed: return "failed";
		default: return "timed out";
		}
	}

	// `stop` returns true when the drive was stopped (or there was none).
	inline BoundedRestoreResult RunBoundedRestore(std::function<bool()> stop, unsigned timeoutMs)
	{
		if (!stop)
			return BoundedRestoreResult::Restored;
		auto promise = std::make_shared<std::promise<BoundedRestoreResult>>();
		std::future<BoundedRestoreResult> future = promise->get_future();
		try
		{
			std::thread([promise, stop]()
			{
				BoundedRestoreResult result = BoundedRestoreResult::Failed;
				try
				{
					result = stop() ? BoundedRestoreResult::Restored : BoundedRestoreResult::NotConfirmed;
				}
				catch (...)
				{
					result = BoundedRestoreResult::Failed;
				}
				try { promise->set_value(result); } catch (...) {}
			}).detach();
		}
		catch (...)
		{
			return BoundedRestoreResult::Failed;
		}
		if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
			return BoundedRestoreResult::TimedOut;
		return future.get();
	}
}
