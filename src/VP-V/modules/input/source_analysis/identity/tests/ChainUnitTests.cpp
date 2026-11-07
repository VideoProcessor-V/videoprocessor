/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Scaling-chain estimator units (input.source_analysis (identity/chain/)). CPU only, synthetic only: the scene is a
// 1/f-like image built from value-noise octaves (sizes in the ratio 0.59, so no octave is itself an exact simple
// upscale) plus grain, generated with a fixed-seed LCG (never a random device, never a clock). Chains are made with
// the library's own kernel weights (KernelBank.h; its closed forms are pinned independently in
// ChainKernelBankTests) and rounded to 8 or 10 bits AFTER the upscale, like a player.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/identity/chain/ChainEvidence.h>
#include <VP-V/modules/input/source_analysis/identity/chain/DescaleVerifier.h>
#include <VP-V/modules/input/source_analysis/identity/chain/LineSampler.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PolyphaseProposer.h>
#include <VP-V/modules/input/source_analysis/identity/chain/PowerProbe.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace ChainSynth
	{
		using namespace SourceIdentity::Chain;

		constexpr int kFrameH = 1080;
		constexpr int kFrameW = 1920;
		constexpr double kHf = 5.0;        // amplitude (10-bit codes) of the finest octave of the scene
		constexpr double kGrain = 0.8;
		constexpr int kBilinear = 0;
		constexpr int kBicubic = 1;
		constexpr int kLanczos3 = 5;
		constexpr int kSpline36 = 8;

		struct Lcg
		{
			uint32_t x;
			explicit Lcg(uint32_t seed) : x(seed) {}
			double Next() { x = x * 1664525u + 1013904223u; return static_cast<double>(x >> 8) / 16777216.0; }
			double Normal()
			{
				double u1 = Next();
				if (u1 < 1e-12)
					u1 = 1e-12;
				const double u2 = Next();
				return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
			}
		};

		struct Image
		{
			int h = 0;
			int w = 0;
			std::vector<double> v;
			double* Row(int y) { return v.data() + static_cast<size_t>(y) * static_cast<size_t>(w); }
			const double* Row(int y) const { return v.data() + static_cast<size_t>(y) * static_cast<size_t>(w); }
		};

		inline Image ResizeAxis(const Image& in, int axis, int nOut, int kernel)
		{
			const int r = Kernel(kernel).radius;
			const int taps = 2 * r;
			const int nIn = axis == 0 ? in.h : in.w;
			Image out;
			out.h = axis == 0 ? nOut : in.h;
			out.w = axis == 0 ? in.w : nOut;
			out.v.assign(static_cast<size_t>(out.h) * static_cast<size_t>(out.w), 0.0);
			std::vector<double> wt(static_cast<size_t>(nOut) * static_cast<size_t>(taps));
			std::vector<int> idx(static_cast<size_t>(nOut) * static_cast<size_t>(taps));
			for (int i = 0; i < nOut; ++i)
			{
				const double p = (static_cast<double>(i) + 0.5) * static_cast<double>(nIn) / static_cast<double>(nOut) - 0.5;
				const double fl = std::floor(p);
				TapWeights(kernel, p - fl, &wt[static_cast<size_t>(i) * static_cast<size_t>(taps)]);
				for (int s = 0; s < taps; ++s)
				{
					int c = static_cast<int>(fl) - r + 1 + s;
					c = c < 0 ? 0 : (c > nIn - 1 ? nIn - 1 : c);
					idx[static_cast<size_t>(i) * static_cast<size_t>(taps) + static_cast<size_t>(s)] = c;
				}
			}
			if (axis == 0)
			{
				for (int i = 0; i < nOut; ++i)
				{
					double* dst = out.Row(i);
					for (int s = 0; s < taps; ++s)
					{
						const size_t k = static_cast<size_t>(i) * static_cast<size_t>(taps) + static_cast<size_t>(s);
						const double w = wt[k];
						const double* src = in.Row(idx[k]);
						for (int x = 0; x < in.w; ++x)
							dst[x] += w * src[x];
					}
				}
			}
			else
			{
				for (int y = 0; y < in.h; ++y)
				{
					const double* src = in.Row(y);
					double* dst = out.Row(y);
					for (int i = 0; i < nOut; ++i)
					{
						double acc = 0.0;
						for (int s = 0; s < taps; ++s)
						{
							const size_t k = static_cast<size_t>(i) * static_cast<size_t>(taps) + static_cast<size_t>(s);
							acc += wt[k] * src[idx[k]];
						}
						dst[i] = acc;
					}
				}
			}
			return out;
		}

		// 1/f-like scene: octave j is a Gaussian grid of (h, w) x 0.59^j resized to (h, w); amplitude hf x 1.35^j.
		inline Image Scene(int h, int w, double hf, uint32_t seed, double grain)
		{
			Lcg g(seed);
			Image img;
			img.h = h;
			img.w = w;
			img.v.assign(static_cast<size_t>(h) * static_cast<size_t>(w), 0.0);
			for (int j = 0;; ++j)
			{
				const int sh = static_cast<int>(std::floor(static_cast<double>(h) * std::pow(0.59, j) + 0.5));
				const int sw = static_cast<int>(std::floor(static_cast<double>(w) * std::pow(0.59, j) + 0.5));
				if (sh < 4 || sw < 4)
					break;
				Image grid;
				grid.h = sh;
				grid.w = sw;
				grid.v.resize(static_cast<size_t>(sh) * static_cast<size_t>(sw));
				for (size_t i = 0; i < grid.v.size(); ++i)
					grid.v[i] = g.Normal();
				const Image big = (sh == h && sw == w) ? grid : ResizeAxis(ResizeAxis(grid, 0, h, kBicubic), 1, w, kBicubic);
				const double amp = hf * std::pow(1.35, j);
				for (size_t i = 0; i < img.v.size(); ++i)
					img.v[i] += amp * big.v[i];
			}
			if (grain > 0.0)
				for (size_t i = 0; i < img.v.size(); ++i)
					img.v[i] += grain * g.Normal();
			for (size_t i = 0; i < img.v.size(); ++i)
				img.v[i] += 500.0;
			return img;
		}

		inline void Quantize(Image& img, int bits)
		{
			const double step = static_cast<double>(1 << (10 - bits));
			for (size_t i = 0; i < img.v.size(); ++i)
				img.v[i] = std::floor(img.v[i] / step + 0.5) * step;
		}

		// Unsharp mask, Gaussian sigma 1 (radius 3), edge replicated.
		inline Image Usm(const Image& in, double amount)
		{
			double k[7];
			double sum = 0.0;
			for (int i = 0; i < 7; ++i)
			{
				const double x = static_cast<double>(i - 3);
				k[i] = std::exp(-0.5 * x * x);
				sum += k[i];
			}
			for (int i = 0; i < 7; ++i)
				k[i] /= sum;
			Image t = in;
			for (int y = 0; y < in.h; ++y)
				for (int x = 0; x < in.w; ++x)
				{
					double a = 0.0;
					for (int i = -3; i <= 3; ++i)
					{
						int xx = x + i;
						xx = xx < 0 ? 0 : (xx > in.w - 1 ? in.w - 1 : xx);
						a += k[i + 3] * in.Row(y)[xx];
					}
					t.Row(y)[x] = a;
				}
			Image out = in;
			for (int y = 0; y < in.h; ++y)
				for (int x = 0; x < in.w; ++x)
				{
					double a = 0.0;
					for (int i = -3; i <= 3; ++i)
					{
						int yy = y + i;
						yy = yy < 0 ? 0 : (yy > in.h - 1 ? in.h - 1 : yy);
						a += k[i + 3] * t.Row(yy)[x];
					}
					out.Row(y)[x] = in.Row(y)[x] + amount * (in.Row(y)[x] - a);
				}
			return out;
		}

		// 8x8 "coded" blocks: the AC of every block is scaled by `ac` around its mean (an 8-px grid).
		inline Image Blocky(const Image& in, double ac)
		{
			Image out = in;
			const int h8 = in.h / 8 * 8;
			const int w8 = in.w / 8 * 8;
			for (int by = 0; by < h8; by += 8)
				for (int bx = 0; bx < w8; bx += 8)
				{
					double m = 0.0;
					for (int y = 0; y < 8; ++y)
						for (int x = 0; x < 8; ++x)
							m += in.Row(by + y)[bx + x];
					m /= 64.0;
					for (int y = 0; y < 8; ++y)
						for (int x = 0; x < 8; ++x)
							out.Row(by + y)[bx + x] = m + (in.Row(by + y)[bx + x] - m) * ac;
				}
			return out;
		}

		inline Image Letterbox(const Image& content, int frameH, double barLevel)
		{
			Image out;
			out.h = frameH;
			out.w = content.w;
			out.v.assign(static_cast<size_t>(frameH) * static_cast<size_t>(content.w), barLevel);
			const int y0 = (frameH - content.h) / 2;
			for (int y = 0; y < content.h; ++y)
				std::memcpy(out.Row(y0 + y), content.Row(y), static_cast<size_t>(content.w) * sizeof(double));
			return out;
		}

		inline LineRect MakeRect(int x, int y, int w, int h)
		{
			LineRect r;
			r.x = x;
			r.y = y;
			r.w = w;
			r.h = h;
			return r;
		}

		struct Axes
		{
			bool ok = false;
			LineSet h;   // axis 0, rows
			LineSet v;   // axis 1, columns
		};

		inline Axes MakeAxes(const Image& img, const LineRect& active, uint64_t seq)
		{
			Axes a;
			const LinePlan p = LineSampler::Plan(active, seq);
			a.ok = p.valid && LineSampler::Fill(p, 0, img.v.data(), img.w, 1.0, a.h) && LineSampler::Fill(p, 1, img.v.data(), img.w, 1.0, a.v);
			return a;
		}

		// Scenes are expensive; the sampled lines of one scene serve several tests.
		inline const Axes& Cached(const std::string& key, const std::function<Image()>& make, const LineRect& active)
		{
			static std::map<std::string, Axes> cache;
			auto it = cache.find(key);
			if (it == cache.end())
				it = cache.insert(std::make_pair(key, MakeAxes(make(), active, 7))).first;
			return it->second;
		}

		inline Image ChainScene(int latV, int latH, int outV, int outH, int kernel, uint32_t seed, int bits, double usm, double hf = kHf)
		{
			Image sc = Scene(latV, latH, hf, seed, kGrain);
			Image up = ResizeAxis(ResizeAxis(sc, 0, outV, kernel), 1, outH, kernel);
			if (usm > 0.0)
				up = Usm(up, usm);
			Quantize(up, bits);
			return up;
		}

		inline const Axes& FullChain(int latV, int latH, int kernel, uint32_t seed, int bits, double usm = 0.0)
		{
			std::ostringstream key;
			key << "chain" << latV << "x" << latH << "k" << kernel << "s" << seed << "b" << bits << "u" << usm;
			return Cached(key.str(), [=]() { return ChainScene(latV, latH, kFrameH, kFrameW, kernel, seed, bits, usm); }, MakeRect(0, 0, kFrameW, kFrameH));
		}

		inline const Axes& Native(double hf, uint32_t seed, int bits, double blockyAc = 0.0)
		{
			std::ostringstream key;
			key << "native" << hf << "s" << seed << "b" << bits << "k" << blockyAc;
			return Cached(key.str(), [=]() {
				Image im = Scene(kFrameH, kFrameW, hf, seed, 0.5 + 0.15 * hf);
				if (blockyAc > 0.0)
					im = Blocky(im, blockyAc);
				Quantize(im, bits);
				return im;
			}, MakeRect(0, 0, kFrameW, kFrameH));
		}

		struct AxisRun
		{
			VerifyResult vr;
			std::vector<ProposedCandidate> props;
			std::vector<PeriodScore> periods;
		};

		inline AxisRun Analyse(const LineSet& ls, const std::vector<int>& caller, DescaleVerifier& verifier, int evaluations = 1)
		{
			PolyphaseProposer prop;
			for (int e = 0; e < evaluations; ++e)
				prop.AddEvaluation(ls);
			ProposedCandidate pc[kMaxCandidates];
			const int np = prop.Propose(ls.axisLen, caller.empty() ? nullptr : caller.data(), static_cast<int>(caller.size()), pc, kMaxCandidates);
			AxisRun r;
			r.props.assign(pc, pc + np);
			PeriodScore ps[kMaxCandidates];
			const int nper = prop.Periods(ps, kMaxCandidates);
			r.periods.assign(ps, ps + nper);
			std::vector<int> cand;
			for (int i = 0; i < np; ++i)
				cand.push_back(pc[i].n);
			if (!cand.empty())
				verifier.Evaluate(ls, cand.data(), static_cast<int>(cand.size()), VerifyOptions(), r.vr);
			return r;
		}

		inline const CandidateResult* FindExact(const AxisRun& r, int n)
		{
			for (size_t i = 0; i < r.vr.candidates.size(); ++i)
				if (r.vr.candidates[i].valid && r.vr.candidates[i].exactDone && r.vr.candidates[i].n == n)
					return &r.vr.candidates[i];
			return nullptr;
		}

		inline bool HasProposal(const AxisRun& r, int n)
		{
			for (size_t i = 0; i < r.props.size(); ++i)
				if (r.props[i].n == n)
					return true;
			return false;
		}

		inline bool FamilyOk(int trueKernel, int foundKernel, bool bilinearBicubicAlike)
		{
			const Family a = Kernel(trueKernel).family;
			const Family b = Kernel(foundKernel).family;
			if (a == b)
				return true;
			const bool aLin = a == Family::Bilinear || a == Family::Bicubic;
			const bool bLin = b == Family::Bilinear || b == Family::Bicubic;
			return bilinearBicubicAlike && aLin && bLin;
		}

		inline std::wstring Wide(const std::string& s)
		{
			return std::wstring(s.begin(), s.end());
		}

		inline std::string Describe(const char* tag, const AxisRun& r)
		{
			std::ostringstream o;
			o << tag << ":";
			for (size_t i = 0; i < r.vr.candidates.size(); ++i)
			{
				const CandidateResult& c = r.vr.candidates[i];
				o << " [n=" << c.n << (c.exactDone ? " exact" : " coarse") << " T=" << c.stat << " k=" << (c.kernel >= 0 ? Kernel(c.kernel).name : "-") << "]";
			}
			return o.str();
		}

		// A found chain: the true size is verified at full lines, with at least `maxStat` of dip.
		inline void ExpectChain(const char* tag, const LineSet& ls, int trueN, int trueKernel, double maxStat, bool checkFamily, bool twoX, DescaleVerifier& verifier)
		{
			const AxisRun r = Analyse(ls, std::vector<int>(), verifier);
			const std::string msg = Describe(tag, r);
			Logger::WriteMessage(msg.c_str());
			Assert::IsTrue(HasProposal(r, trueN), Wide(std::string(tag) + ": the true size was not proposed").c_str());
			const CandidateResult* c = FindExact(r, trueN);
			Assert::IsTrue(c != nullptr, Wide(std::string(tag) + ": the true size was not verified at full lines").c_str());
			Assert::IsTrue(c->stat <= maxStat, Wide(std::string(tag) + ": the dip at the true size is too shallow").c_str());
			if (checkFamily)
				Assert::IsTrue(FamilyOk(trueKernel, c->kernel, twoX), Wide(std::string(tag) + ": wrong kernel family").c_str());
		}

		inline bool SameBytes(const std::vector<double>& a, const std::vector<double>& b)
		{
			return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
		}

		inline bool SameCandidate(const CandidateResult& a, const CandidateResult& b)
		{
			return a.nProposed == b.nProposed && a.n == b.n && a.valid == b.valid && a.exactDone == b.exactDone && a.kernel == b.kernel
				&& a.family == b.family && std::memcmp(&a.phase, &b.phase, sizeof(double)) == 0 && std::memcmp(&a.stat, &b.stat, sizeof(double)) == 0
				&& std::memcmp(&a.coarseStat, &b.coarseStat, sizeof(double)) == 0 && std::memcmp(&a.kernelMargin, &b.kernelMargin, sizeof(double)) == 0
				&& std::memcmp(&a.familyMargin, &b.familyMargin, sizeof(double)) == 0 && std::memcmp(&a.floorCodes, &b.floorCodes, sizeof(double)) == 0
				&& a.reason == b.reason && a.lineCount == b.lineCount && a.segCount == b.segCount
				&& SameBytes(a.lineStat, b.lineStat) && SameBytes(a.lineBase, b.lineBase) && SameBytes(a.lineRms, b.lineRms)
				&& SameBytes(a.segSum, b.segSum) && SameBytes(a.segStat, b.segStat) && SameBytes(a.segBase, b.segBase) && a.segRows == b.segRows;
		}

		// Hand-made verifier result for the evidence logic tests: 32 clean dip lines of detail `base` codes.
		inline CandidateResult MakeCandidate(int n, double lineStat, double base)
		{
			CandidateResult c;
			c.nProposed = n;
			c.n = n;
			c.valid = true;
			c.exactDone = true;
			c.kernel = kLanczos3;
			c.family = static_cast<int>(Family::Lanczos);
			c.stat = lineStat;
			c.lineCount = 32;
			c.segCount = 16;
			c.lineStat.assign(32, lineStat);
			c.lineBase.assign(32, base);
			c.lineRms.assign(32, 0.3);
			c.segSum.assign(32 * 16, 1.0);
			c.segRows.assign(32 * 16, kSegPx);
			c.segStat.assign(32 * 16, lineStat);
			c.segBase.assign(32 * 16, base);
			return c;
		}

		inline AxisObservation Observe(const VerifyResult& vr, int axisLen, uint32_t shot, bool seen)
		{
			AxisObservation o;
			o.verify = &vr;
			o.axisLen = axisLen;
			o.floorCodes = 0.29;
			o.probeRan = true;
			o.probeSeen = seen;
			o.shotId = shot;
			return o;
		}
	}

	TEST_CLASS(ChainKernelBankTests)
	{
	public:
		TEST_METHOD(EveryKernelIsAPartitionOfUnityAndSplinesMatchTheirClosedForms)
		{
			using namespace SourceIdentity::Chain;
			const double ts[] = { 0.0, 0.13, 0.5, 0.77, 0.999 };
			for (int k = 0; k < kKernelCount; ++k)
				for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); ++i)
				{
					double w[8];
					TapWeights(k, ts[i], w);
					double sum = 0.0;
					for (int s = 0; s < 2 * Kernel(k).radius; ++s)
						sum += w[s];
					Assert::AreEqual(1.0, sum, 1e-12, L"weights sum to one");
				}
			// Spline36 / spline16 closed forms (Avisynth/zimg polynomials).
			auto spline36 = [](double x) {
				x = x < 0 ? -x : x;
				if (x < 1.0)
					return ((13.0 / 11.0 * x - 453.0 / 209.0) * x - 3.0 / 209.0) * x + 1.0;
				if (x < 2.0)
				{
					x -= 1.0;
					return ((-6.0 / 11.0 * x + 270.0 / 209.0) * x - 156.0 / 209.0) * x;
				}
				if (x < 3.0)
				{
					x -= 2.0;
					return ((1.0 / 11.0 * x - 45.0 / 209.0) * x + 26.0 / 209.0) * x;
				}
				return 0.0;
			};
			auto spline16 = [](double x) {
				x = x < 0 ? -x : x;
				if (x < 1.0)
					return ((x - 9.0 / 5.0) * x - 1.0 / 5.0) * x + 1.0;
				if (x < 2.0)
				{
					x -= 1.0;
					return ((-1.0 / 3.0 * x + 4.0 / 5.0) * x - 7.0 / 15.0) * x;
				}
				return 0.0;
			};
			for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); ++i)
			{
				double w36[8];
				double w16[8];
				TapWeights(8, ts[i], w36);
				TapWeights(7, ts[i], w16);
				for (int s = 0; s < 6; ++s)
					Assert::AreEqual(spline36(ts[i] - static_cast<double>(s - 2)), w36[s], 1e-12, L"spline36 matches its closed form");
				for (int s = 0; s < 4; ++s)
					Assert::AreEqual(spline16(ts[i] - static_cast<double>(s - 1)), w16[s], 1e-12, L"spline16 matches its closed form");
			}
		}

		TEST_METHOD(InterpolatingKernelsReturnTheSampleAtPhaseZeroAndBilinearIsLinear)
		{
			using namespace SourceIdentity::Chain;
			const int centre[] = { 0, 1, 2, 4, 5, 6, 7, 8, 9 };   // every interpolating kernel (not B/C 1/3)
			for (size_t i = 0; i < sizeof(centre) / sizeof(centre[0]); ++i)
			{
				double w[8];
				TapWeights(centre[i], 0.0, w);
				const int r = Kernel(centre[i]).radius;
				for (int s = 0; s < 2 * r; ++s)
					Assert::AreEqual(s == r - 1 ? 1.0 : 0.0, w[s], 1e-12, L"phase zero is a copy");
			}
			double w[8];
			TapWeights(0, 0.25, w);
			Assert::AreEqual(0.75, w[0], 1e-12);
			Assert::AreEqual(0.25, w[1], 1e-12);
		}
	};

	TEST_CLASS(ChainLineSamplerTests)
	{
	public:
		TEST_METHOD(PlanIsStratifiedInsideTheMarginAndDeterministic)
		{
			using namespace SourceIdentity::Chain;
			const LineRect active = ChainSynth::MakeRect(0, 0, 3840, 2160);
			const LinePlan a = LineSampler::Plan(active, 7);
			const LinePlan b = LineSampler::Plan(active, 7);
			const LinePlan c = LineSampler::Plan(active, 8);
			Assert::IsTrue(a.valid && a.rowCount == 32 && a.colCount == 32);
			bool differs = false;
			for (int i = 0; i < 32; ++i)
			{
				Assert::IsTrue(a.rowY[i] == b.rowY[i] && a.colX[i] == b.colX[i], L"same sequence, same plan");
				Assert::IsTrue(a.rowY[i] >= 48 && a.rowY[i] < 2160 - 48, L"rows inside the margin");
				Assert::IsTrue(a.colX[i] >= 48 && a.colX[i] < 3840 - 48, L"columns inside the margin");
				const int lo = 48 + static_cast<int>((static_cast<int64_t>(2160 - 96) * i) / 32);
				const int hi = 48 + static_cast<int>((static_cast<int64_t>(2160 - 96) * (i + 1)) / 32);
				Assert::IsTrue(a.rowY[i] >= lo && a.rowY[i] < hi, L"one row per stratum");
				if (i > 0)
					Assert::IsTrue(a.rowY[i] > a.rowY[i - 1], L"strata are disjoint, so the rows are distinct and ordered");
				if (a.rowY[i] != c.rowY[i] || a.colX[i] != c.colX[i])
					differs = true;
			}
			Assert::IsTrue(differs, L"another sequence number picks other lines");
		}

		TEST_METHOD(LetterboxedRectangleKeepsTheLinesInsideTheActiveArea)
		{
			using namespace SourceIdentity::Chain;
			const LineRect active = ChainSynth::MakeRect(0, 140, 1920, 800);
			const LinePlan p = LineSampler::Plan(active, 3);
			Assert::IsTrue(p.valid);
			for (int i = 0; i < p.rowCount; ++i)
				Assert::IsTrue(p.rowY[i] >= 140 + 48 && p.rowY[i] < 140 + 800 - 48, L"no row from the bars");
			for (int i = 0; i < p.colCount; ++i)
				Assert::IsTrue(p.colX[i] >= 48 && p.colX[i] < 1920 - 48);
			const LinePlan tiny = LineSampler::Plan(ChainSynth::MakeRect(0, 0, 100, 100), 3);
			Assert::IsFalse(tiny.valid, L"too little left after the margins");
		}

		TEST_METHOD(FillCopiesTheRightSamplesInCodeUnits)
		{
			using namespace SourceIdentity::Chain;
			const int w = 640;
			const int h = 480;
			std::vector<uint16_t> plane(static_cast<size_t>(w) * h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					plane[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>((x * 3 + y * 5) & 1023);
			const LineRect active = ChainSynth::MakeRect(10, 20, 600, 440);
			const LinePlan p = LineSampler::Plan(active, 11);
			Assert::IsTrue(p.valid);
			LineSet rows;
			LineSet cols;
			Assert::IsTrue(LineSampler::Fill(p, 0, plane.data(), w, 1.0, rows));
			Assert::IsTrue(LineSampler::Fill(p, 1, plane.data(), w, 0.25, cols));
			Assert::IsTrue(rows.axis == 0 && rows.axisLen == 600 && rows.origin == 48 && rows.length == 600 - 96);
			Assert::IsTrue(cols.axis == 1 && cols.axisLen == 440 && cols.origin == 48 && cols.length == 440 - 96);
			// Bars baked into the scaled raster: the scaler axis is the whole frame.
			const LineRect frame = ChainSynth::MakeRect(0, 0, w, h);
			LineSet frameRows;
			Assert::IsTrue(LineSampler::Fill(p, 0, plane.data(), w, 1.0, frameRows, &frame));
			Assert::IsTrue(frameRows.axisLen == 640 && frameRows.origin == p.inner.x && frameRows.length == rows.length);
			for (int j = 0; j < rows.count; ++j)
				for (int i = 0; i < rows.length; i += 17)
					Assert::AreEqual(static_cast<double>(plane[static_cast<size_t>(p.rowY[j]) * w + p.inner.x + i]), rows.Line(j)[i], 0.0);
			for (int j = 0; j < cols.count; ++j)
				for (int i = 0; i < cols.length; i += 17)
					Assert::AreEqual(0.25 * static_cast<double>(plane[static_cast<size_t>(p.inner.y + i) * w + p.colX[j]]), cols.Line(j)[i], 0.0);
		}
	};

	TEST_CLASS(ChainVerifierTests)
	{
	public:
		TEST_METHOD(NoiselessLatentIsRecoveredExactlyAtTheTrueSizeOnly)
		{
			using namespace SourceIdentity::Chain;
			const int N = 1080;
			const int n = 540;
			const int origin = 48;
			const int length = 984;
			ChainSynth::Lcg g(5u);
			std::vector<double> z(n + 40, 0.0);
			for (size_t i = 0; i < z.size(); ++i)
				z[i] = 10.0 * g.Normal();
			std::vector<double> y(static_cast<size_t>(length), 0.0);
			const int r = Kernel(5).radius;
			for (int j = 0; j < length; ++j)
			{
				const double p = (static_cast<double>(origin + j) + 0.5) * n / N - 0.5;
				const double fl = std::floor(p);
				double w[8];
				TapWeights(5, p - fl, w);
				double acc = 0.0;
				for (int s = 0; s < 2 * r; ++s)
					acc += w[s] * z[static_cast<size_t>(static_cast<int>(fl) - r + 1 + s + 5)];
				y[static_cast<size_t>(j)] = acc;
			}
			std::vector<double> scratch;
			double seg[kMaxSegs];
			int segRows[kMaxSegs];
			DescaleOperator exact;
			Assert::IsTrue(exact.Build(N, n, 5, 0.0, origin, length));
			double rss = 0.0;
			int rows = 0;
			exact.Residual(y.data(), scratch, rss, rows, seg, segRows);
			Assert::IsTrue(rows == length - 2 * exact.Margin() && rows > 0);
			Assert::IsTrue(std::sqrt(rss / rows) < 1e-6, L"a true latent grid leaves no residual");
			DescaleOperator off;
			Assert::IsTrue(off.Build(N, n + 20, 5, 0.0, origin, length));
			off.Residual(y.data(), scratch, rss, rows, seg, segRows);
			Assert::IsTrue(std::sqrt(rss / rows) > 0.5, L"a wrong size cannot represent the line");
			double segTotal = 0.0;
			int segRowTotal = 0;
			for (int s = 0; s < kMaxSegs; ++s)
			{
				segTotal += seg[s];
				segRowTotal += segRows[s];
			}
			Assert::AreEqual(rss, segTotal, 1e-9 * (1.0 + rss), L"segment sums add up to the residual");
			Assert::IsTrue(segRowTotal == rows);
		}

		TEST_METHOD(HalfSizeChainsOfEveryFamilyAreFoundAt10Bits)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			const int kernels[4] = { kBilinear, kBicubic, kLanczos3, kSpline36 };
			for (int i = 0; i < 4; ++i)
			{
				const Axes& a = FullChain(540, 960, kernels[i], 101u + i, 10);
				Assert::IsTrue(a.ok);
				ExpectChain("1/2 V", a.v, 540, kernels[i], -0.5, true, true, v);
				ExpectChain("1/2 H", a.h, 960, kernels[i], -0.5, true, true, v);
			}
		}

		TEST_METHOD(OtherRatiosAreFoundAtBothBitDepths)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			struct Case { int latV; int latH; const char* tag; };
			const Case cases[3] = { { 720, 1280, "2/3" }, { 360, 640, "1/3" }, { 450, 800, "5/12" } };
			for (int i = 0; i < 3; ++i)
			{
				const Axes& a10 = FullChain(cases[i].latV, cases[i].latH, kLanczos3, 201u + i, 10);
				ExpectChain((std::string(cases[i].tag) + " lanczos3 10-bit V").c_str(), a10.v, cases[i].latV, kLanczos3, -0.5, true, false, v);
				const Axes& a8 = FullChain(cases[i].latV, cases[i].latH, kLanczos3, 201u + i, 8);
				ExpectChain((std::string(cases[i].tag) + " lanczos3 8-bit V").c_str(), a8.v, cases[i].latV, kLanczos3, -0.12, false, false, v);
			}
			const Axes& sp = FullChain(450, 800, kSpline36, 231u, 10);
			ExpectChain("5/12 spline36 10-bit H", sp.h, 800, kSpline36, -0.5, true, false, v);
			const Axes& bc = FullChain(720, 1280, kBicubic, 232u, 10);
			ExpectChain("2/3 bicubic 10-bit V", bc.v, 720, kBicubic, -0.5, true, false, v);
		}

		TEST_METHOD(SharpeningAfterTheUpscaleKeepsTheSizeAndTheFamily)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			const Axes& a10 = FullChain(540, 960, kLanczos3, 301u, 10, 0.6);
			ExpectChain("1/2 lanczos3 + USM 0.6 10-bit V", a10.v, 540, kLanczos3, -0.5, true, true, v);
			const Axes& a8 = FullChain(540, 960, kLanczos3, 301u, 8, 0.6);
			ExpectChain("1/2 lanczos3 + USM 0.6 8-bit V", a8.v, 540, kLanczos3, -0.12, false, true, v);
		}

		TEST_METHOD(DoubleScalingShowsAtLeastOneOfItsLatentGrids)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			const Axes& a = Cached("double", []() {
				Image sc = Scene(360, 640, kHf, 401u, kGrain);
				Image mid = ResizeAxis(ResizeAxis(sc, 0, 540, kBicubic), 1, 960, kBicubic);
				Image up = ResizeAxis(ResizeAxis(mid, 0, kFrameH, kLanczos3), 1, kFrameW, kLanczos3);
				Quantize(up, 10);
				return up;
			}, MakeRect(0, 0, kFrameW, kFrameH));
			const AxisRun r = Analyse(a.v, std::vector<int>(), v);
			Logger::WriteMessage(Describe("double 360->540->1080 V", r).c_str());
			const CandidateResult* last = FindExact(r, 540);
			const CandidateResult* first = FindExact(r, 360);
			Assert::IsTrue((last != nullptr && last->stat <= -0.12) || (first != nullptr && first->stat <= -0.12),
				L"one of the two latent grids is verified at full lines");
		}

		TEST_METHOD(LetterboxedPictureIsAnalysedInsideTheActiveArea)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			const Axes& a = Cached("letterbox", []() {
				Image content = ChainScene(400, 960, 800, kFrameW, kLanczos3, 501u, 10, 0.0);
				return Letterbox(content, kFrameH, 64.0);
			}, MakeRect(0, 140, kFrameW, 800));
			Assert::IsTrue(a.ok);
			Assert::IsTrue(a.v.axisLen == 800 && a.v.origin == 48 && a.h.axisLen == kFrameW);
			ExpectChain("letterbox V (N = 800)", a.v, 400, kLanczos3, -0.5, true, true, v);
			ExpectChain("letterbox H", a.h, 960, kLanczos3, -0.5, true, true, v);
		}

		TEST_METHOD(NativePicturesShowNoDipAtAnyCommonSize)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			const double hfs[3] = { 10.0, 5.0, 2.0 };
			for (int b = 0; b < 2; ++b)
				for (int i = 0; i < 3; ++i)
				{
					const int bits = b == 0 ? 10 : 8;
					const Axes& a = Native(hfs[i], 601u + i, bits);
					const int caller[7] = { 540, 720, 360, 450, 810, 945, 675 };
					const AxisRun r = Analyse(a.v, std::vector<int>(caller, caller + 7), v);
					Logger::WriteMessage(Describe("native V", r).c_str());
					for (size_t k = 0; k < r.vr.candidates.size(); ++k)
					{
						const CandidateResult& c = r.vr.candidates[k];
						if (c.valid)
							Assert::IsTrue(c.stat > -0.2, L"a native picture shows no dip at a common size");
					}
					for (size_t k = 0; k < r.props.size(); ++k)
						Assert::IsTrue(r.props[k].source == kSourceCaller, L"no polyphase evidence on a native picture");
				}
		}

		TEST_METHOD(EightPixelBlocksOnANativeGiveOnlyAShallowDipAtTheGridSizes)
		{
			using namespace ChainSynth;
			DescaleVerifier v;
			const Axes& a = Native(kHf, 701u, 10, 0.7);
			const int caller[7] = { 540, 810, 945, 675, 720, 360, 450 };
			const AxisRun r = Analyse(a.v, std::vector<int>(caller, caller + 7), v);
			Logger::WriteMessage(Describe("blocky native V", r).c_str());
			for (size_t k = 0; k < r.vr.candidates.size(); ++k)
			{
				const CandidateResult& c = r.vr.candidates[k];
				if (c.valid)
					Assert::IsTrue(c.stat > -0.3, L"a coding grid dips far less than an upscale");
			}
		}

		TEST_METHOD(SameLinesGiveByteIdenticalResultsInAnyCandidateOrderAndWithAWarmCache)
		{
			using namespace ChainSynth;
			const Axes& a = FullChain(720, 1280, kBicubic, 232u, 10);
			const int fwd[4] = { 720, 360, 540, 810 };
			const int rev[4] = { 810, 540, 360, 720 };
			DescaleVerifier v1;
			DescaleVerifier v2;
			VerifyResult r1;
			VerifyResult r2;
			VerifyResult r3;
			VerifyResult r4;
			Assert::IsTrue(v1.Evaluate(a.v, fwd, 4, VerifyOptions(), r1));
			Assert::IsTrue(v2.Evaluate(a.v, fwd, 4, VerifyOptions(), r2));
			Assert::IsTrue(v1.Evaluate(a.v, rev, 4, VerifyOptions(), r3));   // warm cache, other order
			VerifierSettings st;
			st.cacheLimit = 3;
			DescaleVerifier v4(st);
			Assert::IsTrue(v4.Evaluate(a.v, fwd, 4, VerifyOptions(), r4));
			for (size_t i = 0; i < 4; ++i)
			{
				Assert::IsTrue(ChainSynth::SameCandidate(r1.candidates[i], r2.candidates[i]), L"two fresh runs agree bit for bit");
				Assert::IsTrue(ChainSynth::SameCandidate(r1.candidates[i], r4.candidates[i]), L"the cache size never changes a number");
				Assert::IsTrue(ChainSynth::SameCandidate(r1.candidates[i], r3.candidates[3 - i]), L"candidate order and a warm cache never change a number");
			}
			Assert::IsTrue(v1.CachedOperators() <= v1.Settings().cacheLimit);
		}
	};

	TEST_CLASS(ChainProposerTests)
	{
	public:
		TEST_METHOD(PeriodsOfUpscaledChainsAreFoundAndNativesHaveNone)
		{
			using namespace ChainSynth;
			struct Case { int latV; int latH; int q; int n; };
			const Case cases[4] = { { 540, 960, 2, 540 }, { 720, 1280, 3, 720 }, { 360, 640, 3, 360 }, { 450, 800, 12, 450 } };
			for (int i = 0; i < 4; ++i)
			{
				const Axes& a = FullChain(cases[i].latV, cases[i].latH, kLanczos3, 801u + i, 10);
				PolyphaseProposer prop;
				Assert::IsTrue(prop.AddEvaluation(a.v));
				PeriodScore ps[kMaxCandidates];
				const int np = prop.Periods(ps, kMaxCandidates);
				bool found = false;
				for (int k = 0; k < np; ++k)
					if (ps[k].q == cases[i].q)
						found = true;
				Assert::IsTrue(found, L"the true period is accepted");
				ProposedCandidate pc[kMaxCandidates];
				const int c = prop.Propose(a.v.axisLen, nullptr, 0, pc, kMaxCandidates);
				bool has = false;
				for (int k = 0; k < c; ++k)
					if (pc[k].n == cases[i].n)
						has = true;
				Assert::IsTrue(has, L"the true size is among the proposals");
				Assert::IsTrue(c <= kMaxCandidates);
			}
			for (int b = 0; b < 2; ++b)
			{
				const Axes& a = Native(kHf, 811u, b == 0 ? 10 : 8);
				PolyphaseProposer prop;
				prop.AddEvaluation(a.v);
				prop.AddEvaluation(a.h);
				PeriodScore ps[kMaxCandidates];
				Assert::IsTrue(prop.Periods(ps, kMaxCandidates) == 0, L"a native picture has no phase period");
			}
		}

		TEST_METHOD(CallerSizesComeFirstAndAreDeduplicated)
		{
			using namespace ChainSynth;
			const Axes& a = FullChain(540, 960, kLanczos3, 801u, 10);
			PolyphaseProposer prop;
			prop.AddEvaluation(a.v);
			const int caller[4] = { 777, 540, 777, 5 };   // 5 is not a plausible latent size
			ProposedCandidate pc[kMaxCandidates];
			const int c = prop.Propose(a.v.axisLen, caller, 4, pc, kMaxCandidates);
			Assert::IsTrue(c >= 2);
			Assert::IsTrue(pc[0].n == 777 && pc[0].source == kSourceCaller);
			Assert::IsTrue(pc[1].n == 540 && pc[1].source == kSourceCaller);
			int count540 = 0;
			for (int i = 0; i < c; ++i)
				if (pc[i].n == 540)
					++count540;
			Assert::IsTrue(count540 == 1, L"the polyphase 540 is not listed twice");
		}

		TEST_METHOD(AccumulationIsDeterministicAndResetClears)
		{
			using namespace ChainSynth;
			const Axes& a = FullChain(720, 1280, kLanczos3, 202u, 10);
			PolyphaseProposer p1;
			PolyphaseProposer p2;
			for (int e = 0; e < 3; ++e)
			{
				p1.AddEvaluation(a.v);
				p2.AddEvaluation(a.v);
			}
			PeriodScore s1[kMaxCandidates];
			PeriodScore s2[kMaxCandidates];
			const int n1 = p1.Periods(s1, kMaxCandidates);
			const int n2 = p2.Periods(s2, kMaxCandidates);
			Assert::IsTrue(n1 == n2 && n1 > 0);
			for (int i = 0; i < n1; ++i)
				Assert::IsTrue(s1[i].q == s2[i].q && std::memcmp(&s1[i].dBic, &s2[i].dBic, sizeof(double)) == 0, L"same sequence, same numbers");
			p1.Reset();
			Assert::IsTrue(p1.Periods(s1, kMaxCandidates) == 0 && p1.Evaluations() == 0);
		}
	};

	TEST_CLASS(ChainEvidenceTests)
	{
	public:
		TEST_METHOD(ThresholdsFollowTheDocumentedFormula)
		{
			using namespace SourceIdentity::Chain;
			const Thresholds t = ComputeThresholds(EvidenceSettings());
			Assert::IsTrue(t.c1 > 14.7 && t.c1 < 15.0, L"1 per 100 h, 10 s, 8 candidates x 10 kernels is about 15 nats");
			Assert::AreEqual(t.c1 + 3.0, t.c2, 1e-12);
			EvidenceSettings s;
			s.targetRatePerHour = 1.0;
			const Thresholds t2 = ComputeThresholds(s);
			Assert::IsTrue(t2.c1 < t.c1, L"a laxer target rate lowers c1");
			Assert::AreEqual(-std::log(1.0 * 10.0 / 3600.0) + std::log(80.0), t2.c1, 1e-9);
		}

		TEST_METHOD(NullClassesAreChosenByTheSizeRelativeToTheAxis)
		{
			using namespace SourceIdentity::Chain;
			Assert::IsTrue(ClassifyLatent(540, 1080) == kClassK8, L"1/2 = 4/8");
			Assert::IsTrue(ClassifyLatent(541, 1080) == kClassK8, L"within +-2");
			Assert::IsTrue(ClassifyLatent(945, 1080) == kClassK8, L"7/8");
			Assert::IsTrue(ClassifyLatent(675, 1080) == kClassK8, L"5/8 is k/8");
			Assert::IsTrue(ClassifyLatent(1080 * 9 / 16, 1080) == kClassK16, L"9/16");
			Assert::IsTrue(ClassifyLatent(720, 1080) == kClassRational, L"2/3");
			Assert::IsTrue(ClassifyLatent(360, 1080) == kClassRational, L"1/3");
			Assert::IsTrue(ClassifyLatent(450, 1080) == kClassGeneric, L"5/12 has q = 12 > 6");
			Assert::IsTrue(ClassifyLatent(1000, 1080) == kClassGeneric);
		}

		TEST_METHOD(PlaceholderIncrementsFavourADipAndPenaliseNoDip)
		{
			using namespace SourceIdentity::Chain;
			const NullTables t = NullTables::Placeholder();
			Assert::IsTrue(t.placeholder);
			for (int c = 0; c < kNullClassCount; ++c)
			{
				Assert::IsTrue(t.LogLikelihoodRatio(c, -1.2) > 5.0, L"a deep dip is decisive");
				Assert::IsTrue(t.LogLikelihoodRatio(c, 0.0) < 0.0, L"no dip counts against an upscale");
			}
			Assert::IsTrue(t.LogLikelihoodRatio(kClassK8, -0.15) < 0.0, L"a shallow dip at a coding-grid size is not evidence");
		}

		TEST_METHOD(OneShotIsCappedAndSeveralShotsReachC1)
		{
			using namespace ChainSynth;
			VerifyResult vr;
			vr.candidates.push_back(MakeCandidate(540, -1.2, 5.0));
			ChainEvidence ev;
			const double c1 = ev.GetThresholds().c1;
			EvidenceUpdate u;
			for (int e = 0; e < 6; ++e)
			{
				Assert::IsTrue(ev.Submit(1, Observe(vr, 1080, 1, false), u));
				Assert::IsTrue(u.informative && !u.hold);
			}
			AxisSummary s = ev.Summary(1);
			Assert::IsTrue(s.hasBest && s.n == 540);
			Assert::IsTrue(s.llr <= 0.5 * c1 + 1e-9 && s.llr > 0.5 * c1 - 1e-9, L"one shot saturates at c1 / 2");
			Assert::IsFalse(s.aboveC1);
			for (uint32_t shot = 2; shot <= 4; ++shot)
				for (int e = 0; e < 3; ++e)
					ev.Submit(1, Observe(vr, 1080, shot, false), u);
			s = ev.Summary(1);
			Assert::IsTrue(s.aboveC1 && s.aboveC2, L"four shots of decisive evidence pass c1 and c2");
			Assert::IsTrue(s.shots == 4 && s.informativeEvals == 15);
			Assert::IsTrue(s.otherAxisAbstains, L"the other axis gave nothing: relaxed, no prior");
			Assert::AreEqual(0.0, s.prior, 0.0);
			Assert::IsTrue((u.reason & kReasonNoPower) != 0, L"unseen probe is reported");
			Assert::IsFalse(u.nativeCounted);
		}

		TEST_METHOD(NoDipEvidenceAccumulatesAgainstTheUpscaleAndForNativeOnlyWithPower)
		{
			using namespace ChainSynth;
			VerifyResult vr;
			vr.candidates.push_back(MakeCandidate(720, -0.005, 5.0));
			ChainEvidence ev;
			EvidenceUpdate u;
			for (uint32_t shot = 1; shot <= 4; ++shot)
				ev.Submit(0, Observe(vr, 1080, shot, false), u);
			Assert::IsTrue(ev.Summary(0).llr < 0.0, L"a flat statistic is evidence against an upscale");
			Assert::IsTrue(ev.Summary(0).nativeEvals == 0, L"no native evidence without power");
			ChainEvidence ev2;
			for (uint32_t shot = 1; shot <= 4; ++shot)
				for (int e = 0; e < 4; ++e)
					ev2.Submit(0, Observe(vr, 1080, shot, true), u);
			const AxisSummary s = ev2.Summary(0);
			Assert::IsTrue(s.nativeEvals == 16 && s.nativeLlr > 0.0, L"with power, no dip is positive native evidence");
			Assert::IsTrue(u.nativeCounted && u.nativeIncrement > 0.0);
			Assert::IsTrue(s.llr < ev2.GetThresholds().c1);
		}

		TEST_METHOD(LineGateMakesLowDetailEvaluationsAbstain)
		{
			using namespace ChainSynth;
			VerifyResult vr;
			vr.candidates.push_back(MakeCandidate(540, -1.0, 0.5));   // detail 0.5 codes < 3 x 0.29 floor... gate is 0.87
			ChainEvidence ev;
			EvidenceUpdate u;
			Assert::IsTrue(ev.Submit(1, Observe(vr, 1080, 1, false), u));
			Assert::IsFalse(u.informative);
			Assert::IsTrue((u.reason & kReasonLowDetail) != 0);
			Assert::IsFalse(ev.Summary(1).hasBest, L"abstaining leaves no hypothesis");
			// An 8-bit floor (1.15 codes) raises the gate to 3.5 codes.
			VerifyResult vr2;
			vr2.candidates.push_back(MakeCandidate(540, -1.0, 2.0));
			AxisObservation o = Observe(vr2, 1080, 1, false);
			o.floorCodes = 1.155;
			Assert::IsTrue(ev.Submit(1, o, u));
			Assert::IsFalse(u.informative);
			o.floorCodes = 0.29;
			Assert::IsTrue(ev.Submit(1, o, u));
			Assert::IsTrue(u.informative, L"the same detail passes at a 10-bit floor");
		}

		TEST_METHOD(ContradictingLinesAreMaskedBelowTwentyPercentAndHoldAbove)
		{
			using namespace ChainSynth;
			ChainEvidence ev;
			EvidenceUpdate u;
			VerifyResult few;
			few.candidates.push_back(MakeCandidate(720, -0.6, 5.0));
			for (int j = 0; j < 4; ++j)
				few.candidates[0].lineStat[static_cast<size_t>(j * 8)] = 0.0;   // 4 of 32 lines (12.5%) see no dip
			Assert::IsTrue(ev.Submit(1, Observe(few, 1080, 1, false), u));
			Assert::IsTrue(u.informative && !u.hold);
			Assert::IsTrue((u.reason & kReasonMasked) != 0 && u.cand[0].linesMasked == 4);
			Assert::AreEqual(-0.6, u.cand[0].stat, 1e-9, L"the masked lines do not pull the statistic");
			VerifyResult many;
			many.candidates.push_back(MakeCandidate(720, -0.6, 5.0));
			for (int j = 0; j < 10; ++j)
				many.candidates[0].lineStat[static_cast<size_t>(j)] = 0.0;      // 10 of 32 (31%)
			ChainEvidence ev2;
			Assert::IsTrue(ev2.Submit(1, Observe(many, 1080, 1, true), u));
			Assert::IsTrue(u.hold && !u.informative && (u.reason & kReasonMixed) != 0, L"a mixed picture is a visible HOLD hint");
			Assert::IsFalse(ev2.Summary(1).hasBest, L"a HOLD accumulates nothing");
			Assert::IsTrue(ev2.Summary(1).informativeEvals == 0);
			// The segment map along the lines: ten of sixteen segments see no dip on every line.
			VerifyResult segs;
			segs.candidates.push_back(MakeCandidate(720, -0.6, 5.0));
			for (int j = 0; j < 32; ++j)
				for (int g = 0; g < 10; ++g)
					segs.candidates[0].segStat[static_cast<size_t>(j * 16 + g)] = 0.0;
			ChainEvidence ev3;
			Assert::IsTrue(ev3.Submit(1, Observe(segs, 1080, 1, true), u));
			Assert::IsTrue(u.hold && (u.reason & kReasonMixed) != 0, L"overlay over part of the line is a HOLD hint too");
		}

		TEST_METHOD(TwoAxisAgreementAddsAPriorAndDisagreementSubtractsIt)
		{
			using namespace ChainSynth;
			VerifyResult v1;
			VerifyResult h1;
			VerifyResult hOther;
			v1.candidates.push_back(MakeCandidate(720, -1.0, 5.0));
			h1.candidates.push_back(MakeCandidate(1280, -1.0, 5.0));        // 1280/1920 = 720/1080
			hOther.candidates.push_back(MakeCandidate(960, -1.0, 5.0));     // 1/2
			EvidenceUpdate u;
			ChainEvidence agree;
			ChainEvidence disagree;
			agree.Submit(1, Observe(v1, 1080, 1, false), u);
			agree.Submit(0, Observe(h1, 1920, 1, false), u);
			disagree.Submit(1, Observe(v1, 1080, 1, false), u);
			disagree.Submit(0, Observe(hOther, 1920, 1, false), u);
			const AxisSummary a = agree.Summary(1);
			const AxisSummary d = disagree.Summary(1);
			Assert::AreEqual(2.0, a.prior, 1e-12);
			Assert::AreEqual(-2.0, d.prior, 1e-12);
			Assert::IsFalse(a.otherAxisAbstains);
			Assert::AreEqual(a.llr + 2.0, a.llrWithPrior, 1e-12);
			// Order of submission never matters.
			ChainEvidence rev;
			rev.Submit(0, Observe(h1, 1920, 1, false), u);
			rev.Submit(1, Observe(v1, 1080, 1, false), u);
			const AxisSummary r = rev.Summary(1);
			Assert::IsTrue(std::memcmp(&r.llrWithPrior, &a.llrWithPrior, sizeof(double)) == 0 && r.n == a.n);
		}

		TEST_METHOD(NativePicturesNeverCrossC1OnThePlaceholderNulls)
		{
			using namespace ChainSynth;
			DescaleVerifier verifier;
			ChainEvidence ev;
			struct Frame { double hf; uint32_t seed; int bits; double blocky; };
			const Frame frames[7] = {
				{ 10.0, 901u, 10, 0.0 }, { 5.0, 902u, 10, 0.0 }, { 2.0, 903u, 10, 0.0 }, { 10.0, 904u, 8, 0.0 },
				{ 5.0, 905u, 8, 0.0 }, { kHf, 906u, 10, 0.7 }, { kHf, 907u, 8, 0.7 } };
			uint32_t shot = 0;
			bool nativeEarned = false;
			for (int f = 0; f < 7; ++f)
			{
				++shot;
				const Axes& a = Native(frames[f].hf, frames[f].seed, frames[f].bits, frames[f].blocky);
				const double floorCodes = frames[f].bits == 8 ? 4.0 / std::sqrt(12.0) : 1.0 / std::sqrt(12.0);
				const double step = frames[f].bits == 8 ? 4.0 : 1.0;
				for (int axis = 0; axis < 2; ++axis)
				{
					const LineSet& ls = axis == 0 ? a.h : a.v;
					std::vector<int> caller;
					const int N = ls.axisLen;
					const int num[7] = { 1, 2, 1, 5, 3, 7, 5 };
					const int den[7] = { 2, 3, 3, 12, 4, 8, 8 };
					for (int k = 0; k < 7; ++k)
						caller.push_back(ChainSynth::RoundToInt(static_cast<double>(N) * num[k] / den[k]));
					const AxisRun run = Analyse(ls, caller, verifier);
					ProbeResult pr;
					PowerProbe::Run(verifier, ls, floorCodes, step, ProbeSettings(), pr);
					AxisObservation o;
					o.verify = &run.vr;
					o.axisLen = N;
					o.floorCodes = floorCodes;
					o.probeRan = pr.ran;
					o.probeSeen = pr.seen;
					o.probeReason = pr.reason;
					o.shotId = shot;
					EvidenceUpdate u;
					for (int e = 0; e < 3; ++e)
						ev.Submit(axis, o, u);
					if (pr.seen)
						nativeEarned = true;
				}
			}
			for (int axis = 0; axis < 2; ++axis)
			{
				const AxisSummary s = ev.Summary(axis);
				std::ostringstream m;
				m << "native axis " << axis << ": best n=" << s.n << " llr=" << s.llr << " c1=" << ev.GetThresholds().c1 << " native=" << s.nativeLlr;
				Logger::WriteMessage(m.str().c_str());
				Assert::IsFalse(s.aboveC1, L"a native picture never reaches the upscale threshold");
				Assert::IsTrue(s.llr < ev.GetThresholds().c1);
				if (s.hasBest)
					Assert::IsTrue(s.llr < 0.5 * ev.GetThresholds().c1, L"not even half way");
			}
			Assert::IsTrue(nativeEarned, L"at least the detailed 10-bit frames let the probe see its injection");
			Assert::IsTrue(ev.Summary(1).nativeLlr > 0.0);
		}

		TEST_METHOD(FlatFramesAbstainAndTrueChainsEarnTheirEvidence)
		{
			using namespace ChainSynth;
			DescaleVerifier verifier;
			// Flat: a constant picture with a trace of noise, built directly as lines.
			LineSet flat;
			flat.axis = 1;
			flat.axisLen = 1080;
			flat.origin = 48;
			flat.Resize(32, 984);
			ChainSynth::Lcg g(3u);
			for (int j = 0; j < 32; ++j)
				for (int i = 0; i < 984; ++i)
					flat.Line(j)[i] = std::floor(500.0 + 0.3 * g.Normal() + 0.5);
			const AxisRun run = Analyse(flat, std::vector<int>(1, 540), verifier);
			ProbeResult pr;
			Assert::IsTrue(PowerProbe::Run(verifier, flat, 1.0 / std::sqrt(12.0), 1.0, ProbeSettings(), pr));
			Assert::IsFalse(pr.seen);
			Assert::IsTrue((pr.reason & kReasonFlat) != 0 && (pr.reason & kReasonNoPower) != 0);
			ChainEvidence ev;
			AxisObservation o;
			o.verify = &run.vr;
			o.axisLen = 1080;
			o.floorCodes = 1.0 / std::sqrt(12.0);
			o.probeRan = pr.ran;
			o.probeSeen = pr.seen;
			o.probeReason = pr.reason;
			o.shotId = 1;
			EvidenceUpdate u;
			Assert::IsTrue(ev.Submit(1, o, u));
			Assert::IsFalse(u.informative, L"a flat frame abstains");
			Assert::IsFalse(ev.Summary(1).hasBest);
			// A real 10-bit chain: the evidence crosses c1 over several shots.
			const Axes& a = FullChain(540, 960, kLanczos3, 101u + 2, 10);
			const AxisRun cr = Analyse(a.v, std::vector<int>(), verifier);
			ChainEvidence ev2;
			for (uint32_t shot = 1; shot <= 4; ++shot)
			{
				AxisObservation oc;
				oc.verify = &cr.vr;
				oc.axisLen = 1080;
				oc.floorCodes = 1.0 / std::sqrt(12.0);
				oc.shotId = shot;
				for (int e = 0; e < 3; ++e)
					ev2.Submit(1, oc, u);
			}
			const AxisSummary s = ev2.Summary(1);
			Assert::IsTrue(s.hasBest && s.n == 540 && s.aboveC1, L"a decisive 10-bit chain latches its size evidence");
		}
	};

	TEST_CLASS(ChainProbeTests)
	{
	public:
		TEST_METHOD(ProbeSeesItsInjectionOnTexturedPicturesAndNamesWhyNotOtherwise)
		{
			using namespace ChainSynth;
			DescaleVerifier verifier;
			const Axes& strong10 = Native(10.0, 1001u, 10);
			ProbeResult pr;
			Assert::IsTrue(PowerProbe::Run(verifier, strong10.v, 1.0 / std::sqrt(12.0), 1.0, ProbeSettings(), pr));
			Assert::IsTrue(pr.ran && pr.seen && pr.stat < -0.15 && pr.reason == kReasonNone, L"textured 10-bit: seen");
			Assert::IsTrue(PowerProbe::Run(verifier, strong10.h, 1.0 / std::sqrt(12.0), 1.0, ProbeSettings(), pr));
			Assert::IsTrue(pr.seen, L"textured 10-bit, other axis: seen");
			const Axes& strong8 = Native(14.0, 1002u, 8);
			Assert::IsTrue(PowerProbe::Run(verifier, strong8.v, 4.0 / std::sqrt(12.0), 4.0, ProbeSettings(), pr));
			Assert::IsTrue(pr.seen, L"detailed 8-bit: seen");
			// Soft: structure, but nothing above the floor.
			LineSet soft;
			soft.axis = 1;
			soft.axisLen = 1080;
			soft.origin = 48;
			soft.Resize(32, 984);
			for (int j = 0; j < 32; ++j)
				for (int i = 0; i < 984; ++i)
					soft.Line(j)[i] = std::floor(500.0 + 100.0 * std::sin((i + 48) / 37.0) * std::cos((j * 31 + 17) / 53.0) + 0.5);
			Assert::IsTrue(PowerProbe::Run(verifier, soft, 1.0 / std::sqrt(12.0), 1.0, ProbeSettings(), pr));
			Assert::IsFalse(pr.seen);
			Assert::IsTrue((pr.reason & kReasonSoft) != 0 && (pr.reason & kReasonNoPower) != 0, L"smooth picture: soft");
			// Dark: the same smooth structure at a low level.
			LineSet dark = soft;
			for (size_t i = 0; i < dark.data.size(); ++i)
				dark.data[i] -= 440.0;
			Assert::IsTrue(PowerProbe::Run(verifier, dark, 1.0 / std::sqrt(12.0), 1.0, ProbeSettings(), pr));
			Assert::IsFalse(pr.seen);
			Assert::IsTrue((pr.reason & kReasonDark) != 0);
			// Too short for the probe: reported, never "native".
			LineSet shortLines;
			shortLines.axis = 1;
			shortLines.axisLen = 1080;
			shortLines.origin = 48;
			shortLines.Resize(8, 110);
			Assert::IsFalse(PowerProbe::Run(verifier, shortLines, 0.29, 1.0, ProbeSettings(), pr));
			Assert::IsTrue((pr.reason & kReasonNoPower) != 0 && !pr.seen);
		}

		TEST_METHOD(ProbeIsDeterministic)
		{
			using namespace ChainSynth;
			const Axes& a = Native(10.0, 1001u, 10);
			DescaleVerifier v1;
			DescaleVerifier v2;
			ProbeResult p1;
			ProbeResult p2;
			PowerProbe::Run(v1, a.v, 0.29, 1.0, ProbeSettings(), p1);
			PowerProbe::Run(v2, a.v, 0.29, 1.0, ProbeSettings(), p2);
			PowerProbe::Run(v1, a.v, 0.29, 1.0, ProbeSettings(), p2);   // warm cache
			Assert::IsTrue(std::memcmp(&p1.stat, &p2.stat, sizeof(double)) == 0 && p1.gatedLines == p2.gatedLines && p1.seen == p2.seen);
		}
	};

	TEST_CLASS(ChainTimingTests)
	{
	public:
		// Logs milliseconds per evaluation at 3840x2160; no timing assertion (the budget is measured at C2).
		TEST_METHOD(LogsTheCostOfOneEvaluationAtUltraHd)
		{
			using namespace ChainSynth;
			using Clock = std::chrono::steady_clock;
			Image latent = Scene(1080, 1920, kHf, 1101u, kGrain);
			Image up = ResizeAxis(ResizeAxis(latent, 0, 2160, kLanczos3), 1, 3840, kLanczos3);
			Quantize(up, 10);
			Axes ax = MakeAxes(up, MakeRect(0, 0, 3840, 2160), 1);
			Assert::IsTrue(ax.ok);
			DescaleVerifier verifier;
			PolyphaseProposer prop[2];
			ChainEvidence ev;
			double totalMs = 0.0;
			for (int pass = 0; pass < 2; ++pass)
			{
				std::ostringstream m;
				m << "pass " << (pass == 0 ? "cold" : "warm") << ":";
				for (int axis = 0; axis < 2; ++axis)
				{
					const LineSet& ls = axis == 0 ? ax.h : ax.v;
					const Clock::time_point t0 = Clock::now();
					prop[axis].AddEvaluation(ls);
					ProposedCandidate pc[kMaxCandidates];
					const int caller[1] = { ls.axisLen / 2 };
					const int np = prop[axis].Propose(ls.axisLen, caller, 1, pc, kMaxCandidates);
					const Clock::time_point t1 = Clock::now();
					std::vector<int> cand;
					for (int i = 0; i < np; ++i)
						cand.push_back(pc[i].n);
					VerifyResult vr;
					verifier.Evaluate(ls, cand.data(), static_cast<int>(cand.size()), VerifyOptions(), vr);
					const Clock::time_point t2 = Clock::now();
					ProbeResult pr;
					PowerProbe::Run(verifier, ls, 0.29, 1.0, ProbeSettings(), pr);
					const Clock::time_point t3 = Clock::now();
					AxisObservation o;
					o.verify = &vr;
					o.axisLen = ls.axisLen;
					o.floorCodes = 0.29;
					o.probeRan = pr.ran;
					o.probeSeen = pr.seen;
					o.shotId = 1;
					EvidenceUpdate u;
					ev.Submit(axis, o, u);
					const Clock::time_point t4 = Clock::now();
					auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
					m << " axis" << axis << "(proposer " << ms(t0, t1) << " ms, verifier " << ms(t1, t2) << " ms for " << cand.size()
						<< " candidates, probe " << ms(t2, t3) << " ms, evidence " << ms(t3, t4) << " ms)";
					totalMs += ms(t0, t4);
				}
				Logger::WriteMessage(m.str().c_str());
			}
			std::ostringstream tot;
			tot << "total of both passes: " << totalMs << " ms; cached operators: " << verifier.CachedOperators();
			Logger::WriteMessage(tot.str().c_str());
		}
	};
}
