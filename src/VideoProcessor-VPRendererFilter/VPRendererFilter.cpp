#include <pch.h>

#include "VPRendererFilter.h"

#include "VPRendererFilterLog.h"
#include "VPRendererFilterRegistration.h"
#include "VPRendererFilterRepack.h"
#include "VPRendererPlugin.h"

#include <DebugLog.h>
#include <VideoFrame.h>
#include <VideoConversionOverride.h>

#include <exception>
#include <new>


namespace VPRendererFilter
{
	namespace
	{
		// Rejections and per-frame failures are logged this many times each.
		constexpr int kLogLimit = 32;

		bool ShouldLog(std::atomic<int>& counter)
		{
			return counter.fetch_add(1, std::memory_order_relaxed) < kLogLimit;
		}

		std::atomic<int> g_rejectLogs{ 0 };
		std::atomic<int> g_frameErrorLogs{ 0 };

		const char* RendererStateName(RendererState state)
		{
			switch (state)
			{
			case RENDERSTATE_READY: return "ready";
			case RENDERSTATE_RENDERING: return "rendering";
			case RENDERSTATE_STOPPED: return "stopped";
			case RENDERSTATE_UNKNOWN: return "unknown";
			case RENDERSTATE_STOPPING: return "stopping";
			case RENDERSTATE_STARTING: return "starting";
			case RENDERSTATE_FAILED: return "failed";
			}
			return "?";
		}

		int64_t QpcNow()
		{
			LARGE_INTEGER now = {};
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}
	}


	//
	// Input pin
	//

	CVPRendererInputPin::CVPRendererInputPin(CVPRendererFilter* filter, HRESULT* phr) :
		CRendererInputPin(filter, phr, L"In")
	{
	}

	HRESULT CVPRendererInputPin::GetMediaType(int position, CMediaType* mediaType)
	{
		return static_cast<CVPRendererFilter*>(m_pRenderer)->GetProposedMediaType(
			position, mediaType);
	}


	//
	// Construction
	//

	CUnknown* WINAPI CVPRendererFilter::CreateInstance(LPUNKNOWN outer, HRESULT* phr)
	{
		try
		{
			CVPRendererFilter* filter = new CVPRendererFilter(outer, phr);
			return filter;
		}
		catch (const std::exception& e)
		{
			DebugLog::Log("VP Renderer filter construction failed: %s", e.what());
		}
		catch (...)
		{
			DebugLog::Log("VP Renderer filter construction failed");
		}
		if (phr)
			*phr = E_OUTOFMEMORY;
		return nullptr;
	}


	CVPRendererFilter::CVPRendererFilter(LPUNKNOWN outer, HRESULT* phr) :
		CBaseRenderer(kClsidVPRendererFilter, NAME("VP Renderer"), outer, phr),
		m_formatAdapter(CreateLocalFormatAdapter()),
		m_clockAdapter(CreateLocalClockAdapter()),
		m_stepRelease(TRUE)
	{
		VPRendererFilterLog::Initialize();
		DebugLog::Log("VP Renderer filter created: module=%ls log=%ls",
			FilterModulePath().c_str(), VPRendererFilterLog::ActivePath().c_str());
	}


	CVPRendererFilter::~CVPRendererFilter()
	{
		try
		{
			DestroyRenderer();
			m_window.Destroy();
		}
		catch (...)
		{
		}
		DebugLog::Log("VP Renderer filter destroyed");
	}


	STDMETHODIMP CVPRendererFilter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
	{
		CheckPointer(ppv, E_POINTER);
		if (riid == IID_IVideoWindow)
			return GetInterface(static_cast<IVideoWindow*>(this), ppv);
		if (riid == IID_IBasicVideo || riid == IID_IBasicVideo2)
			return GetInterface(static_cast<IBasicVideo2*>(this), ppv);
		if (riid == IID_IQualProp)
			return GetInterface(static_cast<IQualProp*>(this), ppv);
		if (riid == __uuidof(IVideoFrameStep))
			return GetInterface(static_cast<IVideoFrameStep*>(this), ppv);
		return CBaseRenderer::NonDelegatingQueryInterface(riid, ppv);
	}


	CBasePin* CVPRendererFilter::GetPin(int n)
	{
		CAutoLock lock(&m_ObjectCreationLock);
		if (n != 0)
			return nullptr;
		if (!m_pInputPin)
		{
			HRESULT hr = S_OK;
			CVPRendererInputPin* pin = new (std::nothrow) CVPRendererInputPin(this, &hr);
			if (!pin)
				return nullptr;
			if (FAILED(hr))
			{
				delete pin;
				return nullptr;
			}
			m_pInputPin = pin;
			m_vpInputPin = pin;
		}
		return m_pInputPin;
	}


	//
	// Negotiation
	//

	HRESULT CVPRendererFilter::GetProposedMediaType(int position, CMediaType* mediaType)
	{
		CheckPointer(mediaType, E_POINTER);
		if (position < 0)
			return E_INVALIDARG;
		const std::vector<SubtypeInfo>& subtypes = AcceptedSubtypes();
		if (static_cast<size_t>(position) >= subtypes.size())
			return VFW_S_NO_MORE_ITEMS;
		// Partially specified: the decoder fills in the format block.
		mediaType->InitMediaType();
		mediaType->SetType(&kMediaTypeVideo);
		mediaType->SetSubtype(&subtypes[static_cast<size_t>(position)].subtype);
		mediaType->SetFormatType(&GUID_NULL);
		return S_OK;
	}


	HRESULT CVPRendererFilter::CheckMediaType(const CMediaType* mediaType)
	{
		CheckPointer(mediaType, E_POINTER);
		VideoFormat format;
		std::string reason;
		if (!ParseVideoMediaType(*mediaType, format, reason))
		{
			if (ShouldLog(g_rejectLogs))
				DebugLog::Log("VP Renderer filter: media type rejected: %s", reason.c_str());
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		// Refuse every type (so the pin never connects) when the plugin is not
		// usable: missing, or not of ABI VP_LIBPLACEBO_PLUGIN_API_VERSION.
		const LoadedPlugin& plugin = GetVPRendererPlugin();
		if (!plugin.IsLoaded())
		{
			if (ShouldLog(g_rejectLogs))
				DebugLog::Log("VP Renderer filter: refusing connection: %s", plugin.failure.c_str());
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		return S_OK;
	}


	HRESULT CVPRendererFilter::SetMediaType(const CMediaType* mediaType)
	{
		CheckPointer(mediaType, E_POINTER);
		VideoFormat format;
		std::string reason;
		if (!ParseVideoMediaType(*mediaType, format, reason))
		{
			DebugLog::Log("VP Renderer filter: SetMediaType rejected: %s", reason.c_str());
			return VFW_E_TYPE_NOT_ACCEPTED;
		}
		{
			std::lock_guard<std::mutex> lock(m_formatMutex);
			m_format = format;
			m_hasFormat = true;
			++m_formatGeneration;
		}
		SubtypeInfo info;
		LookupSubtype(format.subtype, info);
		DebugLog::Log("VP Renderer filter: format %s %ux%u (coded %ux%u stride %u%s) "
			"origin (%u,%u) aspect %u:%u AvgTimePerFrame %lld %s colorimetry present=%d "
			"range=%u matrix=%u primaries=%u transfer=%u",
			info.name, format.width, format.height, format.codedWidth, format.codedHeight,
			format.sourceStrideBytes, format.bottomUp ? " bottom-up" : "",
			format.originX, format.originY, format.aspectX, format.aspectY,
			static_cast<long long>(format.avgTimePerFrame),
			format.hasVideoInfo2 ? "VIH2" : "VIH",
			format.colorimetry.present ? 1 : 0, format.colorimetry.nominalRange,
			format.colorimetry.transferMatrix, format.colorimetry.primaries,
			format.colorimetry.transferFunction);
		m_discontinuityPending = true;
		return CBaseRenderer::SetMediaType(mediaType);
	}


	HRESULT CVPRendererFilter::CompleteConnect(IPin* receivePin)
	{
		HRESULT hr = CBaseRenderer::CompleteConnect(receivePin);
		if (FAILED(hr))
			return hr;
		std::string reason;
		hr = CreateRenderer(reason);
		if (FAILED(hr))
		{
			DebugLog::Log("VP Renderer filter: connection refused: %s (hr=0x%08lX)",
				reason.c_str(), hr);
			return hr;
		}
		return S_OK;
	}


	HRESULT CVPRendererFilter::BreakConnect()
	{
		const HRESULT hr = CBaseRenderer::BreakConnect();
		if (FAILED(hr))
			return hr;
		DestroyRenderer();
		{
			std::lock_guard<std::mutex> lock(m_formatMutex);
			m_hasFormat = false;
		}
		return hr;
	}


	bool CVPRendererFilter::CurrentFormat(VideoFormat& format, uint64_t* generation) const
	{
		std::lock_guard<std::mutex> lock(m_formatMutex);
		if (!m_hasFormat)
			return false;
		format = m_format;
		if (generation)
			*generation = m_formatGeneration;
		return true;
	}


	//
	// Renderer lifetime
	//

	CVPRendererFilter::RendererPtr CVPRendererFilter::Renderer() const
	{
		std::lock_guard<std::mutex> lock(m_rendererMutex);
		return m_renderer;
	}


	HWND CVPRendererFilter::EnsureWindow()
	{
		if (!m_window.Hwnd())
		{
			VPRendererVideoWindow::Callbacks callbacks;
			callbacks.onSize = [this]() { OnWindowSize(); };
			callbacks.onPaint = [this]()
			{
				if (RendererPtr renderer = Renderer())
					renderer->OnPaint();
			};
			callbacks.onDisplayChange = [this]()
			{
				if (RendererPtr renderer = Renderer())
					renderer->OnDisplayChange();
			};
			if (!m_window.Create(callbacks))
				return nullptr;
		}
		return m_window.Hwnd();
	}


	void CVPRendererFilter::OnWindowSize()
	{
		{
			std::lock_guard<std::mutex> lock(m_windowStateMutex);
			if (m_defaultDestination)
				m_destinationRect = DefaultDestinationRect();
		}
		if (RendererPtr renderer = Renderer())
			renderer->OnSize();
	}


	HRESULT CVPRendererFilter::CreateRenderer(std::string& reason)
	{
		if (Renderer())
			return S_OK;
		const LoadedPlugin& plugin = GetVPRendererPlugin();
		if (!plugin.IsLoaded())
		{
			reason = plugin.failure;
			return VFW_E_NO_ACCEPTABLE_TYPES;
		}
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
		{
			reason = "the video window could not be created";
			return E_FAIL;
		}

		try
		{
			const uint32_t generation = ++m_rendererGeneration;
			// TODO(VP Renderer integration, agent D): renderer configuration path
			// (VP's renderer config for the filter), queue capacity and the
			// unified profile snapshot; the empty path means plugin defaults.
			IVideoRenderer* raw = plugin.createRenderer(static_cast<IRendererCallback*>(this),
				generation, hwnd, &m_timingClock, true, kDefaultFrameQueueCapacity,
				VideoConversionOverride::VIDEOCONVERSION_NONE, "", &VPRendererFilterLog::Sink);
			if (!raw)
			{
				reason = "the VP renderer plugin failed to create a renderer";
				return E_FAIL;
			}
			const VideoProcessorLibplaceboDestroyRendererFn destroy = plugin.destroyRenderer;
			RendererPtr renderer(raw, [destroy](IVideoRenderer* r)
			{
				try { r->Stop(); }
				catch (...) { DebugLog::Log("VP Renderer filter: renderer Stop threw"); }
				destroy(r);
			});

			if (!renderer->SupportsTimedFileFrames())
			{
				reason = "the VP renderer does not support timed file frames (ABI 35 renderer side missing)";
				return E_FAIL;
			}

			{
				std::lock_guard<std::mutex> lock(m_rendererMutex);
				m_renderer = renderer;
				m_videoStateSent = false;
			}

			// The executable's order: video state (when known), Build, Start. A
			// stream without AvgTimePerFrame sends its state on the first sample.
			std::string stateReason;
			if (!EnsureVideoState(nullptr, stateReason))
				DebugLog::Log("VP Renderer filter: video state deferred to the first sample: %s",
					stateReason.c_str());
			renderer->Build();
			renderer->Start();
			DebugLog::Log("VP Renderer filter: renderer generation %u started (plugin %ls)",
				generation, plugin.path.c_str());
			return S_OK;
		}
		catch (const std::exception& e)
		{
			reason = std::string("renderer start failed: ") + e.what();
		}
		catch (...)
		{
			reason = "renderer start failed";
		}
		DestroyRenderer();
		return E_FAIL;
	}


	void CVPRendererFilter::DestroyRenderer()
	{
		RendererPtr renderer;
		{
			std::lock_guard<std::mutex> lock(m_rendererMutex);
			renderer.swap(m_renderer);
			m_videoStateSent = false;
		}
		if (!renderer)
			return;
		try
		{
			renderer->CancelFileFrameWait();
		}
		catch (...)
		{
		}
		renderer.reset();   // Stop + destroy (unless the window thread still holds it)
		DebugLog::Log("VP Renderer filter: renderer released");
	}


	bool CVPRendererFilter::EnsureVideoState(IMediaSample* sample, std::string& reason)
	{
		RendererPtr renderer = Renderer();
		if (!renderer)
		{
			reason = "no renderer";
			return false;
		}
		VideoFormat format;
		uint64_t generation = 0;
		if (!CurrentFormat(format, &generation))
		{
			reason = "no media type";
			return false;
		}
		if (m_videoStateSent && generation == m_sentFormatGeneration)
			return true;

		int64_t duration = format.avgTimePerFrame;
		if (duration <= 0 && sample)
		{
			REFERENCE_TIME start = 0, stop = 0;
			if (SUCCEEDED(sample->GetTime(&start, &stop)) && stop > start)
				duration = stop - start;
		}

		VideoStateComPtr state;
		RationalRate rate;
		if (!m_formatAdapter->BuildVideoState(format, duration, state, rate, reason))
			return false;
		if (!renderer->OnVideoState(state))
		{
			// TODO(VP Renderer integration, agent A): a renderer that cannot take
			// the change in place needs a rebuild; the filter only logs it now.
			reason = "the renderer rejected the video state";
			return false;
		}
		m_sourceRate = rate;
		m_videoStateSent = true;
		m_sentFormatGeneration = generation;
		m_discontinuityPending = true;
		DebugLog::Log("VP Renderer filter: video state sent: %ux%u %ls rate %u/%u",
			format.width, format.height, ::ToString(format.encoding),
			rate.numerator, rate.denominator);
		return true;
	}


	//
	// Streaming
	//

	HRESULT CVPRendererFilter::ShouldDrawSampleNow(IMediaSample*, REFERENCE_TIME*,
		REFERENCE_TIME*)
	{
		// VP schedules by FileFrameTiming::presentationQpc; hand over at once.
		return S_OK;
	}


	void CVPRendererFilter::CancelQueueWait(const RendererPtr& renderer)
	{
		m_waitCancelled = true;
		if (renderer)
		{
			try { renderer->CancelFileFrameWait(); }
			catch (...) {}
		}
	}


	bool CVPRendererFilter::WaitForQueueSlot(const RendererPtr& renderer)
	{
		for (;;)
		{
			if (m_pInputPin->IsFlushing() || m_State == State_Stopped)
				return false;
			if (renderer->WaitForFileFrameSlot(kQueueWaitSliceMs))
				return true;
			if (m_waitCancelled.load())
			{
				// CancelFileFrameWait holds until the next timed frame. When the
				// stream is live again (after a flush the queue was reset) the
				// sample goes through; otherwise it is abandoned.
				return !m_pInputPin->IsFlushing() && m_State != State_Stopped;
			}
		}
	}


	HRESULT CVPRendererFilter::Receive(IMediaSample* sample)
	{
		// A completed frame step holds the stream until the graph pauses.
		while (m_stepHold.load() && m_State == State_Running && !m_pInputPin->IsFlushing())
			m_stepRelease.Wait(kQueueWaitSliceMs);

		// Never drop: wait (outside every filter lock) until VP has room.
		if (RendererPtr renderer = Renderer())
		{
			try
			{
				if (m_State != State_Stopped && !WaitForQueueSlot(renderer))
					return S_FALSE;
			}
			catch (const std::exception& e)
			{
				if (ShouldLog(g_frameErrorLogs))
					DebugLog::Log("VP Renderer filter: queue wait threw: %s", e.what());
			}
		}
		return CBaseRenderer::Receive(sample);
	}


	void CVPRendererFilter::OnReceiveFirstSample(IMediaSample* sample)
	{
		// Paused: show the pre-roll picture after a stop, seek or flush and
		// hold it. A pause from running keeps its sample for Run instead.
		if (!m_awaitingPreroll.load())
			return;
		REFERENCE_TIME start = 0, stop = 0;
		sample->GetTime(&start, &stop);
		if (SUCCEEDED(DeliverSample(sample, true)))
		{
			m_prerollSample = sample;
			m_prerollStart = start;
		}
	}


	HRESULT CVPRendererFilter::DoRenderSample(IMediaSample* sample)
	{
		if (m_prerollSample)
		{
			REFERENCE_TIME start = 0, stop = 0;
			sample->GetTime(&start, &stop);
			const bool alreadyShown = sample == m_prerollSample && start == m_prerollStart;
			m_prerollSample = nullptr;
			if (alreadyShown)
				return S_OK;
		}
		return DeliverSample(sample, false);
	}


	HRESULT CVPRendererFilter::DeliverSample(IMediaSample* sample, bool pausedHold)
	{
		try
		{
			RendererPtr renderer = Renderer();
			if (!renderer)
				return E_UNEXPECTED;
			std::string reason;
			if (!EnsureVideoState(sample, reason))
			{
				if (ShouldLog(g_frameErrorLogs))
					DebugLog::Log("VP Renderer filter: frame not delivered: %s", reason.c_str());
				return E_FAIL;
			}
			VideoFormat format;
			if (!CurrentFormat(format))
				return E_UNEXPECTED;

			BYTE* data = nullptr;
			if (FAILED(sample->GetPointer(&data)) || !data)
				return E_POINTER;
			const long length = sample->GetActualDataLength();

			// System-memory copy into the tight layout VP expects; the copy
			// outlives the sample (VP queues it) through the buffer's refcount.
			FrameBufferPool::Buffer* buffer = m_bufferPool.Acquire(
				TightFrameBytes(format.encoding, format.width, format.height));
			RepackSource source;
			source.data = data;
			source.size = length > 0 ? static_cast<size_t>(length) : 0;
			source.strideBytes = format.sourceStrideBytes;
			source.codedHeight = format.codedHeight;
			source.bottomUp = format.bottomUp;
			source.originX = format.originX;
			source.originY = format.originY;
			if (!RepackFrame(format.encoding, format.width, format.height, source,
				buffer->Data(), buffer->Size(), &reason))
			{
				buffer->Release();
				if (ShouldLog(g_frameErrorLogs))
					DebugLog::Log("VP Renderer filter: frame repack failed: %s (sample %ld bytes)",
						reason.c_str(), length);
				return E_FAIL;
			}

			const long stepping = m_stepRemaining.load();
			REFERENCE_TIME start = 0, stop = 0;
			const bool hasTime = SUCCEEDED(sample->GetTime(&start, &stop));

			FileFrameTiming timing;
			const bool immediate = pausedHold || stepping > 0 || !hasTime ||
				m_State != State_Running;
			timing.presentationQpc = immediate ? 0 :
				m_clockAdapter->PresentationQpc(m_pClock, m_tStart, start);
			timing.sourceRateNumerator = m_sourceRate.numerator;
			timing.sourceRateDenominator = m_sourceRate.denominator;
			timing.sourceRateHz = m_sourceRate.denominator ?
				static_cast<double>(m_sourceRate.numerator) / m_sourceRate.denominator : 0.0;
			timing.playbackRate = m_pInputPin->CurrentRate();
			timing.sampleStart100ns = hasTime ? start : 0;
			timing.sampleStop100ns = hasTime ? stop : 0;
			timing.discontinuity = m_discontinuityPending.exchange(false) ||
				sample->IsDiscontinuity() == S_OK || stepping > 0;
			timing.isFileSource = true;
			timing.paused = pausedHold || stepping > 0;

			const int64_t now = m_timingClock.TimingClockNow();
			VideoFrame frame(buffer->Data(), ++m_frameCounter, now, buffer);
			renderer->OnTimedVideoFrame(frame, timing);
			buffer->Release();   // VP holds its own reference while queued

			m_waitCancelled = false;
			m_awaitingPreroll = false;
			const int64_t last = m_lastDeliveryQpc.exchange(now);
			if (last > 0 && !timing.paused)
			{
				const int64_t interval = now - last;
				const int64_t average = m_averageIntervalQpc.load();
				m_averageIntervalQpc = average > 0 ? (average * 15 + interval) / 16 : interval;
			}
			++m_framesDelivered;

			if (stepping > 0 && --m_stepRemaining == 0)
			{
				m_stepHold = true;
				m_stepRelease.Reset();
				NotifyEvent(EC_STEP_COMPLETE, FALSE, 0);
			}
			return S_OK;
		}
		catch (const std::exception& e)
		{
			if (ShouldLog(g_frameErrorLogs))
				DebugLog::Log("VP Renderer filter: frame delivery threw: %s", e.what());
		}
		catch (...)
		{
			if (ShouldLog(g_frameErrorLogs))
				DebugLog::Log("VP Renderer filter: frame delivery threw");
		}
		return E_FAIL;
	}


	HRESULT CVPRendererFilter::BeginFlush()
	{
		RendererPtr renderer = Renderer();
		CancelQueueWait(renderer);
		m_stepHold = false;
		m_stepRelease.Set();
		m_discontinuityPending = true;
		m_awaitingPreroll = true;
		m_prerollSample = nullptr;
		m_clockAdapter->Reset();
		if (renderer)
		{
			try { renderer->ResetLiveQueue(); }
			catch (...) { DebugLog::Log("VP Renderer filter: ResetLiveQueue threw"); }
		}
		return CBaseRenderer::BeginFlush();
	}


	HRESULT CVPRendererFilter::EndFlush()
	{
		return CBaseRenderer::EndFlush();
	}


	STDMETHODIMP CVPRendererFilter::Stop()
	{
		RendererPtr renderer = Renderer();
		CancelQueueWait(renderer);
		m_stepHold = false;
		m_stepRemaining = 0;
		m_stepRelease.Set();
		const HRESULT hr = CBaseRenderer::Stop();
		m_discontinuityPending = true;
		m_awaitingPreroll = true;
		m_prerollSample = nullptr;
		m_clockAdapter->Reset();
		if (renderer)
		{
			try { renderer->ResetLiveQueue(); }
			catch (...) { DebugLog::Log("VP Renderer filter: ResetLiveQueue threw"); }
		}
		return hr;
	}


	STDMETHODIMP CVPRendererFilter::Pause()
	{
		// A completed step is released by the pause the graph issues for it.
		m_stepHold = false;
		m_stepRelease.Set();
		// TODO(VP Renderer integration, agent A): a pause from running leaves
		// the frames VP already queued presenting on their QPC times; ABI 35 has
		// no hold/re-time call for queued file frames.
		return CBaseRenderer::Pause();
	}


	STDMETHODIMP CVPRendererFilter::Run(REFERENCE_TIME startTime)
	{
		return CBaseRenderer::Run(startTime);
	}


	//
	// IRendererCallback
	//

	void CVPRendererFilter::OnRendererState(RendererState state, uint32_t generation)
	{
		DebugLog::Log("VP Renderer filter: renderer generation %u state %s",
			generation, RendererStateName(state));
		if (state == RENDERSTATE_FAILED && generation == m_rendererGeneration)
			NotifyEvent(EC_ERRORABORT, E_FAIL, 0);
	}


	void CVPRendererFilter::OnRendererDetailString(const CString& details, uint32_t generation)
	{
		DebugLog::Log("VP Renderer filter: renderer generation %u: %ls",
			generation, details.GetString());
	}


	//
	// IDispatch (IVideoWindow / IBasicVideo2 are used through their vtables)
	//

	STDMETHODIMP CVPRendererFilter::GetTypeInfoCount(UINT* count)
	{
		CheckPointer(count, E_POINTER);
		*count = 0;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::GetTypeInfo(UINT, LCID, ITypeInfo** typeInfo)
	{
		CheckPointer(typeInfo, E_POINTER);
		*typeInfo = nullptr;
		return E_NOTIMPL;
	}

	STDMETHODIMP CVPRendererFilter::GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*)
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CVPRendererFilter::Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*,
		VARIANT*, EXCEPINFO*, UINT*)
	{
		return E_NOTIMPL;
	}


	//
	// IQualProp
	//

	STDMETHODIMP CVPRendererFilter::get_FramesDroppedInRenderer(int* frames)
	{
		CheckPointer(frames, E_POINTER);
		*frames = 0;
		if (RendererPtr renderer = Renderer())
		{
			try { *frames = static_cast<int>(renderer->DroppedFrameCount()); }
			catch (...) {}
		}
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_FramesDrawn(int* frames)
	{
		CheckPointer(frames, E_POINTER);
		*frames = m_framesDelivered.load();
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_AvgFrameRate(int* rateTimes100)
	{
		CheckPointer(rateTimes100, E_POINTER);
		const int64_t interval = m_averageIntervalQpc.load();
		const int64_t frequency = m_timingClock.TimingClockTicksPerSecond();
		*rateTimes100 = interval > 0 ? static_cast<int>((frequency * 100 + interval / 2) / interval) : 0;
		return S_OK;
	}

	// TODO(VP Renderer integration, agent A): presentation jitter and sync
	// offset from VP's presentation telemetry; the filter cannot see them.
	STDMETHODIMP CVPRendererFilter::get_Jitter(int* jitterMs)
	{
		CheckPointer(jitterMs, E_POINTER);
		*jitterMs = 0;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_AvgSyncOffset(int* offsetMs)
	{
		CheckPointer(offsetMs, E_POINTER);
		*offsetMs = 0;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_DevSyncOffset(int* deviationMs)
	{
		CheckPointer(deviationMs, E_POINTER);
		*deviationMs = 0;
		return S_OK;
	}


	//
	// IVideoFrameStep
	//

	STDMETHODIMP CVPRendererFilter::Step(DWORD frames, IUnknown*)
	{
		if (frames == 0)
			return E_INVALIDARG;
		m_stepHold = false;
		m_stepRelease.Set();
		m_stepRemaining = static_cast<long>(frames);
		m_discontinuityPending = true;   // a step is a discontinuity for VP
		DebugLog::Log("VP Renderer filter: frame step %lu", frames);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::CanStep(long, IUnknown*)
	{
		return m_pInputPin && m_pInputPin->IsConnected() ? S_OK : S_FALSE;
	}

	STDMETHODIMP CVPRendererFilter::CancelStep()
	{
		const bool wasStepping = m_stepRemaining.exchange(0) > 0;
		m_stepHold = false;
		m_stepRelease.Set();
		if (wasStepping)
			DebugLog::Log("VP Renderer filter: frame step cancelled");
		return S_OK;
	}
}
