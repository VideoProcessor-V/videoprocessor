// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// Not built with the test project's precompiled header (it is listed NotUsing), so it includes
// pch.h itself: IRenderer.h needs the Windows/ATL setup.
#include "pch.h"
#include "CppUnitTest.h"

// The generic hook layer (M2 P0), settings side: the settings extension registry (H2), the
// host-side renderer extension lookup (H5) and the dialog extension registry (H4; plain C++, so
// it is tested here rather than in a GUI project). Empty = Bill's behaviour; registered =
// dispatch in registration order; ResetForTesting empties the registry again.

#include <ConfigFile.h>
#include <ConfigSchema.h>
#include <IRenderer.h>
#include <RendererProfileConfig.h>
#include <SettingsExtension.h>
#include "../../../VideoProcessor-GUI/DialogExtension.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace HookLayerSettingsTest
{
	namespace
	{
		namespace SE = SettingsExtension;
		namespace DE = DialogExtension;

		// Owns keys "alpha" (valid when "ok"), "bad" (always invalid, no message) and "boom"
		// (throws); everything else is not its key.
		class FakeSettings : public SE::ISettingsExtension
		{
		public:
			explicit FakeSettings(std::string group, std::string tag = "") : group_(std::move(group)), tag_(std::move(tag)) {}
			mutable int calls = 0;

			SE::Verdict Validate(const std::string& group, const std::string& key,
				const std::string& value, std::string& error) const override
			{
				++calls;
				if (group != group_) return SE::Verdict::NotMine;
				if (key == "alpha") { if (value == "ok") return SE::Verdict::Valid; error = "alpha " + tag_; return SE::Verdict::Invalid; }
				if (key == "bad") return SE::Verdict::Invalid;
				if (key == "boom") throw std::runtime_error("extension fault");
				return SE::Verdict::NotMine;
			}
			bool IsKnownKey(const std::string& section, const std::string& key) const override
			{ return section == group_ && (key == "alpha" || key == "bad"); }
			bool DefaultValue(const std::string& group, const std::string& key, std::string& value) const override
			{
				if (group != group_ || key != "alpha") return false;
				value = "default-" + tag_;
				return true;
			}
		private:
			std::string group_, tag_;
		};

		// Leaves the registries empty whatever a test does.
		struct Scope
		{
			Scope() { SE::ResetForTesting(); DE::ResetForTesting(); }
			~Scope() { SE::ResetForTesting(); DE::ResetForTesting(); }
		};

		// A config file with the given text (the Lib's ConfigFile loads from disk only).
		struct TempConfig
		{
			std::string path;
			ConfigFile config;
			explicit TempConfig(const std::string& text)
			{
				const char* temp = std::getenv("TEMP");
				path = std::string(temp ? temp : ".") + "\\hooklayer-settings-tests.cfg";
				{ std::ofstream(path, std::ios::trunc) << text; }
				config.Load(path, ConfigFile::ReadPolicy::Fresh);
			}
			~TempConfig() { std::remove(path.c_str()); }
		};

		class MinimalRenderer : public IVideoRenderer
		{
		public:
			size_t GetConvertedQueueSize() override { return 0; }
			bool OnVideoState(VideoStateComPtr&) override { return true; }
			void OnVideoFrame(VideoFrame&) override {}
			HRESULT OnWindowsEvent(LONG_PTR, LONG_PTR) override { return S_OK; }
			void Build() override {}
			void Start() override {}
			void Stop() override {}
			void Reset() override {}
			void OnSize() override {}
			void OnPaint() override {}
			void SetFrameQueueMaxSize(size_t) override {}
			void SetSceneAwareTimingCorrection(bool) override {}
			size_t GetFrameQueueSize() override { return 0; }
			double EntryLatencyMs() const override { return 0; }
			double ExitLatencyMs() const override { return 0; }
			uint64_t DroppedFrameCount() const override { return 0; }
		};

		class OfferingRenderer : public MinimalRenderer
		{
		public:
			int token = 0;
			void* QueryExtension(const char* iid) override { return std::string(iid) == "test.iid" ? &token : nullptr; }
		};

		class FakeDialog : public DE::IDialogExtension
		{
		public:
			FakeDialog(std::vector<std::string>* log, std::string name) : log_(log), name_(std::move(name)) {}
			bool takeRpc = false, takeShortcut = false;
			int policyDelta = 0;
			void OnTimer() override { log_->push_back(name_ + ":timer"); }
			bool OnRpcRequest(const std::string& verb, const std::string& payload, std::string& reply) override
			{ log_->push_back(name_ + ":rpc"); if (takeRpc) reply = name_ + verb + payload; return takeRpc; }
			void StatsRows(std::vector<DE::StatsRow>& rows) override { rows.emplace_back(L"row", std::wstring(name_.begin(), name_.end())); }
			int OnRendererFailure(int bill) override { return bill + policyDelta; }
			bool OnShortcut(int) override { log_->push_back(name_ + ":key"); return takeShortcut; }
		private:
			std::vector<std::string>* log_;
			std::string name_;
		};

		class ThrowingDialog : public DE::IDialogExtension
		{
		public:
			void OnTimer() override { throw 1; }
			bool OnRpcRequest(const std::string&, const std::string&, std::string&) override { throw 1; }
			void StatsRows(std::vector<DE::StatsRow>&) override { throw 1; }
			int OnRendererFailure(int) override { throw 1; }
			bool OnShortcut(int) override { throw 1; }
		};
	}

	TEST_CLASS(HookLayerSettingsTests)
	{
	public:
		TEST_METHOD(EmptyRegistryIsNotMineAndFalse)
		{
			Scope scope;
			std::string error = "keep", value = "keep";
			Assert::IsTrue(SE::Validate("g", "alpha", "ok", error) == SE::Verdict::NotMine);
			Assert::AreEqual(std::string("keep"), error);
			Assert::IsFalse(SE::IsKnownKey("g", "alpha"));
			Assert::IsFalse(SE::DefaultValue("g", "alpha", value));
			Assert::AreEqual(std::string("keep"), value);
		}

		TEST_METHOD(EmptyRegistryLeavesBillsValidationUnchanged)
		{
			Scope scope;
			std::string expected;
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "no_such_vpv_key", "1", expected));
			Assert::IsTrue(expected == "an input-owned setting");
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "tone_mapping", "auto", expected));
			const std::vector<ConfigSchema::KeyRule> rules = { ConfigSchema::Boolean("known") };
			std::string schemaError;
			TempConfig good("[s]\nknown: true\n");
			Assert::IsTrue(ConfigSchema::ValidateSection(good.config, "s", rules, schemaError));
			TempConfig unknown("[s]\nother: 1\n");
			Assert::IsFalse(ConfigSchema::ValidateSection(unknown.config, "s", rules, schemaError));
			Assert::AreEqual(std::string("[s] unknown key 'other'"), schemaError);
		}

		TEST_METHOD(RegisteredExtensionAnswersForItsKeys)
		{
			Scope scope;
			FakeSettings first("grp");
			SE::Register(&first);
			std::string error;
			Assert::IsTrue(SE::Validate("grp", "alpha", "ok", error) == SE::Verdict::Valid);
			Assert::IsTrue(SE::Validate("grp", "zzz", "x", error) == SE::Verdict::NotMine);
			Assert::IsTrue(SE::Validate("other", "alpha", "ok", error) == SE::Verdict::NotMine);
			Assert::IsTrue(SE::IsKnownKey("grp", "alpha"));
			Assert::IsFalse(SE::IsKnownKey("grp", "zzz"));
			std::string value;
			Assert::IsTrue(SE::DefaultValue("grp", "alpha", value));
			Assert::AreEqual(std::string("default-"), value);
		}

		TEST_METHOD(InvalidAlwaysCarriesAnError)
		{
			Scope scope;
			FakeSettings ext("grp", "tag");
			SE::Register(&ext);
			std::string error;
			Assert::IsTrue(SE::Validate("grp", "alpha", "no", error) == SE::Verdict::Invalid);
			Assert::AreEqual(std::string("alpha tag"), error);
			error.clear();
			Assert::IsTrue(SE::Validate("grp", "bad", "x", error) == SE::Verdict::Invalid);
			Assert::IsFalse(error.empty(), L"an Invalid verdict without a message must still say something");
		}

		TEST_METHOD(RegistrationOrderIsDispatchOrderAndFirstMatchWins)
		{
			Scope scope;
			FakeSettings a("grp", "A"), b("grp", "B");
			SE::Register(&a);
			SE::Register(nullptr);   // ignored
			SE::Register(&b);
			std::string error, value;
			Assert::IsTrue(SE::Validate("grp", "alpha", "no", error) == SE::Verdict::Invalid);
			Assert::AreEqual(std::string("alpha A"), error);
			Assert::AreEqual(1, a.calls);
			Assert::AreEqual(0, b.calls, L"a later extension is not asked once an earlier one claims the key");
			Assert::IsTrue(SE::DefaultValue("grp", "alpha", value));
			Assert::AreEqual(std::string("default-A"), value);
		}

		TEST_METHOD(ThrowingExtensionIsSkipped)
		{
			Scope scope;
			FakeSettings a("grp", "A"), b("grp", "B");
			SE::Register(&a);
			SE::Register(&b);
			std::string error;
			// Both throw for "boom": the caller sees NotMine, never an exception.
			Assert::IsTrue(SE::Validate("grp", "boom", "x", error) == SE::Verdict::NotMine);
		}

		TEST_METHOD(BillsKeysComeFirstAndExtensionFillsTheFallThrough)
		{
			Scope scope;
			FakeSettings ext("input", "X");
			SE::Register(&ext);
			std::string expected;
			// Bill's own key: accepted without consulting the extension.
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "tone_mapping", "auto", expected));
			Assert::AreEqual(0, ext.calls);
			// Bill's own key, bad value: still Bill's answer (the extension says NotMine for it).
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "tone_mapping", "nonsense", expected));
			// The extension's key: accepted / rejected with its message.
			Assert::IsTrue(RendererProfileConfig::ValidateProfileSetting("input", "alpha", "ok", expected));
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "alpha", "no", expected));
			Assert::AreEqual(std::string("alpha X"), expected);
			// An unknown key keeps Bill's message.
			Assert::IsFalse(RendererProfileConfig::ValidateProfileSetting("input", "zzz", "1", expected));
			Assert::IsTrue(expected == "an input-owned setting");

			const std::vector<ConfigSchema::KeyRule> rules = { ConfigSchema::Boolean("known") };
			std::string schemaError;
			TempConfig good("[input]\nknown: true\nalpha: ok\n");
			Assert::IsTrue(ConfigSchema::ValidateSection(good.config, "input", rules, schemaError));
			TempConfig bad("[input]\nalpha: no\n");
			Assert::IsFalse(ConfigSchema::ValidateSection(bad.config, "input", rules, schemaError));
			Assert::AreEqual(std::string("alpha X"), schemaError);
		}

		TEST_METHOD(ResetForTestingEmptiesTheRegistry)
		{
			Scope scope;
			FakeSettings ext("grp");
			SE::Register(&ext);
			std::string error;
			Assert::IsTrue(SE::Validate("grp", "alpha", "ok", error) == SE::Verdict::Valid);
			SE::ResetForTesting();
			Assert::IsTrue(SE::Validate("grp", "alpha", "ok", error) == SE::Verdict::NotMine);
			Assert::IsFalse(SE::IsKnownKey("grp", "alpha"));
		}

		TEST_METHOD(QueryExtensionDefaultsToNull)
		{
			MinimalRenderer plain;
			Assert::IsTrue(plain.QueryExtension("test.iid") == nullptr);
			Assert::IsTrue(plain.QueryExtension(nullptr) == nullptr);
			OfferingRenderer offering;
			IVideoRenderer& asBase = offering;
			Assert::IsTrue(asBase.QueryExtension("test.iid") == &offering.token);
			Assert::IsTrue(asBase.QueryExtension("other") == nullptr);
		}

		TEST_METHOD(DialogEmptyRegistryReturnsBillsValues)
		{
			Scope scope;
			std::string reply = "keep";
			std::vector<DE::StatsRow> rows;
			DE::OnTimer();
			Assert::IsFalse(DE::OnRpcRequest("v", "p", reply));
			Assert::AreEqual(std::string("keep"), reply);
			DE::StatsRows(rows);
			Assert::IsTrue(rows.empty());
			Assert::AreEqual(7, DE::OnRendererFailure(7));
			Assert::IsFalse(DE::OnShortcut(3));
		}

		TEST_METHOD(DialogDispatchOrderAndFirstMatch)
		{
			Scope scope;
			std::vector<std::string> log;
			FakeDialog a(&log, "a"), b(&log, "b");
			b.takeRpc = true; b.takeShortcut = true;
			a.policyDelta = 10; b.policyDelta = 5;
			DE::Register(&a);
			DE::Register(&b);
			DE::OnTimer();
			std::string reply;
			Assert::IsTrue(DE::OnRpcRequest("v", "p", reply));
			Assert::AreEqual(std::string("bvp"), reply);
			Assert::IsTrue(DE::OnShortcut(1));
			std::vector<DE::StatsRow> rows;
			DE::StatsRows(rows);
			Assert::AreEqual(size_t(2), rows.size());
			Assert::IsTrue(rows[0].second == L"a" && rows[1].second == L"b");
			Assert::AreEqual(115, DE::OnRendererFailure(100));
			const std::vector<std::string> want = { "a:timer", "b:timer", "a:rpc", "b:rpc", "a:key", "b:key" };
			Assert::IsTrue(log == want);
			a.takeRpc = true;
			log.clear();
			Assert::IsTrue(DE::OnRpcRequest("v", "p", reply));
			Assert::AreEqual(size_t(1), log.size(), L"first handler wins; the second is not asked");
		}

		TEST_METHOD(DialogThrowingExtensionNeverReachesTheCaller)
		{
			Scope scope;
			ThrowingDialog thrower;
			DE::Register(&thrower);
			std::string reply = "keep";
			std::vector<DE::StatsRow> rows;
			DE::OnTimer();
			Assert::IsFalse(DE::OnRpcRequest("v", "p", reply));
			DE::StatsRows(rows);
			Assert::AreEqual(42, DE::OnRendererFailure(42));
			Assert::IsFalse(DE::OnShortcut(1));
			DE::ResetForTesting();
			DE::OnTimer();   // empty again
		}
	};
}
