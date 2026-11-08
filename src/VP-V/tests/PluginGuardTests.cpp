// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// M2 P2 (D-P2b): the extension-ABI pairing guard (VpvPluginGuard.h). A plugin without the export (a Bill DLL, a
// VP-V DLL of the old 45-number line) or with another number is refused with a visible reason; the built
// renderer DLLs (both flavours) pass. The DLL check needs the Release renderer DLLs next to the test DLL, like
// LibplaceboPluginProxyTestsVpv::RendererDllExportsTheHostBuildId.

#include <VP-V/abi/VpvAbi.h>
#include <VP-V/abi/VpvAttachGate.h>
#include <VP-V/abi/VpvPluginGuard.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
#include <vprenderer/OptionalRendererLayout.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace PluginGuardTestsNs
{
	namespace
	{
		uint32_t __cdecl Matching() { return Vpv::kVpvExtensionAbi; }
		uint32_t __cdecl Older() { return Vpv::kVpvExtensionAbi - 1; }
		uint32_t __cdecl Newer() { return Vpv::kVpvExtensionAbi + 1; }

		bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

		bool __cdecl AcceptsMatching(uint32_t abi) { return abi == Vpv::kVpvExtensionAbi; }
		bool __cdecl RefusesEverything(uint32_t) { return false; }

		std::string g_factoryLog;
		void __cdecl CollectFactoryLog(const char* message) { if (message) g_factoryLog += std::string(message) + "\n"; }
	}

	TEST_CLASS(PluginGuardTests)
	{
	public:
		TEST_METHOD(AMatchingExportIsAccepted)
		{
			std::string reason = "stale";
			Assert::IsTrue(Vpv::PluginGuard::CheckAbiFunction(&Matching, reason));
			Assert::IsTrue(reason.empty());
		}

		TEST_METHOD(AMissingExportIsRefusedWithAVisibleReason)
		{
			std::string reason;
			Assert::IsFalse(Vpv::PluginGuard::CheckAbiFunction(nullptr, reason));
			Assert::IsTrue(Contains(reason, "VP Renderer DLL refused"));
			Assert::IsTrue(Contains(reason, Vpv::kVpvAbiExport), L"the reason names the missing export");
			Assert::IsTrue(Contains(reason, "Redeploy"), L"the reason says what to do");
		}

		TEST_METHOD(AMismatchedNumberIsRefusedWithBothNumbers)
		{
			std::string reason;
			Assert::IsFalse(Vpv::PluginGuard::CheckAbiFunction(&Newer, reason));
			Assert::IsTrue(Contains(reason, ("extension ABI " + std::to_string(Vpv::kVpvExtensionAbi + 1)).c_str()));
			Assert::IsTrue(Contains(reason, ("expects " + std::to_string(Vpv::kVpvExtensionAbi)).c_str()));
			reason.clear();
			Assert::IsFalse(Vpv::PluginGuard::CheckAbiFunction(&Older, reason));
			Assert::IsTrue(Contains(reason, ("extension ABI " + std::to_string(Vpv::kVpvExtensionAbi - 1)).c_str()));
		}

		TEST_METHOD(ANullModuleIsRefused)
		{
			std::string reason;
			Assert::IsFalse(Vpv::PluginGuard::Check(nullptr, reason));
			Assert::IsTrue(Contains(reason, "VP Renderer DLL refused"));
		}

		// A module that has no such export at all (the test host itself).
		TEST_METHOD(AModuleWithoutTheExportIsRefused)
		{
			HMODULE self = nullptr;
			Assert::IsTrue(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Matching), &self) != FALSE);
			std::string reason;
			Assert::IsFalse(Vpv::PluginGuard::Check(self, reason));
			Assert::IsTrue(Contains(reason, Vpv::kVpvAbiExport));
		}

		// ---- the attach handshake (F1 of the P2 opus gate) --------------------------------------------------
		TEST_METHOD(TheGateIsClosedUntilAttachedWithTheMatchingNumber)
		{
			Vpv::AttachGate gate;
			Assert::IsFalse(gate.IsAttached(), L"a fresh DLL is unattached");
			Assert::IsFalse(gate.Attach(0));
			Assert::IsFalse(gate.Attach(Vpv::kVpvExtensionAbi - 1));
			Assert::IsFalse(gate.Attach(Vpv::kVpvExtensionAbi + 1));
			Assert::IsFalse(gate.IsAttached(), L"a wrong number never opens the gate");
			Assert::IsTrue(gate.Attach(Vpv::kVpvExtensionAbi));
			Assert::IsTrue(gate.IsAttached());
			Assert::IsFalse(gate.Attach(Vpv::kVpvExtensionAbi + 1));
			Assert::IsTrue(gate.IsAttached(), L"a later wrong number does not close it");
		}

		TEST_METHOD(AttachFunctionRefusesAMissingOrRefusingExportWithAReason)
		{
			std::string reason;
			Assert::IsTrue(Vpv::PluginGuard::AttachFunction(&AcceptsMatching, reason));
			Assert::IsTrue(reason.empty());
			Assert::IsFalse(Vpv::PluginGuard::AttachFunction(nullptr, reason));
			Assert::IsTrue(Contains(reason, "VP Renderer DLL refused"));
			Assert::IsTrue(Contains(reason, Vpv::kVpvAttachExport), L"the reason names the missing export");
			Assert::IsTrue(Contains(reason, "Redeploy"));
			reason.clear();
			Assert::IsFalse(Vpv::PluginGuard::AttachFunction(&RefusesEverything, reason));
			Assert::IsTrue(Contains(reason, ("extension ABI " + std::to_string(Vpv::kVpvExtensionAbi)).c_str()));
			Assert::IsTrue(Contains(reason, "Redeploy"));
		}

		TEST_METHOD(AttachToANullOrForeignModuleIsRefused)
		{
			std::string reason;
			Assert::IsFalse(Vpv::PluginGuard::Attach(nullptr, reason));
			Assert::IsTrue(Contains(reason, Vpv::kVpvAttachExport));
			HMODULE self = nullptr;
			Assert::IsTrue(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Matching), &self) != FALSE);
			Assert::IsFalse(Vpv::PluginGuard::Attach(self, reason), L"a Bill DLL has no attach export");
		}

		// A host that never attaches (Bill's executable) must get nullptr from the factory, never a renderer of this
		// DLL's extended layouts. The factory logs which refusal it made, which tells it from the null-callback path
		// without ever building a renderer. Process-global DLL state: when an earlier test of this process already
		// attached, the unattached branch cannot be shown again and the test says so (the gate itself is covered by
		// TheGateIsClosedUntilAttachedWithTheMatchingNumber).
		TEST_METHOD(ANonVpHostThatNeverAttachesGetsNoRenderer)
		{
			HMODULE testModule = nullptr;
			Assert::IsTrue(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Matching), &testModule) != FALSE);
			wchar_t modulePath[MAX_PATH] = {};
			GetModuleFileNameW(testModule, modulePath, MAX_PATH);
			const std::wstring fullPath(modulePath);
			const std::wstring root = fullPath.substr(0, fullPath.find_last_of(L"\\/"));
			for (const bool beta : { false, true })
			{
				const std::wstring pluginPath = OptionalRendererLayout::PluginPath(root, beta);
				HMODULE plugin = LoadLibraryExW(pluginPath.c_str(), nullptr,
					LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				Assert::IsNotNull(plugin, L"Build the Release renderer DLLs before this test");
				const auto create = reinterpret_cast<VideoProcessorLibplaceboCreateRendererFn>(
					GetProcAddress(plugin, VP_LIBPLACEBO_CREATE_EXPORT));
				const auto attach = reinterpret_cast<Vpv::VpvAttachFn>(GetProcAddress(plugin, Vpv::kVpvAttachExport));
				Assert::IsTrue(create != nullptr && attach != nullptr, L"the DLL exports the factory and the attach");
				// A wrong number is refused whatever the state.
				Assert::IsFalse(attach(Vpv::kVpvExtensionAbi + 1));
				Assert::IsFalse(attach(0));
				// Probe: a null callback never builds anything; the log tells which gate answered.
				g_factoryLog.clear();
				Assert::IsNull(create(nullptr, 1, nullptr, nullptr, true, 4, VideoConversionOverride::VIDEOCONVERSION_NONE, "", &CollectFactoryLog));
				const bool unattached = Contains(g_factoryLog, "did not attach");
				if (unattached)
				{
					// The host passes a (dummy) callback: still no renderer, the factory never touches it.
					char dummy = 0;
					g_factoryLog.clear();
					Assert::IsNull(create(reinterpret_cast<IRendererCallback*>(&dummy), 1, nullptr, nullptr, true, 4,
						VideoConversionOverride::VIDEOCONVERSION_NONE, "", &CollectFactoryLog),
						L"an unattached DLL builds no renderer");
					Assert::IsTrue(Contains(g_factoryLog, "did not attach"));
				}
				else
					Logger::WriteMessage("INCONCLUSIVE-UNATTACHED-BRANCH: this DLL instance was attached earlier in the process\n");
				create(nullptr, 1, nullptr, nullptr, true, 4, VideoConversionOverride::VIDEOCONVERSION_NONE, "", nullptr);   // drop the test's log sink
				FreeLibrary(plugin);
			}
		}

		TEST_METHOD(TheExportNameIsTheDocumentedOne)
		{
			Assert::AreEqual(std::string("VideoProcessorLibplaceboVpvAbi"), std::string(Vpv::kVpvAbiExport));
			Assert::AreEqual(std::string("VideoProcessorLibplaceboVpvAttach"), std::string(Vpv::kVpvAttachExport));
			Assert::AreEqual(uint32_t(2), Vpv::kVpvExtensionAbi);
		}

		TEST_METHOD(TheBuiltRendererDllsPassTheGuard)
		{
			HMODULE testModule = nullptr;
			Assert::IsTrue(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&Matching), &testModule) != FALSE);
			wchar_t modulePath[MAX_PATH] = {};
			GetModuleFileNameW(testModule, modulePath, MAX_PATH);
			const std::wstring fullPath(modulePath);
			const std::wstring root = fullPath.substr(0, fullPath.find_last_of(L"\\/"));
			for (const bool beta : { false, true })
			{
				const std::wstring pluginPath = OptionalRendererLayout::PluginPath(root, beta);
				HMODULE plugin = LoadLibraryExW(pluginPath.c_str(), nullptr,
					LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
				Assert::IsNotNull(plugin, L"Build the Release renderer DLLs before this test");
				std::string reason;
				bool ok = Vpv::PluginGuard::Check(plugin, reason);
				if (ok) ok = Vpv::PluginGuard::Attach(plugin, reason);
				const auto getAbi = reinterpret_cast<Vpv::VpvAbiFn>(GetProcAddress(plugin, Vpv::kVpvAbiExport));
				const auto getApi = reinterpret_cast<VideoProcessorLibplaceboGetApiVersionFn>(
					GetProcAddress(plugin, VP_LIBPLACEBO_VERSION_EXPORT));
				const uint32_t abi = getAbi ? getAbi() : 0;
				const uint32_t api = getApi ? getApi() : 0;
				FreeLibrary(plugin);
				Assert::IsTrue(ok, std::wstring(reason.begin(), reason.end()).c_str());
				Assert::AreEqual(Vpv::kVpvExtensionAbi, abi);
				Assert::AreEqual(VP_LIBPLACEBO_PLUGIN_API_VERSION, api, L"the DLL reports Bill's number");
			}
		}
	};
}
