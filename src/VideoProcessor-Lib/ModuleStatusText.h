#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Generic module status text (IVideoRenderer::GetModuleStatus / InvokeModuleCommand).
//
// A renderer publishes the status of its modules through ONE versioned entry instead of one
// virtual per status line, so a new status line needs no new virtual and no plugin ABI change.
// The text is a small key/value format, documented here and shared by both sides:
//
//   status  := line ( '\n' line )*          (an empty status has no lines)
//   line    := key '=' value
//   key     := one or more of [a-z 0-9 _ . -]   (stable machine name, never translated)
//   value   := any text, escaped: '\' -> "\\", newline -> "\n", carriage return -> "\r"
//              (so a value is one line; Parse restores it exactly; may contain '='; may be empty)
//
// - Line order is meaningful: consumers that show the lines keep it.
// - A line that does not parse (no '=', or an invalid key) is skipped, and so is a key the
//   consumer does not know: an older host simply ignores newer lines (forward compatible).
// - A key appears at most once per status; Append replaces nothing, so writers keep keys unique.
// - The module id "" asks for the index: one "module=<id>" line per module the renderer answers.
// - A command reply (InvokeModuleCommand) uses the same format; by convention its "status" key
//   carries the one line to show to the user.
//
// Header-only, pure C++14, no Windows: templated on the character type so a wide (CString)
// renderer and host exchange text without any code-page conversion.
namespace ModuleStatusText
{
	constexpr const char* kIndexKey = "module";
	constexpr const char* kCommandStatusKey = "status";

	template <class Char>
	struct BasicEntry
	{
		std::basic_string<Char> key;
		std::basic_string<Char> value;
	};
	using Entry = BasicEntry<char>;
	using WideEntry = BasicEntry<wchar_t>;

	inline bool IsKeyChar(unsigned int c)
	{
		return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
	}

	template <class Char>
	bool IsValidKey(const Char* key, size_t length)
	{
		if (!key || length == 0)
			return false;
		for (size_t i = 0; i < length; ++i)
			if (!IsKeyChar(static_cast<unsigned int>(key[i])))
				return false;
		return true;
	}

	inline bool IsValidKey(const char* key)
	{
		return key && IsValidKey(key, std::char_traits<char>::length(key));
	}

	// Appends "key=value" (with the separating newline when text is not empty), the value
	// escaped so it stays one line and Parse returns it unchanged. Returns false (and appends
	// nothing) for an invalid key.
	template <class Char>
	bool Append(std::basic_string<Char>& text, const char* key, const std::basic_string<Char>& value)
	{
		if (!IsValidKey(key))
			return false;
		if (!text.empty())
			text += static_cast<Char>('\n');
		for (const char* k = key; *k; ++k)
			text += static_cast<Char>(*k);
		text += static_cast<Char>('=');
		const Char backslash = static_cast<Char>('\\');
		for (Char c : value)
		{
			if (c == backslash) { text += backslash; text += backslash; }
			else if (c == static_cast<Char>('\n')) { text += backslash; text += static_cast<Char>('n'); }
			else if (c == static_cast<Char>('\r')) { text += backslash; text += static_cast<Char>('r'); }
			else text += c;
		}
		return true;
	}

	// The inverse of Append's escaping; an unknown escape is kept as written.
	template <class Char>
	std::basic_string<Char> Unescape(const Char* begin, const Char* end)
	{
		std::basic_string<Char> out;
		out.reserve(static_cast<size_t>(end - begin));
		const Char backslash = static_cast<Char>('\\');
		for (const Char* p = begin; p < end; ++p)
		{
			if (*p != backslash || p + 1 == end) { out += *p; continue; }
			const Char next = p[1];
			if (next == backslash) out += backslash;
			else if (next == static_cast<Char>('n')) out += static_cast<Char>('\n');
			else if (next == static_cast<Char>('r')) out += static_cast<Char>('\r');
			else { out += *p; continue; }
			++p;
		}
		return out;
	}

	template <class Char>
	bool Append(std::basic_string<Char>& text, const char* key, const Char* value)
	{
		return Append(text, key, value ? std::basic_string<Char>(value) : std::basic_string<Char>());
	}

	// Every well-formed line, in order.
	template <class Char>
	std::vector<BasicEntry<Char>> Parse(const std::basic_string<Char>& text)
	{
		std::vector<BasicEntry<Char>> entries;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find(static_cast<Char>('\n'), start);
			if (end == std::basic_string<Char>::npos)
				end = text.size();
			// The key is short: look for '=' within this line only.
			const Char* line = text.data() + start;
			const Char* lineEnd = text.data() + end;
			const Char* equals = std::char_traits<Char>::find(line, end - start, static_cast<Char>('='));
			if (equals && IsValidKey(line, static_cast<size_t>(equals - line)))
			{
				if (lineEnd > equals + 1 && lineEnd[-1] == static_cast<Char>('\r'))
					--lineEnd;   // a CRLF transport (a CR inside a value is escaped)
				BasicEntry<Char> entry;
				entry.key.assign(line, equals);
				entry.value = Unescape(equals + 1, lineEnd);
				entries.push_back(entry);
			}
			if (end == text.size())
				break;
			start = end + 1;
		}
		return entries;
	}

	template <class Char>
	bool KeyEquals(const std::basic_string<Char>& key, const char* name)
	{
		size_t i = 0;
		for (; name[i] != '\0'; ++i)
			if (i >= key.size() || key[i] != static_cast<Char>(name[i]))
				return false;
		return i == key.size();
	}

	// The first value of `key`; false when the status has no such line.
	template <class Char>
	bool Find(const std::basic_string<Char>& text, const char* key, std::basic_string<Char>& value)
	{
		for (const BasicEntry<Char>& entry : Parse(text))
			if (KeyEquals(entry.key, key))
			{
				value = entry.value;
				return true;
			}
		value.clear();
		return false;
	}

	// The index status for module id "": one "module=<id>" line per id.
	template <class Char>
	std::basic_string<Char> Index(const std::vector<std::basic_string<Char>>& moduleIds)
	{
		std::basic_string<Char> text;
		for (const auto& id : moduleIds)
			Append(text, kIndexKey, id);
		return text;
	}
}
