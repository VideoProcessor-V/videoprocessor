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
		nextSendAllowed(std::chrono::steady_clock::now())
	{
		// Before Open: the first probe framing (the FL35's "cr"), acknowledged
		// with read_back on - as before profiles.
		const DeviceProfile& device = config.device;
		const DeviceFraming* first = device.Framing(device.probeOrder.empty() ? device.sendOnly : device.probeOrder.front());
		if (!first && !device.framings.empty()) first = &device.framings.front();
		if (first) framing = { first->name, first->letterCase, first->suffix, configIn.readBack };
		if (port) port->SetFormat(device.dataBits, device.parity, device.stopBits, device.flowControl);
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

	// Device profiles (feat/dd-projector-profiles): every byte comes from the
	// profile's templates in the current framing. A value outside the declared
	// device range, or a command the profile does not have, renders nothing and
	// is never sent.
	bool BarcoAsciiDriver::RenderSet(const std::string& name, int value, std::string& bytes, std::string& reply) const
	{
		const DeviceProfile& device = config.device;
		DeviceCommandId id = DeviceCommandId::Light;
		if (!DeviceCommandFromId(name, id))
		{
			DebugLog::Log("Dynamic Drive: unknown command %s %d not sent", name.c_str(), value);
			return false;
		}
		int deviceValue = value;
		if (id == DeviceCommandId::Light) deviceValue = device.LightToDevice(value);
		else if (id == DeviceCommandId::Contrast) deviceValue = device.ContrastToDevice(value);
		else if (id == DeviceCommandId::DimMode)
		{
			// VP's 0 = standard, 1 = custom dim mode.
			const DeviceCommand& dim = device.Command(id);
			deviceValue = value == 0 ? dim.standard : value == 1 ? dim.custom : dim.max + 1;
		}
		const DeviceFraming format{ framing.name, framing.suffix, framing.letterCase, framing.acknowledged };
		std::string error;
		if (!RenderDeviceSet(device, format, id, deviceValue, bytes, error))
		{
			DebugLog::Log("Dynamic Drive: %s %d not sent (%s): %s", name.c_str(), value, device.DisplayName().c_str(),
				error.c_str());
			return false;
		}
		reply = device.Command(id).reply;
		return true;
	}

	bool BarcoAsciiDriver::RenderQuery(int command, int which, std::string& bytes, std::string& reply) const
	{
		const DeviceProfile& device = config.device;
		const DeviceCommandId id = static_cast<DeviceCommandId>(command);
		const DeviceFraming format{ framing.name, framing.suffix, framing.letterCase, framing.acknowledged };
		if (!RenderDeviceQuery(device, format, id, static_cast<DeviceQuery>(which), bytes)) return false;
		reply = device.Command(id).reply;
		return !reply.empty();
	}

	bool BarcoAsciiDriver::Transact(const std::string& command, const char* mnemonic,
		int* value, int timeoutMs, int maxGapWaitMs, int sentLight, bool abortable, int sentContrast,
		const int* generation, int expectedGeneration, bool* wrote)
	{
		using namespace std::chrono;
		if (wrote) *wrote = false;
		{
			std::unique_lock<std::mutex> lock(stateMutex);
			// Post-merge audit 2026-10-17: a value superseded (CancelLight, SetLightNow,
			// a hold) before or during the gap wait is not sent.
			const auto superseded = [&] { return generation && *generation != expectedGeneration; };
			if (superseded()) return false;
			const steady_clock::time_point allowed = nextSendAllowed;
			const auto now = steady_clock::now();
			if (allowed > now)
			{
				auto until = allowed;
				if (maxGapWaitMs >= 0 && allowed - now > milliseconds(maxGapWaitMs))
				{
					// Only the bounded exit restore gets here: full light matters
					// more than the rest of a pause.
					DebugLog::Log("Dynamic Drive: \"%s\" sent %lld ms before the command gap/pause allowed (exit is bounded)",
						Printable(command).c_str(),
						static_cast<long long>(duration_cast<milliseconds>(allowed - now).count() - maxGapWaitMs));
					until = now + milliseconds(maxGapWaitMs);
				}
				// audit 2026-10-16: the pre-wait (up to 2 s after a missing reply)
				// holds portMutex; the driver thread's and the script's commands
				// (abortable) end it as soon as the exit restore begins, unsent,
				// so the restore is not locked out of the port.
				if (abortable)
				{
					gapWake.wait_until(lock, until, [&] { return shuttingDown || stopping || superseded(); });
					if (shuttingDown || stopping) return false;
				}
				else
					while (steady_clock::now() < until && !superseded())
						gapWake.wait_until(lock, until);
			}
			// Rechecked after the gap wait, under stateMutex.
			if (superseded())
			{
				DebugLog::Log("Dynamic Drive: \"%s\" not sent (superseded during the command gap)",
					Printable(command).c_str());
				return false;
			}
		}
		std::string error;
		bool written = false;
		// audit 2026-10-16: a port closed after failed writes is reopened
		// here (with its back-off) instead of failing for the rest of the run.
		if (EnsurePortLocked(error))
		{
			port->DiscardInput();
			written = port->Write(command, error);
		}
		lastWriteOk = written;
		if (wrote) *wrote = written;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			if (written)
			{
				++commands;
				++sinceBurst;
				// led_landing_ms (2026-10-17): the moment of the write.
				const auto writtenAt = steady_clock::now();
				if (sentLight >= 0)
				{
					++lightSends;
					lastSentLight = sentLight;
					lastLightSentAt = writtenAt;
				}
				if (sentContrast >= 0)
				{
					++contrastSends;
					lastSentContrast = sentContrast;
					lastContrastSentAt = writtenAt;
				}
			}
		}
		// A command whose profile entry names no reply is never waited on.
		const bool awaitReply = framing.acknowledged && mnemonic && *mnemonic;
		bool ok = written && !awaitReply;
		if (!written)
		{
			// audit 2026-10-16: an unplugged port fails every command; log the
			// first failure, then at most once per 10 s with a count, and after
			// 5 failures in a row close the port so it is reopened.
			const auto failedAt = steady_clock::now();
			++writeFailures;
			++unloggedWriteFailures;
			if (!writeFailureLogged || failedAt - lastWriteFailureLog >= seconds(10))
			{
				DebugLog::Log("Dynamic Drive: projector write of \"%s\" failed: %s (%d failed write(s) since the last report)",
					Printable(command).c_str(), error.c_str(), unloggedWriteFailures);
				writeFailureLogged = true;
				lastWriteFailureLog = failedAt;
				unloggedWriteFailures = 0;
			}
			if (writeFailures >= 5 && port->IsOpen())
			{
				port->Close();
				reopenBackoffMs = reopenBackoffMs == 0 ? 2000 : std::min(reopenBackoffMs * 2, 30000);
				reopenAt = failedAt + milliseconds(reopenBackoffMs);
				DebugLog::Log("Dynamic Drive: %d projector writes failed in a row; %s closed, reopening in %d ms",
					writeFailures, config.port.c_str(), reopenBackoffMs);
			}
		}
		else
		{
			const auto now = steady_clock::now();
			if (unloggedWriteFailures > 0 && now - lastWriteFailureLog >= seconds(10))
			{
				DebugLog::Log("Dynamic Drive: projector writes succeed again (%d failed write(s) since the last report)",
					unloggedWriteFailures);
				lastWriteFailureLog = now;
				unloggedWriteFailures = 0;
			}
			writeFailures = 0;
			// audit 2026-10-16: the reopen back-off resets below, once the
			// reply wait has not closed the port.
		}
		if (written && awaitReply)
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
					// audit 2026-10-16: a failed read closed the port; every
					// further read fails at once, so waiting out the ack timeout
					// here only spun holding portMutex.
					if (!port->IsOpen()) break;
					std::lock_guard<std::mutex> guard(stateMutex);
					if (abortable && shuttingDown) break;
					continue;
				}
				std::string name;
				int parsed = 0;
				// The profile's reply grammar ([reply]; the FL35's is ParseReply's).
				if (MatchDeviceReply(config.device, line, name, parsed) && SameMnemonic(name, mnemonic))
				{
					if (value) *value = parsed;
					ok = true;
					break;
				}
				if (!line.empty())
					DebugLog::Log("Dynamic Drive: projector reply \"%s\" ignored while waiting for %s",
						Printable(line).c_str(), mnemonic);
			}
			if (!ok)
			{
				{
					std::lock_guard<std::mutex> guard(stateMutex);
					++timeouts;
				}
				DebugLog::Log("Dynamic Drive: no %s reply within %d ms to \"%s\"",
					mnemonic, timeoutMs, Printable(command).c_str());
			}
		}
		if (written)
		{
			if (port->IsOpen())
				reopenBackoffMs = 0;
			else
			{
				// audit 2026-10-16: the port closed on a failed read (device
				// unplugged): treated like repeated write failures - reopened only
				// after the back-off (2 s doubling to 30 s), so the next command
				// does not reopen, resync and close again at once.
				const auto closedAt = steady_clock::now();
				reopenBackoffMs = reopenBackoffMs == 0 ? 2000 : std::min(reopenBackoffMs * 2, 30000);
				reopenAt = closedAt + milliseconds(reopenBackoffMs);
				DebugLog::Log("Dynamic Drive: reading %s failed; port closed, reopening in %d ms",
					config.port.c_str(), reopenBackoffMs);
			}
		}
		int gap = config.minCommandGapMs;
		// Barco: at least 2 s before sending again when no reply came (the
		// profile's no_reply_backoff_ms).
		const int backoffMs = std::max(0, config.device.noReplyBackoffMs);
		const bool noReply = written && awaitReply && !ok;
		if (noReply) gap = std::max(gap, backoffMs);
		// audit 2026-10-16: a failed write is retried; never in a tight loop,
		// even with min_command_gap_ms 0.
		if (!written) gap = std::max(gap, 200);
		std::lock_guard<std::mutex> guard(stateMutex);
		// audit 2026-10-16: kept apart, so the script and sequence sends (no
		// command gap) still keep it.
		if (noReply) replyBackoffUntil = steady_clock::now() + milliseconds(backoffMs);
		if (config.burstCommands > 0 && sinceBurst >= config.burstCommands)
		{
			gap = std::max(gap, config.burstPauseMs);
			sinceBurst = 0;
		}
		nextSendAllowed = steady_clock::now() + milliseconds(gap);
		return ok;
	}

	// A query's answer in VP units (light 0-255, contrast 50-100).
	bool BarcoAsciiDriver::QueryLocked(int command, int which, int& value)
	{
		std::string bytes, reply;
		if (!framing.acknowledged || !RenderQuery(command, which, bytes, reply)) return false;
		int answer = 0;
		if (!Transact(bytes, reply.c_str(), &answer, config.ackTimeoutMs, -1)) return false;
		value = ToVp(command, answer);
		return true;
	}

	int BarcoAsciiDriver::ToVp(int command, int deviceValue) const
	{
		const DeviceCommandId id = static_cast<DeviceCommandId>(command);
		if (id == DeviceCommandId::Light) return config.device.LightFromDeviceRounded(deviceValue);
		if (id == DeviceCommandId::Contrast) return config.device.ContrastFromDevice(deviceValue);
		return deviceValue;
	}

	// One set command by VP id and VP value: rendered from the profile, then
	// Transact (whose reply value comes back in VP units). A value the profile
	// refuses is not sent and is not a failed write (never retried).
	bool BarcoAsciiDriver::TransactSet(const std::string& name, int value, int* confirmed, int timeoutMs,
		int maxGapWaitMs, int sentLight, bool abortable, int sentContrast, const int* generation,
		int expectedGeneration, bool* wrote, int budgetMs)
	{
		if (wrote) *wrote = false;
		std::string bytes, reply;
		if (!RenderSet(name, value, bytes, reply))
		{
			lastWriteOk = true;
			return false;
		}
		int answer = -1;
		const bool ok = budgetMs >= 0 ?
			BoundedTransactLocked(bytes, reply.c_str(), confirmed ? &answer : nullptr, budgetMs, sentLight, sentContrast) :
			Transact(bytes, reply.c_str(), confirmed ? &answer : nullptr, timeoutMs, maxGapWaitMs, sentLight, abortable,
				sentContrast, generation, expectedGeneration, wrote);
		if (confirmed)
		{
			DeviceCommandId id = DeviceCommandId::Light;
			DeviceCommandFromId(name, id);
			*confirmed = answer >= 0 ? ToVp(static_cast<int>(id), answer) : -1;
		}
		return ok;
	}

	bool BarcoAsciiDriver::EnsurePortLocked(std::string& error)
	{
		using namespace std::chrono;
		if (port->IsOpen()) return true;
		const auto now = steady_clock::now();
		if (now < reopenAt)
		{
			// audit 2026-10-16: the exit restore tries one Open regardless of
			// the back-off (once per shutdown), so an outage that ended during
			// the back-off does not leave the projector dim after exit.
			bool force = false;
			{
				std::lock_guard<std::mutex> stateGuard(stateMutex);
				force = shuttingDown && !shutdownOpenTried;
			}
			if (!force)
			{
				error = config.port + " closed; reopening in " +
					std::to_string(duration_cast<milliseconds>(reopenAt - now).count()) + " ms";
				return false;
			}
			shutdownOpenTried = true;
			DebugLog::Log("Dynamic Drive: exit restore - trying to reopen %s ahead of its back-off", config.port.c_str());
		}
		std::string openError;
		if (port->Open(config.port, config.baud, openError))
		{
			// The back-off itself resets only after a write succeeds, so a port
			// that opens but cannot write is not reopened in a tight loop.
			{
				// audit 2026-10-16 follow-up: what the projector holds is unknown
				// now; the worker redoes the handshake and re-sends the targets
				// once this command is done.
				std::lock_guard<std::mutex> stateGuard(stateMutex);
				state.lightKnown = false;
				state.contrastKnown = false;
				resyncAfterReopen = true;
				wake.notify_one();
			}
			++reopensUnlogged;
			if (lastReopenLog == steady_clock::time_point() || now - lastReopenLog >= seconds(10))
			{
				DebugLog::Log("Dynamic Drive: %s reopened (%d reopen(s) since the last report)",
					config.port.c_str(), reopensUnlogged);
				lastReopenLog = now;
				reopensUnlogged = 0;
			}
			writeFailures = 0;
			return true;
		}
		reopenBackoffMs = reopenBackoffMs == 0 ? 2000 : std::min(reopenBackoffMs * 2, 30000);
		reopenAt = now + milliseconds(reopenBackoffMs);
		error = openError + "; retrying in " + std::to_string(reopenBackoffMs) + " ms";
		return false;
	}

	// Candidate framings, tried in order for terminator: auto. The script's
	// bytes (lower case, literal "cr", then echo's CR LF) are the
	// unacknowledged fallback: what the projector accepted for years without
	// anyone reading its replies.
	// Device profiles: the framings, their order and the send-only fallback
	// come from [framing] (the FL35's: cr, literal_cr, then script).
	void BarcoAsciiDriver::SelectFraming()
	{
		const DeviceProfile& device = config.device;
		auto make = [](const DeviceFraming& f, bool acknowledged) { return Framing{ f.name, f.letterCase, f.suffix, acknowledged }; };
		const std::string wanted = config.TerminatorName();
		const DeviceFraming* sendOnly = device.Framing(device.sendOnly);
		const DeviceFraming* fallback = device.Framing(device.fallback.empty() ? device.sendOnly : device.fallback);
		if (!config.readBack || !device.readBackSupported)
		{
			// Send-only: no probing, no handshake, no queries.
			const DeviceFraming* chosen = wanted == "auto" ? sendOnly : device.Framing(wanted);
			if (!chosen) chosen = sendOnly;
			if (chosen) framing = make(*chosen, false);
			DebugLog::Log("Dynamic Drive: read_back off; sending with framing \"%s\" without waiting for replies",
				framing.name.c_str());
			return;
		}
		std::vector<const DeviceFraming*> candidates;
		if (wanted == "auto")
		{
			for (const std::string& name : device.probeOrder)
				if (const DeviceFraming* f = device.Framing(name)) candidates.push_back(f);
		}
		else if (const DeviceFraming* f = device.Framing(wanted))
		{
			// A configured send-only framing is used as it is (the old script's bytes).
			if (!f->probe) { framing = make(*f, false); return; }
			candidates.push_back(f);
		}
		const bool handshake = config.echo && device.Has(DeviceCommandId::Handshake);
		for (const DeviceFraming* candidate : candidates)
		{
			framing = make(*candidate, true);
			if (handshake)
			{
				std::string bytes, reply;
				if (RenderSet("ECHO", 0, bytes, reply))
					Transact(bytes, reply.c_str(), nullptr, config.ackTimeoutMs, -1);
			}
			int light = 0;
			if (QueryLocked(static_cast<int>(DeviceCommandId::Light), static_cast<int>(DeviceQuery::Value), light))
			{
				std::lock_guard<std::mutex> guard(stateMutex);
				state.lightKnown = true;
				state.light = light;
				DebugLog::Log("Dynamic Drive: projector answers with framing \"%s\" (LPW1=%d)",
					framing.name.c_str(), light);
				return;
			}
		}
		if (wanted == "auto")
		{
			if (fallback) framing = make(*fallback, false);
			DebugLog::Log(
				"Dynamic Drive: projector did not answer with the %s framings; sending framing \"%s\" (for the FL35 the old script's exact bytes) without waiting for replies",
				device.DisplayName().c_str(), framing.name.c_str());
		}
		else
			DebugLog::Log("Dynamic Drive: projector did not answer with configured framing \"%s\"",
				framing.name.c_str());
	}

	bool BarcoAsciiDriver::Open(std::string& error)
	{
		Close();
		if (!port || !port->Open(config.port, config.baud, error))
		{
			if (!port) error = "no serial port";
			DebugLog::Log("Dynamic Drive: %s; projector control disabled", error.c_str());
			return false;
		}
		{
			std::lock_guard<std::timed_mutex> guard(portMutex);
			writeFailures = 0;
			reopenBackoffMs = 0;
			reopenAt = std::chrono::steady_clock::time_point();
			shutdownOpenTried = false;   // audit 2026-10-16
			SelectFraming();
			int minimum = 0, maximum = 0;
			if (framing.acknowledged)
			{
				const int light = static_cast<int>(DeviceCommandId::Light);
				const bool haveMin = config.lightMin >= 0 || QueryLocked(light, static_cast<int>(DeviceQuery::Minimum), minimum);
				const bool haveMax = config.lightMax >= 0 || QueryLocked(light, static_cast<int>(DeviceQuery::Maximum), maximum);
				int contrast = 0;
				const bool haveContrast = QueryLocked(static_cast<int>(DeviceCommandId::Contrast),
					static_cast<int>(DeviceQuery::Value), contrast);
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
			pendingContrast = -1;
			scriptLight = -1;      // audit 2026-10-16
			scriptContrast = -1;
			opened = true;
		}
		worker = std::thread(&BarcoAsciiDriver::Worker, this);
		const LightRange reported = QueryLightRange();
		DebugLog::Log("Dynamic Drive: barco_ascii on %s %d baud, framing=%s acknowledged=%d range=%s device=%s",
			config.port.c_str(), config.baud, framing.name.c_str(), framing.acknowledged ? 1 : 0,
			reported.known ? (std::to_string(reported.minimum) + "-" +
				std::to_string(reported.maximum)).c_str() : "unknown", config.device.DisplayName().c_str());
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
			const int value = static_cast<int>(DeviceQuery::Value);
			const bool haveLight = QueryLocked(static_cast<int>(DeviceCommandId::Light), value, light);
			const bool haveContrast = QueryLocked(static_cast<int>(DeviceCommandId::Contrast), value, contrast);
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
		// audit 2026-10-16: not after the exit restore began (the worker
		// skips targets then, and a pending one kept it spinning).
		if (!opened || shuttingDown) return;
		pendingLight = Clamp(level);
		pendingJump = false;
		scriptLight = -1;   // audit 2026-10-16: the driver's own targets now
		wake.notify_one();
	}

	void BarcoAsciiDriver::SetContrastAsync(int value)
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!opened || shuttingDown) return;
		pendingContrast = value;
		// audit 2026-10-16: a hold's -1 also stops a value the worker has
		// already taken but not yet written (a newer value simply follows it).
		if (value < 0)
		{
			++contrastGeneration;
			gapWake.notify_all();   // post-merge audit 2026-10-17: a CNTR in its gap wait too
		}
		else scriptContrast = -1;   // audit 2026-10-16
		wake.notify_one();
	}

	void BarcoAsciiDriver::CancelLight()
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!opened || shuttingDown) return;
		// As a script send does: a value already taken by the worker but not
		// yet written is stopped by the generation.
		pendingLight = -1;
		pendingJump = false;
		++lightGeneration;
		wake.notify_one();
		// Post-merge audit 2026-10-17: also an LPW1 already inside Transact's gap wait.
		gapWake.notify_all();
	}

	void BarcoAsciiDriver::SetLightJump(int level)
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!opened || shuttingDown) return;   // audit 2026-10-16
		pendingLight = Clamp(level);
		pendingJump = true;
		scriptLight = -1;   // audit 2026-10-16
		wake.notify_one();
	}

	bool BarcoAsciiDriver::QueueSequenceNow(const std::vector<std::pair<std::string, int>>& commands, int stepMs)
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		if (!opened || shuttingDown) return false;
		sequence.assign(commands.begin(), commands.end());
		sequenceStepMs = std::max(1, stepMs);
		sequenceNextAllowed = std::chrono::steady_clock::now();
		wake.notify_one();
		return true;
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
		// A command the device profile does not have (e.g. no dim mode) has
		// nothing to set or restore: nothing is sent.
		DeviceCommandId id = DeviceCommandId::Light;
		if (DeviceCommandFromId(mnemonic, id) && !config.device.Has(id))
		{
			DebugLog::Log("Dynamic Drive: %s not sent: the device profile %s has no such command", mnemonic.c_str(),
				config.device.DisplayName().c_str());
			return true;
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
		const bool light = mnemonic == "LPW1";
		const bool contrast = mnemonic == "CNTR";
		const bool ok = TransactSet(mnemonic, value, nullptr, config.ackTimeoutMs, -1,
			timeoutMs > 0 && light ? value : -1, false, timeoutMs > 0 && contrast ? value : -1, nullptr, 0, nullptr,
			timeoutMs > 0 ? left : -1);
		if (ok && contrast)
		{
			std::lock_guard<std::mutex> stateGuard(stateMutex);
			state.contrastKnown = true;
			state.contrast = value;
		}
		return ok;
	}

	// audit 2026-10-16: a bounded (exit) command. The command gap is waited
	// in full whenever what is left of the budget allows it, keeping at least
	// a short reply wait (a quarter of the budget, at most 100 ms); the reply
	// gets the rest, up to ack_timeout_ms. Previously both were capped at
	// half the budget, so a 300-350 ms budget could not keep Barco's 500 ms
	// gap and the restore could be dropped. Caller holds portMutex.
	bool BarcoAsciiDriver::BoundedTransactLocked(const std::string& command, const char* mnemonic, int* value,
		int budgetMs, int sentLight, int sentContrast)
	{
		using namespace std::chrono;
		int gapMs = 0;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			const auto now = steady_clock::now();
			if (nextSendAllowed > now)
				gapMs = static_cast<int>(duration_cast<milliseconds>(nextSendAllowed - now).count()) + 1;
		}
		const int replyFloor = std::min(100, budgetMs / 4);
		const int gapWait = std::min(gapMs, std::max(0, budgetMs - replyFloor));
		const int reply = std::max(1, std::min(config.ackTimeoutMs, budgetMs - gapWait));
		return Transact(command, mnemonic, value, reply, gapWait, sentLight, false, sentContrast);
	}

	int BarcoAsciiDriver::CommandGapMs() const
	{
		return std::max(0, config.minCommandGapMs);
	}

	bool BarcoAsciiDriver::SendScript(const std::string& mnemonic, int value, bool* written)
	{
		const bool light = mnemonic == "LPW1";
		const bool contrast = mnemonic == "CNTR";
		int sent = value;
		if (written) *written = false;
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			if (!opened || shuttingDown) return false;
			if (light) sent = Clamp(value);
		}
		std::lock_guard<std::timed_mutex> lock(portMutex);
		{
			// The script's own sleeps space its commands (fast dumps 100 ms
			// apart): no command gap or burst pause, like the custom-dim
			// sequence. The exit restore may have begun while this waited.
			std::lock_guard<std::mutex> guard(stateMutex);
			if (shuttingDown) return false;
			// audit 2026-10-16: but Barco's 2 s after a missing reply stays.
			nextSendAllowed = std::max(std::chrono::steady_clock::now(), replyBackoffUntil);
			sinceBurst = 0;
			// audit 2026-10-16: this is now the newest value for its command.
			// A resync target (or any value) the driver thread still holds or
			// has taken is dropped, so it cannot land after this one, and the
			// script's intended value is kept even when the write fails: after
			// a reopen the resync re-sends what the script controller believes,
			// not the last value that reached the port (CNTR stuck at 58 while
			// the script believes 50).
			if (light)
			{
				pendingLight = -1;
				++lightGeneration;
				scriptLight = sent;
			}
			else if (contrast)
			{
				pendingContrast = -1;
				++contrastGeneration;
				scriptContrast = sent;
			}
		}
		int confirmed = -1;
		const bool ok = TransactSet(mnemonic, sent, &confirmed,
			config.ackTimeoutMs, -1, light ? sent : -1, true, contrast ? sent : -1, nullptr, 0, written);
		std::lock_guard<std::mutex> guard(stateMutex);
		if (ok && light) lastLight = confirmed >= 0 ? confirmed : sent;
		if (ok && contrast)
		{
			state.contrastKnown = true;
			state.contrast = confirmed >= 0 ? confirmed : sent;
		}
		return ok;
	}

	void BarcoAsciiDriver::BeginShutdown()
	{
		std::lock_guard<std::mutex> guard(stateMutex);
		shuttingDown = true;
		pendingLight = -1;
		pendingContrast = -1;
		++lightGeneration;      // audit 2026-10-16
		++contrastGeneration;
		sequence.clear();
		wake.notify_all();
		gapWake.notify_all();   // audit 2026-10-16: ends an abortable pre-wait
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
			// audit 2026-10-16: nor a target the worker took before this.
			++lightGeneration;
			scriptLight = -1;   // audit 2026-10-16
			clamped = Clamp(level);
			// Post-merge audit 2026-10-17: a worker LPW1 already inside Transact's gap
			// wait ends it unsent (it holds the port this call needs).
			gapWake.notify_all();
		}
		std::unique_lock<std::timed_mutex> lock(portMutex, std::defer_lock);
		if (timeoutMs > 0)
		{
			// Bounded (exit): the whole call stays within timeoutMs - up to half
			// waiting for a command in flight (BeginShutdown ends its reply
			// wait), the rest split between the gap and the reply.
			if (!lock.try_lock_for(milliseconds(timeoutMs / 2)))
			{
				DebugLog::Log("Dynamic Drive: port still busy after %d ms; light not restored", timeoutMs / 2);
				return false;
			}
		}
		else
			lock.lock();
		int confirmed = -1;
		const int left = timeoutMs > 0 ? std::max(0, timeoutMs -
			static_cast<int>(duration_cast<milliseconds>(steady_clock::now() - start).count())) : 0;
		const bool ok = TransactSet("LPW1", clamped, &confirmed, config.ackTimeoutMs, -1, clamped, false, -1,
			nullptr, 0, nullptr, timeoutMs > 0 ? left : -1);
		std::lock_guard<std::mutex> stateGuard(stateMutex);
		if (ok) lastLight = confirmed >= 0 ? confirmed : clamped;
		return ok;
	}

	bool BarcoAsciiDriver::SetContrast(int value)
	{
		if (!opened) return false;
		// A device profile without contrast: nothing to set.
		if (!config.device.Has(DeviceCommandId::Contrast)) return true;
		{
			std::lock_guard<std::mutex> stateGuard(stateMutex);
			scriptContrast = -1;   // audit 2026-10-16
		}
		std::lock_guard<std::timed_mutex> guard(portMutex);
		int confirmed = -1;
		const bool ok = TransactSet("CNTR", value, &confirmed,
			config.ackTimeoutMs, -1, -1, false, value);
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
			bool jump = false;
			int lightTargetGeneration = 0;
			{
				std::unique_lock<std::mutex> lock(stateMutex);
				wake.wait(lock, [this]
					{ return stopping || pendingLight >= 0 || pendingContrast >= 0 || !sequence.empty() || resyncAfterReopen; });
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
					// audit 2026-10-16: Barco's 2 s after a missing reply stays.
					nextSendAllowed = std::max(std::chrono::steady_clock::now(), replyBackoffUntil);
					lock.unlock();
					{
						std::lock_guard<std::timed_mutex> guard(portMutex);
						bool skip = false;
						{
							std::lock_guard<std::mutex> stateGuard(stateMutex);
							skip = shuttingDown;
						}
						if (!skip)
							TransactSet(command.first, command.second, nullptr, config.ackTimeoutMs, -1, -1, true);
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
				if (shuttingDown)
				{
					resyncAfterReopen = false;
					// audit 2026-10-16: nothing more is sent; a target left here
					// would wake this loop again at once (a spin).
					pendingLight = -1;
					pendingContrast = -1;
					sequence.clear();
					continue;
				}
				if (resyncAfterReopen)
				{
					// audit 2026-10-16 follow-up: after a reopen, Open()'s ECHO
					// (when the framing is acknowledged and echo is on), then the
					// light and CNTR the model holds - CNTR 50, the startup value,
					// when none was sent - unless newer targets are waiting.
					resyncAfterReopen = false;
					transmitting = true;
					lock.unlock();
					{
						std::lock_guard<std::timed_mutex> guard(portMutex);
						bool skip = false;
						{
							std::lock_guard<std::mutex> stateGuard(stateMutex);
							skip = shuttingDown;
						}
						if (!skip && framing.acknowledged && config.echo && config.device.Has(DeviceCommandId::Handshake))
							TransactSet("ECHO", 0, nullptr, config.ackTimeoutMs, -1, -1, true);
					}
					lock.lock();
					transmitting = false;
					if (!stopping && !shuttingDown)
					{
						// audit 2026-10-16: after script sends, the values the
						// script intended (kept even when their writes failed), not
						// the last ones that reached the port; a newer SendScript
						// drops these again (pending cleared, generation bumped).
						const int lightTarget = pendingLight >= 0 ? pendingLight :
							scriptLight >= 0 ? scriptLight : lastSentLight;
						const int contrastTarget = pendingContrast >= 0 ? pendingContrast :
							scriptContrast >= 0 ? scriptContrast : lastSentContrast >= 0 ? lastSentContrast : 50;
						DebugLog::Log("Dynamic Drive: %s reopened; re-sending LPW1 %d and CNTR %d",
							config.port.c_str(), lightTarget, contrastTarget);
						if (config.device.Has(DeviceCommandId::Contrast))
							pendingContrast = contrastTarget;
						if (pendingLight < 0 && lightTarget >= 0)
						{
							pendingLight = lightTarget;
							pendingJump = true;   // the level itself, not split
						}
					}
					continue;
				}
				if (pendingContrast >= 0)
				{
					// 7c-1: a CNTR crossing takes this slot; a waiting LPW1
					// keeps its place for the next one.
					const int contrast = pendingContrast;
					const int generation = contrastGeneration;
					pendingContrast = -1;
					transmitting = true;
					contrastInFlight = true;
					lock.unlock();
					{
						std::lock_guard<std::timed_mutex> guard(portMutex);
						bool skip = false;
						{
							// audit 2026-10-16: cancelled (a hold's -1, or shutdown)
							// while this waited for the port: not sent.
							std::lock_guard<std::mutex> stateGuard(stateMutex);
							skip = shuttingDown || contrastGeneration != generation;
						}
						int confirmed = -1;
						if (!skip &&
							TransactSet("CNTR", contrast, &confirmed,
								config.ackTimeoutMs, -1, -1, true, contrast, &contrastGeneration, generation))
						{
							std::lock_guard<std::mutex> stateGuard(stateMutex);
							state.contrastKnown = true;
							state.contrast = confirmed >= 0 ? confirmed : contrast;
						}
						else if (!skip && !lastWriteOk)
						{
							// audit 2026-10-16: a failed write is sent again at the
							// next slot (the command gap), unless replaced or
							// cancelled meanwhile.
							std::lock_guard<std::mutex> stateGuard(stateMutex);
							if (!stopping && !shuttingDown && pendingContrast < 0 && contrastGeneration == generation)
								pendingContrast = contrast;
						}
					}
					lock.lock();
					transmitting = false;
					contrastInFlight = false;
					continue;
				}
				if (pendingLight < 0) continue;
				target = pendingLight;
				pendingLight = -1;
				jump = pendingJump;
				lightTargetGeneration = lightGeneration;
				pendingJump = false;
				// Dim at most max_levels_per_command per command (a cut jump is
				// sent whole); the rest of the drop stays pending for the next
				// command unless a newer target replaces it.
				if (!jump && config.maxLevelsPerCommand > 0 && lastSentLight >= 0 &&
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
				// audit 2026-10-16: nor one SetLightNow replaced meanwhile.
				std::lock_guard<std::mutex> stateGuard(stateMutex);
				if (shuttingDown || lightGeneration != lightTargetGeneration)
				{
					transmitting = false;
					continue;
				}
			}
			int confirmed = -1;
			const bool ok = TransactSet("LPW1", target, &confirmed,
				config.ackTimeoutMs, -1, target, true, -1, &lightGeneration, lightTargetGeneration);
			std::lock_guard<std::mutex> stateGuard(stateMutex);
			transmitting = false;
			if (ok) lastLight = confirmed >= 0 ? confirmed : target;
			// audit 2026-10-16: a failed write is sent again at the next slot
			// (the command gap) unless a newer target is waiting; the model only
			// follows levels really written, so it stays on the old one meanwhile.
			else if (!lastWriteOk && !stopping && !shuttingDown && pendingLight < 0 &&
				lightGeneration == lightTargetGeneration)
			{
				pendingLight = target;
				pendingJump = jump;
				wake.notify_one();
			}
		}
	}

	void BarcoAsciiDriver::Close()
	{
		{
			std::lock_guard<std::mutex> guard(stateMutex);
			stopping = true;
			opened = false;
			pendingLight = -1;
			pendingContrast = -1;
		}
		wake.notify_all();
		gapWake.notify_all();   // audit 2026-10-16
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
		status.contrastSends = contrastSends;
		status.lastSentContrast = lastSentContrast;
		status.lastLightSentAt = lastLightSentAt;
		status.lastContrastSentAt = lastContrastSentAt;
		status.nextSendAt = nextSendAllowed;   // bug hunt 2
		status.busy = pendingLight >= 0 || pendingContrast >= 0 || transmitting;
		status.contrastBusy = pendingContrast >= 0 || contrastInFlight;   // audit 2026-10-16
		status.sequenceBusy = !sequence.empty() || sequenceInFlight;
		status.sequencesDone = sequencesDone;
		return status;
	}
}
