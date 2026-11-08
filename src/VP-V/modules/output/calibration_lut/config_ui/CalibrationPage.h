// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Calibration settings page of the Config editor (automatic display calibration with a meter;
// docs/calibration/AUTO-CALIBRATION.md): the [calibration] settings, the wizard, the calibration RPC
// and its status poll. A Config page provider (ConfigPageHost.h): it registers the page as a sub-page
// of the master VP-V page, grouped under the Output unit, and joins its RPC thread when the editor
// closes. It reaches the editor only through ConfigPageHost::EditorKit.
//
// One instance serves every editor of the process; per-editor state is keyed by the editor's
// ConfigFormApi and dropped in OnEditorClosing. UI thread only.

#include <VideoProcessor-Config/ConfigPageHost.h>

#include <map>
#include <memory>

namespace Vpv
{
	class CalibrationPage final : public ConfigPageHost::IConfigPageProvider
	{
	public:
		CalibrationPage();
		~CalibrationPage() override;

		void AppendPages(ConfigPageHost::ConfigFormApi& api, std::vector<ConfigPageHost::ConfigPage>& pages) override;
		void OnEditorClosing(ConfigPageHost::ConfigFormApi& api) override;

		// Test seams: feeds a status reply (as the RPC would) to the editor's Calibration page that holds
		// `inPage`; the number of editors the provider currently serves.
		void ApplyStatusForTesting(QWidget* inPage, const QString& text, const QString& error, const QString& command);
		int StateCountForTesting() const;

	private:
		struct State;
		QWidget* Build(State& st);
		void Run(State& st, const QString& command, const QString& arguments);
		void Apply(State& st, const QString& text, const QString& error, const QString& command);

		std::map<ConfigPageHost::ConfigFormApi*, std::unique_ptr<State>> states_;
	};

	// The process-wide instance that VpvConfigProviders.h registers.
	CalibrationPage& CalibrationPageInstance();
}
