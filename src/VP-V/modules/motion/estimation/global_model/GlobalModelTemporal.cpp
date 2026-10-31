// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Global camera model: temporal filter and fixed-lag smoother. Deterministic, no clocks.
#include "GlobalModelTemporal.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "GlobalModelGeometry.h"

namespace CameraModel
{
	namespace
	{
		// TemporalFilter scheme (per component, scalar Kalman-style with a damped rate):
		//   predicted  = value + rate * dt
		//   innovation = measured - predicted
		//   q          = kProcess * dt * (|innovation| + |value|)^2 + 1e-6 * measured^2 + 1e-12
		//   P          = var + q            R = (kMeasure * |measured|)^2 + 1e-12
		//   gain       = P / (P + R)        value = predicted + gain * innovation
		//   var        = (1 - gain) * P
		//   rate      += kRate * gain * innovation / dt   (only when dt > 0), then clamped to
		//                +-kRateLimit * |value| per second so a step cannot cause a large overshoot.
		// With dt <= 0 there is no prediction step and q has only the floor: a pure blend.
		const double kProcess = 1.0;
		const double kMeasure = 0.05;
		const double kInitial = 0.25;
		const double kRate = 0.2;
		const double kRateLimit = 0.5;
		const double kSigmaRel = 0.02;

		inline double Sq(double v) { return v * v; }

		inline void ToArray(const SimilarityRates& r, double* a)
		{
			a[0] = r.logScale; a[1] = r.angle; a[2] = r.tx; a[3] = r.ty;
		}

		inline SimilarityRates FromArray(const double* a)
		{
			SimilarityRates r;
			r.logScale = a[0]; r.angle = a[1]; r.tx = a[2]; r.ty = a[3];
			return r;
		}

		inline bool BreaksWindow(const CameraMotion& r)
		{
			return !r.valid || r.reason == Reason::Cut || r.reason == Reason::Flash
				|| r.reason == Reason::GenerationReset;
		}

		struct Member
		{
			const CameraMotion* rec;
			double w;
		};
	}

	void TemporalFilter::Reset()
	{
		m_valid = false;
		for (int c = 0; c < 4; ++c) { m_value[c] = 0; m_rate[c] = 0; m_var[c] = 1; }
	}

	bool TemporalFilter::Predict(double dtSeconds, SimilarityRates& out) const
	{
		if (!m_valid) return false;
		const double dt = dtSeconds > 0 ? dtSeconds : 0;
		double p[4];
		for (int c = 0; c < 4; ++c) p[c] = m_value[c] + m_rate[c] * dt;
		out = FromArray(p);
		return true;
	}

	double TemporalFilter::Consistency(double dtSeconds, const SimilarityRates& measured) const
	{
		if (!m_valid) return 0.5;
		const double dt = dtSeconds > 0 ? dtSeconds : 0;
		double m[4];
		ToArray(measured, m);
		double pred[4];
		double sumAbs = 0;
		for (int c = 0; c < 4; ++c) { pred[c] = m_value[c] + m_rate[c] * dt; sumAbs += std::fabs(pred[c]); }
		const double floorSigma = 1e-9 * sumAbs + 1e-12;
		double acc = 0;
		for (int c = 0; c < 4; ++c)
		{
			const double sigma = std::sqrt(m_var[c]) + kSigmaRel * std::fabs(pred[c]) + floorSigma;
			acc += Sq(m[c] - pred[c]) / Sq(sigma);
		}
		const double d = std::sqrt(acc) / 2.0;
		if (!std::isfinite(d)) return 0.0;
		return std::exp(-0.5 * d * d);
	}

	void TemporalFilter::Update(double dtSeconds, const SimilarityRates& measured)
	{
		double m[4];
		ToArray(measured, m);
		if (!m_valid)
		{
			for (int c = 0; c < 4; ++c)
			{
				m_value[c] = m[c];
				m_rate[c] = 0;
				m_var[c] = Sq(kInitial * m[c]) + 1e-12;
			}
			m_valid = true;
			return;
		}
		const double dt = dtSeconds > 0 ? dtSeconds : 0;
		for (int c = 0; c < 4; ++c)
		{
			const double pred = m_value[c] + m_rate[c] * dt;
			const double innov = m[c] - pred;
			const double mag = std::fabs(innov) + std::fabs(m_value[c]);
			const double q = kProcess * dt * Sq(mag) + 1e-6 * Sq(m[c]) + 1e-12;
			const double P = m_var[c] + q;
			const double R = Sq(kMeasure * m[c]) + 1e-12;
			const double gain = P / (P + R);
			m_value[c] = pred + gain * innov;
			m_var[c] = (1.0 - gain) * P;
			if (dt > 1e-12)
			{
				double r = m_rate[c] + kRate * gain * innov / dt;
				const double lim = kRateLimit * std::fabs(m_value[c]);
				if (r > lim) r = lim;
				if (r < -lim) r = -lim;
				m_rate[c] = r;
			}
		}
	}

	void FixedLagSmoother::Reset()
	{
		m_records.clear();
	}

	void FixedLagSmoother::Push(const CameraMotion& record)
	{
		CameraMotion copy = record;
		copy.blocks.reset();
		copy.objects.clear();
		for (auto it = m_records.begin(); it != m_records.end(); ++it)
		{
			if (it->seq == copy.seq) { *it = copy; return; }
		}
		auto pos = m_records.end();
		while (pos != m_records.begin() && (pos - 1)->seq > copy.seq) --pos;
		m_records.insert(pos, copy);
		const size_t cap = static_cast<size_t>(2 * m_lag + 3);
		while (m_records.size() > cap) m_records.pop_front();
	}

	bool FixedLagSmoother::Smoothed(uint64_t seq, Params& out) const
	{
		const CameraMotion* centre = nullptr;
		for (size_t i = 0; i < m_records.size(); ++i)
			if (m_records[i].seq == seq) { centre = &m_records[i]; break; }
		if (!centre || !centre->valid) return false;
		if (m_lag == 0) { out = centre->params; return true; }
		if (centre->reason == Reason::Cut || centre->reason == Reason::Flash
			|| centre->reason == Reason::GenerationReset || !(centre->pairSeconds > 0))
		{
			out = centre->params;
			return true;
		}
		const uint64_t newest = m_records.back().seq;
		const int lag = m_lag;

		auto find = [&](uint64_t s) -> const CameraMotion*
		{
			for (size_t i = 0; i < m_records.size(); ++i)
				if (m_records[i].seq == s) return &m_records[i];
			return nullptr;
		};

		std::vector<Member> past, future;
		{
			const CameraMotion* prev = centre;
			for (int k = 1; k <= lag; ++k)
			{
				if (seq < static_cast<uint64_t>(k)) break;
				const CameraMotion* r = find(seq - static_cast<uint64_t>(k));
				if (!r) continue;
				if (BreaksWindow(*r) || r->resyncGeneration != prev->resyncGeneration
					|| r->rebaseGeneration != prev->rebaseGeneration) break;
				past.push_back({ r, static_cast<double>(lag + 1 - k) });
				prev = r;
			}
		}
		{
			const CameraMotion* prev = centre;
			for (int k = 1; k <= lag; ++k)
			{
				const uint64_t s = seq + static_cast<uint64_t>(k);
				const CameraMotion* r = find(s);
				if (!r)
				{
					if (s > newest) return false;
					continue;
				}
				if (BreaksWindow(*r) || r->resyncGeneration != prev->resyncGeneration
					|| r->rebaseGeneration != prev->rebaseGeneration) break;
				future.push_back({ r, static_cast<double>(lag + 1 - k) });
				prev = r;
			}
		}

		std::vector<Member> window;
		for (size_t i = past.size(); i > 0; --i) window.push_back(past[i - 1]);
		window.push_back({ centre, static_cast<double>(lag + 1) });
		for (size_t i = 0; i < future.size(); ++i) window.push_back(future[i]);

		ModelType highest = ModelType::None;
		for (size_t i = 0; i < window.size(); ++i)
			if (static_cast<int>(window[i].rec->params.type) > static_cast<int>(highest))
				highest = window[i].rec->params.type;

		const double sec = centre->pairSeconds;

		if (highest != ModelType::Affine && highest != ModelType::Homography)
		{
			double sum[4] = { 0, 0, 0, 0 };
			double wsum = 0;
			bool allTranslation = true;
			for (size_t i = 0; i < window.size(); ++i)
			{
				const CameraMotion& r = *window[i].rec;
				const double w = window[i].w;
				sum[0] += w * r.rates.logScale;
				sum[1] += w * r.rates.angle;
				sum[2] += w * r.rates.tx;
				sum[3] += w * r.rates.ty;
				wsum += w;
				if (r.params.type != ModelType::Translation) allTranslation = false;
			}
			SimilarityRates avg;
			avg.logScale = sum[0] / wsum; avg.angle = sum[1] / wsum;
			avg.tx = sum[2] / wsum; avg.ty = sum[3] / wsum;
			if (allTranslation)
			{
				Params p;
				p.type = ModelType::Translation;
				p.h[2] = avg.tx * sec;
				p.h[5] = avg.ty * sec;
				out = p;
			}
			else
			{
				// Rebuilt through FromSimilarity so the rotation convention is the library's own.
				Params p = FromSimilarity(avg, sec);
				p.type = ModelType::Similarity;
				out = p;
			}
			return true;
		}

		// Corner-displacement method (per second, weighted), refitted exactly through the four
		// corners of the centre record's picture.
		double cdx[4] = { 0, 0, 0, 0 }, cdy[4] = { 0, 0, 0, 0 };
		double wsum = 0;
		for (size_t i = 0; i < window.size(); ++i)
		{
			const CameraMotion& r = *window[i].rec;
			if (!(r.pairSeconds > 0)) continue;
			const double rw = 0.5 * r.picture.Width(), rh = 0.5 * r.picture.Height();
			const double cx[4] = { -rw, rw, -rw, rw };
			const double cy[4] = { -rh, -rh, rh, rh };
			for (int c = 0; c < 4; ++c)
			{
				double dx = 0, dy = 0;
				Displacement(r.params, cx[c], cy[c], &dx, &dy);
				cdx[c] += window[i].w * dx / r.pairSeconds;
				cdy[c] += window[i].w * dy / r.pairSeconds;
			}
			wsum += window[i].w;
		}
		if (!(wsum > 0)) return false;
		const double hw = 0.5 * centre->picture.Width(), hh = 0.5 * centre->picture.Height();
		const double px[4] = { -hw, hw, -hw, hw };
		const double py[4] = { -hh, -hh, hh, hh };
		Sample s[4];
		for (int c = 0; c < 4; ++c)
		{
			s[c].x = px[c]; s[c].y = py[c];
			s[c].dx = cdx[c] / wsum * sec;
			s[c].dy = cdy[c] / wsum * sec;
			s[c].w = 1;
		}
		Params p;
		if (!SolveMinimal(highest, s, MinimalCount(highest), p)) return false;
		p.type = highest;
		out = p;
		return true;
	}
}
