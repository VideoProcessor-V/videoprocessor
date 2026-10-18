#pragma once

#include <cstdint>

// h3-latch 2026-10-18 (bug hunt round 3 K3): the uncropped first frame.
//
// At start-up, and again at every renderer or live queue reset (BeginQueueGeneration resets the
// active-picture timeline), the first crop decision takes the detector's confirmations plus the
// renderer's start-up stalls: the uncropped picture - bars, and the subtitles and edges placed for
// it - stayed on screen for 1.0 s (native) to 1.5 s (1080) before the crop landed (gate-verify 3).
//
// [vprenderer] active_picture_startup_hold (default on): while automatic crop (or NLS) is wanted and
// no crop decision of the new generation exists yet, the output is held black - unless the same
// source (size and format) was decided before, when its remembered decision is shown at once (a
// remembered crop is re-applied; a remembered full raster needs no hold). The first decision ends
// the hold, and so does maximumHoldSeconds (a picture whose detector cannot decide still appears).
// Presentation only: the detector, the subtitle worker, A-lens and edge blanking run unchanged on
// the source frames and decide exactly as before; the hold never grants or withdraws crop authority.
class StartupCropHold
{
public:
	struct Rect
	{
		int left = 0, top = 0, right = 0, bottom = 0;
		bool Valid() const { return right > left && bottom > top; }
	};
	struct Key
	{
		int width = 0, height = 0;
		uint64_t format = 0;
		bool operator==(const Key& o) const { return width == o.width && height == o.height && format == o.format; }
		bool operator!=(const Key& o) const { return !(*this == o); }
	};
	enum class Action { Present, Black, Remembered };

	void SetEnabled(bool on) { enabled = on; if (!on) holding = false; }
	bool Enabled() const { return enabled; }
	void SetMaximumHold(double seconds) { maximumHold = seconds > 0.0 ? seconds : 0.0; }
	double MaximumHold() const { return maximumHold; }

	// A new queue generation (start-up, renderer reset, live queue reset): hold from `now`.
	void Begin(double now)
	{
		holding = enabled;
		start = now;
	}

	// Every presented frame. `cropWanted`: automatic crop or NLS is in use (else nothing to wait for).
	// `decided`: this generation has a crop decision for the frame - a crop applied (`cropApplied`,
	// `crop` in source pixels) or a trusted full raster.
	Action Frame(const Key& key, double now, bool cropWanted, bool decided, bool cropApplied, const Rect& crop)
	{
		if (!enabled || !cropWanted)
		{
			holding = false;
			return Action::Present;
		}
		if (decided)
		{
			remembered.key = key;
			remembered.cropApplied = cropApplied && crop.Valid();
			remembered.crop = crop;
			haveRemembered = true;
			holding = false;
			return Action::Present;
		}
		if (!holding)
			return Action::Present;
		if (now - start >= maximumHold)
		{
			holding = false;
			timeouts++;
			return Action::Present;
		}
		if (haveRemembered && remembered.key == key)
			return remembered.cropApplied ? Action::Remembered : Action::Present;
		return Action::Black;
	}

	bool Holding() const { return holding; }
	const Rect& RememberedCrop() const { return remembered.crop; }
	unsigned long long Timeouts() const { return timeouts; }
	static const char* Name(Action a) { return a == Action::Black ? "black" : a == Action::Remembered ? "remembered crop" : "present"; }

private:
	struct Remembered
	{
		Key key;
		bool cropApplied = false;
		Rect crop;
	} remembered;
	bool haveRemembered = false;
	bool enabled = true;
	bool holding = false;
	double start = 0.0;
	// Start-up measured 1.0 s native / 1.5 s 1080 to the first crop (gate-verify 3, warm cache); a
	// cold shader compile can add more. Beyond this the picture is shown undecided.
	double maximumHold = 2.5;
	unsigned long long timeouts = 0;
};
