// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraLabelProducer.h"

#include <VP-V/core/VramUsers.h>

#include <chrono>
#include <cmath>
#include <cstdint>

namespace CameraLink
{
	// GEM-129: validates that `field` is either on `blocks`'s exact grid or a cleanly regriddable integer-multiple
	// finer native grid (e.g. 8-px AheadFit field vs 16-px consumer BlockData), and populates `in` on `blocks`'s grid.
	// Exact derivation: a picture of W x H source px gives src = ceil(W / srcBs) native cells and dst = ceil(W / dstBs)
	// consumer blocks; when dstBs = scale * srcBs for integer scale >= 1, ceil(ceil(W / srcBs) / scale) == ceil(W / dstBs)
	// holds identically iff (src + scale - 1) / scale == dst. When scale > 1, samples each consumer block from the native
	// cell its centre lies in — matching AheadFlow.h RegridToConsumerGrid so aux.excluded and P8 texture dims align 1:1.
	// Returns false on any incompatible grid shape or block-size ratio (caller refuses with "camera labels: grid mismatch").
	bool PrepareComputePairInput(const FieldCopy& field, const CameraModel::BlockData& blocks, const CameraModel::Rect& picture,
		CameraModel::PairInput& in, std::vector<float>& regridX, std::vector<float>& regridY, std::vector<float>& regridTrust)
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

	CameraLabelProducer::CameraLabelProducer(pl_log log, pl_gpu gpu, LabelMode mode, int ringSlots, int liveSlots,
		const CameraModel::Config& fitConfig, bool hostReadable)
		: m_log(log), m_gpu(gpu), m_mode(mode), m_cfg(fitConfig), m_readable(hostReadable),
		m_ring(static_cast<size_t>(ringSlots > 0 ? ringSlots : 1), nullptr),
		m_live(static_cast<size_t>(liveSlots > 0 ? liveSlots : 1), nullptr)
	{
		if (mode == LabelMode::Compute && gpu)
			m_pass.reset(new CameraModel::LabelPass(log, gpu, hostReadable));
	}

	CameraLabelProducer::~CameraLabelProducer()
	{
		Release();
	}

	void CameraLabelProducer::Release()
	{
		VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserCameraModel);
		m_declaredVram = 0;
		if (m_gpu)
		{
			for (pl_tex& t : m_ring)
				pl_tex_destroy(m_gpu, &t);
			for (pl_tex& t : m_live)
				pl_tex_destroy(m_gpu, &t);
			pl_tex_destroy(m_gpu, &m_fieldTex);
			pl_tex_destroy(m_gpu, &m_auxTex);
		}
		m_pass.reset();
		m_gpu = nullptr;
	}

	uint64_t CameraLabelProducer::HeldVramBytes() const
	{
		uint64_t bytes = 0;
		for (pl_tex t : m_ring)
			bytes += VpSkeleton::TexBytesOf(t);
		for (pl_tex t : m_live)
			bytes += VpSkeleton::TexBytesOf(t);
		bytes += VpSkeleton::TexBytesOf(m_fieldTex);
		bytes += VpSkeleton::TexBytesOf(m_auxTex);
		return bytes;
	}

	void CameraLabelProducer::UpdateVramDeclaration()
	{
		const uint64_t held = HeldVramBytes();
		if (held == m_declaredVram)
			return;
		if (held > 0)
		{
			VpSkeleton::VramUsers::Instance().MarkActive(this, VpSkeleton::kUserCameraModel, true);
			VpSkeleton::VramUsers::Instance().DeclareFixed(this, VpSkeleton::kUserCameraModel, held);
		}
		else
		{
			VpSkeleton::VramUsers::Instance().Release(this, VpSkeleton::kUserCameraModel);
		}
		m_declaredVram = held;
	}

	pl_tex* CameraLabelProducer::SlotOf(LabelTarget target)
	{
		std::vector<pl_tex>& v = target.live ? m_live : m_ring;
		if (target.index < 0 || static_cast<size_t>(target.index) >= v.size())
			return nullptr;
		return &v[static_cast<size_t>(target.index)];
	}

	pl_tex CameraLabelProducer::TextureOf(LabelTarget target) const
	{
		const std::vector<pl_tex>& v = target.live ? m_live : m_ring;
		if (target.index < 0 || static_cast<size_t>(target.index) >= v.size())
			return nullptr;
		return v[static_cast<size_t>(target.index)];
	}

	bool CameraLabelProducer::EnsureSlot(pl_tex* tex, int w, int h)
	{
		if (!m_gpu)
		{
			m_error = "no GPU device";
			return false;
		}
		pl_fmt fmt = pl_find_fmt(m_gpu, PL_FMT_FLOAT, 4, 32, 32,
			static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_BLITTABLE));
		if (!fmt)   // upload mode needs no blit
			fmt = pl_find_fmt(m_gpu, PL_FMT_FLOAT, 4, 32, 32, static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE));
		if (!fmt)
		{
			m_error = "no sampleable host-writable rgba32f format";
			return false;
		}
		pl_tex_params p{};
		p.w = w;
		p.h = h;
		p.format = fmt;
		p.sampleable = true;
		p.host_writable = true;
		p.blit_dst = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
		p.host_readable = m_readable && (fmt->caps & PL_FMT_CAP_HOST_READABLE);
		const bool ok = pl_tex_recreate(m_gpu, tex, &p);
		UpdateVramDeclaration();
		if (!ok)
		{
			m_error = "cannot create the P8 texture";
			return false;
		}
		return true;
	}

	bool CameraLabelProducer::Upload(pl_tex* tex, const std::vector<float>& texels, int w, int h)
	{
		if (!EnsureSlot(tex, w, h))
			return false;
		pl_tex_transfer_params up{};
		up.tex = *tex;
		up.ptr = const_cast<float*>(texels.data());
		if (!pl_tex_upload(m_gpu, &up))
		{
			m_error = "P8 upload failed";
			return false;
		}
		return true;
	}

	bool CameraLabelProducer::ProduceUpload(const CameraModel::CameraMotion& rec, pl_tex* slot)
	{
		if (!rec.blocks || !PackLabelTexels(*rec.blocks, m_scratch))
		{
			m_error = "upload mode needs the CPU labels of the record";
			return false;
		}
		return Upload(slot, m_scratch, rec.blocks->gridW, rec.blocks->gridH);
	}

	bool CameraLabelProducer::ProduceCompute(const CameraModel::CameraMotion& rec, const FieldCopy* field, pl_tex* slot)
	{
		if (!field || !field->Valid() || field->fwdY.size() != field->Blocks() || !rec.blocks)
		{
			m_error = "compute mode needs the field copy";
			return false;
		}
		if (field->fwdTrust.size() != field->Blocks())
		{
			m_error = "field copy has no trust plane";
			return false;
		}
		CameraModel::PairInput in;
		if (!PrepareComputePairInput(*field, *rec.blocks, rec.picture, in, m_regridX, m_regridY, m_regridTrust))
		{
			++m_gridMismatches;
			m_error = "camera labels: grid mismatch";
			return false;
		}
		if (!m_pass)
		{
			m_error = "compute mode needs the field copy";
			return false;
		}
		if (in.fwdX == m_regridX.data())
			++m_regridded;
		const CameraModel::LabelConstants k = CameraModel::MakeLabelConstants(rec, m_cfg, in);
		CameraModel::PackLabelInputs(in, *rec.blocks, m_scratch, m_scratchAux);
		if (!Upload(&m_fieldTex, m_scratch, in.gridW, in.gridH) || !Upload(&m_auxTex, m_scratchAux, in.gridW, in.gridH))
			return false;
		if (!m_pass->Run(k, m_fieldTex, m_auxTex, m_cfg.overlayZeroClusterMin))
		{
			m_error = m_pass->Error();
			return false;
		}
		if (!EnsureSlot(slot, in.gridW, in.gridH))
			return false;
		if (!m_pass->Output() || !m_pass->Output()->params.blit_src || !(*slot)->params.blit_dst)
		{
			m_error = "compute mode: the pass output or the P8 slot is not blittable on this device";
			return false;
		}
		pl_tex_blit_params blit{};
		blit.src = m_pass->Output();
		blit.dst = *slot;
		blit.src_rc.x0 = 0; blit.src_rc.y0 = 0; blit.src_rc.x1 = in.gridW; blit.src_rc.y1 = in.gridH;   // pl_rect3d: z left default
		blit.dst_rc = blit.src_rc;
		blit.sample_mode = PL_TEX_SAMPLE_NEAREST;
		pl_tex_blit(m_gpu, &blit);
		return true;
	}

	bool CameraLabelProducer::Produce(const CameraModel::CameraMotion& rec, const FieldCopy* field, LabelTarget target)
	{
		m_error.clear();
		pl_tex* slot = SlotOf(target);
		if (!slot)
		{
			m_error = "no such P8 slot";
			return false;
		}
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = m_mode == LabelMode::Upload ? ProduceUpload(rec, slot) : ProduceCompute(rec, field, slot);
		const uint32_t us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - t0).count());
		m_costUs = m_costUs == 0 ? us : (m_costUs * 3u + us) / 4u;
		return ok;
	}
}
