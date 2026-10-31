// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Test helper (header-only): deterministic synthetic vector fields for the global camera model
// fitter. A Field owns the per-block arrays; Input() returns a PairInput whose pointers refer to
// them, so the Field must outlive (and must not be moved or resized while in use by) that input.
#pragma once

#include <VP-V/modules/motion/estimation/global_model/GlobalModelFit.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelGeometry.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace VideoProcessorTest
{
	namespace CameraSynth
	{
		using namespace CameraModel;

		struct GridSpec
		{
			int w = 0, h = 0;
			double blockSize = 16;
		};

		inline GridSpec Grid4K() { GridSpec g; g.w = 240; g.h = 135; g.blockSize = 16; return g; }
		inline GridSpec Grid8K() { GridSpec g; g.w = 480; g.h = 270; g.blockSize = 16; return g; }
		inline GridSpec GridOdd() { GridSpec g; g.w = 60; g.h = 60; g.blockSize = 8; return g; }

		// ---- model builders (picture-centred coordinates) ----
		inline Params MakeParams(ModelType type, const double (&h)[9])
		{
			Params p;
			p.type = type;
			for (int i = 0; i < 9; ++i) p.h[i] = h[i];
			return p;
		}

		inline Params SimilarityModel(double scale, double angle, double tx, double ty)
		{
			const double c = scale * std::cos(angle), s = scale * std::sin(angle);
			const double h[9] = { c, -s, tx, s, c, ty, 0, 0, 1 };
			return MakeParams(ModelType::Similarity, h);
		}

		inline Params PanModel(double tx, double ty)
		{
			const double h[9] = { 1, 0, tx, 0, 1, ty, 0, 0, 1 };
			return MakeParams(ModelType::Translation, h);
		}

		inline Params ZoomModel(double scale) { return SimilarityModel(scale, 0, 0, 0); }

		// A mild homography: translation plus perspective terms that scale with 1/picture width.
		inline Params PerspectiveModel(double pictureWidth, double tx, double ty)
		{
			const double h[9] = { 1, 0, tx, 0, 1, ty, 0.04 / pictureWidth, 0.02 / pictureWidth, 1 };
			return MakeParams(ModelType::Homography, h);
		}

		// Deterministic per-frame handheld shake (rotation, scale, translation jitter), relative to size.
		inline Params ShakeModel(SplitMix64& rng, double pictureWidth)
		{
			const double a = (rng.Unit() - 0.5) * 2 * 0.004;
			const double s = 1.0 + (rng.Unit() - 0.5) * 2 * 0.002;
			const double tx = (rng.Unit() - 0.5) * 2 * 0.003 * pictureWidth;
			const double ty = (rng.Unit() - 0.5) * 2 * 0.003 * pictureWidth;
			return SimilarityModel(s, a, tx, ty);
		}

		// ---- field ----
		struct Field
		{
			int gridW = 0, gridH = 0;
			double blockSize = 16;
			Rect picture;
			std::vector<float> fx, fy, trust, bx, by, btrust, texture, pixelResidual;
			std::vector<uint8_t> guard;
			bool haveBackward = false, haveTexture = false, haveGuard = false, havePixelResidual = false;
			bool haveEngine = false;
			double engineGx = 0, engineGy = 0;

			Field() {}
			explicit Field(const GridSpec& g) { Init(g.w, g.h, g.blockSize); }
			Field(const GridSpec& g, const Rect& pic) { Init(g.w, g.h, g.blockSize); picture = pic; }

			void Init(int w, int h, double bs)
			{
				gridW = w; gridH = h; blockSize = bs;
				picture.left = 0; picture.top = 0;
				picture.right = static_cast<int>(w * bs); picture.bottom = static_cast<int>(h * bs);
				const size_t n = static_cast<size_t>(w) * h;
				fx.assign(n, 0); fy.assign(n, 0); trust.assign(n, 1.0f);
				bx.assign(n, 0); by.assign(n, 0); btrust.assign(n, 1.0f);
				texture.assign(n, 1.0f); pixelResidual.assign(n, 0.0f);
				guard.assign(n, 0);
				haveBackward = haveTexture = haveGuard = havePixelResidual = false;
			}

			size_t Count() const { return static_cast<size_t>(gridW) * gridH; }
			size_t Index(int x, int y) const { return static_cast<size_t>(y) * gridW + x; }
			double Diagonal() const { return std::sqrt(double(picture.Width()) * picture.Width() + double(picture.Height()) * picture.Height()); }
			double NoiseScale() const { return blockSize / 16.0; }

			void Centre(int x, int y, double* cx, double* cy) const { BlockCentre(x, y, blockSize, picture, cx, cy); }

			// Forward vectors of the model at every block centre; with backward the inverse model's vectors too.
			void FromModel(const Params& p, bool withBackward = false)
			{
				const Params inv = Invert(p);
				for (int y = 0; y < gridH; ++y)
					for (int x = 0; x < gridW; ++x)
					{
						double cx, cy, dx, dy;
						Centre(x, y, &cx, &cy);
						Displacement(p, cx, cy, &dx, &dy);
						fx[Index(x, y)] = static_cast<float>(dx); fy[Index(x, y)] = static_cast<float>(dy);
						Displacement(inv, cx, cy, &dx, &dy);
						bx[Index(x, y)] = static_cast<float>(dx); by[Index(x, y)] = static_cast<float>(dy);
					}
				haveBackward = withBackward;
			}

			void BackwardFromModel(const Params& p)
			{
				const Params inv = Invert(p);
				for (int y = 0; y < gridH; ++y)
					for (int x = 0; x < gridW; ++x)
					{
						double cx, cy, dx, dy;
						Centre(x, y, &cx, &cy);
						Displacement(inv, cx, cy, &dx, &dy);
						bx[Index(x, y)] = static_cast<float>(dx); by[Index(x, y)] = static_cast<float>(dy);
					}
				haveBackward = true;
			}

			// Replace the backward field with deterministic garbage of the given amplitude (px).
			void CorruptBackward(double amplitude, uint64_t seed)
			{
				SplitMix64 rng(seed);
				for (size_t i = 0; i < Count(); ++i)
				{
					bx[i] = static_cast<float>((rng.Unit() - 0.5) * 2 * amplitude);
					by[i] = static_cast<float>((rng.Unit() - 0.5) * 2 * amplitude);
				}
				haveBackward = true;
			}

			// Uniform noise with standard deviation sigmaPx16 (given for 16-px blocks) * blockSize/16.
			void AddNoise(double sigmaPx16, uint64_t seed)
			{
				SplitMix64 rng(seed);
				const double amp = sigmaPx16 * NoiseScale() * std::sqrt(12.0);
				for (size_t i = 0; i < Count(); ++i)
				{
					fx[i] += static_cast<float>((rng.Unit() - 0.5) * amp);
					fy[i] += static_cast<float>((rng.Unit() - 0.5) * amp);
				}
			}

			// Measured vector (dx, dy) absolute, trusted, on a block rectangle.
			void AddObject(int x0, int y0, int w, int h, double dx, double dy)
			{
				for (int y = y0; y < y0 + h; ++y)
					for (int x = x0; x < x0 + w; ++x)
					{
						if (x < 0 || y < 0 || x >= gridW || y >= gridH) continue;
						fx[Index(x, y)] = static_cast<float>(dx); fy[Index(x, y)] = static_cast<float>(dy);
						trust[Index(x, y)] = 1.0f;
					}
			}

			void AddTinyObject(int x0, int y0, int blocks, double dx, double dy) { AddObject(x0, y0, blocks, 1, dx, dy); }

			// Flat region: trust 0, vector zero, texture value.
			void SetUntrusted(int x0, int y0, int w, int h, double textureValue)
			{
				haveTexture = true;
				for (int y = y0; y < y0 + h; ++y)
					for (int x = x0; x < x0 + w; ++x)
					{
						if (x < 0 || y < 0 || x >= gridW || y >= gridH) continue;
						trust[Index(x, y)] = 0.0f; texture[Index(x, y)] = static_cast<float>(textureValue);
						fx[Index(x, y)] = 0; fy[Index(x, y)] = 0;
					}
			}

			void SetPixelResidual(int x, int y, double v) { havePixelResidual = true; pixelResidual[Index(x, y)] = static_cast<float>(v); }

			// Letterbox: top/bottom block rows zero vectors, trusted, and the picture rectangle shrinks to the rest.
			void AddBars(int topRows, int bottomRows)
			{
				for (int y = 0; y < gridH; ++y)
				{
					if (y >= topRows && y < gridH - bottomRows) continue;
					for (int x = 0; x < gridW; ++x)
					{
						fx[Index(x, y)] = 0; fy[Index(x, y)] = 0; trust[Index(x, y)] = 1.0f;
						bx[Index(x, y)] = 0; by[Index(x, y)] = 0;
					}
				}
				picture.top = static_cast<int>(topRows * blockSize);
				picture.bottom = static_cast<int>((gridH - bottomRows) * blockSize);
			}

			// Static logo: zero vector, trusted; optionally flagged by the text guard.
			void AddLogo(int x0, int y0, int w, int h, bool withGuard)
			{
				for (int y = y0; y < y0 + h; ++y)
					for (int x = x0; x < x0 + w; ++x)
					{
						fx[Index(x, y)] = 0; fy[Index(x, y)] = 0; trust[Index(x, y)] = 1.0f;
						if (withGuard) { guard[Index(x, y)] = 1; haveGuard = true; }
					}
			}

			// Guarded text tiles with garbage vectors.
			void AddTextTiles(int x0, int y0, int w, int h, uint64_t seed)
			{
				SplitMix64 rng(seed);
				haveGuard = true;
				for (int y = y0; y < y0 + h; ++y)
					for (int x = x0; x < x0 + w; ++x)
					{
						guard[Index(x, y)] = 1;
						fx[Index(x, y)] = static_cast<float>((rng.Unit() - 0.5) * 80 * NoiseScale());
						fy[Index(x, y)] = static_cast<float>((rng.Unit() - 0.5) * 80 * NoiseScale());
					}
			}

			void SetEngine(double gx, double gy) { haveEngine = true; engineGx = gx; engineGy = gy; }

			PairInput Input(double pairSeconds, uint64_t seq) const
			{
				PairInput in;
				in.gridW = gridW; in.gridH = gridH; in.blockSize = blockSize; in.picture = picture;
				in.fwdX = fx.data(); in.fwdY = fy.data(); in.fwdTrust = trust.data();
				if (haveBackward) { in.bwdX = bx.data(); in.bwdY = by.data(); in.bwdTrust = btrust.data(); }
				if (haveGuard) in.textGuard = guard.data();
				if (haveTexture) in.texture = texture.data();
				if (havePixelResidual) in.pixelResidual = pixelResidual.data();
				if (haveEngine) { in.haveEngineGlobal = true; in.engineGx = engineGx; in.engineGy = engineGy; }
				in.pairSeconds = pairSeconds;
				in.seq = seq; in.pairFrom = seq > 0 ? seq - 1 : 0; in.pairTo = seq;
				return in;
			}
		};

		// ---- result helpers ----
		inline int CountLabel(const CameraMotion& r, Label l)
		{
			int c = 0;
			if (!r.blocks) return 0;
			for (uint8_t v : r.blocks->label) if (v == static_cast<uint8_t>(l)) ++c;
			return c;
		}

		inline int CountLabelInRect(const CameraMotion& r, const Field& f, int x0, int y0, int w, int h, Label l)
		{
			int c = 0;
			if (!r.blocks) return 0;
			for (int y = y0; y < y0 + h; ++y)
				for (int x = x0; x < x0 + w; ++x)
					if (x >= 0 && y >= 0 && x < f.gridW && y < f.gridH && r.blocks->label[f.Index(x, y)] == static_cast<uint8_t>(l)) ++c;
			return c;
		}

		inline double ModelError(const CameraMotion& r, const Params& truth, const Rect& picture)
		{
			return MaxCornerDifference(r.params, truth, picture);
		}

		inline std::string Describe(const CameraMotion& r)
		{
			char b[512];
			snprintf(b, sizeof(b), "valid=%d type=%d reason=%s conf=%.3f hyp=%u n=%u inl=%.2f cov=%.2f amb=%.2f inv=%.2f tmp=%.2f",
				r.valid ? 1 : 0, static_cast<int>(r.params.type), ReasonText(r.reason), r.confidence, r.hypotheses, r.samples,
				r.signals.inlierShare, r.signals.coverage, r.signals.ambiguity, r.signals.inverse, r.signals.temporal);
			return b;
		}
	}
}
