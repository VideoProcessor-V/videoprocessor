#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/scaling/rebase/KernelInverse.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// docs/VP-stage12-build-plan.md, kernel-matched re-base (owner 2026-10-16: optimise 1080p).
namespace KernelInverseTest
{
	namespace
	{
		std::string Env(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string out = _dupenv_s(&v, &n, name) == 0 && v ? v : "";
			std::free(v);
			return out;
		}
		// A textured 1080 picture (smoothed noise), values 0.2..0.8.
		std::vector<float> Texture(int n, unsigned seed)
		{
			std::mt19937 rng(seed);
			std::uniform_real_distribution<float> u(0.0f, 1.0f);
			std::vector<float> noise(static_cast<size_t>(n) * n), out(noise.size());
			for (float& v : noise) v = u(rng);
			for (int y = 0; y < n; ++y)
				for (int x = 0; x < n; ++x)
				{
					float s = 0.0f;
					for (int k = -1; k <= 1; ++k)
						for (int j = -1; j <= 1; ++j)
							s += noise[static_cast<size_t>((std::min)(n - 1, (std::max)(0, y + k))) * n + (std::min)(n - 1, (std::max)(0, x + j))];
					out[static_cast<size_t>(y) * n + x] = 0.2f + 0.6f * s / 9.0f;
				}
			return out;
		}
		// Separable 2x upscale with a candidate.
		std::vector<float> Upscale(const KernelInverse::Candidate& c, const std::vector<float>& x, int m)
		{
			const int n = 2 * m;
			std::vector<float> rows(static_cast<size_t>(m) * n), out(static_cast<size_t>(n) * n), in(static_cast<size_t>(m)), o(static_cast<size_t>(n));
			for (int y = 0; y < m; ++y) KernelInverse::UpscaleLine(c, &x[static_cast<size_t>(y) * m], m, &rows[static_cast<size_t>(y) * n]);
			for (int col = 0; col < n; ++col)
			{
				for (int y = 0; y < m; ++y) in[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * n + col];
				KernelInverse::UpscaleLine(c, in.data(), m, o.data());
				for (int y = 0; y < n; ++y) out[static_cast<size_t>(y) * n + col] = o[static_cast<size_t>(y)];
			}
			return out;
		}
	}

	TEST_CLASS(KernelInverseTests)
	{
	public:
		TEST_METHOD(EachInverseRecoversItsUpscaleExactly)
		{
			std::mt19937 rng(5);
			std::uniform_real_distribution<float> u(0.1f, 0.9f);
			std::vector<float> x(400), y(800), back(400);
			for (float& v : x) v = u(rng);
			for (const auto& c : KernelInverse::Candidates())
			{
				KernelInverse::UpscaleLine(c, x.data(), 400, y.data());
				KernelInverse::InverseLine(c, y.data(), 800, back.data());
				double worst = 0.0;
				for (int i = 40; i < 360; ++i) worst = (std::max)(worst, static_cast<double>(std::fabs(back[static_cast<size_t>(i)] - x[static_cast<size_t>(i)])));
				char line[120];
				snprintf(line, sizeof(line), "%-28s %zu taps: worst error %.2e (codes %.4f)\n", c.name.c_str(), c.weights.size(), worst, worst * 1023);
				Logger::WriteMessage(line);
				Assert::IsTrue(worst * 1023 < 0.05, L"the inverse recovers the 1080 samples (well under a code)");
			}
		}

		TEST_METHOD(ATileIdentifiesItsKernelAndNativeIsNeverAccepted)
		{
			const int m = 96, n = 2 * m;
			const double floor10 = KernelInverse::FloorCodes(10.0, 10);
			const auto& list = KernelInverse::Candidates();
			for (size_t k = 0; k < list.size(); ++k)
			{
				std::vector<float> up = Upscale(list[k], Texture(m, 11 + static_cast<unsigned>(k)), m);
				for (float& v : up) v = std::round(v * 1023.0f) / 1023.0f;   // a 10-bit signal
				const KernelInverse::TileFit fit = KernelInverse::FitTile(up.data(), n, n);
				char line[160];
				snprintf(line, sizeof(line), "%-28s -> %-28s residual %.3f next %.3f floor %.3f %s\n", list[k].name.c_str(),
					list[static_cast<size_t>(fit.best)].name.c_str(), fit.bestResidual, fit.secondResidual, floor10,
					KernelInverse::Accept(fit, floor10) ? "accepted" : "not accepted");
				Logger::WriteMessage(line);
				if (KernelInverse::Accept(fit, floor10))
					Assert::AreEqual(static_cast<int>(k), fit.best, L"accepted only for the right kernel");
			}
			// Native 4K-class texture (not an upscale): never accepted.
			const std::vector<float> native = Texture(n, 99);
			const KernelInverse::TileFit fit = KernelInverse::FitTile(native.data(), n, n);
			Assert::IsFalse(KernelInverse::Accept(fit, floor10), L"native: no kernel fits");
		}

		TEST_METHOD(RealUpscalesAreIdentifiedAndRecovered)
		{
			// tools: ffmpeg x265 1080p decodes upscaled 2x by ffmpeg (VP_TEST_KERNELID_DIR, e.g.
			// D:\hfr\kernelid: <clip>_1080_dec.yuv and <clip>_4k_<kernel>.yuv, 10-bit 4:2:0).
			const std::string folder = Env("VP_TEST_KERNELID_DIR");
			if (folder.empty()) { Logger::WriteMessage("VP_TEST_KERNELID_DIR not set: skipped\n"); return; }
			auto luma = [](const std::string& path, int w, int h) {
				std::vector<float> v;
				FILE* f = nullptr;
				if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return v;
				std::vector<uint16_t> raw(static_cast<size_t>(w) * h);
				if (fread(raw.data(), 2, raw.size(), f) == raw.size())
				{
					v.resize(raw.size());
					for (size_t i = 0; i < raw.size(); ++i) v[i] = raw[i] / 1023.0f;
				}
				fclose(f);
				return v;
			};
			const double floor10 = KernelInverse::FloorCodes(10.0, 10);
			int checked = 0;
			for (const char* clip : { "Bosphorus", "ReadySetGo" })
			{
				const std::vector<float> dec = luma(folder + "\\" + clip + "_1080_dec.yuv", 1920, 1080);
				if (dec.empty()) continue;
				const std::pair<const char*, const char*> cases[] = { { "bicubic", "bicubic C0.6" }, { "lanczos", "lanczos3" } };
				for (const auto& entry : cases)
				{
					const char* file = entry.first;
					const std::string expect = entry.second;
					const std::vector<float> up = luma(folder + "\\" + clip + "_4k_" + file + ".yuv", 3840, 2160);
					if (up.empty()) continue;
					int right = 0, accepted = 0;
					double gainSum = 0.0;
					for (int t = 0; t < 4; ++t)
					{
						const int x1 = 300 + t * 380, y1 = 200 + t * 160, n = 256;
						const KernelInverse::TileFit fit = KernelInverse::FitTile(&up[static_cast<size_t>(2 * y1) * 3840 + 2 * x1], n, 3840);
						if (!KernelInverse::Accept(fit, floor10)) continue;
						++accepted;
						const auto& c = KernelInverse::Candidates()[static_cast<size_t>(fit.best)];
						right += c.name == expect ? 1 : 0;
						// The recovered 1080 against the decode, and a 2x2 average against it.
						double eInv = 0.0, eBox = 0.0;
						long count = 0;
						std::vector<float> row(static_cast<size_t>(n)), half(static_cast<size_t>(n / 2));
						std::vector<float> rows(static_cast<size_t>(n) * (n / 2));
						for (int y = 0; y < n; ++y)
							KernelInverse::InverseLine(c, &up[static_cast<size_t>(2 * y1 + y) * 3840 + 2 * x1], n, &rows[static_cast<size_t>(y) * (n / 2)]);
						for (int x = 0; x < n / 2; ++x)
						{
							for (int y = 0; y < n; ++y) row[static_cast<size_t>(y)] = rows[static_cast<size_t>(y) * (n / 2) + x];
							KernelInverse::InverseLine(c, row.data(), n, half.data());
							for (int y = 16; y < n / 2 - 16; ++y)
							{
								if (x < 16 || x >= n / 2 - 16) continue;
								const float truth = dec[static_cast<size_t>(y1 + y) * 1920 + x1 + x];
								const size_t b = static_cast<size_t>(2 * y1 + 2 * y) * 3840 + 2 * x1 + 2 * x;
								const float box = (up[b] + up[b + 1] + up[b + 3840] + up[b + 3841]) / 4.0f;
								eInv += (half[static_cast<size_t>(y)] - truth) * (half[static_cast<size_t>(y)] - truth);
								eBox += (box - truth) * (box - truth);
								++count;
							}
						}
						gainSum += 10.0 * std::log10(eBox / (std::max)(1e-20, eInv));
					}
					char line[160];
					snprintf(line, sizeof(line), "%s %s: accepted %d of 4 tiles, right kernel %d, gain over a 2x2 average %.1f dB\n",
						clip, file, accepted, right, accepted ? gainSum / accepted : 0.0);
					Logger::WriteMessage(line);
					Assert::AreEqual(accepted, right, L"every accepted tile names the right kernel");
					Assert::IsTrue(accepted >= 2, L"most tiles decide");
					Assert::IsTrue(gainSum / (std::max)(1, accepted) > 6.0, L"the inverse is much closer to the decode");
					++checked;
				}
			}
			Assert::IsTrue(checked > 0, L"material found");
		}
	};
}
