#pragma once

// Shared helpers for the net runner tests (CPU and GPU): paths, the synthetic package, the
// PyTorch reference cases. Test-only; included by NetRunnerTests.cpp and PlNetRunnerGpuTests.cpp.

#include "CppUnitTest.h"

#include <VP-V/nets/CpuNetExecutor.h>
#include <VP-V/nets/NetSettingsHash.h>
#include <VP-V/nets/NetSha256.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace NetRunnerTestsNs
{
	using namespace VpNets;
	using namespace Microsoft::VisualStudio::CppUnitTestFramework;

	inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
	inline void Log(const std::string& s) { Logger::WriteMessage((s + "\n").c_str()); }

	// ------------------------------------------------------------------ host texel decoding
	// IEEE half bits -> float (NaN folded to inf: never expected in net textures).
	inline float HalfBitsToFloat(uint16_t hb)
	{
		const int e = (hb >> 10) & 31, m = hb & 1023;
		const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? INFINITY : std::ldexp(static_cast<float>(m + 1024), e - 25);
		return (hb & 0x8000) ? -v : v;
	}

	// A downloaded texture's host bytes -> CpuTexture. `hostBits` is the format's host_bits[0], NOT
	// its component_depth: libplacebo's D3D11 "r16f"/"rg16f"/"rgba16f" are EMULATED formats that
	// store R16_FLOAT on the GPU but transfer 32-bit floats to and from the host (component_depth 16,
	// host_bits 32, texel_size 4 per component; the real half host layout is "r16hf"). Decoding those
	// bytes as halves read the low 16 bits of each fp32 (mantissa bits only) and produced the
	// "implausible" fp16 gate logits (2.0, -0.0078, -2, -512) of 2026-10-24 (fix/net-fp16-gate).
	inline CpuTexture DecodeHostTexels(const uint8_t* raw, int w, int h, int comps, int hostBits)
	{
		CpuTexture out(w, h, comps);
		for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
			for (int c = 0; c < comps; ++c)
			{
				float v;
				if (hostBits == 16)
				{
					uint16_t hb;
					std::memcpy(&hb, raw + (i * comps + c) * 2, 2);
					v = HalfBitsToFloat(hb);
				}
				else std::memcpy(&v, raw + (i * comps + c) * 4, 4);
				out.rgba[i * 4 + c] = v;
			}
		return out;
	}

	// ------------------------------------------------------------------ paths and files
	inline std::string DataDir()
	{
		std::string f = __FILE__;
		const size_t slash = f.find_last_of("/\\");
		return f.substr(0, slash + 1) + "data/";
	}

	// Where exports may be: %VP_NETS_EXPORT_DIR% (';'-separated), the round-1 kit drive, the
	// laptop's smoke export. Read only.
	inline std::vector<std::string> ExportDirs()
	{
		std::vector<std::string> dirs;
		char* v = nullptr;
		size_t n = 0;
		if (_dupenv_s(&v, &n, "VP_NETS_EXPORT_DIR") == 0 && v)
		{
			std::string list = v;
			free(v);
			size_t a = 0;
			while (a <= list.size())
			{
				const size_t b = list.find(';', a);
				const std::string d = list.substr(a, b == std::string::npos ? std::string::npos : b - a);
				if (!d.empty()) dirs.push_back(d);
				if (b == std::string::npos) break;
				a = b + 1;
			}
		}
		dirs.push_back("E:/VP-NN-TrainKit/work/staging");
		dirs.push_back("D:/nn-corpus/VP-NN-TrainKit/smoke/staging");
		return dirs;
	}

	inline std::vector<float> ReadF32(const std::string& path, size_t count)
	{
		std::vector<float> v(count, 0.0f);
		std::ifstream f(path, std::ios::binary);
		if (f) f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(count * sizeof(float)));
		return v;
	}


	// ------------------------------------------------------------------ the synthetic package
	// The synthetic export's fingerprint fields; its id is computed like the kit's (h over them).
	inline std::vector<std::pair<std::string, std::string>> SyntheticFields()
	{
		return {
			{ "vp_commit", "462a95bc test" }, { "renderer_dll_sha256", "2e84598c7612762c" }, { "rebase_settings", "r1" },
			{ "grain_split", "g1" }, { "stage_order", "s1" }, { "scaler_blend_settings", "b1" }, { "bounds", "n1" },
			{ "label_schema", "vpnn-labels-v1" }, { "stage_contract", "test contract | A: in = luma" },
			{ "texture_formats", "all net intermediates rgba32f (test); re-base pass outputs r16; x2 H->V hand-off r16" },
			{ "upstream_nets", "" }, { "input_stage", "A: test input" },
		};
	}

	inline std::string SyntheticId()
	{
		KitHash::Value o = KitHash::Value::Obj();
		for (const auto& kv : SyntheticFields()) o.fields[kv.first] = KitHash::Value::Str(kv.second);
		return KitHash::H(o);
	}

	// Exporter format, three passes: features (fragment), a ceil-half-size 2x2 average (compute,
	// RPN size), the output (mirrored taps, a mat4, a PARAM, a swizzle compound assignment).
	inline std::string SyntheticShader()
	{
		std::string header =
			"// VP-NN T_tiny: test net in the exporter's format\n"
			"// HOOK LUMA; out = HOOKED + gain * ((M * f).x - half)\n"
			"// VPNN pipeline fingerprint: " + SyntheticId() + " input_stage=A: test input\n"
			"// VP must compare it with the running pipeline's and bypass this net (log line) on a mismatch.\n";
		for (const auto& kv : SyntheticFields())
			if (kv.first != "input_stage") header += "//   " + kv.first + ": " + kv.second + "\n";
		return header +
			"//!PARAM vpnn_gain\n"
			"//!TYPE float\n"
			"//!MINIMUM 0.0\n"
			"//!MAXIMUM 1.0\n"
			"0.25\n"
			"\n"
			"//!DESC T features\n"
			"//!HOOK LUMA\n"
			"//!BIND HOOKED\n"
			"//!SAVE TF0\n"
			"//!COMPONENTS 4\n"
			"\n"
			"vec4 hook() {\n"
			"    float c = HOOKED_texOff(vec2(0.0)).x;\n"
			"    return vec4(c, c * c, 0.0, 0.0);\n"
			"}\n"
			"\n"
			"//!DESC T half-size average (tiles)\n"
			"//!HOOK LUMA\n"
			"//!BIND TF0\n"
			"//!COMPUTE 16 16\n"
			"//!SAVE TH\n"
			"//!WIDTH HOOKED.w 2 / 0.25 +\n"
			"//!HEIGHT HOOKED.h 2 / 0.25 +\n"
			"//!COMPONENTS 1\n"
			"\n"
			"void hook() {\n"
			"    ivec2 P = ivec2(gl_GlobalInvocationID.xy);\n"
			"    if (any(greaterThanEqual(P, imageSize(out_image)))) return;\n"
			"    float s = 0.0, n = 0.0;\n"
			"    for (int dy = 0; dy < 2; dy++)\n"
			"        for (int dx = 0; dx < 2; dx++) {\n"
			"            ivec2 q = P * 2 + ivec2(dx, dy);\n"
			"            if (q.x < int(TF0_size.x) && q.y < int(TF0_size.y)) { s += texelFetch(TF0_raw, q, 0).x * TF0_mul; n += 1.0; }\n"
			"        }\n"
			"    imageStore(out_image, P, vec4(s / max(n, 1.0), 0.0, 0.0, 0.0));\n"
			"}\n"
			"\n"
			"//!DESC T output\n"
			"//!HOOK LUMA\n"
			"//!BIND HOOKED\n"
			"//!BIND TF0\n"
			"//!BIND TH\n"
			"//!COMPONENTS 1\n"
			"\n"
			"ivec2 mi(ivec2 p, ivec2 s) { p = abs(p); return (s - 1) - abs((s - 1) - p); }\n"
			"const float K[2] = float[2](1.0, 0.5);\n"
			"vec4 hook() {\n"
			"    ivec2 o = ivec2(gl_FragCoord.xy);\n"
			"    const mat4 M = mat4(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0);\n"
			"    vec4 f = texelFetch(TF0_raw, mi(o + ivec2(1, 0), ivec2(TF0_size)), 0) * TF0_mul;\n"
			"    float h = texelFetch(TH_raw, o / 2, 0).x * TH_mul;\n"
			"    vec4 c = HOOKED_texOff(vec2(0.0));\n"
			"    c.x += vpnn_gain * ((M * f).x - h) * K[o.x % 2 == 0 ? 0 : 1];\n"
			"    return c;\n"
			"}\n";
	}

	inline NetPackage SyntheticPackage()
	{
		ParseResult r = ParseShaderText(SyntheticShader(), "T_tiny");
		Assert::IsTrue(r.ok, W(r.error).c_str());
		return r.package;
	}

	inline CpuTexture SyntheticPlane(int w, int h)
	{
		CpuTexture t(w, h, 1);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				t.rgba[(static_cast<size_t>(y) * w + x) * 4] = 0.2f + 0.05f * ((x * 7 + y * 3) % 11);
		return t;
	}

	// The synthetic net's maths written directly (the oracle for the interpreter).
	inline float SyntheticExpected(const CpuTexture& in, int x, int y, float gain)
	{
		const int w = in.width, h = in.height;
		auto mirror = [](int p, int s) { p = std::abs(p); return (s - 1) - std::abs((s - 1) - p); };
		const float c1 = in.At(mirror(x + 1, w), y);
		const float mf = 1.0f * c1 + 5.0f * c1 * c1;   // (M * f).x, column-major
		const int hx = x / 2, hy = y / 2;
		float s = 0.0f, n = 0.0f;
		for (int dy = 0; dy < 2; ++dy)
			for (int dx = 0; dx < 2; ++dx)
			{
				const int qx = hx * 2 + dx, qy = hy * 2 + dy;
				if (qx < w && qy < h) { s += in.At(qx, qy); n += 1.0f; }
			}
		const float half = s / std::max(n, 1.0f);
		return in.At(x, y) + gain * (mf - half) * (x % 2 == 0 ? 1.0f : 0.5f);
	}

	// ------------------------------------------------------------------ reference cases
	struct RefCase
	{
		std::string name;
		Json desc;
		NetPackage pkg;
		std::vector<CpuTexture> textures;
		NetRunInputs inputs;
		std::vector<float> expected;
		int outW = 0, outH = 0;
	};

	// Loads tests/data/<name> and its export; false (with the reason in *skip) when the export is
	// not available here or differs from the one the case was made from.
	inline bool LoadCase(const std::string& name, RefCase& c, std::string* skip)
	{
		c = RefCase();
		c.name = name;
		std::string text, err;
		Assert::IsTrue(ReadTextFile(DataDir() + name + "/case.json", text), W("missing reference case " + name).c_str());
		Assert::IsTrue(ParseJson(text, c.desc, &err), W(err).c_str());
		const std::string shaderFile = c.desc["shader"].text;
		std::string shader;
		bool found = false;
		std::string exportDir;
		for (const std::string& dir : ExportDirs())
		{
			std::string candidate;
			if (ReadTextFile(dir + "/shaders/" + shaderFile, candidate) && Sha256Hex(candidate) == c.desc["shader_sha256"].text)
			{
				shader = candidate;
				exportDir = dir;
				found = true;
				break;
			}
		}
		if (!found)
		{
			*skip = "no export here matches the " + shaderFile + " this case was made from";
			return false;
		}
		if (c.desc.Has("text_patch"))
		{
			const std::string from = c.desc["text_patch"]["from"].text, to = c.desc["text_patch"]["to"].text;
			const size_t at = shader.find(from);
			Assert::IsTrue(at != std::string::npos && shader.find(from, at + 1) == std::string::npos, W("patch target not unique: " + from).c_str());
			shader.replace(at, from.size(), to);
		}
		ParseResult pr = ParseShaderText(shader, shaderFile.substr(0, shaderFile.size() - 5));
		Assert::IsTrue(pr.ok, W(pr.error).c_str());
		c.pkg = pr.package;
		// The export json's fingerprint (fp16 check, complete fields), when it is beside the shader:
		// export_<stem>.json, or export_<stem without _h / _v>.json for the two-pass nets.
		{
			std::string stem = c.pkg.name, jtext, jerr;
			if (!ReadTextFile(exportDir + "/export_" + stem + ".json", jtext) && stem.size() > 2 && stem[stem.size() - 2] == '_')
				ReadTextFile(exportDir + "/export_" + stem.substr(0, stem.size() - 2) + ".json", jtext);
			Json ej;
			if (!jtext.empty() && ParseJson(jtext, ej, &jerr))
				Assert::IsTrue(MergeExportFingerprint(c.pkg, ej, &jerr), W(jerr).c_str());
		}
		const Json& binds = c.desc["bindings"];
		c.textures.reserve(binds.items.size());
		for (const Json& b : binds.items)
		{
			const int w = static_cast<int>(b["width"].number), h = static_cast<int>(b["height"].number), comps = static_cast<int>(b["components"].number);
			const std::vector<float> v = ReadF32(DataDir() + name + "/" + b["file"].text, static_cast<size_t>(w) * h * comps);
			c.textures.push_back(CpuTexture::FromPlane(w, h, comps, v.data()));
			c.inputs.textures[b["name"].text] = c.textures.back().Ref();
		}
		for (const auto& kv : c.desc["params"].fields) c.inputs.params[kv.first] = kv.second.number;
		c.outW = static_cast<int>(c.desc["output"]["width"].number);
		c.outH = static_cast<int>(c.desc["output"]["height"].number);
		c.expected = ReadF32(DataDir() + name + "/expected.f32", static_cast<size_t>(c.outW) * c.outH);
		return true;
	}

	inline double MaxDiffCodes(const CpuTexture& out, const std::vector<float>& ref, int w, int h)
	{
		double m = 0.0;
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				m = std::max(m, static_cast<double>(std::fabs(out.At(x, y) - ref[static_cast<size_t>(y) * w + x])));
		return m * 1023.0;
	}

	inline bool RunCase(RefCase& c, TexFormat fmt, CpuNetExecutor& ex, std::string* err)
	{
		NetPrepareParams pp;
		pp.intermediate = fmt;
		if (!ex.Prepare(NetDeviceContext(), c.pkg, pp, err)) return false;
		NetTexture out;
		return ex.Run(NetDeviceContext(), c.inputs, out, err);
	}

	// data/<set>/<case>: round1 = the trained round-1 export (E:), smoke = the laptop's smoke export.
	const char* const kSets[] = { "round1", "smoke" };
	const char* const kCases[] = { "C_rebase_refine_h", "C_rebase_refine_v", "D_hybrid_refine", "A_single_small",
		"A_single_small_open", "A_motion_small", "A_motion_small_open" };

}
