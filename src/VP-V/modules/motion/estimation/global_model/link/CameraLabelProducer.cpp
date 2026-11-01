// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "CameraLabelProducer.h"

#include <chrono>

namespace CameraLink
{
	CameraLabelProducer::CameraLabelProducer(pl_log log, pl_gpu gpu, LabelMode mode, int ringSlots, int liveSlots,
		const CameraModel::Config& fitConfig, bool hostReadable)
		: m_log(log), m_gpu(gpu), m_mode(mode), m_cfg(fitConfig), m_readable(hostReadable),
		m_ring(static_cast<size_t>(ringSlots > 0 ? ringSlots : 1), nullptr),
		m_live(static_cast<size_t>(liveSlots > 0 ? liveSlots : 1), nullptr)
	{
		if (mode == LabelMode::Compute)
			m_pass.reset(new CameraModel::LabelPass(log, gpu, hostReadable));
	}

	CameraLabelProducer::~CameraLabelProducer()
	{
		for (pl_tex& t : m_ring)
			pl_tex_destroy(m_gpu, &t);
		for (pl_tex& t : m_live)
			pl_tex_destroy(m_gpu, &t);
		pl_tex_destroy(m_gpu, &m_fieldTex);
		pl_tex_destroy(m_gpu, &m_auxTex);
		m_pass.reset();
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
		if (!pl_tex_recreate(m_gpu, tex, &p))
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
		if (!field || !field->Valid() || !rec.blocks || !m_pass)
		{
			m_error = "compute mode needs the field copy";
			return false;
		}
		CameraModel::PairInput in;
		in.gridW = field->gridW;
		in.gridH = field->gridH;
		in.blockSize = field->blockSize;
		in.picture = rec.picture;
		in.fwdX = field->fwdX.data();
		in.fwdY = field->fwdY.data();
		in.fwdTrust = field->fwdTrust.size() == field->Blocks() ? field->fwdTrust.data() : nullptr;
		if (!in.fwdTrust)
		{
			m_error = "field copy has no trust plane";
			return false;
		}
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
