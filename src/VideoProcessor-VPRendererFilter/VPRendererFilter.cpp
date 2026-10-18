#include <pch.h>

#include "VPRendererFilter.h"

#include "VPRendererFilterLog.h"
#include "VPRendererFilterRegistration.h"
#include "VPRendererFilterRepack.h"
#include "VPRendererPlugin.h"

#include <ConfigFile.h>
#include <DebugLog.h>
#include <IMediaSideData.h>
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

	STDMETHODIMP CVPRendererInputPin::NewSegment(REFERENCE_TIME start, REFERENCE_TIME stop,
		double rate)
	{
		const HRESULT hr = CRendererInputPin::NewSegment(start, stop, rate);
		if (SUCCEEDED(hr))
			static_cast<CVPRendererFilter*>(m_pRenderer)->OnNewSegment(start, stop, rate);
		return hr;
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
			if (m_host)
				m_host->Stop();
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
		// The next connection is a new stream: no HDR10 / DV carried over.
		m_sideData.Reset();
		m_loggedDolbyVision = false;
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
		const HRESULT hr = CreateRendererObject(reason);
		if (FAILED(hr))
			return hr;
		// The executable's order: video state (when known), Build, Start. A
		// stream without AvgTimePerFrame sends its state once the sample
		// timestamps give the rate.
		std::string stateReason;
		VideoFormat format;
		RationalRate initialRate;
		if (CurrentFormat(format))
			initialRate = filesource::SnapAvgTimePerFrame(format.avgTimePerFrame);
		m_creatingRenderer = true;   // a refusal here is not answered with a restart
		const bool sent = EnsureVideoState(initialRate, stateReason);
		m_creatingRenderer = false;
		if (!sent)
			DebugLog::Log("VP Renderer filter: video state deferred to the first samples: %s",
				stateReason.c_str());
		if (!StartRendererObject(reason))
		{
			DestroyRenderer();
			return E_FAIL;
		}
		return S_OK;
	}


	bool CVPRendererFilter::StartRendererObject(std::string& reason)
	{
		RendererPtr renderer = Renderer();
		if (!renderer)
		{
			reason = "no renderer";
			return false;
		}
		try
		{
			renderer->Build();
			renderer->Start();
			DebugLog::Log("VP Renderer filter: renderer generation %u started (plugin %ls, "
				"renderer config '%s')", m_rendererGeneration, GetVPRendererPlugin().path.c_str(),
				m_rendererConfigPath.c_str());
			// The profile snapshot (rules see source=file), applied the way the
			// executable applies it; OnHostApplied takes the queue size.
			if (m_host)
				m_host->SetRenderer(renderer.get());
			return true;
		}
		catch (const std::exception& e)
		{
			reason = std::string("renderer start failed: ") + e.what();
		}
		catch (...)
		{
			reason = "renderer start failed";
		}
		return false;
	}


	HRESULT CVPRendererFilter::CreateRendererObject(std::string& reason)
	{
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
			StartHost();
			// VideoProcessorRenderer.cfg beside the VideoProcessor.cfg FileHost
			// found (the executable loads it from its own folder; MPC's folder
			// means nothing here). Empty = the plugin's defaults.
			std::string rendererConfigPath;
			if (m_host)
			{
				const std::wstring mainPath = m_host->ConfigurationPath();
				const size_t slash = mainPath.find_last_of(L"\\/");
				if (slash != std::wstring::npos)
				{
					const std::wstring candidate = mainPath.substr(0, slash + 1) +
						std::wstring(CA2W(ConfigFile::RENDERER_FILENAME));
					if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
						rendererConfigPath = std::string(CW2A(candidate.c_str()));
				}
			}

			const uint32_t generation = ++m_rendererGeneration;
			IVideoRenderer* raw = plugin.createRenderer(static_cast<IRendererCallback*>(this),
				generation, hwnd, &m_timingClock, true, kDefaultFrameQueueCapacity,
				VideoConversionOverride::VIDEOCONVERSION_NONE, rendererConfigPath.c_str(),
				&VPRendererFilterLog::Sink);
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
				reason = "the VP renderer does not support timed file frames (ABI 37 renderer side missing)";
				return E_FAIL;
			}

			{
				std::lock_guard<std::mutex> lock(m_rendererMutex);
				m_renderer = renderer;
				m_stateTracker.Reset();
				m_appliedQueueSize = 0;
			}
			m_rendererConfigPath = rendererConfigPath;
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
			m_stateTracker.Reset();
		}
		if (!renderer)
			return;
		// Before the renderer goes: FileHost holds a raw pointer to it. With
		// source=file the renderer parks its LED drive on Stop for
		// file_led_holdover_ms so MPC's per-file graph rebuild does not flash
		// the projector to full light (LedDriveHoldover.h).
		if (m_host)
			m_host->SetRenderer(nullptr);
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


	bool CVPRendererFilter::StartHost()
	{
		if (m_hostStarted)
			return m_host != nullptr;
		m_hostStarted = true;
		try
		{
			// The FileHost code is linked into this module, so its module
			// directory is the .ax's folder.
			std::unique_ptr<FileSource::FileHost> host(new FileSource::FileHost());
			host->SetRendererName("VP Renderer (DirectShow filter)");
			host->SetApplyHandler([this](const FileSource::FileHost::ApplyOutcome& outcome)
			{
				OnHostApplied(outcome);
			});
			std::string error;
			if (!host->Start(error))
				DebugLog::Log("VP Renderer filter: configuration: %s (defaults in force; a later "
					"valid VideoProcessor.cfg is picked up)", error.c_str());
			else
				DebugLog::Log("VP Renderer filter: configuration %ls (file_av_offset_ms %d, "
					"file_led_holdover_ms %u)", host->ConfigurationPath().c_str(),
					host->AvOffsetMs(), host->LedHoldoverMs());
			m_timing.SetAvOffsetMs(host->AvOffsetMs());
			m_host = std::move(host);
			return true;
		}
		catch (const std::exception& e)
		{
			DebugLog::Log("VP Renderer filter: FileHost failed to start: %s", e.what());
		}
		return false;
	}


	void CVPRendererFilter::OnHostApplied(const FileSource::FileHost::ApplyOutcome& outcome)
	{
		DebugLog::Log("VP Renderer filter: profile %s%s%s%s%s", outcome.applied ? "applied" : "not applied",
			outcome.activeState.empty() ? "" : ": ", outcome.activeState.c_str(),
			outcome.reason.empty() ? "" : " - ", outcome.reason.c_str());
		if (!m_host)
			return;
		m_timing.SetAvOffsetMs(m_host->AvOffsetMs());
		RendererPtr renderer = Renderer();
		if (!renderer || !outcome.applied)
			return;
		// The queue profile's capacity, as VideoProcessor.exe applies it.
		if (const auto snapshot = m_host->Snapshot())
		{
			if (snapshot->queue.hasQueueSize && snapshot->queue.queueSize > 0 &&
				m_appliedQueueSize.exchange(snapshot->queue.queueSize) != snapshot->queue.queueSize)
			{
				try { renderer->SetFrameQueueMaxSize(snapshot->queue.queueSize); }
				catch (const std::exception& e)
				{
					DebugLog::Log("VP Renderer filter: queue size %zu refused: %s",
						static_cast<size_t>(snapshot->queue.queueSize), e.what());
				}
			}
		}
		if (outcome.rendererRestartRequired || outcome.liveResetRequired)
		{
			// A renderer restart takes effect with the next graph (MPC builds one
			// per file); a live reset only needs the queue re-primed.
			DebugLog::Log("VP Renderer filter: profile change needs %s",
				outcome.rendererRestartRequired ? "a renderer restart (next file)" : "a queue reset");
			if (outcome.liveResetRequired && !outcome.rendererRestartRequired)
			{
				try { renderer->ResetLiveQueue(); }
				catch (...) {}
				m_timing.MarkDiscontinuity();
			}
		}
	}


	void CVPRendererFilter::OnNewSegment(REFERENCE_TIME start, REFERENCE_TIME stop, double rate)
	{
		const bool rateChanged = m_timing.NewSegment(start, stop, rate);
		DebugLog::Log("VP Renderer filter: new segment %lld..%lld rate %.3f%s",
			static_cast<long long>(start), static_cast<long long>(stop), rate,
			rateChanged ? " (rate changed)" : "");
	}


	namespace
	{
		HRESULT GetSampleSideData(void* context, const GUID& type, const BYTE** data, size_t* size)
		{
			return static_cast<IMediaSideData*>(context)->GetSideData(type, data, size);
		}
	}


	void CVPRendererFilter::ReadSideData(IMediaSample* sample)
	{
		CComQIPtr<IMediaSideData> sideData(sample);
		if (!sideData)
			return;
		if (m_sideData.Update(&GetSampleSideData, sideData.p))
		{
			const HDRData& hdr = m_sideData.StaticHdr();
			DebugLog::Log("VP Renderer filter: HDR10 metadata: mastering %.4f-%.1f cd/m2, "
				"MaxCLL %.0f MaxFALL %.0f", hdr.masteringDisplayMinLuminance,
				hdr.masteringDisplayMaxLuminance, hdr.maxCll, hdr.maxFall);
		}
		if (m_sideData.Last().hasDolbyVision && !m_loggedDolbyVision)
		{
			m_loggedDolbyVision = true;
			// ABI 37 has no per-frame Dolby Vision entry point for file frames:
			// the base layer is shown with its static metadata (see the report).
			DebugLog::Log("VP Renderer filter: Dolby Vision metadata present (profile %u, "
				"extensions 0x%08X); not yet passed to the renderer, base layer shown",
				m_sideData.Last().dolbyVision.guessedProfile,
				m_sideData.Last().dolbyVision.extensionLevelMask);
		}
	}


	bool CVPRendererFilter::EnsureVideoState(const RationalRate& rate, std::string& reason)
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
		const bool hasHdr = m_sideData.HasStaticHdr();

		// Bug hunt 3 (C1): a refused state (new display mode, pixel format or
		// effective profile after a media-type change, a measured rate, a seek)
		// used to leave every later frame undelivered. The renderer is now
		// replaced and the state becomes the fresh renderer's first.
		class Sink final : public IFileVideoStateSink
		{
		public:
			explicit Sink(CVPRendererFilter& filterIn) : filter(filterIn) {}
			bool SendVideoState(VideoStateComPtr& state) override
			{
				RendererPtr current = filter.Renderer();
				if (!current || !current->OnVideoState(state))
					return false;
				sent = state;
				return true;
			}
			bool RestartRenderer(std::string& why) override
			{
				if (filter.m_creatingRenderer)
				{
					why = "the new renderer refused its first video state";
					return false;
				}
				DebugLog::Log("VP Renderer filter: the renderer cannot take the new video state in "
					"place; restarting it (generation %u)", filter.m_rendererGeneration);
				filter.DestroyRenderer();
				return SUCCEEDED(filter.CreateRendererObject(why));
			}
			void StartRestartedRenderer() override
			{
				std::string why;
				if (!filter.StartRendererObject(why))
				{
					DebugLog::Log("VP Renderer filter: restarted renderer did not start: %s", why.c_str());
					filter.DestroyRenderer();
				}
			}
			VideoStateComPtr sent;
		private:
			CVPRendererFilter& filter;
		};

		Sink sink(*this);
		const FileStateOutcome outcome = m_stateTracker.Ensure(format, generation, rate,
			hasHdr ? &m_sideData.StaticHdr() : nullptr, sink, reason);
		if (outcome == FileStateOutcome::Unchanged)
			return true;
		if (outcome == FileStateOutcome::Deferred || outcome == FileStateOutcome::Failed)
		{
			if (outcome == FileStateOutcome::Failed)
				DebugLog::Log("VP Renderer filter: video state %s: %s", ToString(outcome), reason.c_str());
			return false;
		}
		const FileColorDecision color = DecideColor(format, hasHdr);
		const RationalRate sentRate = m_stateTracker.SentRate();
		m_timing.MarkDiscontinuity();
		if (m_host && sink.sent)
			m_host->UpdateVideoState(sink.sent);
		DebugLog::Log("VP Renderer filter: video state %s: %ux%u %ls rate %u/%u eotf %ls%s "
			"colorspace %ls%s hdr10 %s", ToString(outcome), format.width, format.height,
			::ToString(format.encoding), sentRate.numerator, sentRate.denominator, ::ToString(color.eotf),
			color.eotfAssumed ? " (assumed)" : "", ::ToString(color.colorSpace),
			color.colorSpaceFromHeight ? " (from height)" : "", hasHdr ? "yes" : "no");
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

			// Side data first: a static HDR10 change goes out with the state.
			ReadSideData(sample);

			REFERENCE_TIME start = 0, stop = 0;
			const HRESULT timeResult = sample->GetTime(&start, &stop);
			const bool hasStart = SUCCEEDED(timeResult);
			const bool hasStop = timeResult == S_OK;   // VFW_S_NO_STOP_TIME: start only
			const bool sampleDiscontinuity = sample->IsDiscontinuity() == S_OK;
			VideoFormat currentFormat;
			const int64_t avgTimePerFrame = CurrentFormat(currentFormat) ?
				currentFormat.avgTimePerFrame : 0;
			// AvgTimePerFrame snapped to a rational, checked against the PTS deltas
			// (a header of 0 or one that disagrees: the measured rate). Held and
			// stepped pictures are not a cadence.
			const RationalRate rate = m_timing.ObserveSample(hasStart && !pausedHold,
				start, sampleDiscontinuity, avgTimePerFrame, m_stateTracker.SentRate());

			std::string reason;
			if (!EnsureVideoState(rate, reason))
			{
				if (ShouldLog(g_frameErrorLogs))
					DebugLog::Log("VP Renderer filter: frame not delivered: %s", reason.c_str());
				return E_FAIL;
			}
			// A refused state may have replaced the renderer: deliver to the
			// current one, once it renders (a frame offered before then is not
			// queued, and a paused pre-roll would stay black).
			{
				RendererPtr current = Renderer();
				if (!current)
					return E_UNEXPECTED;
				if (current != renderer)
				{
					renderer = current;
					// A cancel left from an earlier flush or stop belongs to the
					// old renderer; a new one re-cancels this wait.
					m_waitCancelled = false;
					if (!WaitForQueueSlot(renderer))
						return S_FALSE;
				}
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

			// Reference clock <-> QPC: one correlation per frame keeps the fit
			// current (FileClockMapper smooths and detects clock jumps).
			if (m_pClock)
			{
				filesource::ClockCorrelation correlation;
				if (filesource::SampleReferenceClockCorrelation(m_pClock, correlation))
					m_timing.AddCorrelation(correlation);
			}
			if (m_host)
				m_timing.SetAvOffsetMs(m_host->AvOffsetMs());
			if (stepping > 0)
				m_timing.FrameStep();
			if (m_discontinuityPending.exchange(false))
				m_timing.MarkDiscontinuity();

			filesource::FileSampleTimes times;
			times.hasStart = hasStart;
			times.start100ns = hasStart ? start : 0;
			times.hasStop = hasStop;
			times.stop100ns = hasStop ? stop : 0;
			times.discontinuityFlag = sampleDiscontinuity;
			// Present at tStart + rtStart (+ file_av_offset_ms) on the QPC clock
			// while running; held pictures (pre-roll, pause, step) at once.
			FileFrameTiming timing = m_timing.BuildTiming(times, m_stateTracker.SentRate());
			if (pausedHold || stepping > 0 || m_State != State_Running)
			{
				timing.presentationQpc = 0;
				timing.paused = true;
			}

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
		m_timing.BeginFlush();
		if (renderer)
		{
			try { renderer->ResetLiveQueue(); }
			catch (...) { DebugLog::Log("VP Renderer filter: ResetLiveQueue threw"); }
		}
		return CBaseRenderer::BeginFlush();
	}


	HRESULT CVPRendererFilter::EndFlush()
	{
		m_timing.EndFlush();
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
		m_timing.Stop();
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
		// Known limit (ABI 37): a pause from running leaves the frames VP has
		// already queued (at most the queue depth) presenting on their QPC
		// times before the picture holds; there is no re-time call for queued
		// file frames yet.
		m_timing.Pause();
		return CBaseRenderer::Pause();
	}


	STDMETHODIMP CVPRendererFilter::Run(REFERENCE_TIME startTime)
	{
		// The resume's new tStart re-maps the stream onto the clock.
		m_timing.Run(startTime);
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

	// TODO(VP Renderer): presentation jitter and sync
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
