#pragma once

#include "GlslMini.h"
#include "NetRunner.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// The CPU reference executor: runs an exported net's passes on the CPU, in 32-bit float, by
// interpreting the same GLSL the GPU will compile (GlslMini.h).
//
// Why: it proves the package parsing (passes, sizes, bindings, PARAM labels, weights) and the pass
// maths against the TrainKit's PyTorch reference BEFORE the GPU wiring (barrier B1), and it stays
// as the oracle for the pl_dispatch runner's GPU tests (same inputs -> compare). Slow: test sizes
// only. It implements INetRunner so tests exercise the exact contract the renderer will use.
//
// Semantics follow libplacebo's custom_mpv.c: NAME_tex = bilinear/clamp, NAME_pos = this pixel's
// normalised position, NAME_size = textureSize, out size = roundf(RPN), a pass without SAVE
// replaces the hooked stage, a 1/2-component texture reads back as (r, g, 0, 1).
// Intermediate formats are emulated: rgba16f rounds every stored value to half precision.
namespace VpNets
{
	struct CpuTexture
	{
		int width = 0, height = 0;
		int components = 4;
		std::vector<float> rgba;   // 4 floats per texel (unused channels: g=b=0, a=1)

		CpuTexture() {}
		CpuTexture(int w, int h, int comps) : width(w), height(h), components(comps), rgba(static_cast<size_t>(w) * h * 4, 0.0f)
		{
			for (size_t i = 0; i < rgba.size(); i += 4) rgba[i + 3] = 1.0f;
		}
		// From a packed plane of `comps` floats per pixel.
		static CpuTexture FromPlane(int w, int h, int comps, const float* data)
		{
			CpuTexture t(w, h, comps);
			for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
				for (int c = 0; c < comps && c < 4; ++c)
					t.rgba[i * 4 + c] = data[i * comps + c];
			return t;
		}
		float At(int x, int y, int c = 0) const { return rgba[(static_cast<size_t>(y) * width + x) * 4 + c]; }
		NetTexture Ref(TexFormat format = TexFormat::R32f)
		{
			NetTexture t;
			t.handle = this;
			t.width = width;
			t.height = height;
			t.components = components;
			t.format = format;
			return t;
		}
	};

	// IEEE half rounding (round to nearest even), as an rgba16f store does.
	float RoundToHalf(float v);

	class CpuNetExecutor : public INetRunner
	{
	public:
		CpuNetExecutor();
		~CpuNetExecutor() override;

		const char* Name() const override { return "cpu-reference"; }
		NetCapabilities Capabilities(const NetDeviceContext& device) const override;
		bool Prepare(const NetDeviceContext& device, const NetPackage& pkg, const NetPrepareParams& params, std::string* error) override;
		bool Run(const NetDeviceContext& device, const NetRunInputs& in, NetTexture& output, std::string* error) override;
		void Release() override;

		// Test access: an intermediate (by SAVE name) or the output of the last Run.
		const CpuTexture* Texture(const std::string& name) const;
		const CpuTexture& Output() const { return output; }
		size_t PassCount() const { return programs.size(); }

	private:
		bool RunPass(size_t index, const NetRunInputs& in, std::string* error);
		const CpuTexture* Lookup(const std::string& name, const std::string& hook) const;

		NetPackage pkg;
		NetPrepareParams params;
		std::vector<std::unique_ptr<Glsl::Program>> programs;
		std::map<std::string, CpuTexture> saved;            // SAVE textures of this run
		std::map<std::string, CpuTexture> stageWrites;      // passes without SAVE (they replace their stage)
		std::map<std::string, const CpuTexture*> stages;    // hook stage -> current texture
		std::map<std::string, const CpuTexture*> inputs;    // contract inputs of this run
		CpuTexture zeroMask;                                // A_motion's single-frame fallback mask
		CpuTexture output;
		bool prepared = false;
	};
}
