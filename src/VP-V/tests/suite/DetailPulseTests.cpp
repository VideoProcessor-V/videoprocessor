#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/DetailPulseModel.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>
#include <VP-V/tests/support/GrainReferenceModel.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>

#include <VP-V/modules/repair/texture_restore/DetailPulse.h>
#include <VP-V/modules/control/skeleton/gpu/OwnerGpuDevice.h>
#include <VP-V/modules/input/source_analysis/regions/RegionEvidence.h>
#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>
#include <VP-V/modules/motion/estimation/global_model/link/CameraRecordPool.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>
#include <VP-V/modules/repair/grain_split/BandMemoFill.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// detail_pulse_stabilise (docs/VP-detail-restore.md; DetailPulse.h). CPU tests of the reference
// model (DetailPulseModel.h, mirroring the shader), GPU tests of the shader against the model
// (names start with DetailPulseGpu; WARP unless VP_TEST_GPU=hardware) and a real-clip test
// (DetailPulseRealClip; only with VP_TEST_REAL_FOOTAGE set to the folder holding breathing\).
//
// The coded sequences simulate a starved GOP on a static picture: every frame gets fresh grain,
// then 8x8 DCT quantisation, fine on I-frames, coarse on B-frames, P between - so the grain and
// fine texture pulse with the GOP as in the research material.
namespace DetailPulseTestsNs
{
	using GrainModel::Image;
	using DetailPulseModel::Field;
	using DetailPulseModel::Frame;
	using DetailPulseModel::Model;
	using DetailPulseModel::Settings;

	// A static picture (10-bit codes): a gentle gradient and fine texture in both bands.
	inline double Picture(double x, double y, int variant = 0)
	{
		if (variant == 0)
			return 512.0 + 40.0 * std::sin(6.283185307 * x / 211.0) + 40.0 * std::sin(6.283185307 * y / 173.0 + 1.0) +
				10.0 * std::sin(6.283185307 * x / 7.3 + 0.4) * std::sin(6.283185307 * y / 9.1) +
				6.0 * std::sin(6.283185307 * (x + y) / 4.7);
		return 300.0 + 90.0 * std::sin(6.283185307 * x / 61.0 + 2.0) * std::cos(6.283185307 * y / 47.0) +
			12.0 * std::sin(6.283185307 * (x - y) / 5.9);
	}

	inline Image PictureImage(int w, int h, int variant = 0)
	{
		Image p(w, h);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				p.at(x, y) = Picture(x + 0.5, y + 0.5, variant);
		return p;
	}

	// Orthonormal 8x8 DCT quantisation as an encoder does it: every coefficient but DC to a
	// multiple of q with a dead zone (rounding offset 1/3 intra, 1/6 inter: small coefficients -
	// grain, fine texture - go to zero, so coarse frames lose detail energy).
	inline Image Quantise(const Image& img, double q, double offset)
	{
		double c[8][8];
		for (int u = 0; u < 8; ++u)
			for (int x = 0; x < 8; ++x)
				c[u][x] = (u == 0 ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0)) * std::cos((2 * x + 1) * u * 3.14159265358979 / 16.0);
		Image out = img;
		for (int by = 0; by + 8 <= img.h; by += 8)
			for (int bx = 0; bx + 8 <= img.w; bx += 8)
			{
				double blk[8][8], tmp[8][8], co[8][8];
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x) blk[y][x] = img.at(bx + x, by + y);
				for (int y = 0; y < 8; ++y)
					for (int u = 0; u < 8; ++u)
					{
						double a = 0.0;
						for (int x = 0; x < 8; ++x) a += c[u][x] * blk[y][x];
						tmp[y][u] = a;
					}
				for (int v = 0; v < 8; ++v)
					for (int u = 0; u < 8; ++u)
					{
						double a = 0.0;
						for (int y = 0; y < 8; ++y) a += c[v][y] * tmp[y][u];
						co[v][u] = (u == 0 && v == 0) ? a : (a < 0.0 ? -1.0 : 1.0) * q * std::floor(std::fabs(a) / q + offset);
					}
				for (int v = 0; v < 8; ++v)
					for (int x = 0; x < 8; ++x)
					{
						double a = 0.0;
						for (int u = 0; u < 8; ++u) a += c[u][x] * co[v][u];
						tmp[v][x] = a;
					}
				for (int y = 0; y < 8; ++y)
					for (int x = 0; x < 8; ++x)
					{
						double a = 0.0;
						for (int v = 0; v < 8; ++v) a += c[v][y] * tmp[v][x];
						out.at(bx + x, by + y) = a;
					}
			}
		return out;
	}

	// Frame types by time: an I-frame at the start of every second, a P-frame every sixth of a
	// second, B-frames between (at 24 fps: I B B B P B B B P ...).
	inline char TypeAt(int k, double hz)
	{
		const int perSecond = static_cast<int>(std::lround(hz));
		const int perP = (std::max)(1, perSecond / 6);
		if (k % perSecond == 0) return 'I';
		if (k % perP == 0) return 'P';
		return 'B';
	}

	inline double StepFor(char type) { return type == 'I' ? 4.0 : type == 'P' ? 8.0 : 14.0; }
	inline double OffsetFor(char type) { return type == 'I' ? 1.0 / 3.0 : 1.0 / 6.0; }

	struct Coded
	{
		Image picture;
		std::vector<Image> frames;
		std::vector<char> types;
	};

	// count coded frames at hz: picture + fresh grain (10-bit codes), quantised by type; rounded
	// to whole codes (the GPU test uploads them exactly).
	inline Coded MakeCoded(int w, int h, int count, double hz, double grainSigma, bool soft, unsigned seed, bool quantise = true)
	{
		std::mt19937 rng(seed);
		Coded c;
		c.picture = PictureImage(w, h);
		for (int k = 0; k < count; ++k)
		{
			const char t = TypeAt(k, hz);
			Image f = c.picture + GrainModel::Grain(w, h, grainSigma, soft, rng);
			if (quantise) f = Quantise(f, StepFor(t), OffsetFor(t));
			for (double& v : f.v) v = std::round((std::max)(0.0, (std::min)(1023.0, v)));
			c.frames.push_back(f);
			c.types.push_back(t);
		}
		return c;
	}

	// e2.py's fine detail: the frame minus a 5x5 box (edges clamped), RMS over the frame.
	inline double HfRms(const Image& a)
	{
		double sq = 0.0;
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x)
			{
				double box = 0.0;
				for (int j = -2; j <= 2; ++j)
					for (int i = -2; i <= 2; ++i) box += a.get(x + i, y + j);
				const double d = a.at(x, y) - box / 25.0;
				sq += d * d;
			}
		return std::sqrt(sq / a.v.size());
	}

	// Breathing (e2.py): the standard deviation of the frame-to-frame HF RMS change over its mean.
	inline double Breathing(const std::vector<double>& h)
	{
		if (h.size() < 3) return 0.0;
		double mean = 0.0;
		for (double v : h) mean += v;
		mean /= h.size();
		std::vector<double> d;
		for (size_t i = 1; i < h.size(); ++i) d.push_back(h[i] - h[i - 1]);
		double md = 0.0;
		for (double v : d) md += v;
		md /= d.size();
		double var = 0.0;
		for (double v : d) var += (v - md) * (v - md);
		return std::sqrt(var / d.size()) / mean;
	}

	inline std::vector<Frame> Run(const std::vector<Image>& frames, const Settings& s, const std::vector<const Field*>* fields = nullptr,
		bool ahead = false, uint64_t firstSequence = 1)
	{
		Model m;
		std::vector<Frame> out;
		for (size_t k = 0; k < frames.size(); ++k)
		{
			if (ahead && k + 1 < frames.size()) m.MeasureAhead(frames[k + 1], firstSequence + k + 1, s);
			out.push_back(m.Process(frames[k], firstSequence + k, s, fields ? (*fields)[k] : nullptr));
		}
		return out;
	}

	inline std::wstring W(const char* text) { return std::wstring(text, text + strlen(text)); }

	inline double MaxAbsDiff(const Image& a, const Image& b, int x0 = 0, int y0 = 0, int x1 = -1, int y1 = -1)
	{
		if (x1 < 0) x1 = a.w;
		if (y1 < 0) y1 = a.h;
		double m = 0.0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) m = (std::max)(m, std::fabs(a.at(x, y) - b.at(x, y)));
		return m;
	}

	// A picture moved by (dx, dy) px, edges clamped (a Stage B phase picture / an extra of a pan).
	inline Image Shifted(const Image& a, int dx, int dy)
	{
		Image s(a.w, a.h);
		for (int y = 0; y < a.h; ++y)
			for (int x = 0; x < a.w; ++x) s.at(x, y) = a.get(x - dx, y - dy);
		return s;
	}

	// 2x2 box down (the crossfade's other re-base, in the tests).
	inline Image HalfBox(const Image& a)
	{
		Image h((a.w + 1) / 2, (a.h + 1) / 2);
		for (int y = 0; y < h.h; ++y)
			for (int x = 0; x < h.w; ++x)
				h.at(x, y) = std::round(0.25 * (a.get(2 * x, 2 * y) + a.get(2 * x + 1, 2 * y) + a.get(2 * x, 2 * y + 1) + a.get(2 * x + 1, 2 * y + 1)));
		return h;
	}

	// The scene readers of the shared cut / fade judge (registered fix D8). The unit tests run without a skeleton store, so the
	// default reader is replaced for the lifetime of a PulseGpu: no frame is a cut or a fade.
	bool NoCutReader(uint64_t, FrameAnalysis::Scene& scene) { scene = FrameAnalysis::Scene(); return true; }
	bool MissingReader(uint64_t, FrameAnalysis::Scene&) { return false; }
	uint64_t g_cutAt = 0, g_fadeAt = 0;
	bool CutOrFadeAtReader(uint64_t sequence, FrameAnalysis::Scene& scene)
	{
		scene = FrameAnalysis::Scene();
		scene.cut = sequence == g_cutAt;
		scene.fade = sequence == g_fadeAt;
		return true;
	}

	// The GPU tests' device (WARP unless VP_TEST_GPU=hardware) and r16 luma helpers.
	struct PulseGpu
	{
		pl_log log = nullptr;
		pl_d3d11 d3d = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string device;
		PulseGpu()
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			// VP_TEST_PL_LOG=1: libplacebo's errors (a shader that does not compile) into the test log.
			char flag[8] = {};
			if (GetEnvironmentVariableA("VP_TEST_PL_LOG", flag, sizeof(flag)) > 0 && flag[0] == '1')
			{
				logParams.log_level = PL_LOG_ERR;
				logParams.log_cb = [](void*, enum pl_log_level, const char* msg) { Logger::WriteMessage(msg); Logger::WriteMessage("\n"); };
			}
			log = pl_log_create(PL_API_VER, &logParams);
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
			d3d = pl_d3d11_create(log, &deviceParams);
			Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
			gpu = d3d->gpu;
			dispatch = pl_dispatch_create(log, gpu);
			VpDetailRestore::DefaultSceneReader() = &NoCutReader;
		}
		~PulseGpu()
		{
			VpDetailRestore::DefaultSceneReader() = &VpDetailRestore::ReadSceneFromSkeleton;   // the production default (C5)
			pl_dispatch_destroy(&dispatch);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}
		pl_tex Upload(const Image& img) const
		{
			std::vector<uint16_t> data(img.v.size());
			for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint16_t>(static_cast<int>(img.v[i]) << 6);
			pl_tex_params p{};
			p.w = img.w; p.h = img.h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
			return pl_tex_create(gpu, &p);
		}
		// One-plane rgba32f: the 10-bit limited luma as normalised RGB with a chroma offset per
		// channel that leaves Kr R + Kg G + Kb B = Y (the subtitle move's composed picture).
		pl_tex UploadRgb(const Image& img, const double k[3], const double offset[3]) const
		{
			std::vector<float> data(img.v.size() * 4);
			for (size_t i = 0; i < img.v.size(); ++i)
			{
				const double y = (img.v[i] - 64.0) / 876.0;
				for (int c = 0; c < 3; ++c) data[i * 4 + c] = static_cast<float>(y + offset[c]);
				data[i * 4 + 3] = 1.0f;
			}
			(void)k;
			pl_tex_params p{};
			p.w = img.w; p.h = img.h; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = data.data();
			return pl_tex_create(gpu, &p);
		}
		Image Readback(pl_tex tex) const
		{
			const int w = tex->params.w, h = tex->params.h;
			pl_tex_params rp{};
			rp.w = w; rp.h = h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
			pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
			Assert::IsTrue(pl_tex_download(gpu, &d));
			pl_tex_destroy(gpu, &readable);
			Image out(w, h);
			for (size_t i = 0; i < raw.size(); ++i) out.v[i] = raw[i] / 64.0;
			return out;
		}
		// rgba float plane -> per channel images (normalised values).
		std::vector<Image> ReadbackRgb(pl_tex tex) const
		{
			const int w = tex->params.w, h = tex->params.h;
			pl_tex_params rp{};
			rp.w = w; rp.h = h; rp.format = pl_find_named_fmt(gpu, "rgba32f"); rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			std::vector<float> raw(static_cast<size_t>(w) * h * 4);
			pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
			Assert::IsTrue(pl_tex_download(gpu, &d));
			pl_tex_destroy(gpu, &readable);
			std::vector<Image> out(4, Image(w, h));
			for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
				for (int c = 0; c < 4; ++c) out[c].v[i] = raw[i * 4 + c];
			return out;
		}
		static pl_frame LumaFrame(pl_tex tex)
		{
			pl_frame f{};
			f.num_planes = 1;
			f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
			f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
			f.color = pl_color_space_bt709;
			return f;
		}
		static pl_frame RgbFrame(pl_tex tex)
		{
			pl_frame f{};
			f.num_planes = 1;
			f.planes[0].texture = tex; f.planes[0].components = 4;
			for (int i = 0; i < 4; ++i) f.planes[0].component_mapping[i] = i;
			f.repr.sys = PL_COLOR_SYSTEM_RGB; f.repr.levels = PL_COLOR_LEVELS_FULL;
			f.color = pl_color_space_bt709;
			return f;
		}
	};

	TEST_CLASS(DetailPulseTests)
	{
	public:
		TEST_METHOD(DetailPulseKeepsSteadyGrain)
		{
			// Steady detail (a static picture, fresh grain every frame, no coding pulse): the grain
			// RMS stays within +-5 % on every frame, white and soft, 0.5-4 8-bit codes; frame-to-frame
			// grain correlation is not increased (nothing is carried between frames).
			const int w = 256, h = 192, margin = 8;
			for (bool soft : { false, true })
				for (double sigma8 : { 0.5, 1.0, 2.0, 4.0 })
				{
					const Coded c = MakeCoded(w, h, 16, 23.976, sigma8 * 4.0, soft, 11u + static_cast<unsigned>(sigma8 * 8) + (soft ? 100u : 0u), false);
					Settings s;
					const std::vector<Frame> out = Run(c.frames, s);
					double worst = 1.0, gmax = 1.0, corrIn = 0.0, corrOut = 0.0;
					for (size_t k = 0; k < out.size(); ++k)
					{
						const double ri = GrainModel::Rms(c.frames[k] - c.picture, margin);
						const double ro = GrainModel::Rms(out[k].out - c.picture, margin);
						if (std::fabs(ro / ri - 1.0) > std::fabs(worst - 1.0)) worst = ro / ri;
						gmax = (std::max)(gmax, (std::max)(out[k].g1, out[k].g2));
						Assert::IsTrue(out[k].g1 >= 1.0 && out[k].g2 >= 1.0);
						if (k > 0)
						{
							corrIn += GrainModel::Corr(c.frames[k] - c.picture, c.frames[k - 1] - c.picture, margin);
							corrOut += GrainModel::Corr(out[k].out - c.picture, out[k - 1].out - c.picture, margin);
						}
					}
					corrIn /= out.size() - 1;
					corrOut /= out.size() - 1;
					char text[200];
					snprintf(text, sizeof(text), "%s grain %.1f codes: grain RMS ratio worst %.4f, gain max %.4f, frame corr in %.4f out %.4f",
						soft ? "soft" : "white", sigma8, worst, gmax, corrIn, corrOut);
					Logger::WriteMessage(text);
					Assert::IsTrue(worst > 0.95 && worst < 1.05, W(text).c_str());
					Assert::IsTrue(corrOut <= corrIn + 0.01, W(text).c_str());
				}
			// A truly steady picture (the same frame again under new sequences): exactly gain 1.
			{
				const Coded c = MakeCoded(w, h, 1, 23.976, 4.0, false, 5u, true);
				std::vector<Image> same(10, c.frames[0]);
				const std::vector<Frame> out = Run(same, Settings());
				for (const Frame& f : out)
				{
					Assert::AreEqual(1.0, f.g1);
					Assert::AreEqual(1.0, f.g2);
					Assert::IsTrue(f.out.v == same[0].v, L"steady detail: the frame unchanged");
				}
			}
		}

		TEST_METHOD(DetailPulseSteadiesCodedPulse)
		{
			// A static picture coded as a starved GOP (fresh grain, 8x8 DCT, q 4 / 8 / 14 for I / P / B):
			// the breathing (e2.py) at least halves; no frame's detail is lowered; I-frames keep
			// gain 1.00 +- 0.01; gains within [1, cap].
			const int w = 256, h = 192;
			for (bool soft : { false, true })
			{
				const Coded c = MakeCoded(w, h, 48, 23.976, 4.0, soft, soft ? 202u : 101u);
				Settings s;
				const std::vector<Frame> causal = Run(c.frames, s);
				const std::vector<Frame> ahead = Run(c.frames, s, nullptr, true);
				std::vector<double> hin, hc, ha;
				double iGain = 1.0, gmax = 1.0;
				for (size_t k = 0; k < c.frames.size(); ++k)
				{
					hin.push_back(HfRms(c.frames[k]));
					hc.push_back(HfRms(causal[k].out));
					ha.push_back(HfRms(ahead[k].out));
					Assert::IsTrue(hc[k] >= hin[k] - 1e-9 && ha[k] >= hin[k] - 1e-9, L"no frame's detail lowered");
					for (const Frame* f : { &causal[k], &ahead[k] })
					{
						Assert::IsTrue(f->g1 >= 1.0 && f->g2 >= 1.0 && f->g1 <= s.cap + 1e-12 && f->g2 <= s.cap + 1e-12);
						gmax = (std::max)(gmax, (std::max)(f->g1, f->g2));
						if (c.types[k] == 'I')
							iGain = (std::max)(iGain, (std::max)(f->g1, f->g2));
					}
				}
				const double b0 = Breathing(hin), bc = Breathing(hc), ba = Breathing(ha);
				std::string trace;
				for (size_t k = 0; k < c.frames.size(); ++k)
				{
					char line[160];
					snprintf(line, sizeof(line), "%zu %c E %.2f %.2f g %.3f %.3f hf %.3f -> %.3f\n", k, c.types[k], causal[k].e1, causal[k].e2,
						causal[k].g1, causal[k].g2, hin[k], hc[k]);
					trace += line;
				}
				Logger::WriteMessage(trace.c_str());
				char text[200];
				snprintf(text, sizeof(text), "%s grain: breathing %.4f -> causal %.4f, with look-ahead %.4f; I-frame gain max %.4f, gain max %.3f",
					soft ? "soft" : "white", b0, bc, ba, iGain, gmax);
				Logger::WriteMessage(text);
				Assert::IsTrue(bc <= 0.5 * b0 && ba <= 0.5 * b0, W(text).c_str());
				Assert::IsTrue(iGain <= 1.01, W(text).c_str());
			}
		}

		TEST_METHOD(DetailPulseFallbackAndVeto)
		{
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 24, 23.976, 4.0, false, 303u);
			Settings s;
			// No vectors: a frame-level gain on the B-frames.
			const std::vector<Frame> plain = Run(c.frames, s);
			double bmax = 1.0;
			for (size_t k = 0; k < plain.size(); ++k)
				if (c.types[k] == 'B') bmax = (std::max)(bmax, (std::max)(plain[k].g1, plain[k].g2));
			Assert::IsTrue(bmax > 1.05, L"the no-vector fallback lifts the B-frames");

			// The base-change veto: new content in a 64 x 64 region of B-frame 2 (lifted towards the I-frame before it) (tiles 2-3 across and
			// down): those tiles keep gain 1 (the pixels between their centres are untouched).
			{
				std::vector<Image> frames = c.frames;
				const int k = 2;
				Assert::AreEqual('B', c.types[k]);
				for (int y = 64; y < 128; ++y)
					for (int x = 64; x < 128; ++x)
						frames[k].at(x, y) = 700.0;   // a flat bright object
				const std::vector<Frame> out = Run(frames, s);
				char text[200];
				snprintf(text, sizeof(text), "veto frame: g %.4f %.4f (unchanged run %.4f %.4f), restart %d, cut share %.3f, E %.2f %.2f",
					out[k].g1, out[k].g2, plain[k].g1, plain[k].g2, out[k].restart ? 1 : 0, out[k].cutShare, out[k].e1, out[k].e2);
				Logger::WriteMessage(text);
				Assert::IsTrue(out[k].g1 > 1.0 || out[k].g2 > 1.0, W(text).c_str());
				Assert::IsFalse(out[k].restart, L"a local change is not a cut");
				Assert::IsTrue(MaxAbsDiff(out[k].out, frames[k], 80, 80, 112, 112) < 1e-9, L"changed tiles keep gain 1");
				Assert::IsTrue(MaxAbsDiff(out[k].out, frames[k], 160, 0, 256, 192) > 0.1, L"unchanged tiles are stabilised");
			}

			// A field marking the blocks under that region untrusted: those tiles keep gain 1.
			{
				Field trusted(w / 16, h / 16, 1.0), marked(w / 16, h / 16, 1.0);
				for (int by = 4; by < 8; ++by)
					for (int bx = 4; bx < 8; ++bx) marked.at(bx, by) = 0.2;
				std::vector<const Field*> fields(c.frames.size(), &trusted);
				const int k = 2;
				fields[k] = &marked;
				const std::vector<Frame> out = Run(c.frames, s, &fields);
				Assert::IsTrue(MaxAbsDiff(out[k].out, c.frames[k], 80, 80, 112, 112) < 1e-9, L"untrusted tiles keep gain 1");
				Assert::IsTrue(MaxAbsDiff(out[k].out, c.frames[k], 160, 0, 256, 192) > 0.1, L"trusted tiles are stabilised");
				// A fully trusted static field: the no-vector result.
				std::vector<const Field*> all(c.frames.size(), &trusted);
				const std::vector<Frame> withField = Run(c.frames, s, &all);
				for (size_t j = 0; j < c.frames.size(); ++j)
				{
					Assert::AreEqual(plain[j].g1, withField[j].g1, 1e-12);
					Assert::AreEqual(plain[j].g2, withField[j].g2, 1e-12);
					Assert::IsTrue(MaxAbsDiff(plain[j].out, withField[j].out) < 1e-9, L"trusted static field = no vectors");
				}
			}
		}

		TEST_METHOD(DetailPulseRestartsOnCutsFadesAndGaps)
		{
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 24, 23.976, 4.0, false, 404u);
			Settings s;
			const std::vector<Frame> base = Run(c.frames, s);
			// Cut: B-frame 6 shows another picture.
			{
				std::vector<Image> frames = c.frames;
				std::mt19937 rng(9u);
				const Image other = PictureImage(w, h, 1) + GrainModel::Grain(w, h, 4.0, false, rng);
				frames[6] = Quantise(other, StepFor('B'), OffsetFor('B'));
				const std::vector<Frame> out = Run(frames, s);
				Assert::IsTrue(out[6].restart, L"a cut restarts the history");
				Assert::AreEqual(1.0, out[6].g1);
				Assert::AreEqual(1.0, out[6].g2);
			}
			// Fade: from frame 5 the picture darkens by 3 % a frame; from the second fade frame on the
			// history restarts on every frame (gain 1).
			{
				std::vector<Image> frames = c.frames;
				for (size_t k = 5; k < frames.size(); ++k)
				{
					const double f = 1.0 - 0.03 * (k - 4);
					for (double& v : frames[k].v) v = std::round(v * f);
				}
				const std::vector<Frame> out = Run(frames, s);
				for (size_t k = 6; k < frames.size(); ++k)
				{
					Assert::IsTrue(out[k].restart, L"a fade restarts the history");
					Assert::AreEqual(1.0, out[k].g1);
					Assert::AreEqual(1.0, out[k].g2);
				}
			}
			// Forget: the next frame starts over.
			{
				Model m;
				for (int k = 0; k < 2; ++k) m.Process(c.frames[k], k + 1, s);
				m.Forget();
				const Frame f = m.Process(c.frames[2], 3, s);
				Assert::IsTrue(f.restart);
				Assert::AreEqual(1.0, f.g1);
				Assert::IsTrue(base[2].g1 > 1.0 || base[2].g2 > 1.0, L"(without Forget the frame had a gain)");
			}
			// A repeat reuses the decision and never advances the history.
			{
				Model m;
				std::vector<Frame> out;
				for (int k = 0; k < 7; ++k) out.push_back(m.Process(c.frames[k], k + 1, s));
				const Frame again = m.Process(c.frames[6], 7, s);
				Assert::IsTrue(again.repeat);
				Assert::AreEqual(out[6].g1, again.g1);
				Assert::AreEqual(out[6].g2, again.g2);
				Assert::IsTrue(again.out.v == out[6].out.v);
				Assert::AreEqual(6, m.HistoryFrames());
				const Frame next = m.Process(c.frames[7], 8, s);
				Assert::AreEqual(base[7].g1, next.g1);
				Assert::AreEqual(base[7].g2, next.g2);
				Assert::IsTrue(next.out.v == base[7].out.v, L"the repeat left the history as it was");
			}
			// A non-consecutive sequence (a gap) restarts it.
			{
				Model m;
				for (int k = 0; k < 6; ++k) m.Process(c.frames[k], k + 1, s);
				const Frame f = m.Process(c.frames[6], 9, s);
				Assert::IsTrue(f.restart);
				Assert::AreEqual(1.0, f.g1);
				Assert::AreEqual(1.0, f.g2);
				Assert::AreEqual(0, m.HistoryFrames());
			}
		}

		TEST_METHOD(DetailPulseRateAndResolutionIndependent)
		{
			// The same GOP in time at 24 and 48 fps (history in seconds): the gains at the same
			// instants agree.
			const int w = 192, h = 128;
			{
				const Coded c24 = MakeCoded(w, h, 24, 24.0, 4.0, false, 505u);
				const Coded c48 = MakeCoded(w, h, 48, 48.0, 4.0, false, 606u);
				Settings s24, s48;
				s24.sourceHz = 24.0;
				s48.sourceHz = 48.0;
				const std::vector<Frame> o24 = Run(c24.frames, s24), o48 = Run(c48.frames, s48);
				double worst = 0.0;
				for (size_t k = 0; k < o24.size(); ++k)
				{
					Assert::AreEqual(c24.types[k], c48.types[2 * k]);
					worst = (std::max)(worst, (std::max)(std::fabs(o24[k].g1 - o48[2 * k].g1), std::fabs(o24[k].g2 - o48[2 * k].g2)));
				}
				char text[120];
				snprintf(text, sizeof(text), "24 vs 48 fps: worst gain difference at the same instant %.4f", worst);
				Logger::WriteMessage(text);
				Assert::IsTrue(worst < 0.04, W(text).c_str());
			}
			// The same coded clip at twice the size (pixel-doubled) on a 4320-line plane: the bands
			// follow GrainScale, the gains agree with the 2160-line original.
			{
				const Coded c = MakeCoded(w, h, 24, 23.976, 4.0, false, 707u);
				std::vector<Image> big;
				for (const Image& f : c.frames)
				{
					Image b(2 * w, 2 * h);
					for (int y = 0; y < 2 * h; ++y)
						for (int x = 0; x < 2 * w; ++x) b.at(x, y) = f.at(x / 2, y / 2);
					big.push_back(b);
				}
				Settings lowRes, highRes;
				lowRes.planeLines = 2160.0;
				highRes.planeLines = 4320.0;
				const std::vector<Frame> os = Run(c.frames, lowRes), ol = Run(big, highRes);
				double worst = 0.0, lifted = 1.0;
				for (size_t k = 0; k < os.size(); ++k)
				{
					worst = (std::max)(worst, (std::max)(std::fabs(os[k].g1 - ol[k].g1), std::fabs(os[k].g2 - ol[k].g2)));
					lifted = (std::max)(lifted, os[k].g2);
				}
				char text[120];
				snprintf(text, sizeof(text), "2160 vs 4320 lines: worst gain difference %.4f (gain max %.3f)", worst, lifted);
				Logger::WriteMessage(text);
				Assert::IsTrue(lifted > 1.03 && worst < 0.04, W(text).c_str());
			}
		}

		TEST_METHOD(DetailPulseCapAndOff)
		{
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 24, 23.976, 4.0, false, 808u);
			Settings s;
			s.cap = 1.05;
			double gmax = 1.0;
			for (const Frame& f : Run(c.frames, s))
			{
				Assert::IsTrue(f.g1 <= 1.05 + 1e-12 && f.g2 <= 1.05 + 1e-12 && f.g1 >= 1.0 && f.g2 >= 1.0);
				gmax = (std::max)(gmax, (std::max)(f.g1, f.g2));
			}
			Assert::AreEqual(1.05, gmax, 1e-12, L"the cap binds");
			s.cap = 2.0;   // clamped to kCapMax
			for (const Frame& f : Run(c.frames, s))
				Assert::IsTrue(f.g1 <= DetailPulse::kCapMax && f.g2 <= DetailPulse::kCapMax);
			// Cap 1: the input bit for bit (off: the caller does not call Apply at all).
			s.cap = 1.0;
			const std::vector<Frame> off = Run(c.frames, s);
			for (size_t k = 0; k < off.size(); ++k)
				Assert::IsTrue(off[k].out.v == c.frames[k].v, L"cap 1.0 returns the input bit-exactly");
		}

		// Bug hunt 33 F1: a letterboxed / pillarboxed picture. The bars (black, code 64) come out as
		// the source bit for bit, the bar edge is no unsharp-mask edge (the picture rows beside it
		// change no more than the interior), and the pulse still steadies the picture. Without the
		// picture rectangle (the old behaviour) the bar edge undershoots below black with the pulse.
		static Image Barred(const Image& f, const int b[4])
		{
			Image o = f;
			for (int y = 0; y < f.h; ++y)
				for (int x = 0; x < f.w; ++x)
					if (x < b[0] || x > b[2] || y < b[1] || y > b[3]) o.at(x, y) = 64.0;
			return o;
		}

		TEST_METHOD(DetailPulseLeavesLetterboxAndPillarboxBarsAlone)
		{
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 24, 23.976, 4.0, false, 3301u);
			// Letterbox with edges inside tiles (rows 24 and 167), and pillarbox (columns 40 and 215).
			const int boxes[2][4] = { { 0, 24, w - 1, 167 }, { 40, 0, 215, h - 1 } };
			for (const auto& b : boxes)
			{
				std::vector<Image> frames;
				for (const Image& f : c.frames) frames.push_back(Barred(f, b));
				Settings s;
				s.picture = { b[0] / static_cast<float>(w), b[1] / static_cast<float>(h),
					(b[2] + 1) / static_cast<float>(w), (b[3] + 1) / static_cast<float>(h) };
				int bounds[4];
				Assert::IsTrue(DetailPulse::PictureBounds(s.picture, w, h, bounds));
				for (int i = 0; i < 4; ++i) Assert::AreEqual(b[i], bounds[i], L"PictureBounds rounding");
				const std::vector<Frame> withRect = Run(frames, s);
				const std::vector<Frame> without = Run(frames, Settings());
				const bool letterbox = b[1] > 0;
				double gmax = 1.0, edgeMax = 0.0, interiorMax = 0.0, oldBarMin = 1e9;
				for (size_t k = 0; k < frames.size(); ++k)
				{
					gmax = (std::max)(gmax, (std::max)(withRect[k].g1, withRect[k].g2));
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
						{
							const bool bar = x < b[0] || x > b[2] || y < b[1] || y > b[3];
							const double in = frames[k].at(x, y);
							if (bar)
							{
								Assert::AreEqual(in, withRect[k].out.at(x, y), L"a bar pixel changed");
								oldBarMin = (std::min)(oldBarMin, without[k].out.at(x, y));
								continue;
							}
							const int d = letterbox ? (std::min)(y - b[1], b[3] - y) : (std::min)(x - b[0], b[2] - x);
							const double delta = std::fabs(withRect[k].out.at(x, y) - in);
							if (d < 2) edgeMax = (std::max)(edgeMax, delta);
							else if (d >= 16) interiorMax = (std::max)(interiorMax, delta);
						}
				}
				char text[220];
				snprintf(text, sizeof(text), "%s: gain max %.3f, picture edge |delta| max %.3f, interior %.3f; without the rectangle the bars reach %.2f",
					letterbox ? "letterbox" : "pillarbox", gmax, edgeMax, interiorMax, oldBarMin);
				Logger::WriteMessage(text);
				Assert::IsTrue(gmax > 1.01, W(text).c_str());                        // the pulse is steadied
				Assert::IsTrue(edgeMax <= 2.0 * interiorMax + 0.5, W(text).c_str()); // no edge halo
				Assert::IsTrue(oldBarMin < 63.5, W(text).c_str());                   // the old undershoot (what this guards)
			}
		}

		TEST_METHOD(DetailPulseSharedJudgeVerdictRestartsTheModel)
		{
			// Registered fix D8: with a scene reader the model restarts on the shared judge's verdict (cut or fade), and on a missing
			// record (no private fallback); the retired tile law stays the reference when the reader is null (the tests above).
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 24, 23.976, 4.0, false, 404u);
			Settings tile;
			const std::vector<Frame> base = Run(c.frames, tile);
			Settings shared;
			shared.sceneReader = &CutOrFadeAtReader;
			g_cutAt = 0; g_fadeAt = 0;
			const std::vector<Frame> none = Run(c.frames, shared);
			for (size_t k = 0; k < none.size(); ++k)
			{
				Assert::AreEqual(base[k].restart, none[k].restart, L"no verdict: the same restarts as the tile law on a clean GOP");
				Assert::AreEqual(base[k].g1, none[k].g1);
			}
			g_cutAt = 7;   // frame index 6
			const std::vector<Frame> cut = Run(c.frames, shared);
			Assert::IsTrue(cut[6].restart, L"the shared judge's cut restarts the history");
			Assert::AreEqual(1.0, cut[6].g1);
			Assert::AreEqual(1.0, cut[6].g2);
			g_cutAt = 0; g_fadeAt = 7;
			const std::vector<Frame> fade = Run(c.frames, shared);
			Assert::IsTrue(fade[6].restart, L"the shared judge's fade restarts the history");
			Settings missing;
			missing.sceneReader = &MissingReader;
			const std::vector<Frame> none2 = Run(c.frames, missing);
			for (size_t k = 1; k < none2.size(); ++k)
				Assert::IsTrue(none2[k].restart, L"no scene record: restart, never a private judgement");
		}

		TEST_METHOD(DetailPulseGpuSceneVerdictRestartsAndMissingIsCounted)
		{
			// Registered fix D8 on the GPU: the module restarts (gain 1, output = input) on the reader's cut and on a missing
			// record, and counts the missing ones; it never judges privately.
			PulseGpu g;
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 8, 23.976, 4.0, false, 404u);
			std::vector<pl_tex> textures;
			for (const Image& f : c.frames) textures.push_back(g.Upload(f));
			const int cutIndex = 5;
			g_cutAt = static_cast<uint64_t>(cutIndex) + 1; g_fadeAt = 0;
			{
				DetailPulse::Module module;
				bool liftedBefore = false;
				for (int k = 0; k < static_cast<int>(c.frames.size()); ++k)
				{
					DetailPulse::Params p;
					p.sequence = static_cast<uint64_t>(k) + 1;
					p.sceneReader = &CutOrFadeAtReader;
					pl_frame out{};
					Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures[k]), p, out), W(module.LastError().c_str()).c_str());
					const Image gpuOut = g.Readback(out.planes[0].texture);
					const double change = MaxAbsDiff(gpuOut, c.frames[k]);
					if (k == cutIndex)
						Assert::IsTrue(change < 0.01, L"the shared judge's cut restarts: output is the input");
					else if (k > 0 && k < cutIndex && change > 0.1)
						liftedBefore = true;
				}
				Assert::AreEqual(0ull, module.SceneMissing(), L"every record was readable");
				Assert::IsTrue(liftedBefore, L"before the cut the stabiliser still acts");
				module.Release(g.gpu);
			}
			{
				DetailPulse::Module module;
				const int frames = static_cast<int>(c.frames.size());
				for (int k = 0; k < frames; ++k)
				{
					DetailPulse::Params p;
					p.sequence = static_cast<uint64_t>(k) + 1;
					p.sceneReader = &MissingReader;
					pl_frame out{};
					Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures[k]), p, out), W(module.LastError().c_str()).c_str());
					Assert::IsTrue(MaxAbsDiff(g.Readback(out.planes[0].texture), c.frames[k]) < 0.01, L"no record: restart, output is the input");
				}
				Assert::AreEqual(static_cast<unsigned long long>(frames - 1), module.SceneMissing(), L"each consecutive frame without a record is counted");
				module.Release(g.gpu);
			}
			for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
		}

		// GF-P5 S5: a stage that starts over (Forget) has no scene verdict yet: the "no scene record" state of the last frame and
		// its history keep are cleared, not left on the status line. GPU (WARP unless VP_TEST_GPU=hardware), as its neighbours.
		TEST_METHOD(DetailPulseForgetClearsTheSceneJudgeState)
		{
			PulseGpu g;
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 4, 23.976, 4.0, false, 404u);
			std::vector<pl_tex> textures;
			for (const Image& f : c.frames) textures.push_back(g.Upload(f));
			{
				DetailPulse::Module module;
				for (int k = 0; k < 3; ++k)
				{
					DetailPulse::Params p;
					p.sequence = static_cast<uint64_t>(k) + 1;
					p.sceneReader = &MissingReader;   // no scene record
					pl_frame out{};
					Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures[k]), p, out), W(module.LastError().c_str()).c_str());
				}
				Assert::IsTrue(module.SceneJudgeUnavailable(), L"precondition: the last frame had no scene record");
				module.Forget();
				Assert::IsFalse(module.SceneJudgeUnavailable(), L"Forget clears the scene-judge fallback");
				Assert::AreEqual(1.0f, module.LastHistoryKeep(), 1e-6f, L"and the history keep");
				module.Release(g.gpu);
			}
			for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
		}

		// GF-P5 S5: a self-restart frame (no valid history: a sequence gap) asks no scene verdict, so the previous frame's "no scene
		// record" state is not carried over: not unavailable, keep 0.
		TEST_METHOD(DetailPulseSelfRestartFrameClearsTheSceneJudgeState)
		{
			PulseGpu g;
			const int w = 256, h = 192;
			const Coded c = MakeCoded(w, h, 4, 23.976, 4.0, false, 404u);
			std::vector<pl_tex> textures;
			for (const Image& f : c.frames) textures.push_back(g.Upload(f));
			{
				DetailPulse::Module module;
				const uint64_t sequences[4] = { 1, 2, 3, 10 };   // the last one is not the next frame: it restarts on its own
				for (int k = 0; k < 4; ++k)
				{
					DetailPulse::Params p;
					p.sequence = sequences[k];
					p.sceneReader = &MissingReader;   // no scene record
					pl_frame out{};
					Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures[k]), p, out), W(module.LastError().c_str()).c_str());
					if (k == 2)
						Assert::IsTrue(module.SceneJudgeUnavailable(), L"precondition: a consecutive frame without a record is unavailable");
				}
				Assert::IsFalse(module.SceneJudgeUnavailable(), L"the self-restart frame is not unavailable");
				Assert::AreEqual(0.0f, module.LastHistoryKeep(), 1e-6f, L"its history keep is 0");
				module.Release(g.gpu);
			}
			for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
		}

		TEST_METHOD(DetailPulseGpuLetterboxMatchesModel)
		{
			// Bug hunt 33 F1 (GPU vs model; WARP unless VP_TEST_GPU=hardware): letterbox and
			// pillarbox edges inside tiles, the fast and the original passes, a look-ahead: within
			// 0.05 codes of the model, the bars bit for bit the source.
			PulseGpu g;
			const int w = 200, h = 136;
			const Coded c = MakeCoded(w, h, 8, 23.976, 4.0, false, 3302u);
			const int boxes[2][4] = { { 0, 17, w - 1, 118 }, { 21, 0, 180, h - 1 } };
			for (const auto& b : boxes)
				for (bool fast : { true, false })
				{
					std::vector<Image> frames;
					std::vector<pl_tex> textures;
					for (const Image& f : c.frames)
					{
						frames.push_back(Barred(f, b));
						textures.push_back(g.Upload(frames.back()));
					}
					DetailPulse::Module module;
					Model model;
					Settings s;
					s.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module (SHOULD 5)
					s.picture = { b[0] / static_cast<float>(w), b[1] / static_cast<float>(h),
						(b[2] + 1) / static_cast<float>(w), (b[3] + 1) / static_cast<float>(h) };
					double worst = 0.0, barWorst = 0.0;
					for (int k = 0; k < static_cast<int>(frames.size()); ++k)
					{
						const uint64_t seq = static_cast<uint64_t>(k) + 1;
						DetailPulse::Params p;
						p.sequence = seq;
						p.fast = fast;
						p.picture = s.picture;
						if (k > 0 && k + 1 < static_cast<int>(frames.size()))
						{
							Assert::IsTrue(module.MeasureAhead(g.gpu, g.dispatch, textures[k + 1], seq + 1, p), W(module.LastError().c_str()).c_str());
							model.MeasureAhead(frames[k + 1], seq + 1, s);
						}
						pl_frame out{};
						Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures[k]), p, out), W(module.LastError().c_str()).c_str());
						const Image gpuOut = g.Readback(out.planes[0].texture);
						worst = (std::max)(worst, MaxAbsDiff(gpuOut, model.Process(frames[k], seq, s).out));
						for (int y = 0; y < h; ++y)
							for (int x = 0; x < w; ++x)
								if (x < b[0] || x > b[2] || y < b[1] || y > b[3])
									barWorst = (std::max)(barWorst, std::fabs(gpuOut.at(x, y) - frames[k].at(x, y)));
					}
					char text[160];
					snprintf(text, sizeof(text), "%s %s: worst GPU vs model %.4f codes, bars %.4f", b[1] > 0 ? "letterbox" : "pillarbox",
						fast ? "fast" : "original", worst, barWorst);
					Logger::WriteMessage(text);
					Assert::IsTrue(worst < 0.05, W(text).c_str());
					Assert::AreEqual(0.0, barWorst, W(text).c_str());
					module.Release(g.gpu);
					for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
				}
		}

		TEST_METHOD(DetailPulseGpuMatchesModel)
		{
			// The shader against the model on a short coded sequence (edges: partial tiles), without
			// and with a field (and a look-ahead / a repeat), within a small tolerance in 10-bit codes.
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			pl_log log = pl_log_create(PL_API_VER, &logParams);
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			Logger::WriteMessage(("GPU tests on: " + device + "\n").c_str());
			pl_d3d11 d3d = pl_d3d11_create(log, &deviceParams);
			Assert::IsNotNull(d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3d->gpu;
			pl_dispatch dispatch = pl_dispatch_create(log, gpu);
			const int w = 200, h = 136;
			const Coded c = MakeCoded(w, h, 10, 23.976, 4.0, false, 909u);
			auto upload = [&](const Image& img) {
				std::vector<uint16_t> data(img.v.size());
				for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint16_t>(static_cast<int>(img.v[i]) << 6);
				pl_tex_params p{};
				p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readback = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = w; rp.h = h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				pl_tex readable = pl_tex_create(gpu, &rp);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
				pl_tex_transfer_params d{}; d.tex = readable; d.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &readable);
				Image out(w, h);
				for (size_t i = 0; i < raw.size(); ++i) out.v[i] = raw[i] / 64.0;
				return out;
			};
			auto frameOf = [&](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			std::vector<pl_tex> textures;
			for (const Image& f : c.frames) textures.push_back(upload(f));
			// The field: 16 x 16 native blocks, rgba32f (vx, vy, trust, error); a 3 x 3 block patch untrusted.
			Field marked((w + 15) / 16, (h + 15) / 16, 1.0);
			for (int by = 2; by < 5; ++by)
				for (int bx = 3; bx < 6; ++bx) marked.at(bx, by) = 0.1;
			std::vector<float> fieldData;
			for (double t : marked.trust) { fieldData.push_back(0.0f); fieldData.push_back(0.0f); fieldData.push_back(static_cast<float>(t)); fieldData.push_back(0.0f); }
			pl_tex_params fp{};
			fp.w = marked.w; fp.h = marked.h; fp.format = pl_find_named_fmt(gpu, "rgba32f"); fp.sampleable = true; fp.initial_data = fieldData.data();
			pl_tex fieldTex = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(fieldTex);
			for (int variant = 0; variant < 2; ++variant)
			{
				const bool useField = variant == 1, ahead = variant == 0;
				DetailPulse::Module module;
				Model model;
				Settings s;
				s.sceneReader = &NoCutReader;   // D8: this test builds its own device, so the module is given the same reader (NoCutReader) explicitly via p.sceneReader; the model follows that verdict
				double worst = 0.0;
				for (int k = 0; k < static_cast<int>(c.frames.size()); ++k)
				{
					const uint64_t seq = static_cast<uint64_t>(k) + 1;
					DetailPulse::Params p;
					p.sequence = seq;
					p.sceneReader = &NoCutReader;   // own device, no PulseGpu fixture default: the module must read the model's verdict
					p.field = useField ? fieldTex : nullptr;
					if (ahead && k + 1 < static_cast<int>(c.frames.size()) && k > 0)
					{
						Assert::IsTrue(module.MeasureAhead(gpu, dispatch, textures[k + 1], seq + 1, p), W(module.LastError().c_str()).c_str());
						model.MeasureAhead(c.frames[k + 1], seq + 1, s);
					}
					const int reps = useField && k == 5 ? 2 : 1;   // a repeat of frame 5
					for (int r = 0; r < reps; ++r)
					{
						pl_frame out{};
						Assert::IsTrue(module.Apply(gpu, dispatch, frameOf(textures[k]), p, out), W(module.LastError().c_str()).c_str());
						const Frame f = model.Process(c.frames[k], seq, s, useField ? &marked : nullptr);
						const Image gpuOut = readback(out.planes[0].texture);
						worst = (std::max)(worst, MaxAbsDiff(gpuOut, f.out));
					}
				}
				char text[120];
				snprintf(text, sizeof(text), "%s: worst difference GPU vs model %.4f codes", useField ? "field + repeat" : "no field + look-ahead", worst);
				Logger::WriteMessage(text);
				Assert::IsTrue(worst < 0.05, W(text).c_str());
				module.Release(gpu);
			}
			for (pl_tex& t : textures) pl_tex_destroy(gpu, &t);
			pl_tex_destroy(gpu, &fieldTex);
			pl_dispatch_destroy(&dispatch);
			pl_d3d11_destroy(&d3d);
			pl_log_destroy(&log);
		}

		TEST_METHOD(DetailPulseRepeatOfAShiftedPictureHasNoGhost)
		{
			// Hunt 3 F6: a repeat whose picture is another one (a Stage B phase picture, a gap
			// fill, a de-judder extra: A moved along a pan) takes the frame's decision with ITS
			// OWN bands. With A's stored low band instead, D2 = G1(S) - L_A carries the motion
			// displacement: a ghost of A's low band x (g2 - 1). The model (the shader's reference)
			// must equal the picture's own bands exactly; the stale form's ghost is measured.
			const int w = 256, h = 192, margin = 40;
			const Coded c = MakeCoded(w, h, 8, 23.976, 4.0, false, 1201u);
			Settings s;
			const std::vector<Frame> all = Run(c.frames, s);
			int kk = 1;
			for (int k = 1; k < static_cast<int>(all.size()); ++k)
				if (all[k].g2 > all[kk].g2) kk = k;
			Model m;
			std::vector<Frame> out;
			for (int k = 0; k <= kk; ++k) out.push_back(m.Process(c.frames[k], k + 1, s));
			const Frame& a = out[kk];
			Assert::IsTrue(a.g2 > 1.02, L"(a B-frame with a mid-band gain)");
			const Image shifted = Shifted(c.frames[kk], 6, 4);
			const Frame rep = m.Process(shifted, kk + 1, s);
			Assert::IsTrue(rep.repeat);
			const double scale = DetailPulse::Scale(h);
			const DetailPulseModel::Bands own = DetailPulseModel::MakeBands(shifted, scale);
			const Image expected = DetailPulseModel::ApplyGains(shifted, own, a.tileWeight, a.g1, a.g2);
			Assert::IsTrue(MaxAbsDiff(rep.out, expected) == 0.0, L"the repeat uses the picture's own bands");
			// The stale form (A's low band): its difference to the correct output is the ghost.
			const DetailPulseModel::Bands bandsA = DetailPulseModel::MakeBands(c.frames[kk], scale);
			double ghost = 0.0, n = 0.0;
			for (int y = margin; y < h - margin; ++y)
				for (int x = margin; x < w - margin; ++x)
				{
					const double g1 = shifted.at(x, y) - own.d1.at(x, y);
					const double staleD2 = g1 - GrainModel::LowAt(bandsA.lowHalf, x + 0.5, y + 0.5);
					const double wt = GrainModel::Bilinear(a.tileWeight, (x + 0.5) / DetailPulse::kTilePx, (y + 0.5) / DetailPulse::kTilePx);
					const double stale = shifted.at(x, y) + wt * ((a.g1 - 1.0) * own.d1.at(x, y) + (a.g2 - 1.0) * staleD2);
					ghost += (stale - rep.out.at(x, y)) * (stale - rep.out.at(x, y));
					n += 1.0;
				}
			ghost = std::sqrt(ghost / n);
			// The correct repeat's delta follows the picture: the shifted picture's delta is A's
			// delta shifted (an even move keeps the half-resolution boxes aligned, so the bands are
			// shift-equivariant), wherever the tile weights are flat.
			double follow = 0.0;
			bool flat = true;
			for (double v : a.tileWeight.v) flat = flat && v > 0.999;
			if (flat)
				for (int y = margin; y < h - margin; ++y)
					for (int x = margin; x < w - margin; ++x)
						follow = (std::max)(follow, std::fabs((rep.out.at(x, y) - shifted.at(x, y)) - (a.out.at(x - 6, y - 4) - c.frames[kk].at(x - 6, y - 4))));
			char text[200];
			snprintf(text, sizeof(text), "repeat of a picture moved (6, 4) px: stale-low ghost RMS %.4f codes (g2 %.3f); correct delta vs A's moved delta %.2e (flat weights %d)",
				ghost, a.g2, follow, flat ? 1 : 0);
			Logger::WriteMessage(text);
			Assert::IsTrue(flat, L"tile weights must be flat (>0.999) so shift-equivariance check runs");
			Assert::IsTrue(ghost > 0.02, W(text).c_str());
			Assert::IsTrue(follow < 1e-9, W(text).c_str());
		}

		TEST_METHOD(DetailPulseGpuRepeatOfAShiftedPictureMatchesModel)
		{
			// Hunt 3 F6 (GPU vs model): frame 5 repeated with the picture moved (4, 2) px, then
			// the sequence goes on: the repeat and every later frame within 0.05 codes of the model.
			PulseGpu g;
			const int w = 200, h = 136;
			const Coded c = MakeCoded(w, h, 9, 23.976, 4.0, false, 1301u);
			DetailPulse::Module module;
			Model model;
			Settings s;
			s.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module (SHOULD 5)
			double worstRepeat = 0.0, worst = 0.0;
			std::vector<pl_tex> textures;
			for (int k = 0; k < static_cast<int>(c.frames.size()); ++k)
			{
				const uint64_t seq = static_cast<uint64_t>(k) + 1;
				DetailPulse::Params p;
				p.sequence = seq;
				textures.push_back(g.Upload(c.frames[k]));
				pl_frame out{};
				Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), p, out), W(module.LastError().c_str()).c_str());
				worst = (std::max)(worst, MaxAbsDiff(g.Readback(out.planes[0].texture), model.Process(c.frames[k], seq, s).out));
				if (k == 5)
				{
					const Image moved = Shifted(c.frames[k], 4, 2);
					textures.push_back(g.Upload(moved));
					Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), p, out), W(module.LastError().c_str()).c_str());
					worstRepeat = MaxAbsDiff(g.Readback(out.planes[0].texture), model.Process(moved, seq, s).out);
				}
			}
			char text[160];
			snprintf(text, sizeof(text), "repeat of a moved picture: worst GPU vs model %.4f codes (repeat), %.4f (frames)", worstRepeat, worst);
			Logger::WriteMessage(text);
			Assert::IsTrue(worstRepeat < 0.05 && worst < 0.05, W(text).c_str());
			module.Release(g.gpu);
			for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
		}

		TEST_METHOD(DetailPulseGpuForeignGeometryKeepsTheHistory)
		{
			// Hunt 3 (crossfade lead): the path being left (another re-base: a half-size plane) takes
			// the frame's decision with its own bands; the main path's output and history are
			// untouched (the frame Apply returned stays valid; the next frames are as without it).
			PulseGpu g;
			const int w = 200, h = 136;
			const Coded c = MakeCoded(w, h, 8, 23.976, 4.0, false, 1401u);
			DetailPulse::Module module;
			Model model;
			Settings s, half;
			s.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module (SHOULD 5)
			half.sceneReader = &NoCutReader;
			half.planeLines = (h + 1) / 2;
			double worstMain = 0.0, worstForeign = 0.0;
			std::vector<pl_tex> textures;
			for (int k = 0; k < static_cast<int>(c.frames.size()); ++k)
			{
				const uint64_t seq = static_cast<uint64_t>(k) + 1;
				DetailPulse::Params p;
				p.sequence = seq;
				textures.push_back(g.Upload(c.frames[k]));
				pl_frame out{};
				Assert::IsTrue(module.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), p, out), W(module.LastError().c_str()).c_str());
				const Frame f = model.Process(c.frames[k], seq, s);
				const Image halfPic = HalfBox(c.frames[k]);
				textures.push_back(g.Upload(halfPic));
				DetailPulse::Params pf = p;
				pf.planeLines = half.planeLines;
				pl_frame foreign{};
				Assert::IsTrue(module.ApplyForeign(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), pf, foreign), W(module.LastError().c_str()).c_str());
				Assert::IsTrue(foreign.planes[0].texture != out.planes[0].texture, L"its own output texture");
				worstForeign = (std::max)(worstForeign, MaxAbsDiff(g.Readback(foreign.planes[0].texture), model.ProcessForeign(halfPic, half).out));
				worstMain = (std::max)(worstMain, MaxAbsDiff(g.Readback(out.planes[0].texture), f.out));
				Assert::AreEqual(model.HistoryFrames(), module.HistoryFrames(), L"the history is not touched");
			}
			char text[160];
			snprintf(text, sizeof(text), "foreign geometry: worst GPU vs model %.4f codes (foreign), %.4f (main)", worstForeign, worstMain);
			Logger::WriteMessage(text);
			Assert::IsTrue(worstForeign < 0.05 && worstMain < 0.05, W(text).c_str());
			module.Release(g.gpu);
			for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
		}

		TEST_METHOD(DetailPulseGpuRgbPictureMatchesLumaAndKeepsHistory)
		{
			// Hunt 3 F8 / F9: a one-plane RGB picture (the subtitle move's composed picture during
			// a cue) is stabilised on its luma in the planes' codes: the same delta on R, G and B
			// (= the luma model's delta / 876), and the history runs on through the cue (frames 3-5
			// RGB, the rest planes) without a restart.
			PulseGpu g;
			const int w = 200, h = 136;
			const Coded c = MakeCoded(w, h, 9, 23.976, 4.0, false, 1501u);
			double k[3];
			DetailPulse::LumaWeights(PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, k);
			// Chroma offsets with Kr a + Kg b + Kb c = 0 (Y recovered exactly).
			const double offset[3] = { 0.03, -(k[0] * 0.03 + k[2] * -0.05) / k[1], -0.05 };
			DetailPulse::Module module;
			Model model;
			Settings s;
			s.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module (SHOULD 5)
			double worst = 0.0, worstSpread = 0.0;
			std::vector<pl_tex> textures;
			for (int f = 0; f < static_cast<int>(c.frames.size()); ++f)
			{
				const uint64_t seq = static_cast<uint64_t>(f) + 1;
				const bool rgb = f >= 3 && f <= 5;
				DetailPulse::Params p;
				p.sequence = seq;
				p.lumaSystem = PL_COLOR_SYSTEM_BT_709;
				p.codeLevels = PL_COLOR_LEVELS_LIMITED;
				textures.push_back(rgb ? g.UploadRgb(c.frames[f], k, offset) : g.Upload(c.frames[f]));
				pl_frame out{};
				const bool ok = module.Apply(g.gpu, g.dispatch, rgb ? PulseGpu::RgbFrame(textures.back()) : PulseGpu::LumaFrame(textures.back()), p, out);
				Assert::IsTrue(ok, W(("frame " + std::to_string(f) + ": " + module.LastError()).c_str()).c_str());
				const Frame mf = model.Process(c.frames[f], seq, s);
				if (rgb)
				{
					const std::vector<Image> ch = g.ReadbackRgb(out.planes[0].texture);
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x)
						{
							const double yIn = (c.frames[f].at(x, y) - 64.0) / 876.0;
							const double want = (mf.out.at(x, y) - c.frames[f].at(x, y)) / 876.0;
							double lo = 1e9, hi = -1e9;
							for (int cc = 0; cc < 3; ++cc)
							{
								const double d = ch[cc].at(x, y) - (yIn + offset[cc]);
								lo = (std::min)(lo, d);
								hi = (std::max)(hi, d);
								worst = (std::max)(worst, std::fabs(d - want) * 876.0);
							}
							worstSpread = (std::max)(worstSpread, (hi - lo) * 876.0);
						}
				}
				else
					worst = (std::max)(worst, MaxAbsDiff(g.Readback(out.planes[0].texture), mf.out));
				Assert::AreEqual(model.HistoryFrames(), module.HistoryFrames(), L"the history runs through the cue");
			}
			char text[160];
			snprintf(text, sizeof(text), "RGB cue frames: worst GPU vs luma model %.4f codes, R/G/B delta spread %.5f codes", worst, worstSpread);
			Logger::WriteMessage(text);
			Assert::IsTrue(worst < 0.05 && worstSpread < 0.01, W(text).c_str());
			Assert::AreEqual(8, module.HistoryFrames(), L"no restart at the cue's start or end");
			module.Release(g.gpu);
			for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
		}

		TEST_METHOD(DetailPulseGpuNonLumaPlaneIsCountedAsInactive)
		{
			// Hunt 3 F8: a picture the pulse cannot read (plane 0 neither luma nor RGB) is a counted
			// failure with its reason (the renderer reports "inactive"), never a silent no-op.
			PulseGpu g;
			const Image img = PictureImage(64, 32);
			pl_tex t = g.Upload(img);
			pl_frame f = PulseGpu::LumaFrame(t);
			f.planes[0].component_mapping[0] = PL_CHANNEL_CB;
			DetailPulse::Module module;
			DetailPulse::Params p;
			p.sequence = 1;
			pl_frame out{};
			Assert::IsFalse(module.Apply(g.gpu, g.dispatch, f, p, out));
			Assert::AreEqual(1ull, module.Failures(), L"counted");
			Assert::IsFalse(module.LastError().empty());
			module.Release(g.gpu);
			pl_tex_destroy(g.gpu, &t);
		}

		TEST_METHOD(DetailPulseGpuLookAheadInTheDebandedDomain)
		{
			// Hunt 3 F4: Apply measures the frame after VP's debander; the look-ahead must measure
			// the next frame debanded too. A steady banded picture (the same frame every time):
			// with the look-ahead debanded the gain is exactly 1 (the output is the debanded input
			// bit for bit); measured raw it carries the band energy the debander removed (gain > 1).
			PulseGpu g;
			const int w = 192, h = 128;
			Image banded(w, h);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					const double ramp = 300.0 + 120.0 * x / w + 40.0 * y / h;
					banded.at(x, y) = 4.0 * std::floor(ramp / 4.0) + std::round(1.5 * std::sin(6.283185307 * (x + 2 * y) / 9.0));
				}
			pl_tex tex = g.Upload(banded);
			VpDebander::Module debander;
			VpDebander::Params dp;
			dp.thresholdCodes = 6.0;
			dp.colorDepth = 10;
			dp.radiusScale = h / 1080.0;
			for (int mode = 0; mode < 2; ++mode)
			{
				const bool debandAhead = mode == 1;
				DetailPulse::Module module;
				double moved = 0.0;
				for (int k = 0; k < 10; ++k)
				{
					const uint64_t seq = static_cast<uint64_t>(k) + 1;
					DetailPulse::Params p;
					p.sequence = seq;
					dp.sequence = seq;
					pl_frame cur{};
					Assert::IsTrue(debander.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(tex), dp, cur, 0), W(debander.LastError().c_str()).c_str());
					const Image debandedIn = g.Readback(cur.planes[0].texture);
					if (k > 0)
					{
						if (debandAhead)
							Assert::IsTrue(module.MeasureAhead(g.gpu, g.dispatch, PulseGpu::LumaFrame(tex), seq + 1, p, &debander, &dp), W(module.LastError().c_str()).c_str());
						else
							Assert::IsTrue(module.MeasureAhead(g.gpu, g.dispatch, tex, seq + 1, p), W(module.LastError().c_str()).c_str());
					}
					pl_frame out{};
					Assert::IsTrue(module.Apply(g.gpu, g.dispatch, cur, p, out), W(module.LastError().c_str()).c_str());
					moved = (std::max)(moved, MaxAbsDiff(g.Readback(out.planes[0].texture), debandedIn));
				}
				char text[160];
				snprintf(text, sizeof(text), "steady banded picture, look-ahead %s: worst change %.4f codes", debandAhead ? "debanded" : "raw", moved);
				Logger::WriteMessage(text);
				if (debandAhead)
					Assert::IsTrue(moved == 0.0, W(text).c_str());
				else
					Assert::IsTrue(moved > 0.0, W(text).c_str());
				module.Release(g.gpu);
			}
			debander.Release(g.gpu);
			pl_tex_destroy(g.gpu, &tex);
		}

		TEST_METHOD(DetailPulseGpuFastMatchesOriginal)
		{
			// detail_pulse_fast against the original passes (detail_pulse_fast off) on the same
			// inputs: new frames with and without a field, a look-ahead, repeats of a moved picture
			// (hunt 3 F6), another geometry (the crossfade's path being left) and a one-plane RGB
			// cue; odd sizes (partial tiles, odd half planes). The error budget: at most one r16
			// storage step (1/64 of a 10-bit code) on a pixel, where another fp32 rounding of the
			// same sums lands on the other side of a storage step; the share of such pixels is
			// reported. The fast path is also checked against the model (0.05 codes, as
			// DetailPulseGpuMatchesModel).
			PulseGpu g;
			double worstLuma = 0.0, worstRgb = 0.0, worstModel = 0.0;
			int changedPixels = 0;
			long long compared = 0, differing = 0;
			auto compare = [&](const Image& x, const Image& y) {
				for (size_t i = 0; i < x.v.size(); ++i)
				{
					const double d = std::fabs(x.v[i] - y.v[i]);
					worstLuma = (std::max)(worstLuma, d);
					differing += d > 0.0 ? 1 : 0;
				}
				compared += static_cast<long long>(x.v.size());
			};
			for (int geo = 0; geo < 2; ++geo)
			{
				const int w = geo == 0 ? 200 : 233, h = geo == 0 ? 136 : 121;
				const Coded c = MakeCoded(w, h, 10, 23.976, 4.0, false, 1601u + geo);
				Field marked((w + 15) / 16, (h + 15) / 16, 1.0);
				for (int by = 2; by < 5; ++by)
					for (int bx = 3; bx < 6; ++bx) marked.at(bx, by) = 0.1;
				std::vector<float> fieldData;
				for (double t : marked.trust) { fieldData.push_back(0.0f); fieldData.push_back(0.0f); fieldData.push_back(static_cast<float>(t)); fieldData.push_back(0.0f); }
				pl_tex_params fp{};
				fp.w = marked.w; fp.h = marked.h; fp.format = pl_find_named_fmt(g.gpu, "rgba32f"); fp.sampleable = true; fp.initial_data = fieldData.data();
				pl_tex fieldTex = pl_tex_create(g.gpu, &fp);
				double k3[3];
				DetailPulse::LumaWeights(PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709, k3);
				const double offset[3] = { 0.03, -(k3[0] * 0.03 + k3[2] * -0.05) / k3[1], -0.05 };
				std::vector<pl_tex> textures;
				for (int useField = 0; useField < 2; ++useField)
				{
					DetailPulse::Module fast, orig;
					Model model;
					Settings s;
					s.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module (SHOULD 5)
					for (int k = 0; k < static_cast<int>(c.frames.size()); ++k)
					{
						const uint64_t seq = static_cast<uint64_t>(k) + 1;
						DetailPulse::Params p;
						p.sequence = seq;
						p.field = useField ? fieldTex : nullptr;
						p.lumaSystem = PL_COLOR_SYSTEM_BT_709;
						p.codeLevels = PL_COLOR_LEVELS_LIMITED;
						DetailPulse::Params po = p;
						po.fast = false;
						if (!useField && k > 0 && k + 1 < static_cast<int>(c.frames.size()))
						{
							textures.push_back(g.Upload(c.frames[k + 1]));
							Assert::IsTrue(fast.MeasureAhead(g.gpu, g.dispatch, textures.back(), seq + 1, p), W(fast.LastError().c_str()).c_str());
							Assert::IsTrue(orig.MeasureAhead(g.gpu, g.dispatch, textures.back(), seq + 1, po), W(orig.LastError().c_str()).c_str());
							model.MeasureAhead(c.frames[k + 1], seq + 1, s);
						}
						const bool rgb = useField && k >= 6 && k <= 7;
						textures.push_back(rgb ? g.UploadRgb(c.frames[k], k3, offset) : g.Upload(c.frames[k]));
						const pl_frame in = rgb ? PulseGpu::RgbFrame(textures.back()) : PulseGpu::LumaFrame(textures.back());
						pl_frame a{}, b{};
						Assert::IsTrue(fast.Apply(g.gpu, g.dispatch, in, p, a), W(fast.LastError().c_str()).c_str());
						Assert::IsTrue(orig.Apply(g.gpu, g.dispatch, in, po, b), W(orig.LastError().c_str()).c_str());
						const Frame mf = model.Process(c.frames[k], seq, s, useField ? &marked : nullptr);
						if (rgb)
						{
							const std::vector<Image> ca = g.ReadbackRgb(a.planes[0].texture), cb = g.ReadbackRgb(b.planes[0].texture);
							for (int cc = 0; cc < 3; ++cc) worstRgb = (std::max)(worstRgb, 876.0 * MaxAbsDiff(ca[cc], cb[cc]));
						}
						else
						{
							const Image ia = g.Readback(a.planes[0].texture), ib = g.Readback(b.planes[0].texture);
							compare(ia, ib);
							worstModel = (std::max)(worstModel, MaxAbsDiff(ia, mf.out));
							for (size_t i = 0; i < ia.v.size(); ++i) changedPixels += ia.v[i] != c.frames[k].v[i] ? 1 : 0;
						}
						if (k == 4 || k == 8)
						{
							// A repeat of another picture (moved), then another geometry.
							const Image moved = Shifted(c.frames[k], 3 + geo, 2);
							textures.push_back(g.Upload(moved));
							Assert::IsTrue(fast.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), p, a), W(fast.LastError().c_str()).c_str());
							Assert::IsTrue(orig.Apply(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), po, b), W(orig.LastError().c_str()).c_str());
							const Image ra = g.Readback(a.planes[0].texture);
							compare(ra, g.Readback(b.planes[0].texture));
							worstModel = (std::max)(worstModel, MaxAbsDiff(ra, model.Process(moved, seq, s, useField ? &marked : nullptr).out));
							const Image halfPic = HalfBox(c.frames[k]);
							textures.push_back(g.Upload(halfPic));
							DetailPulse::Params pf = p, pfo = po;
							pf.planeLines = pfo.planeLines = (h + 1) / 2;
							Assert::IsTrue(fast.ApplyForeign(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), pf, a), W(fast.LastError().c_str()).c_str());
							Assert::IsTrue(orig.ApplyForeign(g.gpu, g.dispatch, PulseGpu::LumaFrame(textures.back()), pfo, b), W(orig.LastError().c_str()).c_str());
							compare(g.Readback(a.planes[0].texture), g.Readback(b.planes[0].texture));
						}
						Assert::AreEqual(orig.HistoryFrames(), fast.HistoryFrames());
					}
					fast.Release(g.gpu);
					orig.Release(g.gpu);
				}
				for (pl_tex& t : textures) pl_tex_destroy(g.gpu, &t);
				pl_tex_destroy(g.gpu, &fieldTex);
			}
			char text[256];
			snprintf(text, sizeof(text), "fast vs original: worst %.6f codes (luma; %lld of %lld pixels differ, %.4f %%), %.6f codes (RGB); fast vs model %.4f codes; %d pixels changed by the pulse",
				worstLuma, differing, compared, 100.0 * differing / (std::max)(compared, 1LL), worstRgb, worstModel, changedPixels);
			Logger::WriteMessage(text);
			Assert::IsTrue(changedPixels > 0, W(text).c_str());
			Assert::IsTrue(worstLuma <= 1.0 / 64.0 + 1e-9 && worstRgb < 0.01 && worstModel < 0.05, W(text).c_str());
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(DetailPulseGpuTiming)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DetailPulseGpuTiming)
		{
			// Apply at 3840x2160, 2560x1600 and 1920x1080 (r16 luma, 4:2:0 chroma passing through),
			// detail_pulse_fast off and on, median over frames, CPU wall clock around pl_gpu_finish:
			//  - new frames with gains (two pictures alternating, the second with 0.75 x the first's
			//    detail: measure + reduce + apply, a gain above 1 on every other frame);
			//  - new frames steady (one picture: gain exactly 1 after the first);
			//  - repeats of another picture (hunt 3 F6: a de-judder extra or gap fill; the apply
			//    pass alone, with a gain above 1).
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Gpu,
				"DetailPulseGpuTiming", ExtendedRangeGpuProbe::OnHardwareGpu(), "needs VP_TEST_GPU=hardware"))
				return;
			PulseGpu g;
			LARGE_INTEGER freq;
			QueryPerformanceFrequency(&freq);
			const int sizes[3][2] = { { 3840, 2160 }, { 2560, 1600 }, { 1920, 1080 } };
			for (const auto& size : sizes)
			{
				const int w = size[0], h = size[1];
				std::mt19937 rng(1u);
				std::uniform_int_distribution<int> noise(-35, 35);
				std::vector<uint16_t> da(static_cast<size_t>(w) * h), db(da.size());
				for (size_t i = 0; i < da.size(); ++i)
				{
					const int n = noise(rng);
					da[i] = static_cast<uint16_t>((512 + n) << 6);
					db[i] = static_cast<uint16_t>(static_cast<int>(std::lround(512 + 0.75 * n)) << 6);
				}
				auto plane = [&](int pw, int ph, const uint16_t* data) {
					pl_tex_params tp{};
					tp.w = pw; tp.h = ph; tp.format = pl_find_named_fmt(g.gpu, "r16"); tp.sampleable = true; tp.initial_data = data;
					pl_tex t = pl_tex_create(g.gpu, &tp);
					Assert::IsNotNull(t);
					return t;
				};
				std::vector<uint16_t> chroma(static_cast<size_t>(w / 2) * (h / 2), static_cast<uint16_t>(512 << 6));
				pl_tex lumaA = plane(w, h, da.data()), lumaB = plane(w, h, db.data());
				pl_tex cb = plane(w / 2, h / 2, chroma.data()), cr = plane(w / 2, h / 2, chroma.data());
				auto frameOf = [&](pl_tex luma) {
					pl_frame frame{};
					frame.num_planes = 3;
					const pl_tex t[3] = { luma, cb, cr };
					for (int p = 0; p < 3; ++p) { frame.planes[p].texture = t[p]; frame.planes[p].components = 1; }
					frame.planes[0].component_mapping[0] = PL_CHANNEL_Y;
					frame.planes[1].component_mapping[0] = PL_CHANNEL_CB;
					frame.planes[2].component_mapping[0] = PL_CHANNEL_CR;
					frame.repr.sys = PL_COLOR_SYSTEM_BT_709; frame.repr.levels = PL_COLOR_LEVELS_LIMITED;
					frame.repr.bits.sample_depth = 16; frame.repr.bits.color_depth = 10; frame.repr.bits.bit_shift = 6;
					frame.color = pl_color_space_bt709;
					return frame;
				};
				const pl_frame fa = frameOf(lumaA), fb = frameOf(lumaB);
				std::string gpuLines;
				DetailPulse::SetPassTimers(g.gpu, true);
				std::string line = "DetailPulseGpuTiming on " + g.device + ": " + std::to_string(w) + "x" + std::to_string(h);
				for (int fast = 0; fast < 2; ++fast)
				{
					double median[3] = {}, batched[3] = {};
					for (int mode = 0; mode < 3; ++mode)
					{
						DetailPulse::Module module;
						std::vector<double> ms, cpuMs;
						uint64_t seq = 0;
						for (int k = 0; k < 48; ++k)
						{
							DetailPulse::Params p;
							p.fast = fast == 1;
							pl_frame out{};
							const bool timedRepeat = mode == 2 && k >= 2;
							if (!timedRepeat) ++seq;
							p.sequence = seq;
							const pl_frame& in = mode == 0 ? ((k & 1) ? fb : fa) : mode == 1 ? fa : (k == 1 ? fb : fa);
							pl_gpu_finish(g.gpu);
							LARGE_INTEGER t0, t1;
							QueryPerformanceCounter(&t0);
							Assert::IsTrue(module.Apply(g.gpu, g.dispatch, in, p, out), W(module.LastError().c_str()).c_str());
							LARGE_INTEGER tc;
							QueryPerformanceCounter(&tc);
							pl_gpu_finish(g.gpu);
							QueryPerformanceCounter(&t1);
							if (k >= 8) ms.push_back(1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart);
							if (k >= 8) cpuMs.push_back(1000.0 * (tc.QuadPart - t0.QuadPart) / freq.QuadPart);
							if ((mode == 0 && k == 9) || (mode == 2 && k == 47))
							{
								// A gain above 1 here (not the copy path).
								const Image o = g.Readback(out.planes[0].texture);
								const uint16_t* src = mode == 0 ? db.data() : da.data();
								int changed = 0;
								for (size_t i = 0; i < o.v.size(); i += 97) changed += o.v[i] != src[i] / 64.0 ? 1 : 0;
								Assert::IsTrue(changed > 0, L"the timed frames carry a gain");
							}
						}
						if (GetEnvironmentVariableA("VP_TEST_PULSE_FRAMES", nullptr, 0) > 0)
						{
							std::string all = "frames " + std::to_string(w) + (fast ? " fast" : " original") + " mode " + std::to_string(mode) + ":";
							for (double v : ms) { char t[16]; snprintf(t, sizeof(t), " %.2f", v); all += t; }
							all += " | cpu:";
							for (double v : cpuMs) { char t[16]; snprintf(t, sizeof(t), " %.2f", v); all += t; }
							Logger::WriteMessage(all.c_str());
						}
						std::sort(ms.begin(), ms.end());
						median[mode] = ms[ms.size() / 2];
						// Throughput: 4 batches of 32 frames between two finishes (no per-frame sync
						// latency, the GPU kept at its clocks), the median batch per frame.
						std::vector<double> batches;
						double gpuMs[4];
						DetailPulse::PassTimes(gpuMs, true);
						for (int b = 0; b < 4; ++b)
						{
							pl_gpu_finish(g.gpu);
							LARGE_INTEGER t0, t1;
							QueryPerformanceCounter(&t0);
							for (int k = 0; k < 32; ++k)
							{
								DetailPulse::Params p;
								p.fast = fast == 1;
								if (mode != 2) ++seq;
								p.sequence = seq;
								pl_frame out{};
								const pl_frame& in = mode == 0 ? ((seq & 1) ? fa : fb) : fa;
								Assert::IsTrue(module.Apply(g.gpu, g.dispatch, in, p, out), W(module.LastError().c_str()).c_str());
							}
							pl_gpu_finish(g.gpu);
							QueryPerformanceCounter(&t1);
							batches.push_back(1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart / 32.0);
						}
						std::sort(batches.begin(), batches.end());
						batched[mode] = 0.5 * (batches[1] + batches[2]);
						DetailPulse::PassTimes(gpuMs, true);
						char gt[160];
						snprintf(gt, sizeof(gt), "  [%s %s gpu: low %.3f, measure %.3f, reduce %.3f, apply %.3f ms]", fast ? "fast" : "orig",
							mode == 0 ? "new" : mode == 1 ? "steady" : "repeat", gpuMs[3], gpuMs[0], gpuMs[1], gpuMs[2]);
						gpuLines += gt;
					}
					char text[256];
					for (int mode = 0; mode < 3; ++mode)
					{
						Assert::IsTrue(std::isfinite(median[mode]) && median[mode] > 0.0, L"DetailPulse median GPU time must be finite and positive");
						Assert::IsTrue(std::isfinite(batched[mode]) && batched[mode] > 0.0, L"DetailPulse batched GPU time must be finite and positive");
					}
					snprintf(text, sizeof(text), " | %s: new %.3f ms, steady %.3f ms, repeat %.3f ms (batched %.3f / %.3f / %.3f)", fast ? "fast" : "original",
						median[0], median[1], median[2], batched[0], batched[1], batched[2]);
					line += text;
				}
				DetailPulse::SetPassTimers(g.gpu, false);
				Logger::WriteMessage((line + " ||" + gpuLines).c_str());
				for (pl_tex t : { lumaA, lumaB, cb, cr }) pl_tex_destroy(g.gpu, &t);
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(DetailPulseRealClipBreathing)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Footage")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DetailPulseRealClipBreathing)
		{
			// D:\hfr\breathing (research material): x265 CRF 24 / 32 of a clean 1536x768 clip,
			// yuv420p10le, 48 frames. The CPU model without vectors, causal and with the look-ahead.
			char* folder = nullptr;
			size_t length = 0;
			const bool hasFolder = _dupenv_s(&folder, &length, "VP_TEST_REAL_FOOTAGE") == 0 && folder && *folder;
			const std::string folderStr = hasFolder ? folder : "";
			std::free(folder);
			if (!HardwarePrerequisites::RequireOrSkip(HardwarePrerequisites::Kind::Footage,
				"DetailPulseRealClipBreathing", !folderStr.empty(), "set VP_TEST_REAL_FOOTAGE"))
				return;
			const std::string dir = folderStr + "\\breathing\\";
			const int w = 1536, h = 768, n = 48;
			const size_t frameSamples = static_cast<size_t>(w) * h * 3 / 2;
			auto luma = [&](const std::string& name, int k) {
				Image img(w, h);
				FILE* f = nullptr;
				Assert::IsTrue(fopen_s(&f, (dir + name).c_str(), "rb") == 0 && f, W((dir + name).c_str()).c_str());
				std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
				_fseeki64(f, static_cast<long long>(k) * frameSamples * 2, SEEK_SET);
				const size_t got = fread(raw.data(), 2, raw.size(), f);
				fclose(f);
				Assert::IsTrue(raw.size() == got, L"short read");
				for (size_t i = 0; i < raw.size(); ++i) img.v[i] = raw[i];
				return img;
			};
			auto types = [&](const std::string& name) {
				std::string t;
				FILE* f = nullptr;
				if (fopen_s(&f, (dir + name).c_str(), "rb") == 0 && f)
				{
					int ch;
					while ((ch = fgetc(f)) != EOF)
						if (ch == 'I' || ch == 'P' || ch == 'B') t += static_cast<char>(ch);
					fclose(f);
				}
				return t;
			};
			auto psnr = [](const Image& a, const Image& b) {
				double sq = 0.0;
				for (size_t i = 0; i < a.v.size(); ++i) sq += (a.v[i] - b.v[i]) * (a.v[i] - b.v[i]);
				return 10.0 * std::log10(1023.0 * 1023.0 / (sq / a.v.size()));
			};
			// Streamed frame by frame (the whole clip in doubles would be ~1 GB).
			for (const char* crf : { "24", "32" })
			{
				const std::string dec = std::string("dec") + crf + ".yuv";
				const std::string t = types(std::string("types") + crf + ".txt");
				Assert::IsTrue(t.size() >= static_cast<size_t>(n), L"frame types");
				std::vector<double> hin;
				double b0 = 0.0;
				for (int ahead = 0; ahead < 2; ++ahead)
				{
					Model model;
					Settings s;
					s.sceneReader = &NoCutReader;   // D8: the model follows the same shared verdict as the module (SHOULD 5)
					std::vector<double> hout;
					double meanLoss = 0.0, worstLoss = 0.0, iGain = 1.0, gmax = 1.0;
					bool lowered = false;
					Image next = luma(dec, 0);
					for (int k = 0; k < n; ++k)
					{
						const Image cur = next;
						if (k + 1 < n)
						{
							next = luma(dec, k + 1);
							if (ahead) model.MeasureAhead(next, k + 2, s);
						}
						const Frame f = model.Process(cur, k + 1, s);
						const Image o = luma("orig.yuv", k);
						if (ahead == 0) hin.push_back(HfRms(cur));
						hout.push_back(HfRms(f.out));
						lowered = lowered || hout[k] < hin[k] - 1e-9;
						const double loss = psnr(cur, o) - psnr(f.out, o);
						meanLoss += loss / n;
						worstLoss = (std::max)(worstLoss, loss);
						gmax = (std::max)(gmax, (std::max)(f.g1, f.g2));
						if (t[k] == 'I') iGain = (std::max)(iGain, (std::max)(f.g1, f.g2));
					}
					b0 = Breathing(hin);
					const double b1 = Breathing(hout);
					char text[260];
					snprintf(text, sizeof(text), "CRF %s %s: breathing %.4f -> %.4f (%.0f %%), PSNR loss mean %.3f dB worst %.3f dB, I-frame gain %.4f, gain max %.3f",
						crf, ahead ? "causal + look-ahead" : "causal", b0, b1, 100.0 * (1.0 - b1 / b0), meanLoss, worstLoss, iGain, gmax);
					Logger::WriteMessage(text);
					Assert::IsFalse(lowered, W(text).c_str());
					Assert::IsTrue(b1 <= (ahead ? 0.5 : 0.6) * b0, W(text).c_str());   // causal 43-46 %, with the look-ahead 58-60 % (2026-10-17)
					Assert::IsTrue(iGain <= 1.01, W(text).c_str());
					Assert::IsTrue(meanLoss <= 0.15 && worstLoss <= 0.35, W(text).c_str());
				}
			}
		}

		BEGIN_TEST_METHOD_ATTRIBUTE(DetailPulseGpuBandMemoOneComputeAndFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DetailPulseGpuBandMemoOneComputeAndFallback)
		{
			namespace GB = Vpv::GrainBand;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			struct LogGuard
			{
				pl_log log = nullptr;
				explicit LogGuard(pl_log l) : log(l) {}
				~LogGuard() { if (log) pl_log_destroy(&log); }
				LogGuard(const LogGuard&) = delete;
				LogGuard& operator=(const LogGuard&) = delete;
			};
			struct D3dGuard
			{
				pl_d3d11 d3d = nullptr;
				explicit D3dGuard(pl_d3d11 d) : d3d(d) {}
				~D3dGuard() { if (d3d) pl_d3d11_destroy(&d3d); }
				D3dGuard(const D3dGuard&) = delete;
				D3dGuard& operator=(const D3dGuard&) = delete;
			};
			struct DispatchGuard
			{
				pl_dispatch dispatch = nullptr;
				explicit DispatchGuard(pl_dispatch d) : dispatch(d) {}
				~DispatchGuard() { if (dispatch) pl_dispatch_destroy(&dispatch); }
				DispatchGuard(const DispatchGuard&) = delete;
				DispatchGuard& operator=(const DispatchGuard&) = delete;
			};
			struct TexGuard
			{
				pl_gpu gpu = nullptr;
				pl_tex tex = nullptr;
				TexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
				~TexGuard() { if (gpu && tex) pl_tex_destroy(gpu, &tex); }
				TexGuard(const TexGuard&) = delete;
				TexGuard& operator=(const TexGuard&) = delete;
			};
			struct PulseGuard
			{
				pl_gpu gpu = nullptr;
				DetailPulse::Module mod;
				explicit PulseGuard(pl_gpu g) : gpu(g) {}
				~PulseGuard() { mod.Release(gpu); }
				PulseGuard(const PulseGuard&) = delete;
				PulseGuard& operator=(const PulseGuard&) = delete;
			};
			struct MemoGpuGuard
			{
				Vpv::Skeleton::OwnerGpuDevice ownerDev;
				VpSkeleton::SkeletonMemo memo;
				explicit MemoGpuGuard(pl_gpu g) : ownerDev(g) {}
				~MemoGpuGuard()
				{
					memo.ReleaseGpuSpots();
					ownerDev.Release();
				}
				MemoGpuGuard(const MemoGpuGuard&) = delete;
				MemoGpuGuard& operator=(const MemoGpuGuard&) = delete;
			};

			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			LogGuard logGuard(pl_log_create(PL_API_VER, &logParams));
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			D3dGuard d3dGuard(pl_d3d11_create(logGuard.log, &deviceParams));
			Assert::IsNotNull(d3dGuard.d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3dGuard.d3d->gpu;
			DispatchGuard dispGuard(pl_dispatch_create(logGuard.log, gpu));
			pl_dispatch dispatch = dispGuard.dispatch;
			Assert::IsNotNull(dispatch, L"pl_dispatch_create succeeded");

			const int w = 128, h = 96;
			const int hw = (w + 1) / 2, hh = (h + 1) / 2;
			// Frame 0 = sharp I-frame (step 4), Frame 1 = coarse B-frame (step 14): non-trivial g2 > 1 boost on frame 1.
			const Coded c = MakeCoded(w, h, 2, 23.976, 4.0, false, 909u);

			auto uploadFull = [&](const Image& img, std::vector<uint16_t>& rawOut) {
				rawOut.resize(img.v.size());
				for (size_t i = 0; i < rawOut.size(); ++i)
					rawOut[i] = static_cast<uint16_t>(static_cast<int>(img.v[i]) << 6);
				pl_tex_params p{};
				p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = rawOut.data();
				return pl_tex_create(gpu, &p);
			};
			auto uploadHalf = [&](const std::vector<uint16_t>& raw) {
				std::vector<float> half(static_cast<size_t>(hw) * hh, 0.0f);
				for (int qy = 0; qy < hh; ++qy)
					for (int qx = 0; qx < hw; ++qx)
					{
						const int x0 = (std::min)(2 * qx, w - 1), x1 = (std::min)(2 * qx + 1, w - 1);
						const int y0 = (std::min)(2 * qy, h - 1), y1 = (std::min)(2 * qy + 1, h - 1);
						const float a = static_cast<float>(raw[static_cast<size_t>(y0) * w + x0]) / 65535.0f;
						const float b = static_cast<float>(raw[static_cast<size_t>(y0) * w + x1]) / 65535.0f;
						const float cc = static_cast<float>(raw[static_cast<size_t>(y1) * w + x0]) / 65535.0f;
						const float d = static_cast<float>(raw[static_cast<size_t>(y1) * w + x1]) / 65535.0f;
						half[static_cast<size_t>(qy) * hw + qx] = 0.25f * (a + b + cc + d);
					}
				pl_tex_params p{};
				p.w = hw; p.h = hh; p.format = pl_find_named_fmt(gpu, "r32f"); p.sampleable = true; p.initial_data = half.data();
				return pl_tex_create(gpu, &p);
			};
			auto readbackCodes = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = w; rp.h = h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				TexGuard readable(gpu, pl_tex_create(gpu, &rp));
				Assert::IsNotNull(readable.tex);
				pl_tex_blit_params bp{};
				bp.src = tex;
				bp.dst = readable.tex;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
				pl_tex_transfer_params dl{};
				dl.tex = readable.tex;
				dl.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				Image out(w, h);
				for (size_t i = 0; i < raw.size(); ++i)
					out.v[i] = raw[i] / 64.0;
				return out;
			};
			auto frameOf = [](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex;
				f.planes[0].components = 1;
				f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709;
				f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16;
				f.repr.bits.color_depth = 10;
				f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};

			std::vector<uint16_t> raw0, raw1;
			TexGuard tex0(gpu, uploadFull(c.frames[0], raw0));
			TexGuard tex1(gpu, uploadFull(c.frames[1], raw1));
			TexGuard half0(gpu, uploadHalf(raw0));
			TexGuard half1(gpu, uploadHalf(raw1));
			const std::vector<float> zeroHalf(static_cast<size_t>(hw) * hh, 0.0f);
			pl_tex_params zp{};
			zp.w = hw; zp.h = hh; zp.format = pl_find_named_fmt(gpu, "r32f"); zp.sampleable = true; zp.initial_data = zeroHalf.data();
			TexGuard zeroTex(gpu, pl_tex_create(gpu, &zp));
			Assert::IsTrue(tex0.tex && tex1.tex && half0.tex && half1.tex && zeroTex.tex);

			MemoGpuGuard gpuMemo(gpu);
			int hostTag = 0;
			gpuMemo.memo.SetHost(&hostTag);
			gpuMemo.memo.SetRenderThread(std::this_thread::get_id());
			gpuMemo.memo.SetGpuDispatcherPresent(true);
			gpuMemo.memo.SetGpuBudget(true, uint64_t(1) << 28);
			gpuMemo.memo.SetGpuDevice(&gpuMemo.ownerDev);

			pl_tex h0 = half0.tex, h1 = half1.tex;
			VpSkeleton::OwnerFill p1Fill;
			p1Fill.view = true;
			p1Fill.frameScoped = true;
			p1Fill.fill = [h0, h1, hw, hh](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq < 1u || req.seq > 2u)
					return false;
				pl_tex chosen = (req.seq == 1u) ? h0 : h1;
				out.gpu.resource = const_cast<void*>(static_cast<const void*>(chosen));
				out.gpu.width = static_cast<uint32_t>(hw);
				out.gpu.height = static_cast<uint32_t>(hh);
				out.gpu.bytes = 0;
				return true;
			};
			Assert::IsTrue(gpuMemo.memo.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Gpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP1HalfLuma), std::move(p1Fill)) == Reg::Registered);

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			const uint64_t reserve = GB::BandMemoReserveBytes(VpSkeleton::Dims{ static_cast<uint32_t>(w), static_cast<uint32_t>(h) });
			Assert::IsTrue(gpuMemo.memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu,
				GB::kBandMemoUnit, GB::MakeBandMemoOwnerFill(reserve, counters)) == Reg::Registered);

			// Two independent DetailPulse readers share kBandMemo: 2 frames => exactly 2 fills (1 per frame), 4 hits total.
			PulseGuard dpA(gpu), dpB(gpu), dpFallback(gpu), dpUnreg(gpu), dpZero(gpu);
			pl_tex fullTexs[2] = { tex0.tex, tex1.tex };
			pl_frame outA{}, outB{}, outFallback{}, outUnreg{}, outZero{};
			VpSkeleton::SkeletonMemo unregMemo;

			MemoGpuGuard zeroMemo(gpu);
			zeroMemo.memo.SetHost(&hostTag);
			zeroMemo.memo.SetRenderThread(std::this_thread::get_id());
			zeroMemo.memo.SetGpuDispatcherPresent(true);
			zeroMemo.memo.SetGpuBudget(true, uint64_t(1) << 28);
			zeroMemo.memo.SetGpuDevice(&zeroMemo.ownerDev);
			pl_tex zeroPtr = zeroTex.tex;
			VpSkeleton::OwnerFill zeroFill;
			using VK = VpSkeleton::VariantKey;
			zeroFill.variantAxes = uint32_t(VK::kAxisPlane) | uint32_t(VK::kAxisSize) | uint32_t(VK::kAxisScale)
				| uint32_t(VK::kAxisSigma) | uint32_t(VK::kAxisStage) | uint32_t(VK::kAxisParam0) | uint32_t(VK::kAxisParam1);
			zeroFill.view = true;
			zeroFill.frameScoped = true;
			zeroFill.fill = [zeroPtr, hw, hh](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				out.gpu.resource = const_cast<void*>(static_cast<const void*>(zeroPtr));
				out.gpu.width = static_cast<uint32_t>(hw);
				out.gpu.height = static_cast<uint32_t>(hh);
				out.gpu.bytes = 0;
				return true;
			};
			Assert::IsTrue(zeroMemo.memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu,
				GB::kBandMemoUnit, std::move(zeroFill)) == Reg::Registered);

			for (int k = 0; k < 2; ++k)
			{
				DetailPulse::Params p;
				p.sequence = static_cast<uint64_t>(k) + 1u;
				p.sceneReader = &NoCutReader;
				p.memo = &gpuMemo.memo;
				Assert::IsTrue(dpA.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), p, outA), W(dpA.mod.LastError().c_str()).c_str());
				Assert::IsTrue(dpB.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), p, outB), W(dpB.mod.LastError().c_str()).c_str());

				DetailPulse::Params pf = p;
				pf.memo = nullptr;
				Assert::IsTrue(dpFallback.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), pf, outFallback), W(dpFallback.mod.LastError().c_str()).c_str());

				DetailPulse::Params pu = p;
				pu.memo = &unregMemo;
				Assert::IsTrue(dpUnreg.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), pu, outUnreg), W(dpUnreg.mod.LastError().c_str()).c_str());

				DetailPulse::Params pz = p;
				pz.memo = &zeroMemo.memo;
				Assert::IsTrue(dpZero.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), pz, outZero), W(dpZero.mod.LastError().c_str()).c_str());
			}

			Assert::AreEqual(uint64_t(2), counters->fills.load(), L"two frames across two readers compute kBandMemo once per frame (fills == 2)");
			Assert::AreEqual(2ull, dpA.mod.BandMemoHits(), L"dpA hit kBandMemo on both frames");
			Assert::AreEqual(2ull, dpB.mod.BandMemoHits(), L"dpB hit kBandMemo on both frames");
			Assert::AreEqual(0ull, dpA.mod.BandMemoFallbacks(), L"dpA had zero fallbacks");
			Assert::AreEqual(0ull, dpB.mod.BandMemoFallbacks(), L"dpB had zero fallbacks");
			Assert::IsTrue(gpuMemo.memo.Counters(VpSkeleton::kBandMemo).reusedMemo >= 2u, L"memo recorded reuse for second reader");

			Assert::AreEqual(0ull, dpFallback.mod.BandMemoHits(), L"memo=nullptr has zero hits");
			Assert::AreEqual(2ull, dpFallback.mod.BandMemoFallbacks(), L"memo=nullptr increments BandMemoFallbacks on both frames");
			Assert::IsFalse(dpFallback.mod.LastBandMemoFallback().empty(), L"memo=nullptr records visible fallback reason");
			Assert::AreEqual(0ull, dpUnreg.mod.BandMemoHits(), L"unregistered memo has zero hits");
			Assert::AreEqual(2ull, dpUnreg.mod.BandMemoFallbacks(), L"unregistered memo increments fallbacks");
			Assert::IsFalse(dpUnreg.mod.LastBandMemoFallback().empty(), L"unregistered memo records visible fallback reason");

			const Image imgA = readbackCodes(outA.planes[0].texture);
			const Image imgB = readbackCodes(outB.planes[0].texture);
			const Image imgFallback = readbackCodes(outFallback.planes[0].texture);
			const Image imgZero = readbackCodes(outZero.planes[0].texture);

			double maxAB = 0.0, maxMemoVsFallback = 0.0, maxBoostVsInput = 0.0, maxZeroVsMemo = 0.0;
			for (size_t i = 0; i < imgA.v.size(); ++i)
			{
				maxAB = (std::max)(maxAB, std::fabs(imgA.v[i] - imgB.v[i]));
				maxMemoVsFallback = (std::max)(maxMemoVsFallback, std::fabs(imgA.v[i] - imgFallback.v[i]));
				maxBoostVsInput = (std::max)(maxBoostVsInput, std::fabs(imgA.v[i] - c.frames[1].v[i]));
				maxZeroVsMemo = (std::max)(maxZeroVsMemo, std::fabs(imgZero.v[i] - imgA.v[i]));
			}
			Assert::AreEqual(0.0, maxAB, 0.0, L"second reader matches first reader bit-for-bit");
			Assert::IsTrue(maxMemoVsFallback <= 0.05, L"kBandMemo path matches memo=nullptr fallback within 0.05 10-bit codes");
			Assert::IsTrue(maxBoostVsInput > 1.0, L"DetailPulse actively boosted the coarse B-frame (non-vacuous)");
			Assert::AreEqual(2ull, dpZero.mod.BandMemoHits(), L"dpZero consumed tampered kBandMemo");
			Assert::IsTrue(maxZeroVsMemo > 0.5, L"negative control: zeroed low band alters DetailPulse output by > 0.5 10-bit codes");
		}

		// W23-B3 M2#9 (15c C3b): DetailPulse acquires shared P11 band energies (re_s2/re_s4) through SkeletonMemo,
		// increments P11EnergyHits with zero fallbacks; absent/unregistered P11 increments P11EnergyFallbacks with a
		// visible reason; swapping/poisoning P11 re_s2 energies shifts DetailPulse output on the exact same input.
		BEGIN_TEST_METHOD_ATTRIBUTE(DetailPulseAcquiresP11BandEnergiesWithVisibleFallback)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DetailPulseAcquiresP11BandEnergiesWithVisibleFallback)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;
			struct LogGuard { pl_log log = nullptr; explicit LogGuard(pl_log l) : log(l) {} ~LogGuard() { if (log) pl_log_destroy(&log); } };
			struct D3dGuard { pl_d3d11 d3d = nullptr; explicit D3dGuard(pl_d3d11 d) : d3d(d) {} ~D3dGuard() { if (d3d) pl_d3d11_destroy(&d3d); } };
			struct DispatchGuard { pl_dispatch d = nullptr; explicit DispatchGuard(pl_dispatch dp) : d(dp) {} ~DispatchGuard() { if (d) pl_dispatch_destroy(&d); } };
			struct TexGuard { pl_gpu g = nullptr; pl_tex t = nullptr; TexGuard(pl_gpu gpu, pl_tex tex) : g(gpu), t(tex) {} ~TexGuard() { if (g && t) pl_tex_destroy(g, &t); } };
			struct PulseGuard { pl_gpu g = nullptr; DetailPulse::Module mod; explicit PulseGuard(pl_gpu gpu) : g(gpu) {} ~PulseGuard() { mod.Release(g); } };
			struct MemoGpuGuard
			{
				Vpv::Skeleton::OwnerGpuDevice ownerDev;
				VpSkeleton::SkeletonMemo memo;
				explicit MemoGpuGuard(pl_gpu g) : ownerDev(g) {}
				~MemoGpuGuard() { memo.ReleaseGpuSpots(); ownerDev.Release(); }
			};

			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			LogGuard logGuard(pl_log_create(PL_API_VER, &logParams));
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			D3dGuard d3dGuard(pl_d3d11_create(logGuard.log, &deviceParams));
			Assert::IsNotNull(d3dGuard.d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3dGuard.d3d->gpu;
			DispatchGuard dispGuard(pl_dispatch_create(logGuard.log, gpu));
			pl_dispatch dispatch = dispGuard.d;
			Assert::IsNotNull(dispatch, L"pl_dispatch_create succeeded");

			const int w = 128, h = 96;
			const Coded c = MakeCoded(w, h, 2, 23.976, 4.0, false, 909u);
			const int p11B = RegionEvidence::BlockSizeFor(h);
			const RegionEvidence::Grid g = RegionEvidence::MakeGrid(w, h, p11B);
			const size_t nb = g.Count();

			auto uploadFull = [&](const Image& img) {
				std::vector<uint16_t> raw(img.v.size());
				for (size_t i = 0; i < raw.size(); ++i)
					raw[i] = static_cast<uint16_t>(static_cast<int>(img.v[i]) << 6);
				pl_tex_params p{};
				p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = raw.data();
				return pl_tex_create(gpu, &p);
			};
			auto uploadRgba = [&](const std::vector<float>& data) {
				pl_tex_params p{};
				p.w = g.gw; p.h = g.gh; p.format = pl_find_named_fmt(gpu, "rgba32f"); p.sampleable = true; p.initial_data = data.data();
				return pl_tex_create(gpu, &p);
			};
			auto readbackCodes = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = w; rp.h = h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				TexGuard readable(gpu, pl_tex_create(gpu, &rp));
				Assert::IsNotNull(readable.t);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable.t;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
				pl_tex_transfer_params dl{}; dl.tex = readable.t; dl.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				Image out(w, h);
				for (size_t i = 0; i < raw.size(); ++i) out.v[i] = raw[i] / 64.0;
				return out;
			};
			auto frameOf = [](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};
			auto makeP11 = [&](const Image& img, std::vector<float>& s2, std::vector<float>& s4) {
				GrainSplit::Plane pl(w, h);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x) pl.at(x, y) = img.at(x, y);
				const GrainSplit::Split sp = GrainSplit::Compute(pl, double(p11B) / 16.0, p11B, GrainSplit::Params());
				s2.assign(nb * 4u, 0.0f); s4.assign(nb * 4u, 0.0f);
				for (size_t i = 0; i < nb; ++i)
				{
					s2[i * 4 + 0] = static_cast<float>(sp.e1[i]);
					s2[i * 4 + 1] = static_cast<float>(sp.e2[i]);
					s2[i * 4 + 2] = static_cast<float>(sp.e3[i]);
					s4[i * 4 + 0] = 1.0f; s4[i * 4 + 2] = 1.0f;
					s4[i * 4 + 3] = static_cast<float>(sp.N[i]);
				}
			};

			TexGuard tex0(gpu, uploadFull(c.frames[0])), tex1(gpu, uploadFull(c.frames[1]));
			std::vector<float> s20, s40, s21, s41;
			makeP11(c.frames[0], s20, s40);
			makeP11(c.frames[1], s21, s41);
			TexGuard s2Tex0(gpu, uploadRgba(s20)), s4Tex0(gpu, uploadRgba(s40));
			TexGuard s2Tex1(gpu, uploadRgba(s21)), s4Tex1(gpu, uploadRgba(s41));
			// Negative control: reversed P11 band energies (frame 0 tiny, frame 1 huge) suppress DetailPulse's B-frame boost.
			std::vector<float> pS20(nb * 4u, 0.0f), pS21(nb * 4u, 0.0f), pS4(nb * 4u, 0.0f);
			for (size_t i = 0; i < nb; ++i)
			{
				pS20[i * 4 + 0] = 0.01f; pS20[i * 4 + 1] = 0.01f; pS20[i * 4 + 2] = 0.01f;
				pS21[i * 4 + 0] = 2000.0f; pS21[i * 4 + 1] = 2000.0f; pS21[i * 4 + 2] = 500.0f;
				pS4[i * 4 + 0] = 1.0f; pS4[i * 4 + 2] = 1.0f; pS4[i * 4 + 3] = 0.0f;
			}
			TexGuard ps2Tex0(gpu, uploadRgba(pS20)), ps2Tex1(gpu, uploadRgba(pS21)), ps4Tex(gpu, uploadRgba(pS4));
			Assert::IsTrue(tex0.t && tex1.t && s2Tex0.t && s4Tex0.t && s2Tex1.t && s4Tex1.t && ps2Tex0.t && ps2Tex1.t && ps4Tex.t);

			int hostTag = 0;
			auto initMemo = [&](MemoGpuGuard& mg, pl_tex a2, pl_tex a4, pl_tex b2, pl_tex b4) {
				mg.memo.SetHost(&hostTag);
				mg.memo.SetRenderThread(std::this_thread::get_id());
				mg.memo.SetGpuDispatcherPresent(true);
				mg.memo.SetGpuBudget(true, uint64_t(1) << 28);
				mg.memo.SetGpuDevice(&mg.ownerDev);
				VpSkeleton::OwnerFill fill;
				fill.view = true;
				fill.frameScoped = true;
				fill.fill = [a2, a4, b2, b4, g](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
					if (req.seq != 1u && req.seq != 2u) return false;
					pl_tex s2 = (req.seq == 1u) ? a2 : b2;
					pl_tex s4 = (req.seq == 1u) ? a4 : b4;
					out.gpu.resource = const_cast<void*>(static_cast<const void*>(s2));
					out.gpu.view = const_cast<void*>(static_cast<const void*>(s4));
					out.gpu.width = uint32_t(g.gw);
					out.gpu.height = uint32_t(g.gh);
					out.gpu.bytes = 0;
					return true;
				};
				Assert::IsTrue(mg.memo.RegisterOwnerFill(VpSkeleton::kP11RegionBlockStats, VpSkeleton::AcquireForm::Gpu,
					VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP11RegionBlockStats), std::move(fill)) == Reg::Registered);
			};

			MemoGpuGuard p11Memo(gpu), poisonMemo(gpu);
			initMemo(p11Memo, s2Tex0.t, s4Tex0.t, s2Tex1.t, s4Tex1.t);
			initMemo(poisonMemo, ps2Tex0.t, ps4Tex.t, ps2Tex1.t, ps4Tex.t);
			VpSkeleton::SkeletonMemo unregMemo;

			PulseGuard dpP11(gpu), dpNull(gpu), dpUnreg(gpu), dpPoison(gpu);
			pl_tex fullTexs[2] = { tex0.t, tex1.t };
			pl_frame outP11{}, outNull{}, outUnreg{}, outPoison{};
			for (int k = 0; k < 2; ++k)
			{
				DetailPulse::Params p;
				p.sequence = static_cast<uint64_t>(k) + 1u;
				p.sceneReader = &NoCutReader;
				p.memo = &p11Memo.memo;
				Assert::IsTrue(dpP11.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), p, outP11), W(dpP11.mod.LastError().c_str()).c_str());

				DetailPulse::Params pn = p; pn.memo = nullptr;
				Assert::IsTrue(dpNull.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), pn, outNull), W(dpNull.mod.LastError().c_str()).c_str());

				DetailPulse::Params pu = p; pu.memo = &unregMemo;
				Assert::IsTrue(dpUnreg.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), pu, outUnreg), W(dpUnreg.mod.LastError().c_str()).c_str());

				DetailPulse::Params pp = p; pp.memo = &poisonMemo.memo;
				Assert::IsTrue(dpPoison.mod.Apply(gpu, dispatch, frameOf(fullTexs[k]), pp, outPoison), W(dpPoison.mod.LastError().c_str()).c_str());
			}

			Assert::AreEqual(2ull, dpP11.mod.P11EnergyHits(), L"DP hit P11 on both frames");
			Assert::AreEqual(0ull, dpP11.mod.P11EnergyFallbacks(), L"DP had zero P11 fallbacks when memo provided P11");
			Assert::IsTrue(dpP11.mod.LastP11EnergyFallback().empty(), L"no fallback reason on P11 hit");

			Assert::AreEqual(0ull, dpNull.mod.P11EnergyHits(), L"memo=nullptr has zero P11 hits");
			Assert::AreEqual(2ull, dpNull.mod.P11EnergyFallbacks(), L"memo=nullptr increments P11EnergyFallbacks");
			Assert::IsFalse(dpNull.mod.LastP11EnergyFallback().empty(), L"memo=nullptr records visible fallback reason");

			Assert::AreEqual(0ull, dpUnreg.mod.P11EnergyHits(), L"unregistered memo has zero P11 hits");
			Assert::AreEqual(2ull, dpUnreg.mod.P11EnergyFallbacks(), L"unregistered memo increments P11EnergyFallbacks");
			Assert::IsFalse(dpUnreg.mod.LastP11EnergyFallback().empty(), L"unregistered memo records visible fallback reason");

			Assert::AreEqual(2ull, dpPoison.mod.P11EnergyHits(), L"poisoned memo still hit P11");
			const Image imgP11 = readbackCodes(outP11.planes[0].texture);
			const Image imgNull = readbackCodes(outNull.planes[0].texture);
			const Image imgPoison = readbackCodes(outPoison.planes[0].texture);
			double maxBoostVsIn = 0.0, maxNullBoostVsIn = 0.0, maxNullDelta = 0.0, maxPoisonDelta = 0.0;
			double sqNullDelta = 0.0, sqPoisonDelta = 0.0;
			for (size_t i = 0; i < imgP11.v.size(); ++i)
			{
				const double dNull = imgP11.v[i] - imgNull.v[i];
				const double dPoison = imgP11.v[i] - imgPoison.v[i];
				maxBoostVsIn = (std::max)(maxBoostVsIn, std::fabs(imgP11.v[i] - c.frames[1].v[i]));
				maxNullBoostVsIn = (std::max)(maxNullBoostVsIn, std::fabs(imgNull.v[i] - c.frames[1].v[i]));
				maxNullDelta = (std::max)(maxNullDelta, std::fabs(dNull));
				maxPoisonDelta = (std::max)(maxPoisonDelta, std::fabs(dPoison));
				sqNullDelta += dNull * dNull;
				sqPoisonDelta += dPoison * dPoison;
			}
			const double rmsNullDelta = std::sqrt(sqNullDelta / static_cast<double>(imgP11.v.size()));
			const double rmsPoisonDelta = std::sqrt(sqPoisonDelta / static_cast<double>(imgP11.v.size()));
			Assert::IsTrue(maxBoostVsIn > 0.5, L"non-vacuous: DP with real P11 actively boosts the coarse B-frame");
			Assert::IsTrue(maxNullBoostVsIn > 0.5, L"non-vacuous: DP fallback (memo=nullptr) actively boosts the coarse B-frame");
			Assert::IsTrue(maxPoisonDelta > 0.5, L"negative control: swapped/poisoned P11 re_s2 shifts DP output on the exact same input");
			Assert::IsTrue(rmsNullDelta < 0.25 && maxNullDelta < 1.0,
				L"DP P11 output matches memo=nullptr fallback quantitatively (accurate channel & code_scale2 ingest)");
			Assert::IsTrue(maxNullDelta < 0.25 * maxPoisonDelta && rmsNullDelta < 0.25 * rmsPoisonDelta,
				L"DP P11 parity vs memo=nullptr is strictly tighter than poisoned P11 divergence");
		}

		// W23-B3 M2#12 / HIGH #3: end-to-end DetailPulse::Module::Apply exercises vector-veto with trusted P7
		// (Reason::Ok, confidence 0.85) on a pan-cut scene verdict: default params.vectorVeto=true suppresses
		// restart (SceneVectorVetoes() >= 1, keeps history); negative control params.vectorVeto=false restarts.
		BEGIN_TEST_METHOD_ATTRIBUTE(DetailPulseGpuVectorVetoSuppressesPanCutRestart)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(DetailPulseGpuVectorVetoSuppressesPanCutRestart)
		{
			using Reg = VpSkeleton::SkeletonMemo::Registration;
			struct LogGuard { pl_log log = nullptr; explicit LogGuard(pl_log l) : log(l) {} ~LogGuard() { if (log) pl_log_destroy(&log); } };
			struct D3dGuard { pl_d3d11 d3d = nullptr; explicit D3dGuard(pl_d3d11 d) : d3d(d) {} ~D3dGuard() { if (d3d) pl_d3d11_destroy(&d3d); } };
			struct DispatchGuard { pl_dispatch d = nullptr; explicit DispatchGuard(pl_dispatch dp) : d(dp) {} ~DispatchGuard() { if (d) pl_dispatch_destroy(&d); } };
			struct TexGuard { pl_gpu g = nullptr; pl_tex t = nullptr; TexGuard(pl_gpu gpu, pl_tex tex) : g(gpu), t(tex) {} ~TexGuard() { if (g && t) pl_tex_destroy(g, &t); } };
			struct PulseGuard { pl_gpu g = nullptr; DetailPulse::Module mod; explicit PulseGuard(pl_gpu gpu) : g(gpu) {} ~PulseGuard() { mod.Release(g); } };

			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			LogGuard logGuard(pl_log_create(PL_API_VER, &logParams));
			std::string device;
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			D3dGuard d3dGuard(pl_d3d11_create(logGuard.log, &deviceParams));
			Assert::IsNotNull(d3dGuard.d3d, L"Could not create the libplacebo test device");
			pl_gpu gpu = d3dGuard.d3d->gpu;
			DispatchGuard dispGuard(pl_dispatch_create(logGuard.log, gpu));
			pl_dispatch dispatch = dispGuard.d;
			Assert::IsNotNull(dispatch, L"pl_dispatch_create succeeded");

			const int w = 128, h = 96;
			const Coded c = MakeCoded(w, h, 2, 23.976, 4.0, false, 909u);
			auto uploadFull = [&](const Image& img) {
				std::vector<uint16_t> raw(img.v.size());
				for (size_t i = 0; i < raw.size(); ++i)
					raw[i] = static_cast<uint16_t>(static_cast<int>(img.v[i]) << 6);
				pl_tex_params p{};
				p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = raw.data();
				return pl_tex_create(gpu, &p);
			};
			auto readbackCodes = [&](pl_tex tex) {
				pl_tex_params rp{};
				rp.w = w; rp.h = h; rp.format = tex->params.format; rp.host_readable = true; rp.blit_dst = true;
				TexGuard readable(gpu, pl_tex_create(gpu, &rp));
				Assert::IsNotNull(readable.t);
				pl_tex_blit_params bp{}; bp.src = tex; bp.dst = readable.t;
				pl_tex_blit(gpu, &bp);
				std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
				pl_tex_transfer_params dl{}; dl.tex = readable.t; dl.ptr = raw.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				Image out(w, h);
				for (size_t i = 0; i < raw.size(); ++i) out.v[i] = raw[i] / 64.0;
				return out;
			};
			auto frameOf = [](pl_tex tex) {
				pl_frame f{};
				f.num_planes = 1;
				f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
				f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
				f.color = pl_color_space_bt709;
				return f;
			};

			TexGuard tex0(gpu, uploadFull(c.frames[0])), tex1(gpu, uploadFull(c.frames[1]));
			Assert::IsTrue(tex0.t != nullptr && tex1.t != nullptr);

			VpSkeleton::SkeletonMemo memoP7;
			CameraModel::CameraMotion cam;
			cam.seq = 2u; cam.pairFrom = 1u; cam.pairTo = 2u;
			cam.pairSeconds = 1.0 / 23.976;
			cam.valid = true;
			cam.reason = CameraModel::Reason::Ok;
			cam.confidence = 0.85;
			cam.params.type = CameraModel::ModelType::Translation;
			cam.params.h[2] = 12.0; cam.params.h[5] = 0.0;
			cam.picture.left = 0; cam.picture.top = 0; cam.picture.right = w; cam.picture.bottom = h;
			VpSkeleton::OwnerFill p7Fill;
			p7Fill.renderThreadOnly = false;
			p7Fill.fill = [cam](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) -> bool {
				if (req.seq != 2u) return false;
				uint8_t buf[CameraLink::kRecordPodMaxBytes]{};
				const size_t n = CameraLink::EncodeRecordPod(cam, buf, sizeof(buf));
				if (n == 0u) return false;
				out.bytes.assign(buf, buf + n);
				return true;
			};
			Assert::IsTrue(memoP7.RegisterOwnerFill(VpSkeleton::kP7CameraFit, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP7CameraFit), std::move(p7Fill)) == Reg::Registered);

			VpDetailRestore::SceneReaderFn panCutReader = [](uint64_t seq, FrameAnalysis::Scene& sc) -> bool {
				sc = FrameAnalysis::Scene();
				if (seq == 2u)
				{
					sc.cut = true;
					sc.confirmed = true;
					sc.cutStrength = 1.0f;
				}
				return true;
			};

			PulseGuard dpVeto(gpu), dpNoVeto(gpu);
			pl_tex inputs[2] = { tex0.t, tex1.t };
			pl_frame outVeto{}, outNoVeto{};
			for (int k = 0; k < 2; ++k)
			{
				DetailPulse::Params pVeto;
				Assert::IsTrue(pVeto.vectorVeto, L"DetailPulse::Params::vectorVeto defaults true");
				pVeto.sequence = static_cast<uint64_t>(k) + 1u;
				pVeto.sceneReader = panCutReader;
				pVeto.memo = &memoP7;
				Assert::IsTrue(dpVeto.mod.Apply(gpu, dispatch, frameOf(inputs[k]), pVeto, outVeto), W(dpVeto.mod.LastError().c_str()).c_str());

				DetailPulse::Params pNoVeto = pVeto;
				pNoVeto.vectorVeto = false;
				Assert::IsTrue(dpNoVeto.mod.Apply(gpu, dispatch, frameOf(inputs[k]), pNoVeto, outNoVeto), W(dpNoVeto.mod.LastError().c_str()).c_str());
			}

			Assert::AreEqual(1ull, dpVeto.mod.SceneVectorVetoes(), L"DP with default vectorVeto=true vetoes pan-cut restart on trusted P7");
			Assert::AreEqual(1.0f, dpVeto.mod.LastHistoryKeep(), 1e-6f, L"vetoed pan keeps full history");
			Assert::AreEqual(0ull, dpNoVeto.mod.SceneVectorVetoes(), L"negative control: vectorVeto=false does NOT veto pan-cut restart");
			Assert::AreEqual(0.0f, dpNoVeto.mod.LastHistoryKeep(), 1e-6f, L"negative control: unvetoed pan cut restarts history");

			const Image imgVeto = readbackCodes(outVeto.planes[0].texture);
			const Image imgNoVeto = readbackCodes(outNoVeto.planes[0].texture);
			double maxVetoBoost = 0.0, maxVetoVsNoVeto = 0.0;
			for (size_t i = 0; i < imgVeto.v.size(); ++i)
			{
				maxVetoBoost = (std::max)(maxVetoBoost, std::fabs(imgVeto.v[i] - c.frames[1].v[i]));
				maxVetoVsNoVeto = (std::max)(maxVetoVsNoVeto, std::fabs(imgVeto.v[i] - imgNoVeto.v[i]));
			}
			Assert::IsTrue(maxVetoBoost > 0.5, L"vetoed DP retained frame 0 history and boosted coarse frame 1");
			Assert::IsTrue(maxVetoVsNoVeto > 0.5, L"negative control: suppressing vs taking restart materially alters DP GPU output");
		}
	};
}

// B2 (shared unit U2): DetailPulse's fine-band Gaussian now calls VpUnit::BandFromBase. The frozen copy below is the
// private implementation it replaced; the numbers (and so the emitted GLSL tables) must be bit-identical.
namespace DetailPulseBandFromBaseNs
{
	inline int OldFineRadius(double scale) { return static_cast<int>(std::ceil(3.0 * DetailPulse::kSigmaFine * scale - 1e-9)); }
	inline double OldFineWeight(int i, double scale)
	{
		const double s = DetailPulse::kSigmaFine * scale;
		const int r = OldFineRadius(scale);
		double sum = 0.0;
		for (int k = -r; k <= r; ++k) sum += std::exp(-0.5 * k * k / (s * s));
		return std::exp(-0.5 * i * i / (s * s)) / sum;
	}

	TEST_CLASS(DetailPulseBandFromBaseIdentityTests)
	{
	public:
		TEST_METHOD(FineRadiusAndWeightAreBitIdenticalToTheFrozenPrivateCopy)
		{
			std::vector<double> scales;
			for (double s = 1.0; s <= 2.5001; s += 0.0625) scales.push_back(s);
			for (double lines : { 480.0, 720.0, 1080.0, 1440.0, 2160.0, 3240.0, 4320.0, 5400.0, 8640.0 }) scales.push_back(DetailPulse::Scale(lines));
			for (double scale : scales)
			{
				const int r = OldFineRadius(scale);
				Assert::AreEqual(r, DetailPulse::FineRadius(scale), L"radius");
				for (int i = -r; i <= r; ++i)
				{
					const double a = OldFineWeight(i, scale), b = DetailPulse::FineWeight(i, scale);
					Assert::IsTrue(std::memcmp(&a, &b, sizeof(double)) == 0, L"weight bits");
				}
			}
		}

		TEST_METHOD(EmittedFineWeightTableTextIsByteIdentical)
		{
			for (double scale : { 1.0, 1.5, 2.0, 2.5 })
			{
				const int r = OldFineRadius(scale);
				const std::string before = VpvGpu::ConstArray("w", 2 * r + 1, [&](int i) { return OldFineWeight(i - r, scale); });
				const std::string after = VpvGpu::ConstArray("w", 2 * DetailPulse::FineRadius(scale) + 1,
					[&](int i) { return DetailPulse::FineWeight(i - DetailPulse::FineRadius(scale), scale); });
				Assert::AreEqual(before, after, L"GLSL table text");
			}
		}

		// W23-B3 M2#2 (U2 B4 CPU BandMemo): DetailPulse::AcquireFineCpuBand & AcquireWideCpuBand hit CPU kBandMemo
		// once per key, match independent BandFromBase::BlurTaps bit-for-bit, and visibly report fallbacks.
		TEST_METHOD(DetailPulseCpuBandAcquireAndVisibleFallback)
		{
			namespace BFB = VpUnit::BandFromBase;
			namespace GB = Vpv::GrainBand;
			using Reg = VpSkeleton::SkeletonMemo::Registration;

			const int W = 64, H = 48;
			const int HW = (W + 1) / 2, HH = (H + 1) / 2;
			const size_t nFull = static_cast<size_t>(W) * H;
			const size_t nHalf = static_cast<size_t>(HW) * HH;

			std::vector<float> fullPlane(nFull, 0.0f);
			uint32_t rng = 0x2468ace0u;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					rng = rng * 1664525u + 1013904223u;
					const float noise = (static_cast<float>((rng >> 8) & 1023u) / 1023.0f - 0.5f) * 0.2f;
					const float edge = (x >= 24 && x < 40 && y >= 12 && y < 36) ? 0.4f : 0.15f;
					fullPlane[static_cast<size_t>(y) * W + x] = (std::min)(1.0f, (std::max)(0.0f, edge + noise));
				}

			std::vector<uint16_t> halfU16(nHalf, 0u);
			std::vector<float> halfFloat(nHalf, 0.0f);
			constexpr float kInvU16Max = 1.0f / 65535.0f;
			for (int y = 0; y < HH; ++y)
				for (int x = 0; x < HW; ++x)
				{
					const float avg = 0.25f * (fullPlane[static_cast<size_t>(2 * y) * W + 2 * x]
						+ fullPlane[static_cast<size_t>(2 * y) * W + 2 * x + 1]
						+ fullPlane[static_cast<size_t>(2 * y + 1) * W + 2 * x]
						+ fullPlane[static_cast<size_t>(2 * y + 1) * W + 2 * x + 1]);
					const uint16_t u = static_cast<uint16_t>(std::lround(avg * 65535.0f));
					halfU16[static_cast<size_t>(y) * HW + x] = u;
					halfFloat[static_cast<size_t>(y) * HW + x] = static_cast<float>(u) * kInvU16Max;
				}

			VpSkeleton::SkeletonMemo memo;
			const uint64_t kSeq = 50u;
			VpSkeleton::OwnerFill p13Cpu;
			p13Cpu.frameScoped = true;
			p13Cpu.renderThreadOnly = false;
			p13Cpu.fill = [&fullPlane, kSeq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq != kSeq) return false;
				out.bytes.resize(fullPlane.size() * sizeof(float));
				std::memcpy(out.bytes.data(), fullPlane.data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP13SourceRing), std::move(p13Cpu)) == Reg::Registered);

			VpSkeleton::OwnerFill p1Cpu;
			p1Cpu.frameScoped = true;
			p1Cpu.renderThreadOnly = false;
			p1Cpu.fill = [&halfU16, kSeq](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo&, VpSkeleton::OwnerFillOutput& out) {
				if (req.seq != kSeq) return false;
				out.bytes.resize(halfU16.size() * sizeof(uint16_t));
				std::memcpy(out.bytes.data(), halfU16.data(), out.bytes.size());
				return true;
			};
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP1HalfLuma, VpSkeleton::AcquireForm::Cpu,
				VpSkeleton::SkeletonMemo::OwnerUnitOf(VpSkeleton::kP1HalfLuma), std::move(p1Cpu)) == Reg::Registered);

			const std::shared_ptr<GB::Counters> counters = std::make_shared<GB::Counters>();
			Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Cpu,
				GB::kBandMemoUnit, GB::MakeBandMemoCpuOwnerFill(counters)) == Reg::Registered);

			// Independent BlurTaps references.
			std::vector<float> fineRef, wideRef;
			const std::vector<float> fineTaps = BFB::Taps<float>(BFB::kSigmaFine, 3.0, true, 0.0);
			BFB::BlurTaps<float>(fullPlane, fineRef, W, H, fineTaps, BFB::EdgeMode::Replicate);
			const double wideSigma = DetailPulse::WideSigma(1.0);
			const int wr = GrainPreserve::LowRadius(wideSigma);
			std::vector<float> wideTaps(static_cast<size_t>(2 * wr + 1));
			for (int i = -wr; i <= wr; ++i)
				wideTaps[static_cast<size_t>(i + wr)] = static_cast<float>(GrainPreserve::LowWeight(i, wideSigma));
			BFB::BlurTaps<float>(halfFloat, wideRef, HW, HH, wideTaps, BFB::EdgeMode::Replicate);

			// Memo hits: 1 fine fill + 1 wide fill, reused on second acquire.
			const DetailPulse::CpuBandResult fineMemo = DetailPulse::AcquireFineCpuBand(fullPlane, W, H, 1.0, &memo, kSeq);
			const DetailPulse::CpuBandResult wideMemo = DetailPulse::AcquireWideCpuBand(halfFloat, HW, HH, 1.0, &memo, kSeq);
			Assert::IsTrue(fineMemo.fromBandMemo && fineMemo.fallbackReason.empty(), L"AcquireFineCpuBand hits kBandMemo");
			Assert::IsTrue(wideMemo.fromBandMemo && wideMemo.fallbackReason.empty(), L"AcquireWideCpuBand hits kBandMemo");
			Assert::AreEqual(uint64_t(2), counters->fills.load(), L"1 fine fill + 1 wide fill");
			Assert::AreEqual(fineRef.size(), fineMemo.samples.size());
			Assert::AreEqual(wideRef.size(), wideMemo.samples.size());
			Assert::IsTrue(std::memcmp(fineMemo.samples.data(), fineRef.data(), fineRef.size() * sizeof(float)) == 0,
				L"AcquireFineCpuBand matches independent BlurTaps bit-for-bit");
			Assert::IsTrue(std::memcmp(wideMemo.samples.data(), wideRef.data(), wideRef.size() * sizeof(float)) == 0,
				L"AcquireWideCpuBand matches independent BlurTaps bit-for-bit");

			const DetailPulse::CpuBandResult fineRepeat = DetailPulse::AcquireFineCpuBand(fullPlane, W, H, 1.0, &memo, kSeq);
			const DetailPulse::CpuBandResult wideRepeat = DetailPulse::AcquireWideCpuBand(halfFloat, HW, HH, 1.0, &memo, kSeq);
			Assert::AreEqual(uint64_t(2), counters->fills.load(), L"repeated CPU band acquires reuse memo without recompute");
			Assert::IsTrue(fineRepeat.fromBandMemo && wideRepeat.fromBandMemo);

			// Negative controls: memo=nullptr, seq=0, unregistered memo visibly report fallback while staying bit-exact.
			VpSkeleton::SkeletonMemo emptyMemo;
			const DetailPulse::CpuBandResult fineNull = DetailPulse::AcquireFineCpuBand(fullPlane, W, H, 1.0, nullptr, kSeq);
			const DetailPulse::CpuBandResult wideNull = DetailPulse::AcquireWideCpuBand(halfFloat, HW, HH, 1.0, nullptr, kSeq);
			const DetailPulse::CpuBandResult fineUnreg = DetailPulse::AcquireFineCpuBand(fullPlane, W, H, 1.0, &emptyMemo, kSeq);
			const DetailPulse::CpuBandResult wideUnkeyed = DetailPulse::AcquireWideCpuBand(halfFloat, HW, HH, 1.0, &memo, 0u);
			Assert::IsFalse(fineNull.fromBandMemo || fineNull.fallbackReason.empty(), L"fine memo=nullptr fallback visible");
			Assert::IsFalse(wideNull.fromBandMemo || wideNull.fallbackReason.empty(), L"wide memo=nullptr fallback visible");
			Assert::IsFalse(fineUnreg.fromBandMemo || fineUnreg.fallbackReason.empty(), L"fine unregistered fallback visible");
			Assert::IsFalse(wideUnkeyed.fromBandMemo || wideUnkeyed.fallbackReason.empty(), L"wide seq=0 fallback visible");
			Assert::IsTrue(std::memcmp(fineNull.samples.data(), fineRef.data(), fineRef.size() * sizeof(float)) == 0, L"fine fallback bit-exact");
			Assert::IsTrue(std::memcmp(wideNull.samples.data(), wideRef.data(), wideRef.size() * sizeof(float)) == 0, L"wide fallback bit-exact");

			// Non-vacuity negative control: unblurred planes diverge substantially from blurred bands.
			float maxFineDiff = 0.0f, maxWideDiff = 0.0f;
			for (size_t i = 0; i < nFull; ++i)
				maxFineDiff = (std::max)(maxFineDiff, std::fabs(fineMemo.samples[i] - fullPlane[i]));
			for (size_t i = 0; i < nHalf; ++i)
				maxWideDiff = (std::max)(maxWideDiff, std::fabs(wideMemo.samples[i] - halfFloat[i]));
			Assert::IsTrue(maxFineDiff > 0.05f && maxWideDiff > 0.05f, L"negative control: blur materially transforms input");
		}
	};
}
