#pragma once
/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Bug hunt 3 (SH10, 2026-10-18): one shortcut grammar for Config and the
// player. VP parsed chords itself (F-keys as one or two digits) while Config
// used RendererProfileConfig::CanonicalizeKeyChord (any integer form), so a
// chord Config accepted could be rejected by VP, which then silently kept its
// compiled default. VP now parses through the same canonicalizer.
#include <RendererProfileConfig.h>

#include <cstdint>
#include <string>

struct ParsedShortcutChord
{
	bool control = false;
	bool alt = false;
	bool shift = false;
	uint16_t virtualKey = 0;   // VK_ code: 'A'-'Z', '0'-'9', VK_F1.., VK_ESCAPE, VK_RETURN
	std::string canonical;     // Config's text form
};

inline bool ParseShortcutChord(const std::string& text, ParsedShortcutChord& parsed)
{
	parsed = ParsedShortcutChord();
	std::string canonical;
	if (!RendererProfileConfig::CanonicalizeKeyChord(text, canonical))
		return false;
	parsed.canonical = canonical;
	std::string rest = canonical;
	for (;;)
	{
		if (rest.compare(0, 5, "Ctrl+") == 0) { parsed.control = true; rest.erase(0, 5); }
		else if (rest.compare(0, 4, "Alt+") == 0) { parsed.alt = true; rest.erase(0, 4); }
		else if (rest.compare(0, 6, "Shift+") == 0) { parsed.shift = true; rest.erase(0, 6); }
		else break;
	}
	if (rest == "Esc") parsed.virtualKey = 0x1B;          // VK_ESCAPE
	else if (rest == "Enter") parsed.virtualKey = 0x0D;   // VK_RETURN
	else if (rest.size() >= 2 && rest[0] == 'F')
		parsed.virtualKey = static_cast<uint16_t>(0x70 + std::stoi(rest.substr(1)) - 1);   // VK_F1..VK_F24
	else if (rest.size() == 1)
		parsed.virtualKey = static_cast<uint16_t>(static_cast<unsigned char>(rest[0]));
	else
		return false;
	return true;
}
