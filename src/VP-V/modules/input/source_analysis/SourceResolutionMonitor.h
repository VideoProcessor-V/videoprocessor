#pragma once

#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>

#include <libplacebo/renderer.h>

#include <algorithm>
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
		bool frameBlack = false;       // ... and was a real black (every tile flat at black; latch fixes)
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
		double cutSpan = 0.0;          // seconds between the frames the last cut was found between
		// h3-latch (source_resolution_cut_fixes): the cuts a switch may use (judged at once; the same as
		// `cuts` without the cut fixes) and the span of the last; the last confirmed cut's first frame
		// (seconds, -1 = none); the evidence behind the declared value; candidates dropped (flashes,
		// unconfirmed near misses); tracker steps the look-ahead covered.
		unsigned long long moments = 0;
		double momentSpan = 0.0;
		double lastCutSeconds = -1.0;
		int shots = 0;
		double evidenceSeconds = 0.0;
		unsigned long long flashes = 0, coveredSteps = 0;
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

		// Latch fixes (owner 2026-10-17): how a change is shown. An engagement confirmed by the
		// evidence switches at the first cut, real black, fast pan or low-detail moment, at once
		// (the event hides it); with none of them within source_resolution_engage_max_wait_s it
		// crossfades from the path on screen to the re-based one over source_resolution_crossfade_s.
		// A return to native (detail above, or the confidence gone) is immediate, faded over at
		// most kMaxReleaseFade (source_resolution_release_fade_s). A discontinuity is a hard switch.
		// The renderer draws `from` with weight 1 - `weight` and `to` with `weight`
		// (pl_render_image_mix, FadeMixTimestamps).
		struct Transition
		{
			double fromX = 1.0, fromY = 1.0;   // the factors being left
			double toX = 1.0, toY = 1.0;       // the factors applied (AutoFactors' result)
			double weight = 1.0;               // of `to`: 1 = no fade in progress
			double duration = 0.0;             // of the fade (0 = a hard switch)
			bool Active() const { return weight < 1.0; }
		};
		// Render thread, after AutoFactors (the same clock; < 0 = the steady clock).
		Transition AutoTransition(double seconds = -1.0) const;
		void SetEngageTiming(double maxWaitSeconds, double crossfadeSeconds, double releaseFadeSeconds)
		{
			engageMaxWait = (std::max)(0.0, maxWaitSeconds);
			crossfade = (std::max)(0.0, crossfadeSeconds);
			releaseFade = (std::min)(kMaxReleaseFade, (std::max)(0.0, releaseFadeSeconds));
		}
		double EngageMaxWait() const { return engageMaxWait; }
		double CrossfadeSeconds() const { return crossfade; }
		double ReleaseFade() const { return releaseFade; }
		static constexpr double kMaxReleaseFade = 0.25;
		// The whole frame's motion of the source pair ending at `sequence` (native px per source
		// frame; the optical-flow engine's global vector when it is usable), for the fast-pan switch.
		void SetMotion(double pixelsPerFrame, uint64_t sequence) { panPixelsPerFrame = pixelsPerFrame; panSequence = sequence; }
		// A pan at least this fast (picture heights per second, so independent of the resolution and
		// of the frame rate) blurs the picture enough to hide the switch.
		static constexpr double kPanHeightsPerSecond = 0.5;
		// The fade's weight of the new path: linear in time, 0 at the start, exactly 1 at the end.
		static double FadeWeight(double elapsed, double duration);
		// The frame-mix timestamps (old, new) that give libplacebo's triangle mixer the weights
		// (1 - w, w), and a CPU model of that mix (tests: monotonic, exact at the endpoints).
		static void FadeMixTimestamps(double weight, float timestamps[2]);
		static float FadeMixValue(float oldValue, float newValue, double weight);
		double AutoRatio() const { return autoAppliedShared.load(std::memory_order_acquire); }
		// The source_resolution setting in force (render thread sets it; any
		// thread reads it, for the status line).
		void SetSelection(const std::string& value);
		// source_resolution_text_guard (owner 2026-10-16): leave tiles holding overlay text
		// (the player's subtitles at the output resolution) out of the readings.
		void SetTextGuard(bool on) { textGuard.store(on, std::memory_order_release); }
		// source_resolution_mixed_guard (audit 2026-10-16): a mixed screen reads native.
		void SetMixedGuard(bool on) { mixedGuard.store(on, std::memory_order_release); }
		// Hunt 2 M2/M3 (source_coding_fixes): the robust coding readings (Thresholds::codingFixes).
		void SetCodingFixes(bool on) { codingFixes.store(on, std::memory_order_release); }
		// [vprenderer] source_resolution_latch_fixes (latch review 2026-10-17, default on): re-engage
		// on the standard after native evidence, level-aware native readings, the per-tile floor test,
		// shape-aware cuts, the real-black engage, the look-ahead preferred, the tile a fraction of the
		// picture height, tracker gaps routed as discontinuities. false: the behaviour before.
		void SetLatchFixes(bool on) { latchFixes.store(on, std::memory_order_release); }
		bool LatchFixes() const { return latchFixes.load(std::memory_order_acquire); }
		// [vprenderer] source_resolution_cut_fixes (h3-latch 2026-10-18, default on; needs the latch fixes):
		// the cut judge tells camera moves, scrolls, dark passes and flashes from cuts (the look-ahead's
		// per-frame judge with the flash window, the luma distribution, the next analysed frame), finds
		// same-level shot / reverse-shot cuts, and the shots need part of the time. false: the latch
		// fixes' judge alone.
		void SetCutFixes(bool on) { cutFixes.store(on, std::memory_order_release); }
		bool CutFixes() const { return cutFixes.load(std::memory_order_acquire); }
		// The look-ahead's per-frame evidence for the source frames (from, to] (tests, diagnostics).
		SourceResolution::FrameEvidence LookAheadEvidence(uint64_t from, uint64_t to) const;
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
		// The largest analysis tile (the readback buffer's size); with the latch fixes the tile is
		// SourceResolution::TileSize(picture height) (128 at 1080, 256 at 2160), else always kTile.
		static constexpr int kTile = 256;
		int TileFor(double pictureHeight) const { return LatchFixes() ? SourceResolution::TileSize(pictureHeight, kTile) : kTile; }
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
		// ... with tiles of `tileSize` px (TileFor) and the signal's black (normalised).
		void AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight,
			const std::vector<float>& tileX, const std::vector<float>& tileY, int baseTiles, int tileSize, double blackLevel = 64.0 / 1023.0);
		// ... and the frame's source sequence (the look-ahead's evidence for the step; 0 = none).
		void AnalyseTilesForTest(const std::vector<float>& tiles, int count, double seconds, int nativeWidth, int nativeHeight,
			const std::vector<float>& tileX, const std::vector<float>& tileY, int baseTiles, int tileSize, double blackLevel, uint64_t sequence);

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
			int tileSize = kTile;
			double blackLevel = 64.0 / 1023.0;
			uint64_t sequence = 0;       // the source frame copied (h3-latch: look-ahead evidence)
		};
		void Worker();
		void Fail(const char* what, double seconds);
		void Record(const std::string& row);   // with `mutex` held
		void FlushHistory(bool force);
		void Analyse(Job& job);
		void ResetEvidence(const char* cause);   // with `mutex` held: back to native, all evidence cleared
		std::string gapReason;                   // the last gap's text (ResetEvidence keeps a pointer)
		double lastAnalysedSeconds = -1.0;

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
		int aheadTileSize = kTile;
		AheadState ahead;
		uint64_t aheadLastSequence = 0;
		double aheadLastMean = -1.0;
		std::vector<double> aheadLastPattern;   // latch fixes: the last ahead frame with a shape
		SourceResolution::Tracker::CutJudge aheadCutJudge;
		// Cut fixes: the per-frame judge (render thread writes, worker reads Evidence) under its mutex.
		SourceResolution::FrameCutJudge frameJudge;
		mutable std::mutex frameJudgeMutex;
		double blackLevelSeen = 64.0 / 1023.0;  // the source's black (Frame), for the look-ahead
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
		std::atomic<bool> codingFixes{ true };
		std::atomic<bool> latchFixes{ true };
		std::atomic<bool> cutFixes{ true };
		// Worker sets, render thread reads: sample the dense grid (a lower reading declared or
		// being gathered by any of the trackers).
		std::atomic<bool> denseGrid{ false };
		std::atomic<int> evidenceShots{ SourceResolution::Tracker::Settings{}.minimumShots };
		std::atomic<double> evidenceSeconds{ SourceResolution::Tracker::Settings{}.minimumSeconds };
		std::string selection = "native";
		unsigned long long autoPendingCuts = 0;
		unsigned long long autoPendingFrames = 0;   // engage also on a blank frame analysed after this
		bool disengagePending = false;   // confidence fell: native at the next cut or after the wait
		// Latch fixes: the switch triggers and the fade (render thread).
		int EngageTrigger(const Snapshot& s, unsigned long long pendingCuts, unsigned long long pendingFrames, double pendingSince,
			double now, uint64_t renderSequence, bool cutAllowed, std::string& why);   // 0 none, 1 at once, 2 crossfade
		void Transit(double factorX, double factorY, double now);
		double engageMaxWait = 20.0, crossfade = 1.5, releaseFade = 0.25;
		double autoPendingSince = 0.0, axisPendingSince = 0.0;
		unsigned long long cutsNotUsable = ~0ull;   // a cut whose moment is not known (a lost copy): not switched on
		double nextFadeSeconds = 0.0;
		double fadeFromX = 1.0, fadeFromY = 1.0, lastFactorX = 1.0, lastFactorY = 1.0, fadeStart = 0.0, fadeDuration = 0.0;
		uint64_t clockSequence = 0, panSequence = 0;
		double clockSeconds = 0.0, secondsPerFrame = 0.0, panPixelsPerFrame = 0.0;
		double disengageSince = 0.0;
		unsigned long long disengageCuts = 0;
		unsigned long long disengageFrames = 0;
	};

	// The stats-line / status text.
	std::string Describe(const Snapshot& s, double autoRatio, bool autoSelected);
}
