#include <pch.h>
#include "OutputSegmentsMerge.h"

namespace OutputSegments
{
	using SegmentRunner::Mode;

	Merger::Merger(pl_gpu gpu, OutputColourSegment& s2In, OutputFinishSegment& s3In)
		: s2(s2In), s3(s3In), runner(gpu)
	{
	}

	bool Merger::Configure(const PipelineSlots::Registry& registry, bool mergedWanted, bool float32, uint32_t sampleEvery,
		uint64_t signature, FallbackStatus::Registry& fallbacks)
	{
		runner.SetFloat32(float32);
		runner.SetSampleEvery(sampleEvery);
		const bool a = s2.Segment().Configure(registry, mergedWanted, signature, fallbacks) == Mode::Merged;
		const bool b = s3.Segment().Configure(registry, mergedWanted, signature, fallbacks) == Mode::Merged;
		active = a || b;
		return active;
	}

	bool Merger::Member(const pl_hook* hook) const
	{
		return (s2.Segment().CurrentMode() == Mode::Merged && s2.Member(hook)) ||
			(s3.Segment().CurrentMode() == Mode::Merged && s3.Member(hook));
	}

	Merger::Scope::Scope(Merger* merger, pl_render_params& p, const pl_render_errors* errors)
		: params(p), hooks(p.hooks), count(p.num_hooks)
	{
		if (!merger || !merger->active || !p.hooks || p.num_hooks < 2)
			return;
		Merger& m = *merger;
		m.in.assign(p.hooks, p.hooks + p.num_hooks);
		m.lastMembers.clear();
		for (const pl_hook* h : m.in)
			if (h && m.Member(h))
				m.lastMembers.emplace_back(h->signature, m.s2.Member(h));
		m.runner.Fuse(m.in, m.merged, [&m](const pl_hook* h) { return m.Member(h); }, errors);
		p.hooks = m.merged.empty() ? nullptr : m.merged.data();
		p.num_hooks = static_cast<int>(m.merged.size());
	}

	Merger::Scope::~Scope()
	{
		params.hooks = hooks;
		params.num_hooks = count;
	}

	void Merger::Report(FallbackStatus::Registry& fallbacks)
	{
		for (const HookSegment::Failure& f : runner.TakeFailures())
		{
			bool inS2 = false;
			for (const auto& m : lastMembers)
				if (m.first == f.signature)
					inS2 = m.second;
			(inS2 ? s2.Segment() : s3.Segment()).Downgrade(f.reason, fallbacks);
		}
		for (const auto& t : runner.TakeTimings())
		{
			const PipelineSlots::Id slot = s2.SlotOf(t.first);
			if (slot != PipelineSlots::Id::Count)
				s2.Segment().Timing().Poll(slot, t.second);
			else if (s3.Member(t.first))
				s3.Segment().Timing().Poll(PipelineSlots::Id::OutputDither, t.second);
		}
		active = s2.Segment().CurrentMode() == Mode::Merged || s3.Segment().CurrentMode() == Mode::Merged;
	}
}
