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
//
// Bug hunt 3 (SH12, 2026-10-18): the live files are per instance -
// "crash-state-<pid>.txt" / ".tmp" / ".dmp" - and each running instance holds
// "crash-state-<pid>.lock" open exclusively (deleted on close, also when the
// process is killed). Two VPs started together used to share one live file:
// the second start renamed the first's live state to "crash state (N)" (a
// crash that never happened, counted against the ten kept) and both then
// overwrote one file. Now a start keeps only the leftovers whose owner is
// gone, so the kept files are real unclean exits.
namespace CrashState
{
	// The single live name of earlier versions (still kept as a leftover).
	constexpr const char* kLiveName = "crash-state.txt";
	constexpr const char* kLiveDumpName = "crash-state.dmp";
	constexpr int kDefaultKeep = 10;

	// "crash-state-<instance>" + extension (".txt", ".dmp", ".lock").
	std::string InstanceFileName(unsigned long instanceId, const char* extension);

	// Keeps every leftover live file (and dump) whose instance is no longer
	// running - plus the single-name leftover of earlier versions - as
	// "crash state (N).txt" / ".dmp", N one above the highest kept (oldest
	// leftover first), then removes the oldest beyond `keep`. A running
	// instance's live files are never touched. Returns the names it created.
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

		// Starts writing `directory`\crash-state-<instance>.txt every `periodMs`;
		// `recent` supplies the recent log lines (called on the writer thread, and
		// also on the caller's thread by Text() and by Stop()'s final write, so it
		// must be thread-safe). instanceId names the live files (0 = this
		// process's ID); tests run two "instances" in one process with distinct IDs.
		// If the instance lock (crash-state-<instance>.lock, SH12) cannot be taken,
		// the live files are named crash-state-<instance>.unlocked.txt / .dmp
		// instead: another instance's PreserveLeftover never takes an unlocked
		// instance's live file for a crash (that was the false "crash state (N)"),
		// at the cost that this instance's own crash is not kept as one.
		// InstanceLocked() tells which; the failure is logged.
		bool Start(const std::string& directory, LinesSource recent, int periodMs = 1000,
			unsigned long instanceId = 0);
		// A named value shown in the state section (replaces an earlier value).
		void SetValue(const std::string& key, const std::string& value);
		// Writes the file now (atomically). False when not started or the write failed.
		bool WriteNow();
		// Stops the writer. clean = a normal exit: the live file is deleted;
		// otherwise it stays for the next start to keep.
		void Stop(bool clean);
		bool Running() const { return running.load(); }
		std::string LivePath() const;
		std::string LiveDumpPath() const;
		unsigned long InstanceId() const { return instanceId; }
		// False when Start could not take the instance lock (see Start).
		bool InstanceLocked() const { return !unlocked; }
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
		unsigned long instanceId = 0;
		void* instanceLock = nullptr;   // HANDLE of crash-state-<id>.lock
		bool unlocked = false;          // Start could not take the lock: ".unlocked" live names
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
