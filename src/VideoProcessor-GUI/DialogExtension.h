// VPV-HOOK-FILE
/*
 * Copyright(C) 2021 Dennis Fleurbaaij <mail@dennisfleurbaaij.com>
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 3.
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along with this program. If not, see < https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

// Main dialog extension point: an extension is told about the dialog's one-second timer, may
// answer remote requests, add statistics rows, adjust the renderer-failure policy and take
// shortcut ids, without the dialog knowing it. With nothing registered every call below does
// nothing and returns the dialog's own value.
//
// Registration contract: Register() is called only at start-up, before the dialog creates its
// timers or worker threads, and the registry is read-only afterwards (UI thread only, no locks).
// Registration order is call order. Extensions are not owned and must outlive the registry.
// No MFC types appear here so the registry can be unit tested.
namespace DialogExtension
{
	using StatsRow = std::pair<std::wstring, std::wstring>;

	class IDialogExtension
	{
	public:
		virtual ~IDialogExtension() = default;

		// Once per second from the dialog's timer.
		virtual void OnTimer() {}
		// True when the request was handled; `reply` then holds the answer.
		virtual bool OnRpcRequest(const std::string& verb, const std::string& payload, std::string& reply)
		{ (void)verb; (void)payload; (void)reply; return false; }
		// Append rows to the statistics list.
		virtual void StatsRows(std::vector<StatsRow>& rows) { (void)rows; }
		// Receives the policy the dialog chose and returns the policy to use.
		virtual int OnRendererFailure(int billPolicy) { return billPolicy; }
		// True when the shortcut id was taken.
		virtual bool OnShortcut(int id) { (void)id; return false; }
	};

	namespace Detail
	{
		inline std::vector<IDialogExtension*>& Registry()
		{
			static std::vector<IDialogExtension*> registry;
			return registry;
		}
	}

	// Start-up only. A null pointer is ignored.
	inline void Register(IDialogExtension* extension) noexcept
	{
		try { if (extension) Detail::Registry().push_back(extension); }
		catch (...) {}
	}

	inline void OnTimer() noexcept
	{
		try { for (IDialogExtension* e : Detail::Registry()) { try { e->OnTimer(); } catch (...) {} } }
		catch (...) {}
	}

	// First extension that returns true wins.
	inline bool OnRpcRequest(const std::string& verb, const std::string& payload, std::string& reply) noexcept
	{
		try
		{
			for (IDialogExtension* e : Detail::Registry())
			{
				try { if (e->OnRpcRequest(verb, payload, reply)) return true; } catch (...) {}
			}
		}
		catch (...) {}
		return false;
	}

	inline void StatsRows(std::vector<StatsRow>& rows) noexcept
	{
		try { for (IDialogExtension* e : Detail::Registry()) { try { e->StatsRows(rows); } catch (...) {} } }
		catch (...) {}
	}

	// Each extension sees the previous one's answer; an extension that throws is skipped.
	inline int OnRendererFailure(int billPolicy) noexcept
	{
		int policy = billPolicy;
		try
		{
			for (IDialogExtension* e : Detail::Registry())
			{
				try { policy = e->OnRendererFailure(policy); } catch (...) {}
			}
		}
		catch (...) {}
		return policy;
	}

	// First extension that returns true wins.
	inline bool OnShortcut(int id) noexcept
	{
		try
		{
			for (IDialogExtension* e : Detail::Registry())
			{
				try { if (e->OnShortcut(id)) return true; } catch (...) {}
			}
		}
		catch (...) {}
		return false;
	}

	// Tests only.
	inline void ResetForTesting() noexcept
	{
		try { Detail::Registry().clear(); } catch (...) {}
	}
}
