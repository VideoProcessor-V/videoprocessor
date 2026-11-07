// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.grain_split on the GPU: the shared grain-free picture of one plane, as compute passes that mirror the CPU
// reference model GrainSplit::Compute (GrainSplit.h) pass by pass (same separable Gaussians and edge replication,
// same sub-block grain level, neighbourhood minimum and Wiener share). Plane-generic: luma for temporal_detail,
// luma and each chroma plane for the chroma upscaler's grain guard.
//
// Passes (all compute, nothing read back):
//   1. GH     rows:    Y -> (G1 Y, Gb Y, G6 Y, Y)                       (rgba32f, full size)
//   2. GV     columns: -> bands (d1, d2, d3, base)                       (rgba32f, full size)
//   3. QUIET  one thread per block: the 16 sub-block energies of D = d1 + d2, mean of the quietest x cal; the
//             block mean of d3^2 (the focus band's energy, read by temporal_detail)
//   4. NMIN   one thread per block: the minimum over (2 n + 1)^2 blocks -> blocks (N, e3)
//   5. F      per pixel: e3x3 = mean3x3(D^2), f = clamp(1 - kappa N / max(e, floor), 0, 1) -> split (f d1, f d2, f, base)
// The split texture reuses the rows texture of pass 1. Values are codes of the signal (the plane's value / CodeUnit).
// clean = base + s1 + s2 and G = Y - clean are formed by the reader (Y = clean + G exactly by construction).
//
// Cost (estimate): about 85 texture reads per pixel; two rgba32f planes (32 B/px: ~265 MB at 3840x2160, ~66 MB at
// 1080p; a chroma plane of 4:2:0 a quarter of that); well under 1.5 ms at 4K on an RTX 3090, ~4-6 ms on the RTX 3050
// Ti laptop (estimates; GrainSplitGpuTiming measures). Deterministic: fixed loop orders, no atomics.
#pragma once

#include <VP-V/core/ScratchArena.h>
#include <VP-V/modules/control/gpu_memory/PlTexBackend.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders/custom.h>

#include <VP-V/core/FallbackCatalog.h>

#include <cstdint>
#include <string>
#include <vector>

namespace GrainSplit
{
	namespace Gpu
	{
		// Scratch-arena scope of one Compute (one render stage = one scope; GpuAllocator.h).
		constexpr uint16_t kArenaScope = 0x0201;

		// The product of one frame of one plane (valid until the next Compute of the same Splitter).
		// Owner rule (2026-10-31): ONE producer, many readers. The host computes it once, early, on the decoded source
		// (its active picture), publishes it as the shared `grain.split` product (RegionEvidence::GrainProduct::gpu, read through GrainView), and every repair
		// reads the same Frame; the exact original grain is added back once, at the end of the source-domain block. A reader
		// never re-estimates grain from a processed frame; its own Splitter is only a visible fallback.
		struct Frame
		{
			// Full:    split = w x h rgba32f (s1 = f d1, s2 = f d2, f, base; codes); blocks = bw x bh rgba32f (N, e3, 0, 0).
			// Compact: split = texture A, w x h rg16f (s1, s2 in codes); splitB = texture B, w x h rg16 unorm (f, base x
			//          codeUnit, i.e. the plane value); blocks = bw x bh rgba32f (N, e3, e1, e2). Read ONLY through ViewGlsl / BindView.
			pl_tex split = nullptr;
			pl_tex blocks = nullptr;   // N (grain level, codes^2 of D), e3 (block mean of d3^2)
			int w = 0, h = 0, bw = 0, bh = 0, B = 16;
			double scale = 1.0;
			double codeUnit = 0.0;     // the plane's code unit it was computed with
			int component = -1;        // the plane component it was computed on
			GrainSplit::Storage storage = GrainSplit::Storage::Full;
			pl_tex splitB = nullptr;   // Compact only (texture B)
			uint64_t seq = 0;          // the source frame it was computed for (Options::seq)
			uint32_t version = 0;      // GrainSplit::kUnitVersion of the unit that made it (0 = not made by a Splitter)
		};

		inline bool Compact(const Frame* f) { return f && f->storage == GrainSplit::Storage::Compact; }

		constexpr uint64_t kAnySeq = ~uint64_t(0);

		enum class FitResult : uint8_t { Fits, Missing, WrongSize, Stale };

		// Whether a published product fits a reader's plane: same unit version, the expected source frame (kAnySeq = the
		// reader does not know it), size, block, scale, code unit and component. Else the reader falls back visibly (a
		// product of another bit depth, chroma channel, frame or unit version is never accepted silently):
		// Missing = nothing usable, Stale = another frame or version, WrongSize = another geometry/unit/component.
		inline FitResult CheckFit(const Frame* f, int w, int h, int B, double scale, double codeUnit, int component,
			uint64_t expectedSeq = kAnySeq)
		{
			if (!f || !f->split || !f->blocks || (f->storage == GrainSplit::Storage::Compact && !f->splitB))
				return FitResult::Missing;
			if (f->version != GrainSplit::kUnitVersion || (expectedSeq != kAnySeq && f->seq != expectedSeq))
				return FitResult::Stale;
			if (f->w != w || f->h != h || f->B != B || f->scale != scale || f->codeUnit != codeUnit || f->component != component)
				return FitResult::WrongSize;
			return FitResult::Fits;
		}

		inline bool Fits(const Frame* f, int w, int h, int B, double scale, double codeUnit, int component,
			uint64_t expectedSeq = kAnySeq)
		{
			return CheckFit(f, w, h, B, scale, codeUnit, component, expectedSeq) == FitResult::Fits;
		}

		// The storage a Compute really uses. Compact is refused (Full instead, visibly) when it would be wrong:
		//  - the plane is not UNORM/UINT: a float plane can hold blacker-than-black / whiter-than-white that the unorm
		//    base of the compact layout would clamp;
		//  - 1 / codeUnit > 4096 (more than 12 bits of codes): the fp16 s1/s2 would lose precision (and overflow).
		struct StorageChoice
		{
			GrainSplit::Storage storage = GrainSplit::Storage::Full;
			bool downgraded = false;
			std::string why;
		};

		inline StorageChoice ChooseStorage(GrainSplit::Storage requested, pl_fmt planeFormat, double codeUnit)
		{
			StorageChoice c;
			c.storage = requested;
			if (requested != GrainSplit::Storage::Compact)
				return c;
			if (!planeFormat || !(planeFormat->type == PL_FMT_UNORM || planeFormat->type == PL_FMT_UINT))
				c.why = "the plane is not an integer format (compact would clamp blacker-than-black and whiter-than-white)";
			else if (!(codeUnit > 0.0) || 1.0 / codeUnit > 4096.0)
				c.why = "more than 12 bits of codes (compact fp16 would lose precision)";
			if (!c.why.empty())
			{
				c.storage = GrainSplit::Storage::Full;
				c.downgraded = true;
			}
			return c;
		}

		inline FallbackCatalog::Decision StorageFallback(const StorageChoice& c)
		{
			if (!c.downgraded)
				return FallbackCatalog::Released("grain split storage");
			return FallbackCatalog::Engaged("grain split storage", "compact refused, using full: " + c.why);
		}

		// Output textures supplied by the caller (a skeleton node's slot); null = the Splitter owns them as before.
		// Full: split = w x h rgba32f, blocks = bw x bh rgba32f. Compact: split = w x h rg16f (A), splitB = w x h rg16 unorm
		// (B), blocks = bw x bh rgba32f (both layouts). Never destroyed by the Splitter.
		struct Targets { pl_tex split = nullptr, splitB = nullptr, blocks = nullptr; };

		struct Options
		{
			GrainSplit::Storage storage = GrainSplit::Storage::Full;
			VpSkeleton::ScratchArena<VpSkeleton::PlTexBackend>* arena = nullptr;   // intermediates; null = private textures
			Targets targets;
			uint64_t seq = 0;   // the source frame this split is for: stamped into Frame::seq
		};

		class Splitter
		{
		public:
			Splitter() = default;
			~Splitter() = default;   // textures belong to the gpu: Release
			Splitter(const Splitter&) = delete;
			Splitter& operator=(const Splitter&) = delete;

			// The split of `plane` (component `component`, 0..3) whose values are codes x codeUnit. scale multiplies
			// the sigmas, B is the block edge in plane pixels. False (with LastError) on any failure.
			bool Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B,
				const Params& params = Params());
			// The same with a storage layout, the scratch arena for the intermediates and caller-supplied outputs.
			// Options() = the call above, byte for byte. Arena shortage: false, ScratchShort() true, LastError "scratch memory
			// short (grain split)..." (no private temporaries are made instead; the caller falls back visibly).
			bool Compute(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B,
				const Params& params, const Options& options);
			// The five passes of Compute as separate calls (70b lane form: one chunk per pass): Step(0..4) with the SAME arguments
			// and options on every call records exactly Compute's dispatches in order (rows, columns, grain level, minimum, Wiener
			// share), so Compute == the step loop byte for byte. Step 0 validates and resolves the outputs and intermediates
			// (private textures survive between steps and gaps; with options.arena the CALLER holds the arena scope kArenaScope
			// across the five steps); Out() is valid after step 4. A failing or out-of-order step returns false and ends the run.
			static constexpr uint32_t StepCount() { return 5; }
			bool Step(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, double scale, int B, const Params& params,
				const Options& options, uint32_t step);
			// The scratch-arena requests of one Compute (scope kArenaScope, owner "grain_split", T0): the rgba32f temporaries.
			static std::vector<VpSkeleton::ArenaRequest> ArenaRequests(pl_gpu gpu, int w, int h, int B, GrainSplit::Storage storage);
			bool ScratchShort() const { return m_scratchShort; }
			// "" or why the requested Compact storage was replaced by Full (ChooseStorage); StorageDecision() is the
			// FallbackCatalog entry (engaged while the last Compute had to fall back).
			const std::string& StorageNote() const { return m_storageNote; }
			FallbackCatalog::Decision StorageDecision() const
			{
				StorageChoice c;
				c.downgraded = !m_storageNote.empty();
				c.why = m_storageNote;
				return StorageFallback(c);
			}
			const Frame& Out() const { return m_out; }
			void Release(pl_gpu gpu);
			const std::string& LastError() const { return m_error; }

		private:
			// The run of the step API: set by step 0, cleared after step 4 or on a failure.
			struct Run
			{
				bool active = false;
				uint32_t next = 0;
				bool compact = false;
				GrainSplit::Storage storage = GrainSplit::Storage::Full;
				int w = 0, h = 0, bw = 0, bh = 0;
				pl_tex rows = nullptr, bands = nullptr, quiet = nullptr, blocks = nullptr, outA = nullptr, outB = nullptr;
			};
			bool Begin(pl_gpu gpu, pl_dispatch dispatch, pl_tex plane, int component, double codeUnit, int B, const Params& params, const Options& options);
			Run m_run;
			pl_tex m_rows = nullptr, m_bands = nullptr, m_quiet = nullptr, m_blocks = nullptr, m_outA = nullptr, m_outB = nullptr;
			Frame m_out;
			std::string m_error;
			bool m_scratchShort = false;
			std::string m_storageNote;
		};

		// The Gaussian weights of GrainSplit::Gauss (radius max(1, ceil(3 sigma)), normalised over the taps).
		int GaussRadius(double sigma);
		double GaussWeight(int k, double sigma);

		// THE ONE READ ACCESSOR. GLSL prelude defining vec4 gs_fetch(ivec2 p) -> (s1, s2, f, base) in CODES and
		// vec2 gs_blocks(ivec2 b) -> (N, e3), for either layout. Full samplers: `split`, `gblocks`; Compact: `gs_a`, `gs_b`,
		// `gblocks` (libplacebo declares them from the descriptor names: BindView supplies them; do not declare uniforms).
		// Modifier accessors (same prelude): vec2 gs_energies(ivec2 b) -> (e1, e2) block means of d1^2, d2^2 (codes^2) or (-1, -1)
		// when the layout has none (Full); float gs_tilt(ivec2 b) = log2((e1 + 1e-3) / (e2 + 1e-3)) (0 when unavailable);
		// `#define GS_HAS_ENERGIES 1|0` tells a shader which. A reader treats "unavailable" as absent and derives its own.
		std::string ViewGlsl(GrainSplit::Storage storage, double codeUnit);
		// Appends the sampled descriptors ViewGlsl reads (Full: split, gblocks; Compact: gs_a, gs_b, gblocks).
		void BindView(const Frame& f, std::vector<pl_shader_desc>& out);

		// THE EXPLICIT FIELD TABLE of both layouts (owner decision: one main map plus small modifier fields). Base = the main
		// split map (s1, s2, base); Modifiers = what steers a derivation: per-pixel Wiener share f, per-block grain level N,
		// focus-band energy e3 and, in Compact only, the block mean band energies e1, e2 (the spectral tilt is DERIVED from them:
		// gs_tilt = log2((e1 + 1e-3) / (e2 + 1e-3)), never stored). Full leaves blocks.zw at 0: gs_energies returns (-1, -1) and
		// gs_tilt 0 there (GS_HAS_ENERGIES 0) and readers derive their own.
		enum class FieldRole : uint8_t { Base, Modifier };
		enum class FieldDomain : uint8_t { Pixel, Block };

		struct GrainField
		{
			const char* name;
			FieldRole role;
			FieldDomain domain;
			const char* textureFull;      // texture and channel in the Full layout ("-" = not stored)
			const char* textureCompact;   // ... and in the Compact layout
		};

		inline const GrainField* GrainFieldTable(size_t* count)
		{
			static const GrainField kTable[] = {
				{ "s1", FieldRole::Base, FieldDomain::Pixel, "split.x", "A.x" },
				{ "s2", FieldRole::Base, FieldDomain::Pixel, "split.y", "A.y" },
				{ "base", FieldRole::Base, FieldDomain::Pixel, "split.w", "B.y (x codeUnit)" },
				{ "f", FieldRole::Modifier, FieldDomain::Pixel, "split.z", "B.x" },
				{ "N", FieldRole::Modifier, FieldDomain::Block, "blocks.x", "blocks.x" },
				{ "e3", FieldRole::Modifier, FieldDomain::Block, "blocks.y", "blocks.y" },
				{ "e1", FieldRole::Modifier, FieldDomain::Block, "-", "blocks.z" },
				{ "e2", FieldRole::Modifier, FieldDomain::Block, "-", "blocks.w" },
			};
			if (count) *count = sizeof(kTable) / sizeof(kTable[0]);
			return kTable;
		}

		// FNV-1a 64 of the Full-mode pass bodies (rows, columns, quiet, minimum, Wiener: carriage returns skipped), in
		// that order. The Full path must stay byte-identical to the pre-SA2 shaders: a test pins this to the value of
		// the strings at 160987e5. Any intended change of the law bumps GrainSplit::kUnitVersion AND this pin.
		uint64_t FullShaderFingerprint();
	}
}
