// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// COMPUTE-ONCE detector 3 (refs/129 addendum 2026-11-13; interface D:\codex\vp-handover\lead7\iface-L-DET.md section 2):
// every GPU pass is attributed to an owning product. A pass is a list of steps (libplacebo's pl_shader_info::steps: the
// friendly names of its operations; a VP pass names its own). Each step is matched against the rule table (substring,
// case-sensitive, first rule wins); a step no rule matches takes the dispatch's default owner (GpuAttributionPl.h Watch),
// and with no default owner it is UNATTRIBUTED: hidden work, listed with the pass's GPU time.
// Kind::PrivateCopy marks a step that recomputes a shared unit (a hidden copy), listed under that unit.
//
// Fed only while ComputeOnce::Enabled() (the PassTimerHook info_callback inside pl_render_image and the pl_dispatch
// callbacks of watched VP dispatch objects). Pure C++14, no libplacebo or Windows headers.

#include <VP-V/core/ComputeOnce.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace GpuAttribution
{
	enum class Kind { Owner, PrivateCopy };

	struct Rule
	{
		std::string pattern;   // substring of a step
		std::string owner;     // a ProductCatalog token, or "pathway:<name>"
		Kind kind = Kind::Owner;
		std::string unit;      // PrivateCopy: the shared unit recomputed (a bare "U<n>" id)
		mutable uint64_t hits = 0;   // passes charged by this rule (the report's non-vacuity rows; under the registry mutex)
	};

	struct OwnerRow { std::string owner; uint64_t passes = 0; uint64_t ns = 0; };
	struct StepRow { std::string step; std::string owner; std::string unit; std::string example; uint64_t passes = 0; uint64_t ns = 0; };

	// libplacebo's own render steps (sh_describe names of 7.360): Bill's output pathway, owned by the renderer core.
	inline std::vector<Rule> SeedRules()
	{
		static const char* const kPathway[] = { "scaling (", "tone map", "gamut map", "color decoding", "color encoding",
			"colorspace conversion", "dithering", "error diffusion", "debanding", "deinterlacing", "frame mixing", "merging planes",
			"overlay", "draw border", "peak detection", "film grain", "ICC 3DLUT", "custom 1DLUT", "custom 3DLUT", "distortion",
			"cone distortion", "reshaping", "feature extraction", "blur downscale pass", "blur upscale pass", "oversample",
			"bicubic", "bilinear", "nearest", "hermite", "gaussian" };
		std::vector<Rule> rules;
		for (const char* p : kPathway)
		{
			Rule r;
			r.pattern = p;
			r.owner = "pathway:libplacebo_render";
			rules.push_back(r);
		}
		// VP render-pass hooks inside pl_render_image (PassTimerHook has no per-hook defaultOwner).
		{
			Rule r;
			r.pattern = "VP extended range:";
			r.owner = "extended_range";
			rules.insert(rules.begin(), r);
		}
		// VP passes that recompute a shared unit privately (hidden copies, tools/check_shared_units.py ids).
		static const char* const kPrivate[][3] = {
			{ "motion (D1", "dejudder", "U10" },
			{ "deband: grain split", "deband", "U1" },
			{ "deband: grain guard", "deband", "U5" },   // VpDebander.cpp GUARD_BODY pass (refs/126 s3: the one REAL U5 site)
			{ "adaptive downscale: measure", "adaptive_downscale", "U5" },   // MEASURE_BODY flat-pixel energy, also the U4 edge term (refs/126 s4 X5)
			{ "texture restore: half", "texture_restore", "U3" },
			{ "texture restore: low bands", "texture_restore", "U2" },
			{ "texture restore: tile statistics", "texture_restore", "U4" },
			{ "texture restore: cuts and fades", "texture_restore", "U7" } };
		for (const auto& p : kPrivate)
		{
			Rule r;
			r.pattern = p[0];
			r.owner = p[1];
			r.kind = Kind::PrivateCopy;
			r.unit = p[2];
			rules.insert(rules.begin(), r);
		}
		return rules;
	}

	namespace detail
	{
		inline const char*& SuppressedPrivateCopyUnit()
		{
			static thread_local const char* unit = nullptr;
			return unit;
		}
	}

	// Scoped thread-local suppression of PrivateCopy charging for a named shared unit (e.g. warm-up pre-compile
	// of a shader whose pass stays attributed to its owner without being counted as a hidden private copy).
	class SuppressPrivateCopyScope
	{
	public:
		explicit SuppressPrivateCopyScope(const char* unit)
			: prev_(detail::SuppressedPrivateCopyUnit())
		{
			if (unit != nullptr)
				detail::SuppressedPrivateCopyUnit() = unit;
		}
		~SuppressPrivateCopyScope()
		{
			detail::SuppressedPrivateCopyUnit() = prev_;
		}
		SuppressPrivateCopyScope(const SuppressPrivateCopyScope&) = delete;
		SuppressPrivateCopyScope& operator=(const SuppressPrivateCopyScope&) = delete;
	private:
		const char* prev_;
	};

	class Registry
	{
	public:
		Registry() : rules_(SeedRules()) {}

		void AddRule(const std::string& pattern, const std::string& owner, Kind kind = Kind::Owner, const std::string& unit = std::string())
		{
			std::lock_guard<std::mutex> lock(mutex_);
			Rule r;
			r.pattern = pattern;
			r.owner = owner;
			r.kind = kind;
			r.unit = unit;
			// A lane's own rows go in front of the seed rows (a VP step naming e.g. "scaling (" is the VP owner's).
			rules_.insert(rules_.begin() + static_cast<std::ptrdiff_t>(added_++), r);
		}

		// One completed pass: its steps, its full description (for the report), its GPU time, the dispatch's owner (or null).
		void Note(const char* const* steps, int numSteps, const char* description, uint64_t ns, const char* defaultOwner)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			++passes_;
			std::string owner;
			bool unattributed = false;
			const int n = steps ? numSteps : 0;
			if (n <= 0)
			{
				// A pass naming no step: only its description (or nothing) identifies it.
				const std::string step = description && description[0] ? description : "(unnamed pass)";
				const Rule* r = Match(step);
				if (r)
					Charge(*r, step, description, ns, owner);
				else if (defaultOwner)
					owner = defaultOwner;
				else
				{
					AddStep(unattributed_, step, std::string(), std::string(), description, ns);
					unattributed = true;
				}
			}
			for (int i = 0; i < n; ++i)
			{
				const std::string step = steps[i] ? steps[i] : "";
				const Rule* r = Match(step);
				if (r)
					Charge(*r, step, description, ns, owner);
				else if (defaultOwner)
				{
					if (owner.empty())
						owner = defaultOwner;
				}
				else
				{
					AddStep(unattributed_, step, std::string(), std::string(), description, ns);
					unattributed = true;
				}
			}
			if (unattributed)
				++unattributedPasses_;
			if (!owner.empty())
			{
				OwnerRow& o = owners_[owner];
				o.owner = owner;
				++o.passes;
				o.ns += ns;
			}
		}

		std::vector<OwnerRow> Owners() const { std::lock_guard<std::mutex> lock(mutex_); return Values(owners_); }
		std::vector<StepRow> Unattributed() const { std::lock_guard<std::mutex> lock(mutex_); return Values(unattributed_); }
		std::vector<StepRow> PrivateCopies() const { std::lock_guard<std::mutex> lock(mutex_); return Values(private_); }
		uint64_t Passes() const { std::lock_guard<std::mutex> lock(mutex_); return passes_; }
		uint64_t UnattributedPasses() const { std::lock_guard<std::mutex> lock(mutex_); return unattributedPasses_; }
		void NoteFrame() { std::lock_guard<std::mutex> lock(mutex_); ++frames_; }
		uint64_t Frames() const { std::lock_guard<std::mutex> lock(mutex_); return frames_; }
		std::vector<Rule> Rules() const { std::lock_guard<std::mutex> lock(mutex_); return rules_; }
		void Reset()
		{
			std::lock_guard<std::mutex> lock(mutex_);
			owners_.clear();
			unattributed_.clear();
			private_.clear();
			passes_ = unattributedPasses_ = frames_ = 0;
			for (const Rule& r : rules_)
				r.hits = 0;
		}

	private:
		template<class M> static std::vector<typename M::mapped_type> Values(const M& m)
		{
			std::vector<typename M::mapped_type> out;
			for (const auto& kv : m)
				out.push_back(kv.second);
			return out;
		}
		const Rule* Match(const std::string& step) const
		{
			for (const Rule& r : rules_)
				if (!r.pattern.empty() && step.find(r.pattern) != std::string::npos)
					return &r;
			return nullptr;
		}
		void Charge(const Rule& r, const std::string& step, const char* description, uint64_t ns, std::string& owner)
		{
			const char* sup = detail::SuppressedPrivateCopyUnit();
			if (!(r.kind == Kind::PrivateCopy && sup != nullptr && r.unit == sup))
			{
				++r.hits;
				if (r.kind == Kind::PrivateCopy)
					AddStep(private_, step, r.owner, r.unit, description, ns);
			}
			// The pass's owner: the first VP owner of its steps, else the pathway.
			if (owner.empty() || (owner.compare(0, 8, "pathway:") == 0 && r.owner.compare(0, 8, "pathway:") != 0))
				owner = r.owner;
		}
		static void AddStep(std::map<std::string, StepRow>& m, const std::string& step, const std::string& owner,
			const std::string& unit, const char* description, uint64_t ns)
		{
			StepRow& s = m[step];
			if (s.step.empty())
			{
				s.step = step;
				s.owner = owner;
				s.unit = unit;
				s.example = description ? description : "";
			}
			++s.passes;
			s.ns += ns;
		}

		mutable std::mutex mutex_;
		std::vector<Rule> rules_;
		size_t added_ = 0;
		std::map<std::string, OwnerRow> owners_;
		std::map<std::string, StepRow> unattributed_;
		std::map<std::string, StepRow> private_;
		uint64_t passes_ = 0, unattributedPasses_ = 0, frames_ = 0;
	};

	namespace detail
	{
		template<class T> struct Global { static Registry registry; };
		template<class T> Registry Global<T>::registry;
	}
	inline Registry& Global() { return detail::Global<void>::registry; }

	inline void Note(const char* const* steps, int numSteps, const char* description, uint64_t ns, const char* defaultOwner)
	{
		if (ComputeOnce::Enabled())
			Global().Note(steps, numSteps, description, ns, defaultOwner);
	}

	// One GPU pass issued outside libplacebo (raw D3D11 Dispatch, NVOF Execute): counted and attributed to its owner,
	// untimed (ms 0) - no pl_dispatch callback exists for it. One line after the submit; off = one relaxed load.
	inline void NoteExternal(const char* step, const char* owner)
	{
		if (ComputeOnce::Enabled())
		{
			const char* steps[1] = { step };
			Global().Note(steps, 1, step, 0, owner);
		}
	}
}
