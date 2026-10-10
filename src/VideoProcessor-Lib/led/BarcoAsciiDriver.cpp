#include <pch.h>
#include "BarcoAsciiDriver.h"

#include "../DebugLog.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace LedDrive
{
	namespace
	{
		bool AllOf(const std::string& text, int (*predicate)(int))
		{
			return std::all_of(text.begin(), text.end(),
				[predicate](char c) { return predicate(static_cast<unsigned char>(c)) != 0; });
		}

		std::string Printable(const std::string& bytes)
		{
			std::string text;
			for (char c : bytes)
				text += c == '\r' ? std::string("\\r") : c == '\n' ? std::string("\\n") : std::string(1, c);
			return text;
		}

		bool SameMnemonic(const std::string& a, const char* b)
		{
			const std::string other(b);
			return a.size() == other.size() && std::equal(a.begin(), a.end(), other.begin(),
				[](char x, char y)
				{
					return std::toupper(static_cast<unsigned char>(x)) ==
						std::toupper(static_cast<unsigned char>(y));
				});
		}
	}

	BarcoAsciiDriver::BarcoAsciiDriver(const ProjectorConfig& configIn,
		std::unique_ptr<ISerialPort> portIn)
		: config(configIn), port(std::move(portIn)),
		framing{ "cr", false, "\r", configIn.readBack },
		nextSendAllowed(std::chrono::steady_clock::now())
	{
	}

	BarcoAsciiDriver::~BarcoAsciiDriver()
	{
		Close();
	}

	bool BarcoAsciiDriver::ParseReply(const std::string& line, std::string& mnemonic, int& value)
	{
		std::istringstream fields(line);
		std::string address, name, number, extra;
		if (!(fields >> address >> name >> number) || (fields >> extra))
			return false;
		if (address.size() != 4 || address[0] != '%' ||
			!AllOf(address.substr(1), ::isdigit))
			return false;
		if (name.size() != 4 || !AllOf(name, ::isalnum))
			return false;
		if (number.empty() || number.size() > 9 || !AllOf(number, ::isdigit))
			return false;
		mnemonic = name;
		value = std::stoi(number);
		return true;
	}

	std::string BarcoAsciiDriver::Format(const char* mnemonic, const std::string& argument) const
	{
		std::string name(mnemonic);
		if (framing.lowercase)
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return ":" + name + argument + framing.suffix;
	}

	bool BarcoAsciiDriver::Transact(const std::string& command, const char* mnemonic,
		int* value, int timeoutMs, int maxGapWaitMs, int sentLight, bool abortable)
	{
		using namespace std::chrono;
		steady_clock::time_point allowed;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			allowed = nextSendAllowed;
		}
		const auto now = steady_clock::now();
		if (allowed > now)
		{
			auto wait = allowed - now;
			if (maxGapWaitMs >= 0 && wait > milliseconds(maxGapWaitMs))
			{
				// Only the bounded exit restore gets here: full light matters
				// more than the rest of a pause.
				DebugLog::Log("LED drive: \"%s\" sent %lld ms before the command gap/pause allowed (exit is bounded)",
					Printable(command).c_str(),
					static_cast<long long>(duration_cast<milliseconds>(wait).count() - maxGapWaitMs));
				wait = milliseconds(maxGapWaitMs);
			}
			std::this_thread::sleep_for(wait);
		}
		port->DiscardInput();
		std::string error;
		const bool written = port->Write(command, error);
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			if (written)
			{
				++commands;
				++sinceBurst;
				if (sentLight >= 0)
				{
					++lightSends;
					lastSentLight = sentLight;
				}
			}
		}
		bool ok = written && !framing.acknowledged;
		if (!written)
			DebugLog::Log("LED drive: projector write of \"%s\" failed: %s",
				Printable(command).c_str(), error.c_str());
		else if (framing.acknowledged)
		{
			const auto deadline = steady_clock::now() + milliseconds(timeoutMs);
			std::string line;
			for (;;)
			{
				const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
				if (remaining <= 0)
					break;
				// Short reads, so an exit can end the wait of a command in flight.
				if (!port->ReadLine(static_cast<int>(std::min<long long>(remaining, 50)), line))
				{
					std::lock_guard<std::mutex> guard(stateMutex);
					if (abortable && shuttingDown) break;
					continue;
				}
				std::string name;
				int parsed = 0;
				if (ParseReply(line, name, parsed) && SameMnemonic(name, mnemonic))
				{
					if (value) *value = parsed;
					ok = true;
					break;
				}
				if (!line.empty())
					DebugLog::Log("LED drive: projector reply \"%s\" ignored while waiting for %s",
						Printable(line).c_str(), mnemonic);
			}
			if (!ok)
			{
				{
					std::lock_guard<std::mutex> guard(stateMutex);
					++timeouts;
				}
				DebugLog::Log("LED drive: no %s reply within %d ms to \"%s\"",
					mnemonic, timeoutMs, Printable(command).c_str());
			}
		}
		int gap = config.minCommandGapMs;
		// Barco: at least 2 s before sending again when no reply came.
		if (written && framing.acknowledged && !ok) gap = std::max(gap, 2000);
		std::lock_guard<std::mutex> guard(stateMutex);
		if (config.burstCommands > 0 && sinceBurst >= config.burstCommands)
		{
			gap = std::max(gap, config.burstPauseMs);
			sinceBurst = 0;
		}
		nextSendAllowed = steady_clock::now() + milliseconds(gap);
		return ok;
	}

	bool BarcoAsciiDriver::QueryLocked(const char* mnemonic, const std::string& argument, int& value)
	{
		return framing.acknowledged &&
			Transact(Format(mnemonic, argument), mnemonic, &value, config.ackTimeoutMs, -1);
	}

	// Candidate framings, tried in order for terminator: auto. The script's
	// bytes (lower case, literal "cr", then echo's CR LF) are the
	// unacknowledged fallback: what the projector accepted for years without
	// anyone reading its replies.
	void BarcoAsciiDriver::SelectFraming()
	{
		const Framing cr{ "cr", false, "\r", true };
		const Framing literalCr{ "literal_cr", false, "cr\r", true };
		const Framing script{ "script", true, "cr\r\n", false };
		if (!config.readBack)
		{
			// Send-only: no probing, no ECHO, no queries.
			switch (config.terminator)
			{
			case Terminator::Cr: framing = { "cr", false, "\r", false }; break;
			case Terminator::LiteralCr: framing = { "literal_cr", false, "cr\r", false }; break;
			default: framing = script; break;
			}
			DebugLog::Log("LED drive: read_back off; sending with framing \"%s\" without waiting for replies",
				framing.name);
			return;
		}
		std::vector<Framing> candidates;
		switch (config.terminator)
		{
		case Terminator::Cr: candidates = { cr }; break;
		case Terminator::LiteralCr: candidates = { literalCr }; break;
		case Terminator::Script: framing = script; return;
		default: candidates = { cr, literalCr }; break;
		}
		for (const Framing& candidate : candidates)
		{
			framing = candidate;
			if (config.echo)
				Transact(Format("ECHO", "1"), "ECHO", nullptr, config.ackTimeoutMs, -1);
			int light = 0;
			if (QueryLocked("LPW1", "?", light))
			{
				std::lock_guard<std::mutex> guard(stateMutex);
				state.lightKnown = true;
				state.light = light;
				DebugLog::Log("LED drive: projector answers with framing \"%s\" (LPW1=%d)",
					framing.name, light);
				return;
			}
		}
		if (config.terminator == Terminator::Auto)
		{
			framing = script;
			DebugLog::Log(
				"LED drive: projector did not answer with \\r or cr\\r framing; sending the old script's exact bytes without waiting for replies");
		}
		else
			DebugLog::Log("LED drive: projector did not answer with configured framing \"%s\"",
				framing.name);
	}

	bool BarcoAsciiDriver::Open(std::string& error)
	{
		Close();
		if (!port || !port->Open(config.port, config.baud, error))
		{
			if (!port) error = "no serial port";
			DebugLog::Log("LED drive: %s; projector control disabled", error.c_str());
			return false;
		}
		{
			std::lock_guard<std::timed_mutex> guard(portMutex);
			SelectFraming();
			int minimum = 0, maximum = 0;
			if (framing.acknowledged)
			{
				const bool haveMin = config.lightMin >= 0 || QueryLocked("LPW1", " ?N", minimum);
				const bool haveMax = config.lightMax >= 0 || QueryLocked("LPW1", " ?M", maximum);
				int contrast = 0;
				const bool haveContrast = QueryLocked("CNTR", "?", contrast);
				std::lock_guard<std::mutex> stateGuard(stateMutex);
				if (haveMin && haveMax)
				{
					range.known = true;
					range.minimum = config.lightMin >= 0 ? config.lightMin : minimum;
					range.maximum = config.lightMax >= 0 ? config.lightMax : maximum;
				}
				state.contrastKnown = haveContrast;
				state.contrast = contrast;
			}
		}
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			stopping = false;
			shuttingDown = false;
			pendingLight = -1;
			opened = true;
		}
		worker = std::thread(&BarcoAsciiDriver::Worker, this);
		const LightRange reported = QueryLightRange();
		DebugLog::Log("LED drive: barco_ascii on %s %d baud, framing=%s acknowledged=%d range=%s",
			config.port.c_str(), config.baud, framing.name, framing.acknowledged ? 1 : 0,
			reported.known ? (std::to_string(reported.minimum) + "-" +
				std::to_string(reported.maximum)).c_str() : "unknown");
		return true;
	}

	LightRange BarcoAsciiDriver::QueryLightRange()
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		LightRange result = range;
		if (!result.known && config.lightMin >= 0 && config.lightMax >= 0)
		{
			result.known = true;
			result.minimum = config.lightMin;
			result.maximum = config.lightMax;
		}
		return result;
	}

	ProjectorState BarcoAsciiDriver::ReadState()
	{
		if (opened && framing.acknowledged)
		{
			std::lock_guard<std::timed_mutex> guard(portMutex);
			int light = 0, contrast = 0;
			const bool haveLight = QueryLocked("LPW1", "?", light);
			const bool haveContrast = QueryLocked("CNTR", "?", contrast);
			std::lock_guard<std::mutex> stateGuard(stateMutex);
			if (haveLight) { state.lightKnown = true; state.light = light; }
			if (haveContrast) { state.contrastKnown = true; state.contrast = contrast; }
		}
		std::lock_guard<std::mutex> guard(stateMutex);
		return state;
	}

	int BarcoAsciiDriver::Clamp(int level) const
	{
		int low = config.lightMin >= 0 ? config.lightMin : range.known ? range.minimum : 0;
		int high = config.lightMax >= 0 ? config.lightMax : range.known ? range.maximum : 255;
		if (low > high) std::swap(low, high);
		return std::max(low, std::min(level, high));
	}

	void BarcoAsciiDriver::SetLight(int level)
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!opened) return;
		pendingLight = Clamp(level);
		wake.notify_one();
	}

	bool BarcoAsciiDriver::QueueSequence(const std::vector<std::pair<std::string, int>>& commands, int stepMs)
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!opened || shuttingDown) return false;
		sequence.assign(commands.begin(), commands.end());
		sequenceStepMs = std::max(1, stepMs);
		wake.notify_one();
		return true;
	}

	bool BarcoAsciiDriver::SendNow(const std::string& mnemonic, int value, int timeoutMs)
	{
		using namespace std::chrono;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			if (!opened) return false;
		}
		std::unique_lock<std::timed_mutex> lock(portMutex, std::defer_lock);
		const auto start = steady_clock::now();
		if (timeoutMs > 0)
		{
			if (!lock.try_lock_for(milliseconds(timeoutMs / 2))) return false;
		}
		else
			lock.lock();
		const int left = timeoutMs > 0 ? std::max(0, timeoutMs -
			static_cast<int>(duration_cast<milliseconds>(steady_clock::now() - start).count())) : 0;
		return timeoutMs > 0 ?
			Transact(Format(mnemonic.c_str(), std::to_string(value)), mnemonic.c_str(), nullptr,
				std::max(1, std::min(left / 2, config.ackTimeoutMs)), left / 2) :
			Transact(Format(mnemonic.c_str(), std::to_string(value)), mnemonic.c_str(), nullptr,
				config.ackTimeoutMs, -1);
	}

	void BarcoAsciiDriver::BeginShutdown()
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		shuttingDown = true;
		pendingLight = -1;
		sequence.clear();
		wake.notify_all();
	}

	bool BarcoAsciiDriver::SetLightNow(int level, int timeoutMs)
	{
		using namespace std::chrono;
		const auto start = steady_clock::now();
		int clamped = 0;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			if (!opened) return false;
			pendingLight = -1;
			clamped = Clamp(level);
		}
		std::unique_lock<std::timed_mutex> lock(portMutex, std::defer_lock);
		if (timeoutMs > 0)
		{
			// Bounded (exit): the whole call stays within timeoutMs - up to half
			// waiting for a command in flight (BeginShutdown ends its reply
			// wait), the rest split between the gap and the reply.
			if (!lock.try_lock_for(milliseconds(timeoutMs / 2)))
			{
				DebugLog::Log("LED drive: port still busy after %d ms; light not restored", timeoutMs / 2);
				return false;
			}
		}
		else
			lock.lock();
		int confirmed = -1;
		const int left = timeoutMs > 0 ? std::max(0, timeoutMs -
			static_cast<int>(duration_cast<milliseconds>(steady_clock::now() - start).count())) : 0;
		const bool ok = timeoutMs > 0 ?
			Transact(Format("LPW1", std::to_string(clamped)), "LPW1", &confirmed,
				std::max(1, std::min(left / 2, config.ackTimeoutMs)), left / 2, clamped) :
			Transact(Format("LPW1", std::to_string(clamped)), "LPW1", &confirmed,
				config.ackTimeoutMs, -1, clamped);
		std::lock_guard<std::mutex> stateGuard(stateMutex);
		if (ok) lastLight = confirmed >= 0 ? confirmed : clamped;
		return ok;
	}

	bool BarcoAsciiDriver::SetContrast(int value)
	{
		if (!opened) return false;
		std::lock_guard<std::timed_mutex> guard(portMutex);
		int confirmed = -1;
		const bool ok = Transact(Format("CNTR", std::to_string(value)), "CNTR", &confirmed,
			config.ackTimeoutMs, -1);
		std::lock_guard<std::mutex> stateGuard(stateMutex);
		if (ok)
		{
			state.contrastKnown = true;
			state.contrast = confirmed >= 0 ? confirmed : value;
		}
		return ok;
	}

	void BarcoAsciiDriver::Worker()
	{
		for (;;)
		{
			int target = -1;
			{
				std::unique_lock<std::mutex> lock(stateMutex);
				wake.wait(lock, [this] { return stopping || pendingLight >= 0 || !sequence.empty(); });
				if (!stopping && !shuttingDown && !sequence.empty())
				{
					// Experimental custom dim: its own spacing (ftb_step_ms, like
					// the old script's 100 ms contrast steps), ahead of targets.
					while (!stopping && std::chrono::steady_clock::now() < sequenceNextAllowed)
						wake.wait_until(lock, sequenceNextAllowed);
					if (stopping) return;
					if (shuttingDown || sequence.empty()) continue;
					const std::pair<std::string, int> command = sequence.front();
					sequence.pop_front();
					sequenceInFlight = true;
					nextSendAllowed = std::chrono::steady_clock::now();
					lock.unlock();
					{
						std::lock_guard<std::timed_mutex> guard(portMutex);
						bool skip = false;
						{
							std::lock_guard<std::mutex> stateGuard(stateMutex);
							skip = shuttingDown;
						}
						if (!skip)
							Transact(Format(command.first.c_str(), std::to_string(command.second)),
								command.first.c_str(), nullptr, config.ackTimeoutMs, -1, -1, true);
					}
					lock.lock();
					sequenceInFlight = false;
					sequenceNextAllowed = std::chrono::steady_clock::now() +
						std::chrono::milliseconds(sequenceStepMs);
					if (sequence.empty()) ++sequencesDone;
					continue;
				}
				// Wait out the gap or pause here, where Close() can interrupt
				// it, and keep taking newer targets meanwhile.
				while (!stopping && std::chrono::steady_clock::now() < nextSendAllowed)
					wake.wait_until(lock, nextSendAllowed);
				if (stopping) return;
				if (shuttingDown || pendingLight < 0) continue;
				target = pendingLight;
				pendingLight = -1;
				// Dim at most max_levels_per_command per command; the rest of
				// the drop stays pending for the next command unless a newer
				// target replaces it.
				if (config.maxLevelsPerCommand > 0 && lastSentLight >= 0 &&
					target < lastSentLight - config.maxLevelsPerCommand)
				{
					pendingLight = target;
					target = lastSentLight - config.maxLevelsPerCommand;
				}
				transmitting = true;
			}
			std::lock_guard<std::timed_mutex> guard(portMutex);
			{
				// The exit restore may have begun while this target waited for
				// the port: never send a stale level after it.
				std::lock_guard<std::mutex> stateGuard(stateMutex);
				if (shuttingDown)
				{
					transmitting = false;
					continue;
				}
			}
			int confirmed = -1;
			const bool ok = Transact(Format("LPW1", std::to_string(target)), "LPW1", &confirmed,
				config.ackTimeoutMs, -1, target, true);
			std::lock_guard<std::mutex> stateGuard(stateMutex);
			transmitting = false;
			if (ok) lastLight = confirmed >= 0 ? confirmed : target;
		}
	}

	void BarcoAsciiDriver::Close()
	{
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			stopping = true;
			opened = false;
			pendingLight = -1;
		}
		wake.notify_all();
		if (worker.joinable()) worker.join();
		if (port) port->Close();
	}

	DriverStatus BarcoAsciiDriver::Status() const
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		DriverStatus status;
		status.framing = framing.name;
		status.acknowledged = framing.acknowledged;
		status.commands = commands;
		status.timeouts = timeouts;
		status.lastLight = lastLight;
		status.lightSends = lightSends;
		status.lastSentLight = lastSentLight;
		status.busy = pendingLight >= 0 || transmitting;
		status.sequenceBusy = !sequence.empty() || sequenceInFlight;
		status.sequencesDone = sequencesDone;
		return status;
	}
}
