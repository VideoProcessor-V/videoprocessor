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
#include <memory>
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
		std::shared_ptr<const void> keepAlive;          // owns the storage behind label: keeps it valid while the view lives
		double pictureCx = 0.0, pictureCy = 0.0;        // picture centre, plane pixels
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
	class FieldMotionSource : public IMotionSource
	{
	public:
		void SetBackward(uint64_t seq, const MotionPair& pair) { m_backward[seq] = pair; }
		void SetForward(uint64_t seq, const MotionPair& pair) { m_forward[seq] = pair; }
		void Clear() { m_backward.clear(); m_forward.clear(); }
		// Drops every pair older than oldestNeeded (bug hunt 38 H1: the maps otherwise grow without bound).
		void PruneField(uint64_t oldestNeeded)
		{
			m_backward.erase(m_backward.begin(), m_backward.lower_bound(oldestNeeded));
			m_forward.erase(m_forward.begin(), m_forward.lower_bound(oldestNeeded));
		}
		size_t FieldCount() const { return m_backward.size() + m_forward.size(); }
		bool Backward(uint64_t seq, MotionPair& out) const override { return Find(m_backward, seq, out); }
		bool Forward(uint64_t seq, MotionPair& out) const override { return Find(m_forward, seq, out); }

	private:
		static bool Find(const std::map<uint64_t, MotionPair>& m, uint64_t seq, MotionPair& out)
		{
			const auto it = m.find(seq);
			if (it == m.end() || !it->second.valid) return false;
			out = it->second;
			return true;
		}
		std::map<uint64_t, MotionPair> m_backward, m_forward;
	};

#ifdef TEMPORAL_DETAIL_HAVE_CAMERA_MODEL
	// Wraps a FieldMotionSource and adds the camera model's per-pair record. Compiled only where the
	// camera model's header exists (branch feat/camera-model); without it the module runs on the field.
	// Single-threaded: SetCamera/Prune/Camera must all be called from one thread (the host adapter's).
	class CameraModelMotionSource : public FieldMotionSource
	{
	public:
		explicit CameraModelMotionSource(const CameraModel::Policy& policy) : m_policy(policy) {}
		void SetCamera(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> record) { m_cam[seq] = std::move(record); }
		// Drops the records of every sequence below oldestNeeded (each holds a block-data buffer).
		// Also drops the field pairs below it (FieldMotionSource::PruneField, bug hunt 38 H1): one Prune bounds every map.
		void Prune(uint64_t oldestNeeded)
		{
			m_cam.erase(m_cam.begin(), m_cam.lower_bound(oldestNeeded));
			PruneField(oldestNeeded);
		}
		// Clears the camera records as well as the field pairs (hides FieldMotionSource::Clear, which would keep m_cam).
		void Clear()
		{
			FieldMotionSource::Clear();
			m_cam.clear();
		}
		size_t CameraCount() const { return m_cam.size(); }

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
			out.keepAlive = r.blocks;   // the view outlives a SetCamera replacement or a Prune of this seq
			out.pictureCx = r.picture.CentreX();
			out.pictureCy = r.picture.CentreY();
			return true;
		}

	private:
		CameraModel::Policy m_policy;
		std::map<uint64_t, std::shared_ptr<const CameraModel::CameraMotion>> m_cam;
	};
#endif
}
