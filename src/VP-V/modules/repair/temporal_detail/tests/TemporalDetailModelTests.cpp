// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail CPU model tests (README.md): synthetic scenes with exact vectors, a coding simulation
// (an I-like frame keeps the master's fine band, a B-like frame attenuates it), fresh grain per frame.
// CPU only, small planes, no renderer fixtures.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/repair/temporal_detail/TemporalDetail.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailModel.h>
#include <VP-V/core/Ramp.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailMotion.h>
#include <VP-V/core/Products.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailShaders.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	using namespace TemporalDetail;

	constexpr int kW = 128, kH = 96;
	constexpr double kHz = 23.976;
	constexpr double kPi = 3.14159265358979323846;

	void Log(const char* text) { Logger::WriteMessage(text); }
	void LogF(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0)
	{
		char line[256];
		std::snprintf(line, sizeof line, fmt, a, b, c, d, e);
		Logger::WriteMessage(line);
	}

	// --- Synthetic master: low-frequency level, textured except in flat windows (32 px, period 64: the grain-level
	// estimate needs quiet areas within 80 px, as real pictures have), soft edges ---
	double Pulse(double t)
	{
		const double m = t - 64.0 * std::floor(t / 64.0);
		return 1.0 - RampMath::Smoothstep<double>(13.0, 16.0, std::fabs(m - 32.0), RampMath::SmoothRule::Unguarded);
	}

	double Master(double x, double y, double ox = 0.0, double oy = 0.0)
	{
		x += ox; y += oy;
		struct S { double fx, fy, ph, a; };
		static const S fine[] = { { 1 / 4.5, 0.05, 0.3, 10 }, { 0.04, 1 / 5.3, 1.1, 9 }, { 0.7 / 6.1, 0.7 / 6.1, 2.0, 8 } };
		static const S mid[] = { { 1 / 11.0, 0.02, 0.7, 14 }, { 0.03, 1 / 14.0, 2.5, 12 }, { 0.7 / 18.0, -0.7 / 18.0, 1.3, 10 } };
		const double env = 1.0 - Pulse(x) * Pulse(y);
		double t = 0.0;
		for (const S& s : fine) t += s.a * std::sin(2 * kPi * (s.fx * x + s.fy * y) + s.ph);
		for (const S& s : mid) t += s.a * std::sin(2 * kPi * (s.fx * x + s.fy * y) + s.ph);
		const double level = 110.0 + 25.0 * std::sin(2 * kPi * x / 97.0 + 0.5) * std::cos(2 * kPi * y / 131.0);
		const double edges = 14.0 * std::tanh((x * 0.8 + y * 0.6 - 40.0) / 1.0) + 10.0 * std::tanh((x - y * 0.7 - 140.0) / 1.3);
		return level + env * t + edges;
	}

	Plane Sample(double dx, double dy, double ox = 0.0, double oy = 0.0, int w = kW, int h = kH)
	{
		Plane p(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x) p.at(x, y) = Master(x - dx, y - dy, ox, oy);
		return p;
	}

	// The coding simulation: Q = 1 keeps the fine band, smaller Q attenuates it (edges and the mid band stay).
	Plane Code(const Plane& truth, const std::function<double(int, int)>& q)
	{
		const Plane g1 = Detail::Gauss(truth, 1.0);
		Plane o(truth.w, truth.h);
		for (int y = 0; y < truth.h; ++y)
			for (int x = 0; x < truth.w; ++x)
				o.at(x, y) = truth.at(x, y) - (1.0 - q(x, y)) * (truth.at(x, y) - g1.at(x, y));
		return o;
	}
	Plane Code(const Plane& truth, double q)
	{
		if (q >= 1.0) return truth;
		return Code(truth, [q](int, int) { return q; });
	}

	// Deterministic normal noise (Box-Muller on mt19937 integers: no library distribution).
	struct Noise
	{
		std::mt19937 g;
		explicit Noise(uint32_t seed) : g(seed) {}
		double Uniform() { return (static_cast<double>(g()) + 0.5) / 4294967296.0; }
		double Next() { return std::sqrt(-2.0 * std::log(Uniform())) * std::cos(2.0 * kPi * Uniform()); }
	};
	void AddGrain(Plane& p, Noise& n, double sigma)
	{
		for (double& v : p.v) v += sigma * n.Next();
	}

	// Coding qualities: I key, P good, B soft, b half.
	double QualityOf(char c)
	{
		switch (c) { case 'I': return 1.0; case 'P': return 0.8; case 'B': return 0.3; case 'b': return 0.55; default: return 1.0; }
	}
	std::vector<double> Pattern(const char* s, int n)
	{
		const size_t len = std::strlen(s);
		std::vector<double> q;
		for (int i = 0; i < n; ++i) q.push_back(QualityOf(s[static_cast<size_t>(i) % len]));
		return q;
	}
	// Reference-quality frames at irregular gaps (e.g. 3, 7, 2, 11, 5): I at every gap boundary, B between.
	std::vector<double> Irregular(const std::vector<int>& gaps, int n)
	{
		std::vector<double> q(static_cast<size_t>(n), 0.3);
		int pos = 0;
		for (size_t i = 0; pos < n; ++i)
		{
			q[static_cast<size_t>(pos)] = 1.0;
			pos += gaps[i % gaps.size()];
		}
		return q;
	}

	// --- Metrics (interior, 8 px margin) ---
	double Mse(const Plane& a, const Plane& b, int x0 = 8, int y0 = 8, int x1 = kW - 8, int y1 = kH - 8)
	{
		double s = 0.0;
		int n = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) { const double d = a.at(x, y) - b.at(x, y); s += d * d; ++n; }
		return s / n;
	}
	double FineEnergy(const Plane& p, int x0 = 8, int y0 = 8, int x1 = kW - 8, int y1 = kH - 8)
	{
		const Plane g = Detail::Gauss(p, 1.0);
		double s = 0.0;
		int n = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) { const double d = p.at(x, y) - g.at(x, y); s += d * d; ++n; }
		return s / n;
	}
	Plane Band(const Plane& p, double sigma)   // p - G(sigma) p
	{
		const Plane g = Detail::Gauss(p, sigma);
		Plane o(p.w, p.h);
		for (size_t i = 0; i < p.v.size(); ++i) o.v[i] = p.v[i] - g.v[i];
		return o;
	}
	double Corr(const Plane& a, const Plane& b, int x0 = 8, int y0 = 8, int x1 = kW - 8, int y1 = kH - 8)
	{
		double sa = 0, sb = 0, sab = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) { sa += a.at(x, y) * a.at(x, y); sb += b.at(x, y) * b.at(x, y); sab += a.at(x, y) * b.at(x, y); }
		return sab / std::sqrt(sa * sb + 1e-12);
	}
	double Mean(const std::vector<double>& v, size_t from, size_t to)
	{
		double s = 0.0;
		for (size_t i = from; i < to; ++i) s += v[i];
		return s / static_cast<double>(to - from);
	}
	double Variance(const std::vector<double>& v, size_t from, size_t to)
	{
		const double m = Mean(v, from, to);
		double s = 0.0;
		for (size_t i = from; i < to; ++i) s += (v[i] - m) * (v[i] - m);
		return s / static_cast<double>(to - from);
	}
	double PsnrOf(double mse) { return 10.0 * std::log10(255.0 * 255.0 / (mse + 1e-12)); }
	bool Same(const Plane& a, const Plane& b) { return a.w == b.w && a.h == b.h && a.v.size() == b.v.size() && std::memcmp(a.v.data(), b.v.data(), a.v.size() * sizeof(double)) == 0; }

	// --- Streams and runs ---
	struct Stream
	{
		std::vector<Plane> truth, frames, grain;   // grain: the exact grain field added to each frame (white, sigma)
	};
	// truthAt(t): the uncoded frame; qualityAt(t, x, y): its coding quality.
	Stream Build(int n, const std::function<Plane(int)>& truthAt, const std::function<double(int, int, int)>& qualityAt, uint32_t seed = 7, double grain = 1.2)
	{
		Stream s;
		Noise noise(seed);
		for (int t = 0; t < n; ++t)
		{
			s.truth.push_back(truthAt(t));
			Plane f = Code(s.truth.back(), [&](int x, int y) { return qualityAt(t, x, y); });
			const Plane clean = f;
			AddGrain(f, noise, grain);
			Plane g(f.w, f.h);
			for (size_t i = 0; i < g.v.size(); ++i) g.v[i] = f.v[i] - clean.v[i];
			s.grain.push_back(std::move(g));
			s.frames.push_back(std::move(f));
		}
		return s;
	}
	Stream BuildPan(const std::vector<double>& q, double panX, double panY = 0.0, uint32_t seed = 7, double grain = 1.2)
	{
		return Build(static_cast<int>(q.size()), [&](int t) { return Sample(panX * t, panY * t); },
			[&](int t, int, int) { return q[static_cast<size_t>(t)]; }, seed, grain);
	}

	void SetPan(FieldMotionSource& m, int n, double px, double py)
	{
		for (int t = 0; t < n; ++t)
		{
			m.SetBackward(static_cast<uint64_t>(t), MotionPair::Uniform(kW, kH, 16.0, -px, -py));
			m.SetForward(static_cast<uint64_t>(t), MotionPair::Uniform(kW, kH, 16.0, px, py));
		}
	}

	struct Run
	{
		std::vector<Plane> out;
		std::vector<Diagnostics> diag;
	};
	using Hook = std::function<void(int, Model::FrameIn&)>;
	Run Go(Model& model, const std::vector<Plane>& frames, const IMotionSource* motion, double hz = kHz, double planeLines = 1080.0, int future = 0, const Hook& hook = nullptr)
	{
		Run r;
		for (size_t t = 0; t < frames.size(); ++t)
		{
			Model::FrameIn in;
			in.luma = frames[t];
			in.seq = t;
			in.contentHz = hz;
			in.planeLines = planeLines;
			in.motion = motion;
			for (int k = 1; k <= future && t + static_cast<size_t>(k) < frames.size(); ++k)
			{
				Model::FutureFrame ff;
				ff.luma = &frames[t + static_cast<size_t>(k)];
				ff.seq = t + static_cast<size_t>(k);
				in.future.push_back(ff);
			}
			if (hook) hook(static_cast<int>(t), in);
			r.out.push_back(model.Process(in));
			r.diag.push_back(model.Diag());
		}
		return r;
	}
	// The designed behaviour these tests measure includes the fallback boost; the product default has it off (owner
	// 2026-10-31, TemporalDetailBoostAndAutoDefaults covers the default).
	Settings Steady()
	{
		Settings s;
		s.mode = Mode::Steady;
		s.boost = true;
		return s;
	}
	Run GoSteady(const std::vector<Plane>& frames, const IMotionSource* motion, const Settings& s = Steady(), int future = 0, double hz = kHz)
	{
		Model m(s);
		return Go(m, frames, motion, hz, 1080.0, future);
	}

	std::vector<double> Series(const Run& r, const std::vector<Plane>& ref, int from, int to, const std::function<double(const Plane&, const Plane&)>& f)
	{
		std::vector<double> v;
		for (int t = from; t < to; ++t) v.push_back(f(r.out[static_cast<size_t>(t)], ref[static_cast<size_t>(t)]));
		return v;
	}

	// --- The owner's grain metrics on a GRAIN ISOLATE ---
	// The same truth and coding with two independent grain seeds a, b: (frames_a - frames_b) is pure input grain and
	// (out_a - out_b) is pure output grain plus any seed-dependent change of the transfer (counted against the module).
	// Restored structure and the steadied I/B pulse are common to both runs and cancel, so they are not mistaken for
	// frozen grain (the raw "output minus the clean coded frame" residual counts restored structure as frozen grain).
	struct GrainMetrics
	{
		double rmsRatio = 0, lag1Off = 0, lag1On = 0, bands[4] = { 0, 0, 0, 0 };
		double Increase() const { return lag1On - lag1Off; }
		bool Holds() const
		{
			bool ok = std::fabs(rmsRatio - 1.0) <= 0.05 && Increase() <= 0.05;
			for (double b : bands) ok = ok && b >= 0.95 && b <= 1.05;
			return ok;
		}
	};
	GrainMetrics GrainIsolate(const Stream& a, const Stream& b, const Run& ra, const Run& rb, int t0, int n, int tx0)
	{
		GrainMetrics m;
		std::vector<Plane> gi, go;
		double eIn = 0, eOut = 0, cIn = 0, cOut = 0;
		for (int t = t0; t < n; ++t)
		{
			const size_t u = static_cast<size_t>(t);
			Plane di(kW, kH), dd(kW, kH);
			for (size_t i = 0; i < di.v.size(); ++i) { di.v[i] = a.frames[u].v[i] - b.frames[u].v[i]; dd.v[i] = ra.out[u].v[i] - rb.out[u].v[i]; }
			gi.push_back(Band(di, 1.0)); go.push_back(Band(dd, 1.0));
			for (int y = 8; y < kH - 8; ++y)
				for (int x = tx0; x < kW - 8; ++x) { eIn += gi.back().at(x, y) * gi.back().at(x, y); eOut += go.back().at(x, y) * go.back().at(x, y); }
		}
		for (size_t t = 1; t < gi.size(); ++t)
			for (int y = 8; y < kH - 8; ++y)
				for (int x = tx0; x < kW - 8; ++x) { cIn += gi[t].at(x, y) * gi[t - 1].at(x, y); cOut += go[t].at(x, y) * go[t - 1].at(x, y); }
		m.rmsRatio = std::sqrt(eOut / eIn);
		m.lag1Off = cIn / eIn * static_cast<double>(gi.size()) / static_cast<double>(gi.size() - 1);
		m.lag1On = cOut / eOut * static_cast<double>(go.size()) / static_cast<double>(go.size() - 1);
		// whole-field per-pixel temporal spectrum of the isolate, 4 bands (DFT over the window)
		const int N = n - t0;
		double bi[4] = { 0, 0, 0, 0 }, bo[4] = { 0, 0, 0, 0 };
		for (int y = 8; y < kH - 8; y += 3)
			for (int x = 8; x < kW - 8; x += 3)
				for (int k = 0; k < 2; ++k)
				{
					std::vector<double> v(static_cast<size_t>(N));
					double mean = 0;
					for (int j = 0; j < N; ++j)
					{
						const size_t u = static_cast<size_t>(t0 + j);
						mean += v[static_cast<size_t>(j)] = k ? ra.out[u].at(x, y) - rb.out[u].at(x, y) : a.frames[u].at(x, y) - b.frames[u].at(x, y);
					}
					mean /= N;
					for (int f = 1; f <= N / 2; ++f)
					{
						double re = 0, im = 0;
						for (int j = 0; j < N; ++j)
						{
							const double ang = 2 * kPi * f * j / N;
							re += (v[static_cast<size_t>(j)] - mean) * std::cos(ang);
							im += (v[static_cast<size_t>(j)] - mean) * std::sin(ang);
						}
						(k ? bo : bi)[(std::min)(3, (f - 1) * 4 / (N / 2))] += re * re + im * im;
					}
				}
		for (int q = 0; q < 4; ++q) m.bands[q] = bo[q] / bi[q];
		return m;
	}
	void LogGrain(const char* what, const GrainMetrics& m)
	{
		LogF(what);
		LogF("  textured grain RMS on/off %.4f; lag-1 off %.4f on %.4f (increase %.4f)", m.rmsRatio, m.lag1Off, m.lag1On, m.Increase());
		LogF("  whole-field grain temporal bands on/off %.4f %.4f %.4f %.4f", m.bands[0], m.bands[1], m.bands[2], m.bands[3]);
	}
	// The KeepsGrain scene: left half flat, right half textured, IBBBPBBB, static.
	Stream KeepsGrainScene(uint32_t seed, double sigma, int n)
	{
		const std::vector<double> q = Pattern("IBBBPBBB", n);
		return Build(n, [](int)
			{
				Plane p = Sample(0, 0);
				for (int y = 0; y < kH; ++y)
					for (int x = 0; x < kW / 2; ++x) p.at(x, y) = 100.0 + 0.1 * y;
				return p;
			}, [q](int t, int, int) { return q[static_cast<size_t>(t)]; }, seed, sigma);
	}
	Run GoTuned(const std::vector<Plane>& frames, const IMotionSource* motion, const Model::Tuning& tune)
	{
		Model m(Steady());
		m.SetTuning(tune);
		return Go(m, frames, motion);
	}
	// --- TD-NONFINITE tests: access to Model's private members by explicit instantiation (the language's sanctioned way past
	// `private`: no macro, no production change). EvaluateBlock, the stored sources, the per-block state, the per-block gate
	// factors and the plane size are the seams the non-finite tests plant values through. ---
	template <class Tag, typename Tag::type M> struct Rob { friend typename Tag::type Grab(Tag) { return M; } };
	struct EvalTag
	{
		using type = Detail::Eval (Model::*)(int, int, const Detail::Analysis&, const Detail::Source&, bool, double) const;
		friend type Grab(EvalTag);
	};
	struct SlotsTag { using type = std::vector<Detail::Source> Model::*; friend type Grab(SlotsTag); };
	struct BlocksTag { using type = std::vector<Detail::BlockState> Model::*; friend type Grab(BlocksTag); };
	struct GatesTag { using type = std::vector<float> Model::*; friend type Grab(GatesTag); };
	struct WidthTag { using type = int Model::*; friend type Grab(WidthTag); };
	struct HeightTag { using type = int Model::*; friend type Grab(HeightTag); };
	template struct Rob<EvalTag, &Model::EvaluateBlock>;
	template struct Rob<SlotsTag, &Model::m_slots>;
	template struct Rob<BlocksTag, &Model::m_blocks>;
	template struct Rob<GatesTag, &Model::m_blockGates>;
	template struct Rob<WidthTag, &Model::m_w>;
	template struct Rob<HeightTag, &Model::m_h>;

	// One block's validation: a real analysis of the synthetic master, the same frame as the stored source (identity motion),
	// with clean block maps (N = 1, F = 0.5). Every gate of block (0, 0) is then open except the soft "stronger" gate, so a
	// planted value that closes a gate is the planted value's doing.
	struct EvalFixture
	{
		Model model;
		Detail::Analysis A;
		Detail::Source src;
		EvalFixture() : model(Steady())
		{
			(model.*Grab(WidthTag{})) = kW;
			(model.*Grab(HeightTag{})) = kH;
			A = Detail::Analyse(Sample(0, 0), 1.0, 16);
			const size_t nb = static_cast<size_t>(A.bw) * static_cast<size_t>(A.bh);
			for (size_t i = 0; i < nb; ++i) { A.N[i] = 1.0; A.F[i] = 0.5; }
			src.id = 0; src.seq = 0; src.future = false;
			src.s1 = A.s1; src.s2 = A.s2; src.s = A.s; src.base = A.base; src.grain = A.grain;
			src.N = A.N; src.F = A.F; src.q = A.q;
			src.cx.assign(nb, 0.0); src.cy.assign(nb, 0.0); src.u.assign(nb, 0.0);
		}
		Detail::Eval Eval() const { return (model.*Grab(EvalTag{}))(0, 0, A, src, false, kHz); }
	};

	std::wstring WideOf(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	bool AllFinite(const std::vector<double>& v)
	{
		for (double x : v) if (!std::isfinite(x)) return false;
		return true;
	}

}

namespace VideoProcessorTest
{
	// Model-only tests of repair.temporal_detail (README.md); the plan: tests/README.md.
	TEST_CLASS(TemporalDetailModelTests)
	{
	public:

		TEST_METHOD(TemporalDetailOffIsByteIdentical)
		{
			const Stream s = BuildPan(Pattern("IBBBPBBB", 24), 0.0);
			FieldMotionSource motion;
			SetPan(motion, 24, 0.0, 0.0);
			Settings off;   // default Off
			Model model(off);
			const Run r = Go(model, s.frames, &motion);
			for (size_t t = 0; t < s.frames.size(); ++t)
				Assert::IsTrue(Same(r.out[t], s.frames[t]), L"Off: the output is the input, bit for bit");
			Assert::AreEqual(std::string("off"), model.Diag().status, L"Off reports itself");
			Assert::AreEqual(0, model.Diag().frames, L"Off touches no state");

			// Steady on a frame with no reference: nothing to transfer, so the input comes back bit for bit.
			Model steady(Steady());
			Model::FrameIn in;
			in.luma = s.frames[0];
			in.seq = 0;
			in.contentHz = kHz;
			in.motion = &motion;
			Assert::IsTrue(Same(steady.Process(in), s.frames[0]), L"Steady, first frame (no reference): bit-identical");
			// A later frame with nothing valid to transfer (flat noise, no pulse yet) stays bit-identical too.
			Model::FrameIn in2 = in;
			in2.luma = s.frames[1];
			in2.seq = 1;
			Assert::IsTrue(Same(steady.Process(in2), s.frames[1]), L"Steady, no pulse evidence yet: bit-identical");
		}

		TEST_METHOD(TemporalDetailSteadiesIBPulse)
		{
			const int n = 56;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			double mseOff = 0, mseOn = 0;
			int cnt = 0;
			std::vector<double> eOff, eOn;
			for (int t = 24; t < n; ++t)
			{
				eOff.push_back(FineEnergy(s.frames[static_cast<size_t>(t)]));
				eOn.push_back(FineEnergy(on.out[static_cast<size_t>(t)]));
				if (t % 8 == 0 || t % 8 == 4) continue;   // B frames only
				mseOff += Mse(s.frames[static_cast<size_t>(t)], s.truth[static_cast<size_t>(t)]);
				mseOn += Mse(on.out[static_cast<size_t>(t)], s.truth[static_cast<size_t>(t)]);
				++cnt;
			}
			mseOff /= cnt; mseOn /= cnt;
			const double gain = PsnrOf(mseOn) - PsnrOf(mseOff);
			const double vOff = Variance(eOff, 0, eOff.size()), vOn = Variance(eOn, 0, eOn.size());
			LogF("B-frame PSNR off %.2f dB, on %.2f dB (gain %.2f)", PsnrOf(mseOff), PsnrOf(mseOn), gain);
			LogF("fine-structure energy variance off %.3f, on %.3f (ratio %.3f)", vOff, vOn, vOn / vOff);
			Assert::IsTrue(gain >= 1.0, L"B-frame PSNR against the master improves by >= 1 dB");
			// The transfer runs under the template grain budget (kGrainBudget): the owner's grain bounds come first, so the
			// pulse is steadied less than at the designed strength (TemporalDetailGainIsStructure logs both).
			Assert::IsTrue(vOn <= 0.75 * vOff, L"the I/B fine-structure energy variance drops by >= 25%");
		}

		TEST_METHOD(TemporalDetailActivePictureKeepsBars)
		{
			// Letterbox: the top kBar rows are a black bar (with a little coding noise); the picture below pulses I/B.
			constexpr int n = 40;
			static constexpr int kBar = 16;   // static: usable in the capture-less hooks
			Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			const std::vector<Plane> noBar = s.frames;   // the same picture without the bar (baseline for the band)
			Noise noise(99);
			for (Plane& f : s.frames)
				for (int y = 0; y < kBar; ++y)
					for (int x = 0; x < kW; ++x) f.at(x, y) = 16.0 + 0.3 * noise.Next();
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			auto whole = [](int, Model::FrameIn& in) { in.active.left = 0; in.active.top = 0; in.active.right = kW; in.active.bottom = kH; };
			auto box = [](int, Model::FrameIn& in) { in.active.left = 0; in.active.top = kBar; in.active.right = kW; in.active.bottom = kH; };
			Model plain(Steady()), explicitWhole(Steady()), boxed(Steady());
			Settings so = Steady();
			so.boostMax = 0.0;
			Model boxedNoBoost(so), noBarNoBoost(so);
			const Run a = Go(plain, s.frames, &motion);
			const Run b = Go(explicitWhole, s.frames, &motion, kHz, 1080.0, 0, whole);
			const Run c = Go(boxed, s.frames, &motion, kHz, 1080.0, 0, box);
			const Run d = Go(boxedNoBoost, s.frames, &motion, kHz, 1080.0, 0, box);
			const Run e = Go(noBarNoBoost, noBar, &motion);   // the picture alone: the band is interior there
			double barMax = 0.0, mseOff = 0.0, mseOn = 0.0, under = 0.0, underNoBar = 0.0;
			int cnt = 0;
			for (int f = 0; f < n; ++f)
			{
				const size_t u = static_cast<size_t>(f);
				Assert::IsTrue(Same(a.out[u], b.out[u]), L"an explicit whole-plane rectangle equals no rectangle (bit-identical)");
				for (int y = 0; y < kBar; ++y)
					for (int x = 0; x < kW; ++x)
					{
						barMax = (std::max)(barMax, std::fabs(c.out[u].at(x, y) - s.frames[u].at(x, y)));
						barMax = (std::max)(barMax, std::fabs(d.out[u].at(x, y) - s.frames[u].at(x, y)));
					}
				// boundary band (the first 16 picture rows, transfer only): the dip below the input's local 5 x 5 minimum
				// (restored detail legitimately leaves the soft B-frame's range) is no larger than with no bar at all
				for (int y = kBar; y < kBar + 16; ++y)
					for (int x = 2; x < kW - 2; ++x)
					{
						double lo = 1e9;
						for (int yy = (std::max)(kBar, y - 2); yy <= y + 2; ++yy)
							for (int xx = x - 2; xx <= x + 2; ++xx) lo = (std::min)(lo, s.frames[u].at(xx, yy));
						under = (std::max)(under, lo - d.out[u].at(x, y));
						underNoBar = (std::max)(underNoBar, lo - e.out[u].at(x, y));
					}
				if (f >= 16 && f % 8 != 0 && f % 8 != 4)
				{
					mseOff += Mse(s.frames[u], s.truth[u], 8, kBar + 8);
					mseOn += Mse(c.out[u], s.truth[u], 8, kBar + 8);
					++cnt;
				}
			}
			const double gain = PsnrOf(mseOn / cnt) - PsnrOf(mseOff / cnt);
			LogF("letterbox: bar change max %.6f, band undershoot (boost off) %.3f (same picture, no bar %.3f), picture B-frame gain %.2f dB",
				barMax, under, underNoBar, gain);
			Assert::IsTrue(barMax == 0.0, L"bar pixels are byte-identical");
			Assert::IsTrue(under <= underNoBar + 1.0, L"no extra undershoot at the picture/bar boundary (transfer only) vs the same picture without a bar");
			Assert::IsTrue(gain >= 1.0, L"the picture below the bar still gains >= 1 dB on B-frames");
		}

		TEST_METHOD(TemporalDetailActiveRectangleChangeResets)
		{
			const int n = 12;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			Model m(Steady());
			const Run r = Go(m, s.frames, &motion, kHz, 1080.0, 0, [](int t, Model::FrameIn& in) { in.active.left = 0; in.active.top = t < 6 ? 0 : 8; in.active.right = kW; in.active.bottom = kH; });
			Assert::IsTrue(r.diag[6].resets == r.diag[5].resets + 1, L"a new active rectangle resets the store (once)");
			Assert::IsTrue(r.diag[7].resets == r.diag[6].resets, L"an unchanged rectangle does not reset");
		}

		TEST_METHOD(TemporalDetailSubPixelPanAligns)
		{
			const double pans[2] = { 0.25, 0.37 };
			for (double pan : pans)
			{
				const int n = 48;
				const Stream s = BuildPan(Pattern("IBBBPBBB", n), pan);
				FieldMotionSource motion;
				SetPan(motion, n, pan, 0.0);
				const Run on = GoSteady(s.frames, &motion);
				double mseOff = 0, mseOn = 0, cOff = 0, cOn = 0;
				int cnt = 0;
				for (int t = 24; t < n; ++t)
				{
					if (t % 8 == 0 || t % 8 == 4) continue;
					const size_t u = static_cast<size_t>(t);
					mseOff += Mse(s.frames[u], s.truth[u]);
					mseOn += Mse(on.out[u], s.truth[u]);
					const Plane tb = Band(s.truth[u], 2.5);
					cOff += Corr(Band(s.frames[u], 2.5), tb);
					cOn += Corr(Band(on.out[u], 2.5), tb);
					++cnt;
				}
				mseOff /= cnt; mseOn /= cnt; cOff /= cnt; cOn /= cnt;
				LogF("pan %.2f: B-frame MSE off %.3f on %.3f; structure correlation with the master off %.4f on %.4f", pan, mseOff, mseOn, cOff, cOn);
				Assert::IsTrue(mseOn < mseOff * 0.9, L"sub-pixel pan: B-frame error against the master drops");
				Assert::IsTrue(cOn >= cOff, L"no doubling: the output structure correlates with the master at least as well as the input");
			}
		}

		TEST_METHOD(TemporalDetailFocusPullToSoftNoTransfer)
		{
			const int n = 80;
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const Stream s = Build(n, [](int t)
				{
					const double sigma = t < 24 ? 0.0 : (std::min)(3.0, 3.0 * (t - 24) / 24.0);
					const Plane m = Sample(0, 0);
					return sigma > 0.0 ? Detail::Gauss(m, sigma) : m;
				}, [&](int t, int, int) { return q[static_cast<size_t>(t)]; });
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			std::vector<double> eOff, eOn;
			for (int t = 56; t < n; ++t) { eOff.push_back(FineEnergy(s.frames[static_cast<size_t>(t)])); eOn.push_back(FineEnergy(on.out[static_cast<size_t>(t)])); }
			const double mOff = Mean(eOff, 0, eOff.size()), mOn = Mean(eOn, 0, eOn.size());
			LogF("softened frames: fine energy off %.4f on %.4f (ratio %.4f); retires %.0f", mOff, mOn, mOn / mOff, static_cast<double>(on.diag.back().retires));
			Assert::IsTrue(mOn <= mOff * 1.02, L"at softened frames the output fine-structure energy is <= off x 1.02");
			Assert::IsTrue(on.diag.back().retires > 0, L"the diagnostics show the reference retired");
			// During the pull itself nothing sharper than the picture is imposed either.
			for (int t = 36; t < 56; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				Assert::IsTrue(FineEnergy(on.out[u]) <= FineEnergy(s.frames[u]) * 1.10 + 0.05, L"during the pull no stale sharp structure is brought back");
			}
		}

		TEST_METHOD(TemporalDetailFocusPullToSharpFollows)
		{
			const int n = 80;
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const Stream s = Build(n, [](int t)
				{
					const double sigma = t < 24 ? 3.0 : (std::max)(0.0, 3.0 - 3.0 * (t - 24) / 24.0);
					const Plane m = Sample(0, 0);
					return sigma > 0.0 ? Detail::Gauss(m, sigma) : m;
				}, [&](int t, int, int) { return q[static_cast<size_t>(t)]; });
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			double worstSoftening = 1e9, worstRel = 0.0;
			for (int t = 30; t < n; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				worstSoftening = (std::min)(worstSoftening, FineEnergy(on.out[u]) / FineEnergy(s.frames[u]));
				worstRel = (std::max)(worstRel, Mse(on.out[u], s.truth[u]) / (Mse(s.frames[u], s.truth[u]) + 1e-9));
			}
			LogF("soft -> sharp: lowest fine energy out/in %.3f, worst MSE(out)/MSE(in) vs truth %.3f, refreshes %.0f", worstSoftening, worstRel, static_cast<double>(on.diag.back().refreshes));
			Assert::IsTrue(worstSoftening >= 0.97, L"the output follows the sharpening input: never softer than it");
			Assert::IsTrue(worstRel <= 1.05, L"the old soft reference is never imposed on sharpening frames");
			Assert::IsTrue(on.diag.back().refreshes > 4, L"references refresh to the sharper frames");
		}

		TEST_METHOD(TemporalDetailRackFocusPerRegion)
		{
			const int n = 80;
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const Stream s = Build(n, [](int t)
				{
					const double sl = t < 24 ? 0.0 : (std::min)(3.0, 3.0 * (t - 24) / 24.0);
					const double sr = t < 24 ? 3.0 : (std::max)(0.0, 3.0 - 3.0 * (t - 24) / 24.0);
					const Plane m = Sample(0, 0);
					const Plane L = sl > 0.0 ? Detail::Gauss(m, sl) : m;
					const Plane R = sr > 0.0 ? Detail::Gauss(m, sr) : m;
					Plane o = m;
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x) o.at(x, y) = x < kW / 2 ? L.at(x, y) : R.at(x, y);
					return o;
				}, [&](int t, int, int) { return q[static_cast<size_t>(t)]; });
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			double eOff = 0, eOn = 0, mseL = 0, mseLon = 0, mseR = 0, mseRon = 0;
			int cnt = 0;
			for (int t = 60; t < n; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				eOff += FineEnergy(s.frames[u], 8, 8, 56, kH - 8);
				eOn += FineEnergy(on.out[u], 8, 8, 56, kH - 8);
				mseL += Mse(s.frames[u], s.truth[u], 8, 8, 56, kH - 8); mseLon += Mse(on.out[u], s.truth[u], 8, 8, 56, kH - 8);
				mseR += Mse(s.frames[u], s.truth[u], 72, 8, kW - 8, kH - 8); mseRon += Mse(on.out[u], s.truth[u], 72, 8, kW - 8, kH - 8);
				++cnt;
			}
			LogF("rack focus: left (defocused) fine energy off %.4f on %.4f; right MSE off %.3f on %.3f", eOff / cnt, eOn / cnt, mseR / cnt, mseRon / cnt);
			Assert::IsTrue(eOn <= eOff * 1.05, L"the defocused half does not exceed the input's fine energy");
			Assert::IsTrue(mseLon <= mseL * 1.05, L"the defocused half is not pushed away from its truth");
			Assert::IsTrue(mseRon <= mseR * 1.05, L"the sharpening half is not made worse");
		}

		TEST_METHOD(TemporalDetailMotionBlurNoTransfer)
		{
			const int n = 64;
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const Stream s = Build(n, [](int t)
				{
					const Plane m = Sample(0, 0);
					if (t < 30) return m;
					Plane o = m;
					const int r = 10;
					for (int y = 40; y < 80; ++y)
						for (int x = 40; x < 88; ++x)
						{
							double a = 0.0, wsum = 0.0;
							for (int i = -r; i <= r; ++i)
							{
								const double g = std::exp(-0.5 * i * i / (3.5 * 3.5));
								a += g * m.get(x + i, y);
								wsum += g;
							}
							o.at(x, y) = a / wsum;
						}
					return o;
				}, [&](int t, int, int) { return q[static_cast<size_t>(t)]; });
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			double eOff = 0, eOn = 0;
			int cnt = 0;
			for (int t = 44; t < n; ++t)
			{
				eOff += FineEnergy(s.frames[static_cast<size_t>(t)], 56, 56, 72, 72);
				eOn += FineEnergy(on.out[static_cast<size_t>(t)], 56, 56, 72, 72);
				++cnt;
			}
			LogF("motion blur region (core, clear of the half-block that bilinear weights reach): fine energy off %.4f on %.4f (ratio %.4f)", eOff / cnt, eOn / cnt, eOn / eOff);
			Assert::IsTrue(eOn <= eOff * 1.03, L"no transfer onto the motion-blurred region");
		}

		TEST_METHOD(TemporalDetailOcclusionNoGhost)
		{
			const int n = 64;
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const double speed = 2.0;
			const int ow = 40, oy0 = 28, oh = 40;
			auto objX = [&](int t) { return 10.0 + speed * t; };
			const Stream s = Build(n, [&](int t)
				{
					Plane p(kW, kH);
					const double x0 = objX(t);
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x)
						{
							const bool inside = x >= x0 && x < x0 + ow && y >= oy0 && y < oy0 + oh;
							p.at(x, y) = inside ? Master(x - x0, y - oy0, 200.0, 120.0) : Master(x, y);
						}
					return p;
				}, [&](int t, int, int) { return q[static_cast<size_t>(t)]; });
			// Block vectors: the object's blocks (centre inside) move with it, the wall is still.
			FieldMotionSource motion;
			for (int t = 0; t < n; ++t)
			{
				MotionPair b = MotionPair::Uniform(kW, kH, 16.0, 0.0, 0.0), f = b;
				for (int by = 0; by < b.gridH; ++by)
					for (int bx = 0; bx < b.gridW; ++bx)
					{
						const double cx = (bx + 0.5) * 16.0, cy = (by + 0.5) * 16.0;
						const size_t i = static_cast<size_t>(by) * b.gridW + bx;
						if (cx >= objX(t) && cx < objX(t) + ow && cy >= oy0 && cy < oy0 + oh) b.vx[i] = static_cast<float>(-speed);
						if (cx >= objX(t) - speed && cx < objX(t) - speed + ow && cy >= oy0 && cy < oy0 + oh) f.vx[i] = static_cast<float>(speed);
					}
				motion.SetBackward(static_cast<uint64_t>(t), b);
				motion.SetForward(static_cast<uint64_t>(t), f);
			}
			const Run on = GoSteady(s.frames, &motion);
			double mseOff = 0, mseOn = 0, minBase = 1.0;
			int cnt = 0;
			for (int t = 36; t < 56; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				const int rx0 = static_cast<int>(objX(t - 6)), rx1 = static_cast<int>(objX(t));   // uncovered since six frames ago
				mseOff += Mse(s.frames[u], s.truth[u], rx0, oy0, rx1, oy0 + oh);
				mseOn += Mse(on.out[u], s.truth[u], rx0, oy0, rx1, oy0 + oh);
				++cnt;
				for (size_t b = 0; b < on.diag[u].baseMatch.size(); ++b)
					if (on.diag[u].slotId[b] >= 0 && on.diag[u].hard[b] < 0.5) minBase = (std::min)(minBase, on.diag[u].baseMatch[b]);
			}
			mseOff /= cnt; mseOn /= cnt;
			LogF("revealed strip MSE off %.3f on %.3f; lowest base-match gate seen %.3f", mseOff, mseOn, minBase);
			Assert::IsTrue(mseOn <= mseOff + 0.3 + 0.03 * mseOff, L"the revealed region carries no ghost of the occluder");
			Assert::IsTrue(minBase < 0.5, L"the base-match veto is visible in the diagnostics");
		}

		TEST_METHOD(TemporalDetailCutAndStampReset)
		{
			const int n = 40, cutAt = 24;
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const Stream s = Build(n, [&](int t) { return t < cutAt ? Sample(0, 0) : Sample(0, 0, 77.0, 31.0); },
				[&](int t, int, int) { return q[static_cast<size_t>(t)]; });
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			Model model(Steady());
			const Run r = Go(model, s.frames, &motion, kHz, 1080.0, 0, [&](int t, Model::FrameIn& in) { in.cut = (t == cutAt); });
			Assert::AreEqual(1, r.diag[static_cast<size_t>(cutAt)].resets - r.diag[static_cast<size_t>(cutAt) - 1].resets, L"the cut resets once");
			double leak = 0.0;
			for (int t = cutAt; t < cutAt + 4; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				Assert::IsTrue(Same(r.out[u], s.frames[u]), L"post-cut frames carry nothing from the old scene");
				leak += std::fabs(Corr(Band(r.out[u], 1.0), Band(s.truth[static_cast<size_t>(cutAt) - 1], 1.0)));
			}
			LogF("post-cut correlation of the output fine band with the pre-cut scene: %.4f (input alone: %.4f)", leak / 4,
				std::fabs(Corr(Band(s.frames[static_cast<size_t>(cutAt)], 1.0), Band(s.truth[static_cast<size_t>(cutAt) - 1], 1.0))));

			// Each counter with the Reset policy resets; settings[repair] alone does not.
			const char* names[5] = { "full", "continuity", "rebaseH", "rebaseV", "settingsRepair" };
			for (int c = 0; c < 5; ++c)
			{
				Model m(Steady());
				const int change = 26;
				const std::vector<double> qq = Pattern("IBBBPBBB", 28);
				const Stream st = BuildPan(qq, 0.0);
				FieldMotionSource mo;
				SetPan(mo, 28, 0.0, 0.0);
				const Run rr = Go(m, st.frames, &mo, kHz, 1080.0, 0, [&](int t, Model::FrameIn& in)
					{
						if (t >= change)
						{
							switch (c)
							{
							case 0: in.stamps.full = 1; break;
							case 1: in.stamps.continuity = 1; break;
							case 2: in.stamps.rebaseH = 1; break;
							case 3: in.stamps.rebaseV = 1; break;
							default: in.stamps.settingsRepair = 1; break;
							}
						}
					});
				const int before = rr.diag[static_cast<size_t>(change) - 1].resets, after = rr.diag[static_cast<size_t>(change)].resets;
				if (c < 4)
				{
					Assert::AreEqual(1, after - before, (std::wstring(L"a change of ") + std::wstring(names[c], names[c] + std::strlen(names[c])) + L" resets").c_str());
					Assert::IsTrue(Same(rr.out[static_cast<size_t>(change)], st.frames[static_cast<size_t>(change)]), L"the reset frame is the input");
				}
				else Assert::AreEqual(0, after - before, L"settings[repair] alone does not reset");
			}
			// UpdateSettings knows which fields changed.
			Model m(Steady());
			const Stream st = BuildPan(Pattern("IBBBPBBB", 20), 0.0);
			FieldMotionSource mo;
			SetPan(mo, 20, 0.0, 0.0);
			const Run prev = Go(m, st.frames, &mo);
			const int base = m.Diag().resets;
			{
				// F2: a repeat (same sequence) returns the last output unchanged and advances nothing.
				Model::FrameIn rep;
				rep.luma = st.frames[19];
				rep.seq = 19;
				rep.contentHz = kHz;
				rep.motion = &mo;
				const Plane again = m.Process(rep);
				Assert::AreEqual(base, m.Diag().resets, L"a repeat does not reset");
				Assert::AreEqual(20, m.Diag().frames, L"a repeat advances no state");
				Assert::IsTrue(Same(again, prev.out.back()), L"a repeat returns the last frame's output unchanged");
			}
			Settings s2 = Steady();
			s2.strength = 0.5;
			s2.boostMax = 0.1;
			m.UpdateSettings(s2);
			Assert::AreEqual(base, m.Diag().resets, L"strength and boost changes do not reset");
			s2.maxAgeSeconds = 1.0;
			m.UpdateSettings(s2);
			Assert::AreEqual(base + 1, m.Diag().resets, L"a max_age change resets");
			s2.mode = Mode::Off;
			m.UpdateSettings(s2);
			Assert::AreEqual(base + 2, m.Diag().resets, L"a mode change resets");
		}

		TEST_METHOD(TemporalDetailDeterministic)
		{
			const int n = 32;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.37);
			FieldMotionSource motion;
			SetPan(motion, n, 0.37, 0.0);
			const Run a = GoSteady(s.frames, &motion, Steady(), 2);
			const Run b = GoSteady(s.frames, &motion, Steady(), 2);
			for (size_t t = 0; t < static_cast<size_t>(n); ++t)
			{
				Assert::IsTrue(Same(a.out[t], b.out[t]), L"bitwise-equal outputs");
				Assert::IsTrue(a.diag[t] == b.diag[t], L"equal diagnostics");
			}
		}

		TEST_METHOD(TemporalDetailKeepsGrain)
		{
			const int n = 96;
			const double sigma = 1.5;
			// Left half flat, right half textured, fresh grain per frame.
			const std::vector<double> q = Pattern("IBBBPBBB", n);
			const Stream s = Build(n, [](int)
				{
					Plane p = Sample(0, 0);
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW / 2; ++x) p.at(x, y) = 100.0 + 0.1 * y;
					return p;
				}, [&](int t, int, int) { return q[static_cast<size_t>(t)]; }, 11, sigma);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			// (1) grain RMS on the flat area
			double rOff = 0, rOn = 0;
			int cnt = 0;
			for (int t = 32; t < n; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				rOff += std::sqrt(FineEnergy(s.frames[u], 8, 8, 56, kH - 8));
				rOn += std::sqrt(FineEnergy(on.out[u], 8, 8, 56, kH - 8));
				++cnt;
			}
			rOff /= cnt; rOn /= cnt;
			LogF("flat-area grain RMS off %.4f on %.4f (ratio %.4f)", rOff, rOn, rOn / rOff);
			Assert::IsTrue(std::fabs(rOn / rOff - 1.0) <= 0.05, L"grain RMS on the flat area within 5%");
			// (2) per-pixel temporal spectrum, 4 bands, averaged over pixels (DFT of 64 frames)
			const int t0 = 32, N = 64;
			auto spectrum = [&](const std::function<double(int, int, int)>& sample, int x0, int x1, double* bands)
			{
				for (int b = 0; b < 4; ++b) bands[b] = 0.0;
				int pixels = 0;
				for (int y = 8; y < kH - 8; y += 3)
					for (int x = x0; x < x1; x += 3)
					{
						std::vector<double> v(N);
						double m = 0;
						for (int k = 0; k < N; ++k) m += v[static_cast<size_t>(k)] = sample(t0 + k, x, y);
						m /= N;
						for (int f = 1; f <= 32; ++f)
						{
							double re = 0, im = 0;
							for (int k = 0; k < N; ++k)
							{
								const double a = 2 * kPi * f * k / N;
								re += (v[static_cast<size_t>(k)] - m) * std::cos(a);
								im += (v[static_cast<size_t>(k)] - m) * std::sin(a);
							}
							bands[(f - 1) / 8] += re * re + im * im;
						}
						++pixels;
					}
				for (int b = 0; b < 4; ++b) bands[b] /= pixels;
			};
			double so[4], sn[4];
			spectrum([&](int t, int x, int y) { return s.frames[static_cast<size_t>(t)].at(x, y); }, 8, 56, so);
			spectrum([&](int t, int x, int y) { return on.out[static_cast<size_t>(t)].at(x, y); }, 8, 56, sn);
			for (int b = 0; b < 4; ++b)
			{
				LogF("flat area temporal band %.0f: on/off %.4f", b, sn[b] / so[b]);
				Assert::IsTrue(std::fabs(sn[b] / so[b] - 1.0) <= 0.05, L"flat-area temporal spectrum band within 5%");
			}
			// (3) the grain component G = Y_out - base - S_out of the whole field is the input's G, exactly.
			double worst = 0.0;
			for (int t = 32; t < n; ++t)
			{
				const Diagnostics& d = on.diag[static_cast<size_t>(t)];
				const Plane& o = on.out[static_cast<size_t>(t)];
				for (size_t i = 0; i < o.v.size(); ++i)
					worst = (std::max)(worst, std::fabs(o.v[i] - d.base.v[i] - d.structureOut.v[i] - d.grainOut.v[i]));
				for (size_t i = 0; i < o.v.size(); ++i) Assert::IsTrue(d.grainOut.v[i] == d.grain.v[i], L"no guard add-back in normal operation: G_out is G_in");
			}
			LogF("max |Y_out - base - S_out - G_in| = %.3g", worst);
			Assert::IsTrue(worst < 1e-9, L"only the structure changes: the grain component is untouched everywhere");
			double go[4], gn[4];
			spectrum([&](int t, int x, int y) { return on.diag[static_cast<size_t>(t)].grain.at(x, y); }, 8, kW - 8, go);
			spectrum([&](int t, int x, int y) {
				const size_t u = static_cast<size_t>(t);
				return on.out[u].at(x, y) - on.diag[u].base.at(x, y) - on.diag[u].structureOut.at(x, y);
			}, 8, kW - 8, gn);
			for (int b = 0; b < 4; ++b)
			{
				Assert::IsTrue(go[b] > 0.0, L"input grain component temporal spectrum must be positive");
				Assert::IsTrue(std::fabs(gn[b] / go[b] - 1.0) <= 0.05, L"grain component temporal spectrum within 5%");
			}
			// Owner bounds, ASSERTED on the grain isolate (seed 11 above, seed 12 here, the same scene): textured grain RMS
			// within 5%, frozen-grain lag-1 correlation increase <= 0.05, whole-field temporal bands within 0.95 .. 1.05.
			const Stream b = KeepsGrainScene(12, sigma, n);
			const Run onB = GoSteady(b.frames, &motion);
			const GrainMetrics m = GrainIsolate(s, b, on, onB, 32, n, 72);
			LogGrain("grain isolate (owner bounds):", m);
			Assert::IsTrue(std::fabs(m.rmsRatio - 1.0) <= 0.05, L"textured grain RMS within 5% of off");
			Assert::IsTrue(m.Increase() <= 0.05, L"frozen-grain lag-1 correlation increase <= 0.05");
			for (int q = 0; q < 4; ++q) Assert::IsTrue(m.bands[q] >= 0.95 && m.bands[q] <= 1.05, L"whole-field grain temporal band within 0.95 .. 1.05 of off");
			// The current frame's own grain is never touched: the change (out - in) carries none of the frame's exact grain field.
			{
				double num = 0, den = 0;
				for (int t = 32; t < n; ++t)
				{
					const size_t u = static_cast<size_t>(t);
					for (int y = 8; y < kH - 8; ++y)
						for (int x = 8; x < kW - 8; ++x)
						{
							const double g = s.grain[u].at(x, y);
							num += (on.out[u].at(x, y) - s.frames[u].at(x, y)) * g;
							den += g * g;
						}
				}
				LogF("regression of (out - in) on the frame's own grain field: %.5f (0 = untouched)", num / den);
				Assert::IsTrue(std::fabs(num / den) <= 0.01, L"the current grain is neither removed nor amplified");
			}
			// For reference only (not grain metrics): the raw output spectrum and the raw residual against the clean coded
			// frame also contain the restored structure and the removed I/B pulse, which the grain isolate cancels.
			{
				double wo[4], wn[4];
				spectrum([&](int t, int x, int y) { return s.frames[static_cast<size_t>(t)].at(x, y); }, 8, kW - 8, wo);
				spectrum([&](int t, int x, int y) { return on.out[static_cast<size_t>(t)].at(x, y); }, 8, kW - 8, wn);
				LogF("raw whole-field temporal bands on/off (structure + pulse + grain): %.4f %.4f %.4f %.4f", wn[0] / wo[0], wn[1] / wo[1], wn[2] / wo[2], wn[3] / wo[3]);
			}
		}

		TEST_METHOD(TemporalDetailGrainGuardRestoresBounds)
		{
			// The output-vs-input grain guard (README step 10): the transfer is FORCED to damage grain (Model::Tuning: remove
			// half the current grain, or add the reference's own warped grain), and the guard must bring the owner's bounds back.
			const int n = 96;
			const double sigma = 1.5;
			const Stream a = KeepsGrainScene(11, sigma, n), b = KeepsGrainScene(12, sigma, n);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			struct Case { const char* name; double drop, freeze; };
			const Case cases[2] = { { "forced drop 0.5 (current grain removed)", 0.5, 0.0 }, { "forced freeze 1.0 (reference grain added)", 0.0, 1.0 } };
			for (const Case& c : cases)
			{
				Model::Tuning off, on;
				off.guard = false; off.forceDrop = c.drop; off.forceFreeze = c.freeze;
				on = off; on.guard = true;
				const Run ra0 = GoTuned(a.frames, &motion, off), rb0 = GoTuned(b.frames, &motion, off);
				const Run ra1 = GoTuned(a.frames, &motion, on), rb1 = GoTuned(b.frames, &motion, on);
				const GrainMetrics m0 = GrainIsolate(a, b, ra0, rb0, 32, n, 72), m1 = GrainIsolate(a, b, ra1, rb1, 32, n, 72);
				Log(c.name);
				LogGrain(" guard off:", m0);
				LogGrain(" guard on:", m1);
				int addBack = 0;
				bool visible = false;
				for (const Diagnostics& d : ra1.diag) { addBack += d.addBackBlocks; visible = visible || d.status.find("grain guard") != std::string::npos; }
				LogF(" guard trips %.0f, add-back block-frames %.0f", static_cast<double>(ra1.diag.back().guardTrips), static_cast<double>(addBack));
				Assert::IsFalse(m0.Holds(), L"the forced damage breaks the owner's bounds without the guard");
				Assert::IsTrue(m1.Holds(), L"the guard restores the owner's bounds");
				Assert::IsTrue(ra1.diag.back().guardTrips > 0 || addBack > 0, L"the guard acted (counted)");
				Assert::IsTrue(visible, L"the guard is visible in the status");
			}
			// Normal operation: the guard stays quiet (no trips) on the same scene.
			const Run quiet = GoSteady(a.frames, &motion);
			Assert::AreEqual(0, quiet.diag.back().guardTrips, L"no guard trips in normal operation");
		}

		TEST_METHOD(TemporalDetailGainIsStructure)
		{
			// Where the gain comes from (SteadiesIBPulse stream, B frames): R grain seeds; the seed mean of the output is its
			// structure, the deviation from it its grain. Own-grain retention = regression of that deviation on each frame's
			// exact grain field (corrected for the mean's 1/R share); foreign grain = the remainder of the grain energy.
			const int n = 56, R = 4;
			const double sigma = 1.2;
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			auto study = [&](const Model::Tuning& tune, double& gainDb, double& structDb, double& retention, double& foreign)
			{
				std::vector<Stream> ss;
				std::vector<Run> rr;
				for (int r = 0; r < R; ++r)
				{
					ss.push_back(BuildPan(Pattern("IBBBPBBB", n), 0.0, 0.0, 7u + 101u * static_cast<uint32_t>(r), sigma));
					rr.push_back(GoTuned(ss.back().frames, &motion, tune));
				}
				double mOff = 0, mOn = 0, sOff = 0, sOn = 0, gOn = 0, own = 0, ownDen = 0;
				int cnt = 0;
				for (int t = 24; t < n; ++t)
				{
					if (t % 8 == 0 || t % 8 == 4) continue;
					const size_t u = static_cast<size_t>(t);
					Plane mean(kW, kH), cleanIn(kW, kH);
					for (int r = 0; r < R; ++r)
						for (size_t i = 0; i < mean.v.size(); ++i) mean.v[i] += rr[static_cast<size_t>(r)].out[u].v[i] / R;
					for (size_t i = 0; i < cleanIn.v.size(); ++i) cleanIn.v[i] = ss[0].frames[u].v[i] - ss[0].grain[u].v[i];
					double var = 0;
					for (int r = 0; r < R; ++r)
					{
						const Stream& st = ss[static_cast<size_t>(r)];
						const Plane& o = rr[static_cast<size_t>(r)].out[u];
						mOff += Mse(st.frames[u], st.truth[u]); mOn += Mse(o, st.truth[u]);
						Plane dev(kW, kH);
						for (size_t i = 0; i < dev.v.size(); ++i) dev.v[i] = o.v[i] - mean.v[i];
						var += Mse(dev, Plane(kW, kH));
						for (int y = 8; y < kH - 8; ++y)
							for (int x = 8; x < kW - 8; ++x) { own += dev.at(x, y) * st.grain[u].at(x, y); ownDen += st.grain[u].at(x, y) * st.grain[u].at(x, y); }
					}
					var /= (R - 1);
					sOn += Mse(mean, ss[0].truth[u]) - var / R;
					sOff += Mse(cleanIn, ss[0].truth[u]);
					gOn += var;
					++cnt;
				}
				mOff /= cnt * R; mOn /= cnt * R; sOff /= cnt; sOn /= cnt; gOn /= cnt;
				retention = own / ownDen / (1.0 - 1.0 / R);
				foreign = (std::max)(0.0, gOn - retention * retention * sigma * sigma) / (sigma * sigma);
				gainDb = PsnrOf(mOn) - PsnrOf(mOff);
				structDb = 10.0 * std::log10((sOff + sigma * sigma) / ((std::max)(sOn, 0.0) + sigma * sigma));
				LogF("  total gain %.2f dB; structure MSE %.3f -> %.3f (gain with the own grain kept: %.2f dB)", gainDb, sOff, sOn, structDb);
				LogF("  own-grain retention %.4f; foreign (frozen/reference) grain energy %.4f of sigma^2", retention, foreign);
			};
			double g, sdb, ret, fr;
			Model::Tuning designed;
			designed.grainBudget = 0.0;
			Log("designed strength (no template grain budget; for the record, breaks the KeepsGrain bounds):");
			study(designed, g, sdb, ret, fr);
			Log("default (template grain budget kGrainBudget):");
			study(Model::Tuning(), g, sdb, ret, fr);
			// Multi-frame template cleaner + budget 0.08 (strict bounds asserted in KeepsGrain): 6.3 dB measured (3.0 dB with
			// the single-frame split's template at its own strict-bounds budget 0.07).
			Assert::IsTrue(g >= 5.5 && sdb >= 5.0, L"the default gains >= 5.5 dB, all of it structure");
			Assert::IsTrue(std::fabs(ret - 1.0) <= 0.02, L"the current grain is kept (retention within 2%)");
			Assert::IsTrue(fr <= 0.05, L"foreign grain energy <= 5% of the grain energy");
		}

		TEST_METHOD(TemporalDetailCleanerAtStrictBounds)
		{
			// The multi-frame template cleaner (README step 4a). (1) Its transform reconstructs exactly at unit gains: the
			// orthonormal DCT round trip and the sine window's half-step power sum. (2) At the default budget it holds the
			// owner's strict bounds on the two-seed grain isolate and gains far more than the single-frame split's template
			// at that template's own strict-bounds budget (0.07); the template's residual grain share drops.
			{
				const Detail::CleanBasis b = Detail::MakeCleanBasis(8, 2.5);
				Noise nz(5);
				std::vector<double> x(64), X(64), y(64), tmp(64);
				for (double& v : x) v = nz.Next();
				Detail::Dct2(b, x.data(), X.data(), tmp.data(), false);
				Detail::Dct2(b, X.data(), y.data(), tmp.data(), true);
				double worst = 0.0, ex = 0.0, eX = 0.0;
				for (int i = 0; i < 64; ++i) { worst = (std::max)(worst, std::fabs(x[i] - y[i])); ex += x[i] * x[i]; eX += X[i] * X[i]; }
				Assert::IsTrue(worst < 1e-12 && std::fabs(eX / ex - 1.0) < 1e-12, L"orthonormal DCT round trip is exact");
				for (int i = 0; i < 4; ++i)
					Assert::IsTrue(std::fabs(b.win[i] * b.win[i] + b.win[i + 4] * b.win[i + 4] - 1.0) < 1e-12, L"window power sums to 1 at a half-patch step");
				Assert::IsTrue(b.hs[0] == 0.0, L"the detail band holds no grain at DC");
			}
			const int n = 56, R = 4, nk = 96;
			FieldMotionSource motion;
			SetPan(motion, nk, 0.0, 0.0);
			auto gainOf = [&](const Model::Tuning& tune)
			{
				double mOff = 0, mOn = 0;
				for (int r = 0; r < R; ++r)
				{
					const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0, 0.0, 7u + 101u * static_cast<uint32_t>(r), 1.2);
					const Run on = GoTuned(s.frames, &motion, tune);
					for (int t = 24; t < n; ++t)
					{
						if (t % 8 == 0 || t % 8 == 4) continue;
						mOff += Mse(s.frames[static_cast<size_t>(t)], s.truth[static_cast<size_t>(t)]);
						mOn += Mse(on.out[static_cast<size_t>(t)], s.truth[static_cast<size_t>(t)]);
					}
				}
				return PsnrOf(mOn) - PsnrOf(mOff);
			};
			const Stream a = KeepsGrainScene(11, 1.5, nk), b = KeepsGrainScene(12, 1.5, nk);
			auto bounds = [&](const Model::Tuning& tune, double& nT)
			{
				const Run ra = GoTuned(a.frames, &motion, tune), rb = GoTuned(b.frames, &motion, tune);
				double s = 0.0;
				int c = 0;
				for (int t = 32; t < nk; ++t)
					for (size_t u = 0; u < ra.diag[static_cast<size_t>(t)].templateGrain.size(); ++u)
						if (ra.diag[static_cast<size_t>(t)].templateGrain[u] > 0.0) { s += ra.diag[static_cast<size_t>(t)].templateGrain[u]; ++c; }
				nT = c ? s / c : 0.0;
				Assert::AreEqual(0, ra.diag.back().guardTrips, L"no guard trips in normal operation");
				return GrainIsolate(a, b, ra, rb, 32, nk, 72);
			};
			Model::Tuning single;
			single.templateClean = false;
			single.grainBudget = 0.07;   // the single-frame template's strict-bounds budget
			double nTc = 0, nTs = 0;
			const double gClean = gainOf(Model::Tuning()), gSingle = gainOf(single);
			const GrainMetrics mClean = bounds(Model::Tuning(), nTc), mSingle = bounds(single, nTs);
			LogF("B-frame gain: multi-frame cleaner %.2f dB (budget %.3f), single-frame template %.2f dB (budget 0.07)", gClean, kGrainBudget, gSingle);
			LogF("mean template grain share sum c^2 rho^2: cleaner %.4f, single-frame %.4f", nTc, nTs);
			LogGrain("cleaner, grain isolate (owner bounds):", mClean);
			LogGrain("single-frame template, grain isolate:", mSingle);
			Assert::IsTrue(mClean.Holds(), L"the cleaner holds the owner's strict bounds at the default budget");
			Assert::IsTrue(mSingle.Holds(), L"the single-frame reference also holds them at its own budget");
			Assert::IsTrue(gClean >= gSingle + 2.0, L"the cleaner gains >= 2 dB more at the strict bounds");
			Assert::IsTrue(nTc < 0.6 * nTs, L"the cleaned template carries much less residual grain");
		}

		TEST_METHOD(GrainSplitAddBackIsExact)
		{
			// repair.grain_split: Y = clean + G by construction; clean = base + f D1 + f D2; deterministic.
			const Stream s = BuildPan(Pattern("IBBBPBBB", 2), 0.0);
			const GrainSplit::Split a = GrainSplit::Compute(s.frames[1], 1.0, 16), b = GrainSplit::Compute(s.frames[1], 1.0, 16);
			double worst = 0.0, worstClean = 0.0;
			for (size_t i = 0; i < a.clean.v.size(); ++i)
			{
				worst = (std::max)(worst, std::fabs(a.clean.v[i] + a.grain.v[i] - s.frames[1].v[i]));
				worstClean = (std::max)(worstClean, std::fabs(a.base.v[i] + a.s1.v[i] + a.s2.v[i] - a.clean.v[i]));
				Assert::IsTrue(a.f.v[i] >= 0.0 && a.f.v[i] <= 1.0, L"Wiener share in 0 .. 1");
			}
			LogF("max |clean + G - Y| %.3g, max |base + s1 + s2 - clean| %.3g", worst, worstClean);
			Assert::IsTrue(worst < 1e-12 && worstClean < 1e-12, L"the add-back is exact");
			Assert::IsTrue(a.clean.v == b.clean.v && a.grain.v == b.grain.v && a.N == b.N, L"deterministic");
			Assert::IsTrue(std::string(GrainSplit::kProduct.id) == "grain.split", L"declared product");
		}

		TEST_METHOD(TemporalDetailFlickerFreeRefresh)
		{
			const int n = 56;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const Run on = GoSteady(s.frames, &motion);
			std::vector<double> eOff, eOn;
			for (int t = 24; t < n; ++t) { eOff.push_back(FineEnergy(s.frames[static_cast<size_t>(t)])); eOn.push_back(FineEnergy(on.out[static_cast<size_t>(t)])); }
			const double fOff = std::sqrt(Variance(eOff, 0, eOff.size())) / Mean(eOff, 0, eOff.size());
			const double fOn = std::sqrt(Variance(eOn, 0, eOn.size())) / Mean(eOn, 0, eOn.size());
			LogF("flicker (std/mean of per-frame structure energy) off %.4f on %.4f", fOff, fOn);
			Assert::IsTrue(fOn < fOff, L"flicker drops");
			const double rise = 1.0 / Frames(kRiseSeconds, kHz), fall = 1.0 / Frames(kFallSeconds, kHz);
			double worstUp = 0.0, worstDown = 0.0;
			for (int t = 16; t < n; ++t)
				for (size_t b = 0; b < on.diag[0].weight.size(); ++b)
				{
					const double dw = on.diag[static_cast<size_t>(t)].weight[b] - on.diag[static_cast<size_t>(t) - 1].weight[b];
					worstUp = (std::max)(worstUp, dw); worstDown = (std::max)(worstDown, -dw);
				}
			LogF("largest per-block weight step up %.3f (limit %.3f), down %.3f (limit %.3f)", worstUp, rise, worstDown, fall);
			Assert::IsTrue(worstUp <= rise + 0.05, L"no weight step above the limiter's rise");
			Assert::IsTrue(worstDown <= fall + 0.05, L"no weight step above the limiter's fall");

			// Age-out over max_age: the pulse stops, the last reference ages, the weight leaves gradually.
			const int m = 24 + 66;
			std::vector<double> q = Pattern("IBBBPBBB", 24);
			while (static_cast<int>(q.size()) < m) q.push_back(0.3);
			const Stream a = BuildPan(q, 0.0);
			FieldMotionSource mo;
			SetPan(mo, m, 0.0, 0.0);
			const Run ra = GoSteady(a.frames, &mo);
			// the block with the largest weight at frame 30 is followed
			size_t blk = 0;
			for (size_t b = 0; b < ra.diag[30].weight.size(); ++b) if (ra.diag[30].weight[b] > ra.diag[30].weight[blk]) blk = b;
			std::vector<double> w;
			for (int t = 0; t < m; ++t) w.push_back(ra.diag[static_cast<size_t>(t)].weight[blk]);
			double peak = 0; int tPeak = 0;
			for (int t = 0; t < m; ++t) if (w[static_cast<size_t>(t)] > peak) { peak = w[static_cast<size_t>(t)]; tPeak = t; }
			int t1 = -1, t2 = -1;
			for (int t = tPeak; t < m; ++t)
			{
				if (t1 < 0 && w[static_cast<size_t>(t)] < 0.95 * peak && t > 40) t1 = t;
				if (t1 >= 0 && t2 < 0 && w[static_cast<size_t>(t)] < 0.02) t2 = t;
			}
			double worstDrop = 0.0;
			for (int t = 30; t < m; ++t) worstDrop = (std::max)(worstDrop, w[static_cast<size_t>(t) - 1] - w[static_cast<size_t>(t)]);
			LogF("age-out: peak %.3f at frame %.0f, falls below 95%% at %.0f and below 0.02 at %.0f", peak, tPeak, t1, t2);
			LogF("largest single-frame drop %.3f (limit 1/Frames(0.3 s) = %.3f)", worstDrop, fall);
			Assert::IsTrue(peak > 0.5 && t1 > 0 && t2 > t1, L"the weight is applied and leaves");
			Assert::IsTrue(t2 - t1 >= Frames(0.3, kHz), L"the age-out is gradual: it spans at least Frames(0.3 s) frames");
			Assert::IsTrue(worstDrop <= fall + 0.02, L"no single-frame drop above 1/Frames(0.3 s)");
		}

		TEST_METHOD(TemporalDetailFutureReferee)
		{
			// A: from frame 24 the right half loses the fine band in EVERY frame (the picture means to be soft).
			{
				const int n = 36;
				const std::vector<double> q = Pattern("IBBBPBBB", n);
				const Stream s = Build(n, [](int) { return Sample(0, 0); },
					[&](int t, int x, int) { return (t >= 24 && x >= kW / 2) ? 0.3 : q[static_cast<size_t>(t)]; });
				FieldMotionSource motion;
				SetPan(motion, n, 0.0, 0.0);
				const Run noQueue = GoSteady(s.frames, &motion, Steady(), 0);
				const Run queue = GoSteady(s.frames, &motion, Steady(), 3);
				double eOff = 0, eNo = 0, eQ = 0, minRef = 1.0;
				int cnt = 0;
				for (int t = 25; t < 32; ++t)
				{
					const size_t u = static_cast<size_t>(t);
					eOff += FineEnergy(s.frames[u], 72, 8, kW - 8, kH - 8);
					eNo += FineEnergy(noQueue.out[u], 72, 8, kW - 8, kH - 8);
					eQ += FineEnergy(queue.out[u], 72, 8, kW - 8, kH - 8);
					for (size_t b = 0; b < queue.diag[u].referee.size(); ++b)
						if (static_cast<int>(b) % queue.diag[u].bw >= queue.diag[u].bw / 2 + 1 && queue.diag[u].slotId[b] >= 0) minRef = (std::min)(minRef, queue.diag[u].referee[b]);
					++cnt;
				}
				LogF("soft right half, frames 25-31: fine energy off %.4f, no queue %.4f, queue %.4f; lowest referee gate %.3f", eOff / cnt, eNo / cnt, eQ / cnt, minRef);
				Assert::IsTrue(eQ <= eOff * 1.05, L"the future is also soft: no transfer");
				Assert::IsTrue(eQ <= eNo + 1e-9, L"the queue never transfers more than the past-only mode here");
				Assert::IsTrue(minRef < 0.5, L"the referee veto is visible in the diagnostics");
			}
			// B: a scene change leaves the past references useless; the I-like frame in the queue serves the last B-frames.
			{
				const int n = 40, change = 24;
				std::vector<double> q = Pattern("IBBBPBBB", change);
				for (int t = change; t < n; ++t) q.push_back(t < change + 12 ? 0.3 : (t == change + 12 ? 1.0 : 0.3));
				const Stream s = Build(n, [&](int t) { return t < change ? Sample(0, 0) : Sample(0, 0, 150.0, 60.0); },
					[&](int t, int, int) { return q[static_cast<size_t>(t)]; });
				FieldMotionSource motion;
				SetPan(motion, n, 0.0, 0.0);
				const Run noQueue = GoSteady(s.frames, &motion, Steady(), 0);
				const Run queue = GoSteady(s.frames, &motion, Steady(), 3);
				double mOff = 0, mNo = 0, mQ = 0;
				int cnt = 0;
				for (int t = change + 9; t < change + 12; ++t)   // the last B-frames before the I-like frame
				{
					const size_t u = static_cast<size_t>(t);
					mOff += Mse(s.frames[u], s.truth[u]); mNo += Mse(noQueue.out[u], s.truth[u]); mQ += Mse(queue.out[u], s.truth[u]);
					++cnt;
				}
				LogF("last B-frames before the I-like frame: MSE off %.3f, past only %.3f, with the future slot %.3f", mOff / cnt, mNo / cnt, mQ / cnt);
				Assert::IsTrue(mQ < mOff * 0.9, L"the future slot improves the last B-frames");
				Assert::IsTrue(mQ < mNo * 0.9, L"the future slot beats the past-only mode");
			}
		}

		TEST_METHOD(TemporalDetailIrregularCodingPattern)
		{
			struct Case { const char* name; std::vector<double> q; };
			const int n = 60;
			const std::vector<Case> cases = {
				{ "irregular gaps 3,7,2,11,5", Irregular({ 3, 7, 2, 11, 5, 9, 4, 6 }, n) },
				{ "hierarchical IbBbPbBb", Pattern("IbBbPbBb", n) },
			};
			for (const Case& c : cases)
			{
				const Stream s = BuildPan(c.q, 0.0);
				FieldMotionSource motion;
				SetPan(motion, n, 0.0, 0.0);
				const Run on = GoSteady(s.frames, &motion);
				double mOff = 0, mOn = 0;
				int cnt = 0;
				std::vector<double> eOff, eOn;
				for (int t = 32; t < n; ++t)
				{
					const size_t u = static_cast<size_t>(t);
					eOff.push_back(FineEnergy(s.frames[u])); eOn.push_back(FineEnergy(on.out[u]));
					if (c.q[u] >= 0.8) continue;
					mOff += Mse(s.frames[u], s.truth[u]); mOn += Mse(on.out[u], s.truth[u]);
					++cnt;
				}
				mOff /= cnt; mOn /= cnt;
				const double vOff = Variance(eOff, 0, eOff.size()), vOn = Variance(eOn, 0, eOn.size());
				LogF("pattern: lower-quality frame MSE off %.3f on %.3f; fine-energy variance ratio %.3f", mOff, mOn, vOn / vOff);
				Log(c.name);
				Assert::IsTrue(mOn < mOff * 0.9, L"the pattern is steadied (error drops)");
				Assert::IsTrue(vOn <= 0.7 * vOff, L"the fine-structure variance drops");
			}
		}

		TEST_METHOD(TemporalDetailScaleAndRateGeneral)
		{
			Assert::AreEqual(7, Frames(0.3, 23.976), L"0.3 s at 23.976 Hz");
			Assert::AreEqual(18, Frames(0.3, 59.94), L"0.3 s at 59.94 Hz");
			Assert::AreEqual(Frames(0.3, kReferenceHz), Frames(0.3, 0.0), L"unknown rate falls back to the reference");
			Assert::AreEqual(1, Frames(0.001, 24.0), L"never below one frame");
			Assert::AreEqual(1.0, Scale(1080), 1e-12, L"1080 lines");
			Assert::AreEqual(1.0, Scale(2160), 1e-12, L"2160 lines");
			Assert::AreEqual(2.0, Scale(4320), 1e-12, L"4320 lines");

			const int n = 20;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const double lines[3] = { 1080.0, 2160.0, 4320.0 };
			const int expectPx[3] = { 16, 16, 32 };
			for (int i = 0; i < 3; ++i)
			{
				Model m(Steady());
				const Run r = Go(m, s.frames, &motion, kHz, lines[i]);
				LogF("planeLines %.0f: block %.0f px", lines[i], static_cast<double>(r.diag.back().blockPx));
				Assert::AreEqual(expectPx[i], r.diag.back().blockPx, L"block size follows the scale");
				for (const Plane& p : r.out)
					for (double v : p.v) Assert::IsTrue(std::isfinite(v) && v > -50.0 && v < 400.0, L"finite, sane output");
			}

			// The same behaviour in seconds at 23.976 and 59.94 Hz: the age-out (pulse stops) takes the same time.
			double seconds[2] = { 0, 0 };
			const double hzs[2] = { 23.976, 59.94 };
			for (int k = 0; k < 2; ++k)
			{
				const double hz = hzs[k];
				const int period = Frames(8 / 23.976, hz);   // the same pulse period in seconds
				std::vector<double> q;
				for (int t = 0; t < Frames(0.6, hz); ++t) q.push_back((t % period) == 0 ? 1.0 : ((t % period) == period / 2 ? 0.8 : 0.3));
				const int pulseFrames = static_cast<int>(q.size());
				while (static_cast<int>(q.size()) < pulseFrames + Frames(1.6, hz)) q.push_back(0.3);
				const Stream a = BuildPan(q, 0.0);
				FieldMotionSource mo;
				SetPan(mo, static_cast<int>(q.size()), 0.0, 0.0);
				Model m(Steady());
				const Run r = Go(m, a.frames, &mo, hz);
				size_t blk = 0;
				const size_t probe = static_cast<size_t>(pulseFrames - 2);
				for (size_t b = 0; b < r.diag[probe].weight.size(); ++b) if (r.diag[probe].weight[b] > r.diag[probe].weight[blk]) blk = b;
				double peak = 0; int tPeak = 0;
				for (size_t t = 0; t < r.diag.size(); ++t) if (r.diag[t].weight[blk] > peak) { peak = r.diag[t].weight[blk]; tPeak = static_cast<int>(t); }
				int t1 = -1, t2 = -1;
				for (size_t t = static_cast<size_t>(tPeak); t < r.diag.size(); ++t)
				{
					const double wv = r.diag[t].weight[blk];
					if (t1 < 0 && wv < 0.5 * peak && static_cast<int>(t) > pulseFrames) t1 = static_cast<int>(t);
					if (t2 < 0 && wv < 0.02 && static_cast<int>(t) > pulseFrames) t2 = static_cast<int>(t);
				}
				seconds[k] = t1 >= 0 ? (t1 - pulseFrames) / hz : -1.0;
				LogF("%.3f Hz: pulse stops at frame %.0f, weight halves after %.2f s", hz, static_cast<double>(pulseFrames), seconds[k]);
				Assert::IsTrue(peak > 0.3 && t1 > 0, L"the weight is applied and leaves");
			}
			Assert::IsTrue(seconds[0] > 0 && seconds[1] > 0 && seconds[1] / seconds[0] > 0.7 && seconds[1] / seconds[0] < 1.4, L"the age-out takes the same seconds at 24 and 60 Hz");
		}

		TEST_METHOD(TemporalDetailCameraSourcePruneAndViewLifetime)
		{
#ifdef TEMPORAL_DETAIL_HAVE_CAMERA_MODEL
			CameraModel::Policy policy;
			policy.enabled = true;
			policy.threshold = 0.5;
			auto record = [](uint8_t labelValue)
			{
				auto blocks = std::make_shared<CameraModel::BlockData>();
				blocks->gridW = 4; blocks->gridH = 3; blocks->blockSize = 16.0;
				blocks->label.assign(12, labelValue);
				auto r = std::make_shared<CameraModel::CameraMotion>();
				r->valid = true;
				r->reason = CameraModel::Reason::Ok;
				r->confidence = 1.0;
				r->blocks = blocks;
				return std::shared_ptr<const CameraModel::CameraMotion>(r);
			};
			CameraModelMotionSource src(policy);
			for (uint64_t q = 1; q <= 5; ++q) src.SetCamera(q, record(1));
			src.Prune(3);
			Assert::AreEqual(size_t{ 3 }, src.CameraCount(), L"seqs 3..5 kept");
			CameraPairView v;
			Assert::IsFalse(src.Camera(2, v), L"old seq pruned");
			Assert::IsTrue(src.Camera(3, v) && src.Camera(5, v), L"newer seqs kept");
			// The view stays valid after its seq is replaced and after a prune.
			Assert::IsTrue(src.Camera(4, v) && v.label != nullptr);
			src.SetCamera(4, record(0));
			src.Prune(100);
			Assert::AreEqual(size_t{ 0 }, src.CameraCount());
			for (int i = 0; i < 12; ++i) Assert::AreEqual(1, static_cast<int>(v.label[i]), L"view's labels intact");
#else
			Assert::Fail(L"TEMPORAL_DETAIL_HAVE_CAMERA_MODEL must be defined");
#endif
		}

		TEST_METHOD(TemporalDetailCameraAdapterFallback)
		{
			// A test source that adds a camera view (the real adapter needs the camera branch's header).
			struct CamSource : FieldMotionSource
			{
				bool usable = true;
				double pan = 0.0;
				std::vector<uint8_t> labels = std::vector<uint8_t>(8 * 6, 1);
				bool Camera(uint64_t, CameraPairView& v) const override
				{
					if (!usable) return false;
					v = CameraPairView();
					v.usable = true;
					v.h[2] = pan;   // frame N-1 -> N: the content moves by +pan
					v.confidence = 1.0;
					v.gridW = 8; v.gridH = 6; v.blockSize = 16.0;
					v.label = labels.data();
					v.pictureCx = 0.5 * kW; v.pictureCy = 0.5 * kH;
					return true;
				}
			};
			const int n = 40;
			const double pan = 0.37;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), pan);
			FieldMotionSource field;
			SetPan(field, n, pan, 0.0);
			CamSource noCam;
			noCam.usable = false;
			SetPan(noCam, n, pan, 0.0);
			CamSource cam;
			cam.pan = pan;
			SetPan(cam, n, pan, 0.0);
			const Run a = GoSteady(s.frames, &field), b = GoSteady(s.frames, &noCam), c = GoSteady(s.frames, &cam);
			double worst = 0.0;
			int camBlocks = 0;
			for (size_t t = 0; t < static_cast<size_t>(n); ++t)
			{
				Assert::IsTrue(Same(a.out[t], b.out[t]), L"no camera view: identical to the field motion");
				for (size_t i = 0; i < a.out[t].v.size(); ++i) worst = (std::max)(worst, std::fabs(a.out[t].v[i] - c.out[t].v[i]));
				for (uint8_t cflag : c.diag[t].camera) camBlocks += cflag;
			}
			LogF("camera view equal to the field: max |difference| %.3g codes", worst);
			Assert::IsTrue(camBlocks > 0, L"the camera labels reach the block diagnostics");
			Assert::IsTrue(worst < 1e-4, L"a usable camera view equal to the field motion gives the field result");
		}

		TEST_METHOD(TemporalDetailBoostFallbackBounded)
		{
			// No motion at all: the store cannot be carried (visible status), so only the bounded structure-only boost can act.
			const int n = 56;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			const Run on = GoSteady(s.frames, nullptr);
			double eOff = 0, eOn = 0, mOff = 0, mOn = 0, maxBoost = 0.0;
			int cnt = 0;
			for (int t = 32; t < n; ++t)
			{
				const size_t u = static_cast<size_t>(t);
				if (t % 8 == 0 || t % 8 == 4) continue;
				eOff += FineEnergy(s.frames[u]); eOn += FineEnergy(on.out[u]);
				mOff += Mse(s.frames[u], s.truth[u]); mOn += Mse(on.out[u], s.truth[u]);
				for (double b : on.diag[u].boost) maxBoost = (std::max)(maxBoost, b);
				++cnt;
			}
			LogF("no motion: B-frame fine energy off %.3f on %.3f, MSE off %.3f on %.3f, largest boost %.3f", eOff / cnt, eOn / cnt, mOff / cnt, mOn / cnt, maxBoost);
			Assert::AreEqual(std::string("motion lost"), on.diag.back().status, L"the missing motion is visible in the status");
			Assert::IsTrue(maxBoost > 0.02 && maxBoost <= Steady().boostMax + 1e-12, L"the boost acts and stays within boost_max");
			Assert::IsTrue(eOn >= eOff && eOn <= eOff * (1.0 + Steady().boostMax) * (1.0 + Steady().boostMax) * 1.02, L"fine energy rises by at most the boost's square");
			Assert::IsTrue(mOn <= mOff, L"the boost moves the picture toward the master, never away");
		}

		TEST_METHOD(TemporalDetailSettingsParse)
		{
			Settings s;
			std::vector<std::string> errors;
			std::map<std::string, std::string> kv = { { "temporal_detail", "Steady" }, { "temporal_detail_strength", "0.5" }, { "temporal_detail_max_age", "3" }, { "temporal_detail_boost_max", "0.2" }, { "unknown_key", "x" } };
			Assert::IsTrue(ParseSettings(kv, s, &errors), L"valid keys parse");
			Assert::IsTrue(errors.empty(), L"no errors");
			Assert::IsTrue(s.mode == Mode::Steady, L"mode steady (case-insensitive)");
			Assert::AreEqual(0.5, s.strength, 1e-12, L"strength");
			Assert::AreEqual(3.0, s.maxAgeSeconds, 1e-12, L"max_age");
			Assert::AreEqual(0.2, s.boostMax, 1e-12, L"boost_max");

			const Settings keep = s;
			errors.clear();
			std::map<std::string, std::string> bad = { { "temporal_detail", "maybe" }, { "temporal_detail_strength", "1.01" }, { "temporal_detail_max_age", "0.24" }, { "temporal_detail_boost_max", "abc" } };
			Assert::IsFalse(ParseSettings(bad, s, &errors), L"invalid values fail");
			Assert::AreEqual(static_cast<size_t>(4), errors.size(), L"one error per invalid value");
			Assert::IsTrue(s.mode == keep.mode && s.strength == keep.strength && s.maxAgeSeconds == keep.maxAgeSeconds && s.boostMax == keep.boostMax, L"invalid values keep the previous ones");

			// range edges
			struct Edge { const char* key; const char* value; bool ok; };
			const Edge edges[] = {
				{ "temporal_detail_strength", "0", true }, { "temporal_detail_strength", "1", true }, { "temporal_detail_strength", "-0.01", false },
				{ "temporal_detail_max_age", "0.25", true }, { "temporal_detail_max_age", "4", true }, { "temporal_detail_max_age", "4.1", false },
				{ "temporal_detail_boost_max", "0", true }, { "temporal_detail_boost_max", "0.3", true }, { "temporal_detail_boost_max", "0.31", false },
				{ "temporal_detail", "off", true }, { "temporal_detail", "", false }, { "temporal_detail_strength", "nan", false },
			};
			for (const Edge& e : edges)
			{
				Settings t;
				std::map<std::string, std::string> one = { { e.key, e.value } };
				Assert::IsTrue(ParseSettings(one, t, nullptr) == e.ok, (std::wstring(L"range edge ") + std::wstring(e.key, e.key + std::strlen(e.key)) + L"=" + std::wstring(e.value, e.value + std::strlen(e.value))).c_str());
			}
			// Declared tables are consistent (compile-time data the host will map onto core/Products.h).
			Assert::IsTrue(PolicyOf(Counter::Full) == ReaderPolicy::Reset && PolicyOf(Counter::Timing) == ReaderPolicy::Ignore, L"reader policies");
			{   // TemporalDetail's products are registered in core/Products.h (its private descriptors were retired).
				const VpSkeleton::Registry reg = VpSkeleton::SkeletonProducts();
				Assert::IsTrue(reg.Has(VpSkeleton::kTdRefstore) && reg.Has(VpSkeleton::kTdAnalysis) && reg.Has(VpSkeleton::kTdAdmission), L"td products");
				Assert::AreEqual(std::string("td.memory"), std::string(reg.Get(VpSkeleton::kTdRefstore).token), L"id 17 is td.memory in unit v2 (doc 72)");
				Assert::AreEqual(0, static_cast<int>(reg.Get(VpSkeleton::kTdRefstore).history), L"td.memory is the whole budget-governed store (v2): no per-slot history");
			}
			Assert::IsTrue(Shaders::kMeasBody != nullptr && Shaders::kCleanBody != nullptr && Shaders::kWeightBody != nullptr && Shaders::kTransferBody != nullptr, L"shaders declared");
		}

		TEST_METHOD(TemporalDetailBoostAndAutoDefaults)
		{
			// Owner 2026-10-31: the fallback boost is off by default (it was the only part acting on real encodes and cost
			// PSNR); temporal_detail = auto runs only while the source-ID consensus says the stream is low quality, with
			// hysteresis, and is the input itself (bit-exact) while idle.
			Settings def;
			Assert::IsFalse(def.boost, L"boost off by default");
			std::map<std::string, std::string> kv = { { "temporal_detail", "AUTO" }, { "temporal_detail_boost", "on" } };
			Settings p;
			Assert::IsTrue(ParseSettings(kv, p, nullptr) && p.mode == Mode::Auto && p.boost, L"auto and the boost switch parse");
			std::map<std::string, std::string> bad = { { "temporal_detail_boost", "maybe" } };
			Assert::IsFalse(ParseSettings(bad, p, nullptr) || !p.boost, L"an invalid boost value keeps the previous one");

			const int n = 32;
			const Stream st = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			Settings steadyDefault;
			steadyDefault.mode = Mode::Steady;   // boost off
			Model m(steadyDefault);
			double boostSum = 0.0;
			for (int t = 0; t < n; ++t)
			{
				Model::FrameIn in;
				in.luma = st.frames[static_cast<size_t>(t)];
				in.seq = static_cast<uint64_t>(t);
				in.contentHz = kHz;
				in.motion = &motion;
				m.Process(in);
				for (double b : m.Diag().boost) boostSum += b;
			}
			Assert::AreEqual(0.0, boostSum, L"no boost with the default settings");

			// The gate: tier 1 (low) engages after 1 s, tier 3 releases after 3 s, unknown does not engage.
			AutoGate gate;
			int engagedAt = -1, releasedAt = -1;
			for (int t = 0; t < 200; ++t)
			{
				const int tier = t < 10 ? -1 : (t < 100 ? 1 : 3);
				const bool on = gate.Update(tier, kHz);
				if (on && engagedAt < 0) engagedAt = t;
				if (!on && engagedAt >= 0 && releasedAt < 0) releasedAt = t;
			}
			LogF("auto gate: engaged at frame %.0f, released at frame %.0f", engagedAt, releasedAt);
			Log(gate.Status().c_str());
			Assert::AreEqual(10 + Frames(1.0, kHz) - 1, engagedAt, L"engages after the dwell");
			Assert::AreEqual(100 + Frames(3.0, kHz) - 1, releasedAt, L"releases after the longer dwell");
			// Model in auto: idle frames are the input bit-exact; engaged frames process.
			Settings a = Steady();
			a.mode = Mode::Auto;
			Model ma(a);
			bool changedWhileEngaged = false;
			for (int t = 0; t < n; ++t)
			{
				Model::FrameIn in;
				in.luma = st.frames[static_cast<size_t>(t)];
				in.seq = static_cast<uint64_t>(t);
				in.contentHz = kHz;
				in.motion = &motion;
				in.autoEngaged = t >= 8;
				const Plane o = ma.Process(in);
				if (t < 8) Assert::IsTrue(Same(o, in.luma), L"auto idle = the input");
				else if (!Same(o, in.luma)) changedWhileEngaged = true;
			}
			Assert::IsTrue(changedWhileEngaged, L"auto engaged processes");
			// engaged -> idle -> engaged: the store restarts, so the re-engaged run equals a fresh model started there.
			Model mb(a), fresh(a);
			for (int t = 0; t < n; ++t)
			{
				Model::FrameIn in;
				in.luma = st.frames[static_cast<size_t>(t)];
				in.seq = static_cast<uint64_t>(t);
				in.contentHz = kHz;
				in.motion = &motion;
				in.autoEngaged = t < 10 || t >= 14;
				const Plane o = mb.Process(in);
				if (t >= 14)
				{
					const Plane f = fresh.Process(in);
					Assert::IsTrue(Same(o, f), L"re-engaged = a fresh start");
				}
			}
		}

		TEST_METHOD(TemporalDetailMotionSourceIsBounded)
		{
			// Long-run hunt (2026-10-31): the per-sequence pair maps were never pruned. Over 10,000 frames the source stays
			// bounded by its retention window without the host's help, stays at the queue depth when the host prunes the
			// presented frame, survives a seek back, and Clear() empties it; the model keeps working on a pruned source.
			FieldMotionSource m;
			const MotionPair pair = MotionPair::Uniform(kW, kH, 16.0, -0.25, 0.0);
			size_t worst = 0;
			for (uint64_t t = 0; t < 10000; ++t)
			{
				m.SetBackward(t, pair);
				m.SetForward(t, pair);
				worst = (std::max)(worst, m.Size());
			}
			LogF("unpruned host: worst size %.0f entries (retention %.0f frames)", static_cast<double>(worst), static_cast<double>(m.Retention()));
			Assert::IsTrue(worst <= 2 * m.Retention(), L"bounded by the retention window");
			m.SetBackward(5, pair);   // a seek back restarts the window
			Assert::IsTrue(m.Size() <= 2 * m.Retention(), L"bounded after a seek back");
			MotionPair got;
			Assert::IsTrue(m.Backward(5, got), L"the seek target is kept");
			m.Clear();
			Assert::AreEqual(static_cast<size_t>(0), m.Size(), L"Clear empties it");
			// A host that prunes the presented frame (queue 4 ahead) keeps about the queue depth, and the model runs on it.
			const int ahead = 4;
			const Stream s = BuildPan(Pattern("IBBBPBBB", 48), 0.25, 0.0);
			Model model(Steady());
			size_t worstPruned = 0;
			for (uint64_t t = 0; t < 10000; ++t)
			{
				m.SetBackward(t + ahead, pair);
				m.SetForward(t + ahead, pair);
				if (t == 0)
					for (uint64_t k = 0; k < static_cast<uint64_t>(ahead); ++k) { m.SetBackward(k, pair); m.SetForward(k, pair); }
				if (t < 48)
				{
					Model::FrameIn in;
					in.luma = s.frames[static_cast<size_t>(t)];
					in.seq = t;
					in.contentHz = kHz;
					in.motion = &m;
					model.Process(in);
					Assert::IsTrue(model.Diag().status.find("motion lost") == std::string::npos, L"the presented pair is still there");
				}
				m.Prune(t);
				worstPruned = (std::max)(worstPruned, m.Size());
			}
			LogF("pruning host: worst size %.0f entries", static_cast<double>(worstPruned));
			Assert::IsTrue(worstPruned <= static_cast<size_t>(2 * (ahead + 1)), L"the queue depth only");
		}

		TEST_METHOD(TemporalDetailActivePictureKeepsBarsAndEdges)
		{
			// Owner 2026-10-31 (the detail pulse's bar halos): with an active picture (letterbox, pillarbox), bar pixels stay
			// bit-exact, no filter tap reaches across the bar boundary (the picture's output does not depend on the bar's
			// level), and no undershoot / overshoot appears within 8 px of the boundary; a pan runs next to the bar.
			struct Case { const char* name; int pw, ph; ActiveRect rect; };
			const Case cases[] = { { "letterbox", kW, kH + 32, { 0, 16, kW, kH + 16 } }, { "pillarbox", kW + 32, kH, { 16, 0, kW + 16, kH } } };
			for (const Case& c : cases)
			{
				const int n = 24;
				const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.6, 0.0);
				FieldMotionSource motion;
				for (int t = 0; t < n; ++t)
				{
					motion.SetBackward(static_cast<uint64_t>(t), MotionPair::Uniform(c.pw, c.ph, 16.0, -0.6, 0.0));
					motion.SetForward(static_cast<uint64_t>(t), MotionPair::Uniform(c.pw, c.ph, 16.0, 0.6, 0.0));
				}
				Plane outs[2][n], alone[n];
				{
					// The picture alone as a plane: the active picture must be processed exactly like it.
					Model m(Steady());
					for (int t = 0; t < n; ++t)
					{
						Model::FrameIn in;
						in.luma = s.frames[static_cast<size_t>(t)];
						in.seq = static_cast<uint64_t>(t);
						in.contentHz = kHz;
						in.motion = &motion;
						alone[t] = m.Process(in);
					}
				}
				for (int bar = 0; bar < 2; ++bar)
				{
					Model m(Steady());
					for (int t = 0; t < n; ++t)
					{
						Model::FrameIn in;
						in.luma = Plane(c.pw, c.ph, bar ? 900.0 : 16.0);
						for (int y = 0; y < kH; ++y)
							for (int x = 0; x < kW; ++x) in.luma.at(c.rect.left + x, c.rect.top + y) = s.frames[static_cast<size_t>(t)].at(x, y);
						in.seq = static_cast<uint64_t>(t);
						in.contentHz = kHz;
						in.motion = &motion;
						in.active = c.rect;
						outs[bar][t] = m.Process(in);
						const Plane& o = outs[bar][t];
						for (int y = 0; y < c.ph; ++y)
							for (int x = 0; x < c.pw; ++x)
							{
								const bool inside = x >= c.rect.left && x < c.rect.right && y >= c.rect.top && y < c.rect.bottom;
								if (!inside) Assert::IsTrue(o.at(x, y) == in.luma.at(x, y), L"bar pixels bit-exact");
							}
					}
				}
				double worstDep = 0.0, worstEdge = 0.0, worstAlone = 0.0, edgeAlone = 0.0, changed = 0.0;
				for (int t = 0; t < n; ++t)
				{
					const Plane& f = s.frames[static_cast<size_t>(t)];
					double lo = 1e30, hi = -1e30;
					for (double v : f.v) { lo = (std::min)(lo, v); hi = (std::max)(hi, v); }
					for (int y = 0; y < kH; ++y)
						for (int x = 0; x < kW; ++x)
						{
							const double a = outs[0][t].at(c.rect.left + x, c.rect.top + y), b = outs[1][t].at(c.rect.left + x, c.rect.top + y);
							worstDep = (std::max)(worstDep, std::fabs(a - b));
							worstAlone = (std::max)(worstAlone, std::fabs(a - alone[t].at(x, y)));
							changed += std::fabs(a - f.at(x, y)) > 0.05 ? 1.0 : 0.0;
							const bool nearEdge = (c.rect.top > 0 && (y < 8 || y >= kH - 8)) || (c.rect.left > 0 && (x < 8 || x >= kW - 8));
							if (nearEdge)
							{
								worstEdge = (std::max)(worstEdge, (std::max)(lo - a, a - hi));
								edgeAlone = (std::max)(edgeAlone, (std::max)(lo - alone[t].at(x, y), alone[t].at(x, y) - hi));
							}
						}
				}
				char line[200];
				std::snprintf(line, sizeof line, "%s: vs bar level %.3g, vs the picture alone %.3g codes; beyond the frame's range near the boundary %.3g (picture alone %.3g); changed %.0f px",
					c.name, worstDep, worstAlone, worstEdge, edgeAlone, changed);
				Log(line);
				Assert::IsTrue(worstDep == 0.0, L"the picture does not depend on the bars (no tap across the boundary)");
				// The boundary behaves exactly as a plane edge: no undershoot or overshoot the bars add (the detail pulse's halos).
				Assert::IsTrue(worstAlone == 0.0 && worstEdge <= edgeAlone, L"the bar boundary is a plane edge");
				Assert::IsTrue(changed > 0.0, L"the transfer runs inside the picture");
			}
		}

		TEST_METHOD(TemporalDetailFieldSourcePruneBounded)
		{
			// Bug hunt 38 H1: pruning each frame keeps the field source bounded.
			FieldMotionSource src;
			for (uint64_t seq = 0; seq < 10000; ++seq)
			{
				src.SetBackward(seq, MotionPair::Uniform(kW, kH, 16.0, 0.5, 0.25));
				src.SetForward(seq, MotionPair::Uniform(kW, kH, 16.0, -0.5, -0.25));
				src.PruneField(seq >= 8 ? seq - 8 : 0);
				Assert::IsTrue(src.FieldCount() <= 2 * 9, L"field source stays bounded");
			}
			MotionPair out;
			Assert::IsTrue(src.Backward(9999, out) && src.Forward(9999, out), L"newest pair found");
			Assert::IsTrue(src.Backward(9991, out) && src.Forward(9991, out), L"oldest needed pair found");
			Assert::IsFalse(src.Backward(9990, out), L"older pair pruned");
		}

		// --- TD-NONFINITE (gate cb-u1 r1 MUST 3 / MUST 4; README "Non-finite evidence") -----------------------------------------
		// NaN and an infinity are evidence that cannot be compared: the unguarded comparison-based Smoothstep passes NaN
		// through the S form and OPENS the 1 - S form (and +inf opens the S form). Every gate must close instead.

		TEST_METHOD(TemporalDetailNonFiniteEvidenceClosesEveryGate)
		{
			const double values[3] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
			const char* const valueNames[3] = { "NaN", "+inf", "-inf" };
			{
				EvalFixture control;
				const Detail::Eval e = control.Eval();
				Assert::IsTrue(e.hard > 0.1, L"control: a clean, identical, aligned source passes the hard gates");
				Assert::IsTrue(e.corrGate > 0.9 && e.harmGate > 0.9 && e.hbGate > 0.9 && e.focusGate > 0.9 && e.uncGate > 0.9 && e.baseGate > 0.9,
					L"control: every hard gate is open, so a closed gate below is the planted value's doing");
			}
			struct Site { const char* name; std::function<void(EvalFixture&, double)> plant; };
			const std::vector<Site> sites = {
				{ "A.N", [](EvalFixture& f, double v) { f.A.N[0] = v; } },
				{ "A.F", [](EvalFixture& f, double v) { f.A.F[0] = v; } },
				{ "source s1 energy", [](EvalFixture& f, double v) { f.src.s1.at(5, 5) = v; } },
				{ "source s2 energy", [](EvalFixture& f, double v) { f.src.s2.at(5, 5) = v; } },
				{ "source base", [](EvalFixture& f, double v) { f.src.base.at(5, 5) = v; } },
				{ "source u", [](EvalFixture& f, double v) { f.src.u[0] = v; } },
			};
			for (const Site& site : sites)
				for (int k = 0; k < 3; ++k)
				{
					EvalFixture f;
					site.plant(f, values[k]);
					const Detail::Eval e = f.Eval();
					const std::string what = std::string(site.name) + " = " + valueNames[k];
					Assert::IsTrue(e.hard == 0.0, WideOf(what + ": hard is 0").c_str());
					Assert::IsTrue(e.corrGate == 0.0, WideOf(what + ": corr gate is 0").c_str());
					Assert::IsTrue(e.strongerGate == 0.0, WideOf(what + ": stronger gate is 0").c_str());
					Assert::IsTrue(e.harmGate == 0.0, WideOf(what + ": harm gate is 0").c_str());
					Assert::IsTrue(e.hbGate == 0.0, WideOf(what + ": high-band gate is 0").c_str());
					Assert::IsTrue(e.focusGate == 0.0, WideOf(what + ": focus gate is 0").c_str());
					Assert::IsTrue(e.uncGate == 0.0, WideOf(what + ": uncertainty gate is 0").c_str());
					Assert::IsTrue(e.baseGate == 0.0, WideOf(what + ": base gate is 0").c_str());
				}
		}

		TEST_METHOD(TemporalDetailNonFiniteBlockGateFactorsAreNeutral)
		{
			// The regionally adaptive factors are sanitised (non-finite -> 1) before they scale any threshold, so a non-finite
			// factor behaves as the neutral one: the same result as no factors at all, never a poisoned or open gate. A pin of
			// the pre-existing sanitiser contract (SanitizeBlockGate), not a failing-before test.
			// Gate cb-u1 r2 SHOULD 3: at an operating point where each factor CHANGES its gate (a finite control proves it), so
			// "equals neutral" is told apart from "equals a clamp bound": the reference is weaker than the current block
			// (s1 x sqrt(0.8): ratio 0.8, inside the harm ramp of the margin, factor 0), the motion uncertainty is 0.6 x uMax
			// (factor 2) and the base differs by 1.5 x the allowance (factor 1). The strength factor (3) is not read by
			// EvaluateBlock; the GPU texel test covers it end to end.
			const double values[3] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
			const double uMax = kUncertaintyPx * 1.0, allow = kBaseNoiseCodes + kBaseGrainSigmas * std::sqrt(1.0) / 4.0;   // scale 1, N = 1
			auto sensitive = [&](EvalFixture& f)
			{
				for (double& v : f.src.s1.v) v *= std::sqrt(0.8);
				std::fill(f.src.u.begin(), f.src.u.end(), 0.6 * uMax);
				for (double& v : f.src.base.v) v += 1.5 * allow;
			};
			auto withFactor = [&](int k, double v)
			{
				EvalFixture f;
				sensitive(f);
				const size_t nb = static_cast<size_t>(f.A.bw) * static_cast<size_t>(f.A.bh);
				std::vector<float>& g = f.model.*Grab(GatesTag{});
				g.assign(nb * 4, 1.0f);
				g[static_cast<size_t>(k)] = static_cast<float>(v);
				return f.Eval();
			};
			EvalFixture control;
			sensitive(control);
			const Detail::Eval c = control.Eval();
			LogF("sensitive point: harm %.4f, base %.4f, unc %.4f, hard %.4f", c.harmGate, c.baseGate, c.uncGate, c.hard);
			Assert::IsTrue(c.hard > 0.0, L"control: the gates are open");
			Assert::IsTrue(c.harmGate > 0.0 && c.harmGate < 1.0, L"control: the harm gate is inside its ramp");
			Assert::IsTrue(c.baseGate > 0.0 && c.baseGate < 1.0, L"control: the base gate is inside its ramp");
			Assert::IsTrue(c.uncGate > 0.0 && c.uncGate < 1.0, L"control: the uncertainty gate is inside its ramp");
			// Finite control: a factor of 0.5 moves the gate it scales, so a factor that leaked would show.
			Assert::IsTrue(withFactor(0, 0.5).harmGate != c.harmGate, L"control: margin factor 0.5 changes the harm gate");
			Assert::IsTrue(withFactor(1, 0.5).baseGate != c.baseGate, L"control: base allowance factor 0.5 changes the base gate");
			Assert::IsTrue(withFactor(2, 0.5).uncGate != c.uncGate, L"control: uncertainty factor 0.5 changes the uncertainty gate");
			for (int k = 0; k < 3; ++k)
				for (double v : values)
				{
					const Detail::Eval e = withFactor(k, v);
					Assert::IsTrue(std::isfinite(e.hard) && e.hard == c.hard && e.baseGate == c.baseGate && e.uncGate == c.uncGate &&
						e.strongerGate == c.strongerGate && e.harmGate == c.harmGate, L"a non-finite block gate factor equals the neutral factor");
				}
		}

		TEST_METHOD(TemporalDetailNonFiniteStoredSourceKeepsWeightsAndOutputFinite)
		{
			// End to end: a stored reference whose structure, base, motion uncertainty or focus map goes non-finite after the
			// transfer is running (a corrupted upload, an overflow upstream). The block weights, the boost and the picture must
			// stay finite from the next frame on: the gates close instead of passing NaN into the weight limiter.
			const int n = 20, plantAt = 17;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			FieldMotionSource motion;
			SetPan(motion, n, 0.0, 0.0);
			const double values[3] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() };
			const char* const valueNames[3] = { "NaN", "+inf", "-inf" };
			struct Site { const char* name; std::function<void(Detail::Source&, double)> plant; };
			const std::vector<Site> sites = {
				{ "s1", [](Detail::Source& sl, double v) { std::fill(sl.s1.v.begin(), sl.s1.v.end(), v); } },
				{ "s2", [](Detail::Source& sl, double v) { std::fill(sl.s2.v.begin(), sl.s2.v.end(), v); } },
				{ "base", [](Detail::Source& sl, double v) { std::fill(sl.base.v.begin(), sl.base.v.end(), v); } },
				{ "u", [](Detail::Source& sl, double v) { std::fill(sl.u.begin(), sl.u.end(), v); } },
				{ "F", [](Detail::Source& sl, double v) { std::fill(sl.F.begin(), sl.F.end(), v); } },
			};
			for (const Site& site : sites)
				for (int k = 0; k < 3; ++k)
				{
					const std::string what = std::string("stored ") + site.name + " = " + valueNames[k];
					Model m(Steady());
					double warmWeight = 0.0;
					for (int t = 0; t < n; ++t)
					{
						if (t == plantAt)
						{
							std::vector<Detail::Source>& slots = m.*Grab(SlotsTag{});
							Assert::IsTrue(!slots.empty(), WideOf(what + ": the store holds references").c_str());
							for (Detail::Source& sl : slots) site.plant(sl, values[k]);
						}
						Model::FrameIn in;
						in.luma = s.frames[static_cast<size_t>(t)];
						in.seq = static_cast<uint64_t>(t);
						in.contentHz = kHz;
						in.motion = &motion;
						const Plane out = m.Process(in);
						if (t < plantAt)
						{
							for (double w : m.Diag().weight) warmWeight = (std::max)(warmWeight, w);
							continue;
						}
						Assert::IsTrue(AllFinite(m.Diag().weight), WideOf(what + ": every block weight is finite").c_str());
						Assert::IsTrue(AllFinite(m.Diag().boost), WideOf(what + ": every block boost is finite").c_str());
						Assert::IsTrue(AllFinite(out.v), WideOf(what + ": the output picture is finite").c_str());
					}
					Assert::IsTrue(warmWeight > 0.05, WideOf(what + ": the transfer was running before the plant (the test is not vacuous)").c_str());
				}
		}

		TEST_METHOD(TemporalDetailNonFinitePeakKeepsTheBoostStateFinite)
		{
			// MUST 3: a NaN ramp (a NaN peak makes sqrt(max(peak, q) / q) - 1 NaN) passed through Clamp into the boost target,
			// and with the boost already on the hysteresis state bLim became NaN and stayed NaN for good. The boost must close
			// instead, and the state must decay as it would for a zero target.
			const int n = 52, plantAt = 40;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			Model m(Steady());   // boost on, no motion source: only the structure-only boost can act
			for (int t = 0; t < n; ++t)
			{
				if (t == plantAt)
				{
					std::vector<Detail::BlockState>& blocks = m.*Grab(BlocksTag{});
					Assert::IsTrue(!blocks.empty(), L"the block states exist");
					for (Detail::BlockState& st : blocks)
					{
						st.bOn = true;
						st.bLim = 0.3;
						st.qPeak = std::numeric_limits<double>::quiet_NaN();
					}
				}
				Model::FrameIn in;
				in.luma = s.frames[static_cast<size_t>(t)];
				in.seq = static_cast<uint64_t>(t);
				in.contentHz = kHz;
				const Plane out = m.Process(in);
				if (t < plantAt) continue;
				bool stateFinite = true;
				for (const Detail::BlockState& st : m.*Grab(BlocksTag{})) stateFinite = stateFinite && std::isfinite(st.bLim) && st.bLim >= 0.0 && st.bLim <= 1.0;
				Assert::IsTrue(stateFinite, L"the boost limiter state bLim stays finite and in [0, 1] under a NaN peak");
				Assert::IsTrue(AllFinite(m.Diag().boost), L"every block boost is finite under a NaN peak");
				Assert::IsTrue(AllFinite(out.v), L"the output picture is finite under a NaN peak");
			}
		}

		TEST_METHOD(TemporalDetailNonFiniteInfinitePeakNeverOpensTheBoost)
		{
			// Gate cb-u1 r2 MUST 1: a +inf decayed peak (one q = +inf frame) stays +inf for good (inf x decay = inf, no finite q
			// replaces it), and raw = sqrt(inf / q) - 1 = +inf passed the old `Finite(q) && !isnan(raw)` test, so bn = 1: the
			// boost OPENED on every later frame. It must stay closed. The stream is TemporalDetailBoostFallbackBounded's (no
			// motion: only the structure-only boost can act), where the boost gates downstream of raw are open.
			const int n = 56, plantAt = 32;
			const Stream s = BuildPan(Pattern("IBBBPBBB", n), 0.0);
			// infinite = false is the control: a FINITE huge peak (1e6 x the last q) on the same stream. raw is then large and
			// finite, so the boost must act in at least one block (the downstream gates are open: the test is not vacuous).
			auto runPlanted = [&](bool infinite, double& maxBoost, int& plantedKept, bool& closedWhilePlanted)
			{
				Model m(Steady());
				maxBoost = 0.0; plantedKept = 0; closedWhilePlanted = true;
				for (int t = 0; t < n; ++t)
				{
					if (t == plantAt)
					{
						std::vector<Detail::BlockState>& blocks = m.*Grab(BlocksTag{});
						Assert::IsTrue(!blocks.empty(), L"the block states exist");
						for (Detail::BlockState& st : blocks)
						{
							st.bOn = false;
							st.bLim = 0.0;
							st.qPeak = infinite ? std::numeric_limits<double>::infinity() : 1e6 * st.qPrev;
						}
					}
					Model::FrameIn in;
					in.luma = s.frames[static_cast<size_t>(t)];
					in.seq = static_cast<uint64_t>(t);
					in.contentHz = kHz;
					const Plane out = m.Process(in);
					if (t < plantAt) continue;
					const std::vector<Detail::BlockState>& blocks = m.*Grab(BlocksTag{});
					const std::vector<double>& boost = m.Diag().boost;
					Assert::IsTrue(AllFinite(boost) && AllFinite(out.v), L"the boost and the picture stay finite");
					plantedKept = 0;
					for (size_t i = 0; i < blocks.size() && i < boost.size(); ++i)
					{
						maxBoost = (std::max)(maxBoost, boost[i]);
						if (!std::isinf(blocks[i].qPeak)) continue;   // a re-initialised block state is no longer planted
						++plantedKept;
						if (boost[i] != 0.0 || blocks[i].bLim != 0.0 || blocks[i].bOn) closedWhilePlanted = false;
					}
				}
			};
			double controlBoost = 0.0, infBoost = 0.0;
			int controlKept = 0, infKept = 0;
			bool controlClosed = true, infClosed = true;
			runPlanted(false, controlBoost, controlKept, controlClosed);
			runPlanted(true, infBoost, infKept, infClosed);
			LogF("finite huge peak: largest boost %.4f; +inf peak: largest boost %.4f, blocks still planted at the end %.0f", controlBoost, infBoost, infKept);
			Assert::IsTrue(controlBoost > 0.0, L"control: a finite huge peak opens the boost in at least one block (the downstream gates are open)");
			Assert::IsTrue(infKept > 0, L"the +inf peak is still held at the last frame (the planted path was exercised)");
			Assert::IsTrue(infClosed, L"while a block holds a +inf peak its boost, bLim and bOn stay 0 on every frame");
			Assert::IsTrue(infBoost == 0.0, L"the +inf peak run never boosts any block");
		}

		TEST_METHOD(TemporalDetailGateRampsCloseOnNonFiniteAndEqualSmoothstepWhenFinite)
		{
			// RampMath::GateS / GateInv (core/Ramp.h, the U25 owner): non-finite x gives 0 in both forms (the S form does not
			// pass NaN, the 1 - S form does not open on it), and for a finite x with ascending edges both are the temporal-
			// detail Smoothstep bit for bit (same arithmetic), so no finite behaviour moved with the TD-NONFINITE change.
			const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
			for (double x : { nan, inf, -inf })
			{
				Assert::IsTrue(RampMath::GateS(0.3, 0.5, x) == 0.0, L"GateS(double) closes on a non-finite x");
				Assert::IsTrue(RampMath::GateInv(0.3, 0.5, x) == 0.0, L"GateInv(double) closes on a non-finite x");
				Assert::IsTrue(RampMath::GateS(0.3f, 0.5f, static_cast<float>(x)) == 0.0f, L"GateS(float) closes on a non-finite x");
				Assert::IsTrue(RampMath::GateInv(0.3f, 0.5f, static_cast<float>(x)) == 0.0f, L"GateInv(float) closes on a non-finite x");
				Assert::IsTrue(TemporalDetail::GateS(0.3, 0.5, x) == 0.0 && TemporalDetail::GateInv(0.3, 0.5, x) == 0.0, L"the temporal-detail names are the same gates");
			}
			const double edges[][2] = { { 0.0, 1.0 }, { 0.3, 0.5 }, { 0.45, 0.65 }, { 0.5 * kCorrMin, kCorrMin }, { 1.2, 3.6 }, { -2.0, 5.0 }, { 0.0, 0.5 }, { kRho2Lo, kRho2Hi } };
			int compared = 0;
			for (const auto& e : edges)
			{
				std::vector<double> xs = { e[0], e[1], 0.5 * (e[0] + e[1]), -1e9, 1e9 };
				for (double x = -3.0; x <= 6.0; x += 0.0137) xs.push_back(x);
				for (double x : xs)
				{
					const double ref = RampMath::Smoothstep<double>(e[0], e[1], x, RampMath::SmoothRule::Unguarded);
					const double inv = 1.0 - ref;
					const double s = RampMath::GateS(e[0], e[1], x), g = RampMath::GateInv(e[0], e[1], x);
					Assert::IsTrue(std::memcmp(&ref, &s, sizeof ref) == 0, L"GateS is bit-identical to the temporal-detail Smoothstep on finite input");
					Assert::IsTrue(std::memcmp(&inv, &g, sizeof inv) == 0, L"GateInv is bit-identical to 1 - Smoothstep on finite input");
					++compared;
				}
			}
			Assert::IsTrue(compared > 1000, L"a dense grid of finite inputs was compared");
		}

		// W23-B3 M2#3 (U2 B5): compile-time static_assert pinning TD's band sigma constants to GrainSplit owner
		// defaults (VpUnit::BandFromBase::kSigma*), plus runtime parity of TD's Analyse() vs GrainSplit::Compute()
		// and a same-input negative control proving altered sigmas shift the band decomposition.
		TEST_METHOD(TemporalDetailSigmaConstantsEqualGrainSplitOwnerDefaults)
		{
			constexpr GrainSplit::Params kOwner = GrainSplit::DefaultParams();
			static_assert(kOwner.sigmaFine == VpUnit::BandFromBase::kSigmaFine,
				"GrainSplit::DefaultParams().sigmaFine must equal VpUnit::BandFromBase::kSigmaFine");
			static_assert(kOwner.sigmaBase == VpUnit::BandFromBase::kSigmaBase,
				"GrainSplit::DefaultParams().sigmaBase must equal VpUnit::BandFromBase::kSigmaBase");
			static_assert(kOwner.sigmaLow == VpUnit::BandFromBase::kSigmaLow,
				"GrainSplit::DefaultParams().sigmaLow must equal VpUnit::BandFromBase::kSigmaLow");
			static_assert(kOwner.sigmaFine == 1.0 && kOwner.sigmaBase == 2.5 && kOwner.sigmaLow == 6.0,
				"owner band sigmas must remain the canonical U2 defaults (1.0, 2.5, 6.0)");
			static_assert(TemporalDetail::kBlockPx == kOwner.blockPx,
				"TemporalDetail::kBlockPx must equal GrainSplit::DefaultParams().blockPx");

			const GrainSplit::Params tdParams = TemporalDetail::Detail::SplitParams();
			Assert::AreEqual(kOwner.sigmaFine, tdParams.sigmaFine, 0.0, L"TD SplitParams().sigmaFine matches owner default");
			Assert::AreEqual(kOwner.sigmaBase, tdParams.sigmaBase, 0.0, L"TD SplitParams().sigmaBase matches owner default");
			Assert::AreEqual(kOwner.sigmaLow, tdParams.sigmaLow, 0.0, L"TD SplitParams().sigmaLow matches owner default");
			Assert::AreEqual(kOwner.blockPx, tdParams.blockPx, 0.0, L"TD SplitParams().blockPx matches owner default");
			Assert::AreEqual(kOwner.subBlocks, tdParams.subBlocks, L"TD SplitParams().subBlocks matches owner default");
			Assert::AreEqual(kOwner.quietSubBlocks, tdParams.quietSubBlocks, L"TD SplitParams().quietSubBlocks matches owner default");
			Assert::AreEqual(kOwner.neighbourhood, tdParams.neighbourhood, L"TD SplitParams().neighbourhood matches owner default");
			Assert::AreEqual(kOwner.grainCal, tdParams.grainCal, 0.0, L"TD SplitParams().grainCal matches owner default");
			Assert::AreEqual(kOwner.grainCal2, tdParams.grainCal2, 0.0, L"TD SplitParams().grainCal2 matches owner default");
			Assert::AreEqual(kOwner.kappa, tdParams.kappa, 0.0, L"TD SplitParams().kappa matches owner default");

			// Synthetic textured + edge + grain luma plane (10-bit codes).
			const int w = 64, h = 48;
			Plane y(w, h);
			uint32_t rng = 0x7711aa33u;
			for (int py = 0; py < h; ++py)
				for (int px = 0; px < w; ++px)
				{
					rng = rng * 1664525u + 1013904223u;
					const double grain = (static_cast<double>((rng >> 8) & 1023u) / 1023.0 - 0.5) * 16.0;
					const double structure = 512.0 + 48.0 * std::sin(2.0 * kPi * px / 19.0) * std::cos(2.0 * kPi * py / 17.0)
						+ ((px >= 20 && px < 44 && py >= 14 && py < 34) ? 60.0 : 0.0);
					y.at(px, py) = structure + grain;
				}

			const TemporalDetail::Detail::Analysis tdAnalysis = TemporalDetail::Detail::Analyse(y, 1.0, 16);
			const GrainSplit::Split ownerSplit = GrainSplit::Compute(y, 1.0, 16, kOwner);
			Assert::AreEqual(ownerSplit.base.v.size(), tdAnalysis.base.v.size());
			Assert::IsTrue(std::memcmp(tdAnalysis.base.v.data(), ownerSplit.base.v.data(), ownerSplit.base.v.size() * sizeof(double)) == 0,
				L"TD Analyse().base matches GrainSplit::Compute(DefaultParams()).base bit-for-bit");
			Assert::IsTrue(std::memcmp(tdAnalysis.s1.v.data(), ownerSplit.s1.v.data(), ownerSplit.s1.v.size() * sizeof(double)) == 0,
				L"TD Analyse().s1 matches GrainSplit::Compute(DefaultParams()).s1 bit-for-bit");
			Assert::IsTrue(std::memcmp(tdAnalysis.s2.v.data(), ownerSplit.s2.v.data(), ownerSplit.s2.v.size() * sizeof(double)) == 0,
				L"TD Analyse().s2 matches GrainSplit::Compute(DefaultParams()).s2 bit-for-bit");
			Assert::IsTrue(std::memcmp(tdAnalysis.grain.v.data(), ownerSplit.grain.v.data(), ownerSplit.grain.v.size() * sizeof(double)) == 0,
				L"TD Analyse().grain matches GrainSplit::Compute(DefaultParams()).grain bit-for-bit");

			// Same-input negative control (Rule 3): altering sigmaFine & sigmaBase measurably shifts band outputs.
			GrainSplit::Params altered = kOwner;
			altered.sigmaFine = kOwner.sigmaFine * 1.5;
			altered.sigmaBase = kOwner.sigmaBase * 1.5;
			const GrainSplit::Split shifted = GrainSplit::Compute(y, 1.0, 16, altered);
			double maxBaseShift = 0.0, maxS1Shift = 0.0, maxS2Shift = 0.0;
			for (size_t i = 0; i < y.v.size(); ++i)
			{
				maxBaseShift = (std::max)(maxBaseShift, std::fabs(shifted.base.v[i] - tdAnalysis.base.v[i]));
				maxS1Shift = (std::max)(maxS1Shift, std::fabs(shifted.s1.v[i] - tdAnalysis.s1.v[i]));
				maxS2Shift = (std::max)(maxS2Shift, std::fabs(shifted.s2.v[i] - tdAnalysis.s2.v[i]));
			}
			Assert::IsTrue(maxBaseShift > 1.0 && maxS1Shift > 1.0 && maxS2Shift > 1.0,
				L"negative control: altering band sigmas shifts base, s1, and s2 by > 1 code");
		}

		// W23-B3 M2#7 (15b A4 rest): TemporalDetail lag-1 grain guard acquires shared kP10GrainSplit at seq - 1
		// (AcquireForm::Cpu), accepts double and float payloads, genuinely feeds lag-1 guard sums from the acquired
		// payload (no private m_prevGin), and visibly refuses when memo is null, unregistered, missing seq - 1, or
		// has a corrupted payload size.
		TEST_METHOD(TemporalDetailLag1AcquiresP10AndRefusesVisiblyWhenAbsent)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			// Synthetic moving + grainy 3-frame sequence (seq = 1, 2, 3) with exact uniform pan motion.
			const std::vector<double> q = { 1.0, 0.3, 0.3 };
			const Stream s = BuildPan(q, 2.0, 0.0, 4101u, 1.8);
			FieldMotionSource motion;
			for (uint64_t seq = 1u; seq <= 3u; ++seq)
			{
				motion.SetBackward(seq, MotionPair::Uniform(kW, kH, 16.0, -2.0, 0.0));
				motion.SetForward(seq, MotionPair::Uniform(kW, kH, 16.0, 2.0, 0.0));
			}

			// Independent CPU grain planes packed via TemporalDetail::PackCpuGrainPlane for seq = 1, 2, 3.
			std::map<uint64_t, std::vector<uint8_t>> packedBySeq;
			for (uint64_t seq = 1u; seq <= 3u; ++seq)
			{
				const Detail::Analysis a = Detail::Analyse(s.frames[static_cast<size_t>(seq - 1u)], 1.0, 16);
				packedBySeq[seq] = PackCpuGrainPlane(a.grain);
			}

			auto runSeq = [&](Model& model, std::vector<Diagnostics>& diags) {
				diags.clear();
				for (uint64_t seq = 1u; seq <= 3u; ++seq)
				{
					Model::FrameIn in;
					in.luma = s.frames[static_cast<size_t>(seq - 1u)];
					in.seq = seq;
					in.contentHz = kHz;
					in.planeLines = 1080.0;
					in.motion = &motion;
					model.Process(in);
					diags.push_back(model.Diag());
				}
			};

			// 1. Positive path: double-packed kP10GrainSplit published for seq = 1, 2, 3.
			{
				VpSkeleton::SkeletonMemo memo;
				VpSkeleton::OwnerFill fill;
				fill.fill = [&](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					auto it = packedBySeq.find(req.seq);
					if (it == packedBySeq.end())
						return false;
					out.bytes = it->second;
					return true;
				};
				Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP10GrainSplit), std::move(fill)) == Reg::Registered,
					L"CPU kP10GrainSplit owner registered");

				Model model(Steady());
				model.SetMemo(&memo);
				std::vector<Diagnostics> diags;
				runSeq(model, diags);

				Assert::IsFalse(diags[0].lag1Missing, L"seq=1 has no prior frame (lag-1 not attempted)");
				Assert::IsFalse(diags[1].lag1Missing, L"seq=2 acquired lag-1 P10 (seq=1)");
				Assert::IsFalse(diags[2].lag1Missing, L"seq=3 acquired lag-1 P10 (seq=2)");
				Assert::AreEqual(uint64_t(2), model.Lag1Hits(), L"seq=2 and seq=3 both hit lag-1 kP10GrainSplit");
				Assert::AreEqual(uint64_t(0), model.Lag1UnavailableCount(), L"zero lag-1 unavailable refusals when present");
				Assert::IsTrue(diags[1].lag1UnavailableWhy.empty() && diags[2].lag1UnavailableWhy.empty(),
					L"no refusal reason on lag-1 hits");
			}

			// 1b. Positive path (float32 wire format): float[w*h] payload is also cleanly accepted.
			{
				VpSkeleton::SkeletonMemo floatMemo;
				VpSkeleton::OwnerFill fFill;
				fFill.fill = [&](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.seq < 1u || req.seq > 3u)
						return false;
					const Detail::Analysis a = Detail::Analyse(s.frames[static_cast<size_t>(req.seq - 1u)], 1.0, 16);
					out.bytes.resize(a.grain.v.size() * sizeof(float));
					for (size_t i = 0; i < a.grain.v.size(); ++i)
					{
						const float v = static_cast<float>(a.grain.v[i]);
						std::memcpy(out.bytes.data() + i * sizeof(float), &v, sizeof(float));
					}
					return true;
				};
				Assert::IsTrue(floatMemo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP10GrainSplit), std::move(fFill)) == Reg::Registered);
				Model fModel(Steady());
				fModel.SetMemo(&floatMemo);
				std::vector<Diagnostics> fDiags;
				runSeq(fModel, fDiags);
				Assert::AreEqual(uint64_t(2), fModel.Lag1Hits(), L"float-packed lag-1 P10 hits on seq=2 and seq=3");
				Assert::AreEqual(uint64_t(0), fModel.Lag1UnavailableCount());
			}

			// 2. Negative control (a): absent lag-1 P10 (memo=nullptr, unregistered memo, and missing seq-1 frame)
			//    visibly refuses (lag1Missing == true, Lag1UnavailableCount >= 1, non-empty lag1UnavailableWhy).
			{
				// (a1) memo == nullptr
				Model nullModel(Steady());
				std::vector<Diagnostics> nullDiags;
				runSeq(nullModel, nullDiags);
				Assert::AreEqual(uint64_t(0), nullModel.Lag1Hits(), L"memo=nullptr records 0 lag-1 hits");
				Assert::AreEqual(uint64_t(2), nullModel.Lag1UnavailableCount(), L"memo=nullptr refuses on seq=2 and seq=3");
				Assert::IsTrue(nullDiags[1].lag1Missing && nullDiags[2].lag1Missing, L"memo=nullptr flags lag1Missing");
				Assert::IsFalse(nullDiags[1].lag1UnavailableWhy.empty(), L"memo=nullptr records visible reason");
				Assert::IsFalse(nullModel.LastLag1UnavailableWhy().empty(), L"model exposes LastLag1UnavailableWhy");

				// (a2) unregistered memo
				VpSkeleton::SkeletonMemo emptyMemo;
				Model unregModel(Steady());
				unregModel.SetMemo(&emptyMemo);
				std::vector<Diagnostics> unregDiags;
				runSeq(unregModel, unregDiags);
				Assert::AreEqual(uint64_t(0), unregModel.Lag1Hits());
				Assert::AreEqual(uint64_t(2), unregModel.Lag1UnavailableCount());
				Assert::IsTrue(unregDiags[1].lag1Missing && !unregDiags[1].lag1UnavailableWhy.empty(),
					L"unregistered memo visibly refuses lag-1 P10");

				// (a3) missing seq-1 frame: serve seq=1 only, decline seq=2 -> seq=2 hits, seq=3 refuses visibly.
				VpSkeleton::SkeletonMemo partialMemo;
				VpSkeleton::OwnerFill partialFill;
				partialFill.fill = [&](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.seq != 1u)
						return false;
					out.bytes = packedBySeq[1u];
					return true;
				};
				Assert::IsTrue(partialMemo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP10GrainSplit), std::move(partialFill)) == Reg::Registered);
				Model partialModel(Steady());
				partialModel.SetMemo(&partialMemo);
				std::vector<Diagnostics> partialDiags;
				runSeq(partialModel, partialDiags);
				Assert::AreEqual(uint64_t(1), partialModel.Lag1Hits(), L"seq=2 hits seq=1");
				Assert::AreEqual(uint64_t(1), partialModel.Lag1UnavailableCount(), L"seq=3 visibly refuses missing seq=2");
				Assert::IsFalse(partialDiags[1].lag1Missing, L"seq=2 not missing");
				Assert::IsTrue(partialDiags[2].lag1Missing && !partialDiags[2].lag1UnavailableWhy.empty(),
					L"seq=3 flags lag1Missing with non-empty reason when seq=2 absent");
			}

			// 3. Negative control (b): corrupted payload size in kP10GrainSplit CPU slot is visibly rejected.
			{
				VpSkeleton::SkeletonMemo badMemo;
				VpSkeleton::OwnerFill badFill;
				badFill.fill = [](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					out.bytes.assign(17u, uint8_t(0x5A));   // invalid byte count: neither double[w*h] nor float[w*h]
					return true;
				};
				Assert::IsTrue(badMemo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP10GrainSplit), std::move(badFill)) == Reg::Registered);
				Model badModel(Steady());
				badModel.SetMemo(&badMemo);
				std::vector<Diagnostics> badDiags;
				runSeq(badModel, badDiags);
				Assert::AreEqual(uint64_t(0), badModel.Lag1Hits(), L"corrupted payload never counts as a hit");
				Assert::AreEqual(uint64_t(2), badModel.Lag1UnavailableCount(), L"corrupted payload rejected on seq=2 and seq=3");
				Assert::IsTrue(badDiags[1].lag1Missing, L"corrupted payload sets lag1Missing");
				Assert::IsTrue(badDiags[1].lag1UnavailableWhy.find("size mismatch") != std::string::npos,
					L"corrupted payload refusal reason explicitly states size mismatch");
			}

			// 4. Negative control (c): prove lag-1 guard sums genuinely consume acquired kP10GrainSplit (not a
			//    stale/private buffer). Publishing negated frame-2 grain under seq=1 drives rho_in = -1 -> freeze = +1
			//    -> huge guardViolation (> 10.0), whereas absent lag-1 P10 skips lag-1 sums (guardViolation < 1.0).
			{
				const Stream staticScene = KeepsGrainScene(4102u, 2.0, 3);
				FieldMotionSource zeroMotion;
				for (uint64_t seq = 1u; seq <= 3u; ++seq)
				{
					zeroMotion.SetBackward(seq, MotionPair::Uniform(kW, kH, 16.0, 0.0, 0.0));
					zeroMotion.SetForward(seq, MotionPair::Uniform(kW, kH, 16.0, 0.0, 0.0));
				}
				const Detail::Analysis a2 = Detail::Analyse(staticScene.frames[1], 1.0, 16);
				Plane negatedGrain(kW, kH);
				for (size_t i = 0; i < a2.grain.v.size(); ++i)
					negatedGrain.v[i] = -a2.grain.v[i];
				const std::vector<uint8_t> poisonedBytes = PackCpuGrainPlane(negatedGrain);

				VpSkeleton::SkeletonMemo poisonMemo;
				VpSkeleton::OwnerFill poisonFill;
				poisonFill.fill = [&](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.seq != 1u) return false;
					out.bytes = poisonedBytes;
					return true;
				};
				Assert::IsTrue(poisonMemo.RegisterOwnerFill(VpSkeleton::kP10GrainSplit, VpSkeleton::AcquireForm::Cpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP10GrainSplit), std::move(poisonFill)) == Reg::Registered);

				auto maxViolationAtSeq2 = [&](VpSkeleton::SkeletonMemo* m) -> double {
					Model mod(Steady());
					mod.SetMemo(m);
					for (uint64_t seq = 1u; seq <= 2u; ++seq)
					{
						Model::FrameIn in;
						in.luma = staticScene.frames[static_cast<size_t>(seq - 1u)];
						in.seq = seq;
						in.contentHz = kHz;
						in.planeLines = 1080.0;
						in.motion = &zeroMotion;
						mod.Process(in);
					}
					double worst = 0.0;
					for (double v : mod.Diag().guardViolation)
						worst = (std::max)(worst, v);
					return worst;
				};

				const double violWithPoisonP10 = maxViolationAtSeq2(&poisonMemo);
				const double violWithAbsentP10 = maxViolationAtSeq2(nullptr);
				Assert::IsTrue(violWithPoisonP10 > 10.0,
					L"acquired lag-1 P10 feeds sb[3..6] directly (negated lag-1 grain trips freeze violation > 10)");
				Assert::IsTrue(violWithAbsentP10 < 1.0,
					L"absent lag-1 P10 skips lag-1 guard sums instead of silently reusing a private/stale buffer");
			}
		}
	};
}
