// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera link layer (K0): the ONE caller of FrameAnalysis::Store::PublishCamera. The skeleton seal of
// P7 (camera fit) hands the sealed record here; a record the seal rejected (stamps, span, ancestry)
// arrives as null and is NOT published, so the store never serves a camera the seal refused. Pure,
// header-only, C++14, no Windows, no GPU.
#pragma once

#include <VP-V/core/FrameAnalysisStore.h>

#include "CameraMotion.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace CameraLink
{
	class CameraSealToStore
	{
	public:
		explicit CameraSealToStore(FrameAnalysis::Store& store) : m_store(store) {}

		// Seal callback for frame `seq`. `sealedP7` null = P7 rejected at the seal. Returns true only
		// when the store accepted the record.
		bool OnSealed(uint64_t seq, std::shared_ptr<const CameraModel::CameraMotion> sealedP7)
		{
			if (!sealedP7)
			{
				m_skipped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			if (!m_store.PublishCamera(seq, std::move(sealedP7)))
			{
				m_refused.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			m_published.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		uint64_t Published() const { return m_published.load(std::memory_order_relaxed); }
		uint64_t Skipped() const { return m_skipped.load(std::memory_order_relaxed); }
		uint64_t Refused() const { return m_refused.load(std::memory_order_relaxed); }

	private:
		FrameAnalysis::Store& m_store;
		std::atomic<uint64_t> m_published{ 0 };
		std::atomic<uint64_t> m_skipped{ 0 };
		std::atomic<uint64_t> m_refused{ 0 };
	};
}
