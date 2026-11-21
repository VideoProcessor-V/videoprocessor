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
#include <VP-V/modules/input/signal/DvTunnelTapLaw.h>
#include <VP-V/modules/input/source_analysis/CodedLumaSampler.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/core/ComputeOnce.h>
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
				// W21-2: a packed DV tunnel frame is looked at through its 12-bit tap (4095); every other source keeps the 10-bit read.
				// IsValid() (checked above) implies the 12-bit law (DvTunnelTapLaw.h invariant), so a DV frame is never refused here.
				if (luma.format == AnalysisLumaFormat::PackedDolbyVision422)
				{
					// Rule 1: the frame's ONE 12-bit decode (VpFrameAnalysis::Process at arrival, kept by ShareViewOf12) is read when it is
					// still held. Otherwise the walk reads its own sparse samples through the same law (SourceTap12): that read is counted
					// as a private site of the tap12 unit (ComputeOnce report), never uncounted. Resolved on the first sample only, so a
					// frame the monitor skips (already looked at) is neither looked up nor counted.
					const uint64_t sequence = ahead.sourceSequence;
					VpSkeleton::SourceFrameView shared;
					int how = 0;   // 0 = no sample read yet, 1 = the shared 12-bit decode, 2 = own samples (counted)
					SourceResolutionMonitor::CodedLumaSampler tap;
					tap.codeMaximum = static_cast<uint32_t>(DvTunnelTapLaw::kLevels - 1);
					const void* const host = &impl;   // the key Host::Owner(this) registered: this host's kept views only
					tap.read = [&luma, &shared, &how, sequence, host](int x, int y) {
						if (how == 0)
						{
							how = VpFrameAnalysis::FindSharedViewOf12(sequence, luma, shared, host) ? 1 : 2;
							if (how == 2)
								VPV_COMPUTE_ONCE_PRIVATE_AT("analysis.tap12", sequence, "source-look-ahead samples");
						}
						if (how == 1)
							return x >= 0 && y >= 0 && x < static_cast<int>(shared.width) && y < static_cast<int>(shared.height) ?
								static_cast<int>(shared.luma[static_cast<size_t>(y) * shared.strideSamples + static_cast<size_t>(x)]) : 0;
						uint16_t code12 = 0;
						return DvTunnelTapLaw::SourceTap12(luma, x, y, code12) ? static_cast<int>(code12) : 0;
					};
					impl.vpv.sourceAnalysis.sourceMonitor.LookAheadFrame(sequence, luma.width, luma.height, tap,
						0, 0, luma.width, luma.height);
					continue;
				}
				impl.vpv.sourceAnalysis.sourceMonitor.LookAheadFrame(ahead.sourceSequence, luma.width, luma.height,
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
