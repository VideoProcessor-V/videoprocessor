// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a: the renderer's side of IVectorEngineHost, as a header-only adapter template so the renderer's file
// carries no adapter code: the renderer's Impl is the template argument and is complete wherever the adapter
// is made (inside an Impl member function). The adapter forwards to Impl members by name and holds nothing.
// C++14.
#pragma once

#include <VP-V/modules/motion/estimation/VectorEngine.h>
#include <VP-V/modules/motion/estimation/EstimateState.h>   // P5a-5: the estimate state the renderer holds (item a)
#include <VP-V/modules/motion/estimation/MotionRecordPublish.h>   // W1-C4: the pair sample + the one statistics feed (estimate path)

namespace VpMotion
{
	template<class Impl>
	class VectorEngineImplHost : public IVectorEngineHost
	{
	public:
		explicit VectorEngineImplHost(Impl& impl) : impl_(impl) {}
		void SyncFallback(const FallbackCatalog::Decision& decision) override { impl_.SyncFallback(decision); }
		void PublishStatus(bool force) override { impl_.PublishDejudderStatus(force); }
		bool DitherVectorsSupported() const override { return impl_.DitherVectorsSupported(); }
		void NoteSourceMonitorMotion(double sourcePixelsPerFrame, uint64_t sequence) override
		{
			impl_.sourceMonitor.SetMotion(sourcePixelsPerFrame, sequence);
		}
		bool IntentActive() const override { return impl_.intentInterpreter && impl_.intentInterpreter->Active(); }
		void NoteIntentVectors(const IntentInterpreter::VectorSample& sample) override { impl_.intentInterpreter->NoteVectors(sample); }

	private:
		Impl& impl_;
	};

	template<class Impl>
	VectorEngineImplHost<Impl> MakeVectorEngineHost(Impl& impl) { return VectorEngineImplHost<Impl>(impl); }

	// P5a-rest (items j, k): the engine's invalidation sink for the renderer (VectorEngine::InvalidateSink). The
	// host key IS the renderer's Impl (`vectorEngine{ this, &InvalidateHost<Impl> }`), so the request reaches the
	// Impl's own RequestMotionInvalidate (the validity epoch and the frame-analysis store), exactly as the
	// direct calls did. Any thread (as RequestMotionInvalidate).
	template<class Impl>
	void InvalidateHost(const void* host, MotionValidity::Cause cause)
	{
		static_cast<Impl*>(const_cast<void*>(host))->RequestMotionInvalidate(cause);
	}
}
