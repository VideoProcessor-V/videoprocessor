// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// "Set by" for one setting on the VP-V page (Config usability package, 2026-10-25).
// Pure C++17: no Qt, so the wording and the order of the sources are tested without a window.
//
// Every setting shown on a VP-V unit tab says where its value comes from, in plain words:
//   - the Original pathway      (the VP-V page is switched to Bill's pathway: the value is the baseline's);
//   - a master                  (a tier, the de-judder amount, the Advanced LED values ... greyed until Custom);
//   - a profile rule            (a named profile that its own "when" rule selects, and that sets the value);
//   - you                       (the value is written in the profile on screen);
//   - the default profile       (a named profile that does not set it inherits the default profile's value);
//   - VP's built-in default     (nothing sets it).
// The first that applies wins, in that order. A setting that a feature switch leaves unused also says so.

#include <VP-V/core/OriginalPathway.h>

#include <string>

namespace Vpv
{
	enum class ValueSource { BuiltInDefault, DefaultProfile, ThisProfile, RuleProfile, Master, OriginalPathway };

	struct SetByQuery
	{
		bool originalPathway = false;       // the VP-V page is in the Original pathway
		std::string key;                    // the renderer key (looked up in OriginalPathway::Overrides())
		std::string ownValue;               // written in the profile on screen ("" = not written)
		std::string inheritedValue;         // the default profile's value, when another profile is on screen
		bool defaultProfileOnScreen = true;
		std::string profileName;            // the profile on screen (plain name)
		std::string profileRule;            // that profile's "when" rule ("" = none)
		std::string builtInDefault;         // VP's built-in value ("" = unknown)
		std::string master;                 // plain "Repair tier (Low)" when a master locks the setting now, else ""
		std::string effectiveValue;         // what the setting reads now (already resolved), "" = unknown
		std::string notUsedBecause;         // plain "VP's debander is off" when a feature switch leaves it unused, else ""
	};

	struct SetByResult
	{
		ValueSource source = ValueSource::BuiltInDefault;
		std::string text;
	};

	// The Original pathway's value for a key, when the baseline sets it.
	inline bool OriginalPathwayValue(const std::string& key, std::string& value)
	{
		for (const OriginalPathway::Override& o : OriginalPathway::Overrides())
			if (key == o.key) { value = o.value; return true; }
		return false;
	}

	// A value in plain words: on/off capitalised, an empty value as "none".
	inline std::string PlainValue(const std::string& raw)
	{
		std::string v;
		for (char c : raw) if (c != '\r' && c != '\n') v += c;
		const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
		v = a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
		if (v.empty()) return "none";
		if (v[0] >= 'a' && v[0] <= 'z') v[0] = static_cast<char>(v[0] - 'a' + 'A');
		return v;
	}

	inline SetByResult SetBy(const SetByQuery& q)
	{
		SetByResult r;
		std::string value = q.effectiveValue;
		std::string original;
		if (q.originalPathway && OriginalPathwayValue(q.key, original))
		{
			r.source = ValueSource::OriginalPathway;
			r.text = "Set by the Original pathway (VP's additions are off): " + PlainValue(original);
			return r;
		}
		if (value.empty()) value = !q.ownValue.empty() ? q.ownValue : !q.inheritedValue.empty() ? q.inheritedValue : q.builtInDefault;
		const std::string shown = value.empty() ? std::string() : ": " + PlainValue(value);
		if (!q.master.empty())
		{
			r.source = ValueSource::Master;
			r.text = "Set by " + q.master + shown;
		}
		else if (!q.ownValue.empty())
		{
			if (!q.defaultProfileOnScreen && !q.profileRule.empty())
			{
				r.source = ValueSource::RuleProfile;
				r.text = "Set by profile \"" + q.profileName + "\" (chosen by its rule)" + shown;
			}
			else
			{
				r.source = ValueSource::ThisProfile;
				r.text = q.defaultProfileOnScreen ? "Set by you" + shown :
					"Set by you, in profile \"" + q.profileName + "\"" + shown;
			}
		}
		else if (!q.defaultProfileOnScreen && !q.inheritedValue.empty())
		{
			r.source = ValueSource::DefaultProfile;
			r.text = "Set by the default profile" + shown;
		}
		else
		{
			r.source = ValueSource::BuiltInDefault;
			r.text = "Set by VP's built-in default" + shown;
		}
		if (!q.notUsedBecause.empty()) r.text += ". Not used now: " + q.notUsedBecause;
		return r;
	}
}
