#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <SourceResolutionDetector.h>
#include <vprenderer/DetailPulse.h>
#include <vprenderer/TextureRestore.h>

#include "DetailPulseModel.h"
#include "TextureRestoreModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-detail-restore.md: the settings (validation, the profile file), and the source-resolution
// input rule (neither stage can change what the detector reads, nor its decision).
namespace DetailRestoreConfigTestsNs
{
	namespace
	{
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		bool Valid(const char* key, const char* value)
		{
			std::string expected;
			return RendererProfileConfig::ValidateProfileSetting("input", key, value, expected);
		}

		// ---- the source-resolution detector's input -------------------------------------------
		constexpr double kPi = 3.14159265358979323846;
		constexpr int kTile = 256;

		struct Wave { double fx, fy, px, py, amplitude; };

		// A scene mastered at 1080 (half of 2160 lines): waves up to the master's Nyquist.
		std::vector<Wave> MakeScene(unsigned seed)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<double> u(0.0, 1.0);
			std::vector<Wave> waves;
			for (int i = 0; i < 260; ++i)
			{
				const double f = 0.004 * std::pow(0.49 / 0.004, u(rng));
				const double angle = u(rng) * kPi;
				Wave w{ f * std::cos(angle), f * std::sin(angle), u(rng) * 2 * kPi, u(rng) * 2 * kPi, 0.0 };
				w.amplitude = 0.012 / std::sqrt(f);
				waves.push_back(w);
			}
			return waves;
		}

		double CatmullRom(double x)
		{
			x = std::fabs(x);
			return x < 1 ? 1.5 * x * x * x - 2.5 * x * x + 1 : x < 2 ? -0.5 * x * x * x + 2.5 * x * x - 4 * x + 2 : 0.0;
		}

		// A native 4K tile (kTile square) that is a 1080 picture upscaled 2:1 (Catmull-Rom), 10-bit codes in 0..1.
		std::vector<float> UpscaledTile(unsigned seed)
		{
			const double ratio = 0.5;
			const std::vector<Wave> scene = MakeScene(seed);
			const int margin = 4;
			const int mw = static_cast<int>(std::ceil(kTile * ratio)) + 2 * margin;
			const int mx0 = static_cast<int>(std::floor(1000 * ratio)) - margin, my0 = static_cast<int>(std::floor(700 * ratio)) - margin;
			std::vector<double> master(static_cast<size_t>(mw) * mw, 0.45);
			for (int y = 0; y < mw; ++y)
				for (int x = 0; x < mw; ++x)
				{
					double v = 0.45;
					for (const Wave& w : scene)
						v += w.amplitude * std::cos(2 * kPi * (w.fx * (mx0 + x) + w.fy * (my0 + y)) + w.px + w.py);
					master[static_cast<size_t>(y) * mw + x] = v;
				}
			auto resample = [&](const double* src, int stride, double* dst, int dstStride, int origin, int m0) {
				for (int x = 0; x < kTile; ++x)
				{
					const double u = (origin + x + 0.5) * ratio - 0.5 - m0;
					const int c = static_cast<int>(std::floor(u));
					double sum = 0, wsum = 0;
					for (int k = c - 3; k <= c + 4; ++k)
					{
						if (k < 0 || k >= mw) continue;
						const double w = CatmullRom(u - k);
						sum += w * src[static_cast<size_t>(k) * stride];
						wsum += w;
					}
					dst[static_cast<size_t>(x) * dstStride] = wsum != 0 ? sum / wsum : 0;
				}
			};
			std::vector<double> rows(static_cast<size_t>(mw) * kTile), native(static_cast<size_t>(kTile) * kTile);
			for (int y = 0; y < mw; ++y) resample(&master[static_cast<size_t>(y) * mw], 1, &rows[static_cast<size_t>(y) * kTile], 1, 1000, mx0);
			for (int x = 0; x < kTile; ++x) resample(&rows[x], kTile, &native[x], kTile, 700, my0);
			std::vector<float> out(native.size());
			for (size_t i = 0; i < native.size(); ++i)
				out[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, native[i])) * 1023.0) / 1023.0);
			return out;
		}

		std::vector<double> Gaussian(const std::vector<double>& in, double sigma)
		{
			const int radius = static_cast<int>(std::ceil(3.0 * sigma));
			std::vector<double> k(2 * radius + 1);
			double sum = 0;
			for (int i = -radius; i <= radius; ++i) sum += k[i + radius] = std::exp(-0.5 * i * i / (sigma * sigma));
			for (double& v : k) v /= sum;
			std::vector<double> tmp(in.size()), out(in.size());
			for (int y = 0; y < kTile; ++y)
				for (int x = 0; x < kTile; ++x)
				{
					double s = 0;
					for (int i = -radius; i <= radius; ++i)
						s += k[i + radius] * in[static_cast<size_t>(y) * kTile + (std::min)(kTile - 1, (std::max)(0, x + i))];
					tmp[static_cast<size_t>(y) * kTile + x] = s;
				}
			for (int y = 0; y < kTile; ++y)
				for (int x = 0; x < kTile; ++x)
				{
					double s = 0;
					for (int i = -radius; i <= radius; ++i)
						s += k[i + radius] * tmp[static_cast<size_t>((std::min)(kTile - 1, (std::max)(0, y + i))) * kTile + x];
					out[static_cast<size_t>(y) * kTile + x] = s;
				}
			return out;
		}

		// The worst case of either stage on a tile: both detail bands (D1 = Y - G(1.0) Y,
		// D2 = G(1.0) Y - G(2.5) Y) scaled by `gain` (the caps 1.5 and 2.0, or both: 3.0), no gate,
		// no dead zone, no per-tile veto: the most energy the stages could add. Writes a new tile.
		std::vector<float> WorstCaseDetailStage(const std::vector<float>& tile, double gain)
		{
			std::vector<double> y(tile.begin(), tile.end());
			const std::vector<double> g1 = Gaussian(y, 1.0), g2 = Gaussian(g1, std::sqrt(2.5 * 2.5 - 1.0));   // G(2.5) = G(1.0) then this
			std::vector<float> out(tile.size());
			for (size_t i = 0; i < y.size(); ++i)
			{
				const double d1 = y[i] - g1[i], d2 = g1[i] - g2[i];
				const double v = y[i] + (gain - 1.0) * (d1 + d2);
				out[i] = static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0);
			}
			return out;
		}

		// The renderer's order: the monitor reads the frame BEFORE the render hook (re-base ->
		// debander -> detail pulse -> texture restore) runs, and the hook writes new textures.
		struct Rendered
		{
			std::vector<std::vector<float>> monitorInput;   // sourceMonitor.Frame(renderImage)
			std::vector<std::vector<float>> hookOutput;     // what libplacebo scales from
		};

		Rendered RenderModel(const std::vector<std::vector<float>>& original, double stageGain)
		{
			Rendered r;
			r.monitorInput = original;   // captured first
			for (const std::vector<float>& tile : original)
				r.hookOutput.push_back(stageGain > 1.0 ? WorstCaseDetailStage(tile, stageGain) : tile);
			return r;
		}

		SourceResolution::FrameReading Read(const std::vector<std::vector<float>>& tiles)
		{
			std::vector<SourceResolution::Spectrum> spectra(tiles.size());
			for (size_t i = 0; i < tiles.size(); ++i)
				Assert::IsTrue(SourceResolution::Analyse(tiles[i].data(), kTile, kTile, spectra[i], 1023));
			SourceResolution::Thresholds t;
			t.codeMaximum = 1023;
			return SourceResolution::ReadFrame(spectra, t);
		}

		bool SameBytes(const std::vector<std::vector<float>>& a, const std::vector<std::vector<float>>& b)
		{
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (a[i].size() != b[i].size() || std::memcmp(a[i].data(), b[i].data(), a[i].size() * sizeof(float)) != 0) return false;
			return true;
		}

		std::string ReadText(const std::string& path)
		{
			std::ifstream in(path, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		std::string RepositoryRoot()
		{
			std::string root = __FILE__;
			std::replace(root.begin(), root.end(), '/', '\\');
			root.resize(root.rfind("\\src\\VideoProcessor-Test\\"));
			return root;
		}
	}

	TEST_CLASS(DetailRestoreConfigTests)
	{
	public:
		TEST_METHOD(DetailRestoreKeysValidate)
		{
			for (const char* v : { "on", "off", "On" })
				Assert::IsTrue(Valid("detail_pulse_stabilise", v), Wide(v).c_str());
			for (const char* v : { "auto", "statistical", "1", "" })
				Assert::IsFalse(Valid("detail_pulse_stabilise", v), Wide(v).c_str());
			// detail_pulse_fast (2026-10-18): on (default) / off.
			for (const char* v : { "on", "off", "Off" })
				Assert::IsTrue(Valid("detail_pulse_fast", v), Wide(v).c_str());
			for (const char* v : { "auto", "fast", "1", "" })
				Assert::IsFalse(Valid("detail_pulse_fast", v), Wide(v).c_str());
			Assert::IsTrue(DetailPulse::kDefaultFast && DetailPulse::Params().fast, L"detail_pulse_fast defaults on");
			for (const char* v : { "off", "statistical", "Statistical" })
				Assert::IsTrue(Valid("texture_restore", v), Wide(v).c_str());
			for (const char* v : { "on", "auto", "neural", "" })
				Assert::IsFalse(Valid("texture_restore", v), Wide(v).c_str());
		}

		TEST_METHOD(DetailRestoreCapRangesMatchTheModules)
		{
			Assert::AreEqual(1.0, DetailPulse::kCapMin);
			Assert::AreEqual(1.5, DetailPulse::kCapMax);
			Assert::AreEqual(1.35, DetailPulse::kDefaultCap);
			Assert::AreEqual(1.0, TextureRestore::kCapMin);
			Assert::AreEqual(2.0, TextureRestore::kCapMax);
			Assert::AreEqual(1.5, TextureRestore::kDefaultCap);
			for (const char* v : { "1", "1.00", "1.05", "1.35", "1.5", "1.50" })
				Assert::IsTrue(Valid("detail_pulse_cap", v), Wide(v).c_str());
			for (const char* v : { "0.99", "0", "1.51", "2", "abc", "" })
				Assert::IsFalse(Valid("detail_pulse_cap", v), Wide(v).c_str());
			for (const char* v : { "1", "1.00", "1.5", "1.95", "2", "2.00" })
				Assert::IsTrue(Valid("texture_restore_cap", v), Wide(v).c_str());
			for (const char* v : { "0.99", "2.01", "3", "abc", "" })
				Assert::IsFalse(Valid("texture_restore_cap", v), Wide(v).c_str());
		}

		TEST_METHOD(DetailRestoreKeysLoadFromAProfileFile)
		{
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			const std::string path = std::string(dir) + "vp-detail-restore-keys-test.cfg";
			auto loads = [&](const char* body) {
				{
					std::ofstream out(path, std::ios::trunc | std::ios::binary);
					out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n[vprenderer.Default]\nquality: high\n" << body;
				}
				ConfigFile config;
				if (!config.Load(path, ConfigFile::ReadPolicy::Fresh)) return false;
				std::string error;
				return MainConfigSchema::Validate(config, error);
			};
			Assert::IsTrue(loads(""), L"no keys");
			Assert::IsTrue(loads("detail_pulse_stabilise: off\ndetail_pulse_cap: 1.2\ntexture_restore: statistical\ntexture_restore_cap: 1.75\n"), L"valid keys");
			DeleteFileA(path.c_str());
		}

		// Owner requirement: the stages cannot change what the source-resolution detector reads.
		// The renderer takes the monitor's input (renderImage) before the render hook; the hook's
		// stages write to new textures. (a) The input is bit-identical with the stages on and off;
		// (b) even a worst-case output (both bands at the maximum caps) would decide the same.
		TEST_METHOD(DetailRestoreSourceResolutionInputUnchanged)
		{
			std::vector<std::vector<float>> original;
			for (unsigned seed = 1; seed <= 4; ++seed) original.push_back(UpscaledTile(seed));
			const Rendered off = RenderModel(original, 1.0);
			const Rendered pulse = RenderModel(original, DetailPulse::kCapMax);
			const Rendered restore = RenderModel(original, TextureRestore::kCapMax);
			const Rendered both = RenderModel(original, DetailPulse::kCapMax * TextureRestore::kCapMax);
			Assert::IsTrue(SameBytes(off.monitorInput, original), L"off: the monitor's input is the original frame");
			Assert::IsTrue(SameBytes(pulse.monitorInput, off.monitorInput), L"detail pulse on: the monitor's input is unchanged");
			Assert::IsTrue(SameBytes(restore.monitorInput, off.monitorInput), L"texture restore on: the monitor's input is unchanged");
			Assert::IsTrue(SameBytes(both.monitorInput, off.monitorInput), L"both on: the monitor's input is unchanged");
			Assert::IsFalse(SameBytes(both.hookOutput, off.hookOutput), L"the model's worst case does change the hook's output (the check can see a change)");
			Assert::IsTrue(SameBytes(off.hookOutput, original), L"off: the hook passes the frame through");
			const SourceResolution::FrameReading before = Read(off.monitorInput);
			const SourceResolution::FrameReading after = Read(both.monitorInput);
			Assert::AreEqual(before.ratio, after.ratio, 1e-12, L"same reading from the same input");
		}

		TEST_METHOD(DetailRestoreSourceResolutionInputWorstCaseStillDecides1080)
		{
			std::vector<std::vector<float>> original;
			for (unsigned seed = 1; seed <= 4; ++seed) original.push_back(UpscaledTile(seed));
			const SourceResolution::FrameReading base = Read(original);
			char line[160];
			snprintf(line, sizeof(line), "1080-in-4K reading: ratio %.4f (informative tiles %d)\n", base.ratio, base.informativeTiles);
			Logger::WriteMessage(line);
			Assert::IsTrue(std::fabs(base.ratio - 0.5) < 0.02, L"the synthetic 1080-in-4K frame reads 1080");
			// The detail pulse's own cap (1.5) on both bands, ungated: still 1080.
			const Rendered r = RenderModel(original, DetailPulse::kCapMax);
			const SourceResolution::FrameReading worst = Read(r.hookOutput);
			snprintf(line, sizeof(line), "worst case x%.2f: ratio %.4f\n", DetailPulse::kCapMax, worst.ratio);
			Logger::WriteMessage(line);
			Assert::IsTrue(std::fabs(worst.ratio - base.ratio) < 0.02, L"worst-case detail pulse output changes the decision");
		}

		// Measured finding (the lead replaces the crude model with the real ones): the UNGATED model
		// at texture restore's cap 2.0 (and both stages, 3.0) lifts the 1080 picture's leakage above
		// its Nyquist so far that the detector would read native. The real stages are gated (dead
		// zone, focus gate, per-tile veto), so this bounds nothing by itself; it is why the monitor
		// must keep reading the original frame (DetailRestoreSourceResolutionInputUnchanged and
		// ...InputOrderInTheRenderer pin that).
		TEST_METHOD(DetailRestoreSourceResolutionInputUngatedWorstCaseWouldFlipTheReading)
		{
			std::vector<std::vector<float>> original;
			for (unsigned seed = 1; seed <= 4; ++seed) original.push_back(UpscaledTile(seed));
			for (double gain : { TextureRestore::kCapMax, DetailPulse::kCapMax * TextureRestore::kCapMax })
			{
				const SourceResolution::FrameReading worst = Read(RenderModel(original, gain).hookOutput);
				char line[120];
				snprintf(line, sizeof(line), "ungated worst case x%.2f: ratio %.4f\n", gain, worst.ratio);
				Logger::WriteMessage(line);
				Assert::IsTrue(worst.ratio > 0.9, L"the ungated worst case no longer flips the reading (the model changed?)");
			}
		}

		// The real stages (their CPU reference models, DetailPulseModel / TextureRestoreModel, which
		// mirror the shaders) at their maximum caps, in the renderer's order, on a coded 1080-in-4K
		// sequence whose B-frames lost fine detail (so both stages engage): even if the detector
		// read their output instead of the original, every frame would still read 1080. (The
		// renderer never shows it their output: ...InputUnchanged and ...InputOrderInTheRenderer.)
		TEST_METHOD(DetailRestoreSourceResolutionInputRealStagesStillDecide1080)
		{
			std::vector<std::vector<float>> tiles;
			for (unsigned seed = 1; seed <= 4; ++seed) tiles.push_back(UpscaledTile(seed));
			const SourceResolution::FrameReading base = Read(tiles);
			Assert::IsTrue(std::fabs(base.ratio - 0.5) < 0.02, L"the synthetic 1080-in-4K frame reads 1080");
			// The four tiles as one 2x2 picture (codes), and back.
			const int W = 2 * kTile;
			auto compose = [&](const std::vector<std::vector<float>>& t) {
				GrainModel::Image img(W, W);
				for (int q = 0; q < 4; ++q)
					for (int y = 0; y < kTile; ++y)
						for (int x = 0; x < kTile; ++x)
							img.at((q % 2) * kTile + x, (q / 2) * kTile + y) = 1023.0 * t[q][static_cast<size_t>(y) * kTile + x];
				return img;
			};
			auto split = [&](const std::vector<double>& v) {
				std::vector<std::vector<float>> t(4, std::vector<float>(static_cast<size_t>(kTile) * kTile));
				for (int q = 0; q < 4; ++q)
					for (int y = 0; y < kTile; ++y)
						for (int x = 0; x < kTile; ++x)
						{
							const double c = v[static_cast<size_t>((q / 2) * kTile + y) * W + (q % 2) * kTile + x];
							t[q][static_cast<size_t>(y) * kTile + x] = static_cast<float>(std::round((std::min)(1023.0, (std::max)(0.0, c))) / 1023.0);
						}
				return t;
			};
			const GrainModel::Image original = compose(tiles);
			// A coded stand-in: I-frames as mastered, the other frames with part of the fine band
			// gone (a mix towards a 0.8 px blur), as a starved encode's B-frames.
			std::vector<double> blurred;
			{
				std::vector<double> y(original.v.begin(), original.v.end());
				// Gaussian() works on kTile squares: blur each quadrant.
				blurred.assign(y.size(), 0.0);
				for (int q = 0; q < 4; ++q)
				{
					std::vector<double> quad(static_cast<size_t>(kTile) * kTile);
					for (int yy = 0; yy < kTile; ++yy)
						for (int xx = 0; xx < kTile; ++xx)
							quad[static_cast<size_t>(yy) * kTile + xx] = y[static_cast<size_t>((q / 2) * kTile + yy) * W + (q % 2) * kTile + xx];
					const std::vector<double> b = Gaussian(quad, 0.8);
					for (int yy = 0; yy < kTile; ++yy)
						for (int xx = 0; xx < kTile; ++xx)
							blurred[static_cast<size_t>((q / 2) * kTile + yy) * W + (q % 2) * kTile + xx] = b[static_cast<size_t>(yy) * kTile + xx];
				}
			}
			DetailPulseModel::Model pulse;
			DetailPulseModel::Settings ps;
			ps.cap = DetailPulse::kCapMax;
			ps.planeLines = 2160.0;
			ps.sourceHz = 23.976;
			TextureRestoreModel::Model restore;
			TextureRestore::Params tp;
			tp.cap = TextureRestore::kCapMax;
			tp.planeLines = 2160.0;
			tp.sourceHz = 23.976;
			double maxPulse = 1.0, maxChange = 0.0, worstShift = 0.0;
			for (int f = 0; f < 12; ++f)
			{
				const bool iFrame = f % 6 == 0;
				GrainModel::Image coded(W, W);
				for (size_t i = 0; i < coded.v.size(); ++i)
					coded.v[i] = std::round(iFrame ? original.v[i] : 0.4 * original.v[i] + 0.6 * blurred[i]);
				const DetailPulseModel::Frame stabilised = pulse.Process(coded, static_cast<uint64_t>(f + 1), ps);
				maxPulse = (std::max)(maxPulse, (std::max)(stabilised.g1, stabilised.g2));
				TextureRestoreModel::Image in(W, W);
				in.v.assign(stabilised.out.v.begin(), stabilised.out.v.end());
				tp.sequence = static_cast<uint64_t>(f + 1);
				const TextureRestoreModel::Image out = restore.Frame(in, tp);
				for (size_t i = 0; i < out.v.size(); ++i) maxChange = (std::max)(maxChange, std::fabs(out.v[i] - coded.v[i]));
				const SourceResolution::FrameReading codedReading = Read(split(coded.v));
				const SourceResolution::FrameReading staged = Read(split(out.v));
				char line[200];
				snprintf(line, sizeof(line), "frame %d (%s): coded ratio %.4f, after both stages %.4f, pulse gains %.3f / %.3f\n",
					f, iFrame ? "I" : "B", codedReading.ratio, staged.ratio, stabilised.g1, stabilised.g2);
				Logger::WriteMessage(line);
				worstShift = (std::max)(worstShift, std::fabs(staged.ratio - codedReading.ratio));
				Assert::IsTrue(std::fabs(codedReading.ratio - 0.5) < 0.02, L"the coded frame reads 1080");
				Assert::IsTrue(std::fabs(staged.ratio - 0.5) < 0.02, L"the real stages' output would still read 1080");
			}
			char line[160];
			snprintf(line, sizeof(line), "max pulse gain %.3f, max code change %.2f, worst ratio shift %.4f\n", maxPulse, maxChange, worstShift);
			Logger::WriteMessage(line);
			Assert::IsTrue(maxPulse > 1.05, L"the detail pulse engaged (the check exercised the stage)");
		}

		// The renderer's own order, pinned in the source: the monitor reads renderImage before the
		// hook, and neither stage is ever given renderImage.
		TEST_METHOD(DetailRestoreSourceResolutionInputOrderInTheRenderer)
		{
			const std::string text = ReadText(RepositoryRoot() + "\\src\\VideoProcessor-Lib\\vprenderer\\LibplaceboVideoRenderer.cpp");
			Assert::IsFalse(text.empty(), L"the renderer source");
			const size_t monitor = text.find("sourceMonitor.Frame(d3d11->gpu, rebaseDispatch, renderImage,");
			const size_t debander = text.find("debander.Apply(d3d11->gpu, rebaseDispatch, frame,");
			// In the render hook (after its debander); the de-judder extras (another function)
			// apply the frame's decision to their pictures as repeats.
			const size_t pulse = debander == std::string::npos ? std::string::npos : text.find("detailPulse.Apply(", debander);
			const size_t restore = pulse == std::string::npos ? std::string::npos : text.find("textureRestore.Apply(", pulse);
			Assert::IsTrue(monitor != std::string::npos && pulse != std::string::npos && restore != std::string::npos && debander != std::string::npos, L"the calls");
			Assert::IsTrue(monitor < debander && debander < pulse && pulse < restore, L"monitor, then debander, then detail pulse, then texture restore");
			int calls = 0;
			for (const char* call : { "detailPulse.Apply(", "detailPulse.MeasureAhead(", "textureRestore.Apply(" })
				for (size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1))
				{
					const std::string statement = text.substr(at, text.find(';', at) - at);
					Assert::IsTrue(statement.find("renderImage") == std::string::npos, (L"never given renderImage: " + Wide(statement)).c_str());
					++calls;
				}
			// Each stage's Apply in the hook (on the drawn picture, and on A before a Stage B picture)
			// and on a de-judder extra; MeasureAhead once.
			Assert::AreEqual(7, calls, L"three Apply calls per stage and one MeasureAhead");
		}
	};
}
