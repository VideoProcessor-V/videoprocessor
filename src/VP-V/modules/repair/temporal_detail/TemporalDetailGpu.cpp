// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// TemporalDetail GPU dispatch (TemporalDetailGpu.h). Passes per new frame, in order (the model's steps in brackets):
//   grain split (1) -> MEAS (2) -> COMPOSE (5) -> EVAL (6) -> TRACK (3, pass 1) -> ADMIT (4) -> CLEAN x 4 parity
//   classes (4a) -> FINISH + BASECOPY + COPYBLK (4: the new slot) -> WEIGHT (7, pass 2) -> PROJ (8: projections) ->
//   TRANSFER (8) -> GUARD_SUMS / GUARD_EMA / GUARD_POOL (10) -> STATS -> APPLY (9, 10a).
// The block state ping-pongs between two textures; four state passes per frame leave it in set 0.
#include <pch.h>

#include <VP-V/modules/repair/temporal_detail/TemporalDetailGpu.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailModel.h>
#include <VP-V/modules/repair/temporal_detail/TemporalDetailShaders.h>
#include <VP-V/modules/repair/grain_split/GpuPassUtil.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace TemporalDetail
{
	namespace Gpu
	{
		namespace
		{
			constexpr int kMaxK = kSlots;

			struct Geometry
			{
				int w = 0, h = 0, B = 16, bw = 0, bh = 0;
				double scale = 1.0;
				int P = 8, half = 4, npx = 0, npy = 0;
			};

			Geometry MakeGeometry(int w, int h, double planeLines)
			{
				Geometry g;
				g.w = w; g.h = h;
				g.scale = Scale(planeLines > 0.0 ? planeLines : h);
				g.B = (std::max)(1, static_cast<int>(std::floor(kBlockPx * g.scale + 0.5)));
				g.bw = (w + g.B - 1) / g.B;
				g.bh = (h + g.B - 1) / g.B;
				g.P = (std::max)(4, 2 * static_cast<int>(std::floor(0.5 * kCleanPatchPx * g.scale + 0.5)));
				g.half = g.P / 2;
				g.npx = (w + g.half + g.half - 1) / g.half;
				g.npy = (h + g.half + g.half - 1) / g.half;
				return g;
			}

			// Atlas columns for K slots of w x h within maxDim with the fewest cells (0: does not fit).
			int AtlasCols(int K, int w, int h, int maxDim)
			{
				int best = 0, bestCells = 0;
				for (int c = 1; c <= K; ++c)
				{
					const int r = (K + c - 1) / c;
					if (static_cast<long long>(c) * w > maxDim || static_cast<long long>(r) * h > maxDim) continue;
					if (!best || c * r < bestCells) { best = c; bestCells = c * r; }
				}
				return best;
			}

			// The slot atlas: slot k's planes at ((k % cols) w, (k / cols) h) of one texture (no per-slot texture branching).
			std::string SlotFunctions(bool sampled, bool storePix, bool storeBase)
			{
				std::string s = "ivec2 td_slot_org(int k) { return ivec2((k % TD_ACOLS) * TD_W, (k / TD_ACOLS) * TD_H); }\n";
				if (sampled)
				{
					s += "vec4 td_slot_pix(int k, ivec2 q) { return texelFetch(slots, q + td_slot_org(k), 0); }\n";
					s += "float td_slot_base(int k, ivec2 q) { return texelFetch(sbases, q + td_slot_org(k), 0).x * TD_SB_LOAD; }\n";
				}
				if (storePix) s += "void td_slot_store(int k, ivec2 p, vec4 v) { imageStore(slots_out, p + td_slot_org(k), v); }\n";
				if (storeBase) s += "void td_base_store(int k, ivec2 p, vec4 v) { imageStore(sbases_out, p + td_slot_org(k), v); }\n";
				return s;
			}
		}

		struct Module::Impl
		{
			GrainSplit::Gpu::Splitter split;
			pl_tex meas = nullptr, slotBlk = nullptr, evA = nullptr, evB = nullptr, work = nullptr, coef = nullptr, cw = nullptr,
				cf = nullptr, ct = nullptr, delta = nullptr, gsum = nullptr, kback = nullptr, gflag = nullptr, patches = nullptr,
				prevg = nullptr, out = nullptr, crop = nullptr;
			pl_tex atlasPix = nullptr, atlasBase = nullptr, mot[2] = {}, tab[2] = {}, state[2] = {}, counters[2] = {}, acc[2] = {};
			int motCur = 0, tabCur = 0, cntCur = 0;
			bool haveFrame = false, havePrevG = false, forget = false, haveSet = false, countersReady = false;
			uint64_t prevSeq = 0;
			StampView stamps;
			Settings lastSet;
			Geometry geo;
			ActiveRect rect;
			bool baseUnorm = true;
			int slots = 0;
		};

		Module::~Module() { delete m_impl; }

		void Module::Forget()
		{
			if (m_impl) m_impl->forget = true;
		}

		void Module::Release(pl_gpu gpu)
		{
			if (m_impl && gpu)
			{
				Impl& m = *m_impl;
				m.split.Release(gpu);
				for (pl_tex* t : { &m.meas, &m.slotBlk, &m.evA, &m.evB, &m.work, &m.coef, &m.cw, &m.cf, &m.ct, &m.delta, &m.gsum,
					&m.kback, &m.gflag, &m.patches, &m.prevg, &m.out, &m.crop })
					if (*t) pl_tex_destroy(gpu, t);
				if (m.atlasPix) pl_tex_destroy(gpu, &m.atlasPix);
				if (m.atlasBase) pl_tex_destroy(gpu, &m.atlasBase);
				for (int i = 0; i < 2; ++i)
					for (pl_tex* t : { &m.mot[i], &m.tab[i], &m.state[i], &m.counters[i], &m.acc[i] })
						if (*t) pl_tex_destroy(gpu, t);
			}
			delete m_impl;
			m_impl = nullptr;
		}

		uint64_t Module::TextureBytes(int w, int h, double planeLines, int slots)
		{
			const int K = (std::max)(1, (std::min)(kMaxK, slots));
			const Geometry g = MakeGeometry(w, h, planeLines);
			const uint64_t px = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
			const uint64_t nb = static_cast<uint64_t>(g.bw) * static_cast<uint64_t>(g.bh);
			uint64_t b = 0;
			b += 2 * 16 * px;                             // grain split: rows/split + bands (rgba32f)
			const int cols = (std::max)(1, AtlasCols(K, w, h, 16384));
			b += static_cast<uint64_t>(cols * ((K + cols - 1) / cols)) * (8 + 2) * px;   // slot atlas: rgba16f bands + r16 base
			b += 2 * 8 * px;                              // cleaner accumulators (rg32f)
			b += 8 * px + 8 * px + 2 * px + 2 * px;       // delta, prevg (rg32f), output and active-picture crop (16-bit; bars excluded)
			b += nb * 16 * (2 + 1 + 2 * K + 2 * K + 16 + 1 + 3 + 2 + 1) + nb * 4 * (K + 1);   // block grids
			b += static_cast<uint64_t>(g.npx) * static_cast<uint64_t>(g.npy) * 8;          // patch statistics
			return b;
		}

		int Module::SlotsForBudget(uint64_t budgetBytes, int w, int h, double planeLines)
		{
			for (int k = kMaxK; k > 1; --k)
				if (TextureBytes(w, h, planeLines, k) <= budgetBytes) return k;
			return 1;
		}

		bool Module::ReadCounters(pl_gpu gpu, Counters& out)
		{
			out = Counters();
			if (!m_impl || !gpu || !m_impl->countersReady) return false;
			pl_tex src = m_impl->counters[m_impl->cntCur];
			if (!src) return false;
			pl_tex_params rp{};
			rp.w = src->params.w; rp.h = 1; rp.format = src->params.format; rp.host_readable = true; rp.blit_dst = true;
			pl_tex readable = pl_tex_create(gpu, &rp);
			if (!readable) return false;
			pl_tex_blit_params bp{};
			bp.src = src; bp.dst = readable;
			pl_tex_blit(gpu, &bp);
			std::vector<float> v(static_cast<size_t>(rp.w) * src->params.format->num_components, 0.0f);
			pl_tex_transfer_params d{};
			d.tex = readable; d.ptr = v.data();
			const bool ok = pl_tex_download(gpu, &d);
			pl_tex_destroy(gpu, &readable);
			if (!ok) return false;
			const int nc = src->params.format->num_components;
			out.guardActive = static_cast<int>(v[0]);
			out.addBackBlocks = static_cast<int>(v[1]);
			out.guardTrips = static_cast<int>(v[2]);
			out.refreshes = static_cast<int>(v[3]);
			out.admitted = v[static_cast<size_t>(nc)] > 0.5f;
			out.liveSlots = static_cast<int>(v[static_cast<size_t>(nc) + 1]);
			return true;
		}

		bool Module::Apply(pl_gpu gpu, pl_dispatch dispatch, const pl_frame& source, const Params& params, pl_frame& result)
		{
			using namespace VpvGpu;
			m_error.clear();
			const Settings& set = params.settings;
			if (set.mode == Mode::Off)
			{
				// Off: the input itself, no pass; the store is untouched (the model's Off).
				m_status = "off";
				if (m_impl) { m_impl->lastSet = set; m_impl->haveSet = true; }   // switching on again is a mode change: reset
				result = source;
				return true;
			}
			if (set.mode == Mode::Auto && !params.autoEngaged)
			{
				// auto, idle: the input itself, no pass; the store restarts when it engages again.
				m_status = "auto: idle";
				if (m_impl) m_impl->haveFrame = false;
				result = source;
				return true;
			}
			if (!gpu || !dispatch || source.num_planes <= 0)
			{
				m_error = "temporal detail: nothing to process";
				++m_failures;
				return false;
			}
			const pl_tex luma = source.planes[0].texture;
			if (!luma || !luma->params.sampleable || !pl_color_system_is_ycbcr_like(source.repr.sys) ||
				source.planes[0].component_mapping[0] != PL_CHANNEL_Y)
			{
				m_error = "temporal detail: plane 0 is not a sampleable luma plane";
				++m_failures;
				return false;
			}
			if (!m_impl) m_impl = new Impl();
			Impl& m = *m_impl;
			// The reference-store tier (fewer slots on small cards; visible in the status). A change restarts the store.
			const int K = (std::max)(1, (std::min)(kMaxK, params.slots));
			if (K != m.slots)
			{
				m.haveFrame = false;
				m.slots = K;
			}
			// Everything runs on the active picture (its border is the plane edge); bars are copied unchanged.
			const int W = luma->params.w, H = luma->params.h;
			const ActiveRect r = params.active.Resolve(W, H);
			const bool whole = r.left == 0 && r.top == 0 && r.right == W && r.bottom == H;
			const int w = r.Width(), h = r.Height();
			const Geometry g = MakeGeometry(w, h, params.planeLines > 0.0 ? params.planeLines : H);
			const double hz = params.contentHz > 0.0 ? params.contentHz : kReferenceHz;

			// Settings: a mode or max_age change resets (the model's UpdateSettings).
			const bool settingsReset = m.haveSet && (set.mode != m.lastSet.mode || set.maxAgeSeconds != m.lastSet.maxAgeSeconds);
			if (settingsReset) m.haveFrame = false;
			m.lastSet = set;
			m.haveSet = true;
			if (m.forget) { m.haveFrame = false; m.forget = false; }

			// A repeat: the last output again, no state advanced.
			if (m.haveFrame && params.seq == m.prevSeq && w == m.geo.w && h == m.geo.h && r == m.rect && m.out && m.out->params.w == W && m.out->params.h == H)
			{
				pl_frame frame = source;
				frame.planes[0].texture = m.out;
				result = frame;
				return true;
			}

			const char* reason = nullptr;
			if (m.haveFrame)
			{
				if (w != m.geo.w || h != m.geo.h || g.B != m.geo.B || r != m.rect) reason = "geometry";
				else if (params.seq != m.prevSeq + 1) reason = "sequence";
				else if (params.stamps.full != m.stamps.full && PolicyOf(Counter::Full) == ReaderPolicy::Reset) reason = "stamp full";
				else if (params.stamps.continuity != m.stamps.continuity && PolicyOf(Counter::Continuity) == ReaderPolicy::Reset) reason = "stamp continuity";
				else if (params.stamps.rebaseH != m.stamps.rebaseH && PolicyOf(Counter::RebaseH) == ReaderPolicy::Reset) reason = "stamp rebaseH";
				else if (params.stamps.rebaseV != m.stamps.rebaseV && PolicyOf(Counter::RebaseV) == ReaderPolicy::Reset) reason = "stamp rebaseV";
			}
			if (params.cut) reason = "cut";
			else if (params.fade) reason = "fade";
			const bool haveFrame = m.haveFrame && !reason;
			const bool reset = !haveFrame;
			const bool haveBwd = params.backward.tex != nullptr;
			const bool haveFwd = haveBwd && params.forward.tex != nullptr;
			const bool motionLost = haveFrame && !haveBwd;
			const bool composeOn = haveFrame && haveBwd;
			const bool slotsValid = haveFrame && !motionLost;
			const bool lagOk = m.havePrevG && haveBwd && !reason && !reset && w == m.geo.w && h == m.geo.h;

			auto fail = [&](const char* what) {
				m_error = std::string("temporal detail: ") + what;
				++m_failures;
				m.haveFrame = false;   // the next frame restarts
				m.havePrevG = false;
				return false;
			};

			// --- Textures ---
			const pl_fmt f4 = FloatFormat(gpu, 4), f2 = FloatFormat(gpu, 2), f1 = FloatFormat(gpu, 1), h4 = FloatFormat(gpu, 4, 16);
			const bool floatLuma = luma->params.format->type == PL_FMT_FLOAT;
			pl_fmt baseFmt = floatLuma ? nullptr : Unorm16(gpu, 1);
			m.baseUnorm = baseFmt != nullptr;
			if (!baseFmt) baseFmt = f1;
			const int kbw = K * g.bw;
			bool ok = f4 && f2 && f1 && h4 &&
				Ensure(gpu, m.out, W, H, StorableLike(gpu, luma->params.format), true) &&
				Ensure(gpu, m.meas, g.bw, g.bh, f4) && Ensure(gpu, m.slotBlk, kbw, g.bh, f4) &&
				Ensure(gpu, m.evA, kbw, g.bh, f4) && Ensure(gpu, m.evB, kbw, g.bh, f4) && Ensure(gpu, m.work, g.bw, g.bh, f4) &&
				Ensure(gpu, m.coef, kbw, g.bh, f1) && Ensure(gpu, m.cw, g.bw, g.bh, f4) && Ensure(gpu, m.cf, g.bw, g.bh, f4) &&
				Ensure(gpu, m.ct, g.bw, g.bh, f4) && Ensure(gpu, m.delta, w, h, f2) && Ensure(gpu, m.gsum, 2 * g.bw, g.bh, f4) &&
				Ensure(gpu, m.kback, g.bw, g.bh, f1) && Ensure(gpu, m.gflag, g.bw, g.bh, f4) && Ensure(gpu, m.prevg, w, h, f2);
			// The slot atlas within the largest 2-D texture the card allows (else visibly fewer slots would be needed).
			const int maxDim = static_cast<int>(gpu->limits.max_tex_2d_dim > 0 ? gpu->limits.max_tex_2d_dim : 16384);
			const int acols = AtlasCols(K, w, h, maxDim);
			if (!acols) return fail("the reference store does not fit the largest texture (use fewer slots)");
			const int arows = (K + acols - 1) / acols;
			ok = ok && Ensure(gpu, m.atlasPix, acols * w, arows * h, h4) && Ensure(gpu, m.atlasBase, acols * w, arows * h, baseFmt);
			for (int i = 0; ok && i < 2; ++i)
				ok = Ensure(gpu, m.mot[i], kbw, g.bh, f4) && Ensure(gpu, m.tab[i], K + 2, 1, f4) && Ensure(gpu, m.state[i], 8 * g.bw, g.bh, f4);
			if (ok && params.templateClean)
				ok = Ensure(gpu, m.acc[0], w, h, f2) && Ensure(gpu, m.acc[1], w, h, f2) && Ensure(gpu, m.patches, g.npx, g.npy, f2);
			if (ok && !m.counters[0])
			{
				// Zero-initialised (the guard-trip count is cumulative over the stream, as the model's).
				const std::vector<float> zero(2 * 4, 0.0f);
				for (int i = 0; ok && i < 2; ++i)
				{
					pl_tex_params tp{};
					tp.w = 2; tp.h = 1; tp.format = f4; tp.sampleable = true; tp.storable = true; tp.blit_src = true;
					tp.initial_data = f4->num_components == 4 ? zero.data() : nullptr;
					m.counters[i] = pl_tex_create(gpu, &tp);
					ok = m.counters[i] != nullptr;
				}
			}
			if (!ok) return fail("no storage texture for the module");
			pl_tex work = luma;
			if (!whole)
			{
				// The active picture, bit-exact in the plane's own format (its values keep their code unit).
				if (!(luma->params.format->caps & PL_FMT_CAP_STORABLE)) return fail("the luma format is not storable (active-picture crop)");
				if (!Ensure(gpu, m.crop, w, h, luma->params.format)) return fail("no texture for the active picture");
				work = m.crop;
			}

			// --- Grain split (the shared product) ---
			const double cu = CodeUnit(luma, source.repr.bits, params.colorDepth);
			GrainSplit::Params sp;
			sp.sigmaFine = kSigmaFine; sp.sigmaBase = kSigmaBase; sp.sigmaLow = kSigmaLow;
			sp.subBlocks = kSubBlocks; sp.quietSubBlocks = kQuietSubBlocks; sp.neighbourhood = kGrainNeighbourhood;
			sp.grainCal = kGrainCal; sp.kappa = kWienerKappa; sp.energyFloor = kEnergyFloor;
			const std::string rectDefs = DefineInt("TD_OX", r.left) + DefineInt("TD_OY", r.top) + DefineInt("TD_FW", W) + DefineInt("TD_FH", H);
			if (!whole)
			{
				const pl_shader_desc cd[2] = { Sampled("src", luma), Storage("crop_out", m.crop) };
				if (!Pass(dispatch, "VideoProcessor temporal detail: active picture", DefineInt("TD_W", w) + DefineInt("TD_H", h) + rectDefs, Shaders::kCropBody,
					cd, 2, nullptr, 0, (w + 15) / 16, (h + 15) / 16, 0))
					return fail("active picture pass failed");
			}
			// The shared product when it fits (one producer, many readers), else this module's own split (visible).
			const bool shared = GrainSplit::Gpu::Fits(params.split, w, h, g.B, g.scale, cu, 0);
			if (!shared && !m.split.Compute(gpu, dispatch, work, 0, cu, g.scale, g.B, sp)) return fail(m.split.LastError().c_str());
			const GrainSplit::Gpu::Frame& gs = shared ? *params.split : m.split.Out();
			const pl_tex splitTex = gs.split, gblocks = gs.blocks;

			// --- Headers ---
			const std::string geo = DefineInt("TD_W", w) + DefineInt("TD_H", h) + DefineInt("TD_B", g.B) + DefineInt("TD_BW", g.bw) +
				DefineInt("TD_BH", g.bh) + DefineInt("TD_K", K) + DefineInt("TD_ACOLS", acols) + Define("TD_INV_CU", 1.0 / cu) + Define("TD_CU", cu) +
				Define("TD_SB_LOAD", m.baseUnorm ? 1.0 / cu : 1.0) + Define("TD_SB_STORE", m.baseUnorm ? cu : 1.0) + rectDefs;
			std::string law;
			law += Define("TD_FLOOR", kEnergyFloor);
			const GateTuning& gt = params.gates;
			law += Define("TD_CORR_MIN", kCorrMin) + Define("TD_STRONGER_MARGIN", gt.strongerMargin);
			law += Define("TD_SIG_LO", gt.significanceLo) + Define("TD_SIG_HI", gt.significanceHi);
			law += Define("TD_RHO2_LO", kRho2Lo) + Define("TD_RHO2_HI", kRho2Hi) + Define("TD_PULL_LO", kPullLo) + Define("TD_PULL_HI", kPullHi);
			law += Define("TD_UMAX", gt.uncertaintyPx * g.scale) + Define("TD_BASE_NOISE", gt.baseNoiseCodes) + Define("TD_BASE_GRAIN_SIGMAS", gt.baseGrainSigmas);
			law += Define("TD_AGE_FADE", kAgeFadeShare) + Define("TD_REBOUND_TAU", kReboundTau) + Define("TD_REBOUND_MIN", kReboundMinEnergy);
			law += Define("TD_SOFT_STEP", kSoftStep) + Define("TD_RETIRE_MIN", kRetireMinEnergy) + Define("TD_EV_LO", kEvidenceLo) + Define("TD_EV_HI", kEvidenceHi);
			law += Define("TD_EVICT_WEIGHT", kEvictWeight) + Define("TD_TEMPLATE_CLEAN", params.templateClean ? 1.0 : 0.0);
			law += Define("TD_CLEAN_MEMBER_MIN", kCleanMemberMin) + Define("TD_GAIN_LO", kGainLo) + Define("TD_GAIN_HI", kGainHi);
			law += Define("TD_ENTER", kEnterWeight) + Define("TD_LEAVE", kLeaveWeight) + Define("TD_TEMPLATE_MIN_HARD", kTemplateMinHard);
			law += Define("TD_COHERENCE_LO", kCoherenceLo) + Define("TD_COHERENCE_HI", kCoherenceHi) + Define("TD_PIXEL_CAP", kPixelCap);
			law += Define("TD_BOOST_GRAIN_SHARE", kBoostGrainShare) + Define("TD_GUARD_MAX_SHARE", kGuardMaxShare) + Define("TD_ADDBACK_MIN", kGuardAddBackMin);
			law += Define("TD_GUARD_DROP_TRIP", kGuardDropTrip) + Define("TD_GUARD_RISE_TRIP", kGuardRiseTrip) + Define("TD_GUARD_FREEZE_TRIP", kGuardFreezeTrip);
			law += Define("TD_GUARD_RELEASE", kGuardRelease);
			if (params.guard) law += "#define TD_GUARD 1\n";
			if (haveFwd) law += "#define TD_FWD 1\n";
			if (params.quality) law += "#define TD_QUALITY 1\n";
			if (params.blockGates) law += "#define TD_BGATES 1\n";
			if (lagOk) law += "#define TD_LAG 1\n";
			const std::string base = geo + law + Shaders::kCore;
			const std::string slots = SlotFunctions(true, false, false) + Shaders::kSlotSample;

			// --- Per-frame variables ---
			const int fRise = Frames(kRiseSeconds, hz), fFall = Frames(kFallSeconds, hz), fXf = Frames(kCrossfadeSeconds, hz), fRun = Frames(kSoftRunSeconds, hz);
			const float vReset = reset ? 1.0f : 0.0f, vLost = motionLost ? 1.0f : 0.0f, vValid = slotsValid ? 1.0f : 0.0f;
			const float vHave = haveFrame ? 1.0f : 0.0f, vCompose = composeOn ? 1.0f : 0.0f, vHz = static_cast<float>(hz);
			const float vMaxAge = static_cast<float>(set.maxAgeSeconds);
			const float vRebound = static_cast<float>(std::pow(0.5, 1.0 / Frames(set.maxAgeSeconds * 0.5, hz)));
			const float vPeak = static_cast<float>(std::pow(0.5, 1.0 / Frames(set.maxAgeSeconds, hz)));
			const float vInvXf = static_cast<float>(1.0 / fXf), vRun = static_cast<float>(fRun);
			const float vRise = static_cast<float>(1.0 / fRise), vFall = static_cast<float>(1.0 / fFall);
			const float vStrength = static_cast<float>(set.strength), vBoost = static_cast<float>(set.boost ? set.boostMax : 0.0);
			const float vAlpha = static_cast<float>(1.0 - 1.0 / Frames(kGuardStatsSeconds, hz));
			const float vGFall = static_cast<float>(1.0 / Frames(kGuardFallSeconds, hz)), vGRise = static_cast<float>(1.0 / Frames(kGuardRiseSeconds, hz));
			const float vBwdBpx = static_cast<float>(params.backward.blockPx > 0.0 ? params.backward.blockPx : 16.0);
			const float vFwdBpx = static_cast<float>(params.forward.blockPx > 0.0 ? params.forward.blockPx : 16.0);
			const float vBudget = static_cast<float>(params.grainBudget), vShare = static_cast<float>(params.guardTemplateShare);
			const float vKappa = static_cast<float>(params.cleanKappa);
			const pl_shader_var vars[] = {
				Var("reset", &vReset), Var("motion_lost", &vLost), Var("slots_valid", &vValid), Var("have_frame", &vHave),
				Var("compose_on", &vCompose), Var("hz", &vHz), Var("max_age", &vMaxAge), Var("rebound_decay", &vRebound),
				Var("peak_decay", &vPeak), Var("inv_fxf", &vInvXf), Var("f_run", &vRun), Var("rise", &vRise), Var("fall", &vFall),
				Var("strength", &vStrength), Var("boost_max", &vBoost), Var("alpha", &vAlpha), Var("g_fall", &vGFall),
				Var("g_rise", &vGRise), Var("bwd_bpx", &vBwdBpx), Var("fwd_bpx", &vFwdBpx), Var("grain_budget", &vBudget),
				Var("guard_share", &vShare), Var("clean_kappa", &vKappa) };
			const int nv = static_cast<int>(sizeof(vars) / sizeof(vars[0]));

			const int pgx = (w + 15) / 16, pgy = (h + 15) / 16, bgx = (g.bw + 15) / 16, bgy = (g.bh + 15) / 16;
			const size_t redShmem = sizeof(float) * 4 * 256 * 2;
			const int motOld = m.motCur, motNew = 1 - m.motCur, tabOld = m.tabCur, tabNew = 1 - m.tabCur;
			std::vector<pl_shader_desc> d;
			auto slotDescs = [&]() {
				d.push_back(Sampled("slots", m.atlasPix));
				d.push_back(Sampled("sbases", m.atlasBase));
			};
			auto run = [&](const char* what, const std::string& header, const char* body, int gx, int gy, size_t shmem, int gw = 16, int gh = 16) {
				const bool r = Pass(dispatch, what, header, body, d.data(), static_cast<int>(d.size()), vars, nv, gx, gy, shmem, gw, gh);
				d.clear();
				return r;
			};

			// 2: MEAS
			d = { Sampled("split", splitTex), Sampled("gblocks", gblocks), Storage("meas_out", m.meas) };
			if (!run("VideoProcessor temporal detail: block measures", base, Shaders::kMeasBody, bgx, bgy, 0)) return fail("block measures pass failed");
			// 5: COMPOSE
			d = { Sampled("mot", m.mot[motOld]), Sampled("tab_old", m.tab[tabOld]), Storage("mot_out", m.mot[motNew]) };
			if (haveBwd) d.push_back(Sampled("bwd", params.backward.tex));
			if (haveFwd) d.push_back(Sampled("fwd", params.forward.tex));
			{
				const std::string hdr = base + Shaders::kMot + (haveBwd ? Shaders::kPairBwd : "") + (haveFwd ? Shaders::kPairFwd : "");
				const char* body = haveBwd ? Shaders::kComposeBody : R"GLSL(
ivec2 g = ivec2(gl_GlobalInvocationID.xy);
if (g.x < TD_BW * TD_K && g.y < TD_BH) imageStore(mot_out, g, vec4(0.0));
)GLSL";
				if (!run("VideoProcessor temporal detail: composed motion", hdr, body, (kbw + 15) / 16, bgy, 0)) return fail("compose pass failed");
			}
			// 6: EVAL
			d = { Sampled("split", splitTex), Sampled("gblocks", gblocks), Sampled("meas", m.meas), Sampled("mot", m.mot[motNew]),
				Sampled("tab_old", m.tab[tabOld]), Sampled("slot_blk", m.slotBlk), Storage("ev_a_out", m.evA), Storage("ev_b_out", m.evB) };
			if (params.blockGates) d.push_back(Sampled("bgates", params.blockGates));
			slotDescs();
			if (!run("VideoProcessor temporal detail: validation", base + Shaders::kMot + slots + Shaders::kReduce, Shaders::kEvalBody, kbw, g.bh, redShmem))
				return fail("validation pass failed");
			// 3: TRACK (state 0 -> 1)
			d = { Sampled("st_in", m.state[0]), Storage("st_out", m.state[1]), Sampled("meas", m.meas), Sampled("tab_old", m.tab[tabOld]),
				Sampled("ev_b", m.evB), Storage("work_out", m.work) };
			if (params.quality) d.push_back(Sampled("quality", params.quality));
			if (!run("VideoProcessor temporal detail: trackers", base + Shaders::kStateLoad + Shaders::kStateStore, Shaders::kTrackBody, bgx, bgy, 0))
				return fail("tracker pass failed");
			// 4: ADMIT
			d = { Sampled("work", m.work), Sampled("st_in", m.state[1]), Sampled("tab_old", m.tab[tabOld]), Storage("tab_out", m.tab[tabNew]) };
			if (!run("VideoProcessor temporal detail: admission", base + Shaders::kAdmitHeader, Shaders::kAdmitBody, 1, 1, sizeof(int) * 256 * (2 * K + 1)))
				return fail("admission pass failed");
			// 4a: CLEAN (four parity classes; admission frames only do work)
			if (params.templateClean)
			{
				const Detail::CleanBasis cb = Detail::MakeCleanBasis(g.P, kSigmaBase * g.scale);
				const std::string chdr = DefineInt("TD_P", g.P) + DefineInt("TD_HALF", g.half) + DefineInt("TD_NPX", g.npx) + DefineInt("TD_NPY", g.npy) +
					ConstArray("TD_C", g.P * g.P, [&](int i) { return cb.C[static_cast<size_t>(i)]; }) +
					ConstArray("TD_WIN", g.P, [&](int i) { return cb.win[static_cast<size_t>(i)]; }) +
					ConstArray("TD_HS", g.P * g.P, [&](int i) { return cb.hs[static_cast<size_t>(i)]; });
				for (int q = 0; q < 4; ++q)
				{
					const int ex = q & 1, ey = q >> 1;
					const int gx = (g.npx - ex + 1) / 2, gy = (g.npy - ey + 1) / 2;
					d = { Sampled("split", splitTex), Sampled("gblocks", gblocks), Sampled("mot", m.mot[motNew]), Sampled("tab_new", m.tab[tabNew]),
						Sampled("ev_a", m.evA), Sampled("ev_b", m.evB), Storage("patch_out", m.patches), Storage("acc_out", m.acc[q & 1]) };
					if (q > 0) d.push_back(Sampled("acc_in", m.acc[(q - 1) & 1]));
					slotDescs();
					const std::string hdr = base + DefineInt("TD_EX", ex) + DefineInt("TD_EY", ey) + (q > 0 ? "#define TD_ACC_IN 1\n" : "") + chdr +
						Shaders::kMot + slots + Shaders::kOrder + Shaders::kCleanHeader;
					if (gx > 0 && gy > 0 && !run("VideoProcessor temporal detail: template cleaner", hdr, Shaders::kCleanBody, gx, gy,
						sizeof(float) * (2 * g.P * g.P + 2), g.P, g.P))
						return fail("cleaner pass failed");
				}
			}
			const std::string cleanedDef = params.templateClean ? "#define TD_CLEANED 1\n" : "";
			// 4: the new slot (admission frames only)
			d = { Sampled("split", splitTex), Sampled("tab_new", m.tab[tabNew]) };
			if (params.templateClean) d.push_back(Sampled("acc_in", m.acc[1]));
			d.push_back(Storage("slots_out", m.atlasPix));
			if (!run("VideoProcessor temporal detail: store slot", base + cleanedDef + SlotFunctions(false, true, false), Shaders::kFinishBody, pgx, pgy, 0))
				return fail("slot store pass failed");
			d = { Sampled("split", splitTex), Sampled("tab_new", m.tab[tabNew]) };
			d.push_back(Storage("sbases_out", m.atlasBase));
			if (!run("VideoProcessor temporal detail: store slot base", base + SlotFunctions(false, false, true), Shaders::kBaseCopyBody, pgx, pgy, 0))
				return fail("slot base pass failed");
			d = { Sampled("meas", m.meas), Sampled("gblocks", gblocks), Sampled("tab_new", m.tab[tabNew]), Storage("slot_blk_out", m.slotBlk),
				Storage("mot_out", m.mot[motNew]) };
			if (params.templateClean) d.push_back(Sampled("patches", m.patches));
			{
				const std::string hdr = base + cleanedDef + DefineInt("TD_HALF", g.half) + DefineInt("TD_NPX", g.npx) + DefineInt("TD_NPY", g.npy);
				if (!run("VideoProcessor temporal detail: store slot maps", hdr, Shaders::kCopyBlkBody, bgx, bgy, 0)) return fail("slot map pass failed");
			}
			// 7: WEIGHT (state 1 -> 0)
			d = { Sampled("st_in", m.state[1]), Storage("st_out", m.state[0]), Sampled("work", m.work), Sampled("tab_new", m.tab[tabNew]),
				Sampled("meas", m.meas), Sampled("ev_a", m.evA), Sampled("ev_b", m.evB), Sampled("slot_blk", m.slotBlk), Sampled("mot", m.mot[motNew]),
				Storage("coef_out", m.coef), Storage("cw_out", m.cw) };
			if (params.blockGates) d.push_back(Sampled("bgates", params.blockGates));
			if (!run("VideoProcessor temporal detail: weights", base + Shaders::kStateLoad + Shaders::kStateStore + Shaders::kMot + Shaders::kOrder,
				Shaders::kWeightBody, bgx, bgy, 0))
				return fail("weight pass failed");
			// 8: PROJ
			d = { Sampled("cw", m.cw), Sampled("coef", m.coef), Sampled("tab_new", m.tab[tabNew]), Sampled("split", splitTex), Sampled("mot", m.mot[motNew]),
				Storage("cf_out", m.cf), Storage("ct_out", m.ct) };
			slotDescs();
			const std::string tmpl = Shaders::kMot + slots + Shaders::kOrder + Shaders::kTemplate;
			if (!run("VideoProcessor temporal detail: projections", base + tmpl + Shaders::kReduce, Shaders::kProjBody, g.bw, g.bh, redShmem))
				return fail("projection pass failed");
			// 8: TRANSFER
			d = { Sampled("cf", m.cf), Sampled("ct", m.ct), Sampled("coef", m.coef), Sampled("tab_new", m.tab[tabNew]), Sampled("split", splitTex),
				Sampled("gblocks", gblocks), Sampled("mot", m.mot[motNew]), Storage("delta_out", m.delta) };
			slotDescs();
			if (!run("VideoProcessor temporal detail: transfer", base + tmpl, Shaders::kTransferBody, pgx, pgy, 0)) return fail("transfer pass failed");
			// 10: GUARD
			d = { Sampled("split", splitTex), Sampled("src", work), Sampled("delta", m.delta), Storage("gsum_out", m.gsum) };
			if (lagOk) { d.push_back(Sampled("bwd", params.backward.tex)); d.push_back(Sampled("prevg", m.prevg)); }
			if (!run("VideoProcessor temporal detail: grain guard sums",
				base + (lagOk ? std::string(Shaders::kPairBwd) + Shaders::kPrevG : std::string()) + Shaders::kReduce, Shaders::kGuardSumsBody, g.bw, g.bh, redShmem))
				return fail("guard sums pass failed");
			d = { Sampled("st_in", m.state[0]), Storage("st_out", m.state[1]), Sampled("gsum", m.gsum), Storage("kback_out", m.kback) };
			if (!run("VideoProcessor temporal detail: grain guard add-back", base + Shaders::kStateLoad + Shaders::kStateStore, Shaders::kGuardEmaBody, bgx, bgy, 0))
				return fail("guard add-back pass failed");
			d = { Sampled("st_in", m.state[1]), Storage("st_out", m.state[0]), Sampled("kback", m.kback), Storage("gflag_out", m.gflag) };
			if (!run("VideoProcessor temporal detail: grain guard", base + Shaders::kStateLoad + Shaders::kStateStore, Shaders::kGuardPoolBody, bgx, bgy, 0))
				return fail("guard pass failed");
			d = { Sampled("gflag", m.gflag), Sampled("tab_new", m.tab[tabNew]), Sampled("counters_old", m.counters[m.cntCur]),
				Storage("counters_out", m.counters[1 - m.cntCur]) };
			if (!run("VideoProcessor temporal detail: counters", base + Shaders::kStatsHeader, Shaders::kStatsBody, 1, 1, sizeof(float) * 4 * 256))
				return fail("counter pass failed");
			// 9: APPLY
			d = { Sampled("src", luma), Sampled("split", splitTex), Sampled("delta", m.delta), Sampled("kback", m.kback), Storage("dst", m.out),
				Storage("prevg_out", m.prevg) };
			if (!run("VideoProcessor temporal detail: recompose", base, Shaders::kApplyBody, (W + 15) / 16, (H + 15) / 16, 0)) return fail("recompose pass failed");

			m.motCur = motNew;
			m.tabCur = tabNew;
			m.cntCur = 1 - m.cntCur;
			m.countersReady = true;
			m.geo = g;
			m.rect = r;
			m.haveFrame = true;
			m.havePrevG = true;
			m.prevSeq = params.seq;
			m.stamps = params.stamps;
			m_status = reason ? std::string("reset: ") + reason : (motionLost ? std::string("motion lost") : std::string("ok"));
			if (!shared) m_status += params.split ? "; own grain split (the shared product does not fit)" : "; own grain split";
			if (K < kSlots) m_status += "; store " + std::to_string(K) + " of " + std::to_string(kSlots) + " slots (memory tier)";
			if (params.lookAheadAvailable || params.cameraAvailable)
				m_status += std::string("; not on GPU:") + (params.lookAheadAvailable ? " look-ahead" : "") +
					(params.lookAheadAvailable && params.cameraAvailable ? "," : "") + (params.cameraAvailable ? " camera model" : "");
			pl_frame frame = source;
			frame.planes[0].texture = m.out;
			result = frame;
			return true;
		}
	}
}
