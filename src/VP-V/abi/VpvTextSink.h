// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Text across the plugin boundary (D-P2a): no STL type and no CString crosses it. A callee writes UTF-8
// through IVpvTextSink; the caller owns the sink and supplies whatever buffer it likes (StringSink below is
// the host-side one; it lives in the caller's module and the callee sees only the abstract interface).
// Write may be called several times; the text is the concatenation. A callee that has nothing to say writes
// nothing (the caller's text stays empty, as the old CString& getters left it Empty()).

#include <cstddef>
#include <string>

namespace Vpv
{
	class IVpvTextSink
	{
	public:
		virtual void Write(const char* utf8, size_t length) = 0;
	protected:
		~IVpvTextSink() = default;   // never deleted through the interface
	};

	class StringSink final : public IVpvTextSink
	{
	public:
		void Write(const char* utf8, size_t length) override
		{
			if (utf8 && length)
				text.append(utf8, length);
		}
		std::string text;
	};
}
