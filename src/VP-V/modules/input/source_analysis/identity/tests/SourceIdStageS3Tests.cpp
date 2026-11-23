/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Red-Team independent adversarial tests for Source Identity Milestone 1 — Stage S3
// (Rule 3 mandatory in-test negative controls, zero oracle-only tests, zero sleeps, pure CPU C++14 /W4).
//
// Covers every Stage S3 contract surface:
//   1. R1RejectedSubHarmonicDoesNotForgeDisagreeingClassicalConsent (S2 Re-Audit MEDIUM regression)
//   2. DitherFloorDetailAboveProtectsTpdfUpscaleAndFiresOnRealNative4K (EXP-8BIT SHOULD-2)
//   3. DitherAwareDetailAboveInputGuardsAndScaleInvariance (SHOULD-2 guards + TPDF vs uniform + scale invariance)
//   4. EmpiricalFloorShadowByteIdenticalAndLiveAdaptsToTpdfLoopNoise (PKG-SRCID-FLOOR Off/Shadow/Live/8-bit promo)
//   5. EmpiricalFloorAllAxisAndPartialAxisFallbacksVisiblyCounted (Rule 4 all-axis & partial-axis fallback visibility)
//   6. NullTablesStrictDepthIsolationAndPlaceholderVisibility (Rule 1 & Rule 4 depth isolation + forged-param guard)
//   7. DefaultOffByteIdentityAcrossAllS3Switches (all S3 switches OFF by default + byte-identical output)
//   8. DumpV4AppendsStepMarkTentativeBucketAndEmpiricalFloorColumns (76-column Dump v4 contract + RAII env/dir cleanup)
//   9. EmpiricalFloorStackNthElementMatchesFullSortReferenceBitExact (SID-OPT Partition B)
//  10. PolyphaseProposerMaximalFoldingMatchesDirectAllPeriodReference (SID-OPT Partition B)
//  11. ClassicalMirrorCoherenceAndFrameQualityNoiseMatchDirectReference (SID-OPT Partition B)
//  12. CaptureLinesScaleOneAndPowerProbeWorkspaceMatchDirectBitExact (SID-OPT Partition B)
//  13. P3TwoPassExactStagePreservesDippingCandidateRecall (SID-OPT Partition B)
#include "pch.h"
#include "CppUnitTest.h"

#include "ChainEstimatorFixtures.h"

#include <VP-V/core/RobustStats.h>
#include <VP-V/modules/input/source_analysis/SourceResolutionDetector.h>
#include <VP-V/modules/input/source_analysis/identity/FrameQuality.h>
#include <VP-V/modules/input/source_analysis/identity/SidDiagDump.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEdgeStats.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEstimator.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainEvidence.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainJointLatch.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainLines.h>
#include <VP-V/modules/input/source_analysis/identity/chain/ChainStepMark.h>
#include <VP-V/modules/input/source_analysis/identity/chain/KernelBank.h>
#include <VP-V/modules/input/source_analysis/identity/chain/NullTables.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PolyphaseProposer.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PowerProbe.h>
#include <VP-V/modules/input/source_analysis/regions/CodingEvidence.h>
#include <VP-V/modules/scaling/geometry/SourceGeometrySet.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace S3Fx
	{
		namespace C = SourceIdentity::Chain;
		namespace G = SourceIdentity::Geometry;
		namespace SR = SourceResolution;

		constexpr int kUhdW = 3840;
		constexpr int kUhdH = 2160;
		constexpr int kTile = 256;
		constexpr int kBilinear = 0;
		constexpr int kLanczos3 = 5;

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
				if (u1 < 1e-12) u1 = 1e-12;
				const double u2 = Uniform01();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * C::kPi * u2);
			}
		};

		inline uint16_t QuantizeCode(double v, int step)
		{
			const double s = static_cast<double>(step > 0 ? step : 1);
			double q = std::floor(v / s + 0.5) * s;
			if (q < 64.0) q = 64.0;
			if (q > 940.0) q = 940.0;
			return static_cast<uint16_t>(q);
		}

		// Synthesizes a 1D line of length `nOut` from an independent latent signal of length `nIn`
		// upscaled via `KernelBank::TapWeights` (or native broadband when `nIn == nOut`).
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
						if (c < 0) c = 0;
						if (c > nIn - 1) c = nIn - 1;
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
			active.x = 0; active.y = 0; active.w = rasterW; active.h = rasterH;
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

		inline C::LineSnapshot MakeFlatSnapshot(int rasterW, int rasterH, uint16_t code, uint64_t seq, int64_t timeUs, bool cut = false)
		{
			C::LineRect active;
			active.x = 0; active.y = 0; active.w = rasterW; active.h = rasterH;
			const C::LinePlan plan = C::LineSampler::Plan(active, seq);
			Assert::IsTrue(plan.valid, L"fixture: valid flat LinePlan");
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
			snap.rows.assign(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w), code);
			snap.cols.assign(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h), code);
			if (cut)
			{
				snap.flags = C::kEvCut;
				snap.cuts = 1;
				snap.eventTimeUs = timeUs;
			}
			return snap;
		}

		// Builds a 2D synthetic broadband scene (separable cosines with 1/f natural spectrum + optional Gaussian defocus)
		// and renders a real 256x256 tile upscaled from `ratio` via separable Lanczos3 (or native when ratio >= 1.0).
		struct TileScene
		{
			struct Wave { double fx, fy, px, py, amplitude; };
			std::vector<Wave> waves;

			TileScene(uint64_t seed, double defocusSigma, int waveCount = 160, double ampScale = 0.012)
			{
				SplitMix64 rng(seed);
				waves.reserve(static_cast<size_t>(waveCount));
				for (int i = 0; i < waveCount; ++i)
				{
					const double f = 0.004 * std::pow(0.49 / 0.004, rng.Uniform01());
					const double angle = rng.Uniform01() * C::kPi;
					Wave w;
					w.fx = f * std::cos(angle);
					w.fy = f * std::sin(angle);
					w.px = rng.Uniform01() * 2.0 * C::kPi;
					w.py = rng.Uniform01() * 2.0 * C::kPi;
					w.amplitude = (ampScale / std::sqrt(f)) * std::exp(-2.0 * C::kPi * C::kPi * defocusSigma * defocusSigma * f * f);
					waves.push_back(w);
				}
			}

			std::vector<double> Sample(int x0, int y0, int w, int h) const
			{
				std::vector<double> out(static_cast<size_t>(w) * static_cast<size_t>(h), 0.48);
				std::vector<double> cx(static_cast<size_t>(w)), sx(static_cast<size_t>(w));
				std::vector<double> cy(static_cast<size_t>(h)), sy(static_cast<size_t>(h));
				for (size_t wi = 0; wi < waves.size(); ++wi)
				{
					const Wave& v = waves[wi];
					for (int x = 0; x < w; ++x)
					{
						const double a = 2.0 * C::kPi * v.fx * static_cast<double>(x0 + x) + v.px;
						cx[static_cast<size_t>(x)] = std::cos(a);
						sx[static_cast<size_t>(x)] = std::sin(a);
					}
					for (int y = 0; y < h; ++y)
					{
						const double b = 2.0 * C::kPi * v.fy * static_cast<double>(y0 + y) + v.py;
						cy[static_cast<size_t>(y)] = std::cos(b);
						sy[static_cast<size_t>(y)] = std::sin(b);
					}
					for (int y = 0; y < h; ++y)
					{
						double* row = &out[static_cast<size_t>(y) * static_cast<size_t>(w)];
						const double c = v.amplitude * cy[static_cast<size_t>(y)];
						const double s = v.amplitude * sy[static_cast<size_t>(y)];
						for (int x = 0; x < w; ++x)
							row[x] += cx[static_cast<size_t>(x)] * c - sx[static_cast<size_t>(x)] * s;
					}
				}
				return out;
			}
		};

		inline double SincVal(double x)
		{
			return std::fabs(x) < 1e-9 ? 1.0 : std::sin(C::kPi * x) / (C::kPi * x);
		}

		inline double Lanczos3Weight(double x)
		{
			return std::fabs(x) < 3.0 ? SincVal(x) * SincVal(x / 3.0) : 0.0;
		}

		// Renders a continuous (unquantized) 256x256 tile from `scene` at `ratio` (Lanczos3 upscale if ratio < 1.0, native if >= 1.0).
		inline std::vector<double> RenderContinuousTile(const TileScene& scene, double ratio, int originX = 1000, int originY = 700)
		{
			if (ratio >= 1.0)
				return scene.Sample(originX, originY, kTile, kTile);

			const int margin = 4;
			const int mw = static_cast<int>(std::ceil(static_cast<double>(kTile) * ratio)) + 2 * margin;
			const int mx0 = static_cast<int>(std::floor(static_cast<double>(originX) * ratio)) - margin;
			const int my0 = static_cast<int>(std::floor(static_cast<double>(originY) * ratio)) - margin;
			const std::vector<double> master = scene.Sample(mx0, my0, mw, mw);

			auto resample1D = [&](const double* src, int srcStride, int count, double* dst, int dstStride, int origin, int m0) {
				for (int x = 0; x < kTile; ++x)
				{
					const double u = (static_cast<double>(origin + x) + 0.5) * ratio - 0.5 - static_cast<double>(m0);
					const int c = static_cast<int>(std::floor(u));
					double sum = 0.0, wsum = 0.0;
					for (int k = c - 3; k <= c + 4; ++k)
					{
						if (k < 0 || k >= count)
							continue;
						const double w = Lanczos3Weight(u - static_cast<double>(k));
						sum += w * src[static_cast<size_t>(k) * static_cast<size_t>(srcStride)];
						wsum += w;
					}
					dst[static_cast<size_t>(x) * static_cast<size_t>(dstStride)] = wsum != 0.0 ? sum / wsum : 0.48;
				}
			};

			std::vector<double> rows(static_cast<size_t>(mw) * static_cast<size_t>(kTile));
			for (int y = 0; y < mw; ++y)
				resample1D(&master[static_cast<size_t>(y) * static_cast<size_t>(mw)], 1, mw,
					&rows[static_cast<size_t>(y) * static_cast<size_t>(kTile)], 1, originX, mx0);

			std::vector<double> native(static_cast<size_t>(kTile) * static_cast<size_t>(kTile));
			for (int x = 0; x < kTile; ++x)
				resample1D(&rows[static_cast<size_t>(x)], kTile, mw, &native[static_cast<size_t>(x)], kTile, originY, my0);
			return native;
		}

		// Quantizes a continuous 256x256 tile into 8-bit-in-10-bit (multiples of 4 in [64, 940] out of 1023)
		// with genuine 8-bit TPDF dither (step = 4 / 1023), plus optional uniform level shift so sequential frames differ.
		inline std::vector<float> QuantizeTpdf8In10(const std::vector<double>& continuous, uint64_t ditherSeed, double levelShift = 0.0)
		{
			constexpr double kCodeMax = 1023.0;
			constexpr double kStep8 = 4.0 / kCodeMax;
			SplitMix64 rng(ditherSeed);
			std::vector<float> out(continuous.size());
			for (size_t i = 0; i < continuous.size(); ++i)
			{
				const double v = continuous[i] + levelShift + kStep8 * rng.TpdfUnit();
				double q = std::floor((v * kCodeMax) / 4.0 + 0.5) * 4.0;
				if (q < 64.0) q = 64.0;
				if (q > 940.0) q = 940.0;
				out[i] = static_cast<float>(q / kCodeMax);
			}
			return out;
		}

		inline SR::FrameReading BuildFrameFromTiles(const std::vector<std::vector<float>>& tiles, int codeMax = 1023)
		{
			std::vector<SR::Spectrum> spectra(tiles.size());
			for (size_t i = 0; i < tiles.size(); ++i)
			{
				Assert::IsTrue(SR::Analyse(tiles[i].data(), kTile, kTile, spectra[i], codeMax));
				spectra[i].coding = VpUnit::CodingEvidence::MeasureTile(tiles[i].data(), kTile, kTile, codeMax);
			}
			SR::Thresholds thr;
			thr.codeMaximum = codeMax;
			return SR::ReadFrame(spectra, thr);
		}

		inline C::DepthNullTable MakeValidCalibratedTable(C::BitDepthClass d, double sampleHours = 12.0, int nativeShots = 2000, double rationalNullMean = -0.02)
		{
			C::DepthNullTable t = C::DepthNullTable::PlaceholderFor(d);
			t.depthClass = d;
			t.sourceKind = C::NullTableSourceKind::Calibrated;
			t.calibrated = true;
			t.sampleHours = sampleHours;
			t.nativeShots = nativeShots;
			t.tables.placeholder = false;
			t.tables.nul[C::kClassK8].mean = -0.10;       t.tables.nul[C::kClassK8].sd = 0.08;
			t.tables.nul[C::kClassK16].mean = -0.08;      t.tables.nul[C::kClassK16].sd = 0.06;
			t.tables.nul[C::kClassRational].mean = rationalNullMean; t.tables.nul[C::kClassRational].sd = 0.04;
			t.tables.nul[C::kClassGeneric].mean = -0.03;  t.tables.nul[C::kClassGeneric].sd = 0.05;
			for (int c = 0; c < C::kNullClassCount; ++c)
			{
				t.tables.alt[c].mean = -0.34;
				t.tables.alt[c].sd = 0.18;
			}
			return t;
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

		// RAII scope guard for VP_SID_DUMP_V4_DIR + unique temp directory (Rule 2: never leaks env vars or temp files).
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
				dir_ = std::string(tempPath) + "vp_sid_s3_" + (suffix ? suffix : "dump") + "_"
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

	TEST_CLASS(SourceIdStageS3Tests)
	{
	public:
		// 1. S2 Re-Audit MEDIUM regression (ChainEstimator.cpp:1794-1798):
		//    When R1 rejects a sub-harmonic (540p = 1/4) via `aSeesHigher` and selects the clean super-harmonic
		//    (1080p = 1/2) as `hRes`, ChainEstimator MUST NOT forge Classical A consent by overwriting a disagreeing
		//    intermediate Classical A reading (720p = 1/3, with no chain credit at 720p) to 1080p.
		//    Negative control on the EXACT SAME chain snapshots: when Classical A genuinely reports 1080p (0.5),
		//    the exact same current-shot credit DOES enter jointly at 1080p (proving R1 + joint path is exercised).
		TEST_METHOD(R1RejectedSubHarmonicDoesNotForgeDisagreeingClassicalConsent)
		{
			using namespace S3Fx;

			// Build ONE single-shot (14-eval, 0.5 s cadence -> 6.5 s total: >= 3 s dwell, < 10 s min window, < 30 s grace)
			// synthetic 4K stream alternating 960x540 (1/4) and 1920x1080 (1/2) bilinear upscales 1-by-1:
			//   - Strict 1-by-1 alternation ensures neither 960 nor 1920 ever has 2 consecutive misses (preventing +-1 px
			//     slot drift) and gives BOTH 960x540 and 1920x1080 open-shot credit >= 7*(+5 - 3) = +14 >> jNeed (~7.44),
			//     while staying in ONE shot (shot-capped LLR <= 0.5*c1 < c1, so Chain-alone CANNOT enter).
			//   - 1280x720 (1/3) matches neither 1/4 nor 1/2 bilinear and stays deeply negative (<< jNeed).
			std::vector<C::LineSnapshot> stream;
			stream.reserve(14);
			for (int s = 0; s < 14; ++s)
			{
				const bool is540 = (s % 2) == 0;
				const int srcW = is540 ? 960 : 1920;
				const int srcH = is540 ? 540 : 1080;
				stream.push_back(MakeSynthSnapshot(kUhdW, kUhdH, srcW, srcH, kBilinear, 1,
					static_cast<uint64_t>(10 + s), static_cast<int64_t>(s) * 500000,
					31001u + static_cast<uint64_t>(s * 17), s == 0));
			}

			auto runCase = [&](double finalClassicalRatio, int& outLatchNx, int& outLatchNy, C::EstimatorCounters& outCnt) {
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.jointLatch.enabled = true;
				cfg.conventionDrivenProposals = true;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				// Hold detailAbove = true during setup evals 0..12 so both 960x540 and 1920x1080 accumulate
				// current-shot credit without entering early; on eval 13 present the test classical reading.
				est.SetClassicalFresh(true, 0.5, 0.5, true);
				for (size_t i = 0; i < stream.size(); ++i)
				{
					if (i + 1 == stream.size())
						est.SetClassicalFresh(true, finalClassicalRatio, finalClassicalRatio, false);
					clk.Set(static_cast<double>(i) * 1500.0);
					Assert::IsTrue(est.SubmitLines(stream[i]));
				}
				outLatchNx = est.StatusInput().latchNx;
				outLatchNy = est.StatusInput().latchNy;
				outCnt = est.GetCounters();
			};

			// Under test: Classical A reports disagreeing intermediate 720p (1/3), strictly between 1/4 and 1/2.
			// R1 rejects 540p via aSeesHigher and selects 1080p as hRes, but MUST NOT overwrite Classical A (720p -> 1080p)!
			int disagreeNx = -1, disagreeNy = -1;
			C::EstimatorCounters disagreeCnt;
			runCase(1.0 / 3.0, disagreeNx, disagreeNy, disagreeCnt);
			Assert::AreEqual(0, disagreeNx, L"disagreeing Classical A (720p) must NEVER be forged into 1080p joint consent");
			Assert::AreEqual(0, disagreeNy);
			Assert::AreEqual(uint64_t(0), disagreeCnt.jointLatch.entriesJoint, L"zero joint entries when Classical A disagrees with hRes");
			Assert::AreEqual(uint64_t(0), disagreeCnt.jointLatch.entriesChainAlone, L"single-shot credit < c1 cannot enter chain-alone");
			Assert::IsTrue(disagreeCnt.r1HarmonicSelections >= 1, L"R1 RejectedSubHarmonicDetailAbove actually executed");
			Assert::IsTrue(disagreeCnt.jointLatch.wouldBeClassicalOnly >= 1, L"disagreeing 720p Classical A visibly blocked as classical-only");

			// Mandatory in-test negative control #1 (EXACT SAME synthetic snapshots):
			// When Classical A genuinely agrees with hRes (1080p = 0.5), joint entry at 1920x1080 DOES succeed!
			int agreeNx = -1, agreeNy = -1;
			C::EstimatorCounters agreeCnt;
			runCase(0.5, agreeNx, agreeNy, agreeCnt);
			Assert::AreEqual(1920, agreeNx, L"negative control: genuine agreeing Classical A (1080p) enters jointly on exact same stream");
			Assert::AreEqual(1080, agreeNy);
			Assert::AreEqual(uint64_t(1), agreeCnt.jointLatch.entriesJoint, L"negative control: entered via EnteredJoint");
			Assert::AreEqual(uint64_t(0), agreeCnt.jointLatch.entriesChainAlone);
			Assert::IsTrue(agreeCnt.r1HarmonicSelections >= 1);

			// Mandatory in-test negative control #2 (unit-level proof that the old overwrite bug violates the invariant):
			// Overwriting disagreeing 1280x720 classicalNx/Ny to 1920x1080 falsely causes ChainJointLatch::Step to enter!
			{
				const double c1 = C::ComputeThresholds(C::EvidenceSettings()).c1;
				C::JointLatchSettings js;
				js.enabled = true;

				C::JointLatchInput baseIn;
				baseIn.timeUs = 9 * 1000000;
				baseIn.c1 = c1;
				baseIn.rasterW = kUhdW;
				baseIn.rasterH = kUhdH;
				baseIn.chainInformative = true;
				baseIn.haveChainPair = false;
				baseIn.classicalValid = true;
				baseIn.classicalNx = 1280; baseIn.classicalNxDenom = kUhdW;
				baseIn.classicalNy = 720;  baseIn.classicalNyDenom = kUhdH;
				baseIn.currentShotDwellSec = 9.0;
				baseIn.currentShotCreditX = -2.0; // real chain credit at disagreeing 720p is negative
				baseIn.currentShotCreditY = -2.0;

				C::ChainJointLatch honestLatch(js);
				const C::JointLatchOutput honestOut = honestLatch.Step(baseIn);
				Assert::IsFalse(honestOut.latched, L"truthful disagreeing classicalNx/Ny does not latch");
				Assert::IsTrue(honestOut.classicalOnlyBlocked);

				// Naive/buggy pre-fix overwrite: replaces classicalNx/Ny with hRes (1920x1080) and queries 1920x1080 credit!
				C::JointLatchInput forgedIn = baseIn;
				forgedIn.classicalNx = 1920;
				forgedIn.classicalNy = 1080;
				forgedIn.currentShotCreditX = 0.65 * c1;
				forgedIn.currentShotCreditY = 0.65 * c1;
				C::ChainJointLatch naiveLatch(js);
				const C::JointLatchOutput naiveOut = naiveLatch.Step(forgedIn);
				Assert::IsTrue(naiveOut.latched && naiveOut.event == C::JointLatchEvent::EnteredJoint,
					L"negative control: naive classical overwrite falsely forges EnteredJoint at 1920x1080");
			}
		}

		// 2. EXP-8BIT SHOULD-2 end-to-end physics test via real 256x256 8-bit-in-10-bit TPDF-dithered tiles
		//    through SourceResolution::Analyse + MeasureTile + ReadFrame + Tracker::Update:
		//    (a) Detailed 1080p->4K TPDF tiles establish lock -> softer normal-contrast (>= 0.04, NOT dark!)
		//        1080p->4K TPDF tiles drop belowPower & mirror coherence while abovePower stays at TPDF floor:
		//        - Negative control: naive Tracker (ditherFloorDetailAbove = false) FALSELY reverts the valid 0.5 lock!
		//        - Protected Tracker (ditherFloorDetailAbove = true) HOLDS the valid 0.5 lock AND increments DitherFloorStarved()!
		//    (b) Genuine native 4K tiles (still 8-bit-in-10-bit TPDF-dithered!) MUST fire detail_above, revert the
		//        protected Tracker cleanly back to 1.0, and report atDitherFloor == false && hasRealDetailAbove == true!
		TEST_METHOD(DitherFloorDetailAboveProtectsTpdfUpscaleAndFiresOnRealNative4K)
		{
			using namespace S3Fx;

			SR::Tracker::Settings naiveSettings;
			Assert::IsFalse(naiveSettings.ditherFloorDetailAbove, L"ditherFloorDetailAbove must default to false");

			SR::Tracker::Settings protSettings;
			protSettings.ditherFloorDetailAbove = true;

			SR::Tracker naiveTracker(naiveSettings);
			SR::Tracker protTracker(protSettings);

			// Pre-render 3 distinct sharp 1080p->4K scenes (4 tiles each) for multi-shot lock acquisition.
			constexpr int kTilesPerFrame = 4;
			std::vector<std::vector<double>> shotTiles[3];
			for (int shot = 0; shot < 3; ++shot)
			{
				shotTiles[shot].resize(kTilesPerFrame);
				for (int ti = 0; ti < kTilesPerFrame; ++ti)
				{
					const TileScene sc(0x53A10000ull + static_cast<uint64_t>(shot * 100 + ti * 7), 0.0, 160, 0.012);
					shotTiles[shot][static_cast<size_t>(ti)] = RenderContinuousTile(sc, 0.5, 800 + ti * 320, 500 + shot * 256);
				}
			}

			// Phase (a1): 24 sharp 1080p->4K 8-bit-in-10-bit TPDF-dithered frames across 3 shots (0.0 .. 11.5 s).
			double tSec = 0.0;
			int sharpRatioHalfCount = 0;
			for (int f = 0; f < 24; ++f, tSec += 0.5)
			{
				const int shot = f / 8;
				const int inShot = f % 8;
				const double shift = 0.004 * static_cast<double>(inShot) - 0.014;
				std::vector<std::vector<float>> frameTiles(kTilesPerFrame);
				for (int ti = 0; ti < kTilesPerFrame; ++ti)
				{
					frameTiles[static_cast<size_t>(ti)] = QuantizeTpdf8In10(
						shotTiles[shot][static_cast<size_t>(ti)],
						0x71D00000ull + static_cast<uint64_t>(f * 31 + ti),
						shift);
				}
				const SR::FrameReading fr = BuildFrameFromTiles(frameTiles, 1023);
				if (fr.informative() && std::fabs(fr.ratio - 0.5) < 0.05)
					++sharpRatioHalfCount;
				naiveTracker.Update(fr, tSec);
				protTracker.Update(fr, tSec);
			}

			Assert::IsTrue(sharpRatioHalfCount >= 20, L"fixture: sharp 1080p->4K TPDF frames cleanly read ratio ~0.5");
			Assert::IsTrue(std::fabs(naiveTracker.Ratio() - 0.5) < 0.05, L"fixture: naive baseline locked onto 0.5");
			Assert::IsTrue(std::fabs(protTracker.Ratio() - 0.5) < 0.05, L"fixture: protected tracker locked onto 0.5");
			Assert::AreEqual(0ULL, protTracker.DitherFloorStarved(), L"no dither-floor starvation before soft frames");

			// Phase (a2): Softer, normal-contrast (>= 0.04, NOT dark!) 1080p->4K 8-bit-in-10-bit TPDF-dithered frames.
			// Gaussian defocus (sigma = 1.6 master px) drops mid/upper below-band power and mirror coherence (ratio == 1.0)
			// while retaining normal low-frequency scene contrast (>= 0.04) and leaving above-band pinned at 8-bit TPDF floor.
			std::vector<std::vector<double>> softBase(kTilesPerFrame);
			for (int ti = 0; ti < kTilesPerFrame; ++ti)
			{
				const TileScene softSc(0x53B20000ull + static_cast<uint64_t>(ti * 13), 1.6, 160, 0.012);
				softBase[static_cast<size_t>(ti)] = RenderContinuousTile(softSc, 0.5, 900 + ti * 320, 600);
			}

			for (int sf = 0; sf < 3; ++sf, tSec += 0.5)
			{
				std::vector<std::vector<float>> softTiles(kTilesPerFrame);
				for (int ti = 0; ti < kTilesPerFrame; ++ti)
				{
					softTiles[static_cast<size_t>(ti)] = QuantizeTpdf8In10(
						softBase[static_cast<size_t>(ti)],
						0x72E00000ull + static_cast<uint64_t>(sf * 19 + ti),
						0.006 * static_cast<double>(sf));
				}
				const SR::FrameReading softFrame = BuildFrameFromTiles(softTiles, 1023);

				Assert::IsTrue(softFrame.informative(), L"fixture: soft 1080p->4K TPDF frame is informative");
				Assert::IsFalse(softFrame.mixed, L"fixture: soft frame is not mixed");
				Assert::IsTrue(softFrame.contrast >= 0.04, L"fixture: soft frame has normal contrast (>= 0.04, NOT dark!)");
				Assert::AreEqual(1.0, softFrame.ratio, 1e-9, L"fixture: soft frame lost mirror coherence and reads ratio == 1.0");
				Assert::IsTrue(softFrame.quantisationFloor > -6.1 && softFrame.quantisationFloor < -5.7,
					L"fixture: 8-bit-in-10-bit quantisationFloor detected (~ -5.895)");

				SR::DitherDetailAboveInfo softInfo;
				(void)SR::DitherAwareDetailAbove(softFrame, 0.5, true, &softInfo);
				Assert::IsTrue(softInfo.valid, L"DitherAwareDetailAbove valid on soft 1080p TPDF frame");
				Assert::IsTrue(softInfo.atDitherFloor, L"soft 1080p TPDF frame above-band is pinned at TPDF dither floor");
				Assert::IsFalse(softInfo.hasRealDetailAbove, L"soft 1080p TPDF frame has NO real detail above 0.5 Nyquist");

				naiveTracker.Update(softFrame, tSec);
				protTracker.Update(softFrame, tSec);
			}

			// Mandatory in-test negative control (Rule 3) on EXACT SAME frames:
			// Naive Tracker (ditherFloorDetailAbove = false) FALSELY reverts the valid 1080p->4K lock!
			Assert::AreEqual(1.0, naiveTracker.Ratio(), 1e-9,
				L"negative control: naive Tracker (ditherFloorDetailAbove=false) falsely reverts valid 1080p->4K lock on soft TPDF frames");
			// Protected Tracker (ditherFloorDetailAbove = true) HOLDS the valid 1080p->4K lock AND counts DitherFloorStarved!
			Assert::IsTrue(std::fabs(protTracker.Ratio() - 0.5) < 0.05,
				L"protected Tracker (ditherFloorDetailAbove=true) holds valid 1080p->4K lock on soft TPDF frames");
			Assert::IsTrue(protTracker.DitherFloorStarved() >= 3ULL,
				L"protected Tracker visibly counts dither-floor starved frames");
			Assert::AreEqual(0ULL, naiveTracker.DitherFloorStarved());

			// Phase (b): Genuine native 4K tiles containing real high-frequency detail above 0.5 Nyquist
			// (still quantized with the EXACT SAME 8-bit-in-10-bit TPDF dither!).
			std::vector<std::vector<float>> nativeTiles(kTilesPerFrame);
			for (int ti = 0; ti < kTilesPerFrame; ++ti)
			{
				const TileScene natSc(0x53C30000ull + static_cast<uint64_t>(ti * 17), 0.0, 180, 0.013);
				const std::vector<double> natCont = RenderContinuousTile(natSc, 1.0, 1100 + ti * 320, 750);
				nativeTiles[static_cast<size_t>(ti)] = QuantizeTpdf8In10(natCont, 0x73F00000ull + static_cast<uint64_t>(ti));
			}
			const SR::FrameReading nativeFrame = BuildFrameFromTiles(nativeTiles, 1023);
			Assert::IsTrue(nativeFrame.informative(), L"fixture: native 4K TPDF frame is informative");
			Assert::IsFalse(nativeFrame.mixed);
			Assert::AreEqual(1.0, nativeFrame.ratio, 1e-9, L"fixture: native 4K frame reads ratio == 1.0");

			SR::DitherDetailAboveInfo natInfo;
			const double natSubDec = SR::DitherAwareDetailAbove(nativeFrame, 0.5, true, &natInfo);
			Assert::IsTrue(natInfo.valid, L"DitherAwareDetailAbove valid on real native 4K TPDF frame");
			Assert::IsFalse(natInfo.atDitherFloor, L"real native 4K detail clears the TPDF dither floor");
			Assert::IsTrue(natInfo.hasRealDetailAbove, L"real native 4K reports hasRealDetailAbove == true");
			Assert::IsTrue(std::isfinite(natSubDec));

			const unsigned long long starvedBeforeNative = protTracker.DitherFloorStarved();
			protTracker.Update(nativeFrame, tSec);
			Assert::AreEqual(1.0, protTracker.Ratio(), 1e-9,
				L"real native 4K detail fires detail_above and reverts protected Tracker cleanly back to 1.0 (native)");
			Assert::AreEqual(starvedBeforeNative, protTracker.DitherFloorStarved(),
				L"genuine native 4K frame must not be counted as dither-floor starved");
		}

		// 3. DitherAwareDetailAbove input guards, TPDF vs uniform rounding distinction (with negative control),
		//    explicit measuredFloorPowerPerBin override, and strict scale invariance.
		TEST_METHOD(DitherAwareDetailAboveInputGuardsAndScaleInvariance)
		{
			using namespace S3Fx;
			const double nan = std::numeric_limits<double>::quiet_NaN();
			const double inf = std::numeric_limits<double>::infinity();

			constexpr double kStep8 = 4.0 / 1023.0;
			const double qFloor = (kStep8 * kStep8) / 12.0; // uniform rounding power per bin (step^2 / 12)

			auto makeFlatBandFrame = [&](double pBelow, double pAbove, double qFloorLog10) {
				SR::FrameReading fr;
				fr.quantisationFloor = qFloorLog10;
				fr.verticalPower.assign(129, pBelow);
				fr.horizontalPower.assign(129, pBelow);
				for (size_t k = 64; k < 129; ++k)
				{
					fr.verticalPower[k] = pAbove;
					fr.horizontalPower[k] = pAbove;
				}
				return fr;
			};

			// (a) Input guards -> valid == false and returns +inf.
			const SR::FrameReading validBase = makeFlatBandFrame(50.0 * qFloor, 10.0 * qFloor, std::log10(qFloor));
			{
				const double badRatios[] = { 0.0, 1.0, -0.5, 1.5, nan, inf, -inf, 0.98 };
				for (double r : badRatios)
				{
					SR::DitherDetailAboveInfo info;
					const double d = SR::DitherAwareDetailAbove(validBase, r, true, &info);
					Assert::IsFalse(info.valid, L"invalid ratio -> valid == false");
					Assert::IsTrue(std::isinf(d) && d > 0.0, L"invalid ratio -> +inf");
				}

				// Short and mismatched spectra:
				SR::FrameReading shortFr = validBase;
				shortFr.verticalPower.assign(32, qFloor);
				shortFr.horizontalPower.assign(32, qFloor);
				SR::DitherDetailAboveInfo infoShort;
				const double dShort = SR::DitherAwareDetailAbove(shortFr, 0.5, true, &infoShort);
				Assert::IsFalse(infoShort.valid);
				Assert::IsTrue(std::isinf(dShort) && dShort > 0.0);

				SR::FrameReading mismatchFr = validBase;
				mismatchFr.horizontalPower.assign(65, qFloor);
				SR::DitherDetailAboveInfo infoMis;
				const double dMis = SR::DitherAwareDetailAbove(mismatchFr, 0.5, true, &infoMis);
				Assert::IsFalse(infoMis.valid);
				Assert::IsTrue(std::isinf(dMis) && dMis > 0.0);

				// Invalid quantisationFloor (when measuredFloorPowerPerBin == 0):
				const double badQFloors[] = { nan, inf, -inf };
				for (double bf : badQFloors)
				{
					SR::FrameReading badQ = validBase;
					badQ.quantisationFloor = bf;
					SR::DitherDetailAboveInfo infoQ;
					const double dQ = SR::DitherAwareDetailAbove(badQ, 0.5, true, &infoQ, 0.0);
					Assert::IsFalse(infoQ.valid, L"non-finite quantisationFloor -> valid == false");
					Assert::IsTrue(std::isinf(dQ) && dQ > 0.0);
				}

				// Negative / NaN / Inf measuredFloorPowerPerBin:
				const double badMeasured[] = { -1e-6, -1.0, nan, inf, -inf };
				for (double bm : badMeasured)
				{
					SR::DitherDetailAboveInfo infoM;
					const double dM = SR::DitherAwareDetailAbove(validBase, 0.5, true, &infoM, bm);
					Assert::IsFalse(infoM.valid, L"negative/non-finite measuredFloorPowerPerBin -> valid == false");
					Assert::IsTrue(std::isinf(dM) && dM > 0.0);
				}

				// Null info pointer must be safe on both invalid and valid calls:
				Assert::IsTrue(std::isinf(SR::DitherAwareDetailAbove(validBase, nan, true, nullptr)));
				Assert::IsTrue(std::isfinite(SR::DitherAwareDetailAbove(validBase, 0.5, true, nullptr)));
			}

			// (b) TPDF (2 * step^2/12) vs uniform rounding (1 * step^2/12) distinction + negative control.
			{
				// Pure TPDF floor (1.8 * step^2/12 <= 2.25 * 2 * step^2/12): atDitherFloor == true with tpdfDither == true.
				const SR::FrameReading pureTpdf = makeFlatBandFrame(40.0 * qFloor, 1.8 * qFloor, std::log10(qFloor));
				SR::DitherDetailAboveInfo infoPure;
				(void)SR::DitherAwareDetailAbove(pureTpdf, 0.5, true, &infoPure);
				Assert::IsTrue(infoPure.valid);
				Assert::IsTrue(infoPure.atDitherFloor, L"1.8 * step^2/12 sits at TPDF floor");
				Assert::IsFalse(infoPure.hasRealDetailAbove);
				Assert::AreEqual(2.0 * qFloor, infoPure.floorPowerPerBin, 1e-12);

				// Schuchman TPDF quantized error variance = 3.0 * step^2/12:
				//   - Exceeds uniform 2.25 * (1 * step^2/12) => uniform (tpdfDither=false) FALSELY claims detail above!
				//   - Stays inside TPDF 2.25 * (2 * step^2/12) = 4.5 * step^2/12 => tpdfDither=true recognizes TPDF floor!
				const SR::FrameReading schuchman = makeFlatBandFrame(40.0 * qFloor, 3.0 * qFloor, std::log10(qFloor));
				SR::DitherDetailAboveInfo infoUniform, infoTpdf;
				(void)SR::DitherAwareDetailAbove(schuchman, 0.5, false, &infoUniform);
				(void)SR::DitherAwareDetailAbove(schuchman, 0.5, true, &infoTpdf);

				Assert::IsTrue(infoUniform.valid && infoTpdf.valid);
				Assert::AreEqual(1.0 * qFloor, infoUniform.floorPowerPerBin, 1e-12);
				Assert::AreEqual(2.0 * qFloor, infoTpdf.floorPowerPerBin, 1e-12);
				Assert::IsFalse(infoUniform.atDitherFloor, L"negative control: uniform floor misclassifies 3.0 * step^2/12 TPDF noise as above floor");
				Assert::IsTrue(infoUniform.hasRealDetailAbove, L"negative control: uniform floor falsely reports hasRealDetailAbove");
				Assert::IsTrue(infoTpdf.atDitherFloor, L"TPDF floor correctly classifies 3.0 * step^2/12 as atDitherFloor");
				Assert::IsFalse(infoTpdf.hasRealDetailAbove, L"TPDF floor reports hasRealDetailAbove == false");

				// Genuine detail clearly above 2.25 * 2 * step^2/12 (= 4.5 * qFloor):
				const SR::FrameReading realDetail = makeFlatBandFrame(40.0 * qFloor, 6.0 * qFloor, std::log10(qFloor));
				SR::DitherDetailAboveInfo infoReal;
				const double dReal = SR::DitherAwareDetailAbove(realDetail, 0.5, true, &infoReal);
				Assert::IsTrue(infoReal.valid);
				Assert::IsFalse(infoReal.atDitherFloor, L"6.0 * step^2/12 clears 2.25 * 2 * step^2/12 cleanly");
				Assert::IsTrue(infoReal.hasRealDetailAbove);
				Assert::AreEqual(infoReal.floorSubtractedDecades, dReal, 1e-12);
			}

			// (c) Valid positive measuredFloorPowerPerBin > 0 override (e.g. elevated HDMI-loop dither floor):
			//     Negative control: without override, elevated dither noise (8.0 * qFloor) exceeds theoretical
			//     2.25 * 2 * qFloor = 4.5 * qFloor and falsely reads hasRealDetailAbove == true.
			//     With measuredFloorPowerPerBin = 5.0 * qFloor (gate = 11.25 * qFloor): 8.0 * qFloor is caught as
			//     atDitherFloor == true, while real detail at 14.0 * qFloor (> 11.25 * qFloor) still clears cleanly!
			{
				const double measuredFloor = 5.0 * qFloor;
				const SR::FrameReading elevatedNoise = makeFlatBandFrame(60.0 * qFloor, 8.0 * qFloor, std::log10(qFloor));

				SR::DitherDetailAboveInfo infoNoOverride;
				(void)SR::DitherAwareDetailAbove(elevatedNoise, 0.5, true, &infoNoOverride, 0.0);
				Assert::IsTrue(infoNoOverride.valid);
				Assert::IsFalse(infoNoOverride.atDitherFloor,
					L"negative control: theoretical TPDF floor underestimates elevated 8.0 * qFloor noise");
				Assert::IsTrue(infoNoOverride.hasRealDetailAbove,
					L"negative control: without measuredFloorPowerPerBin override, elevated noise reads as detail above");

				SR::DitherDetailAboveInfo infoWithOverride;
				(void)SR::DitherAwareDetailAbove(elevatedNoise, 0.5, true, &infoWithOverride, measuredFloor);
				Assert::IsTrue(infoWithOverride.valid);
				Assert::AreEqual(measuredFloor, infoWithOverride.floorPowerPerBin, 1e-12,
					L"positive measuredFloorPowerPerBin overrides theoretical floorPowerPerBin");
				Assert::IsTrue(infoWithOverride.atDitherFloor,
					L"elevated 8.0 * qFloor noise sits within 2.25 * measuredFloor (11.25 * qFloor)");
				Assert::IsFalse(infoWithOverride.hasRealDetailAbove);

				const SR::FrameReading realAboveMeasured = makeFlatBandFrame(60.0 * qFloor, 14.0 * qFloor, std::log10(qFloor));
				SR::DitherDetailAboveInfo infoMeasuredClear;
				const double dMeasuredClear = SR::DitherAwareDetailAbove(realAboveMeasured, 0.5, true, &infoMeasuredClear, measuredFloor);
				Assert::IsTrue(infoMeasuredClear.valid);
				Assert::AreEqual(measuredFloor, infoMeasuredClear.floorPowerPerBin, 1e-12);
				Assert::IsFalse(infoMeasuredClear.atDitherFloor,
					L"real detail 14.0 * qFloor clears 2.25 * measuredFloor (11.25 * qFloor)");
				Assert::IsTrue(infoMeasuredClear.hasRealDetailAbove);
				Assert::AreEqual(infoMeasuredClear.floorSubtractedDecades, dMeasuredClear, 1e-12);
			}

			// (d) Strict scale invariance: scaling power spectra AND floor power by 16x (2-bit depth shift)
			//     leaves atDitherFloor, hasRealDetailAbove, rawDecades, and floorSubtractedDecades invariant!
			{
				const double scale = 16.0;
				const double cases[2] = { 1.8 * qFloor, 12.0 * qFloor };
				for (int ci = 0; ci < 2; ++ci)
				{
					const SR::FrameReading f1 = makeFlatBandFrame(50.0 * qFloor, cases[ci], std::log10(qFloor));
					const SR::FrameReading f2 = makeFlatBandFrame(50.0 * qFloor * scale, cases[ci] * scale, std::log10(qFloor * scale));
					SR::DitherDetailAboveInfo i1, i2;
					const double d1 = SR::DitherAwareDetailAbove(f1, 0.5, true, &i1);
					const double d2 = SR::DitherAwareDetailAbove(f2, 0.5, true, &i2);
					Assert::IsTrue(i1.valid && i2.valid);
					Assert::AreEqual(i1.atDitherFloor, i2.atDitherFloor, L"scale invariance: atDitherFloor");
					Assert::AreEqual(i1.hasRealDetailAbove, i2.hasRealDetailAbove, L"scale invariance: hasRealDetailAbove");
					Assert::AreEqual(i1.rawDecades, i2.rawDecades, 1e-9, L"scale invariance: rawDecades");
					Assert::AreEqual(i1.floorSubtractedDecades, i2.floorSubtractedDecades, 1e-9, L"scale invariance: floorSubtractedDecades");
					Assert::AreEqual(d1, d2, 1e-9, L"scale invariance: return value");
				}
			}
		}

		// 4. PKG-SRCID-FLOOR: EmpiricalFloorMode Off vs Shadow vs Live (and eightBitPromotion = true) on synthetic
		//    HDMI-loop re-quantized 8-bit-in-10-bit TPDF-scale noise where step == 1 yet true noise sigma is ~1.63 codes.
		//    - Off vs Shadow: byte-identical records, floorCodes, and candidate decisions; Shadow visibly populates
		//      empiricalFloorCodes[0/1] > 1.2 without mutating floorCodes.
		//    - Live & eightBitPromotion: floorCodes adapts upward to ~1.63 (negative control: Off uses hardcoded 0.289,
		//      a > 5x floor underestimation!).
		TEST_METHOD(EmpiricalFloorShadowByteIdenticalAndLiveAdaptsToTpdfLoopNoise)
		{
			using namespace S3Fx;

			const double tpdfSigma8 = C::TpdfDitherFloorCodes(4.0); // ~1.632993 codes
			std::vector<C::LineSnapshot> frames;
			frames.reserve(5);
			for (int i = 0; i < 5; ++i)
			{
				frames.push_back(MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
					static_cast<uint64_t>(10 + i * 10), static_cast<int64_t>(i + 1) * 1000000,
					41001u + static_cast<uint64_t>(i), i == 0 || i == 3, tpdfSigma8, false, 0.75));
			}

			auto runMode = [&](C::EmpiricalFloorMode mode, bool viaSetter, bool eightBitPromo,
				std::vector<G::Packed>& outPacked, std::vector<C::EvaluationDetail>& outDetails,
				std::vector<std::string>& outStatuses, C::EstimatorCounters& outCnt) {
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1; // HDMI-loop re-quantized 10-bit link (step == 1)
				if (!viaSetter)
				{
					cfg.empiricalFloorMode = mode;
					cfg.eightBitPromotion = eightBitPromo;
				}
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				if (viaSetter)
				{
					est.SetEmpiricalFloorMode(mode);
					if (eightBitPromo)
						est.SetEightBitPromotion(true);
				}
				outDetails.clear();
				outPacked.clear();
				outStatuses.clear();
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { outDetails.push_back(d); });
				est.SetMode(C::EstimatorMode::On);

				for (size_t i = 0; i < frames.size(); ++i)
				{
					clk.Set(static_cast<double>(i + 1) * 1500.0);
					Assert::IsTrue(est.SubmitLines(frames[i]));
					G::Record rec;
					Assert::IsTrue(est.Held().ReadLatest(0, rec));
					outPacked.push_back(G::Pack(rec));
					outStatuses.push_back(est.StatusLine());
				}
				outCnt = est.GetCounters();
			};

			std::vector<G::Packed> packOff, packShadow, packLive, packPromo;
			std::vector<C::EvaluationDetail> detOff, detShadow, detLive, detPromo;
			std::vector<std::string> statOff, statShadow, statLive, statPromo;
			C::EstimatorCounters cntOff, cntShadow, cntLive, cntPromo;

			runMode(C::EmpiricalFloorMode::Off, false, false, packOff, detOff, statOff, cntOff);
			runMode(C::EmpiricalFloorMode::Shadow, true, false, packShadow, detShadow, statShadow, cntShadow);
			runMode(C::EmpiricalFloorMode::Live, true, false, packLive, detLive, statLive, cntLive);
			runMode(C::EmpiricalFloorMode::Off, true, true, packPromo, detPromo, statPromo, cntPromo);

			Assert::AreEqual<size_t>(5, detOff.size());
			Assert::AreEqual<size_t>(5, detShadow.size());
			Assert::AreEqual<size_t>(5, detLive.size());
			Assert::AreEqual<size_t>(5, detPromo.size());

			// (a) Off vs Shadow byte-identity + Shadow visibility without mutating floorCodes.
			for (size_t i = 0; i < frames.size(); ++i)
			{
				Assert::IsTrue(ChainFx::SameBytes(packOff[i], packShadow[i]),
					L"EmpiricalFloorMode::Shadow must produce byte-identical Geometry::Record to Off");
				Assert::AreEqual(statOff[i], statShadow[i], L"Shadow status line matches Off");

				const C::EvaluationDetail& dOff = detOff[i];
				const C::EvaluationDetail& dSh = detShadow[i];
				Assert::IsTrue(dOff.empiricalFloorMode == C::EmpiricalFloorMode::Off);
				Assert::IsTrue(dSh.empiricalFloorMode == C::EmpiricalFloorMode::Shadow);

				// Shadow must NOT mutate decision floorCodes or candidate statistics:
				for (int a = 0; a < 2; ++a)
				{
					Assert::AreEqual(dOff.axis[a].floorCodes, dSh.axis[a].floorCodes, 1e-12,
						L"Shadow must not mutate active axis floorCodes");
					Assert::AreEqual(dOff.axis[a].cands.size(), dSh.axis[a].cands.size());
					for (size_t ci = 0; ci < dOff.axis[a].cands.size(); ++ci)
					{
						Assert::AreEqual(dOff.axis[a].cands[ci].n, dSh.axis[a].cands[ci].n);
						Assert::AreEqual(dOff.axis[a].cands[ci].stat, dSh.axis[a].cands[ci].stat, 1e-12);
						Assert::AreEqual(dOff.axis[a].cands[ci].evStat, dSh.axis[a].cands[ci].evStat, 1e-12);
						Assert::AreEqual(dOff.axis[a].cands[ci].linesGated, dSh.axis[a].cands[ci].linesGated);
						Assert::AreEqual(dOff.axis[a].cands[ci].informative, dSh.axis[a].cands[ci].informative);
					}
				}

				// Shadow visibly measures the real ~1.63 code noise on both axes:
				Assert::IsTrue(dSh.empiricalFloorValid[0] && dSh.empiricalFloorValid[1], L"Shadow measured valid empirical floor on both axes");
				Assert::IsTrue(dSh.empiricalFloorCodes[0] > 1.2 && dSh.empiricalFloorCodes[0] < 2.2, L"Shadow empiricalFloorCodes[0] near ~1.63");
				Assert::IsTrue(dSh.empiricalFloorCodes[1] > 1.2 && dSh.empiricalFloorCodes[1] < 2.2, L"Shadow empiricalFloorCodes[1] near ~1.63");
				Assert::IsTrue(dSh.theoreticalFloorCodes > 0.25 && dSh.theoreticalFloorCodes < 0.35, L"theoretical floor for step=1 is ~0.289");
				Assert::IsFalse(dSh.empiricalFloorFallback);

				// Negative control (Off) vs Live & eightBitPromotion on the EXACT SAME frame:
				const C::EvaluationDetail& dLv = detLive[i];
				const C::EvaluationDetail& dPr = detPromo[i];
				Assert::IsTrue(dOff.axis[0].floorCodes < 0.35,
					L"negative control: naive Off uses hardcoded ~0.289 floor (> 5x below true ~1.63 TPDF noise)");
				Assert::IsTrue(dLv.empiricalFloorMode == C::EmpiricalFloorMode::Live);
				Assert::IsTrue(dLv.axis[0].floorCodes > 1.2 && dLv.axis[0].floorCodes < 2.2,
					L"Live mode adapts floorCodes upward to measured ~1.63 TPDF loop noise");
				Assert::IsTrue(dLv.axis[0].floorCodes > 4.0 * dOff.axis[0].floorCodes,
					L"Live floor exceeds naive hardcoded step-1 floor by > 4x");
				Assert::IsFalse(dLv.empiricalFloorFallback);

				Assert::IsTrue(dPr.axis[0].floorCodes > 1.2 && dPr.axis[0].floorCodes < 2.2,
					L"SetEightBitPromotion(true) adapts floorCodes upward to measured ~1.63 TPDF loop noise");
				Assert::IsTrue(dPr.axis[0].floorCodes > 4.0 * dOff.axis[0].floorCodes);
			}

			Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorFallbacks);
			Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorAllAxisFallbacks);
			Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorPartialAxisFallbacks);

			Assert::AreEqual(uint64_t(5), cntShadow.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), cntShadow.empiricalFloorFallbacks);
			Assert::AreEqual(uint64_t(5), cntLive.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), cntLive.empiricalFloorAllAxisFallbacks);
			Assert::AreEqual(uint64_t(0), cntLive.empiricalFloorPartialAxisFallbacks);
			Assert::AreEqual(uint64_t(5), cntPromo.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(5), cntPromo.depthNullTableFallbacks,
				L"SetEightBitPromotion(true) activates depth null-table selection (placeholder fallback counted visibly)");
		}

		// 5. Rule 4 empirical-floor fallback visibility:
		//    - All-axis fallback (constant flat lines MAD == 0, < 5 px degenerate strips, AND mixed failure where
		//      axis X is < 5 px invalid while axis Y is flat MAD == 0) -> empiricalFloorAllAxisFallbacks > 0
		//    - Partial-axis fallback (axis X noisy above theoretical floor, axis Y constant MAD == 0) -> empiricalFloorPartialAxisFallbacks > 0
		//    - Contrast/negative control: noisy X + noisy Y -> 0 fallbacks; Off mode keeps ALL counters strictly 0.
		TEST_METHOD(EmpiricalFloorAllAxisAndPartialAxisFallbacksVisiblyCounted)
		{
			using namespace S3Fx;
			const double tpdfSigma8 = C::TpdfDitherFloorCodes(4.0);

			// Build 5 deterministic test snapshots:
			// (1) Completely constant flat 4K frame (both axes MAD == 0 -> all-axis theoretical fallback).
			const C::LineSnapshot flatSnap = MakeFlatSnapshot(kUhdW, kUhdH, 512, 10, 1000000, true);

			// (2) Degenerate < 5 px strip on both axes (EstimateEmpiricalFloor valid == false on both -> all-axis fallback).
			C::LineSnapshot shortSnap;
			shortSnap.seq = 20;
			shortSnap.timeUs = 2000000;
			shortSnap.rasterW = 4; shortSnap.rasterH = 4;
			shortSnap.active = { 0, 0, 4, 4 };
			shortSnap.inner = { 0, 0, 4, 4 };
			shortSnap.rowCount = 1; shortSnap.colCount = 1;
			shortSnap.rowY = { 2 }; shortSnap.colX = { 2 };
			shortSnap.rows = { 101, 203, 305, 407 };
			shortSnap.cols = { 101, 203, 305, 407 };

			// (3) Mixed-failure all-axis fallback: axis X is degenerate (< 5 px, valid == false) AND axis Y is constant
			//     flat (MAD == 0, usedTheoreticalFallback == true) -> 0 of 2 active axes measured empirically, so this
			//     MUST count as an all-axis fallback (NOT partial-axis!).
			C::LineSnapshot mixedFailSnap;
			mixedFailSnap.seq = 25;
			mixedFailSnap.timeUs = 2500000;
			mixedFailSnap.rasterW = 4; mixedFailSnap.rasterH = 512;
			mixedFailSnap.active = { 0, 0, 4, 512 };
			mixedFailSnap.inner = { 0, 0, 4, 512 };
			mixedFailSnap.rowCount = 1; mixedFailSnap.colCount = 1;
			mixedFailSnap.rowY = { 256 }; mixedFailSnap.colX = { 2 };
			mixedFailSnap.rows = { 101, 203, 305, 407 };
			mixedFailSnap.cols.assign(512, static_cast<uint16_t>(512));
			{
				C::LineSet lsX, lsY;
				Assert::IsTrue(C::ToLineSet(mixedFailSnap, 0, lsX));
				Assert::IsTrue(C::ToLineSet(mixedFailSnap, 1, lsY));
				C::EdgeStatsSettings esOn;
				esOn.enabled = true;
				const C::EmpiricalFloorResult efX = C::EstimateEmpiricalFloor(lsX, 1.0, esOn);
				const C::EmpiricalFloorResult efY = C::EstimateEmpiricalFloor(lsY, 1.0, esOn);
				Assert::IsFalse(efX.valid, L"fixture: mixed-failure X (< 5 px) returns valid == false");
				Assert::IsTrue(efY.valid && efY.usedTheoreticalFallback, L"fixture: mixed-failure Y (flat MAD==0) returns theoretical fallback");
			}

			// (4) Partial-axis snapshot: X axis carries genuine ~1.63 code noise, Y axis is completely constant (MAD == 0).
			C::LineSnapshot partialSnap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
				30, 3000000, 51001u, false, tpdfSigma8, false, 0.75);
			std::fill(partialSnap.cols.begin(), partialSnap.cols.end(), static_cast<uint16_t>(512));

			// (5) Contrast/negative control: BOTH axes carry genuine ~1.63 code noise (zero fallbacks expected).
			const C::LineSnapshot healthySnap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
				40, 4000000, 51002u, false, tpdfSigma8, false, 0.75);

			// Live mode checks:
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.empiricalFloorMode = C::EmpiricalFloorMode::Live;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				C::EvaluationDetail lastDet;
				est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { lastDet = d; });
				est.SetMode(C::EstimatorMode::On);

				// All-axis fallback #1 (flat MAD == 0 on both axes):
				clk.Set(1000.0);
				Assert::IsTrue(est.SubmitLines(flatSnap));
				C::EstimatorCounters c1 = est.GetCounters();
				Assert::AreEqual(uint64_t(1), c1.empiricalFloorAllAxisFallbacks, L"flat both-axis MAD==0 increments all-axis fallback");
				Assert::AreEqual(uint64_t(0), c1.empiricalFloorPartialAxisFallbacks, L"flat both-axis is NOT a partial fallback");
				Assert::IsTrue(c1.empiricalFloorFallbacks >= 1, L"total empiricalFloorFallbacks increments on all-axis fallback");
				Assert::IsTrue(lastDet.empiricalFloorFallback);
				Assert::IsTrue(std::fabs(lastDet.axis[0].floorCodes - C::QuantizationFloorCodes(1.0)) < 1e-2,
					L"flat frame falls back cleanly to theoretical step-1 floor");

				// All-axis fallback #2 (degenerate < 5 px strips):
				clk.Set(2000.0);
				Assert::IsTrue(est.SubmitLines(shortSnap));
				C::EstimatorCounters c2 = est.GetCounters();
				Assert::AreEqual(uint64_t(2), c2.empiricalFloorAllAxisFallbacks, L"short <5 px both axes increments all-axis fallback");
				Assert::AreEqual(uint64_t(0), c2.empiricalFloorPartialAxisFallbacks);
				Assert::IsTrue(c2.empiricalFloorFallbacks >= 2);
				Assert::IsFalse(lastDet.empiricalFloorValid[0]);
				Assert::IsFalse(lastDet.empiricalFloorValid[1]);
				Assert::IsTrue(lastDet.empiricalFloorFallback);

				// All-axis fallback #3 (mixed failure: X < 5 px invalid AND Y constant flat MAD == 0):
				clk.Set(2500.0);
				Assert::IsTrue(est.SubmitLines(mixedFailSnap));
				C::EstimatorCounters c2b = est.GetCounters();
				Assert::AreEqual(uint64_t(3), c2b.empiricalFloorAllAxisFallbacks,
					L"mixed failure (X invalid + Y theoretical fallback) must increment all-axis fallback");
				Assert::AreEqual(uint64_t(0), c2b.empiricalFloorPartialAxisFallbacks,
					L"mixed failure with 0 empirical axes must NOT increment partial-axis fallback");
				Assert::IsTrue(c2b.empiricalFloorFallbacks >= 3);
				Assert::IsFalse(lastDet.empiricalFloorValid[0]);
				Assert::IsTrue(lastDet.empiricalFloorFallback);

				// Partial-axis fallback (X noisy, Y constant MAD == 0):
				clk.Set(3000.0);
				Assert::IsTrue(est.SubmitLines(partialSnap));
				C::EstimatorCounters c3 = est.GetCounters();
				Assert::AreEqual(uint64_t(3), c3.empiricalFloorAllAxisFallbacks, L"partial-axis fallback does not increment all-axis counter");
				Assert::AreEqual(uint64_t(1), c3.empiricalFloorPartialAxisFallbacks, L"noisy X + flat Y visibly increments partial-axis fallback");
				Assert::IsTrue(c3.empiricalFloorFallbacks >= 4);
				Assert::IsTrue(lastDet.empiricalFloorValid[0] && lastDet.empiricalFloorCodes[0] > 1.2);
				Assert::IsTrue(lastDet.empiricalFloorFallback);
				Assert::IsTrue(lastDet.axis[0].floorCodes > 1.2, L"partial-axis preserves valid noisy X measurement");

				// Healthy contrast (both axes noisy): zero new fallbacks!
				clk.Set(4000.0);
				Assert::IsTrue(est.SubmitLines(healthySnap));
				C::EstimatorCounters c4 = est.GetCounters();
				Assert::AreEqual(c3.empiricalFloorAllAxisFallbacks, c4.empiricalFloorAllAxisFallbacks, L"healthy both-axis frame adds 0 all-axis fallbacks");
				Assert::AreEqual(c3.empiricalFloorPartialAxisFallbacks, c4.empiricalFloorPartialAxisFallbacks, L"healthy both-axis frame adds 0 partial-axis fallbacks");
				Assert::AreEqual(c3.empiricalFloorFallbacks, c4.empiricalFloorFallbacks, L"healthy both-axis frame adds 0 total fallbacks");
				Assert::IsFalse(lastDet.empiricalFloorFallback);
			}

			// Off mode negative control: exact same 5 snapshots keep ALL empirical floor counters strictly 0.
			{
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.outputStepOverride = 1;
				cfg.empiricalFloorMode = C::EmpiricalFloorMode::Off;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetMode(C::EstimatorMode::On);

				clk.Set(1000.0); Assert::IsTrue(est.SubmitLines(flatSnap));
				clk.Set(2000.0); Assert::IsTrue(est.SubmitLines(shortSnap));
				clk.Set(2500.0); Assert::IsTrue(est.SubmitLines(mixedFailSnap));
				clk.Set(3000.0); Assert::IsTrue(est.SubmitLines(partialSnap));
				clk.Set(4000.0); Assert::IsTrue(est.SubmitLines(healthySnap));

				const C::EstimatorCounters cntOff = est.GetCounters();
				Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorEvals);
				Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorFallbacks);
				Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorAllAxisFallbacks);
				Assert::AreEqual(uint64_t(0), cntOff.empiricalFloorPartialAxisFallbacks);
			}
		}

		// 6. NullTables.h strict depth isolation, Rule-of-Three bound, forged-calibrated parameter rejection,
		//    and end-to-end ChainEstimator integration (Rule 1 & Rule 4).
		TEST_METHOD(NullTablesStrictDepthIsolationAndPlaceholderVisibility)
		{
			using namespace S3Fx;

			// (a) Names & step/bit-depth classification helpers.
			{
				const char* pName = C::NullTableSourceKindName(C::NullTableSourceKind::Placeholder);
				const char* cName = C::NullTableSourceKindName(C::NullTableSourceKind::Calibrated);
				Assert::IsTrue(pName != nullptr && pName[0] != '\0');
				Assert::IsTrue(cName != nullptr && cName[0] != '\0');
				Assert::AreNotEqual(std::string(pName), std::string(cName));

				const char* uName = C::BitDepthClassName(C::BitDepthClass::Unknown);
				const char* d8Name = C::BitDepthClassName(C::BitDepthClass::N8);
				const char* d10Name = C::BitDepthClassName(C::BitDepthClass::N10);
				const char* d12Name = C::BitDepthClassName(C::BitDepthClass::N12);
				Assert::IsTrue(uName != nullptr && d8Name != nullptr && d10Name != nullptr && d12Name != nullptr);
				Assert::AreNotEqual(std::string(d8Name), std::string(d10Name));
				Assert::AreNotEqual(std::string(d10Name), std::string(d12Name));

				Assert::IsTrue(C::ClassifyBitDepthFromStep(4) == C::BitDepthClass::N8);
				Assert::IsTrue(C::ClassifyBitDepthFromStep(1) == C::BitDepthClass::N10);
				Assert::IsTrue(C::ClassifyBitDepthFromStep(1, 12) == C::BitDepthClass::N12);
				Assert::IsTrue(C::ClassifyBitDepthFromStep(4, 8) == C::BitDepthClass::N8);
				Assert::IsTrue(C::ClassifyBitDepthFromStep(1, 8) == C::BitDepthClass::N8,
					L"HIGH-1 regression: 8-bit HDMI-loop stream (stepCodes == 1, outputBits == 8) must classify as N8, never N10");
				Assert::IsTrue(C::ClassifyBitDepthFromStep(1, 10) == C::BitDepthClass::N10);
				Assert::IsTrue(C::ClassifyBitDepthFromStep(0, 0) == C::BitDepthClass::Unknown);
				Assert::IsTrue(C::ClassifyBitDepthFromStep(-4, 0) == C::BitDepthClass::Unknown);
			}

			// (b) PlaceholderOnly(): uncalibrated across all depths with visible fallback reasons.
			{
				const C::DepthNullTableSet ph = C::DepthNullTableSet::PlaceholderOnly();
				Assert::IsFalse(ph.AnyCalibrated());
				Assert::IsFalse(ph.AllCalibrated());

				const C::BitDepthClass depths[4] = { C::BitDepthClass::N8, C::BitDepthClass::N10, C::BitDepthClass::N12, C::BitDepthClass::Unknown };
				for (int i = 0; i < 4; ++i)
				{
					const C::DepthNullTableLookupResult r = ph.Select(depths[i]);
					Assert::IsTrue(r.requestedDepth == depths[i]);
					Assert::IsFalse(r.calibrated);
					Assert::IsTrue(r.sourceKind == C::NullTableSourceKind::Placeholder);
					Assert::IsTrue(r.usedPlaceholderFallback);
					Assert::IsTrue(r.tables.placeholder);
					Assert::IsTrue(r.fallbackReason != nullptr && r.fallbackReason[0] != '\0');
				}
			}

			// (c) Adversarial cross-depth leakage test + Rule 3 negative control:
			//     Populate ONLY n8 with valid calibrated stats; N10 and N12 MUST refuse cross-substitution!
			{
				C::DepthNullTableSet set = C::DepthNullTableSet::PlaceholderOnly();
				set.n8 = MakeValidCalibratedTable(C::BitDepthClass::N8, 12.0, 2000, -0.02);

				Assert::IsTrue(set.AnyCalibrated());
				Assert::IsFalse(set.AllCalibrated());
				Assert::IsTrue(set.n8.IsCalibrated());
				Assert::IsTrue(set.n8.HasValidParameters());
				Assert::AreEqual(3.0 / 12.0, set.n8.RuleOfThreeBoundPerHour(), 1e-12, L"Rule-of-Three bound == 3.0 / sampleHours");

				const C::DepthNullTableLookupResult r8 = set.Select(C::BitDepthClass::N8);
				Assert::IsTrue(r8.calibrated);
				Assert::IsTrue(r8.sourceKind == C::NullTableSourceKind::Calibrated);
				Assert::IsTrue(r8.resolvedDepth == C::BitDepthClass::N8);
				Assert::IsFalse(r8.usedPlaceholderFallback);
				Assert::IsFalse(r8.tables.placeholder);
				Assert::AreEqual(-0.02, r8.tables.nul[C::kClassRational].mean, 1e-12);

				const C::NullTables phDefault = C::NullTables::Placeholder();
				const C::BitDepthClass otherDepths[3] = { C::BitDepthClass::N10, C::BitDepthClass::N12, C::BitDepthClass::Unknown };
				for (int i = 0; i < 3; ++i)
				{
					const C::DepthNullTableLookupResult rOther = set.Select(otherDepths[i]);
					Assert::IsFalse(rOther.calibrated, L"must refuse cross-depth substitution from N8 to other depths");
					Assert::IsTrue(rOther.sourceKind == C::NullTableSourceKind::Placeholder);
					Assert::IsTrue(rOther.usedPlaceholderFallback);
					Assert::IsTrue(rOther.tables.placeholder);
					Assert::IsTrue(rOther.fallbackReason != nullptr && rOther.fallbackReason[0] != '\0');
					Assert::AreEqual(phDefault.nul[C::kClassRational].mean, rOther.tables.nul[C::kClassRational].mean, 1e-12,
						L"other depth receives conservative Placeholder params, NOT N8's calibrated params");
				}

				// Mandatory in-test negative control (Rule 3): a naive cross-depth substitution (using N8's calibrated
				// table to score a 10-bit stat = -0.15) produces a materially higher, uncalibrated-for-10-bit LLR than
				// the conservative Placeholder fallback!
				const double stat = -0.15;
				const double naiveCrossDepthLlr = set.n8.tables.LogLikelihoodRatio(C::kClassRational, stat);
				const double isolatedN10Llr = set.Select(C::BitDepthClass::N10).tables.LogLikelihoodRatio(C::kClassRational, stat);
				Assert::IsTrue(naiveCrossDepthLlr > isolatedN10Llr + 1.0,
					L"negative control: naive cross-depth N8->N10 substitution inflates 10-bit LLR by > 1 nat");

				// AllCalibrated() becomes true only when all three depths are validly calibrated:
				set.n10 = MakeValidCalibratedTable(C::BitDepthClass::N10, 15.0, 2500, -0.025);
				set.n12 = MakeValidCalibratedTable(C::BitDepthClass::N12, 20.0, 3000, -0.02);
				Assert::IsTrue(set.AllCalibrated());
			}

			// (d) Adversarial forged-calibrated parameter tests:
			//     calibrated = true, sourceKind = Calibrated, tables.placeholder = false BUT invalid params ->
			//     IsCalibrated() MUST be false and Select() MUST fall back visibly with "invalid_table_params_fallback_placeholder"!
			{
				const double nan = std::numeric_limits<double>::quiet_NaN();
				const double inf = std::numeric_limits<double>::infinity();

				auto checkForgedInvalidParams = [](const C::DepthNullTable& bad, const wchar_t* label) {
					Assert::IsFalse(bad.IsCalibrated(), label);
					C::DepthNullTableSet s = C::DepthNullTableSet::PlaceholderOnly();
					s.n10 = bad;
					const C::DepthNullTableLookupResult res = s.Select(C::BitDepthClass::N10);
					Assert::IsFalse(res.calibrated, label);
					Assert::IsTrue(res.sourceKind == C::NullTableSourceKind::Placeholder, label);
					Assert::IsTrue(res.usedPlaceholderFallback, label);
					Assert::IsTrue(res.tables.placeholder, label);
					Assert::IsTrue(res.fallbackReason != nullptr, label);
					Assert::AreEqual(std::string("invalid_table_params_fallback_placeholder"), std::string(res.fallbackReason), label);
				};

				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.sampleHours = 0.0; checkForgedInvalidParams(b, L"sampleHours == 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.sampleHours = -3.0; checkForgedInvalidParams(b, L"sampleHours < 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.sampleHours = nan; checkForgedInvalidParams(b, L"sampleHours == NaN"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.sampleHours = inf; checkForgedInvalidParams(b, L"sampleHours == Inf"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.nativeShots = 0; checkForgedInvalidParams(b, L"nativeShots == 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.nativeShots = -10; checkForgedInvalidParams(b, L"nativeShots < 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.tables.nul[C::kClassK8].sd = 0.0; checkForgedInvalidParams(b, L"nul sd == 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.tables.nul[C::kClassK16].sd = -0.05; checkForgedInvalidParams(b, L"nul sd < 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.tables.alt[C::kClassRational].sd = 0.0; checkForgedInvalidParams(b, L"alt sd == 0"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.tables.nul[C::kClassGeneric].mean = nan; checkForgedInvalidParams(b, L"nul mean == NaN"); }
				{ C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10); b.tables.alt[C::kClassGeneric].mean = inf; checkForgedInvalidParams(b, L"alt mean == Inf"); }

				// Contradictory metadata (tables.placeholder == true or mismatched depthClass) must also refuse calibrated status:
				{
					C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N10);
					b.tables.placeholder = true;
					Assert::IsFalse(b.IsCalibrated());
					C::DepthNullTableSet s = C::DepthNullTableSet::PlaceholderOnly();
					s.n10 = b;
					const C::DepthNullTableLookupResult r = s.Select(C::BitDepthClass::N10);
					Assert::IsFalse(r.calibrated);
					Assert::IsTrue(r.usedPlaceholderFallback);
				}
				{
					C::DepthNullTable b = MakeValidCalibratedTable(C::BitDepthClass::N8); // wrong depthClass placed into n10 slot!
					C::DepthNullTableSet s = C::DepthNullTableSet::PlaceholderOnly();
					s.n10 = b;
					const C::DepthNullTableLookupResult r = s.Select(C::BitDepthClass::N10);
					Assert::IsFalse(r.calibrated, L"mismatched depthClass stored in n10 slot must not validate as N10");
					Assert::IsTrue(r.usedPlaceholderFallback);
				}
			}

			// (e) End-to-end ChainEstimator integration:
			//     PlaceholderOnly() -> placeholderNullTableEvals > 0 and (with useDepthNullTables) depthNullTableFallbacks > 0;
			//     Calibrated N10 installed -> on 10-bit stream nullTablesCalibrated == true, depth == N10, placeholder == 0;
			//     On 8-bit stream with ONLY N10 calibrated -> refuses cross-substitution and falls back visibly!
			{
				const C::LineSnapshot snap10 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1, 10, 1000000, 61001u, true);
				const C::LineSnapshot snap8 = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 4, 20, 2000000, 61002u, true);

				// (e1) PlaceholderOnly with useDepthNullTables = true:
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.outputStepOverride = 1;
					cfg.useDepthNullTables = true;
					cfg.depthNullTables = C::DepthNullTableSet::PlaceholderOnly();
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					C::EvaluationDetail det;
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { det = d; });
					est.SetMode(C::EstimatorMode::On);

					clk.Set(1000.0);
					Assert::IsTrue(est.SubmitLines(snap10));
					const C::EstimatorCounters cnt = est.GetCounters();
					Assert::IsTrue(cnt.placeholderNullTableEvals >= 1, L"PlaceholderOnly visibly increments placeholderNullTableEvals");
					Assert::IsTrue(cnt.depthNullTableFallbacks >= 1, L"PlaceholderOnly with useDepthNullTables increments depthNullTableFallbacks");
					Assert::IsFalse(det.nullTablesCalibrated);
					Assert::IsTrue(det.nullTableDepthClass == C::BitDepthClass::N10);
				}

				// (e2) Install calibrated N10 ONLY via SetDepthNullTables(cal10, true):
				C::DepthNullTableSet cal10Only = C::DepthNullTableSet::PlaceholderOnly();
				cal10Only.n10 = MakeValidCalibratedTable(C::BitDepthClass::N10, 14.0, 2400, -0.02);

				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.outputStepOverride = 1;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetDepthNullTables(cal10Only, true);
					C::EvaluationDetail det;
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { det = d; });
					est.SetMode(C::EstimatorMode::On);

					clk.Set(1000.0);
					Assert::IsTrue(est.SubmitLines(snap10));
					const C::EstimatorCounters cnt = est.GetCounters();
					Assert::IsTrue(det.nullTablesCalibrated, L"calibrated N10 table active on 10-bit stream");
					Assert::IsTrue(det.nullTableDepthClass == C::BitDepthClass::N10);
					Assert::AreEqual(uint64_t(0), cnt.placeholderNullTableEvals, L"zero placeholder evals when N10 is calibrated");
					Assert::AreEqual(uint64_t(0), cnt.depthNullTableFallbacks, L"zero fallbacks when N10 is calibrated");
				}

				// (e3) Same calibrated-N10-only table set on an 8-bit stream (step = 4):
				//      MUST refuse cross-substitution from N10 and fall back visibly to Placeholder!
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.outputStepOverride = 4;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetDepthNullTables(cal10Only, true);
					C::EvaluationDetail det;
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { det = d; });
					est.SetMode(C::EstimatorMode::On);

					clk.Set(1000.0);
					Assert::IsTrue(est.SubmitLines(snap8));
					const C::EstimatorCounters cnt = est.GetCounters();
					Assert::IsFalse(det.nullTablesCalibrated, L"N10 table must NOT cross-substitute onto an 8-bit stream");
					Assert::IsTrue(det.nullTableDepthClass == C::BitDepthClass::N8);
					Assert::IsTrue(cnt.placeholderNullTableEvals >= 1, L"8-bit stream visibly falls back to placeholder");
					Assert::IsTrue(cnt.depthNullTableFallbacks >= 1, L"8-bit cross-depth miss visibly counted in depthNullTableFallbacks");
				}

				// (e4) HIGH-1 regression: 8-bit HDMI-loop stream (SetDiagnosticDepths(8, 8) + outputStepOverride = 1)
				//      with calibrated N10-only table set MUST classify as N8 and refuse N10 cross-substitution!
				{
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.outputStepOverride = 1;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetDiagnosticDepths(8, 8);
					est.SetDepthNullTables(cal10Only, true);
					C::EvaluationDetail det;
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { det = d; });
					est.SetMode(C::EstimatorMode::On);

					clk.Set(1000.0);
					Assert::IsTrue(est.SubmitLines(snap10));
					const C::EstimatorCounters cnt = est.GetCounters();
					Assert::IsTrue(det.nullTableDepthClass == C::BitDepthClass::N8,
						L"HIGH-1 regression: outputBits == 8 with step == 1 must resolve to BitDepthClass::N8");
					Assert::IsFalse(det.nullTablesCalibrated,
						L"HIGH-1 regression: N10 table must NOT cross-substitute onto outputBits == 8, step == 1 stream");
					Assert::IsTrue(cnt.placeholderNullTableEvals >= 1,
						L"HIGH-1 regression: 8-bit HDMI-loop stream visibly falls back to placeholder");
					Assert::IsTrue(cnt.depthNullTableFallbacks >= 1,
						L"HIGH-1 regression: 8-bit HDMI-loop miss visibly counted in depthNullTableFallbacks");
				}
			}
		}

		// 7. Default-OFF byte-identity across ALL Stage S3 switches:
		//    All S3 opt-in switches default OFF, default settings vs explicit default setters produce byte-identical
		//    published records, status lines, evaluation details, and zero opt-in S3 counters.
		TEST_METHOD(DefaultOffByteIdentityAcrossAllS3Switches)
		{
			using namespace S3Fx;

			const C::EstimatorSettings defEst;
			Assert::IsTrue(defEst.empiricalFloorMode == C::EmpiricalFloorMode::Off);
			Assert::IsFalse(defEst.eightBitPromotion);
			Assert::IsFalse(defEst.useDepthNullTables);
			Assert::IsFalse(defEst.depthNullTables.AnyCalibrated());
			Assert::IsFalse(defEst.depthNullTables.AllCalibrated());

			const SR::Tracker::Settings defTrk;
			Assert::IsFalse(defTrk.ditherFloorDetailAbove);

			ChainFx::ManualClock clkA, clkB;
			C::ChainEstimator estDefault(defEst, clkA.Fn());
			C::ChainEstimator estExplicit(defEst, clkB.Fn());
			estDefault.SetSynchronous(true);
			estExplicit.SetSynchronous(true);

			// Explicitly apply all Stage S3 setters with their default OFF values on estExplicit:
			estExplicit.SetEmpiricalFloorMode(C::EmpiricalFloorMode::Off);
			estExplicit.SetEightBitPromotion(false);
			estExplicit.SetDepthNullTables(C::DepthNullTableSet::PlaceholderOnly(), false);

			std::vector<C::EvaluationDetail> detA, detB;
			estDefault.SetEvaluationObserver([&](const C::EvaluationDetail& d) { detA.push_back(d); });
			estExplicit.SetEvaluationObserver([&](const C::EvaluationDetail& d) { detB.push_back(d); });

			estDefault.SetMode(C::EstimatorMode::On);
			estExplicit.SetMode(C::EstimatorMode::On);

			for (int i = 0; i < 6; ++i)
			{
				const uint64_t seq = static_cast<uint64_t>(10 + i * 10);
				const int64_t tUs = static_cast<int64_t>(i + 1) * 1000000;
				clkA.Set(static_cast<double>(i + 1) * 1500.0);
				clkB.Set(static_cast<double>(i + 1) * 1500.0);

				const int srcW = (i < 4) ? 1920 : kUhdW;
				const int srcH = (i < 4) ? 1080 : kUhdH;
				const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, srcW, srcH, kBilinear, 1,
					seq, tUs, 71001u + static_cast<uint64_t>(i), i == 0 || i == 4);

				Assert::IsTrue(estDefault.SubmitLines(snap));
				Assert::IsTrue(estExplicit.SubmitLines(snap));

				G::Record rA, rB;
				Assert::IsTrue(estDefault.Held().ReadLatest(0, rA));
				Assert::IsTrue(estExplicit.Held().ReadLatest(0, rB));
				Assert::IsTrue(ChainFx::SameBytes(G::Pack(rA), G::Pack(rB)),
					L"default settings and explicit OFF S3 setters must be byte-identical");
				Assert::AreEqual(estDefault.StatusLine(), estExplicit.StatusLine());
			}

			Assert::AreEqual<size_t>(6, detA.size());
			Assert::AreEqual<size_t>(6, detB.size());
			for (size_t i = 0; i < detA.size(); ++i)
			{
				Assert::IsTrue(detA[i].empiricalFloorMode == C::EmpiricalFloorMode::Off);
				Assert::IsTrue(detB[i].empiricalFloorMode == C::EmpiricalFloorMode::Off);
				Assert::IsFalse(detA[i].nullTablesCalibrated);
				Assert::IsFalse(detB[i].nullTablesCalibrated);
				Assert::AreEqual(detA[i].axis[0].floorCodes, detB[i].axis[0].floorCodes, 1e-12);
				Assert::AreEqual(detA[i].axis[1].floorCodes, detB[i].axis[1].floorCodes, 1e-12);
			}

			const C::EstimatorCounters cA = estDefault.GetCounters();
			const C::EstimatorCounters cB = estExplicit.GetCounters();
			Assert::AreEqual(uint64_t(6), cA.evaluations);
			Assert::AreEqual(uint64_t(6), cB.evaluations);
			Assert::AreEqual(uint64_t(0), cA.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), cB.empiricalFloorEvals);
			Assert::AreEqual(uint64_t(0), cA.empiricalFloorFallbacks);
			Assert::AreEqual(uint64_t(0), cB.empiricalFloorFallbacks);
			Assert::AreEqual(uint64_t(0), cA.empiricalFloorAllAxisFallbacks);
			Assert::AreEqual(uint64_t(0), cB.empiricalFloorAllAxisFallbacks);
			Assert::AreEqual(uint64_t(0), cA.empiricalFloorPartialAxisFallbacks);
			Assert::AreEqual(uint64_t(0), cB.empiricalFloorPartialAxisFallbacks);
			Assert::AreEqual(uint64_t(0), cA.depthNullTableFallbacks);
			Assert::AreEqual(uint64_t(0), cB.depthNullTableFallbacks);
			Assert::AreEqual(cA.placeholderNullTableEvals, cB.placeholderNullTableEvals);
		}

		// 8. Dump v4 schema extension (59 legacy + 17 new = 76 columns), live step-mark + tentative-bucket +
		//    empirical-floor + null-table row verification, decision-neutrality negative control, and RAII cleanup.
		TEST_METHOD(DumpV4AppendsStepMarkTentativeBucketAndEmpiricalFloorColumns)
		{
			using namespace S3Fx;

			// (a) Verify EvalsHeaderV4() preserves all 59 legacy columns verbatim and appends all 17 new columns (76 total).
			const std::string legacy59 =
				"seq,t_s,shot_id,flags,cut,blank,fade,clear_window,menu_hold,abr_step,area_changed,window_cleared,step_codes,floor_codes,coded_bits,output_bits,"
				"classical_ok,classical_h,classical_v,safety,state_x,state_y,meas_n_x,meas_n_y,app_state_x,app_state_y,app_n_x,app_n_y,N_x,N_y,"
				"llr_x,llr_y,informative_x,informative_y,hold_x,hold_y,probe_seen_x,probe_seen_y,latched,latch_nx,latch_ny,latch_event,"
				"latch_confirmed,latch_deferred_now,latch_defer_cancelled,latch_held_back,clock_s,deferred,dwelling,defer_cause,defer_since_s,"
				"last_confirm_s,newest_credit_x_s,newest_credit_y_s,acc_x,acc_y,bar,c1,cost_ms";
			const std::string appended17 =
				"step_mark_state,step_mark_t0_s,step_mark_blocks,step_mark_confirmed,step_mark_cancelled,"
				"tentative_bucket_active,tentative_bucket_evals,tentative_bucket_voided,"
				"emp_floor_mode,floor_theoretical,emp_floor_x,emp_floor_y,emp_floor_valid_x,emp_floor_valid_y,emp_floor_fallback,"
				"null_calibrated,null_depth_class";
			const std::string expectedHeader = legacy59 + "," + appended17;
			const std::string actualHeader(SourceIdentity::Diag::EvalsHeaderV4());

			Assert::AreEqual(expectedHeader, actualHeader, L"EvalsHeaderV4() must match exact 59 legacy + 17 new columns");
			const std::vector<std::string> headerCols = SplitCsvRow(actualHeader);
			Assert::AreEqual<size_t>(76, headerCols.size(), L"EvalsHeaderV4() has exactly 76 columns");

			// Map column names to indices for strict row verification:
			auto colIdx = [&](const char* name) -> size_t {
				for (size_t i = 0; i < headerCols.size(); ++i)
					if (headerCols[i] == name)
						return i;
				Assert::Fail(L"expected Dump v4 column missing from EvalsHeaderV4()");
				return 0;
			};

			const size_t iSeq = colIdx("seq");
			const size_t iSmState = colIdx("step_mark_state");
			const size_t iSmT0 = colIdx("step_mark_t0_s");
			const size_t iSmBlocks = colIdx("step_mark_blocks");
			const size_t iSmConfirmed = colIdx("step_mark_confirmed");
			const size_t iSmCancelled = colIdx("step_mark_cancelled");
			const size_t iTbActive = colIdx("tentative_bucket_active");
			const size_t iTbEvals = colIdx("tentative_bucket_evals");
			const size_t iTbVoided = colIdx("tentative_bucket_voided");
			const size_t iEmpMode = colIdx("emp_floor_mode");
			const size_t iFloorTheo = colIdx("floor_theoretical");
			const size_t iEmpX = colIdx("emp_floor_x");
			const size_t iEmpY = colIdx("emp_floor_y");
			const size_t iEmpValidX = colIdx("emp_floor_valid_x");
			const size_t iEmpValidY = colIdx("emp_floor_valid_y");
			const size_t iEmpFallback = colIdx("emp_floor_fallback");
			const size_t iNullCal = colIdx("null_calibrated");
			const size_t iNullDepth = colIdx("null_depth_class");

			// Build deterministic synthetic sequence with TPDF-scale loop noise (~1.63 codes, step = 1, 50% flat so
			// lower-quartile empirical floor accurately measures ~1.63 while textured segments stay abundant on both axes):
			//   - EmpiricalFloorMode::Live adapts active floorCodes to ~1.63 (so flat TPDF segments do not false-HOLD),
			//     while Dump v4 logs emp_floor_mode, floor_theoretical (~0.289), AND emp_floor_x/y (~1.63).
			//   - Evals 0..4 (t = 1..5 s): 1080p->4K with agreeing classical A -> latches 1920x1080 (StepMark Idle)
			//   - Spurious ABR mark at t0 = 6.0 s -> Eval 5 (t = 6 s, static 1080p): StepMark Pending, tentative bucket active (1 eval)
			//   - Eval 6 (t = 7 s, static 1080p): unconfirmed timeout -> step_mark_cancelled
			//   - Real ABR step-up mark at t0 = 8.0 s -> Eval 7 (t = 8 s, native 4K): confirmed by chain contradiction -> confirmed + voided
			const double tpdfSigma8 = C::TpdfDitherFloorCodes(4.0);
			std::vector<C::LineSnapshot> seqSnaps;
			seqSnaps.reserve(8);
			for (int i = 0; i < 7; ++i)
			{
				seqSnaps.push_back(MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kBilinear, 1,
					static_cast<uint64_t>(10 + i * 10), static_cast<int64_t>(i + 1) * 1000000,
					81001u + static_cast<uint64_t>(i), i == 0, tpdfSigma8, false, 0.50));
			}
			seqSnaps.push_back(MakeSynthSnapshot(kUhdW, kUhdH, kUhdW, kUhdH, kBilinear, 1,
				80, 8000000, 81099u, false, tpdfSigma8, false, 0.50));

			auto runDumpScenario = [&](std::vector<G::Packed>& outPacked, C::EstimatorCounters& outCnt) {
				ChainFx::ManualClock clk;
				C::EstimatorSettings cfg;
				cfg.safetyRules = true;
				cfg.jointLatch.enabled = true;
				cfg.stepMark.enabled = true;
				cfg.conventionDrivenProposals = true;
				cfg.empiricalFloorMode = C::EmpiricalFloorMode::Live;
				cfg.useDepthNullTables = true;
				cfg.depthNullTables = C::DepthNullTableSet::PlaceholderOnly();
				cfg.outputStepOverride = 1;
				C::ChainEstimator est(cfg, clk.Fn());
				est.SetSynchronous(true);
				est.SetDiagnosticTag("s3_dump_v4");
				est.SetDiagnosticDepths(8, 10);
				est.SetMode(C::EstimatorMode::On);

				outPacked.clear();
				for (size_t i = 0; i < seqSnaps.size(); ++i)
				{
					clk.Set(static_cast<double>(i + 1) * 1500.0);
					if (i < 5)
						est.SetClassicalFresh(true, 0.5, 0.5, false);
					else
						est.SetClassicalFresh(false, 1.0, 1.0, false); // keep classical silent during step-mark phase so eval 5 stays pending!

					if (i == 5)
						est.NoteAbrStepEvent(6000000, +0.25, seqSnaps[i].seq); // spurious mark at t0 = 6.0 s
					if (i == 7)
						est.NoteAbrStepEvent(8000000, +0.50, seqSnaps[i].seq); // real step-up to native 4K at t0 = 8.0 s

					Assert::IsTrue(est.SubmitLines(seqSnaps[i]));
					if (i == 4)
						Assert::AreEqual(1920, est.StatusInput().latchNx, L"fixture: Live empirical floor latches 1920x1080 before ABR step mark");
					if (i == 7)
						Assert::AreEqual(0, est.StatusInput().latchNx, L"fixture: confirmed ABR step-up releases latch to native");
					G::Record rec;
					Assert::IsTrue(est.Held().ReadLatest(0, rec));
					outPacked.push_back(G::Pack(rec));
				}
				outCnt = est.GetCounters();
				est.Stop();
			};

			// RAII guard creates unique temp dir and restores VP_SID_DUMP_V4_DIR even if an assertion fails.
			ScopedDumpV4Dir dumpGuard("evals76");

			// Negative control: with VP_SID_DUMP_V4_DIR unset, NO CSV files are written!
			dumpGuard.SetEnv("");
			std::vector<G::Packed> packOff;
			C::EstimatorCounters cntOff;
			runDumpScenario(packOff, cntOff);
			Assert::AreEqual(0, dumpGuard.CountCsv(), L"negative control: Dump v4 OFF writes zero CSV files");

			// Now enable Dump v4 on the EXACT SAME sequence and verify decision-neutrality + 76-column rows!
			dumpGuard.SetEnv(dumpGuard.Dir());
			std::vector<G::Packed> packOn;
			C::EstimatorCounters cntOn;
			runDumpScenario(packOn, cntOn);

			Assert::AreEqual(packOff.size(), packOn.size());
			for (size_t i = 0; i < packOff.size(); ++i)
				Assert::IsTrue(ChainFx::SameBytes(packOff[i], packOn[i]), L"Dump v4 must stay decision-neutral (byte-identical records)");
			Assert::AreEqual(cntOff.evaluations, cntOn.evaluations);
			Assert::AreEqual(cntOff.stepMark.marksRaised, cntOn.stepMark.marksRaised);
			Assert::AreEqual(cntOff.stepMark.unconfirmedTimeouts, cntOn.stepMark.unconfirmedTimeouts);
			Assert::AreEqual(cntOff.stepMark.confirmedByChain, cntOn.stepMark.confirmedByChain);
			Assert::AreEqual(uint64_t(1), cntOn.stepMark.unconfirmedTimeouts, L"fixture: spurious mark timed out unconfirmed");
			Assert::AreEqual(uint64_t(1), cntOn.stepMark.confirmedByChain, L"fixture: real step-up confirmed by chain");

			const std::string csvPath = dumpGuard.Dir() + "\\s3_dump_v4.b_evals.csv";
			std::ifstream in(csvPath);
			Assert::IsTrue(in.good(), L"b_evals.csv was written");

			std::string fileHeader;
			std::getline(in, fileHeader);
			Assert::AreEqual(expectedHeader, fileHeader, L"emitted b_evals.csv header matches 76-column contract");

			std::vector<std::vector<std::string>> rows;
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty())
					continue;
				const std::vector<std::string> fields = SplitCsvRow(line);
				Assert::AreEqual<size_t>(76, fields.size(), L"EVERY emitted b_evals.csv row must have exact 76 columns");
				rows.push_back(fields);
			}
			in.close();
			Assert::AreEqual<size_t>(8, rows.size(), L"one b_evals.csv row per evaluation");

			// Verify empirical floor & null table columns on EVERY row:
			for (size_t r = 0; r < rows.size(); ++r)
			{
				const auto& f = rows[r];
				Assert::IsFalse(f[iEmpMode].empty(), L"emp_floor_mode populated");
				const double fTheo = std::stod(f[iFloorTheo]);
				const double empX = std::stod(f[iEmpX]);
				const double empY = std::stod(f[iEmpY]);
				Assert::IsTrue(fTheo > 0.20 && fTheo < 0.35, L"floor_theoretical near ~0.289 for step=1");
				Assert::IsTrue(empX > 1.2 && empX < 2.2, L"emp_floor_x reflects ~1.63 TPDF loop noise");
				Assert::IsTrue(empY > 1.2 && empY < 2.2, L"emp_floor_y reflects ~1.63 TPDF loop noise");
				Assert::AreEqual(std::string("1"), f[iEmpValidX], L"emp_floor_valid_x == 1");
				Assert::AreEqual(std::string("1"), f[iEmpValidY], L"emp_floor_valid_y == 1");
				Assert::AreEqual(std::string("0"), f[iEmpFallback], L"emp_floor_fallback == 0 on noisy frames");
				Assert::AreEqual(std::string("0"), f[iNullCal], L"null_calibrated == 0 (Placeholder)");
				Assert::IsFalse(f[iNullDepth].empty(), L"null_depth_class populated");
				Assert::AreNotEqual(std::string("0"), f[iNullDepth]);
				Assert::AreNotEqual(std::string("unknown"), f[iNullDepth]);
				Assert::AreNotEqual(std::string("Unknown"), f[iNullDepth]);
			}

			// Row 0 (seq 10, Idle):
			Assert::AreEqual(std::string("10"), rows[0][iSeq]);
			Assert::AreEqual(std::string("0"), rows[0][iSmBlocks], L"idle row does not block entry/move");
			Assert::AreEqual(std::string("0"), rows[0][iTbActive], L"idle row has inactive tentative bucket");
			Assert::AreEqual(std::string("0"), rows[0][iTbEvals], L"idle row has 0 tentative bucket evals");

			// Row 5 (seq 60, Pending ABR mark raised at t0 = 6.0 s, 1st post-t0 static eval):
			Assert::AreEqual(std::string("60"), rows[5][iSeq]);
			Assert::AreNotEqual(rows[0][iSmState], rows[5][iSmState], L"step_mark_state transitions from Idle to Pending");
			Assert::AreEqual(6.0, std::stod(rows[5][iSmT0]), 1e-3, L"step_mark_t0_s records exact pending mark timestamp");
			Assert::AreEqual(std::string("1"), rows[5][iSmBlocks], L"pending row sets step_mark_blocks == 1");
			Assert::AreEqual(std::string("1"), rows[5][iTbActive], L"pending row sets tentative_bucket_active == 1");
			Assert::IsTrue(std::stoi(rows[5][iTbEvals]) >= 1, L"pending row records >= 1 tentative_bucket_evals");

			// Row 6 (seq 70, unconfirmed timeout after 2nd post-t0 static eval):
			Assert::AreEqual(std::string("70"), rows[6][iSeq]);
			Assert::IsTrue(std::stod(rows[6][iSmCancelled]) >= 1.0, L"unconfirmed timeout row visibly marks step_mark_cancelled");

			// Row 7 (seq 80, real step-up to native 4K confirmed by chain contradiction):
			Assert::AreEqual(std::string("80"), rows[7][iSeq]);
			Assert::IsTrue(std::stod(rows[7][iSmConfirmed]) >= 1.0, L"confirmed step-up row visibly marks step_mark_confirmed");
			Assert::IsTrue(std::stod(rows[7][iTbVoided]) >= 1.0, L"confirmed step-up row visibly marks tentative_bucket_voided");
		}

		// 9. SID-OPT Partition B: Stack-buffer + std::nth_element empirical floor matches an independent direct
		//    full-std::sort MAD + interpolated-quantile reference bit-exactly (< 1e-12) across even, odd, short,
		//    and large segment lengths — with a skewed even-length negative control catching forgotten pair-averaging.
		TEST_METHOD(EmpiricalFloorStackNthElementMatchesFullSortReferenceBitExact)
		{
			using namespace S3Fx;

			auto medianFullSort = [](std::vector<double> vals, bool brokenUpperOnly) -> double {
				std::sort(vals.begin(), vals.end());
				const size_t n = vals.size();
				if (brokenUpperOnly || (n % 2 == 1))
					return vals[n / 2];
				return 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
			};

			auto refEmpiricalFloor = [&](const C::LineSet& ls, double stepCodes, const C::EdgeStatsSettings& es,
				bool brokenUpperOnly, double& outRaw, int& outPosSegs) -> double {
				const int segLen = es.segPx >= 8 ? es.segPx : C::kSegPx;
				const double sqrt6 = std::sqrt(6.0);
				const double theo = stepCodes / std::sqrt(12.0);
				std::vector<double> segSigmas;
				for (int li = 0; li < ls.count; ++li)
				{
					const double* y = ls.Line(li);
					for (int start = 0; start < ls.length; start += segLen)
					{
						int end = start + segLen;
						if (end > ls.length)
							end = ls.length;
						if (end - start < 5)
							continue;
						std::vector<double> diffs;
						diffs.reserve(static_cast<size_t>(end - start - 2));
						for (int i = start + 1; i + 1 < end; ++i)
							diffs.push_back(y[i - 1] - 2.0 * y[i] + y[i + 1]);
						const double med = medianFullSort(diffs, brokenUpperOnly);
						std::vector<double> absDev(diffs.size());
						for (size_t k = 0; k < diffs.size(); ++k)
							absDev[k] = std::fabs(diffs[k] - med);
						const double mad = medianFullSort(absDev, brokenUpperOnly);
						if (mad > 0.0)
							segSigmas.push_back((RobustStats::kMadToSigma * mad) / sqrt6);
					}
				}
				outPosSegs = static_cast<int>(segSigmas.size());
				if (segSigmas.empty())
				{
					outRaw = 0.0;
					return theo;
				}
				std::sort(segSigmas.begin(), segSigmas.end());
				const double q = (std::max)(0.0, (std::min)(0.5, es.quietSegmentQuantile));
				const double pos = q * static_cast<double>(segSigmas.size() - 1);
				const size_t lo = static_cast<size_t>(std::floor(pos));
				const size_t hi = static_cast<size_t>(std::ceil(pos));
				const double frac = pos - static_cast<double>(lo);
				outRaw = (1.0 - frac) * segSigmas[lo] + frac * segSigmas[hi];
				return (std::max)(outRaw, theo);
			};

			auto buildSynthLineSet = [](int count, int length, int srcLen, int step, double extraSigma,
				bool tpdfDither, double flatFrac, uint64_t seed) {
				C::LineSet ls;
				ls.axis = 0;
				ls.axisLen = length;
				ls.origin = 0;
				ls.Resize(count, length);
				SplitMix64 rng(seed);
				std::vector<double> buf;
				for (int j = 0; j < count; ++j)
				{
					SynthLine1D(srcLen, length, kBilinear, step, rng, buf, extraSigma, tpdfDither, flatFrac);
					double* dst = ls.Line(j);
					for (int i = 0; i < length; ++i)
						dst[i] = buf[static_cast<size_t>(i)];
				}
				return ls;
			};

			struct FloorCase
			{
				int count, length, srcLen, step, segPx;
				double extraSigma, flatFrac;
				bool tpdf;
				uint64_t seed;
				const wchar_t* label;
			};
			const FloorCase cases[] = {
				// Even segment (256 -> 254 diffs), 8-bit TPDF mixed quiet/detailed:
				{ 16, 1024, 512, 4, 256, 0.0, 0.60, true, 0x9101ull, L"even segPx=256 8-bit TPDF" },
				// Odd segment (127 -> 125 diffs) + odd tail (111 -> 109 diffs), 10-bit noisy:
				{ 12, 1000, 500, 1, 127, 0.85, 0.55, false, 0x9102ull, L"odd segPx=127 10-bit noisy" },
				// Short even segment (8 -> 6 diffs) & short odd segment (9 -> 7 diffs):
				{ 8, 256, 128, 1, 8, 1.35, 0.50, false, 0x9103ull, L"short even segPx=8" },
				{ 8, 256, 128, 1, 9, 1.35, 0.50, false, 0x9104ull, L"short odd segPx=9" },
				// Larger-than-default segment (512 -> 510 diffs) covering spill/heap fallback if stack cap is 256:
				{ 8, 1024, 512, 1, 512, 1.63, 0.65, false, 0x9105ull, L"large segPx=512" }
			};

			for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci)
			{
				const FloorCase& tc = cases[ci];
				const C::LineSet ls = buildSynthLineSet(tc.count, tc.length, tc.srcLen, tc.step,
					tc.extraSigma, tc.tpdf, tc.flatFrac, tc.seed);
				C::EdgeStatsSettings es;
				es.enabled = true;
				es.segPx = tc.segPx;

				const C::EmpiricalFloorResult got = C::EstimateEmpiricalFloor(ls, static_cast<double>(tc.step), es);
				double refRaw = 0.0;
				int refPos = 0;
				const double refFloor = refEmpiricalFloor(ls, static_cast<double>(tc.step), es, false, refRaw, refPos);

				Assert::IsTrue(got.valid, tc.label);
				Assert::AreEqual(refPos, got.positiveSegments, tc.label);
				Assert::IsTrue(std::fabs(got.floorCodes - refFloor) < 1e-12, tc.label);
				Assert::IsTrue(std::fabs(got.empiricalRawCodes - refRaw) < 1e-12, tc.label);
			}

			// Mandatory in-test negative control (Rule 3): skewed even-length segment (length 8 -> 6 second differences
			// d = { 0, 0, 0, 10, 10, 20 }). True MeanOfMiddle median = 5.0 -> MAD = 5.0; a broken even-length median
			// taking upper element d[3] = 10.0 yields MAD = 10.0 (2x error)!
			{
				const double ySkew[8] = { 500.0, 500.0, 500.0, 500.0, 500.0, 510.0, 530.0, 570.0 };
				C::LineSet lsSkew;
				lsSkew.axis = 0;
				lsSkew.axisLen = 8;
				lsSkew.origin = 0;
				lsSkew.Resize(1, 8);
				for (int i = 0; i < 8; ++i)
					lsSkew.Line(0)[i] = ySkew[i];

				C::EdgeStatsSettings es;
				es.enabled = true;
				es.segPx = 8;

				const C::EmpiricalFloorResult got = C::EstimateEmpiricalFloor(lsSkew, 1.0, es);
				double refRaw = 0.0, brokenRaw = 0.0;
				int refPos = 0, brokenPos = 0;
				const double refFloor = refEmpiricalFloor(lsSkew, 1.0, es, false, refRaw, refPos);
				const double brokenFloor = refEmpiricalFloor(lsSkew, 1.0, es, true, brokenRaw, brokenPos);

				Assert::IsTrue(got.valid);
				Assert::AreEqual(1, got.positiveSegments);
				Assert::IsTrue(std::fabs(got.floorCodes - refFloor) < 1e-12,
					L"EstimateEmpiricalFloor matches exact MeanOfMiddle reference on skewed even segment");
				Assert::IsTrue(std::fabs(brokenFloor - refFloor) > 2.5,
					L"negative control: broken even-length upper-only median deviates by 2x (> 2.5 codes)");
			}

			// LOW-2 guard check: RobustStats::QuantileInPlace with NaN q returns ifEmpty, negative q clamps to min,
			// and q > 1 clamps to max across all RankRules without UB.
			{
				const double nanQ = std::numeric_limits<double>::quiet_NaN();
				const double sentinel = -7.5;
				const RobustStats::RankRule rules[3] = {
					RobustStats::RankRule::Interpolated,
					RobustStats::RankRule::FloorIndex,
					RobustStats::RankRule::NearestRank
				};
				for (int ri = 0; ri < 3; ++ri)
				{
					double bufNan[5] = { 30.0, 10.0, 50.0, 20.0, 40.0 };
					double bufNeg[5] = { 30.0, 10.0, 50.0, 20.0, 40.0 };
					double bufHi[5] = { 30.0, 10.0, 50.0, 20.0, 40.0 };
					Assert::AreEqual(sentinel, RobustStats::QuantileInPlace(bufNan, 5, nanQ, rules[ri], sentinel), 1e-12,
						L"QuantileInPlace(NaN) returns ifEmpty without UB");
					Assert::AreEqual(10.0, RobustStats::QuantileInPlace(bufNeg, 5, -0.25, rules[ri], sentinel), 1e-12,
						L"QuantileInPlace(negative q) clamps cleanly to minimum element without UB");
					Assert::AreEqual(50.0, RobustStats::QuantileInPlace(bufHi, 5, 1.5, rules[ri], sentinel), 1e-12,
						L"QuantileInPlace(q > 1) clamps cleanly to maximum element without UB");
					Assert::AreEqual(sentinel, RobustStats::QuantileInPlace(nullptr, 0, 0.25, rules[ri], sentinel), 1e-12,
						L"QuantileInPlace(empty) returns ifEmpty");
				}
			}
		}

		// 10. SID-OPT Partition B: PolyphaseProposer maximal-period folding matches an independent direct per-period
		//     5-tap polyphase least-squares reference (Gaussian elimination) to < 1e-10 across qMax = { 16, 12, 9 }
		//     on 2:1 and 3:1 upscaled lines, with a broken phase-folding negative control (Rule 3).
		TEST_METHOD(PolyphaseProposerMaximalFoldingMatchesDirectAllPeriodReference)
		{
			using namespace S3Fx;

			// Independent 5x5 Gaussian elimination with partial pivoting (distinct solver from PolyphaseProposer's Cholesky).
			auto fitNormalEqRef = [](const double st[22], double& outRss, double& outCnt) -> bool {
				constexpr int kT = 5;
				outCnt = st[21];
				if (outCnt < static_cast<double>(4 * kT))
					return false;
				double M[kT][kT + 1];
				int idx = 0;
				for (int i = 0; i < kT; ++i)
				{
					for (int j = 0; j <= i; ++j)
					{
						M[i][j] = st[idx];
						M[j][i] = st[idx];
						++idx;
					}
					M[i][kT] = st[15 + i];
				}
				double tr = 0.0;
				for (int i = 0; i < kT; ++i)
					tr += M[i][i];
				const double ridge = 1e-9 * tr / static_cast<double>(kT) + 1e-12;
				for (int i = 0; i < kT; ++i)
					M[i][i] += ridge;

				for (int col = 0; col < kT; ++col)
				{
					int piv = col;
					for (int r = col + 1; r < kT; ++r)
						if (std::fabs(M[r][col]) > std::fabs(M[piv][col]))
							piv = r;
					if (!(std::fabs(M[piv][col]) > 1e-300))
						return false;
					if (piv != col)
						for (int c = col; c <= kT; ++c)
							std::swap(M[col][c], M[piv][c]);
					for (int r = col + 1; r < kT; ++r)
					{
						const double f = M[r][col] / M[col][col];
						for (int c = col; c <= kT; ++c)
							M[r][c] -= f * M[col][c];
					}
				}
				double w[kT] = {};
				for (int i = kT - 1; i >= 0; --i)
				{
					double rhs = M[i][kT];
					for (int c = i + 1; c < kT; ++c)
						rhs -= M[i][c] * w[c];
					w[i] = rhs / M[i][i];
				}
				double fit = 0.0;
				for (int i = 0; i < kT; ++i)
					fit += st[15 + i] * w[i];
				outRss = st[20] - fit;
				if (outRss < 0.0)
					outRss = 0.0;
				return true;
			};

			// Independent direct all-period accumulator (accumulates EVERY q = 2..qMax directly, zero folding).
			auto directPolyphaseReference = [&](const std::vector<C::LineSet>& evals, const C::ProposerSettings& ps,
				std::vector<C::PeriodScore>& outScores, std::vector<std::vector<double>>& outPerQStats) {
				constexpr int kT = 5;
				constexpr int kTerms = 22;
				outPerQStats.assign(static_cast<size_t>(ps.qMax + 1), std::vector<double>());
				for (int q = 2; q <= ps.qMax; ++q)
					outPerQStats[static_cast<size_t>(q)].assign(static_cast<size_t>(q) * kTerms, 0.0);
				double pooled[kTerms] = {};

				for (size_t ei = 0; ei < evals.size(); ++ei)
				{
					const C::LineSet& ls = evals[ei];
					const int nl = (std::min)(ls.count, (std::max)(1, ps.maxLines));
					std::vector<double> v(static_cast<size_t>(ls.length), 0.0);
					for (int li = 0; li < nl; ++li)
					{
						const double* y = ls.Line((li * ls.count) / nl);
						double mean = 0.0;
						for (int k = 0; k < ls.length; ++k)
							mean += y[k];
						mean /= static_cast<double>(ls.length);
						for (int k = 0; k < ls.length; ++k)
							v[static_cast<size_t>(k)] = y[k] - mean;
						for (int k = 3; k <= ls.length - 3; ++k)
						{
							const double f[kT] = {
								v[static_cast<size_t>(k - 3)], v[static_cast<size_t>(k - 2)],
								v[static_cast<size_t>(k - 1)], v[static_cast<size_t>(k + 1)],
								v[static_cast<size_t>(k + 2)]
							};
							const double t = v[static_cast<size_t>(k)];
							double tm[kTerms];
							int idx = 0;
							for (int i = 0; i < kT; ++i)
								for (int j = 0; j <= i; ++j)
									tm[idx++] = f[i] * f[j];
							for (int i = 0; i < kT; ++i)
								tm[15 + i] = f[i] * t;
							tm[20] = t * t;
							tm[21] = 1.0;
							for (int e = 0; e < kTerms; ++e)
								pooled[e] += tm[e];
							for (int q = 2; q <= ps.qMax; ++q)
							{
								const int ph = (ls.origin + k) % q;
								double* dst = &outPerQStats[static_cast<size_t>(q)][static_cast<size_t>(ph) * kTerms];
								for (int e = 0; e < kTerms; ++e)
									dst[e] += tm[e];
							}
						}
					}
				}

				outScores.clear();
				double rssPooled = 0.0, total = 0.0;
				Assert::IsTrue(fitNormalEqRef(pooled, rssPooled, total) && total >= static_cast<double>(ps.minSamples));
				const double lnN = std::log(total);
				for (int q = 2; q <= ps.qMax; ++q)
				{
					double rssQ = 0.0;
					double bestVar = 1e300;
					int lowPhase = 0;
					bool ok = true;
					for (int ph = 0; ph < q; ++ph)
					{
						double rc = 0.0, cc = 0.0;
						if (!fitNormalEqRef(&outPerQStats[static_cast<size_t>(q)][static_cast<size_t>(ph) * kTerms], rc, cc))
						{
							ok = false;
							break;
						}
						rssQ += rc;
						const double var = rc / cc;
						if (var < bestVar)
						{
							bestVar = var;
							lowPhase = ph;
						}
					}
					if (!ok || !(rssQ > 0.0))
						continue;
					C::PeriodScore sc;
					sc.q = q;
					sc.gain = total * std::log(rssPooled / rssQ);
					const double dof = static_cast<double>(kT * (q - 1));
					sc.ratio = sc.gain / dof;
					sc.dBic = sc.gain - dof * lnN;
					sc.lowVarPhase = lowPhase;
					outScores.push_back(sc);
				}
			};

			auto makeUpscaledEvals = [](int srcN, int outN, int origin, uint64_t seedBase) {
				std::vector<C::LineSet> evals(2);
				for (int e = 0; e < 2; ++e)
				{
					C::LineSet& ls = evals[static_cast<size_t>(e)];
					ls.axis = 0;
					ls.axisLen = outN;
					ls.origin = origin;
					ls.Resize(16, 1200);
					SplitMix64 rng(seedBase + static_cast<uint64_t>(e * 997));
					std::vector<double> full;
					for (int j = 0; j < 16; ++j)
					{
						SynthLine1D(srcN, outN, kBilinear, 1, rng, full, 0.0, false, 0.0);
						double* dst = ls.Line(j);
						for (int i = 0; i < 1200; ++i)
							dst[i] = full[static_cast<size_t>(origin + i)];
					}
				}
				return evals;
			};

			const std::vector<C::LineSet> evals2to1 = makeUpscaledEvals(1920, kUhdW, 37, 0xA201ull);
			const std::vector<C::LineSet> evals3to1 = makeUpscaledEvals(1280, kUhdW, 41, 0xA301ull);

			const int qMaxList[3] = { 16, 12, 9 };
			for (int mode = 0; mode < 2; ++mode)
			{
				const std::vector<C::LineSet>& evals = (mode == 0) ? evals2to1 : evals3to1;
				const int expectedQ = (mode == 0) ? 2 : 3;
				const int expectedN = (mode == 0) ? 1920 : 1280;

				for (int qi = 0; qi < 3; ++qi)
				{
					C::ProposerSettings ps;
					ps.qMax = qMaxList[qi];
					C::PolyphaseProposer prop(ps);
					for (size_t e = 0; e < evals.size(); ++e)
						Assert::IsTrue(prop.AddEvaluation(evals[e]));

					C::PeriodScore gotScores[32];
					const int gotCount = prop.AllScores(gotScores, 32);

					std::vector<C::PeriodScore> refScores;
					std::vector<std::vector<double>> refPerQStats;
					directPolyphaseReference(evals, ps, refScores, refPerQStats);

					Assert::AreEqual(static_cast<int>(refScores.size()), gotCount, L"same scored period count across qMax");
					Assert::AreEqual(ps.qMax - 1, gotCount, L"every period 2..qMax scored");

					for (int i = 0; i < gotCount; ++i)
					{
						Assert::AreEqual(refScores[static_cast<size_t>(i)].q, gotScores[i].q);
						const double gainTol = 1e-8 * (std::max)(1.0, std::fabs(refScores[static_cast<size_t>(i)].gain));
						const double ratioTol = 1e-8 * (std::max)(1.0, std::fabs(refScores[static_cast<size_t>(i)].ratio));
						const double dbicTol = 1e-8 * (std::max)(1.0, std::fabs(refScores[static_cast<size_t>(i)].dBic));
						Assert::IsTrue(std::fabs(gotScores[i].gain - refScores[static_cast<size_t>(i)].gain) <= gainTol,
							L"folded period gain matches direct per-period reference within tight relative+absolute tolerance (1e-8)");
						Assert::IsTrue(std::fabs(gotScores[i].ratio - refScores[static_cast<size_t>(i)].ratio) <= ratioTol,
							L"folded period ratio matches direct per-period reference within tight relative+absolute tolerance (1e-8)");
						Assert::IsTrue(std::fabs(gotScores[i].dBic - refScores[static_cast<size_t>(i)].dBic) <= dbicTol,
							L"folded period dBIC matches direct per-period reference within tight relative+absolute tolerance (1e-8)");
						Assert::AreEqual(refScores[static_cast<size_t>(i)].lowVarPhase, gotScores[i].lowVarPhase,
							L"folded lowVarPhase matches direct reference");
					}

					// Verify top proposed period matches the true upscale factor, and true latent size is proposed under it
					// (PolyphaseProposer.h:20-21 proposes all coprime m/q sorted nearest 0.5; for q=3 both 1/3=1280 and 2/3=2560):
					C::ProposedCandidate cands[C::kMaxCandidates];
					const int pc = prop.Propose(kUhdW, nullptr, 0, cands, C::kMaxCandidates);
					Assert::IsTrue(pc >= 1);
					Assert::AreEqual(expectedQ, cands[0].q, L"top polyphase period matches true upscale factor");
					if (mode == 0)
					{
						Assert::AreEqual(expectedN, cands[0].n, L"2:1 top proposed size matches true latent 1920");
					}
					else
					{
						bool found1280 = false;
						bool found2560 = false;
						for (int k = 0; k < pc; ++k)
						{
							if (cands[k].q == expectedQ && cands[k].n == 1280) found1280 = true;
							if (cands[k].q == expectedQ && cands[k].n == 2560) found2560 = true;
						}
						Assert::IsTrue(found1280 && found2560,
							L"3:1 period q=3 proposes both coprime fractions 1/3 (1280) and 2/3 (2560)");
					}

					// Mandatory in-test negative control (Rule 3) on qMax == 16:
					// Fold from production's maximal multiple M = (16 / q) * q (16 for q = 2, 15 for q = 3):
					// mis-folding by contiguous blocks (c * block + b) instead of stride-q (c + k * q)
					// mixes all q phases into every bucket and collapses polyphase gain by > 90%!
					if (ps.qMax == 16)
					{
						constexpr int kTerms = 22;
						const int q = expectedQ;
						const int M = (16 / q) * q; // 16 for 2:1, 15 for 3:1
						const int block = M / q;    // 8 for 2:1, 5 for 3:1
						double pooled[kTerms] = {};
						for (int ph = 0; ph < M; ++ph)
							for (int e = 0; e < kTerms; ++e)
								pooled[e] += refPerQStats[static_cast<size_t>(M)][static_cast<size_t>(ph) * kTerms + e];
						double rssPooled = 0.0, total = 0.0;
						Assert::IsTrue(fitNormalEqRef(pooled, rssPooled, total));

						double rssGood = 0.0, rssBroken = 0.0;
						for (int c = 0; c < q; ++c)
						{
							double stGood[kTerms] = {};
							double stBroken[kTerms] = {};
							for (int cm = c; cm < M; cm += q)
								for (int e = 0; e < kTerms; ++e)
									stGood[e] += refPerQStats[static_cast<size_t>(M)][static_cast<size_t>(cm) * kTerms + e];
							for (int b = 0; b < block; ++b)
								for (int e = 0; e < kTerms; ++e)
									stBroken[e] += refPerQStats[static_cast<size_t>(M)][static_cast<size_t>(c * block + b) * kTerms + e];
							double rg = 0.0, cg = 0.0, rb = 0.0, cb = 0.0;
							Assert::IsTrue(fitNormalEqRef(stGood, rg, cg));
							Assert::IsTrue(fitNormalEqRef(stBroken, rb, cb));
							rssGood += rg;
							rssBroken += rb;
						}
						const double gainGood = total * std::log(rssPooled / rssGood);
						const double gainBroken = total * std::log(rssPooled / rssBroken);
						Assert::IsTrue(gainGood > 1000.0, L"fixture: true stride-q folded polyphase gain is strong (> 1000 nats)");
						Assert::IsTrue(std::fabs(gainGood - gotScores[q - 2].gain) <= 1e-8 * (std::max)(1.0, std::fabs(gainGood)),
							L"true stride-q fold matches PolyphaseProposer within 1e-8 relative tolerance");
						Assert::IsTrue(gainBroken < 0.1 * gainGood,
							L"negative control: mis-folded contiguous phase classes collapse polyphase gain by > 90%");
					}
				}
			}
		}

		// 11. SID-OPT Partition B: Classical SourceResolution::Analyse() mirror coherence & Wiener-Khinchin periodicity
		//     match independent direct std::abs(a * b) and direct std::cos(2*pi*k*lag/n) calculations to < 1e-11,
		//     and FrameQuality::MeasureNoise() matches an independent naive 4-deep 7x7 box sum to < 1e-6 (float parity),
		//     with corrupted |A*B| and mis-indexed cos-LUT negative controls (Rule 3).
		TEST_METHOD(ClassicalMirrorCoherenceAndFrameQualityNoiseMatchDirectReference)
		{
			using namespace S3Fx;

			// Independent radix-2 FFT + Hann window + direct mirror coherence & Wiener-Khinchin periodicity reference.
			auto fftRef = [](std::vector<std::complex<double>>& a) {
				const size_t n = a.size();
				for (size_t i = 1, j = 0; i < n; ++i)
				{
					size_t bit = n >> 1;
					for (; j & bit; bit >>= 1) j ^= bit;
					j ^= bit;
					if (i < j) std::swap(a[i], a[j]);
				}
				for (size_t len = 2; len <= n; len <<= 1)
				{
					const double ang = -2.0 * C::kPi / static_cast<double>(len);
					const std::complex<double> wl(std::cos(ang), std::sin(ang));
					for (size_t i = 0; i < n; i += len)
					{
						std::complex<double> w(1.0, 0.0);
						for (size_t k = 0; k < len / 2; ++k)
						{
							const std::complex<double> u = a[i + k];
							const std::complex<double> v = a[i + k + len / 2] * w;
							a[i + k] = u + v;
							a[i + k + len / 2] = u - v;
							w *= wl;
						}
					}
				}
			};

			auto blockHarm = [](int bin, int n) {
				const int period = n / 16;
				if (period < 2) return false;
				const int r = bin % period;
				return r == 0 || r == 1 || r == period - 1;
			};

			auto directAnalyseRef = [&](const float* luma, int n,
				bool corruptMirrorDenom, int corruptCosMode,
				std::vector<double>& outMirrorV, std::vector<double>& outMirrorH, double& outPeriodicPeak,
				double* outAutoHLag8 = nullptr) {
				std::vector<double> win(static_cast<size_t>(n));
				double winE = 0.0;
				for (int i = 0; i < n; ++i)
				{
					win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * C::kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n));
					winE += win[static_cast<size_t>(i)] * win[static_cast<size_t>(i)];
				}
				const int bins = n / 2 + 1;
				std::vector<double> powAxis[2];
				powAxis[0].assign(static_cast<size_t>(bins), 0.0);
				powAxis[1].assign(static_cast<size_t>(bins), 0.0);
				std::vector<std::complex<double>> line(static_cast<size_t>(n));
				std::vector<std::complex<double>> lines(static_cast<size_t>(n) * static_cast<size_t>(bins));

				for (int pass = 0; pass < 2; ++pass)
				{
					std::vector<double>& mirror = (pass == 0) ? outMirrorV : outMirrorH;
					for (int l = 0; l < n; ++l)
					{
						double mean = 0.0;
						for (int i = 0; i < n; ++i)
							mean += (pass == 0) ? luma[static_cast<size_t>(i) * n + l] : luma[static_cast<size_t>(l) * n + i];
						mean /= static_cast<double>(n);
						for (int i = 0; i < n; ++i)
						{
							const double v = (pass == 0) ? luma[static_cast<size_t>(i) * n + l] : luma[static_cast<size_t>(l) * n + i];
							line[static_cast<size_t>(i)] = std::complex<double>((v - mean) * win[static_cast<size_t>(i)], 0.0);
						}
						fftRef(line);
						for (int k = 0; k <= n / 2; ++k)
						{
							powAxis[pass][static_cast<size_t>(k)] += std::norm(line[static_cast<size_t>(k)]) * ((k == 0 || k == n / 2) ? 1.0 : 2.0);
							lines[static_cast<size_t>(l) * bins + k] = line[static_cast<size_t>(k)];
						}
					}
					for (size_t k = 0; k < powAxis[pass].size(); ++k)
						powAxis[pass][k] /= static_cast<double>(n) * winE * 2.0;

					mirror.assign(static_cast<size_t>(n), 0.0);
					for (int m = n / 8; m < n; ++m)
					{
						const int half = m / 2;
						const int band = (std::max)(4, static_cast<int>(std::lround(0.3 * half)));
						const int kFrom = (std::max)({ 2, half - band, m - n / 2 });
						const int kTo = (m - 5) / 2;
						if (kTo - kFrom < 2) continue;
						int kept = 0;
						for (int k = kFrom; k <= kTo; ++k)
							if (!blockHarm(k, n) && !blockHarm(m - k, n)) ++kept;
						const bool exactOnly = kept < 3 && m < 7 * n / 8 - 2;
						const int period = n / 16;
						auto excl = [&](int b) { return exactOnly ? (period >= 2 && b % period == 0) : blockHarm(b, n); };
						std::complex<double> sum(0.0, 0.0);
						double mag = 0.0;
						for (int k = kFrom; k <= kTo; ++k)
						{
							if (excl(k) || excl(m - k)) continue;
							for (int l = 0; l < n; ++l)
							{
								const std::complex<double> a = lines[static_cast<size_t>(l) * bins + k];
								const std::complex<double> b = lines[static_cast<size_t>(l) * bins + (m - k)];
								const std::complex<double> prod = a * b;
								sum += prod;
								mag += corruptMirrorDenom ? std::abs(a) : std::abs(prod);
							}
						}
						mirror[static_cast<size_t>(m)] = mag > 0.0 ? std::abs(sum) / mag : 0.0;
					}
				}

				const int maxLag = n / 4;
				std::vector<double> winAuto(static_cast<size_t>(maxLag) + 2, 0.0);
				for (int lag = 0; lag <= maxLag + 1; ++lag)
				{
					double s = 0.0;
					for (int i = 0; i + lag < n; ++i)
						s += win[static_cast<size_t>(i)] * win[static_cast<size_t>(i + lag)];
					winAuto[static_cast<size_t>(lag)] = s / winE;
				}
				auto repeatRef = [&](const std::vector<double>& p, double* outLag8) {
					std::vector<double> r(static_cast<size_t>(maxLag) + 2, 0.0);
					for (int lag = 0; lag <= maxLag + 1; ++lag)
					{
						double s = 0.0;
						for (int k = 1; k <= n / 2; ++k)
						{
							int idx = k * lag;
							if (corruptCosMode == 1)
								idx = (k * lag) % (n / 2);      // half-period mod bug: flips negative half-cycle cosine to +1 at lag = 8
							else if (corruptCosMode == 2)
								idx = (k + lag) % n;            // mis-indexed cos LUT (addition instead of product): destroys periodic peak
							s += p[static_cast<size_t>(k)] * std::cos(2.0 * C::kPi * static_cast<double>(idx) / static_cast<double>(n));
						}
						r[static_cast<size_t>(lag)] = s;
					}
					if (r[0] <= 0.0) return 0.0;
					std::vector<double> v(r.size(), 0.0);
					for (size_t lag = 0; lag < r.size(); ++lag)
						v[lag] = winAuto[lag] > 0.05 ? r[lag] / r[0] / winAuto[lag] : 0.0;
					if (outLag8 != nullptr && v.size() > 8)
						*outLag8 = v[8];
					double lowest = v[1], peak = 0.0;
					for (int lag = 2; lag <= maxLag; ++lag)
					{
						const double here = v[static_cast<size_t>(lag)];
						if (lag >= 3 && here >= v[static_cast<size_t>(lag - 1)] && here >= v[static_cast<size_t>(lag + 1)] && here - lowest >= 0.2)
							peak = (std::max)(peak, here);
						lowest = (std::min)(lowest, here);
					}
					return peak;
				};
				outPeriodicPeak = (std::max)(repeatRef(powAxis[0], nullptr), repeatRef(powAxis[1], outAutoHLag8));
			};

			// Independent naive 4-deep 7x7 box-sum reference for FrameQuality::MeasureNoise().
			auto naiveMeasureNoiseRef = [](const float* luma, int T) -> SourceIdentity::FrameQuality::TileNoise {
				SourceIdentity::FrameQuality::TileNoise out;
				const size_t n = static_cast<size_t>(T) * static_cast<size_t>(T);
				std::vector<float> u(n), g1, g2;
				for (size_t i = 0; i < n; ++i)
					u[i] = luma[i] * 255.0f;
				const std::vector<float> t1 = SourceIdentity::FrameQuality::detail::GaussTaps(1.0);
				const std::vector<float> t2 = SourceIdentity::FrameQuality::detail::GaussTaps(2.0);
				SourceIdentity::FrameQuality::detail::Blur(u, g1, T, T, t1);
				SourceIdentity::FrameQuality::detail::Blur(u, g2, T, T, t2);
				std::vector<float> hp(n), e(n);
				for (size_t i = 0; i < n; ++i)
					hp[i] = u[i] - g1[i];
				for (int y = 0; y < T; ++y)
					for (int x = 0; x < T; ++x)
					{
						double s = 0.0;
						for (int dy = -3; dy <= 3; ++dy)
							for (int dx = -3; dx <= 3; ++dx)
							{
								const int ry = SourceIdentity::FrameQuality::detail::Reflect(y + dy, T);
								const int rx = SourceIdentity::FrameQuality::detail::Reflect(x + dx, T);
								const float v = hp[static_cast<size_t>(ry) * T + rx];
								s += static_cast<double>(v) * static_cast<double>(v);
							}
						e[static_cast<size_t>(y) * T + x] = static_cast<float>(s / 49.0);
					}
				std::vector<double> okE;
				std::vector<char> ok(n, 0);
				for (int y = 0; y < T; ++y)
					for (int x = 0; x < T; ++x)
					{
						const int xm = SourceIdentity::FrameQuality::detail::Reflect(x - 1, T);
						const int xp = SourceIdentity::FrameQuality::detail::Reflect(x + 1, T);
						const int ym = SourceIdentity::FrameQuality::detail::Reflect(y - 1, T);
						const int yp = SourceIdentity::FrameQuality::detail::Reflect(y + 1, T);
						const double gx = 0.5 * (g2[static_cast<size_t>(y) * T + xp] - g2[static_cast<size_t>(y) * T + xm]);
						const double gy = 0.5 * (g2[static_cast<size_t>(yp) * T + x] - g2[static_cast<size_t>(ym) * T + x]);
						if (std::sqrt(gx * gx + gy * gy) < 3.0)
						{
							ok[static_cast<size_t>(y) * T + x] = 1;
							okE.push_back(e[static_cast<size_t>(y) * T + x]);
						}
					}
				if (okE.size() < 64) return out;
				const double thr = RobustStats::Quantile(okE, 0.3, RobustStats::RankRule::FloorIndex);
				std::vector<double> residual;
				for (size_t i = 0; i < n; ++i)
					if (ok[i] && e[i] <= thr)
						residual.push_back(hp[i]);
				if (residual.size() < 64) return out;
				const double med = RobustStats::Median(residual, RobustStats::MedianRule::Upper);
				out.sigma = RobustStats::MadSigma(RobustStats::MadAbout(residual, med, RobustStats::MedianRule::Upper));
				out.coverage = static_cast<double>(residual.size()) / static_cast<double>(n);
				out.valid = true;
				return out;
			};

			// Build 3 synthetic 256x256 tiles: (1) 2:1 upscaled, (2) noisy native, (3) periodic grating.
			const TileScene upScene(0xB101ull, 0.0, 140, 0.012);
			const std::vector<double> upCont = RenderContinuousTile(upScene, 0.5, 900, 600);
			std::vector<float> upTile(static_cast<size_t>(kTile) * kTile);
			for (size_t i = 0; i < upTile.size(); ++i)
			{
				const double v = (std::max)(0.0, (std::min)(1.0, upCont[i]));
				upTile[i] = static_cast<float>(std::floor(v * 1023.0 + 0.5) / 1023.0);
			}

			const TileScene natScene(0xB202ull, 1.8, 80, 0.004);
			const std::vector<double> natCont = RenderContinuousTile(natScene, 1.0, 500, 500);
			SplitMix64 rngNat(0xB203ull);
			std::vector<float> natTile(static_cast<size_t>(kTile) * kTile);
			for (size_t i = 0; i < natTile.size(); ++i)
			{
				const double v = (std::max)(0.0, (std::min)(1.0, natCont[i] + (2.0 / 255.0) * rngNat.Normal01()));
				natTile[i] = static_cast<float>(std::floor(v * 1023.0 + 0.5) / 1023.0);
			}

			SplitMix64 rngPer(0xB304ull);
			std::vector<float> perTile(static_cast<size_t>(kTile) * kTile);
			for (int y = 0; y < kTile; ++y)
				for (int x = 0; x < kTile; ++x)
				{
					const double v = 0.5 + 0.20 * std::cos(2.0 * C::kPi * x / 16.0)
						+ 0.15 * std::cos(2.0 * C::kPi * y / 12.0) + 0.015 * rngPer.Normal01();
					const double cl = (std::max)(0.0, (std::min)(1.0, v));
					perTile[static_cast<size_t>(y) * kTile + x] = static_cast<float>(std::floor(cl * 1023.0 + 0.5) / 1023.0);
				}

			// (a) Verify SourceResolution::Analyse mirror coherence & periodicity match direct reference to < 1e-11:
			const std::vector<float>* allTiles[3] = { &upTile, &natTile, &perTile };
			for (int ti = 0; ti < 3; ++ti)
			{
				SR::Spectrum spec;
				Assert::IsTrue(SR::Analyse(allTiles[ti]->data(), kTile, kTile, spec, 1023));
				std::vector<double> refMV, refMH;
				double refPer = 0.0;
				directAnalyseRef(allTiles[ti]->data(), kTile, false, 0, refMV, refMH, refPer);

				Assert::AreEqual(refMV.size(), spec.mirrorVertical.size());
				Assert::AreEqual(refMH.size(), spec.mirrorHorizontal.size());
				double maxDiffV = 0.0, maxDiffH = 0.0;
				for (size_t m = 0; m < refMV.size(); ++m)
				{
					maxDiffV = (std::max)(maxDiffV, std::fabs(spec.mirrorVertical[m] - refMV[m]));
					maxDiffH = (std::max)(maxDiffH, std::fabs(spec.mirrorHorizontal[m] - refMH[m]));
				}
				Assert::IsTrue(maxDiffV < 1e-11, L"mirrorVertical matches direct std::abs(a*b) reference to < 1e-11");
				Assert::IsTrue(maxDiffH < 1e-11, L"mirrorHorizontal matches direct std::abs(a*b) reference to < 1e-11");
				Assert::IsTrue(std::fabs(spec.periodicPeak - refPer) < 1e-11,
					L"periodicPeak matches direct std::cos(2*pi*k*lag/n) reference to < 1e-11");

				if (ti == 0)
					Assert::IsTrue(spec.mirrorVertical[128] > 0.5, L"fixture: 2:1 upscaled tile has strong coherence peak at m=128");
				if (ti == 2)
					Assert::IsTrue(spec.periodicPeak > 0.5, L"fixture: periodic grating has strong non-zero periodicPeak");
			}

			// (b) Verify FrameQuality::MeasureNoise matches naive 4-deep 7x7 box-sum reference to < 1e-6 (float parity):
			{
				const auto gotNat = SourceIdentity::FrameQuality::MeasureNoise(natTile.data(), kTile);
				const auto refNat = naiveMeasureNoiseRef(natTile.data(), kTile);
				Assert::IsTrue(gotNat.valid && refNat.valid, L"fixture: native noisy tile yields valid MeasureNoise");
				Assert::IsTrue(gotNat.sigma > 1.0 && gotNat.sigma < 3.5, L"fixture: realistic 8-bit noise sigma");
				Assert::IsTrue(std::fabs(gotNat.sigma - refNat.sigma) < 1e-6,
					L"FrameQuality::MeasureNoise sigma matches naive 4-deep 7x7 box-sum reference to < 1e-6");
				Assert::IsTrue(std::fabs(gotNat.coverage - refNat.coverage) < 1e-6,
					L"FrameQuality::MeasureNoise coverage matches naive 4-deep 7x7 box-sum reference to < 1e-6");

				const auto gotUp = SourceIdentity::FrameQuality::MeasureNoise(upTile.data(), kTile);
				const auto refUp = naiveMeasureNoiseRef(upTile.data(), kTile);
				Assert::AreEqual(refUp.valid, gotUp.valid);
				if (gotUp.valid)
				{
					Assert::IsTrue(std::fabs(gotUp.sigma - refUp.sigma) < 1e-6);
					Assert::IsTrue(std::fabs(gotUp.coverage - refUp.coverage) < 1e-6);
				}
			}

			// (c) Mandatory in-test negative controls (Rule 3):
			//     (1) Omitting |B| in mirror-coherence denominator fails the match on upscaled tile.
			//     (2) Half-mod cos LUT (% (n/2)) flips half-period autocorrelation at lag=8 from ~-1 to ~+1 (> 1.5),
			//         and mis-indexed cos LUT ((k + lag) % n) collapses periodicPeak (> 0.3) on periodic tile.
			{
				SR::Spectrum upSpec, perSpec;
				Assert::IsTrue(SR::Analyse(upTile.data(), kTile, kTile, upSpec, 1023));
				Assert::IsTrue(SR::Analyse(perTile.data(), kTile, kTile, perSpec, 1023));

				std::vector<double> badMV, badMH;
				double dummyPer = 0.0;
				directAnalyseRef(upTile.data(), kTile, true, 0, badMV, badMH, dummyPer);
				Assert::IsTrue(std::fabs(badMV[128] - upSpec.mirrorVertical[128]) > 0.1,
					L"negative control: omitting |B| in mirror-coherence denominator visibly breaks coherence");

				std::vector<double> dummyMV, dummyMH;
				double refPerCheck = 0.0, halfModPer = 0.0, badStridePer = 0.0;
				double trueLag8 = 0.0, halfModLag8 = 0.0;
				directAnalyseRef(perTile.data(), kTile, false, 0, dummyMV, dummyMH, refPerCheck, &trueLag8);
				directAnalyseRef(perTile.data(), kTile, false, 1, dummyMV, dummyMH, halfModPer, &halfModLag8);
				directAnalyseRef(perTile.data(), kTile, false, 2, dummyMV, dummyMH, badStridePer);
				Assert::IsTrue(trueLag8 < -0.5 && halfModLag8 > 0.5 && std::fabs(halfModLag8 - trueLag8) > 1.5,
					L"negative control: half-period cos LUT mod (n/2) flips lag=8 autocorrelation by > 1.5");
				Assert::IsTrue(std::fabs(badStridePer - perSpec.periodicPeak) > 0.3,
					L"negative control: mis-indexed cos LUT collapses periodicPeak by > 0.3");
			}
		}

		// 12. SID-OPT Partition B: CaptureLines<uint16_t>(scale = 1.0) unity-scale fast path matches general
		//     double round+clamp formula 100% bit-identically (while non-1.0 scales still scale accurately),
		//     AND PowerProbe::Run(..., &ws) across cold miss -> warm hit -> geometry change -> ratio/kernel
		//     change matches uncached PowerProbe::Run(..., nullptr) bit-identically, with negative controls
		//     proving stale cached tap geometry would mismatch and overflow on changed (origin, length) / axisLen.
		TEST_METHOD(CaptureLinesScaleOneAndPowerProbeWorkspaceMatchDirectBitExact)
		{
			using namespace S3Fx;

			// ---- Part A: CaptureLines<uint16_t> unity-scale vs general double round+clamp reference ----
			{
				const int W = 640;
				const int H = 360;
				const int stride = 704; // padded stride > W to catch stride indexing bugs
				SplitMix64 rng(0xC012A001ull);

				std::vector<uint16_t> plane16(static_cast<size_t>(H) * static_cast<size_t>(stride), 0xBEEFu);
				const uint16_t edgeCodes[10] = { 0u, 1u, 64u, 255u, 512u, 940u, 1023u, 4095u, 65534u, 65535u };
				for (int y = 0; y < H; ++y)
				{
					for (int x = 0; x < W; ++x)
					{
						const int idx = y * stride + x;
						if (((x + y) & 31) < 10)
							plane16[static_cast<size_t>(idx)] = edgeCodes[(x + y) & 31];
						else
							plane16[static_cast<size_t>(idx)] = static_cast<uint16_t>(rng.NextU64() & 0xFFFFu);
					}
				}

				C::LineRect active;
				active.x = 16;
				active.y = 12;
				active.w = W - 32;
				active.h = H - 24;
				const C::LinePlan plan = C::LineSampler::Plan(active, 42u, C::SamplerSettings());
				Assert::IsTrue(plan.valid, L"fixture: valid offset LinePlan for CaptureLines");

				auto directCaptureRef = [&](const uint16_t* pl, double scale,
					std::vector<uint16_t>& refRows, std::vector<uint16_t>& refCols) {
					auto roundClamp = [scale](uint16_t v) -> uint16_t {
						const double c = std::floor(static_cast<double>(v) * scale + 0.5);
						return static_cast<uint16_t>(c < 0.0 ? 0.0 : (c > 65535.0 ? 65535.0 : c));
					};
					refRows.resize(static_cast<size_t>(plan.rowCount) * static_cast<size_t>(plan.inner.w));
					refCols.resize(static_cast<size_t>(plan.colCount) * static_cast<size_t>(plan.inner.h));
					for (int j = 0; j < plan.rowCount; ++j)
					{
						const uint16_t* src = pl + static_cast<std::ptrdiff_t>(plan.rowY[j]) * stride + plan.inner.x;
						uint16_t* dst = &refRows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
						for (int i = 0; i < plan.inner.w; ++i)
							dst[i] = roundClamp(src[i]);
					}
					for (int j = 0; j < plan.colCount; ++j)
					{
						const uint16_t* src = pl + static_cast<std::ptrdiff_t>(plan.inner.y) * stride + plan.colX[j];
						uint16_t* dst = &refCols[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.h)];
						for (int i = 0; i < plan.inner.h; ++i)
							dst[i] = roundClamp(src[static_cast<std::ptrdiff_t>(i) * stride]);
					}
				};

				// (1) Unity scale (scale = 1.0): 100% bit-identical to general double round+clamp formula.
				C::LineSnapshot snapUnity;
				Assert::IsTrue(C::CaptureLines<uint16_t>(plan, plane16.data(), stride, 1.0, snapUnity));
				std::vector<uint16_t> refRows1, refCols1;
				directCaptureRef(plane16.data(), 1.0, refRows1, refCols1);
				Assert::AreEqual(plan.rowCount, snapUnity.rowCount);
				Assert::AreEqual(plan.colCount, snapUnity.colCount);
				Assert::AreEqual(refRows1.size(), snapUnity.rows.size());
				Assert::AreEqual(refCols1.size(), snapUnity.cols.size());
				Assert::IsTrue(snapUnity.rows == refRows1, L"CaptureLines<uint16_t>(scale=1.0) rows 100% bit-identical to general formula");
				Assert::IsTrue(snapUnity.cols == refCols1, L"CaptureLines<uint16_t>(scale=1.0) cols 100% bit-identical to general formula");

				// (2) Non-unity scale (scale = 4.0) on uint16_t (with clamp saturation) AND uint8_t planes:
				std::vector<uint16_t> plane8in16(static_cast<size_t>(H) * static_cast<size_t>(stride), 0xBEEFu);
				std::vector<uint8_t> plane8(static_cast<size_t>(H) * static_cast<size_t>(stride), 0xEEu);
				for (int y = 0; y < H; ++y)
				{
					for (int x = 0; x < W; ++x)
					{
						const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x);
						const uint8_t v8 = static_cast<uint8_t>(16u + (rng.NextU64() % 220u));
						plane8[idx] = v8;
						plane8in16[idx] = ((x + y) % 97 == 0) ? 20000u : static_cast<uint16_t>(v8);
					}
				}

				C::LineSnapshot snapScale4;
				Assert::IsTrue(C::CaptureLines<uint16_t>(plan, plane8in16.data(), stride, 4.0, snapScale4));
				std::vector<uint16_t> refRows4, refCols4;
				directCaptureRef(plane8in16.data(), 4.0, refRows4, refCols4);
				Assert::IsTrue(snapScale4.rows == refRows4, L"CaptureLines<uint16_t>(scale=4.0) rows scale & clamp accurately");
				Assert::IsTrue(snapScale4.cols == refCols4, L"CaptureLines<uint16_t>(scale=4.0) cols scale & clamp accurately");

				// Negative control (Rule 3): unity-copying a scale=4.0 plane deviates drastically from true scaled output.
				C::LineSnapshot snapUnscaled;
				Assert::IsTrue(C::CaptureLines<uint16_t>(plan, plane8in16.data(), stride, 1.0, snapUnscaled));
				size_t scaledDiffCount = 0;
				for (size_t i = 0; i < refRows4.size(); ++i)
					if (snapUnscaled.rows[i] != refRows4[i])
						++scaledDiffCount;
				Assert::AreEqual(refRows4.size(), scaledDiffCount,
					L"negative control: bypassing scale=4.0 with unity copy corrupts every non-zero pixel");

				// Also verify non-uint16_t template instantiation (uint8_t, std::false_type dispatch):
				C::LineSnapshot snapU8;
				Assert::IsTrue(C::CaptureLines<uint8_t>(plan, plane8.data(), stride, 4.0, snapU8));
				for (int j = 0; j < plan.rowCount; ++j)
				{
					const uint8_t* src = plane8.data() + static_cast<std::ptrdiff_t>(plan.rowY[j]) * stride + plan.inner.x;
					const uint16_t* got = &snapU8.rows[static_cast<size_t>(j) * static_cast<size_t>(plan.inner.w)];
					for (int i = 0; i < plan.inner.w; ++i)
						Assert::AreEqual(static_cast<uint16_t>(static_cast<unsigned>(src[i]) * 4u), got[i]);
				}
			}

			// ---- Part B: PowerProbe::Run(..., &ws) cold miss -> warm hit -> geometry change -> ratio/kernel change ----
			auto makeNativeLineSet = [](int axisLen, int origin, int length, int count, uint64_t seed, double meanOffset) -> C::LineSet {
				SplitMix64 rng(seed);
				C::LineSet ls;
				ls.axis = 0;
				ls.axisLen = axisLen;
				ls.origin = origin;
				ls.Resize(count, length);
				std::vector<double> full;
				for (int l = 0; l < count; ++l)
				{
					ls.cross[static_cast<size_t>(l)] = 64 + l * 40;
					SynthLine1D(axisLen, axisLen, kBilinear, 1, rng, full, 0.0, false, 0.0);
					double* dst = ls.Line(l);
					for (int i = 0; i < length; ++i)
						dst[i] = full[static_cast<size_t>(origin + i)] + meanOffset;
				}
				return ls;
			};

			const C::LineSet ls1 = makeNativeLineSet(1920, 48, 1824, 16, 0xB101ull, 0.0);
			const C::LineSet ls2 = makeNativeLineSet(1920, 48, 1824, 16, 0xB202ull, 25.0);  // identical geometry, different pixels
			const C::LineSet ls3 = makeNativeLineSet(1920, 128, 1664, 16, 0xB303ull, -15.0); // changed (origin, length)
			const C::LineSet ls4 = makeNativeLineSet(1280, 32, 1216, 16, 0xB404ull, 10.0);   // changed axisLen

			const C::ProbeSettings psDefault;
			C::ProbeSettings psAlt = psDefault;
			psAlt.ratio = 0.53;
			psAlt.kernel = kBilinear;

			const double floorCodes = 0.289;
			const double roundStepCodes = 1.0;

			C::VerifierSettings vs;
			C::DescaleVerifier vCached(vs);
			C::DescaleVerifier vUncached(vs);
			C::PowerProbe::Workspace ws;

			auto checkBitIdentical = [&](const C::LineSet& ls, const C::ProbeSettings& ps,
				C::ProbeResult& outCached, C::ProbeResult& outUncached) {
				const bool okC = C::PowerProbe::Run(vCached, ls, floorCodes, roundStepCodes, ps, outCached, &ws);
				const bool okU = C::PowerProbe::Run(vUncached, ls, floorCodes, roundStepCodes, ps, outUncached, nullptr);
				Assert::IsTrue(okC && okU, L"PowerProbe::Run succeeds on rich native lines");
				Assert::IsTrue(outCached.ran && outUncached.ran);
				Assert::IsTrue(outCached.seen && outUncached.seen, L"fixture: rich native lines produce seen == true");
				Assert::IsTrue(outCached.stat <= ps.seenThreshold);
				Assert::IsTrue(outCached.stat == outUncached.stat, L"PowerProbe stat must be bit-identical with workspace");
				Assert::AreEqual(outUncached.gatedLines, outCached.gatedLines);
				Assert::IsTrue(outCached.gatedLines >= 12, L"fixture: abundant gated lines");
				Assert::AreEqual(outUncached.reason, outCached.reason);
				Assert::IsTrue(outCached.meanCode == outUncached.meanCode, L"PowerProbe meanCode bit-identical");
				Assert::IsTrue(outCached.medianLineStd == outUncached.medianLineStd, L"PowerProbe medianLineStd bit-identical");
				Assert::AreEqual(outUncached.syntheticAxisLen, outCached.syntheticAxisLen);
				Assert::AreEqual(outUncached.syntheticLength, outCached.syntheticLength);
				Assert::IsTrue(ws.geomValid);
				Assert::AreEqual(ls.axisLen, ws.keyAxisLen);
				Assert::AreEqual(ls.origin, ws.keyOrigin);
				Assert::AreEqual(ls.length, ws.keyLength);
				Assert::IsTrue(ws.keyRatio == ps.ratio);
				Assert::AreEqual(ps.kernel, ws.keyKernel);
				Assert::AreEqual(static_cast<size_t>(outCached.syntheticLength), ws.c0.size());
			};

			// (1) Cold miss (ls1):
			C::ProbeResult rc1, ru1;
			checkBitIdentical(ls1, psDefault, rc1, ru1);
			// Snapshot ws1 tap geometry before subsequent calls for the stale-geometry negative control:
			const int staleNn1 = ws.Nn;
			const int staleJlo1 = ws.jlo;
			const int staleJhi1 = ws.jhi;
			const int staleTaps1 = ws.taps;
			const std::vector<double> staleW1 = ws.w;
			const std::vector<int> staleC0_1 = ws.c0;

			// (2) Warm hit (ls2 — same geometry, different pixel content):
			C::ProbeResult rc2, ru2;
			checkBitIdentical(ls2, psDefault, rc2, ru2);
			Assert::IsTrue(rc1.meanCode != rc2.meanCode, L"fixture: ls2 genuinely has different pixel content");
			Assert::AreEqual(staleNn1, ws.Nn);
			Assert::AreEqual(staleJlo1, ws.jlo);
			Assert::AreEqual(staleJhi1, ws.jhi);
			Assert::IsTrue(ws.c0 == staleC0_1, L"warm hit reuses identical cached tap offsets");

			// (3) Geometry change in (origin, length) (ls3):
			C::ProbeResult rc3, ru3;
			checkBitIdentical(ls3, psDefault, rc3, ru3);
			Assert::AreNotEqual(rc1.syntheticLength, rc3.syntheticLength, L"changing (origin, length) changes syntheticLength");
			Assert::AreNotEqual(staleJlo1, ws.jlo, L"changing origin forces workspace jlo rebuild");
			const int rebuiltJlo3 = ws.jlo;
			const std::vector<int> rebuiltC0_3 = ws.c0;
			const std::vector<double> rebuiltW3 = ws.w;

			// (4) Geometry change in axisLen (ls4):
			C::ProbeResult rc4, ru4;
			checkBitIdentical(ls4, psDefault, rc4, ru4);
			Assert::AreNotEqual(rc3.syntheticAxisLen, rc4.syntheticAxisLen, L"changing axisLen changes syntheticAxisLen");
			Assert::AreNotEqual(staleNn1, ws.Nn, L"changing axisLen forces workspace Nn rebuild");
			const std::vector<int> rebuiltC0_4 = ws.c0;
			const std::vector<double> rebuiltW4 = ws.w;

			// (5) Ratio & kernel change on same LineSet (ls4 + psAlt):
			C::ProbeResult rc5, ru5;
			checkBitIdentical(ls4, psAlt, rc5, ru5);
			Assert::AreNotEqual(rc4.syntheticAxisLen, rc5.syntheticAxisLen, L"changing ratio forces workspace rebuild");
			Assert::AreNotEqual(staleTaps1, ws.taps, L"changing kernel from bicubic(4 taps) to bilinear(2 taps) rebuilds taps");

			// (6) Mandatory in-test negative control (Rule 3): prove stale cached tap geometry from ls1
			//     genuinely overflows AND drifts if NOT rebuilt on ls3 (changed origin/length) and ls4 (changed axisLen).
			{
				const int staleL2 = staleJhi1 - staleJlo1 + 1;
				int maxStaleIdx = -1;
				for (size_t j = 0; j < staleC0_1.size(); ++j)
					if (staleC0_1[j] + staleTaps1 - 1 > maxStaleIdx)
						maxStaleIdx = staleC0_1[j] + staleTaps1 - 1;

				// Stale ls1 c0 table reaches beyond ls3.length (1664) AND ls4.length (1216) — would read out of bounds!
				Assert::IsTrue(maxStaleIdx >= ls3.length,
					L"negative control: stale ls1 tap indices overflow ls3 buffer if workspace is not rebuilt on (origin, length) change");
				Assert::IsTrue(maxStaleIdx >= ls4.length,
					L"negative control: stale ls1 tap indices overflow ls4 buffer if workspace is not rebuilt on axisLen change");

				// On ls3 (origin shift 48 -> 128 at same axisLen=1920), c0 is origin-relative so stale ls1 weights
				// introduce a real sub-pixel phase shift (~0.1016 px) that visibly drifts sharp edges (> 1.0 code):
				Assert::IsTrue(staleL2 > rc3.syntheticLength);
				Assert::AreNotEqual(staleJlo1, rebuiltJlo3);
				double maxSampleDrift3 = 0.0;
				const double* y3 = ls3.Line(0);
				for (int j = 0; j < rc3.syntheticLength; ++j)
				{
					if (staleC0_1[static_cast<size_t>(j)] < 0 || staleC0_1[static_cast<size_t>(j)] + staleTaps1 > ls3.length)
						continue;
					double staleAcc = 0.0;
					double trueAcc = 0.0;
					for (int s = 0; s < staleTaps1; ++s)
					{
						staleAcc += staleW1[static_cast<size_t>(j) * static_cast<size_t>(staleTaps1) + static_cast<size_t>(s)]
							* y3[staleC0_1[static_cast<size_t>(j)] + s];
						trueAcc += rebuiltW3[static_cast<size_t>(j) * static_cast<size_t>(staleTaps1) + static_cast<size_t>(s)]
							* y3[rebuiltC0_3[static_cast<size_t>(j)] + s];
					}
					maxSampleDrift3 = (std::max)(maxSampleDrift3, std::fabs(staleAcc - trueAcc));
				}
				Assert::IsTrue(maxSampleDrift3 > 1.0,
					L"negative control: reusing stale ls1 tap weights on origin-shifted ls3 drifts samples by > 1.0 code");

				// On ls4 (axisLen change 1920 -> 1280, different Nn & tap step), stale ls1 tap weights accumulate
				// ~0.5+ px coordinate drift across the safe prefix and corrupt synthetic samples by > 50 codes:
				double maxSampleDrift4 = 0.0;
				const double* y4 = ls4.Line(0);
				for (int j = 0; j < rc4.syntheticLength; ++j)
				{
					if (staleC0_1[static_cast<size_t>(j)] < 0 || staleC0_1[static_cast<size_t>(j)] + staleTaps1 > ls4.length)
						continue;
					double staleAcc = 0.0;
					double trueAcc = 0.0;
					for (int s = 0; s < staleTaps1; ++s)
					{
						staleAcc += staleW1[static_cast<size_t>(j) * static_cast<size_t>(staleTaps1) + static_cast<size_t>(s)]
							* y4[staleC0_1[static_cast<size_t>(j)] + s];
						trueAcc += rebuiltW4[static_cast<size_t>(j) * static_cast<size_t>(staleTaps1) + static_cast<size_t>(s)]
							* y4[rebuiltC0_4[static_cast<size_t>(j)] + s];
					}
					maxSampleDrift4 = (std::max)(maxSampleDrift4, std::fabs(staleAcc - trueAcc));
				}
				Assert::IsTrue(maxSampleDrift4 > 50.0,
					L"negative control: reusing stale ls1 tap weights on axisLen-changed ls4 corrupts synthetic samples by > 50 codes");
			}
		}

		// 13. SID-OPT Partition B: P3 two-pass exact stage (Pass 1 screen at halfWidth = 0 + rep kernels,
		//     Pass 2 full halfWidth + full kernel families on top / protected / dipping candidates) preserves
		//     identical dipping-candidate recall (n, stat, non-rep kernel, kernelMask) vs single-pass exact
		//     across 4K 1920x1080 and 1280x720 streams, saves solves on non-dipping candidates, and works
		//     end-to-end inside ChainEstimator with conventionDrivenProposals + safetyRules (proposer skipped) —
		//     with a Rule 3 negative control proving skipping Pass 2 on an off-by-1 candidate degrades residual
		//     while two-pass expansion recovers the exact n0 minimum.
		TEST_METHOD(P3TwoPassExactStagePreservesDippingCandidateRecall)
		{
			using namespace S3Fx;

			const int kLanczos4 = 6; // non-representative Lanczos family kernel (reps are 0, 1, 5, 8)

			// ---- Part A: Direct DescaleVerifier P3 two-pass vs single-pass exact equivalence ----
			struct DirectCase { int srcW; int srcH; int kernel; uint64_t seed; };
			const DirectCase directCases[2] = {
				{ 1920, 1080, kLanczos4, 0xD101ull }, // proves Pass 2 full-family expansion recovers non-rep kernel 6
				{ 1280, 720,  kBilinear, 0xD202ull }
			};

			for (int ci = 0; ci < 2; ++ci)
			{
				const DirectCase& dc = directCases[ci];
				const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, dc.srcW, dc.srcH, dc.kernel, 1,
					100u + static_cast<uint64_t>(ci), 1000000, dc.seed, true);

				for (int a = 0; a < 2; ++a)
				{
					C::LineSet ls;
					Assert::IsTrue(C::ToLineSet(snap, a, ls));
					const int trueN = (a == 0) ? dc.srcW : dc.srcH;

					int cands[C::kMaxCandidates];
					const int pc = C::BuildConventionCandidates(a, ls.axisLen, kUhdW, kUhdH,
						0, 0, nullptr, 0, true, cands, C::kMaxCandidates);
					Assert::IsTrue(pc >= 5, L"fixture: 4K conventional candidate set has >= 5 candidates");

					// Verify mathematically that skipping PolyphaseProposer under convProposalsOn && rulesActive
					// produces the EXACT same BuildConventionCandidates list:
					C::PolyphaseProposer prop;
					Assert::IsTrue(prop.AddEvaluation(ls));
					C::ProposedCandidate extraProp[C::kMaxCandidates];
					const int extraCnt = prop.Propose(ls.axisLen, nullptr, 0, extraProp, C::kMaxCandidates);
					int extraN[C::kMaxCandidates];
					for (int i = 0; i < extraCnt; ++i)
						extraN[i] = extraProp[i].n;
					int candsWithProp[C::kMaxCandidates];
					const int pcWithProp = C::BuildConventionCandidates(a, ls.axisLen, kUhdW, kUhdH,
						0, 0, extraN, extraCnt, true, candsWithProp, C::kMaxCandidates);
					Assert::AreEqual(pc, pcWithProp, L"conventionOnly candidate count identical with or without proposer");
					for (int i = 0; i < pc; ++i)
						Assert::AreEqual(cands[i], candsWithProp[i],
							L"conventionOnly candidate list identical when proposer is skipped");

					C::VerifyOptions optOne;
					optOne.exactOnly = true;
					optOne.p3TwoPass = false;

					C::VerifyOptions optTwo;
					optTwo.exactOnly = true;
					optTwo.p3TwoPass = true;

					// Also test dip-threshold-only promotion (p3ScreenKeepTop = 0, no protected N) to prove
					// cr.stat < p3ScreenDipThreshold independently triggers Pass 2 full expansion:
					C::VerifyOptions optDipOnly;
					optDipOnly.exactOnly = true;
					optDipOnly.p3TwoPass = true;
					optDipOnly.p3ScreenKeepTop = 0;

					C::DescaleVerifier vOne, vTwo, vDipOnly;
					C::VerifyResult vrOne, vrTwo, vrDipOnly;
					Assert::IsTrue(vOne.Evaluate(ls, cands, pc, optOne, vrOne));
					Assert::IsTrue(vTwo.Evaluate(ls, cands, pc, optTwo, vrTwo));
					Assert::IsTrue(vDipOnly.Evaluate(ls, cands, pc, optDipOnly, vrDipOnly));

					auto findCand = [](const C::VerifyResult& vr, int targetN) -> const C::CandidateResult* {
						for (size_t i = 0; i < vr.candidates.size(); ++i)
							if (vr.candidates[i].valid && vr.candidates[i].exactDone && vr.candidates[i].n == targetN)
								return &vr.candidates[i];
						return nullptr;
					};

					const C::CandidateResult* crOne = findCand(vrOne, trueN);
					const C::CandidateResult* crTwo = findCand(vrTwo, trueN);
					const C::CandidateResult* crDip = findCand(vrDipOnly, trueN);
					Assert::IsNotNull(crOne, L"single-pass exact finds true conventional size");
					Assert::IsNotNull(crTwo, L"P3 two-pass exact finds true conventional size");
					Assert::IsNotNull(crDip, L"P3 two-pass dip-threshold path (keepTop=0) recalls true dipping size");

					Assert::IsTrue(crOne->stat < -0.5, L"fixture: genuine strong upscale dip");
					Assert::AreEqual(crOne->n, crTwo->n);
					Assert::IsTrue(std::fabs(crOne->stat - crTwo->stat) < 1e-12,
						L"P3 two-pass dipping candidate stat matches single-pass exact bit-identically");
					Assert::AreEqual(crOne->kernel, crTwo->kernel,
						L"P3 two-pass Pass 2 full-family expansion recovers identical best kernel");
					Assert::AreEqual(crOne->kernelMask, crTwo->kernelMask,
						L"P3 two-pass Pass 2 evaluates identical kernel family mask");
					if (dc.kernel == kLanczos4)
						Assert::AreEqual(kLanczos4, crTwo->kernel,
							L"Pass 2 expands beyond rep kernels to recover non-rep lanczos4");

					Assert::AreEqual(crOne->n, crDip->n);
					Assert::IsTrue(std::fabs(crOne->stat - crDip->stat) < 1e-12,
						L"dip-threshold-only Pass 2 matches single-pass exact bit-identically");
					Assert::AreEqual(crOne->kernel, crDip->kernel);
					Assert::AreEqual(crOne->kernelMask, crDip->kernelMask);

					// Confirm two-pass genuinely saves verifier solves on non-dipping conventional candidates:
					Assert::IsTrue(vrTwo.solves < vrOne.solves,
						L"P3 two-pass executes strictly fewer solves than full single-pass across conventional list");
				}
			}

			// ---- Part B: End-to-end ChainEstimator (conventionDrivenProposals=true, safetyRules=true) vs non-P3 ref ----
			struct StreamCase { int srcW; int srcH; int kernel; uint64_t seedBase; };
			const StreamCase streamCases[2] = {
				{ 1920, 1080, kLanczos3, 0xD301ull },
				{ 1280, 720,  kBilinear, 0xD402ull }
			};

			for (int si = 0; si < 2; ++si)
			{
				const StreamCase& sc = streamCases[si];
				// 2 shots x 4 evals at 1.2 s cadence (3.6 s dwell per shot > 3.0 s minShotDwellSec) so ChainLatch enters cleanly:
				std::vector<C::LineSnapshot> frames;
				frames.reserve(8);
				for (int i = 0; i < 8; ++i)
				{
					frames.push_back(MakeSynthSnapshot(kUhdW, kUhdH, sc.srcW, sc.srcH, sc.kernel, 1,
						static_cast<uint64_t>(10 + i * 10), static_cast<int64_t>(i + 1) * 1200000,
						sc.seedBase + static_cast<uint64_t>(i), (i == 0 || i == 4)));
				}

				auto runStream = [&](bool p3On, std::vector<C::EvaluationDetail>& outDets,
					G::Record& outFinalRec, C::EstimatorCounters& outCnt) {
					ChainFx::ManualClock clk;
					C::EstimatorSettings cfg;
					cfg.safetyRules = true;
					cfg.conventionDrivenProposals = p3On;
					cfg.outputStepOverride = 1;
					C::ChainEstimator est(cfg, clk.Fn());
					est.SetSynchronous(true);
					est.SetEvaluationObserver([&](const C::EvaluationDetail& d) { outDets.push_back(d); });
					est.SetMode(C::EstimatorMode::On);
					for (size_t i = 0; i < frames.size(); ++i)
					{
						clk.Set(static_cast<double>(i + 1) * 1200.0);
						Assert::IsTrue(est.SubmitLines(frames[i]));
					}
					Assert::IsTrue(est.Held().ReadLatest(0, outFinalRec));
					outCnt = est.GetCounters();
				};

				std::vector<C::EvaluationDetail> detP3, detRef;
				G::Record recP3, recRef;
				C::EstimatorCounters cntP3, cntRef;
				runStream(true, detP3, recP3, cntP3);
				runStream(false, detRef, recRef, cntRef);

				Assert::AreEqual<size_t>(8, detP3.size());
				Assert::AreEqual<size_t>(8, detRef.size());
				Assert::IsTrue(cntP3.p3ConventionProposals >= 16u, L"P3 path active across both axes for all 8 evals");
				Assert::AreEqual(uint64_t(0), cntRef.p3ConventionProposals);

				auto findAxisBest = [](const C::AxisDetail& ad, int targetN, const C::CandidateDetail*& outCd) -> bool {
					outCd = nullptr;
					for (size_t i = 0; i < ad.cands.size(); ++i)
					{
						if (ad.cands[i].exact && ad.cands[i].n == targetN)
						{
							outCd = &ad.cands[i];
							return true;
						}
					}
					return false;
				};

				// Every P3 evaluation recalls exact (srcW, srcH) with deep dip, and eval 0 (identical full-line
				// acquiring stage) matches the non-P3 proposer+verifier reference stat to < 1e-12:
				for (size_t i = 0; i < detP3.size(); ++i)
				{
					const C::CandidateDetail* cdX = nullptr;
					const C::CandidateDetail* cdY = nullptr;
					Assert::IsTrue(findAxisBest(detP3[i].axis[0], sc.srcW, cdX), L"P3 recalls true srcW on every eval");
					Assert::IsTrue(findAxisBest(detP3[i].axis[1], sc.srcH, cdY), L"P3 recalls true srcH on every eval");
					Assert::IsTrue(cdX->stat < -0.5 && cdY->stat < -0.5);
				}

				const C::CandidateDetail* p3X0 = nullptr;
				const C::CandidateDetail* refX0 = nullptr;
				const C::CandidateDetail* p3Y0 = nullptr;
				const C::CandidateDetail* refY0 = nullptr;
				Assert::IsTrue(findAxisBest(detP3[0].axis[0], sc.srcW, p3X0));
				Assert::IsTrue(findAxisBest(detRef[0].axis[0], sc.srcW, refX0));
				Assert::IsTrue(findAxisBest(detP3[0].axis[1], sc.srcH, p3Y0));
				Assert::IsTrue(findAxisBest(detRef[0].axis[1], sc.srcH, refY0));
				Assert::IsTrue(std::fabs(p3X0->stat - refX0->stat) < 1e-12,
					L"P3 (proposer skipped + p3TwoPass) matches non-P3 proposer reference stat on X");
				Assert::IsTrue(std::fabs(p3Y0->stat - refY0->stat) < 1e-12,
					L"P3 (proposer skipped + p3TwoPass) matches non-P3 proposer reference stat on Y");

				// Both P3 (proposer skipped) and non-P3 reference latch to the EXACT same conventional geometry:
				Assert::IsTrue(recP3.codedRaster == G::CodedRaster::PlayerUpscaled);
				Assert::IsTrue(recRef.codedRaster == G::CodedRaster::PlayerUpscaled);
				Assert::IsTrue(recP3.applied[0].state == G::LatchState::Latched);
				Assert::IsTrue(recP3.applied[1].state == G::LatchState::Latched);
				Assert::IsTrue(recRef.applied[0].state == G::LatchState::Latched);
				Assert::IsTrue(recRef.applied[1].state == G::LatchState::Latched);
				Assert::AreEqual(sc.srcW, recP3.applied[0].n);
				Assert::AreEqual(sc.srcH, recP3.applied[1].n);
				Assert::AreEqual(recRef.applied[0].n, recP3.applied[0].n);
				Assert::AreEqual(recRef.applied[1].n, recP3.applied[1].n);
			}

			// ---- Part C: Mandatory in-test negative control (Rule 3) ----
			// On a true 1920 -> 3840 Lanczos3 line, propose an intentionally off-by-1 protected candidate (1921 = n0 + 1,
			// matching ChainEstimator's P3 p3ProtectedN contract for classical/latched sizes) alongside conventional
			// candidates (including dipping super-harmonics 2560 and 2880):
			//   - Skipping Pass 2 expansion (exactHalfWidth = 0 only): stuck at n = 1921 with severely degraded stat.
			//   - Two-pass expansion (p3TwoPass = true, exactHalfWidth = 1, p3ProtectedN[0] = 1921): Pass 2 expands
			//     1921 +- 1 and recovers exact n0 = 1920 with markedly deeper stat (matching single-pass full width)!
			{
				const C::LineSnapshot snap = MakeSynthSnapshot(kUhdW, kUhdH, 1920, 1080, kLanczos3, 1,
					900u, 1000000, 0xD909ull, true);
				C::LineSet lsX;
				Assert::IsTrue(C::ToLineSet(snap, 0, lsX));

				const int offByOneCands[6] = { 1280, 1440, 1600, 1921, 2560, 2880 };

				C::VerifyOptions optSkipPass2;
				optSkipPass2.exactOnly = true;
				optSkipPass2.p3TwoPass = false;
				optSkipPass2.exactHalfWidth = 0;

				C::VerifyOptions optTwoPass;
				optTwoPass.exactOnly = true;
				optTwoPass.p3TwoPass = true;
				optTwoPass.exactHalfWidth = 1;
				optTwoPass.p3ProtectedN[0] = 1921;

				C::VerifyOptions optFullSingle;
				optFullSingle.exactOnly = true;
				optFullSingle.p3TwoPass = false;
				optFullSingle.exactHalfWidth = 1;

				C::DescaleVerifier vSkip, vTwo, vFull;
				C::VerifyResult vrSkip, vrTwo, vrFull;
				Assert::IsTrue(vSkip.Evaluate(lsX, offByOneCands, 6, optSkipPass2, vrSkip));
				Assert::IsTrue(vTwo.Evaluate(lsX, offByOneCands, 6, optTwoPass, vrTwo));
				Assert::IsTrue(vFull.Evaluate(lsX, offByOneCands, 6, optFullSingle, vrFull));

				const C::CandidateResult& crSkip = vrSkip.candidates[3];
				const C::CandidateResult& crTwo = vrTwo.candidates[3];
				const C::CandidateResult& crFull = vrFull.candidates[3];

				Assert::IsTrue(crSkip.valid && crSkip.exactDone);
				Assert::IsTrue(crTwo.valid && crTwo.exactDone);
				Assert::IsTrue(crFull.valid && crFull.exactDone);

				// Negative control: without Pass 2 expansion (exactHalfWidth = 0), n stays stuck at 1921 with degraded residual:
				Assert::AreEqual(1921, crSkip.n,
					L"negative control: skipping Pass 2 expansion leaves off-by-1 candidate stuck at n0 + 1");

				// With two-pass expansion (p3TwoPass = true, hw = 1), Pass 2 recovers exact n0 = 1920:
				Assert::AreEqual(1920, crTwo.n,
					L"P3 two-pass Pass 2 expansion recovers exact n0 = 1920 from off-by-1 candidate n0 + 1");
				Assert::AreEqual(crFull.n, crTwo.n);
				Assert::IsTrue(std::fabs(crTwo.stat - crFull.stat) < 1e-12,
					L"P3 two-pass off-by-1 recovery matches single-pass full-width exact bit-identically");
				Assert::IsTrue(crTwo.stat < crSkip.stat - 0.5,
					L"negative control: off-by-1 without Pass 2 suffers > 0.5 nat stat degradation vs two-pass exact n0");
				Assert::IsTrue(crTwo.floorCodes < crSkip.floorCodes * 0.5,
					L"negative control: off-by-1 without Pass 2 leaves > 2x higher raw RMS residual than recovered n0");
			}
		}
	};
}
