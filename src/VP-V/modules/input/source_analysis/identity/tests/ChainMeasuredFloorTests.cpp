/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team adversarial unit and shadow-integration tests for U#1 (PKG-SRCID-FLOOR / GEM-083):
//   - ChainMeasuredFloorTests: 2nd-difference robust MAD noise-floor estimator (MeasuredFloorCodes in ChainLines.h):
//     quantization floor on constant/linear ramps, unbiased white Gaussian sigma recovery, step-edge impulse rejection,
//     strict line-boundary isolation, < 64 second-diff NaN threshold, linear unit/bit-depth scaling (8/10/12-bit & Q6),
//     rows-only / cols-only symmetry and mismatched buffer safety.
//   - ChainFloorShadowTests: shadow-only wiring in ChainEstimator and Dump v4:
//     zero hot-path execution when neither observer nor diagEvals is active, exact shadow EvaluationDetail population,
//     bit-identical published SourceGeometry::Record outputs and untouched PowerProbe/evidence active floorCodes,
//     Dump v4 b_evals header/row contract ending with ",floor_measured", and untouched CAL_DUMP v3 27-column contract.
// Pure CPU, deterministic fixed-seed LCGs, no wall clock, RAII env/temp cleanup.
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"
#include "ChainEvalSeam.h"

#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/input/source_analysis/identity/SidDiagDump.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainLines.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;
		namespace D = SourceIdentity::Diag;
		using namespace ChainFx;

		// Deterministic Box-Muller PRNG (fixed-seed LCG, no random_device, no clock).
		struct DetRng
		{
			uint32_t state;
			explicit DetRng(uint32_t seed) : state(seed) {}
			double Uniform()
			{
				state = state * 1664525u + 1013904223u;
				return static_cast<double>(state >> 8) / 16777216.0;
			}
			double Normal()
			{
				double u1 = Uniform();
				if (u1 < 1e-12)
					u1 = 1e-12;
				const double u2 = Uniform();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * C::kPi * u2);
			}
		};

		inline uint16_t RoundU16(double v)
		{
			const double c = std::floor(v + 0.5);
			return static_cast<uint16_t>(c < 0.0 ? 0.0 : (c > 65535.0 ? 65535.0 : c));
		}

		// Builds a structurally consistent LineSnapshot with explicit row/col geometry.
		inline C::LineSnapshot MakeSnapshot(int rowCount, int rowLen, int colCount, int colLen)
		{
			C::LineSnapshot s;
			s.active.x = 0;
			s.active.y = 0;
			s.active.w = rowLen > 0 ? rowLen : 1;
			s.active.h = colLen > 0 ? colLen : 1;
			s.inner.x = 0;
			s.inner.y = 0;
			s.inner.w = rowLen;
			s.inner.h = colLen;
			s.rowCount = rowCount;
			s.colCount = colCount;
			if (rowCount > 0)
			{
				s.rowY.resize(static_cast<size_t>(rowCount));
				for (int j = 0; j < rowCount; ++j)
					s.rowY[static_cast<size_t>(j)] = j;
				if (rowLen > 0)
					s.rows.assign(static_cast<size_t>(rowCount) * static_cast<size_t>(rowLen), 0);
			}
			if (colCount > 0)
			{
				s.colX.resize(static_cast<size_t>(colCount));
				for (int j = 0; j < colCount; ++j)
					s.colX[static_cast<size_t>(j)] = j;
				if (colLen > 0)
					s.cols.assign(static_cast<size_t>(colCount) * static_cast<size_t>(colLen), 0);
			}
			return s;
		}

		// Collects interior 2nd differences per line (for in-test naive/unprotected negative controls only).
		inline std::vector<double> CollectInteriorD2(const C::LineSnapshot& s)
		{
			std::vector<double> d2;
			if (s.rowCount > 0 && s.inner.w >= 3 && s.rows.size() == static_cast<size_t>(s.rowCount) * static_cast<size_t>(s.inner.w))
			{
				for (int j = 0; j < s.rowCount; ++j)
				{
					const uint16_t* line = &s.rows[static_cast<size_t>(j) * static_cast<size_t>(s.inner.w)];
					for (int i = 1; i + 1 < s.inner.w; ++i)
						d2.push_back(static_cast<double>(line[i - 1]) - 2.0 * static_cast<double>(line[i]) + static_cast<double>(line[i + 1]));
				}
			}
			if (s.colCount > 0 && s.inner.h >= 3 && s.cols.size() == static_cast<size_t>(s.colCount) * static_cast<size_t>(s.inner.h))
			{
				for (int j = 0; j < s.colCount; ++j)
				{
					const uint16_t* line = &s.cols[static_cast<size_t>(j) * static_cast<size_t>(s.inner.h)];
					for (int i = 1; i + 1 < s.inner.h; ++i)
						d2.push_back(static_cast<double>(line[i - 1]) - 2.0 * static_cast<double>(line[i]) + static_cast<double>(line[i + 1]));
				}
			}
			return d2;
		}

		// Naive flattened-buffer 2nd differences (ignores line boundaries; negative control only).
		inline std::vector<double> CollectFlattenedD2(const std::vector<uint16_t>& buf)
		{
			std::vector<double> d2;
			if (buf.size() < 3)
				return d2;
			d2.reserve(buf.size() - 2);
			for (size_t i = 1; i + 1 < buf.size(); ++i)
				d2.push_back(static_cast<double>(buf[i - 1]) - 2.0 * static_cast<double>(buf[i]) + static_cast<double>(buf[i + 1]));
			return d2;
		}

		inline double RmsOf(const std::vector<double>& v)
		{
			if (v.empty())
				return 0.0;
			double acc = 0.0;
			for (size_t i = 0; i < v.size(); ++i)
				acc += v[i] * v[i];
			return std::sqrt(acc / static_cast<double>(v.size()));
		}

		// Textured + Gaussian-noise luma plane (quantized to `step` codes) so verifier, probe, and evidence do real work.
		inline Plane MakeNoisyTexturedPlane(int w, int h, uint32_t seed, double noiseSigma, int step)
		{
			Plane p;
			p.w = w;
			p.h = h;
			p.luma.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
			DetRng rng(seed);
			const int gw = w / 8 + 2;
			const int gh = h / 8 + 2;
			std::vector<double> grid(static_cast<size_t>(gw) * static_cast<size_t>(gh));
			for (size_t i = 0; i < grid.size(); ++i)
				grid[i] = rng.Uniform() - 0.5;
			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const double fx = static_cast<double>(x) / 8.0;
					const double fy = static_cast<double>(y) / 8.0;
					const int ix = static_cast<int>(fx);
					const int iy = static_cast<int>(fy);
					const double ax = fx - ix;
					const double ay = fy - iy;
					const double v00 = grid[static_cast<size_t>(iy) * static_cast<size_t>(gw) + static_cast<size_t>(ix)];
					const double v10 = grid[static_cast<size_t>(iy) * static_cast<size_t>(gw) + static_cast<size_t>(ix + 1)];
					const double v01 = grid[static_cast<size_t>(iy + 1) * static_cast<size_t>(gw) + static_cast<size_t>(ix)];
					const double v11 = grid[static_cast<size_t>(iy + 1) * static_cast<size_t>(gw) + static_cast<size_t>(ix + 1)];
					const double lf = (v00 * (1.0 - ax) + v10 * ax) * (1.0 - ay) + (v01 * (1.0 - ax) + v11 * ax) * ay;
					double v = 500.0 + 260.0 * lf + noiseSigma * rng.Normal();
					v = std::floor(v / static_cast<double>(step) + 0.5) * static_cast<double>(step);
					if (v < 64.0) v = 64.0;
					if (v > 940.0) v = 940.0;
					p.luma[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = static_cast<uint16_t>(v);
				}
			}
			return p;
		}

		inline C::LineSnapshot CapturePlaneSnapshot(const Plane& p, uint64_t seq, double seconds)
		{
			C::LineRect active;
			active.x = 0;
			active.y = 0;
			active.w = p.w;
			active.h = p.h;
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"line sampler plan must be valid for fixture plane");
			C::LineSnapshot snap;
			Assert::IsTrue(C::CaptureLines<uint16_t>(plan, p.luma.data(), p.w, 1.0, snap), L"CaptureLines succeeded");
			snap.seq = seq;
			snap.timeUs = static_cast<int64_t>(std::llround(seconds * 1e6));
			snap.rasterW = p.w;
			snap.rasterH = p.h;
			return snap;
		}

		inline std::vector<std::string> SplitCsv(const std::string& line)
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

		inline bool ParseDouble(const std::string& s, double& out)
		{
			if (s.empty())
				return false;
			char* end = nullptr;
			const double v = std::strtod(s.c_str(), &end);
			if (end == s.c_str() || *end != '\0')
				return false;
			out = v;
			return true;
		}

		inline std::string MakeTempSubdir(const char* name)
		{
			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			const std::string dir = std::string(temp) + name;
			CreateDirectoryA(dir.c_str(), nullptr);
			return dir;
		}

		inline void ClearCsvInDir(const std::string& dir)
		{
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA((dir + "\\*.csv").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE)
				return;
			do
			{
				DeleteFileA((dir + "\\" + fd.cFileName).c_str());
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}

		// RAII scope clearing diagnostic dump env vars even if an assertion throws.
		struct DiagEnvGuard
		{
			DiagEnvGuard()
			{
				_putenv_s("VP_SID_DUMP_V4_DIR", "");
				_putenv_s("VP_SOURCE_ID_CAL_DUMP", "");
			}
			~DiagEnvGuard()
			{
				_putenv_s("VP_SID_DUMP_V4_DIR", "");
				_putenv_s("VP_SOURCE_ID_CAL_DUMP", "");
			}
		};
	}

	TEST_CLASS(ChainMeasuredFloorTests)
	{
	public:
		TEST_METHOD(ConstantLinesHitQuantizationFloor)
		{
			const double qFloor1 = 1.0 / std::sqrt(12.0);
			const double qFloor4 = 4.0 / std::sqrt(12.0);

			// Constant snapshot: 4 rows x 32 samples => 4 * 30 = 120 >= 64 second diffs, all identically 0.
			C::LineSnapshot snapConst = MakeSnapshot(4, 32, 0, 0);
			for (size_t i = 0; i < snapConst.rows.size(); ++i)
				snapConst.rows[i] = 512;

			Assert::AreEqual(qFloor1, C::MeasuredFloorCodes(snapConst, 1.0), 1e-12,
				L"constant lines must hit exact 1/sqrt(12) quantization floor for stepCodes=1.0");
			Assert::AreEqual(qFloor4, C::MeasuredFloorCodes(snapConst, 4.0), 1e-12,
				L"constant lines must hit exact 4/sqrt(12) quantization floor for stepCodes=4.0");

			// Exact linear ramp snapshot (both rows & cols): d2 == 0 everywhere, 240 >= 64 diffs.
			C::LineSnapshot snapRamp = MakeSnapshot(4, 32, 4, 32);
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 32; ++i)
				{
					snapRamp.rows[static_cast<size_t>(j) * 32u + static_cast<size_t>(i)] = static_cast<uint16_t>(100 + 2 * i);
					snapRamp.cols[static_cast<size_t>(j) * 32u + static_cast<size_t>(i)] = static_cast<uint16_t>(200 + 4 * i);
				}

			Assert::AreEqual(qFloor1, C::MeasuredFloorCodes(snapRamp, 1.0), 1e-12,
				L"linear ramp has zero 2nd differences and must hit 1/sqrt(12) for stepCodes=1.0");
			Assert::AreEqual(qFloor4, C::MeasuredFloorCodes(snapRamp, 4.0), 1e-12,
				L"linear ramp has zero 2nd differences and must hit 4/sqrt(12) for stepCodes=4.0");

			// In-test negative controls on the exact same snapshots:
			// (1) Un-floored MAD(d2) returns 0.0 (violating the quantization floor);
			// (2) Naive 1st-difference MAD on snapRamp is inflated by the linear slope (>= 2.0 >> qFloor1).
			const std::vector<double> d2Ramp = CollectInteriorD2(snapRamp);
			const double unfloored = RobustStats::MadSigma(RobustStats::Mad(d2Ramp)) / std::sqrt(6.0);
			Assert::AreEqual(0.0, unfloored, 1e-12, L"negative control: un-floored MAD(d2) collapses to 0 on linear ramp");
			std::vector<double> d1Abs;
			for (int j = 0; j < 4; ++j)
				for (int i = 1; i < 32; ++i)
				{
					const size_t idx = static_cast<size_t>(j) * 32u + static_cast<size_t>(i);
					d1Abs.push_back(std::fabs(static_cast<double>(snapRamp.rows[idx]) - static_cast<double>(snapRamp.rows[idx - 1])));
					d1Abs.push_back(std::fabs(static_cast<double>(snapRamp.cols[idx]) - static_cast<double>(snapRamp.cols[idx - 1])));
				}
			const double naiveD1Sigma = RobustStats::MadSigma(RobustStats::Median(d1Abs)) / std::sqrt(2.0);
			Assert::IsTrue(naiveD1Sigma > 2.0, L"negative control: 1st-difference estimator is inflated by linear slope");
		}

		TEST_METHOD(WhiteGaussianNoiseMatchesKnownSigma)
		{
			// 16 rows x 512 samples => 8160 second diffs, known i.i.d. Gaussian sigma = 6.0 codes.
			const double trueSigma = 6.0;
			C::LineSnapshot snap = MakeSnapshot(16, 512, 0, 0);
			DetRng rng(20261125u);
			for (size_t i = 0; i < snap.rows.size(); ++i)
				snap.rows[i] = RoundU16(512.0 + trueSigma * rng.Normal());

			const double est = C::MeasuredFloorCodes(snap, 1.0);
			Assert::IsTrue(std::isfinite(est), L"estimated floor must be finite");
			Assert::IsTrue(est >= 5.4 && est <= 6.6,
				L"MeasuredFloorCodes must recover known sigma=6.0 within 10% ([5.4, 6.6])");

			// In-test negative control on the exact same snapshot:
			// un-scaled MadSigma (missing / sqrt(6)) overestimates by ~sqrt(6) (~2.45x -> ~14.7 > 12.0).
			const std::vector<double> d2 = CollectInteriorD2(snap);
			Assert::AreEqual<size_t>(16u * 510u, d2.size());
			const double unscaledMadSigma = RobustStats::MadSigma(RobustStats::Mad(d2));
			Assert::IsTrue(unscaledMadSigma > 12.0,
				L"negative control: omitting /sqrt(6) overestimates white Gaussian sigma by ~2.45x (> 12.0)");
		}

		TEST_METHOD(StepEdgesDoNotInflateFloor)
		{
			// Clean piecewise-constant step edges (600-code jumps every 64 px, flat interiors):
			// 8 rows x 512 samples => 4080 second diffs, ~97.3% zero, ~2.7% sparse +-600 impulses.
			C::LineSnapshot snap = MakeSnapshot(8, 512, 0, 0);
			for (int j = 0; j < 8; ++j)
				for (int i = 0; i < 512; ++i)
					snap.rows[static_cast<size_t>(j) * 512u + static_cast<size_t>(i)] = (((i / 64) % 2) == 0) ? 200u : 800u;

			const double qFloor1 = 1.0 / std::sqrt(12.0);
			const double qFloor4 = 4.0 / std::sqrt(12.0);
			Assert::AreEqual(qFloor1, C::MeasuredFloorCodes(snap, 1.0), 1e-12,
				L"robust MAD must reject sparse step-edge impulses and hit quantization floor (step=1)");
			Assert::AreEqual(qFloor4, C::MeasuredFloorCodes(snap, 4.0), 1e-12,
				L"robust MAD must reject sparse step-edge impulses and hit quantization floor (step=4)");

			// In-test negative control on the exact same snapshot:
			// RMS of d2 / sqrt(6) is severely inflated (~40.6 >> 5.0) by the sparse step impulses.
			const std::vector<double> d2 = CollectInteriorD2(snap);
			const double naiveRms = RmsOf(d2) / std::sqrt(6.0);
			Assert::IsTrue(naiveRms > 5.0,
				L"negative control: non-robust RMS of d2/sqrt(6) on the exact same step-edge snapshot must be >> 5.0");
		}

		TEST_METHOD(NeverCrossesLineBoundaries)
		{
			const double qFloor1 = 1.0 / std::sqrt(12.0);

			// Part A: 32 rows x 5 samples (32 * 3 = 96 >= 64 interior diffs), distinct constant row j = j * 30.
			C::LineSnapshot snap32x5 = MakeSnapshot(32, 5, 0, 0);
			for (int j = 0; j < 32; ++j)
			{
				const uint16_t val = static_cast<uint16_t>(j * 30);
				for (int i = 0; i < 5; ++i)
					snap32x5.rows[static_cast<size_t>(j) * 5u + static_cast<size_t>(i)] = val;
			}
			Assert::AreEqual(qFloor1, C::MeasuredFloorCodes(snap32x5, 1.0), 1e-12,
				L"32 rows x 5 constant lines must never cross row boundaries and must hit quantization floor");

			// Negative control on snap32x5: flattened d2 without line boundaries injects 62 spurious +-30 spikes.
			const std::vector<double> flat32x5 = CollectFlattenedD2(snap32x5.rows);
			Assert::IsTrue(RmsOf(flat32x5) / std::sqrt(6.0) > 5.0,
				L"negative control: flattened d2 across 32x5 buffer creates huge boundary spikes");

			// Part B: 36 rows x 3 samples + 36 cols x 3 samples (72 >= 64 interior diffs, all identically 0),
			// where >66% of flattened-buffer d2 entries straddle line boundaries so even flattened MAD explodes!
			C::LineSnapshot snapLen3 = MakeSnapshot(36, 3, 36, 3);
			for (int j = 0; j < 36; ++j)
			{
				const uint16_t val = static_cast<uint16_t>(64 + j * 24);
				for (int i = 0; i < 3; ++i)
				{
					snapLen3.rows[static_cast<size_t>(j) * 3u + static_cast<size_t>(i)] = val;
					snapLen3.cols[static_cast<size_t>(j) * 3u + static_cast<size_t>(i)] = val;
				}
			}
			Assert::AreEqual(qFloor1, C::MeasuredFloorCodes(snapLen3, 1.0), 1e-12,
				L"36 rows x 3 + 36 cols x 3 constant lines must hit exact quantization floor without crossing boundaries");

			// Strongest same-input negative control: computing MAD(d2) on flattened rows+cols without line boundaries
			// produces MAD == 24.0 => MadSigma/sqrt(6) ~ 14.5 >> floor!
			std::vector<double> flatCombined = CollectFlattenedD2(snapLen3.rows);
			const std::vector<double> flatCols = CollectFlattenedD2(snapLen3.cols);
			flatCombined.insert(flatCombined.end(), flatCols.begin(), flatCols.end());
			const double naiveFlatMadFloor = RobustStats::MadSigma(RobustStats::Mad(flatCombined)) / std::sqrt(6.0);
			Assert::IsTrue(naiveFlatMadFloor > 10.0,
				L"negative control: flattened-buffer MAD(d2)/sqrt(6) without line boundaries explodes (>> floor)");
		}

		TEST_METHOD(TooFewSecondDiffsReturnsNaN)
		{
			// Empty snapshot => NaN.
			const C::LineSnapshot empty;
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(empty, 1.0)), L"empty snapshot must return NaN");

			// Lines of length 2 have 0 second diffs regardless of line count => NaN.
			C::LineSnapshot len2 = MakeSnapshot(40, 2, 0, 0);
			for (size_t i = 0; i < len2.rows.size(); ++i)
				len2.rows[i] = 400;
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(len2, 1.0)), L"lines shorter than 3 samples have 0 diffs -> NaN");

			// 2 rows x 20 samples => 36 < 64 diffs => NaN.
			C::LineSnapshot snap36 = MakeSnapshot(2, 20, 0, 0);
			for (size_t i = 0; i < snap36.rows.size(); ++i)
				snap36.rows[i] = 400;
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(snap36, 1.0)), L"36 diffs (< 64) must return NaN");

			// Exact sub-threshold boundary: 1 row x 65 samples => 63 < 64 diffs => NaN.
			C::LineSnapshot snap63 = MakeSnapshot(1, 65, 0, 0);
			for (size_t i = 0; i < snap63.rows.size(); ++i)
				snap63.rows[i] = 400;
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(snap63, 1.0)), L"63 diffs (< 64) must return NaN");

			// Threshold boundary negative/positive control: 2 rows x 34 samples => exact 64 diffs => finite!
			C::LineSnapshot snap64 = MakeSnapshot(2, 34, 0, 0);
			for (size_t i = 0; i < snap64.rows.size(); ++i)
				snap64.rows[i] = 400;
			const double v64 = C::MeasuredFloorCodes(snap64, 1.0);
			Assert::IsFalse(std::isnan(v64), L"exact 64 diffs must NOT return NaN");
			Assert::IsTrue(std::isfinite(v64), L"exact 64 diffs must return a finite value");
			Assert::AreEqual(1.0 / std::sqrt(12.0), v64, 1e-12, L"exact 64 constant diffs must return quantization floor");
		}

		TEST_METHOD(StepAndBitDepthScaling)
		{
			// Noise-dominated scaling: 10-bit (x1, step=1), 8-bit in 10-bit (x4, step=4),
			// Q6 12-bit (x16, step=16), Q6 10-bit (x64, step=64) — all values stay <= 250 * 64 = 16000 <= 65535.
			C::LineSnapshot snap1 = MakeSnapshot(16, 256, 0, 0);
			DetRng rng(424242u);
			for (size_t i = 0; i < snap1.rows.size(); ++i)
			{
				double v = 200.0 + 6.0 * rng.Normal();
				if (v < 64.0) v = 64.0;
				if (v > 250.0) v = 250.0;
				snap1.rows[i] = RoundU16(v);
			}

			auto scaleSnap = [](const C::LineSnapshot& src, uint16_t factor) {
				C::LineSnapshot out = src;
				for (size_t i = 0; i < out.rows.size(); ++i)
					out.rows[i] = static_cast<uint16_t>(out.rows[i] * factor);
				for (size_t i = 0; i < out.cols.size(); ++i)
					out.cols[i] = static_cast<uint16_t>(out.cols[i] * factor);
				return out;
			};

			const C::LineSnapshot snap4 = scaleSnap(snap1, 4u);
			const C::LineSnapshot snap16 = scaleSnap(snap1, 16u);
			const C::LineSnapshot snap64 = scaleSnap(snap1, 64u);

			const double f1 = C::MeasuredFloorCodes(snap1, 1.0);
			const double f4 = C::MeasuredFloorCodes(snap4, 4.0);
			const double f16 = C::MeasuredFloorCodes(snap16, 16.0);
			const double f64 = C::MeasuredFloorCodes(snap64, 64.0);

			Assert::IsTrue(f1 > 4.5 && f1 < 7.5, L"base 10-bit noisy snapshot must be noise-dominated near sigma=6");
			Assert::AreEqual(4.0 * f1, f4, 1e-9, L"scaling samples & step by 4x (8-bit vs 10-bit) must scale floor by exact 4x");
			Assert::AreEqual(16.0 * f1, f16, 1e-9, L"scaling samples & step by 16x (Q6 12-bit) must scale floor by exact 16x");
			Assert::AreEqual(64.0 * f1, f64, 1e-9, L"scaling samples & step by 64x (Q6 10-bit) must scale floor by exact 64x");
			Assert::AreEqual(4.0 * f16, f64, 1e-9, L"Q6 10-bit vs Q6 12-bit must scale by exact 4x");

			// Quantization-floor-dominated linear scaling on constant snapshots.
			C::LineSnapshot c1 = MakeSnapshot(4, 32, 0, 0);
			for (size_t i = 0; i < c1.rows.size(); ++i)
				c1.rows[i] = 100u;
			const C::LineSnapshot c4 = scaleSnap(c1, 4u);
			const C::LineSnapshot c16 = scaleSnap(c1, 16u);
			const C::LineSnapshot c64 = scaleSnap(c1, 64u);

			const double q1 = C::MeasuredFloorCodes(c1, 1.0);
			const double q4 = C::MeasuredFloorCodes(c4, 4.0);
			const double q16 = C::MeasuredFloorCodes(c16, 16.0);
			const double q64 = C::MeasuredFloorCodes(c64, 64.0);
			Assert::AreEqual(4.0 * q1, q4, 1e-12, L"constant quantization floor scales by exact 4x");
			Assert::AreEqual(16.0 * q1, q16, 1e-12, L"constant quantization floor scales by exact 16x");
			Assert::AreEqual(64.0 * q1, q64, 1e-12, L"constant quantization floor scales by exact 64x");

			// In-test negative control: a hardcoded 10-bit stepCodes=1.0 on the 4x-scaled constant snapshot
			// underestimates the 8-bit quantization floor by 4x (violating linear scaling).
			const double wrongStepFloor = C::MeasuredFloorCodes(c4, 1.0);
			Assert::IsTrue(wrongStepFloor < 0.3 * q4,
				L"negative control: passing unscaled stepCodes=1.0 on 4x-scaled constant data fails 4x scaling");
		}

		TEST_METHOD(RowsOnlyOrColsOnlyHandledCleanly)
		{
			// Build identical noisy samples in a rows-only snapshot and a cols-only snapshot (8 x 64 => 496 >= 64 diffs).
			DetRng rng(777888u);
			std::vector<uint16_t> samples(8u * 64u);
			for (size_t i = 0; i < samples.size(); ++i)
				samples[i] = RoundU16(500.0 + 5.0 * rng.Normal());

			C::LineSnapshot rowsOnly = MakeSnapshot(8, 64, 0, 0);
			rowsOnly.rows = samples;
			C::LineSnapshot colsOnly = MakeSnapshot(0, 0, 8, 64);
			colsOnly.cols = samples;

			const double fRows = C::MeasuredFloorCodes(rowsOnly, 1.0);
			const double fCols = C::MeasuredFloorCodes(colsOnly, 1.0);
			Assert::IsTrue(std::isfinite(fRows) && fRows > 3.5 && fRows < 6.5, L"rows-only snapshot recovers noise floor");
			Assert::IsTrue(std::isfinite(fCols) && fCols > 3.5 && fCols < 6.5, L"cols-only snapshot recovers noise floor");
			Assert::AreEqual(fRows, fCols, 1e-12, L"rows-only and cols-only on identical line data must agree identically");

			// Mismatched buffer sizes / negative dimensions must return NaN cleanly with zero OOB reads.
			C::LineSnapshot rowsTruncated = rowsOnly;
			rowsTruncated.rows.pop_back();
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(rowsTruncated, 1.0)),
				L"truncated rows buffer must return NaN without OOB");

			C::LineSnapshot rowsOversized = rowsOnly;
			rowsOversized.rows.push_back(500u);
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(rowsOversized, 1.0)),
				L"oversized rows buffer must return NaN");

			C::LineSnapshot colsTruncated = colsOnly;
			colsTruncated.cols.pop_back();
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(colsTruncated, 1.0)),
				L"truncated cols buffer must return NaN without OOB");

			C::LineSnapshot colsOversized = colsOnly;
			colsOversized.cols.push_back(500u);
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(colsOversized, 1.0)),
				L"oversized cols buffer must return NaN");

			C::LineSnapshot negRows = rowsOnly;
			negRows.rowCount = -2;
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(negRows, 1.0)),
				L"negative rowCount must return NaN");

			C::LineSnapshot negWidth = rowsOnly;
			negWidth.inner.w = -64;
			Assert::IsTrue(std::isnan(C::MeasuredFloorCodes(negWidth, 1.0)),
				L"negative inner.w must return NaN");
		}
	};

	TEST_CLASS(ChainFloorShadowTests)
	{
	public:
		TEST_METHOD(ShadowComputationRunsOnlyWhenObserverOrDiagOpen)
		{
			DiagEnvGuard envGuard;

			const Plane plane10 = MakeNoisyTexturedPlane(400, 300, 101u, 6.0, 1);
			const Plane plane8 = MakeNoisyTexturedPlane(400, 300, 202u, 8.0, 4);
			const C::LineSnapshot snap10 = CapturePlaneSnapshot(plane10, 10u, 1.0);
			const C::LineSnapshot snap8 = CapturePlaneSnapshot(plane8, 40u, 2.5);
			Assert::AreEqual(1, C::DetectStepCodes(snap10), L"10-bit plane detected as step=1");
			Assert::AreEqual(4, C::DetectStepCodes(snap8), L"8-bit-in-10-bit plane detected as step=4");

			const double expected10 = C::MeasuredFloorCodes(snap10, 1.0);
			const double expected8 = C::MeasuredFloorCodes(snap8, 4.0);
			Assert::IsTrue(expected10 > 3.0, L"10-bit noisy snapshot measured floor is well above active 0.289");
			Assert::IsTrue(expected8 > 4.0, L"8-bit noisy snapshot measured floor is well above active 1.155");

			// 1. Neither observer nor diagEvals open => shadow computation must NOT run (floorMeasured counter == 0).
			{
				ManualClock clk;
				C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::Shadow);
				Assert::IsTrue(est.SubmitLines(snap10));
				clk.Set(1500.0);
				Assert::IsTrue(est.SubmitLines(snap8));
				const C::EstimatorCounters cnt = est.GetCounters();
				Assert::AreEqual<uint64_t>(2u, cnt.evaluations, L"both evaluations ran");
				Assert::AreEqual<uint64_t>(0u, cnt.floorMeasured,
					L"negative control: without observer or diagEvals open, shadow floorMeasured must stay 0");
				est.Stop();
			}

			// 2. Observer attached => shadow computation runs once per evaluation and populates EvaluationDetail.
			{
				ManualClock clk;
				C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
				est.SetSynchronous(true);
				std::vector<C::EvaluationDetail> details;
				est.SetEvaluationObserver([&details](const C::EvaluationDetail& d) { details.push_back(d); });
				est.SetMode(C::EstimatorMode::Shadow);

				Assert::IsTrue(est.SubmitLines(snap10));
				Assert::AreEqual<uint64_t>(1u, est.GetCounters().floorMeasured,
					L"observer attached: floorMeasured counter increments to 1 after first eval");
				Assert::AreEqual<size_t>(1u, details.size());
				Assert::AreEqual(1, details[0].stepCodes);
				Assert::AreEqual(0.289, details[0].floorCodes, 1e-12, L"active floorCodes stays 0.289 for step=1");
				Assert::AreEqual(expected10, details[0].floorMeasured, 1e-12,
					L"detail.floorMeasured must equal MeasuredFloorCodes(snap10, 1.0)");

				clk.Set(1500.0);
				Assert::IsTrue(est.SubmitLines(snap8));
				Assert::AreEqual<uint64_t>(2u, est.GetCounters().floorMeasured,
					L"observer attached: floorMeasured counter increments to 2 after second eval");
				Assert::AreEqual<size_t>(2u, details.size());
				Assert::AreEqual(4, details[1].stepCodes);
				Assert::AreEqual(1.155, details[1].floorCodes, 1e-12, L"active floorCodes stays 1.155 for step=4");
				Assert::AreEqual(expected8, details[1].floorMeasured, 1e-12,
					L"detail.floorMeasured must equal MeasuredFloorCodes(snap8, 4.0)");
				est.Stop();
			}

			// 3. diagEvals open WITHOUT observer => shadow computation also runs for Dump v4.
			{
				const std::string dir = MakeTempSubdir("sid_floor_shadow_gate_v4");
				ClearCsvInDir(dir);
				_putenv_s("VP_SID_DUMP_V4_DIR", dir.c_str());
				uint64_t measuredCount = 0;
				{
					ManualClock clk;
					C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
					est.SetSynchronous(true);
					est.SetDiagnosticTag("diag_only");
					est.SetMode(C::EstimatorMode::Shadow);
					Assert::IsTrue(est.SubmitLines(snap10));
					measuredCount = est.GetCounters().floorMeasured;
					est.Stop();
				}
				_putenv_s("VP_SID_DUMP_V4_DIR", "");
				Assert::AreEqual<uint64_t>(1u, measuredCount,
					L"diagEvals open without observer must still compute shadow floorMeasured");
				ClearCsvInDir(dir);
				RemoveDirectoryA(dir.c_str());
			}
		}

		TEST_METHOD(ActiveDecisionsAndPowerProbeFloorAreByteIdentical)
		{
			DiagEnvGuard envGuard;

			// Sequence of noisy 10-bit snapshots (sigma = 8.0 >> active 0.289) plus 8-bit snapshots (sigma = 12.0 >> 1.155).
			std::vector<C::LineSnapshot> seq10;
			for (int i = 0; i < 6; ++i)
			{
				const Plane p = MakeNoisyTexturedPlane(400, 300, 500u + static_cast<uint32_t>(i), 8.0, 1);
				C::LineSnapshot s = CapturePlaneSnapshot(p, 100u + 30u * static_cast<uint64_t>(i), 1.0 + 1.2 * i);
				if (i == 3)
				{
					s.flags |= C::kEvCut;
					s.cuts = 1;
				}
				Assert::AreEqual(1, C::DetectStepCodes(s));
				seq10.push_back(s);
			}

			ManualClock clkA, clkB;
			C::ChainEstimator estNoObs(C::EstimatorSettings(), clkA.Fn());
			C::ChainEstimator estWithObs(C::EstimatorSettings(), clkB.Fn());
			estNoObs.SetSynchronous(true);
			estWithObs.SetSynchronous(true);

			std::vector<C::EvaluationDetail> details;
			estWithObs.SetEvaluationObserver([&details](const C::EvaluationDetail& d) { details.push_back(d); });

			estNoObs.SetMode(C::EstimatorMode::Shadow);
			estWithObs.SetMode(C::EstimatorMode::Shadow);

			for (size_t i = 0; i < seq10.size(); ++i)
			{
				clkA.Set(1200.0 * static_cast<double>(i));
				clkB.Set(1200.0 * static_cast<double>(i));
				Assert::IsTrue(estNoObs.SubmitLines(seq10[i]));
				Assert::IsTrue(estWithObs.SubmitLines(seq10[i]));

				G::Record recNoObs, recWithObs;
				Assert::IsTrue(estNoObs.Held().ReadLatest(0, recNoObs), L"estNoObs published a record");
				Assert::IsTrue(estWithObs.Held().ReadLatest(0, recWithObs), L"estWithObs published a record");
				Assert::IsTrue(SameBytes(G::Pack(recNoObs), G::Pack(recWithObs)),
					L"published SourceGeometry::Record must be bit-identical with and without shadow floor computation");
				Assert::AreEqual(estNoObs.StatusLine(), estWithObs.StatusLine(),
					L"status line must be identical");

				Assert::AreEqual<size_t>(i + 1u, details.size());
				const C::EvaluationDetail& d = details[i];
				Assert::IsTrue(d.axis[0].have && d.axis[1].have, L"both axes evaluated");
				Assert::AreEqual(0.289, d.floorCodes, 1e-12, L"active floorCodes remains 0.289");
				Assert::AreEqual(d.floorCodes, d.axis[0].floorCodes, 1e-12,
					L"axis 0 floorCodes (PowerProbe / evidence) must remain untouched active floor");
				Assert::AreEqual(d.floorCodes, d.axis[1].floorCodes, 1e-12,
					L"axis 1 floorCodes (PowerProbe / evidence) must remain untouched active floor");
				// Negative control proving hazard is real: measured noise floor is > 20x higher than active 0.289.
				Assert::IsTrue(d.floorMeasured > 20.0 * d.floorCodes,
					L"negative control: input sigma=8.0 yields floorMeasured > 20x active floor (leakage would diverge)");
				Assert::AreEqual(C::MeasuredFloorCodes(seq10[i], 1.0), d.floorMeasured, 1e-12);
			}

			Assert::AreEqual<uint64_t>(0u, estNoObs.GetCounters().floorMeasured);
			Assert::AreEqual<uint64_t>(static_cast<uint64_t>(seq10.size()), estWithObs.GetCounters().floorMeasured);
			Assert::AreEqual(estNoObs.GetCounters().evaluations, estWithObs.GetCounters().evaluations);
			estNoObs.Stop();
			estWithObs.Stop();

			// Also verify 8-bit-in-10-bit (step = 4, active floor = 1.155, noisy sigma = 12.0 >> 1.155).
			const Plane p8 = MakeNoisyTexturedPlane(400, 300, 909u, 12.0, 4);
			const C::LineSnapshot snap8 = CapturePlaneSnapshot(p8, 200u, 1.0);
			Assert::AreEqual(4, C::DetectStepCodes(snap8));
			ManualClock clk8A, clk8B;
			C::ChainEstimator est8NoObs(C::EstimatorSettings(), clk8A.Fn());
			C::ChainEstimator est8WithObs(C::EstimatorSettings(), clk8B.Fn());
			est8NoObs.SetSynchronous(true);
			est8WithObs.SetSynchronous(true);
			C::EvaluationDetail d8;
			bool got8 = false;
			est8WithObs.SetEvaluationObserver([&d8, &got8](const C::EvaluationDetail& d) { d8 = d; got8 = true; });
			est8NoObs.SetMode(C::EstimatorMode::Shadow);
			est8WithObs.SetMode(C::EstimatorMode::Shadow);
			Assert::IsTrue(est8NoObs.SubmitLines(snap8));
			Assert::IsTrue(est8WithObs.SubmitLines(snap8));
			Assert::IsTrue(got8);
			G::Record r8A, r8B;
			Assert::IsTrue(est8NoObs.Held().ReadLatest(0, r8A));
			Assert::IsTrue(est8WithObs.Held().ReadLatest(0, r8B));
			Assert::IsTrue(SameBytes(G::Pack(r8A), G::Pack(r8B)),
				L"8-bit noisy snapshot Record must be bit-identical with and without shadow floor");
			Assert::AreEqual(1.155, d8.floorCodes, 1e-12);
			Assert::AreEqual(d8.floorCodes, d8.axis[0].floorCodes, 1e-12);
			Assert::AreEqual(d8.floorCodes, d8.axis[1].floorCodes, 1e-12);
			Assert::IsTrue(d8.floorMeasured > 7.0 * d8.floorCodes,
				L"negative control: 8-bit sigma=12.0 yields floorMeasured > 7x active 1.155");
			est8NoObs.Stop();
			est8WithObs.Stop();
		}

		TEST_METHOD(DiagEvalsV4HeaderEndsWithFloorMeasuredAndCalDumpUntouched)
		{
			DiagEnvGuard envGuard;

			// 1. Verify EvalsHeaderV4() contract ends with ",floor_measured" right after "cost_ms".
			const std::string header = D::EvalsHeaderV4();
			const std::string expectedSuffix = ",floor_measured";
			Assert::IsTrue(header.size() >= expectedSuffix.size() &&
				header.compare(header.size() - expectedSuffix.size(), expectedSuffix.size(), expectedSuffix) == 0,
				L"Diag::EvalsHeaderV4() must end with \",floor_measured\"");
			const std::vector<std::string> hdrCols = SplitCsv(header);
			Assert::IsTrue(hdrCols.size() >= 2u);
			Assert::AreEqual(std::string("floor_measured"), hdrCols.back(), L"last column of EvalsHeaderV4 must be floor_measured");
			Assert::AreEqual(std::string("cost_ms"), hdrCols[hdrCols.size() - 2u],
				L"floor_measured must be appended directly after cost_ms without shifting earlier columns");
			size_t idxFloorCodes = hdrCols.size();
			for (size_t i = 0; i < hdrCols.size(); ++i)
				if (hdrCols[i] == "floor_codes")
					idxFloorCodes = i;
			Assert::IsTrue(idxFloorCodes < hdrCols.size(), L"floor_codes column must still exist in EvalsHeaderV4");

			// 2. Real Dump v4 b_evals CSV: verify column count and parsed floor_measured value for noisy & constant inputs.
			const Plane noisyPlane = MakeNoisyTexturedPlane(400, 300, 31415u, 7.0, 1);
			const C::LineSnapshot snapNoisy = CapturePlaneSnapshot(noisyPlane, 10u, 1.0);
			Assert::AreEqual(1, C::DetectStepCodes(snapNoisy));
			const double expectedNoisy = C::MeasuredFloorCodes(snapNoisy, 1.0);
			Assert::IsTrue(expectedNoisy > 4.0, L"noisy snapshot measured floor >> 0.289");

			Plane constPlane;
			constPlane.w = 400;
			constPlane.h = 300;
			constPlane.luma.assign(static_cast<size_t>(constPlane.w) * static_cast<size_t>(constPlane.h), 400u);
			const C::LineSnapshot snapConst = CapturePlaneSnapshot(constPlane, 40u, 2.5);
			Assert::AreEqual(4, C::DetectStepCodes(snapConst));
			const double expectedConst = C::MeasuredFloorCodes(snapConst, 4.0);
			Assert::AreEqual(4.0 / std::sqrt(12.0), expectedConst, 1e-12);

			const std::string v4Dir = MakeTempSubdir("sid_floor_v4_csv_test");
			ClearCsvInDir(v4Dir);
			_putenv_s("VP_SID_DUMP_V4_DIR", v4Dir.c_str());

			uint64_t diagMeasuredCount = 0;
			{
				// Destroy ChainEstimator before reading CSV so Diag::File::~File() flushes and closes the file.
				ManualClock clk;
				C::ChainEstimator est(C::EstimatorSettings(), clk.Fn());
				est.SetSynchronous(true);
				est.SetDiagnosticTag("floor_v4");
				est.SetMode(C::EstimatorMode::Shadow);
				Assert::IsTrue(est.SubmitLines(snapNoisy));
				clk.Set(1500.0);
				Assert::IsTrue(est.SubmitLines(snapConst));
				diagMeasuredCount = est.GetCounters().floorMeasured;
				est.Stop();
			}
			_putenv_s("VP_SID_DUMP_V4_DIR", "");
			Assert::AreEqual<uint64_t>(2u, diagMeasuredCount, L"diagEvals computed floorMeasured for both dumped rows");

			{
				const std::string evalsPath = v4Dir + "\\floor_v4.b_evals.csv";
				std::ifstream in(evalsPath.c_str());
				Assert::IsTrue(in.good(), L"b_evals.csv must be written when VP_SID_DUMP_V4_DIR is set");
				std::string fileHdr, row1, row2, extra;
				Assert::IsTrue(static_cast<bool>(std::getline(in, fileHdr)));
				Assert::AreEqual(header, fileHdr, L"b_evals.csv header must match Diag::EvalsHeaderV4() verbatim");
				Assert::IsTrue(static_cast<bool>(std::getline(in, row1)), L"first b_evals row present");
				Assert::IsTrue(static_cast<bool>(std::getline(in, row2)), L"second b_evals row present");
				Assert::IsFalse(static_cast<bool>(std::getline(in, extra)) && !extra.empty(), L"exactly two rows dumped");

				const std::vector<std::string> cols1 = SplitCsv(row1);
				const std::vector<std::string> cols2 = SplitCsv(row2);
				Assert::AreEqual(hdrCols.size(), cols1.size(), L"row 1 column count must match EvalsHeaderV4");
				Assert::AreEqual(hdrCols.size(), cols2.size(), L"row 2 column count must match EvalsHeaderV4");

				double parsedActive1 = 0.0, parsedMeas1 = 0.0;
				Assert::IsTrue(ParseDouble(cols1[idxFloorCodes], parsedActive1));
				Assert::IsTrue(ParseDouble(cols1.back(), parsedMeas1));
				Assert::AreEqual(0.289, parsedActive1, 1e-4, L"row 1 active floor_codes stays 0.289");
				Assert::AreEqual(expectedNoisy, parsedMeas1, 1e-4 * expectedNoisy + 1e-5,
					L"row 1 floor_measured column must parse to MeasuredFloorCodes(snapNoisy, 1.0)");
				Assert::AreEqual(D::Num(expectedNoisy), cols1.back(),
					L"row 1 floor_measured text must match Diag::Num formatting");
				// Negative control: floor_measured on noisy frame is genuinely distinct from active floor_codes.
				Assert::IsTrue(parsedMeas1 > 10.0 * parsedActive1,
					L"negative control: dumped floor_measured (~7) must not be a copy of active floor_codes (0.289)");

				double parsedActive2 = 0.0, parsedMeas2 = 0.0;
				Assert::IsTrue(ParseDouble(cols2[idxFloorCodes], parsedActive2));
				Assert::IsTrue(ParseDouble(cols2.back(), parsedMeas2));
				Assert::AreEqual(1.155, parsedActive2, 1e-4, L"row 2 active floor_codes stays 1.155");
				Assert::AreEqual(expectedConst, parsedMeas2, 1e-4,
					L"row 2 floor_measured column must parse to quantization floor 4/sqrt(12)");
				Assert::AreEqual(D::Num(expectedConst), cols2.back());
			}
			ClearCsvInDir(v4Dir);
			RemoveDirectoryA(v4Dir.c_str());

			// 3. Verify CAL_DUMP v3 27-column contract is completely untouched (schema AND active floor_codes value).
			SidChain::ApplyRegistrations();
			SidChain::EvalSeam& seam = SidChain::Seam();
			Assert::IsTrue(static_cast<bool>(seam.reelBegin) && static_cast<bool>(seam.frame) && static_cast<bool>(seam.reelEnd),
				L"ChainEvalSeam must be registered");

			const std::string calDir = MakeTempSubdir("sid_floor_cal_v3_check");
			const std::string calCostDir = calDir + "\\cost";
			ClearCsvInDir(calCostDir);
			ClearCsvInDir(calDir);
			_putenv_s("VP_SOURCE_ID_CAL_DUMP", calDir.c_str());

			seam.reelBegin("floor_cal_v3");
			for (int i = 0; i < 4; ++i)
				seam.frame(noisyPlane.luma.data(), noisyPlane.w, noisyPlane.h, noisyPlane.w,
					static_cast<uint64_t>(i) + 1u, 1.1 * static_cast<double>(i));
			seam.reelEnd("floor_cal_v3");
			_putenv_s("VP_SOURCE_ID_CAL_DUMP", "");

			{
				const std::string calPath = calDir + "\\floor_cal_v3.csv";
				std::ifstream in(calPath.c_str());
				Assert::IsTrue(in.good(), L"CAL_DUMP v3 CSV written");
				std::string calHdr, calLine;
				Assert::IsTrue(static_cast<bool>(std::getline(in, calHdr)));
				const std::string expectedCalV3Header =
					"reel_id,title,eval_seq,time_s,axis,N,n,class,T,stage,best_kernel,kernel_margin,lines_passed,lines,power_seen,"
					"floor_codes,truth_n,bit_depth,truth_kernel,truth_sharpen,shot_id,T_ev,informative,master,has_truth,truth_n1,cut_id";
				Assert::AreEqual(expectedCalV3Header, calHdr, L"CAL_DUMP v3 header must remain byte-identical");
				Assert::AreEqual<size_t>(27u, SplitCsv(calHdr).size(), L"CAL_DUMP v3 must keep exact 27 columns");
				Assert::IsTrue(calHdr.find("floor_measured") == std::string::npos,
					L"CAL_DUMP v3 header must NOT contain floor_measured");
				int calRows = 0;
				while (std::getline(in, calLine))
				{
					if (calLine.empty())
						continue;
					++calRows;
					const std::vector<std::string> f = SplitCsv(calLine);
					Assert::AreEqual<size_t>(27u, f.size(), L"every CAL_DUMP v3 row must have exact 27 columns");
					double calFloor = 0.0;
					Assert::IsTrue(ParseDouble(f[15], calFloor), L"CAL_DUMP v3 column 15 (floor_codes) parses as number");
					Assert::AreEqual(0.289, calFloor, 1e-4,
						L"CAL_DUMP v3 floor_codes must remain active 0.289 on 10-bit noisy plane, untouched by floor_measured");
				}
				Assert::IsTrue(calRows > 0, L"CAL_DUMP v3 produced candidate rows");
			}

			ClearCsvInDir(calCostDir);
			RemoveDirectoryA(calCostDir.c_str());
			ClearCsvInDir(calDir);
			RemoveDirectoryA(calDir.c_str());
		}
	};
}
