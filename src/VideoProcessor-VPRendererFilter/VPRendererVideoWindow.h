#pragma once

// VP Renderer DirectShow filter: the HWND VP renders into.
//
// The window lives on its own thread with its own message loop (so VP's
// present/resize work never depends on the player's UI thread being idle). It
// starts as a hidden pop-up and becomes a WS_CHILD of the player's video window
// on IVideoWindow::put_Owner. Mouse and keyboard input is posted to the message
// drain (IVideoWindow::put_MessageDrain) like the system video renderers do.

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>


namespace VPRendererFilter
{
	class VPRendererVideoWindow
	{
	public:
		struct Callbacks
		{
			std::function<void()> onSize;           // client size changed
			std::function<void()> onPaint;          // WM_PAINT
			std::function<void()> onDisplayChange;  // WM_DISPLAYCHANGE
		};

		VPRendererVideoWindow() = default;
		~VPRendererVideoWindow();
		VPRendererVideoWindow(const VPRendererVideoWindow&) = delete;
		VPRendererVideoWindow& operator=(const VPRendererVideoWindow&) = delete;

		// Starts the window thread and waits for the HWND. Idempotent.
		bool Create(const Callbacks& callbacks);
		void Destroy();
		HWND Hwnd() const { return m_hwnd.load(std::memory_order_acquire); }

		// IVideoWindow plumbing. All are safe from any thread.
		void SetOwner(HWND owner);
		HWND Owner() const { return m_owner.load(std::memory_order_acquire); }
		void SetMessageDrain(HWND drain) { m_drain.store(drain, std::memory_order_release); }
		HWND MessageDrain() const { return m_drain.load(std::memory_order_acquire); }
		void SetCursorHidden(bool hidden) { m_cursorHidden.store(hidden, std::memory_order_release); }
		bool CursorHidden() const { return m_cursorHidden.load(std::memory_order_acquire); }

	private:
		static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
		LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
		void ThreadMain();
		bool ForwardToDrain(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

		Callbacks m_callbacks;
		std::thread m_thread;
		std::mutex m_startMutex;
		std::condition_variable m_started;
		bool m_startDone = false;
		std::atomic<HWND> m_hwnd{ nullptr };
		std::atomic<HWND> m_owner{ nullptr };
		std::atomic<HWND> m_drain{ nullptr };
		std::atomic<bool> m_cursorHidden{ false };
	};
}
