// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// P10 D5 phase A, carrier (a) (lead ruling, design-check D5): the CORE-LEVEL interface through which the queue walk hands the
// frames queued ahead of the render to the P13 source ring (input.capture). 0 Bill lines, 0 hook-file lines: the walk
// (MotionAhead::CollectQueueAhead, the existing S2 call site) finds the feed of its host here, through HostSlots, exactly as
// it finds any other host-keyed module state; RendererExtension.h carries nothing new (no QueueView::ahead, no AheadSlot).
//
// The ring's DEFINITION is core/RingContract.h (SourceRingContract: a seq-keyed window of BACK + 1 + LEAD frames, both
// derived); input.capture's SourceRing is its storage. This header only names the one frame record and the one port.
//
//   AheadFrame     one queued frame as the walk sees it: seq, size and the frame's extension slot, OPAQUE here (motion never
//                  includes capture: the payload is decoded by input.capture's ring). Valid for the Feed call only.
//   ISourceRingFeed  what a ring implements: Wanted() (one acquire load: the walk builds no list when nobody wants one) and
//                  Feed() (render-loop thread, queue lock held: allocation-free, destroys nothing).
//   SourceRingFeeds  the per-host table of feeds (Register at the host binding, Unregister at its end, Find on any thread).
//
// Header-only, C++14.

#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/hub/HostSlots.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace VpSkeleton
{
	struct AheadFrame
	{
		uint64_t seq = 0;                       // the frame's source sequence
		int32_t width = 0;                      // the frame width in pixels (0 = not delivered)
		int32_t height = 0;                     // the frame height in pixels (0 = not delivered)
		std::shared_ptr<void> extension;        // the queued frame's raw extension slot, passed OPAQUELY (input.capture decodes it)
	};

	// The most frames one Feed pass carries: the walk's stack array (the ahead ceiling plus the front frame).
	constexpr size_t kMaxFeedFrames = static_cast<size_t>(kAheadCeiling) + 1u;

	class ISourceRingFeed
	{
	public:
		virtual ~ISourceRingFeed() {}
		virtual bool Wanted() const = 0;
		// `frames[0..count)`: the queue's frames from the front, at most kMaxFeedFrames. An empty list is an update too.
		virtual void Feed(const AheadFrame* frames, size_t count) = 0;
	};

	// The per-host port (HostSlots needs a `host` member).
	struct SourceRingFeedPort
	{
		const void* host = nullptr;
		std::shared_ptr<ISourceRingFeed> feed;
	};

	class SourceRingFeeds
	{
	public:
		static bool Register(const void* host, std::shared_ptr<ISourceRingFeed> feed)
		{
			std::shared_ptr<SourceRingFeedPort> port = std::make_shared<SourceRingFeedPort>();
			port->host = host;
			port->feed = std::move(feed);
			return Table().Put(host, port);
		}
		static void Unregister(const void* host) { Table().Remove(host, true); }
		// Any thread. Null for an unknown host.
		static std::shared_ptr<ISourceRingFeed> Find(const void* host)
		{
			const std::shared_ptr<SourceRingFeedPort> port = Table().Find(host);
			return port ? port->feed : nullptr;
		}

	private:
		static Vpv::Hub::HostSlots<SourceRingFeedPort>& Table()
		{
			static Vpv::Hub::HostSlots<SourceRingFeedPort> table;
			return table;
		}
	};
}
