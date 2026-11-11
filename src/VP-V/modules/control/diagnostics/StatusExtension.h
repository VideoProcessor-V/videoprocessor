#pragma once
// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// W1-C14: the ONE reader of VP's status lines (FallbackStatus heading + VpStatusText lines) for
// the GUI, the Config status heading, Ctrl+I, the crash-state keys and the Ctrl+Alt+D bundle.
// Pure (no Windows, no MFC, C++14), wchar_t text exactly as the renderer's VP module publishes it
// (src/VP-V/core/StatusText.h, FallbackStatus.h). The MFC adapter src/VP-V/gui/VpStatusAdapter.h
// is a thin client: it polls the renderer, converts CString <-> std::wstring, and calls here.
// Every function applies the same VpStatusText builders the adapter called before, so every
// row, key and value is byte-identical (VpStatusAdapterTests, StatusTextTests).
//
// The Config shell heading and the dialog heading are Bill's files and still read their own
// copy; they move onto this reader when their packages move (doc 93 W1-C14).

#include <VP-V/core/StatusText.h>

#include <string>
#include <utility>
#include <vector>

namespace StatusExtension
{
	using Lines = VpStatusText::RendererLines<wchar_t>;
	using Line = VpStatusText::Line<wchar_t>;
	using Rows = VpStatusText::CtrlIRows<wchar_t>;
	using Pairs = std::vector<std::pair<std::string, std::string>>;
	// The host's text conversion for state/bundle values (the old call sites used CStringA).
	using NarrowFn = std::string (*)(const std::wstring&);

	// One poll of the renderer's VP module.
	struct Snapshot
	{
		bool rendererPresent = false;
		Lines vp;
	};

	// `answered` = the renderer returned its "vp" module text (VpvCalls::GetModuleStatus true).
	// No renderer or no answer: every line unknown, as the old per-line getters.
	Snapshot Read(bool rendererPresent, bool answered, const std::wstring& moduleText);

	// The "Fallbacks engaged" heading for the one-time notices and the Config status ("" = none).
	std::string FallbacksHeading(const Snapshot& snapshot, NarrowFn narrow);

	// The VP lines the dialog adds (its own state, not the renderer's), already wide.
	struct DialogTexts
	{
		std::wstring keyOverrides;
		std::wstring room;
		std::wstring content;
		std::wstring motionIntent;
		std::wstring ambientIntent;
		std::wstring blackIntent;
		std::wstring displayIntent;
	};

	// Ctrl+I: VP's rows for the overlay's two extension points. The de-judder line is the
	// VP Renderer's only (`isAlphaRenderer`), as before.
	Rows CtrlIRows(const Snapshot& snapshot, const DialogTexts& dialog, bool isAlphaRenderer);

	// What the host's own renderer getters return for the crash-state and bundle lines.
	struct HostLines
	{
		Line presentation;
		Line renderStall;
		Line outputMode;   // bundle only
	};

	// Crash state: the renderer-owned (key, value) pairs in the old order; every key "-" with no
	// renderer. Only `content` and the intent lines of `dialog` are used.
	Pairs CrashState(const Snapshot& snapshot, const HostLines& host, const DialogTexts& dialog, NarrowFn narrow);

	// Ctrl+Alt+D: the renderer's live lines at the key press, in the old order.
	Pairs Bundle(const Snapshot& snapshot, const HostLines& host, NarrowFn narrow);
}
