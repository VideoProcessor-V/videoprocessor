#pragma once

#include "PipelineSlots.h"
#include "OrderValidator.h"
#include "SlotContracts.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The ONE slot registry the renderer consults (barrier R2): which slots run (slot-level switches
// and the zero-cost rule), the Original pathway baseline (ADR-0019), and the module order
// (ADR-0021). See core/README.md for how the core headers connect.
namespace PipelineSlots
{
	// Slot-level enable/disable, which slots have a component, the order, and the Original
	// pathway mode. Defaults: every slot with a component is enabled (its own settings still
	// decide whether it does anything); every placeholder is disabled and has no component.
	class Registry
	{
	public:
		Registry()
		{
			for (const Info& info : Slots())
			{
				const size_t i = static_cast<size_t>(info.id);
				component[i] = info.status != Status::Placeholder;
				enabled[i] = component[i];
			}
			ResetOrder();
		}

		void SetEnabled(Id id, bool on) { enabled[static_cast<size_t>(id)] = on; }
		bool Enabled(Id id) const { return enabled[static_cast<size_t>(id)]; }

		// A real component registers itself when it lands in its slot (or is unavailable, e.g.
		// missing net weights: then the slot reports its "Fallbacks engaged" entry and stays inert).
		void SetComponent(Id id, bool present) { component[static_cast<size_t>(id)] = present; }
		bool HasComponent(Id id) const { return component[static_cast<size_t>(id)]; }

		// THE zero-cost rule: only an enabled slot with a component may run a pass or register
		// a hook. Everything else is skipped entirely.
		bool Runs(Id id) const { return Enabled(id) && HasComponent(id); }

		// ORIGINAL PATHWAY (owner 2026-10-24, ADR-0019): every VP addition off, every mixed slot
		// in its upstream form and the designed order, so the pipeline reproduces Bill's original
		// pathway. Components read UpstreamForm() to drop their VP options (the settings
		// overrides are listed per slot in slots.json "original_pathway").
		void ApplyOriginalPathway()
		{
			for (const Info& info : Slots())
			{
				const size_t i = static_cast<size_t>(info.id);
				if (info.origin == Origin::Vp)
					enabled[i] = false;
				upstreamForm[i] = info.origin != Origin::Vp;
			}
			ResetOrder();
			original = true;
		}
		bool OriginalPathway() const { return original; }
		bool UpstreamForm(Id id) const { return upstreamForm[static_cast<size_t>(id)]; }

		// ---- the order (ADR-0021) ----
		void ResetOrder() { SetFullOrder(DefaultOrder()); }
		const std::vector<Id>& CurrentOrder() const { return order; }
		bool IsDefaultOrder() const { return order == DefaultOrder(); }

		// Moves the listed slots among the positions they occupy now, in the listed order (e.g.
		// {repair.encoding, scaling.rebase} swaps those two). A rejected order changes nothing.
		bool SetOrder(const std::vector<Id>& slots, std::string* reason = nullptr)
		{
			std::vector<size_t> positions;
			for (Id id : slots)
			{
				if (static_cast<size_t>(id) >= kCount)
				{
					if (reason) *reason = "unknown slot";
					return false;
				}
				positions.push_back(static_cast<size_t>(rank[static_cast<size_t>(id)]));
			}
			std::sort(positions.begin(), positions.end());
			if (std::adjacent_find(positions.begin(), positions.end()) != positions.end())
			{
				if (reason) *reason = "a slot is listed twice";
				return false;
			}
			std::vector<Id> candidate = order;
			for (size_t i = 0; i < slots.size(); ++i)
				candidate[positions[i]] = slots[i];
			if (!Validate(candidate, reason))
				return false;
			SetFullOrder(candidate);
			return true;
		}

		// The same from a setting: slot names separated by commas, spaces or '>'. Empty = the
		// designed order.
		bool SetOrderByNames(const std::string& text, std::string* reason = nullptr)
		{
			std::vector<Id> slots;
			std::string token;
			bool ok = true;
			auto flush = [&]()
			{
				if (token.empty() || !ok)
					return;
				const Id id = FromName(token.c_str());
				if (id == Id::Count)
				{
					if (reason) *reason = "unknown slot '" + token + "'";
					ok = false;
				}
				else
					slots.push_back(id);
				token.clear();
			};
			for (char ch : text)
			{
				if (ch == ',' || ch == ' ' || ch == '>' || ch == ';' || ch == '\t')
					flush();
				else
					token += ch;
			}
			flush();
			if (!ok)
				return false;
			if (slots.empty())
			{
				ResetOrder();
				return true;
			}
			std::vector<Id> saved = order;
			ResetOrder();   // a setting describes a change from the designed order
			if (SetOrder(slots, reason))
				return true;
			SetFullOrder(saved);
			return false;
		}

		// ADR-0005 convenience: the two candidate entrance orders.
		void SetRebaseFirst(bool first)
		{
			ResetOrder();
			if (!first)
				SetOrder({ Id::RepairEncoding, Id::ScalingRebase });
		}
		bool RebaseFirst() const
		{
			return rank[static_cast<size_t>(Id::ScalingRebase)] < rank[static_cast<size_t>(Id::RepairEncoding)];
		}

		// The effective position of a slot (the sorting key).
		int Order(Id id) const { return rank[static_cast<size_t>(id)]; }

		// What a net fingerprint records about everything upstream of `id` in the pixel path:
		// the running non-host slots before it, in order. Moving, adding or removing an upstream
		// module changes the key, and a net trained on another key is bypassed (visibly).
		std::string UpstreamKey(Id id) const
		{
			std::string key;
			for (Id other : order)
			{
				if (other == id)
					break;
				if (ZoneOf(other) == Zone::Host || !Runs(other))
					continue;
				if (!key.empty()) key += '>';
				key += Get(other).name;
			}
			return key;
		}

		// Slots that run, in effective order (all of them, or those at one hook point).
		std::vector<Id> Active() const { return Collect(false, HookPoint::Host); }
		std::vector<Id> Active(HookPoint hook) const { return Collect(true, hook); }
		// The entrance stages that run this frame, in effective order.
		std::vector<Id> ActiveEntrance() const
		{
			std::vector<Id> ids = Active(HookPoint::Prepass);
			ids.erase(std::remove_if(ids.begin(), ids.end(), [](Id id) { return !IsEntranceStage(id); }), ids.end());
			return ids;
		}

	private:
		void SetFullOrder(const std::vector<Id>& full)
		{
			order = full;
			for (size_t i = 0; i < order.size(); ++i)
				rank[static_cast<size_t>(order[i])] = static_cast<int>(i);
		}

		std::vector<Id> Collect(bool filter, HookPoint hook) const
		{
			std::vector<Id> ids;
			for (Id id : order)
				if (Runs(id) && (!filter || Get(id).hook == hook))
					ids.push_back(id);
			return ids;
		}

		std::array<bool, kCount> enabled{};
		std::array<bool, kCount> component{};
		std::array<bool, kCount> upstreamForm{};
		std::array<int, kCount> rank{};
		std::vector<Id> order;
		bool original = false;
	};
}
