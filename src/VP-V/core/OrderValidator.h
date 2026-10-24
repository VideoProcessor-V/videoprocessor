#pragma once

#include "PipelineSlots.h"
#include "SlotContracts.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Accepts or rejects a whole module order with a plain-language reason (ADR-0021). Pure: no
// renderer state; SlotRegistry.h calls it before any order is used, so an invalid order never runs.
namespace PipelineSlots
{
	inline const char* ResName(uint8_t r)
	{
		return r == Res::Delivered ? "the delivered resolution" : r == Res::Encoded ? "the encoded resolution" :
			r == Res::Processing ? "the processing resolution" : "the output resolution";
	}
	inline const char* ResNeedName(uint8_t r)
	{
		return r == Res::Delivered ? "the delivered resolution" : r == Res::Processing ? "the processing resolution" :
			r == (Res::Delivered | Res::Encoded) ? "the delivered or encoded resolution" : "another resolution";
	}
	inline const char* LightName(uint8_t l)
	{
		return l == Light::Source ? "the source signal" : l == Light::Target ? "target light" : "projector codes";
	}

	// Accepts or rejects a whole order (every slot exactly once). The reason is plain language.
	inline bool Validate(const std::vector<Id>& order, std::string* reason)
	{
		auto fail = [reason](const std::string& text) { if (reason) *reason = text; return false; };
		if (order.size() != kCount)
			return fail("the order must list every slot exactly once");
		std::array<int, kCount> position{};
		position.fill(-1);
		for (size_t i = 0; i < order.size(); ++i)
		{
			const size_t k = static_cast<size_t>(order[i]);
			if (k >= kCount || position[k] >= 0)
				return fail("the order must list every slot exactly once");
			position[k] = static_cast<int>(i);
		}
		// 1. Zones keep their sequence: a slot never leaves its zone.
		int lastZone = -1;
		Id lastId = Id::Count;
		for (Id id : order)
		{
			const Zone zone = ZoneOf(id);
			if (zone == Zone::Host)
				continue;
			if (static_cast<int>(zone) < lastZone)
				return fail(std::string(Get(id).name) + " cannot move out of its zone (it would run after " + Get(lastId).name + ")");
			lastZone = static_cast<int>(zone);
			lastId = id;
		}
		// 2. A fixed slot keeps its place relative to every other slot of its zone.
		for (const Info& a : Slots())
		{
			if (ContractOf(a.id).movable || ZoneOf(a.id) == Zone::Host)
				continue;
			for (const Info& b : Slots())
			{
				if (a.id == b.id || ZoneOf(b.id) != ZoneOf(a.id))
					continue;
				const bool designedBefore = b.order < a.order;
				const bool nowBefore = position[static_cast<size_t>(b.id)] < position[static_cast<size_t>(a.id)];
				if (designedBefore != nowBefore)
					return fail(std::string(a.name) + " is fixed in its zone and cannot change places with " + b.name);
			}
		}
		// 3. Hard precede rules.
		for (const Precede& rule : HardRules())
			if (position[static_cast<size_t>(rule.first)] > position[static_cast<size_t>(rule.second)])
				return fail(std::string(Get(rule.first).name) + " must run before " + Get(rule.second).name + ": " + rule.why);
		// 4. The domain walk through each movable zone, with every slot assumed on, so the
		// validity of an order never depends on which slots happen to be enabled.
		struct State { uint8_t res, light, layout; bool vectors, modified; Id resBy, lightBy, modifiedBy; };
		auto walk = [&](Zone zone, State s) -> bool
		{
			for (Id id : order)
			{
				if (ZoneOf(id) != zone)
					continue;
				const Contract c = ContractOf(id);
				const std::string name = Get(id).name;
				if (!(c.needRes & s.res))
					return fail(name + " needs " + ResNeedName(c.needRes) + ", but at that position it gets " + ResName(s.res) +
						" (after " + Get(s.resBy).name + ")");
				if (!(c.needLight & s.light))
					return fail(name + " needs " + LightName(c.needLight) + ", but at that position it gets " + LightName(s.light) +
						" (after " + Get(s.lightBy).name + ")");
				if (!(c.needLayout & s.layout))
					return fail(name + " needs subsampled chroma, but the chroma is already full resolution at that position");
				if (c.needsVectors && !s.vectors)
					return fail(name + " needs motion vectors, so it must run after motion.estimation");
				if (c.needsOriginal && s.modified)
					return fail(name + " must see the original delivered frame, but " + Get(s.modifiedBy).name + " runs before it");
				if (c.giveRes) { s.res = c.giveRes; s.resBy = id; }
				if (c.giveLight) { s.light = c.giveLight; s.lightBy = id; }
				if (c.giveLayout) s.layout = c.giveLayout;
				if (c.givesVectors) s.vectors = true;
				if (c.modifies && !s.modified) { s.modified = true; s.modifiedBy = id; }
			}
			return true;
		};
		if (!walk(Zone::FrontEnd, { Res::Delivered, Light::Source, Layout::Subsampled, false, false,
			Id::InputCapture, Id::InputCapture, Id::InputCapture }))
			return false;
		if (!walk(Zone::OutputHooks, { Res::Output, Light::Target, Layout::Full, true, true,
			Id::ScalingOutput, Id::ColourToneGamut, Id::ScalingOutput }))
			return false;
		if (reason) reason->clear();
		return true;
	}
}
