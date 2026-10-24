#pragma once

#include <algorithm>
#include <cstdint>

// Efficiency audit 2026-10-24 #8b: a VP feature's GPU textures (ExtendedRange's two source-size
// rgba32f textures, AdaptiveDownscale, CrossfadeMix scratch, the re-base, the debander, texture
// restore) used to stay allocated until device teardown after the feature went idle or was
// switched off (e.g. ExtendedRange ~266 MB at 4K after SDR -> HDR). On the 4 GB laptop that is
// what pushed MotionEngineIdle into pressure releases.
//
// A Releaser per feature decides WHEN to free them, with hysteresis so nothing thrashes:
//   - released after idleSeconds without the feature running (default 10 s);
//   - a feature that comes back soon after a release (within thrashWindowSeconds) doubles its
//     idle time, up to maxIdleSeconds, so a feature toggling with the content (a scene-dependent
//     decision) stops being released and recreated;
//   - under video-memory pressure (the caller's MotionEngineIdle-style reading) the minimum idle
//     time is used.
// Byte-identical: the textures are recreated by the feature's own Ensure* on its next frame and
// every one of them is fully written before it is read (each feature's own contract; the GPU test
// re-checks this per feature, docs/design/efficiency-byte-identical-tests.md #8b).
// Pure, CPU-tested; the renderer calls Note() once per frame per feature and, when it returns
// true, the feature's Release().
namespace FeatureTextureIdle
{
	struct Settings
	{
		double idleSeconds = 10.0;
		double maxIdleSeconds = 120.0;
		double thrashWindowSeconds = 30.0;
	};

	class Releaser
	{
	public:
		Releaser() = default;
		explicit Releaser(const Settings& s) : settings(s), idle(s.idleSeconds) {}

		// One frame: whether the feature ran (or holds textures it will read next frame),
		// whether it holds textures now, and the time. True = release the textures now.
		bool Note(bool active, bool holdsTextures, double nowSeconds, bool memoryPressure = false)
		{
			if (active)
			{
				if (releasedAt >= 0.0 && nowSeconds - releasedAt < settings.thrashWindowSeconds)
				{
					idle = (std::min)(settings.maxIdleSeconds, idle * 2.0);
					++thrashBackoffs;
				}
				releasedAt = -1.0;
				lastActive = nowSeconds;
				seenActive = true;
				return false;
			}
			if (!holdsTextures || !seenActive)
				return false;
			const double wait = memoryPressure ? settings.idleSeconds : idle;
			if (nowSeconds - lastActive < wait)
				return false;
			releasedAt = nowSeconds;
			seenActive = false;
			++releases;
			return true;
		}
		double IdleSeconds() const { return idle; }
		uint64_t Releases() const { return releases; }
		uint64_t ThrashBackoffs() const { return thrashBackoffs; }

	private:
		Settings settings{};
		double idle = Settings{}.idleSeconds;
		double lastActive = 0.0;
		double releasedAt = -1.0;
		bool seenActive = false;
		uint64_t releases = 0;
		uint64_t thrashBackoffs = 0;
	};
}
