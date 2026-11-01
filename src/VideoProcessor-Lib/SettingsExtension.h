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
#include <vector>

// Settings extension point.
//
// An extension owns a set of configuration keys. New keys register through it instead of being
// added to the key lists and validators in RendererProfileConfig.h / ConfigSchema.h. The built-in
// validators are always consulted first; an extension is asked only about a key they do not
// recognise, so with nothing registered every result is exactly the built-in result.
//
// Registration contract: Register() is called only during start-up, before any worker thread
// runs, and the registry is read-only afterwards. There are no locks on the query path. The
// registration order is the query order; the first extension that does not answer NotMine wins.
// Extensions are not owned by the registry and must outlive it (use static storage).
namespace SettingsExtension
{
	enum class Verdict { NotMine, Valid, Invalid };

	class ISettingsExtension
	{
	public:
		virtual ~ISettingsExtension() = default;

		// Judge `value` for `key` in `group`. NotMine: the key is not this extension's. Invalid:
		// the key is this extension's but the value is unacceptable; set `error` to say why.
		virtual Verdict Validate(const std::string& group, const std::string& key,
			const std::string& value, std::string& error) const = 0;

		// True when `key` in `section` belongs to this extension.
		virtual bool IsKnownKey(const std::string& section, const std::string& key) const
		{
			(void)section; (void)key;
			return false;
		}

		// Fill `value` with the default of an owned key. False: not this extension's key.
		virtual bool DefaultValue(const std::string& group, const std::string& key,
			std::string& value) const
		{
			(void)group; (void)key; (void)value;
			return false;
		}
	};

	namespace Detail
	{
		inline std::vector<const ISettingsExtension*>& Registry()
		{
			static std::vector<const ISettingsExtension*> registry;
			return registry;
		}
	}

	// Start-up only. A null pointer is ignored.
	inline void Register(const ISettingsExtension* extension) noexcept
	{
		try { if (extension) Detail::Registry().push_back(extension); }
		catch (...) {}
	}

	// NotMine when nothing is registered or no extension claims the key. An extension that
	// throws is treated as NotMine. An Invalid verdict always carries a non-empty `error`.
	inline Verdict Validate(const std::string& group, const std::string& key,
		const std::string& value, std::string& error) noexcept
	{
		try
		{
			for (const ISettingsExtension* extension : Detail::Registry())
			{
				try
				{
					std::string message;
					const Verdict verdict = extension->Validate(group, key, value, message);
					if (verdict == Verdict::NotMine) continue;
					if (verdict == Verdict::Invalid)
					{
						error = message.empty()
							? "[" + group + "] key '" + key + "' has an invalid value"
							: message;
					}
					return verdict;
				}
				catch (...) {}
			}
		}
		catch (...) {}
		return Verdict::NotMine;
	}

	inline bool IsKnownKey(const std::string& section, const std::string& key) noexcept
	{
		try
		{
			for (const ISettingsExtension* extension : Detail::Registry())
			{
				try { if (extension->IsKnownKey(section, key)) return true; }
				catch (...) {}
			}
		}
		catch (...) {}
		return false;
	}

	inline bool DefaultValue(const std::string& group, const std::string& key,
		std::string& value) noexcept
	{
		try
		{
			for (const ISettingsExtension* extension : Detail::Registry())
			{
				try { if (extension->DefaultValue(group, key, value)) return true; }
				catch (...) {}
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
