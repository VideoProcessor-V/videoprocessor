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

	bool PrepareComputePairInput(const FieldCopy& field, const CameraModel::BlockData& blocks, const CameraModel::Rect& picture,
		CameraModel::PairInput& in, std::vector<float>& regridX, std::vector<float>& regridY, std::vector<float>& regridTrust);

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
		// Releases all owned label/input textures and LabelPass, withdrawing VramUsers (kUserCameraModel). Idempotent.
		void Release();
		uint64_t DeclaredVramBytes() const { return m_declaredVram; }

	private:
		pl_tex* SlotOf(LabelTarget target);
		bool EnsureSlot(pl_tex* tex, int w, int h);
		bool Upload(pl_tex* tex, const std::vector<float>& texels, int w, int h);
		bool ProduceUpload(const CameraModel::CameraMotion& rec, pl_tex* slot);
		bool ProduceCompute(const CameraModel::CameraMotion& rec, const FieldCopy* field, pl_tex* slot);
		uint64_t HeldVramBytes() const;
		void UpdateVramDeclaration();

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
		uint64_t m_declaredVram = 0;
		std::string m_error;
	};
}
