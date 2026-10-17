#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Crash state (owner 2026-10-15): a small text file of the last few seconds
// of VP's state, rewritten about once a second and deleted on a clean exit.
// A run that crashes (or is killed) leaves it behind; the next start keeps it
// as "crash state (N).txt" - at most `keep`, the oldest dropped - so anyone
// who hit a crash can send it. The write is atomic (a temporary file, then a
// replace), so a crash in the middle of a write never leaves a torn file. On
// an unhandled exception the handler writes once more and adds a small
// Windows minidump ("crash state (N).dmp" after the next start).
namespace CrashState
{
	constexpr const char* kLiveName = "crash-state.txt";
	constexpr const char* kLiveDumpName = "crash-state.dmp";
	constexpr int kDefaultKeep = 10;

	// Keeps a leftover live file (and dump) from a run that did not exit
	// cleanly as "crash state (N).txt" / ".dmp", N one above the highest kept,
	// then removes the oldest beyond `keep`. Returns the names it created.
	std::vector<std::string> PreserveLeftover(const std::string& directory, int keep = kDefaultKeep);

	// The numbers of the kept "crash state (N).txt" files, ascending.
	std::vector<int> KeptNumbers(const std::string& directory);

	class Recorder
	{
	public:
		using LinesSource = std::function<std::vector<std::string>()>;

		Recorder() = default;
		~Recorder();
		Recorder(const Recorder&) = delete;
		Recorder& operator=(const Recorder&) = delete;

		// Starts writing `directory`\crash-state.txt every `periodMs`; `recent`
		// supplies the recent log lines (it is called on the writer thread).
		bool Start(const std::string& directory, LinesSource recent, int periodMs = 1000);
		// A named value shown in the state section (replaces an earlier value).
		void SetValue(const std::string& key, const std::string& value);
		// Writes the file now (atomically). False when not started or the write failed.
		bool WriteNow();
		// Stops the writer. clean = a normal exit: the live file is deleted;
		// otherwise it stays for the next start to keep.
		void Stop(bool clean);
		bool Running() const { return running.load(); }
		std::string LivePath() const;
		std::string Text() const;   // what the next write would contain (tests)

		// Unhandled exceptions and std::terminate: a final write and a minidump
		// beside the live file, then Windows' normal handling continues.
		void InstallCrashHandler();
		bool WriteMinidump(void* exceptionPointers);
		// audit 2026-10-16: the handlers' write. Sets "crash", never blocks on a
		// held lock (bounded try-lock), ignores re-entry on the same thread and
		// uses the recent log lines of the last periodic write. True when written.
		bool WriteFromHandler(const std::string& crash);

	private:
		void Loop();
		std::string Compose(const std::map<std::string, std::string>& valuesIn,
			const std::vector<std::string>& recentLines) const;
		bool WriteAtomically(const std::string& content);

		std::string directory;
		LinesSource recent;
		int periodMs = 1000;
		unsigned long long startedTick = 0;
		std::string startedAt;
		mutable std::mutex mutex;
		std::map<std::string, std::string> values;
		std::vector<std::string> lastRecent;   // guarded by `mutex`
		std::mutex writeMutex;
		std::thread worker;
		std::condition_variable wake;
		std::mutex wakeMutex;
		std::atomic<bool> running{ false };
		std::atomic<bool> stopping{ false };
	};

	// The process-wide recorder the app starts.
	Recorder& Global();
}
