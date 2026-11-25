// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// SK-MV hiccup gap seed. CPU: when GapSeed::Make seeds and when it must not (a resync, a seek, more than the gap, no pair, an ambiguous
// pair). GPU (WARP unless hardware): a synthetic pan with one dropped frame - the fill pair seeded from the last valid shared pair has an
// error no larger than the unseeded one - and an invalid seed leaves the result byte-identical to the unseeded estimate.
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/motion/estimation/GapSeed.h>
#include <VP-V/modules/motion/estimation/global_model/MatcherGlobalD1.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	TEST_CLASS(GapSeedTests)
	{
	public:
		TEST_METHOD(SeedsOnlyWhenTheStateMovedForTheGapAndThePairIsUsable)
		{
			using VpMotion::GapSeed;
			// A pair (9 -> 10) with 1.5 cells of horizontal motion per frame, usable (ambiguity < 0.9) both ways; the fill pair is 10 -> 12.
			float g[2][4] = { { 1.5f, 0.25f, 0.2f, 1.0f }, { -1.5f, -0.25f, 0.2f, 1.0f } };
			GapSeed s = GapSeed::Make(g, 10, 10, 12, 7, 8, true);
			Assert::IsTrue(s.valid);
			Assert::AreEqual(3.0, static_cast<double>(s.global[0][0]), 1e-6, L"scaled by the two periods the gap spans");
			Assert::AreEqual(-0.5, static_cast<double>(s.global[1][1]), 1e-6);
			Assert::AreEqual(0.5, static_cast<double>(s.global[0][2]), 1e-6);
			Assert::IsFalse(GapSeed::Make(nullptr, 10, 10, 12, 7, 8, true).valid, L"no shared pair");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 11, 7, 8, true).valid, L"not a gap pair");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 10 + GapSeed::kMaxPeriods + 1, 7, 8, true).valid, L"too long to be a hiccup");
			Assert::IsFalse(GapSeed::Make(g, 9, 10, 12, 7, 8, true).valid, L"the pair does not end at the gap's first frame");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 8, false).valid, L"the last cause was a resync / seek / format change, not the gap");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 9, true).valid, L"the epoch moved for more than the gap");
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 7, true).valid, L"the epoch did not move: not a gap");
			g[1][2] = 1.0f;
			Assert::IsFalse(GapSeed::Make(g, 10, 10, 12, 7, 8, true).valid, L"an ambiguous whole-frame motion is no prior");
			// The SAD-vs-zero rejection (the matcher's check): the seed must explain both directions far better than the zero vector.
			Assert::IsTrue(GapSeed::Beats(1000.0f, 10.0f, 20.0f), L"a seed that removes nearly all the zero mismatch holds");
			Assert::IsFalse(GapSeed::Beats(1000.0f, 900.0f, 10.0f), L"worse than zero in one direction: dropped");
			Assert::IsFalse(GapSeed::Beats(1000.0f, 1100.0f, 1100.0f), L"worse than zero: dropped");
			Assert::IsFalse(GapSeed::Beats(0.0f, 0.0f, 0.0f), L"identical frames: nothing to seed");
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(SeededFillPairIsNoWorseThanUnseededAndAnInvalidSeedIsByteIdentical)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SeededFillPairIsNoWorseThanUnseededAndAnInvalidSeedIsByteIdentical)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			pl_gpu gpu = fixture.Gpu();
			const int width = 768, height = 384, perFrame = 40, dropped = 2;   // frames 0 and 2: one frame dropped between
			const int truth = perFrame * dropped;
			auto frame = [&](int shift) {
				return MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
			};
			const DejudderFrame a = frame(0), b = frame(truth);
			DejudderGpuPlanes pa, pb;
			UploadDejudderFrame(gpu, a, pa);
			UploadDejudderFrame(gpu, b, pb);
			auto settings = [] {
				Dejudder::Settings s = DejudderTestSettings();
				s.globalPrior = true;
				s.estCandidates = true;
				return s;
			};
			auto error = [&](const DejudderField& f) {
				double sum = 0.0;
				long n = 0;
				for (int by = 2; by < f.blocksY - 2; ++by)
					for (int bx = 2; bx < f.blocksX - 2; ++bx)
					{
						// An untrusted block counts as fully wrong: a seed that fails to help must show.
						sum += f.trusted(bx, by) ? std::hypot(f.vx(bx, by) - truth, f.vy(bx, by)) : static_cast<double>(truth);
						++n;
					}
				return n ? sum / n : 1e9;
			};
			Dejudder::MotionWarp unseeded(fixture.Log(), gpu, settings());
			Assert::IsTrue(unseeded.Estimate(pa.planes, pb.planes));
			const DejudderField fieldUnseeded = DownloadField(gpu, unseeded);
			// The shared pair before the gap: perFrame source px of motion, i.e. perFrame / 8 cells; the gap spans two periods.
			float g[2][4] = { { perFrame / 8.0f, 0.0f, 0.2f, 1.0f }, { -perFrame / 8.0f, 0.0f, 0.2f, 1.0f } };
			const VpMotion::GapSeed seed = VpMotion::GapSeed::Make(g, 10, 10, 10 + dropped, 7, 8, true);
			Assert::IsTrue(seed.valid);
			Dejudder::MotionWarp seeded(fixture.Log(), gpu, settings());
			seeded.SetSearchSeed(seed.global);
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes));
			Assert::IsTrue(seeded.SearchSeedUsed(), L"D1 started from the gap seed");
			const DejudderField fieldSeeded = DownloadField(gpu, seeded);
			Assert::IsTrue(error(fieldSeeded) <= error(fieldUnseeded) + 1e-6, L"seeded error <= unseeded error");
			// An invalid seed is never handed over: the estimate is the unseeded one, byte for byte; and a seed is consumed by one estimate.
			const VpMotion::GapSeed none = VpMotion::GapSeed::Make(g, 9, 10, 10 + dropped, 7, 8, true);
			Assert::IsFalse(none.valid);
			Dejudder::MotionWarp invalid(fixture.Log(), gpu, settings());
			Assert::IsTrue(invalid.Estimate(pa.planes, pb.planes));
			Assert::IsFalse(invalid.SearchSeedUsed());
			Assert::IsTrue(DownloadField(gpu, invalid).texels == fieldUnseeded.texels, L"no seed: byte-identical to today's estimate");
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes));
			Assert::IsFalse(seeded.SearchSeedUsed(), L"the seed was spent by the first estimate");
		}

		// A seed handed to the OUTER matcher reaches the instance that searches: at lowRes (a 4K-class input) the search runs in the half-size
		// inner, so the outer forwards the seed (like the pair computed ahead) and reports the inner's "used". The seed is in the searching
		// instance's own input pixels (half of the source's), as the shared pair's whole-frame motion is.
		BEGIN_TEST_METHOD_ATTRIBUTE(LowResSeedReachesTheInnerAndIsNoWorseThanUnseeded)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(LowResSeedReachesTheInnerAndIsNoWorseThanUnseeded)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			for (int all = 0; all < 2; ++all)   // dejudder_low_res on (the inner searches, the outer refines) and all (the inner makes the field)
			{
				const CaseResult r = RunCase(fixture, 2048, 1536, true, all == 1, kPreGapTruth);
				Assert::IsTrue(r.lowResEngaged, L"the 1536-line input halves for the search");
				Assert::IsTrue(r.seededUsed, L"D1 started from the gap seed in the inner (the outer reports it)");
				Assert::IsTrue(r.seeded.trustedError <= r.unseeded.trustedError + 1e-6, L"lowRes: seeded trusted-block error <= unseeded");
				Assert::IsTrue(r.seeded.untrustedShare <= r.unseeded.untrustedShare + 1e-9, L"lowRes: seeded untrusted share <= unseeded");
				Assert::IsFalse(r.spentSeedUsedAgain, L"the seed is spent by the one estimate (the inner too)");
			}
		}

		// A wrong seed (the motion CHANGED during the hiccup) must not make the fill pair worse than the unseeded estimate. Two bounds, kept
		// SEPARATE so a loss in one cannot hide behind the other: the mean error of the blocks the matcher TRUSTS (source pixels) may exceed the
		// unseeded one by at most kErrorEpsilonPx, and the share of interior blocks left untrusted by at most kShareEpsilon. Cases: the pan
		// reversed, stopped, and ran at half and at double the pre-gap speed. If any of this fails the seed has to be rejected, not shipped.
		BEGIN_TEST_METHOD_ATTRIBUTE(AWrongSeedIsNoWorseThanUnseededPlusEpsilon)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(AWrongSeedIsNoWorseThanUnseededPlusEpsilon)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo test device");
			for (const int truth : { -kPreGapTruth, 0, kPreGapTruth / 2, kPreGapTruth * 2 })
			{
				const CaseResult full = RunCase(fixture, 768, 384, false, false, truth);
				const std::wstring name = L"full resolution, true motion " + std::to_wstring(truth) + L" px: ";
				Assert::IsTrue(full.seeded.trustedError <= full.unseeded.trustedError + kErrorEpsilonPx, (name + L"trusted-block error").c_str());
				Assert::IsTrue(full.seeded.untrustedShare <= full.unseeded.untrustedShare + kShareEpsilon, (name + L"untrusted share").c_str());
			}
			for (const int truth : { -kPreGapTruth, 0 })
			{
				const CaseResult low = RunCase(fixture, 2048, 1536, true, false, truth);
				Assert::IsTrue(low.lowResEngaged);
				const std::wstring name = L"lowRes, true motion " + std::to_wstring(truth) + L" px: ";
				Assert::IsTrue(low.seeded.trustedError <= low.unseeded.trustedError + kErrorEpsilonPx, (name + L"trusted-block error").c_str());
				Assert::IsTrue(low.seeded.untrustedShare <= low.unseeded.untrustedShare + kShareEpsilon, (name + L"untrusted share").c_str());
			}
		}

	private:
		static constexpr int kPerFrame = 40, kDropped = 2, kPreGapTruth = kPerFrame * kDropped;   // the pre-gap pan, and what it would span over the gap
		static constexpr double kErrorEpsilonPx = 0.5, kShareEpsilon = 0.05;

		struct FieldScore
		{
			double trustedError = 1e9;     // mean |v - truth| over the trusted interior blocks (source px); 0 when none is trusted
			double untrustedShare = 1.0;   // share of the interior blocks that are not trusted
		};
		struct CaseResult
		{
			FieldScore unseeded, seeded;
			bool seededUsed = false, spentSeedUsedAgain = true, lowResEngaged = false;
		};

		static FieldScore Score(const DejudderField& f, int truth)
		{
			FieldScore score;
			double sum = 0.0;
			long trusted = 0, total = 0;
			for (int by = 2; by < f.blocksY - 2; ++by)
				for (int bx = 2; bx < f.blocksX - 2; ++bx)
				{
					++total;
					if (!f.trusted(bx, by))
						continue;
					sum += std::hypot(f.vx(bx, by) - truth, f.vy(bx, by));
					++trusted;
				}
			score.trustedError = trusted ? sum / trusted : 0.0;
			score.untrustedShare = total ? 1.0 - static_cast<double>(trusted) / total : 1.0;
			return score;
		}

		// One fill pair over a dropped frame (frames 0 and 2): the content moves `truth` source px between them (kPreGapTruth = the pan
		// continued as before the gap, the seed is right; anything else = the motion changed during the hiccup, the seed from the pre-gap pair
		// is wrong). Estimated unseeded, then seeded from the pre-gap motion.
		static CaseResult RunCase(TargetLutGpuFixture& fixture, int width, int height, bool lowRes, bool lowResAll, int truth)
		{
			pl_gpu gpu = fixture.Gpu();
			auto frame = [&](int shift) {
				return MakeDejudderFrame(width, height, [shift](int x, int y) { return DejudderTexture(x - shift, y); },
					[shift](int x, int y, int c) { return DejudderTexture((x - shift / 2) * 2, y, 7 + c); });
			};
			DejudderGpuPlanes pa, pb;
			UploadDejudderFrame(gpu, frame(0), pa);
			UploadDejudderFrame(gpu, frame(truth), pb);
			auto settings = [&] {
				Dejudder::Settings s = DejudderTestSettings();
				s.globalPrior = true;
				s.estCandidates = true;
				s.lowRes = lowRes;
				s.lowResAll = lowResAll;
				return s;
			};
			CaseResult r;
			Dejudder::MotionWarp unseeded(fixture.Log(), gpu, settings());
			Assert::IsTrue(unseeded.Estimate(pa.planes, pb.planes), std::wstring(unseeded.LastError().begin(), unseeded.LastError().end()).c_str());
			r.lowResEngaged = unseeded.SearchHalvings() > 0;
			r.unseeded = Score(DownloadField(gpu, unseeded), truth);
			// The shared pair before the gap: kPerFrame source px per frame (always +), in the searching instance's input pixels: half of the
			// source's at lowRes; D1 cells are 1/8 of those. The gap spans kDropped periods.
			const float cell = (lowRes ? 0.5f : 1.0f) * kPerFrame / 8.0f;
			float g[2][4] = { { cell, 0.0f, 0.2f, 1.0f }, { -cell, 0.0f, 0.2f, 1.0f } };
			const VpMotion::GapSeed seed = VpMotion::GapSeed::Make(g, 10, 10, 10 + kDropped, 7, 8, true);
			Assert::IsTrue(seed.valid);
			Dejudder::MotionWarp seeded(fixture.Log(), gpu, settings());
			seeded.SetSearchSeed(seed.global);
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes), std::wstring(seeded.LastError().begin(), seeded.LastError().end()).c_str());
			r.seededUsed = seeded.SearchSeedUsed();
			r.seeded = Score(DownloadField(gpu, seeded), truth);
			Assert::IsTrue(seeded.Estimate(pa.planes, pb.planes));
			r.spentSeedUsedAgain = seeded.SearchSeedUsed();
			return r;
		}
	};

	TEST_CLASS(MatcherGlobalD1TextPinTests)
	{
	public:
		TEST_METHOD(AllFourteenD1ShadersMatchPinnedFnv1aHashesAndInvariants)
		{
			using namespace VpMotion::MatcherGlobalD1;

			// Anchor the FNV-1a 64-bit helper against standard reference vectors before pinning any shader text.
			Assert::AreEqual(static_cast<uint64_t>(0xcbf29ce484222325ull), Fnv1a64(""), L"FNV-1a empty vector");
			Assert::AreEqual(static_cast<uint64_t>(0xaf63dc4c8601ec8cull), Fnv1a64("a"), L"FNV-1a 'a' vector");
			Assert::AreEqual(static_cast<uint64_t>(0x85944171f73967e8ull), Fnv1a64("foobar"), L"FNV-1a 'foobar' vector");

			struct ShaderPin
			{
				const wchar_t* name;
				const char* text;
				size_t expectedBytes;
				uint64_t expectedFnv1a;
			};

			// Pinned exact LF byte lengths and 64-bit FNV-1a hashes of all 14 D1 shaders moved in U10 C2
			// (100% byte-identical to pre-move GLOBAL_* literals in DejudderWarp.cpp, including J#2 textureLod).
			const ShaderPin kPins[] = {
				{ L"kGlobalHeader",        kGlobalHeader,        1598u, 0x17a501efed77eb72ull },
				{ L"kGlobalBody",          kGlobalBody,          1645u, 0x0b8c0d479fc80432ull },
				{ L"kGlobalSumHeader",     kGlobalSumHeader,       51u, 0x2f3a2ec48067b836ull },
				{ L"kGlobalL2Body",        kGlobalL2Body,         965u, 0x7ee2c658db228c76ull },
				{ L"kGlobalPickHeader",    kGlobalPickHeader,     317u, 0xd01ff3eeb6fe373eull },
				{ L"kGlobalPick2Body",     kGlobalPick2Body,      999u, 0xf5f131cbb976fe82ull },
				{ L"kGlobalL1Body",        kGlobalL1Body,        1167u, 0xe371fc529cc369e2ull },
				{ L"kGlobalPick1Body",     kGlobalPick1Body,      994u, 0xb67b5a8137f2bcc8ull },
				{ L"kGlobalBothHeader",    kGlobalBothHeader,      51u, 0x2f3a2ec48067b836ull },
				{ L"kGlobalBothL2Header",  kGlobalBothL2Header,   269u, 0x436464375b44a2d7ull },
				{ L"kGlobalBothL2Body",    kGlobalBothL2Body,    1021u, 0x40870b534356f715ull },
				{ L"kGlobalBothPick2Body", kGlobalBothPick2Body,  978u, 0x4200a76b88e38f73ull },
				{ L"kGlobalBothL1Body",    kGlobalBothL1Body,    1600u, 0x74b446fe0fac90d0ull },
				{ L"kGlobalBothPick1Body", kGlobalBothPick1Body, 1081u, 0xf54a48013e59081cull },
			};
			Assert::AreEqual(static_cast<size_t>(14u), sizeof(kPins) / sizeof(kPins[0]), L"all 14 D1 shader constants must be pinned");

			for (const ShaderPin& pin : kPins)
			{
				Assert::IsNotNull(pin.text, pin.name);
				Assert::IsNull(std::strchr(pin.text, '\r'), pin.name);
				Assert::AreEqual(pin.expectedBytes, std::strlen(pin.text), pin.name);
				Assert::AreEqual(pin.expectedFnv1a, Fnv1a64(pin.text), pin.name);
			}

			// Key structural invariants: J#2 explicit textureLod optimization in kGlobalBothL1Body
			// and M2 second-candidate #ifdef GLOBAL_TWO guards in both pick-1 kernels.
			const std::string bothL1(kGlobalBothL1Body);
			Assert::IsTrue(bothL1.find("textureLod(l1_b, t, 0.0)") != std::string::npos,
				L"kGlobalBothL1Body must preserve J#2 textureLod(l1_b, t, 0.0)");
			Assert::IsTrue(bothL1.find("textureLod(l1_a, t, 0.0)") != std::string::npos,
				L"kGlobalBothL1Body must preserve J#2 textureLod(l1_a, t, 0.0)");
			Assert::IsTrue(bothL1.find("float fa = texelFetch(l1_a, p, 0).r") == std::string::npos,
				L"kGlobalBothL1Body must not contain removed pre-J#2 unconditional texelFetch");
			Assert::IsTrue(std::string(kGlobalPick1Body).find("#ifdef GLOBAL_TWO") != std::string::npos,
				L"kGlobalPick1Body must preserve #ifdef GLOBAL_TWO");
			Assert::IsTrue(std::string(kGlobalBothPick1Body).find("#ifdef GLOBAL_TWO") != std::string::npos,
				L"kGlobalBothPick1Body must preserve #ifdef GLOBAL_TWO");

			// Derived D1 search grid dimensions.
			Assert::AreEqual(8, kL2RadiusTexels, L"kL2RadiusTexels");
			Assert::AreEqual(17, kL2Candidates, L"kL2Candidates");
			Assert::AreEqual(2 * kL2RadiusTexels + 1, kL2Candidates, L"kL2Candidates derivation");
			Assert::AreEqual(4, kL1HalfStepRadius, L"kL1HalfStepRadius");
			Assert::AreEqual(9, kL1Candidates, L"kL1Candidates");
			Assert::AreEqual(2 * kL1HalfStepRadius + 1, kL1Candidates, L"kL1Candidates derivation");
			Assert::AreEqual(16, kWorkgroupSide, L"kWorkgroupSide");

			// Same-input negative control: mutating 1 single byte in a local copy of kGlobalBothL1Body
			// ('0' -> '1' in textureLod(..., 0.0)) keeps byte length identical (defeating a length-only check)
			// while strictly changing the FNV-1a hash (proving 1-byte shader drift is always detected).
			std::string mutatedBothL1(kGlobalBothL1Body);
			const size_t lodPos = mutatedBothL1.find("textureLod(l1_b, t, 0.0)");
			Assert::IsTrue(lodPos != std::string::npos, L"locate textureLod(l1_b, t, 0.0)");
			const size_t zeroPos = mutatedBothL1.find("0.0", lodPos);
			Assert::IsTrue(zeroPos != std::string::npos && mutatedBothL1[zeroPos] == '0', L"locate '0' in 0.0");
			mutatedBothL1[zeroPos] = '1';
			Assert::AreEqual(static_cast<size_t>(1600u), mutatedBothL1.size(),
				L"negative control keeps exact byte length so only FNV-1a detects the 1-byte drift");
			const uint64_t mutatedHash = Fnv1a64(mutatedBothL1.c_str());
			Assert::IsTrue(mutatedHash != 0x74b446fe0fac90d0ull,
				L"1-byte mutation of kGlobalBothL1Body must change the FNV-1a hash");
			Assert::AreEqual(static_cast<uint64_t>(0xe05e37492e792165ull), mutatedHash,
				L"1-byte mutated hash matches independent offline computation");
		}

		TEST_METHOD(RunSingleAndRunBothRejectInvalidInputsVisiblyWithoutDispatching)
		{
			using namespace VpMotion::MatcherGlobalD1;

			// Distinct non-null sentinel addresses (never dereferenced in CPU-only dispatch tests).
			static const unsigned char kSentinelStorage[16] = {};
			const pl_gpu sentinelGpu = reinterpret_cast<pl_gpu>(&kSentinelStorage[0]);
			const pl_tex texFromL1 = reinterpret_cast<pl_tex>(&kSentinelStorage[1]);
			const pl_tex texToL1 = reinterpret_cast<pl_tex>(&kSentinelStorage[2]);
			const pl_tex texFromL2 = reinterpret_cast<pl_tex>(&kSentinelStorage[3]);
			const pl_tex texToL2 = reinterpret_cast<pl_tex>(&kSentinelStorage[4]);
			const pl_tex texOut0 = reinterpret_cast<pl_tex>(&kSentinelStorage[5]);
			const pl_tex texOut1 = reinterpret_cast<pl_tex>(&kSentinelStorage[6]);
			const pl_tex texSecond0 = reinterpret_cast<pl_tex>(&kSentinelStorage[7]);
			const pl_tex texSecond1 = reinterpret_cast<pl_tex>(&kSentinelStorage[8]);
			const pl_tex texSad2 = reinterpret_cast<pl_tex>(&kSentinelStorage[9]);
			const pl_tex texSad1 = reinterpret_cast<pl_tex>(&kSentinelStorage[10]);
			const pl_tex texPick = reinterpret_cast<pl_tex>(&kSentinelStorage[11]);

			struct DispatchRecord
			{
				std::string description;
				const char* header = nullptr;
				const char* body = nullptr;
				int numDescriptors = 0;
				std::vector<std::string> descNames;
				std::vector<pl_tex> descObjects;
				std::vector<enum pl_desc_type> descTypes;
				std::vector<enum pl_tex_sample_mode> descSampleModes;
				int w = 0;
				int h = 0;
				int threadsX = 0;
				int threadsY = 0;
			};

			int dispatchCount = 0;
			std::vector<DispatchRecord> records;
			int failOnCall = 0;   // 0 = never fail, N >= 1 = return false on Nth dispatch call

			auto resetMock = [&](int failCall)
			{
				dispatchCount = 0;
				records.clear();
				failOnCall = failCall;
			};

			auto recordDescriptors = [](DispatchRecord& rec, const pl_shader_desc* descriptors, int numDescriptors)
			{
				rec.numDescriptors = numDescriptors;
				for (int i = 0; i < numDescriptors; ++i)
				{
					rec.descNames.push_back(descriptors && descriptors[i].desc.name ? descriptors[i].desc.name : "");
					rec.descObjects.push_back(descriptors ? static_cast<pl_tex>(descriptors[i].binding.object) : nullptr);
					rec.descTypes.push_back(descriptors ? descriptors[i].desc.type : PL_DESC_INVALID);
					rec.descSampleModes.push_back(descriptors ? descriptors[i].binding.sample_mode : PL_TEX_SAMPLE_NEAREST);
				}
			};

			const DispatchFn countingDispatch = [&](const char* description, const char* header, const char* body,
				const pl_shader_desc* descriptors, int numDescriptors,
				const pl_shader_var*, int, int w, int h, int group) -> bool
			{
				++dispatchCount;
				DispatchRecord rec;
				rec.description = description ? description : "";
				rec.header = header;
				rec.body = body;
				rec.w = w;
				rec.h = h;
				rec.threadsX = group;
				rec.threadsY = group;
				recordDescriptors(rec, descriptors, numDescriptors);
				records.push_back(rec);
				return failOnCall == 0 || dispatchCount != failOnCall;
			};

			const DispatchPerBlockFn countingPerBlock = [&](const char* description, const char* header, const char* body,
				const pl_shader_desc* descriptors, int numDescriptors,
				const pl_shader_var*, int, int w, int h, int threadsX, int threadsY) -> bool
			{
				++dispatchCount;
				DispatchRecord rec;
				rec.description = description ? description : "";
				rec.header = header;
				rec.body = body;
				rec.w = w;
				rec.h = h;
				rec.threadsX = threadsX;
				rec.threadsY = threadsY;
				recordDescriptors(rec, descriptors, numDescriptors);
				records.push_back(rec);
				return failOnCall == 0 || dispatchCount != failOnCall;
			};

			// ScratchTextures default-initializes all caller-owned scratch pointers to nullptr.
			const ScratchTextures defaultScratch{};
			Assert::IsNull(defaultScratch.sad2, L"ScratchTextures::sad2 default nullptr");
			Assert::IsNull(defaultScratch.sad1, L"ScratchTextures::sad1 default nullptr");
			Assert::IsNull(defaultScratch.pick, L"ScratchTextures::pick default nullptr");

			auto expectSingleReject = [&](const wchar_t* label, pl_gpu gpu, bool serialReference,
				pl_tex fromL1, pl_tex toL1, pl_tex fromL2, pl_tex toL2, pl_tex out,
				pl_tex sad2Init, pl_tex sad1Init, pl_tex pickInit,
				const DispatchFn& disp, const DispatchPerBlockFn& dispBlock)
			{
				resetMock(0);
				pl_tex sad2 = sad2Init, sad1 = sad1Init, pick = pickInit;
				std::string error;
				const bool ok = RunSingle(gpu, serialReference, fromL1, toL1, fromL2, toL2, out, nullptr,
					sad2, sad1, pick, disp, dispBlock, error);
				Assert::IsFalse(ok, label);
				Assert::IsFalse(error.empty(), label);
				Assert::AreEqual(0, dispatchCount, label);
			};

			auto expectBothReject = [&](const wchar_t* label, pl_gpu gpu,
				pl_tex l1A, pl_tex l1B, pl_tex l2A, pl_tex l2B, pl_tex* outPtr,
				pl_tex sad2Init, pl_tex sad1Init, pl_tex pickInit,
				const DispatchPerBlockFn& dispBlock)
			{
				resetMock(0);
				pl_tex sad2 = sad2Init, sad1 = sad1Init, pick = pickInit;
				std::string error;
				const bool ok = RunBoth(gpu, l1A, l1B, l2A, l2B, outPtr, nullptr,
					sad2, sad1, pick, dispBlock, error);
				Assert::IsFalse(ok, label);
				Assert::IsFalse(error.empty(), label);
				Assert::AreEqual(0, dispatchCount, label);
			};

			// ---- RunSingle negative controls (serial + parallel) ----
			expectSingleReject(L"RunSingle serial: missing dispatch callback",
				sentinelGpu, true, texFromL1, texToL1, texFromL2, texToL2, texOut0,
				texSad2, texSad1, texPick, DispatchFn{}, countingPerBlock);
			expectSingleReject(L"RunSingle serial: null fromL1",
				sentinelGpu, true, nullptr, texToL1, texFromL2, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle serial: null toL1",
				sentinelGpu, true, texFromL1, nullptr, texFromL2, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle serial: null fromL2",
				sentinelGpu, true, texFromL1, texToL1, nullptr, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle serial: null toL2",
				sentinelGpu, true, texFromL1, texToL1, texFromL2, nullptr, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle serial: null out",
				sentinelGpu, true, texFromL1, texToL1, texFromL2, texToL2, nullptr,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);

			expectSingleReject(L"RunSingle parallel: missing dispatchPerBlock callback",
				sentinelGpu, false, texFromL1, texToL1, texFromL2, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, DispatchPerBlockFn{});
			expectSingleReject(L"RunSingle parallel: null gpu with all scratch textures null",
				nullptr, false, texFromL1, texToL1, texFromL2, texToL2, texOut0,
				nullptr, nullptr, nullptr, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null gpu with null sad2 scratch",
				nullptr, false, texFromL1, texToL1, texFromL2, texToL2, texOut0,
				nullptr, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null gpu with null sad1 scratch",
				nullptr, false, texFromL1, texToL1, texFromL2, texToL2, texOut0,
				texSad2, nullptr, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null gpu with null pick scratch",
				nullptr, false, texFromL1, texToL1, texFromL2, texToL2, texOut0,
				texSad2, texSad1, nullptr, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null fromL1",
				sentinelGpu, false, nullptr, texToL1, texFromL2, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null toL1",
				sentinelGpu, false, texFromL1, nullptr, texFromL2, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null fromL2",
				sentinelGpu, false, texFromL1, texToL1, nullptr, texToL2, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null toL2",
				sentinelGpu, false, texFromL1, texToL1, texFromL2, nullptr, texOut0,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);
			expectSingleReject(L"RunSingle parallel: null out",
				sentinelGpu, false, texFromL1, texToL1, texFromL2, texToL2, nullptr,
				texSad2, texSad1, texPick, countingDispatch, countingPerBlock);

			// ---- RunBoth negative controls ----
			pl_tex validOut[2] = { texOut0, texOut1 };
			pl_tex nullOut0[2] = { nullptr, texOut1 };
			pl_tex nullOut1[2] = { texOut0, nullptr };

			expectBothReject(L"RunBoth: missing dispatchPerBlock callback",
				sentinelGpu, texFromL1, texToL1, texFromL2, texToL2, validOut,
				texSad2, texSad1, texPick, DispatchPerBlockFn{});
			expectBothReject(L"RunBoth: null out array pointer",
				sentinelGpu, texFromL1, texToL1, texFromL2, texToL2, nullptr,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null out[0]",
				sentinelGpu, texFromL1, texToL1, texFromL2, texToL2, nullOut0,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null out[1]",
				sentinelGpu, texFromL1, texToL1, texFromL2, texToL2, nullOut1,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null l1A",
				sentinelGpu, nullptr, texToL1, texFromL2, texToL2, validOut,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null l1B",
				sentinelGpu, texFromL1, nullptr, texFromL2, texToL2, validOut,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null l2A",
				sentinelGpu, texFromL1, texToL1, nullptr, texToL2, validOut,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null l2B",
				sentinelGpu, texFromL1, texToL1, texFromL2, nullptr, validOut,
				texSad2, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null gpu with all scratch textures null",
				nullptr, texFromL1, texToL1, texFromL2, texToL2, validOut,
				nullptr, nullptr, nullptr, countingPerBlock);
			expectBothReject(L"RunBoth: null gpu with null bothSad2 scratch",
				nullptr, texFromL1, texToL1, texFromL2, texToL2, validOut,
				nullptr, texSad1, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null gpu with null bothSad1 scratch",
				nullptr, texFromL1, texToL1, texFromL2, texToL2, validOut,
				texSad2, nullptr, texPick, countingPerBlock);
			expectBothReject(L"RunBoth: null gpu with null bothPick scratch",
				nullptr, texFromL1, texToL1, texFromL2, texToL2, validOut,
				texSad2, texSad1, nullptr, countingPerBlock);

			// ---- RunSingle serial reference positive control + failure propagation ----
			{
				resetMock(0);
				pl_tex sad2 = nullptr, sad1 = nullptr, pick = nullptr;
				std::string error;
				const bool ok = RunSingle(sentinelGpu, true, texFromL1, texToL1, texFromL2, texToL2, texOut0, nullptr,
					sad2, sad1, pick, countingDispatch, DispatchPerBlockFn{}, error);
				Assert::IsTrue(ok, L"RunSingle serial reference succeeds with valid sentinels and mock DispatchFn");
				Assert::AreEqual(1, dispatchCount, L"serial reference dispatches exactly once");
				Assert::AreEqual(static_cast<size_t>(1u), records.size());
				const DispatchRecord& rec = records[0];
				Assert::AreEqual(std::string("VideoProcessor de-judder: global motion (D1, serial reference)"),
					rec.description, L"serial reference pass label");
				Assert::IsTrue(rec.header == kGlobalHeader, L"serial reference dispatches exact kGlobalHeader pointer");
				Assert::IsTrue(rec.body == kGlobalBody, L"serial reference dispatches exact kGlobalBody pointer");
				Assert::AreEqual(5, rec.numDescriptors, L"serial reference binds 5 descriptors");
				Assert::AreEqual(std::string("from_l1"), rec.descNames[0]);
				Assert::AreEqual(std::string("to_l1"), rec.descNames[1]);
				Assert::AreEqual(std::string("from_l2"), rec.descNames[2]);
				Assert::AreEqual(std::string("to_l2"), rec.descNames[3]);
				Assert::AreEqual(std::string("dst"), rec.descNames[4]);
				Assert::IsTrue(rec.descObjects[0] == texFromL1, L"descriptor 0 object == fromL1");
				Assert::IsTrue(rec.descObjects[1] == texToL1, L"descriptor 1 object == toL1");
				Assert::IsTrue(rec.descObjects[2] == texFromL2, L"descriptor 2 object == fromL2");
				Assert::IsTrue(rec.descObjects[3] == texToL2, L"descriptor 3 object == toL2");
				Assert::IsTrue(rec.descObjects[4] == texOut0, L"descriptor 4 object == out");
				Assert::IsTrue(rec.descTypes[0] == PL_DESC_SAMPLED_TEX && rec.descSampleModes[0] == PL_TEX_SAMPLE_NEAREST);
				Assert::IsTrue(rec.descTypes[1] == PL_DESC_SAMPLED_TEX && rec.descSampleModes[1] == PL_TEX_SAMPLE_LINEAR);
				Assert::IsTrue(rec.descTypes[2] == PL_DESC_SAMPLED_TEX && rec.descSampleModes[2] == PL_TEX_SAMPLE_NEAREST);
				Assert::IsTrue(rec.descTypes[3] == PL_DESC_SAMPLED_TEX && rec.descSampleModes[3] == PL_TEX_SAMPLE_NEAREST);
				Assert::IsTrue(rec.descTypes[4] == PL_DESC_STORAGE_IMG);
				Assert::AreEqual(kWorkgroupSide, rec.w);
				Assert::AreEqual(kWorkgroupSide, rec.h);
				Assert::AreEqual(kWorkgroupSide, rec.threadsX);

				// Failure propagation when DispatchFn returns false.
				resetMock(1);
				error.clear();
				const bool failOk = RunSingle(sentinelGpu, true, texFromL1, texToL1, texFromL2, texToL2, texOut0, nullptr,
					sad2, sad1, pick, countingDispatch, DispatchPerBlockFn{}, error);
				Assert::IsFalse(failOk, L"RunSingle serial reference must propagate false from DispatchFn");
				Assert::AreEqual(1, dispatchCount, L"failed DispatchFn was invoked exactly once");
			}

			// ---- RunSingle parallel & RunBoth positive dispatch wiring + mid-chain failure propagation ----
			{
				resetMock(0);
				pl_tex sad2 = texSad2, sad1 = texSad1, pick = texPick;
				std::string error;
				const bool okSingle = RunSingle(sentinelGpu, false, texFromL1, texToL1, texFromL2, texToL2, texOut0, texSecond0,
					sad2, sad1, pick, DispatchFn{}, countingPerBlock, error);
				Assert::IsTrue(okSingle, L"RunSingle parallel succeeds with pre-populated scratch sentinels");
				Assert::AreEqual(4, dispatchCount, L"RunSingle parallel dispatches all 4 D1 stages");
				Assert::IsTrue(records[0].body == kGlobalL2Body && records[0].w == kL2Candidates && records[0].h == kL2Candidates);
				Assert::IsTrue(records[1].body == kGlobalPick2Body && records[1].w == 1 && records[1].h == 1);
				Assert::IsTrue(records[2].body == kGlobalL1Body && records[2].w == kL1Candidates && records[2].h == kL1Candidates);
				Assert::IsTrue(records[3].body == kGlobalPick1Body && records[3].numDescriptors == 4);
				Assert::IsTrue(std::string(records[3].header).find("#define GLOBAL_TWO 1") != std::string::npos,
					L"RunSingle parallel enables GLOBAL_TWO when second != nullptr");

				resetMock(2);
				const bool failSingleMid = RunSingle(sentinelGpu, false, texFromL1, texToL1, texFromL2, texToL2, texOut0, nullptr,
					sad2, sad1, pick, DispatchFn{}, countingPerBlock, error);
				Assert::IsFalse(failSingleMid, L"RunSingle parallel propagates stage-2 dispatch failure");
				Assert::AreEqual(2, dispatchCount, L"RunSingle parallel stops dispatching after stage-2 failure");

				resetMock(0);
				pl_tex secondBoth[2] = { texSecond0, texSecond1 };
				const bool okBoth = RunBoth(sentinelGpu, texFromL1, texToL1, texFromL2, texToL2, validOut, secondBoth,
					sad2, sad1, pick, countingPerBlock, error);
				Assert::IsTrue(okBoth, L"RunBoth succeeds with pre-populated scratch sentinels");
				Assert::AreEqual(4, dispatchCount, L"RunBoth dispatches all 4 bidirectional D1 stages");
				Assert::IsTrue(records[0].body == kGlobalBothL2Body && records[0].w == 2 * kL2Candidates && records[0].h == kL2Candidates);
				Assert::IsTrue(records[1].body == kGlobalBothPick2Body && records[1].w == 2 && records[1].h == 1);
				Assert::IsTrue(records[2].body == kGlobalBothL1Body && records[2].w == 2 * kL1Candidates && records[2].h == kL1Candidates);
				Assert::IsTrue(records[3].body == kGlobalBothPick1Body && records[3].numDescriptors == 6);
				Assert::IsTrue(std::string(records[3].header).find("#define GLOBAL_TWO 1") != std::string::npos,
					L"RunBoth enables GLOBAL_TWO when second[0] and second[1] are non-null");

				resetMock(2);
				const bool failBothMid = RunBoth(sentinelGpu, texFromL1, texToL1, texFromL2, texToL2, validOut, nullptr,
					sad2, sad1, pick, countingPerBlock, error);
				Assert::IsFalse(failBothMid, L"RunBoth propagates stage-2 dispatch failure");
				Assert::AreEqual(2, dispatchCount, L"RunBoth stops dispatching after stage-2 failure");
			}
		}

	private:
		static uint64_t Fnv1a64(const char* text)
		{
			uint64_t h = 0xcbf29ce484222325ull;
			for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p)
			{
				if (*p == '\r')
					continue;
				h ^= static_cast<uint64_t>(*p);
				h *= 0x100000001b3ull;
			}
			return h;
		}
	};
}
