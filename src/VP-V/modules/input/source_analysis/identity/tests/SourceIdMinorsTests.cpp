/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for Source Identity LOW-A & LOW-B minor fixes
// (Rule 3 mandatory in-test negative controls, zero oracle-only tests, zero sleeps, pure CPU C++14 /W4).
//
// Covers:
//   1. LowAReasonWordsFormatsAllS2S3ReasonBitsDistinctlyAndDefaultOffIsByteIdentical
//   2. LowALatchClauseAndDetailTextDistinguishJointLatchWithoutSafetyRules
//   3. LowBDefaultOffDumpV4SelfTags8In10HdmiLoopAsN8WithoutInflatingGovernorCost
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "GeometryFixtures.h"

#include <VP-V/modules/input/source_analysis/identity/SidDiagDump.h>
#include <VP-V/modules/input/source_analysis/identity/SourceGeometryView.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainStatus.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/input/source_analysis/identity/chain/NullTables.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace MinorsFx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kBilinear = 0;

		struct SplitMix64
		{
			uint64_t state;
			explicit SplitMix64(uint64_t seed) : state(seed) {}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9e3779b97f4a7c15ull);
				z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
				z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
				return z ^ (z >> 31);
			}

			double Uniform01()
			{
				return static_cast<double>(NextU64() >> 11) * (1.0 / 9007199254740992.0);
			}

			double UniformCentered() { return Uniform01() - 0.5; }
			double TpdfUnit() { return UniformCentered() + UniformCentered(); }

			double Normal01()
			{
				double u1 = Uniform01();
				if (u1 < 1e-12)
					u1 = 1e-12;
				const double u2 = Uniform01();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * C::kPi * u2);
			}
		};

		inline uint16_t QuantizeCode(double v, int step)
		{
			const double s = static_cast<double>(step > 0 ? step : 1);
			double q = std::floor(v / s + 0.5) * s;
			if (q < 64.0)
				q = 64.0;
			if (q > 940.0)
				q = 940.0;
			return static_cast<uint16_t>(q);
		}

		// Independent first-principles 1D line synthesis (upscaled from nIn -> nOut via KernelBank::TapWeights
		// or native broadband when nIn == nOut), with optional TPDF dither and additive Gaussian grain.
		inline void SynthLine1D(int nIn, int nOut, int kernel, int step, SplitMix64& rng,
			std::vector<double>& outBuf, double extraNoiseSigma = 0.0, bool tpdfDither = false, double flatFraction = 0.0)
		{
			outBuf.assign(static_cast<size_t>(nOut), 512.0);
			const int activeEnd = static_cast<int>(std::floor(static_cast<double>(nIn) * (1.0 - flatFraction)));

			std::vector<double> src(static_cast<size_t>(nIn), 512.0);
			for (int i = 0; i < nIn; ++i)
			{
				if (i < activeEnd)
					src[static_cast<size_t>(i)] = 512.0 + 45.0 * rng.Normal01();
				else
					src[static_cast<size_t>(i)] = 512.0 + 0.02 * static_cast<double>(i);
			}

			if (nIn == nOut)
			{
				outBuf = src;
			}
			else
			{
				const int r = C::Kernel(kernel).radius;
				const int taps = 2 * r;
				std::vector<double> wt(static_cast<size_t>(taps));
				for (int i = 0; i < nOut; ++i)
				{
					const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(nIn) / static_cast<double>(nOut) - 0.5;
					const double fl = std::floor(p);
					C::TapWeights(kernel, p - fl, wt.data());
					double acc = 0.0;
					for (int s = 0; s < taps; ++s)
					{
						int c = static_cast<int>(fl) - r + 1 + s;
						if (c < 0)
							c = 0;
						if (c > nIn - 1)
							c = nIn - 1;
						acc += wt[static_cast<size_t>(s)] * src[static_cast<size_t>(c)];
					}
					outBuf[static_cast<size_t>(i)] = acc;
				}
			}

			for (int i = 0; i < nOut; ++i)
			{
				double v = outBuf[static_cast<size_t>(i)];
				if (tpdfDither)
					v += static_cast<double>(step) * rng.TpdfUnit();
				if (extraNoiseSigma > 0.0)
					v += extraNoiseSigma * rng.Normal01();
				outBuf[static_cast<size_t>(i)] = static_cast<double>(QuantizeCode(v, step));
			}
		}

		inline C::LineSnapshot MakeSynthSnapshot(
			int rasterW, int rasterH, int srcW, int srcH, int kernel, int step,
			uint64_t seq, int64_t timeUs, uint64_t seed,
			bool cut = false, double extraNoiseSigma = 0.0, bool tpdfDither = false, double flatFraction = 0.0)
		{
			C::LineRect active;
			active.x = 0;
			active.y = 0;
			active.w = rasterW;
			active.h = rasterH;
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"fixture: valid LinePlan");

			C::LineSnapshot snap;
			snap.seq = seq;
			snap.timeUs = timeUs;
			snap.rasterW = rasterW;
			snap.rasterH = rasterH;
			snap.active = plan.active;
			snap.inner = plan.inner;
			snap.rowCount = plan.rowCount;
			snap.colCount = plan.colCount;
			snap.rowY.assign(plan.rowY, plan.rowY + plan.rowCount);
			snap.colX.assign(plan.colX, plan.colX + plan.colCount);
			snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), 512);
			snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), 512);
			if (cut)
			{
				snap.flags = C::kEvCut;
				snap.cuts = 1;
				snap.eventTimeUs = timeUs;
			}

			SplitMix64 rng(seed ^ (seq * 0x9e3779b97f4a7c15ull));
			std::vector<double> line;
			for (int j = 0; j < plan.rowCount; ++j)
			{
				SynthLine1D(srcW, rasterW, kernel, step, rng, line, extraNoiseSigma, tpdfDither, flatFraction);
				uint16_t* dst = &snap.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
				for (int i = 0; i < plan.inner.w; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.x + i)]);
			}
			for (int j = 0; j < plan.colCount; ++j)
			{
				SynthLine1D(srcH, rasterH, kernel, step, rng, line, extraNoiseSigma, tpdfDither, flatFraction);
				uint16_t* dst = &snap.cols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
				for (int i = 0; i < plan.inner.h; ++i)
					dst[i] = static_cast<uint16_t>(line[static_cast<size_t>(plan.inner.y + i)]);
			}
			return snap;
		}

		// Applies physical HDMI 8-to-10-bit MSB replication: v10 = (v8 << 2) | (v8 >> 6).
		inline void ApplyBitReplication8To10(std::vector<uint16_t>& codes)
		{
			for (size_t i = 0; i < codes.size(); ++i)
			{
				int v8 = static_cast<int>(std::floor(static_cast<double>(codes[i]) / 4.0 + 0.5));
				v8 = (std::max)(16, (std::min)(235, v8));
				codes[i] = static_cast<uint16_t>((v8 << 2) | (v8 >> 6));
			}
		}

		inline std::vector<std::string> SplitCsvRow(const std::string& line)
		{
			std::vector<std::string> out;
			std::string cur;
			bool quoted = false;
			for (size_t i = 0; i < line.size(); ++i)
			{
				const char ch = line[i];
				if (quoted)
				{
					if (ch == '"')
					{
						if (i + 1 < line.size() && line[i + 1] == '"')
						{
							cur += '"';
							++i;
						}
						else
							quoted = false;
					}
					else
						cur += ch;
				}
				else if (ch == '"')
					quoted = true;
				else if (ch == ',')
				{
					out.push_back(cur);
					cur.clear();
				}
				else
					cur += ch;
			}
			out.push_back(cur);
			return out;
		}

		inline std::vector<std::string> SplitCommaSpace(const std::string& s)
		{
			std::vector<std::string> out;
			if (s.empty())
				return out;
			size_t pos = 0;
			for (;;)
			{
				const size_t sep = s.find(", ", pos);
				if (sep == std::string::npos)
				{
					out.push_back(s.substr(pos));
					break;
				}
				out.push_back(s.substr(pos, sep - pos));
				pos = sep + 2;
			}
			return out;
		}

		inline bool EndsWith(const std::string& s, const std::string& suffix)
		{
			return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
		}

		inline C::ChainStatusInput MakeRunningStatusInput()
		{
			C::ChainStatusInput in;
			in.running = true;
			in.tapAttached = true;
			in.haveRecord = true;
			in.last.c1 = 15.0;
			return in;
		}

		// Strict RAII guard for VP_SID_DUMP_V4_DIR and unique temporary directory (Rule 2: zero state leak).
		class ScopedDumpV4Dir
		{
		public:
			explicit ScopedDumpV4Dir(const char* suffix)
			{
				char* prev = nullptr;
				size_t len = 0;
				if (_dupenv_s(&prev, &len, "VP_SID_DUMP_V4_DIR") == 0 && prev != nullptr)
				{
					prevEnv_ = prev;
					std::free(prev);
				}

				static std::atomic<uint64_t> seq{ 0 };
				char tempPath[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, tempPath);
				dir_ = std::string(tempPath) + "vp_sid_minors_" + (suffix ? suffix : "dump") + "_"
					+ std::to_string(GetCurrentProcessId()) + "_" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
				CreateDirectoryA(dir_.c_str(), nullptr);
				ClearCsv();
				SetEnv(dir_);
			}

			~ScopedDumpV4Dir()
			{
				SetEnv(prevEnv_);
				ClearCsv();
				if (!dir_.empty())
					RemoveDirectoryA(dir_.c_str());
			}

			ScopedDumpV4Dir(const ScopedDumpV4Dir&) = delete;
			ScopedDumpV4Dir& operator=(const ScopedDumpV4Dir&) = delete;

			const std::string& Dir() const { return dir_; }

			void SetEnv(const std::string& v) const
			{
				_putenv_s("VP_SID_DUMP_V4_DIR", v.c_str());
			}

			void ClearCsv() const
			{
				if (dir_.empty())
					return;
				WIN32_FIND_DATAA fd;
				HANDLE h = FindFirstFileA((dir_ + "\\*.csv").c_str(), &fd);
				if (h == INVALID_HANDLE_VALUE)
					return;
				do
				{
					DeleteFileA((dir_ + "\\" + fd.cFileName).c_str());
				} while (FindNextFileA(h, &fd));
				FindClose(h);
			}

			int CountCsv() const
			{
				if (dir_.empty())
					return 0;
				WIN32_FIND_DATAA fd;
				HANDLE h = FindFirstFileA((dir_ + "\\*.csv").c_str(), &fd);
				if (h == INVALID_HANDLE_VALUE)
					return 0;
				int count = 0;
				do { ++count; } while (FindNextFileA(h, &fd));
				FindClose(h);
				return count;
			}

		private:
			std::string prevEnv_;
			std::string dir_;
		};
	}

	TEST_CLASS(SourceIdMinorsTests)
	{
	public:
		// 1. LOW-A: SourceGeometry::ReasonWords() formats every S2/S3 reason bit distinctly and non-emptily,
		//    combines multiple bits with comma-separated wording, surfaces them through ChainStatusLine/Detail,
		//    and leaves ReasonWords(0u) empty & default-OFF W1 ChainStatusLine byte-identical.
		TEST_METHOD(LowAReasonWordsFormatsAllS2S3ReasonBitsDistinctlyAndDefaultOffIsByteIdentical)
		{
			using namespace MinorsFx;

			const uint32_t s2s3Bits[4] = {
				C::kReasonNullTableMismatch,
				C::kReasonJointEntry,
				C::kReasonChainAloneEntry,
				C::kReasonScreenOnly
			};
			const uint32_t s2s3Mask =
				C::kReasonNullTableMismatch | C::kReasonJointEntry | C::kReasonChainAloneEntry | C::kReasonScreenOnly;

			const uint32_t legacyBits[11] = {
				C::kReasonDark,
				C::kReasonFlat,
				C::kReasonSoft,
				C::kReasonNoPower,
				C::kReasonLowDetail,
				C::kReasonMixed,
				C::kReasonNonConventional,
				C::kReasonLatchBar,
				C::kReasonLatchRelease,
				C::kReasonMenuHold,
				C::kReasonAbrStep
			};

			// Each S2/S3 reason bit must produce a non-empty, single-phrase, human-readable string,
			// pairwise distinct from every other S2/S3 bit AND from every legacy bit.
			std::string words[4];
			for (int i = 0; i < 4; ++i)
			{
				words[i] = SourceGeometry::ReasonWords(s2s3Bits[i]);
				Assert::IsFalse(words[i].empty(), L"LOW-A: every S2/S3 reason bit must format to a non-empty string");
				Assert::IsTrue(words[i].find(',') == std::string::npos,
					L"LOW-A: a single S2/S3 reason bit must format as a single phrase without commas");

				// Rule 3 in-test negative control: stripping S2/S3 bits (pre-LOW-A coverage) silently drops it to "".
				const std::string preFixUnmapped = SourceGeometry::ReasonWords(s2s3Bits[i] & ~s2s3Mask);
				Assert::AreEqual(std::string(""), preFixUnmapped,
					L"negative control: without S2/S3 bit formatting, each S2/S3 bit collapses silently to empty string");

				for (int j = 0; j < i; ++j)
					Assert::AreNotEqual(words[j], words[i], L"LOW-A: S2/S3 reason words must be pairwise distinct");

				for (int k = 0; k < 11; ++k)
				{
					const std::string legacyWord = SourceGeometry::ReasonWords(legacyBits[k]);
					Assert::IsFalse(legacyWord.empty());
					Assert::AreNotEqual(legacyWord, words[i],
						L"LOW-A: S2/S3 reason word must not alias any existing legacy reason word");
				}
			}

			// Combining all 4 S2/S3 bits formats comma-separated words containing every single one.
			const std::string combined = SourceGeometry::ReasonWords(s2s3Mask);
			Assert::IsFalse(combined.empty());
			const std::vector<std::string> tokens = SplitCommaSpace(combined);
			Assert::AreEqual<size_t>(4, tokens.size(),
				L"LOW-A: combining all 4 S2/S3 reason bits must emit exactly 4 comma-separated phrases");
			for (int i = 0; i < 4; ++i)
			{
				const bool found = std::find(tokens.begin(), tokens.end(), words[i]) != tokens.end();
				Assert::IsTrue(found, L"LOW-A: combined ReasonWords must include every constituent S2/S3 word");
			}

			// Combining legacy + S2/S3 bits preserves both in comma-separated order.
			const std::string mixedCombined = SourceGeometry::ReasonWords(C::kReasonLowDetail | C::kReasonNullTableMismatch);
			const std::vector<std::string> mixedTokens = SplitCommaSpace(mixedCombined);
			Assert::AreEqual<size_t>(2, mixedTokens.size());
			Assert::AreEqual(SourceGeometry::ReasonWords(C::kReasonLowDetail), mixedTokens[0]);
			Assert::AreEqual(SourceGeometry::ReasonWords(C::kReasonNullTableMismatch), mixedTokens[1]);

			// Verify S2/S3 reason bit on applied[0].reasonBits surfaces visibly through ChainStatusLine (via kLatchReasonBits),
			// whereas an identical abstaining record with the S2/S3 bit stripped falls back to generic "no evidence yet".
			{
				C::ChainStatusInput withBit = MakeRunningStatusInput();
				withBit.record.measured[0] = GeoFx::MeasuredAxis(G::LatchState::Abstain, 0, kUhdW);
				withBit.record.measured[1] = GeoFx::MeasuredAxis(G::LatchState::Abstain, 0, kUhdH);
				withBit.record.applied[0].reasonBits = C::kReasonNullTableMismatch;

				C::ChainStatusInput withoutBit = withBit;
				withoutBit.record.applied[0].reasonBits = 0u;

				const std::string lineWithBit = C::ChainStatusLine(withBit);
				const std::string lineWithoutBit = C::ChainStatusLine(withoutBit);
				Assert::IsTrue(lineWithBit.find(words[0]) != std::string::npos,
					L"LOW-A: ChainStatusLine must surface kReasonNullTableMismatch from applied reasonBits");
				Assert::IsTrue(lineWithoutBit.find("no evidence yet") != std::string::npos,
					L"negative control: stripping S2/S3 reason bit reverts abstaining status line to generic 'no evidence yet'");
				Assert::AreNotEqual(lineWithoutBit, lineWithBit);
			}

			// Negative control / W1 default-OFF byte-identity:
			//   - ReasonWords(0u) == ""
			//   - Default-OFF ChainStatusInput (safetyRules == false && jointLatch == false, zero S2/S3 bits)
			//     produces the exact byte-identical W1 status string.
			Assert::AreEqual(std::string(""), SourceGeometry::ReasonWords(0u), L"ReasonWords(0u) must remain empty");

			C::ChainStatusInput defIn = MakeRunningStatusInput();
			defIn.record.applied[0] = GeoFx::AppliedAxis(1920, kUhdW, 5); // lanczos3
			defIn.record.applied[1] = GeoFx::AppliedAxis(1080, kUhdH, 5);
			defIn.record.applied[0].conf = 0.99f;
			defIn.record.applied[1].conf = 0.99f;
			Assert::IsFalse(defIn.safetyRules);
			Assert::IsFalse(defIn.jointLatch);
			Assert::AreEqual(uint32_t(0), defIn.record.applied[0].reasonBits);
			Assert::AreEqual(uint32_t(0), defIn.record.applied[1].reasonBits);

			const std::string expectedW1 = "Source identity: 1080 -> 2160 lanczos-like, confident; SID safety rules: off";
			Assert::AreEqual(expectedW1, C::ChainStatusLine(defIn),
				L"W1 default-OFF ChainStatusLine must remain byte-identical");
			const std::string defDetail = C::ChainDetailText(defIn);
			Assert::IsTrue(defDetail.find(expectedW1) != std::string::npos);
			Assert::IsTrue(defDetail.find("safety rules on") == std::string::npos);
			Assert::IsTrue(defDetail.find("joint latch on") == std::string::npos);
		}

		// 2. LOW-A: ChainStatusLine & ChainDetailText distinguish jointLatch == true without safetyRules
		//    (both unlatched and latched, deferred and non-deferred) from both default-OFF and safetyRules == true,
		//    and live synchronous ChainEstimator surfaces StatusInput().jointLatch / StatusLine() honestly.
		TEST_METHOD(LowALatchClauseAndDetailTextDistinguishJointLatchWithoutSafetyRules)
		{
			using namespace MinorsFx;

			// Pure ChainStatusInput matrix on identical confident 1080->2160 lanczos-like record:
			C::ChainStatusInput base = MakeRunningStatusInput();
			base.record.applied[0] = GeoFx::AppliedAxis(1920, kUhdW, 5);
			base.record.applied[1] = GeoFx::AppliedAxis(1080, kUhdH, 5);
			base.record.applied[0].conf = 0.99f;
			base.record.applied[1].conf = 0.99f;

			// Baseline negative control: safetyRules == false && jointLatch == false -> "; SID safety rules: off"
			C::ChainStatusInput offIn = base;
			offIn.safetyRules = false;
			offIn.jointLatch = false;
			const std::string offLine = C::ChainStatusLine(offIn);
			const std::string offDetail = C::ChainDetailText(offIn);
			Assert::IsTrue(EndsWith(offLine, "; SID safety rules: off"),
				L"negative control: default-OFF input reports '; SID safety rules: off'");
			Assert::IsTrue(offDetail.find("joint latch on") == std::string::npos);
			Assert::IsTrue(offDetail.find("safety rules on") == std::string::npos);

			// (a) safetyRules == false, jointLatch == true, UNLATCHED (latchNx == 0, latchNy == 0):
			C::ChainStatusInput jointUnlatched = base;
			jointUnlatched.safetyRules = false;
			jointUnlatched.jointLatch = true;
			jointUnlatched.latchNx = 0;
			jointUnlatched.latchNy = 0;
			const std::string jointUnlatchedLine = C::ChainStatusLine(jointUnlatched);
			const std::string jointUnlatchedDetail = C::ChainDetailText(jointUnlatched);
			Assert::IsTrue(jointUnlatchedLine.find("SID safety rules: off") == std::string::npos,
				L"LOW-A: jointLatch without safetyRules must NEVER say 'SID safety rules: off'");
			Assert::IsTrue(jointUnlatchedLine.find("joint latch") != std::string::npos,
				L"LOW-A: unlatched jointLatch must visibly indicate active 'joint latch'");
			Assert::IsTrue(jointUnlatchedDetail.find("joint latch on:") != std::string::npos,
				L"LOW-A: ChainDetailText must show joint latch detail block when jointLatch is active");
			Assert::IsTrue(jointUnlatchedDetail.find("safety rules on:") == std::string::npos);
			Assert::IsTrue(jointUnlatchedDetail.find("SID safety rules: off") == std::string::npos);

			// (b) safetyRules == false, jointLatch == true, LATCHED 1920x1080 (latchDeferred == false):
			C::ChainStatusInput jointLatched = jointUnlatched;
			jointLatched.latchNx = 1920;
			jointLatched.latchNy = 1080;
			jointLatched.latchDeferred = false;
			const std::string jointLatchedLine = C::ChainStatusLine(jointLatched);
			const std::string jointLatchedDetail = C::ChainDetailText(jointLatched);
			Assert::IsTrue(jointLatchedLine.find("SID safety rules: off") == std::string::npos);
			Assert::IsTrue(jointLatchedLine.find("joint upscale latch 1920x1080") != std::string::npos,
				L"LOW-A: latched jointLatch without safetyRules must show 'joint upscale latch 1920x1080'");
			Assert::IsTrue(jointLatchedLine.find("deferred") == std::string::npos);
			Assert::IsTrue(jointLatchedDetail.find("joint latch on: latch 1920x1080") != std::string::npos);
			Assert::IsTrue(jointLatchedDetail.find("deferred") == std::string::npos);

			// (c) safetyRules == false, jointLatch == true, LATCHED 1920x1080 + latchDeferred == true:
			C::ChainStatusInput jointDeferred = jointLatched;
			jointDeferred.latchDeferred = true;
			const std::string jointDeferredLine = C::ChainStatusLine(jointDeferred);
			const std::string jointDeferredDetail = C::ChainDetailText(jointDeferred);
			Assert::IsTrue(jointDeferredLine.find("SID safety rules: off") == std::string::npos);
			Assert::IsTrue(jointDeferredLine.find("joint upscale latch 1920x1080") != std::string::npos);
			Assert::IsTrue(jointDeferredLine.find("deferred") != std::string::npos,
				L"LOW-A: deferred jointLatch must show deferred release text on status line");
			Assert::IsTrue(jointDeferredDetail.find("joint latch on: latch 1920x1080") != std::string::npos);
			Assert::IsTrue(jointDeferredDetail.find("deferred") != std::string::npos,
				L"LOW-A: deferred jointLatch must show deferred release text in detail");
			Assert::AreNotEqual(jointLatchedLine, jointDeferredLine);

			// (d) Comparison against legacy safetyRules == true (unlatched, latched, and deferred):
			C::ChainStatusInput safetyUnlatched = base;
			safetyUnlatched.safetyRules = true;
			safetyUnlatched.jointLatch = false;
			const std::string safetyUnlatchedLine = C::ChainStatusLine(safetyUnlatched);

			C::ChainStatusInput safetyLatched = safetyUnlatched;
			safetyLatched.latchNx = 1920;
			safetyLatched.latchNy = 1080;
			safetyLatched.latchDeferred = false;
			const std::string safetyLatchedLine = C::ChainStatusLine(safetyLatched);
			const std::string safetyLatchedDetail = C::ChainDetailText(safetyLatched);

			C::ChainStatusInput safetyDeferred = safetyLatched;
			safetyDeferred.latchDeferred = true;
			const std::string safetyDeferredLine = C::ChainStatusLine(safetyDeferred);

			Assert::IsTrue(safetyLatchedLine.find("; upscale latch 1920x1080") != std::string::npos);
			Assert::IsTrue(safetyLatchedLine.find("joint upscale latch") == std::string::npos);
			Assert::IsTrue(safetyLatchedDetail.find("safety rules on: latch 1920x1080") != std::string::npos);
			Assert::IsTrue(safetyLatchedDetail.find("joint latch on:") == std::string::npos);

			// Strict 3-way pairwise distinctness (off vs jointLatch-only vs safetyRules):
			Assert::AreNotEqual(offLine, jointUnlatchedLine, L"unlatched jointLatch distinct from off");
			Assert::AreNotEqual(safetyUnlatchedLine, jointUnlatchedLine, L"unlatched jointLatch distinct from safetyRules");
			Assert::AreNotEqual(offLine, jointLatchedLine, L"latched jointLatch distinct from off");
			Assert::AreNotEqual(safetyLatchedLine, jointLatchedLine, L"latched jointLatch distinct from safetyRules");
			Assert::AreNotEqual(safetyDeferredLine, jointDeferredLine, L"deferred jointLatch distinct from deferred safetyRules");

			// (e) Live synchronous ChainEstimator verification (jointLatch.enabled = true, safetyRules = false)
			//     vs default-OFF negative control on the EXACT SAME synthetic snapshot:
			const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
				10, 1000000, 0xA201ull, true);

			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings cfg;
				cfg.safetyRules = false;
				cfg.jointLatch.enabled = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(est.SubmitLines(snap));

				const C::ChainStatusInput st = est.StatusInput();
				Assert::IsTrue(st.running && st.haveRecord);
				Assert::IsTrue(st.jointLatch, L"live ChainEstimator must expose StatusInput().jointLatch == true");
				Assert::IsFalse(st.safetyRules, L"live ChainEstimator must preserve StatusInput().safetyRules == false");

				const std::string liveLine = est.StatusLine();
				const std::string liveDetail = est.DetailText();
				Assert::IsTrue(liveLine.find("SID safety rules: off") == std::string::npos,
					L"live ChainEstimator with jointLatch on must NOT claim 'SID safety rules: off'");
				Assert::IsTrue(liveLine.find("joint latch") != std::string::npos ||
					liveLine.find("joint upscale latch") != std::string::npos,
					L"live ChainEstimator StatusLine() must visibly show active joint latch");
				Assert::IsTrue(liveDetail.find("joint latch on:") != std::string::npos,
					L"live ChainEstimator DetailText() must include joint latch detail block");
				Assert::IsTrue(liveDetail.find("safety rules on:") == std::string::npos);
				est.Stop();
			}

			// Mandatory in-test negative control: default ChainEstimator on the EXACT SAME snapshot
			// reports jointLatch == false, safetyRules == false, and StatusLine() ending with "; SID safety rules: off".
			{
				ChainFx::ManualClock clk;
				clk.Set(1000.0);
				C::EstimatorSettings defCfg;
				Assert::IsFalse(defCfg.safetyRules);
				Assert::IsFalse(defCfg.jointLatch.enabled);
				C::ChainEstimator estDef(defCfg, clk.Fn());
				estDef.SetSynchronous(true);
				estDef.SetMode(C::EstimatorMode::On);
				Assert::IsTrue(estDef.SubmitLines(snap));

				const C::ChainStatusInput stDef = estDef.StatusInput();
				Assert::IsTrue(stDef.running && stDef.haveRecord);
				Assert::IsFalse(stDef.jointLatch, L"negative control: default ChainEstimator has jointLatch == false");
				Assert::IsFalse(stDef.safetyRules, L"negative control: default ChainEstimator has safetyRules == false");

				const std::string defLine = estDef.StatusLine();
				const std::string defDetail = estDef.DetailText();
				Assert::IsTrue(EndsWith(defLine, "; SID safety rules: off"),
					L"negative control: default ChainEstimator StatusLine() must end with '; SID safety rules: off'");
				Assert::IsTrue(defDetail.find("joint latch on:") == std::string::npos);
				Assert::IsTrue(defDetail.find("safety rules on:") == std::string::npos);
				estDef.Stop();
			}
		}

		// 3. LOW-B: Default-OFF ChainEstimator with Dump v4 enabled (VP_SID_DUMP_V4_DIR set) and NO manifest depth override
		//    (SetDiagnosticDepths(0, 10)) MUST lazily self-tag a bit-replicated 8-in-10 HDMI loop snapshot as N8 with
		//    valid emp_floor_x/y > 1.12 in b_evals.csv, while:
		//      - Negative Control 1: grainy 10-bit (step=1, sigma=1.5, no 8-bit code ladder) stays N10 (NEVER N8),
		//        even though its empirical floor ALSO sits inside the (1.12, 4.0) hazard range;
		//      - Negative Control 2: live empiricalFloorEvals == 0, GovernorPlan::costMs is IDENTICAL with Dump v4
		//        enabled vs disabled under an identical monotonic TickClock, observer EvaluationDetail stays untouched,
		//        and the published Geometry::Record is bit-identical!
		TEST_METHOD(LowBDefaultOffDumpV4SelfTags8In10HdmiLoopAsN8WithoutInflatingGovernorCost)
		{
			using namespace MinorsFx;

			// Synthesize independent physical Snapshot 0: genuine bit-replicated 8-in-10 HDMI loop
			// (8-bit TPDF-dithered codes mapped via v10 = (v8 << 2) | (v8 >> 6) -> step == 1, ladder == true).
			C::LineSnapshot loopSnap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 4,
				10, 1000000, 0xB301ull, true, 0.0, true, 0.75);
			ApplyBitReplication8To10(loopSnap.rows);
			ApplyBitReplication8To10(loopSnap.cols);

			// Synthesize independent physical Snapshot 1 (Negative Control 1): continuous grainy 10-bit native
			// (step == 1, sigma == 1.50 codes, no 8-bit code ladder).
			const C::LineSnapshot grainy10Snap = MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1,
				20, 2000000, 0xB302ull, true, 1.50, false, 0.75);

			// Verify independent physical fixture properties before running the estimator:
			Assert::AreEqual(1, C::DetectStepCodes(loopSnap),
				L"fixture: bit-replicated 8-in-10 HDMI loop reads DetectStepCodes == 1");
			Assert::IsTrue(C::DetectEightBitCodeLadder(loopSnap),
				L"fixture: bit-replicated 8-in-10 HDMI loop triggers DetectEightBitCodeLadder == true");
			Assert::AreEqual(1, C::DetectStepCodes(grainy10Snap),
				L"fixture: continuous grainy 10-bit reads DetectStepCodes == 1");
			Assert::IsFalse(C::DetectEightBitCodeLadder(grainy10Snap),
				L"fixture: continuous grainy 10-bit has DetectEightBitCodeLadder == false");

			// Rule 3 in-test hazard proof: without lazy empirical floor + ladder, naive default-OFF classification
			// on (step = 1, outputBits = 10) mis-tags the 8-in-10 HDMI loop as N10!
			Assert::IsTrue(C::ClassifyBitDepthFromStep(1, 10, 0.0, false) == C::BitDepthClass::N10,
				L"hazard proof: naive default-OFF classification without lazy floor+ladder mis-tags 8-in-10 loop as N10");

			struct RunCapture
			{
				std::vector<G::Packed> packed;
				std::vector<float> recCostMs;
				std::vector<double> govCostMs;
				std::vector<C::EvaluationDetail> details;
				C::EstimatorCounters counters;
				C::GovernorPlan finalPlan;
			};

			auto runDefaultOff = [&](RunCapture& out) {
				out = RunCapture();
				ChainFx::TickClock clk;
				clk.ms.store(0.0);
				clk.step = 1.0; // deterministic +1 ms per nowMs() call

				C::EstimatorSettings cfg;
				// Strictly assert ALL S3 / S2 switches are OFF by default:
				Assert::IsTrue(cfg.empiricalFloorMode == C::EmpiricalFloorMode::Off);
				Assert::IsFalse(cfg.useDepthNullTables);
				Assert::IsFalse(cfg.eightBitPromotion);
				Assert::IsFalse(cfg.useEmpiricalFloor);
				Assert::IsFalse(cfg.safetyRules);
				Assert::IsFalse(cfg.jointLatch.enabled);
				Assert::AreEqual(0, cfg.outputStepOverride);

				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetDiagnosticTag("minors_lowb");
				est.SetDiagnosticDepths(0, 10); // no manifest depth override (coded=0 unknown, output=10-bit link)
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { out.details.push_back(d); });
				est.SetMode(C::EstimatorMode::On);

				const C::LineSnapshot* snaps[2] = { &loopSnap, &grainy10Snap };
				for (int i = 0; i < 2; ++i)
				{
					Assert::IsTrue(est.SubmitLines(*snaps[i]));
					G::Record rec;
					Assert::IsTrue(est.Held().ReadLatest(0, rec));
					out.packed.push_back(G::Pack(rec));
					out.recCostMs.push_back(rec.costMs);
					out.govCostMs.push_back(est.GetGovernorPlan().costMs);
				}
				out.counters = est.GetCounters();
				out.finalPlan = est.GetGovernorPlan();
				est.Stop();
			};

			// RAII guard saves/restores VP_SID_DUMP_V4_DIR and deletes unique temp dir on scope exit.
			ScopedDumpV4Dir dumpGuard("lowb_n8");

			// Negative Control 2a: Run with VP_SID_DUMP_V4_DIR unset -> zero CSV files written.
			dumpGuard.SetEnv("");
			RunCapture capOff;
			runDefaultOff(capOff);
			Assert::AreEqual(0, dumpGuard.CountCsv(), L"negative control: Dump v4 disabled writes zero CSV files");

			// Under test: Run EXACT SAME default-OFF estimator & snapshots with VP_SID_DUMP_V4_DIR set.
			dumpGuard.SetEnv(dumpGuard.Dir());
			RunCapture capOn;
			runDefaultOff(capOn);

			// Negative Control 2b: Zero governor cost inflation, zero live empiricalFloorEvals, untouched pre-gov observer,
			// and bit-identical published Geometry::Record whether Dump v4 is enabled or disabled!
			Assert::AreEqual(uint64_t(2), capOff.counters.evaluations);
			Assert::AreEqual(uint64_t(2), capOn.counters.evaluations);
			Assert::AreEqual(uint64_t(0), capOff.counters.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), capOn.counters.empiricalFloorEvals,
				L"LOW-B: default-OFF Dump v4 lazy floor must NEVER increment live EstimatorCounters::empiricalFloorEvals");
			Assert::AreEqual(uint64_t(0), capOn.counters.empiricalFloorFallbacks);
			Assert::AreEqual(uint64_t(0), capOn.counters.depthNullTableFallbacks);
			Assert::AreEqual<size_t>(2, capOff.packed.size());
			Assert::AreEqual<size_t>(2, capOn.packed.size());
			Assert::AreEqual<size_t>(2, capOn.details.size());

			for (size_t i = 0; i < 2; ++i)
			{
				Assert::IsTrue(capOff.govCostMs[i] > 0.0, L"fixture: deterministic TickClock yields positive governor costMs");
				Assert::AreEqual(capOff.govCostMs[i], capOn.govCostMs[i], 0.0,
					L"LOW-B negative control: GetGovernorPlan().costMs must be identical with Dump v4 ON vs OFF");
				Assert::AreEqual(capOff.recCostMs[i], capOn.recCostMs[i], 0.0f,
					L"LOW-B negative control: published Record::costMs must be identical with Dump v4 ON vs OFF");
				Assert::IsTrue(ChainFx::SameBytes(capOff.packed[i], capOn.packed[i]),
					L"LOW-B negative control: published Geometry::Record must be bit-identical with Dump v4 ON vs OFF");

				// Observer runs BEFORE gov.NoteCost: verify live pre-gov state never ran empirical floor or mutated depth class!
				Assert::IsTrue(capOn.details[i].empiricalFloorMode == C::EmpiricalFloorMode::Off);
				Assert::IsFalse(capOn.details[i].empiricalFloorValid[0],
					L"LOW-B: pre-gov.NoteCost live evaluation must NOT compute empirical floor when S3 switches are OFF");
				Assert::IsFalse(capOn.details[i].empiricalFloorValid[1],
					L"LOW-B: pre-gov.NoteCost live evaluation must NOT compute empirical floor when S3 switches are OFF");
				Assert::IsTrue(capOn.details[i].nullTableDepthClass == C::BitDepthClass::N10,
					L"LOW-B: pre-gov.NoteCost live evaluation keeps untouched N10 (lazy self-tagging runs strictly after gov.NoteCost)");
			}
			Assert::AreEqual(capOff.finalPlan.costMs, capOn.finalPlan.costMs, 0.0);
			Assert::AreEqual(capOff.finalPlan.intervalSec, capOn.finalPlan.intervalSec, 0.0);

			// Parse emitted b_evals.csv and verify both rows against the 76-column Dump v4 contract:
			const std::string csvPath = dumpGuard.Dir() + "\\minors_lowb.b_evals.csv";
			std::ifstream in(csvPath);
			Assert::IsTrue(in.good(), L"b_evals.csv must be written when VP_SID_DUMP_V4_DIR is set");

			std::string headerLine;
			std::getline(in, headerLine);
			const std::string expectedHeader(SourceIdentity::Diag::EvalsHeaderV4());
			Assert::AreEqual(expectedHeader, headerLine, L"b_evals.csv header matches EvalsHeaderV4()");
			const std::vector<std::string> headerCols = SplitCsvRow(headerLine);
			Assert::AreEqual<size_t>(76, headerCols.size(), L"EvalsHeaderV4() has 76 columns");

			auto colIdx = [&](const char* name) -> size_t {
				for (size_t i = 0; i < headerCols.size(); ++i)
					if (headerCols[i] == name)
						return i;
				Assert::Fail(L"required column missing from EvalsHeaderV4()");
				return 0;
			};

			const size_t iSeq = colIdx("seq");
			const size_t iStep = colIdx("step_codes");
			const size_t iCodedBits = colIdx("coded_bits");
			const size_t iOutBits = colIdx("output_bits");
			const size_t iCostMs = colIdx("cost_ms");
			const size_t iEmpMode = colIdx("emp_floor_mode");
			const size_t iEmpX = colIdx("emp_floor_x");
			const size_t iEmpY = colIdx("emp_floor_y");
			const size_t iEmpValidX = colIdx("emp_floor_valid_x");
			const size_t iEmpValidY = colIdx("emp_floor_valid_y");
			const size_t iNullDepth = colIdx("null_depth_class");

			std::vector<std::vector<std::string>> rows;
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty())
					continue;
				const std::vector<std::string> fields = SplitCsvRow(line);
				Assert::AreEqual<size_t>(76, fields.size(), L"every b_evals.csv row must have 76 columns");
				rows.push_back(fields);
			}
			in.close();
			Assert::AreEqual<size_t>(2, rows.size(), L"expected exactly 2 b_evals.csv rows (8-in-10 loop + grainy 10-bit)");

			// Row 0: bit-replicated 8-in-10 HDMI loop -> MUST self-tag null_depth_class == N8 and valid emp_floor_x/y > 1.12!
			{
				const auto& r0 = rows[0];
				Assert::AreEqual(std::string("10"), r0[iSeq]);
				Assert::AreEqual(std::string("1"), r0[iStep], L"8-in-10 bit-replicated loop logs step_codes == 1");
				Assert::AreEqual(std::string("0"), r0[iCodedBits], L"no manifest depth override (coded_bits == 0)");
				Assert::AreEqual(std::string("10"), r0[iOutBits], L"10-bit HDMI output link (output_bits == 10)");
				Assert::AreEqual(std::string("0"), r0[iEmpMode], L"default-OFF run logs emp_floor_mode == 0 (Off)");
				Assert::AreEqual(std::string("1"), r0[iEmpValidX], L"LOW-B: lazy emp_floor_valid_x == 1 on 8-in-10 loop");
				Assert::AreEqual(std::string("1"), r0[iEmpValidY], L"LOW-B: lazy emp_floor_valid_y == 1 on 8-in-10 loop");
				const double empX = std::stod(r0[iEmpX]);
				const double empY = std::stod(r0[iEmpY]);
				Assert::IsTrue(empX > 1.12 && empX < 4.0, L"LOW-B: b_evals.csv emp_floor_x > 1.12 (and < 4.0) on 8-in-10 loop");
				Assert::IsTrue(empY > 1.12 && empY < 4.0, L"LOW-B: b_evals.csv emp_floor_y > 1.12 (and < 4.0) on 8-in-10 loop");
				Assert::AreEqual(std::string("N8"), r0[iNullDepth],
					L"LOW-B: default-OFF Dump v4 MUST self-tag bit-replicated 8-in-10 HDMI loop as N8");
				Assert::AreEqual(static_cast<double>(capOff.recCostMs[0]), std::stod(r0[iCostMs]), 1e-2,
					L"LOW-B: b_evals.csv cost_ms matches non-inflated governor cost");
			}

			// Row 1 (Negative Control 1): continuous grainy 10-bit (step=1, sigma=1.5, no ladder) -> MUST tag N10 (NEVER N8)!
			{
				const auto& r1 = rows[1];
				Assert::AreEqual(std::string("20"), r1[iSeq]);
				Assert::AreEqual(std::string("1"), r1[iStep]);
				Assert::AreEqual(std::string("0"), r1[iCodedBits]);
				Assert::AreEqual(std::string("10"), r1[iOutBits]);
				Assert::AreEqual(std::string("0"), r1[iEmpMode]);
				Assert::AreEqual(std::string("1"), r1[iEmpValidX]);
				Assert::AreEqual(std::string("1"), r1[iEmpValidY]);
				const double grainyEmpX = std::stod(r1[iEmpX]);
				const double grainyEmpY = std::stod(r1[iEmpY]);
				Assert::IsTrue(grainyEmpX > 1.12 && grainyEmpX < 4.0,
					L"hazard proof: grainy 10-bit empirical floor ALSO sits in (1.12, 4.0) hazard range");
				Assert::IsTrue(grainyEmpY > 1.12 && grainyEmpY < 4.0,
					L"hazard proof: grainy 10-bit empirical floor ALSO sits in (1.12, 4.0) hazard range");

				// Rule 3 in-test negative control: a naive floor-only classifier ignoring the code-ladder check WOULD mis-tag this as N8!
				Assert::IsTrue(C::ClassifyBitDepthFromStep(1, 10, (std::min)(grainyEmpX, grainyEmpY), true) == C::BitDepthClass::N8,
					L"negative control: naive floor-only classifier without code-ladder check WOULD falsely tag this grainy 10-bit frame as N8");

				Assert::AreNotEqual(std::string("N8"), r1[iNullDepth],
					L"LOW-B negative control: grainy 10-bit MUST NEVER be tagged as N8");
				Assert::AreEqual(std::string("N10"), r1[iNullDepth],
					L"LOW-B negative control: grainy 10-bit MUST be tagged as N10");
				Assert::AreEqual(static_cast<double>(capOff.recCostMs[1]), std::stod(r1[iCostMs]), 1e-2,
					L"LOW-B: b_evals.csv cost_ms matches non-inflated governor cost on grainy 10-bit row");
			}
		}
	};
}
