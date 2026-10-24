#pragma once

#include "NetFusion.h"
#include "NetRunner.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct pl_hook_params;

// PlNetRunner: INetRunner on libplacebo (barrier B1). Runs an exported net's passes itself, with
// the CALL SITE's pl_gpu + pl_dispatch, on caller-provided pl_tex inputs and VP-owned
// intermediates; never as a libplacebo hook (those only run inside pl_render, in the wrong place).
//
// The shim mirrors libplacebo's custom_mpv.c (hook_hook / bind_pass_tex), through the public
// pl_shader_custom API, per pass:
//   - each //!BIND name N (HOOKED = the hooked stage) is a SAMPLED_TEX descriptor "N_raw" (LINEAR,
//     CLAMP) plus the macros N_pos, N_size (textureSize), N_pt, N_mul, N_tex(pos), N_texOff(off);
//   - each //!PARAM is a shader variable (int or float) from NetRunInputs::params;
//   - fragment pass: "color = hook();" + pl_dispatch_finish into the pass texture; compute pass:
//     out_image as a STORAGE_IMG descriptor, "hook();", pl_dispatch_compute(ceil(size / block));
//   - out size = roundf(RPN) (EvalSizeExpr); a pass without SAVE replaces the hooked stage.
// Intermediates use the format the capability check picked (fp16 where the fingerprint allows,
// else rgba32f), sized by component count (r / rg / rgba), pooled across frames (recreated only
// when the size or format changes). Cost: one pl_timer per pass; the sum of the latest results is
// recorded into SlotTiming at each Run.
//
// Entry modes (same object, same Run):
//   - front end: NetDeviceContext{renderer gpu, VP's dispatch}, inputs = VP-owned textures;
//   - the hybrid scaler's POST_KERNEL (PL_HOOK_SIG_TEX): DeviceFromHook(params) and
//     RunInPostKernelHook(params, scalerSrc, out): SCALER_OUT = params->tex.
// The output texture is owned by the runner and valid until its next Run or Release.
namespace VpNets
{
	class PlNetRunner : public INetRunner
	{
	public:
		PlNetRunner();
		~PlNetRunner() override;

		const char* Name() const override { return "pl_dispatch"; }
		NetCapabilities Capabilities(const NetDeviceContext& device) const override;
		bool Prepare(const NetDeviceContext& device, const NetPackage& pkg, const NetPrepareParams& params, std::string* error) override;
		bool Run(const NetDeviceContext& device, const NetRunInputs& in, NetTexture& output, std::string* error) override;
		void Release() override;

		// POST_KERNEL entry: SCALER_OUT is the hook's texture; SCALER_SRC is the scaler's source,
		// which the hybrid module holds. `mulOut`/`mulSrc`: NetTexture::mul of the two.
		bool RunInPostKernelHook(const pl_hook_params* params, void* scalerSrcTex, NetTexture& output, std::string* error,
			float mulOut = 1.0f, float mulSrc = 1.0f);
		static NetDeviceContext DeviceFromHook(const pl_hook_params* params);

		// Fused compute layers (NetFusion.h): replaces each run of exported conv-group passes with
		// one compute pass per layer, same weights and maths. Set before Prepare. fp16Storage keeps
		// the layer outputs and the shared tile in fp16 (a side option; the default keeps fp32).
		void SetFusion(bool enabled, FusedKind kind = FusedKind::Direct, bool fp16Storage = false)
		{
			fusion = enabled;
			fusedKind = kind;
			fusedFp16 = fp16Storage;
		}
		size_t FusedLayers() const;   // layers running fused (after Prepare)

		// Test knob: pretend a format is unavailable (the visible-bypass path).
		void DisableFormat(TexFormat f) { disabled.push_back(f); }

		// Test access: a pass texture by SAVE name ("" = the net output); a pl_tex or null.
		void* Intermediate(const std::string& name) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
		int RunFusedLayer(const ConvLayer& layer, const FusedShader& shader, void* dispatch, void* timer, std::string* error);
		std::vector<TexFormat> disabled;
		bool fusion = false;
		FusedKind fusedKind = FusedKind::Direct;
		bool fusedFp16 = false;
	};
}
