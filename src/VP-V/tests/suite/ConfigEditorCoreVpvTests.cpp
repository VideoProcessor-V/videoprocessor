// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// VP-V tests split out of Bill's ConfigEditorCoreTests.cpp by tools/m2/move_tests.py.
#include "pch.h"

#include <ConfigEditorCore.h>
#include <ConfigFile.h>
#include <ConfigurationIdentity.h>
#include <ConfigurationApplyPolicy.h>
#include <ConfigurationRpcProtocol.h>
#include <RendererResetPolicy.h>
#include "CppUnitTest.h"

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
	std::wstring MakeTemporaryConfigPath(const wchar_t* prefix)
	{
		wchar_t directory[MAX_PATH] = {};
		if (GetTempPathW(ARRAYSIZE(directory), directory) == 0)
			return {};
		wchar_t path[MAX_PATH] = {};
		if (GetTempFileNameW(directory, prefix, 0, path) == 0)
			return {};
		DeleteFileW(path);
		return path;
	}

	void WriteBytes(const std::wstring& path, const std::string& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
	}

	std::string ReadBytes(const std::wstring& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(ConfigEditorCoreTestsVpv)
	{
	public:

		// Bug hunt 2 2026-10-17 ("Dynamic Drive"): a file that spells the LED
		// section [dynamic_drive.*] is read and edited there - never migrated
		// to, or duplicated as, [led_drive.*]. A fresh file gets
		// [led_drive.default]; with both present [led_drive.*] is used (VP's
		// precedence).
		TEST_METHOD(DynamicDriveAliasSectionIsEditedInPlace)
		{
			const std::wstring targetPath = L"Z:\\target-only\\VideoProcessor.cfg";
			{
				// A real file, so the save runs VP's validation of the alias.
				const std::wstring path = MakeTemporaryConfigPath(L"vdd");
				WriteBytes(path, "[Dynamic_Drive.Default]\r\nenabled: true # mine\r\n"
					"[projector.Default]\r\nport: COM1\r\n");
				ConfigEditorCore::ConfigDocument document;
				std::wstring error;
				Assert::IsTrue(document.Load(path, error), error.c_str());
				Assert::AreEqual(std::string("dynamic_drive.default"), document.StoredSection("led_drive.default"));
				Assert::AreEqual(std::string("true"), document.Get("led_drive.default", "enabled"));
				Assert::IsTrue(document.SetKnown("led_drive.default", "enabled", "false"));
				Assert::IsTrue(document.SetKnown("led_drive.default", "led_btb_passthrough", "false"));
				Assert::IsFalse(document.AddSection("led_drive.default"), L"the alias section already exists");
				Assert::IsTrue(document.RemoveKnown("led_drive.default", "led_btb_passthrough"));
				Assert::IsTrue(document.SetKnown("led_drive.default", "led_btb_passthrough", "false"));
				const std::string saved = document.Serialize();
				Assert::IsTrue(saved.find("[led_drive") == std::string::npos, L"no [led_drive] section was added");
				Assert::IsTrue(saved.find("[Dynamic_Drive.Default]\r\nenabled: false # mine\r\nled_btb_passthrough: false\r\n"
					"[projector.Default]") != std::string::npos, L"edited in place, in the alias section");
				ConfigEditorCore::SaveResult result;
				Assert::IsTrue(ConfigEditorCore::SaveSafely(document, result, error), error.c_str());
				Assert::AreEqual(saved, ReadBytes(path));
				DeleteFileW(path.c_str());
				if (!result.backupPath.empty()) DeleteFileW(result.backupPath.c_str());
				// Other sections are untouched by the alias.
				Assert::AreEqual(std::string("projector.default"), document.StoredSection("projector.default"));
			}
			{
				ConfigEditorCore::ConfigDocument document;
				document.LoadBytes(targetPath, "[general]\r\nrenderer: alpha\r\n");
				Assert::IsTrue(document.SetKnown("led_drive.default", "enabled", "true"));
				const std::string saved = document.Serialize();
				Assert::IsTrue(saved.find("[led_drive.default]\r\nenabled: true") != std::string::npos,
					L"a fresh file gets [led_drive.default]");
				Assert::IsTrue(saved.find("dynamic_drive") == std::string::npos);
			}
			{
				ConfigEditorCore::ConfigDocument document;
				document.LoadBytes(targetPath,
					"[led_drive.Default]\r\nenabled: false\r\n[dynamic_drive.Default]\r\nenabled: true\r\n");
				Assert::AreEqual(std::string("false"), document.Get("led_drive.default", "enabled"),
					L"[led_drive.*] takes precedence");
				Assert::IsTrue(document.SetKnown("led_drive.default", "led_floor", "90"));
				Assert::AreEqual(std::string("[led_drive.Default]\r\nenabled: false\r\nled_floor: 90\r\n"
					"[dynamic_drive.Default]\r\nenabled: true\r\n"), document.Serialize());
			}
		}
	};
}
