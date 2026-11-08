// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// The chain estimator worker (see ChainEstimator.h). C++14. No Win32 here: the disk is behind the evidence log's IEvidenceFs.
#include "ChainEstimator.h"

#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace SourceIdentity
{
	namespace Chain
	{
		namespace
		{
			const char* const kSinkName = "chain";

			double SteadyMs()
			{
				return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
			}

			int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
		}

		struct ChainEstimator::State : public std::enable_shared_from_this<ChainEstimator::State>
		{
			struct ClockEntry
			{
				uint64_t seq = 0;
				double seconds = 0.0;
				LineRect rect;
			};
			struct EventRec
			{
				uint64_t seq = 0;
				int kind = 0;
			};
			struct Job
			{
				std::shared_ptr<const LineSnapshot> snap;
				FrameRef frame;
				uint64_t gen = 0;
			};
			struct AxisRun
			{
				bool have = false;
				LineSet ls;
				VerifyResult vr;
				ProbeResult pr;
				EvidenceUpdate up;
			};

			// ---- configuration ----------------------------------------------------------------------------------------
			EstimatorSettings cfg;
			NowMsFn nowMs;
			Thresholds thr;
			ReadingLevels levels;

			// ---- inputs and the queue (m: leaf lock, never held across work or I/O) ----------------------------------
			mutable std::mutex m;
			std::condition_variable cv;
			std::deque<Job> jobs;
			std::deque<ClockEntry> clock;
			std::vector<EventRec> events;
			std::vector<uint64_t> repeats;
			bool stopping = false;
			bool cfgDirty = false;
			bool resetPending = false;
			std::string resetCause;
			bool hotkeyPending = false;
			std::string hotkeyWhy;
			EstimatorMode reqMode = EstimatorMode::Off;
			bool reqOriginal = false;
			std::string reqFolder;
			EvidenceLevel reqLevel = EvidenceLevel::Off;
			uint64_t runEpoch = 0;     // bumped at every not-running -> running transition (a clean start)
			uint64_t gen = 0;          // bumped at every discontinuity (jobs of an older era are dropped)
			std::string formatInfo;
			bool classicalValid = false;
			double classicalRatio[2] = { 1.0, 1.0 };
			uint64_t streamKey = 0;
			bool haveStreamKey = false;
			bool haveResetClock = false;       // NoteDiscontinuity saw a frame: the reset record's sequence and time
			uint64_t resetSeq = 0;
			int64_t resetTimeUs = 0;

			// ---- lock free flags and counters ---------------------------------------------------------------------------
			std::atomic<int> modeA{ 0 };
			std::atomic<bool> originalA{ false };
			std::atomic<bool> running{ false };
			std::atomic<bool> sync{ false };
			std::atomic<bool> stopped{ false };
			std::atomic<bool> wantFrames{ false };
			std::atomic<bool> latchedPower{ false };
			std::atomic<bool> paused{ false };
			EvaluationObserver observer;   // set before the estimator runs
			std::atomic<double> nextDueMs{ 0.0 };
			std::atomic<double> intervalMs{ 1000.0 };
			std::atomic<double> acquiringIntervalMs{ 1000.0 };   // the PLANNED acquiring interval as of the last evaluation (cost aware)
			std::atomic<double> lastStartMs{ 0.0 };              // wall time the newest evaluation started
			std::atomic<unsigned> nativeHeldMask{ 0u };          // axes (bit 0 = H, 1 = V) on which the monitor left a native-looking frame out since the last evaluation (N2)
			std::atomic<double> lastEventDueMs{ -1e18 };   // the last time an event pulled the next evaluation forward (at most once per second)
			std::atomic<double> lastEvalWallMs{ -1.0 };    // wall time the newest evaluation finished (-1: none since the start)
			std::atomic<double> runStartMs{ 0.0 };         // wall time of the clean start
			std::atomic<uint64_t> streamKeyA{ 0 };
			std::atomic<bool> haveKeyA{ false };
			std::atomic<uint64_t> cEvaluations{ 0 }, cDelivered{ 0 }, cDropped{ 0 }, cRepeats{ 0 }, cNoClock{ 0 }, cUnusable{ 0 },
				cStale{ 0 }, cDiscontinuities{ 0 }, cWindowClears{ 0 };

			// ---- the worker's own state (procMutex: one processing thread at a time) ---------------------------------------
			mutable std::mutex procMutex;
			AxisReader reader[2];
			ChainEvidence evidence;
			PolyphaseProposer proposer[2];
			std::unique_ptr<DescaleVerifier> verifier;
			std::unique_ptr<DescaleVerifier> probeVerifier;
			Governor gov;
			EvidenceLog log;
			Geometry::HeldState held;
			bool appliedRunning = false;
			uint64_t appliedEpoch = 0;
			bool haveArea = false;
			LineRect lastActive;
			uint32_t shotId = 0;
			uint64_t discontinuities = 0, geometryGen = 0, appliedGen = 0;
			int64_t lastTimeUs = 0;
			uint64_t lastSeq = 0;
			int lastAxisLen[2] = { 0, 0 };
			bool haveEvaluated = false;
			uint64_t repeatsLogged = 0;

			// ---- status (statusMutex) ---------------------------------------------------------------------------------------
			mutable std::mutex statusMutex;
			LogEval statusEval;
			bool haveStatusEval = false;
			GovernorPlan statusPlan;
			std::string unusableReason;

			// ---- the tap and the thread -----------------------------------------------------------------------------------
			std::mutex tapMutex;
			const void* tapOwner = nullptr;
			std::mutex threadMutex;
			std::thread worker;
			bool threadStarted = false;

			State(const EstimatorSettings& s, NowMsFn n, std::shared_ptr<IEvidenceFs> fs, StampFn stamp)
				: cfg(s), nowMs(n ? n : NowMsFn(&SteadyMs)), thr(ComputeThresholds(s.evidence)), levels(ReadingLevels::From(thr)),
				evidence(s.evidence), gov(s.governor), log(s.log, std::move(fs), nowMs, std::move(stamp))
			{
				cfg.queueDepth = ClampInt(cfg.queueDepth, 1, 2);
				cfg.clockRing = ClampInt(cfg.clockRing, 4, 4096);
				for (int a = 0; a < 2; ++a)
				{
					reader[a] = AxisReader(cfg.reading, levels);
					proposer[a] = PolyphaseProposer(cfg.proposer);
				}
				BuildVerifiers();
				intervalMs.store(cfg.governor.acquiringIntervalSec * 1000.0);
				acquiringIntervalMs.store(cfg.governor.acquiringIntervalSec * 1000.0);
				statusPlan = gov.Plan(false);
			}

			~State() { Stop(); }

			static double VerifierClock(void* ctx) { return static_cast<State*>(ctx)->nowMs(); }

			void BuildVerifiers()
			{
				VerifierSettings vs = cfg.verifier;
				vs.nowMs = &State::VerifierClock;
				vs.clockCtx = this;
				verifier.reset(new DescaleVerifier(vs));
				probeVerifier.reset(new DescaleVerifier(vs));
			}

			// ---- lifecycle ------------------------------------------------------------------------------------------------
			// With `m` held: the requested mode / Original flag and `running` change together (two threads cannot leave running true
			// beside an applied Off).
			void RecomputeRunningLocked()
			{
				const bool now = modeA.load(std::memory_order_acquire) != static_cast<int>(EstimatorMode::Off) &&
					!originalA.load(std::memory_order_acquire) && !stopped.load(std::memory_order_acquire);
				const bool was = running.exchange(now, std::memory_order_acq_rel);
				if (now && !was)
				{
					++runEpoch;
					cfgDirty = true;
					runStartMs.store(nowMs(), std::memory_order_release);   // the status must not read "stalled" before the worker's first pass
					lastEvalWallMs.store(-1.0, std::memory_order_release);
				}
			}

			void UpdateTap()
			{
				namespace Tap = VpFrameAnalysis::SourceFrameTap;
				std::lock_guard<std::mutex> lk(tapMutex);
				if (tapOwner == nullptr)
					return;
				const bool want = running.load(std::memory_order_acquire) && !stopped.load(std::memory_order_acquire);
				const bool have = Tap::Registered(tapOwner, kSinkName);
				if (want && !have)
				{
					const std::weak_ptr<State> weak = shared_from_this();
					Tap::Register(tapOwner, kSinkName,
						[weak](const VpSkeleton::SourceFrameView& v) {
							if (const std::shared_ptr<State> s = weak.lock())
								s->Deliver(v);
						},
						[weak]() {
							const std::shared_ptr<State> s = weak.lock();
							return s && s->Wants();
						});
				}
				else if (!want && have)
					Tap::Unregister(tapOwner, kSinkName);
			}

			void EnsureThread()
			{
				if (sync.load(std::memory_order_acquire) || !running.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire))
					return;
				std::lock_guard<std::mutex> lk(threadMutex);
				if (threadStarted || stopped.load(std::memory_order_acquire))
					return;
				threadStarted = true;
				worker = std::thread([this]() { Run(); });
			}

			void Wake()
			{
				if (sync.load(std::memory_order_acquire))
				{
					Pump();
					return;
				}
				EnsureThread();
				cv.notify_all();
			}

			void SetMode(EstimatorMode mode)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqMode = mode;
					cfgDirty = true;
					modeA.store(static_cast<int>(mode), std::memory_order_release);
					RecomputeRunningLocked();
				}
				UpdateTap();
				Wake();
			}

			void SetOriginal(bool original)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqOriginal = original;
					cfgDirty = true;
					originalA.store(original, std::memory_order_release);
					RecomputeRunningLocked();
				}
				UpdateTap();
				Wake();
			}

			bool AttachTap(const void* owner)
			{
				if (owner == nullptr)
					return false;
				{
					namespace Tap = VpFrameAnalysis::SourceFrameTap;
					std::lock_guard<std::mutex> lk(tapMutex);
					if (tapOwner != nullptr && tapOwner != owner)
						Tap::Unregister(tapOwner, kSinkName);   // moved to another renderer: the old registration goes
					tapOwner = owner;
				}
				tapOwnerAtomic.store(owner, std::memory_order_release);
				UpdateTap();
				return true;
			}

			void DetachTap()
			{
				namespace Tap = VpFrameAnalysis::SourceFrameTap;
				std::lock_guard<std::mutex> lk(tapMutex);
				if (tapOwner != nullptr)
					Tap::Unregister(tapOwner, kSinkName);
				tapOwner = nullptr;
				tapOwnerAtomic.store(nullptr, std::memory_order_release);
			}

			void Stop()
			{
				stopped.store(true, std::memory_order_release);
				running.store(false, std::memory_order_release);
				DetachTap();
				{
					std::lock_guard<std::mutex> lk(m);
					stopping = true;
				}
				cv.notify_all();
				std::lock_guard<std::mutex> lk(threadMutex);
				if (worker.joinable())
					worker.join();
			}

			void SetEvidence(const std::string& folder, EvidenceLevel level)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqFolder = folder;
					reqLevel = level;
					cfgDirty = true;
				}
				wantFrames.store(level == EvidenceLevel::Frames, std::memory_order_release);
				Wake();
			}

			EvidenceLevel GetEvidenceLevel() const
			{
				std::lock_guard<std::mutex> lk(m);
				return reqLevel;
			}

			void RequestDump(const std::string& why)
			{
				if (!running.load(std::memory_order_acquire))
				{
					// A press while nothing runs is refused visibly, never kept to dump an unrelated moment after a later start.
					log.PostNotice("Source identity: evidence dump ignored (the chain estimator is not running)");
					return;
				}
				{
					std::lock_guard<std::mutex> lk(m);
					hotkeyPending = true;
					hotkeyWhy = why.empty() ? std::string("hotkey") : why;
				}
				Wake();
			}

			// ---- inputs ----------------------------------------------------------------------------------------------------
			void NoteFrame(uint64_t seq, double seconds, int x, int y, int w, int h)
			{
				if (!running.load(std::memory_order_acquire))
					return;
				ClockEntry c;
				c.seq = seq;
				c.seconds = seconds;
				c.rect.x = x;
				c.rect.y = y;
				c.rect.w = w;
				c.rect.h = h;
				std::lock_guard<std::mutex> lk(m);
				clock.push_back(c);
				while (static_cast<int>(clock.size()) > cfg.clockRing)
					clock.pop_front();
			}

			void NoteEvent(EstimatorEvent kind, uint64_t seq)
			{
				if (!running.load(std::memory_order_acquire))
					return;
				EventRec e;
				e.seq = seq;
				e.kind = static_cast<int>(kind);
				{
					std::lock_guard<std::mutex> lk(m);
					events.push_back(e);
					while (static_cast<int>(events.size()) > cfg.eventCap)
						events.erase(events.begin());
				}
				if (latchedPower.load(std::memory_order_acquire))
				{
					// While latched the cadence is long (5 s, more when stretched): a cut / blank / fade pulls the next evaluation forward
					// (at most once per second) so the regime boundary is seen at the cut, not seconds later (doc 78 section 3).
					const double now = nowMs();
					const double last = lastEventDueMs.load(std::memory_order_acquire);
					if (now - last >= 1000.0 || now < last)
					{
						lastEventDueMs.store(now, std::memory_order_release);
						if (nextDueMs.load(std::memory_order_acquire) > now)
							nextDueMs.store(now, std::memory_order_release);
					}
				}
			}

			// N2 (S1 bound): the monitor left a native-looking informative frame out on the axes of `mask` because this estimator reads them
			// confidently below native. While that goes on, the estimator's own reading must be re-tested quickly: the confident cadence
			// (5 s, 2 % share) does not apply; the evaluation after this note, and every one while notes keep coming, runs at the
			// ACQUIRING cadence, and an evaluation already scheduled at the long interval is pulled forward to one acquiring interval
			// after the last start. No new fixed number: both intervals are the governor's plan. Leaf atomics only (render / analysis thread).
			void NoteNativeHeld(unsigned mask)
			{
				if (mask == 0u || !running.load(std::memory_order_acquire))
					return;
				nativeHeldMask.fetch_or(mask, std::memory_order_acq_rel);
				if (latchedPower.exchange(false, std::memory_order_acq_rel))
				{
					const double acq = acquiringIntervalMs.load(std::memory_order_acquire);
					const double due = lastStartMs.load(std::memory_order_acquire) + acq;
					if (nextDueMs.load(std::memory_order_acquire) > due)
						nextDueMs.store(due, std::memory_order_release);
					intervalMs.store(acq, std::memory_order_release);
				}
			}

			void NoteRepeat(uint64_t seq)
			{
				if (!running.load(std::memory_order_acquire))
					return;
				std::lock_guard<std::mutex> lk(m);
				repeats.push_back(seq);
				while (static_cast<int>(repeats.size()) > cfg.repeatCap)
					repeats.erase(repeats.begin());
			}

			void NoteDiscontinuity(const char* cause)
			{
				if (!running.load(std::memory_order_acquire))
					return;
				{
					std::lock_guard<std::mutex> lk(m);
					++gen;
					if (!clock.empty())   // a second call right behind the first (the clock is already cleared) keeps the first call's record
					{
						haveResetClock = true;
						resetSeq = clock.back().seq;
						resetTimeUs = static_cast<int64_t>(std::llround(clock.back().seconds * 1e6));
					}
					jobs.clear();
					events.clear();
					repeats.clear();
					clock.clear();
					resetPending = true;
					resetCause = cause != nullptr ? cause : "discontinuity";
				}
				Wake();
			}

			void SetStreamKey(uint64_t key)
			{
				if (haveKeyA.load(std::memory_order_acquire) && streamKeyA.load(std::memory_order_acquire) == key)
					return;
				bool changed = false;
				{
					std::lock_guard<std::mutex> lk(m);
					changed = haveStreamKey && key != streamKey;
					streamKey = key;
					haveStreamKey = true;
				}
				streamKeyA.store(key, std::memory_order_release);
				haveKeyA.store(true, std::memory_order_release);
				if (changed)
					NoteDiscontinuity("stream key changed");
			}

			void SetClassical(bool valid, double ratioH, double ratioV)
			{
				std::lock_guard<std::mutex> lk(m);
				classicalValid = valid;
				classicalRatio[0] = ratioH;
				classicalRatio[1] = ratioV;
			}

			void SetFormatInfo(const std::string& text)
			{
				std::lock_guard<std::mutex> lk(m);
				formatInfo = text;
			}

			// ---- the sink -----------------------------------------------------------------------------------------------
			bool Wants() const
			{
				return running.load(std::memory_order_acquire) && nowMs() >= nextDueMs.load(std::memory_order_acquire);
			}

			// With `m` held: the source time and active rectangle of frame `seq` (interpolated, extrapolated beyond the ends).
			bool ResolveClock(uint64_t seq, double& seconds, LineRect& rect) const
			{
				if (clock.empty())
					return false;
				const ClockEntry* lo = nullptr;   // greatest seq <= seq
				const ClockEntry* hi = nullptr;   // smallest seq >= seq
				for (const ClockEntry& c : clock)
				{
					if (c.seq <= seq && (lo == nullptr || c.seq > lo->seq))
						lo = &c;
					if (c.seq >= seq && (hi == nullptr || c.seq < hi->seq))
						hi = &c;
				}
				if (lo != nullptr && hi != nullptr)
				{
					rect = lo->rect;
					if (lo->seq == hi->seq)
						seconds = lo->seconds;
					else
						seconds = lo->seconds + (hi->seconds - lo->seconds) * static_cast<double>(seq - lo->seq) / static_cast<double>(hi->seq - lo->seq);
					return true;
				}
				const ClockEntry* edge = lo != nullptr ? lo : hi;   // seq lies beyond the newest (lo) or before the oldest (hi)
				const ClockEntry* nb = nullptr;
				for (const ClockEntry& c : clock)
				{
					if (&c == edge)
						continue;
					if (lo != nullptr)
					{
						if (c.seq < edge->seq && (nb == nullptr || c.seq > nb->seq))
							nb = &c;
					}
					else if (c.seq > edge->seq && (nb == nullptr || c.seq < nb->seq))
						nb = &c;
				}
				double period = 0.0;   // seconds per sequence step
				if (nb != nullptr)
					period = (edge->seconds - nb->seconds) / (static_cast<double>(edge->seq) - static_cast<double>(nb->seq));
				const double dseq = static_cast<double>(seq) - static_cast<double>(edge->seq);
				rect = edge->rect;
				seconds = edge->seconds + (period > 0.0 ? dseq * period : 0.0);
				return true;
			}

			void Deliver(const VpSkeleton::SourceFrameView& v)
			{
				if (!running.load(std::memory_order_acquire) || !v.Valid())
					return;
				const double now = nowMs();
				if (now < nextDueMs.load(std::memory_order_acquire))
					return;   // a frame handed over although none is due (the predicate and the delivery raced): ignored
				double seconds = 0.0;
				LineRect rect;
				bool haveClock = false, repeat = false;
				uint64_t g = 0, key = 0;
				{
					std::lock_guard<std::mutex> lk(m);
					g = gen;
					key = streamKey;
					haveClock = ResolveClock(v.seq, seconds, rect);
					for (uint64_t r : repeats)
						if (r == v.seq)
							repeat = true;
				}
				if (repeat)
				{
					cRepeats.fetch_add(1, std::memory_order_relaxed);
					return;   // the due slot is kept: the next frame is taken instead
				}
				if (!haveClock)
				{
					cNoClock.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				// The active rectangle inside the frame (w <= 0: the whole frame).
				if (rect.w <= 0 || rect.h <= 0)
				{
					rect.x = 0;
					rect.y = 0;
					rect.w = static_cast<int>(v.width);
					rect.h = static_cast<int>(v.height);
				}
				rect.x = (std::max)(0, (std::min)(rect.x, static_cast<int>(v.width) - 1));
				rect.y = (std::max)(0, (std::min)(rect.y, static_cast<int>(v.height) - 1));
				rect.w = (std::min)(rect.w, static_cast<int>(v.width) - rect.x);
				rect.h = (std::min)(rect.h, static_cast<int>(v.height) - rect.y);
				const bool latched = latchedPower.load(std::memory_order_acquire);
				const LinePlan plan = LineSampler::Plan(rect, v.seq, latched ? cfg.samplerLatched : cfg.sampler);
				const double interval = intervalMs.load(std::memory_order_acquire);
				if (!plan.valid)
				{
					cUnusable.fetch_add(1, std::memory_order_relaxed);
					{
						std::lock_guard<std::mutex> lk(statusMutex);
						unusableReason = "active area too small for the lines";
					}
					nextDueMs.store(now + interval, std::memory_order_release);
					return;
				}
				std::shared_ptr<LineSnapshot> snap = std::make_shared<LineSnapshot>();
				const double scale = std::pow(2.0, static_cast<double>(v.msbShift) - 6.0);   // codes of the tap's plane -> 10-bit codes
				if (!CaptureLines<uint16_t>(plan, v.luma, static_cast<std::ptrdiff_t>(v.strideSamples), scale, *snap))
				{
					cUnusable.fetch_add(1, std::memory_order_relaxed);
					nextDueMs.store(now + interval, std::memory_order_release);
					return;
				}
				{
					std::lock_guard<std::mutex> lk(statusMutex);
					unusableReason.clear();   // lines were taken: an earlier "area too small" no longer applies
				}
				snap->seq = v.seq;
				snap->timeUs = static_cast<int64_t>(std::llround(seconds * 1e6));
				snap->streamKey = key;
				Job job;
				job.gen = g;
				if (wantFrames.load(std::memory_order_acquire))
				{
					job.frame.keepAlive = v.keepAlive;
					job.frame.luma = v.luma;
					job.frame.width = v.width;
					job.frame.height = v.height;
					job.frame.strideSamples = v.strideSamples;
					job.frame.msbShift = v.msbShift;
					job.frame.seq = v.seq;
				}
				{
					std::lock_guard<std::mutex> lk(m);
					if (gen != g)
						return;   // a discontinuity during the copy: this frame belongs to the old source
					uint32_t flags = 0;
					int32_t cuts = 0;
					for (size_t i = 0; i < events.size();)
					{
						if (events[i].seq <= v.seq)
						{
							if (events[i].kind == static_cast<int>(EstimatorEvent::Cut))
							{
								flags |= kEvCut;
								++cuts;   // each cut is a shot (ChainEvidence's per-shot cap)
							}
							else if (events[i].kind == static_cast<int>(EstimatorEvent::ClearWindow))
								flags |= kEvClearWindow;   // the evidence window is cleared before this evaluation (Process)
							else
								flags |= events[i].kind == static_cast<int>(EstimatorEvent::Blank) ? kEvBlank : kEvFade;
							events.erase(events.begin() + static_cast<std::ptrdiff_t>(i));
						}
						else
							++i;
					}
					snap->flags = flags;
					snap->cuts = cuts;
					job.snap = snap;
					while (static_cast<int>(jobs.size()) >= cfg.queueDepth)
					{
						// Newest wins, but the events the dropped job carried were consumed from the pending list: they move to the new
						// snapshot, so the shot count and the flags in the log still see them.
						const std::shared_ptr<const LineSnapshot>& old = jobs.front().snap;
						if (old)
						{
							snap->flags |= old->flags;
							snap->cuts += old->cuts;
						}
						jobs.pop_front();
						cDropped.fetch_add(1, std::memory_order_relaxed);
					}
					jobs.push_back(std::move(job));
				}
				cDelivered.fetch_add(1, std::memory_order_relaxed);
				nextDueMs.store(now + interval, std::memory_order_release);
				if (sync.load(std::memory_order_acquire))
					Pump();
				else
					cv.notify_one();
			}

			// The replay entry (see ChainEstimator::SubmitLines).
			bool SubmitSnapshot(const LineSnapshot& snap)
			{
				if (!running.load(std::memory_order_acquire))
					return false;
				Job job;
				std::shared_ptr<LineSnapshot> copy = std::make_shared<LineSnapshot>(snap);
				{
					std::lock_guard<std::mutex> lk(m);
					job.gen = gen;
					// A pending ClearWindow event of a frame at or before this snapshot's sequence is consumed here as Deliver would (the clear is
					// idempotent, so a dumped flag and a pending event cannot double count); cuts / blanks / fades are NOT merged: the replay carries its own.
					for (size_t i = 0; i < events.size();)
					{
						if (events[i].kind == static_cast<int>(EstimatorEvent::ClearWindow) && events[i].seq <= snap.seq)
						{
							copy->flags |= kEvClearWindow;
							events.erase(events.begin() + static_cast<std::ptrdiff_t>(i));
						}
						else
							++i;
					}
					job.snap = copy;
					while (static_cast<int>(jobs.size()) >= cfg.queueDepth)
					{
						jobs.pop_front();
						cDropped.fetch_add(1, std::memory_order_relaxed);
					}
					jobs.push_back(std::move(job));
				}
				if (sync.load(std::memory_order_acquire))
					Pump();
				else
					cv.notify_one();
				return true;
			}

			void Pause(bool pause)
			{
				{
					std::lock_guard<std::mutex> lk(m);   // under the worker's wait lock: no lost wake-up
					paused.store(pause, std::memory_order_release);
				}
				if (!pause)
					cv.notify_all();
			}

			// ---- the worker ---------------------------------------------------------------------------------------------
			void Run()
			{
				for (;;)
				{
					{
						std::unique_lock<std::mutex> lk(m);
						cv.wait(lk, [this]() {
							return stopping || (!paused.load(std::memory_order_acquire) && (!jobs.empty() || cfgDirty || resetPending || hotkeyPending));
						});
						if (stopping)
							return;
					}
					Pump();
				}
			}

			// All the work that is waiting, on the calling thread (the worker, or any thread in synchronous mode).
			void Pump()
			{
				auto heldBack = [this]() { return paused.load(std::memory_order_acquire) && !sync.load(std::memory_order_acquire); };
				if (heldBack())
					return;
				std::lock_guard<std::mutex> pl(procMutex);
				while (!heldBack() && Step())   // a pause set while draining takes effect before the next job
				{
				}
			}

			bool Step()
			{
				Job job;
				bool haveJob = false, dirty = false, reset = false, hot = false;
				std::string cause, hotWhy, folder;
				EstimatorMode mode = EstimatorMode::Off;
				bool original = false;
				EvidenceLevel level = EvidenceLevel::Off;
				uint64_t epoch = 0, g = 0;
				{
					std::lock_guard<std::mutex> lk(m);
					dirty = cfgDirty;
					cfgDirty = false;
					mode = reqMode;
					original = reqOriginal;
					folder = reqFolder;
					level = reqLevel;
					epoch = runEpoch;
					reset = resetPending;
					resetPending = false;
					cause = resetCause;
					hot = hotkeyPending;
					hotkeyPending = false;
					hotWhy = hotkeyWhy;
					g = gen;
					if (!jobs.empty())
					{
						job = std::move(jobs.front());
						jobs.pop_front();
						haveJob = true;
					}
				}
				if (!dirty && !reset && !hot && !haveJob)
					return false;
				if (dirty)
					ApplyConfig(mode, original, folder, level, epoch);
				if (reset && appliedRunning)
					DoReset(cause);
				if (hot)
					log.RequestDump(hotWhy);
				if (haveJob)
				{
					if (!appliedRunning || job.gen != g)
						cStale.fetch_add(1, std::memory_order_relaxed);
					else
						Process(job);
				}
				log.Poll();
				return true;
			}

			void ApplyConfig(EstimatorMode mode, bool original, const std::string& folder, EvidenceLevel level, uint64_t epoch)
			{
				const bool wantRun = mode != EstimatorMode::Off && !original && !stopped.load(std::memory_order_acquire);
				if (wantRun && (!appliedRunning || epoch != appliedEpoch))
					FreshStart();
				else if (!wantRun && appliedRunning)
					StopRun();
				appliedRunning = wantRun;
				appliedEpoch = epoch;
				log.Configure(folder, level, wantRun, mode == EstimatorMode::Shadow ? "shadow" : "on");
			}

			void FreshStart()
			{
				for (int a = 0; a < 2; ++a)
				{
					reader[a] = AxisReader(cfg.reading, levels);
					proposer[a].Reset();
					lastAxisLen[a] = 0;
				}
				evidence = ChainEvidence(cfg.evidence);
				verifier->ClearCache();
				probeVerifier->ClearCache();
				gov.Reset();
				shotId = 0;
				discontinuities = 0;
				geometryGen = 0;
				appliedGen = 0;
				haveArea = false;
				haveEvaluated = false;
				lastTimeUs = 0;
				lastSeq = 0;
				latchedPower.store(false, std::memory_order_release);
				nativeHeldMask.store(0u, std::memory_order_release);
				held.Reset();
				nextDueMs.store(0.0, std::memory_order_release);
				intervalMs.store(cfg.governor.acquiringIntervalSec * 1000.0, std::memory_order_release);
				acquiringIntervalMs.store(cfg.governor.acquiringIntervalSec * 1000.0, std::memory_order_release);
				lastEventDueMs.store(-1e18, std::memory_order_release);
				lastEvalWallMs.store(-1.0, std::memory_order_release);
				runStartMs.store(nowMs(), std::memory_order_release);
				{
					std::lock_guard<std::mutex> lm(m);
					haveResetClock = false;   // the discontinuity record of an earlier run is meaningless now
				}
				std::lock_guard<std::mutex> lk(statusMutex);
				haveStatusEval = false;
				statusPlan = gov.Plan(false);
				unusableReason.clear();
			}

			void StopRun()
			{
				log.Close(lastTimeUs, "chain estimator off");
				held.Reset();
				latchedPower.store(false, std::memory_order_release);
				nextDueMs.store(0.0, std::memory_order_release);
			}

			static bool AxisConfident(const AxisReader& r) { return r.HasConfident(); }

			Geometry::Record MakeRecord(const ReadingOutput out[2], uint64_t seq, int64_t timeUs, uint64_t recordStreamKey, const LineRect& area,
				double costMs, double intervalSec)
			{
				Geometry::Record rec;
				rec.flags = static_cast<uint16_t>(modeA.load(std::memory_order_acquire) == static_cast<int>(EstimatorMode::Shadow) ? Geometry::kFlagShadow : 0);
				rec.streamKey = recordStreamKey;   // the evaluated snapshot's key, not the live one (a key change mid-evaluation)
				rec.evalSeq = seq;
				rec.evalTimeUs = timeUs;
				rec.maxAgeUs = Geometry::MaxAgeUs(static_cast<int64_t>(std::llround(intervalSec * 1e6)));
				rec.geometryGen = geometryGen;
				rec.appliedGen = appliedGen;
				rec.discontinuities = static_cast<uint32_t>(discontinuities);
				rec.costMs = static_cast<float>(costMs);
				for (int a = 0; a < 2; ++a)
				{
					rec.measured[a] = out[a].measured;
					rec.applied[a] = out[a].applied;
				}
				rec.area.x = area.x;
				rec.area.y = area.y;
				rec.area.w = area.w;
				rec.area.h = area.h;
				rec.area.conf = area.w > 0 ? 1.0f : 0.0f;
				const bool l0 = out[0].applied.state == Geometry::LatchState::Latched;   // Latched = the reserved numeric of CONFIDENT
				const bool l1 = out[1].applied.state == Geometry::LatchState::Latched;
				const bool up = (l0 && out[0].applied.n < out[0].applied.N) || (l1 && out[1].applied.n < out[1].applied.N);
				if (up)
					rec.codedRaster = Geometry::CodedRaster::PlayerUpscaled;
				else if (l0 && l1)
					rec.codedRaster = Geometry::CodedRaster::NativeCoded;
				return rec;
			}

			// A discontinuity: every unit back to UNKNOWN, one reset record, the log's segment closed.
			void DoReset(const std::string& cause)
			{
				++discontinuities;
				cDiscontinuities.fetch_add(1, std::memory_order_relaxed);
				ReadingOutput out[2];
				for (int a = 0; a < 2; ++a)
				{
					FieldEvidence fe;
					fe.axisLen = lastAxisLen[a];
					EvalContext ctx;
					ctx.seq = lastSeq;
					ctx.timeUs = lastTimeUs;
					ctx.discontinuity = true;
					out[a] = reader[a].Step(fe, ctx);
					proposer[a].Reset();
				}
				evidence.Reset();
				haveArea = false;
				latchedPower.store(false, std::memory_order_release);
				if (out[0].measuredChanged || out[1].measuredChanged)
					++geometryGen;
				if (out[0].appliedChanged || out[1].appliedChanged)
					++appliedGen;
				nativeHeldMask.store(0u, std::memory_order_release);
				nextDueMs.store(0.0, std::memory_order_release);   // acquisition restarts at once
				intervalMs.store(cfg.governor.acquiringIntervalSec * 1000.0, std::memory_order_release);
				if (haveEvaluated)
				{
					const GovernorPlan plan = gov.Plan(false);
					// evalSeq = the era's FIRST sequence: one past the newest frame the monitor had announced (or evaluated) when the
					// discontinuity came. The monitor's floor is the row sequence after the last analysed frame, so the newest announced
					// frame itself can sit just below it; one past never does. The reset clock is consumed here.
					uint64_t base = lastSeq;
					int64_t timeUs = lastTimeUs;
					{
						std::lock_guard<std::mutex> lk(m);
						if (haveResetClock)
						{
							if (resetSeq >= base)
							{
								base = resetSeq;
								timeUs = resetTimeUs;
							}
							haveResetClock = false;
						}
					}
					const uint64_t seq = base + 1;
					lastSeq = seq;
					lastTimeUs = timeUs;
					held.Publish(MakeRecord(out, seq, timeUs, streamKeyA.load(std::memory_order_acquire), lastActive, 0.0, plan.intervalSec));
				}
				log.OnDiscontinuity(lastTimeUs, cause);
				std::lock_guard<std::mutex> lk(statusMutex);
				statusPlan = gov.Plan(false);
			}

			void FillLogAxis(LogAxis& L, const AxisRun& run, const ReadingOutput& out, int axisLen) const
			{
				L.evaluated = run.have;
				L.informative = run.up.informative;
				L.hold = run.up.hold;
				L.state = out.state;
				L.appliedState = out.applied.state;
				L.N = axisLen;
				L.measuredN = out.measured.n;
				L.appliedN = out.applied.n;
				L.measuredKernel = out.measured.kernelIndex;
				L.appliedKernel = out.applied.kernelIndex;
				L.measuredLlr = out.measured.llr;
				L.appliedLlr = out.applied.llr;
				L.kernelConf = out.measured.kernelConf;
				L.reasonBits = out.reasonBits;
				L.readingReason = static_cast<int>(out.reason);
				L.stateChanged = out.stateChanged;
				L.measuredChanged = out.measuredChanged;
				L.appliedChanged = out.appliedChanged;
				if (!run.have)
					return;
				L.lines = run.ls.count;
				const int n = run.up.candidateCount;
				int order[kMaxCandidates];
				for (int i = 0; i < n; ++i)
					order[i] = i;
				std::stable_sort(order, order + n, [&run](int a, int b) {
					const CandidateEvidence& x = run.up.cand[a];
					const CandidateEvidence& y = run.up.cand[b];
					if (x.informative != y.informative)
						return x.informative;
					if (x.increment != y.increment)
						return x.increment > y.increment;
					return x.n < y.n;
				});
				L.candCount = n < 3 ? n : 3;
				for (int i = 0; i < L.candCount; ++i)
				{
					const CandidateEvidence& c = run.up.cand[order[i]];
					L.top[i].n = c.n;
					L.top[i].stat = c.stat;
					L.top[i].increment = c.increment;
					L.top[i].kernel = c.kernel;
					L.top[i].familyMargin = c.familyMargin;
					L.top[i].informative = c.informative;
				}
				if (n > 0)
				{
					L.gatedLines = run.up.cand[order[0]].linesGated;
					L.maskedLines = run.up.cand[order[0]].linesMasked;
				}
				L.probeRan = run.pr.ran;
				L.probeSeen = run.pr.seen;
			}

			// One evaluation of both axes (the worker, or the calling thread in synchronous mode).
			void Process(const Job& job)
			{
				const LineSnapshot& s = *job.snap;
				const double t0 = nowMs();
				gov.NoteStart(t0);
				lastStartMs.store(t0, std::memory_order_release);
				std::string fmt;
				bool classicalOk = false;
				double classical[2] = { 1.0, 1.0 };
				{
					std::lock_guard<std::mutex> lk(m);
					fmt = formatInfo;
					classicalOk = classicalValid;
					classical[0] = classicalRatio[0];
					classical[1] = classicalRatio[1];
				}
				log.SetFormatInfo(fmt);

				const bool areaChanged = haveArea && (s.active.x != lastActive.x || s.active.y != lastActive.y || s.active.w != lastActive.w ||
					s.active.h != lastActive.h);
				haveArea = true;
				lastActive = s.active;
				uint32_t flags = s.flags;
				if (areaChanged)
				{
					flags |= kEvAreaChanged;
					proposer[0].Reset();
					proposer[1].Reset();
				}
				const int cutsNow = s.cuts > 0 ? s.cuts : ((flags & kEvCut) != 0u ? 1 : 0);
				shotId += static_cast<uint32_t>(cutsNow);
				// Revert window: the evidence window (both axes, as a resync reset does without the reset record) is cleared BEFORE this evaluation
				// on a ClearWindow event (the monitor: the end of a menu / mixed episode, the first cut after one) and at the first cut after a HOLD
				// evaluation of this estimator (ChainEvidence::NoteCut). Black frames and ordinary cuts do not clear. The reading is a pure function
				// of the evidence, so this evaluation reads UNSURE until new evidence builds; a genuine native picture is then no longer
				// contradicted by the old reading.
				bool windowCleared = false;
				if ((flags & kEvClearWindow) != 0u)
				{
					evidence.Reset();
					windowCleared = true;
				}
				else if (cutsNow > 0)
					windowCleared = evidence.NoteCut();
				if (windowCleared)
				{
					proposer[0].Reset();
					proposer[1].Reset();
					cWindowClears.fetch_add(1, std::memory_order_relaxed);
				}
				const bool latchedMode = latchedPower.load(std::memory_order_acquire);
				const int step = cfg.outputStepOverride > 0 ? cfg.outputStepOverride : DetectStepCodes(s);
				const double floorCodes = step >= 4 ? 1.155 : 0.289;   // the dof-normalised rounding / dither sigma (ChainEvidence.h)

				AxisRun run[2];
				for (int a = 0; a < 2; ++a)
				{
					AxisRun& r = run[a];
					if (!ToLineSet(s, a, r.ls))
						continue;
					r.have = true;
					const int N = r.ls.axisLen;
					proposer[a].AddEvaluation(r.ls);
					int caller[4];
					int cc = 0;
					auto addCaller = [&caller, &cc](int n) {
						if (n < 16 || cc >= 4)
							return;
						for (int i = 0; i < cc; ++i)
							if (caller[i] == n)
								return;
						caller[cc++] = n;
					};
					if (reader[a].HasConfident() && !reader[a].ConfidentNative())
						addCaller(reader[a].ConfidentN());
					const AxisSummary prev = evidence.Summary(a);
					if (prev.hasBest)
						addCaller(prev.n);
					addCaller(RoundToInt(static_cast<double>(N) * 0.5));
					addCaller(RoundToInt(static_cast<double>(N) * 2.0 / 3.0));
					ProposedCandidate prop[kMaxCandidates];
					const int pc = proposer[a].Propose(N, caller, cc, prop, kMaxCandidates);
					int cands[kMaxCandidates];
					for (int i = 0; i < pc; ++i)
						cands[i] = prop[i].n;
					VerifyOptions opt;
					if (latchedMode)
						opt.exactLines = cfg.latchedExactLines;
					if (reader[a].HasConfident() && !reader[a].ConfidentNative())
						opt.forceExactN = reader[a].ConfidentN();
					verifier->Evaluate(r.ls, cands, pc, opt, r.vr);
					PowerProbe::Run(*probeVerifier, r.ls, floorCodes, static_cast<double>(step), cfg.probe, r.pr);
				}
				for (int a = 0; a < 2; ++a)
				{
					if (!run[a].have)
						continue;
					AxisObservation obs;
					obs.verify = &run[a].vr;
					obs.axisLen = run[a].ls.axisLen;
					obs.floorCodes = floorCodes;
					obs.probeRan = run[a].pr.ran;
					obs.probeSeen = run[a].pr.seen;
					obs.probeReason = run[a].pr.reason;
					obs.shotId = shotId;
					obs.timeUs = s.timeUs;   // the recent window (N3) follows the measured evaluation rate in source time
					evidence.Submit(a, obs, run[a].up);
				}
				if (observer)
				{
					EvaluationDetail d;
					d.seq = s.seq;
					d.timeUs = s.timeUs;
					d.streamKey = s.streamKey;
					d.shotId = shotId;   // the estimator's own detected shot (obs.shotId above)
					d.stepCodes = step;
					for (int a = 0; a < 2; ++a)
					{
						AxisDetail& ad = d.axis[a];
						ad.have = run[a].have;
						if (!run[a].have)
							continue;
						ad.N = run[a].ls.axisLen;
						ad.lines = run[a].ls.count;
						ad.probeRan = run[a].pr.ran;
						ad.probeSeen = run[a].pr.seen;
						ad.floorCodes = floorCodes;
						int k = 0;   // ChainEvidence::Submit numbers the exact-stage candidates in order
						for (size_t i = 0; i < run[a].vr.candidates.size(); ++i)
						{
							const CandidateResult& cr = run[a].vr.candidates[i];
							if (!cr.valid)
								continue;
							CandidateDetail cd;
							cd.nProposed = cr.nProposed;
							cd.n = cr.n;
							cd.exact = cr.exactDone;
							cd.stat = cr.stat;
							cd.coarseStat = cr.coarseStat;
							if (cr.exactDone)
							{
								cd.kernel = cr.kernel;
								cd.kernelMargin = cr.kernelMargin;
								cd.familyMargin = cr.familyMargin;
								if (cr.lineCount >= 1 && k < run[a].up.candidateCount)
								{
									cd.linesGated = run[a].up.cand[k].linesGated;
									cd.informative = run[a].up.cand[k].informative && run[a].up.cand[k].statValid && !run[a].up.hold;   // a HOLD evaluation accumulates nothing (SID-CAL r2 M1)
									cd.evJudged = run[a].up.cand[k].statValid;
									cd.evStat = run[a].up.cand[k].stat;
									++k;
								}
							}
							ad.cands.push_back(cd);
						}
					}
					observer(d);
				}
				ReadingOutput out[2];
				bool anyReset = false;
				for (int a = 0; a < 2; ++a)
				{
					FieldEvidence fe;
					if (run[a].have)
					{
						const AxisSummary sum = evidence.Summary(a);
						fe = MakeFieldEvidence(sum, run[a].up, run[a].ls.axisLen, cfg.reading, &run[a].vr);
						lastAxisLen[a] = run[a].ls.axisLen;
					}
					EvalContext ctx;
					ctx.seq = s.seq;
					ctx.timeUs = s.timeUs;
					ctx.activeAreaChanged = (flags & kEvAreaChanged) != 0u;
					out[a] = reader[a].Step(fe, ctx);
					if (out[a].resetEvidence)
					{
						evidence.ResetAxis(a);
						proposer[a].Reset();
						anyReset = true;
					}
				}
				if (anyReset)
				{
					++discontinuities;
					cDiscontinuities.fetch_add(1, std::memory_order_relaxed);
				}
				if (out[0].measuredChanged || out[1].measuredChanged)
					++geometryGen;
				if (out[0].appliedChanged || out[1].appliedChanged)
					++appliedGen;

				bool cold = false;
				for (int a = 0; a < 2; ++a)
					if (run[a].have && (run[a].vr.cold || run[a].pr.cold))
						cold = true;
				const double cost = nowMs() - t0;
				gov.NoteCost(cost, cold);
				// Both axes CONFIDENT: the long cadence (5 s, 2% share); anything else evaluates at the acquiring cadence. maxAgeUs follows
				// the PLANNED interval, not the nominal one.
				// N2 (S1 bound): while the monitor keeps leaving native-looking frames out on the strength of this very reading (a note since
				// the last evaluation), the long cadence does not apply: the reading is re-tested at the acquiring cadence, so a source that
				// genuinely became native flips it quickly (and the window of ChainEvidence follows the measured, shorter interval).
				const bool bothConfident = AxisConfident(reader[0]) && AxisConfident(reader[1]);
				const bool nativeHeld = nativeHeldMask.exchange(0u, std::memory_order_acq_rel) != 0u;
				const bool latchedWithPower = bothConfident && !nativeHeld;
				latchedPower.store(latchedWithPower, std::memory_order_release);
				acquiringIntervalMs.store(gov.Plan(false).intervalSec * 1000.0, std::memory_order_release);
				const GovernorPlan plan = gov.Plan(latchedWithPower);
				nextDueMs.store(t0 + plan.intervalSec * 1000.0, std::memory_order_release);
				intervalMs.store(plan.intervalSec * 1000.0, std::memory_order_release);

				const Geometry::Record rec = MakeRecord(out, s.seq, s.timeUs, s.streamKey, s.active, cost, plan.intervalSec);
				held.Publish(rec);
				haveEvaluated = true;
				lastTimeUs = s.timeUs;
				lastSeq = s.seq;
				cEvaluations.fetch_add(1, std::memory_order_relaxed);
				lastEvalWallMs.store(nowMs(), std::memory_order_release);

				LogEval le;
				le.seq = s.seq;
				le.timeUs = s.timeUs;
				le.streamKey = s.streamKey;
				le.flags = flags;
				le.active = s.active;
				for (int a = 0; a < 2; ++a)
					FillLogAxis(le.axis[a], run[a], out[a], run[a].have ? run[a].ls.axisLen : lastAxisLen[a]);
				le.costMs = cost;
				le.cold = cold;
				le.intervalSec = plan.intervalSec;
				le.c1 = thr.c1;
				le.stepCodes = step;
				le.classicalValid = classicalOk;
				le.classicalRatio[0] = classical[0];
				le.classicalRatio[1] = classical[1];
				le.repeatsSkipped = cRepeats.load(std::memory_order_relaxed);
				le.queueDropped = cDropped.load(std::memory_order_relaxed);
				le.evalCount = cEvaluations.load(std::memory_order_relaxed);
				le.lines = job.snap;
				log.OnEvaluation(le, job.frame.Valid() ? &job.frame : nullptr);
				{
					std::lock_guard<std::mutex> lk(statusMutex);
					statusEval = le;
					statusEval.lines.reset();
					haveStatusEval = true;
					statusPlan = plan;
				}
			}

			// ---- status ---------------------------------------------------------------------------------------------------
			ChainStatusInput StatusInput() const
			{
				ChainStatusInput in;
				const int mode = modeA.load(std::memory_order_acquire);
				in.shadow = mode == static_cast<int>(EstimatorMode::Shadow);
				{
					namespace Tap = VpFrameAnalysis::SourceFrameTap;
					const void* owner = tapOwnerAtomic.load(std::memory_order_acquire);
					// Attached = an owner is set AND the sink is still registered with it (a host teardown removes registrations silently).
					in.tapAttached = owner != nullptr && Tap::Registered(owner, kSinkName);
					in.tapLost = running.load(std::memory_order_acquire) && owner != nullptr && !in.tapAttached;
				}
				in.running = running.load(std::memory_order_acquire);
				if (in.running)
				{
					const double last = lastEvalWallMs.load(std::memory_order_acquire);
					const double since = nowMs() - (last >= 0.0 ? last : runStartMs.load(std::memory_order_acquire));
					in.wallMsSinceLastEvaluation = since < 0.0 ? 0.0 : since;
				}
				if (!in.running)
				{
					if (originalA.load(std::memory_order_acquire))
						in.off = "Original pathway";
					else if (mode == static_cast<int>(EstimatorMode::Off))
						in.off = "chain estimator off";
					else
						in.off = "stopped";
				}
				in.haveRecord = held.ReadLatest(0, in.record);
				{
					std::lock_guard<std::mutex> lk(statusMutex);
					in.haveEval = haveStatusEval;
					in.last = statusEval;
					in.governor = statusPlan;
					in.unusableReason = unusableReason;
				}
				const EstimatorCounters c = Counters();
				in.evaluations = c.evaluations;
				in.delivered = c.delivered;
				in.dropped = c.dropped;
				in.repeatsSkipped = c.repeatsSkipped;
				in.noClock = c.noClock;
				in.unusable = c.unusable;
				in.stale = c.stale;
				in.evidence = log.GetStatus();
				in.evidenceText = log.StatusText();
				return in;
			}

			bool TapAttachedConst() const
			{
				return tapOwnerAtomic.load(std::memory_order_acquire) != nullptr;
			}
			std::atomic<const void*> tapOwnerAtomic{ nullptr };

			EstimatorCounters Counters() const
			{
				EstimatorCounters c;
				c.evaluations = cEvaluations.load(std::memory_order_relaxed);
				c.delivered = cDelivered.load(std::memory_order_relaxed);
				c.dropped = cDropped.load(std::memory_order_relaxed);
				c.repeatsSkipped = cRepeats.load(std::memory_order_relaxed);
				c.noClock = cNoClock.load(std::memory_order_relaxed);
				c.unusable = cUnusable.load(std::memory_order_relaxed);
				c.stale = cStale.load(std::memory_order_relaxed);
				c.discontinuities = cDiscontinuities.load(std::memory_order_relaxed);
				c.windowClears = cWindowClears.load(std::memory_order_relaxed);
				return c;
			}

			bool WaitIdle(int timeoutMs) const
			{
				const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
				for (;;)
				{
					bool empty = false;
					{
						std::lock_guard<std::mutex> lk(m);
						empty = jobs.empty() && !cfgDirty && !resetPending && !hotkeyPending;
					}
					if (paused.load(std::memory_order_acquire))
						empty = false;
					if (empty)
					{
						std::unique_lock<std::mutex> pl(procMutex, std::try_to_lock);
						if (pl.owns_lock())
							return true;
					}
					if (std::chrono::steady_clock::now() >= end)
						return false;
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			}
		};

		// ---- the public handle ---------------------------------------------------------------------------------------------
		ChainEstimator::ChainEstimator(const EstimatorSettings& settings, NowMsFn nowMs, std::shared_ptr<IEvidenceFs> fs, StampFn stamp)
			: st_(std::make_shared<State>(settings, std::move(nowMs), std::move(fs), std::move(stamp)))
		{
		}

		ChainEstimator::~ChainEstimator()
		{
			if (st_)
				st_->Stop();
		}

		void ChainEstimator::SetMode(EstimatorMode mode) { st_->SetMode(mode); }
		EstimatorMode ChainEstimator::Mode() const { return static_cast<EstimatorMode>(st_->modeA.load(std::memory_order_acquire)); }
		void ChainEstimator::SetOriginalPathway(bool original) { st_->SetOriginal(original); }
		bool ChainEstimator::Running() const { return st_->running.load(std::memory_order_acquire); }
		bool ChainEstimator::AttachTap(const void* owner) { return st_->AttachTap(owner); }
		void ChainEstimator::DetachTap() { st_->DetachTap(); }
		bool ChainEstimator::TapAttached() const { return st_->TapAttachedConst(); }
		void ChainEstimator::Stop() { st_->Stop(); }

		void ChainEstimator::NoteFrame(uint64_t seq, double seconds, int x, int y, int w, int h) { st_->NoteFrame(seq, seconds, x, y, w, h); }
		void ChainEstimator::NoteEvent(EstimatorEvent kind, uint64_t seq) { st_->NoteEvent(kind, seq); }
		void ChainEstimator::NoteRepeat(uint64_t seq) { st_->NoteRepeat(seq); }
		void ChainEstimator::NoteDiscontinuity(const char* cause) { st_->NoteDiscontinuity(cause); }
		void ChainEstimator::NoteNativeHeld(unsigned axisMask) { st_->NoteNativeHeld(axisMask); }
		void ChainEstimator::SetStreamKey(uint64_t key) { st_->SetStreamKey(key); }
		void ChainEstimator::SetClassical(bool valid, double ratioH, double ratioV) { st_->SetClassical(valid, ratioH, ratioV); }
		void ChainEstimator::SetFormatInfo(const std::string& text) { st_->SetFormatInfo(text); }

		void ChainEstimator::SetEvidence(const std::string& folder, EvidenceLevel level) { st_->SetEvidence(folder, level); }
		EvidenceLevel ChainEstimator::GetEvidenceLevel() const { return st_->GetEvidenceLevel(); }
		void ChainEstimator::RequestEvidenceDump(const std::string& why) { st_->RequestDump(why); }
		bool ChainEstimator::TakeNotice(std::string& text) { return st_->log.TakeNotice(text); }

		bool ChainEstimator::Wants() const { return st_->Wants(); }
		void ChainEstimator::Deliver(const VpSkeleton::SourceFrameView& view) { st_->Deliver(view); }

		const Geometry::HeldState& ChainEstimator::Held() const { return st_->held; }
		ChainStatusInput ChainEstimator::StatusInput() const { return st_->StatusInput(); }
		std::string ChainEstimator::StatusLine() const { return ChainStatusLine(st_->StatusInput()); }
		std::string ChainEstimator::DetailText() const { return ChainDetailText(st_->StatusInput()); }
		std::string ChainEstimator::EvidenceStatusText() const { return st_->log.StatusText(); }
		EstimatorCounters ChainEstimator::GetCounters() const { return st_->Counters(); }
		GovernorPlan ChainEstimator::GetGovernorPlan() const
		{
			std::lock_guard<std::mutex> lk(st_->statusMutex);
			return st_->statusPlan;
		}

		void ChainEstimator::SetEvaluationObserver(EvaluationObserver observer) { st_->observer = std::move(observer); }
		void ChainEstimator::SetSynchronous(bool on) { st_->sync.store(on, std::memory_order_release); }
		bool ChainEstimator::WaitIdle(int timeoutMs) const { return st_->WaitIdle(timeoutMs); }
		void ChainEstimator::PauseForTest(bool pause) { st_->Pause(pause); }
		void ChainEstimator::ForceLatchedPowerForTest(bool on) { st_->latchedPower.store(on, std::memory_order_release); }
		bool ChainEstimator::LatchedPowerForTest() const { return st_->latchedPower.load(std::memory_order_acquire); }
		void ChainEstimator::ForceNextDueForTest(double ms) { st_->nextDueMs.store(ms, std::memory_order_release); st_->intervalMs.store(ms - st_->lastStartMs.load(std::memory_order_acquire), std::memory_order_release); }
		double ChainEstimator::IntervalSec() const { return st_->intervalMs.load(std::memory_order_acquire) / 1000.0; }
		bool ChainEstimator::SubmitLines(const LineSnapshot& snapshot) { return st_->SubmitSnapshot(snapshot); }
	}
}
