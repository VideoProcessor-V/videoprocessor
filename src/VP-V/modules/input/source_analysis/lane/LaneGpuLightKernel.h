// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// 70b G4: the GPU kernel of P12 (light level) for the skeleton's render-thread dispatcher (modules/control/skeleton/gpu/
// SkeletonGpuDispatcher.h). One compute pass, one thread per block: LightLevel::Blocks::LightLevelBlocksPassGlsl() (the
// shader of the CPU twin LightLevelBlocks.h). Inputs: P11's grids S0 / S3 / S4 (terms 0, 3, 4 of its layout), the product's
// own previous frame (history 1, the one-pole state) and the 1024 x 1 rgba32f light-level LUT, which is part of the
// product: built on the CPU (LightLevel::Build / PackTexels) at Prepare() / the first Step after SetSettings(), uploaded
// by this kernel, freed in Release(). The product is one rgba32f grid (J_surround, J_local, V, conf) at the block edge
// RegionEvidence::BlockSizeFor(height), 16 B per cell: the declaration of P12 (core/Products.h).
//
// The previous frame is used (ll_reset 0) only when the dispatcher found P12(N-1) (ownPrev*), it is not the output slot,
// and this kernel itself made N-1 in the full mode since the last Prepare(); anything else resets the one-pole (ll_reset 1,
// the CPU twin's First / NoHistory / Context reset).
//
// The frame-level value of the shader (ll_frame_j: the window fallback of blocks without picture support) is the J of
// P4's mean picture code through the LUT; the CPU twin takes the coverage-weighted mean of P11's J_local instead, which
// would need a read-back of the grid. It only reaches blocks with no picture inside their window (conf 0 there).
//
// Visible downgrades (Fallbacks(), never log-only): "P12 frame-level" (P11 has no content for the frame: every block gets
// one level from P4's APL, conf = Params::frameLevelConf; without the APL zeros, i.e. code only), "display facts: defaults"
// (the LUT is built from the default settings and an SDR context until SetSettings supplies them) and "P12 smoothing
// reset" (the previous frame was not available although this kernel had produced frames).

#include <VP-V/modules/input/source_analysis/lane/LaneGpuProducts.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelLut.h>
#include <VP-V/modules/input/source_analysis/light_level/LightLevelSettings.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VpLaneProducts
{
	// ---- the P12 begin gate (70b section 3) ---------------------------------------------------------------------------
	// Lane P12(N) waits while P12(N-1) is genuinely in progress (it is the one-pole state): Pending. Ring and Live are readable;
	// None (never made / shed) and Failed will not become readable by waiting, so P12(N) starts without a previous frame and the
	// kernel resets the one-pole (ll_reset = 1, shown as light_level_smoothing). The integrator maps AnalysisLane::ProductRead::State onto this enum.
	enum class LightPrevState : uint8_t { None, Pending, Failed, Ring, Live };

	inline bool LightPrevReadable(LightPrevState s) { return s == LightPrevState::Ring || s == LightPrevState::Live; }
	// Only a previous frame still in progress holds the next one back.
	inline bool LightPrevWorthWaitingFor(LightPrevState s) { return s == LightPrevState::Pending; }

	// True = P12(seq) may start. The first frame (seq 0: no previous) is always allowed (the kernel resets). `stateOf(seq - 1)`
	// is the read state of P12(seq - 1) (the integrator's ReadOf call, render thread). `why` (optional) gets the reason text
	// when the answer is false.
	inline bool LightLevelBeginGate(uint64_t seq, const std::function<LightPrevState(uint64_t)>& stateOf, std::string* why = nullptr)
	{
		if (seq == 0)
			return true;
		if (!stateOf || !LightPrevWorthWaitingFor(stateOf(seq - 1)))
			return true;
		if (why != nullptr)
			*why = "waits for P12(N-1)";
		return false;
	}

	class LightLevelKernel : public IProductKernel
	{
	public:
		VpSkeleton::ProductId Product() const override { return VpSkeleton::kP12LightLevel; }
		// One absolute rgba32f term at shift 4 (bw x bh): the declaration's 16 B per cell (ProductTexLayout::MatchesDecl).
		VpSkeleton::ProductTexLayout Layout(VpSkeleton::Dims dims) const override;
		uint32_t Steps(VpSkeleton::Dims dims) const override { return StepsFor(Product(), dims); }
		bool NeedsTap() const override { return false; }
		// Builds the LUT (CPU) and uploads it; forgets the frame history (a size or settings change resets the one-pole).
		bool Prepare(pl_gpu gpu, pl_dispatch dispatch, VpSkeleton::Dims dims) override;
		bool Step(KernelContext& c, uint32_t step) override;
		void Release(pl_gpu gpu) override;
		std::string LastError() const override { return m_error; }
		std::vector<FallbackCatalog::Decision> Fallbacks() const override;

		// The light-level settings and display context the LUT is built from; bumps the generation (the next Prepare / Step
		// rebuilds and re-uploads the LUT and resets the one-pole). Until called, the defaults and an SDR context are used.
		void SetSettings(const LightLevel::Settings& settings, const LightLevel::Context& context);
		// The measured source frame rate (Hz) of the one-pole's per-frame weight (0 = unknown: the 60 Hz reference weight).
		void SetFrameRate(double hz) { m_frameRateHz = hz; }
		// The frame confidence gate (1 = P11 was made for this LUT, 0 = another display context). The GPU P11 form carries
		// no LUT fingerprint, so the integrator sets it when it knows better. Default 1.
		void SetFrameGate(float gate) { m_gate = gate; }

		uint32_t Generation() const { return m_generation; }
		const std::string& LutFingerprint() const { return m_fingerprint; }
		// The last Step: the full per-block mode (false = frame level / code only), whether the previous frame was used.
		bool LastFull() const { return m_lastFull; }
		bool LastUsedPrev() const { return m_lastUsedPrev; }
		bool DisplayFactsAreDefaults() const { return m_defaultFacts; }

		// The pass GLSL (header and body) and its FNV-1a 64 fingerprint (pinned by the tests).
		static std::string HeaderGlsl();
		static std::string BodyGlsl();
		static uint64_t PassFingerprint();

	private:
		bool Fail(KernelContext& c, const std::string& why);
		bool EnsureDummy(pl_gpu gpu);
		bool EnsureLut(pl_gpu gpu);
		bool FullRecorded(uint64_t seq) const;
		void Record(uint64_t seq, bool full);

		pl_tex m_dummy = nullptr;    // the S0 / S3 / S4 / previous-frame stand-in (1 x 1, never read in the modes that bind it)
		pl_tex m_lutTex = nullptr;
		LightLevel::Settings m_settings = LightLevel::Read([](const std::string&, std::string&) { return false; });
		LightLevel::Context m_context;
		std::vector<float> m_texels;
		std::string m_fingerprint;
		std::string m_header, m_body;
		std::string m_error;
		std::string m_frameWhy;
		double m_frameRateHz = 0.0;
		float m_gate = 1.0f;
		uint32_t m_generation = 1;
		uint32_t m_builtGeneration = 0;
		bool m_defaultFacts = true;
		bool m_lastFull = true;
		bool m_lastUsedPrev = false;
		bool m_resetUnexpected = false;
		std::vector<std::pair<uint64_t, bool>> m_history;   // (seq, made in the full mode), newest last, bounded
	};
}
