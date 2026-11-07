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
//                                 -> the classical form, VISIBLY: the returned Decision is engaged with the reason, and it
//                                    recovers by itself when the products return. Its key is the consumer's id PLUS the cause
//                                    (FallbackKey: "product.chroma_cfl [td: absent]"), never the consumer's bare id: the bare id
//                                    belongs to the consumer's own classical cause (a missing chroma split, ...) and one entry may
//                                    not stand for, overwrite or release the other. Two causes are two entries (the GUI notice and
//                                    Registry.Engage key on the key). Recovery goes through SyncRead(), which releases every cause
//                                    key of the consumer that no longer applies.
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
#include <cstring>
#include <string>
#include <vector>

namespace TdProducts
{
	// What the producer says about one product right now.
	struct Entry
	{
		bool published = false;
		uint32_t unitVersion = 0;   // the unit version that made it (the contract's unitVersion expected)
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

	// Why a product is not usable: one bit per cause. A read's causeMask is the OR over its missing products (a mixed read has a
	// mixed key); kCauseNoEngine stands alone (no producer at all).
	enum : unsigned { kCauseAbsent = 1u, kCauseUnitVersion = 2u, kCauseStale = 4u, kCauseNoEngine = 8u, kCauseAll = 15u };

	struct ReadResult
	{
		bool requested = false;
		bool usable = false;                  // the engine's products may be used by the consumer
		unsigned missingMask = 0;             // bit i = product index i (IndexOf) not usable
		unsigned causeMask = 0;               // kCause* of the missing products; 0 unless the classical form stands in
		std::string reason;                   // "" unless the classical form stands in for a requested engine
		// Engaged exactly when requested && !usable; its id is then FallbackKey(consumer, causeMask). While not engaged the id is
		// EMPTY on purpose: the per-frame read of a consumer that is not requested must not build a string (allocation-free); release
		// through SyncRead(), never through Sync() on this decision.
		FallbackCatalog::Decision fallback;
	};

	inline unsigned CauseOf(const Entry& e, uint32_t expectedVersion, int64_t nowUs)
	{
		if (!e.published)
			return kCauseAbsent;
		if (e.unitVersion != expectedVersion)
			return kCauseUnitVersion;
		if (e.maxAgeUs > 0 && (nowUs - e.timeUs > e.maxAgeUs || nowUs < e.timeUs - e.maxAgeUs))
			return kCauseStale;
		return 0;
	}

	inline const char* CauseWord(unsigned cause)
	{
		switch (cause)
		{
		case kCauseAbsent: return "absent";
		case kCauseUnitVersion: return "unit version";
		case kCauseStale: return "stale";
		case kCauseNoEngine: return "engine not built";
		}
		return "";
	}

	inline const char* WhyNotUsable(const Entry& e, uint32_t expectedVersion, int64_t nowUs)
	{
		const unsigned cause = CauseOf(e, expectedVersion, nowUs);
		return cause ? CauseWord(cause) : nullptr;
	}

	constexpr size_t kConsumerCount = 6;   // Consumer::AdaptiveScaler .. Engine

	// The key of a consumer's temporal-detail fallback for a cause set: "<consumer id> [td: absent+stale]". Built once for every
	// (consumer, cause set) at first use, then returned by reference: the per-frame path never builds a string. The text has no "(", ")"
	// or "; " (FallbackStatus::Ids splits on those), so the heading and the GUI notice keep the key whole.
	inline const std::string& FallbackKey(Consumer consumer, unsigned causeMask)
	{
		static const std::vector<std::string> keys = []()
		{
			std::vector<std::string> k(kConsumerCount * (kCauseAll + 1));
			for (size_t c = 0; c < kConsumerCount; ++c)
				for (unsigned m = 1; m <= kCauseAll; ++m)
				{
					std::string causes;
					for (unsigned bit : { unsigned(kCauseAbsent), unsigned(kCauseUnitVersion), unsigned(kCauseStale), unsigned(kCauseNoEngine) })
						if (m & bit)
							causes += (causes.empty() ? "" : "+") + std::string(CauseWord(bit));
					k[c * (kCauseAll + 1) + m] = std::string(FallbackIdOf(Consumer(c))) + " [td: " + causes + "]";
				}
			return k;
		}();
		static const std::string none;
		const size_t c = size_t(consumer);
		return c < kConsumerCount && (causeMask & kCauseAll) != 0 ? keys[c * (kCauseAll + 1) + (causeMask & kCauseAll)] : none;
	}

	// `snapshot` may be null (no producer at all).
	//
	// The per-frame path builds no string, engaged or not: the result lives in a per-thread, per-consumer cache and is returned by
	// reference (valid until the next Read of the same consumer on the same thread; copy it to keep it, as the consumers do). The
	// reason and the Engaged decision are rebuilt ONLY when what they say changes: which products are missing and for which cause
	// (a superset of the cause mask: two products missing for the same cause give another text). The caller's copy-assignment into its
	// own ReadResult reuses its string capacity, so a steady engaged read does not allocate there either.
	inline const ReadResult& Read(Consumer consumer, const Snapshot* snapshot, bool requested, int64_t nowUs)
	{
		struct Cache
		{
			ReadResult result;
			bool primed = false;    // result.reason / result.fallback describe `noEngine` + `codes`
			bool noEngine = false;
			unsigned char codes[kCount] = {};   // per product index: 0 usable, 1 absent, 2 unit version, 3 stale
		};
		static thread_local Cache caches[kConsumerCount];
		Cache& c = caches[size_t(consumer) < kConsumerCount ? size_t(consumer) : 0u];
		ReadResult& r = c.result;
		r.requested = requested;
		r.usable = false;
		r.missingMask = 0;
		r.causeMask = 0;
		if (!requested)
		{
			r.reason.clear();            // clear() keeps the capacity: no allocation
			r.fallback.id.clear();
			r.fallback.reason.clear();
			r.fallback.engaged = false;
			c.primed = false;
			return r;
		}
		unsigned char codes[kCount] = {};
		const bool noEngine = snapshot == nullptr;
		for (ProductId p : NeededBy(consumer))
		{
			if (noEngine)
			{
				r.missingMask |= 1u << unsigned(IndexOf(p));
				continue;
			}
			const unsigned cause = CauseOf(snapshot->Of(p), ContractOf(p)->unitVersion, nowUs);
			if (!cause)
				continue;
			r.missingMask |= 1u << unsigned(IndexOf(p));
			r.causeMask |= cause;
			codes[IndexOf(p)] = cause == kCauseAbsent ? 1u : cause == kCauseUnitVersion ? 2u : 3u;
		}
		if (noEngine)
			r.causeMask = kCauseNoEngine;
		if (r.missingMask == 0)
		{
			r.usable = true;
			r.reason.clear();
			r.fallback.id.clear();
			r.fallback.reason.clear();
			r.fallback.engaged = false;
			c.primed = false;
			return r;
		}
		const bool same = c.primed && c.noEngine == noEngine && std::memcmp(c.codes, codes, sizeof(codes)) == 0;
		if (!same)
		{
			r.reason.clear();
			if (noEngine)
				r.reason = CauseWord(kCauseNoEngine);
			else
				for (ProductId p : NeededBy(consumer))
				{
					const unsigned code = codes[IndexOf(p)];
					if (!code)
						continue;
					r.reason += (r.reason.empty() ? "" : "; ") + std::string(ContractOf(p)->token) + " "
					            + CauseWord(code == 1u ? kCauseAbsent : code == 2u ? kCauseUnitVersion : kCauseStale);
				}
			// Keyed by consumer + cause (never the consumer's bare id: that is the key of its own classical cause) and worded for the
			// engine's cause: the existing entry's text names another cause (e.g. a missing chroma split) and would be false here.
			r.fallback = FallbackCatalog::Engaged(FallbackKey(consumer, r.causeMask).c_str(),
				std::string("temporal-detail products unavailable, classical form runs: ") + r.reason);
			std::memcpy(c.codes, codes, sizeof(codes));
			c.noEngine = noEngine;
			c.primed = true;
		}
		return r;
	}

	// Applies a read to a registry: engages the key of its cause, and releases every OTHER cause key of the consumer (a read that
	// moved from "absent" to "stale" replaces the entry, one that recovered removes it). The consumer's bare id is never touched
	// (it belongs to its own classical cause). True when the heading changed.
	inline bool SyncRead(FallbackStatus::Registry& registry, Consumer consumer, const ReadResult& r)
	{
		bool changed = false;
		const std::string& engagedKey = r.fallback.engaged ? FallbackKey(consumer, r.causeMask) : FallbackKey(consumer, 0);
		for (unsigned m = 1; m <= kCauseAll; ++m)
		{
			const std::string& key = FallbackKey(consumer, m);
			if (r.fallback.engaged && key == engagedKey)
				continue;
			changed = registry.Release(key) || changed;
		}
		if (r.fallback.engaged)
			changed = FallbackCatalog::Sync(registry, r.fallback) || changed;
		return changed;
	}

	// The status line of a consumer under a read: its fallback wording while the classical form stands in, "" otherwise.
	inline std::string StatusLine(Consumer consumer, const ReadResult& r)
	{
		return r.fallback.engaged ? std::string(Name(consumer)) + ": classical form (temporal-detail products: " + r.reason + ")"
		                          : std::string();
	}

	// The reset-1 read of a consumer: the placeholder producer, the engine not requested. Byte-identical to not reading at all.
	inline ReadResult ReadToday(Consumer consumer)   // by value: engine not requested, every string empty (no allocation)
	{
		return Read(consumer, PlaceholderSnapshot(), kEngineRequestedToday, 0);
	}
}
