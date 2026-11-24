// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// The chain estimator worker (see ChainEstimator.h). C++14. No Win32 here: the disk is behind the evidence log's IEvidenceFs.
#include "ChainEstimator.h"

#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/identity/SidDiagDump.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
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
			bool reqSafety = false;    // SID r2: requested safety rules / ABR trigger (taken at the next clean start)
			bool reqAbr = false;
			// Stage S2 opt-in requested settings (ALL default OFF; taken cleanly at FreshStart):
			JointLatchSettings reqJointLatch;
			StepMarkSettings reqStepMark;
			bool reqConvProposals = false;
			EdgeStatsSettings reqEdgeStats;
			bool reqEmpiricalFloor = false;
			// Stage S3 opt-in requested settings (ALL default OFF; taken cleanly at FreshStart):
			EmpiricalFloorMode reqEmpiricalFloorMode = EmpiricalFloorMode::Off;
			bool reqEmpiricalFloorModeExplicit = false;
			bool reqEightBitPromotion = false;
			bool reqUseDepthNullTables = false;
			DepthNullTableSet reqDepthNullTables = DepthNullTableSet::PlaceholderOnly();
			struct ClassicalFreshState
			{
				bool valid = false;
				double ratioH = 1.0, ratioV = 1.0;
				bool detailAbove = false;
				bool mixed = false;
				bool subtitle = false;
				bool inFade = false;
				uint64_t frameSeq = 0;
				int64_t timeUs = -1;
				double analysisIntervalSec = 0.0;
				std::function<bool(double, double)> detailAboveAtRatio;
				uint64_t updateGen = 0;
			};
			ClassicalFreshState classicalFresh;
			uint64_t classicalFreshGen = 0;
			struct AbrStepEventRec
			{
				uint64_t seq = 0;
				int64_t t0Us = 0;
				double ratioStep = 0.0;
			};
			std::vector<AbrStepEventRec> abrStepEvents;
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
			std::atomic<bool> safetyA{ false };   // SID r2: events MenuHold / AbrStep are taken only while the rules are requested
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
				cStale{ 0 }, cDiscontinuities{ 0 }, cWindowClears{ 0 }, cFadeShots{ 0 };
			std::atomic<uint64_t> cLatchEntries{ 0 }, cLatchMoves{ 0 }, cLatchReleases{ 0 }, cLatchRelBound{ 0 }, cLatchRelNative{ 0 }, cLatchRelMenu{ 0 },
				cLatchRelAbr{ 0 }, cLatchRelReset{ 0 }, cLatchHeldBack{ 0 }, cLatchNonConv{ 0 }, cLatchConfirms{ 0 }, cLatchRelHard{ 0 },
				cLatchDeferred{ 0 }, cLatchDeferCancelled{ 0 }, cLatchRelAtCut{ 0 }, cLatchRelCeiling{ 0 };
			// Stage S2 counters (zero while Stage S2 switches remain OFF):
			mutable std::mutex s2CounterMutex;
			JointLatchCounters s2JointCounters;
			StepMarkCounters s2StepCounters;
			std::atomic<uint64_t> cR1HarmonicSelections{ 0 }, cP3ConvProposals{ 0 }, cEmpiricalFloorEvals{ 0 }, cEmpiricalFloorFallbacks{ 0 };
			// Stage S3 counters (Rule 4 explicit fallback / placeholder visibility):
			std::atomic<uint64_t> cEmpiricalFloorAllAxisFallbacks{ 0 }, cEmpiricalFloorPartialAxisFallbacks{ 0 },
				cPlaceholderNullTableEvals{ 0 }, cDepthNullTableFallbacks{ 0 };
			// Pass-2 audit closure counters (M1, M3, M4, L2):
			std::atomic<uint64_t> cClassicalFreshStaleDrops{ 0 }, cP3ScreenOnlyCandidates{ 0 },
				cEdgeStatsNullTableAbstains{ 0 }, cAbrStepEventsEvicted{ 0 };
			// fx/dv-frame-quiet: each ceiling release is stamped with the newest source frame the estimator had seen, so the frame-bound
			// monitor counts only the releases up to the frame it is allowed to see (LatchCeilingReleasesThrough), not the live counter.
			std::atomic<uint64_t> newestSeqA{ 0 };
			mutable std::mutex ceilLogMutex;
			std::deque<std::pair<uint64_t, uint64_t>> ceilLog;   // (frame stamp, cumulative releases), oldest first, bounded
			uint64_t ceilLogTrimmed = 0;                         // the cumulative count of the entries trimmed off the front
			void LogCeilingRelease(uint64_t cumulative)
			{
				std::lock_guard<std::mutex> lk(ceilLogMutex);
				ceilLog.push_back(std::make_pair(newestSeqA.load(std::memory_order_acquire), cumulative));
				while (ceilLog.size() > 64) { ceilLogTrimmed = ceilLog.front().second; ceilLog.pop_front(); }
			}
			uint64_t CeilingReleasesThrough(uint64_t seq) const
			{
				std::lock_guard<std::mutex> lk(ceilLogMutex);
				uint64_t n = ceilLogTrimmed;
				for (const auto& e : ceilLog) { if (e.first <= seq) n = e.second; else break; }
				return n;
			}
			std::atomic<bool> latchDeferredA{ false };   // SID r2 N1: a starvation release waits for the next cut / black / fade (status)
			std::atomic<bool> safetyLiveA{ false };      // SID r2: the rules applied at the clean start (status)
			std::atomic<int> latchNxA{ 0 }, latchNyA{ 0 };   // SID r2: the latched size (0 = no latch; status)

			// ---- the worker's own state (procMutex: one processing thread at a time) ---------------------------------------
			mutable std::mutex procMutex;
			AxisReader reader[2];
			ChainEvidence evidence;
			PolyphaseProposer proposer[2];
			std::unique_ptr<DescaleVerifier> verifier;
			std::unique_ptr<DescaleVerifier> probeVerifier;
			// SID-OPT: hoisted per-axis run buffers & PowerProbe workspaces (under procMutex; retain capacity across evals, zero state leak).
			AxisRun evalRun[2];
			PowerProbe::Workspace probeWs[2];
			Governor gov;
			EvidenceLog log;
			Geometry::HeldState held;
			UpscaleLatch latch;                       // SID r2 R5 / R7 (inert unless safetyOn)
			bool safetyOn = false;                    // applied at the clean start
			// Stage S2 worker instances (ALL inert unless explicitly enabled):
			ChainJointLatch jointLatch;               // OD-1 / OD-2 two-witness latch
			ChainStepMark stepMark;                   // Fast ABR step sentinel
			bool jointLatchOn = false;
			bool stepMarkOn = false;
			bool convProposalsOn = false;
			JointLatchCounters jointLatchBase;
			StepMarkCounters stepMarkBase;
			uint64_t stepMarkEvalSeq = 0;
			bool stepMarkImmediateReleasePending = false;
			uint64_t lastStepMarkClassicalSeq = 0;
			uint64_t lastStepMarkClassicalGen = 0;
			Geometry::AxisGeom heldApplied[2];        // the applied geometry the latch carries (updated by every confident reading of the latched size)
			Geometry::AxisGeom lastFiltered[2];       // the last published applied geometry (for appliedChanged)
			LatchCounters latchBase;                  // counters of the latch already folded into the atomics
			std::string dumpPath;                     // R4: VP_SID_R4_DUMP_PATH (empty = off)
			std::unique_ptr<DescaleVerifier> dumpVerifier;
			uint64_t dumpRows = 0;
			// Dump v4 (SidDiagDump.h, VP_SID_DUMP_V4_DIR; decision neutral): files, the tag, its own verifier, this evaluation's latch output.
			Diag::Tag diagTag;
			Diag::File diagEvals, diagSizes;
			std::unique_ptr<DescaleVerifier> v4Verifier;
			LatchOutput diagLo;                       // the latch output of the newest evaluation (ApplyLatch; none while the rules are off)
			LatchEvent diagResetEvent = LatchEvent::None;   // ... or of the reset branch of Process
			std::atomic<int> diagCodedBits{ 0 }, diagOutputBits{ 0 };   // the caller's depth hint (0 = unknown)
			bool appliedRunning = false;
			uint64_t appliedEpoch = 0;
			bool haveArea = false;
			LineRect lastActive;
			uint32_t shotId = 0;
			// SID r2 N2 (F1): with the safety rules on, a black or fade evaluation without a cut also starts a new evidence shot, at most one
			// per minimum shot dwell after the previous shot start (a run of black frames never splits one shot into many capped shots).
			bool haveShotStart = false;
			int64_t shotStartUs = 0;
			bool shotStartedNow = false;   // this evaluation started a new evidence shot (a cut, or the F1 black / fade): the latch's boundary
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
				reqSafety = cfg.safetyRules;
				reqAbr = cfg.latch.abrStepTrigger;
				reqJointLatch = cfg.jointLatch;
				reqStepMark = cfg.stepMark;
				reqConvProposals = cfg.conventionDrivenProposals;
				reqEdgeStats = cfg.evidence.edgeStats;
				reqEmpiricalFloorMode = cfg.useEmpiricalFloor ? EmpiricalFloorMode::Live : cfg.empiricalFloorMode;
				reqEmpiricalFloor = (reqEmpiricalFloorMode == EmpiricalFloorMode::Live);
				reqEmpiricalFloorModeExplicit = !cfg.useEmpiricalFloor && (cfg.empiricalFloorMode != EmpiricalFloorMode::Off);
				reqEightBitPromotion = cfg.eightBitPromotion;
				reqUseDepthNullTables = cfg.useDepthNullTables;
				reqDepthNullTables = cfg.depthNullTables;
				safetyA.store(reqSafety || reqJointLatch.enabled, std::memory_order_release);
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
				// The probe's operators are 1.64x longer but only a handful (one kernel, five sizes per axis): an eighth of the byte budget holds them.
				VerifierSettings pvs = vs;
				pvs.cacheBytes = vs.cacheBytes / 8;
				probeVerifier.reset(new DescaleVerifier(pvs));
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

			void SetSafety(bool on)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					if (reqSafety == on)
						return;
					reqSafety = on;
					safetyA.store(reqSafety || reqJointLatch.enabled, std::memory_order_release);
					cfgDirty = true;
					++runEpoch;   // the rules change the reading: a clean start
				}
				Wake();
			}

			void SetAbr(bool on)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					if (reqAbr == on)
						return;
					reqAbr = on;
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			void SetJointLatch(const JointLatchSettings& s)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqJointLatch = s;
					safetyA.store(reqSafety || reqJointLatch.enabled, std::memory_order_release);
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			JointLatchSettings GetJointLatch() const
			{
				std::lock_guard<std::mutex> lk(m);
				return reqJointLatch;
			}

			void SetStepMark(const StepMarkSettings& s)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqStepMark = s;
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			StepMarkSettings GetStepMark() const
			{
				std::lock_guard<std::mutex> lk(m);
				return reqStepMark;
			}

			void SetConvProposals(bool on)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					if (reqConvProposals == on)
						return;
					reqConvProposals = on;
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			bool GetConvProposals() const
			{
				std::lock_guard<std::mutex> lk(m);
				return reqConvProposals;
			}

			void SetEdgeStats(const EdgeStatsSettings& s, bool empiricalFloor)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqEdgeStats = s;
					if (empiricalFloor)
					{
						reqEmpiricalFloorMode = EmpiricalFloorMode::Live;
						reqEmpiricalFloor = true;
						reqEmpiricalFloorModeExplicit = false;
					}
					else if (!reqEmpiricalFloorModeExplicit)
					{
						reqEmpiricalFloorMode = EmpiricalFloorMode::Off;
						reqEmpiricalFloor = false;
					}
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			EdgeStatsSettings GetEdgeStats() const
			{
				std::lock_guard<std::mutex> lk(m);
				return reqEdgeStats;
			}

			void SetEmpiricalFloorMode(EmpiricalFloorMode mode)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqEmpiricalFloorMode = mode;
					reqEmpiricalFloor = (mode == EmpiricalFloorMode::Live);
					reqEmpiricalFloorModeExplicit = true;
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			void SetEightBitPromotion(bool on)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqEightBitPromotion = on;
					cfgDirty = true;
					++runEpoch;
				}
				Wake();
			}

			void SetDepthNullTables(const DepthNullTableSet& tables, bool enable)
			{
				{
					std::lock_guard<std::mutex> lk(m);
					reqDepthNullTables = tables;
					reqUseDepthNullTables = enable;
					cfgDirty = true;
					++runEpoch;
				}
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
				newestSeqA.store(seq, std::memory_order_release);
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
				if ((kind == EstimatorEvent::MenuHold || kind == EstimatorEvent::AbrStep) && !safetyA.load(std::memory_order_acquire))
					return;   // SID r2 events: inert while the rules are off (byte identical)
				EventRec e;
				e.seq = seq;
				e.kind = static_cast<int>(kind);
				{
					std::lock_guard<std::mutex> lk(m);
					if (kind == EstimatorEvent::MenuHold)
					{
						// r2 fix M2: the monitor reports a counted menu / overlay episode on every frame; at most ONE MenuHold waits at a time
						// (the next evaluation consumes it), so a menu never evicts pending Cut / Blank / Fade / ClearWindow events from the
						// capped queue. Each evaluation during the episode still sees the menu (R7: nothing enters during one).
						for (size_t i = 0; i < events.size(); ++i)
							if (events[i].kind == static_cast<int>(EstimatorEvent::MenuHold))
								return;
					}
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

			void SetClassicalFresh(const ClassicalFreshInput& in)
			{
				std::lock_guard<std::mutex> lk(m);
				classicalFresh.valid = in.valid;
				classicalFresh.ratioH = in.ratioH;
				classicalFresh.ratioV = in.ratioV;
				classicalFresh.detailAbove = in.detailAbove;
				classicalFresh.mixed = in.mixed;
				classicalFresh.subtitle = in.subtitle;
				classicalFresh.inFade = in.inFade;
				classicalFresh.frameSeq = in.frameSeq;
				classicalFresh.timeUs = in.timeUs;
				classicalFresh.analysisIntervalSec = in.analysisIntervalSec;
				classicalFresh.detailAboveAtRatio = in.detailAboveAtRatio;
				classicalFresh.updateGen = ++classicalFreshGen;
			}

			void NoteAbrStepEvent(int64_t t0Us, double ratioStep, uint64_t seq)
			{
				if (!running.load(std::memory_order_acquire))
					return;
				bool pullForward = false;
				{
					std::lock_guard<std::mutex> lk(m);
					if (!reqStepMark.enabled)
						return;
					AbrStepEventRec rec;
					rec.seq = seq;
					rec.t0Us = t0Us;
					rec.ratioStep = ratioStep;
					abrStepEvents.push_back(rec);
					while (static_cast<int>(abrStepEvents.size()) > cfg.eventCap)
					{
						abrStepEvents.erase(abrStepEvents.begin());
						cAbrStepEventsEvicted.fetch_add(1, std::memory_order_relaxed);
					}
					pullForward = std::isfinite(ratioStep) && std::fabs(ratioStep) >= reqStepMark.minRatioStep;
				}
				if (pullForward && latchedPower.load(std::memory_order_acquire))
				{
					const double now = nowMs();
					if (nextDueMs.load(std::memory_order_acquire) > now)
						nextDueMs.store(now, std::memory_order_release);
				}
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
				snap->rasterW = static_cast<int32_t>(v.width);    // SID r2 L3: the delivered raster (R2 / R3 ratios)
				snap->rasterH = static_cast<int32_t>(v.height);
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
							else if (events[i].kind == static_cast<int>(EstimatorEvent::MenuHold))
								flags |= kEvMenuHold;
							else if (events[i].kind == static_cast<int>(EstimatorEvent::AbrStep))
								flags |= kEvAbrStep;
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
					// SID r2 (M2 test path): a pending MenuHold is the monitor's episode state like ClearWindow (no picture event), consumed the same
					// way (only queued while the safety rules are on, so off stays byte identical).
					for (size_t i = 0; i < events.size();)
					{
						const bool clear = events[i].kind == static_cast<int>(EstimatorEvent::ClearWindow);
						const bool menu = events[i].kind == static_cast<int>(EstimatorEvent::MenuHold);
						if ((clear || menu) && events[i].seq <= snap.seq)
						{
							copy->flags |= clear ? kEvClearWindow : kEvMenuHold;
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
					evalRun[a] = AxisRun();
					probeWs[a].Reset();
				}
				{
					bool safe = false, abr = false, convP = false;
					bool eightBitP = false, useDepthN = false;
					EmpiricalFloorMode empMode = EmpiricalFloorMode::Off;
					JointLatchSettings jCfg;
					StepMarkSettings smCfg;
					EdgeStatsSettings esCfg;
					DepthNullTableSet depthN = DepthNullTableSet::PlaceholderOnly();
					{
						std::lock_guard<std::mutex> lk(m);
						safe = reqSafety;
						abr = reqAbr;
						jCfg = reqJointLatch;
						smCfg = reqStepMark;
						convP = reqConvProposals;
						esCfg = reqEdgeStats;
						empMode = reqEmpiricalFloorMode;
						eightBitP = reqEightBitPromotion;
						useDepthN = reqUseDepthNullTables;
						depthN = reqDepthNullTables;
						abrStepEvents.clear();
					}
					if (jointLatchOn)
					{
						const JointLatchOutput jlo = jointLatch.Reset();
						FoldJointLatchCounters();
						NoteJointLatchEvent(jlo, lastTimeUs);
					}
					else if (safetyOn)
					{
						// M4: a latch held across a clean start is released visibly (counted, noticed) before the layer is rebuilt.
						const LatchOutput lo = latch.Reset();
						FoldLatchCounters();
						NoteLatchEvent(lo, lastTimeUs);
					}
					if (stepMarkOn)
					{
						stepMark.Reset(StepMarkReason::ResetDiscontinuity);
						FoldStepMarkCounters();
					}
					safetyOn = safe;
					jointLatchOn = jCfg.enabled;
					stepMarkOn = smCfg.enabled;
					convProposalsOn = convP;
					const bool rulesActive = safe || jointLatchOn;
					safetyLiveA.store(rulesActive, std::memory_order_release);
					latchNxA.store(0, std::memory_order_release);
					latchDeferredA.store(false, std::memory_order_release);
					latchNyA.store(0, std::memory_order_release);
					cfg.evidence.conventionOnly = rulesActive;   // R2
					cfg.evidence.pairPrior = rulesActive;        // R3
					if (eightBitP)
						esCfg.enabled = true;                    // Stage S3 eightBitPromotion enables edgeStats
					cfg.evidence.edgeStats = esCfg;              // Stage S2/S3 EXP-8BIT (default OFF)
					cfg.latch.enabled = safe && !jointLatchOn;   // R5 / R7 legacy latch when jointLatch is OFF
					cfg.latch.abrStepTrigger = abr;
					cfg.jointLatch = jCfg;
					cfg.stepMark = smCfg;
					cfg.conventionDrivenProposals = convP;
					cfg.empiricalFloorMode = empMode;
					cfg.useEmpiricalFloor = (empMode == EmpiricalFloorMode::Live);
					cfg.eightBitPromotion = eightBitP;
					cfg.useDepthNullTables = useDepthN;
					cfg.depthNullTables = depthN;
					latch = UpscaleLatch(cfg.latch);
					latchBase = LatchCounters();
					jointLatch = ChainJointLatch(jCfg);
					jointLatchBase = JointLatchCounters();
					stepMark = ChainStepMark(smCfg);
					stepMarkBase = StepMarkCounters();
					stepMarkEvalSeq = 0;
					stepMarkImmediateReleasePending = false;
					lastStepMarkClassicalSeq = 0;
					lastStepMarkClassicalGen = 0;
					for (int a = 0; a < 2; ++a)
					{
						heldApplied[a] = Geometry::AxisGeom();
						lastFiltered[a] = Geometry::AxisGeom();
					}
					ConfigureR4Dump();
				}
				evidence = ChainEvidence(cfg.evidence);
				verifier->ClearCache();
				probeVerifier->ClearCache();
				gov.Reset();
				shotId = 0;
				haveShotStart = false;
				shotStartUs = 0;
				shotStartedNow = false;
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
					evalRun[a].have = false;
					evalRun[a].pr = ProbeResult();
					evalRun[a].up = EvidenceUpdate();
					evalRun[a].vr = VerifyResult();
					probeWs[a].Reset();
				}
				evidence.Reset();
				if (stepMarkOn)
				{
					stepMark.Reset(StepMarkReason::ResetDiscontinuity);
					FoldStepMarkCounters();
					stepMarkImmediateReleasePending = false;
				}
				lastStepMarkClassicalSeq = 0;
				lastStepMarkClassicalGen = 0;
				if (jointLatchOn)
				{
					const JointLatchOutput jlo = jointLatch.Reset();
					FoldJointLatchCounters();
					NoteJointLatchEvent(jlo, lastTimeUs);
					latchNxA.store(0, std::memory_order_release);
					latchDeferredA.store(false, std::memory_order_release);
					latchNyA.store(0, std::memory_order_release);
					for (int a = 0; a < 2; ++a)
					{
						heldApplied[a] = Geometry::AxisGeom();
						lastFiltered[a] = Geometry::AxisGeom();
					}
				}
				else if (safetyOn)
				{
					const LatchOutput lo = latch.Reset();   // M4: counted AND noticed
					FoldLatchCounters();
					NoteLatchEvent(lo, lastTimeUs);
					latchNxA.store(0, std::memory_order_release);
					latchDeferredA.store(false, std::memory_order_release);
					latchNyA.store(0, std::memory_order_release);
					for (int a = 0; a < 2; ++a)
					{
						heldApplied[a] = Geometry::AxisGeom();
						lastFiltered[a] = Geometry::AxisGeom();
					}
				}
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
			// ---- SID r2: R4 enabler (opt-in diagnostic dump, no decision change) ------------------------------------------
			// VP_SID_R4_DUMP_PATH = an absolute scratch file: every Process appends, per axis, the exact-stage statistic of EVERY conventional
			// size (scaling.geometry SourceGeometrySet.h) on all the sampled lines, by a verifier of its own (nothing the decision reads is
			// touched). Columns: seq,timeUs,axis,N,n,valid,stat,kernel,lines. Cleared by unsetting the variable.
			void ConfigureR4Dump()
			{
				dumpPath.clear();
				dumpVerifier.reset();
				dumpRows = 0;
#ifdef _MSC_VER
				char* v = nullptr;
				size_t len = 0;
				if (_dupenv_s(&v, &len, "VP_SID_R4_DUMP_PATH") == 0 && v != nullptr)
				{
					dumpPath = v;
					std::free(v);
				}
#else
				const char* v = std::getenv("VP_SID_R4_DUMP_PATH");
				if (v != nullptr)
					dumpPath = v;
#endif
				if (dumpPath.empty())
					return;
				VerifierSettings vs = cfg.verifier;
				vs.nowMs = &State::VerifierClock;
				vs.clockCtx = this;
				vs.cacheBytes = vs.cacheBytes / 8;
				dumpVerifier.reset(new DescaleVerifier(vs));
				log.PostNotice("Source identity: R4 diagnostic dump on (" + dumpPath + ")");
			}

			void DumpR4(const AxisRun run[2], uint64_t seq, int64_t timeUs, int rasterW, int rasterH)
			{
				if (!dumpVerifier)
					return;
				std::ofstream f(dumpPath.c_str(), std::ios::app);
				if (!f)
					return;
				if (dumpRows == 0)
					f << "seq,timeUs,axis,N,n,valid,stat,kernel,lines\n";
				std::size_t count = 0;
				const Scaling::GeometrySet::Geometry* geoms = Scaling::GeometrySet::Geometries(count);
				for (int a = 0; a < 2; ++a)
				{
					if (!run[a].have)
						continue;
					const int N = run[a].ls.axisLen;
					int cands[32];
					int cc = 0;
					for (std::size_t i = 0; i < count && cc < 32; ++i)
					{
						double rx[2], ry[2];   // L3: from the delivered raster (every listed size, parked ones too: a diagnostic; both forms)
						const int forms = Scaling::GeometrySet::RatioForms(geoms[i], rasterW, rasterH, rx, ry);
						for (int f = 0; f < forms && cc < 32; ++f)
						{
							const double r = a == 0 ? rx[f] : ry[f];
							const int n = RoundToInt(static_cast<double>(N) * r);
							if (n < 16 || n >= N)
								continue;
							bool dup = false;
							for (int k = 0; k < cc; ++k)
								dup = dup || cands[k] == n;
							if (!dup)
								cands[cc++] = n;
						}
					}
					if (cc == 0)
						continue;
					VerifyOptions opt;
					opt.exactOnly = true;
					VerifyResult vr;
					dumpVerifier->Evaluate(run[a].ls, cands, cc, opt, vr);
					for (size_t i = 0; i < vr.candidates.size(); ++i)
					{
						const CandidateResult& cr = vr.candidates[i];
						f << seq << ',' << timeUs << ',' << a << ',' << N << ',' << cr.nProposed << ',' << (cr.valid ? 1 : 0) << ',' << cr.stat << ','
							<< cr.kernel << ',' << cr.lineCount << '\n';
						++dumpRows;
					}
				}
			}

			struct DiagEvalExtra
			{
				bool stepMarkConfirmed = false;
				bool stepMarkCancelled = false;
				bool tentativeBucketVoided = false;
				EmpiricalFloorMode empFloorMode = EmpiricalFloorMode::Off;
				double floorTheoretical = 0.0;
				double empFloor[2] = { 0.0, 0.0 };
				bool empValid[2] = { false, false };
				bool empFallback = false;
				bool nullCalibrated = false;
				BitDepthClass nullDepth = BitDepthClass::Unknown;
			};

			// ---- Dump v4 (doc 100 s4 P0; contract identity/README.md "Dump v4"): one b_evals row per evaluation, one b_sizes row per
			// evaluation x axis x conventional size of the published set (scaling.geometry SourceGeometrySet.h; parked sizes too). Reads the
			// decision state through const accessors only; the exact statistic of every size comes from a verifier of its own.
			void DumpV4(const AxisRun run[2], const ReadingOutput out[2], uint32_t flags, bool windowCleared, uint64_t seq, int64_t timeUs,
				int rasterW, int rasterH, int step, double floorCodes, bool classicalOk, const double classical[2], double costMs,
				const DiagEvalExtra& extra)
			{
				std::ofstream& f = diagEvals.Out();
				const int coded = diagCodedBits.load(std::memory_order_acquire), outBits = diagOutputBits.load(std::memory_order_acquire);
				const LatchEvent ev = diagLo.event != LatchEvent::None ? diagLo.event : diagResetEvent;
				const std::string t = Diag::Num(static_cast<double>(timeUs) / 1e6);
				f << seq << ',' << t << ',' << shotId << ',' << flags << ',' << ((flags & kEvCut) ? 1 : 0) << ',' << ((flags & kEvBlank) ? 1 : 0) << ','
					<< ((flags & kEvFade) ? 1 : 0) << ',' << ((flags & kEvClearWindow) ? 1 : 0) << ',' << ((flags & kEvMenuHold) ? 1 : 0) << ','
					<< ((flags & kEvAbrStep) ? 1 : 0) << ',' << ((flags & kEvAreaChanged) ? 1 : 0) << ',' << (windowCleared ? 1 : 0) << ','
					<< step << ',' << Diag::Num(floorCodes) << ',' << coded << ',' << outBits << ',' << (classicalOk ? 1 : 0) << ','
					<< Diag::Num(classical[0]) << ',' << Diag::Num(classical[1]) << ',' << (safetyOn ? 1 : 0);
				for (int a = 0; a < 2; ++a) f << ',' << static_cast<int>(out[a].measured.state);
				for (int a = 0; a < 2; ++a) f << ',' << out[a].measured.n;
				for (int a = 0; a < 2; ++a) f << ',' << static_cast<int>(out[a].applied.state);
				for (int a = 0; a < 2; ++a) f << ',' << out[a].applied.n;
				for (int a = 0; a < 2; ++a) f << ',' << (run[a].have ? run[a].ls.axisLen : 0);
				for (int a = 0; a < 2; ++a) f << ',' << Diag::Num(out[a].measured.llr);
				for (int a = 0; a < 2; ++a) f << ',' << ((run[a].have && run[a].up.informative) ? 1 : 0);
				for (int a = 0; a < 2; ++a) f << ',' << ((run[a].have && run[a].up.hold) ? 1 : 0);
				for (int a = 0; a < 2; ++a) f << ',' << ((run[a].have && run[a].pr.seen) ? 1 : 0);
				f << ',' << (latch.Latched() ? 1 : 0) << ',' << latch.LatchedNx() << ',' << latch.LatchedNy() << ',' << static_cast<int>(ev) << ','
					<< (diagLo.confirmed ? 1 : 0) << ',' << (diagLo.deferredNow ? 1 : 0) << ',' << (diagLo.deferCancelled ? 1 : 0) << ','
					<< (diagLo.heldBack ? 1 : 0) << ',' << Diag::Num(latch.ReleaseClockSec()) << ',' << (latch.Deferred() ? 1 : 0) << ','
					<< (latch.Dwelling() ? 1 : 0) << ',' << static_cast<int>(latch.DeferCause()) << ','
					<< Diag::Num(static_cast<double>(latch.DeferSinceUs()) / 1e6) << ','
					<< Diag::Num(static_cast<double>(latch.LastConfirmUs()) / 1e6) << ','
					<< Diag::Num(static_cast<double>(latch.NewestCreditUs(0)) / 1e6) << ','
					<< Diag::Num(static_cast<double>(latch.NewestCreditUs(1)) / 1e6) << ',' << Diag::Num(latch.AccumulatedCredit(0)) << ','
					<< Diag::Num(latch.AccumulatedCredit(1)) << ',' << Diag::Num(latch.Bar(timeUs, thr.c1)) << ',' << Diag::Num(thr.c1) << ','
					<< Diag::Num(costMs)
					<< ',' << (stepMarkOn ? static_cast<int>(stepMark.State()) : 0)
					<< ',' << Diag::Num((stepMarkOn && stepMark.MarkTimeUs() >= 0) ? (static_cast<double>(stepMark.MarkTimeUs()) / 1e6) : -1.0)
					<< ',' << ((stepMarkOn && stepMark.BlocksEntryOrMove()) ? 1 : 0)
					<< ',' << (extra.stepMarkConfirmed ? 1 : 0)
					<< ',' << (extra.stepMarkCancelled ? 1 : 0)
					<< ',' << ((stepMarkOn && stepMark.IsPending()) ? 1 : 0)
					<< ',' << (stepMarkOn ? static_cast<int>(stepMark.PendingBucket().size()) : 0)
					<< ',' << (extra.tentativeBucketVoided ? 1 : 0)
					<< ',' << static_cast<int>(extra.empFloorMode)
					<< ',' << Diag::Num(extra.floorTheoretical)
					<< ',' << Diag::Num(extra.empFloor[0])
					<< ',' << Diag::Num(extra.empFloor[1])
					<< ',' << (extra.empValid[0] ? 1 : 0)
					<< ',' << (extra.empValid[1] ? 1 : 0)
					<< ',' << (extra.empFallback ? 1 : 0)
					<< ',' << (extra.nullCalibrated ? 1 : 0)
					<< ',' << BitDepthClassName(extra.nullDepth) << '\n';
				diagEvals.EndRow();
				if (!diagSizes.Ready(diagTag, "b_sizes", Diag::SizesHeaderV4()))
					return;
				if (!v4Verifier)
				{
					VerifierSettings vs = cfg.verifier;
					vs.nowMs = &State::VerifierClock;
					vs.clockCtx = this;
					vs.cacheBytes = vs.cacheBytes / 8;
					v4Verifier.reset(new DescaleVerifier(vs));
				}
				std::ofstream& g = diagSizes.Out();
				std::size_t count = 0;
				const Scaling::GeometrySet::Geometry* geoms = Scaling::GeometrySet::Geometries(count);
				for (int a = 0; a < 2; ++a)
				{
					if (!run[a].have)
						continue;
					const int N = run[a].ls.axisLen;
					int cands[32];
					int gi[32];
					int cc = 0;
					for (std::size_t i = 0; i < count && cc < 32; ++i)
					{
						double rx = 0.0, ry = 0.0;
						if (!Scaling::GeometrySet::RatiosFor(geoms[i], rasterW, rasterH, rx, ry))
							continue;
						const int n = RoundToInt(static_cast<double>(N) * (a == 0 ? rx : ry));
						if (n < 16 || n >= N)
							continue;
						bool dup = false;
						for (int k = 0; k < cc; ++k)
							dup = dup || cands[k] == n;
						if (dup)
							continue;
						gi[cc] = static_cast<int>(i);
						cands[cc++] = n;
					}
					if (cc == 0)
						continue;
					VerifyOptions opt;
					opt.exactOnly = true;
					VerifyResult vr;
					v4Verifier->Evaluate(run[a].ls, cands, cc, opt, vr);
					for (int k = 0; k < cc; ++k)
					{
						const CandidateResult* cr = nullptr;
						for (size_t i = 0; i < vr.candidates.size() && cr == nullptr; ++i)
							if (vr.candidates[i].nProposed == cands[k])
								cr = &vr.candidates[i];
						int64_t newest = -1;
						double raw = 0.0;
						const double credit = evidence.CreditAtN(a, cands[k], &newest, &raw);
						const double llr = evidence.LlrAtN(a, cands[k]);
						const ChainEvidence::ShotDiag sd = evidence.ShotDiagAtN(a, cands[k]);
						const Scaling::GeometrySet::Geometry& geo = geoms[gi[k]];
						g << seq << ',' << t << ',' << shotId << ',' << (a == 0 ? 'x' : 'y') << ',' << N << ',' << cands[k] << ",\"" << geo.name << "\","
							<< (geo.apply ? 0 : 1) << ',' << ((cr && cr->valid && cr->exactDone) ? 1 : 0) << ',' << (cr ? Diag::Num(cr->stat) : std::string()) << ','
							<< (cr ? cr->kernel : -1) << ',' << (cr ? cr->lineCount : 0) << ',' << Diag::Num(credit) << ',' << Diag::Num(raw) << ','
							<< Diag::Num(static_cast<double>(newest) / 1e6) << ',' << (llr <= -1e8 ? std::string() : Diag::Num(llr)) << ','
							<< (sd.haveSlot ? Diag::Num(sd.open) : std::string()) << ',' << sd.openShotId << ','
							<< Diag::Num(static_cast<double>(sd.openNewestUs) / 1e6) << ',' << sd.closedShots << ',' << Diag::Num(sd.closedSum) << ','
							<< Diag::Num(sd.pending) << ',' << sd.shotKey << ',' << Diag::Num(static_cast<double>(sd.shotStartUs) / 1e6) << ','
							<< (sd.shotMet ? 1 : 0) << '\n';
						diagSizes.EndRow();
					}
				}
			}

			// ---- SID r2: the asymmetric latch over the readers outputs (R5, R7) ----------------------------------------------
			static Geometry::AxisGeom UnsureLike(const Geometry::AxisGeom& g, uint32_t bits)
			{
				Geometry::AxisGeom u;
				u.state = Geometry::LatchState::Unknown;
				u.N = g.N;
				u.reasonBits = g.reasonBits | bits;
				return u;
			}

			void FoldLatchCounters()
			{
				const LatchCounters& c = latch.Counters();
				cLatchEntries.fetch_add(c.entries - latchBase.entries, std::memory_order_relaxed);
				cLatchMoves.fetch_add(c.moves - latchBase.moves, std::memory_order_relaxed);
				cLatchReleases.fetch_add(c.releases - latchBase.releases, std::memory_order_relaxed);
				cLatchRelBound.fetch_add(c.releasesBound - latchBase.releasesBound, std::memory_order_relaxed);
				cLatchRelNative.fetch_add(c.releasesNative - latchBase.releasesNative, std::memory_order_relaxed);
				cLatchRelMenu.fetch_add(c.releasesMenu - latchBase.releasesMenu, std::memory_order_relaxed);
				cLatchRelAbr.fetch_add(c.releasesAbr - latchBase.releasesAbr, std::memory_order_relaxed);
				cLatchRelReset.fetch_add(c.releasesReset - latchBase.releasesReset, std::memory_order_relaxed);
				cLatchHeldBack.fetch_add(c.heldBack - latchBase.heldBack, std::memory_order_relaxed);
				cLatchNonConv.fetch_add(c.nonConventional - latchBase.nonConventional, std::memory_order_relaxed);
				cLatchConfirms.fetch_add(c.confirms - latchBase.confirms, std::memory_order_relaxed);
				cLatchRelHard.fetch_add(c.releasesHardBound - latchBase.releasesHardBound, std::memory_order_relaxed);
				cLatchDeferred.fetch_add(c.deferred - latchBase.deferred, std::memory_order_relaxed);
				cLatchDeferCancelled.fetch_add(c.deferCancelled - latchBase.deferCancelled, std::memory_order_relaxed);
				cLatchRelAtCut.fetch_add(c.releasesAtCut - latchBase.releasesAtCut, std::memory_order_relaxed);
				const uint64_t ceilingNow = c.releasesCeiling - latchBase.releasesCeiling;
				cLatchRelCeiling.fetch_add(ceilingNow, std::memory_order_relaxed);   // the monitor's crossfade cue
				if (ceilingNow != 0) LogCeilingRelease(cLatchRelCeiling.load(std::memory_order_relaxed));
				latchDeferredA.store(latch.Deferred(), std::memory_order_release);
				latchBase = c;
			}

			void NoteLatchEvent(const LatchOutput& lo, int64_t timeUs)
			{
				// Every release is visible: a one-time notice (the counters are in the status).
				if (LatchEventIsRelease(lo.event))
					log.PostNotice(std::string("Source identity: upscale latch ") + LatchEventText(lo.event) + " at " + std::to_string(timeUs / 1000000) + " s");
				else if (lo.deferredNow)
					log.PostNotice("Source identity: upscale latch not re-confirmed in time at " + std::to_string(timeUs / 1000000) +
						" s; its release waits for the next cut, black or fade (the new shot may re-confirm it)");
				else if (lo.deferCancelled)
					log.PostNotice("Source identity: upscale latch re-confirmed at " + std::to_string(timeUs / 1000000) + " s; the deferred release is cancelled");
			}

			void FoldJointLatchCounters()
			{
				const JointLatchCounters& c = jointLatch.Counters();
				const uint64_t ceilingNow = c.releasesCeiling - jointLatchBase.releasesCeiling;
				cLatchRelCeiling.fetch_add(ceilingNow, std::memory_order_relaxed);
				if (ceilingNow != 0)
					LogCeilingRelease(cLatchRelCeiling.load(std::memory_order_relaxed));
				latchDeferredA.store(jointLatch.Deferred(), std::memory_order_release);
				{
					std::lock_guard<std::mutex> lk(s2CounterMutex);
					s2JointCounters.entriesChainAlone += c.entriesChainAlone - jointLatchBase.entriesChainAlone;
					s2JointCounters.entriesJoint += c.entriesJoint - jointLatchBase.entriesJoint;
					s2JointCounters.moves += c.moves - jointLatchBase.moves;
					s2JointCounters.confirmsChain += c.confirmsChain - jointLatchBase.confirmsChain;
					s2JointCounters.g2JointRenewals += c.g2JointRenewals - jointLatchBase.g2JointRenewals;
					s2JointCounters.classicalHoldPauses += c.classicalHoldPauses - jointLatchBase.classicalHoldPauses;
					s2JointCounters.classicalHoldDeniedByB += c.classicalHoldDeniedByB - jointLatchBase.classicalHoldDeniedByB;
					s2JointCounters.wouldBeClassicalOnly += c.wouldBeClassicalOnly - jointLatchBase.wouldBeClassicalOnly;
					s2JointCounters.classicalVoided += c.classicalVoided - jointLatchBase.classicalVoided;
					s2JointCounters.blockedByStepMark += c.blockedByStepMark - jointLatchBase.blockedByStepMark;
					s2JointCounters.heldBack += c.heldBack - jointLatchBase.heldBack;
					s2JointCounters.nonConventional += c.nonConventional - jointLatchBase.nonConventional;
					s2JointCounters.detailAboveTransientIgnored += c.detailAboveTransientIgnored - jointLatchBase.detailAboveTransientIgnored;
					s2JointCounters.releases += c.releases - jointLatchBase.releases;
					s2JointCounters.releasesStarvation += c.releasesStarvation - jointLatchBase.releasesStarvation;
					s2JointCounters.releasesHardBound += c.releasesHardBound - jointLatchBase.releasesHardBound;
					s2JointCounters.releasesCeiling += ceilingNow;
					s2JointCounters.releasesAtCut += c.releasesAtCut - jointLatchBase.releasesAtCut;
					s2JointCounters.releasesNative += c.releasesNative - jointLatchBase.releasesNative;
					s2JointCounters.releasesDetailAbove += c.releasesDetailAbove - jointLatchBase.releasesDetailAbove;
					s2JointCounters.releasesMenu += c.releasesMenu - jointLatchBase.releasesMenu;
					s2JointCounters.releasesAbr += c.releasesAbr - jointLatchBase.releasesAbr;
					s2JointCounters.releasesReset += c.releasesReset - jointLatchBase.releasesReset;
					s2JointCounters.deferred += c.deferred - jointLatchBase.deferred;
					s2JointCounters.deferCancelled += c.deferCancelled - jointLatchBase.deferCancelled;
					s2JointCounters.invalidInputs += c.invalidInputs - jointLatchBase.invalidInputs;
				}
				jointLatchBase = c;
			}

			void FoldStepMarkCounters()
			{
				const StepMarkCounters& c = stepMark.Counters();
				{
					std::lock_guard<std::mutex> lk(s2CounterMutex);
					s2StepCounters.marksRaised += c.marksRaised - stepMarkBase.marksRaised;
					s2StepCounters.ignoredBelowRatioStep += c.ignoredBelowRatioStep - stepMarkBase.ignoredBelowRatioStep;
					s2StepCounters.coalescedWhilePending += c.coalescedWhilePending - stepMarkBase.coalescedWhilePending;
					s2StepCounters.rejectedInvalidInput += c.rejectedInvalidInput - stepMarkBase.rejectedInvalidInput;
					s2StepCounters.confirmedByChain += c.confirmedByChain - stepMarkBase.confirmedByChain;
					s2StepCounters.confirmedByClassical += c.confirmedByClassical - stepMarkBase.confirmedByClassical;
					s2StepCounters.immediateReleases += c.immediateReleases - stepMarkBase.immediateReleases;
					s2StepCounters.challengesOpened += c.challengesOpened - stepMarkBase.challengesOpened;
					s2StepCounters.challengesCleared += c.challengesCleared - stepMarkBase.challengesCleared;
					s2StepCounters.challengesResumedAfterUnconfirmed += c.challengesResumedAfterUnconfirmed - stepMarkBase.challengesResumedAfterUnconfirmed;
					s2StepCounters.challengeReleases += c.challengeReleases - stepMarkBase.challengeReleases;
					s2StepCounters.unconfirmedTimeouts += c.unconfirmedTimeouts - stepMarkBase.unconfirmedTimeouts;
					s2StepCounters.bucketOverflowDrops += c.bucketOverflowDrops - stepMarkBase.bucketOverflowDrops;
					s2StepCounters.resetsOnCut += c.resetsOnCut - stepMarkBase.resetsOnCut;
					s2StepCounters.resetsDiscontinuity += c.resetsDiscontinuity - stepMarkBase.resetsDiscontinuity;
					s2StepCounters.resetsBackwardTime += c.resetsBackwardTime - stepMarkBase.resetsBackwardTime;
				}
				stepMarkBase = c;
			}

			void NoteJointLatchEvent(const JointLatchOutput& jlo, int64_t timeUs)
			{
				if (JointLatchEventIsRelease(jlo.event))
					log.PostNotice(std::string("Source identity: joint latch ") + JointLatchReasonText(jlo.reason) + " at " + std::to_string(timeUs / 1000000) + " s");
				else if (jlo.deferredNow)
					log.PostNotice(std::string("Source identity: joint latch ") + JointLatchReasonText(jlo.reason) + " at " + std::to_string(timeUs / 1000000) + " s");
				else if (jlo.deferCancelled)
					log.PostNotice(std::string("Source identity: joint latch ") + JointLatchReasonText(jlo.reason) + " at " + std::to_string(timeUs / 1000000) + " s");
				else if (jlo.classicalOnlyBlocked)
					log.PostNotice(std::string("Source identity: ") + JointLatchReasonText(jlo.reason) + " at " + std::to_string(timeUs / 1000000) + " s");
			}

			// Stage S2: R1 fundamental-harmonic order selection (SelectFundamentalHarmonic, ChainJointLatch.h) +
			// two-witness ChainJointLatch (Owner-approved OD-1 & OD-2). Replaces legacy TODO(SID r2 R1) when jointLatchOn.
			void ApplyJointLatch(ReadingOutput out[2], const AxisRun run[2], uint32_t flags, int64_t timeUs,
				int rasterW, int rasterH, const ClassicalFreshState& cf)
			{
				bool conf[2], nat[2];
				for (int a = 0; a < 2; ++a)
				{
					conf[a] = out[a].applied.state == Geometry::LatchState::Latched;
					nat[a] = conf[a] && out[a].applied.n >= out[a].applied.N;
				}
				const int Nx = run[0].have ? run[0].ls.axisLen : (out[0].applied.N > 0 ? out[0].applied.N : rasterW);
				const int Ny = run[1].have ? run[1].ls.axisLen : (out[1].applied.N > 0 ? out[1].applied.N : rasterH);

				JointLatchInput jIn;
				jIn.timeUs = timeUs;
				jIn.c1 = thr.c1;
				jIn.rasterW = rasterW;
				jIn.rasterH = rasterH;
				jIn.menuHold = (flags & (kEvMenuHold | kEvClearWindow)) != 0u || (run[0].have && run[0].up.hold) || (run[1].have && run[1].up.hold);
				jIn.mixedContent = cf.mixed;
				jIn.subtitleOverlay = cf.subtitle;
				jIn.inFade = cf.inFade || ((flags & kEvFade) != 0u);
				jIn.stepMarkPending = stepMarkOn && stepMark.BlocksEntryOrMove();
				jIn.abrStepImmediateRelease = stepMarkImmediateReleasePending;
				stepMarkImmediateReleasePending = false;
				jIn.shotBoundary = shotStartedNow;
				jIn.chainNativeBoth = nat[0] && nat[1];
				jIn.chainInformative = (run[0].have && run[0].up.informative) || (run[1].have && run[1].up.informative);

				// Fresh per-frame classical witness A (never sticky tracker state).
				const bool cfRatiosFinite = std::isfinite(cf.ratioH) && std::isfinite(cf.ratioV) && cf.ratioH > 0.0 && cf.ratioV > 0.0;
				const bool classicalHasUpscale = cf.valid && cfRatiosFinite &&
					cf.ratioH < 1.0 && cf.ratioV < 1.0 && Nx > 0 && Ny > 0;
				if (classicalHasUpscale)
				{
					jIn.classicalValid = true;
					jIn.classicalNx = RoundToInt(static_cast<double>(Nx) * cf.ratioH);
					jIn.classicalNxDenom = Nx;
					jIn.classicalNy = RoundToInt(static_cast<double>(Ny) * cf.ratioV);
					jIn.classicalNyDenom = Ny;
				}
				jIn.classicalNativeReading = cf.valid && cfRatiosFinite && !(cf.ratioH < 1.0 && cf.ratioV < 1.0);

				// Witness B chain-alone pair (confident pair on both axes).
				const AxisSummary sumX = evidence.Summary(0);
				const AxisSummary sumY = evidence.Summary(1);
				jIn.haveChainPair = conf[0] && conf[1] && !(nat[0] && nat[1]);
				if (jIn.haveChainPair)
				{
					jIn.chainNx = out[0].applied.n;
					jIn.chainNxDenom = out[0].applied.N;
					jIn.chainNy = out[1].applied.n;
					jIn.chainNyDenom = out[1].applied.N;
					jIn.chainLlrX = out[0].applied.llr;
					jIn.chainLlrY = out[1].applied.llr;
					jIn.chainShots = (std::min)(sumX.shots, sumY.shots);
				}

				// R1 Fundamental-Harmonic Order Selection (ChainJointLatch.h::SelectFundamentalHarmonic):
				// Evaluate conventional pairs across the union of this evaluation's judged candidates AND live
				// evidence hypothesis slots (so P3 screen-only omission on one frame never blinds R1 to a live
				// slot holding current-shot credit or chain-alone evidence), picking the smallest integer-ratio
				// fundamental confirmed by A roll-off (!detailAbove) + B current-shot credit.
				const double jNeed = jointLatch.JointFraction(timeUs) * thr.c1;
				if (Nx > 0 && Ny > 0)
				{
					std::vector<int> candNs[2];
					for (int a = 0; a < 2; ++a)
					{
						auto addUniqueN = [&](int n) {
							if (n <= 0)
								return;
							for (size_t k = 0; k < candNs[a].size(); ++k)
								if (candNs[a][k] == n)
									return;
							candNs[a].push_back(n);
						};
						if (run[a].have)
						{
							for (int i = 0; i < run[a].up.candidateCount; ++i)
								addUniqueN(run[a].up.cand[i].n);
						}
						std::vector<int> activeNs;
						evidence.ActiveCandidateNs(a, activeNs);
						for (size_t i = 0; i < activeNs.size(); ++i)
							addUniqueN(activeNs[i]);
						if (jIn.haveChainPair)
							addUniqueN(a == 0 ? jIn.chainNx : jIn.chainNy);
					}
					std::vector<HarmonicCandidate> harmCands;
					for (size_t ix = 0; ix < candNs[0].size(); ++ix)
					{
						const int nx = candNs[0][ix];
						const ChainEvidence::ShotDiag dx = evidence.ShotDiagAtN(0, nx);
						for (size_t iy = 0; iy < candNs[1].size(); ++iy)
						{
							const int ny = candNs[1][iy];
							if (!Scaling::GeometrySet::AgreeingPair(nx, Nx, ny, Ny, rasterW, rasterH))
								continue;
							const ChainEvidence::ShotDiag dy = evidence.ShotDiagAtN(1, ny);
							HarmonicCandidate hc;
							hc.nx = nx; hc.Nx = Nx;
							hc.ny = ny; hc.Ny = Ny;
							hc.creditX = dx.open;
							hc.creditY = dy.open;
							const double rx = static_cast<double>(nx) / static_cast<double>(Nx);
							const double ry = static_cast<double>(ny) / static_cast<double>(Ny);
							const bool aAgrees = classicalHasUpscale &&
								Scaling::GeometrySet::Near(cf.ratioH, rx) && Scaling::GeometrySet::Near(cf.ratioV, ry);
							const bool aSeesHigher = classicalHasUpscale &&
								(cf.ratioH > rx * (1.0 + Scaling::GeometrySet::kConventionTolerance) ||
								 cf.ratioV > ry * (1.0 + Scaling::GeometrySet::kConventionTolerance));
							const bool candDetailAbove = cf.valid
								? (cf.detailAboveAtRatio ? cf.detailAboveAtRatio(rx, ry) : cf.detailAbove)
								: false;
							hc.classicalRollOffConfirmed = aAgrees && !candDetailAbove;
							hc.detailAbove = candDetailAbove || aSeesHigher;
							harmCands.push_back(hc);
						}
					}
					if (!harmCands.empty())
					{
						const HarmonicSelectionResult hRes = SelectFundamentalHarmonic(harmCands, rasterW, rasterH, jNeed);
						if (hRes.found && (hRes.reason == HarmonicSelectionReason::SelectedFundamentalConfirmed ||
							hRes.reason == HarmonicSelectionReason::RejectedSubHarmonicDetailAbove))
						{
							cR1HarmonicSelections.fetch_add(1, std::memory_order_relaxed);
							// Apply R1-selected fundamental (or clean super-harmonic after rejecting detailAbove sub-harmonic)
							// to both chain pair (when chain-alone eligible) and joint target.
							if (jIn.haveChainPair)
							{
								const bool overridesChain =
									(jIn.chainNx > hRes.nx ? jIn.chainNx - hRes.nx : hRes.nx - jIn.chainNx) > 1 ||
									(jIn.chainNy > hRes.ny ? jIn.chainNy - hRes.ny : hRes.ny - jIn.chainNy) > 1;
								jIn.chainNx = hRes.nx; jIn.chainNxDenom = hRes.Nx;
								jIn.chainNy = hRes.ny; jIn.chainNyDenom = hRes.Ny;
								if (overridesChain)
								{
									// Refresh per-axis LLR for the R1-selected candidate so an under-c1 candidate can
									// never inherit the rejected/alias candidate's >= c1 LLR for EnteredChainAlone.
									jIn.chainLlrX = evidence.LlrAtN(0, hRes.nx);
									jIn.chainLlrY = evidence.LlrAtN(1, hRes.ny);
									if (jIn.chainLlrX <= -1e8 || jIn.chainLlrY <= -1e8)
										jIn.haveChainPair = false;
								}
							}
							// Stage S3 fix (Stage S2 re-audit MEDIUM): only snap Classical A's target to hRes when Classical A
							// actually agreed with hRes within convention tolerance; never forge Classical A consent when it disagreed.
							if (jIn.classicalValid && classicalHasUpscale && hRes.Nx > 0 && hRes.Ny > 0 &&
								Scaling::GeometrySet::Near(cf.ratioH, static_cast<double>(hRes.nx) / static_cast<double>(hRes.Nx)) &&
								Scaling::GeometrySet::Near(cf.ratioV, static_cast<double>(hRes.ny) / static_cast<double>(hRes.Ny)))
							{
								jIn.classicalNx = hRes.nx; jIn.classicalNxDenom = hRes.Nx;
								jIn.classicalNy = hRes.ny; jIn.classicalNyDenom = hRes.Ny;
							}
						}
						else if (!hRes.found && hRes.reason == HarmonicSelectionReason::RejectedSubHarmonicDetailAbove)
						{
							// All candidate pairs were invalidated by detail_above: clear BOTH chain pair and classical
							// proposal so a detail_above-contradicted sub-harmonic cannot enter via chain-alone OR joint
							// (jIn.classicalDetailAbove remains intact for M1 debounced release while latched).
							jIn.haveChainPair = false;
							jIn.classicalValid = false;
						}
					}
				}

				// B1 / M1: evaluate per-candidate positive detail_above at the latched pair (when latched) or the
				// unlatched target pair (chain pair and/or classical upscale pair); never equate classical reading
				// native (>= 1.0) with positive detail_above.
				if (cf.valid)
				{
					if (jointLatch.Latched() && cf.detailAboveAtRatio)
					{
						const double latchedRx = jointLatch.LatchedNxDenom() > 0
							? static_cast<double>(jointLatch.LatchedNx()) / static_cast<double>(jointLatch.LatchedNxDenom()) : 1.0;
						const double latchedRy = jointLatch.LatchedNyDenom() > 0
							? static_cast<double>(jointLatch.LatchedNy()) / static_cast<double>(jointLatch.LatchedNyDenom()) : 1.0;
						jIn.classicalDetailAbove = cf.detailAboveAtRatio(latchedRx, latchedRy);
					}
					else if (!jointLatch.Latched() && cf.detailAboveAtRatio)
					{
						if (jIn.classicalValid && jIn.classicalNxDenom > 0 && jIn.classicalNyDenom > 0)
						{
							const double classRx = static_cast<double>(jIn.classicalNx) / static_cast<double>(jIn.classicalNxDenom);
							const double classRy = static_cast<double>(jIn.classicalNy) / static_cast<double>(jIn.classicalNyDenom);
							if (cf.detailAboveAtRatio(classRx, classRy))
							{
								if (jIn.haveChainPair)
									jIn.classicalValid = false;
								else
									jIn.classicalDetailAbove = true;
							}
						}
						if (jIn.haveChainPair && jIn.chainNxDenom > 0 && jIn.chainNyDenom > 0)
						{
							const double chainRx = static_cast<double>(jIn.chainNx) / static_cast<double>(jIn.chainNxDenom);
							const double chainRy = static_cast<double>(jIn.chainNy) / static_cast<double>(jIn.chainNyDenom);
							jIn.classicalDetailAbove = cf.detailAboveAtRatio(chainRx, chainRy);
						}
						else if (!jIn.classicalValid)
						{
							jIn.classicalDetailAbove = cf.detailAbove;
						}
					}
					else
					{
						jIn.classicalDetailAbove = cf.detailAbove;
					}
				}
				else
				{
					jIn.classicalDetailAbove = false;
				}

				// Witness B current-shot dwell & credit for the joint / G2 target pair:
				// Prefer the proposed classical pair FIRST so canMoveJoint queries Witness B's real credit for the
				// proposed new size rather than hijacking the previously latched pair's credit (when classical agrees
				// with Latched for G2 freshJointReproof, classicalNx/Ny == LatchedNx/Ny).
				const int targetNx = jIn.classicalValid ? jIn.classicalNx : (jointLatch.Latched() ? jointLatch.LatchedNx() : jIn.chainNx);
				const int targetNy = jIn.classicalValid ? jIn.classicalNy : (jointLatch.Latched() ? jointLatch.LatchedNy() : jIn.chainNy);
				jIn.currentShotDwellSec = (std::min)(evidence.CurrentShotDwellSec(0, timeUs), evidence.CurrentShotDwellSec(1, timeUs));
				if (targetNx > 0 && targetNy > 0)
				{
					const ChainEvidence::ShotDiag dx = evidence.ShotDiagAtN(0, targetNx);
					const ChainEvidence::ShotDiag dy = evidence.ShotDiagAtN(1, targetNy);
					jIn.currentShotCreditX = dx.open;
					jIn.currentShotCreditY = dy.open;
					if (dx.openNewestUs >= 0 && dy.openNewestUs >= 0)
						jIn.currentShotNewestCreditUs = (std::min)(dx.openNewestUs, dy.openNewestUs);
					else
						jIn.currentShotNewestCreditUs = (std::max)(dx.openNewestUs, dy.openNewestUs);
				}

				// Witness B incremental credits for the currently latched size (M1).
				if (jointLatch.Latched())
				{
					jIn.gainX = evidence.CreditAtN(0, jointLatch.LatchedNx(), &jIn.gainTimeX, &jIn.rawX);
					jIn.gainY = evidence.CreditAtN(1, jointLatch.LatchedNy(), &jIn.gainTimeY, &jIn.rawY);
				}

				const JointLatchOutput jlo = jointLatch.Step(jIn);
				FoldJointLatchCounters();
				NoteJointLatchEvent(jlo, timeUs);

				// Discredit stale evidence window on starvation releases (matching UpscaleLatch M1 rule).
				if (jlo.event == JointLatchEvent::ReleasedStarvation ||
					jlo.event == JointLatchEvent::ReleasedHardBound ||
					jlo.event == JointLatchEvent::ReleasedCeiling)
				{
					evidence.Reset();
				}

				latchNxA.store(jlo.latched ? jlo.nx : 0, std::memory_order_release);
				latchNyA.store(jlo.latched ? jlo.ny : 0, std::memory_order_release);
				for (int a = 0; a < 2; ++a)
				{
					const bool confUp = conf[a] && !nat[a];
					Geometry::AxisGeom g = out[a].applied;
					if (jlo.latched)
					{
						const int ln = a == 0 ? jlo.nx : jlo.ny;
						const int lN = a == 0 ? jlo.Nx : jlo.Ny;
						const int dn = out[a].applied.n > ln ? out[a].applied.n - ln : ln - out[a].applied.n;
						if (confUp && dn <= 1)
						{
							heldApplied[a] = out[a].applied;
						}
						else if (heldApplied[a].state == Geometry::LatchState::Latched &&
							(heldApplied[a].n > ln ? heldApplied[a].n - ln : ln - heldApplied[a].n) <= 1)
						{
							g = heldApplied[a];
						}
						else
						{
							// Joint entry (1-shot A+B) or R1 fundamental override: synthesize confident applied AxisGeom for the latched pair.
							Geometry::AxisGeom jg;
							jg.state = Geometry::LatchState::Latched;
							jg.n = ln;
							jg.N = lN;
							jg.ratio = lN > 0 ? static_cast<float>(static_cast<double>(ln) / static_cast<double>(lN)) : 0.0f;
							const double creditAxis = a == 0 ? jIn.currentShotCreditX : jIn.currentShotCreditY;
							// L1 (Rule 1): use honest shot-capped evidence LLR (never uncapped ShotDiag::open) so 1-shot
							// Joint entry reports capped LLR (<= shotCapFraction * c1) and honest derived conf < 1.0f.
							const double capLlr = evidence.LlrAtN(a, ln);
							const double shotCap = cfg.evidence.shotCapFraction * levels.c1;
							const double honestLlr = capLlr > -1e8
								? (std::max)(0.0, capLlr)
								: ClampD(creditAxis, 0.0, shotCap);
							jg.llr = static_cast<float>(honestLlr);
							jg.conf = static_cast<float>(ConfidenceOfLlr(static_cast<double>(jg.llr), levels.c1));
							jg.kernelIndex = out[a].measured.kernelIndex;
							jg.kernelConf = out[a].measured.kernelConf;
							heldApplied[a] = jg;
							g = jg;
						}
						// L1 (Rule 4): expose latch origin (Joint A+B vs Chain-Alone) explicitly in published reasonBits.
						const uint32_t originBit = (jlo.origin == JointLatchOrigin::Joint)
							? kReasonJointEntry
							: kReasonChainAloneEntry;
						const uint32_t originMask = kReasonJointEntry | kReasonChainAloneEntry;
						heldApplied[a].reasonBits = (heldApplied[a].reasonBits & ~originMask) | originBit;
						g.reasonBits = (g.reasonBits & ~originMask) | originBit;
					}
					else
					{
						heldApplied[a] = Geometry::AxisGeom();
						if (confUp)
							g = UnsureLike(out[a].applied, jlo.reasonBits | kReasonLatchBar);
						else if (jlo.reasonBits != 0u && !conf[a])
							g.reasonBits |= jlo.reasonBits;
					}
					const bool changed = g.state != lastFiltered[a].state || g.n != lastFiltered[a].n || g.kernelIndex != lastFiltered[a].kernelIndex;
					out[a].applied = g;
					out[a].appliedChanged = changed;
					lastFiltered[a] = g;
				}
			}

			// Filters the readers applied geometry through the legacy r2 latch (used when safetyOn && !jointLatchOn).
			// R1 fundamental-harmonic selection is implemented in ApplyJointLatch via SelectFundamentalHarmonic (ChainJointLatch.h).
			void ApplyLatch(ReadingOutput out[2], const AxisRun run[2], uint32_t flags, int64_t timeUs, int rasterW, int rasterH)
			{
				bool conf[2], nat[2];
				for (int a = 0; a < 2; ++a)
				{
					conf[a] = out[a].applied.state == Geometry::LatchState::Latched;
					nat[a] = conf[a] && out[a].applied.n >= out[a].applied.N;
				}
				LatchInput in;
				in.timeUs = timeUs;
				in.c1 = thr.c1;
				in.informative = (run[0].have && run[0].up.informative) || (run[1].have && run[1].up.informative);
				in.menuHold = (flags & (kEvMenuHold | kEvClearWindow)) != 0u || (run[0].have && run[0].up.hold) || (run[1].have && run[1].up.hold);
				in.abrStep = (flags & kEvAbrStep) != 0u;
				in.abrStepImmediateRelease = stepMarkImmediateReleasePending;
				stepMarkImmediateReleasePending = false;
				in.nativeBoth = nat[0] && nat[1];
				in.shotBoundary = shotStartedNow;   // N1 / N2: a new evidence shot (a cut, or a rate-limited black / fade, F1) starts the dwell
				in.havePair = !(stepMarkOn && stepMark.BlocksEntryOrMove()) && conf[0] && conf[1] && !(nat[0] && nat[1]);
				if (in.havePair)
				{
					in.nx = out[0].applied.n;
					in.Nx = out[0].applied.N;
					in.ny = out[1].applied.n;
					in.Ny = out[1].applied.N;
					in.llrX = out[0].applied.llr;
					in.llrY = out[1].applied.llr;
				}
				in.rasterW = rasterW;
				in.rasterH = rasterH;
				if (latch.Latched())
				{
					// M1: only the evidence CREDITED by this evaluation re-confirms (never the windowed total).
					in.gainX = evidence.CreditAtN(0, latch.LatchedNx(), &in.gainTimeX, &in.rawX);
					in.gainY = evidence.CreditAtN(1, latch.LatchedNy(), &in.gainTimeY, &in.rawY);
				}
				const LatchOutput lo = latch.Step(in);
				diagLo = lo;   // dump v4 (a copy, read only)
				FoldLatchCounters();
				NoteLatchEvent(lo, timeUs);
				if (LatchEventIsEvidenceRelease(lo.event))
					evidence.Reset();   // M1: a release for lack of new evidence; the stale window must not re-enter at the next evaluation
				latchNxA.store(lo.latched ? lo.nx : 0, std::memory_order_release);
				latchNyA.store(lo.latched ? lo.ny : 0, std::memory_order_release);
				for (int a = 0; a < 2; ++a)
				{
					const bool confUp = conf[a] && !nat[a];
					Geometry::AxisGeom g = out[a].applied;
					if (lo.latched)
					{
						const int ln = a == 0 ? lo.nx : lo.ny;
						const int dn = out[a].applied.n > ln ? out[a].applied.n - ln : ln - out[a].applied.n;
						if (confUp && dn <= 1)
							heldApplied[a] = out[a].applied;   // H1: a confident reading of the latched (or just moved) size
						else if (heldApplied[a].state == Geometry::LatchState::Latched)
							g = heldApplied[a];                // H1 / L6: any other reading publishes the held latch
						else
							g = UnsureLike(out[a].applied, kReasonLatchBar);   // nothing held for this axis yet: never another size
					}
					else
					{
						heldApplied[a] = Geometry::AxisGeom();
						if (confUp)
							g = UnsureLike(out[a].applied, lo.reasonBits | kReasonLatchBar);
						else if (lo.reasonBits != 0u && !conf[a])
							g.reasonBits |= lo.reasonBits;
					}
					const bool changed = g.state != lastFiltered[a].state || g.n != lastFiltered[a].n || g.kernelIndex != lastFiltered[a].kernelIndex;
					out[a].applied = g;
					out[a].appliedChanged = changed;
					lastFiltered[a] = g;
				}
			}

			void Process(const Job& job)
			{
				const LineSnapshot& s = *job.snap;
				const double t0 = nowMs();
				gov.NoteStart(t0);
				// SID r2 L3: the delivered raster (a snapshot without one, e.g. a test feed, falls back to its active extent).
				const int rasterW = s.rasterW > 0 ? static_cast<int>(s.rasterW) : s.active.w;
				const int rasterH = s.rasterH > 0 ? static_cast<int>(s.rasterH) : s.active.h;
				evidence.ClearCredits();   // M1: an axis not submitted this evaluation credits nothing
				diagLo = LatchOutput();    // dump v4: this evaluation's latch output (none until ApplyLatch runs)
				diagResetEvent = LatchEvent::None;
				lastStartMs.store(t0, std::memory_order_release);
				std::string fmt;
				bool classicalOk = false;
				double classical[2] = { 1.0, 1.0 };
				ClassicalFreshState cf;
				std::vector<AbrStepEventRec> abrStepsNow;
				{
					std::lock_guard<std::mutex> lk(m);
					fmt = formatInfo;
					classicalOk = classicalValid;
					classical[0] = classicalRatio[0];
					classical[1] = classicalRatio[1];
					cf = classicalFresh;
					if (stepMarkOn && !abrStepEvents.empty())
					{
						for (size_t i = 0; i < abrStepEvents.size();)
						{
							if (abrStepEvents[i].seq <= s.seq)
							{
								abrStepsNow.push_back(abrStepEvents[i]);
								abrStepEvents.erase(abrStepEvents.begin() + static_cast<std::ptrdiff_t>(i));
							}
							else
								++i;
						}
					}
				}
				log.SetFormatInfo(fmt);

				// M1: bound the age of the "fresh" classical witness (2x analysis interval cutoff, Rule 1/Rule 4).
				if (cf.valid && cf.timeUs >= 0 && s.timeUs >= 0)
				{
					const double baseIntervalSec = (std::isfinite(cf.analysisIntervalSec) && cf.analysisIntervalSec > 0.0)
						? cf.analysisIntervalSec
						: (std::max)(gov.Plan(false).intervalSec, 0.5);
					const double maxAgeUs = 2.0 * baseIntervalSec * 1e6;
					if (s.timeUs > cf.timeUs && static_cast<double>(s.timeUs - cf.timeUs) > maxAgeUs)
					{
						cf.valid = false;
						cf.detailAbove = false;
						cf.detailAboveAtRatio = nullptr;
						cClassicalFreshStaleDrops.fetch_add(1, std::memory_order_relaxed);
					}
				}

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
				shotStartedNow = cutsNow > 0;
				const bool rulesActive = safetyOn || jointLatchOn;
				if (!shotStartedNow && rulesActive && (flags & (kEvBlank | kEvFade)) != 0u)
				{
					// N2 (F1): a black / fade with no cut is a visible transition: a new evidence shot, so a true upscale can re-confirm in
					// the scene after it (and a deferred starvation release waits for that shot's dwell, ChainLatch N1). Rate-limited to
					// one per minimum shot dwell since the previous shot start (a backward time step re-anchors).
					const double minDwell = cfg.evidence.minShotDwellSec > 0.0 ? cfg.evidence.minShotDwellSec : kMinShotDwellSec;
					const int64_t since = s.timeUs - shotStartUs;
					if (!haveShotStart || since < 0 || static_cast<double>(since) / 1e6 >= minDwell)
					{
						++shotId;
						shotStartedNow = true;
						cFadeShots.fetch_add(1, std::memory_order_relaxed);
					}
				}
				if (shotStartedNow)
				{
					haveShotStart = true;
					shotStartUs = s.timeUs;
				}
				// Stage S2: ChainStepMark pre-eval processing (clean reset on cut/area change, raise pending mark on ABR step,
				// optional fast classical confirmation). Inert unless stepMarkOn.
				auto currentLatchCtx = [&]() {
					StepMarkLatchContext ctx;
					if (jointLatchOn && jointLatch.Latched())
					{
						ctx.latched = true;
						ctx.nx = jointLatch.LatchedNx(); ctx.Nx = jointLatch.LatchedNxDenom();
						ctx.ny = jointLatch.LatchedNy(); ctx.Ny = jointLatch.LatchedNyDenom();
					}
					else if (!jointLatchOn && safetyOn && latch.Latched())
					{
						ctx.latched = true;
						ctx.nx = latch.LatchedNx(); ctx.Nx = latch.LatchedNxDenom() > 0 ? latch.LatchedNxDenom() : (lastAxisLen[0] > 0 ? lastAxisLen[0] : rasterW);
						ctx.ny = latch.LatchedNy(); ctx.Ny = latch.LatchedNyDenom() > 0 ? latch.LatchedNyDenom() : (lastAxisLen[1] > 0 ? lastAxisLen[1] : rasterH);
					}
					return ctx;
				};
				DiagEvalExtra diagExtra;
				if (stepMarkOn)
				{
					if (areaChanged)
					{
						stepMark.Reset(StepMarkReason::ResetDiscontinuity);
						stepMarkImmediateReleasePending = false;
					}
					else if (cutsNow > 0)
					{
						stepMark.Reset(StepMarkReason::ResetOnCut);
					}
					for (size_t i = 0; i < abrStepsNow.size(); ++i)
						stepMark.OnAbrStep(abrStepsNow[i].t0Us, abrStepsNow[i].ratioStep);
					// H1: deduplicate classical frames so one stale snapshot can never be counted as two consecutive frames.
					const bool cfIsNew = cf.valid &&
						((cf.frameSeq > 0 && cf.frameSeq != lastStepMarkClassicalSeq) ||
						 (cf.frameSeq == 0 && cf.updateGen > 0 && cf.updateGen != lastStepMarkClassicalGen));
					if (stepMark.IsPending() && cfIsNew)
					{
						if (cf.frameSeq > 0)
							lastStepMarkClassicalSeq = cf.frameSeq;
						lastStepMarkClassicalGen = cf.updateGen;
						const StepMarkLatchContext lctx = currentLatchCtx();
						StepMarkClassicalFrame cfr;
						cfr.timeUs = cf.timeUs >= 0 ? cf.timeUs : s.timeUs;
						const bool cfRatiosFinite = std::isfinite(cf.ratioH) && std::isfinite(cf.ratioV) && cf.ratioH > 0.0 && cf.ratioV > 0.0;
						cfr.informative = cfRatiosFinite;
						cfr.voided = cf.mixed || cf.subtitle || cf.inFade || ((flags & kEvMenuHold) != 0u);
						cfr.nativeReading = cfRatiosFinite && !(cf.ratioH < 1.0 && cf.ratioV < 1.0);
						if (lctx.latched && lctx.Nx > 0 && lctx.Ny > 0)
						{
							const double latchedRx = static_cast<double>(lctx.nx) / static_cast<double>(lctx.Nx);
							const double latchedRy = static_cast<double>(lctx.ny) / static_cast<double>(lctx.Ny);
							cfr.positiveDetailAbove = cf.detailAboveAtRatio
								? cf.detailAboveAtRatio(latchedRx, latchedRy)
								: cf.detailAbove;
						}
						else
						{
							cfr.positiveDetailAbove = false;
						}
						cfr.rasterW = rasterW;
						cfr.rasterH = rasterH;
						const int activeW = s.active.w > 0 ? s.active.w : (lastAxisLen[0] > 0 ? lastAxisLen[0] : rasterW);
						const int activeH = s.active.h > 0 ? s.active.h : (lastAxisLen[1] > 0 ? lastAxisLen[1] : rasterH);
						if (cfRatiosFinite && !cfr.nativeReading && activeW > 0 && activeH > 0)
						{
							cfr.hasPair = true;
							cfr.nx = RoundToInt(static_cast<double>(activeW) * cf.ratioH);
							cfr.Nx = activeW;
							cfr.ny = RoundToInt(static_cast<double>(activeH) * cf.ratioV);
							cfr.Ny = activeH;
						}
						const StepMarkOutput smo = stepMark.OnClassicalFrame(cfr, lctx);
						if (smo.action == StepMarkAction::ConfirmVoidOnly ||
							smo.action == StepMarkAction::ConfirmVoidAndRelease ||
							smo.action == StepMarkAction::ConfirmVoidAndChallenge)
						{
							evidence.VoidBefore(smo.actionFromTimeUs);
							diagExtra.stepMarkConfirmed = true;
							diagExtra.tentativeBucketVoided = true;
							if (smo.action == StepMarkAction::ConfirmVoidAndRelease)
								stepMarkImmediateReleasePending = true;
						}
						else if (smo.action == StepMarkAction::CancelAndMergeBucket)
						{
							diagExtra.stepMarkCancelled = true;
						}
					}
					FoldStepMarkCounters();
				}
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
				const double theoreticalFloorCodes = step >= 4 ? 1.155 : 0.289;   // the dof-normalised rounding / dither sigma (ChainEvidence.h)
				double floorCodes = theoreticalFloorCodes;
				const EmpiricalFloorMode effFloorMode = (cfg.eightBitPromotion || cfg.useEmpiricalFloor)
					? EmpiricalFloorMode::Live
					: cfg.empiricalFloorMode;

				AxisRun (&run)[2] = evalRun;
				for (int a = 0; a < 2; ++a)
				{
					run[a].have = false;
					run[a].pr = ProbeResult();
					run[a].up = EvidenceUpdate();
					run[a].vr.candidates.clear();
					run[a].vr.reason = 0;
					run[a].vr.costMs = 0.0;
					run[a].vr.cold = false;
					run[a].vr.operatorBuilds = 0;
					run[a].vr.solves = 0;
					run[a].vr.solvesReused = 0;
					if (ToLineSet(s, a, run[a].ls))
						run[a].have = true;
				}
				// Stage S3 (PKG-SRCID-FLOOR / SHOULD-1 / EXP-8BIT, opt-in, default OFF): empirical floor policy (Off / Shadow / Live).
				double empFloor[2] = { 0.0, 0.0 };
				bool empValid[2] = { false, false };
				bool empFallback = false;
				bool haveDepthEmpFloor = false;
				double empFloorForDepth = 0.0;
				auto recordDepthFloor = [&](const EmpiricalFloorResult& ef, const EdgeStatsSettings& efCfg) {
					if (!ef.valid)
						return;
					// M5 physics (Rule 1): EstimateEmpiricalFloor skips exact-zero-MAD flat segments. If clean flat
					// segments account for >= quietSegmentQuantile (25%) of totalSegments, the true lower-quartile
					// noise floor is sub-1-code (clean 10-bit ramp/flat), NOT an 8-bit HDMI-loop floor.
					const double q = std::isfinite(efCfg.quietSegmentQuantile)
						? ClampD(efCfg.quietSegmentQuantile, 0.0, 0.5)
						: (1.0 / 4.0);
					const double axisDepthFloor = (ef.totalSegments > 0 &&
						static_cast<double>(ef.zeroSegmentsSkipped) >= q * static_cast<double>(ef.totalSegments))
						? 0.0
						: ef.floorCodes;
					if (!haveDepthEmpFloor || axisDepthFloor < empFloorForDepth)
					{
						empFloorForDepth = axisDepthFloor;
						haveDepthEmpFloor = true;
					}
				};
				if (effFloorMode != EmpiricalFloorMode::Off)
				{
					EdgeStatsSettings empCfg = cfg.evidence.edgeStats;
					empCfg.enabled = true;
					double maxMeasuredFloor = 0.0;
					int activeAxes = 0;
					int validAxes = 0;
					int clampedFallbackAxes = 0;
					int invalidAxes = 0;
					for (int a = 0; a < 2; ++a)
					{
						if (!run[a].have)
							continue;
						++activeAxes;
						const EmpiricalFloorResult ef = EstimateEmpiricalFloor(run[a].ls, static_cast<double>(step), empCfg);
						empFloor[a] = ef.floorCodes;
						empValid[a] = ef.valid;
						recordDepthFloor(ef, empCfg);
						if (ef.valid)
						{
							++validAxes;
							if (ef.floorCodes > maxMeasuredFloor)
								maxMeasuredFloor = ef.floorCodes;
							if (ef.usedTheoreticalFallback)
								++clampedFallbackAxes;
						}
						else
						{
							++invalidAxes;
						}
					}
					const int fallbackAxes = invalidAxes + clampedFallbackAxes;
					if (validAxes > 0)
					{
						cEmpiricalFloorEvals.fetch_add(1, std::memory_order_relaxed);
						if (effFloorMode == EmpiricalFloorMode::Live)
							floorCodes = maxMeasuredFloor;
						if (fallbackAxes > 0)
						{
							empFallback = true;
							cEmpiricalFloorFallbacks.fetch_add(1, std::memory_order_relaxed);
							if (fallbackAxes >= activeAxes)
								cEmpiricalFloorAllAxisFallbacks.fetch_add(1, std::memory_order_relaxed);
							else
								cEmpiricalFloorPartialAxisFallbacks.fetch_add(1, std::memory_order_relaxed);
						}
					}
					else if (activeAxes > 0)
					{
						// Rule 4: record all-axis fallback visibly whenever empirical floor failed on every active axis.
						empFallback = true;
						cEmpiricalFloorFallbacks.fetch_add(1, std::memory_order_relaxed);
						cEmpiricalFloorAllAxisFallbacks.fetch_add(1, std::memory_order_relaxed);
					}
				}
				else if (cfg.useDepthNullTables || cfg.eightBitPromotion)
				{
					// M5: even with EmpiricalFloorMode::Off, measure empirical floor into a local diagnostic variable
					// for depth classification so an 8-bit-in-10-bit HDMI loop (step==1, outputBits==10) classifies as N8, never N10.
					EdgeStatsSettings empCfg = cfg.evidence.edgeStats;
					empCfg.enabled = true;
					for (int a = 0; a < 2; ++a)
					{
						if (!run[a].have)
							continue;
						const EmpiricalFloorResult ef = EstimateEmpiricalFloor(run[a].ls, static_cast<double>(step), empCfg);
						recordDepthFloor(ef, empCfg);
					}
				}
				// Stage S3 (M5): bit-depth-stratified null tables & placeholder visibility (strict depth isolation; default OFF).
				const BitDepthClass reqDepth = ClassifyBitDepthFromStep(
					step, diagOutputBits.load(std::memory_order_acquire), haveDepthEmpFloor ? empFloorForDepth : 0.0);
				bool nullCalibrated = !evidence.Nulls().placeholder;
				const BitDepthClass nullDepth = reqDepth;
				if (cfg.useDepthNullTables || cfg.eightBitPromotion)
				{
					const DepthNullTableLookupResult nulRes = cfg.depthNullTables.Select(reqDepth);
					evidence.SetNullTables(nulRes.tables);
					nullCalibrated = nulRes.calibrated;
					if (nulRes.usedPlaceholderFallback)
						cDepthNullTableFallbacks.fetch_add(1, std::memory_order_relaxed);
				}
				if (evidence.Nulls().placeholder)
					cPlaceholderNullTableEvals.fetch_add(1, std::memory_order_relaxed);

				diagExtra.empFloorMode = effFloorMode;
				diagExtra.floorTheoretical = theoreticalFloorCodes;
				diagExtra.empFloor[0] = empFloor[0];
				diagExtra.empFloor[1] = empFloor[1];
				diagExtra.empValid[0] = empValid[0];
				diagExtra.empValid[1] = empValid[1];
				diagExtra.empFallback = empFallback;
				diagExtra.nullCalibrated = nullCalibrated;
				diagExtra.nullDepth = nullDepth;

				for (int a = 0; a < 2; ++a)
				{
					AxisRun& r = run[a];
					if (!r.have)
						continue;
					const int N = r.ls.axisLen;
					const bool skipProposer = convProposalsOn && rulesActive;
					if (!skipProposer)
						proposer[a].AddEvaluation(r.ls);
					int cands[kMaxCandidates];
					int pc = 0;
					VerifyOptions opt;
					if (convProposalsOn)
					{
						// Stage S2 (P3): convention-driven exact proposals prioritized as classical A -> latched -> SourceGeometrySet.
						const double cRatio = a == 0 ? cf.ratioH : cf.ratioV;
						const int classicalN = (cf.valid && std::isfinite(cRatio) && cRatio > 0.0 && cRatio < 1.0)
							? RoundToInt(static_cast<double>(N) * cRatio) : 0;
						int latchedN = 0;
						if (jointLatchOn && jointLatch.Latched())
							latchedN = a == 0 ? jointLatch.LatchedNx() : jointLatch.LatchedNy();
						else if (safetyOn && latch.Latched())
							latchedN = a == 0 ? latch.LatchedNx() : latch.LatchedNy();
						else if (reader[a].HasConfident() && !reader[a].ConfidentNative())
							latchedN = reader[a].ConfidentN();
						if (skipProposer)
						{
							// SID-OPT: when rulesActive, BuildConventionCandidates already enumerates all non-parked conventional
							// geometries + classicalN + latchedN, so proposer extra candidates cannot add any new conventional size.
							pc = BuildConventionCandidates(a, N, rasterW, rasterH, classicalN, latchedN,
								nullptr, 0, true, cands, kMaxCandidates);
						}
						else
						{
							ProposedCandidate prop[kMaxCandidates];
							const int extraCnt = proposer[a].Propose(N, nullptr, 0, prop, kMaxCandidates);
							int extraCands[kMaxCandidates];
							for (int i = 0; i < extraCnt; ++i)
								extraCands[i] = prop[i].n;
							pc = BuildConventionCandidates(a, N, rasterW, rasterH, classicalN, latchedN,
								extraCands, extraCnt, false, cands, kMaxCandidates);
						}
						opt.exactOnly = true;       // P3: exact-verify prioritized conventional candidates directly
						// SID-OPT: two-pass P3 screen only when whole-line stat is the active estimator (not diluted sparse 8-bit edgeStats)
						opt.p3TwoPass = !cfg.evidence.edgeStats.enabled && !cfg.eightBitPromotion;
						opt.p3ProtectedN[0] = classicalN;
						opt.p3ProtectedN[1] = latchedN;
						cP3ConvProposals.fetch_add(1, std::memory_order_relaxed);
					}
					else
					{
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
						pc = proposer[a].Propose(N, caller, cc, prop, kMaxCandidates);
						for (int i = 0; i < pc; ++i)
							cands[i] = prop[i].n;
					}
					opt.exactLines = ExactLinesFor(cfg, latchedMode, reader[a].HasConfident(), reader[1 - a].HasConfident());
					if (reader[a].HasConfident() && !reader[a].ConfidentNative())
						opt.forceExactN = reader[a].ConfidentN();
					verifier->Evaluate(r.ls, cands, pc, opt, r.vr);
					for (size_t ci = 0; ci < r.vr.candidates.size(); ++ci)
						if (r.vr.candidates[ci].valid && (r.vr.candidates[ci].reason & kReasonScreenOnly) != 0u)
							cP3ScreenOnlyCandidates.fetch_add(1, std::memory_order_relaxed);
					PowerProbe::Run(*probeVerifier, r.ls, floorCodes, static_cast<double>(step), cfg.probe, r.pr, &probeWs[a]);
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
					obs.rasterW = rasterW;   // R2 / R3 ratios from the delivered raster (L3)
					obs.rasterH = rasterH;
					evidence.Submit(a, obs, run[a].up);
					for (int ci = 0; ci < run[a].up.candidateCount; ++ci)
						if (run[a].up.cand[ci].edgeStatsNullMismatch)
							cEdgeStatsNullTableAbstains.fetch_add(1, std::memory_order_relaxed);
				}
				// Stage S2: feed current-eval chain evidence into ChainStepMark while Pending or Challenging (inert unless stepMarkOn).
				if (stepMarkOn && (stepMark.IsPending() || stepMark.IsChallenging()))
				{
					const StepMarkLatchContext lctx = currentLatchCtx();
					TentativeEvalRecord rec;
					rec.timeUs = s.timeUs;
					rec.evalSeq = ++stepMarkEvalSeq;
					rec.rasterW = rasterW;
					rec.rasterH = rasterH;

					// Build TentativeEvalRecord from THIS evaluation's fresh run[a].up (never stale pre-step Summary).
					bool nativePos[2] = { false, false };
					bool latchedPos[2] = { false, false };
					bool latchedNeg[2] = { false, false };
					double latchedInc[2] = { 0.0, 0.0 };
					for (int a = 0; a < 2; ++a)
					{
						if (!run[a].have || run[a].up.hold)
							continue;
						const EvidenceUpdate& up = run[a].up;
						nativePos[a] = up.nativeCounted && up.nativeIncrement > 0.0;
						const int lTarget = a == 0 ? lctx.nx : lctx.ny;
						if (lctx.latched && lTarget > 0)
						{
							for (int i = 0; i < up.candidateCount; ++i)
							{
								const CandidateEvidence& ce = up.cand[i];
								if (!ce.informative)
									continue;
								const int d = ce.n > lTarget ? ce.n - lTarget : lTarget - ce.n;
								if (d <= 1)
								{
									if (ce.increment > 0.0)
									{
										latchedPos[a] = true;
										if (ce.increment > latchedInc[a])
											latchedInc[a] = ce.increment;
									}
									else if (ce.increment < 0.0)
									{
										latchedNeg[a] = true;
									}
								}
							}
						}
					}

					// Select best positive informative candidate pair on THIS evaluation.
					if (run[0].have && run[1].have && !run[0].up.hold && !run[1].up.hold)
					{
						const int Nx = run[0].ls.axisLen;
						const int Ny = run[1].ls.axisLen;
						bool foundPair = false;
						int bestNx = 0, bestNy = 0;
						double bestMinInc = 0.0, bestSumInc = 0.0, bestIncX = 0.0, bestIncY = 0.0;
						for (int ix = 0; ix < run[0].up.candidateCount; ++ix)
						{
							const CandidateEvidence& cx = run[0].up.cand[ix];
							if (!cx.informative || !(cx.increment > 0.0) || cx.n <= 0)
								continue;
							for (int iy = 0; iy < run[1].up.candidateCount; ++iy)
							{
								const CandidateEvidence& cy = run[1].up.cand[iy];
								if (!cy.informative || !(cy.increment > 0.0) || cy.n <= 0)
									continue;
								if (rulesActive && !Scaling::GeometrySet::AgreeingPair(cx.n, Nx, cy.n, Ny, rasterW, rasterH))
									continue;
								const double minInc = (std::min)(cx.increment, cy.increment);
								const double sumInc = cx.increment + cy.increment;
								if (!foundPair ||
									minInc > bestMinInc + 1e-12 ||
									(std::fabs(minInc - bestMinInc) <= 1e-12 &&
										(sumInc > bestSumInc + 1e-12 ||
											(std::fabs(sumInc - bestSumInc) <= 1e-12 &&
												static_cast<int64_t>(cx.n) * cy.n < static_cast<int64_t>(bestNx) * bestNy))))
								{
									foundPair = true;
									bestNx = cx.n;
									bestNy = cy.n;
									bestMinInc = minInc;
									bestSumInc = sumInc;
									bestIncX = cx.increment;
									bestIncY = cy.increment;
								}
							}
						}
						if (foundPair)
						{
							// R1 harmonic guard: when the latched pair itself earned positive credit on BOTH axes
							// this eval, a super-harmonic alias (e.g. 2/3 on a static 1/3 stream) must not falsely
							// contradict the latched fundamental on a spurious ABR mark.
							bool suppressSuperHarmonicAlias = false;
							if (lctx.latched && latchedPos[0] && latchedPos[1] && lctx.Nx > 0 && lctx.Ny > 0 && Nx > 0 && Ny > 0)
							{
								const double lrx = static_cast<double>(lctx.nx) / static_cast<double>(lctx.Nx);
								const double lry = static_cast<double>(lctx.ny) / static_cast<double>(lctx.Ny);
								const double rx = static_cast<double>(bestNx) / static_cast<double>(Nx);
								const double ry = static_cast<double>(bestNy) / static_cast<double>(Ny);
								if (IsIntegerHarmonicOf(lrx, rx) && IsIntegerHarmonicOf(lry, ry))
									suppressSuperHarmonicAlias = true;
							}
							rec.hasChainPair = true;
							if (suppressSuperHarmonicAlias)
							{
								rec.nx = lctx.nx; rec.Nx = lctx.Nx;
								rec.ny = lctx.ny; rec.Ny = lctx.Ny;
								rec.creditX = latchedInc[0];
								rec.creditY = latchedInc[1];
							}
							else
							{
								rec.nx = bestNx; rec.Nx = Nx;
								rec.ny = bestNy; rec.Ny = Ny;
								rec.creditX = bestIncX;
								rec.creditY = bestIncY;
							}
						}
					}

					// Current-eval native support (both axes counted positive native increment on THIS eval).
					rec.chainSeesNative = nativePos[0] && nativePos[1];
					// Direct chain contradiction against active latch on THIS eval: neither axis supported the
					// latched size, and both axes actively contradicted it (negative credit at latched size or
					// paired with positive native evidence on the other axis).
					if (lctx.latched && !latchedPos[0] && !latchedPos[1])
					{
						if ((latchedNeg[0] && latchedNeg[1]) ||
							(latchedNeg[0] && nativePos[1]) ||
							(nativePos[0] && latchedNeg[1]))
						{
							rec.chainContradicts = true;
						}
					}

					const bool reconfirmedNow = lctx.latched &&
						evidence.CreditAtN(0, lctx.nx) > 0.0 && evidence.CreditAtN(1, lctx.ny) > 0.0;
					const StepMarkOutput smo = stepMark.OnChainEvaluation(rec, lctx, reconfirmedNow);
					if (smo.action == StepMarkAction::ConfirmVoidOnly ||
						smo.action == StepMarkAction::ConfirmVoidAndRelease ||
						smo.action == StepMarkAction::ConfirmVoidAndChallenge)
					{
						evidence.VoidBefore(smo.actionFromTimeUs);
						diagExtra.stepMarkConfirmed = true;
						diagExtra.tentativeBucketVoided = true;
					}
					else if (smo.action == StepMarkAction::CancelAndMergeBucket)
					{
						diagExtra.stepMarkCancelled = true;
					}
					if (smo.action == StepMarkAction::ConfirmVoidAndRelease ||
						smo.action == StepMarkAction::ChallengeExpiredRelease)
					{
						stepMarkImmediateReleasePending = true;
					}
					FoldStepMarkCounters();
				}

				if (observer)
				{
					EvaluationDetail d;
					d.seq = s.seq;
					d.timeUs = s.timeUs;
					d.streamKey = s.streamKey;
					d.shotId = shotId;   // the estimator's own detected shot (obs.shotId above)
					d.stepCodes = step;
					d.cachedMB = static_cast<double>(verifier->CachedBytes() + probeVerifier->CachedBytes()) / 1048576.0;
					d.empiricalFloorMode = effFloorMode;
					d.theoreticalFloorCodes = theoreticalFloorCodes;
					d.empiricalFloorCodes[0] = empFloor[0];
					d.empiricalFloorCodes[1] = empFloor[1];
					d.empiricalFloorValid[0] = empValid[0];
					d.empiricalFloorValid[1] = empValid[1];
					d.empiricalFloorFallback = empFallback;
					d.nullTablesCalibrated = nullCalibrated;
					d.nullTableDepthClass = nullDepth;
					for (int a = 0; a < 2; ++a)
					{
						AxisDetail& ad = d.axis[a];
						ad.have = run[a].have;
						if (!run[a].have)
							continue;
						ad.N = run[a].ls.axisLen;
						ad.lines = run[a].ls.count;
						ad.operatorBuilds = run[a].vr.operatorBuilds;
						ad.solves = run[a].vr.solves;
						ad.solvesReused = run[a].vr.solvesReused;
						for (size_t ci = 0; ci < run[a].vr.candidates.size() && ad.exactLines == 0; ++ci)
							if (run[a].vr.candidates[ci].valid && run[a].vr.candidates[ci].exactDone)
								ad.exactLines = run[a].vr.candidates[ci].lineCount;
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
							cd.screenOnly = (cr.reason & kReasonScreenOnly) != 0u;
							cd.stat = cr.stat;
							cd.coarseStat = cr.coarseStat;
							if (cr.exactDone && !cd.screenOnly)
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
				if (jointLatchOn)
				{
					if (anyReset || areaChanged)
					{
						const JointLatchOutput jlo = jointLatch.Reset();
						FoldJointLatchCounters();
						NoteJointLatchEvent(jlo, s.timeUs);
						heldApplied[0] = Geometry::AxisGeom();
						heldApplied[1] = Geometry::AxisGeom();
					}
					ApplyJointLatch(out, run, flags, s.timeUs, rasterW, rasterH, cf);
				}
				else if (safetyOn)
				{
					if (anyReset || areaChanged)
					{
						const LatchOutput lo = latch.Reset();   // a discontinuity / active-area change: the latch goes (counted, noticed: M4)
						diagResetEvent = lo.event;   // dump v4 (read only)
						FoldLatchCounters();
						NoteLatchEvent(lo, s.timeUs);
						heldApplied[0] = Geometry::AxisGeom();
						heldApplied[1] = Geometry::AxisGeom();
					}
					ApplyLatch(out, run, flags, s.timeUs, rasterW, rasterH);
				}
				else
				{
					stepMarkImmediateReleasePending = false;
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
				// M3: the R4 dump runs AFTER the cost is measured, so its verifier and file I/O never feed the governor (decision neutral).
				if (!dumpPath.empty())
					DumpR4(run, s.seq, s.timeUs, rasterW, rasterH);
				// Both axes CONFIDENT: the long cadence (5 s, 2% share); anything else evaluates at the acquiring cadence. maxAgeUs follows
				// the PLANNED interval, not the nominal one.
				// N2 (S1 bound): while the monitor keeps leaving native-looking frames out on the strength of this very reading (a note since
				// the last evaluation), the long cadence does not apply: the reading is re-tested at the acquiring cadence, so a source that
				// genuinely became native flips it quickly (and the window of ChainEvidence follows the measured, shorter interval).
				const bool bothConfident = rulesActive ? (out[0].applied.state == Geometry::LatchState::Latched && out[1].applied.state == Geometry::LatchState::Latched)
					: (AxisConfident(reader[0]) && AxisConfident(reader[1]));
				const bool nativeHeld = nativeHeldMask.exchange(0u, std::memory_order_acq_rel) != 0u;
				// N1: while the new shot's re-confirm dwell is open (or ABR step mark wants fast confirmation) re-test at acquiring cadence.
				const bool dwell = (jointLatchOn && jointLatch.Dwelling()) || (!jointLatchOn && safetyOn && latch.Dwelling()) ||
					(stepMarkOn && stepMark.PullNextEval());
				const bool latchedWithPower = bothConfident && !nativeHeld && !dwell;
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
				// Dump v4 (VP_SID_DUMP_V4_DIR): LAST, after the cost, the schedule, the published record and the status (audit M2: its I/O and
				// verifier delay nothing the decision or its timing reads); reads only (its verifier and its files are its own).
				if (diagEvals.Ready(diagTag, "b_evals", Diag::EvalsHeaderV4()))
					DumpV4(run, out, flags, windowCleared, s.seq, s.timeUs, rasterW, rasterH, step, floorCodes, classicalOk, classical, cost, diagExtra);
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
				in.windowClears = c.windowClears;
				in.safetyRules = safetyLiveA.load(std::memory_order_acquire);
				in.latchNx = latchNxA.load(std::memory_order_acquire);
				in.latchNy = latchNyA.load(std::memory_order_acquire);
				in.latchDeferred = latchDeferredA.load(std::memory_order_acquire);
				in.latch = c.latch;
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
				c.latch.entries = cLatchEntries.load(std::memory_order_relaxed);
				c.latch.moves = cLatchMoves.load(std::memory_order_relaxed);
				c.latch.releases = cLatchReleases.load(std::memory_order_relaxed);
				c.latch.releasesBound = cLatchRelBound.load(std::memory_order_relaxed);
				c.latch.releasesNative = cLatchRelNative.load(std::memory_order_relaxed);
				c.latch.releasesMenu = cLatchRelMenu.load(std::memory_order_relaxed);
				c.latch.releasesAbr = cLatchRelAbr.load(std::memory_order_relaxed);
				c.latch.releasesReset = cLatchRelReset.load(std::memory_order_relaxed);
				c.latch.heldBack = cLatchHeldBack.load(std::memory_order_relaxed);
				c.latch.nonConventional = cLatchNonConv.load(std::memory_order_relaxed);
				c.latch.confirms = cLatchConfirms.load(std::memory_order_relaxed);
				c.latch.releasesHardBound = cLatchRelHard.load(std::memory_order_relaxed);
				c.latch.deferred = cLatchDeferred.load(std::memory_order_relaxed);
				c.latch.deferCancelled = cLatchDeferCancelled.load(std::memory_order_relaxed);
				c.latch.releasesAtCut = cLatchRelAtCut.load(std::memory_order_relaxed);
				c.latch.releasesCeiling = cLatchRelCeiling.load(std::memory_order_relaxed);
				c.fadeShots = cFadeShots.load(std::memory_order_relaxed);
				{
					std::lock_guard<std::mutex> lk(s2CounterMutex);
					c.jointLatch = s2JointCounters;
					c.stepMark = s2StepCounters;
				}
				c.r1HarmonicSelections = cR1HarmonicSelections.load(std::memory_order_relaxed);
				c.p3ConventionProposals = cP3ConvProposals.load(std::memory_order_relaxed);
				c.empiricalFloorEvals = cEmpiricalFloorEvals.load(std::memory_order_relaxed);
				c.empiricalFloorFallbacks = cEmpiricalFloorFallbacks.load(std::memory_order_relaxed);
				c.empiricalFloorAllAxisFallbacks = cEmpiricalFloorAllAxisFallbacks.load(std::memory_order_relaxed);
				c.empiricalFloorPartialAxisFallbacks = cEmpiricalFloorPartialAxisFallbacks.load(std::memory_order_relaxed);
				c.placeholderNullTableEvals = cPlaceholderNullTableEvals.load(std::memory_order_relaxed);
				c.depthNullTableFallbacks = cDepthNullTableFallbacks.load(std::memory_order_relaxed);
				c.classicalFreshStaleDrops = cClassicalFreshStaleDrops.load(std::memory_order_relaxed);
				c.p3ScreenOnlyCandidates = cP3ScreenOnlyCandidates.load(std::memory_order_relaxed);
				c.edgeStatsNullTableAbstains = cEdgeStatsNullTableAbstains.load(std::memory_order_relaxed);
				c.abrStepEventsEvicted = cAbrStepEventsEvicted.load(std::memory_order_relaxed);
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
		void ChainEstimator::SetClassicalFresh(const ClassicalFreshInput& in)
		{
			st_->SetClassicalFresh(in);
		}
		void ChainEstimator::SetClassicalFresh(bool valid, double ratioH, double ratioV, bool detailAbove, bool mixed, bool subtitle, bool inFade)
		{
			ClassicalFreshInput in;
			in.valid = valid;
			in.ratioH = ratioH;
			in.ratioV = ratioV;
			in.detailAbove = detailAbove;
			in.mixed = mixed;
			in.subtitle = subtitle;
			in.inFade = inFade;
			st_->SetClassicalFresh(in);
		}
		void ChainEstimator::NoteAbrStepEvent(int64_t t0Us, double ratioStep, uint64_t seq) { st_->NoteAbrStepEvent(t0Us, ratioStep, seq); }
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
		void ChainEstimator::SetSafetyRules(bool on) { st_->SetSafety(on); }
		bool ChainEstimator::SafetyRules() const { return st_->safetyA.load(std::memory_order_acquire); }
		void ChainEstimator::SetLatchAbrStepTrigger(bool on) { st_->SetAbr(on); }
		void ChainEstimator::SetJointLatchSettings(const JointLatchSettings& s) { st_->SetJointLatch(s); }
		JointLatchSettings ChainEstimator::GetJointLatchSettings() const { return st_->GetJointLatch(); }
		void ChainEstimator::SetStepMarkSettings(const StepMarkSettings& s) { st_->SetStepMark(s); }
		StepMarkSettings ChainEstimator::GetStepMarkSettings() const { return st_->GetStepMark(); }
		void ChainEstimator::SetConventionDrivenProposals(bool on) { st_->SetConvProposals(on); }
		bool ChainEstimator::ConventionDrivenProposals() const { return st_->GetConvProposals(); }
		void ChainEstimator::SetEdgeStatsSettings(const EdgeStatsSettings& s, bool empiricalFloor) { st_->SetEdgeStats(s, empiricalFloor); }
		EdgeStatsSettings ChainEstimator::GetEdgeStatsSettings() const { return st_->GetEdgeStats(); }
		void ChainEstimator::SetEmpiricalFloorMode(EmpiricalFloorMode m) { st_->SetEmpiricalFloorMode(m); }
		void ChainEstimator::SetEightBitPromotion(bool on) { st_->SetEightBitPromotion(on); }
		void ChainEstimator::SetDepthNullTables(const DepthNullTableSet& tables, bool enable) { st_->SetDepthNullTables(tables, enable); }
		void ChainEstimator::SetDiagnosticTag(const std::string& tag) { st_->diagTag.Set(tag); }
		void ChainEstimator::SetDiagnosticDepths(int codedBits, int outputBits)
		{
			st_->diagCodedBits.store(codedBits, std::memory_order_release);
			st_->diagOutputBits.store(outputBits, std::memory_order_release);
		}
		uint64_t ChainEstimator::LatchCeilingReleases() const { return st_->cLatchRelCeiling.load(std::memory_order_acquire); }
		uint64_t ChainEstimator::LatchCeilingReleasesThrough(uint64_t sourceSequence) const { return st_->CeilingReleasesThrough(sourceSequence); }
		void ChainEstimator::NoteLatchCeilingForTest()
		{
			const uint64_t cumulative = st_->cLatchRelCeiling.fetch_add(1, std::memory_order_acq_rel) + 1;
			st_->LogCeilingRelease(cumulative);
		}
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
		double ChainEstimator::AcquiringIntervalSec() const { return st_->acquiringIntervalMs.load(std::memory_order_acquire) / 1000.0; }
		bool ChainEstimator::SubmitLines(const LineSnapshot& snapshot) { return st_->SubmitSnapshot(snapshot); }
	}
}
