#pragma once

// VP Renderer: a DirectShow video renderer filter that hands every decoded
// frame to VideoProcessor's renderer plugin (VideoProcessorVPRenderer.dll, ABI
// 35) so MPC-HC/MPC-BE play local files through VP's full processing chain.
//
// Scheduling is VP's: the filter draws every sample immediately
// (ShouldDrawSampleNow -> S_OK) and stamps it with FileFrameTiming (QPC due
// time from the graph clock, exact source rate). Nothing is dropped here: the
// streaming thread waits in Receive until VP's queue has room.

#include <IRenderer.h>
#include <FileFrameTiming.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "FilterTimingGlue.h"
#include "VPRendererFilterMediaTypes.h"
#include "VPRendererFrameBufferPool.h"
#include "VPRendererVideoWindow.h"


namespace VPRendererFilter
{
	class CVPRendererFilter;

	// Frames VP may queue ahead of presentation.
	// TODO(VP Renderer integration, agent D): from VP's queue settings
	// (queue_size / the active queue profile) instead of this default.
	constexpr size_t kDefaultFrameQueueCapacity = 16;
	// Receive waits for a queue slot in slices of this length so a flush or
	// stop is noticed promptly.
	constexpr uint32_t kQueueWaitSliceMs = 20;

	// Input pin: CRendererInputPin plus GetMediaType (the proposal list).
	class CVPRendererInputPin final : public CRendererInputPin
	{
	public:
		CVPRendererInputPin(CVPRendererFilter* filter, HRESULT* phr);
		HRESULT GetMediaType(int position, CMediaType* mediaType) override;
	};


	class CVPRendererFilter final :
		public CBaseRenderer,
		public IVideoWindow,
		public IBasicVideo2,
		public IQualProp,
		public IVideoFrameStep,
		private IRendererCallback
	{
	public:
		static CUnknown* WINAPI CreateInstance(LPUNKNOWN outer, HRESULT* phr);

		CVPRendererFilter(LPUNKNOWN outer, HRESULT* phr);
		~CVPRendererFilter() override;

		DECLARE_IUNKNOWN
		STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

		// CBaseRenderer
		CBasePin* GetPin(int n) override;
		HRESULT CheckMediaType(const CMediaType* mediaType) override;
		HRESULT SetMediaType(const CMediaType* mediaType) override;
		HRESULT CompleteConnect(IPin* receivePin) override;
		HRESULT BreakConnect() override;
		HRESULT DoRenderSample(IMediaSample* sample) override;
		HRESULT ShouldDrawSampleNow(IMediaSample* sample, REFERENCE_TIME* start,
			REFERENCE_TIME* stop) override;
		void OnReceiveFirstSample(IMediaSample* sample) override;
		HRESULT Receive(IMediaSample* sample) override;
		HRESULT BeginFlush() override;
		HRESULT EndFlush() override;
		STDMETHODIMP Stop() override;
		STDMETHODIMP Pause() override;
		STDMETHODIMP Run(REFERENCE_TIME startTime) override;

		// Proposal order for the pin (P010, P016, NV12, v210, RGB32).
		HRESULT GetProposedMediaType(int position, CMediaType* mediaType);

		// IDispatch (shared by IVideoWindow and IBasicVideo2; not scriptable)
		STDMETHODIMP GetTypeInfoCount(UINT* count) override;
		STDMETHODIMP GetTypeInfo(UINT index, LCID lcid, ITypeInfo** typeInfo) override;
		STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR* names, UINT count, LCID lcid,
			DISPID* ids) override;
		STDMETHODIMP Invoke(DISPID id, REFIID riid, LCID lcid, WORD flags,
			DISPPARAMS* params, VARIANT* result, EXCEPINFO* exception, UINT* argError) override;

		// IVideoWindow
		STDMETHODIMP put_Caption(BSTR caption) override;
		STDMETHODIMP get_Caption(BSTR* caption) override;
		STDMETHODIMP put_WindowStyle(long style) override;
		STDMETHODIMP get_WindowStyle(long* style) override;
		STDMETHODIMP put_WindowStyleEx(long style) override;
		STDMETHODIMP get_WindowStyleEx(long* style) override;
		STDMETHODIMP put_AutoShow(long autoShow) override;
		STDMETHODIMP get_AutoShow(long* autoShow) override;
		STDMETHODIMP put_WindowState(long state) override;
		STDMETHODIMP get_WindowState(long* state) override;
		STDMETHODIMP put_BackgroundPalette(long palette) override;
		STDMETHODIMP get_BackgroundPalette(long* palette) override;
		STDMETHODIMP put_Visible(long visible) override;
		STDMETHODIMP get_Visible(long* visible) override;
		STDMETHODIMP put_Left(long left) override;
		STDMETHODIMP get_Left(long* left) override;
		STDMETHODIMP put_Width(long width) override;
		STDMETHODIMP get_Width(long* width) override;
		STDMETHODIMP put_Top(long top) override;
		STDMETHODIMP get_Top(long* top) override;
		STDMETHODIMP put_Height(long height) override;
		STDMETHODIMP get_Height(long* height) override;
		STDMETHODIMP put_Owner(OAHWND owner) override;
		STDMETHODIMP get_Owner(OAHWND* owner) override;
		STDMETHODIMP put_MessageDrain(OAHWND drain) override;
		STDMETHODIMP get_MessageDrain(OAHWND* drain) override;
		STDMETHODIMP get_BorderColor(long* color) override;
		STDMETHODIMP put_BorderColor(long color) override;
		STDMETHODIMP get_FullScreenMode(long* fullScreen) override;
		STDMETHODIMP put_FullScreenMode(long fullScreen) override;
		STDMETHODIMP SetWindowForeground(long focus) override;
		STDMETHODIMP NotifyOwnerMessage(OAHWND hwnd, long message, LONG_PTR wParam,
			LONG_PTR lParam) override;
		STDMETHODIMP SetWindowPosition(long left, long top, long width, long height) override;
		STDMETHODIMP GetWindowPosition(long* left, long* top, long* width, long* height) override;
		STDMETHODIMP GetMinIdealImageSize(long* width, long* height) override;
		STDMETHODIMP GetMaxIdealImageSize(long* width, long* height) override;
		STDMETHODIMP GetRestorePosition(long* left, long* top, long* width, long* height) override;
		STDMETHODIMP HideCursor(long hide) override;
		STDMETHODIMP IsCursorHidden(long* hidden) override;

		// IBasicVideo / IBasicVideo2
		STDMETHODIMP get_AvgTimePerFrame(REFTIME* time) override;
		STDMETHODIMP get_BitRate(long* rate) override;
		STDMETHODIMP get_BitErrorRate(long* rate) override;
		STDMETHODIMP get_VideoWidth(long* width) override;
		STDMETHODIMP get_VideoHeight(long* height) override;
		STDMETHODIMP put_SourceLeft(long value) override;
		STDMETHODIMP get_SourceLeft(long* value) override;
		STDMETHODIMP put_SourceWidth(long value) override;
		STDMETHODIMP get_SourceWidth(long* value) override;
		STDMETHODIMP put_SourceTop(long value) override;
		STDMETHODIMP get_SourceTop(long* value) override;
		STDMETHODIMP put_SourceHeight(long value) override;
		STDMETHODIMP get_SourceHeight(long* value) override;
		STDMETHODIMP put_DestinationLeft(long value) override;
		STDMETHODIMP get_DestinationLeft(long* value) override;
		STDMETHODIMP put_DestinationWidth(long value) override;
		STDMETHODIMP get_DestinationWidth(long* value) override;
		STDMETHODIMP put_DestinationTop(long value) override;
		STDMETHODIMP get_DestinationTop(long* value) override;
		STDMETHODIMP put_DestinationHeight(long value) override;
		STDMETHODIMP get_DestinationHeight(long* value) override;
		STDMETHODIMP SetSourcePosition(long left, long top, long width, long height) override;
		STDMETHODIMP GetSourcePosition(long* left, long* top, long* width, long* height) override;
		STDMETHODIMP SetDefaultSourcePosition() override;
		STDMETHODIMP SetDestinationPosition(long left, long top, long width, long height) override;
		STDMETHODIMP GetDestinationPosition(long* left, long* top, long* width, long* height) override;
		STDMETHODIMP SetDefaultDestinationPosition() override;
		STDMETHODIMP GetVideoSize(long* width, long* height) override;
		STDMETHODIMP GetVideoPaletteEntries(long start, long count, long* retrieved,
			long* palette) override;
		STDMETHODIMP GetCurrentImage(long* bufferSize, long* image) override;
		STDMETHODIMP IsUsingDefaultSource() override;
		STDMETHODIMP IsUsingDefaultDestination() override;
		STDMETHODIMP GetPreferredAspectRatio(long* aspectX, long* aspectY) override;

		// IQualProp
		STDMETHODIMP get_FramesDroppedInRenderer(int* frames) override;
		STDMETHODIMP get_FramesDrawn(int* frames) override;
		STDMETHODIMP get_AvgFrameRate(int* rateTimes100) override;
		STDMETHODIMP get_Jitter(int* jitterMs) override;
		STDMETHODIMP get_AvgSyncOffset(int* offsetMs) override;
		STDMETHODIMP get_DevSyncOffset(int* deviationMs) override;

		// IVideoFrameStep
		STDMETHODIMP Step(DWORD frames, IUnknown* stepObject) override;
		STDMETHODIMP CanStep(long multiple, IUnknown* stepObject) override;
		STDMETHODIMP CancelStep() override;

	private:
		// IRendererCallback (VP renderer -> filter; any thread)
		void OnRendererState(RendererState state, uint32_t generation) override;
		void OnRendererDetailString(const CString& details, uint32_t generation) override;

		using RendererPtr = std::shared_ptr<IVideoRenderer>;
		RendererPtr Renderer() const;
		HRESULT CreateRenderer(std::string& reason);
		void DestroyRenderer();
		bool EnsureVideoState(IMediaSample* sample, std::string& reason);
		HRESULT DeliverSample(IMediaSample* sample, bool pausedHold);
		bool WaitForQueueSlot(const RendererPtr& renderer);
		void CancelQueueWait(const RendererPtr& renderer);
		bool CurrentFormat(VideoFormat& format, uint64_t* generation = nullptr) const;
		HWND EnsureWindow();
		RECT DefaultSourceRect() const;
		RECT DefaultDestinationRect() const;
		void OnWindowSize();

		CVPRendererInputPin* m_vpInputPin = nullptr;

		// Format (SetMediaType / dynamic changes on a sample).
		mutable std::mutex m_formatMutex;
		VideoFormat m_format;
		bool m_hasFormat = false;
		uint64_t m_formatGeneration = 0;      // bumped by every SetMediaType

		// Renderer.
		mutable std::mutex m_rendererMutex;
		RendererPtr m_renderer;
		uint32_t m_rendererGeneration = 0;
		uint64_t m_sentFormatGeneration = 0;   // format of the last OnVideoState
		bool m_videoStateSent = false;
		RationalRate m_sourceRate;
		QpcTimingClock m_timingClock;
		std::unique_ptr<IFilterFormatAdapter> m_formatAdapter;
		std::unique_ptr<IFilterClockAdapter> m_clockAdapter;
		FrameBufferPool m_bufferPool;
		VPRendererVideoWindow m_window;

		// Streaming state (streaming thread; flags read across threads).
		std::atomic<bool> m_discontinuityPending{ true };
		std::atomic<bool> m_awaitingPreroll{ true };
		std::atomic<bool> m_waitCancelled{ false };
		IMediaSample* m_prerollSample = nullptr;   // identity only, never dereferenced
		REFERENCE_TIME m_prerollStart = 0;
		uint64_t m_frameCounter = 0;

		// Frame step: frames left (0 = none), and a completed step holds the
		// streaming thread until the graph pauses.
		std::atomic<long> m_stepRemaining{ 0 };
		std::atomic<bool> m_stepHold{ false };
		CAMEvent m_stepRelease;

		// IQualProp.
		std::atomic<int> m_framesDelivered{ 0 };
		std::atomic<int64_t> m_lastDeliveryQpc{ 0 };
		std::atomic<int64_t> m_averageIntervalQpc{ 0 };

		// IVideoWindow / IBasicVideo state the window does not hold itself.
		mutable std::mutex m_windowStateMutex;
		RECT m_sourceRect = {};
		RECT m_destinationRect = {};
		bool m_defaultSource = true;
		bool m_defaultDestination = true;
		long m_autoShow = OATRUE;
		long m_borderColor = 0;
	};
}
