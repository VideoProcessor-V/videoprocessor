#include <pch.h>
#include "OutputSegmentsMerge.h"

namespace OutputSegments
{
	using SegmentRunner::Mode;

	namespace
	{
		// A cheap signature of what the segment checks depend on (audit L3): the module order, which
		// slots run, and the switch. No strings, no allocation.
		uint64_t Signature(const PipelineSlots::Registry& registry, bool mergedWanted)
		{
			uint64_t h = 14695981039346656037ull;
			auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
			for (PipelineSlots::Id id : registry.CurrentOrder())
				mix((static_cast<uint64_t>(id) << 1) | (registry.Runs(id) ? 1u : 0u));
			mix(mergedWanted ? 0x5a : 0xa5);
			return h;
		}
	}

	Merger::Merger(pl_gpu gpu, OutputColourSegment& s2In, OutputFinishSegment& s3In)
		: s2(s2In), s3(s3In), runner(gpu)
	{
	}

	bool Merger::Configure(const PipelineSlots::Registry& registry, bool mergedWanted, uint32_t sampleEvery,
		FallbackStatus::Registry& registryOfFallbacks)
	{
		fallbacks = &registryOfFallbacks;
		fallbacksChanged = false;
		runner.SetSampleEvery(sampleEvery);
		const uint64_t signature = Signature(registry, mergedWanted);
		const bool a = s2.Segment().Configure(registry, mergedWanted, signature, registryOfFallbacks) == Mode::Merged;
		const bool b = s3.Segment().Configure(registry, mergedWanted, signature, registryOfFallbacks) == Mode::Merged;
		fallbacksChanged = s2.Segment().FallbackChanged() || s3.Segment().FallbackChanged();
		// A settings change: stages that failed inside a merged hook get another chance (audit H1b).
		if (s2.Segment().Rechecked() || s3.Segment().Rechecked())
			runner.ClearFailed();
		active = a || b;
		return active;
	}

	bool Merger::Member(const pl_hook* hook) const
	{
		return (s2.Segment().CurrentMode() == Mode::Merged && s2.Member(hook)) ||
			(s3.Segment().CurrentMode() == Mode::Merged && s3.Member(hook));
	}

	SegmentRunner::Segment& Merger::Owner(const pl_hook* hook)
	{
		// Matched at failure time against the segments' own members (audit L6).
		return s2.Member(hook) ? s2.Segment() : s3.Segment();
	}

	std::string Merger::Describe(const pl_hook* hook) const
	{
		const PipelineSlots::Id slot = s2.SlotOf(hook);
		if (slot != PipelineSlots::Id::Count)
			return PipelineSlots::Get(slot).name;
		return s3.StageName(hook);
	}

	void Merger::Downgrade(SegmentRunner::Segment& segment, const std::string& reason)
	{
		if (!fallbacks)
			return;
		segment.Downgrade(reason, *fallbacks);
		fallbacksChanged = fallbacksChanged || segment.FallbackChanged();
		active = s2.Segment().CurrentMode() == Mode::Merged || s3.Segment().CurrentMode() == Mode::Merged;
	}

	// libplacebo disabled a merged hook (audit H2): its stages would vanish together, so the owning
	// segment returns to the separate form at once.
	void Merger::CheckDisabled(const pl_render_errors* errors)
	{
		for (const std::vector<const pl_hook*>& run : runner.DisabledRuns(errors))
		{
			if (run.empty())
				continue;
			std::string stages;
			for (const pl_hook* h : run)
				stages += (stages.empty() ? "" : ", ") + Describe(h);
			Downgrade(Owner(run.front()), "libplacebo disabled the merged hook (" + stages + ")");
		}
	}

	Merger::Scope::Scope(Merger* merger, pl_render_params& p, const pl_render_errors* errors)
		: params(p), hooks(p.hooks), count(p.num_hooks)
	{
		if (!merger)
			return;
		merger->CheckDisabled(errors);
		if (!merger->active || !p.hooks || p.num_hooks < 2)
			return;
		Merger& m = *merger;
		m.in.assign(p.hooks, p.hooks + p.num_hooks);
		m.runner.Fuse(m.in, m.merged, [&m](const pl_hook* h) { return m.Member(h); }, errors);
		p.hooks = m.merged.empty() ? nullptr : m.merged.data();
		p.num_hooks = static_cast<int>(m.merged.size());
	}

	Merger::Scope::~Scope()
	{
		params.hooks = hooks;
		params.num_hooks = count;
	}

	std::vector<uint64_t> Merger::Report(const pl_render_errors* errors)
	{
		std::vector<uint64_t> failed;
		CheckDisabled(errors);
		for (const HookSegment::Failure& f : runner.TakeFailures())
		{
			failed.push_back(f.signature);
			if (f.hook)
				Downgrade(Owner(f.hook), Describe(f.hook) + ": " + f.reason);
		}
		for (const auto& t : runner.TakeTimings())
		{
			const PipelineSlots::Id slot = s2.SlotOf(t.first);
			if (slot != PipelineSlots::Id::Count)
				s2.Segment().Timing().Poll(slot, t.second);
			else if (s3.Member(t.first))
				s3.Segment().Timing().Poll(PipelineSlots::Id::OutputDither, t.second);
		}
		return failed;
	}
}
