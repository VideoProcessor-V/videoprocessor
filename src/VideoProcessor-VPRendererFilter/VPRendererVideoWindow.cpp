#include <pch.h>

#include "VPRendererVideoWindow.h"
#include "VPRendererPlugin.h"

#include <DebugLog.h>

#include <windowsx.h>

#include <algorithm>


namespace VPRendererFilter
{
	namespace
	{
		const wchar_t* const kWindowClass = L"VPRendererFilterVideoWindow";
		constexpr UINT kMessageDestroy = WM_APP + 0x51;

		bool IsMouseMessage(UINT message)
		{
			return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
		}

		bool IsKeyboardMessage(UINT message)
		{
			return message >= WM_KEYFIRST && message <= WM_KEYLAST;
		}
	}


	VPRendererVideoWindow::~VPRendererVideoWindow()
	{
		Destroy();
	}


	bool VPRendererVideoWindow::Create(const Callbacks& callbacks)
	{
		if (Hwnd())
			return true;
		if (m_thread.joinable())
			return false;   // a previous thread failed to create its window

		m_callbacks = callbacks;
		{
			std::lock_guard<std::mutex> lock(m_startMutex);
			m_startDone = false;
		}
		m_thread = std::thread(&VPRendererVideoWindow::ThreadMain, this);
		std::unique_lock<std::mutex> lock(m_startMutex);
		m_started.wait(lock, [this]() { return m_startDone; });
		if (!Hwnd())
		{
			lock.unlock();
			m_thread.join();
			return false;
		}
		return true;
	}


	void VPRendererVideoWindow::Destroy()
	{
		const HWND hwnd = Hwnd();
		if (hwnd)
		{
			// Detach from the player's window on the calling thread (normally the
			// player's UI thread, which owns the parent) so the window thread's
			// teardown never has to send to a thread that is waiting for it.
			if (Owner())
				SetOwner(nullptr);
			PostMessageW(hwnd, kMessageDestroy, 0, 0);
		}
		if (m_thread.joinable())
			m_thread.join();
		m_hwnd.store(nullptr, std::memory_order_release);
	}


	void VPRendererVideoWindow::SetOwner(HWND owner)
	{
		m_owner.store(owner, std::memory_order_release);
		const HWND hwnd = Hwnd();
		if (!hwnd)
			return;

		LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
		if (owner)
		{
			// Clear WS_POPUP and set WS_CHILD before SetParent (SetParent docs).
			style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
			SetWindowLongPtrW(hwnd, GWL_STYLE, style);
			SetParent(hwnd, owner);
		}
		else
		{
			SetParent(hwnd, nullptr);
			style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
			SetWindowLongPtrW(hwnd, GWL_STYLE, style);
		}
		SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
			SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}


	void VPRendererVideoWindow::ThreadMain()
	{
		const HINSTANCE instance = FilterModuleHandle();
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.style = CS_DBLCLKS;
		wc.lpfnWndProc = &VPRendererVideoWindow::WindowProc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		wc.lpszClassName = kWindowClass;
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			DebugLog::Log("VP Renderer filter: RegisterClassEx failed error=%lu", GetLastError());

		const HWND owner = Owner();
		RECT area = { 0, 0, 1, 1 };
		if (owner)
			GetClientRect(owner, &area);
		const HWND hwnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW, kWindowClass,
			L"VP Renderer", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
			0, 0, (std::max)(1L, area.right - area.left), (std::max)(1L, area.bottom - area.top),
			nullptr, nullptr, instance, this);
		if (!hwnd)
			DebugLog::Log("VP Renderer filter: CreateWindowEx failed error=%lu", GetLastError());
		m_hwnd.store(hwnd, std::memory_order_release);
		if (hwnd && owner)
			SetOwner(owner);
		{
			std::lock_guard<std::mutex> lock(m_startMutex);
			m_startDone = true;
		}
		m_started.notify_all();
		if (!hwnd)
			return;

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}


	LRESULT CALLBACK VPRendererVideoWindow::WindowProc(HWND hwnd, UINT message,
		WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_NCCREATE)
		{
			const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA,
				reinterpret_cast<LONG_PTR>(create->lpCreateParams));
		}
		VPRendererVideoWindow* self = reinterpret_cast<VPRendererVideoWindow*>(
			GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (!self)
			return DefWindowProcW(hwnd, message, wParam, lParam);
		return self->HandleMessage(hwnd, message, wParam, lParam);
	}


	bool VPRendererVideoWindow::ForwardToDrain(HWND hwnd, UINT message, WPARAM wParam,
		LPARAM lParam)
	{
		const HWND drain = MessageDrain();
		if (!drain)
			return false;
		// Client coordinates of mouse messages are re-expressed in the drain's
		// client area; the wheel messages already carry screen coordinates.
		if (IsMouseMessage(message) && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL)
		{
			POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			MapWindowPoints(hwnd, drain, &point, 1);
			lParam = MAKELPARAM(point.x, point.y);
		}
		PostMessageW(drain, message, wParam, lParam);
		return true;
	}


	LRESULT VPRendererVideoWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wParam,
		LPARAM lParam)
	{
		if ((IsMouseMessage(message) || IsKeyboardMessage(message)) &&
			ForwardToDrain(hwnd, message, wParam, lParam))
			return 0;

		try
		{
			switch (message)
			{
			case WM_SIZE:
				if (m_callbacks.onSize)
					m_callbacks.onSize();
				return 0;

			case WM_PAINT:
			{
				PAINTSTRUCT paint;
				BeginPaint(hwnd, &paint);
				EndPaint(hwnd, &paint);
				if (m_callbacks.onPaint)
					m_callbacks.onPaint();
				return 0;
			}

			case WM_ERASEBKGND:
				// VP presents the whole client area; erasing would flicker.
				return 1;

			case WM_DISPLAYCHANGE:
				if (m_callbacks.onDisplayChange)
					m_callbacks.onDisplayChange();
				break;

			case WM_SETCURSOR:
				if (LOWORD(lParam) == HTCLIENT && CursorHidden())
				{
					SetCursor(nullptr);
					return TRUE;
				}
				break;

			case kMessageDestroy:
				DestroyWindow(hwnd);
				return 0;

			case WM_DESTROY:
				PostQuitMessage(0);
				return 0;
			}
		}
		catch (const std::exception& e)
		{
			DebugLog::Log("VP Renderer filter window message 0x%04X threw: %s", message, e.what());
		}
		catch (...)
		{
			DebugLog::Log("VP Renderer filter window message 0x%04X threw", message);
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}
}
