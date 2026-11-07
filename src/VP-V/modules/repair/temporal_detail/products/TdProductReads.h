// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// repair.temporal_detail / products: the ONE read API of the reserved engine products and their placeholder producer.
//
// Every consumer (adaptive scaling, dither need / sigma-delta, the chroma stage) calls Read() where it would use the engine's
// products. The rule is the one of SourceIdentity::View (doc 61 2.4):
//
//   engine not requested          -> not a fallback: usable = false, the classical form runs exactly as before (byte-identical).
//   engine requested, a product the consumer needs is absent / stale / of another unit version, or there is no engine at all
//                                 -> the classical form, VISIBLY: the returned Decision is engaged (FallbackCatalog
//                                    "product.td_*") with the reason, and it recovers by itself when the products return.
//   engine requested, every needed product published, fresh and of this unit version -> usable = true (reset 2).
//
// Reset 1 ships the PLACEHOLDER producer: PlaceholderSnapshot() publishes nothing, and kEngineRequestedToday is false (no
// configuration key exists yet: the SCHEMA lock is held, the key is listed as deferred in the README), so every call today is
// "not requested" and no status line appears. Reset 2 replaces the placeholder with the engine's snapshot and flips the request
// to its key; nothing in the consumers changes. Pure functions over plain structs (no clock: `nowUs` is passed in).
// Header-only, C++14.
#pragma once

#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/modules/repair/temporal_detail/products/TdProductContracts.h>

#include <cstdint>
#include <string>

namespace TdProducts
{
	// What the producer says about one product right now.
	struct Entry
	{
		bool published = false;
		uint32_t unitVersion = 0;   // the unit version that made it (kUnitVersion expected)
		int64_t timeUs = 0;         // source time of the content
		int64_t maxAgeUs = 0;       // 0 = no age limit (a stored slot); otherwise stale after this age
	};

	struct Snapshot
	{
		Entry entry[kCount];
		const Entry& Of(ProductId id) const { return entry[IndexOf(id)]; }
	};

	// The reset-1 placeholder producer: nothing is ever published.
	inline const Snapshot* PlaceholderSnapshot()
	{
		static const Snapshot empty{};
		return &empty;
	}

	// Whether the engine is asked for. No configuration key exists in reset 1 (deferred, SCHEMA lock), so this is false.
	constexpr bool kEngineRequestedToday = false;

	struct ReadResult
	{
		bool requested = false;
		bool usable = false;                  // the engine's products may be used by the consumer
		unsigned missingMask = 0;             // bit i = product index i (IndexOf) not usable
		std::string reason;                   // "" unless the classical form stands in for a requested engine
		FallbackCatalog::Decision fallback;   // engaged exactly when requested && !usable
	};

	inline const char* WhyNotUsable(const Entry& e, int64_t nowUs)
	{
		if (!e.published)
			return "absent";
		if (e.unitVersion != kUnitVersion)
			return "unit version";
		if (e.maxAgeUs > 0 && (nowUs - e.timeUs > e.maxAgeUs || nowUs < e.timeUs - e.maxAgeUs))
			return "stale";
		return nullptr;
	}

	// `snapshot` may be null (no producer at all).
	inline ReadResult Read(Consumer consumer, const Snapshot* snapshot, bool requested, int64_t nowUs)
	{
		ReadResult r;
		r.requested = requested;
		const char* id = FallbackIdOf(consumer);
		if (!requested)
		{
			r.fallback = FallbackCatalog::Released(id);
			return r;
		}
		if (!snapshot)
		{
			r.reason = "engine not built";
			for (ProductId p : NeededBy(consumer))
				r.missingMask |= 1u << unsigned(IndexOf(p));
		}
		else
		{
			for (ProductId p : NeededBy(consumer))
			{
				const char* why = WhyNotUsable(snapshot->Of(p), nowUs);
				if (!why)
					continue;
				r.missingMask |= 1u << unsigned(IndexOf(p));
				r.reason += (r.reason.empty() ? "" : "; ") + std::string(ContractOf(p)->token) + " " + why;
			}
		}
		if (r.missingMask == 0)
		{
			r.usable = true;
			r.fallback = FallbackCatalog::Released(id);
			return r;
		}
		r.fallback = FallbackCatalog::ProductFallback(id, r.reason);
		return r;
	}

	// The status line of a consumer under a read: its fallback wording while the classical form stands in, "" otherwise.
	inline std::string StatusLine(Consumer consumer, const ReadResult& r)
	{
		return r.fallback.engaged ? std::string(Name(consumer)) + ": classical form (temporal-detail products: " + r.reason + ")"
		                          : std::string();
	}

	// The reset-1 read of a consumer: the placeholder producer, the engine not requested. Byte-identical to not reading at all.
	inline ReadResult ReadToday(Consumer consumer)
	{
		return Read(consumer, PlaceholderSnapshot(), kEngineRequestedToday, 0);
	}
}
