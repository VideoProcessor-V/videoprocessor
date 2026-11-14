// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The compute-once detectors (2: unit call counters, 3: GPU pass attribution) made visible through the existing hub
// status / command / lifecycle seams (no renderer call site; iface-L-DET section 3):
//  - Fallbacks status: one line "compute-once: ..." while VP_DEBUG_COMPUTE_ONCE is on and a violation exists (reaches
//    Ctrl+I's fallbacks row, the Config status and the Ctrl+Alt+D bundle with the other fallback lines);
//  - OnCommand("compute_once", "report"): the full report text;
//  - the log: every report line at GpuReleased / Shutdown and every 30 s of rendering while on (the Ctrl+Alt+D
//    bundle's log excerpt carries it; the acceptance harness parses it).
// Off (the default) nothing is shown, nothing is logged and the AfterRender handler returns after one flag load.

#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/diagnostics/ComputeOnceReport.h>

#include <DebugLog.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace Vpv
{
	namespace Diagnostics
	{
		class ComputeOnceModule : public Hub::ISkeletonModule
		{
		public:
			const char* Name() const override { return "compute_once"; }

			void OnLifecycle(RendererExtension::LifecycleFacts& f) override
			{
				if (f.kind == RendererExtension::Lifecycle::GpuReleased || f.kind == RendererExtension::Lifecycle::Shutdown)
					LogReport("teardown");
			}

			bool OnFrame(RendererExtension::FrameContext& c) override
			{
				if (c.phase != RendererExtension::FramePhase::AfterRender || !ComputeOnce::Enabled())
					return false;
				const auto now = std::chrono::steady_clock::now();
				if (!started_)
				{
					started_ = true;
					last_ = now;
				}
				else if (now - last_ >= std::chrono::seconds(30))
				{
					last_ = now;
					LogReport("periodic");
				}
				return false;
			}

			void OnStatus(const RendererExtension::Context&, RendererExtension::StatusKind kind,
				std::vector<std::string>& lines, const char*, const char*) override
			{
				if (kind != RendererExtension::StatusKind::Fallbacks)
					return;
				const std::string line = ComputeOnceReport::StatusLine();
				if (!line.empty())
					lines.push_back(line);
			}

			bool OnCommand(const RendererExtension::Context&, const char* module, const char* command, std::string& reply) override
			{
				if (!module || std::string(module) != "compute_once" || !command || std::string(command) != "report")
					return false;
				reply.clear();
				for (const std::string& l : ComputeOnceReport::Lines())
					reply += l + "\n";
				if (reply.empty())
					reply = std::string("compute-once: off (set ") + ComputeOnce::kEnvName + "=1)\n";
				return true;
			}

		private:
			static void LogReport(const char* why)
			{
				if (!ComputeOnce::Enabled())
					return;
				DebugLog::Log("Compute-once report (%s)", why);
				for (const std::string& l : ComputeOnceReport::Lines())
					DebugLog::Log("%s", l.c_str());
			}

			bool started_ = false;
			std::chrono::steady_clock::time_point last_;
		};

		inline void RegisterComputeOnceModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			Hub::ExtensionHub::Instance().AddModule(std::unique_ptr<Hub::ISkeletonModule>(new ComputeOnceModule()));
		}
	}
}
