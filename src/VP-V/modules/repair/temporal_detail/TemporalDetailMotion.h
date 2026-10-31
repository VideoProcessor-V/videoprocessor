// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail motion interface. The module reads motion only through IMotionSource:
//  * Backward(seq): the vectors of the pair (seq -> seq - 1) on frame seq's block grid;
//  * Forward(seq):  the vectors of the pair (seq - 1 -> seq) on frame seq - 1's block grid (queued modes);
//  * Camera(seq):   the camera model's record for the same pair, through CameraPairView.
// Vectors are in source pixels OF THE RE-BASED PLANE: the host scales them to the plane before handing
// them over (the module never rescales). A vector at position p of the destination frame is the matching
// position in the other frame minus p. Header-only, C++14-compatible, no Windows headers.
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#if __has_include(<VP-V/modules/motion/estimation/global_model/CameraAccessor.h>)
#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#define TEMPORAL_DETAIL_HAVE_CAMERA_MODEL 1
#endif

namespace TemporalDetail
{
	struct MotionPair
	{
		bool valid = false;
		int gridW = 0, gridH = 0;
		double blockPx = 16.0;
		std::vector<float> vx, vy, trust;   // gridW x gridH, raster; trust 0..1
		double originX = 0.0, originY = 0.0; // the reader's coordinate origin in the grid's plane (an active-picture crop)

		static MotionPair Uniform(int imageW, int imageH, double blockPx, double dx, double dy, double trust = 1.0)
		{
			MotionPair m;
			m.valid = true;
			m.blockPx = blockPx;
			m.gridW = static_cast<int>(std::ceil(imageW / blockPx));
			m.gridH = static_cast<int>(std::ceil(imageH / blockPx));
			const size_t n = static_cast<size_t>(m.gridW) * m.gridH;
			m.vx.assign(n, static_cast<float>(dx));
			m.vy.assign(n, static_cast<float>(dy));
			m.trust.assign(n, static_cast<float>(trust));
			return m;
		}
	};

	// The camera model's record for one pair, as the module needs it (maps frame N-1 -> N).
	struct CameraPairView
	{
		bool usable = false;
		double h[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };   // row-major 3x3, picture-centred source px (CameraMotion.h)
		double confidence = 0.0;
		int gridW = 0, gridH = 0;
		double blockSize = 0.0;
		const uint8_t* label = nullptr;                 // gridW x gridH, 1 = Camera
		double pictureCx = 0.0, pictureCy = 0.0;        // picture centre, plane pixels
		double originX = 0.0, originY = 0.0;            // the reader's origin in the label grid's plane (an active-picture crop)
	};

	class IMotionSource
	{
	public:
		virtual ~IMotionSource() = default;
		virtual bool Backward(uint64_t seq, MotionPair& out) const = 0;
		virtual bool Forward(uint64_t seq, MotionPair& out) const = 0;
		virtual bool Camera(uint64_t seq, CameraPairView& out) const { (void)seq; (void)out; return false; }
	};

	// Per-sequence pairs fed by the caller (host adapter or tests). Camera() is always false.
	// Bounded: the model reads only the presented frame's pairs and those of the queued frames ahead of it (composition
	// is incremental, nothing older is read again), so entries are pruned (a) older than the newest stored sequence minus
	// the retention window (a frame count, not a rate: it covers the deepest queue ahead of the presented frame), (b) older
	// than the presented frame when the host calls Prune(presented), and (c) all at Clear(). The host calls Clear() on every
	// seek, reset or stamp change (sequence numbers may be reused: a stale pair must never answer for a new frame).
	class FieldMotionSource : public IMotionSource
	{
	public:
		static constexpr uint64_t kDefaultRetention = 128;  // frames kept behind the newest pair (the host also calls Prune per presented frame)
		void SetRetention(uint64_t frames) { m_retention = frames < 1 ? 1 : frames; Trim(); }
		uint64_t Retention() const { return m_retention; }
		void SetBackward(uint64_t seq, const MotionPair& pair) { m_backward[seq] = pair; Note(seq); }
		void SetForward(uint64_t seq, const MotionPair& pair) { m_forward[seq] = pair; Note(seq); }
		virtual void Clear() { m_backward.clear(); m_forward.clear(); m_newest = 0; m_haveNewest = false; }
		// Pairs older than the presented frame are never read again.
		virtual void Prune(uint64_t presented) { Drop(m_backward, presented); Drop(m_forward, presented); }
		virtual size_t Size() const { return m_backward.size() + m_forward.size(); }
		bool Backward(uint64_t seq, MotionPair& out) const override { return Find(m_backward, seq, out); }
		bool Forward(uint64_t seq, MotionPair& out) const override { return Find(m_forward, seq, out); }

	protected:
		template <typename M>
		static void Drop(M& m, uint64_t from) { m.erase(m.begin(), m.lower_bound(from)); }
		uint64_t Floor() const { return m_haveNewest && m_newest >= m_retention ? m_newest - m_retention + 1 : 0; }
		uint64_t Ceiling() const { return m_newest + m_retention; }
		virtual void Trim()
		{
			const uint64_t f = Floor(), c = Ceiling();
			Drop(m_backward, f); Drop(m_forward, f); DropAbove(m_backward, c); DropAbove(m_forward, c);
		}
		void Note(uint64_t seq)
		{
			// A sequence far behind the newest (a seek back, a restart) starts the window again from it.
			if (!m_haveNewest || seq > m_newest || m_newest - seq >= m_retention) { m_newest = seq; m_haveNewest = true; }
			Trim();
		}
		template <typename M>
		static void DropAbove(M& m, uint64_t above) { m.erase(m.upper_bound(above), m.end()); }

	private:
		static bool Find(const std::map<uint64_t, MotionPair>& m, uint64_t seq, MotionPair& out)
		{
			const auto it = m.find(seq);
			if (it == m.end() || !it->second.valid) return false;
			out = it->second;
			return true;
		}
		std::map<uint64_t, MotionPair> m_backward, m_forward;
		uint64_t m_retention = kDefaultRetention, m_newest = 0;
		bool m_haveNewest = false;
	};

#ifdef TEMPORAL_DETAIL_HAVE_CAMERA_MODEL
	// Wraps a FieldMotionSource and adds the camera model's per-pair record. Compiled only where the
	// camera model's header exists (branch feat/camera-model); without it the module runs on the field.
	class CameraModelMotionSource : public FieldMotionSource
	{
	public:
		explicit CameraModelMotionSource(const CameraModel::Policy& policy) : m_policy(policy) {}
		void SetCamera(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> record) { m_cam[seq] = std::move(record); Note(seq); }
		void Clear() override { FieldMotionSource::Clear(); m_cam.clear(); }
		void Prune(uint64_t presented) override { FieldMotionSource::Prune(presented); Drop(m_cam, presented); }
		size_t Size() const override { return FieldMotionSource::Size() + m_cam.size(); }

		bool Camera(uint64_t seq, CameraPairView& out) const override
		{
			out = CameraPairView();
			const auto it = m_cam.find(seq);
			if (it == m_cam.end() || !it->second) return false;
			const CameraModel::CameraMotion& r = *it->second;
			if (!CameraModel::ModelUsable(m_policy, &r)) return false;
			if (r.reason != CameraModel::Reason::Ok && r.reason != CameraModel::Reason::PostCutSeeded && r.reason != CameraModel::Reason::FlashBridged) return false;
			if (r.confidence < m_policy.threshold) return false;
			out.usable = true;
			for (int i = 0; i < 9; ++i) out.h[i] = r.params.h[i];
			out.confidence = r.confidence;
			out.gridW = r.blocks->gridW;
			out.gridH = r.blocks->gridH;
			out.blockSize = r.blocks->blockSize;
			out.label = r.blocks->label.empty() ? nullptr : r.blocks->label.data();
			out.pictureCx = r.picture.CentreX();
			out.pictureCy = r.picture.CentreY();
			return true;
		}

	protected:
		void Trim() override { FieldMotionSource::Trim(); Drop(m_cam, Floor()); DropAbove(m_cam, Ceiling()); }

	private:
		CameraModel::Policy m_policy;
		std::map<uint64_t, std::shared_ptr<const CameraModel::CameraMotion>> m_cam;
	};
#endif
}
