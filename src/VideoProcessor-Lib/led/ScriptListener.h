#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

// LED mode "listen" (owner 2026-10-13): the original beta 22 script drives the
// projector on COM1 and, with its VP addendum (docs/reference/main_vp.py),
// tells VideoProcessor each command it sent and what it measured, one UDP
// datagram per message to 127.0.0.1:listen_port:
//   VP1 <KIND> <value> t=<seconds> mean=<mean2> max=<max_val> hl=<0|1> tp=<target power> tc=<target contrast>
// KIND is LPW1, CNTR (a command sent, its value) or TICK (one script loop's
// measurement and targets; value -1). VP never opens the serial port in this
// mode; the listener binds the loopback address only.
namespace LedDrive
{
	struct ScriptMessage
	{
		std::string kind;
		int value = -1;
		double scriptSeconds = 0.0;
		int mean = -1;
		int maximum = -1;
		int highlight = -1;
		int targetPower = -1;
		int targetContrast = -1;
		// The sender's UDP port on 127.0.0.1 (0 = unknown): relay sends only
		// what the script VP runs sent (audit 2026-10-14b).
		unsigned short senderPort = 0;
	};

	// The process that owns 127.0.0.1:port (UDP), or 0.
	unsigned long UdpPortOwner(unsigned short port);

	// Parses one datagram; false for anything that is not a VP1 message.
	bool ParseScriptMessage(const std::string& text, ScriptMessage& out);

	class ScriptListener
	{
	public:
		ScriptListener() = default;
		~ScriptListener();
		ScriptListener(const ScriptListener&) = delete;
		ScriptListener& operator=(const ScriptListener&) = delete;

		// Binds 127.0.0.1:port (UDP). False with a reason when it cannot.
		bool Open(int port, std::string& error);
		void Close();
		bool IsOpen() const { return socketHandle != invalidSocket; }
		int Port() const { return boundPort; }
		// Every message that arrives within waitMs (returns as soon as at least
		// one has arrived and the socket is drained, or at the timeout). The
		// wait is in 50 ms slices; `interrupted` (when given) ends it early
		// (audit 2026-10-14: a stop or a new setting must not wait a whole tick).
		std::vector<ScriptMessage> Receive(int waitMs, const std::function<bool()>& interrupted = {});

	private:
		static constexpr uintptr_t invalidSocket = ~static_cast<uintptr_t>(0);
		uintptr_t socketHandle = invalidSocket;
		bool winsockStarted = false;
		int boundPort = 0;
	};

	// mode: listen's data log (owner 2026-10-13): logs\led-listen.csv beside
	// vp.log, the script's measurement and targets next to VP's own, for
	// adapting VP's copy of the script. Sized for sessions of hours (owner
	// 2026-10-14): a row for every LPW1/CNTR (relay: sent 1/0 and how long
	// the write took), a TICK row only when VP's copy starts or stops
	// agreeing (or the disagreement changes), and a "# summary" line every
	// 10 s. A new session appends a "# session" line; past
	// kListenCsvCapBytes the file becomes led-listen.prev.csv, so at most
	// twice the cap is kept. A file with an older header is moved aside.
	constexpr const char* kListenCsvHeader =
		"time_ms,kind,value,script_mean,script_max,script_highlight,script_target_light,script_target_contrast,"
		"vp_apl,vp_max,vp_highlight_percentile,vp_highlight_area,vp_highlight,vp_target_light,vp_target_contrast,"
		"last_contrast_heard,vp_gain,relay_sent,relay_write_ms";
	constexpr long long kListenCsvCapBytes = 8ll * 1024 * 1024;
	// Opens `path` for appending (rotated first when past the cap or written
	// with another header), writes the header when new and then `first`;
	// returns the file's size.
	long long OpenListenCsv(std::ofstream& csv, const std::string& path, const std::string& first);
	// In `directory`, or beside the VP log when it is empty.
	std::string ListenCsvPath(const std::string& directory = std::string());
	// The monitor's file: one line of key=value pairs, overwritten on each
	// update (the script's latest commands and measurement, VP's, and the
	// session's agreement).
	std::string ListenNowPath(const std::string& directory = std::string());

	// Tests: sends one datagram to 127.0.0.1:port.
	bool SendScriptMessageForTest(int port, const std::string& text);
}
