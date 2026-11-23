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
				// Adversarial sequence-uniqueness test:
				// Every unique seq MUST be emitted with produced == true AT MOST ONCE across initial warmup,
				// steady-state GOP, multiple CutResets, post-cut warmups, and repeated stream-end causal flushes!
				TdV2FrameRoleDetector det(2);
				StandInRegistry reg;
				std::map<uint64_t, int> emitCount;

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
					MakeEv(5,  0.91),                         // emits seq 3
					MakeEv(6,  0.94, 0.5, 0.0, 0.0, true),    // CutReset #1 -> buffered, produced == false
					MakeEv(7,  0.54),                         // post-cut warmup -> produced == false
					MakeEv(8,  0.89),                         // emits seq 6 (post-cut anchor!)
					MakeEv(9,  0.53),                         // emits seq 7
					MakeEv(10, 0.90),                         // emits seq 8
					MakeEv(11, 0.93, 0.5, 0.0, 0.0, true),    // CutReset #2 -> buffered, produced == false
					MakeEv(12, 0.51),                         // post-cut warmup -> produced == false
					MakeEv(13, 0.88),                         // emits seq 11
					MakeEv(14, 0.52)                          // emits seq 12
				};

				for (size_t i = 0; i < sizeof(stream) / sizeof(stream[0]); ++i)
				{
					const auto dec = det.PushAndClassify(stream[i], &reg);
					if (i == 0 || i == 1 || i == 5 || i == 6 || i == 10 || i == 11)
					{
						Assert::IsFalse(dec.produced,
							L"Warmup and lead-2 CutReset calls must keep produced == false to prevent duplicate seq");
					}
					recordEmit(dec);
				}

				// Calling ClassifyLatestCausal() twice in a row must emit seq 14 at most once!
				const auto flush1 = det.ClassifyLatestCausal(&reg);
				const auto flush2 = det.ClassifyLatestCausal(&reg);
				recordEmit(flush1);
				recordEmit(flush2);
				Assert::IsTrue(flush1.produced, L"First stream-end causal flush emits latest buffered frame");
				Assert::IsFalse(flush2.produced, L"Second repeated flush must NOT duplicate lastEmittedSeq");

				// Verify post-cut anchor seq 6 and seq 11 WERE emitted once (not lost!) and NO seq exceeded 1 emission.
				Assert::AreEqual(1, emitCount[6], L"Post-cut frame seq 6 must be emitted exactly once");
				Assert::AreEqual(1, emitCount[11], L"Post-cut frame seq 11 must be emitted exactly once");
				for (const auto& kv : emitCount)
				{
					Assert::AreEqual(1, kv.second, L"Every unique seq must be emitted with produced==true at most once");
				}

				// Also verify lead == 0 (causal P15c) emits cut frame once and never duplicates on subsequent flush.
				TdV2FrameRoleDetector detCausal(0);
				std::map<uint64_t, int> causalEmit;
				const auto c0 = detCausal.PushAndClassify(MakeEv(50, 0.95, 0.5, 0.0, 0.0, true), &reg);
				Assert::IsTrue(c0.produced, L"Lead-0 CutReset emits immediately once");
				++causalEmit[c0.record.seq];
				const auto cFlush = detCausal.ClassifyLatestCausal(&reg);
				Assert::IsFalse(cFlush.produced, L"Lead-0 flush right after CutReset must not duplicate seq");
				Assert::AreEqual(1, causalEmit[50]);
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
		};
	}
}
