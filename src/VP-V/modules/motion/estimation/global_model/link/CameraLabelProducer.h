// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K2-1): the P8 producer, ONE unit (CameraLink::kCameraUnitVersion) in two modes, either/or per graph and never
// mixed within one:
//   Upload   (default while cpuLabels = true) pack the CPU labels (CameraLabelPack.h) and upload them: exact with the CPU accessor.
//   Compute  (switch camera_labels_gpu, forced when the record is labelsOnGpu) LabelPass from the FieldCopy: the cost path at 8K.
// Texture slots mirror the P8 SlotSet: one per ring slot and per live-pool buffer (the host sizes them from the graph node).
// Render thread only (libplacebo). GPU objects are released in the destructor (OnGpuReleased).
#pragma once

#include "CameraLabelFill.h"
#include "CameraLabelPack.h"

#include <VP-V/modules/motion/estimation/global_model/CameraModelGpu.h>

#include <libplacebo/gpu.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace CameraLink
{
	enum class LabelMode : uint8_t { Upload, Compute };

	// The one choice rule: compute when asked or forced; upload otherwise.
	inline LabelMode ChooseLabelMode(bool computeSwitch, bool recordLabelsOnGpu)
	{
		return (computeSwitch || recordLabelsOnGpu) ? LabelMode::Compute : LabelMode::Upload;
	}

	// GEM-129: validates that `field` is either on `blocks`'s exact grid or a cleanly regriddable integer-multiple
	// finer native grid (e.g. 8-px AheadFit field vs 16-px consumer BlockData), and populates `in` on `blocks`'s grid.
	// Exact derivation: a picture of W x H source px gives src = ceil(W / srcBs) native cells and dst = ceil(W / dstBs)
	// consumer blocks; when dstBs = scale * srcBs for integer scale >= 1, ceil(ceil(W / srcBs) / scale) == ceil(W / dstBs)
	// holds identically iff (src + scale - 1) / scale == dst. When scale > 1, samples each consumer block from the native
	// cell its centre lies in — matching AheadFlow.h RegridToConsumerGrid so aux.excluded and P8 texture dims align 1:1.
	// Returns false on any incompatible grid shape or block-size ratio (caller refuses with "camera labels: grid mismatch").
	inline bool PrepareComputePairInput(const FieldCopy& field, const CameraModel::BlockData& blocks,
		const CameraModel::Rect& picture, CameraModel::PairInput& in,
		std::vector<float>& regridX, std::vector<float>& regridY, std::vector<float>& regridTrust)
	{
		if (!field.Valid() || field.fwdY.size() != field.Blocks() || field.fwdTrust.size() != field.Blocks() ||
			!CameraModel::IsFiniteBits(field.blockSize))
			return false;
		if (blocks.gridW <= 0 || blocks.gridH <= 0 || !(blocks.blockSize > 0.0) ||
			!CameraModel::IsFiniteBits(blocks.blockSize))
			return false;
		const size_t dstBlocks = static_cast<size_t>(blocks.gridW) * static_cast<size_t>(blocks.gridH);
		if (!blocks.excluded.empty() && blocks.excluded.size() != dstBlocks)
			return false;

		const double ratio = blocks.blockSize / field.blockSize;
		if (!CameraModel::IsFiniteBits(ratio) || ratio < 1.0 - 1e-6 || ratio > static_cast<double>(INT32_MAX))
			return false;
		const int64_t scale = static_cast<int64_t>(std::llround(ratio));
		if (scale < 1 || std::fabs(blocks.blockSize - field.blockSize * static_cast<double>(scale)) > 1e-6 * blocks.blockSize)
			return false;
		const int64_t expectedW = (static_cast<int64_t>(field.gridW) + scale - 1) / scale;
		const int64_t expectedH = (static_cast<int64_t>(field.gridH) + scale - 1) / scale;
		if (expectedW != blocks.gridW || expectedH != blocks.gridH)
			return false;

		in = CameraModel::PairInput();
		in.gridW = blocks.gridW;
		in.gridH = blocks.gridH;
		in.blockSize = blocks.blockSize;
		in.picture = picture;
		in.quantumPx = field.quantumPx;
		if (scale == 1)
		{
			in.fwdX = field.fwdX.data();
			in.fwdY = field.fwdY.data();
			in.fwdTrust = field.fwdTrust.data();
			return true;
		}
		regridX.resize(dstBlocks);
		regridY.resize(dstBlocks);
		regridTrust.resize(dstBlocks);
		const int srcW = field.gridW;
		const int srcH = field.gridH;
		const double srcBs = field.blockSize;
		const double dstBs = blocks.blockSize;
		auto cellOf = [](double centre, double cellSize, int grid) -> int {
			const int c = static_cast<int>(std::floor(centre / cellSize));
			return c < 0 ? 0 : c > grid - 1 ? grid - 1 : c;
		};
		for (int ty = 0; ty < blocks.gridH; ++ty)
		{
			const int cy = cellOf((ty + 0.5) * dstBs, srcBs, srcH);
			const size_t srcRow = static_cast<size_t>(cy) * static_cast<size_t>(srcW);
			const size_t dstRow = static_cast<size_t>(ty) * static_cast<size_t>(blocks.gridW);
			for (int tx = 0; tx < blocks.gridW; ++tx)
			{
				const int cx = cellOf((tx + 0.5) * dstBs, srcBs, srcW);
				const size_t srcIdx = srcRow + static_cast<size_t>(cx);
				const size_t dstIdx = dstRow + static_cast<size_t>(tx);
				regridX[dstIdx] = field.fwdX[srcIdx];
				regridY[dstIdx] = field.fwdY[srcIdx];
				regridTrust[dstIdx] = field.fwdTrust[srcIdx];
			}
		}
		in.fwdX = regridX.data();
		in.fwdY = regridY.data();
		in.fwdTrust = regridTrust.data();
		return true;
	}

	class CameraLabelProducer : public ILabelDevice
	{
	public:
		CameraLabelProducer(pl_log log, pl_gpu gpu, LabelMode mode, int ringSlots, int liveSlots,
			const CameraModel::Config& fitConfig = CameraModel::Config(), bool hostReadable = false);
		~CameraLabelProducer() override;
		CameraLabelProducer(const CameraLabelProducer&) = delete;
		CameraLabelProducer& operator=(const CameraLabelProducer&) = delete;

		bool Produce(const CameraModel::CameraMotion& rec, const FieldCopy* field, LabelTarget target) override;
		pl_tex TextureOf(LabelTarget target) const override;
		uint32_t PredictUs() const override { return m_costUs; }

		LabelMode Mode() const { return m_mode; }
		const std::string& Error() const { return m_error; }
		uint64_t GridMismatchCount() const { return m_gridMismatches; }
		uint64_t RegridCount() const { return m_regridded; }

	private:
		pl_tex* SlotOf(LabelTarget target);
		bool EnsureSlot(pl_tex* tex, int w, int h);
		bool Upload(pl_tex* tex, const std::vector<float>& texels, int w, int h);
		bool ProduceUpload(const CameraModel::CameraMotion& rec, pl_tex* slot);
		bool ProduceCompute(const CameraModel::CameraMotion& rec, const FieldCopy* field, pl_tex* slot);

		pl_log m_log;
		pl_gpu m_gpu;
		LabelMode m_mode;
		CameraModel::Config m_cfg;
		bool m_readable;
		std::vector<pl_tex> m_ring, m_live;
		pl_tex m_fieldTex = nullptr, m_auxTex = nullptr;
		std::unique_ptr<CameraModel::LabelPass> m_pass;
		std::vector<float> m_scratch, m_scratchAux;
		std::vector<float> m_regridX, m_regridY, m_regridTrust;
		uint32_t m_costUs = 0;
		uint64_t m_gridMismatches = 0;
		uint64_t m_regridded = 0;
		std::string m_error;
	};
}
