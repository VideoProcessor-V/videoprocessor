// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// IVectorEngine long-pair service (doc 72 "Service: I-to-I long-range flow"): SubmitLongPair(lumaA, lumaB, hintField) ->
// async forward/backward field + cost on the NVOF 4x4 grid of half luma, once per detected I (previous I vs new I),
// seeded with TD's composed trail field. NOT a skeleton product: the pair distance is the measured GOP length.
//
// Paths (never silent): the second NVOF session with external hints (NvOpticalFlowHinted.h) when it opens; otherwise
// the matcher seeded search (the CPU reference in SeededSearch.h, or the matcher backend P5a binds through
// ISeededBackend) with the visible entry LongPair::Decision / StatusLine ("motion: long-pair seeded search (NVOF with
// hints unavailable: <reason>)"). A pair submitted WITHOUT a hint runs the same session unhinted (not a fallback).
//
// Inputs are CPU 8-bit half-luma planes (as the look-ahead producer's HalfLuma8 makes them); the service copies them
// on submit. All GPU work happens on the service's own D3D11 device on the render adapter, never on the render thread.
// Module side only: bound to IVectorEngine at P5a (the renderer item lists do not compile this file yet).
#pragma once

#include "LongPairTypes.h"
#include "SeededSearch.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IDXGIAdapter;

namespace VpLongPair
{
	// The matcher's seeded search, bound at P5a; the default (null) is the CPU reference.
	class ISeededBackend
	{
	public:
		virtual ~ISeededBackend() {}
		virtual const char* Name() const = 0;
		virtual bool Run(const uint8_t* lumaA, const uint8_t* lumaB, int w, int h, const HintField* hint, Field& out, std::string& error) = 0;
	};

	struct Result
	{
		uint64_t ticket = 0;
		bool ok = false;
		Path path = Path::None;
		Why why;                 // hint supplied? why the hinted session did not run
		Field field;
		std::string error;       // set when !ok (no path could produce a field)
		double ms = 0.0;
	};

	struct Config
	{
		bool useNvof = true;      // code default; false = seeded search only (tests, machines without the engine)
		int nvofPreset = 0;       // 0 slow, 1 medium, 2 fast (NvOpticalFlow::Preset order)
		int searchRadius = kSearchRadius;
		size_t maxPending = 2;    // beyond this a submit is refused (counted), never queued without bound
		size_t maxResults = 8;
	};

	struct Stats
	{
		uint64_t submitted = 0, refused = 0, done = 0, nvofHinted = 0, nvofUnhinted = 0, seeded = 0, failed = 0;
	};

	class Service
	{
	public:
		explicit Service(const Config& config = Config());
		~Service();
		Service(const Service&) = delete;
		Service& operator=(const Service&) = delete;

		// The adapter the render device runs on (the service creates its own device on it); null = system default.
		// Call before the first submit.
		void SetAdapter(IDXGIAdapter* adapter);

		// Async: copies the planes and the hint, returns a ticket (> 0), or 0 when refused (bad size / queue full).
		uint64_t SubmitLongPair(const uint8_t* lumaA, const uint8_t* lumaB, int w, int h, const HintField* hint);
		// Non-blocking: moves the finished result for `ticket` out; false while pending / unknown.
		bool TryTake(uint64_t ticket, Result& out);
		// Synchronous (tests, the bench dump): same path logic on the calling thread.
		Result RunNow(const uint8_t* lumaA, const uint8_t* lumaB, int w, int h, const HintField* hint);

		void SetBackend(ISeededBackend* backend);
		Stats GetStats() const;
		// The visible-status state of the most recent finished pair.
		std::string StatusLine() const;
		FallbackCatalog::Decision CurrentDecision() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
}
