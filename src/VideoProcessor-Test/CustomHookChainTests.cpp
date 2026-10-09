#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/CustomHookChain.h>

#include <map>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	ConfiguredShaderRule CustomRule(const char* name, const char* file,
		unsigned stageOrder, unsigned selectionOrder = UINT_MAX)
	{
		ConfiguredShaderRule rule;
		rule.name = name;
		rule.label = name;
		rule.filename = file;
		rule.stageOrder = stageOrder;
		rule.profileSelectionOrder = selectionOrder;
		return rule;
	}

	// Stand-in for pl_mpv_user_shader_parse: the shader text names the stage
	// ("OUTPUT"/"CHROMA") and a "FAIL" marker makes parsing fail.
	struct FakeShaders
	{
		std::map<std::string, std::string> files;
		std::vector<std::string> parsed;
		int destroyed = 0;
		uint64_t nextSignature = 100;
		pl_var_data value{};
		pl_hook_par parameter{};

		FakeShaders()
		{
			parameter.name = "noise_amplitude";
			parameter.type = PL_VAR_FLOAT;
			parameter.data = &value;
			parameter.minimum.f = 0.0f;
			parameter.maximum.f = 0.5f;
		}

		CustomHookChain::Loader Loader()
		{
			return {
				[this](const std::string& file, std::string& source, std::string& reason)
				{
					const auto found = files.find(file);
					if (found == files.end()) { reason = "cannot open shader file"; return false; }
					source = found->second;
					return true;
				},
				[this](const std::string& source) -> const pl_hook*
				{
					parsed.push_back(source);
					if (source.find("FAIL") != std::string::npos) return nullptr;
					auto* hook = new pl_hook{};
					hook->stages = source.find("OUTPUT") != std::string::npos ?
						PL_HOOK_OUTPUT : PL_HOOK_CHROMA_INPUT;
					hook->signature = nextSignature++;
					hook->parameters = &parameter;
					hook->num_parameters = 1;
					return hook;
				},
				[this](const pl_hook*& hook) { delete hook; hook = nullptr; ++destroyed; }
			};
		}
	};

	CustomHookChain::TemplateValues Output2560x1600()
	{
		CustomHookChain::TemplateValues values;
		values.outputWidth = 2560;
		values.outputHeight = 1600;
		values.sourceWidth = 3840;
		values.sourceHeight = 2160;
		values.refreshHz = 60.0;
		values.sourceFps = 23.976;
		return values;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(CustomHookChainTests)
	{
	public:
		TEST_METHOD(ChainOrderFollowsSelectionThenStageAndExcludesNlsAndOff)
		{
			ConfiguredShaderRule nls = CustomRule("nls", "NLS.glsl", 10, 0);
			nls.nls = true;
			ConfiguredShaderRule off;
			off.none = true;
			ConfiguredShaderRule fileless = CustomRule("base", "", 0, 0);
			const auto rules = CustomHookChain::SelectRules({
				CustomRule("sigma", "Temporal sigma-delta.glsl", 90, 2),
				nls, off, fileless,
				CustomRule("ssim", "SSimDownscaler.glsl", 20, 1),
				CustomRule("cfl", "CfL_Prediction.glsl", 10, 0),
				CustomRule("late", "Late.glsl", 5),
				CustomRule("early", "Early.glsl", 1) });
			Assert::AreEqual(size_t{ 5 }, rules.size());
			Assert::AreEqual(std::string("cfl"), rules[0].name);
			Assert::AreEqual(std::string("ssim"), rules[1].name);
			Assert::AreEqual(std::string("sigma"), rules[2].name);
			Assert::AreEqual(std::string("early"), rules[3].name, L"Stage order breaks ties");
			Assert::AreEqual(std::string("late"), rules[4].name);
		}

		TEST_METHOD(TemplatesResolveBuiltInsAndParametersAndRejectLeftovers)
		{
			std::string source = "//!SIZE {{output_width}} {{output_height}}\n"
				"// {{source_width}}x{{source_height}} {{refresh_hz}} {{source_fps}} {{gain}}";
			std::string reason;
			Assert::IsTrue(CustomHookChain::ApplyTemplates(source, Output2560x1600(),
				{ { "gain", "1.5" } }, reason));
			Assert::AreEqual(std::string("//!SIZE 2560 1600\n// 3840x2160 60 23.976 1.5"), source);

			std::string unresolved = "{{missing}}";
			Assert::IsFalse(CustomHookChain::ApplyTemplates(unresolved, Output2560x1600(), {}, reason));
			Assert::IsTrue(reason.find("unresolved") != std::string::npos);

			std::string early = "//!SIZE {{output_width}} 1";
			Assert::IsFalse(CustomHookChain::ApplyTemplates(early, {}, {}, reason));
			Assert::IsTrue(reason.find("not known yet") != std::string::npos);

			// GLSL initialiser braces are code, not templates.
			std::string glsl = "vec2 q[2] = {{0.0, 0.0}, {2.0, 0.0}};";
			Assert::IsTrue(CustomHookChain::ApplyTemplates(glsl, Output2560x1600(), {}, reason));
			Assert::AreEqual(std::string("vec2 q[2] = {{0.0, 0.0}, {2.0, 0.0}};"), glsl);

			// Built-ins are authoritative; a parameter cannot redefine them.
			std::string builtIn = "{{output_width}}";
			Assert::IsTrue(CustomHookChain::ApplyTemplates(builtIn, Output2560x1600(),
				{ { "output_width", "1" } }, reason));
			Assert::AreEqual(std::string("2560"), builtIn);
		}

		TEST_METHOD(TemplateKeyTracksOnlyBuiltInsTheShaderUses)
		{
			auto values = Output2560x1600();
			const std::string output = "//!SIZE {{output_width}} {{output_height}}";
			const std::string key = CustomHookChain::TemplateKey(output, values);
			values.sourceWidth = 1920;
			values.sourceFps = 59.94;
			Assert::AreEqual(key, CustomHookChain::TemplateKey(output, values));
			values.outputWidth = 1920;
			Assert::AreNotEqual(key, CustomHookChain::TemplateKey(output, values));
			Assert::AreEqual(std::string(), CustomHookChain::TemplateKey("no templates", values));
		}

		TEST_METHOD(HookParametersAreSetByNameAndClampedToDeclaredRange)
		{
			pl_var_data amplitude{}, steps{}, count{};
			pl_hook_par parameters[3]{};
			parameters[0].name = "noise_amplitude";
			parameters[0].type = PL_VAR_FLOAT;
			parameters[0].data = &amplitude;
			parameters[0].minimum.f = 0.0f;
			parameters[0].maximum.f = 0.5f;
			parameters[1].name = "steps";
			parameters[1].type = PL_VAR_SINT;
			parameters[1].data = &steps;
			parameters[1].minimum.i = -4;
			parameters[1].maximum.i = 4;
			parameters[2].name = "untouched";
			parameters[2].type = PL_VAR_UINT;
			parameters[2].data = &count;
			parameters[2].minimum.u = 0;
			parameters[2].maximum.u = 9;
			count.u = 7;
			pl_hook hook{};
			hook.parameters = parameters;
			hook.num_parameters = 3;

			const auto applied = CustomHookChain::ApplyHookParameters(&hook, {
				{ "noise_amplitude", "0.14999999999999999" }, { "steps", "-9" },
				{ "not_a_param", "1" }, { "untouched", "abc" } });
			Assert::AreEqual(size_t{ 2 }, applied.size());
			Assert::AreEqual(0.15f, amplitude.f, 1e-6f);
			Assert::AreEqual(-4, steps.i);
			Assert::AreEqual(7u, count.u);

			CustomHookChain::ApplyHookParameters(&hook, { { "noise_amplitude", "3" } });
			Assert::AreEqual(0.5f, amplitude.f, 1e-6f);
		}

		TEST_METHOD(OutputStageHookSuppressesDitherAndOtherStagesDoNot)
		{
			FakeShaders shaders;
			shaders.files["chroma.glsl"] = "CHROMA";
			shaders.files["quant.glsl"] = "OUTPUT {{output_width}}";
			CustomHookChain::Chain chain(shaders.Loader());

			chain.Configure({ CustomRule("cfl", "chroma.glsl", 10) });
			chain.Ensure(Output2560x1600());
			Assert::IsFalse(chain.SuppressesDither(false));

			chain.Configure({ CustomRule("cfl", "chroma.glsl", 10),
				CustomRule("sigma", "quant.glsl", 90) });
			chain.Ensure(Output2560x1600());
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 2 }, hooks.size());
			Assert::IsTrue(chain.SuppressesDither(false));
			Assert::IsTrue(CustomHookChain::HooksOutput(hooks[1]));
			Assert::AreEqual(std::string("OUTPUT 2560"), shaders.parsed.back());
		}

		TEST_METHOD(ParamValuesFromConfigurationReachTheParsedHook)
		{
			FakeShaders shaders;
			shaders.files["quant.glsl"] = "OUTPUT";
			CustomHookChain::Chain chain(shaders.Loader());
			ConfiguredShaderRule rule = CustomRule("sigma", "quant.glsl", 90);
			rule.parameters["noise_amplitude"] = "0.25";
			chain.Configure({ rule });
			chain.Ensure(Output2560x1600());
			Assert::AreEqual(0.25f, shaders.value.f, 1e-6f);
		}

		TEST_METHOD(LoadFailuresDropOnlyTheFailingHook)
		{
			FakeShaders shaders;
			shaders.files["good.glsl"] = "CHROMA";
			shaders.files["broken.glsl"] = "OUTPUT FAIL";
			shaders.files["template.glsl"] = "OUTPUT {{nope}}";
			CustomHookChain::Chain chain(shaders.Loader());
			chain.Configure({ CustomRule("missing", "absent.glsl", 1),
				CustomRule("broken", "broken.glsl", 2),
				CustomRule("template", "template.glsl", 3),
				CustomRule("good", "good.glsl", 4) });
			chain.Ensure(Output2560x1600());
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size());
			// A failed OUTPUT hook must leave libplacebo's dither in place.
			Assert::IsFalse(chain.SuppressesDither(false));
			const size_t attempts = shaders.parsed.size();
			chain.Ensure(Output2560x1600());
			Assert::AreEqual(attempts, shaders.parsed.size(), L"Failures must not be retried every frame");
		}

		TEST_METHOD(HooksReloadOnlyWhenAUsedTemplateValueChanges)
		{
			FakeShaders shaders;
			shaders.files["quant.glsl"] = "OUTPUT {{output_width}}x{{output_height}}";
			CustomHookChain::Chain chain(shaders.Loader());
			chain.Configure({ CustomRule("sigma", "quant.glsl", 90) });

			Assert::IsFalse(chain.Ensure({}), L"Unknown output size must wait, not fail");
			Assert::AreEqual(size_t{ 0 }, shaders.parsed.size());
			auto values = Output2560x1600();
			Assert::IsTrue(chain.Ensure(values));
			values.sourceWidth = 1920;
			Assert::IsFalse(chain.Ensure(values));
			Assert::AreEqual(size_t{ 1 }, shaders.parsed.size());
			values.outputWidth = 1920;
			values.outputHeight = 1080;
			Assert::IsTrue(chain.Ensure(values));
			Assert::AreEqual(std::string("OUTPUT 1920x1080"), shaders.parsed.back());
			Assert::AreEqual(1, shaders.destroyed);
		}

		TEST_METHOD(CinemaOnlyHooksAreSkippedInLowLatency)
		{
			FakeShaders shaders;
			shaders.files["quant.glsl"] = "OUTPUT";
			shaders.files["judder.glsl"] = "CHROMA";
			CustomHookChain::Chain chain(shaders.Loader());
			ConfiguredShaderRule judder = CustomRule("judder", "judder.glsl", 50);
			judder.cinemaOnly = true;
			chain.Configure({ CustomRule("sigma", "quant.glsl", 90), judder });
			chain.Ensure(Output2560x1600());
			std::vector<const pl_hook*> cinema, game;
			chain.Collect(false, cinema);
			chain.Collect(true, game);
			Assert::AreEqual(size_t{ 2 }, cinema.size());
			Assert::AreEqual(size_t{ 1 }, game.size());
			Assert::IsTrue(chain.SuppressesDither(true), L"Dithering stages run in every profile");
			Assert::AreEqual(size_t{ 1 }, chain.ActiveDescriptions(true).size());
		}

		TEST_METHOD(RuntimeFailureLatchesUntilSelectionChanges)
		{
			FakeShaders shaders;
			shaders.files["quant.glsl"] = "OUTPUT";
			CustomHookChain::Chain chain(shaders.Loader());
			chain.Configure({ CustomRule("sigma", "quant.glsl", 90) });
			chain.Ensure(Output2560x1600());
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			const uint64_t signature = hooks.front()->signature;
			const uint64_t unrelated = 7;
			Assert::IsFalse(chain.MarkRuntimeFailures(&unrelated, 1));
			Assert::IsTrue(chain.MarkRuntimeFailures(&signature, 1));
			hooks.clear();
			chain.Collect(false, hooks);
			Assert::IsTrue(hooks.empty());
			Assert::IsFalse(chain.SuppressesDither(false), L"Configured dither resumes after a runtime failure");
			chain.Ensure(Output2560x1600());
			Assert::AreEqual(size_t{ 1 }, shaders.parsed.size(), L"A latched hook is not rebuilt");

			const auto reset = chain.Configure({ CustomRule("sigma", "quant.glsl", 90) });
			Assert::AreEqual(size_t{ 1 }, reset.size());
			Assert::AreEqual(signature, reset.front());
			chain.Ensure(Output2560x1600());
			hooks.clear();
			chain.Collect(false, hooks);
			Assert::AreEqual(size_t{ 1 }, hooks.size());
		}

		TEST_METHOD(NoCustomRulesLeavesTheRenderPathUnchanged)
		{
			FakeShaders shaders;
			CustomHookChain::Chain chain(shaders.Loader());
			ConfiguredShaderRule nls = CustomRule("nls", "NLS.glsl", 10);
			nls.nls = true;
			chain.Configure({ nls });
			Assert::IsFalse(chain.Ensure(Output2560x1600()));
			std::vector<const pl_hook*> hooks;
			chain.Collect(false, hooks);
			Assert::IsTrue(hooks.empty());
			Assert::IsFalse(chain.SuppressesDither(false));
			Assert::AreEqual(size_t{ 0 }, chain.ConfiguredCount());
			Assert::IsTrue(shaders.parsed.empty());
		}
	};
}
