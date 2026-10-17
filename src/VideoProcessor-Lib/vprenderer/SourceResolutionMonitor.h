#pragma once

#include <SourceResolutionDetector.h>

#include <libplacebo/renderer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
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
		bool frameBlank = false;       // the last analysed frame had no detail (black, a fade's dark end, flat)
		double liveRatio = 1.0;        // the spec's live reading: median of the last ~2 s, snapped on a cut
		double declaredRatio = 1.0;    // the tracker's conservative value
		double confidence = 0.0;       // of the declared value (0..1)
		// Per axis (owner 2026-10-16: a source upscaled in one direction only), when the
		// both-axes reading is native: the width / height ratios declared by the per-axis
		// trackers (stricter evidence), and their confidence. 1 = native on that axis.
		double axisRatioX = 1.0, axisRatioY = 1.0, axisConfidence = 0.0;
		// Kernel-matched re-base (docs/VP-stage12-build-plan.md): the player's upscale
		// kernel identified at a 2:1 reading (KernelInverse candidate, -1 = none), and the
		// accepted tile fits behind it (in the window) / that agree.
		int kernel = -1;
		int kernelVotes = 0, kernelAgree = 0;
		double bitrateQuality = 1.0;   // spec 1b (smoothed)
		double effectiveBits = 0.0;    // spec 2d (0 = unknown)
		double coherenceV = 0.0, coherenceH = 0.0;
		unsigned long long frames = 0, cuts = 0, discontinuities = 0;
		std::string lastDiscontinuity;
		double analyseMs = 0.0;        // worker time for the last frame
		int tiles = 0;                 // tiles in the last analysed frame (8 = base grid, 16 = dense)
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
		// Why a source cannot be measured ("" = it can). Dolby Vision (2026-10-17):
		// measured on its decoded intensity (I) plane, planes[0] (12-bit IPT-PQ,
		// the tunnel's luma-like channel), as any YCbCr luma.
		static std::string NotReadReason(const pl_frame& source);
		// Any thread: what came before no longer describes the source.
		void Discontinuity(const char* cause);
		Snapshot Read() const;

		// Render thread: the re-base factor auto wants for a native height
		// (1 = native), and why ("" when unchanged). Planner Q2 (2026-10-16): back to
		// native at once on detail above the re-based height or a discontinuity; when
		// only the confidence fell, at the next cut, at most kDisengageWaitSeconds later.
		// `seconds`: a monotonic clock (tests); < 0 = the steady clock.
		double AutoFactor(int nativeHeight, double minimumConfidence, std::string& why, double seconds = -1.0, uint64_t renderSequence = 0);
		// Per axis: the uniform factor when it is engaged (both = AutoFactor), else the
		// per-axis factors (width, height) under the same rules - engage at a cut or a
		// black frame, native at once on detail or a discontinuity.
		void AutoFactors(int nativeWidth, int nativeHeight, double minimumConfidence, std::string& why,
			double& factorX, double& factorY, double seconds = -1.0, uint64_t renderSequence = 0);
		static constexpr double kDisengageWaitSeconds = 5.0;
		double AutoRatio() const { return autoAppliedShared.load(std::memory_order_acquire); }
		// The source_resolution setting in force (render thread sets it; any
		// thread reads it, for the status line).
		void SetSelection(const std::string& value);
		// source_resolution_text_guard (owner 2026-10-16): leave tiles holding overlay text
		// (the player's subtitles at the output resolution) out of the readings.
		void SetTextGuard(bool on) { textGuard.store(on, std::memory_order_release); }
		// source_resolution_mixed_guard (audit 2026-10-16): a mixed screen reads native.
		void SetMixedGuard(bool on) { mixedGuard.store(on, std::memory_order_release); }
		// source_resolution_evidence_shots / _seconds (the Picture slider): the shots and time
		// of agreeing evidence before auto declares a lower source; the per-axis trackers
		// need twice both.
		void SetEvidence(int shots, double seconds)
		{
			evidenceShots.store(shots, std::memory_order_release);
			evidenceSeconds.store(seconds, std::memory_order_release);
		}
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
		// The base 4 x 2 grid, plus two more rows (SourceResolution::PlaceTiles) while a lower
		// reading is declared or being gathered - so a picture-in-picture or UI between the base
		// tiles is seen before the frame is re-based (mixed-screen guard).
		static constexpr int kMaxTiles = 16;
		static constexpr int kBaseTiles = 8;
		bool DenseGrid() const { return denseGrid.load(std::memory_order_acquire); }

		// Look-ahead verification (owner 2026-10-16: "since we have look ahead the
		// analysis can look ahead and ensure the upcoming frames are in accordance
		// with the previous assumption"). The render loop shows the monitor the
		// queued frames ahead of display (10-bit luma through `luma(x, y)`, the
		// active picture rectangle), oldest first. A cheap check per frame finds a
		// cut or a black/flat frame ahead; while an auto change is pending, the
		// first frame with picture after it is analysed on a separate worker (the
		// tracker is not touched). AutoFactor, told the sequence being rendered,
		// then switches exactly on the cut / black frame when that reading agrees
		// with the change, or cancels the change when the next shot disagrees (it
		// waits for a later cut). With no frames ahead nothing changes.
		using LumaSampler = std::function<int(int x, int y)>;
		void LookAheadFrame(uint64_t sequence, int width, int height, const LumaSampler& luma,
			int left, int top, int right, int bottom);
		struct AheadState
		{
			uint64_t eventSequence = 0;    // the cut / black frame ahead (0 = none)
			uint64_t targetSequence = 0;   // the frame analysed for it
			bool ready = false, informative = false;
			double ratio = 1.0;
			unsigned long long verified = 0, cancelled = 0;
		};
		AheadState Ahead() const;
		// Tests without a GPU: one analysed frame from normalised tiles (kTile x kTile each),
		// through the same path as a captured frame, synchronously.
		void AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight);
		// ... with the tiles' centres (normalised) and the base-grid count, as Frame() hands a
		// captured job to the worker (SourceResolution::PlaceTiles; the dense grid included).
		void AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight,
			const std::vector<float>& tileX, const std::vector<float>& tileY, int baseTiles);

	private:
		struct Job
		{
			std::vector<float> values;   // tiles x kTile x kTile, normalised samples
			int tiles = 0;
			int codeMaximum = 1023;
			double scale = 1023.0;       // sample -> code
			double seconds = 0.0;
			int nativeWidth = 0, nativeHeight = 0;
			// The tiles' centres in the picture (normalised; empty: the 4-column layout) and how
			// many of them are the base grid (the frame signature; 0 = all).
			std::vector<float> tileX, tileY;
			int baseTiles = 0;
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

		// Look-ahead (render thread, plus its worker under aheadMutex).
		void AheadWorker();
		mutable std::mutex aheadMutex;
		std::condition_variable aheadWake;
		std::thread aheadThread;
		bool aheadStopping = false, aheadHaveJob = false;
		std::vector<float> aheadTiles;   // the target frame's tiles, normalised
		int aheadTileCount = 0;
		AheadState ahead;
		uint64_t aheadLastSequence = 0;
		double aheadLastMean = -1.0;
		bool aheadAwaitTarget = false;   // the event frame was black: analyse the next frame with picture
		unsigned long long autoNotBeforeCuts = 0;   // after a cancel: the cut at the event is not used

		// Kernel identification (worker side): accepted fits (seconds, candidate, shot).
		struct KernelVote { double seconds; int candidate; unsigned long long shot; };
		std::deque<KernelVote> kernelVotes;
		unsigned long long kernelFitCounter = 0;
		// Per-axis trackers and their coherence history (worker side).
		SourceResolution::Tracker trackerV{ AxisSettings() }, trackerH{ AxisSettings() };
		std::deque<std::pair<double, double>> coherenceV, coherenceH;
		static SourceResolution::Tracker::Settings AxisSettings()
		{
			// Stricter than the both-axes standard: twice the shots and the time.
			SourceResolution::Tracker::Settings s;
			s.minimumShots *= 2;
			s.minimumSeconds *= 2.0;
			return s;
		}
		// Per-axis auto (render thread).
		double axisApplied[2] = { 1.0, 1.0 }, axisPending[2] = { 1.0, 1.0 };
		unsigned long long axisPendingCuts = 0, axisPendingFrames = 0, axisDiscontinuities = 0;

		// Auto (render thread).
		std::atomic<bool> autoReset{ false };
		double autoApplied = 1.0, autoPending = 1.0;
		std::atomic<double> autoAppliedShared{ 1.0 };
		std::atomic<bool> textGuard{ true };
		std::atomic<bool> mixedGuard{ true };
		// Worker sets, render thread reads: sample the dense grid (a lower reading declared or
		// being gathered by any of the trackers).
		std::atomic<bool> denseGrid{ false };
		std::atomic<int> evidenceShots{ SourceResolution::Tracker::Settings{}.minimumShots };
		std::atomic<double> evidenceSeconds{ SourceResolution::Tracker::Settings{}.minimumSeconds };
		std::string selection = "native";
		unsigned long long autoPendingCuts = 0;
		unsigned long long autoPendingFrames = 0;   // engage also on a blank frame analysed after this
		bool disengagePending = false;   // confidence fell: native at the next cut or after the wait
		double disengageSince = 0.0;
		unsigned long long disengageCuts = 0;
		unsigned long long disengageFrames = 0;
	};

	// The stats-line / status text.
	std::string Describe(const Snapshot& s, double autoRatio, bool autoSelected);
}
