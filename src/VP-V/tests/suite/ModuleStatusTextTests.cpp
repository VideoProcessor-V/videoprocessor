#include "pch.h"
#include "CppUnitTest.h"

// The generic module status format (ModuleStatusText.h; IVideoRenderer::GetModuleStatus and
// InvokeModuleCommand): "key=value" lines, order kept, malformed lines and unknown keys skipped,
// values one line each, and the "" index request.

#include <ModuleStatusText.h>

#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ModuleStatusTextTest
{
	TEST_CLASS(ModuleStatusTextTests)
	{
	public:
		TEST_METHOD(AppendAndParseKeepOrderAndValues)
		{
			std::string text;
			Assert::IsTrue(ModuleStatusText::Append(text, "dejudder", std::string("on, 24p in 60 Hz")));
			Assert::IsTrue(ModuleStatusText::Append(text, "fallbacks", std::string("Fallbacks engaged: a (x=1; y); b")));
			Assert::IsTrue(ModuleStatusText::Append(text, "empty", std::string()));
			Assert::AreEqual(std::string("dejudder=on, 24p in 60 Hz\nfallbacks=Fallbacks engaged: a (x=1; y); b\nempty="), text);
			const auto entries = ModuleStatusText::Parse(text);
			Assert::AreEqual(size_t(3), entries.size());
			Assert::AreEqual(std::string("dejudder"), entries[0].key);
			Assert::AreEqual(std::string("Fallbacks engaged: a (x=1; y); b"), entries[1].value, L"'=' inside a value is kept");
			Assert::AreEqual(std::string(), entries[2].value, L"an empty value is a line");
		}

		TEST_METHOD(AValueIsOneLineAndRoundTripsExactly)
		{
			std::wstring text;
			const std::wstring value = L"one\ntwo\r\nthree \\n C:\\path\\ end\\";
			Assert::IsTrue(ModuleStatusText::Append(text, "a", value));
			Assert::AreEqual(std::wstring(L"a=one\\ntwo\\r\\nthree \\\\n C:\\\\path\\\\ end\\\\"), text);
			const auto entries = ModuleStatusText::Parse(text + L"\r\nb=x");
			Assert::AreEqual(size_t(2), entries.size(), L"the value stays one line");
			Assert::AreEqual(value, entries[0].value, L"Parse restores the value exactly");
			Assert::AreEqual(std::wstring(L"x"), entries[1].value);
			Assert::AreEqual(std::wstring(L"odd \\q kept"), ModuleStatusText::Parse(std::wstring(L"k=odd \\q kept"))[0].value);
		}

		TEST_METHOD(InvalidKeysAreRefusedAndMalformedLinesSkipped)
		{
			std::string text;
			Assert::IsFalse(ModuleStatusText::Append(text, "Upper", std::string("x")));
			Assert::IsFalse(ModuleStatusText::Append(text, "has space", std::string("x")));
			Assert::IsFalse(ModuleStatusText::Append(text, "", std::string("x")));
			Assert::IsTrue(text.empty());
			const auto entries = ModuleStatusText::Parse(std::string("no equals\n=novalue\nBad=1\nok.key-2_x=fine\r\n\nlast=1"));
			Assert::AreEqual(size_t(2), entries.size());
			Assert::AreEqual(std::string("ok.key-2_x"), entries[0].key);
			Assert::AreEqual(std::string("fine"), entries[0].value, L"a CRLF line ending is not part of the value");
			Assert::AreEqual(std::string("last"), entries[1].key);
		}

		TEST_METHOD(FindReturnsTheFirstValueAndIgnoresUnknownKeys)
		{
			const std::wstring text = L"newer_key=ignored by old hosts\nstatus=DV comparison capture: started";
			std::wstring value;
			Assert::IsTrue(ModuleStatusText::Find(text, ModuleStatusText::kCommandStatusKey, value));
			Assert::AreEqual(std::wstring(L"DV comparison capture: started"), value);
			Assert::IsFalse(ModuleStatusText::Find(text, "missing", value));
			Assert::IsTrue(value.empty());
			Assert::IsTrue(ModuleStatusText::Parse(std::wstring()).empty());
		}

		TEST_METHOD(IndexListsOneModulePerLine)
		{
			const std::string index = ModuleStatusText::Index(std::vector<std::string>{ "vp", "subtitles" });
			Assert::AreEqual(std::string("module=vp\nmodule=subtitles"), index);
			Assert::AreEqual(size_t(2), ModuleStatusText::Parse(index).size());
		}
	};
}
