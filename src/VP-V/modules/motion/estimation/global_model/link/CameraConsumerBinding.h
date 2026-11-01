// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link (K2-3): what ONE camera consumer (grain map, debander, ...) binds into its shader per frame:
//   * its Policy (PolicyFor + its own `enabled`) and its own BlendState, reset by the Indicator reader (BindBlendState: the
//     reader hook runs on the render thread, as the BlendState does, so this binder is thread-safe by construction);
//   * Update() runs at most ONCE per SOURCE frame: a cadence repeat re-presents the same source frame and must not advance
//     the rate limit;
//   * the AccessorConstants (MakeAccessorConstants), the P8 texture as the sampler "camera_model", the six uniforms named
//     exactly as CameraAccessorGlsl.h declares them, and a per-consumer #define that exists ONLY when the consumer is ENABLED
//     (policy), never depending on whether P8 is present this frame: with the camera off the shader text and its cache key are
//     unchanged, with it on they are constant. While P8 is absent a 1x1 dummy texture is bound with camera_enabled = 0;
//   * the DomainMap is the identity on the SOURCE plane (output mapping is K7's job).
// P8 absent (the labels view is not available) -> enabled = 0: today's values, with the reason counted for the status.
// Pure apart from the pl_tex handle it passes through. Render thread.
#pragma once

#include "CameraLink.h"

#include <VP-V/modules/motion/estimation/global_model/CameraAccessor.h>
#include <VP-V/modules/motion/estimation/global_model/CameraAccessorGlsl.h>
#include <VP-V/modules/motion/estimation/global_model/CameraReaders.h>

#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>

#include <cstdint>
#include <string>

namespace CameraLink
{
	// Names of the six uniforms in CameraAccessorGlsl.h, in declaration order.
	constexpr const char* kUniformEnabled = "camera_enabled";
	constexpr const char* kUniformWeight = "camera_weight";
	constexpr const char* kUniformPrefer = "camera_prefer";
	constexpr const char* kUniformAbsThr = "camera_abs_thr";
	constexpr const char* kUniformRelThr = "camera_rel_thr";
	constexpr const char* kUniformGrid = "camera_grid";
	constexpr const char* kSamplerName = "camera_model";

	// Identity map of source-plane coordinates (block grid space -> source px is the record's blockSize).
	struct DomainMap
	{
		double scaleX = 1.0, scaleY = 1.0, offsetX = 0.0, offsetY = 0.0;
		bool IsIdentity() const { return scaleX == 1.0 && scaleY == 1.0 && offsetX == 0.0 && offsetY == 0.0; }
	};

	// The values to hand to the shader this frame.
	struct CameraBinding
	{
		CameraModel::AccessorConstants constants;
		pl_tex tex = nullptr;               // null when constants.enabled == 0
		bool defineActive = false;          // add `defineName` to the shader text only when true
		std::string defineName;             // e.g. CAMERA_MODEL_GRAIN_MAP
		DomainMap domain;
		int labelsFloats[2] = { 0, 0 };     // grid (w, h) of the texture

		// pl_shader_desc for the sampled texture (nullptr tex -> a zeroed desc the caller must not bind).
		pl_shader_desc Desc() const
		{
			pl_shader_desc d{};
			d.desc.name = kSamplerName;
			d.desc.type = PL_DESC_SAMPLED_TEX;
			d.binding.object = tex;
			d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
			d.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
			return d;
		}
	};

	// Why a frame ran without the model (counted; the status line sums them).
	enum class BindOutcome : uint8_t { Off, Applied, NoLabels, NoRecord, RepeatedFrame };

	struct BindingStats
	{
		uint64_t updates = 0, applied = 0, noLabels = 0, noRecord = 0, repeats = 0;
	};

	class CameraConsumerBinding
	{
	public:
		CameraConsumerBinding(CameraConsumer consumer, bool enabled);

		// A 1x1 texture bound while P8 is absent, so the shader text, its cache key and the descriptor set never change per frame.
		void SetDummyTexture(pl_tex t) { m_dummy = t; }
		void SetEnabled(bool on) { m_policy.enabled = on; }
		bool Enabled() const { return m_policy.enabled; }
		const CameraModel::Policy& GetPolicy() const { return m_policy; }
		CameraModel::Policy& MutablePolicy() { return m_policy; }
		double Weight() const { return m_state.Weight(); }

		// The reader hooks (reset only: the blend state keeps no timing).
		CameraReaderHooks Hooks() { return BindBlendState(m_state); }

		// Once per frame, render thread. `sourceFrame` identifies the SOURCE frame being presented; the blend state advances
		// only when it differs from the previous call. record = the P7 record of that frame (sealed), labels = LabelsFor.
		BindOutcome Update(uint64_t sourceFrame, const CameraModel::CameraMotion* record, const CameraLabelsView& labels);

		const CameraBinding& Current() const { return m_binding; }
		const BindingStats& Stats() const { return m_stats; }
		const char* DefineName() const { return m_defineName.c_str(); }

	private:
		CameraConsumer m_consumer;
		CameraModel::Policy m_policy;
		CameraModel::BlendState m_state;
		pl_tex m_dummy = nullptr;
		CameraBinding m_binding;
		BindingStats m_stats;
		std::string m_defineName;
		bool m_haveFrame = false;
		uint64_t m_lastSourceFrame = 0;
	};
}
