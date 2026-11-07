// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Dynamic Drive settings page of the Config editor (Dynamic Drive on/off, the command source, the
// LED floors and speeds, compensate's contrast assist and cuts, the LED profile file, the advanced
// LED settings): the [led_drive.default] settings, plus the projector's cards, which
// drive.projector_control owns (projector_control/config_ui/ProjectorCards). A Config page provider
// (ConfigPageHost.h): it registers the page as a sub-page of the master VP-V page, grouped under the
// Drive unit, and joins the RS-232 test thread when the editor closes. It reaches the editor only
// through ConfigPageHost::EditorKit.
//
// One instance serves every editor of the process; per-editor state is keyed by the editor's
// ConfigFormApi and dropped in OnEditorClosing. UI thread only.

#include <VideoProcessor-Config/ConfigPageHost.h>

#include <QThread>
#include <QWidget>

#include <map>
#include <memory>

namespace Vpv
{
	class LedDrivePage final : public ConfigPageHost::IConfigPageProvider
	{
	public:
		LedDrivePage();
		~LedDrivePage() override;

		void AppendPages(ConfigPageHost::ConfigFormApi& api, std::vector<ConfigPageHost::ConfigPage>& pages) override;
		void OnEditorClosing(ConfigPageHost::ConfigFormApi& api) override;

		// Test seams: the number of editors the provider currently serves; whether an editor's RS-232
		// connection test is running.
		int StateCountForTesting() const;
		bool ProjectorTestRunningForTesting(QWidget* editorWindow) const;
		// Makes the editor with this window believe an RS-232 test runs on `thread` (false: no such editor).
		bool AdoptProjectorTestThreadForTesting(QWidget* editorWindow, QThread* thread);

	private:
		struct State;
		std::map<ConfigPageHost::ConfigFormApi*, std::unique_ptr<State>> states_;
	};

	// The process-wide instance that VpvConfigProviders.h registers.
	LedDrivePage& LedDrivePageInstance();
}
