#pragma once
/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// The DV comparison capture (DvCompareCapture.h, feat/dv-l8) as one of the renderer's
// rendered-frame captures (VP diagnostics adapter, WP-17). The renderer holds one
// VpDiagnostics::RenderedFrameCaptures, built by RendererCaptures() at construction, and calls it
// at the frame-rendered and capture-requested hook points; this wrapper forwards to the recorder
// unchanged (same calls, same order, same files).
#include <VP-V/modules/control/diagnostics/DiagnosticsAdapter.h>
#include <VP-V/modules/input/signal/DvCompareCapture.h>

#include <memory>
#include <string>

namespace DvCompareCapture
{
	constexpr const char* kCommand = "dv_compare_capture";

	class Sink : public VpDiagnostics::RenderedFrameCapture
	{
	public:
		explicit Sink(const Options& options) : recorder(options) {}

		const char* Command() const override { return kCommand; }
		void Configure(const std::string& folder) override { recorder.Configure(folder); }
		bool Enabled() const override { return recorder.Enabled(); }
		void OnFrameRendered(const VpDiagnostics::RenderedFrameContext& f) override
		{
			if (f.state)
				recorder.OnRender(*f.state, f.rawFrame, f.counter, f.dvMetadata, f.dvTrimsEnabled, f.dvTrims,
					f.targetNits);
		}
		std::string Toggle() override { return recorder.Toggle(); }

	private:
		Recorder recorder;
	};

	// The renderer's captures (registered before the render thread starts).
	inline VpDiagnostics::RenderedFrameCaptures RendererCaptures()
	{
		VpDiagnostics::RenderedFrameCaptures captures;
		captures.Register(std::unique_ptr<VpDiagnostics::RenderedFrameCapture>(new Sink(Options::FromEnvironment())));
		return captures;
	}
}
