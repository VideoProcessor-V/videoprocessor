// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Synthetic and real-texture scenes of the photometric refinement tests (doc 103 s3 R3), shared by the CPU reference
// tests (PhotometricRefineTests.cpp) and the GPU equivalence tests (PhotometricRefineGpuTests.cpp). Moved unchanged
// from PhotometricRefineTests.cpp.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/PhotometricRefine.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace R3Scenes
	{
		using namespace CameraModel;

		const double kPi = 3.14159265358979323846;

		struct Wave { double kx, ky, ph, amp; };

		// Band-limited texture in frame source px: 128 + sum of plane waves, wavelengths 6..64 engine px, amplitude ~ sqrt(lambda).
		inline std::vector<Wave> MakeWaves(uint64_t seed, double s, double totalAmp)
		{
			SplitMix64 rng(seed);
			std::vector<Wave> waves;
			const int count = 12;
			double sum = 0;
			for (int k = 0; k < count; ++k)
			{
				const double lambdaLuma = 6.0 * std::pow(64.0 / 6.0, k / static_cast<double>(count - 1));
				const double lambda = lambdaLuma * s;
				const double ang = 2.0 * kPi * rng.Unit();
				Wave w;
				w.kx = 2.0 * kPi / lambda * std::cos(ang);
				w.ky = 2.0 * kPi / lambda * std::sin(ang);
				w.ph = 2.0 * kPi * rng.Unit();
				w.amp = std::sqrt(lambdaLuma);
				sum += w.amp;
				waves.push_back(w);
			}
			for (size_t k = 0; k < waves.size(); ++k)
				waves[k].amp *= totalAmp / sum;
			return waves;
		}

		// Source position X = M * Y + c (frame px) for an output position Y: the layer shows tex(X).
		struct Layer
		{
			const std::vector<Wave>* waves = nullptr;
			double m[4] = { 1, 0, 0, 1 };
			double c[2] = { 0, 0 };
			bool clip = false;                      // only inside [l, r) x [t, b) (frame px, by pixel centre)
			double l = 0, t = 0, r = 0, b = 0;
		};

		inline uint32_t Hash(uint64_t a)
		{
			SplitMix64 g(a);
			return static_cast<uint32_t>(g.Next() >> 32);
		}

		// Renders one engine-scale luma plane (pixel centre s*(i + 0.5)); layers later in the list paint over earlier ones.
		inline std::vector<double> RenderD(int w, int h, double s, const std::vector<Layer>& layers, double gain, double offset, uint64_t noiseSeed, double noiseAmp)
		{
			std::vector<double> out(static_cast<size_t>(w) * h, 0.0);
			std::vector<double> sx(w), cxv(w), sy(h), cy(h);
			for (size_t li = 0; li < layers.size(); ++li)
			{
				const Layer& L = layers[li];
				std::vector<double> acc(static_cast<size_t>(w) * h, 128.0);
				for (size_t k = 0; k < L.waves->size(); ++k)
				{
					const Wave& wv = (*L.waves)[k];
					const double kx = L.m[0] * wv.kx + L.m[2] * wv.ky;   // M^T k
					const double ky = L.m[1] * wv.kx + L.m[3] * wv.ky;
					const double ph = wv.ph + wv.kx * L.c[0] + wv.ky * L.c[1];
					for (int i = 0; i < w; ++i)
					{
						const double X = s * (i + 0.5);
						sx[i] = std::sin(kx * X);
						cxv[i] = std::cos(kx * X);
					}
					for (int j = 0; j < h; ++j)
					{
						const double Y = s * (j + 0.5);
						sy[j] = std::sin(ky * Y + ph);
						cy[j] = std::cos(ky * Y + ph);
					}
					for (int j = 0; j < h; ++j)
					{
						double* row = &acc[static_cast<size_t>(j) * w];
						const double a = wv.amp * cy[j], bb = wv.amp * sy[j];
						for (int i = 0; i < w; ++i)
							row[i] += sx[i] * a + cxv[i] * bb;
					}
				}
				for (int j = 0; j < h; ++j)
					for (int i = 0; i < w; ++i)
					{
						if (L.clip)
						{
							const double X = s * (i + 0.5), Y = s * (j + 0.5);
							if (X < L.l || X >= L.r || Y < L.t || Y >= L.b)
								continue;
						}
						out[static_cast<size_t>(j) * w + i] = acc[static_cast<size_t>(j) * w + i];
					}
			}
			for (int j = 0; j < h; ++j)
				for (int i = 0; i < w; ++i)
				{
					const size_t idx = static_cast<size_t>(j) * w + i;
					const uint32_t hv = Hash(noiseSeed * 0x100000001B3ull + idx);
					const double noise = noiseAmp * ((hv & 0xFFFFu) / 32767.5 - 1.0);
					out[idx] = gain * out[idx] + offset + noise;
				}
			return out;
		}

		inline std::vector<uint8_t> To8(const std::vector<double>& v)
		{
			std::vector<uint8_t> o(v.size());
			for (size_t k = 0; k < v.size(); ++k)
				o[k] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::floor(v[k] + 0.5))));
			return o;
		}

		inline std::vector<uint16_t> To16(const std::vector<double>& v)
		{
			std::vector<uint16_t> o(v.size());
			for (size_t k = 0; k < v.size(); ++k)
				o[k] = static_cast<uint16_t>(std::min(65535.0, std::max(0.0, std::floor(v[k] * 257.0 + 0.5))));
			return o;
		}

		inline Rect Pic(int w, int h)
		{
			Rect r;
			r.left = 0; r.top = 0; r.right = w; r.bottom = h;
			return r;
		}

		inline Params Translation(double tx, double ty)
		{
			Params p;
			p.type = ModelType::Translation;
			p.h[2] = tx;
			p.h[5] = ty;
			return p;
		}

		inline LumaPairView View8(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int w, int h, double s)
		{
			LumaPairView v;
			v.from.data8 = a.data(); v.from.width = w; v.from.height = h; v.from.stride = w;
			v.to.data8 = b.data(); v.to.width = w; v.to.height = h; v.to.stride = w;
			v.sourcePxPerPixel = s;
			v.codeScale = 1.0;
			return v;
		}

		// Layer showing the texture moved by `shift` (frame px): X = Y - shift.
		inline Layer Shifted(const std::vector<Wave>& waves, double shiftX, double shiftY)
		{
			Layer L;
			L.waves = &waves;
			L.c[0] = -shiftX;
			L.c[1] = -shiftY;
			return L;
		}


		// Box-downscale (integer factor s) of an 8-bit full-resolution plane from crop origin (ox, oy), cw x ch source px,
		// plus deterministic uniform noise of +-noiseAmp codes; the producer's box convention (pixel centre s*(i + 0.5)).
		inline std::vector<uint8_t> BoxCrop(const std::vector<uint8_t>& full, int fullW, int ox, int oy, int cw, int ch, int s, uint64_t noiseSeed, double noiseAmp)
		{
			const int w = cw / s, h = ch / s;
			std::vector<uint8_t> o(static_cast<size_t>(w) * h);
			for (int j = 0; j < h; ++j)
				for (int i = 0; i < w; ++i)
				{
					double sum = 0;
					for (int b = 0; b < s; ++b)
						for (int a = 0; a < s; ++a)
							sum += full[static_cast<size_t>(oy + j * s + b) * fullW + (ox + i * s + a)];
					const size_t idx = static_cast<size_t>(j) * w + i;
					const uint32_t hv = Hash(noiseSeed * 0x100000001B3ull + idx);
					const double v = sum / (s * s) + noiseAmp * ((hv & 0xFFFFu) / 32767.5 - 1.0);
					o[idx] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, std::floor(v + 0.5))));
				}
			return o;
		}

		inline std::string EnvText(const char* n)
		{
			char* v = nullptr;
			size_t sz = 0;
			std::string out;
			if (_dupenv_s(&v, &sz, n) == 0 && v)
			{
				out = v;
				free(v);
			}
			return out;
		}
	}
}
