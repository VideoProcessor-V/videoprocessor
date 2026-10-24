#pragma once

// VP-V tier model (settings agent, 2026-10-24). Pure C++17: no Qt, no Windows, so the Config
// widget, the Config tests and any later host share one implementation.
//
// A unit (Input, Motion, Repair, Scaling, Colour, Dynamic Drive, Output; docs/design/slots.json
// units) may have Classic / Low / Mid / Upper tiers, or fewer where it cannot tell them apart
// today. Each tier is a preset of EXISTING renderer-profile keys; no key stores the tier itself
// (owner slider scheme: keep the underlying keys). The tier shown is read back from the values:
// the preset whose keys all match, else "Custom". The global tier sets every tiered unit to its
// highest tier at or below the global one.
//
// The data comes from VpvTierTable.generated.h (tools/gen_vpv_tiers.py).

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	struct KeyValue { const char* key; const char* value; };
	struct OnEnter { const char* key; const char* from; const char* to; };
	struct Preset
	{
		const char* tier = nullptr;
		const char* description = nullptr;
		const char* cost = nullptr;
		std::vector<KeyValue> set;        // define and write the tier
		std::vector<OnEnter> onEnter;     // written only when the current value equals 'from'
	};
	struct Toggle
	{
		const char* key;
		const char* label;
		std::vector<const char*> values;
		const char* slot;
	};
	struct Planned { const char* key; const char* label; const char* slot; };
	struct PlannedTier { const char* tier; const char* note; };
	struct SlotInfo
	{
		const char* id;
		const char* name;
		const char* unit;
		const char* status;     // built / partial / placeholder
		int order;
		const char* fallback;   // the "Fallbacks engaged" entry text, or nullptr
	};
	struct Unit
	{
		const char* id = nullptr;
		const char* name = nullptr;
		const char* summary = nullptr;
		const char* untieredReason = nullptr;
		std::vector<const char*> slotIds;
		std::vector<Preset> presets;        // in tier order
		std::vector<PlannedTier> plannedTiers;
		std::vector<const char*> taste;     // never greyed upstream (the motion slider)
		std::vector<Toggle> toggles;
		std::vector<Planned> planned;       // shown disabled: "not yet implemented"
		bool Tiered() const { return !presets.empty(); }
	};
	struct Table
	{
		const char* slotsFrozen = nullptr;
		std::vector<const char*> tiers;     // the global tiers, Classic .. Upper
		const char* section = nullptr;      // [vpv]
		const char* pathwayKey = nullptr;
		const char* pathwayDefault = nullptr;
		const char* delaySection = nullptr;
		const char* delayKey = nullptr;
		int recommendedFrames = 4;
		std::vector<KeyValue> defaults;     // built-in value when a key is absent
		std::vector<SlotInfo> slotList;
		std::vector<Unit> units;

		const char* Default(const std::string& key) const
		{
			for (const KeyValue& kv : defaults)
				if (key == kv.key) return kv.value;
			return "";
		}
		const SlotInfo* Slot(const std::string& id) const
		{
			for (const SlotInfo& s : slotList)
				if (id == s.id) return &s;
			return nullptr;
		}
		const Unit* FindUnit(const std::string& id) const
		{
			for (const Unit& u : units)
				if (id == u.id) return &u;
			return nullptr;
		}
		int GlobalIndex(const std::string& tier) const
		{
			for (size_t i = 0; i < tiers.size(); ++i)
				if (tier == tiers[i]) return static_cast<int>(i);
			return -1;
		}
	};

	// The stored value of a key in the profile being edited (inherited values included), or ""
	// when nothing sets it (the built-in default then applies).
	using Getter = std::function<std::string(const std::string& key)>;

	inline std::string Lower(std::string s)
	{
		for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return s;
	}

	inline std::string Trim(const std::string& s)
	{
		const size_t a = s.find_first_not_of(" \t");
		if (a == std::string::npos) return {};
		const size_t b = s.find_last_not_of(" \t");
		return s.substr(a, b - a + 1);
	}

	// Config writes on/off; older files and the parsers also accept true/false/yes/no.
	inline std::string Canonical(const std::string& raw)
	{
		const std::string v = Lower(Trim(raw));
		if (v == "true" || v == "yes") return "on";
		if (v == "false" || v == "no") return "off";
		return v;
	}

	inline bool SameValue(const std::string& a, const std::string& b)
	{
		const std::string x = Canonical(a), y = Canonical(b);
		char* endX = nullptr;
		char* endY = nullptr;
		const double dx = std::strtod(x.c_str(), &endX);
		const double dy = std::strtod(y.c_str(), &endY);
		if (!x.empty() && !y.empty() && *endX == 0 && *endY == 0)
			return std::abs(dx - dy) < 1e-9;
		return x == y;
	}

	inline std::string Effective(const Table& table, const Getter& get, const std::string& key)
	{
		const std::string v = Trim(get(key));
		return v.empty() ? std::string(table.Default(key)) : v;
	}

	// The preset whose 'set' keys all match (the one with most keys when several do), or -1
	// (Custom). An untiered unit gives -1.
	inline int MatchTier(const Table& table, const Unit& unit, const Getter& get)
	{
		int best = -1;
		size_t bestKeys = 0;
		for (size_t p = 0; p < unit.presets.size(); ++p)
		{
			bool all = true;
			for (const KeyValue& kv : unit.presets[p].set)
				if (!SameValue(Effective(table, get, kv.key), kv.value)) { all = false; break; }
			if (all && (best < 0 || unit.presets[p].set.size() > bestKeys))
			{
				best = static_cast<int>(p);
				bestKeys = unit.presets[p].set.size();
			}
		}
		return best;
	}

	// The key writes that select preset p: the set keys, then - only when the unit is leaving its
	// first tier (Classic: everything off) - the on_enter keys whose current value equals 'from'
	// (e.g. the default motion taste comes back; a taste chosen with another tier is never touched).
	// Only keys whose effective value changes are listed.
	inline std::vector<std::pair<std::string, std::string>> ApplyTier(const Table& table, const Unit& unit, int p,
		const Getter& get)
	{
		std::vector<std::pair<std::string, std::string>> writes;
		if (p < 0 || p >= static_cast<int>(unit.presets.size())) return writes;
		const Preset& preset = unit.presets[static_cast<size_t>(p)];
		for (const KeyValue& kv : preset.set)
			if (!SameValue(Effective(table, get, kv.key), kv.value))
				writes.emplace_back(kv.key, kv.value);
		const bool leavingFirst = p > 0 && MatchTier(table, unit, get) == 0;
		for (const OnEnter& e : preset.onEnter)
			if (leavingFirst && SameValue(Effective(table, get, e.key), e.from))
				writes.emplace_back(e.key, e.to);
		return writes;
	}

	// The unit preset the global tier g selects: the highest unit tier at or below g, else the
	// lowest one; -1 for an untiered unit.
	inline int GlobalToUnit(const Table& table, const Unit& unit, int g)
	{
		if (!unit.Tiered()) return -1;
		int chosen = 0;
		for (size_t p = 0; p < unit.presets.size(); ++p)
			if (table.GlobalIndex(unit.presets[p].tier) <= g) chosen = static_cast<int>(p);
		return chosen;
	}

	// The global tier when every tiered unit sits exactly on it, else -1 (Custom per unit).
	inline int MatchGlobal(const Table& table, const Getter& get)
	{
		for (int g = static_cast<int>(table.tiers.size()) - 1; g >= 0; --g)
		{
			bool all = true;
			for (const Unit& u : table.units)
				if (u.Tiered() && MatchTier(table, u, get) != GlobalToUnit(table, u, g)) { all = false; break; }
			if (all) return g;
		}
		return -1;
	}

	// Keys the VP-V section owns while its pathway is on (greyed "managed by VP-V" in the
	// upstream pages): tier keys and toggles, never the taste keys.
	inline std::set<std::string> ManagedKeys(const Table& table)
	{
		std::set<std::string> keys, taste;
		for (const Unit& u : table.units)
		{
			for (const char* k : u.taste) taste.insert(k);
			for (const Preset& p : u.presets)
				for (const KeyValue& kv : p.set) keys.insert(kv.key);
			for (const Toggle& t : u.toggles) keys.insert(t.key);
		}
		for (const std::string& k : taste) keys.erase(k);
		return keys;
	}

	// The planned (not yet implemented) note for a unit tier above the ones it lists.
	inline const char* PlannedTierNote(const Unit& unit, const std::string& tier)
	{
		for (const PlannedTier& t : unit.plannedTiers)
			if (tier == t.tier) return t.note;
		return nullptr;
	}
}
