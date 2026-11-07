// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ChainEvidenceLog (SID-C2b, doc 78 section 9b): the shadow-deploy evidence log of the chain estimator. Long-run confident-wrong
// tracking on real feeds, development clips for thin coverage buckets, exact offline replay of suspect decisions.
//
// WORKER-SIDE ONLY. Every method that touches the disk (OnEvaluation, OnDiscontinuity, Close, Configure) is called by the
// estimator's worker thread, never by the render thread or the frame-analysis worker; the few any-thread entry points (RequestDump,
// TakeNotice, GetStatus) only touch a small mutex-guarded status block.
//
// What it writes into one `sid-evidence` folder (beside the log), all named `<yyyymmdd-hhmmss>-...` so the file name order is the age
// order (the 2 GB cap evicts the oldest first, FIFO):
//   <stamp>-log-NNN.jsonl   JSON lines: one row per SEGMENT (the span between discontinuities, regime boundaries and applied
//                           changes), one row per state change (full detail), one aggregate row per 10 s of source time, one
//                           row per trigger. Rotates at rotateBytes.
//   <stamp>-dump-NN-<why>.sidl      the RAM ring of the last 30 evaluations' sampled lines, uncompressed binary with a small
//                           JSON header, written on a TRIGGER (level log+lines and above). Format: see SerializeDump.
//   <stamp>-dump-NN-<why>-frameK.u16  at most 4 evaluation frames (full luma planes) beside the lines, level log+lines+frames only;
//                           local only, never committed or uploaded (film-frame rule).
//   coverage.txt            the coverage bucket counts (not subject to the FIFO).
// Triggers (rate limited: <= 1 per 5 minutes and <= 20 per 24 hours; the report-a-problem hotkey is exempt from the gap and the day
// budget, with a 5 s debounce, so a deliberate press is never silently dropped): a latch that disagrees with the classical decision,
// a revert within 60 s of a latch (flap suspect), a latch with LLR <= c1 + 1 nat (thin), HOLD for more than 60 s, UNSTABLE, power
// unseen in more than 80% of the probes over 5 minutes, a coverage bucket below its target count, the hotkey.
// Guardrails: off when `active` is false (the Original pathway, consensus off, level off); a one-time notice when it starts
// ("Source identity: evidence logging on (shadow)") and a status text with the folder size; a failed write is a visible notice, once
// per failure episode.
//
// Header + .cpp (Win32 file system behind IEvidenceFs; tests use an in-memory one). C++14.
#include "ChainLatch.h"
#include "ChainLines.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		enum class EvidenceLevel : int { Off = 0, Log = 1, Lines = 2, Frames = 3 };

		// "off", "log", "log+lines", "log+lines+frames" (the values of `source_id_evidence`).
		const char* EvidenceLevelText(EvidenceLevel level);
		bool ParseEvidenceLevel(const std::string& text, EvidenceLevel& out);

		typedef std::function<double()> NowMsFn;          // monotonic milliseconds
		typedef std::function<std::string()> StampFn;     // local time as "yyyymmdd-hhmmss"

		struct FileInfo
		{
			std::string name;
			uint64_t bytes = 0;
		};

		// The file system behind the log (the default is the Win32 disk; tests use an in-memory one).
		class IEvidenceFs
		{
		public:
			virtual ~IEvidenceFs() {}
			virtual bool CreateDirs(const std::string& folder, std::string& error) = 0;
			virtual bool Write(const std::string& path, const std::string& data, bool append, std::string& error) = 0;
			virtual std::vector<FileInfo> List(const std::string& folder) = 0;   // names only, no sub folders
			virtual bool Remove(const std::string& path) = 0;
			virtual bool Read(const std::string& path, std::string& data) = 0;
		};
		std::shared_ptr<IEvidenceFs> MakeDiskEvidenceFs();
		std::string JoinPath(const std::string& folder, const std::string& name);
		// The default wall-clock stamp "yyyymmdd-hhmmss" (local time).
		std::string LocalStamp();

		struct EvidenceLogSettings
		{
			uint64_t capBytes = 2ull * 1024ull * 1024ull * 1024ull;   // 2 GB, FIFO eviction
			size_t ringEvaluations = 30;                              // evaluations of lines kept in RAM
			double triggerGapMs = 5.0 * 60.0 * 1000.0;                // <= 1 dump per 5 minutes
			int maxPerDay = 20;                                       // <= 20 dumps per 24 hours
			double dayMs = 24.0 * 3600.0 * 1000.0;
			double hotkeyDebounceMs = 5000.0;
			int64_t aggregateEveryUs = 10 * 1000000ll;                // one aggregate row per 10 s of source time
			uint64_t rotateBytes = 20ull * 1024ull * 1024ull;         // the JSON-lines file rotates here
			int framesPerDump = 4;
			double thinMarginNats = 1.0;                              // latch with LLR <= c1 + this = thin
			int64_t flapWindowUs = 60 * 1000000ll;                    // a revert within this of a latch
			int64_t holdTriggerUs = 60 * 1000000ll;                   // HOLD longer than this
			int64_t powerWindowUs = 5 * 60 * 1000000ll;               // the power-probe window
			double powerUnseenShare = 0.8;
			int coverageTarget = 3;                                   // latches wanted per coverage bucket
			double disagreeTolerance = 0.02;                          // relative ratio difference from the classical decision
		};

		// One candidate of the latest evaluation (the top 3 are logged).
		struct LogCandidate
		{
			int n = 0;
			double stat = 0.0;
			double increment = 0.0;
			int kernel = -1;
			double familyMargin = 0.0;
			bool informative = false;
		};

		// One axis of one evaluation, in plain terms (built by the estimator from LatchOutput, EvidenceUpdate and the verifier).
		struct LogAxis
		{
			bool evaluated = false;
			bool informative = false;
			bool hold = false;
			Geometry::LatchState state = Geometry::LatchState::Unknown;          // the field state (overlays included)
			Geometry::LatchState appliedState = Geometry::LatchState::Unknown;
			int N = 0;
			int measuredN = 0, appliedN = 0;                                       // 0 = none
			int measuredKernel = -1, appliedKernel = -1;
			double measuredLlr = 0.0, appliedLlr = 0.0, kernelConf = 0.0;
			uint32_t reasonBits = 0;
			int latchReason = 0;                                                   // LatchReason as an int
			bool stateChanged = false, measuredChanged = false, appliedChanged = false, bypassedTMin = false;
			int lines = 0, gatedLines = 0, maskedLines = 0;                        // best candidate's line counts
			bool probeRan = false, probeSeen = false;
			int candCount = 0;
			LogCandidate top[3];
		};

		struct LogEval
		{
			uint64_t seq = 0;
			int64_t timeUs = 0;
			uint64_t streamKey = 0;
			uint32_t flags = 0;                  // kEv*
			LineRect active;
			LogAxis axis[2];
			double costMs = 0.0;
			bool cold = false;
			double intervalSec = 0.0;
			double c1 = 0.0, c2 = 0.0;
			int stepCodes = 1;                   // 4 = an 8-bit picture, 1 = 10-bit
			bool classicalValid = false;         // the classical decision's ratios (1 = native)
			double classicalRatio[2] = { 1.0, 1.0 };
			uint64_t repeatsSkipped = 0;         // cumulative since the estimator started
			uint64_t queueDropped = 0;           // cumulative: jobs dropped newest-wins
			uint64_t evalCount = 0;              // cumulative evaluations
			std::shared_ptr<const LineSnapshot> lines;   // kept in the RAM ring (level log+lines and above)
		};

		// A full luma plane held for a possible dump (level log+lines+frames).
		struct FrameRef
		{
			std::shared_ptr<const void> keepAlive;
			const uint16_t* luma = nullptr;
			uint32_t width = 0, height = 0, strideSamples = 0;
			uint8_t msbShift = 0;
			uint64_t seq = 0;
			bool Valid() const { return luma != nullptr && width > 0 && height > 0 && strideSamples >= width; }
		};

		// The dump of a ring: "SIDL", u32 version (2), u32 header length, the JSON header, u32 count, then per evaluation
		// u64 seq, i64 timeUs, u64 streamKey, u32 flags, i32 cuts, i64 eventTimeUs, i32 active x y w h, i32 inner x y w h, u32 rowCount, u32 colCount,
		// i32 rowY[rowCount], i32 colX[colCount], u16 rows[rowCount x inner.w], u16 cols[colCount x inner.h]; little endian.
		std::string SerializeDump(const std::string& headerJson, const std::vector<std::shared_ptr<const LineSnapshot>>& evals);
		// False on a bad magic / version or a truncated body (never guesses).
		bool ParseDump(const std::string& bytes, std::string& headerJson, std::vector<LineSnapshot>& evals);
		// A frame file: "SIDF", u32 version (1), u64 seq, u32 width, u32 height, u32 msbShift, then width x height u16 (rows packed).
		std::string SerializeFrame(const FrameRef& frame);

		struct EvidenceStatus
		{
			bool active = false;
			EvidenceLevel level = EvidenceLevel::Off;
			std::string folder;
			uint64_t bytes = 0;            // total of the managed files
			int files = 0;
			uint64_t dumps = 0;            // dumps written
			uint64_t triggers = 0;         // triggers raised
			uint64_t suppressed = 0;       // triggers the rate limits dropped
			uint64_t evicted = 0;          // files removed by the FIFO cap
			uint64_t rows = 0;             // JSON rows written
			uint64_t failedWrites = 0;
			bool noFolder = false;         // a level was requested while the estimator runs, but there is no folder (nothing is logged)
			std::string error;             // the last failure ("" = fine)
			std::string lastTrigger;       // "<why> @ seq N" of the last raised trigger
		};

		class EvidenceLog
		{
		public:
			EvidenceLog(const EvidenceLogSettings& settings, std::shared_ptr<IEvidenceFs> fs, NowMsFn nowMs, StampFn stamp);

			const EvidenceLogSettings& Settings() const { return s_; }

			// Worker. Turns the log on (`active`: the estimator runs in shadow / on and the pathway is not Original) or off at
			// `level`; `modeText` ("shadow" / "on") goes into the one-time notice. An off or inactive log closes the open
			// segment, drops the RAM ring and the held frames and does no more disk work. A change of folder starts a new session.
			void Configure(const std::string& folder, EvidenceLevel level, bool active, const std::string& modeText);
			bool Active() const { return active_; }
			EvidenceLevel Level() const { return level_; }
			// Worker. Whether the full planes are wanted (level log+lines+frames while active).
			bool WantsFrames() const { return active_ && level_ == EvidenceLevel::Frames; }
			// Worker. Whether the ring of lines is kept (level log+lines and above while active).
			bool WantsLines() const { return active_ && (level_ == EvidenceLevel::Lines || level_ == EvidenceLevel::Frames); }

			// Worker. Free text for the segment rows: the input format (size, rate, bit depth, colour format, transfer).
			void SetFormatInfo(const std::string& text) { format_ = text; }

			// Worker, once per evaluation, after the latch. `frame` (optional, frames level) is the evaluated plane.
			void OnEvaluation(const LogEval& eval, const FrameRef* frame);
			// Worker. A discontinuity / reset: the open segment is closed with this cause.
			void OnDiscontinuity(int64_t timeUs, const std::string& cause);
			// Worker. Closes the open segment and flushes (stop, off, Original).
			void Close(int64_t timeUs, const std::string& cause);

			// Any thread. The report-a-problem hotkey: the next OnEvaluation (or Poll) writes a dump.
			void RequestDump(const std::string& why);
			// Worker. Handles a requested dump without waiting for the next evaluation (uses the last evaluation's seq).
			void Poll();

			// Any thread. A one-time notice from outside the log (the estimator's refused hotkey, ...).
			void PostNotice(const std::string& text);
			// Any thread.
			bool TakeNotice(std::string& text);
			EvidenceStatus GetStatus() const;
			// "evidence: log+lines, 12.3 MB in sid-evidence (cap 2048 MB), 2 dumps" / "evidence: off" / "evidence: write failed (...)" /
			// "evidence: log+lines requested, no folder (set the history folder)".
			std::string StatusText() const;

		private:
			struct SegAxis
			{
				int64_t stateUs[static_cast<int>(Geometry::LatchState::Count)] = {};
				Geometry::LatchState lastState = Geometry::LatchState::Unknown;
				bool haveLast = false;
				int evals = 0, informative = 0, probeRan = 0, probeSeen = 0;
				int gateEvals = 0;
				double gatedShareSum = 0.0, maskedShareSum = 0.0;
				LogAxis last;
			};
			struct Segment
			{
				bool open = false;
				int64_t startUs = 0, endUs = 0;
				std::string stamp;
				uint64_t streamKey = 0;
				LineRect active;
				std::string format;
				uint64_t firstSeq = 0, lastSeq = 0;
				int evals = 0;
				SegAxis axis[2];
				std::vector<double> costs;
				double costSum = 0.0;
				uint64_t repeatsAtStart = 0, repeatsAtEnd = 0;
				double intervalSec = 0.0;
				int stepCodes = 1;
				bool classicalValid = false;
				double classicalRatio[2] = { 1.0, 1.0 };
				double chainRatio[2] = { 1.0, 1.0 };
				bool chainLatched[2] = { false, false };
			};

			void OpenSegment(const LogEval& e);
			void CloseSegment(int64_t timeUs, const std::string& why);
			void AccumulateSegment(const LogEval& e);
			void WriteStateRows(const LogEval& e);
			void MaybeAggregate(const LogEval& e);
			void CheckTriggers(const LogEval& e, const FrameRef* frame);
			void Raise(const std::string& why, const LogEval& e, bool hotkey);
			void WriteDump(const std::string& why, const LogEval& e);
			void AppendRow(const std::string& json);
			void FlushRows();
			bool WriteFile(const std::string& name, const std::string& data, bool append);
			void EnforceCap();
			void LoadFolder();
			void LoadCoverage();
			void SaveCoverage();
			void NoteFailure(const std::string& error);
			void PublishStatus();
			void Notice(const std::string& text);
			std::string Name(const std::string& suffix) const;
			std::string CoverageKey(const LogEval& e, int axis) const;

			EvidenceLogSettings s_;
			std::shared_ptr<IEvidenceFs> fs_;
			NowMsFn nowMs_;
			StampFn stamp_;

			// Worker-only state.
			bool active_ = false;
			EvidenceLevel level_ = EvidenceLevel::Off;
			std::string folder_, mode_, session_, format_;
			bool noticeGiven_ = false;
			bool noFolder_ = false, noFolderNoticed_ = false;
			bool folderLoaded_ = false;
			std::map<std::string, uint64_t> sizes_;      // managed files: name -> bytes
			std::string logName_;
			uint64_t logBytes_ = 0;
			int logIndex_ = 0;
			int dumpIndex_ = 0;
			int segmentIndex_ = 0;
			std::string pending_;                        // rows not yet written
			Segment seg_;
			std::vector<std::shared_ptr<const LineSnapshot>> ring_;
			std::vector<FrameRef> frames_;
			int64_t lastAggUs_ = 0;
			bool haveAgg_ = false;
			uint64_t aggEvals_ = 0, aggInformative_ = 0, aggRepeats_ = 0, aggDropped_ = 0;
			uint64_t lastRepeats_ = 0, lastDropped_ = 0, lastEvalCount_ = 0;
			double aggCostSum_ = 0.0;
			int64_t lastSeq_ = 0;
			LogEval lastEval_;
			bool haveLastEval_ = false;
			// Trigger state.
			double lastDumpMs_ = 0.0;
			bool haveDump_ = false;
			double lastHotkeyMs_ = 0.0;
			bool haveHotkey_ = false;
			std::vector<double> dumpTimesMs_;
			int64_t holdSinceUs_[2] = { -1, -1 };
			bool holdRaised_[2] = { false, false };
			int64_t lastAppliedChangeUs_[2] = { 0, 0 };
			bool haveAppliedChange_[2] = { false, false };
			struct ProbePoint { int64_t timeUs; bool seen; };
			std::vector<ProbePoint> probes_;
			bool powerRaised_ = false;
			std::map<std::string, int> coverage_;
			bool coverageDirty_ = false;

			// Any-thread state.
			mutable std::mutex pub_;
			EvidenceStatus status_;
			std::vector<std::string> notices_;
			std::string hotkeyWhy_;
			bool hotkeyPending_ = false;
		};
	}
}
