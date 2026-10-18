#pragma once

// Report-a-problem bundle (owner 2026-10-17). The state_report shortcut
// (default Ctrl+Alt+D), the system-menu item "Save problem report" and, when
// [logging] auto_report is on, VP itself write logs\reports\vp-report-<time>.zip:
// a store-only ZIP (StoreZipWriter.h) a person can send and Claude can read.
//
//   README.md            field guide: what every file and field means
//   manifest.json        build ID, commit, plugin API, EXE/DLL SHA-256, contents
//   state-history.jsonl  the DiagnosticHistory rings of the EXE and renderer DLL
//   log-excerpt.txt      the tail of the active log (redacted)
//   crash-state.txt      the live crash-state values now
//   crash-state-last.txt the newest kept "crash state (N).txt", if any
//   live-settings.json   the effective profile selections and runtime state
//   config/<file>        each configuration file, redacted
//   system.json          GPU, driver, OS, displays and modes
//   thumbnail.png        only when [logging] report_thumbnail is on (default off)
//
// No video pixels unless the thumbnail switch is on, no minidump, no user or
// computer name, no paths outside VP's folder, no secrets (Redact()).
// The newest 20 reports are kept (ApplyRetention).
//
// C++14.

#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace StateReport
{
	constexpr int kDefaultKeep = 20;
	constexpr const char* kFilePrefix = "vp-report-";
	constexpr const char* kFileSuffix = ".zip";
	constexpr int kFormatVersion = 1;

	// ---- Redaction -------------------------------------------------------------

	struct RedactionContext
	{
		// Folders whose paths are kept, shown with the given placeholder, e.g.
		// { "C:\\Videoprocessor\\vp", "<vp>" }, { "C:\\Windows", "<windows>" }.
		std::vector<std::pair<std::string, std::string>> keptRoots;
		// Words replaced wherever they appear (case-insensitive), e.g. the user
		// name -> "<user>", the computer name -> "<host>". Words shorter than 3
		// characters are ignored (they would erase ordinary text).
		std::vector<std::pair<std::string, std::string>> words;
	};

	// The context for this process: the EXE folder as <vp>, the Windows folder
	// as <windows>, the user and computer names.
	RedactionContext CurrentRedactionContext();

	// Removes personal data and secrets from text (config, log lines, JSON):
	//  - absolute paths (X:\..., X:/..., \\server\...) outside the kept roots
	//    become "<path>"; inside a kept root the root becomes its placeholder;
	//  - values of keys naming a secret (password, secret, token, api_key,
	//    credential, auth, serial, license, host, address, url, ...) become
	//    "<redacted>" ("key: value", "key = value", "\"key\": \"value\"");
	//  - e-mail addresses become "<email>", URLs become "<url>";
	//  - the context's words (user name, computer name) become placeholders.
	std::string Redact(const std::string& text, const RedactionContext& context);

	// True when a key name names a secret (exposed for tests).
	bool IsSecretKey(const std::string& key);

	// ---- Files and retention ---------------------------------------------------

	// "vp-report-20261017-213045-123.zip" for the given local time.
	std::string FileName(const SYSTEMTIME& localTime);
	// The report files in `directory`, oldest first (by name = by time).
	std::vector<std::wstring> ListReports(const std::wstring& directory);
	// Deletes the oldest reports beyond `keep`; returns the names removed.
	std::vector<std::wstring> ApplyRetention(const std::wstring& directory, int keep = kDefaultKeep);

	// Lower-case hex SHA-256 of a file; empty when it cannot be read.
	std::string Sha256File(const std::wstring& path, uint64_t* bytes = nullptr);
	std::string Sha256Hex(const void* data, size_t size);

	// The last `maxBytes` of a text file, starting at a line boundary.
	std::string ReadFileTail(const std::wstring& path, size_t maxBytes);
	std::string ReadWholeFile(const std::wstring& path, size_t maxBytes = 8 * 1024 * 1024);

	// The newest kept "crash state (N).txt" in `directory` (empty when none),
	// with its file name.
	std::string NewestKeptCrashState(const std::wstring& directory, std::string* name = nullptr);

	// ---- System ----------------------------------------------------------------

	// GPU adapters (name, vendor/device, driver version, memory), outputs
	// (current mode, refresh, bit depth, HDR colour space and luminance), the
	// active display paths (monitor model, exact refresh), OS version, CPU,
	// memory and power source. No user or computer name.
	std::string CollectSystemJson();

	// Scales an image file (PNG/BMP/JPEG, via WIC) to at most maxWidth pixels
	// wide and encodes it as PNG. COM must be initialised on the thread.
	bool MakeThumbnailPng(const std::wstring& imagePath, unsigned maxWidth,
		std::vector<unsigned char>& png, std::string* error = nullptr);

	// ---- The bundle ------------------------------------------------------------

	struct BinaryFile
	{
		std::string name;      // shown name, relative to the VP folder
		std::wstring path;     // full path (never written to the bundle)
	};

	struct Inputs
	{
		std::wstring vpRoot;              // folder of VideoProcessor.exe
		std::wstring reportsDirectory;    // usually <vpRoot>\logs\reports
		std::string trigger;              // "hotkey", "menu", "auto:<reason>", "test"
		std::string buildId;
		std::string commit;
		std::string version;              // VP version text (version.h)
		uint32_t pluginApi = 0;
		std::string rendererName;         // "VP Renderer", "DirectShow", "none"
		std::vector<BinaryFile> binaries;
		std::string historyExe;           // DiagnosticHistory JSONL, src "exe"
		std::string historyRenderer;      // from IVideoRenderer::WriteDiagnosticHistory
		bool rendererContributed = false;
		std::string logExcerpt;
		std::string crashStateLive;
		std::wstring crashStateDirectory; // where "crash state (N).txt" are kept
		std::vector<std::pair<std::string, std::string>> liveSettings;
		std::vector<std::wstring> configFiles;
		std::string systemJson;           // empty = CollectSystemJson()
		bool thumbnailEnabled = false;
		std::vector<unsigned char> thumbnailPng;
		std::string thumbnailNote;        // why there is none, when enabled
		int keep = kDefaultKeep;
		SYSTEMTIME localTime = {};        // wYear 0 = now
		RedactionContext redaction;       // empty = CurrentRedactionContext()
	};

	struct Result
	{
		bool ok = false;
		std::wstring path;                // the written zip
		std::string fileName;
		std::string error;
		size_t bytes = 0;
		std::vector<std::string> entries;
		std::vector<std::wstring> removed;
	};

	std::string ManifestJson(const Inputs& inputs, const std::string& createdLocal,
		const std::vector<std::pair<std::string, std::string>>& binaryHashes,
		const std::vector<std::string>& contents, size_t exeRows, size_t rendererRows);
	std::string LiveSettingsJson(const std::vector<std::pair<std::string, std::string>>& settings);
	std::string ReadmeText();

	// Builds the archive in memory, writes it atomically (temporary file, then
	// rename) and applies retention.
	Result Write(const Inputs& inputs);

	// Escapes a string for a JSON string literal (no surrounding quotes).
	std::string JsonEscape(const std::string& text);

	// ---- auto_report ------------------------------------------------------------

	// auto_report rate limit: at most one automatic report per
	// `minimumIntervalMs` and at most `maxPerSession` per run. Manual reports
	// (hotkey, menu) are not limited.
	class AutoReportLimiter
	{
	public:
		explicit AutoReportLimiter(uint64_t minimumIntervalMs = 10ull * 60 * 1000, int maxPerSession = 5)
			: minimumIntervalMs_(minimumIntervalMs), maxPerSession_(maxPerSession) {}
		// True (and counted) when an automatic report may be written now.
		bool Allow(uint64_t nowMs)
		{
			if (count_ >= maxPerSession_)
				return false;
			if (count_ > 0 && nowMs - lastMs_ < minimumIntervalMs_)
				return false;
			lastMs_ = nowMs;
			++count_;
			return true;
		}
		int Count() const { return count_; }

	private:
		uint64_t minimumIntervalMs_;
		int maxPerSession_;
		uint64_t lastMs_ = 0;
		int count_ = 0;
	};
}
