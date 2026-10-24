#pragma once

#include <cstdio>
#include <string>
#include <type_traits>

// Efficiency audit 2026-10-24 #10 (upstreamable, no VP-V dependency): a change-detection key
// for "log this line when the state changes" sites on the render thread. It replaces a
// std::ostringstream that was built, formatted and str()-copied twice on every refresh.
//
// The text is the SAME as `std::ostringstream << value` with the default (classic) locale and
// flags, so a key changes exactly when the old ostringstream key changed (same log lines):
//   bool / integers / enums: decimal ("%d"-style; bool as 0/1, as without boolalpha);
//   float / double: "%g" (precision 6), as the default ostream float format;
//   char: the character; const char* / std::string: the text.
// One reserved buffer, no locale, no stream. Compare with Changed(last): it copies into `last`
// (reusing its capacity) only on a change.
class ChangeKey
{
public:
	ChangeKey() { text.reserve(256); }

	ChangeKey& operator<<(bool v) { return Int(v ? 1 : 0); }
	ChangeKey& operator<<(char c) { text.push_back(c); return *this; }
	ChangeKey& operator<<(signed char c) { text.push_back(static_cast<char>(c)); return *this; }     // as ostream: a character
	ChangeKey& operator<<(unsigned char c) { text.push_back(static_cast<char>(c)); return *this; }
	ChangeKey& operator<<(const char* s) { if (s) text.append(s); return *this; }
	ChangeKey& operator<<(const std::string& s) { text.append(s); return *this; }
	ChangeKey& operator<<(float v) { return Float(static_cast<double>(v)); }
	ChangeKey& operator<<(double v) { return Float(v); }
	template <typename T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, bool>::value &&
		!std::is_same<T, char>::value && !std::is_same<T, signed char>::value && !std::is_same<T, unsigned char>::value, int>::type = 0>
	ChangeKey& operator<<(T v) { return std::is_signed<T>::value ? Int(static_cast<long long>(v)) : UInt(static_cast<unsigned long long>(v)); }
	// Unscoped enums stream as their integer (as with ostream's integral promotion).
	template <typename T, typename std::enable_if<std::is_enum<T>::value, int>::type = 0>
	ChangeKey& operator<<(T v) { return Int(static_cast<long long>(v)); }

	const std::string& str() const { return text; }
	// True (and `last` updated) when the key differs from `last`.
	bool Changed(std::string& last) const
	{
		if (last == text)
			return false;
		last.assign(text);
		return true;
	}

private:
	ChangeKey& Int(long long v) { char b[32]; const int n = std::snprintf(b, sizeof(b), "%lld", v); text.append(b, n > 0 ? n : 0); return *this; }
	ChangeKey& UInt(unsigned long long v) { char b[32]; const int n = std::snprintf(b, sizeof(b), "%llu", v); text.append(b, n > 0 ? n : 0); return *this; }
	ChangeKey& Float(double v) { char b[48]; const int n = std::snprintf(b, sizeof(b), "%g", v); text.append(b, n > 0 ? n : 0); return *this; }
	std::string text;
};
