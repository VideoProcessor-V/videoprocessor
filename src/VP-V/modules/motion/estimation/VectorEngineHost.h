// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// M2 P5a: the renderer's side of IVectorEngineHost, as a header-only adapter template so the renderer's file
// carries no adapter code: the renderer's Impl is the template argument and is complete wherever the adapter
// is made (inside an Impl member function). The adapter forwards to Impl members by name and holds nothing.
// C++14.
#pragma once

#include <VP-V/modules/motion/estimation/VectorEngine.h>

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
}
