#pragma once

#include <atomic>

// Planner 2026-10-13, item 7: a user shader that keeps state in //!STORAGE
// textures (the temporal sigma-delta) marks that state valid with a tag. The
// textures start uninitialised, and on a hardware GPU a newly created hook
// can receive the memory of a hook just freed - tag included - and trust its
// stale state on its first refresh. Each hook gets its own `instance_tag`
// (this counter, set once when the hook is parsed) and writes and checks a
// tag derived from it, so inherited state never matches. One counter per
// program (an inline function's static), shared by the hook chain and the
// tests' loaders.
inline int NextHookInstanceTag()
{
	static std::atomic<int> counter{ 0 };
	return ++counter;
}
