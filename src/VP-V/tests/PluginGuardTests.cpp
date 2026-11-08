// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "CppUnitTest.h"

// M2 P2 (D-P2b): the extension-ABI pairing guard (VpvPluginGuard.h). A plugin without the export (a Bill DLL, a
// VP-V DLL of the old 45-number line) or with another number is refused with a visible reason; the built
// renderer DLLs (both flavours) pass. The DLL check needs the Release renderer DLLs next to the test DLL, like
// LibplaceboPluginProxyTestsVpv::RendererDllExportsTheHostBuildId.

#include <VP-V/abi/VpvAbi.h>
#include <VP-V/abi/VpvPluginGuard.h>
#include <vprenderer/LibplaceboRendererPluginApi.h>
#include <vprenderer/OptionalRendererLayout.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace PluginGuardTestsNs
{
	namespace
	{
		uint32_t __cdecl Matching() { return Vpv::kVpvExtensionAbi; }
		uint32_t __cdecl Older() { return Vpv::kVpvExtensionAbi - 1; }
		uint32_t __cdecl Newer() { return Vpv::kVpvExtensionAbi + 1; }

		bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
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

		TEST_METHOD(TheExportNameIsTheDocumentedOne)
		{
			Assert::AreEqual(std::string("VideoProcessorLibplaceboVpvAbi"), std::string(Vpv::kVpvAbiExport));
			Assert::AreEqual(uint32_t(1), Vpv::kVpvExtensionAbi);
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
				const bool ok = Vpv::PluginGuard::Check(plugin, reason);
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
