// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera K3: the VP_CAMERA_DUMP writer. VP_CAMERA_DUMP=<dir> writes, per fitted pair (frames a -> b = pairFrom -> pairTo):
//   <dir>/<clip>/flow_<a>_<b>.npy     forward  block field, float32 (gridH, gridW, 2) = (dx, dy), SOURCE pixels
//   <dir>/<clip>/flow_<b>_<a>.npy     backward block field (only when the pair has one)
//   <dir>/<clip>/camera_<b>.json      the camera record (model, confidence, reason, ...) and a `mode` tag (K4:
//                                     nvof_seed | engine | engine_camera)
// Names are `flow_%06d_%06d.npy`, the contract of tools/eval/vector_source.py. <clip> is VP_CAMERA_DUMP_CLIP (default "clip").
// (VP_CAMERA_DUMP=1 keeps its older meaning: log the camera status once a second; it writes no file.)
//
// Threading: Offer() runs on the camera worker after Fit (two shared_ptr copies, one short lock, never a wait, never a write); the
// WRITER THREAD exists only when the object was built with startThread = true (the extension does that only when the variable
// names a directory). The queue is bounded: a full queue DROPS the new item and counts it (Stats().dropped, shown in the status and
// Fallbacks, never silent). Without a thread (tests) DrainOnce() writes synchronously.
// Pure of Windows headers here; header C++14.
#pragma once

#include "FieldCopy.h"

#include <VP-V/modules/motion/estimation/global_model/CameraMotion.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace CameraLink
{
	struct DumpConfig
	{
		std::string directory;          // VP_CAMERA_DUMP
		std::string clip = "clip";      // sub-directory (VP_CAMERA_DUMP_CLIP)
		std::string mode = "engine";    // K4 tag: nvof_seed | engine | engine_camera
		size_t queueCapacity = 8;       // items waiting for the writer; more are dropped
	};

	struct DumpStats
	{
		uint64_t offered = 0, written = 0, dropped = 0, failed = 0;
		size_t queued = 0;
	};

	// ---- pure helpers (tested without files or threads) ----
	// "flow_000012_000011.npy"
	std::string FlowFileName(uint64_t from, uint64_t to);
	// "camera_000012.json"
	std::string CameraFileName(uint64_t to);
	// A complete .npy (format 1.0, little endian float32, C order, shape (h, w, 2)) from two planes of h*w values.
	std::vector<uint8_t> BuildNpy(const std::vector<float>& x, const std::vector<float>& y, int w, int h);
	// The camera record as JSON text (rec == null: a field-only entry with "record": false).
	std::string CameraJson(const CameraModel::CameraMotion* rec, const FieldCopy& field, const std::string& mode);

	class CameraDump
	{
	public:
		// startThread: the writer thread exists only then.
		CameraDump(const DumpConfig& config, bool startThread);
		~CameraDump();   // stops the thread after it has written what is queued
		CameraDump(const CameraDump&) = delete;
		CameraDump& operator=(const CameraDump&) = delete;

		// The directory named by VP_CAMERA_DUMP, or "" when unset, empty, "0" / "1" or a bare on/off word (those are switches, not
		// directories: "1" is the status-log meaning, the others start nothing).
		static std::string DirectoryFromEnvironment();
		static std::string ClipFromEnvironment();

		// Camera worker, after Fit. false = dropped (counted). A null field is dropped as well (nothing to write).
		bool Offer(std::shared_ptr<const FieldCopy> field, std::shared_ptr<const CameraModel::CameraMotion> record);
		// Writes every queued item on the calling thread (tests; the destructor does not drain a thread-less object). Returns the count written.
		size_t DrainOnce();

		DumpStats Stats() const;
		bool HasThread() const { return m_thread.joinable(); }
		const DumpConfig& Config() const { return m_config; }
		// "<dir>/<clip>": where the files go.
		std::string ClipDirectory() const;

	private:
		struct Item
		{
			std::shared_ptr<const FieldCopy> field;
			std::shared_ptr<const CameraModel::CameraMotion> record;
		};
		void Run();
		bool Write(const Item& item);

		DumpConfig m_config;
		mutable std::mutex m_mutex;
		std::condition_variable m_cv;
		std::deque<Item> m_queue;
		DumpStats m_stats;
		bool m_stop = false;
		bool m_dirReady = false;
		std::thread m_thread;
	};
}
