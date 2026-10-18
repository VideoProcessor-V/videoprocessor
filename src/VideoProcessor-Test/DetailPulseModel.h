#pragma once

// CPU reference model of detail_pulse_stabilise (DetailPulse.h has the maths; DetailPulse.cpp
// the shader). One luma plane in codes of the signal, double precision, the shader's tile
// layout (kTilePx tiles = its work blocks), its reductions (mean over the measured tiles,
// winsorised), its ring and gain law and its apply (tile weight bilinear between tile
// centres). Edges clamped at every filter step, as in the shader.
//
// Use: one Model per stream; Process() once per presented source frame, in order (a repeat
// passes the same sequence again). MeasureAhead(next) before Process(current) gives the
// look-ahead. Other tests may use the building blocks (Bands, Measure).

#include "GrainReferenceModel.h"

#include <vprenderer/DetailPulse.h>
#include <vprenderer/GrainPreserve.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace DetailPulseModel
{
	using GrainModel::Image;

	struct Settings
	{
		double cap = DetailPulse::kDefaultCap;
		int colorDepth = 10;
		double planeLines = 0.0;      // 0 = the image's height
		double sourceHz = 0.0;        // 0 = reference 23.976
		double bitrateQuality = 1.0;
		double fieldScale = 1.0;      // field (native) px per plane px down
		double fieldScaleX = 0.0;     // across (0 = fieldScale)
	};

	// The de-judder's block field: one trust value per 16 x 16 native block (.z in the shader).
	struct Field
	{
		int w = 0, h = 0;
		std::vector<double> trust;
		Field() = default;
		Field(int bw, int bh, double fill) : w(bw), h(bh), trust(static_cast<size_t>(bw) * bh, fill) {}
		double& at(int x, int y) { return trust[static_cast<size_t>(y) * w + x]; }
		double at(int x, int y) const { return trust[static_cast<size_t>(y) * w + x]; }
	};

	struct Frame
	{
		Image out;                    // the stabilised luma (codes)
		double g1 = 1.0, g2 = 1.0;    // the frame's band gains
		bool restart = true;          // the history restarted on this frame (gain 1)
		bool repeat = false;          // a repeat: the stored decision, the history not advanced
		bool aheadUsed = false;       // a look-ahead entry was available (not necessarily larger)
		double e1 = 0.0, e2 = 0.0;    // the frame's band energies (codes^2)
		double meanLuma = 0.0;
		double cutShare = 0.0;
		Image tileWeight;             // per tile veto weight (1 = full gain)
	};

	// The two bands of a plane: D1 = Y - G(s1) Y, D2 = G(s1) Y - L (L bilinear from the half band).
	struct Bands
	{
		Image d1, d2, lowHalf;
	};

	inline Bands MakeBands(const Image& y, double scale)
	{
		const int r = DetailPulse::FineRadius(scale);
		std::vector<double> w(2 * r + 1);
		for (int i = -r; i <= r; ++i) w[i + r] = DetailPulse::FineWeight(i, scale);
		Image across(y.w, y.h), fine(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int x = 0; x < y.w; ++x)
			{
				double a = 0.0;
				for (int k = -r; k <= r; ++k) a += w[k + r] * y.get(x + k, yy);
				across.at(x, yy) = a;
			}
		for (int yy = 0; yy < y.h; ++yy)
			for (int x = 0; x < y.w; ++x)
			{
				double a = 0.0;
				for (int k = -r; k <= r; ++k) a += w[k + r] * across.get(x, yy + k);
				fine.at(x, yy) = a;
			}
		Bands b;
		b.lowHalf = GrainModel::Low(y, DetailPulse::WideSigma(scale));
		b.d1 = Image(y.w, y.h);
		b.d2 = Image(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int x = 0; x < y.w; ++x)
			{
				const double g1 = fine.at(x, yy);
				b.d1.at(x, yy) = y.at(x, yy) - g1;
				b.d2.at(x, yy) = g1 - GrainModel::LowAt(b.lowHalf, x + 0.5, yy + 0.5);
			}
		return b;
	}

	// The measure pass's per-tile statistics.
	struct Tiles
	{
		int tx = 0, ty = 0;
		Image e1, e2, meanL, spread, untrusted, change, weight, measurable;
	};

	inline Tiles Measure(const Bands& b, const Settings& s, const Field* field, const Tiles* prev)
	{
		const int T = DetailPulse::kTilePx, HT = T / 2;
		const int w = b.d1.w, h = b.d1.h;
		const int hw = b.lowHalf.w, hh = b.lowHalf.h;
		Tiles t;
		t.tx = (w + T - 1) / T;
		t.ty = (h + T - 1) / T;
		for (Image* img : { &t.e1, &t.e2, &t.meanL, &t.spread, &t.untrusted, &t.change, &t.weight, &t.measurable })
			*img = Image(t.tx, t.ty);
		const double toFieldX = s.fieldScaleX > 0.0 ? s.fieldScaleX : s.fieldScale, toFieldY = s.fieldScale;
		const double veto = DetailPulse::VetoCodes(s.bitrateQuality, s.colorDepth);
		for (int by = 0; by < t.ty; ++by)
			for (int bx = 0; bx < t.tx; ++bx)
			{
				double s1 = 0.0, s2 = 0.0, n = 0.0, un = 0.0;
				for (int cy = 0; cy < HT; ++cy)
					for (int cx = 0; cx < HT; ++cx)
					{
						double cell = 0.0;
						for (int q = 0; q < 4; ++q)
						{
							const int x = bx * T + 2 * cx + (q & 1), y = by * T + 2 * cy + (q >> 1);
							if (x < w && y < h)
							{
								s1 += b.d1.at(x, y) * b.d1.at(x, y);
								s2 += b.d2.at(x, y) * b.d2.at(x, y);
								cell += 1.0;
							}
						}
						n += cell;
						if (field && cell > 0.0)
						{
							const double lx = (bx * T + 2 * cx + 1) * toFieldX, ly = (by * T + 2 * cy + 1) * toFieldY;
							const int fx = (std::max)(0, (std::min)(field->w - 1, static_cast<int>(std::floor(lx / 16.0))));
							const int fy = (std::max)(0, (std::min)(field->h - 1, static_cast<int>(std::floor(ly / 16.0))));
							if (field->at(fx, fy) <= DetailPulse::kTrustMin) un += cell;
						}
					}
				double ls = 0.0, ls2 = 0.0, ln = 0.0;
				for (int qy = by * HT; qy < (std::min)(hh, (by + 1) * HT); ++qy)
					for (int qx = bx * HT; qx < (std::min)(hw, (bx + 1) * HT); ++qx)
					{
						const double l = b.lowHalf.at(qx, qy);
						ls += l;
						ls2 += l * l;
						ln += 1.0;
					}
				const double nn = (std::max)(n, 1.0), lnn = (std::max)(ln, 1.0);
				const double mean = ls / lnn;
				t.e1.at(bx, by) = s1 / nn;
				t.e2.at(bx, by) = s2 / nn;
				t.meanL.at(bx, by) = mean;
				t.spread.at(bx, by) = std::sqrt((std::max)(ls2 / lnn - mean * mean, 0.0));
				t.untrusted.at(bx, by) = un / nn;
				double change = 0.0, wt = 1.0, measurable = 1.0;
				if (prev)
				{
					change = (std::max)(std::fabs(mean - prev->meanL.at(bx, by)), std::fabs(t.spread.at(bx, by) - prev->spread.at(bx, by)));
					const double en = (s1 + s2) / nn, ep = prev->e1.at(bx, by) + prev->e2.at(bx, by);
					if (en > DetailPulse::kTileEnergyJump * ep || ep > DetailPulse::kTileEnergyJump * en) measurable = 0.0;
				}
				t.measurable.at(bx, by) = measurable;
				if (field) wt = DetailPulse::TrustWeight(un / nn);
				else if (prev) wt = DetailPulse::VetoWeight(change, veto);
				t.change.at(bx, by) = change;
				t.weight.at(bx, by) = wt;
			}
		return t;
	}

	// The reduce pass's frame statistics.
	struct FrameStats
	{
		double e1 = 0.0, e2 = 0.0, meanLuma = 0.0, cutShare = 0.0, untrustedShare = 0.0;
		bool measured = false;
	};

	inline FrameStats Reduce(const Tiles& t, const Settings& s)
	{
		const double floorEnergy = DetailPulse::FloorEnergy(s.colorDepth);
		const double cut = DetailPulse::CutCodes(s.bitrateQuality, s.colorDepth);
		double a1x = 0.0, a1y = 0.0, a1n = 0.0, my = 0.0, cuts = 0.0, unt = 0.0, n = 0.0;
		for (size_t i = 0; i < t.e1.v.size(); ++i)
		{
			if (t.e1.v[i] + t.e2.v[i] >= floorEnergy && t.weight.v[i] >= 0.5 && t.measurable.v[i] > 0.5) { a1x += t.e1.v[i]; a1y += t.e2.v[i]; a1n += 1.0; }
			my += t.meanL.v[i];
			cuts += t.change.v[i] > cut ? 1.0 : 0.0;
			unt += t.untrusted.v[i] > 0.5 ? 1.0 : 0.0;
			n += 1.0;
		}
		const double m1 = a1x / (std::max)(a1n, 1.0), m2 = a1y / (std::max)(a1n, 1.0);
		double w1 = 0.0, w2 = 0.0;
		for (size_t i = 0; i < t.e1.v.size(); ++i)
			if (t.e1.v[i] + t.e2.v[i] >= floorEnergy && t.weight.v[i] >= 0.5 && t.measurable.v[i] > 0.5)
			{
				w1 += (std::min)(t.e1.v[i], DetailPulse::kWinsorise * m1);
				w2 += (std::min)(t.e2.v[i], DetailPulse::kWinsorise * m2);
			}
		FrameStats f;
		f.measured = a1n > 0.0;
		f.e1 = f.measured ? w1 / a1n : 0.0;
		f.e2 = f.measured ? w2 / a1n : 0.0;
		f.meanLuma = my / (std::max)(n, 1.0);
		f.cutShare = cuts / (std::max)(n, 1.0);
		f.untrustedShare = unt / (std::max)(n, 1.0);
		return f;
	}

	// The apply pass: Y + w ((g1 - 1) D1 + (g2 - 1) D2), w bilinear between tile centres.
	inline Image ApplyGains(const Image& y, const Bands& b, const Image& tileWeight, double g1, double g2)
	{
		const double T = DetailPulse::kTilePx;
		Image out(y.w, y.h);
		for (int yy = 0; yy < y.h; ++yy)
			for (int x = 0; x < y.w; ++x)
			{
				const double w = GrainModel::Bilinear(tileWeight, (x + 0.5) / T, (yy + 0.5) / T);
				const double delta = w * ((g1 - 1.0) * b.d1.at(x, yy) + (g2 - 1.0) * b.d2.at(x, yy));
				out.at(x, yy) = y.at(x, yy) + delta;
			}
		return out;
	}

	class Model
	{
	public:
		Frame Process(const Image& luma, uint64_t sequence, const Settings& s, const Field* field = nullptr)
		{
			const double scale = DetailPulse::Scale(s.planeLines > 0.0 ? s.planeLines : luma.h);
			const bool sameGeometry = decided && luma.w == geoW && luma.h == geoH && scale == geoScale;
			const bool repeat = sameGeometry && sequence != 0 && sequence == lastSequence;
			const bool consecutive = sameGeometry && lastSequence != 0 && sequence == lastSequence + 1;
			const Bands b = MakeBands(luma, scale);
			Frame f;
			if (repeat)
			{
				f = last;
				f.repeat = true;
				f.out = ApplyGains(luma, b, last.tileWeight, last.g1, last.g2);
				return f;
			}
			const Tiles t = Measure(b, s, field, consecutive ? &prevTiles : nullptr);
			const FrameStats fs = Reduce(t, s);
			const int slot = decided ? (cur + 1) % DetailPulse::kRingSlots : 0;
			const int nValid = consecutive ? (std::min)(behind + 1, DetailPulse::kRingSlots - 2) : 0;
			const int nUse = (std::min)(nValid, DetailPulse::WindowFrames(s.sourceHz));
			const bool aheadValid = aheadSequence != 0 && sequence != 0 && aheadSequence == sequence + 1 &&
				aheadW == luma.w && aheadH == luma.h && aheadScale == scale;
			const double cutCodes = DetailPulse::CutCodes(s.bitrateQuality, s.colorDepth);
			bool cut = nValid > 0 && fs.cutShare > DetailPulse::kCutShare;
			if (field) cut = cut && fs.untrustedShare > 0.5;
			bool fade = false;
			if (nValid >= 2)
			{
				const double p1 = ring[(slot + kRing - 1) % kRing][2], p2 = ring[(slot + kRing - 2) % kRing][2];
				const double d0 = fs.meanLuma - p1, d1 = p1 - p2;
				const double fc = DetailPulse::FadeCodes(s.colorDepth);
				fade = std::fabs(d0) >= fc && std::fabs(d1) >= fc && d0 * d1 > 0.0;
			}
			const bool restart = nValid == 0 || cut || fade;
			double r1 = fs.e1, r2 = fs.e2;
			if (!restart)
			{
				const double decay = DetailPulse::DecayPerFrame(s.sourceHz);
				double dec = 1.0;
				for (int j = 1; j <= nUse; ++j)
				{
					const std::array<double, 4>& e = ring[(slot + kRing - j) % kRing];
					dec *= decay;
					if (e[3] < 0.5) break;
					r1 = (std::max)(r1, e[0] * dec);
					r2 = (std::max)(r2, e[1] * dec);
					if (e[3] > 1.5) break;
				}
				if (aheadValid)
				{
					const std::array<double, 4>& a = ring[kRing + 1];
					const double ra = (a[0] + a[1]) / (std::max)(fs.e1 + fs.e2, 1e-20);
					if (std::fabs(a[2] - fs.meanLuma) <= cutCodes && ra <= DetailPulse::kAheadMaxRatio && ra * DetailPulse::kAheadMaxRatio >= 1.0)
					{
						r1 = (std::max)(r1, a[0] * decay);
						r2 = (std::max)(r2, a[1] * decay);
					}
				}
			}
			f.g1 = f.g2 = 1.0;
			if (!restart && fs.measured)
			{
				f.g1 = DetailPulse::Gain(r1, fs.e1, s.cap);
				f.g2 = DetailPulse::Gain(r2, fs.e2, s.cap);
			}
			ring[slot] = { fs.e1, fs.e2, fs.meanLuma, restart ? 2.0 : 1.0 };
			f.restart = restart;
			f.aheadUsed = aheadValid;
			f.e1 = fs.e1;
			f.e2 = fs.e2;
			f.meanLuma = fs.meanLuma;
			f.cutShare = fs.cutShare;
			f.tileWeight = t.weight;
			f.out = ApplyGains(luma, b, t.weight, f.g1, f.g2);
			prevTiles = t;
			cur = slot;
			behind = nValid;
			lastSequence = sequence;
			geoW = luma.w;
			geoH = luma.h;
			geoScale = scale;
			decided = true;
			last = f;
			last.out = Image();
			return f;
		}

		// The next source frame's energies (the ring's look-ahead entry).
		void MeasureAhead(const Image& luma, uint64_t sequence, const Settings& s)
		{
			const double scale = DetailPulse::Scale(s.planeLines > 0.0 ? s.planeLines : luma.h);
			const Bands b = MakeBands(luma, scale);
			const FrameStats fs = Reduce(Measure(b, s, nullptr, nullptr), s);
			ring[kRing + 1] = { fs.e1, fs.e2, fs.meanLuma, 1.0 };
			aheadSequence = sequence;
			aheadW = luma.w;
			aheadH = luma.h;
			aheadScale = scale;
		}

		void Forget()
		{
			decided = false;
			behind = 0;
			aheadSequence = 0;
		}

		uint64_t HistorySequence() const { return decided ? lastSequence : 0; }
		int HistoryFrames() const { return decided ? behind : 0; }

	private:
		static constexpr int kRing = DetailPulse::kRingSlots;
		std::vector<std::array<double, 4>> ring = std::vector<std::array<double, 4>>(kRing + 2, std::array<double, 4>{ { 0.0, 0.0, 0.0, 0.0 } });
		Tiles prevTiles;
		Frame last;
		int cur = 0, behind = 0;
		bool decided = false;
		uint64_t lastSequence = 0, aheadSequence = 0;
		int geoW = 0, geoH = 0, aheadW = 0, aheadH = 0;
		double geoScale = 0.0, aheadScale = 0.0;
	};
}
