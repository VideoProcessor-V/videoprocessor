#pragma once

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// "Fallbacks engaged" (owner rule vp-no-silent-fallbacks, 2026-10-20; r6 fix 7, 2026-10-24).
// Every AUTOMATIC fallback or quality downgrade is visible, never log-only, under ONE heading in
// the Ctrl+I overlay, the Config status and the Ctrl+Alt+D bundle:
//
//   Fallbacks engaged: capture GPU unpack (CPU formatters for the session: <reason>); render-ahead (off: ...)
//   Fallbacks engaged: none
//
// plus a brief one-time notice (no always-on OSD) the first time each fallback engages in a session.
//
// A component that can fall back owns a stable id (the entry's name) and calls Engage(id, reason)
// when it falls back and Release(id) when it recovers; nothing else is needed. The renderer
// publishes Heading() through IVideoRenderer::GetFallbacksStatus; the GUI shows it and asks
// Notices::Take for the entries it has not announced yet.
//
// Header-only and pure (no Windows, no clock): shared by the EXE, the renderer DLL and the tests.
namespace FallbackStatus
{
	constexpr const char* kHeading = "Fallbacks engaged: ";
	constexpr const char* kNone = "none";
	constexpr const char* kSeparator = "; ";

	struct Entry
	{
		std::string id;       // stable short name, e.g. "capture GPU unpack"
		std::string reason;   // short reason / active path, e.g. "CPU formatters for the session: <error>"
	};

	// "Fallbacks engaged: a (why); b (why)" or "Fallbacks engaged: none". Ids keep engagement order.
	inline std::string Format(const std::vector<Entry>& entries)
	{
		std::string text = kHeading;
		if (entries.empty())
			return text + kNone;
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (i) text += kSeparator;
			text += entries[i].id;
			if (!entries[i].reason.empty())
				text += " (" + entries[i].reason + ")";
		}
		return text;
	}

	// The ids in a Format()ted heading (for the GUI's one-time notices). Reasons may contain
	// "; " inside parentheses; only top-level separators split.
	inline std::vector<std::string> Ids(const std::string& heading)
	{
		std::vector<std::string> ids;
		const std::string prefix = kHeading;
		if (heading.compare(0, prefix.size(), prefix) != 0)
			return ids;
		const std::string body = heading.substr(prefix.size());
		if (body.empty() || body == kNone)
			return ids;
		int depth = 0;
		std::string current;
		bool inReason = false;
		auto flush = [&]()
		{
			size_t end = current.find_last_not_of(' ');
			std::string id = end == std::string::npos ? std::string() : current.substr(0, end + 1);
			if (!id.empty())
				ids.push_back(id);
			current.clear();
			inReason = false;
		};
		for (size_t i = 0; i < body.size(); ++i)
		{
			const char c = body[i];
			if (c == '(')
			{
				if (depth++ == 0)
					inReason = true;
				continue;
			}
			if (c == ')')
			{
				depth = (std::max)(0, depth - 1);
				continue;
			}
			if (depth == 0 && c == ';' && i + 1 < body.size() && body[i + 1] == ' ')
			{
				flush();
				++i;
				continue;
			}
			if (depth == 0 && !inReason)
				current += c;
		}
		flush();
		return ids;
	}

	// The engaged fallbacks of one component (thread-safe). Engage/Release return true when the
	// heading changed, so the caller can log once.
	class Registry
	{
	public:
		bool Engage(const std::string& id, const std::string& reason)
		{
			std::lock_guard<std::mutex> guard(mutex);
			for (Entry& e : entries)
				if (e.id == id)
				{
					if (e.reason == reason)
						return false;
					e.reason = reason;
					return true;
				}
			entries.push_back({ id, reason });
			return true;
		}
		bool Release(const std::string& id)
		{
			std::lock_guard<std::mutex> guard(mutex);
			const auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == id; });
			if (it == entries.end())
				return false;
			entries.erase(it);
			return true;
		}
		bool Engaged(const std::string& id) const
		{
			std::lock_guard<std::mutex> guard(mutex);
			return std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == id; });
		}
		std::vector<Entry> Entries() const
		{
			std::lock_guard<std::mutex> guard(mutex);
			return entries;
		}
		std::string Heading() const { return Format(Entries()); }
		void Clear()
		{
			std::lock_guard<std::mutex> guard(mutex);
			entries.clear();
		}

	private:
		mutable std::mutex mutex;
		std::vector<Entry> entries;
	};

	// The GUI side: each fallback id is announced once per session (a brief transient notice),
	// however often it engages and releases afterwards.
	class Notices
	{
	public:
		// The ids in `heading` not announced yet (now marked announced).
		std::vector<std::string> Take(const std::string& heading)
		{
			std::vector<std::string> fresh;
			for (const std::string& id : Ids(heading))
				if (announced.insert(id).second)
					fresh.push_back(id);
			return fresh;
		}
		bool Announced(const std::string& id) const { return announced.count(id) != 0; }

	private:
		std::set<std::string> announced;
	};
}
