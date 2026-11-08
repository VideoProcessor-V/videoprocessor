// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Plan 58 T1 / B4 (call-site group B): the source-resolution look-ahead walk, moved out of the
// renderer's render loop. The try-body is the old code verbatim (one tab less, `m_impl->` ->
// `impl.`, queue vector -> `view.frames`); the catch keeps the log line and the generic ReportWorker
// hook and now engages the visible FallbackCatalog::SourceResolutionWorker() entry (released after 8
// good batches).
//
// Threading: the render-loop thread, outside renderMutex (as before). The LookAheadFailures counter
// (Impl::sourceAheadFailures) is touched by that thread only; no atomics.
#include <exception>
#include <vector>

#include <AnalysisFrame.h>
#include <AnalysisLumaSource.h>
#include <DebugLog.h>
#include <VP-V/core/FallbackCatalog.h>
#include <vprenderer/RendererExtension.h>

namespace SourceResolution
{
	template <class Impl, class Queued>
	struct SourceAheadView
	{
		Impl& impl;
		std::vector<Queued>& frames;   // the frames the render loop selected (not released here)
	};

	template <class Impl, class Queued>
	void RunAheadWalk(SourceAheadView<Impl, Queued>& view)
	{
		Impl& impl = view.impl;
		try
		{
			for (Queued& ahead : view.frames)
			{
				const auto& aheadState = *ahead.state;
				if (!aheadState.displayMode) continue;
				const auto prepared = Impl::PrepareFrameAnalysis(ahead.frame, aheadState, ahead.converted, ahead.analysis,
					AnalysisSourceProfile::QueuedCapture, ahead.activePictureIdentity.sourceFormatGeneration, false,
					aheadState.dolbyVisionTunnel);
				const AnalysisLumaSource& luma = prepared.source;
				if (!luma.IsValid()) continue;
				impl.sourceMonitor.LookAheadFrame(ahead.sourceSequence, luma.width, luma.height,
					[&luma](int x, int y) {
						uint16_t code = 0;   // WP-23: the shared decode when ready
						return luma.SampleLuma(x, y, code) ? static_cast<int>(code) : 0;
					}, 0, 0, luma.width, luma.height);
			}
		}
		catch (const std::exception& e)
		{
			DebugLog::Log("Source resolution look-ahead skipped: %s", e.what());
			RendererExtension::ReportWorker(&impl, "source-look-ahead", e.what());
			impl.SyncFallback(impl.sourceAheadFailures.Note(true, e.what()));
			return;
		}
		impl.SyncFallback(impl.sourceAheadFailures.Note(false));
	}
}
