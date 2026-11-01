// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// output.dither / dither need map on the GPU (libplacebo wrapper, header-only, namespace DitherNeed::Gpu).
//
//   BlockPass  one compute pass over the region block grid (DitherNeedGlsl.h: BlockPassHeaderGlsl / BlockPassBodyGlsl): reads
//              the grain split's block texture, the region precursors (+ aux) and the two light-level tables, applies the law
//              and the temporal state, and writes the per-block extra amplitude. It owns two tiny rgba32f block-grid textures
//              used as ping-pong (previous / new state): THE PRODUCT ITSELF is the newest of them (Current(), e in .x).
//              The flags come from DitherNeed::StateTracker::Decide (the same decision MapT takes; no CPU planes needed): reset
//              (no valid state, discontinuity, grid / LUT / colour depth / aBase change) and off (missing input, not needed,
//              setting off). BlockPass::Step does Decide + input validation + the pass; a skipped, missing or failed frame
//              Invalidate()s the product (Current() is null, the guard binds nothing, the sigma-delta sees no map).
//   HookGuard  binds the product to the sigma-delta hook(s) of a frame: dither_need = 1, the output-pixel -> block-grid mapping
//              (dn_scale_x/y, dn_off_x/y, dn_block) and the texture VPDN, through CustomHookChain::SetHookParameter and the
//              libplacebo fork's pl_mpv_user_shader_set_texture (patch 0004; PL_VP_MPV_USER_SHADER_SET_TEXTURE). Restored
//              (dither_need 0, VPDN unbound) by Release() / the destructor. Without the fork call Apply() sets nothing.
//
// VRAM cost (the whole product): state = 2 x blocks x 16 bytes (two rgba32f block-grid textures). 1080p at block 16:
// 120 x 68 = 8160 blocks = 261 KB; 4K at block 32 is smaller than that. Negligible beside the grain split (32 B per
// pixel). Cost of the pass: one thread per block, about 30 texel reads and two LUT fetch pairs, far below 0.05 ms (estimate).
// Threading: render thread only.
#pragma once

#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/output/dither/DitherNeed.h>
#include <VP-V/modules/output/dither/DitherNeedGlsl.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <algorithm>
#include <string>
#include <vector>

namespace DitherNeed
{
	namespace Gpu
	{
		// Everything one block pass needs. The textures are the SHARED products (not owned here).
		struct Args
		{
			pl_tex gsBlocks = nullptr;                                         // grain split blocks (N in .x), grid size
			pl_tex precursors[RegionEvidence::kPrecursorTextures] = {};        // s0 .. s4 and aux, grid size
			pl_tex lut = nullptr;                                              // light_level_lut, 1024 x 1
			pl_tex lutOut = nullptr;                                           // light_level_out, 1024 x 1
			int gridW = 0, gridH = 0;                                          // the block grid
			int blockPx = 16;                                                  // block edge in source pixels (re_block)
			RegionEvidence::AccessorUniforms derive;                           // RegionEvidence::Uniforms(DeriveParams)
			Params params;
			double qOut = 4.0;                                                 // DitherNeed::QOut(colorBits)
			double alpha = 1.0;                                                // DitherNeed::AlphaFor(dt, tau)
			bool reset = true;                                                 // e = target for every block
			bool off = false;                                                  // skipped frame: Run() invalidates and returns
		};

		// Internal linkage, as SigmaDeltaVectors: the bodies depend on which libplacebo the translation unit sees.
		namespace
		{
			inline bool Supported()
			{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
				return true;
#else
				return false;
#endif
			}
		}

		class BlockPass
		{
		public:
			BlockPass() = default;
			~BlockPass() = default;   // textures belong to the gpu: Release
			BlockPass(const BlockPass&) = delete;
			BlockPass& operator=(const BlockPass&) = delete;

			// Tests read the state back.
			void SetHostReadable(bool on) { m_hostReadable = on; }

			// Input validation (the shared textures must fit the block grid; the LUT tables are 1024 wide): the DitherNeed::Missing bits.
			static unsigned Validate(const Args& a)
			{
				unsigned m = 0;
				auto size = [](pl_tex t, int w, int h) { return t && t->params.w == w && t->params.h == h; };
				if (!a.gsBlocks) m |= kNoGrain;
				else if (!size(a.gsBlocks, a.gridW, a.gridH)) m |= kGrainGridMismatch;
				bool havePre = true, preFit = true;
				for (pl_tex t : a.precursors)
				{
					if (!t) havePre = false;
					else if (!size(t, a.gridW, a.gridH)) preFit = false;
				}
				if (!havePre) m |= kNoPrecursors;
				else if (!preFit) m |= kEvidenceGridMismatch;
				if (!a.lut || !a.lutOut) m |= kNoLut;
				else if (!size(a.lut, LightLevel::kLutSize, 1) || !size(a.lutOut, LightLevel::kLutSize, 1)) m |= kLutSizeMismatch;
				return m;
			}

			struct StepResult
			{
				Decision decision;       // from the shared StateTracker (status text included)
				bool ran = false;        // the block pass ran: Current() is the product
				unsigned missing = 0;    // decision.missing: the caller's bits + the validation + kPassFailed
			};

			// One frame: Decide (the caller's `fa.missing`, e.g. no LUT / unsupported domain / no set_texture, plus the GPU
			// input validation), then the pass when On. Every other outcome Invalidate()s the product. The block-grid geometry
			// of `fa` and `a` must agree (fa.grid.gw/gh = a.gridW/gridH). a.reset / a.off are set from the decision.
			// The pass values (params, qOut, alpha) are taken from `fa`, the one copy of the frame's values; a block grid that
			// `fa` and `a` disagree on is a grain-grid mismatch. requireBind (default true): the product is useless without the
			// fork's set_texture, so the frame reports kNoSetTexture; pass false to exercise the block pass alone.
			StepResult Step(pl_gpu gpu, pl_dispatch dispatch, Args a, FrameArgs fa, bool requireBind = true)
			{
				StepResult r;
				if (fa.enabled)   // a missing product must read Unavailable, not NotNeeded, exactly as on the CPU
				{
					fa.missing |= Validate(a);
					if (fa.grid.gw != a.gridW || fa.grid.gh != a.gridH) fa.missing |= kGrainGridMismatch;
					if (requireBind && !Supported()) fa.missing |= kNoSetTexture;
				}
				a.params = fa.params;
				a.qOut = QOut(fa.colorBits);
				a.alpha = AlphaFor(fa.dtSeconds, fa.params.tauSeconds);
				r.decision = m_tracker.Decide(fa);
				r.missing = r.decision.missing;
				if (r.decision.off) { Invalidate(); return r; }
				a.reset = r.decision.reset;
				a.off = false;
				if (!Run(gpu, dispatch, a))
				{
					m_tracker.Invalidate();
					Invalidate();
					fa.missing |= kPassFailed;
					r.decision.result = Result::Unavailable;
					r.decision.off = true;
					r.decision.reset = false;
					r.decision.missing = fa.missing;
					r.decision.status = StatusOf(true, fa.missing, true);
					r.missing = fa.missing;
					return r;
				}
				r.ran = true;
				return r;
			}

			// A skipped frame (setting off, not needed, an input missing, a failed pass): nothing is bound, the next pass resets.
			void Invalidate() { m_valid = false; m_tracker.Invalidate(); }

			// One pass. A (re)created state texture forces reset (its contents are undefined) and clears the valid flag; the
			// flag is set again only by a successful pass; a failed pass invalidates. a.off = Invalidate and return true.
			bool Run(pl_gpu gpu, pl_dispatch dispatch, const Args& a)
			{
				using namespace VpvGpu;
				m_error.clear();
				if (a.off) { Invalidate(); return true; }
				if (!gpu || !dispatch || !a.gsBlocks || !a.lut || !a.lutOut || a.gridW < 1 || a.gridH < 1 || a.blockPx < 1)
				{
					Invalidate();
					m_error = "dither need: invalid input";
					return false;
				}
				for (pl_tex t : a.precursors)
					if (!t) { Invalidate(); m_error = "dither need: missing precursor texture"; return false; }
				if (const unsigned bad = Validate(a))
				{
					Invalidate();
					m_error = "dither need: " + MissingText(bad);
					return false;
				}
				const pl_fmt f4 = FloatFormat(gpu, 4);
				bool fresh = !m_valid;
				for (int i = 0; i < 2; ++i)
				{
					pl_tex before = m_tex[i];
					if (!EnsureState(gpu, m_tex[i], a.gridW, a.gridH, f4))
					{
						Invalidate();
						m_error = "dither need: no rgba32f storage texture";
						return false;
					}
					if (m_tex[i] != before) { fresh = true; m_valid = false; }
				}
				const int prev = m_cur, next = 1 - m_cur;
				const int reset = (a.reset || fresh) ? 1 : 0, off = 0;
				const int grid[2] = { a.gridW, a.gridH };
				const int block = a.blockPx;
				const float p0[4] = { static_cast<float>(a.params.aBase), static_cast<float>(a.params.aNeed),
					static_cast<float>(a.params.aMax), static_cast<float>(a.params.deadband) };
				const float p1[2] = { static_cast<float>(a.params.visLo), static_cast<float>(a.params.visHi) };
				const float qout = static_cast<float>(a.qOut), alpha = static_cast<float>(a.alpha);
				const pl_shader_var vars[11] = {
					{ pl_var_ivec2("re_grid"), grid, true }, { pl_var_int("re_block"), &block, true },
					{ pl_var_vec4("re_d0"), a.derive.d0, true }, { pl_var_vec4("re_d1"), a.derive.d1, true },
					{ pl_var_vec4("re_d2"), a.derive.d2, true }, { pl_var_vec4("dn_p0"), p0, true },
					{ pl_var_vec2("dn_p1"), p1, true }, { pl_var_float("dn_qout"), &qout, true },
					{ pl_var_float("dn_alpha"), &alpha, true }, { pl_var_int("dn_reset"), &reset, true },
					{ pl_var_int("dn_off"), &off, true } };
				const pl_shader_desc desc[11] = { Sampled("dn_gs", a.gsBlocks), Sampled("re_s0", a.precursors[0]),
					Sampled("re_s1", a.precursors[1]), Sampled("re_s2", a.precursors[2]), Sampled("re_s3", a.precursors[3]),
					Sampled("re_s4", a.precursors[4]), Sampled("re_aux", a.precursors[5]), Sampled("light_level_lut", a.lut),
					Sampled("light_level_out", a.lutOut), Sampled("dn_prev", m_tex[prev]), Storage("dn_state", m_tex[next]) };
				const std::string header = BlockPassHeaderGlsl();
				const int gx = (a.gridW + 15) / 16, gy = (a.gridH + 15) / 16;
				if (!Pass(dispatch, "VideoProcessor dither need: block pass", header, BlockPassBodyGlsl(), desc, 11, vars, 11, gx, gy, 0))
				{
					Invalidate();
					m_error = "dither need: block pass failed";
					return false;
				}
				m_cur = next;
				m_valid = true;
				m_w = a.gridW; m_h = a.gridH;
				return true;
			}

			// The newest state (e in .x), valid after a successful Run until the next Run or Release; null before the first.
			pl_tex Current() const { return m_valid ? m_tex[m_cur] : nullptr; }
			int GridW() const { return m_w; }
			int GridH() const { return m_h; }
			// 2 x blocks x 16 bytes.
			size_t VramBytes() const { return m_valid ? static_cast<size_t>(2) * m_w * m_h * 16 : 0; }
			const std::string& LastError() const { return m_error; }
			const StateTracker& Tracker() const { return m_tracker; }

			void Release(pl_gpu gpu)
			{
				if (gpu)
					for (pl_tex& t : m_tex)
						if (t) pl_tex_destroy(gpu, &t);
				m_tex[0] = m_tex[1] = nullptr;
				m_valid = false;
				m_cur = 0;
				m_w = m_h = 0;
				m_tracker.Invalidate();
			}

		private:
			bool EnsureState(pl_gpu gpu, pl_tex& tex, int w, int h, pl_fmt fmt) const
			{
				pl_tex_params tp{};
				tp.w = w;
				tp.h = h;
				tp.format = fmt;
				tp.sampleable = true;
				tp.storable = true;
				tp.host_readable = m_hostReadable;
				return fmt && pl_tex_recreate(gpu, &tex, &tp);
			}

			pl_tex m_tex[2] = { nullptr, nullptr };
			int m_cur = 0, m_w = 0, m_h = 0;
			bool m_valid = false, m_hostReadable = false;
			std::string m_error;
			StateTracker m_tracker;
		};

		// Output pixel -> block grid mapping: src = (pos + 0.5) * scale + off (source pixels), block edge in source pixels
		// (DitherNeed::SampleExtra's arguments).
		struct Placement
		{
			float scaleX = 1.0f, scaleY = 1.0f, offX = 0.0f, offY = 0.0f;
			int blockPx = 16;
		};

		// Internal linkage, as SigmaDeltaVectors: the bodies depend on which libplacebo the translation unit sees.
		namespace
		{
			inline bool SetState(const pl_hook* hook, pl_tex state)
			{
#ifdef PL_VP_MPV_USER_SHADER_SET_TEXTURE
				return hook && pl_mpv_user_shader_set_texture(hook, "VPDN", state);
#else
				(void)hook;
				(void)state;
				return false;
#endif
			}

			// Sets dither_need 1, the mapping and VPDN on every sigma-delta hook of the frame; a hook that does not take the
			// texture goes back to dither_need 0. Restored when released or destroyed (i.e. after this render).
			class HookGuard
			{
			public:
				HookGuard() = default;
				HookGuard(const HookGuard&) = delete;
				HookGuard& operator=(const HookGuard&) = delete;
				~HookGuard() { Release(); }

				// True when any hook took the product. Without the fork call (7.360) nothing is set and false is returned
				// (the caller reports DitherNeed::kNoSetTexture / kPassFailed from MissingAfterApply). A null state binds nothing.
				bool Apply(const pl_hook* const* hooks, int count, pl_tex state, const Placement& p)
				{
					if (!Supported() || !state || !hooks) return false;
					for (int i = 0; i < count; ++i)
					{
						const pl_hook* hook = hooks[i];
						if (!hook || !CustomHookChain::SetHookParameter(hook, "dither_need", 1.0)) continue;
						CustomHookChain::SetHookParameter(hook, "dn_scale_x", p.scaleX);
						CustomHookChain::SetHookParameter(hook, "dn_scale_y", p.scaleY);
						CustomHookChain::SetHookParameter(hook, "dn_off_x", p.offX);
						CustomHookChain::SetHookParameter(hook, "dn_off_y", p.offY);
						CustomHookChain::SetHookParameter(hook, "dn_block", static_cast<double>(p.blockPx));
						if (SetState(hook, state))
							m_applied.push_back(hook);
						else
							CustomHookChain::SetHookParameter(hook, "dither_need", 0.0);
					}
					return !m_applied.empty();
				}

				// The Missing bit to report when Apply returned false for a non-null state.
				static unsigned MissingAfterApply() { return Supported() ? kPassFailed : kNoSetTexture; }

				void Release()
				{
					for (const pl_hook* hook : m_applied)
					{
						CustomHookChain::SetHookParameter(hook, "dither_need", 0.0);
						SetState(hook, nullptr);
					}
					m_applied.clear();
				}

			private:
				std::vector<const pl_hook*> m_applied;
			};
		}
	}
}
