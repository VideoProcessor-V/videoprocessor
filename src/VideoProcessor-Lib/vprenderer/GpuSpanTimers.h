#pragma once

#include <VP-V/core/PassTimers.h>

#include <atlbase.h>
#include <d3d11.h>

#include <array>
#include <cstdint>
#include <string>

// Efficiency timers T2 (audit 2026-10-24 section 4): GPU time of work libplacebo's per-pass
// callback cannot see - the entrance stages (re-base, deband, texture restore / detail pulse),
// which run on their own pl_dispatch outside pl_render_image. Each span is a D3D11 disjoint query
// and two timestamps on the immediate context (libplacebo's D3D11 backend records into the same
// context in order, so they bracket the stage's passes). Results are POLLED with
// D3D11_ASYNC_GETDATA_DONOTFLUSH frames later - never waited for, never flushed. A span not ready
// after kMaxAgeFrames polls is dropped (counted). Render thread only. Nothing changes a pixel.
class GpuSpanTimers
{
public:
	static constexpr size_t kSpans = 32;
	static constexpr int kMaxAgeFrames = 16;

	// -1 when not timed (no device, ring full) - End(-1) does nothing.
	int Begin(ID3D11Device* device, const char* name)
	{
		if (!device || !Bind(device))
			return -1;
		for (size_t n = 0; n < kSpans; ++n)
		{
			const size_t i = (next + n) % kSpans;
			Span& s = spans[i];
			if (s.state != State::Free)
				continue;
			if (!s.disjoint && !Make(s))
				return -1;
			s.name = name ? name : "(unnamed span)";
			s.age = 0;
			context->Begin(s.disjoint);
			context->End(s.begin);
			s.state = State::Open;
			next = (i + 1) % kSpans;
			return static_cast<int>(i);
		}
		++ringFull;
		return -1;
	}
	void End(int token)
	{
		if (token < 0 || static_cast<size_t>(token) >= kSpans || !context)
			return;
		Span& s = spans[static_cast<size_t>(token)];
		if (s.state != State::Open)
			return;
		context->End(s.end);
		context->End(s.disjoint);
		s.state = State::Pending;
	}
	// Once per refresh: resolve finished spans (DONOTFLUSH, never waits) and invoke onResolved(name, ms, ns).
	template<class Fn>
	void PollEach(Fn&& onResolved)
	{
		if (!context)
			return;
		for (Span& s : spans)
		{
			if (s.state != State::Pending)
				continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
			UINT64 t0 = 0, t1 = 0;
			if (context->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				context->GetData(s.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				context->GetData(s.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
			{
				if (!dj.Disjoint && dj.Frequency > 0 && t1 >= t0)
				{
					const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
					const uint64_t ns = static_cast<uint64_t>(ms * 1e6 + 0.5);
					onResolved(s.name, ms, ns);
				}
				s.state = State::Free;
			}
			else if (++s.age > kMaxAgeFrames)
			{
				++dropped;
				s.state = State::Free;
			}
		}
	}
	// Resolve a specific Pending span by token (non-blocking if wait == false; yields until ready if wait == true).
	// Frees the slot once resolved (or on disjoint/error). Returns true with msOut set when a valid duration resolved.
	bool PollOne(int token, bool wait, double& msOut)
	{
		msOut = 0.0;
		if (token < 0 || static_cast<size_t>(token) >= kSpans || !context)
			return false;
		Span& s = spans[static_cast<size_t>(token)];
		if (s.state != State::Pending)
			return false;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
		UINT64 t0 = 0, t1 = 0;
		const UINT flags = wait ? 0u : D3D11_ASYNC_GETDATA_DONOTFLUSH;
		for (;;)
		{
			const HRESULT hrD = context->GetData(s.disjoint, &dj, sizeof(dj), flags);
			if (hrD == S_FALSE)
			{
				if (!wait)
					return false;
				SwitchToThread();
				continue;
			}
			if (FAILED(hrD))
			{
				s.state = State::Free;
				return false;
			}
			break;
		}
		const bool got = context->GetData(s.begin, &t0, sizeof(t0), flags) == S_OK &&
			context->GetData(s.end, &t1, sizeof(t1), flags) == S_OK && !dj.Disjoint && dj.Frequency > 0 && t1 >= t0;
		s.state = State::Free;
		if (!got)
			return false;
		msOut = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
		return true;
	}
	// Once per refresh: finished spans into the accumulator as Kind::GpuGroup samples.
	void Poll(PassTimers::Accumulator& acc)
	{
		PollEach([&acc](const std::string& name, double ms, uint64_t) {
			acc.Add(PassTimers::Kind::GpuGroup, name, ms);
		});
	}
	// Before the device goes.
	void Release()
	{
		for (Span& s : spans)
			s = Span{};
		context.Release();
		device = nullptr;
	}
	uint64_t Dropped() const { return dropped; }
	uint64_t RingFull() const { return ringFull; }

private:
	enum class State { Free, Open, Pending };
	struct Span
	{
		CComPtr<ID3D11Query> disjoint, begin, end;
		std::string name;
		int age = 0;
		State state = State::Free;
	};
	bool Bind(ID3D11Device* d)
	{
		if (device == d && context)
			return true;
		Release();
		d->GetImmediateContext(&context);
		device = d;
		return context != nullptr;
	}
	bool Make(Span& s)
	{
		D3D11_QUERY_DESC dq{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC tq{ D3D11_QUERY_TIMESTAMP, 0 };
		return SUCCEEDED(device->CreateQuery(&dq, &s.disjoint)) && SUCCEEDED(device->CreateQuery(&tq, &s.begin)) &&
			SUCCEEDED(device->CreateQuery(&tq, &s.end));
	}

	ID3D11Device* device = nullptr;
	CComPtr<ID3D11DeviceContext> context;
	std::array<Span, kSpans> spans{};
	size_t next = 0;
	uint64_t dropped = 0, ringFull = 0;
};
