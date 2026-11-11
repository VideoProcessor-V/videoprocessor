// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pch.h"

#include "StatusExtension.h"

namespace StatusExtension
{
	Snapshot Read(bool rendererPresent, bool answered, const std::wstring& moduleText)
	{
		Snapshot snapshot;
		snapshot.rendererPresent = rendererPresent;
		if (rendererPresent && answered)
			snapshot.vp = VpStatusText::ReadRendererModule(moduleText);
		return snapshot;
	}

	std::string FallbacksHeading(const Snapshot& snapshot, NarrowFn narrow)
	{
		return snapshot.vp.fallbacks.known ? narrow(snapshot.vp.fallbacks.text) : std::string();
	}

	Rows CtrlIRows(const Snapshot& snapshot, const DialogTexts& dialog, bool isAlphaRenderer)
	{
		VpStatusText::CtrlILines<wchar_t> lines;
		lines.motionIntent = dialog.motionIntent;
		lines.ambientIntent = dialog.ambientIntent;
		lines.blackIntent = dialog.blackIntent;
		lines.displayIntent = dialog.displayIntent;
		lines.keyOverrides = dialog.keyOverrides;
		lines.room = dialog.room;
		lines.content = dialog.content;
		if (isAlphaRenderer)
			lines.dejudder = snapshot.vp.dejudder.text;
		lines.resync = snapshot.vp.resync.text;
		lines.fallbacks = snapshot.vp.fallbacks.text;
		return VpStatusText::BuildCtrlIRows(lines);
	}

	Pairs CrashState(const Snapshot& snapshot, const HostLines& host, const DialogTexts& dialog, NarrowFn narrow)
	{
		VpStatusText::CrashStateInputs<wchar_t> in;
		if (snapshot.rendererPresent)
		{
			in.dejudder = snapshot.vp.dejudder;
			in.fallbacks = snapshot.vp.fallbacks;
			in.ledDrive = snapshot.vp.ledDrive;
			in.sourceResolution = snapshot.vp.sourceResolution;
			in.presentation = host.presentation;
			in.renderStall = host.renderStall;
			in.content = dialog.content;
			in.motionIntent = dialog.motionIntent;
			in.ambientIntent = dialog.ambientIntent;
			in.blackIntent = dialog.blackIntent;
			in.displayIntent = dialog.displayIntent;
		}
		return VpStatusText::CrashStateRendererLines(snapshot.rendererPresent, in,
			[narrow](const std::wstring& text) { return narrow(text); });
	}

	Pairs Bundle(const Snapshot& snapshot, const HostLines& host, NarrowFn narrow)
	{
		VpStatusText::BundleInputs<wchar_t> in;
		in.presentation = host.presentation;
		in.dejudder = snapshot.vp.dejudder;
		in.fallbacks = snapshot.vp.fallbacks;
		in.renderStall = host.renderStall;
		in.outputMode = host.outputMode;
		in.resync = snapshot.vp.resync;
		in.resyncEvents = snapshot.vp.resyncEvents;
		return VpStatusText::BundleRendererSettings(in,
			[narrow](const std::wstring& text) { return narrow(text); });
	}
}
