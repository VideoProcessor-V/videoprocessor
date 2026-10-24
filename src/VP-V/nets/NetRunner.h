#pragma once

#include "NetPackage.h"

#include <VP-V/core/PipelineSlots.h>
#include <VP-V/core/SlotTiming.h>

#include <map>
#include <string>
#include <vector>

// The NetRunner interface (barrier B1): runs one NetPackage's passes on textures VP owns.
//
// Why an interface: the same net code must serve two places with different owners of the GPU
// state, and a CPU reference must run the identical contract in the tests:
//   - the FRONT END (A, C, B, the 4K refine): the renderer's pl_gpu + its own pl_dispatch, on the
//     entrance's VP-owned textures, once per source frame (B3);
//   - the hybrid scaler's POST_KERNEL hook (D): pl_hook_params carries the gpu and a dispatch
//     for that call; SCALER_SRC is the hook's source, SCALER_OUT its output (AdaptiveDownscale.cpp).
// So the device context is passed to Prepare/Run instead of being owned by the runner.
//
// Implementations: CpuNetExecutor (CpuNetExecutor.h, tests), PlNetRunner (PlNetRunner.h, the
// pl_dispatch shim; a stub until the skeleton lead wires it). C++14, no libplacebo here.
namespace VpNets
{
	// A texture as the runner sees it. `handle` is a pl_tex for PlNetRunner, a CpuTexture* for
	// the CPU executor.
	struct NetTexture
	{
		void* handle = nullptr;
		int width = 0, height = 0;
		int components = 1;
		TexFormat format = TexFormat::Unknown;
		// NAME_mul: the factor that turns a sampled value into the net's normalised value (1 for
		// float and full-range textures; for a 10-bit value MSB-aligned in r16, 65535 / 65472).
		float mul = 1.0f;
		// The picture's rect inside the texture (x0, y0, x1, y1 in texels); all 0 = the whole texture.
		float rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		bool HasRect() const { return rect[2] > rect[0] && rect[3] > rect[1]; }
		bool Valid() const { return handle && width > 0 && height > 0; }
	};

	// The GPU objects of the CALL SITE (front end: the renderer's; POST_KERNEL: pl_hook_params).
	struct NetDeviceContext
	{
		void* gpu = nullptr;        // pl_gpu
		void* dispatch = nullptr;   // pl_dispatch
	};

	// What the GPU (or the CPU executor) can do, for the capability check.
	struct NetCapabilities
	{
		bool fragment = true;
		bool compute = false;
		bool renderableRgba16f = false, renderableRgba32f = false;
		bool storableRgba16f = false, storableRgba32f = false;     // compute passes write storage images
		bool linearRgba16f = false, linearRgba32f = false;         // NAME_tex() bilinear sampling
		int maxTextureSize = 0;                                    // 0 = unknown / unlimited
	};

	struct CapabilityResult
	{
		bool ok = false;
		std::string reason;
		TexFormat intermediate = TexFormat::Unknown;
	};

	// Picks the intermediate format (fp16 by default where the package allows it) and checks the
	// passes can run: compute passes need compute + a storable format, fragment passes a
	// renderable one, NAME_tex() sampling a linear-filterable one.
	inline CapabilityResult CheckCapabilities(const NetPackage& pkg, const NetCapabilities& caps)
	{
		CapabilityResult r;
		bool needCompute = false, needFragment = false;
		for (const NetPass& p : pkg.passes) (p.IsCompute() ? needCompute : needFragment) = true;
		auto fits = [&](bool half)
		{
			if (needFragment && !(half ? caps.renderableRgba16f : caps.renderableRgba32f)) return false;
			if (needCompute && !(half ? caps.storableRgba16f : caps.storableRgba32f)) return false;
			if (pkg.contract.usesLinearSampling && !(half ? caps.linearRgba16f : caps.linearRgba32f)) return false;
			return true;
		};
		if (needCompute && !caps.compute)
		{
			r.reason = "compute shaders unavailable (tile-gated passes)";
			return r;
		}
		if (needFragment && !caps.fragment)
		{
			r.reason = "fragment passes unavailable";
			return r;
		}
		const NetFingerprint& fp = pkg.fingerprint;
		const bool halfOk = fp.intermediate == TexFormat::Rgba16f || (fp.intermediate == TexFormat::Rgba32f && fp.fp16Allowed);
		if (halfOk && fits(true)) { r.ok = true; r.intermediate = TexFormat::Rgba16f; return r; }
		if (fp.intermediate != TexFormat::Rgba16f && fits(false)) { r.ok = true; r.intermediate = TexFormat::Rgba32f; return r; }
		r.reason = std::string("no usable intermediate format (trained ") + FormatName(fp.intermediate) +
			(pkg.contract.usesLinearSampling ? ", needs linear filtering" : "") + ")";
		return r;
	}

	// Per-frame state the net may depend on.
	struct NetState
	{
		bool neighboursValid = true;   // motion vectors valid this frame (vp-vector-validity): A_motion's neighbours
		bool lowLatency = false;       // queue 1: no look-ahead, so no A_motion
	};

	struct NetRunInputs
	{
		std::map<std::string, NetTexture> textures;   // contract input name -> texture (HOOKED, VPNN_PREV, SCALER_SRC, ...)
		std::map<std::string, double> params;         // PARAM labels (NetLabels.h for schema v1)
		NetState state;
		// UpstreamNetsKey() of the nets that ran upstream THIS frame (a per-frame bypass upstream
		// changes it): compared with the key the net was loaded for.
		std::string upstreamNets;
		bool upstreamNetsKnown = false;
	};

	struct NetPrepareParams
	{
		TexFormat intermediate = TexFormat::Rgba16f;  // from CheckCapabilities
		TexFormat output = TexFormat::Unknown;        // Unknown = the hooked input's format; R16Unorm quantises (r16 hand-off)
	};

	// The 8K rule (owner; every exported header): no neural stage for sources above 3840x2160 on
	// either axis; the classical path alone. Checked on the hooked input, before any pass runs.
	inline bool NeuralBypassForSize(int width, int height)
	{
		return width > 3840 || height > 2160;
	}

	// How the inputs are bound for one run, after the contract rules.
	struct ResolvedInputs
	{
		bool ok = false;
		bool bypass = false;            // run nothing: the caller keeps its input (a "Fallbacks engaged" entry)
		std::string reason;
		bool neighbourFallback = false; // A_motion without valid neighbours: prev = next = HOOKED, mask = 0
		std::vector<std::string> missingLabels;   // labels not supplied: the export's defaults are used
	};

	// Validates `in` against the package's binding contract. A_motion's single-frame fallback
	// (all-zero mask) is the net's own trained behaviour, so invalid vectors are not a bypass.
	inline ResolvedInputs ResolveInputs(const NetPackage& pkg, const NetRunInputs& in)
	{
		ResolvedInputs r;
		const BindingContract& c = pkg.contract;
		const char* hooked = c.site == RunSite::PostKernelHook ? "SCALER_OUT" : "HOOKED";
		const auto h = in.textures.find(hooked);
		if (h == in.textures.end() || !h->second.Valid())
		{
			r.reason = std::string("no ") + hooked + " texture";
			return r;
		}
		const NetTexture& ht = h->second;
		// The 8K rule applies to the source the net works on: SCALER_SRC for D.
		const auto src = in.textures.find(c.site == RunSite::PostKernelHook ? "SCALER_SRC" : "HOOKED");
		if (src != in.textures.end() && NeuralBypassForSize(src->second.width, src->second.height))
		{
			r.ok = true;
			r.bypass = true;
			r.reason = "source above 3840x2160 (8K rule)";
			return r;
		}
		for (const std::string& name : c.inputs)
		{
			const bool neighbour = name == "VPNN_PREV" || name == "VPNN_NEXT" || name == "VPNN_MASK";
			const auto it = in.textures.find(name);
			const bool have = it != in.textures.end() && it->second.Valid();
			if (neighbour)
			{
				if (!have || !in.state.neighboursValid) { r.neighbourFallback = true; continue; }
				if (it->second.width != ht.width || it->second.height != ht.height)
				{
					r.reason = name + " is " + std::to_string(it->second.width) + "x" + std::to_string(it->second.height) +
						", the contract needs the hooked size";
					return r;
				}
				if (name == "VPNN_MASK" && it->second.components < 2)
				{
					r.reason = "VPNN_MASK needs 2 components (.x prev, .y next validity)";
					return r;
				}
				continue;
			}
			if (name == "HOOKED" || name == hooked) continue;
			if (!have)
			{
				r.reason = "no " + name + " texture";
				return r;
			}
		}
		for (const std::string& l : c.labels)
			if (!in.params.count(l)) r.missingLabels.push_back(l);
		r.ok = true;
		return r;
	}

	// Low-latency rule (vp-neural-bounded): queue 1 has no look-ahead, so no A_motion; use
	// A_single (or bypass when absent). Normal latency prefers A_motion and falls back to A_single
	// when vectors are invalid only if A_motion is missing (A_motion's zero mask IS its single-frame
	// fallback, so it keeps running).
	enum class RepairChoice { Motion, Single, Bypass };
	inline RepairChoice ChooseRepairVariant(bool lowLatency, bool haveMotion, bool haveSingle)
	{
		if (lowLatency) return haveSingle ? RepairChoice::Single : RepairChoice::Bypass;
		if (haveMotion) return RepairChoice::Motion;
		return haveSingle ? RepairChoice::Single : RepairChoice::Bypass;
	}

	class INetRunner
	{
	public:
		virtual ~INetRunner() {}
		virtual const char* Name() const = 0;
		virtual NetCapabilities Capabilities(const NetDeviceContext& device) const = 0;
		// Compiles/parses the passes; allocation of intermediates may wait for the first Run.
		virtual bool Prepare(const NetDeviceContext& device, const NetPackage& pkg, const NetPrepareParams& params, std::string* error) = 0;
		// Runs every pass. `output` receives the net output (owned by the runner, valid until the next
		// Run/Release). Call ResolveInputs first: a bypass never reaches Run.
		virtual bool Run(const NetDeviceContext& device, const NetRunInputs& in, NetTexture& output, std::string* error) = 0;
		virtual void Release() = 0;

		// The intermediate format the runner will really use for `chosen` (a runner option such as
		// fused fp16 storage may lower it); the fingerprint is checked against this (audit 2026-10-24).
		virtual TexFormat EffectiveIntermediate(const NetPackage&, TexFormat chosen) const { return chosen; }

		// Cost hook: each Run's time is recorded against `slot` (SlotTiming, barrier B4). The CPU
		// executor records wall-clock ms; the GPU runner records timestamp-query ms.
		void SetTiming(SlotTiming::Accumulator* acc, PipelineSlots::Id slot) { timing = acc; timingSlot = slot; }

	protected:
		void RecordTiming(double ms) { if (timing) timing->Record(timingSlot, ms); }
		SlotTiming::Accumulator* timing = nullptr;
		PipelineSlots::Id timingSlot = PipelineSlots::Id::ControlNetRuntime;
	};
}
