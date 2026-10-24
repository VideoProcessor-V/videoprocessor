#pragma once

#include "NetFingerprint.h"
#include "NetRunner.h"

#include <string>

// NetSlot: one net in one slot, from package to per-frame run, with every refusal visible.
//
// Why: the load-time rules (capabilities -> intermediate format -> fingerprint -> prepare) and
// the per-frame rules (contract inputs, the 8K rule, A_motion's neighbour fallback) are the same
// for every net and both run sites, so they live in one place the renderer calls instead of
// being re-implemented per slot. Any refusal bypasses THIS net only and is published through
// NetBypassSet ("Fallbacks engaged: nets: bypassed (<net>: <reason>)"); a recovery clears it.
// Header-only, C++14; the runner (CPU or pl_dispatch) is injected.
namespace VpNets
{
	class NetSlot
	{
	public:
		NetSlot(INetRunner& runner, NetBypassSet& bypass) : runner(runner), bypass(bypass) {}

		// Load time (and whenever the pipeline state changes: a new UpstreamKey, settings, format).
		// `ctx.intermediate` is overwritten with the format the capability check picks.
		bool Load(const NetDeviceContext& device, const NetPackage& package, NetRuntimeContext ctx)
		{
			pkg = package;
			ready = false;
			const CapabilityResult caps = CheckCapabilities(pkg, runner.Capabilities(device));
			if (!caps.ok)
				return Refuse(caps.reason);
			ctx.intermediate = caps.intermediate;
			lastCheck = CheckFingerprint(pkg, ctx);
			if (!lastCheck.match)
				return Refuse(lastCheck.Summary());
			NetPrepareParams pp;
			pp.intermediate = caps.intermediate;
			pp.output = output;
			std::string err;
			if (!runner.Prepare(device, pkg, pp, &err))
				return Refuse(err);
			intermediate = caps.intermediate;
			ready = true;
			bypass.Clear(pkg.name);
			return true;
		}

		// Per frame. True = `out` holds the net output; false = the caller keeps its input (the net is
		// bypassed for this frame, visibly when it is a fault; the 8K rule is a designed bypass and
		// is shown too, so the operator knows the nets are not running).
		bool Run(const NetDeviceContext& device, const NetRunInputs& in, NetTexture& out)
		{
			if (!ready) return false;
			const ResolvedInputs r = ResolveInputs(pkg, in);
			if (!r.ok) return Refuse(r.reason, false);
			if (r.bypass) return Refuse(r.reason, false);
			std::string err;
			if (!runner.Run(device, in, out, &err)) return Refuse(err, false);
			bypass.Clear(pkg.name);
			return true;
		}

		bool Ready() const { return ready; }
		TexFormat Intermediate() const { return intermediate; }
		const FingerprintResult& LastFingerprint() const { return lastCheck; }
		void SetOutputFormat(TexFormat f) { output = f; }   // R16Unorm: the r16 hand-off between re-base axes

	private:
		bool Refuse(const std::string& reason, bool unload = true)
		{
			if (unload) ready = false;
			bypass.Bypass(pkg.name.empty() ? std::string("net") : pkg.name, reason);
			return false;
		}

		INetRunner& runner;
		NetBypassSet& bypass;
		NetPackage pkg;
		FingerprintResult lastCheck;
		TexFormat intermediate = TexFormat::Unknown;
		TexFormat output = TexFormat::Unknown;
		bool ready = false;
	};
}
