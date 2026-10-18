#pragma once

#include "../ConfigFile.h"
#include "../ConfigSchema.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

// Dynamic Drive device profiles (feat/dd-projector-profiles, 2026-10-17).
//
// Every projector-specific fact the Dynamic Drive needs - the RS-232 command
// set and its framing, the reply grammar, the drive and contrast ranges, the
// LED response timing, the step and pacing limits, the floors, the light
// curve and the restore-on-exit values - lives in a device profile, a small
// file in VP's own cfg format (*.vpprojector; profiles\dynamic-drive\ ships
// barco-fl35.vpprojector). The Barco F35/FL35 profile is also compiled in:
// DeviceProfile's defaults ARE that profile, so a configuration that names no
// device_profile behaves exactly as before. DeviceProfileTests asserts that
// the shipped file loads to exactly these defaults and drives byte-identical
// command streams.
//
// VP's internal units do not change: light levels 0-255 and contrast 50-100
// (50 = neutral). A profile maps them linearly onto the device's own command
// values ([light] min..max, [contrast] min..max; identity for the FL35).
// Every value a profile states is in device units; VP converts.
//
// Safety: commands are only ever rendered from the profile's templates, a
// value outside the declared device range is refused (never sent), the set of
// commands is closed (light, contrast, handshake, dim mode, custom dim - no
// power, lamp, input or firmware slot), and a profile that fails validation
// is never used: VP then runs no Dynamic Drive and says why.
//
// Authoring guide: docs\dynamic-drive\PROFILE-AUTHORING-FOR-AI-AGENTS.md.
// Header-only (like LedDriveConfig.h) so the Config app and its tests use the
// same validator without extra project files.
namespace LedDrive
{
	constexpr int DEVICE_PROFILE_SCHEMA = 1;
	constexpr const char* DEVICE_PROFILE_EXTENSION = ".vpprojector";
	// Relative to the VP folder (next to VideoProcessor.exe and the cfg).
	constexpr const char* DEVICE_PROFILE_FOLDER = "profiles\\dynamic-drive";

	// The closed set of commands a profile can define.
	enum class DeviceCommandId { Light, Contrast, DimMode, CustomRed, CustomGreen, CustomBlue, Handshake, Count };
	constexpr int DEVICE_COMMAND_COUNT = static_cast<int>(DeviceCommandId::Count);

	inline const char* DeviceCommandSection(DeviceCommandId id)
	{
		switch (id)
		{
		case DeviceCommandId::Light: return "command.light";
		case DeviceCommandId::Contrast: return "command.contrast";
		case DeviceCommandId::DimMode: return "command.dim_mode";
		case DeviceCommandId::CustomRed: return "command.custom_red";
		case DeviceCommandId::CustomGreen: return "command.custom_green";
		case DeviceCommandId::CustomBlue: return "command.custom_blue";
		case DeviceCommandId::Handshake: return "command.handshake";
		default: return "";
		}
	}

	// The driver interface names commands with VP's historical (Barco) ids:
	// LPW1 = light, CNTR = contrast, LDMM = dim mode, LDCR/LDCG/LDCB = custom
	// dim colours, ECHO = handshake. They are VP's logical names only; the
	// bytes always come from the profile's templates.
	inline bool DeviceCommandFromId(const std::string& name, DeviceCommandId& id)
	{
		static const struct { const char* name; DeviceCommandId id; } table[] = {
			{ "LPW1", DeviceCommandId::Light }, { "CNTR", DeviceCommandId::Contrast },
			{ "LDMM", DeviceCommandId::DimMode }, { "LDCR", DeviceCommandId::CustomRed },
			{ "LDCG", DeviceCommandId::CustomGreen }, { "LDCB", DeviceCommandId::CustomBlue },
			{ "ECHO", DeviceCommandId::Handshake } };
		for (const auto& entry : table)
			if (name == entry.name) { id = entry.id; return true; }
		return false;
	}

	struct DeviceFraming
	{
		std::string name;
		// Decoded bytes appended to every command.
		std::string suffix;
		// "as_written", "lower" or "upper": applied to the template's literal
		// text (never to the rendered value).
		std::string letterCase = "as_written";
		// The projector can acknowledge commands sent this way (read-back may
		// probe it); a send-only framing is never waited on.
		bool probe = true;
	};

	struct DeviceCommand
	{
		bool present = false;
		// Templates (raw, escapes and {v} placeholders resolved by Render).
		std::string set;
		std::string query;
		std::string queryMin;
		std::string queryMax;
		// The name the projector's replies carry for this command (matched
		// case-insensitively against [reply] name_group); "" = no reply wait.
		std::string reply;
		// Device range for dim mode / custom dim values.
		int min = 0;
		int max = 0;
		// dim mode only: the standard and the custom value.
		int standard = 0;
		int custom = 1;
	};

	struct DeviceProfile
	{
		// [profile]
		int schema = DEVICE_PROFILE_SCHEMA;
		std::string maker = "Barco";
		std::string model = "F35 / FL35";
		std::string version = "1";
		std::string source = "Barco 601-0381-00 RS232 ASCII protocol; VP's FL35 driver as of 2026-10-17";
		bool reviewed = true;
		// Where it was loaded from; "" = the compiled-in FL35 profile.
		std::string path;

		// [transport]
		int baud = 19200;
		int dataBits = 8;
		std::string parity = "none";
		int stopBits = 1;
		std::string flowControl = "none";
		bool readBackSupported = true;
		bool readBack = false;

		// [framing]
		std::string prefix = ":";
		std::vector<DeviceFraming> framings;
		std::vector<std::string> probeOrder;
		std::string fallback;
		std::string sendOnly;

		// [reply]
		std::string replyPattern = "^\\s*%\\d{3}\\s+([A-Za-z0-9]{4})\\s+(\\d{1,9})\\s*$";
		int replyNameGroup = 1;
		int replyValueGroup = 2;

		DeviceCommand commands[DEVICE_COMMAND_COUNT];

		// [light] (device units)
		int lightMin = 0;
		int lightMax = 255;
		int floor = 100;
		int floorReactive = 100;
		int floorBlack = 60;
		// drive : relative light, ascending; empty = light proportional to drive.
		std::vector<std::pair<double, double>> lightCurve;

		// [contrast] (device units; min is VP's neutral contrast)
		int contrastMin = 50;
		int contrastMax = 100;
		int contrastMaxUsable = 74;
		int dmdClip = 59;   // -1 = contrastMaxUsable

		// [timing]
		int latencyMs = 80;
		int settleMs = 300;
		int contrastLatencyMs = 80;
		int contrastSettleMs = 0;
		int ackTimeoutMs = 300;
		int noReplyBackoffMs = 2000;

		// [limits]
		int maxStep = 12;
		int minCommandGapMs = 200;
		int burstCommands = 0;
		int burstPauseMs = 5000;

		// [restore] (device units; -1 light = the user's led_ceiling)
		int restoreLight = -1;
		int restoreContrast = 50;

		DeviceProfile()
		{
			// The FL35's three framings (BarcoAsciiDriver before profiles):
			// ":MNEMvalue\r", ":MNEMvaluecr\r" and the old script's exact bytes
			// (lower case, a literal "cr", then echo's CR LF), never acknowledged.
			framings = {
				{ "cr", "\r", "as_written", true },
				{ "literal_cr", "cr\r", "as_written", true },
				{ "script", "cr\r\n", "lower", false } };
			probeOrder = { "cr", "literal_cr" };
			fallback = "script";
			sendOnly = "script";
			DeviceCommand& light = commands[static_cast<int>(DeviceCommandId::Light)];
			light.present = true;
			light.set = "LPW1{v}";
			light.query = "LPW1?";
			light.queryMin = "LPW1 ?N";
			light.queryMax = "LPW1 ?M";
			light.reply = "LPW1";
			DeviceCommand& contrast = commands[static_cast<int>(DeviceCommandId::Contrast)];
			contrast.present = true;
			contrast.set = "CNTR{v}";
			contrast.query = "CNTR?";
			contrast.reply = "CNTR";
			DeviceCommand& handshake = commands[static_cast<int>(DeviceCommandId::Handshake)];
			handshake.present = true;
			handshake.set = "ECHO1";
			handshake.reply = "ECHO";
			DeviceCommand& dim = commands[static_cast<int>(DeviceCommandId::DimMode)];
			dim.present = true;
			dim.set = "LDMM{v}";
			dim.reply = "LDMM";
			dim.min = 0;
			dim.max = 1;
			dim.standard = 0;
			dim.custom = 1;
			const char* channels[3][2] = { { "LDCR{v}", "LDCR" }, { "LDCG{v}", "LDCG" }, { "LDCB{v}", "LDCB" } };
			for (int i = 0; i < 3; ++i)
			{
				DeviceCommand& channel = commands[static_cast<int>(DeviceCommandId::CustomRed) + i];
				channel.present = true;
				channel.set = channels[i][0];
				channel.reply = channels[i][1];
				channel.min = 0;
				channel.max = 40;
			}
		}

		const DeviceCommand& Command(DeviceCommandId id) const { return commands[static_cast<int>(id)]; }
		bool Has(DeviceCommandId id) const { return Command(id).present; }
		bool BuiltIn() const { return path.empty(); }

		std::string DisplayName() const
		{
			return maker + " " + model + " (profile " + version + (BuiltIn() ? ", built-in)" : ")");
		}

		const DeviceFraming* Framing(const std::string& name) const
		{
			for (const DeviceFraming& framing : framings)
				if (framing.name == name) return &framing;
			return nullptr;
		}

		// VP light 0-255 <-> device units (identity for 0..255).
		int LightToDevice(int level) const
		{
			if (lightMin == 0 && lightMax == 255) return level;
			return static_cast<int>(std::lround(lightMin + level * (lightMax - lightMin) / 255.0));
		}
		double LightFromDevice(double value) const
		{
			if (lightMin == 0 && lightMax == 255) return value;
			return (value - lightMin) * 255.0 / std::max(1, lightMax - lightMin);
		}
		int LightFromDeviceRounded(int value) const
		{
			return static_cast<int>(std::lround(LightFromDevice(value)));
		}
		// VP contrast 50-100 <-> device units (identity for 50..100).
		int ContrastToDevice(int contrast) const
		{
			if (contrastMin == 50 && contrastMax == 100) return contrast;
			return static_cast<int>(std::lround(contrastMin + (contrast - 50) * (contrastMax - contrastMin) / 50.0));
		}
		int ContrastFromDevice(int value) const
		{
			if (contrastMin == 50 && contrastMax == 100) return value;
			return 50 + static_cast<int>(std::lround((value - contrastMin) * 50.0 / std::max(1, contrastMax - contrastMin)));
		}
		// A device step limit in VP levels (0 = no limit).
		int StepToVp(int step) const
		{
			if (step <= 0) return 0;
			if (lightMin == 0 && lightMax == 255) return step;
			return std::max(1, static_cast<int>(std::lround(step * 255.0 / std::max(1, lightMax - lightMin))));
		}

		const std::regex& ReplyRegex() const
		{
			if (!replyRegex || replyRegexSource != replyPattern)
			{
				replyRegex = std::make_shared<const std::regex>(replyPattern, std::regex::ECMAScript);
				replyRegexSource = replyPattern;
			}
			return *replyRegex;
		}

	private:
		mutable std::shared_ptr<const std::regex> replyRegex;
		mutable std::string replyRegexSource;
	};

	namespace DeviceProfileDetail
	{
		inline bool ParseInt(const std::string& text, int minimum, int maximum, int& value)
		{
			try
			{
				const std::string trimmed = ConfigFile::Trim(text);
				size_t used = 0;
				const long long parsed = std::stoll(trimmed, &used);
				if (used != trimmed.size() || parsed < minimum || parsed > maximum) return false;
				value = static_cast<int>(parsed);
				return true;
			}
			catch (const std::exception&) { return false; }
		}

		inline bool ParseBool(const std::string& text, bool& value)
		{
			const std::string n = ConfigFile::NormalizeName(text);
			if (n == "yes" || n == "true" || n == "on" || n == "1") { value = true; return true; }
			if (n == "no" || n == "false" || n == "off" || n == "0") { value = false; return true; }
			return false;
		}

		inline std::string Unquote(const std::string& text)
		{
			const std::string trimmed = ConfigFile::Trim(text);
			if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
				return trimmed.substr(1, trimmed.size() - 2);
			return trimmed;
		}

		inline int HexDigit(char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		// \r \n \t \\ \" \xHH; anything else after a backslash is an error.
		inline bool DecodeEscapes(const std::string& text, std::string& out, std::string& error)
		{
			out.clear();
			for (size_t i = 0; i < text.size(); ++i)
			{
				const char c = text[i];
				if (c != '\\') { out += c; continue; }
				if (i + 1 >= text.size()) { error = "ends with a lone backslash"; return false; }
				const char e = text[++i];
				if (e == 'r') out += '\r';
				else if (e == 'n') out += '\n';
				else if (e == 't') out += '\t';
				else if (e == '\\') out += '\\';
				else if (e == '"') out += '"';
				else if (e == 'x')
				{
					const int high = i + 1 < text.size() ? HexDigit(text[i + 1]) : -1;
					const int low = i + 2 < text.size() ? HexDigit(text[i + 2]) : -1;
					if (high < 0 || low < 0) { error = "\\x needs two hex digits"; return false; }
					out += static_cast<char>(high * 16 + low);
					i += 2;
				}
				else { error = std::string("unknown escape \\") + e + " (use \\r \\n \\t \\\\ \\\" or \\xHH)"; return false; }
			}
			return true;
		}

		inline std::string Printable(const std::string& bytes)
		{
			std::string text;
			for (unsigned char c : bytes)
			{
				if (c == '\r') text += "\\r";
				else if (c == '\n') text += "\\n";
				else if (c < 0x20 || c >= 0x7F)
				{
					char hex[8];
					sprintf_s(hex, "\\x%02X", c);
					text += hex;
				}
				else text += static_cast<char>(c);
			}
			return text;
		}

		inline std::string ApplyCase(const std::string& text, const std::string& letterCase)
		{
			std::string out = text;
			if (letterCase == "lower")
				std::transform(out.begin(), out.end(), out.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			else if (letterCase == "upper")
				std::transform(out.begin(), out.end(), out.begin(),
					[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			return out;
		}

		// {v}, {v:d}, {v:Nd}, {v:0Nd}, {v:x}, {v:X}, {v:0Nx}, {v:0NX} (N 1-9).
		inline bool FormatValue(const std::string& spec, int value, std::string& out, std::string& error)
		{
			if (spec.empty()) { out = std::to_string(value); return true; }
			std::string body = spec;
			const char kind = body.back();
			if (kind != 'd' && kind != 'x' && kind != 'X') { error = "format {v:" + spec + "} must end in d, x or X"; return false; }
			body.pop_back();
			const bool zero = !body.empty() && body[0] == '0';
			if (zero) body.erase(0, 1);
			int width = 0;
			if (!body.empty() && !ParseInt(body, 1, 9, width)) { error = "format {v:" + spec + "} has a bad width (1-9)"; return false; }
			if (kind != 'd' && value < 0) { error = "a hex value cannot be negative"; return false; }
			char format[16];
			sprintf_s(format, "%%%s%s%c", zero ? "0" : "", width > 0 ? std::to_string(width).c_str() : "", kind);
			char text[64];
			sprintf_s(text, format, value);
			out = text;
			return true;
		}

		// Renders one template: literal text (escapes decoded, letter case
		// applied), {v} replaced by the value. hasValue false refuses {v}.
		inline bool RenderTemplate(const std::string& raw, bool hasValue, int value, const std::string& letterCase,
			std::string& out, std::string& error)
		{
			out.clear();
			std::string literal;
			bool sawValue = false;
			for (size_t i = 0; i < raw.size(); ++i)
			{
				const char c = raw[i];
				if (c == '}') { error = "stray '}' (write \\x7D for a literal brace)"; return false; }
				if (c != '{') { literal += c; continue; }
				const size_t close = raw.find('}', i);
				if (close == std::string::npos) { error = "unclosed '{'"; return false; }
				const std::string field = raw.substr(i + 1, close - i - 1);
				std::string spec;
				if (field == "v") {}
				else if (field.size() > 2 && field.compare(0, 2, "v:") == 0) spec = field.substr(2);
				else { error = "unknown placeholder {" + field + "} (only {v} or {v:format})"; return false; }
				if (!hasValue) { error = "takes no value, but has {" + field + "}"; return false; }
				std::string decoded;
				if (!DecodeEscapes(literal, decoded, error)) return false;
				out += ApplyCase(decoded, letterCase);
				literal.clear();
				std::string formatted;
				if (!FormatValue(spec, value, formatted, error)) return false;
				out += formatted;
				sawValue = true;
				i = close;
			}
			std::string decoded;
			if (!DecodeEscapes(literal, decoded, error)) return false;
			out += ApplyCase(decoded, letterCase);
			if (hasValue && !sawValue) { error = "needs a {v} placeholder for the value"; return false; }
			if (out.empty()) { error = "renders to nothing"; return false; }
			return true;
		}

		inline std::vector<std::string> SplitList(const std::string& text)
		{
			std::vector<std::string> items;
			std::string item;
			for (char c : text + ",")
			{
				if (c == ',')
				{
					const std::string trimmed = ConfigFile::NormalizeName(item);
					if (!trimmed.empty()) items.push_back(trimmed);
					item.clear();
				}
				else item += c;
			}
			return items;
		}

		inline bool ValidName(const std::string& name)
		{
			return !name.empty() && name.size() <= 32 && std::all_of(name.begin(), name.end(),
				[](unsigned char c) { return std::isalnum(c) || c == '_'; });
		}

		// "drive:relative, ..." with drive and relative light both rising,
		// relative light above 0 and at most 10.
		inline bool ParseCurve(const std::string& text, std::vector<std::pair<double, double>>& curve)
		{
			curve.clear();
			if (ConfigFile::Trim(text).empty()) return true;
			for (const std::string& raw : SplitList(text))
			{
				const size_t colon = raw.find(':');
				if (colon == std::string::npos) return false;
				try
				{
					size_t used = 0;
					const std::string a = ConfigFile::Trim(raw.substr(0, colon));
					const std::string b = ConfigFile::Trim(raw.substr(colon + 1));
					const double drive = std::stod(a, &used);
					if (used != a.size()) return false;
					const double light = std::stod(b, &used);
					if (used != b.size()) return false;
					if (!std::isfinite(drive) || !std::isfinite(light) || light <= 0.0 || light > 10.0) return false;
					if (!curve.empty() && (drive <= curve.back().first || light <= curve.back().second)) return false;
					curve.push_back({ drive, light });
				}
				catch (const std::exception&) { return false; }
			}
			return true;
		}

		inline bool Contains(const std::vector<std::string>& list, const std::string& value)
		{
			return std::find(list.begin(), list.end(), value) != list.end();
		}
	}

	// Renders a command's set template for a device value: prefix, template,
	// framing suffix. False (nothing to send) outside the declared device range
	// or when the profile has no such command.
	inline bool RenderDeviceSet(const DeviceProfile& profile, const DeviceFraming& framing, DeviceCommandId id,
		int deviceValue, std::string& bytes, std::string& error)
	{
		const DeviceCommand& command = profile.Command(id);
		if (!command.present) { error = std::string("the device profile has no [") + DeviceCommandSection(id) + "]"; return false; }
		const bool hasValue = id != DeviceCommandId::Handshake;
		if (hasValue)
		{
			int low = command.min, high = command.max;
			if (id == DeviceCommandId::Light) { low = profile.lightMin; high = profile.lightMax; }
			else if (id == DeviceCommandId::Contrast) { low = profile.contrastMin; high = profile.contrastMax; }
			if (low > high) std::swap(low, high);
			if (deviceValue < low || deviceValue > high)
			{
				error = std::string("value ") + std::to_string(deviceValue) + " is outside [" + DeviceCommandSection(id) +
					"]'s device range " + std::to_string(low) + "-" + std::to_string(high) + "; not sent";
				return false;
			}
		}
		std::string body;
		if (!DeviceProfileDetail::RenderTemplate(command.set, hasValue, deviceValue, framing.letterCase, body, error))
			return false;
		std::string prefix;
		if (!DeviceProfileDetail::DecodeEscapes(profile.prefix, prefix, error)) return false;
		bytes = prefix + body + framing.suffix;
		return true;
	}

	enum class DeviceQuery { Value, Minimum, Maximum };

	inline bool RenderDeviceQuery(const DeviceProfile& profile, const DeviceFraming& framing, DeviceCommandId id,
		DeviceQuery which, std::string& bytes)
	{
		const DeviceCommand& command = profile.Command(id);
		const std::string& raw = which == DeviceQuery::Minimum ? command.queryMin :
			which == DeviceQuery::Maximum ? command.queryMax : command.query;
		if (!command.present || raw.empty()) return false;
		std::string body, error, prefix;
		if (!DeviceProfileDetail::RenderTemplate(raw, false, 0, framing.letterCase, body, error)) return false;
		if (!DeviceProfileDetail::DecodeEscapes(profile.prefix, prefix, error)) return false;
		bytes = prefix + body + framing.suffix;
		return true;
	}

	// One reply line -> (name, device value). False for anything else.
	inline bool MatchDeviceReply(const DeviceProfile& profile, const std::string& line, std::string& name, int& value)
	{
		std::smatch match;
		try
		{
			if (!std::regex_match(line, match, profile.ReplyRegex())) return false;
		}
		catch (const std::regex_error&) { return false; }
		if (static_cast<int>(match.size()) <= std::max(profile.replyNameGroup, profile.replyValueGroup)) return false;
		const std::string number = match[profile.replyValueGroup].str();
		int parsed = 0;
		if (number.empty() || number.size() > 10 ||
			!DeviceProfileDetail::ParseInt(number, -1000000000, 1000000000, parsed))
			return false;
		name = match[profile.replyNameGroup].str();
		value = parsed;
		return true;
	}

	inline bool SameReplyName(const std::string& a, const std::string& b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
			[](char x, char y)
			{
				return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
			});
	}

	// The schema, section by section (closed: unknown sections and keys are
	// errors). Ranges here are VP's sanity bounds; a profile must also stay
	// inside the projector manual's ranges, which only its author can check.
	inline std::vector<std::pair<std::string, std::vector<ConfigSchema::KeyRule>>> DeviceProfileSchema()
	{
		using namespace ConfigSchema;
		auto text = [](const char* key, size_t maximum)
		{
			return KeyRule{ key, [maximum](const std::string& v)
				{ const std::string t = DeviceProfileDetail::Unquote(v); return !t.empty() && t.size() <= maximum; },
				"text of 1 to " + std::to_string(maximum) + " characters" };
		};
		auto optionalText = [](const char* key, size_t maximum)
		{
			return KeyRule{ key, [maximum](const std::string& v) { return DeviceProfileDetail::Unquote(v).size() <= maximum; },
				"text of at most " + std::to_string(maximum) + " characters" };
		};
		auto templ = [](const char* key, bool value)
		{
			return KeyRule{ key, [value](const std::string& v)
				{
					std::string out, error;
					return DeviceProfileDetail::RenderTemplate(DeviceProfileDetail::Unquote(v), value, 0, "as_written", out, error);
				},
				value ? "a command template with one {v} (e.g. LPW1{v}; escapes \\r \\n \\t \\\\ \\\" \\xHH)" :
					"a command template without {v} (e.g. LPW1?)" };
		};
		auto escaped = [](const char* key, bool allowEmpty)
		{
			return KeyRule{ key, [allowEmpty](const std::string& v)
				{
					std::string out, error;
					return DeviceProfileDetail::DecodeEscapes(DeviceProfileDetail::Unquote(v), out, error) &&
						(allowEmpty || !out.empty());
				},
				"bytes with escapes \\r \\n \\t \\\\ \\\" \\xHH" };
		};
		auto replyName = [](const char* key)
		{
			return KeyRule{ key, [](const std::string& v)
				{
					const std::string t = DeviceProfileDetail::Unquote(v);
					return t.size() <= 16 && std::all_of(t.begin(), t.end(),
						[](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
				},
				"the reply's command name (letters, digits, _ or -; may be empty)" };
		};
		auto list = [](const char* key)
		{
			return KeyRule{ key, [](const std::string& v)
				{
					for (const std::string& item : DeviceProfileDetail::SplitList(v))
						if (!DeviceProfileDetail::ValidName(item)) return false;
					return true;
				},
				"a comma-separated list of framing names" };
		};
		auto name = [](const char* key)
		{
			return KeyRule{ key, [](const std::string& v) { return DeviceProfileDetail::ValidName(ConfigFile::NormalizeName(v)); },
				"a framing name (letters, digits, _)" };
		};
		std::vector<std::pair<std::string, std::vector<KeyRule>>> schema;
		schema.push_back({ "profile", {
			Integer("schema", 1, 1), text("maker", 64), text("model", 64), text("version", 32),
			optionalText("source", 400), optionalText("notes", 400), Boolean("reviewed") } });
		schema.push_back({ "transport", {
			Choice("type", { "serial" }), Integer("baud", 1200, 921600), Choice("data_bits", { "7", "8" }),
			Choice("parity", { "none", "even", "odd" }), Choice("stop_bits", { "1", "2" }),
			Choice("flow_control", { "none", "rts_cts", "xon_xoff" }), Boolean("read_back"),
			Boolean("read_back_supported"), Choice("reply_line_end", { "cr" }) } });
		schema.push_back({ "framing", { escaped("prefix", true), list("probe_order"), name("fallback"), name("send_only") } });
		schema.push_back({ "framing.*", { escaped("suffix", false),
			Choice("case", { "as_written", "lower", "upper" }), Boolean("probe") } });
		schema.push_back({ "reply", {
			KeyRule{ "pattern", [](const std::string& v)
				{
					try { std::regex re(DeviceProfileDetail::Unquote(v), std::regex::ECMAScript); return re.mark_count() >= 2; }
					catch (const std::regex_error&) { return false; }
				}, "an ECMAScript regular expression with at least two capture groups (name and value)" },
			Integer("name_group", 1, 9), Integer("value_group", 1, 9) } });
		schema.push_back({ "command.light", { templ("set", true), templ("query", false), templ("query_min", false),
			templ("query_max", false), replyName("reply") } });
		schema.push_back({ "command.contrast", { templ("set", true), templ("query", false), replyName("reply") } });
		schema.push_back({ "command.handshake", { templ("send", false), replyName("reply") } });
		schema.push_back({ "command.dim_mode", { templ("set", true), Integer("standard", 0, 65535),
			Integer("custom", 0, 65535), replyName("reply") } });
		for (const char* channel : { "command.custom_red", "command.custom_green", "command.custom_blue" })
			schema.push_back({ channel, { templ("set", true), Integer("min", 0, 65535), Integer("max", 0, 65535),
				replyName("reply") } });
		schema.push_back({ "light", { Integer("min", 0, 65535), Integer("max", 1, 65535), Integer("floor", 0, 65535),
			Integer("floor_reactive", 0, 65535), Integer("floor_black", 0, 65535),
			KeyRule{ "curve", [](const std::string& v) { std::vector<std::pair<double, double>> c;
				return DeviceProfileDetail::ParseCurve(v, c); },
				"drive:relative_light pairs, both rising (light above 0, at most 10), e.g. 60:0.55, 80:0.78, 100:1.00; "
				"empty = light proportional to drive" } } });
		schema.push_back({ "contrast", { Integer("min", 0, 65535), Integer("max", 0, 65535), Integer("max_usable", 0, 65535),
			KeyRule{ "dmd_clip", [](const std::string& v) { int x = 0; return DeviceProfileDetail::ParseInt(v, -1, 65535, x); },
				"-1 (= max_usable) or a device contrast value" } } });
		schema.push_back({ "timing", { Integer("latency_ms", 0, 2000), Integer("settle_ms", 0, 5000),
			Integer("contrast_latency_ms", 0, 2000), Integer("contrast_settle_ms", 0, 5000),
			Integer("ack_timeout_ms", 20, 5000), Integer("no_reply_backoff_ms", 0, 60000) } });
		schema.push_back({ "limits", { Integer("max_step", 0, 65535), Integer("min_command_gap_ms", 0, 5000),
			Integer("burst_commands", 0, 1000), Integer("burst_pause_ms", 0, 60000) } });
		schema.push_back({ "restore", {
			KeyRule{ "light", [](const std::string& v)
				{ int x = 0; return ConfigFile::NormalizeName(v) == "ceiling" || DeviceProfileDetail::ParseInt(v, 0, 65535, x); },
				"ceiling (the user's led_ceiling, 255 = full light by default) or a device light value" },
			Integer("contrast", 0, 65535) } });
		return schema;
	}

	// Keys every profile must state (the rest default to the FL35's values
	// only where noted in the authoring guide; a profile should state them all).
	inline std::vector<std::pair<std::string, std::string>> DeviceProfileRequiredKeys()
	{
		return {
			{ "profile", "schema" }, { "profile", "maker" }, { "profile", "model" }, { "profile", "version" },
			{ "transport", "type" }, { "transport", "baud" }, { "transport", "read_back_supported" },
			{ "framing", "send_only" },
			{ "command.light", "set" },
			{ "light", "min" }, { "light", "max" }, { "light", "floor" }, { "light", "floor_reactive" }, { "light", "floor_black" },
			{ "timing", "latency_ms" }, { "timing", "settle_ms" },
			{ "limits", "max_step" }, { "limits", "min_command_gap_ms" },
			{ "restore", "light" } };
	}

	namespace DeviceProfileDetail
	{
		inline const std::vector<ConfigSchema::KeyRule>* RulesFor(
			const std::vector<std::pair<std::string, std::vector<ConfigSchema::KeyRule>>>& schema, const std::string& section)
		{
			for (const auto& entry : schema)
				if (entry.first == section) return &entry.second;
			if (section.rfind("framing.", 0) == 0 && section.size() > 8)
				for (const auto& entry : schema)
					if (entry.first == "framing.*") return &entry.second;
			return nullptr;
		}

		inline bool Get(const ConfigFile& file, const char* section, const char* key, std::string& value)
		{
			if (!file.TryGetString(section, key, value)) return false;
			value = Unquote(value);
			return true;
		}

		// Templates whose text names something a profile must never send
		// (power, lamp, input, reset, firmware) need [profile] reviewed = yes.
		inline std::string RiskyWord(const std::string& templateText)
		{
			std::string upper = templateText;
			std::transform(upper.begin(), upper.end(), upper.begin(),
				[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			for (const char* word : { "POWR", "POWER", "PWR", "STBY", "STANDBY", "LAMP", "RST", "RESET", "FACT",
				"FIRM", "FWUP", "UPDATE", "INPUT", "SHUT", "REBOOT" })
				if (upper.find(word) != std::string::npos) return word;
			return {};
		}
	}

	// Loads and validates a profile file. On any problem: false, `error` names
	// the file, the section, the key and what is wrong, and `profile` is left
	// untouched. A profile is all-or-nothing: VP never uses part of one.
	inline bool LoadDeviceProfile(const std::string& path, DeviceProfile& profileOut, std::string& error)
	{
		using namespace DeviceProfileDetail;
		error.clear();
		const std::string where = "device profile " + path + ": ";
		ConfigFile file;
		if (path.empty() || !file.Load(path, ConfigFile::ReadPolicy::Fresh))
		{
			error = "device profile " + path + ": cannot read the file";
			return false;
		}
		for (const std::string& warning : file.GetWarnings())
		{
			// Strict: a malformed line, a duplicate key or section is refused.
			error = where + warning;
			return false;
		}
		const auto schema = DeviceProfileSchema();
		for (const std::string& section : file.GetSectionNames())
		{
			const auto* rules = RulesFor(schema, section);
			if (!rules)
			{
				error = where + "unknown section [" + section + "] (the schema is closed: see the authoring guide)";
				return false;
			}
			std::string sectionError;
			if (!ConfigSchema::ValidateSection(file, section, *rules, sectionError))
			{
				error = where + sectionError;
				return false;
			}
		}
		for (const auto& required : DeviceProfileRequiredKeys())
		{
			std::string value;
			if (!file.TryGetString(required.first, required.second, value))
			{
				error = where + "[" + required.first + "] " + required.second + " is required";
				return false;
			}
		}

		DeviceProfile p;
		p.path = path;
		std::string value;
		auto readInt = [&](const char* section, const char* key, int& target)
		{
			// The schema already checked each key's own range (baud up to 921600).
			if (file.TryGetString(section, key, value)) ParseInt(value, -1, 1000000, target);
		};
		auto readBool = [&](const char* section, const char* key, bool& target)
		{
			if (file.TryGetString(section, key, value)) ParseBool(value, target);
		};
		// [profile]
		readInt("profile", "schema", p.schema);
		Get(file, "profile", "maker", p.maker);
		Get(file, "profile", "model", p.model);
		Get(file, "profile", "version", p.version);
		p.source.clear();
		Get(file, "profile", "source", p.source);
		p.reviewed = false;
		readBool("profile", "reviewed", p.reviewed);
		// [transport]
		readInt("transport", "baud", p.baud);
		readInt("transport", "data_bits", p.dataBits);
		if (Get(file, "transport", "parity", value)) p.parity = ConfigFile::NormalizeName(value);
		readInt("transport", "stop_bits", p.stopBits);
		if (Get(file, "transport", "flow_control", value)) p.flowControl = ConfigFile::NormalizeName(value);
		readBool("transport", "read_back_supported", p.readBackSupported);
		p.readBack = false;
		readBool("transport", "read_back", p.readBack);
		// [framing] and [framing.<name>]
		p.prefix.clear();
		Get(file, "framing", "prefix", p.prefix);
		p.framings.clear();
		for (const std::string& section : file.GetSectionNames())
		{
			if (section.rfind("framing.", 0) != 0) continue;
			DeviceFraming framing;
			framing.name = section.substr(8);
			if (!ValidName(framing.name))
			{
				error = where + "[" + section + "] framing names use letters, digits and _ (at most 32)";
				return false;
			}
			if (!Get(file, section.c_str(), "suffix", value))
			{
				error = where + "[" + section + "] suffix is required (the bytes that end each command, e.g. \\r)";
				return false;
			}
			DecodeEscapes(value, framing.suffix, error);
			if (Get(file, section.c_str(), "case", value)) framing.letterCase = ConfigFile::NormalizeName(value);
			framing.probe = p.readBackSupported;
			if (file.TryGetString(section, "probe", value)) ParseBool(value, framing.probe);
			p.framings.push_back(framing);
		}
		if (p.framings.empty())
		{
			error = where + "needs at least one [framing.<name>] section";
			return false;
		}
		p.probeOrder.clear();
		if (Get(file, "framing", "probe_order", value)) p.probeOrder = SplitList(value);
		p.fallback.clear();
		if (Get(file, "framing", "fallback", value)) p.fallback = ConfigFile::NormalizeName(value);
		Get(file, "framing", "send_only", value);
		p.sendOnly = ConfigFile::NormalizeName(value);
		// [reply]
		if (Get(file, "reply", "pattern", value)) p.replyPattern = value;
		else p.replyPattern.clear();
		readInt("reply", "name_group", p.replyNameGroup);
		readInt("reply", "value_group", p.replyValueGroup);
		// [command.*]
		for (int i = 0; i < DEVICE_COMMAND_COUNT; ++i)
		{
			const DeviceCommandId id = static_cast<DeviceCommandId>(i);
			const char* section = DeviceCommandSection(id);
			DeviceCommand command;
			command.present = file.HasSection(section);
			if (command.present)
			{
				const char* setKey = id == DeviceCommandId::Handshake ? "send" : "set";
				if (!Get(file, section, setKey, command.set))
				{
					error = where + "[" + section + "] " + setKey + " is required";
					return false;
				}
				Get(file, section, "query", command.query);
				Get(file, section, "query_min", command.queryMin);
				Get(file, section, "query_max", command.queryMax);
				Get(file, section, "reply", command.reply);
				readInt(section, "min", command.min);
				readInt(section, "max", command.max);
				readInt(section, "standard", command.standard);
				readInt(section, "custom", command.custom);
				if (id == DeviceCommandId::DimMode)
				{
					std::string standardText, customText;
					if (!file.TryGetString(section, "standard", standardText) || !file.TryGetString(section, "custom", customText))
					{
						error = where + "[" + section + "] needs standard and custom (the dim-mode values)";
						return false;
					}
					command.min = std::min(command.standard, command.custom);
					command.max = std::max(command.standard, command.custom);
				}
				if (id >= DeviceCommandId::CustomRed && id <= DeviceCommandId::CustomBlue)
				{
					std::string minText, maxText;
					if (!file.TryGetString(section, "min", minText) || !file.TryGetString(section, "max", maxText) ||
						command.min >= command.max)
					{
						error = where + "[" + section + "] needs min < max (the custom dim's device range)";
						return false;
					}
				}
			}
			p.commands[i] = command;
		}
		// [light]
		readInt("light", "min", p.lightMin);
		readInt("light", "max", p.lightMax);
		readInt("light", "floor", p.floor);
		readInt("light", "floor_reactive", p.floorReactive);
		readInt("light", "floor_black", p.floorBlack);
		p.lightCurve.clear();
		if (Get(file, "light", "curve", value)) ParseCurve(value, p.lightCurve);
		// [contrast]
		readInt("contrast", "min", p.contrastMin);
		readInt("contrast", "max", p.contrastMax);
		readInt("contrast", "max_usable", p.contrastMaxUsable);
		readInt("contrast", "dmd_clip", p.dmdClip);
		// [timing]
		readInt("timing", "latency_ms", p.latencyMs);
		readInt("timing", "settle_ms", p.settleMs);
		readInt("timing", "contrast_latency_ms", p.contrastLatencyMs);
		readInt("timing", "contrast_settle_ms", p.contrastSettleMs);
		readInt("timing", "ack_timeout_ms", p.ackTimeoutMs);
		readInt("timing", "no_reply_backoff_ms", p.noReplyBackoffMs);
		// [limits]
		readInt("limits", "max_step", p.maxStep);
		readInt("limits", "min_command_gap_ms", p.minCommandGapMs);
		readInt("limits", "burst_commands", p.burstCommands);
		readInt("limits", "burst_pause_ms", p.burstPauseMs);
		// [restore]
		if (Get(file, "restore", "light", value))
		{
			if (ConfigFile::NormalizeName(value) == "ceiling") p.restoreLight = -1;
			else ParseInt(value, 0, 65535, p.restoreLight);
		}
		readInt("restore", "contrast", p.restoreContrast);

		// Cross-checks.
		auto fail = [&](const std::string& message) { error = where + message; return false; };
		if (p.lightMin >= p.lightMax) return fail("[light] needs min < max");
		for (const auto& floorKey : { std::make_pair("floor_black", p.floorBlack), std::make_pair("floor_reactive", p.floorReactive),
			std::make_pair("floor", p.floor) })
			if (floorKey.second < p.lightMin || floorKey.second > p.lightMax)
				return fail(std::string("[light] ") + floorKey.first + " " + std::to_string(floorKey.second) +
					" is outside min..max (" + std::to_string(p.lightMin) + ".." + std::to_string(p.lightMax) + ")");
		if (p.floorBlack > p.floor || p.floorBlack > p.floorReactive)
			return fail("[light] needs floor_black <= floor and floor_black <= floor_reactive");
		if (p.restoreLight >= 0 && (p.restoreLight < p.lightMin || p.restoreLight > p.lightMax))
			return fail("[restore] light " + std::to_string(p.restoreLight) + " is outside [light] min..max");
		if (p.maxStep > p.lightMax - p.lightMin)
			return fail("[limits] max_step is larger than the whole [light] range");
		for (const auto& point : p.lightCurve)
			if (point.first < p.lightMin || point.first > p.lightMax)
				return fail("[light] curve drive value " + std::to_string(static_cast<int>(point.first)) + " is outside min..max");
		if (p.Has(DeviceCommandId::Contrast))
		{
			std::string minText, maxText, restoreText;
			if (!file.TryGetString("contrast", "min", minText) || !file.TryGetString("contrast", "max", maxText) ||
				!file.TryGetString("restore", "contrast", restoreText))
				return fail("[command.contrast] needs [contrast] min and max and [restore] contrast");
			if (p.contrastMin >= p.contrastMax) return fail("[contrast] needs min < max");
			if (!file.TryGetString("contrast", "max_usable", value)) p.contrastMaxUsable = p.contrastMax;
			if (p.contrastMaxUsable < p.contrastMin || p.contrastMaxUsable > p.contrastMax)
				return fail("[contrast] max_usable is outside min..max");
			if (p.dmdClip != -1 && (p.dmdClip < p.contrastMin || p.dmdClip > p.contrastMax))
				return fail("[contrast] dmd_clip is outside min..max (or -1)");
			if (p.restoreContrast != p.contrastMin)
				return fail("[restore] contrast must equal [contrast] min: VP's neutral contrast is the value it restores");
		}
		else if (file.HasSection("contrast"))
			return fail("[contrast] given without [command.contrast]");
		const bool anyCustom = p.Has(DeviceCommandId::CustomRed) || p.Has(DeviceCommandId::CustomGreen) ||
			p.Has(DeviceCommandId::CustomBlue);
		const bool allCustom = p.Has(DeviceCommandId::CustomRed) && p.Has(DeviceCommandId::CustomGreen) &&
			p.Has(DeviceCommandId::CustomBlue);
		if (anyCustom && (!allCustom || !p.Has(DeviceCommandId::DimMode)))
			return fail("the custom dim needs all of [command.custom_red], [command.custom_green], [command.custom_blue] and [command.dim_mode]");
		if (anyCustom)
			for (DeviceCommandId id : { DeviceCommandId::CustomRed, DeviceCommandId::CustomGreen, DeviceCommandId::CustomBlue })
				if (p.Command(id).min != 0 || p.Command(id).max < 40)
				{
					// VP's custom dim sends values 0..40 (ftb_custom_start at most 40)
					// unchanged; a narrower device range would refuse them.
					return fail(std::string("[") + DeviceCommandSection(id) + "] VP's custom dim sends 0-40: min must be 0 and max at least 40");
				}
		for (const DeviceFraming& framing : p.framings)
			if (framing.probe && !p.readBackSupported)
				return fail("[framing." + framing.name + "] probe = yes needs [transport] read_back_supported = yes");
		if (!p.Framing(p.sendOnly)) return fail("[framing] send_only names no [framing." + p.sendOnly + "]");
		if (!p.fallback.empty() && !p.Framing(p.fallback))
			return fail("[framing] fallback names no [framing." + p.fallback + "]");
		for (const std::string& name : p.probeOrder)
		{
			const DeviceFraming* framing = p.Framing(name);
			if (!framing) return fail("[framing] probe_order names no [framing." + name + "]");
			if (!framing->probe) return fail("[framing] probe_order lists [framing." + name + "], which has probe = no");
		}
		if (p.readBack && !p.readBackSupported)
			return fail("[transport] read_back = yes needs read_back_supported = yes");
		if (p.readBackSupported)
		{
			if (p.replyPattern.empty()) return fail("read_back_supported = yes needs [reply] pattern");
			if (p.probeOrder.empty()) return fail("read_back_supported = yes needs [framing] probe_order");
			try
			{
				std::regex re(p.replyPattern, std::regex::ECMAScript);
				if (static_cast<int>(re.mark_count()) < std::max(p.replyNameGroup, p.replyValueGroup))
					return fail("[reply] name_group / value_group exceed the pattern's capture groups");
			}
			catch (const std::regex_error&) { return fail("[reply] pattern does not compile"); }
			if (p.replyNameGroup == p.replyValueGroup) return fail("[reply] name_group and value_group must differ");
			if (p.Command(DeviceCommandId::Light).reply.empty())
				return fail("read_back_supported = yes needs [command.light] reply (the name its replies carry)");
		}
		else if (file.HasSection("reply") || !p.Command(DeviceCommandId::Light).query.empty())
			return fail("[reply] and query templates need [transport] read_back_supported = yes");
		// Every template renders at its range ends, and none names a risky word
		// unless the author marked the profile reviewed.
		for (int i = 0; i < DEVICE_COMMAND_COUNT; ++i)
		{
			const DeviceCommandId id = static_cast<DeviceCommandId>(i);
			const DeviceCommand& command = p.Command(id);
			if (!command.present) continue;
			for (const std::string* raw : { &command.set, &command.query, &command.queryMin, &command.queryMax })
			{
				if (raw->empty()) continue;
				const std::string risky = RiskyWord(*raw);
				if (!risky.empty() && !p.reviewed)
					return fail(std::string("[") + DeviceCommandSection(id) + "] template '" + *raw + "' contains '" + risky +
						"'; Dynamic Drive never sends power, lamp, input, reset or firmware commands. If this is "
						"really the light/contrast command, set [profile] reviewed = yes");
			}
			int low = command.min, high = command.max;
			if (id == DeviceCommandId::Light) { low = p.lightMin; high = p.lightMax; }
			if (id == DeviceCommandId::Contrast) { low = p.contrastMin; high = p.contrastMax; }
			for (const DeviceFraming& framing : p.framings)
				for (int sample : { low, high })
				{
					std::string bytes, renderError;
					if (!RenderDeviceSet(p, framing, id, sample, bytes, renderError))
						return fail(std::string("[") + DeviceCommandSection(id) + "] " + renderError);
				}
		}
		profileOut = p;
		return true;
	}

	// Field-by-field comparison; "" when equal, else the first difference
	// (the FL35 equivalence test's message).
	inline std::string DeviceProfileDifference(const DeviceProfile& a, const DeviceProfile& b)
	{
#define VP_DD_DIFF(field) if (!(a.field == b.field)) return #field;
		VP_DD_DIFF(schema) VP_DD_DIFF(baud) VP_DD_DIFF(dataBits) VP_DD_DIFF(parity) VP_DD_DIFF(stopBits)
		VP_DD_DIFF(flowControl) VP_DD_DIFF(readBackSupported) VP_DD_DIFF(readBack) VP_DD_DIFF(prefix)
		VP_DD_DIFF(probeOrder) VP_DD_DIFF(fallback) VP_DD_DIFF(sendOnly) VP_DD_DIFF(replyPattern)
		VP_DD_DIFF(replyNameGroup) VP_DD_DIFF(replyValueGroup) VP_DD_DIFF(lightMin) VP_DD_DIFF(lightMax)
		VP_DD_DIFF(floor) VP_DD_DIFF(floorReactive) VP_DD_DIFF(floorBlack) VP_DD_DIFF(lightCurve)
		VP_DD_DIFF(contrastMin) VP_DD_DIFF(contrastMax) VP_DD_DIFF(contrastMaxUsable) VP_DD_DIFF(dmdClip)
		VP_DD_DIFF(latencyMs) VP_DD_DIFF(settleMs) VP_DD_DIFF(contrastLatencyMs) VP_DD_DIFF(contrastSettleMs)
		VP_DD_DIFF(ackTimeoutMs) VP_DD_DIFF(noReplyBackoffMs) VP_DD_DIFF(maxStep) VP_DD_DIFF(minCommandGapMs)
		VP_DD_DIFF(burstCommands) VP_DD_DIFF(burstPauseMs) VP_DD_DIFF(restoreLight) VP_DD_DIFF(restoreContrast)
#undef VP_DD_DIFF
		if (a.framings.size() != b.framings.size()) return "framings";
		for (size_t i = 0; i < a.framings.size(); ++i)
		{
			const DeviceFraming& x = a.framings[i];
			const DeviceFraming* y = b.Framing(x.name);
			if (!y || y->suffix != x.suffix || y->letterCase != x.letterCase || y->probe != x.probe)
				return "framing." + x.name;
		}
		for (int i = 0; i < DEVICE_COMMAND_COUNT; ++i)
		{
			const DeviceCommand& x = a.commands[i];
			const DeviceCommand& y = b.commands[i];
			if (x.present != y.present || x.set != y.set || x.query != y.query || x.queryMin != y.queryMin ||
				x.queryMax != y.queryMax || x.reply != y.reply || x.min != y.min || x.max != y.max ||
				(i == static_cast<int>(DeviceCommandId::DimMode) && (x.standard != y.standard || x.custom != y.custom)))
				return DeviceCommandSection(static_cast<DeviceCommandId>(i));
		}
		return {};
	}

	// Resolves a device_profile value: absolute, or relative to the folder of
	// the configuration that names it. "" = the built-in FL35 profile.
	inline std::string ResolveDeviceProfilePath(const std::string& value, const std::string& owningConfigPath)
	{
		std::string path = DeviceProfileDetail::Unquote(value);
		if (path.empty()) return {};
		const bool absolute = (path.size() > 1 && path[1] == ':') || path[0] == '\\' || path[0] == '/';
		if (absolute) return path;
		const size_t slash = owningConfigPath.find_last_of("\\/");
		return slash == std::string::npos ? path : owningConfigPath.substr(0, slash + 1) + path;
	}

	// The offline validator's report (VideoProcessor.exe /validate_dd_profile
	// and tools\validate_dd_profile.ps1): "OK ..." or "INVALID ...".
	inline bool ValidateDeviceProfileReport(const std::string& path, std::string& report)
	{
		DeviceProfile profile;
		std::string error;
		if (!LoadDeviceProfile(path, profile, error))
		{
			report = "INVALID " + error + "\nVP would run no Dynamic Drive with this profile selected.";
			return false;
		}
		auto has = [&](DeviceCommandId id) { return profile.Has(id) ? "yes" : "no"; };
		report = "OK device profile " + path + "\n" +
			"  model: " + profile.maker + " " + profile.model + ", profile version " + profile.version +
			", schema " + std::to_string(profile.schema) + "\n" +
			"  transport: serial " + std::to_string(profile.baud) + " baud, " + std::to_string(profile.dataBits) +
			" data bits, parity " + profile.parity + ", " + std::to_string(profile.stopBits) + " stop bit(s), flow " +
			profile.flowControl + "; read-back " + (profile.readBackSupported ? "supported" : "not supported") +
			", default " + (profile.readBack ? "on" : "off") + "\n" +
			"  commands: light yes, contrast " + has(DeviceCommandId::Contrast) + ", handshake " +
			has(DeviceCommandId::Handshake) + ", dim mode " + has(DeviceCommandId::DimMode) + ", custom dim " +
			has(DeviceCommandId::CustomRed) + "\n" +
			"  light " + std::to_string(profile.lightMin) + "-" + std::to_string(profile.lightMax) + ", floors " +
			std::to_string(profile.floor) + "/" + std::to_string(profile.floorReactive) + "/" + std::to_string(profile.floorBlack) +
			", max step " + std::to_string(profile.maxStep) + ", gap " + std::to_string(profile.minCommandGapMs) + " ms\n" +
			"  timing: latency " + std::to_string(profile.latencyMs) + " ms, settle " + std::to_string(profile.settleMs) + " ms\n" +
			"  restore: light " + (profile.restoreLight < 0 ? std::string("ceiling") : std::to_string(profile.restoreLight)) +
			(profile.Has(DeviceCommandId::Contrast) ? ", contrast " + std::to_string(profile.restoreContrast) : std::string()) + "\n";
		// Example bytes in each framing, so the author can compare them with
		// the manual.
		for (const DeviceFraming& framing : profile.framings)
		{
			std::string bytes, ignored;
			report += "  framing " + framing.name + (framing.probe ? "" : " (send-only)") + ": ";
			if (RenderDeviceSet(profile, framing, DeviceCommandId::Light, profile.lightMax, bytes, ignored))
				report += "light max = \"" + DeviceProfileDetail::Printable(bytes) + "\"";
			if (profile.Has(DeviceCommandId::Contrast) &&
				RenderDeviceSet(profile, framing, DeviceCommandId::Contrast, profile.contrastMin, bytes, ignored))
				report += ", contrast neutral = \"" + DeviceProfileDetail::Printable(bytes) + "\"";
			report += "\n";
		}
		return true;
	}
}
