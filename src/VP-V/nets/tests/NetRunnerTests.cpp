#include "CppUnitTest.h"

#include <VP-V/nets/CpuNetExecutor.h>
#include <VP-V/nets/NetFingerprint.h>
#include <VP-V/nets/NetLabels.h>
#include <VP-V/nets/NetSlot.h>
#include <VP-V/nets/PlNetRunner.h>
#include <VP-V/nets/RgbLumaDelta.h>
#include <VP-V/nets/NetSha256.h>
#include <VP-V/nets/NetSettingsHash.h>
#include <VP-V/core/SlotRegistry.h>

#include "NetTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The VP-side net runner groundwork (barrier B1): package parsing, the fingerprint check and its
// "Fallbacks engaged" entry, the runner contract, the CPU reference executor against the TrainKit's
// PyTorch outputs, the RGB luma delta. CPU only.
//
// Real exports: read from %VP_NETS_EXPORT_DIR% (default E:\VP-NN-TrainKit\work\staging, read only).
// The PyTorch reference cases (tests/data, written by tests/make_reference.py) record each
// shader's sha256; a missing or different export logs SKIPPED for that case (the weights are not
// committed). The synthetic package below always runs.
namespace NetRunnerTestsNs
{
	using namespace VpNets;

	TEST_CLASS(NetRunnerTests)
	{
	public:
		// ------------------------------------------------------------ parsing
		TEST_METHOD(JsonReadsExportShapes)
		{
			Json j;
			std::string err;
			Assert::IsTrue(ParseJson("{\"shaders\": [\"a.glsl\", \"b.glsl\"], \"fp16_check\": {\"fp16_within_1_lsb\": false, \"x\": -1.5e-3},"
				" \"s\": \"C:\\\\dev\\u000b\", \"n\": null}", j, &err), W(err).c_str());
			Assert::AreEqual(size_t(2), j["shaders"].items.size());
			Assert::AreEqual(std::string("b.glsl"), j["shaders"].items[1].text);
			Assert::IsFalse(j["fp16_check"]["fp16_within_1_lsb"].boolean);
			Assert::AreEqual(-1.5e-3, j["fp16_check"]["x"].number, 1e-12);
			Assert::AreEqual(std::string("C:\\dev\v"), j["s"].text);
			Assert::IsFalse(ParseJson("{\"a\": }", j, &err));
			Assert::IsTrue(ParseJson("[-Infinity, Infinity, NaN, -2]", j, &err), W(err).c_str());
			Assert::IsTrue(std::isinf(j.items[0].number) && j.items[0].number < 0);
			Assert::AreEqual(-2.0, j.items[3].number, 0.0);
		}

		TEST_METHOD(ParsesTheSyntheticPackage)
		{
			const NetPackage p = SyntheticPackage();
			Assert::AreEqual(size_t(3), p.passes.size());
			Assert::AreEqual(std::string("T features"), p.passes[0].desc);
			Assert::AreEqual(std::string("TF0"), p.passes[0].save);
			Assert::AreEqual(16, p.passes[1].computeX);
			Assert::AreEqual(std::string("HOOKED.w 2 / 0.25 +"), p.passes[1].width);
			Assert::AreEqual(1, p.passes[1].components);
			Assert::IsTrue(p.passes[2].save.empty(), L"the last pass writes the hooked stage");
			Assert::AreEqual(size_t(2), p.savedTextures.size());
			Assert::AreEqual(size_t(1), p.contract.inputs.size());
			Assert::AreEqual(std::string("HOOKED"), p.contract.inputs[0]);
			Assert::AreEqual(std::string("LUMA"), p.contract.outputStage);
			Assert::IsTrue(p.contract.site == RunSite::FrontEnd);
			Assert::IsTrue(p.contract.tileGated, L"a compute pass counts as tile gated");
			Assert::IsFalse(p.contract.neighbours);
			Assert::AreEqual(size_t(1), p.params.size());
			Assert::AreEqual(0.25, p.params[0].defaultValue, 1e-12);
			Assert::AreEqual(std::string("vpnn_gain"), p.contract.labels[0]);
			Assert::IsTrue(p.role == NetRole::RepairDelivered, L"input_stage 'A:' -> repair");
			Assert::IsTrue(SlotOf(p.role) == PipelineSlots::Id::RepairEncoding);
			const NetFingerprint& fp = p.fingerprint;
			Assert::AreEqual(SyntheticId(), fp.id);
			Assert::IsTrue(fp.idVerified, L"the header's id recomputes from its fields");
			Assert::AreEqual(std::string("vpnn-labels-v1"), fp.labelSchema);
			Assert::AreEqual(std::string(""), fp.passOrder, L"B's 'x2 H->V hand-off' wording gives A no pass order");
			Assert::IsTrue(fp.intermediate == TexFormat::Rgba32f);
			Assert::IsTrue(fp.handoff == TexFormat::Unknown, L"the r16 hand-off belongs to the separable chain");
			Assert::AreEqual(std::string("r1"), fp.Field("rebase_settings"));
			Assert::IsTrue(fp.Has("upstream_nets") && fp.Field("upstream_nets").empty(), L"an empty, declared field");
			Assert::AreEqual(size_t(64), p.shaderSha256.size());
			Assert::AreEqual(std::string("T_tiny@") + p.shaderSha256.substr(0, 16), p.Identity());
		}

		TEST_METHOD(RejectsMalformedPackages)
		{
			Assert::IsFalse(ParseShaderText("// nothing\n", "x").ok, L"no passes");
			Assert::IsFalse(ParseShaderText("//!DESC a\n//!HOOK LUMA\n//!BIND HOOKED\n//!SAVE T\n//!COMPONENTS 1\n\nvec4 hook() { return vec4(0.0); }\n", "x").ok,
				L"a net whose last pass saves has no output");
			Assert::IsFalse(ParseShaderText("//!DESC a\n//!HOOK LUMA\n//!OFFSET 1 1\n//!COMPONENTS 1\n\nvec4 hook() { return vec4(0.0); }\n", "x").ok,
				L"directives outside the exporter's subset are refused, not ignored");
			Assert::IsFalse(ParseShaderText("//!DESC a\n//!HOOK LUMA\n//!WIDTH HOOKED.w\n//!COMPONENTS 1\n\nvec4 hook() { return vec4(0.0); }\n", "x").ok,
				L"WIDTH without HEIGHT");
		}

		TEST_METHOD(SizeExpressionsRoundLikeLibplacebo)
		{
			auto lookup = [](const std::string& n, char wh, int& v) { if (n != "HOOKED") return false; v = wh == 'w' ? 25 : 24; return true; };
			int r = 0;
			std::string err;
			Assert::IsTrue(EvalSizeExpr("HOOKED.w 8 / 0.4375 +", lookup, r, &err));
			Assert::AreEqual(4, r, L"ceil(25 / 8) through roundf");
			Assert::IsTrue(EvalSizeExpr("HOOKED.h 8 / 0.4375 +", lookup, r, &err));
			Assert::AreEqual(3, r, L"ceil(24 / 8)");
			Assert::IsTrue(EvalSizeExpr("HOOKED.w 2 *", lookup, r, &err));
			Assert::AreEqual(50, r);
			Assert::IsTrue(EvalSizeExpr("HOOKED.w 2 /", lookup, r, &err));
			Assert::AreEqual(13, r, L"12.5 rounds half away from zero");
			Assert::IsFalse(EvalSizeExpr("OTHER.w", lookup, r, &err));
			Assert::IsFalse(EvalSizeExpr("HOOKED.w +", lookup, r, &err));
		}

		// ------------------------------------------------------------ interpreter + CPU executor
		TEST_METHOD(GlslMiniEvaluatesTheExporterSubset)
		{
			Glsl::Program prog;
			std::string err;
			const char* src =
				"const vec4 KV[3] = vec4[3](vec4(1.0), vec4(2.0, 3.0, 4.0, 5.0), vec4(0.0));\n"
				"int twice(int v) { return v * 2; }\n"
				"vec4 hook() {\n"
				"    ivec2 a = ivec2(7, -7);\n"
				"    ivec2 q = a / 2;\n"                      // integer division truncates
				"    int m = 7 % 3;\n"
				"    vec4 c = vec4(0.5);\n"
				"    c.y += 1.0; c.zw = vec2(3.0, 4.0);\n"
				"    float acc = 0.0;\n"
				"    for (int i = 0; i < 4; i++) { if (i == 2) continue; acc += float(i); }\n"
				"    mat4 M = mat4(2.0);\n"
				"    vec4 mv = M * vec4(1.0, 2.0, 3.0, 4.0);\n"
				"    bool b = acc > 3.0 && !(m == 0);\n"
				"    float t = b ? KV[1].y : -1.0;\n"
				"    return vec4(float(q.x * 10 + q.y), float(twice(m)) + acc, c.y + t + mv.w, clamp(tanh(0.0) + dot(vec2(1.0, 2.0), vec2(3.0, 4.0)), 0.0, 10.0));\n"
				"}\n";
			Assert::IsTrue(prog.Parse(src, &err), W(err).c_str());
			Glsl::Context ctx;
			ctx.externs.resize(prog.Externs().size());
			Glsl::Value r;
			Assert::IsTrue(prog.Call("hook", ctx, r), W(ctx.error).c_str());
			Assert::AreEqual(27.0f, r.f[0], L"7/2 = 3, -7/2 = -3 -> 30 - 3");
			Assert::AreEqual(6.0f, r.f[1], L"twice(1) + (0 + 1 + 3)");
			Assert::AreEqual(1.5f + 3.0f + 8.0f, r.f[2]);
			Assert::AreEqual(10.0f, r.f[3], L"dot 11 clamped to 10");
			// texelFetch outside the texture reads 0 (D3D11 robust access), not the clamped edge.
			Glsl::Program fetch;
			Assert::IsTrue(fetch.Parse("vec4 hook() { return texelFetch(T_raw, ivec2(-1, 0), 0) + texelFetch(T_raw, ivec2(0, 0), 0); }", &err), W(err).c_str());
			Glsl::Context fc;
			fc.externs.resize(fetch.Externs().size());
			const float px[4] = { 0.5f, 0.0f, 0.0f, 1.0f };
			Glsl::TexView tv;
			tv.width = 1;
			tv.height = 1;
			tv.rgba = px;
			fc.textures.push_back(tv);
			fc.externs[0].base = Glsl::Base::Sampler;
			fc.externs[0].n = 1;
			fc.externs[0].i[0] = 0;
			Assert::IsTrue(fetch.Call("hook", fc, r), W(fc.error).c_str());
			Assert::AreEqual(0.5f, r.f[0], L"only the in-bounds fetch counts");
		}

		TEST_METHOD(CpuExecutorRunsTheSyntheticNetExactly)
		{
			const NetPackage pkg = SyntheticPackage();
			CpuNetExecutor ex;
			std::string err;
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			Assert::IsTrue(ex.Prepare(NetDeviceContext(), pkg, pp, &err), W(err).c_str());
			CpuTexture in = SyntheticPlane(7, 5);   // odd sizes: the ceil-half pass and the mirror edge
			NetRunInputs inputs;
			inputs.textures["HOOKED"] = in.Ref();
			inputs.params["vpnn_gain"] = 0.125;
			NetTexture out;
			Assert::IsTrue(ex.Run(NetDeviceContext(), inputs, out, &err), W(err).c_str());
			Assert::AreEqual(7, out.width);
			Assert::AreEqual(5, out.height);
			const CpuTexture* half = ex.Texture("TH");
			Assert::IsNotNull(half);
			Assert::AreEqual(4, half->width, L"roundf(7 / 2 + 0.25)");
			Assert::AreEqual(3, half->height);
			for (int y = 0; y < 5; ++y)
				for (int x = 0; x < 7; ++x)
					Assert::AreEqual(SyntheticExpected(in, x, y, 0.125f), ex.Output().At(x, y), 1e-6f);
			// A missing label uses the export's default (0.25), reported by ResolveInputs.
			inputs.params.clear();
			Assert::AreEqual(size_t(1), ResolveInputs(pkg, inputs).missingLabels.size());
			Assert::IsTrue(ex.Run(NetDeviceContext(), inputs, out, &err), W(err).c_str());
			Assert::AreEqual(SyntheticExpected(in, 3, 2, 0.25f), ex.Output().At(3, 2), 1e-6f);
		}

		TEST_METHOD(HalfPrecisionEmulation)
		{
			Assert::AreEqual(1.0f, RoundToHalf(1.0f));
			Assert::AreEqual(1.0f, RoundToHalf(1.0f + 1.0f / 4096.0f), L"below half an fp16 ulp at 1.0");
			Assert::AreEqual(1.0f + 1.0f / 1024.0f, RoundToHalf(1.0f + 3.0f / 4096.0f));
			Assert::AreEqual(65504.0f, RoundToHalf(65504.0f));
			Assert::IsTrue(std::isinf(RoundToHalf(70000.0f)));
			Assert::AreEqual(0.0f, RoundToHalf(1e-9f));
		}

		TEST_METHOD(TimingHookRecordsAgainstTheSlot)
		{
			const NetPackage pkg = SyntheticPackage();
			CpuNetExecutor ex;
			SlotTiming::Accumulator acc;
			ex.SetTiming(&acc, SlotOf(pkg.role));
			std::string err;
			Assert::IsTrue(ex.Prepare(NetDeviceContext(), pkg, NetPrepareParams(), &err), W(err).c_str());
			CpuTexture in = SyntheticPlane(8, 4);
			NetRunInputs inputs;
			inputs.textures["HOOKED"] = in.Ref();
			NetTexture out;
			Assert::IsTrue(ex.Run(NetDeviceContext(), inputs, out, &err), W(err).c_str());
			Assert::AreEqual(uint64_t(1), acc.Samples(PipelineSlots::Id::RepairEncoding));
		}

		// ------------------------------------------------------------ real exports
		TEST_METHOD(ParsesTheRealExports)
		{
			const char* nets[] = { "A_single_small", "A_motion_small", "C_rebase_refine", "D_hybrid_refine", "B_x2" };
			int parsed = 0;
			for (const std::string& dir : ExportDirs())
			for (const char* net : nets)
			{
				std::vector<NetPackage> pkgs;
				std::string err, probe;
				const std::string json = dir + "/export_" + net + ".json";
				if (!ReadTextFile(json, probe)) continue;
				Json ej;
				Assert::IsTrue(ParseJson(probe, ej, &err), W(err).c_str());
				const bool fp16 = ej["fp16_check"]["fp16_within_1_lsb"].boolean;
				Assert::IsTrue(LoadExport(json, std::string(), pkgs, &err), W(err).c_str());
				Assert::AreEqual(ej["shaders"].items.size(), pkgs.size());
				for (const NetPackage& p : pkgs)
				{
					++parsed;
					const std::string n = p.name;
					Log(dir + ": " + n + ": " + std::to_string(p.passes.size()) + " passes, " + RoleName(p.role) + ", fingerprint " +
						p.fingerprint.id + ", " + std::to_string(p.weightLiterals) + " float literals");
					Assert::AreEqual(ej["fingerprint"]["fingerprint"].text, p.fingerprint.id, W(n).c_str());
					Assert::AreEqual(fp16, p.fingerprint.fp16Allowed, W(n).c_str());
					const bool separable = n[0] == 'B' || n[0] == 'C';
					Assert::AreEqual(std::string(separable ? "H>V" : ""), p.fingerprint.passOrder, W(n).c_str());
					Assert::IsTrue(p.fingerprint.intermediate == TexFormat::Rgba32f, W(n).c_str());
					Assert::IsTrue(p.fingerprint.handoff == (separable ? TexFormat::R16Unorm : TexFormat::Unknown), W(n).c_str());
					Assert::IsTrue(p.fingerprint.idVerified, W(n + ": the id recomputes from the export's fields").c_str());
					Assert::IsFalse(p.fingerprint.Field("stage_contract").empty(), W(n).c_str());
					// The upstream key the design needs is not in the round-1 exports (U3).
					Assert::IsTrue(p.fingerprint.Field("upstream_key").empty(), W(n).c_str());
					// Every pass body is accepted by the CPU interpreter.
					CpuNetExecutor ex;
					Assert::IsTrue(ex.Prepare(NetDeviceContext(), p, NetPrepareParams(), &err), W(err).c_str());
					if (n[0] == 'A')
					{
						Assert::IsTrue(p.role == NetRole::RepairDelivered, W(n).c_str());
						Assert::AreEqual(std::string(kLabelSchemaV1), p.fingerprint.labelSchema);
						Assert::AreEqual(size_t(7), p.contract.labels.size());
						Assert::IsTrue(p.contract.tileGated);
						Assert::AreEqual(n.find("motion") != std::string::npos, p.contract.neighbours);
						Assert::IsTrue(p.contract.site == RunSite::FrontEnd);
						Assert::IsTrue(SlotOf(p.role) == PipelineSlots::Id::RepairEncoding);
						// The label tables match the export's PARAM ranges.
						Assert::AreEqual(4.0, p.FindParam("vpnn_kernel")->maximum, 0.0, L"bicubic, lanczos, spline, bilinear + none");
						Assert::AreEqual(4.0, p.FindParam("vpnn_codec")->maximum, 0.0, L"AVC, HEVC, AV1, VP9, unknown");
						Assert::AreEqual(4.0, p.FindParam("vpnn_codec")->defaultValue, 0.0, L"default = unknown");
						for (const auto& kv : ParamsFromLabelsV1(LabelsV1()))
							Assert::IsNotNull(p.FindParam(kv.first), W(kv.first).c_str());
					}
					else if (n[0] == 'C')
					{
						Assert::IsTrue(p.role == NetRole::RebaseRefine, W(n).c_str());
						Assert::AreEqual(size_t(1), p.contract.inputs.size());
						Assert::IsTrue(SlotOf(p.role) == PipelineSlots::Id::ScalingRebase);
					}
					else if (n[0] == 'B')
					{
						Assert::IsTrue(p.role == NetRole::X2Restore, W(n).c_str());
						Assert::AreEqual(n.back() == 'h' ? std::string("HOOKED.w 2 *") : std::string("HOOKED.w"), p.passes.back().width, W(n).c_str());
					}
					else
					{
						Assert::IsTrue(p.role == NetRole::ScaleRefine, W(n).c_str());
						Assert::IsTrue(p.contract.site == RunSite::PostKernelHook);
						Assert::IsTrue(p.contract.usesLinearSampling, L"D samples the source features bilinearly");
						Assert::AreEqual(size_t(2), p.contract.inputs.size());
						Assert::IsTrue(SlotOf(p.role) == PipelineSlots::Id::ScalingOutput);
					}
				}
			}
			Log("parsed " + std::to_string(parsed) + " real packages");
			if (!parsed) Log("SKIPPED: no export directory present");
		}

		TEST_METHOD(CpuReferenceMatchesPyTorch)
		{
			int ran = 0;
			for (const char* set : kSets)
			for (const char* caseName : kCases)
			{
				const std::string name = std::string(set) + "/" + caseName;
				RefCase c;
				std::string skip, err;
				if (!LoadCase(name, c, &skip)) { Log("SKIPPED " + name + ": " + skip); continue; }
				CpuNetExecutor ex;
				Assert::IsTrue(RunCase(c, TexFormat::Rgba32f, ex, &err), W(err).c_str());
				Assert::AreEqual(c.outW, ex.Output().width, W(name).c_str());
				Assert::AreEqual(c.outH, ex.Output().height, W(name).c_str());
				const double d32 = MaxDiffCodes(ex.Output(), c.expected, c.outW, c.outH);
				// How much the net changed the picture (0 for a closed gate: the exact-copy path).
				const CpuTexture& base = c.textures[0];
				double corr = 0.0;
				for (int y = 0; y < c.outH; ++y)
					for (int x = 0; x < c.outW; ++x)
						corr = std::max(corr, static_cast<double>(std::fabs(c.expected[static_cast<size_t>(y) * c.outW + x] - base.At(x, y))));
				corr *= 1023.0;
				CpuNetExecutor ex16;
				Assert::IsTrue(RunCase(c, TexFormat::Rgba16f, ex16, &err), W(err).c_str());
				const double d16 = MaxDiffCodes(ex16.Output(), c.expected, c.outW, c.outH);
				char line[256];
				snprintf(line, sizeof line, "%-27s max |cpu - pytorch|: rgba32f %.5f codes, rgba16f %.4f codes; net correction up to %.3f codes",
					name.c_str(), d32, d16, corr);
				Log(line);
				Assert::IsTrue(d32 < 0.01, W(name + ": the CPU executor disagrees with PyTorch").c_str());
				const bool gateClosed = c.desc.Has("gate_tiles_open") && c.desc["gate_tiles_open"].number == 0;
				if (gateClosed)
					Assert::AreEqual(0.0, corr, 0.0, L"a closed gate copies the input exactly");
				else
					Assert::IsTrue(corr > 0.01, W(name + ": the case does not exercise the net").c_str());
				if (c.pkg.fingerprint.fp16Allowed)
					Assert::IsTrue(d16 < 0.05, W(name + ": fp16 allowed but drifts").c_str());
				++ran;
			}
			Log("ran " + std::to_string(ran) + " PyTorch reference cases");
		}

		TEST_METHOD(LabelsReachTheNet)
		{
			RefCase c;
			std::string skip, err;
			if (!LoadCase("round1/A_single_small_open", c, &skip) && !LoadCase("smoke/A_single_small_open", c, &skip)) { Log("SKIPPED: " + skip); return; }
			// The case's labels as schema v1 values give the same PARAMs the reference used.
			LabelsV1 l;
			l.ratioY = 0.5; l.ratioX = 0.5; l.kernel = "lanczos"; l.codec = "HEVC"; l.bppTier = 0.5; l.tenBit = true;
			const std::map<std::string, double> params = ParamsFromLabelsV1(l);
			for (const auto& kv : params)
				Assert::AreEqual(c.inputs.params[kv.first], kv.second, 1e-9, W(kv.first).c_str());
			CpuNetExecutor ex;
			c.inputs.params = ParamsFromLabelsV1(LabelsV1());   // unknown codec, native, no kernel
			Assert::IsTrue(RunCase(c, TexFormat::Rgba32f, ex, &err), W(err).c_str());
			Assert::IsTrue(MaxDiffCodes(ex.Output(), c.expected, c.outW, c.outH) > 1e-4, L"different labels must change A's output");
		}

		TEST_METHOD(InvalidVectorsUseTheSingleFrameFallback)
		{
			RefCase c;
			std::string skip, err;
			if (!LoadCase("round1/A_motion_small_open", c, &skip) && !LoadCase("smoke/A_motion_small_open", c, &skip)) { Log("SKIPPED: " + skip); return; }
			CpuNetExecutor viaState;
			c.inputs.state.neighboursValid = false;
			Assert::IsTrue(RunCase(c, TexFormat::Rgba32f, viaState, &err), W(err).c_str());
			// The same as binding prev = next = the frame and an all-zero mask explicitly.
			CpuTexture zero(c.textures[0].width, c.textures[0].height, 2);
			for (size_t i = 0; i < zero.rgba.size(); i += 4) zero.rgba[i] = zero.rgba[i + 1] = 0.0f;
			NetRunInputs explicitIn = c.inputs;
			explicitIn.state.neighboursValid = true;
			explicitIn.textures["VPNN_PREV"] = c.textures[0].Ref();
			explicitIn.textures["VPNN_NEXT"] = c.textures[0].Ref();
			explicitIn.textures["VPNN_MASK"] = zero.Ref();
			CpuNetExecutor ex;
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			Assert::IsTrue(ex.Prepare(NetDeviceContext(), c.pkg, pp, &err), W(err).c_str());
			NetTexture out;
			Assert::IsTrue(ex.Run(NetDeviceContext(), explicitIn, out, &err), W(err).c_str());
			for (int y = 0; y < c.outH; ++y)
				for (int x = 0; x < c.outW; ++x)
					Assert::AreEqual(ex.Output().At(x, y), viaState.Output().At(x, y));
			// Missing neighbour textures are the same fallback, not an error.
			NetRunInputs noNeighbours = c.inputs;
			noNeighbours.state.neighboursValid = true;
			noNeighbours.textures.erase("VPNN_PREV");
			Assert::IsTrue(ResolveInputs(c.pkg, noNeighbours).neighbourFallback);
		}

		// ------------------------------------------------------------ fingerprints + fallbacks
		TEST_METHOD(FingerprintMatchAndMismatch)
		{
			const NetPackage pkg = SyntheticPackage();
			NetRuntimeContext ctx = ContextMatching(pkg);
			ctx.policy.upstreamKey = Requirement::IfDeclared;   // the synthetic export has none, like round 1
			Assert::IsTrue(CheckFingerprint(pkg, ctx).match, W(CheckFingerprint(pkg, ctx).Summary()).c_str());

			// The design's default requires the upstream key: round-1/2 exports are bypassed until the kit writes it.
			NetRuntimeContext strict = ContextMatching(pkg);
			strict.upstreamKey = "scaling.rebase>repair.encoding";
			FingerprintResult r = CheckFingerprint(pkg, strict);
			Assert::IsFalse(r.match);
			Assert::AreEqual(std::string("export has no upstream key"), r.reasons[0]);

			NetRuntimeContext c2 = ctx;
			c2.labelSchema = "vpnn-labels-v2";
			r = CheckFingerprint(pkg, c2);
			Assert::IsFalse(r.match);
			Assert::IsTrue(r.Summary().find("label schema vpnn-labels-v2 != trained vpnn-labels-v1") == 0, W(r.Summary()).c_str());

			c2 = ctx;
			c2.passOrder = "V>H";
			Assert::IsTrue(CheckFingerprint(pkg, c2).match, L"A declares no pass order: not applicable");
			NetPackage sep = pkg;
			sep.role = NetRole::RebaseRefine;
			Assert::IsFalse(CheckFingerprint(sep, c2).match, L"a separable net must declare its pass order");
			sep.fingerprint.passOrder = "H>V";
			Assert::IsFalse(CheckFingerprint(sep, c2).match, L"pass order V>H != H>V");
			c2.passOrder = "H>V";
			Assert::IsTrue(CheckFingerprint(sep, c2).match);

			// The id must recompute from the recorded fields.
			NetPackage edited = pkg;
			edited.fingerprint.idVerified = false;
			Assert::IsTrue(CheckFingerprint(edited, ctx).Summary().find("do not hash to the recorded id") != std::string::npos);

			// Upstream NETS: declared none, A running upstream -> mismatch (and per frame in NetSlot).
			c2 = ctx;
			c2.upstreamNets = UpstreamNetsKey({ { "repair.encoding", "A_motion_small@52fd496d13224b7c" } });
			r = CheckFingerprint(pkg, c2);
			Assert::IsFalse(r.match);
			Assert::IsTrue(r.Summary().find("upstream nets 'repair.encoding=A_motion_small@52fd496d13224b7c' != trained 'none'") == 0, W(r.Summary()).c_str());
			NetPackage noRecord = pkg;
			noRecord.fingerprint.fields.erase("upstream_nets");
			Assert::IsTrue(CheckFingerprint(noRecord, ctx).Summary().find("no upstream-net record") != std::string::npos);

			// D: the hybrid motion mode it was validated for.
			NetPackage d = pkg;
			d.role = NetRole::ScaleRefine;
			Assert::IsTrue(CheckFingerprint(d, ctx).Summary().find("no hybrid motion mode") != std::string::npos);
			d.fingerprint.hybridMotionMode = "no_vectors";
			c2 = ctx;
			c2.hybridMotionMode = "vectors";
			Assert::IsTrue(CheckFingerprint(d, c2).Summary().find("hybrid motion mode vectors != validated no_vectors") == 0);
			c2.hybridMotionMode = "no_vectors";
			Assert::IsTrue(CheckFingerprint(d, c2).match);

			c2 = ctx;
			c2.intermediate = TexFormat::Rgba16f;
			r = CheckFingerprint(pkg, c2);
			Assert::IsFalse(r.match, L"fp16 not proven for this export");
			Assert::IsTrue(r.reasons[0].find("intermediates rgba16f != trained rgba32f") == 0, W(r.reasons[0]).c_str());
			NetPackage proven = pkg;
			proven.fingerprint.fp16Allowed = true;
			Assert::IsTrue(CheckFingerprint(proven, c2).match, L"fp16 allowed where the export proved it");

			c2 = ctx;
			c2.stageContract = "something else";
			Assert::IsFalse(CheckFingerprint(pkg, c2).match, L"stage contract");

			c2 = ctx;
			c2.settingsHashes["rebase_settings"] = "r2";
			r = CheckFingerprint(pkg, c2);
			Assert::IsFalse(r.match, L"a changed upstream setting");
			Assert::IsTrue(r.Summary().find("rebase_settings") == 0, W(r.Summary()).c_str());

			c2 = ctx;
			c2.vpCommit = "a newer commit";
			Assert::IsTrue(CheckFingerprint(pkg, c2).match, L"the commit is recorded, not gating, by default");
			c2.policy.vpCommit = Requirement::Must;
			Assert::IsFalse(CheckFingerprint(pkg, c2).match, L"the bench can require the exact build");

			c2 = ctx;
			c2.labelSchema = "x";
			c2.stageContract = "y";
			r = CheckFingerprint(pkg, c2);
			Assert::AreEqual(size_t(2), r.reasons.size());
			Assert::IsTrue(r.Summary().find("(+1 more)") != std::string::npos);
		}

		TEST_METHOD(RealExportFingerprintsAgainstTheRunningPipeline)
		{
			int checked = 0;
			for (const std::string& dir : ExportDirs())
			{
				std::vector<NetPackage> pkgs;
				std::string err, probe;
				const std::string json = dir + "/export_C_rebase_refine.json";
				if (!ReadTextFile(json, probe)) continue;
				Assert::IsTrue(LoadExport(json, std::string(), pkgs, &err), W(err).c_str());
				const NetPackage& c = pkgs[0];
				NetRuntimeContext ctx = ContextMatching(c);
				ctx.upstreamKey = "scaling.rebase";
				const FingerprintResult r = CheckFingerprint(c, ctx);
				Assert::IsFalse(r.match, L"round-1 exports carry no upstream key");
				Log(dir + ": " + c.name + " vs the running pipeline: " + r.Summary());
				ctx.policy.upstreamKey = Requirement::IfDeclared;
				Assert::IsFalse(CheckFingerprint(c, ctx).match, L"nor an upstream-net record");
				ctx.policy.upstreamNets = Requirement::IfDeclared;
				Assert::IsTrue(CheckFingerprint(c, ctx).match, W(CheckFingerprint(c, ctx).Summary()).c_str());
				ctx.intermediate = TexFormat::Rgba16f;
				Assert::AreEqual(c.fingerprint.fp16Allowed, CheckFingerprint(c, ctx).match, L"fp16 only where the export proved it");
				++checked;
			}
			if (!checked) Log("SKIPPED: no export directory present");
		}

		TEST_METHOD(MismatchEngagesTheFallbackEntry)
		{
			FallbackStatus::Registry registry;
			NetBypassSet bypass(registry);
			const NetPackage pkg = SyntheticPackage();
			NetRuntimeContext ctx = ContextMatching(pkg);
			ctx.policy.upstreamKey = Requirement::IfDeclared;
			ctx.labelSchema = "vpnn-labels-v2";
			Assert::IsTrue(bypass.Apply(pkg.name, CheckFingerprint(pkg, ctx)));
			const std::string heading = registry.Heading();
			Assert::AreEqual(std::string("Fallbacks engaged: nets: bypassed (T_tiny: label schema vpnn-labels-v2 != trained vpnn-labels-v1)"), heading);
			const std::vector<std::string> ids = FallbackStatus::Ids(heading);
			Assert::AreEqual(size_t(1), ids.size());
			Assert::AreEqual(std::string("nets: bypassed"), ids[0]);
			Assert::IsTrue(bypass.Bypass("C_rebase_refine_h", "export has no upstream key"));
			Assert::IsTrue(registry.Heading().find("T_tiny: label schema") != std::string::npos);
			Assert::IsTrue(registry.Heading().find(", C_rebase_refine_h: export has no upstream key)") != std::string::npos);
			Assert::IsFalse(bypass.Bypass("C_rebase_refine_h", "export has no upstream key"), L"unchanged: no new log line");
			ctx.labelSchema = "vpnn-labels-v1";
			Assert::IsTrue(bypass.Apply(pkg.name, CheckFingerprint(pkg, ctx)), L"a match clears that net");
			Assert::IsTrue(bypass.Clear("C_rebase_refine_h"));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading());
		}

		TEST_METHOD(NetSlotLoadsRunsAndBypassesVisibly)
		{
			FallbackStatus::Registry registry;
			NetBypassSet bypass(registry);
			CpuNetExecutor cpu;
			NetSlot slot(cpu, bypass);
			const NetPackage pkg = SyntheticPackage();
			NetRuntimeContext ctx = ContextMatching(pkg);
			ctx.policy.upstreamKey = Requirement::IfDeclared;
			Assert::IsTrue(slot.Load(NetDeviceContext(), pkg, ctx));
			Assert::IsTrue(slot.Intermediate() == TexFormat::Rgba32f, L"not fp16-proven -> rgba32f");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading());

			CpuTexture in = SyntheticPlane(6, 4);
			NetRunInputs inputs;
			inputs.textures["HOOKED"] = in.Ref();
			NetTexture out;
			// A missing label: the net runs on the export's default, visibly degraded.
			Assert::IsTrue(slot.Run(NetDeviceContext(), inputs, out));
			Assert::AreEqual(6, out.width);
			Assert::AreEqual(std::string("Fallbacks engaged: nets: degraded (T_tiny: labels missing (export defaults): vpnn_gain)"), registry.Heading());
			inputs.params["vpnn_gain"] = 0.25;
			Assert::IsTrue(slot.Run(NetDeviceContext(), inputs, out));
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading(), L"labels back: the entry clears");
			Assert::AreEqual(pkg.Identity(), slot.State("repair.encoding").state, L"the downstream key names this net");

			// An upstream net that changes per frame (bypassed upstream) refuses this frame.
			NetRunInputs changed = inputs;
			changed.upstreamNetsKnown = true;
			changed.upstreamNets = UpstreamNetsKey({ { "scaling.rebase", "bypassed" } });
			Assert::IsFalse(slot.Run(NetDeviceContext(), changed, out));
			Assert::IsTrue(registry.Heading().find("T_tiny: upstream nets this frame 'scaling.rebase=bypassed' != loaded 'none'") != std::string::npos,
				W(registry.Heading()).c_str());
			Assert::AreEqual(std::string("bypassed"), slot.State("repair.encoding").state);
			changed.upstreamNets = "";
			Assert::IsTrue(slot.Run(NetDeviceContext(), changed, out));

			// The 8K rule: a designed bypass, still shown.
			CpuTexture big(1, 1, 1);
			NetRunInputs huge;
			huge.textures["HOOKED"] = big.Ref();
			huge.textures["HOOKED"].width = 7680;
			huge.textures["HOOKED"].height = 4320;
			Assert::IsTrue(ResolveInputs(pkg, huge).bypass);
			Assert::IsFalse(slot.Run(NetDeviceContext(), huge, out));
			Assert::IsTrue(registry.Heading().find("T_tiny: source above 3840x2160") != std::string::npos, W(registry.Heading()).c_str());
			Assert::IsTrue(slot.Run(NetDeviceContext(), inputs, out), L"a normal frame recovers");
			Assert::AreEqual(std::string("Fallbacks engaged: none"), registry.Heading());

			// A missing contract input refuses the frame.
			Assert::IsFalse(slot.Run(NetDeviceContext(), NetRunInputs(), out));
			Assert::IsTrue(registry.Heading().find("no HOOKED texture") != std::string::npos);

			// A fingerprint mismatch at load: not ready, entry shown.
			ctx.stageContract = "changed";
			Assert::IsFalse(slot.Load(NetDeviceContext(), pkg, ctx));
			Assert::IsFalse(slot.Ready());
			Assert::IsTrue(registry.Heading().find("stage contract") != std::string::npos, W(registry.Heading()).c_str());

			// Fused fp16 storage on a net whose fingerprint does not allow fp16: the effective format
			// is reported (Unknown) and the load is refused visibly.
			PlNetRunner fused;
			fused.SetFusion(true, FusedKind::Direct, true);
			Assert::IsTrue(fused.EffectiveIntermediate(pkg, TexFormat::Rgba32f) == TexFormat::Unknown);
			NetPackage allowed = pkg;
			allowed.fingerprint.fp16Allowed = true;
			Assert::IsTrue(fused.EffectiveIntermediate(allowed, TexFormat::Rgba32f) == TexFormat::Rgba16f);

			// The pl_dispatch runner without a device refuses, visibly.
			PlNetRunner gpu;
			FallbackStatus::Registry reg2;
			NetBypassSet bypass2(reg2);
			NetSlot gpuSlot(gpu, bypass2);
			ctx.stageContract = pkg.fingerprint.Field("stage_contract");
			Assert::IsFalse(gpuSlot.Load(NetDeviceContext(), pkg, ctx));
			Assert::IsTrue(reg2.Heading().find("nets: bypassed (T_tiny: ") != std::string::npos, W(reg2.Heading()).c_str());
		}

		TEST_METHOD(CapabilityChecksPickTheFormat)
		{
			NetPackage pkg = SyntheticPackage();
			NetCapabilities caps;
			caps.compute = true;
			caps.renderableRgba16f = caps.renderableRgba32f = caps.storableRgba16f = caps.storableRgba32f = true;
			CapabilityResult r = CheckCapabilities(pkg, caps);
			Assert::IsTrue(r.ok && r.intermediate == TexFormat::Rgba32f, L"trained rgba32f, fp16 not proven");
			pkg.fingerprint.fp16Allowed = true;
			r = CheckCapabilities(pkg, caps);
			Assert::IsTrue(r.ok && r.intermediate == TexFormat::Rgba16f, L"fp16 by default where allowed");
			caps.storableRgba16f = false;
			r = CheckCapabilities(pkg, caps);
			Assert::IsTrue(r.ok && r.intermediate == TexFormat::Rgba32f, L"falls back to rgba32f when fp16 is not storable");
			caps.compute = false;
			r = CheckCapabilities(pkg, caps);
			Assert::IsFalse(r.ok, L"tile-gated compute passes need compute");
			caps.compute = true;
			pkg.contract.usesLinearSampling = true;
			r = CheckCapabilities(pkg, caps);
			Assert::IsFalse(r.ok, L"bilinear NAME_tex needs a linear-filterable format");
			caps.linearRgba32f = true;
			Assert::IsTrue(CheckCapabilities(pkg, caps).ok);
		}

		TEST_METHOD(RepairVariantFollowsTheLatencyRules)
		{
			Assert::IsTrue(ChooseRepairVariant(false, true, true) == RepairChoice::Motion);
			Assert::IsTrue(ChooseRepairVariant(true, true, true) == RepairChoice::Single, L"queue 1: no A_motion");
			Assert::IsTrue(ChooseRepairVariant(true, true, false) == RepairChoice::Bypass);
			Assert::IsTrue(ChooseRepairVariant(false, false, true) == RepairChoice::Single);
			Assert::IsTrue(ChooseRepairVariant(false, false, false) == RepairChoice::Bypass);
			Assert::IsTrue(NeuralBypassForSize(3841, 2160));
			Assert::IsTrue(NeuralBypassForSize(3840, 2161));
			Assert::IsFalse(NeuralBypassForSize(3840, 2160));
		}

		// ------------------------------------------------------------ RGB
		TEST_METHOD(RgbLumaDeltaKeepsChromaAndMatchesTheGpuPass)
		{
			const int w = 5, h = 3;
			std::vector<float> rgba(static_cast<size_t>(w) * h * 4), before(static_cast<size_t>(w) * h), after(before.size());
			for (int i = 0; i < w * h; ++i)
			{
				rgba[i * 4 + 0] = 0.1f + 0.05f * i;
				rgba[i * 4 + 1] = 0.6f - 0.02f * i;
				rgba[i * 4 + 2] = 0.3f;
				rgba[i * 4 + 3] = 1.0f;
				before[i] = LumaOf(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], 0.2126f, 0.0722f);
				after[i] = before[i] + 0.004f * ((i % 3) - 1);   // the repaired luma
			}
			const std::vector<float> original = rgba;
			AddLumaDeltaToRgb(before.data(), after.data(), w, rgba.data(), w * 4, 4, w, h);
			for (int i = 0; i < w * h; ++i)
			{
				const float y2 = LumaOf(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], 0.2126f, 0.0722f);
				Assert::AreEqual(after[i], y2, 1e-6f, L"the RGB luma becomes the repaired luma");
				Assert::AreEqual(original[i * 4 + 2] - before[i], rgba[i * 4 + 2] - y2, 1e-6f, L"B - Y unchanged");
				Assert::AreEqual(original[i * 4 + 0] - before[i], rgba[i * 4 + 0] - y2, 1e-6f, L"R - Y unchanged");
				Assert::AreEqual(1.0f, rgba[i * 4 + 3]);
			}
			// Out of range is kept unless clamping is asked for (BTB/WTW preserved).
			std::vector<float> px = { 0.99f, 0.5f, 0.01f, 1.0f };
			const float lb = 0.5f, la = 0.53f;
			AddLumaDeltaToRgb(&lb, &la, 1, px.data(), 4, 4, 1, 1);
			Assert::IsTrue(px[0] > 1.0f);
			std::vector<float> px2 = { 0.99f, 0.5f, 0.01f, 1.0f };
			RgbDeltaOptions clamp;
			clamp.clampToUnit = true;
			AddLumaDeltaToRgb(&lb, &la, 1, px2.data(), 4, 4, 1, 1, clamp);
			Assert::AreEqual(1.0f, px2[0]);

			// The GPU pass text, run through the CPU executor, equals the helper.
			ParseResult pr = ParseShaderText(kRgbLumaDeltaPass, "rgb_luma_delta");
			Assert::IsTrue(pr.ok, W(pr.error).c_str());
			CpuTexture rgb = CpuTexture::FromPlane(w, h, 4, original.data());
			CpuTexture lb2 = CpuTexture::FromPlane(w, h, 1, before.data());
			CpuTexture la2 = CpuTexture::FromPlane(w, h, 1, after.data());
			CpuNetExecutor ex;
			std::string err;
			NetPrepareParams pp;
			pp.intermediate = TexFormat::Rgba32f;
			Assert::IsTrue(ex.Prepare(NetDeviceContext(), pr.package, pp, &err), W(err).c_str());
			NetRunInputs in;
			in.textures["HOOKED"] = rgb.Ref();
			in.textures["LUMA_BEFORE"] = lb2.Ref();
			in.textures["LUMA_AFTER"] = la2.Ref();
			NetTexture out;
			Assert::IsTrue(ex.Run(NetDeviceContext(), in, out, &err), W(err).c_str());
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
					for (int ch = 0; ch < 4; ++ch)
						Assert::AreEqual(rgba[(static_cast<size_t>(y) * w + x) * 4 + ch], ex.Output().At(x, y, ch), 1e-7f);
		}
	};

	// The kit-compatible settings hashes (U3) and the VP stamp sidecar (tools/net_stamp.py).
	inline std::string StampDir(const std::string& set) { return DataDir() + "stamps/" + set; }
	const char* const kSmokeStaging = "D:/nn-corpus/VP-NN-TrainKit/smoke/staging";
	const char* const kKitScalingManifest = "D:/nn-corpus/VP-NN-TrainKit/data/scaling/scaling_manifest.json";

	// A sidecar for the synthetic package, as net_stamp.py writes it.
	inline std::string SyntheticSidecar(const std::string& id, const std::string& sha, const std::string& stageContract)
	{
		return "{\"schema\": \"vp-net-stamp/1\", \"export_fingerprint\": \"" + id + "\", \"shaders\": {\"T_tiny.glsl\": \"" + sha + "\"},"
			" \"fields\": {\"upstream_key\": \"\", \"pass_order\": \"n/a\", \"intermediate_format\": \"rgba32f\", \"handoff_format\": \"n/a\","
			" \"stage_contract\": \"" + stageContract + "\", \"label_schema\": \"vpnn-labels-v1\", \"delivered_bound\": {\"c0\": 1.5, \"c1\": 0.35, \"cmax\": 32.0}}}";
	}

	TEST_CLASS(NetFingerprintToolsTests)
	{
	public:
		TEST_METHOD(PythonJsonTextIsReproduced)
		{
			using KitHash::PyFloat;
			Assert::AreEqual(std::string("6.0"), PyFloat(6.0));
			Assert::AreEqual(std::string("0.35"), PyFloat(0.35));
			Assert::AreEqual(std::string("0.1"), PyFloat(0.1));
			Assert::AreEqual(std::string("1e-05"), PyFloat(1e-5));
			Assert::AreEqual(std::string("0.0001"), PyFloat(1e-4));
			Assert::AreEqual(std::string("1e+16"), PyFloat(1e16));
			Assert::AreEqual(std::string("1.2345678901234568e+17"), PyFloat(123456789012345678.0));
			Assert::AreEqual(std::string("123.456"), PyFloat(123.456));
			Assert::AreEqual(std::string("-2.5"), PyFloat(-2.5));
			// python: json.dumps({'a': 1.0, 'b': [1e-05, 0.35, 2, True, None], 'c': 'x\x0by\xe9'}, sort_keys=True)
			KitHash::Value arr;
			arr.kind = KitHash::Value::Kind::Array;
			arr.items = { KitHash::Value::Float(1e-5), KitHash::Value::Float(0.35), KitHash::Value::Int(2), KitHash::Value::Bool(true), KitHash::Value() };
			KitHash::Value o = KitHash::Value::Obj();
			o.Set("c", KitHash::Value::Str("x\vy\xC3\xA9")).Set("b", arr).Set("a", KitHash::Value::Float(1.0));
			Assert::AreEqual(std::string("{\"a\": 1.0, \"b\": [1e-05, 0.35, 2, true, null], \"c\": \"x\\u000by\\u00e9\"}"), KitHash::Dumps(o));
		}

		TEST_METHOD(SettingsHashesEqualTheKitsRecordedValues)
		{
			// Recorded in every round-1 export (trainkit/main.py fingerprint(), round-1 config).
			Assert::AreEqual(std::string("99f8db95d09e0db0"), RebaseSettingsHash(RebaseHashSettings()));
			Assert::AreEqual(std::string("f3ffb88c0e71ffaf"), GrainSplitHash(GrainSplitHashSettings()));
			Assert::AreEqual(std::string("543b07659e0523c9"), StageOrderHash(kKitStageOrderR1));
			Assert::AreEqual(std::string("f9c6aaffcf97088e"), BoundsHash(BoundsHashSettings()));
			Assert::AreEqual(std::string("44136fa355b3678a"), ScalerBlendHash(Json()), L"no render settings hash as {}");
			// A change in any setting changes its hash; int vs float matters as in Python.
			GrainSplitHashSettings g;
			g.epsCodes = 6.5;
			Assert::AreNotEqual(std::string("f3ffb88c0e71ffaf"), GrainSplitHash(g));
			RebaseHashSettings r;
			r.perAxisInverse = false;
			Assert::AreNotEqual(std::string("99f8db95d09e0db0"), RebaseSettingsHash(r));
			// The kit's render settings (when this machine has the kit data).
			std::string text, err;
			if (ReadTextFile(kKitScalingManifest, text))
			{
				Json m;
				Assert::IsTrue(ParseJson(text, m, &err), W(err).c_str());
				Assert::AreEqual(std::string("353a5d2176c1b714"), ScalerBlendHash(m["settings"]), L"the round-1 scaling manifest's recorded hash");
			}
			else Log("SKIPPED scaler_blend golden: no kit scaling manifest here");
			const std::map<std::string, std::string> all = SettingsHashes(RebaseHashSettings(), GrainSplitHashSettings(), kKitStageOrderR1, Json(), BoundsHashSettings());
			Assert::AreEqual(size_t(5), all.size());
			Assert::AreEqual(std::string("543b07659e0523c9"), all.at("stage_order"));
		}

		TEST_METHOD(FingerprintIdsRecomputeFromTheRecordedFields)
		{
			int checked = 0;
			for (const std::string& dir : ExportDirs())
				for (const char* net : { "A_single_small", "A_motion_small", "C_rebase_refine", "D_hybrid_refine", "B_x2" })
				{
					std::string text, err, id;
					if (!ReadTextFile(dir + "/export_" + net + ".json", text)) continue;
					Json j;
					Assert::IsTrue(ParseJson(text, j, &err), W(err).c_str());
					Assert::IsTrue(FingerprintIntact(j["fingerprint"], &id), W(dir + "/" + net + ": recomputed " + id).c_str());
					Json tampered = j["fingerprint"];
					tampered.fields["stage_contract"].text += " (edited)";
					Assert::IsFalse(FingerprintIntact(tampered), L"an edited field no longer hashes to the id");
					++checked;
				}
			if (!checked) Log("SKIPPED: no export directory present");
		}

		TEST_METHOD(UpstreamKeysCompareOnlyPictureChangingSlots)
		{
			Assert::AreEqual(std::string("repair.encoding"), NormaliseUpstreamKey("motion.estimation>repair.encoding"));
			Assert::AreEqual(std::string(""), NormaliseUpstreamKey("motion.estimation"));
			Assert::AreEqual(std::string("repair.encoding>scaling.rebase>scaling.geometry"), NormaliseUpstreamKey("repair.encoding>scaling.rebase>scaling.geometry"),
				L"fail safe: an unlisted slot (geometry) counts as changing the picture");
			Assert::AreEqual(std::string("repair.encoding"), NormaliseUpstreamKey("repair.encoding>scaling.chroma_upscale"), L"chroma-only: not seen by a luma net");
			Assert::AreEqual(std::string("unknown.slot"), NormaliseUpstreamKey("unknown.slot"), L"unknown names stay (they can only mismatch)");
			// The registry's real key, with A running first (ADR-0005) and deband / texture restore off.
			PipelineSlots::Registry reg;
			reg.SetComponent(PipelineSlots::Id::RepairEncoding, true);
			reg.SetEnabled(PipelineSlots::Id::RepairEncoding, true);
			reg.SetRebaseFirst(false);
			reg.SetEnabled(PipelineSlots::Id::RepairDeband, false);
			reg.SetEnabled(PipelineSlots::Id::RepairTextureRestore, false);
			Assert::AreEqual(std::string(""), NormaliseUpstreamKey(reg.UpstreamKey(PipelineSlots::Id::RepairEncoding)));
			Assert::AreEqual(std::string("repair.encoding"), NormaliseUpstreamKey(reg.UpstreamKey(PipelineSlots::Id::ScalingRebase)));
			reg.SetEnabled(PipelineSlots::Id::RepairDeband, true);
			reg.SetComponent(PipelineSlots::Id::ScalingRoute4k, true);
			reg.SetEnabled(PipelineSlots::Id::ScalingRoute4k, true);
			Assert::AreEqual(std::string("repair.encoding>scaling.rebase>repair.deband"), NormaliseUpstreamKey(reg.UpstreamKey(PipelineSlots::Id::ScalingRoute4k)),
				L"deband on in front of B shows in the key");
		}

		TEST_METHOD(StampSidecarFillsOnlyMissingFields)
		{
			NetPackage pkg = SyntheticPackage();
			const std::string sha = Sha256Hex(SyntheticShader());
			const std::string contract = pkg.fingerprint.Field("stage_contract");
			Json sc;
			std::string err;
			Assert::IsTrue(ParseJson(SyntheticSidecar(pkg.fingerprint.id, sha, contract), sc, &err), W(err).c_str());
			NetRuntimeContext ctx = ContextMatching(pkg);
			ctx.upstreamKey = "motion.estimation";   // SlotRegistry lists it; it does not change pixels
			Assert::IsFalse(CheckFingerprint(pkg, ctx).match, L"no upstream key yet");
			NetPackage stamped = pkg;
			Assert::IsTrue(MergeStampSidecar(stamped, sc, sha, &err), W(err).c_str());
			Assert::IsTrue(stamped.fingerprint.Has("upstream_key"));
			Assert::AreEqual(std::string(""), stamped.fingerprint.Field("upstream_key"), L"A on the original frame: an empty, declared key");
			Assert::IsTrue(std::find(stamped.fingerprint.stamped.begin(), stamped.fingerprint.stamped.end(), "upstream_key") != stamped.fingerprint.stamped.end());
			Assert::IsTrue(std::find(stamped.fingerprint.stamped.begin(), stamped.fingerprint.stamped.end(), "stage_contract") == stamped.fingerprint.stamped.end(),
				L"the export's own field is kept");
			Assert::AreEqual(std::string("{c0:1.5,c1:0.35,cmax:32.0}"), stamped.fingerprint.Field("delivered_bound"));
			Assert::IsTrue(CheckFingerprint(stamped, ctx).match, W(CheckFingerprint(stamped, ctx).Summary()).c_str());
			ctx.upstreamKey = "scaling.rebase";
			Assert::IsTrue(CheckFingerprint(stamped, ctx).Summary().find("upstream key 'scaling.rebase' != trained ''") == 0);

			NetPackage bad = pkg;
			Assert::IsFalse(MergeStampSidecar(bad, sc, "0000", &err), L"another shader");
			Json other;
			Assert::IsTrue(ParseJson(SyntheticSidecar("ffffffffffffffff", sha, contract), other, &err));
			Assert::IsFalse(MergeStampSidecar(bad, other, sha, &err), L"another export");
			Assert::IsTrue(ParseJson(SyntheticSidecar(pkg.fingerprint.id, sha, "a different contract"), other, &err));
			Assert::IsFalse(MergeStampSidecar(bad, other, sha, &err), L"a sidecar may not contradict the export");
			Assert::IsTrue(err.find("stage_contract") != std::string::npos, W(err).c_str());
		}

		TEST_METHOD(StampedRealExportsMatchAMatchingPipeline)
		{
			std::string probe;
			if (!ReadTextFile(std::string(kSmokeStaging) + "/export_C_rebase_refine.json", probe)) { Log("SKIPPED: no smoke export here"); return; }
			std::string manifestText, err;
			Json render;
			if (ReadTextFile(kKitScalingManifest, manifestText)) Assert::IsTrue(ParseJson(manifestText, render, &err));
			const std::map<std::string, std::string> hashes = SettingsHashes(RebaseHashSettings(), GrainSplitHashSettings(), kKitStageOrderR1,
				render["settings"], BoundsHashSettings());
			// The running pipeline's keys come from the registry: A built and running first (ADR-0005),
			// deband / texture restore off, as the kit trained.
			PipelineSlots::Registry reg;
			reg.SetComponent(PipelineSlots::Id::RepairEncoding, true);
			reg.SetEnabled(PipelineSlots::Id::RepairEncoding, true);
			reg.SetEnabled(PipelineSlots::Id::RepairDeband, false);
			reg.SetEnabled(PipelineSlots::Id::RepairTextureRestore, false);
			reg.SetRebaseFirst(false);
			for (const char* net : { "A_single_small", "A_motion_small", "C_rebase_refine", "B_x2", "D_hybrid_refine" })
			{
				std::vector<NetPackage> pkgs;
				Assert::IsTrue(LoadExport(std::string(kSmokeStaging) + "/export_" + net + ".json", std::string(), pkgs, &err, { StampDir("smoke") }), W(err).c_str());
				for (const NetPackage& p : pkgs)
				{
					Assert::IsTrue(p.fingerprint.idVerified, W(p.name).c_str());
					NetRuntimeContext ctx;
					ctx.labelSchema = kLabelSchemaV1;
					ctx.intermediate = TexFormat::Rgba32f;
					ctx.stageContract = p.fingerprint.Field("stage_contract");
					ctx.upstreamKey = reg.UpstreamKey(SlotOf(p.role));
					ctx.upstreamNets = "";
					ctx.settingsHashes = hashes;
					const FingerprintResult r = CheckFingerprint(p, ctx);
					Log(p.name + ": stamped " + std::to_string(p.fingerprint.stamped.size()) + " fields; vs VP (key '" + ctx.upstreamKey + "'): " +
						(r.match ? std::string("MATCH") : r.Summary()));
					if (p.role == NetRole::RepairDelivered)
					{
						Assert::IsTrue(p.fingerprint.Has("upstream_key") && p.fingerprint.Has("upstream_nets"), W(p.name).c_str());
						if (render.IsObject()) Assert::IsTrue(r.match, W(p.name + ": " + r.Summary()).c_str());
						// The default order (re-base before A) is not what A was trained in.
						PipelineSlots::Registry def;
						def.SetComponent(PipelineSlots::Id::RepairEncoding, true);
						def.SetEnabled(PipelineSlots::Id::RepairEncoding, true);
						ctx.upstreamKey = def.UpstreamKey(PipelineSlots::Id::RepairEncoding);
						Assert::IsFalse(CheckFingerprint(p, ctx).match, W(p.name + " in the default order (key '" + ctx.upstreamKey + "')").c_str());
					}
					else
					{
						// Round 1 recorded no upstream nets: no stamp, so C, B and D stay bypassed.
						Assert::IsFalse(p.fingerprint.Has("upstream_key"), W(p.name).c_str());
						Assert::IsFalse(r.match, W(p.name).c_str());
						Assert::IsTrue(std::find(r.reasons.begin(), r.reasons.end(), "export has no upstream key") != r.reasons.end(), W(r.Summary()).c_str());
					}
				}
			}
		}
	};
}
