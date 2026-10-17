#include <pch.h>

// IVideoWindow and IBasicVideo2 on the VP Renderer filter. VP owns the actual
// drawing; these keep the player's view of the window and the video geometry.
// MPC needs both through the graph: a zero video size makes it treat the file
// as audio only.

#include "VPRendererFilter.h"

#include <DebugLog.h>


namespace VPRendererFilter
{
	namespace
	{
		// Styles a video window may not take (as the system renderers reject).
		constexpr long kForbiddenStyles = WS_DISABLED | WS_ICONIC | WS_MAXIMIZE |
			WS_MINIMIZE | WS_HSCROLL | WS_VSCROLL;

		// Window rectangle in its parent's client coordinates (screen when
		// top-level).
		RECT WindowRectInParent(HWND hwnd)
		{
			RECT rect = {};
			GetWindowRect(hwnd, &rect);
			const HWND parent = GetParent(hwnd);
			if (parent)
				MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
			return rect;
		}

		HRESULT MoveWindowTo(HWND hwnd, long left, long top, long width, long height)
		{
			if (width < 0 || height < 0)
				return E_INVALIDARG;
			return SetWindowPos(hwnd, nullptr, left, top, width, height,
				SWP_NOZORDER | SWP_NOACTIVATE) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
		}

		bool ValidRect(const RECT& r, long maxWidth, long maxHeight)
		{
			return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top &&
				(maxWidth <= 0 || r.right <= maxWidth) && (maxHeight <= 0 || r.bottom <= maxHeight);
		}
	}


	//
	// IVideoWindow
	//

	STDMETHODIMP CVPRendererFilter::put_Caption(BSTR caption)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		SetWindowTextW(hwnd, caption ? caption : L"");
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_Caption(BSTR* caption)
	{
		CheckPointer(caption, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		wchar_t text[256] = {};
		GetWindowTextW(hwnd, text, static_cast<int>(_countof(text)));
		*caption = SysAllocString(text);
		return *caption ? S_OK : E_OUTOFMEMORY;
	}

	STDMETHODIMP CVPRendererFilter::put_WindowStyle(long style)
	{
		if (style & kForbiddenStyles)
			return E_INVALIDARG;
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		SetWindowLongPtrW(hwnd, GWL_STYLE, style);
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
			SWP_NOACTIVATE | SWP_FRAMECHANGED);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_WindowStyle(long* style)
	{
		CheckPointer(style, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		*style = static_cast<long>(GetWindowLongPtrW(hwnd, GWL_STYLE));
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_WindowStyleEx(long style)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
			SWP_NOACTIVATE | SWP_FRAMECHANGED);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_WindowStyleEx(long* style)
	{
		CheckPointer(style, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		*style = static_cast<long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_AutoShow(long autoShow)
	{
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		m_autoShow = autoShow == OAFALSE ? OAFALSE : OATRUE;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_AutoShow(long* autoShow)
	{
		CheckPointer(autoShow, E_POINTER);
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		*autoShow = m_autoShow;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_WindowState(long state)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		ShowWindow(hwnd, state);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_WindowState(long* state)
	{
		CheckPointer(state, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		if (IsIconic(hwnd))
			*state = SW_MINIMIZE;
		else if (IsZoomed(hwnd))
			*state = SW_MAXIMIZE;
		else
			*state = IsWindowVisible(hwnd) ? SW_SHOW : SW_HIDE;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_BackgroundPalette(long)
	{
		return S_OK;   // VP never renders palettized output
	}

	STDMETHODIMP CVPRendererFilter::get_BackgroundPalette(long* palette)
	{
		CheckPointer(palette, E_POINTER);
		*palette = OAFALSE;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_Visible(long visible)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		ShowWindow(hwnd, visible == OAFALSE ? SW_HIDE : SW_SHOWNOACTIVATE);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_Visible(long* visible)
	{
		CheckPointer(visible, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		*visible = IsWindowVisible(hwnd) ? OATRUE : OAFALSE;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_Left(long left)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		return MoveWindowTo(hwnd, left, r.top, r.right - r.left, r.bottom - r.top);
	}

	STDMETHODIMP CVPRendererFilter::get_Left(long* left)
	{
		CheckPointer(left, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		*left = WindowRectInParent(hwnd).left;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_Width(long width)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		return MoveWindowTo(hwnd, r.left, r.top, width, r.bottom - r.top);
	}

	STDMETHODIMP CVPRendererFilter::get_Width(long* width)
	{
		CheckPointer(width, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		*width = r.right - r.left;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_Top(long top)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		return MoveWindowTo(hwnd, r.left, top, r.right - r.left, r.bottom - r.top);
	}

	STDMETHODIMP CVPRendererFilter::get_Top(long* top)
	{
		CheckPointer(top, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		*top = WindowRectInParent(hwnd).top;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_Height(long height)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		return MoveWindowTo(hwnd, r.left, r.top, r.right - r.left, height);
	}

	STDMETHODIMP CVPRendererFilter::get_Height(long* height)
	{
		CheckPointer(height, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		*height = r.bottom - r.top;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_Owner(OAHWND owner)
	{
		const HWND parent = reinterpret_cast<HWND>(owner);
		if (parent && !IsWindow(parent))
			return E_INVALIDARG;
		if (!EnsureWindow())
			return E_FAIL;
		m_window.SetOwner(parent);
		DebugLog::Log("VP Renderer filter: owner window %p", parent);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_Owner(OAHWND* owner)
	{
		CheckPointer(owner, E_POINTER);
		*owner = reinterpret_cast<OAHWND>(m_window.Owner());
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_MessageDrain(OAHWND drain)
	{
		m_window.SetMessageDrain(reinterpret_cast<HWND>(drain));
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_MessageDrain(OAHWND* drain)
	{
		CheckPointer(drain, E_POINTER);
		*drain = reinterpret_cast<OAHWND>(m_window.MessageDrain());
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_BorderColor(long* color)
	{
		CheckPointer(color, E_POINTER);
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		*color = m_borderColor;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::put_BorderColor(long color)
	{
		// TODO(VP Renderer): VP draws its own borders.
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		m_borderColor = color;
		return S_OK;
	}

	// Full-screen is the player's job (it resizes the owner window).
	STDMETHODIMP CVPRendererFilter::get_FullScreenMode(long*)
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CVPRendererFilter::put_FullScreenMode(long)
	{
		return E_NOTIMPL;
	}

	STDMETHODIMP CVPRendererFilter::SetWindowForeground(long)
	{
		// A child of the player's window has no foreground of its own.
		return EnsureWindow() ? S_OK : E_FAIL;
	}

	STDMETHODIMP CVPRendererFilter::NotifyOwnerMessage(OAHWND, long message, LONG_PTR,
		LONG_PTR)
	{
		if (message == WM_DISPLAYCHANGE)
		{
			if (RendererPtr renderer = Renderer())
			{
				try { renderer->OnDisplayChange(); }
				catch (...) {}
			}
		}
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::SetWindowPosition(long left, long top, long width,
		long height)
	{
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		return MoveWindowTo(hwnd, left, top, width, height);   // WM_SIZE -> OnSize
	}

	STDMETHODIMP CVPRendererFilter::GetWindowPosition(long* left, long* top, long* width,
		long* height)
	{
		CheckPointer(left, E_POINTER);
		CheckPointer(top, E_POINTER);
		CheckPointer(width, E_POINTER);
		CheckPointer(height, E_POINTER);
		const HWND hwnd = EnsureWindow();
		if (!hwnd)
			return E_FAIL;
		const RECT r = WindowRectInParent(hwnd);
		*left = r.left;
		*top = r.top;
		*width = r.right - r.left;
		*height = r.bottom - r.top;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::GetMinIdealImageSize(long* width, long* height)
	{
		// VP scales to any size; the native size is the only "ideal".
		return GetVideoSize(width, height);
	}

	STDMETHODIMP CVPRendererFilter::GetMaxIdealImageSize(long* width, long* height)
	{
		return GetVideoSize(width, height);
	}

	STDMETHODIMP CVPRendererFilter::GetRestorePosition(long* left, long* top, long* width,
		long* height)
	{
		return GetWindowPosition(left, top, width, height);
	}

	STDMETHODIMP CVPRendererFilter::HideCursor(long hide)
	{
		m_window.SetCursorHidden(hide != OAFALSE);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::IsCursorHidden(long* hidden)
	{
		CheckPointer(hidden, E_POINTER);
		*hidden = m_window.CursorHidden() ? OATRUE : OAFALSE;
		return S_OK;
	}


	//
	// IBasicVideo / IBasicVideo2
	//
	// TODO(VP Renderer): the source and destination
	// rectangles are recorded but VP does not crop/position by them yet (zoom and
	// pan in the player need a viewport call on the renderer).

	RECT CVPRendererFilter::DefaultSourceRect() const
	{
		VideoFormat format;
		RECT r = {};
		if (CurrentFormat(format))
		{
			r.right = static_cast<LONG>(format.width);
			r.bottom = static_cast<LONG>(format.height);
		}
		return r;
	}

	RECT CVPRendererFilter::DefaultDestinationRect() const
	{
		RECT r = {};
		const HWND hwnd = m_window.Hwnd();
		if (hwnd)
			GetClientRect(hwnd, &r);
		return r;
	}

	STDMETHODIMP CVPRendererFilter::get_AvgTimePerFrame(REFTIME* time)
	{
		CheckPointer(time, E_POINTER);
		VideoFormat format;
		if (!CurrentFormat(format))
			return VFW_E_NOT_CONNECTED;
		*time = static_cast<REFTIME>(format.avgTimePerFrame) / 10000000.0;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_BitRate(long* rate)
	{
		CheckPointer(rate, E_POINTER);
		VideoFormat format;
		if (!CurrentFormat(format))
			return VFW_E_NOT_CONNECTED;
		*rate = static_cast<long>(format.bitRate);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_BitErrorRate(long* rate)
	{
		CheckPointer(rate, E_POINTER);
		VideoFormat format;
		if (!CurrentFormat(format))
			return VFW_E_NOT_CONNECTED;
		*rate = static_cast<long>(format.bitErrorRate);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::get_VideoWidth(long* width)
	{
		CheckPointer(width, E_POINTER);
		long height = 0;
		return GetVideoSize(width, &height);
	}

	STDMETHODIMP CVPRendererFilter::get_VideoHeight(long* height)
	{
		CheckPointer(height, E_POINTER);
		long width = 0;
		return GetVideoSize(&width, height);
	}

	STDMETHODIMP CVPRendererFilter::GetVideoSize(long* width, long* height)
	{
		CheckPointer(width, E_POINTER);
		CheckPointer(height, E_POINTER);
		VideoFormat format;
		if (!CurrentFormat(format))
			return VFW_E_NOT_CONNECTED;
		*width = static_cast<long>(format.width);
		*height = static_cast<long>(format.height);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::GetPreferredAspectRatio(long* aspectX, long* aspectY)
	{
		CheckPointer(aspectX, E_POINTER);
		CheckPointer(aspectY, E_POINTER);
		VideoFormat format;
		if (!CurrentFormat(format))
			return VFW_E_NOT_CONNECTED;
		*aspectX = static_cast<long>(format.aspectX);
		*aspectY = static_cast<long>(format.aspectY);
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::SetSourcePosition(long left, long top, long width,
		long height)
	{
		VideoFormat format;
		if (!CurrentFormat(format))
			return VFW_E_NOT_CONNECTED;
		const RECT r = { left, top, left + width, top + height };
		if (!ValidRect(r, static_cast<long>(format.width), static_cast<long>(format.height)))
			return E_INVALIDARG;
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		m_sourceRect = r;
		m_defaultSource = false;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::GetSourcePosition(long* left, long* top, long* width,
		long* height)
	{
		CheckPointer(left, E_POINTER);
		CheckPointer(top, E_POINTER);
		CheckPointer(width, E_POINTER);
		CheckPointer(height, E_POINTER);
		RECT r;
		{
			std::lock_guard<std::mutex> lock(m_windowStateMutex);
			r = m_sourceRect;
			if (m_defaultSource)
				r = DefaultSourceRect();
		}
		*left = r.left;
		*top = r.top;
		*width = r.right - r.left;
		*height = r.bottom - r.top;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::SetDefaultSourcePosition()
	{
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		m_defaultSource = true;
		m_sourceRect = RECT();
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::SetDestinationPosition(long left, long top, long width,
		long height)
	{
		if (width <= 0 || height <= 0)
			return E_INVALIDARG;
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		m_destinationRect = { left, top, left + width, top + height };
		m_defaultDestination = false;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::GetDestinationPosition(long* left, long* top, long* width,
		long* height)
	{
		CheckPointer(left, E_POINTER);
		CheckPointer(top, E_POINTER);
		CheckPointer(width, E_POINTER);
		CheckPointer(height, E_POINTER);
		RECT r;
		{
			std::lock_guard<std::mutex> lock(m_windowStateMutex);
			r = m_defaultDestination ? DefaultDestinationRect() : m_destinationRect;
		}
		*left = r.left;
		*top = r.top;
		*width = r.right - r.left;
		*height = r.bottom - r.top;
		return S_OK;
	}

	STDMETHODIMP CVPRendererFilter::SetDefaultDestinationPosition()
	{
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		m_defaultDestination = true;
		m_destinationRect = RECT();
		return S_OK;
	}

	// The single-value accessors go through the rectangle calls above.
	STDMETHODIMP CVPRendererFilter::put_SourceLeft(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetSourcePosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetSourcePosition(value, t, w, h) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_SourceLeft(long* value)
	{
		CheckPointer(value, E_POINTER);
		long t, w, h;
		return GetSourcePosition(value, &t, &w, &h);
	}

	STDMETHODIMP CVPRendererFilter::put_SourceWidth(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetSourcePosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetSourcePosition(l, t, value, h) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_SourceWidth(long* value)
	{
		CheckPointer(value, E_POINTER);
		long l, t, h;
		return GetSourcePosition(&l, &t, value, &h);
	}

	STDMETHODIMP CVPRendererFilter::put_SourceTop(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetSourcePosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetSourcePosition(l, value, w, h) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_SourceTop(long* value)
	{
		CheckPointer(value, E_POINTER);
		long l, w, h;
		return GetSourcePosition(&l, value, &w, &h);
	}

	STDMETHODIMP CVPRendererFilter::put_SourceHeight(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetSourcePosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetSourcePosition(l, t, w, value) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_SourceHeight(long* value)
	{
		CheckPointer(value, E_POINTER);
		long l, t, w;
		return GetSourcePosition(&l, &t, &w, value);
	}

	STDMETHODIMP CVPRendererFilter::put_DestinationLeft(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetDestinationPosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetDestinationPosition(value, t, w, h) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_DestinationLeft(long* value)
	{
		CheckPointer(value, E_POINTER);
		long t, w, h;
		return GetDestinationPosition(value, &t, &w, &h);
	}

	STDMETHODIMP CVPRendererFilter::put_DestinationWidth(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetDestinationPosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetDestinationPosition(l, t, value, h) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_DestinationWidth(long* value)
	{
		CheckPointer(value, E_POINTER);
		long l, t, h;
		return GetDestinationPosition(&l, &t, value, &h);
	}

	STDMETHODIMP CVPRendererFilter::put_DestinationTop(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetDestinationPosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetDestinationPosition(l, value, w, h) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_DestinationTop(long* value)
	{
		CheckPointer(value, E_POINTER);
		long l, w, h;
		return GetDestinationPosition(&l, value, &w, &h);
	}

	STDMETHODIMP CVPRendererFilter::put_DestinationHeight(long value)
	{
		long l, t, w, h;
		HRESULT hr = GetDestinationPosition(&l, &t, &w, &h);
		return SUCCEEDED(hr) ? SetDestinationPosition(l, t, w, value) : hr;
	}

	STDMETHODIMP CVPRendererFilter::get_DestinationHeight(long* value)
	{
		CheckPointer(value, E_POINTER);
		long l, t, w;
		return GetDestinationPosition(&l, &t, &w, value);
	}

	STDMETHODIMP CVPRendererFilter::GetVideoPaletteEntries(long, long, long*, long*)
	{
		return VFW_E_NO_PALETTE_AVAILABLE;
	}

	STDMETHODIMP CVPRendererFilter::GetCurrentImage(long*, long*)
	{
		// TODO(VP Renderer): rendered-output capture
		// (RequestRenderedOutputCapture) could back this.
		return E_NOTIMPL;
	}

	STDMETHODIMP CVPRendererFilter::IsUsingDefaultSource()
	{
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		return m_defaultSource ? S_OK : S_FALSE;
	}

	STDMETHODIMP CVPRendererFilter::IsUsingDefaultDestination()
	{
		std::lock_guard<std::mutex> lock(m_windowStateMutex);
		return m_defaultDestination ? S_OK : S_FALSE;
	}
}
