#pragma once

#include <SourceResolutionDetector.h>

#include <libplacebo/renderer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Stage 12 step B in the renderer (owner 2026-10-15): the effective
// resolution, bitrate quality and effective bit depth of every source, and
// `source_resolution: auto` acting on them.
//
// On the render thread, a few times a second, a compute pass copies tiles of
// the native (unwarped, not re-based) luma into a readback buffer; the
// analysis (SourceResolutionDetector) runs on a worker thread; the render
// thread reads the latest snapshot. Never waits for the GPU or the worker.
//
// Auto (owner: an option now, default native): re-bases from the tracker's
// declared reading when its confidence passes the minimum. Engaging waits
// for the next cut (no change in the middle of a shot); disengaging is
// immediate - detail above the re-based height, a discontinuity (resync,
// source or format change, seek / live queue reset) or a confidence drop.
namespace SourceResolutionMonitor
{
	struct Snapshot
	{
		bool reading = false;          // at least one frame analysed since the last discontinuity
		std::string notRead;           // why the source is not read (empty when it is)
		int nativeWidth = 0, nativeHeight = 0;
		double frameRatio = 1.0;       // the last analysed frame
		double liveRatio = 1.0;        // the spec's live reading: median of the last ~2 s, snapped on a cut
		double declaredRatio = 1.0;    // the tracker's conservative value
		double confidence = 0.0;       // of the declared value (0..1)
		double bitrateQuality = 1.0;   // spec 1b (smoothed)
		double effectiveBits = 0.0;    // spec 2d (0 = unknown)
		double coherenceV = 0.0, coherenceH = 0.0;
		unsigned long long frames = 0, cuts = 0, discontinuities = 0;
		std::string lastDiscontinuity;
		double analyseMs = 0.0;        // worker time for the last frame
		std::string error;             // the last GPU-side failure (empty when none)
	};

	class Monitor
	{
	public:
		Monitor();
		~Monitor();
		Monitor(const Monitor&) = delete;
		Monitor& operator=(const Monitor&) = delete;

		// Render thread, every rendered source frame: submits a tile copy when
		// the interval has passed and a slot is free, and collects finished
		// copies for the worker. `seconds`: a monotonic clock.
		void Frame(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, uint64_t sourceSequence, double seconds);
		// Any thread: what came before no longer describes the source.
		void Discontinuity(const char* cause);
		Snapshot Read() const;

		// Render thread: the re-base factor auto wants for a native height
		// (1 = native), and why ("" when unchanged). Planner Q2 (2026-10-16): back to
		// native at once on detail above the re-based height or a discontinuity; when
		// only the confidence fell, at the next cut, at most kDisengageWaitSeconds later.
		// `seconds`: a monotonic clock (tests); < 0 = the steady clock.
		double AutoFactor(int nativeHeight, double minimumConfidence, std::string& why, double seconds = -1.0);
		static constexpr double kDisengageWaitSeconds = 5.0;
		double AutoRatio() const { return autoAppliedShared.load(std::memory_order_acquire); }
		// The source_resolution setting in force (render thread sets it; any
		// thread reads it, for the status line).
		void SetSelection(const std::string& value);
		std::string Selection() const;

		void Release(pl_gpu gpu);
		// Owner 2026-10-16 (debugging auto): one file per session in `folder`,
		// "source-resolution <yyyymmdd-hhmmss>.csv" - every analysed frame's reading and
		// every auto decision with its reason, the newest kHistoryRows (about ten minutes,
		// ~100 KB), rewritten every few seconds (temp + replace). The newest kHistorySessions
		// files are kept; older ones are deleted when a session starts. Empty folder = off.
		void SetHistoryFolder(const std::string& folder);
		const std::string& HistoryPath() const { return historyPath; }
		static constexpr size_t kHistorySessions = 5;
		void FlushHistoryNow() { FlushHistory(true); }
		static constexpr size_t kHistoryRows = 1200;
		double intervalSeconds = 0.5;   // tile copies per second: 2
		static constexpr int kTile = 256;
		static constexpr int kMaxTiles = 8;

	private:
		struct Job
		{
			std::vector<float> values;   // tiles x kTile x kTile, normalised samples
			int tiles = 0;
			int codeMaximum = 1023;
			double scale = 1023.0;       // sample -> code
			double seconds = 0.0;
			int nativeWidth = 0, nativeHeight = 0;
		};
		void Worker();
		void Fail(const char* what, double seconds);
		void Record(const std::string& row);   // with `mutex` held
		void FlushHistory(bool force);
		void Analyse(Job& job);

		// GPU side (render thread).
		pl_gpu gpu = nullptr;
		pl_buf ring[2] = {};
		bool busy[2] = {};
		Job pendingMeta[2];
		int nextSlot = 0;
		double lastSubmit = -1e9;
		std::string signature;
		std::string notReadLogged;

		// Worker side.
		std::thread worker;
		mutable std::mutex mutex;
		std::condition_variable wake;
		bool stopping = false;
		bool haveJob = false;
		Job job;
		std::atomic<const char*> discontinuityCause{ nullptr };
		SourceResolution::Tracker tracker;
		std::deque<std::pair<double, double>> live;          // (seconds, ratio) of informative frames
		std::deque<std::pair<double, double>> coherence;     // (seconds, min axis coherence)
		unsigned long long lastCuts = 0;
		double qualitySmoothed = -1.0, bitsSmoothed = 0.0;
		Snapshot snapshot;
		std::deque<std::string> history;
		std::string historyPath;
		std::chrono::steady_clock::time_point lastFlush{};
		bool historyDirty = false;

		// Auto (render thread).
		std::atomic<bool> autoReset{ false };
		double autoApplied = 1.0, autoPending = 1.0;
		std::atomic<double> autoAppliedShared{ 1.0 };
		std::string selection = "native";
		unsigned long long autoPendingCuts = 0;
		bool disengagePending = false;   // confidence fell: native at the next cut or after the wait
		double disengageSince = 0.0;
		unsigned long long disengageCuts = 0;
	};

	// The stats-line / status text.
	std::string Describe(const Snapshot& s, double autoRatio, bool autoSelected);
}
