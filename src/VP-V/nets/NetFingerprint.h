#pragma once

#include "NetPackage.h"

#include <VP-V/core/FallbackStatus.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

// The net fingerprint check (vp-neural-bounded: "a per-net compatibility stamp checked at load";
// barrier B1/R3/U3; ADR-0021 for the upstream key).
//
// Why: a net is trained on EXACT pipeline states. If anything in front of it changes (label
// schema, the scaler's pass order H->V, the intermediate format, the stage contract, the modules
// upstream of it and their settings) its training no longer applies, and VP must bypass THAT NET
// ONLY, visibly: "Fallbacks engaged: nets: bypassed (<net>: <reason>)" (vp-no-silent-fallbacks).
//
// The runtime side is a NetRuntimeContext the renderer fills from what it actually runs (the
// SlotRegistry's UpstreamKey for the net's slot, the format the runner picked, the label schema
// VP's identification supplies, the settings hashes). Check() compares field by field with a
// per-field requirement so a field can be recorded without gating (the VP commit, by default).
// Header-only, pure, C++14.
namespace VpNets
{
	enum class Requirement { Must, IfDeclared, Ignore };

	struct FingerprintPolicy
	{
		Requirement labelSchema = Requirement::Must;      // only for nets that declare labels (PARAMs)
		Requirement passOrder = Requirement::Must;
		Requirement intermediate = Requirement::Must;
		Requirement stageContract = Requirement::Must;
		Requirement upstreamKey = Requirement::Must;      // the round-1/2 exports carry none -> bypass
		Requirement settingsHashes = Requirement::Must;   // stage_order, rebase_settings, grain_split, ...
		// The build stamp: a new VP commit must not bypass every net, so the commit and the
		// renderer hash are recorded (shown in Ctrl+Alt+D) but do not gate unless asked (a bench
		// that must reproduce training exactly sets Must).
		Requirement vpCommit = Requirement::Ignore;
		Requirement rendererHash = Requirement::Ignore;
	};

	// What VP runs, as the fingerprint sees it.
	struct NetRuntimeContext
	{
		std::string labelSchema;                          // what VP's identification supplies ("vpnn-labels-v1")
		std::string passOrder = "H>V";                    // VP's separable scaler / re-base pass order
		TexFormat intermediate = TexFormat::Rgba16f;      // the runner's intermediate format (fp16 by default)
		std::string stageContract;                        // the contract VP implements for this net's slot
		std::string upstreamKey;                          // SlotRegistry::UpstreamKey(SlotOf(role))
		std::map<std::string, std::string> settingsHashes;
		std::string vpCommit;
		std::string rendererHash;
		FingerprintPolicy policy;
	};

	// The settings-hash fields the kit writes (U3: VP must compute the same hashes before nets run).
	inline const std::vector<std::string>& SettingsHashKeys()
	{
		static const std::vector<std::string> keys = { "stage_order", "rebase_settings", "grain_split", "scaler_blend_settings", "bounds" };
		return keys;
	}

	struct FingerprintResult
	{
		bool match = false;
		std::vector<std::string> reasons;   // every mismatch, in check order
		// One short line for the "Fallbacks engaged" entry: the first reason (+N more).
		std::string Summary() const
		{
			if (match || reasons.empty()) return std::string();
			std::string s = reasons.front();
			if (reasons.size() > 1) s += " (+" + std::to_string(reasons.size() - 1) + " more)";
			return s;
		}
	};

	namespace FingerprintDetail
	{
		inline std::string Short(const std::string& s, size_t n = 24)
		{
			return s.size() <= n ? s : s.substr(0, n) + "...";
		}

		// Compare one field. `declared` empty = the export does not carry it.
		inline void Field(FingerprintResult& r, Requirement req, const char* what,
			const std::string& declared, const std::string& running)
		{
			if (req == Requirement::Ignore) return;
			if (declared.empty())
			{
				if (req == Requirement::Must)
					r.reasons.push_back(std::string("export has no ") + what);
				return;
			}
			if (declared != running)
				r.reasons.push_back(std::string(what) + " " + (running.empty() ? std::string("(none)") : Short(running)) +
					" != trained " + Short(declared));
		}
	}

	// MATCH / MISMATCH(reasons) for one package against what VP runs.
	inline FingerprintResult CheckFingerprint(const NetPackage& pkg, const NetRuntimeContext& ctx)
	{
		using FingerprintDetail::Field;
		FingerprintResult r;
		const NetFingerprint& fp = pkg.fingerprint;
		const FingerprintPolicy& pol = ctx.policy;

		if (fp.id.empty())
			r.reasons.push_back("export has no fingerprint");

		// Labels: a net with PARAM labels must say which schema they follow.
		if (!pkg.contract.labels.empty())
			Field(r, pol.labelSchema, "label schema", fp.labelSchema, ctx.labelSchema);
		else if (!fp.labelSchema.empty() && pol.labelSchema != Requirement::Ignore && fp.labelSchema != ctx.labelSchema)
			r.reasons.push_back("label schema " + ctx.labelSchema + " != trained " + fp.labelSchema);

		Field(r, pol.passOrder, "pass order", fp.passOrder, ctx.passOrder);

		// Intermediates: the trained format, or fp16 where the export proved fp16 within 1 LSB.
		if (pol.intermediate != Requirement::Ignore)
		{
			if (fp.intermediate == TexFormat::Unknown)
			{
				if (pol.intermediate == Requirement::Must) r.reasons.push_back("export has no intermediate format");
			}
			else if (fp.intermediate != ctx.intermediate &&
				!(fp.intermediate == TexFormat::Rgba32f && ctx.intermediate == TexFormat::Rgba16f && fp.fp16Allowed))
			{
				r.reasons.push_back(std::string("intermediates ") + FormatName(ctx.intermediate) + " != trained " +
					FormatName(fp.intermediate) + (ctx.intermediate == TexFormat::Rgba16f ? " (fp16 not proven within 1 LSB)" : ""));
			}
		}

		Field(r, pol.stageContract, "stage contract", fp.Field("stage_contract"), ctx.stageContract);
		Field(r, pol.upstreamKey, "upstream key", fp.Field("upstream_key"), ctx.upstreamKey);
		for (const std::string& key : SettingsHashKeys())
		{
			const auto it = ctx.settingsHashes.find(key);
			Field(r, pol.settingsHashes, key.c_str(), fp.Field(key), it == ctx.settingsHashes.end() ? std::string() : it->second);
		}
		Field(r, pol.vpCommit, "VP commit", fp.Field("vp_commit"), ctx.vpCommit);
		Field(r, pol.rendererHash, "renderer build", fp.Field("renderer_dll_sha256"), ctx.rendererHash);

		r.match = r.reasons.empty();
		return r;
	}

	// The runtime context that matches a package exactly (tests, and the bench that reproduces
	// training). The upstream key is taken from the export when it has one.
	inline NetRuntimeContext ContextMatching(const NetPackage& pkg)
	{
		NetRuntimeContext c;
		const NetFingerprint& fp = pkg.fingerprint;
		c.labelSchema = fp.labelSchema;
		c.passOrder = fp.passOrder;
		c.intermediate = fp.intermediate == TexFormat::Unknown ? TexFormat::Rgba32f : fp.intermediate;
		c.stageContract = fp.Field("stage_contract");
		c.upstreamKey = fp.Field("upstream_key");
		for (const std::string& key : SettingsHashKeys())
			c.settingsHashes[key] = fp.Field(key);
		c.vpCommit = fp.Field("vp_commit");
		c.rendererHash = fp.Field("renderer_dll_sha256");
		return c;
	}

	// "Fallbacks engaged: nets: bypassed (A_single_small: label schema ...; C_rebase_refine_h: ...)".
	// One entry for all nets, so the heading stays short; each net's reason is kept separately
	// and the entry is released when no net is bypassed.
	class NetBypassSet
	{
	public:
		static constexpr const char* kId = "nets: bypassed";

		explicit NetBypassSet(FallbackStatus::Registry& registry) : registry(registry) {}

		// Returns true when the heading changed (log once).
		bool Bypass(const std::string& net, const std::string& reason)
		{
			auto it = reasons.find(net);
			if (it != reasons.end() && it->second == reason) return false;
			if (it == reasons.end()) order.push_back(net);
			reasons[net] = reason;
			return Publish();
		}
		bool Clear(const std::string& net)
		{
			if (!reasons.erase(net)) return false;
			order.erase(std::remove(order.begin(), order.end(), net), order.end());
			return Publish();
		}
		bool Bypassed(const std::string& net) const { return reasons.count(net) != 0; }

		// Applies a fingerprint result: a mismatch bypasses, a match clears.
		bool Apply(const std::string& net, const FingerprintResult& r)
		{
			return r.match ? Clear(net) : Bypass(net, r.Summary());
		}

	private:
		bool Publish()
		{
			if (order.empty()) return registry.Release(kId);
			std::string text;
			for (const std::string& n : order)
			{
				if (!text.empty()) text += ", ";
				text += n + ": " + reasons[n];
			}
			return registry.Engage(kId, text);
		}

		FallbackStatus::Registry& registry;
		std::map<std::string, std::string> reasons;
		std::vector<std::string> order;
	};
}
