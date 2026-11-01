#pragma once

// CPU reference model of texture_restore (docs/VP-detail-restore.md; the algorithm and its
// constants: src/VP-V/modules/repair/texture_restore/TextureRestore.h). Mirrors the shader passes of
// TextureRestore.cpp in double precision on one luma plane in the signal's codes (the shader
// works in normalised units: every energy, floor and allowance scales with one code, so the
// results agree up to float precision). Positions in pixels, texel centres at +0.5, edges
// clamped, as in the shaders.
//
// Usage: one Model per stream; Frame() per presented source frame (with the frame's sequence
// and optionally the de-judder field), Forget() at discontinuities.

#include <VP-V/modules/repair/texture_restore/TextureRestore.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace TextureRestoreModel
{
	struct Image
	{
		int w = 0, h = 0;
		std::vector<double> v;
		Image() = default;
		Image(int width, int height, double fill = 0.0) : w(width), h(height), v(static_cast<size_t>(width) * height, fill) {}
		double& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
		double get(int x, int y) const { return at((std::max)(0, (std::min)(w - 1, x)), (std::max)(0, (std::min)(h - 1, y))); }
	};

	// The de-judder's block field: one texel per 16x16 native luma block, rgba = (vx, vy) in
	// native luma px (the content moved by +v since the previous frame), trust, error.
	struct Field
	{
		int w = 0, h = 0;
		std::vector<float> v;
		Field() = default;
		Field(int width, int height, float vx, float vy, float trust) : w(width), h(height), v(static_cast<size_t>(width) * height * 4)
		{
			for (size_t i = 0; i < v.size(); i += 4) { v[i] = vx; v[i + 1] = vy; v[i + 2] = trust; v[i + 3] = 0.0f; }
		}
		float* at(int x, int y) { return &v[(static_cast<size_t>(y) * w + x) * 4]; }
		const float* at(int x, int y) const { return &v[(static_cast<size_t>(y) * w + x) * 4]; }
	};

	struct Vec4 { double x = 0, y = 0, z = 0, w = 0; };

	struct TileGrid
	{
		int w = 0, h = 0;
		std::vector<Vec4> v;
		TileGrid() = default;
		TileGrid(int width, int height) : w(width), h(height), v(static_cast<size_t>(width) * height) {}
		Vec4& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
		const Vec4& at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
		const Vec4& get(int x, int y) const { return at((std::max)(0, (std::min)(w - 1, x)), (std::max)(0, (std::min)(h - 1, y))); }
		// Bilinear between tile centres at tile coordinates (u, v) (centre of tile i = i).
		Vec4 bilinear(double u, double vv) const
		{
			const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(vv));
			const double fx = u - x0, fy = vv - y0;
			const Vec4& a = get(x0, y0); const Vec4& b = get(x0 + 1, y0);
			const Vec4& c = get(x0, y0 + 1); const Vec4& d = get(x0 + 1, y0 + 1);
			auto mix = [&](double pa, double pb, double pc, double pd) {
				return (pa * (1.0 - fx) + pb * fx) * (1.0 - fy) + (pc * (1.0 - fx) + pd * fx) * fy;
			};
			Vec4 r;
			r.x = mix(a.x, b.x, c.x, d.x); r.y = mix(a.y, b.y, c.y, d.y);
			r.z = mix(a.z, b.z, c.z, d.z); r.w = mix(a.w, b.w, c.w, d.w);
			return r;
		}
	};

	// --- The bands (shader: STATS / APPLY passes and the half-resolution low passes) ---
	inline Image G1(const Image& y, double scale)
	{
		const int r = TextureRestore::Radius1(scale);
		std::vector<double> w(2 * r + 1);
		for (int k = -r; k <= r; ++k) w[k + r] = TextureRestore::Weight1(k, scale);
		Image across(y.w, y.h), out(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int x = 0; x < y.w; ++x)
			{
				double acc = 0.0;
				for (int k = -r; k <= r; ++k) acc += w[k + r] * y.get(x + k, yy);
				across.at(x, yy) = acc;
			}
		for (int yy = 0; yy < y.h; ++yy)
			for (int x = 0; x < y.w; ++x)
			{
				double acc = 0.0;
				for (int k = -r; k <= r; ++k) acc += w[k + r] * across.get(x, yy + k);
				out.at(x, yy) = acc;
			}
		return out;
	}

	inline Image Half(const Image& y)
	{
		const int hw = (y.w + 1) / 2, hh = (y.h + 1) / 2;
		Image half(hw, hh);
		for (int yy = 0; yy < hh; ++yy)
			for (int x = 0; x < hw; ++x)
				half.at(x, yy) = 0.25 * (y.get(2 * x, 2 * yy) + y.get(2 * x + 1, 2 * yy) + y.get(2 * x, 2 * yy + 1) + y.get(2 * x + 1, 2 * yy + 1));
		return half;
	}

	inline Image HalfBlur(const Image& half, double sigma)
	{
		const int r = TextureRestore::HalfRadius(sigma);
		std::vector<double> w(2 * r + 1);
		for (int k = -r; k <= r; ++k) w[k + r] = TextureRestore::HalfWeight(k, sigma);
		Image across(half.w, half.h), out(half.w, half.h);
		for (int y = 0; y < half.h; ++y)
			for (int x = 0; x < half.w; ++x)
			{
				double acc = 0.0;
				for (int k = -r; k <= r; ++k) acc += w[k + r] * half.get(x + k, y);
				across.at(x, y) = acc;
			}
		for (int y = 0; y < half.h; ++y)
			for (int x = 0; x < half.w; ++x)
			{
				double acc = 0.0;
				for (int k = -r; k <= r; ++k) acc += w[k + r] * across.get(x, y + k);
				out.at(x, y) = acc;
			}
		return out;
	}

	// The half-resolution band at full-resolution pixel (x, y): bilinear at p / 2, clamped.
	inline double LowAt(const Image& half, int x, int y)
	{
		const double hx = (x + 0.5) * 0.5 - 0.5, hy = (y + 0.5) * 0.5 - 0.5;
		const int x0 = static_cast<int>(std::floor(hx)), y0 = static_cast<int>(std::floor(hy));
		const double fx = hx - x0, fy = hy - y0;
		return (half.get(x0, y0) * (1.0 - fx) + half.get(x0 + 1, y0) * fx) * (1.0 - fy) +
			(half.get(x0, y0 + 1) * (1.0 - fx) + half.get(x0 + 1, y0 + 1) * fx) * fy;
	}

	struct Bands
	{
		Image g1, low2, low3;   // low2 / low3 at half resolution
		double scale = 1.0;
	};

	inline Bands MakeBands(const Image& y, double scale)
	{
		Bands b;
		b.scale = scale;
		b.g1 = G1(y, scale);
		const Image half = Half(y);
		b.low2 = HalfBlur(half, TextureRestore::kSigma2 * scale);
		b.low3 = HalfBlur(half, TextureRestore::kSigma3 * scale);
		return b;
	}

	// The band energy per pixel of an image (test measurements): D1, D2, D3 squared, summed over
	// the rectangle [x0, x1) x [y0, y1).
	struct BandEnergy { double e1 = 0, e2 = 0, e3 = 0; };
	inline BandEnergy MeasureBands(const Image& y, int x0, int y0, int x1, int y1, double scale = 1.0)
	{
		const Bands b = MakeBands(y, scale);
		BandEnergy e;
		int n = 0;
		for (int yy = y0; yy < y1; ++yy)
			for (int x = x0; x < x1; ++x)
			{
				const double l2 = LowAt(b.low2, x, yy), l3 = LowAt(b.low3, x, yy);
				const double d1 = y.at(x, yy) - b.g1.at(x, yy), d2 = b.g1.at(x, yy) - l2, d3 = l2 - l3;
				e.e1 += d1 * d1; e.e2 += d2 * d2; e.e3 += d3 * d3;
				++n;
			}
		if (n > 0) { e.e1 /= n; e.e2 /= n; e.e3 /= n; }
		return e;
	}

	// The mean of the up-to-kQuietSubBlocks smallest valid values (valid = >= 0).
	inline double QuietMean(const double* e, int n)
	{
		bool taken[64] = {};
		double sum = 0.0;
		int k = 0;
		for (int pick = 0; pick < TextureRestore::kQuietSubBlocks; ++pick)
		{
			int best = -1;
			for (int i = 0; i < n; ++i)
				if (!taken[i] && e[i] >= 0.0 && (best < 0 || e[i] < e[best])) best = i;
			if (best < 0) break;
			taken[best] = true;
			sum += e[best];
			++k;
		}
		return k > 0 ? sum / k : 0.0;
	}

	// Per-tile debug of the last processed frame.
	struct TileDebug
	{
		double gG = 1, gT1 = 1, gT2 = 1, n1 = 0;   // the gains texture
		double w = 0;                               // the focus gate
		double F = 0, Fref = 0, T1 = 0, T1ref = 0, T2 = 0, T2ref = 0, N1ref = 0;
		bool restart = false;
	};

	class Model
	{
	public:
		void Forget() { forget = true; }
		bool LastWasCut() const { return lastCut; }
		bool LastWasRepeat() const { return lastRepeat; }
		int TileW() const { return tw; }
		int TileH() const { return th; }
		int Tile() const { return tilePx; }
		const std::vector<TileDebug>& Debug() const { return debug; }
		double RestartShare() const
		{
			if (debug.empty()) return 0.0;
			int n = 0;
			for (const TileDebug& t : debug) n += t.restart ? 1 : 0;
			return static_cast<double>(n) / debug.size();
		}

		// One presented source frame: y in the signal's codes; returns the restored luma.
		Image Frame(const Image& y, const TextureRestore::Params& p, const Field* field = nullptr)
		{
			using namespace TextureRestore;
			lastCut = false;
			lastRepeat = false;
			const double cap = EffectiveCap(p.cap);
			if (!(cap > 1.0))
			{
				forget = true;   // the module returns the source; the history restarts when re-enabled
				return y;
			}
			const double lines = p.planeLines > 0.0 ? p.planeLines : y.h;
			const double scale = Scale(lines);
			const int T = TilePx(scale);
			const int ntw = (y.w + T - 1) / T, nth = (y.h + T - 1) / T;
			const bool sameSize = have && ntw == tw && nth == th && T == tilePx && y.w == width && y.h == height;
			const bool repeat = have && sameSize && !forget && p.sequence != 0 && p.sequence == sequence;
			const bool restartAll = !repeat && (!have || !sameSize || forget || p.sequence == 0 || p.sequence != sequence + 1);
			tw = ntw; th = nth; tilePx = T; width = y.w; height = y.h;
			const Bands b = MakeBands(y, scale);
			if (!repeat)
			{
				Advance(y, b, p, field, cap, restartAll);
				prevLow2 = b.low2;
			}
			lastRepeat = repeat;
			return ApplyGains(y, b);
		}

		// Hunt 3 (the crossfade's path being left): the last frame's gains on a picture of
		// another geometry: its own bands at its own scale, the tile gains at the same picture
		// position. Never advances.
		Image ForeignFrame(const Image& y, const TextureRestore::Params& p) const
		{
			using namespace TextureRestore;
			if (!(EffectiveCap(p.cap) > 1.0) || !have) return y;
			const double scale = Scale(p.planeLines > 0.0 ? p.planeLines : y.h);
			return ApplyGains(y, MakeBands(y, scale), static_cast<double>(width) / y.w, static_cast<double>(height) / y.h);
		}

	private:
		bool have = false, forget = false, lastCut = false, lastRepeat = false;
		uint64_t sequence = 0;
		int tw = 0, th = 0, tilePx = 16, width = 0, height = 0;
		TileGrid ref, gains;   // ref: (N1r, T1r, T2r, Fr); gains: (gG, gT1, gT2, N1)
		Image prevLow2;        // the previous frame's L2 (half resolution)
		double m1 = 0.0, m2 = 0.0;
		int hist = 0;
		std::vector<TileDebug> debug;

		// The vector of the field block under plane position (px, py) (continuous, texel centres
		// at +0.5), in plane px; false = no field or an untrusted block.
		static bool VectorAt(const Field* field, const TextureRestore::Params& p, double px, double py, double& vx, double& vy)
		{
			vx = vy = 0.0;
			if (!field) return false;
			const double toX = p.fieldScaleX > 0.0 ? p.fieldScaleX : p.fieldScale, toY = p.fieldScale;
			const int fx = (std::max)(0, (std::min)(field->w - 1, static_cast<int>(std::floor(px * toX / 16.0))));
			const int fy = (std::max)(0, (std::min)(field->h - 1, static_cast<int>(std::floor(py * toY / 16.0))));
			const float* f = field->at(fx, fy);
			if (!(f[2] > 0.5f)) return false;
			vx = f[0] / toX;
			vy = f[1] / toY;
			return true;
		}

		// The half-resolution band at a continuous plane position: bilinear at p / 2, clamped.
		static double LowAtPos(const Image& half, double px, double py)
		{
			const double hx = px * 0.5 - 0.5, hy = py * 0.5 - 0.5;
			const int x0 = static_cast<int>(std::floor(hx)), y0 = static_cast<int>(std::floor(hy));
			const double fx = hx - x0, fy = hy - y0;
			return (half.get(x0, y0) * (1.0 - fx) + half.get(x0 + 1, y0) * fx) * (1.0 - fy) +
				(half.get(x0, y0 + 1) * (1.0 - fx) + half.get(x0 + 1, y0 + 1) * fx) * fy;
		}

		// STATS: per tile (E1, E2, E3, mu) and (quiet sub-block energy of bands 1 and 2, the
		// low band's change energy since the previous frame - along the vector where a trusted
		// one exists - , 0).
		void Stats(const Image& y, const Bands& b, const TextureRestore::Params& p, const Field* field, bool restartAll, TileGrid& a, TileGrid& raw) const
		{
			using namespace TextureRestore;
			const int T = tilePx, sb = T / kSubBlocks;
			a = TileGrid(tw, th);
			raw = TileGrid(tw, th);
			for (int ty = 0; ty < th; ++ty)
				for (int tx = 0; tx < tw; ++tx)
				{
					double sub1[kSubBlocks * kSubBlocks], sub2[kSubBlocks * kSubBlocks];
					double tot[5] = {};
					double n = 0.0;
					for (int s = 0; s < kSubBlocks * kSubBlocks; ++s)
					{
						const int sx = s % kSubBlocks, sy = s / kSubBlocks;
						double acc[5] = {};
						double cnt = 0.0;
						for (int yy = ty * T + sy * sb; yy < ty * T + sy * sb + sb; ++yy)
							for (int x = tx * T + sx * sb; x < tx * T + sx * sb + sb; ++x)
							{
								if (x >= y.w || yy >= y.h) continue;
								const double l2 = LowAt(b.low2, x, yy), l3 = LowAt(b.low3, x, yy);
								const double d1 = y.at(x, yy) - b.g1.at(x, yy), d2 = b.g1.at(x, yy) - l2, d3 = l2 - l3;
								double change = 0.0;
								if (!restartAll)
								{
									double vx, vy;
									VectorAt(field, p, x + 0.5, yy + 0.5, vx, vy);
									const double c = l2 - LowAtPos(prevLow2, x + 0.5 - vx, yy + 0.5 - vy);
									change = c * c;
								}
								acc[0] += d1 * d1; acc[1] += d2 * d2; acc[2] += d3 * d3; acc[3] += l2; acc[4] += change;
								cnt += 1.0;
							}
						for (int c = 0; c < 5; ++c) tot[c] += acc[c];
						n += cnt;
						sub1[s] = cnt > 0.0 ? acc[0] / cnt : -1.0;
						sub2[s] = cnt > 0.0 ? acc[1] / cnt : -1.0;
					}
					const double inv = 1.0 / (std::max)(n, 1.0);
					Vec4& t = a.at(tx, ty);
					t.x = tot[0] * inv; t.y = tot[1] * inv; t.z = tot[2] * inv; t.w = tot[3] * inv;
					Vec4& r = raw.at(tx, ty);
					r.x = QuietMean(sub1, kSubBlocks * kSubBlocks);
					r.y = QuietMean(sub2, kSubBlocks * kSubBlocks);
					r.z = tot[4] * inv;
				}
		}

		// The previous frame's tile position of tile (tx, ty)'s content (tile coordinates); false
		// = an untrusted block (with a field). Without a field the same tile.
		bool Carry(int tx, int ty, const TextureRestore::Params& p, const Field* field, double& u, double& v) const
		{
			const double T = tilePx;
			const double cx = (std::min)((tx + 0.5) * T, width - 0.5), cy = (std::min)((ty + 0.5) * T, height - 0.5);
			u = tx; v = ty;
			if (!field) return true;
			double vx, vy;
			if (!VectorAt(field, p, cx, cy, vx, vy)) return false;
			u = (cx - vx) / T - 0.5; v = (cy - vy) / T - 0.5;
			return true;
		}

		void Advance(const Image& y, const Bands& b, const TextureRestore::Params& p, const Field* field, double cap, bool restartAll)
		{
			using namespace TextureRestore;
			TileGrid a, raw;
			Stats(y, b, p, field, restartAll, a, raw);
			// GRAIN: the minimum over the neighbourhood, calibrated; z = the low band's change.
			TileGrid grain(tw, th);
			for (int ty = 0; ty < th; ++ty)
				for (int tx = 0; tx < tw; ++tx)
				{
					double m1v = std::numeric_limits<double>::infinity(), m2v = m1v;
					for (int dy = -kGrainNeighbourhood; dy <= kGrainNeighbourhood; ++dy)
						for (int dx = -kGrainNeighbourhood; dx <= kGrainNeighbourhood; ++dx)
						{
							const Vec4& r = raw.get(tx + dx, ty + dy);
							m1v = (std::min)(m1v, r.x);
							m2v = (std::min)(m2v, r.y);
						}
					grain.at(tx, ty).x = kGrainCal1 * m1v;
					grain.at(tx, ty).y = kGrainCal2 * m2v;
					grain.at(tx, ty).z = raw.at(tx, ty).z;
				}
			const double allow = MeanAllowanceCodes(p.bitrateQuality, p.colorDepth);
			// A tile's low band changed: its change energy above the coding allowance plus the
			// mid band's grain level times kLowChangeNoise.
			auto changed = [&](int tx, int ty) {
				const Vec4& g = grain.at(tx, ty);
				return g.z > allow * allow + kLowChangeNoise * g.y;
			};
			// FRAME: cut / fade.
			bool flag = restartAll;
			{
				double nchanged = 0.0, count = 0.0, sum = 0.0;
				for (int ty = 0; ty < th; ++ty)
					for (int tx = 0; tx < tw; ++tx)
					{
						sum += a.at(tx, ty).w;
						count += 1.0;
						if (!restartAll && changed(tx, ty)) nchanged += 1.0;
					}
				const double mean = sum / (std::max)(count, 1.0);
				if (!restartAll)
				{
					const double fs = kFadeStepShare * allow;
					const double d0 = mean - m1, d1 = m1 - m2;
					const bool fade = hist >= 2 && std::fabs(d0) > fs && std::fabs(d1) > fs && d0 * d1 > 0.0;
					const bool cut = nchanged > kCutShare * count;
					lastCut = cut || fade;
					flag = cut || fade;
				}
				if (restartAll) { m2 = mean; m1 = mean; hist = 1; }
				else { m2 = m1; m1 = mean; hist = (std::min)(hist + 1, 2); }
			}
			// REF: smoothed current statistics, references, gains.
			const double eight = EightBitStep(p.colorDepth);
			const double fg = std::pow(kGrainFloorCodes8 * eight, 2.0), ft = std::pow(kTextureFloorCodes8 * eight, 2.0);
			const double dec = Decay(p.sourceHz);
			const double sf = std::pow(CodingNoiseCodes8(p.bitrateQuality) * eight, 2.0);   // texture evidence: above grain and coding noise
			TileGrid newRef(tw, th), newGains(tw, th);
			debug.assign(static_cast<size_t>(tw) * th, TileDebug());
			static const double k3[3] = { 0.25, 0.5, 0.25 };
			for (int ty = 0; ty < th; ++ty)
				for (int tx = 0; tx < tw; ++tx)
				{
					double sE1 = 0, sE2 = 0, sE3 = 0, sN1 = 0, sN2 = 0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const double wgt = k3[dx + 1] * k3[dy + 1];
							const Vec4& s = a.get(tx + dx, ty + dy);
							const Vec4& g = grain.get(tx + dx, ty + dy);
							sE1 += wgt * s.x; sE2 += wgt * s.y; sE3 += wgt * s.z;
							sN1 += wgt * g.x; sN2 += wgt * g.y;
						}
					const double T1 = (std::max)(0.0, sE1 - kGrainExcess * sN1);
					const double T2 = (std::max)(0.0, sE2 - kGrainExcess * sN2);
					const double F = T2 / (T2 + sE3 + ft);
					bool restart = flag;
					Vec4 old;
					if (!restart)
					{
						double u, v;
						const bool trusted = Carry(tx, ty, p, field, u, v);
						if (!trusted) restart = true;
						else
						{
							old = field ? ref.bilinear(u, v) : ref.at(tx, ty);
							old.x *= dec; old.y *= dec; old.z *= dec; old.w *= dec;
							if (changed(tx, ty)) restart = true;
							if (F < kPullLo * old.w) restart = true;
							if (T2 + ft < kMidLossLo * (old.z + ft)) restart = true;
						}
					}
					Vec4 r;
					if (restart) { r.x = sN1; r.y = T1; r.z = T2; r.w = F; }
					else { r.x = (std::max)(sN1, old.x); r.y = (std::max)(T1, old.y); r.z = (std::max)(T2, old.z); r.w = (std::max)(F, old.w); }
					newRef.at(tx, ty) = r;
					const double gG = Clamp(std::sqrt((r.x + fg) / ((sN1 + fg) * (1.0 + kGrainMargin))), 1.0, cap);
					double gT1 = Clamp(std::sqrt((r.y + ft) / (T1 + kTextureMargin * sN1 + ft)), 1.0, cap);
					double gT2 = Clamp(std::sqrt((r.z + ft) / (T2 + kTextureMargin * sN2 + ft)), 1.0, cap);
					const double w = Smoothstep(kFocusLo, kFocusHi, F) * Smoothstep(kFocusLo, kFocusHi, r.w) *
						Smoothstep(kPullLo, kPullHi, F / (std::max)(r.w, 1e-30)) *
						Smoothstep(kMidLossLo, kMidLossHi, (T2 + ft) / (r.z + ft)) *
						Smoothstep(kTextureSigLo, kTextureSigHi, r.z / (sN2 + sf)) *
						Smoothstep(kTextureSigLo, kTextureSigHi, T2 / (sN2 + sf));
					gT1 = 1.0 + (gT1 - 1.0) * w;
					gT2 = 1.0 + (gT2 - 1.0) * w;
					Vec4& g = newGains.at(tx, ty);
					g.x = gG; g.y = gT1; g.z = gT2; g.w = sN1;
					TileDebug& d = debug[static_cast<size_t>(ty) * tw + tx];
					d.gG = gG; d.gT1 = gT1; d.gT2 = gT2; d.n1 = sN1; d.w = w;
					d.F = F; d.Fref = r.w; d.T1 = T1; d.T1ref = r.y; d.T2 = T2; d.T2ref = r.z; d.N1ref = r.x;
					d.restart = restart;
				}
			ref = newRef;
			gains = newGains;
			sequence = p.sequence;
			have = true;
			forget = false;
		}

		// APPLY: the pixel split and the gains, bilinear between tile centres.
		// rx, ry: decided plane px per plane px of y (hunt 3: a picture of another geometry reads
		// the tile gains at the same picture position; 1 = the decided geometry itself).
		Image ApplyGains(const Image& y, const Bands& b, double rx = 1.0, double ry = 1.0) const
		{
			using namespace TextureRestore;
			Image out(y.w, y.h);
			// The shader's d1 one pixel beyond the edges is that of the clamp-extended picture
			// (its G1 window shifted, not the edge pixel's d1): the extended picture's own d1.
			Image ext(y.w + 2, y.h + 2);
			for (int yy = 0; yy < ext.h; ++yy)
				for (int x = 0; x < ext.w; ++x) ext.at(x, yy) = y.get(x - 1, yy - 1);
			const Image extG1 = G1(ext, b.scale);
			Image d1(ext.w, ext.h);
			for (size_t i = 0; i < ext.v.size(); ++i) d1.v[i] = ext.v[i] - extG1.v[i];
			const double T = tilePx;
			for (int yy = 0; yy < y.h; ++yy)
				for (int x = 0; x < y.w; ++x)
				{
					double e1 = 0.0;
					for (int dy = -1; dy <= 1; ++dy)
						for (int dx = -1; dx <= 1; ++dx)
						{
							const double v = d1.at(x + 1 + dx, yy + 1 + dy);
							e1 += v * v;
						}
					e1 /= 9.0;
					const double dd1 = d1.at(x + 1, yy + 1);
					const double dd2 = b.g1.at(x, yy) - LowAt(b.low2, x, yy);
					const Vec4 g = gains.bilinear((x + 0.5) * rx / T - 0.5, (yy + 0.5) * ry / T - 0.5);
					const double f = Clamp(1.0 - kWienerKappa * g.w / (e1 + kEnergyEps), 0.0, 1.0);
					out.at(x, yy) = y.at(x, yy) + (g.x - 1.0) * (1.0 - f) * dd1 + (g.y - 1.0) * f * dd1 + (g.z - 1.0) * f * dd2;
				}
			return out;
		}
	};
}
