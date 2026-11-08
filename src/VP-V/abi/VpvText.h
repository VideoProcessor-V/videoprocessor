// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// CString <-> UTF-8 for the extension interfaces (VpvTextSink.h): both sides of the plugin boundary convert
// with these, so text that the old CString& getters carried arrives exactly as it was written (UTF-16 to UTF-8
// and back is lossless for well-formed text). Windows/MFC side; C++14.

#include <VP-V/abi/VpvTextSink.h>

#include <atlstr.h>
#include <string>

namespace Vpv
{
	static_assert(sizeof(TCHAR) == sizeof(wchar_t), "the extension ABI text helpers assume a UNICODE build");

	inline std::string ToUtf8(const CString& text)
	{
		std::string out;
		const int length = text.GetLength();
		if (length <= 0)
			return out;
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.GetString(), length, nullptr, 0, nullptr, nullptr);
		if (bytes <= 0)
			return out;
		out.resize(static_cast<size_t>(bytes));
		WideCharToMultiByte(CP_UTF8, 0, text.GetString(), length, &out[0], bytes, nullptr, nullptr);
		return out;
	}

	inline CString FromUtf8(const char* utf8, size_t length)
	{
		CString out;
		if (!utf8 || length == 0)
			return out;
		const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(length), nullptr, 0);
		if (chars <= 0)
			return out;
		wchar_t* buffer = out.GetBuffer(chars);
		MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(length), buffer, chars);
		out.ReleaseBuffer(chars);
		return out;
	}

	inline CString FromUtf8(const std::string& text) { return FromUtf8(text.data(), text.size()); }

	// Callee side: write a CString through the sink (nothing for an empty string).
	inline void WriteText(IVpvTextSink& sink, const CString& text)
	{
		const std::string utf8 = ToUtf8(text);
		if (!utf8.empty())
			sink.Write(utf8.data(), utf8.size());
	}
}
