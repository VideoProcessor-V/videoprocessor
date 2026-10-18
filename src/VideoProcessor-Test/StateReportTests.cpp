#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <DiagnosticHistory.h>
#include <StateReport.h>
#include <StateReportSettings.h>
#include <StoreZipWriter.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Report-a-problem bundle (owner 2026-10-17): the store-only ZIP, the report
// writer, redaction, retention, the auto-report limiter and the settings.
namespace
{
	std::wstring Widen(const std::string& text)
	{
		return std::wstring(text.begin(), text.end());
	}

	void RemoveTree(const std::string& folder)
	{
		WIN32_FIND_DATAA found{};
		const HANDLE search = FindFirstFileA((folder + "\*").c_str(), &found);
		if (search != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::string name = found.cFileName;
				if (name == "." || name == "..")
					continue;
				const std::string path = folder + "\\" + name;
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					RemoveTree(path);
				else
					DeleteFileA(path.c_str());
			} while (FindNextFileA(search, &found));
			FindClose(search);
		}
		RemoveDirectoryA(folder.c_str());
	}

	// A fresh, empty folder under %TEMP%.
	std::string TempFolder(const char* name)
	{
		char base[MAX_PATH] = {};
		GetTempPathA(MAX_PATH, base);
		const std::string folder = std::string(base) + "vp-state-report-" + name;
		RemoveTree(folder);
		CreateDirectoryA(folder.c_str(), nullptr);
		return folder;
	}

	std::string TempParent()
	{
		char base[MAX_PATH] = {};
		GetTempPathA(MAX_PATH, base);
		std::string parent(base);
		while (!parent.empty() && (parent.back() == '\\' || parent.back() == '/'))
			parent.pop_back();
		return parent;
	}

	void WriteFileText(const std::string& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
	}

	std::vector<unsigned char> ReadBytes(const std::wstring& path)
	{
		std::ifstream in(std::string(path.begin(), path.end()), std::ios::binary);
		std::stringstream text;
		text << in.rdbuf();
		const std::string data = text.str();
		return std::vector<unsigned char>(data.begin(), data.end());
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

	const StoreZipWriter::ReadEntry* Find(const std::vector<StoreZipWriter::ReadEntry>& entries,
		const std::string& name)
	{
		for (const auto& entry : entries)
			if (entry.name == name)
				return &entry;
		return nullptr;
	}

	SYSTEMTIME MakeTime(WORD year, WORD month, WORD day, WORD hour, WORD minute, WORD second, WORD ms)
	{
		SYSTEMTIME time = {};
		time.wYear = year;
		time.wMonth = month;
		time.wDay = day;
		time.wHour = hour;
		time.wMinute = minute;
		time.wSecond = second;
		time.wMilliseconds = ms;
		return time;
	}

	StateReport::RedactionContext TestContext()
	{
		StateReport::RedactionContext context;
		context.keptRoots.emplace_back("C:\\Videoprocessor\\vp", "<vp>");
		context.keptRoots.emplace_back("C:\\Windows", "<windows>");
		context.words.emplace_back("victor", "<user>");
		context.words.emplace_back("CINEMA-PC", "<host>");
		return context;
	}

	std::string Redact(const std::string& text)
	{
		return StateReport::Redact(text, TestContext());
	}

	std::vector<unsigned char> Bytes(const std::string& text)
	{
		return std::vector<unsigned char>(text.begin(), text.end());
	}

	// Writes `text` to a temp .cfg and loads it fresh.
	bool LoadConfig(ConfigFile& config, const std::string& folder, const std::string& text)
	{
		const std::string path = folder + "\\settings.cfg";
		WriteFileText(path, text);
		return config.Load(path, ConfigFile::ReadPolicy::Fresh);
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(StoreZipWriterTests)
	{
	public:
		TEST_METHOD(RoundTripsEntriesByteExact)
		{
			std::string binary;
			for (int value = 0; value < 256; ++value)
				binary.push_back(static_cast<char>(value));
			StoreZipWriter zip(StoreZipWriter::MakeDosTime(2026, 10, 17, 21, 30, 45));
			Assert::IsTrue(zip.Add("empty.txt", "", 0));
			Assert::IsTrue(zip.Add("binary.bin", binary));
			Assert::IsTrue(zip.Add("config/VideoProcessor.cfg", std::string("[a]\nb: c\n")));
			Assert::AreEqual(static_cast<size_t>(3), zip.EntryCount());
			const std::vector<unsigned char> archive = zip.Finish();

			std::vector<StoreZipWriter::ReadEntry> entries;
			std::string error;
			Assert::IsTrue(StoreZipWriter::Read(archive, entries, &error), Widen(error).c_str());
			Assert::AreEqual(static_cast<size_t>(3), entries.size());
			Assert::AreEqual("empty.txt", entries[0].name.c_str());
			Assert::IsTrue(entries[0].data.empty());
			Assert::AreEqual("binary.bin", entries[1].name.c_str());
			Assert::IsTrue(entries[1].data == binary, L"all 256 byte values survive");
			Assert::AreEqual("config/VideoProcessor.cfg", entries[2].name.c_str());
			Assert::AreEqual("[a]\nb: c\n", entries[2].data.c_str());
		}

		TEST_METHOD(KnownCrc32)
		{
			Assert::AreEqual(static_cast<uint32_t>(0xCBF43926u), StoreZipWriter::Crc32("123456789", 9));
			Assert::AreEqual(static_cast<uint32_t>(0), StoreZipWriter::Crc32("", 0));
		}

		TEST_METHOD(InvalidAndDuplicateNamesAreRejected)
		{
			StoreZipWriter zip;
			const char* const invalid[] = { "", "/x", "a\\b", "../x", "a//b", "C:x", "a/../b", "a/", "./x" };
			for (const char* name : invalid)
				Assert::IsFalse(zip.Add(name, std::string("x")), Widen(name).c_str());
			Assert::AreEqual(static_cast<size_t>(0), zip.EntryCount());
			Assert::IsTrue(zip.Add("a/b.txt", std::string("x")));
			Assert::IsFalse(zip.Add("a/b.txt", std::string("y")), L"duplicate");
			Assert::AreEqual(static_cast<size_t>(1), zip.EntryCount());
			zip.Finish();
			Assert::IsFalse(zip.Add("late.txt", std::string("z")), L"after Finish");
		}

		TEST_METHOD(CorruptedDataFailsTheCrcCheck)
		{
			StoreZipWriter zip;
			Assert::IsTrue(zip.Add("a.txt", std::string("hello world")));
			std::vector<unsigned char> archive = zip.Finish();
			std::vector<StoreZipWriter::ReadEntry> entries;
			Assert::IsTrue(StoreZipWriter::Read(archive, entries));
			// The data follows the 30-byte local header and the name.
			archive[30 + 5 + 3] ^= 0x01;
			std::string error;
			Assert::IsFalse(StoreZipWriter::Read(archive, entries, &error));
			Assert::AreEqual("crc", error.c_str());

			std::vector<unsigned char> truncated(archive.begin(), archive.begin() + 10);
			Assert::IsFalse(StoreZipWriter::Read(truncated, entries));
		}
	};

	TEST_CLASS(StateReportTests)
	{
	public:
		TEST_METHOD(Sha256KnownVector)
		{
			Assert::AreEqual("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
				StateReport::Sha256Hex("abc", 3).c_str());
			Assert::AreEqual("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
				StateReport::Sha256Hex("", 0).c_str());
		}

		TEST_METHOD(FileNameFormat)
		{
			Assert::AreEqual("vp-report-20261017-213045-123.zip",
				StateReport::FileName(MakeTime(2026, 10, 17, 21, 30, 45, 123)).c_str());
			Assert::AreEqual("vp-report-20260105-010203-007.zip",
				StateReport::FileName(MakeTime(2026, 1, 5, 1, 2, 3, 7)).c_str());
		}

		TEST_METHOD(RetentionRemovesExactlyTheOldestByName)
		{
			const std::string folder = TempFolder("retention");
			for (int i = 0; i < 25; ++i)
			{
				char name[64];
				std::snprintf(name, sizeof(name), "\\vp-report-20261017-1200%02d-000.zip", i);
				WriteFileText(folder + name, "x");
			}
			WriteFileText(folder + "\\other.zip", "x");
			WriteFileText(folder + "\\vp-report-x.zip.partial", "x");

			const std::vector<std::wstring> removed = StateReport::ApplyRetention(Widen(folder), 20);
			Assert::AreEqual(static_cast<size_t>(5), removed.size());
			for (int i = 0; i < 25; ++i)
			{
				char name[64];
				std::snprintf(name, sizeof(name), "\\vp-report-20261017-1200%02d-000.zip", i);
				const bool exists = GetFileAttributesA((folder + name).c_str()) != INVALID_FILE_ATTRIBUTES;
				Assert::AreEqual(i >= 5, exists, Widen(name).c_str());
			}
			Assert::IsTrue(GetFileAttributesA((folder + "\\other.zip").c_str()) != INVALID_FILE_ATTRIBUTES);
			Assert::IsTrue(GetFileAttributesA((folder + "\\vp-report-x.zip.partial").c_str()) != INVALID_FILE_ATTRIBUTES);
			Assert::AreEqual(static_cast<size_t>(20), StateReport::ListReports(Widen(folder)).size());
			Assert::IsTrue(StateReport::ApplyRetention(Widen(folder), 20).empty(), L"nothing more to remove");
		}

		TEST_METHOD(WriteProducesTheDocumentedBundle)
		{
			const std::string folder = TempFolder("write");
			const std::string vp = folder + "\\vp";
			CreateDirectoryA(vp.c_str(), nullptr);
			WriteFileText(folder + "\\a.bin", "binary A");
			WriteFileText(folder + "\\b.dll", "binary B");
			WriteFileText(vp + "\\VideoProcessor.cfg",
				"[general]\n# comment kept\nqueue_size: 48\npassword: hunter2\napi_key = abc123\n"
				"media = " + folder + "\\..\\Videos\\Film.mkv\nlog = " + vp + "\\logs\\vp.log\n");

			StateReport::Inputs inputs;
			inputs.vpRoot = Widen(vp);
			inputs.reportsDirectory = Widen(vp + "\\logs\\reports");
			inputs.trigger = "test";
			inputs.buildId = "0123456789abcdef0123456789abcdef";
			inputs.commit = "9f8e7d6";
			inputs.version = "1.4.02";
			inputs.pluginApi = 7;
			inputs.rendererName = "VP Renderer";
			inputs.binaries.push_back({ "VideoProcessor.exe", Widen(folder + "\\a.bin") });
			inputs.binaries.push_back({ "vprenderer\\VideoProcessorVPRenderer.dll", Widen(folder + "\\b.dll") });
			inputs.historyExe = "{\"src\":\"exe\",\"ring\":\"decision\"}\n{\"src\":\"exe\",\"ring\":\"stats\"}\n";
			inputs.historyRenderer = "{\"src\":\"renderer\",\"ring\":\"frame\"}\n";
			inputs.rendererContributed = true;
			inputs.logExcerpt = "12:00:00 opened " + folder + "\\..\\Videos\\Film.mkv for victor on CINEMA-PC\n";
			inputs.crashStateLive = "renderer: VP Renderer\nrefresh 23.976 Hz\n";
			inputs.liveSettings.push_back({ "profile", "film" });
			inputs.liveSettings.push_back({ "file", "D:\\Media\\x.mkv" });
			inputs.configFiles.push_back(Widen(vp + "\\VideoProcessor.cfg"));
			inputs.systemJson = "{}";
			inputs.thumbnailEnabled = false;
			inputs.keep = 20;
			inputs.localTime = MakeTime(2026, 10, 17, 21, 30, 45, 123);
			inputs.redaction.keptRoots.emplace_back(vp, "<vp>");
			inputs.redaction.keptRoots.emplace_back("C:\\Windows", "<windows>");
			inputs.redaction.words.emplace_back("victor", "<user>");
			inputs.redaction.words.emplace_back("CINEMA-PC", "<host>");

			const StateReport::Result result = StateReport::Write(inputs);
			Assert::IsTrue(result.ok, Widen(result.error).c_str());
			Assert::AreEqual("vp-report-20261017-213045-123.zip", result.fileName.c_str());
			Assert::IsTrue(GetFileAttributesW(result.path.c_str()) != INVALID_FILE_ATTRIBUTES);
			Assert::IsTrue(GetFileAttributesW((result.path + L".partial").c_str()) == INVALID_FILE_ATTRIBUTES,
				L"no leftover temporary file");

			std::vector<StoreZipWriter::ReadEntry> entries;
			std::string error;
			Assert::IsTrue(StoreZipWriter::Read(ReadBytes(result.path), entries, &error), Widen(error).c_str());
			const char* const expected[] = { "README.md", "manifest.json", "state-history.jsonl",
				"log-excerpt.txt", "crash-state.txt", "live-settings.json", "config/VideoProcessor.cfg",
				"system.json" };
			for (const char* name : expected)
				Assert::IsNotNull(Find(entries, name), Widen(name).c_str());
			Assert::IsNull(Find(entries, "thumbnail.png"), L"no thumbnail when switched off");
			Assert::IsNull(Find(entries, "thumbnail-missing.txt"));
			Assert::AreEqual(sizeof(expected) / sizeof(expected[0]), result.entries.size());

			// README names every file and every stats field.
			const std::string& readme = Find(entries, "README.md")->data;
			const char* const documented[] = { "manifest.json", "state-history.jsonl", "log-excerpt.txt",
				"crash-state.txt", "crash-state-last.txt", "live-settings.json", "config/", "system.json",
				"thumbnail.png" };
			for (const char* name : documented)
				Assert::IsTrue(Contains(readme, name), Widen(std::string("README lacks ") + name).c_str());
			const char* const* stats = DiagnosticHistory::StatsFieldNames();
			for (size_t field = 0; field < DiagnosticHistory::kStatsFields; ++field)
				Assert::IsTrue(Contains(readme, stats[field]),
					Widen(std::string("README lacks stats field ") + stats[field]).c_str());

			// Manifest.
			const std::string& manifest = Find(entries, "manifest.json")->data;
			Assert::IsTrue(Contains(manifest, "\"format_version\": 1"));
			Assert::IsTrue(Contains(manifest, "\"build_id\": \"0123456789abcdef0123456789abcdef\""));
			Assert::IsTrue(Contains(manifest, "\"plugin_api\": 7"));
			Assert::IsTrue(Contains(manifest, "\"commit\": \"9f8e7d6\""));
			Assert::IsTrue(Contains(manifest, "\"trigger\": \"test\""));
			Assert::IsTrue(Contains(manifest, "\"created_local\": \"2026-10-17T21:30:45.123\""));
			Assert::IsTrue(Contains(manifest, "\"history_rows\": {\"exe\": 2, \"renderer\": 1}"));
			uint64_t bytes = 0;
			const std::string hashA = StateReport::Sha256File(Widen(folder + "\\a.bin"), &bytes);
			Assert::AreEqual(static_cast<uint64_t>(8), bytes);
			Assert::AreEqual(StateReport::Sha256Hex("binary A", 8).c_str(), hashA.c_str());
			const std::string hashB = StateReport::Sha256File(Widen(folder + "\\b.dll"));
			Assert::IsTrue(Contains(manifest, "\"sha256\": \"" + hashA + "\""));
			Assert::IsTrue(Contains(manifest, "\"sha256\": \"" + hashB + "\""));
			Assert::IsTrue(Contains(manifest, "VideoProcessor.exe"));
			Assert::IsTrue(Contains(manifest, "VideoProcessorVPRenderer.dll"));
			Assert::IsTrue(Contains(manifest, "\"contents\": [\"README.md\", \"manifest.json\", \"state-history.jsonl\""));
			Assert::IsTrue(Contains(manifest, "\"config/VideoProcessor.cfg\""));
			Assert::IsTrue(Contains(manifest, "\"system.json\""));

			// History: both modules, in order.
			Assert::AreEqual("{\"src\":\"exe\",\"ring\":\"decision\"}\n{\"src\":\"exe\",\"ring\":\"stats\"}\n"
				"{\"src\":\"renderer\",\"ring\":\"frame\"}\n", Find(entries, "state-history.jsonl")->data.c_str());

			// Nothing personal or secret in any entry.
			const std::string parent = TempParent();
			for (const auto& entry : entries)
			{
				Assert::IsFalse(Contains(entry.data, parent), Widen("temp path in " + entry.name).c_str());
				Assert::IsFalse(Contains(entry.data, "\\Users\\"), Widen("Users in " + entry.name).c_str());
				Assert::IsFalse(Contains(entry.data, "hunter2"), Widen("secret in " + entry.name).c_str());
				Assert::IsFalse(Contains(entry.data, "abc123"), Widen("secret in " + entry.name).c_str());
				Assert::IsFalse(Contains(entry.data, "CINEMA-PC"), Widen("host in " + entry.name).c_str());
				Assert::IsFalse(Contains(entry.data, "victor"), Widen("user in " + entry.name).c_str());
			}
			const std::string& config = Find(entries, "config/VideoProcessor.cfg")->data;
			Assert::IsTrue(Contains(config, "# comment kept"));
			Assert::IsTrue(Contains(config, "queue_size: 48"));
			Assert::IsTrue(Contains(config, "password: <redacted>"));
			Assert::IsTrue(Contains(config, "<vp>\\logs\\vp.log"));
			Assert::IsTrue(Contains(config, "media = <path>"));
			const std::string& log = Find(entries, "log-excerpt.txt")->data;
			Assert::IsTrue(Contains(log, "<path>"));
			Assert::IsTrue(Contains(log, "<user>") && Contains(log, "<host>"));
			Assert::IsTrue(Contains(Find(entries, "live-settings.json")->data, "\"profile\": \"film\""));
			Assert::IsFalse(Contains(Find(entries, "live-settings.json")->data, "D:\\Media"));
		}

		TEST_METHOD(ThumbnailSwitch)
		{
			const std::string folder = TempFolder("thumbnail");
			StateReport::Inputs inputs;
			inputs.vpRoot = Widen(folder);
			inputs.reportsDirectory = Widen(folder + "\\reports");
			inputs.trigger = "test";
			inputs.systemJson = "{}";
			inputs.localTime = MakeTime(2026, 10, 17, 21, 30, 45, 1);
			inputs.redaction = TestContext();

			std::string png = "\x89PNG\r\n\x1a\n";
			png.push_back('\0');
			png += "pixels";
			inputs.thumbnailEnabled = true;
			inputs.thumbnailPng = Bytes(png);
			StateReport::Result result = StateReport::Write(inputs);
			Assert::IsTrue(result.ok, Widen(result.error).c_str());
			std::vector<StoreZipWriter::ReadEntry> entries;
			Assert::IsTrue(StoreZipWriter::Read(ReadBytes(result.path), entries));
			const auto* thumbnail = Find(entries, "thumbnail.png");
			Assert::IsNotNull(thumbnail);
			Assert::IsTrue(thumbnail->data == png, L"thumbnail byte-exact");
			Assert::IsNull(Find(entries, "thumbnail-missing.txt"));
			Assert::IsTrue(Contains(Find(entries, "manifest.json")->data, "\"thumbnail\": true"));

			inputs.thumbnailPng.clear();
			inputs.thumbnailNote = "VP Renderer not active";
			inputs.localTime = MakeTime(2026, 10, 17, 21, 30, 46, 1);
			result = StateReport::Write(inputs);
			Assert::IsTrue(result.ok, Widen(result.error).c_str());
			Assert::IsTrue(StoreZipWriter::Read(ReadBytes(result.path), entries));
			Assert::IsNull(Find(entries, "thumbnail.png"));
			const auto* missing = Find(entries, "thumbnail-missing.txt");
			Assert::IsNotNull(missing);
			Assert::IsTrue(Contains(missing->data, "VP Renderer not active"));
		}

		TEST_METHOD(WriteAppliesRetentionIncludingTheNewReport)
		{
			const std::string folder = TempFolder("write-retention");
			const std::string reports = folder + "\\reports";
			CreateDirectoryA(reports.c_str(), nullptr);
			for (int i = 0; i < 20; ++i)
			{
				char name[64];
				std::snprintf(name, sizeof(name), "\\vp-report-20260101-0000%02d-000.zip", i);
				WriteFileText(reports + name, "old");
			}
			StateReport::Inputs inputs;
			inputs.vpRoot = Widen(folder);
			inputs.reportsDirectory = Widen(reports);
			inputs.trigger = "test";
			inputs.systemJson = "{}";
			inputs.keep = 20;
			inputs.localTime = MakeTime(2026, 10, 17, 21, 30, 45, 1);
			inputs.redaction = TestContext();
			const StateReport::Result result = StateReport::Write(inputs);
			Assert::IsTrue(result.ok, Widen(result.error).c_str());
			Assert::AreEqual(static_cast<size_t>(1), result.removed.size());
			const std::vector<std::wstring> left = StateReport::ListReports(Widen(reports));
			Assert::AreEqual(static_cast<size_t>(20), left.size());
			Assert::IsTrue(std::find(left.begin(), left.end(), Widen(result.fileName)) != left.end(),
				L"the new report is kept");
			Assert::IsTrue(GetFileAttributesA((reports + "\\vp-report-20260101-000000-000.zip").c_str()) ==
				INVALID_FILE_ATTRIBUTES, L"the oldest was removed");
		}

		TEST_METHOD(RedactionRemovesPathsAndKeepsVpFolder)
		{
			const std::string text = Redact("log=C:\\Users\\victor\\Videos\\My Film.mkv opened");
			Assert::IsFalse(Contains(text, "Users"), Widen(text).c_str());
			Assert::IsFalse(Contains(text, "victor"), Widen(text).c_str());
			Assert::IsFalse(Contains(text, "My Film"), Widen(text).c_str());
			Assert::IsTrue(Contains(text, "<path>"), Widen(text).c_str());
			Assert::IsTrue(Contains(text, " opened"), Widen(text).c_str());

			Assert::AreEqual("<vp>\\logs\\vp.log", Redact("C:\\Videoprocessor\\vp\\logs\\vp.log").c_str());
			const std::string forward = Redact("c:/videoprocessor/vp/luts/a.cube");
			Assert::IsTrue(forward.compare(0, 4, "<vp>") == 0, Widen(forward).c_str());
			Assert::AreEqual("<path>", Redact("D:\\Media\\x.mkv").c_str());
			Assert::AreEqual("<path>", Redact("\\\\server\\share\\x").c_str());
			Assert::AreEqual("<path>", Redact("E:/a b/c.txt").c_str());
			Assert::AreEqual("<windows>\\System32\\nvapi64.dll",
				Redact("C:\\Windows\\System32\\nvapi64.dll").c_str());
		}

		TEST_METHOD(RedactionRemovesSecretValues)
		{
			const char* const lines[] = { "password: hunter2", "api_key = abc123", "#token: xyz",
				"  \"secret\": \"s3\"", "serial: 1234", "url: http://x.y/z" };
			const char* const secrets[] = { "hunter2", "abc123", "xyz", "s3", "1234", "x.y" };
			for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
			{
				const std::string text = Redact(lines[i]);
				Assert::IsTrue(Contains(text, "<redacted>"), Widen(text).c_str());
				Assert::IsFalse(Contains(text, secrets[i]), Widen(text).c_str());
			}
		}

		TEST_METHOD(RedactionReplacesEmailUrlUserAndHost)
		{
			Assert::AreEqual("contact <email> now", Redact("contact victor@example.com now").c_str());
			Assert::AreEqual("see <url>", Redact("see https://example.com/a?b").c_str());
			Assert::AreEqual("user <user> here", Redact("user Victor here").c_str());
			const std::string host = Redact("machine cinema-pc ready");
			Assert::IsFalse(Contains(host, "cinema-pc"), Widen(host).c_str());
			Assert::IsTrue(Contains(host, "<host>"), Widen(host).c_str());
		}

		TEST_METHOD(RedactionLeavesOrdinaryLinesAlone)
		{
			const char* const lines[] = { "queue_size: 48", "shortcut: Ctrl+Alt+D", "refresh 23.976 Hz",
				"Ratio 16:9", "time 12:34:56.789", "driver 31.0.15.3667", "a:b", "[shortcuts]" };
			for (const char* line : lines)
				Assert::AreEqual(line, Redact(line).c_str());
		}

		TEST_METHOD(SecretKeyClassification)
		{
			const char* const secret[] = { "password", "Api_Key", "TOKEN", "serial_number" };
			for (const char* key : secret)
				Assert::IsTrue(StateReport::IsSecretKey(key), Widen(key).c_str());
			const char* const plain[] = { "queue_size", "shortcut", "state_report", "auto_report",
				"report_thumbnail" };
			for (const char* key : plain)
				Assert::IsFalse(StateReport::IsSecretKey(key), Widen(key).c_str());
		}

		TEST_METHOD(AutoReportLimiterRateLimits)
		{
			StateReport::AutoReportLimiter limiter(600000, 5);
			Assert::AreEqual(0, limiter.Count());
			Assert::IsTrue(limiter.Allow(1000));
			Assert::IsFalse(limiter.Allow(2000));
			Assert::IsFalse(limiter.Allow(600999));
			Assert::IsTrue(limiter.Allow(601000));
			Assert::AreEqual(2, limiter.Count());
			int allowed = 2;
			uint64_t now = 601000;
			for (int i = 0; i < 20; ++i)
			{
				now += 10ull * 60 * 60 * 1000;
				if (limiter.Allow(now))
					++allowed;
			}
			Assert::AreEqual(5, allowed, L"at most five per run");
			Assert::AreEqual(5, limiter.Count());
		}

		TEST_METHOD(CollectSystemJsonHasNoIdentity)
		{
			const std::string json = StateReport::CollectSystemJson();
			Assert::IsTrue(json.compare(0, 1, "{") == 0);
			Assert::IsTrue(Contains(json, "\"os\""));
			Assert::IsTrue(Contains(json, "\"gpus\""));
			Assert::IsTrue(Contains(json, "\"displays\""));
			char computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
			DWORD size = sizeof(computer);
			if (GetComputerNameA(computer, &size) && size >= 3)
				Assert::IsFalse(Contains(json, computer), L"computer name");
			char user[256] = {};
			size = sizeof(user);
			if (GetUserNameA(user, &size) && size > 3)
				Assert::IsFalse(Contains(json, user), L"user name");
		}
	};

	TEST_CLASS(StateReportSettingsTests)
	{
	public:
		TEST_METHOD(AbsentKeysAreOff)
		{
			const std::string folder = TempFolder("settings-absent");
			ConfigFile config;
			Assert::IsTrue(LoadConfig(config, folder, "[logging]\nlevel: info\n"));
			StateReportSettings::Settings settings;
			settings.autoReport = true;
			settings.thumbnail = true;
			std::string error;
			Assert::IsTrue(StateReportSettings::Read(config, settings, error));
			Assert::IsFalse(settings.autoReport);
			Assert::IsFalse(settings.thumbnail);
			Assert::IsTrue(error.empty());
		}

		TEST_METHOD(BooleansAreRead)
		{
			const std::string folder = TempFolder("settings-true");
			ConfigFile config;
			Assert::IsTrue(LoadConfig(config, folder, "[logging]\nauto_report: true\nreport_thumbnail: true\n"));
			StateReportSettings::Settings settings;
			std::string error;
			Assert::IsTrue(StateReportSettings::Read(config, settings, error), Widen(error).c_str());
			Assert::IsTrue(settings.autoReport);
			Assert::IsTrue(settings.thumbnail);

			ConfigFile off;
			Assert::IsTrue(LoadConfig(off, folder, "[logging]\nauto_report: false\nreport_thumbnail: true\n"));
			Assert::IsTrue(StateReportSettings::Read(off, settings, error), Widen(error).c_str());
			Assert::IsFalse(settings.autoReport);
			Assert::IsTrue(settings.thumbnail);
		}

		TEST_METHOD(AnInvalidBooleanKeepsFalseAndReportsAnError)
		{
			const std::string folder = TempFolder("settings-bad");
			ConfigFile config;
			Assert::IsTrue(LoadConfig(config, folder, "[logging]\nauto_report: maybe\n"));
			StateReportSettings::Settings settings;
			std::string error;
			Assert::IsFalse(StateReportSettings::Read(config, settings, error));
			Assert::IsFalse(error.empty());
			Assert::IsFalse(settings.autoReport);
			Assert::IsFalse(settings.thumbnail);
		}

		TEST_METHOD(ChordsParse)
		{
			ACCEL accel = {};
			Assert::IsTrue(StateReportSettings::TryParseChord("Ctrl+Alt+D", accel));
			Assert::AreEqual(static_cast<int>(FVIRTKEY | FCONTROL | FALT), static_cast<int>(accel.fVirt));
			Assert::AreEqual(static_cast<int>('D'), static_cast<int>(accel.key));
			ACCEL spaced = {};
			Assert::IsTrue(StateReportSettings::TryParseChord("ctrl + alt + d", spaced));
			Assert::AreEqual(static_cast<int>(accel.fVirt), static_cast<int>(spaced.fVirt));
			Assert::AreEqual(static_cast<int>(accel.key), static_cast<int>(spaced.key));
			Assert::IsTrue(StateReportSettings::TryParseChord("Shift+F12", accel));
			Assert::AreEqual(static_cast<int>(FVIRTKEY | FSHIFT), static_cast<int>(accel.fVirt));
			Assert::AreEqual(static_cast<int>(VK_F12), static_cast<int>(accel.key));
			Assert::IsTrue(StateReportSettings::TryParseChord("Esc", accel));
			Assert::AreEqual(static_cast<int>(FVIRTKEY), static_cast<int>(accel.fVirt));
			Assert::AreEqual(static_cast<int>(VK_ESCAPE), static_cast<int>(accel.key));

			const char* const invalid[] = { "", "Ctrl+", "Ctrl+Alt", "Ctrl+DD", "Ctrl+F25", "Ctrl+Alt+D+E" };
			for (const char* chord : invalid)
				Assert::IsFalse(StateReportSettings::TryParseChord(chord, accel), Widen(chord).c_str());
		}

		TEST_METHOD(ShortcutResolution)
		{
			ACCEL accel = {};
			std::string error;
			Assert::IsTrue(StateReportSettings::ResolveShortcut(nullptr, accel, error));
			Assert::AreEqual(static_cast<int>(FVIRTKEY | FCONTROL | FALT), static_cast<int>(accel.fVirt));
			Assert::AreEqual(static_cast<int>('D'), static_cast<int>(accel.key));
			Assert::IsTrue(error.empty());

			const std::string folder = TempFolder("shortcut");
			ConfigFile absent;
			Assert::IsTrue(LoadConfig(absent, folder, "[shortcuts]\nshow_profiles: Ctrl+P\n"));
			accel = ACCEL();
			Assert::IsTrue(StateReportSettings::ResolveShortcut(&absent, accel, error));
			Assert::AreEqual(static_cast<int>('D'), static_cast<int>(accel.key));

			ConfigFile empty;
			Assert::IsTrue(LoadConfig(empty, folder, "[shortcuts]\nstate_report:\n"));
			Assert::IsFalse(StateReportSettings::ResolveShortcut(&empty, accel, error));
			Assert::IsTrue(error.empty(), L"empty means no shortcut, not an error");

			ConfigFile custom;
			Assert::IsTrue(LoadConfig(custom, folder, "[shortcuts]\nstate_report: Ctrl+Shift+F9\n"));
			Assert::IsTrue(StateReportSettings::ResolveShortcut(&custom, accel, error));
			Assert::AreEqual(static_cast<int>(FVIRTKEY | FCONTROL | FSHIFT), static_cast<int>(accel.fVirt));
			Assert::AreEqual(static_cast<int>(VK_F9), static_cast<int>(accel.key));

			ConfigFile bad;
			Assert::IsTrue(LoadConfig(bad, folder, "[shortcuts]\nstate_report: Hyper+Q\n"));
			Assert::IsFalse(StateReportSettings::ResolveShortcut(&bad, accel, error));
			Assert::IsFalse(error.empty());
		}
	};
}
