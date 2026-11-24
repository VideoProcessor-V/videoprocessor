// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Temporal Detail v2 — Stage TD-S2 tests (2026-11-23_TD-consolidated-plan.md s3.2):
// Adversarial verification of picture-only P15/P15c frame-role detector (64-byte QualityPattern::Record)
// and visibly labelled P16 block-grid stand-in:
//   - Synthetic GOP cadence (hierarchical + flat-bottom I-B-B-P)
//   - 3:2 pulldown exact-copy protection (never promoted to Peak)
//   - Cut/fade reset & visible abstention
//   - Causal short-queue provisional mode
//   - Strict no-duplicate seq emission across warmup, CutReset, and double flush
//   - Flat-bottom I-B-B-P stability under tiny noise jitter (with naive MAD-only negative control!)
//   - Invalid sourceBits fail-closed with AbstainReason::NonFiniteEvidence
//   - P16 stand-in boundary step detection, exact-copy flags, non-finite fail-closed, and hit counter. CPU only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/repair/temporal_detail/v2/TdV2FrameRole.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace TdV2FrameRoleNs
	{
		using namespace TdV2;
		using QualityPattern::FrameRole;
		using QualityPattern::PeakValley;

		static FrameRoleEvidence MakeEv(uint64_t seq, double score, double stepMean = 1.0,
		                                double copy1 = 0.0, double copy2 = 0.0,
		                                bool isCut = false, bool isFade = false, int sourceBits = 10)
		{
			FrameRoleEvidence ev;
			ev.seq = seq;
			ev.qualityScore = score;
			ev.stepMean = stepMean;
			ev.stepP90 = stepMean * 1.5;
			ev.stepMax = stepMean * 2.0;
			ev.periodX = 8;
			ev.periodY = 8;
			ev.exactCopyFracPrev1 = copy1;
			ev.exactCopyFracPrev2 = copy2;
			ev.isCut = isCut;
			ev.isFade = isFade;
			ev.sourceBits = sourceBits;
			return ev;
		}

		TEST_CLASS(TdV2FrameRoleTests)
		{
		public:
			TEST_METHOD(SyntheticGopDetectsPeaksAndValleysWithLead2)
			{
				// Hierarchical GOP: I(0.96), refB(0.76), B(0.50), refB(0.76), P(0.92), refB(0.74), B(0.48), refB(0.74), P(0.90)
				const double scores[] = { 0.96, 0.76, 0.50, 0.76, 0.92, 0.74, 0.48, 0.74, 0.90 };
				const size_t n = sizeof(scores) / sizeof(scores[0]);

				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;
				std::vector<FrameRoleDecision> decisions;
				for (size_t i = 0; i < n; ++i)
				{
					const auto dec = det.PushAndClassify(MakeEv(100 + i, scores[i], (1.0 - scores[i]) * 8.0), &reg);
					if (i >= 2)
						decisions.push_back(dec);
				}

				// decisions[k] corresponds to input index k (seq 100 + k), for k = 0..6.
				Assert::AreEqual(static_cast<size_t>(7), decisions.size());
				// Verify exact 64-byte QualityPattern::Record header & non-provisional P15 flags.
				Assert::AreEqual(QualityPattern::kUnitVersion, decisions[4].record.unit);
				Assert::AreEqual(static_cast<uint8_t>(2), decisions[4].record.leadUsed);
				Assert::AreEqual(static_cast<uint8_t>(0), decisions[4].record.causal);
				Assert::IsTrue((decisions[4].record.validityBits & QualityPattern::kProvisional) == 0);
				Assert::IsTrue((decisions[4].record.validityBits & QualityPattern::kValidRole) != 0);

				// k = 0 (I=0.96) -> Peak; k = 1 (refB=0.76) -> Reference; k = 2 (B=0.50) -> NonReference/Valley;
				// k = 3 (refB=0.76) -> Reference; k = 4 (P=0.92) -> Peak; k = 6 (B=0.48) -> NonReference/Valley.
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Peak), decisions[0].record.role);
				Assert::AreEqual(static_cast<uint8_t>(PeakValley::Peak), decisions[0].record.peakValley);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Reference), decisions[1].record.role);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), decisions[2].record.role);
				Assert::AreEqual(static_cast<uint8_t>(PeakValley::Valley), decisions[2].record.peakValley);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Reference), decisions[3].record.role);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Peak), decisions[4].record.role);
				Assert::AreEqual(static_cast<uint8_t>(PeakValley::Peak), decisions[4].record.peakValley);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), decisions[6].record.role);
				Assert::AreEqual(static_cast<uint8_t>(PeakValley::Valley), decisions[6].record.peakValley);

				// Also verify flat-bottom I-B-B-P cadence classifies both B-frames as valleys and P as peak.
				TdV2FrameRoleDetector detIbbp(2);
				const double ibbp[] = { 0.95, 0.55, 0.55, 0.88, 0.55, 0.55, 0.88 };
				std::vector<FrameRoleDecision> dIbbp;
				for (size_t i = 0; i < 7; ++i)
				{
					const auto dec = detIbbp.PushAndClassify(MakeEv(200 + i, ibbp[i]), &reg);
					if (i >= 2) dIbbp.push_back(dec);
				}
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), dIbbp[1].record.role);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), dIbbp[2].record.role);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Peak), dIbbp[3].record.role);
				Assert::IsTrue(reg.IsActive(StandInKind::P15FramePattern));
			}

			TEST_METHOD(CadenceRepeatsNeverPromotedToPeaks)
			{
				// 3:2 pulldown style stream: high-score peak followed by exact duplicate, then valley + duplicate, then peak.
				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;

				const FrameRoleEvidence seq[] = {
					MakeEv(1, 0.95, 0.5, 0.0, 0.0),   // unique Peak
					MakeEv(2, 0.95, 0.5, 1.0, 0.0),   // exact copy of seq 1 -> MUST be ExactCopy, NEVER Peak!
					MakeEv(3, 0.50, 4.0, 0.0, 0.0),   // unique Valley
					MakeEv(4, 0.50, 4.0, 1.0, 0.0),   // exact copy of seq 3 -> MUST be ExactCopy
					MakeEv(5, 0.90, 0.8, 0.0, 0.0),   // unique Peak
					MakeEv(6, 0.90, 0.8, 1.0, 0.0),   // exact copy of seq 5
					MakeEv(7, 0.52, 3.8, 0.0, 0.0)    // unique Valley
				};

				std::vector<FrameRoleDecision> out;
				for (size_t i = 0; i < 7; ++i)
				{
					const auto dec = det.PushAndClassify(seq[i], &reg);
					if (i >= 2)
						out.push_back(dec);
				}

				// out[1] is seq 2 (duplicate of high-score Peak): MUST be ExactCopy with kCopyOfPrev1!
				Assert::AreEqual(static_cast<uint64_t>(2), out[1].record.seq);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::ExactCopy), out[1].record.role,
					L"Cadence repeat of a peak frame must NEVER be promoted to Peak");
				Assert::IsTrue((out[1].record.exactCopyFlags & QualityPattern::kCopyOfPrev1) != 0);

				// out[2] is seq 3 (unique valley) and out[4] is seq 5 (unique peak), unaffected by intermediate duplicates!
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), out[2].record.role);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::ExactCopy), out[3].record.role);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Peak), out[4].record.role);
			}

			TEST_METHOD(SceneCutAndFadeResetWindowAndAbstainVisibly)
			{
				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;

				// Feed pre-cut sequence.
				det.PushAndClassify(MakeEv(1, 0.95), &reg);
				det.PushAndClassify(MakeEv(2, 0.55), &reg);
				det.PushAndClassify(MakeEv(3, 0.90), &reg);
				Assert::AreEqual(static_cast<size_t>(3), det.WindowSize());

				// Cut arrives at seq 4 -> flushes window to size 1 and abstains with CutReset.
				const auto cutDec = det.PushAndClassify(MakeEv(4, 0.92, 0.5, 0.0, 0.0, true, false), &reg);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::CutReset), static_cast<uint8_t>(cutDec.abstain));
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Unknown), cutDec.record.role);
				Assert::IsFalse(cutDec.produced, L"Lead-2 cut frame must buffer cleanly without premature emission");
				Assert::AreEqual(static_cast<size_t>(1), det.WindowSize(), L"Cut must flush pre-cut history");
				Assert::AreEqual(static_cast<uint64_t>(1), det.AbstainCount(AbstainReason::CutReset));

				// Next frame at seq 5 still has insufficient post-cut window (lead=2 needs 3 frames).
				const auto post1 = det.PushAndClassify(MakeEv(5, 0.55), &reg);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::InsufficientWindow), static_cast<uint8_t>(post1.abstain));
				Assert::IsFalse(post1.produced, L"Post-cut warmup must not prematurely emit");

				// Fade frame at seq 6 (classified when seq 8 arrives) -> abstains visibly with FadeActive.
				det.PushAndClassify(MakeEv(6, 0.70, 2.0, 0.0, 0.0, false, true), &reg);
				det.PushAndClassify(MakeEv(7, 0.65), &reg);
				const auto fadeDec = det.PushAndClassify(MakeEv(8, 0.60), &reg);
				Assert::AreEqual(static_cast<uint64_t>(6), fadeDec.record.seq);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::FadeActive), static_cast<uint8_t>(fadeDec.abstain));
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Unknown), fadeDec.record.role);
				Assert::AreEqual(static_cast<uint64_t>(1), det.AbstainCount(AbstainReason::FadeActive));
			}

			TEST_METHOD(NoDuplicateSeqEmittedAcrossWarmupCutResetAndFlush)
			{
				// H10 Adversarial EXACTLY-ONCE Delivery Proof (Rule 3 & Rule 4):
				// Every unique pushed seq (1..14) MUST be emitted with produced == true EXACTLY ONCE
				// (zero duplicates AND zero silently dropped pre-cut or stream-end sequences 4, 5, 9, 10, 13!)
				// across initial warmup, steady-state GOP, multiple CutResets (flushedBeforeCut),
				// post-cut warmups, and FlushEndOfStream(), with DroppedRecordCount() == 0!
				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;
				std::map<uint64_t, int> emitCount;
				std::map<uint64_t, int> naiveNoFlushEmitCount;

				auto recordEmit = [&](const FrameRoleDecision& d)
				{
					if (d.produced)
						++emitCount[d.record.seq];
				};

				const FrameRoleEvidence stream[] = {
					MakeEv(1,  0.95),                         // warmup 1 -> produced == false
					MakeEv(2,  0.55),                         // warmup 2 -> produced == false
					MakeEv(3,  0.90),                         // emits seq 1
					MakeEv(4,  0.52),                         // emits seq 2
					MakeEv(5,  0.91),                         // emits seq 3  (buffered: 4, 5)
					MakeEv(6,  0.94, 0.5, 0.0, 0.0, true),    // CutReset #1 -> flushedBeforeCut emits 4, 5; seq 6 buffered
					MakeEv(7,  0.54),                         // post-cut warmup -> produced == false
					MakeEv(8,  0.89),                         // emits seq 6 (post-cut anchor!)
					MakeEv(9,  0.53),                         // emits seq 7
					MakeEv(10, 0.90),                         // emits seq 8  (buffered: 9, 10)
					MakeEv(11, 0.93, 0.5, 0.0, 0.0, true),    // CutReset #2 -> flushedBeforeCut emits 9, 10; seq 11 buffered
					MakeEv(12, 0.51),                         // post-cut warmup -> produced == false
					MakeEv(13, 0.88),                         // emits seq 11
					MakeEv(14, 0.52)                          // emits seq 12 (buffered: 13, 14)
				};

				for (size_t i = 0; i < sizeof(stream) / sizeof(stream[0]); ++i)
				{
					const auto dec = det.PushAndClassify(stream[i], &reg);
					if (i == 0 || i == 1 || i == 5 || i == 6 || i == 10 || i == 11)
					{
						Assert::IsFalse(dec.produced,
							L"Warmup and lead-2 CutReset calls must keep produced == false to prevent duplicate seq");
					}
					if (i == 5 || i == 10)
					{
						Assert::AreEqual(static_cast<size_t>(2), dec.flushedBeforeCut.size(),
							L"CutReset with lead=2 must flush both pending pre-cut buffered frames in flushedBeforeCut");
						for (const auto& fl : dec.flushedBeforeCut)
						{
							Assert::IsTrue(fl.produced, L"Every flushedBeforeCut record must have produced == true");
							Assert::IsTrue((fl.record.validityBits & QualityPattern::kProvisional) != 0,
								L"Pre-cut flushed records with reduced lead must be visibly marked kProvisional");
							recordEmit(fl);
						}
					}
					else
					{
						Assert::IsTrue(dec.flushedBeforeCut.empty(),
							L"Non-cut frames must not populate flushedBeforeCut");
					}
					recordEmit(dec);
					if (dec.produced)
						++naiveNoFlushEmitCount[dec.record.seq];
				}

				// Stream end: FlushEndOfStream() must emit remaining buffered frames (13 and 14) as provisional,
				// and a second call must return empty (zero duplicates)!
				const auto eosFlushed = det.FlushEndOfStream(&reg);
				Assert::AreEqual(static_cast<size_t>(2), eosFlushed.size(),
					L"FlushEndOfStream must flush both trailing buffered frames (13 and 14)");
				for (const auto& ef : eosFlushed)
				{
					Assert::IsTrue(ef.produced, L"FlushEndOfStream records must have produced == true");
					Assert::IsTrue((ef.record.validityBits & QualityPattern::kProvisional) != 0,
						L"End-of-stream flushed records must be visibly marked kProvisional");
					recordEmit(ef);
				}
				const auto eosFlushedAgain = det.FlushEndOfStream(&reg);
				Assert::IsTrue(eosFlushedAgain.empty(),
					L"Second repeated FlushEndOfStream must return empty with zero duplicate seqs");

				// Mandatory Same-Input Negative Control (Rule 3):
				// Prove that ignoring flushedBeforeCut and FlushEndOfStream (pre-H10 behavior) silently loses 5+ frames!
				const size_t naiveMissingCount = 14u - naiveNoFlushEmitCount.size();
				Assert::IsTrue(naiveMissingCount >= 5u,
					L"Negative control: without flushedBeforeCut + FlushEndOfStream, >= 5 frames (4,5,9,10,13,14) are lost");
				Assert::AreEqual(0, naiveNoFlushEmitCount[4]);
				Assert::AreEqual(0, naiveNoFlushEmitCount[5]);
				Assert::AreEqual(0, naiveNoFlushEmitCount[9]);
				Assert::AreEqual(0, naiveNoFlushEmitCount[10]);
				Assert::AreEqual(0, naiveNoFlushEmitCount[13]);

				// STRICT EXACTLY-ONCE invariant across EVERY pushed sequence 1..14!
				Assert::AreEqual(static_cast<size_t>(14), emitCount.size(),
					L"All 14 pushed sequences must be emitted with zero dropped records");
				for (uint64_t s = 1; s <= 14; ++s)
				{
					Assert::AreEqual(1, emitCount[s],
						L"Every pushed sequence (including pre-cut 4,5,9,10 and stream-end 13,14) MUST be emitted EXACTLY ONCE");
				}
				Assert::AreEqual(static_cast<uint64_t>(0), det.DroppedRecordCount(),
					L"Clean cut+EOS flush must record DroppedRecordCount() == 0");

				// Explicit Reset() drop-counter visibility test (Rule 4):
				// Calling Reset() while buffered un-emitted frames sit in window_ MUST increment DroppedRecordCount() visibly!
				{
					TdV2FrameRoleDetector detDrop(2);
					Assert::AreEqual(static_cast<uint64_t>(0), detDrop.DroppedRecordCount());
					detDrop.PushAndClassify(MakeEv(100, 0.95), &reg); // buffered
					detDrop.PushAndClassify(MakeEv(101, 0.55), &reg); // buffered
					Assert::AreEqual(static_cast<uint64_t>(0), detDrop.DroppedRecordCount());
					detDrop.Reset();
					Assert::AreEqual(static_cast<uint64_t>(2), detDrop.DroppedRecordCount(),
						L"Reset() with 2 buffered un-emitted frames MUST visibly increment DroppedRecordCount() by 2");

					// Push 4 frames: seqs 200 & 201 emitted, seqs 202 & 203 still buffered -> Reset() adds 2 more drops.
					detDrop.PushAndClassify(MakeEv(200, 0.95), &reg);
					detDrop.PushAndClassify(MakeEv(201, 0.55), &reg);
					detDrop.PushAndClassify(MakeEv(202, 0.90), &reg); // emits 200
					detDrop.PushAndClassify(MakeEv(203, 0.52), &reg); // emits 201
					detDrop.Reset();
					Assert::AreEqual(static_cast<uint64_t>(4), detDrop.DroppedRecordCount(),
						L"Second Reset() with 2 buffered un-emitted frames must accumulate DroppedRecordCount() == 4");
				}

				// Also verify lead == 0 (causal P15c) emits cut frame once, FlushEndOfStream is empty, and DroppedRecordCount == 0.
				TdV2FrameRoleDetector detCausal(0);
				std::map<uint64_t, int> causalEmit;
				const auto c0 = detCausal.PushAndClassify(MakeEv(50, 0.95, 0.5, 0.0, 0.0, true), &reg);
				Assert::IsTrue(c0.produced, L"Lead-0 CutReset emits immediately once");
				++causalEmit[c0.record.seq];
				const auto cFlush = detCausal.FlushEndOfStream(&reg);
				Assert::IsTrue(cFlush.empty(), L"Lead-0 FlushEndOfStream right after CutReset must not duplicate seq");
				Assert::AreEqual(1, causalEmit[50]);
				Assert::AreEqual(static_cast<uint64_t>(0), detCausal.DroppedRecordCount());

				// Direct ClassifyLatestCausal coverage (R4 L1): emits latest buffered seq provisionally once,
				// suppresses repeat duplicate, and lets FlushEndOfStream emit only the remaining un-emitted seq!
				{
					TdV2FrameRoleDetector detLatest(2);
					Assert::IsFalse(detLatest.PushAndClassify(MakeEv(101, 0.94), &reg).produced);
					Assert::IsFalse(detLatest.PushAndClassify(MakeEv(102, 0.52), &reg).produced);
					const auto lc1 = detLatest.ClassifyLatestCausal(&reg);
					Assert::IsTrue(lc1.produced, L"ClassifyLatestCausal must emit latest buffered frame once");
					Assert::IsTrue((lc1.record.validityBits & QualityPattern::kProvisional) != 0,
						L"ClassifyLatestCausal must set kProvisional visibly");
					Assert::AreEqual(static_cast<uint64_t>(102), lc1.record.seq);
					Assert::IsFalse(detLatest.ClassifyLatestCausal(&reg).produced,
						L"Repeated ClassifyLatestCausal must not duplicate already-emitted seq 102");
					const auto lcEos = detLatest.FlushEndOfStream(&reg);
					Assert::AreEqual(static_cast<size_t>(1), lcEos.size(),
						L"FlushEndOfStream after ClassifyLatestCausal must emit only remaining seq 101");
					Assert::IsTrue(lcEos[0].produced);
					Assert::AreEqual(static_cast<uint64_t>(101), lcEos[0].record.seq);
					Assert::IsFalse(detLatest.ClassifyLatestCausal(&reg).produced,
						L"ClassifyLatestCausal after FlushEndOfStream must return produced == false");
					Assert::AreEqual(static_cast<uint64_t>(0), detLatest.DroppedRecordCount());
				}
			}

			TEST_METHOD(FlatBottomIbbpStabilityUnderTinyNoiseJitter)
			{
				// Flat-bottom I-B-B-P sequence where 2/3 of frames are B-frames (~0.55), so exact MAD == 0!
				// Add tiny picture-score jitter +/-0.002 (exceeding 10-bit quantScoreFloor = 0.5/1023 ~= 0.00049,
				// yet tiny relative to the 0.35 I-vs-B contrast).
				const double jitteredIbbp[] = {
					0.940, 0.552, 0.548, 0.900,   // I, B+, B-, P
					0.548, 0.552, 0.900,          // B-, B+, P
					0.551, 0.549, 0.905,          // B+, B-, P
					0.549, 0.551, 0.900           // B-, B+, P
				};
				const size_t n = sizeof(jitteredIbbp) / sizeof(jitteredIbbp[0]);

				// Mandatory In-Test Negative Control (Rule 3):
				// Prove that on the exact same bimodal flat-bottom I-B-B-P window, a naive MAD-only margin
				// collapses below the 0.004 jitter gap between B+ (0.552) and B- (0.548), causing a naive
				// MAD-only detector to FALSELY flip B+ out of Valley!
				{
					std::vector<double> w0(jitteredIbbp, jitteredIbbp + 5); // {0.94, 0.552, 0.548, 0.90, 0.548}
					const double med = RobustStats::Median(w0, RobustStats::MedianRule::MeanOfMiddle);
					const double mad = RobustStats::MadAbout(w0, med, RobustStats::MedianRule::MeanOfMiddle);
					const double naiveMargin = (std::max)(RobustStats::kMadToSigma * mad / 4.0, 0.5 / 1023.0);
					// For target B+ = 0.552 with neighbors I = 0.940 and B- = 0.548:
					// Flat-bottom valley requires tScore <= nextUniqueScore + margin (0.552 <= 0.548 + margin).
					// Prove naiveMargin < (0.552 - 0.548) = 0.004 -> naive classifier FAILS and drops B+ from Valley!
					const double bGap = 0.552 - 0.548;
					Assert::IsTrue(naiveMargin < bGap,
						L"Negative control: naive MAD-only margin must collapse below B+/B- jitter gap on flat-bottom I-B-B-P!");
				}

				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;
				std::vector<FrameRoleDecision> decs;
				for (size_t i = 0; i < n; ++i)
				{
					const auto dec = det.PushAndClassify(MakeEv(300 + i, jitteredIbbp[i]), &reg);
					if (dec.produced)
						decs.push_back(dec);
				}

				Assert::AreEqual(n - 2, decs.size());
				// Indices 0, 3, 6, 9 are I/P peaks; indices 1,2, 4,5, 7,8, 10 are ALL B-frame valleys!
				for (size_t k = 0; k < decs.size(); ++k)
				{
					const bool expectPeak = (k % 3 == 0);
					if (expectPeak)
					{
						Assert::AreEqual(static_cast<uint8_t>(FrameRole::Peak), decs[k].record.role,
							L"I/P frame must remain Peak under tiny B-frame jitter");
						Assert::AreEqual(static_cast<uint8_t>(PeakValley::Peak), decs[k].record.peakValley);
					}
					else
					{
						Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), decs[k].record.role,
							L"Flat-bottom I-B-B-P B-frame must NOT spuriously flip role under tiny noise jitter");
						Assert::AreEqual(static_cast<uint8_t>(PeakValley::Valley), decs[k].record.peakValley);
					}
				}
			}

			TEST_METHOD(InvalidSourceBitsFailsClosedWithNonFiniteEvidence)
			{
				const int badBits[] = { 0, -8, 25, 100 };
				for (uint8_t lead : { static_cast<uint8_t>(2), static_cast<uint8_t>(0) })
				{
					TdV2FrameRoleDetector det(lead);
					StandInRegistry reg;
					uint64_t expectedAbstains = 0;
					for (size_t i = 0; i < sizeof(badBits) / sizeof(badBits[0]); ++i)
					{
						const auto dec = det.PushAndClassify(
							MakeEv(400 + i, 0.85, 1.0, 0.0, 0.0, false, false, badBits[i]), &reg);
						++expectedAbstains;
						Assert::AreEqual(static_cast<uint8_t>(AbstainReason::NonFiniteEvidence),
							static_cast<uint8_t>(dec.abstain),
							L"Invalid sourceBits must fail closed with AbstainReason::NonFiniteEvidence");
						Assert::AreEqual(static_cast<uint8_t>(FrameRole::Unknown), dec.record.role);
						Assert::AreEqual(expectedAbstains, det.AbstainCount(AbstainReason::NonFiniteEvidence));
						Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));
					}
				}
			}

			TEST_METHOD(CausalShortQueuePublishesProvisionalVisibly)
			{
				// Causal P15c mode (lead = 0): emits immediately on each frame with causal=1 and kProvisional set!
				TdV2FrameRoleDetector det(0);
				StandInRegistry reg;

				const auto d0 = det.PushAndClassify(MakeEv(10, 0.90), &reg);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::InsufficientWindow), static_cast<uint8_t>(d0.abstain));
				Assert::AreEqual(static_cast<uint8_t>(1), d0.record.causal);
				Assert::IsTrue((d0.record.validityBits & QualityPattern::kProvisional) != 0);

				const auto d1 = det.PushAndClassify(MakeEv(11, 0.50), &reg);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::None), static_cast<uint8_t>(d1.abstain));
				Assert::AreEqual(static_cast<uint8_t>(0), d1.record.leadUsed);
				Assert::AreEqual(static_cast<uint8_t>(1), d1.record.causal);
				Assert::IsTrue((d1.record.validityBits & QualityPattern::kProvisional) != 0, L"P15c must set kProvisional visibly");
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::NonReference), d1.record.role);

				const auto d2 = det.PushAndClassify(MakeEv(12, 0.94), &reg);
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Peak), d2.record.role);
				Assert::IsTrue((d2.record.validityBits & QualityPattern::kProvisional) != 0);
			}

			TEST_METHOD(FlatQualityAndNonFiniteAbstainVisibly)
			{
				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;
				// Perfectly constant quality across frames -> no coding rhythm -> LowRhythmContrast abstention.
				det.PushAndClassify(MakeEv(1, 0.80), &reg);
				det.PushAndClassify(MakeEv(2, 0.80), &reg);
				const auto flatDec = det.PushAndClassify(MakeEv(3, 0.80), &reg);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::LowRhythmContrast), static_cast<uint8_t>(flatDec.abstain));
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Unknown), flatDec.record.role);
				Assert::AreEqual(static_cast<uint64_t>(1), det.AbstainCount(AbstainReason::LowRhythmContrast));

				// Non-finite score -> NonFiniteEvidence abstention + visible NonFiniteClosure stand-in token.
				const auto nanDec = det.PushAndClassify(MakeEv(4, std::numeric_limits<double>::quiet_NaN()), &reg);
				Assert::AreEqual(static_cast<uint8_t>(AbstainReason::NonFiniteEvidence), static_cast<uint8_t>(nanDec.abstain));
				Assert::AreEqual(static_cast<uint8_t>(FrameRole::Unknown), nanDec.record.role);
				Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));
			}

			TEST_METHOD(P16StandInDetectsExactCopiesBoundaryStepsAndIncrementsStandInCounter)
			{
				const int w = 64, h = 64;
				const size_t n = static_cast<size_t>(w) * h;
				std::vector<double> smooth(n), blocky(n), copyOfBlocky(n);

				for (int y = 0; y < h; ++y)
				{
					for (int x = 0; x < w; ++x)
					{
						const size_t i = static_cast<size_t>(y) * w + x;
						// Smooth linear ramp: identical interior and boundary gradients -> zero excess step.
						smooth[i] = 200.0 + 1.0 * x + 1.0 * y;
						// Blocky 8x8 step quantization: flat inside each 8x8 tile, 12-code jump at every 8-px edge!
						blocky[i] = 200.0 + 12.0 * (x / 8) + 12.0 * (y / 8);
						copyOfBlocky[i] = blocky[i] + 0.1; // within 0.5-code rounding floor of 10-bit exact copy
					}
				}

				TdV2CodingGridStandIn p16;
				StandInRegistry reg;

				const auto sSmooth = p16.Measure(smooth.data(), nullptr, nullptr, w, h, 10, &reg);
				Assert::IsTrue(sSmooth.valid);
				Assert::AreEqual(4, sSmooth.cellsW);
				Assert::AreEqual(4, sSmooth.cellsH);
				Assert::AreEqual(0.0, sSmooth.stepMean, 1e-9, L"Smooth ramp has zero excess 8-px boundary step");
				Assert::AreEqual(1.0, sSmooth.pictureScore, 1e-9);

				const auto sBlocky = p16.Measure(blocky.data(), smooth.data(), nullptr, w, h, 10, &reg);
				Assert::IsTrue(sBlocky.valid);
				Assert::IsTrue(sBlocky.stepMean > 10.0, L"8x8 blocky picture must register strong boundary step");
				Assert::IsTrue(sBlocky.pictureScore < sSmooth.pictureScore, L"Blocky frame must score strictly below smooth frame");
				Assert::AreEqual(static_cast<uint16_t>(8), sBlocky.detectedPeriodX);
				Assert::AreEqual(static_cast<uint16_t>(8), sBlocky.detectedPeriodY);
				Assert::AreEqual(0.0, sBlocky.exactCopyFracPrev1, 1e-9);

				// Measure exact copy of N-1 and N-2.
				const auto sCopy = p16.Measure(copyOfBlocky.data(), blocky.data(), blocky.data(), w, h, 10, &reg);
				Assert::IsTrue(sCopy.valid);
				Assert::AreEqual(1.0, sCopy.exactCopyFracPrev1, 1e-9, L"All cells match N-1 within 0.5-code floor");
				Assert::AreEqual(1.0, sCopy.exactCopyFracPrev2, 1e-9, L"All cells match N-2 within 0.5-code floor");

				// Non-finite pixel in P16 stand-in must fail closed visibly.
				std::vector<double> nanPlane = smooth;
				nanPlane[123] = std::numeric_limits<double>::quiet_NaN();
				const auto sNan = p16.Measure(nanPlane.data(), smooth.data(), nullptr, w, h, 10, &reg);
				Assert::IsFalse(sNan.valid, L"P16 stand-in must fail closed on NaN pixel");
				Assert::IsTrue(reg.IsActive(StandInKind::NonFiniteClosure));

				// Verify P16 stand-in visibility & exact hit counter (4 Measure calls).
				Assert::IsTrue(reg.IsActive(StandInKind::P16BlockGrid));
				Assert::AreEqual(static_cast<uint64_t>(4), reg.HitCount(StandInKind::P16BlockGrid));
				Assert::IsTrue(reg.FormatStatusSummary().find("P16 grid: TD stand-in") != std::string::npos);
			}

			TEST_METHOD(P16StandInPureWhiteNoiseHasZeroFalseBlockingPeriod)
			{
				// Full-Diff MEDIUM-5 Adversarial Test:
				// Pure unblocked iid white Gaussian noise with sigma = 0.45 * depthScale puts E[|d|] = 0.5078 * depthScale
				// right at copyTol = 0.5 * depthScale. Under an asymmetric activity gate (bx > copyTol), left-tail
				// truncation biases meanBx/meanIx to ~1.14 and falsely triggers detectedPeriod = 8 on unblocked noise!
				// Verify TdV2CodingGridStandIn produces detectedPeriodX == 0 && detectedPeriodY == 0 at 10-bit & 12-bit,
				// while a genuine 8x8 blocked positive control on the exact same noise still detects period == 8.
				const int w = 128, h = 128;
				const size_t n = static_cast<size_t>(w) * h;
				const int depths[2] = { 10, 12 };
				const uint32_t seeds[2] = { 701u, 702u };

				TdV2CodingGridStandIn p16;
				StandInRegistry reg;

				for (int di = 0; di < 2; ++di)
				{
					const int bits = depths[di];
					const double depthScale = DepthScale(bits);
					const double copyTol = 0.5 * depthScale;
					const double sigma = 0.45 * depthScale;
					const double mid = 512.0 * depthScale;

					std::mt19937 rng(seeds[di]);
					std::normal_distribution<double> nd(0.0, sigma);
					std::vector<double> whiteNoise(n), blockedNoise(n);
					for (int y = 0; y < h; ++y)
					{
						for (int x = 0; x < w; ++x)
						{
							const size_t i = static_cast<size_t>(y) * w + x;
							const double g = nd(rng);
							whiteNoise[i] = mid + g;
							// Positive control: identical noise + genuine 8x8 block quantization steps (6 * depthScale).
							const double blockStep = 6.0 * depthScale * static_cast<double>(((x / 8) + (y / 8)) & 1);
							blockedNoise[i] = mid + g + blockStep;
						}
					}

					// Mandatory In-Test Negative Control (Rule 3): naive asymmetric gate (bx > copyTol) on EXACT SAME whiteNoise!
					double naiveBxSum = 0.0, naiveIxSum = 0.0;
					size_t naiveBxCnt = 0, naiveIxCnt = 0;
					for (int cy = 0; cy < h / 16; ++cy)
					{
						for (int cx = 0; cx < w / 16; ++cx)
						{
							double bSum = 0.0, iSum = 0.0;
							size_t bCnt = 0, iCnt = 0;
							for (int y = cy * 16; y < (cy + 1) * 16; ++y)
							{
								for (int x = cx * 16; x < (cx + 1) * 16; ++x)
								{
									if (x + 1 < w)
									{
										const double dx = std::fabs(whiteNoise[static_cast<size_t>(y) * w + x + 1]
										                          - whiteNoise[static_cast<size_t>(y) * w + x]);
										if ((x % 8) == 7) { bSum += dx; ++bCnt; }
										else              { iSum += dx; ++iCnt; }
									}
								}
							}
							if (bCnt > 0 && iCnt > 0 && (bSum / static_cast<double>(bCnt)) > copyTol)
							{
								naiveBxSum += bSum; naiveBxCnt += bCnt;
								naiveIxSum += iSum; naiveIxCnt += iCnt;
							}
						}
					}
					const double naiveRatio = (naiveBxSum / static_cast<double>(naiveBxCnt))
					                        / (naiveIxSum / static_cast<double>(naiveIxCnt));
					const double naiveMargin = kBlockingRatioSigma * std::sqrt(
						kHalfNormalCvSq * (1.0 / static_cast<double>(naiveBxCnt) + 1.0 / static_cast<double>(naiveIxCnt)));
					Assert::IsTrue(naiveRatio > 1.0 + naiveMargin,
						L"Negative control: asymmetric bx > copyTol gate MUST suffer left-tail bias > 1 + 3*sigma on sigma=0.45*depthScale white noise");

					// Protected symmetric-gated P16 stand-in MUST report ZERO false blocking period on pure white noise!
					const auto sNoise = p16.Measure(whiteNoise.data(), nullptr, nullptr, w, h, bits, &reg);
					Assert::IsTrue(sNoise.valid, L"P16 stand-in must return valid summary on pure white noise");
					Assert::AreEqual(static_cast<uint16_t>(0), sNoise.detectedPeriodX,
						L"Pure unblocked white noise (sigma=0.45*depthScale) MUST report detectedPeriodX == 0");
					Assert::AreEqual(static_cast<uint16_t>(0), sNoise.detectedPeriodY,
						L"Pure unblocked white noise (sigma=0.45*depthScale) MUST report detectedPeriodY == 0");

					// Positive control: genuine 8x8 blocked version of the exact same noise MUST still detect period == 8!
					const auto sBlocked = p16.Measure(blockedNoise.data(), nullptr, nullptr, w, h, bits, &reg);
					Assert::IsTrue(sBlocked.valid);
					Assert::AreEqual(static_cast<uint16_t>(8), sBlocked.detectedPeriodX,
						L"Genuine 8x8 blocked positive control MUST detect detectedPeriodX == 8");
					Assert::AreEqual(static_cast<uint16_t>(8), sBlocked.detectedPeriodY,
						L"Genuine 8x8 blocked positive control MUST detect detectedPeriodY == 8");
				}
			}
		};
	}
}
