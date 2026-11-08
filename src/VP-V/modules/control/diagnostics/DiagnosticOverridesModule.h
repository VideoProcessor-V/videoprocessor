// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Plan 58 T1 / B5 (call-site group B): the diagnostic overrides (DiagnosticOverrides.h) made visible
// through the existing hub status hook, so the renderer needs no call site. While any test/debug
// switch is set in the process environment (outside a test harness) the Fallbacks status carries
// one line, "diagnostic override active: <names>" (names only, never values). That line reaches
// Ctrl+I (the fallbacks row), the Config status and Ctrl+Alt+D with the other fallback lines.
// Nothing is shown, and nothing runs per frame, with no override set.
//
// Threading: OnStatus runs on whichever thread asks for the status; the module is stateless apart
// from the (immutable) names provider.
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/diagnostics/DiagnosticOverrides.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Diagnostics
	{
		class DiagnosticOverridesModule : public Hub::ISkeletonModule
		{
		public:
			using NamesProvider = std::function<std::vector<std::string>()>;
			explicit DiagnosticOverridesModule(NamesProvider provider = [] { return VpDiagnostics::OverridesToShow(); })
				: provider_(std::move(provider)) {}
			const char* Name() const override { return "diagnostic_overrides"; }
			void OnStatus(const RendererExtension::Context&, RendererExtension::StatusKind kind,
				std::vector<std::string>& lines, const char*, const char*) override
			{
				if (kind != RendererExtension::StatusKind::Fallbacks)
					return;
				const std::string line = VpDiagnostics::StatusLine(provider_());
				if (!line.empty())
					lines.push_back(line);
			}
		private:
			NamesProvider provider_;
		};

		inline void RegisterDiagnosticOverridesModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new DiagnosticOverridesModule()));
		}
	}
}
