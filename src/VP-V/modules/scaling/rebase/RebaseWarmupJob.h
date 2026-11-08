// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <vprenderer/NlsPipelinePreparation.h>
#include <VP-V/modules/scaling/rebase/RebaseWarmup.h>
#include <VP-V/modules/scaling/rebase/SourceRebase.h>

#include <DebugLog.h>

#include <libplacebo/dispatch.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

// Re-base shader warm-up job (scaling.rebase; fix FX-RB). Runs on the generic preparation worker
// (NlsPipelinePreparation: one background thread, generation-aware, no lock held while compiling).
// Only CPU-owned snapshots cross the thread boundary: the worker owns a PRIVATE D3D11 device, the
// re-base module, its dispatch, a renderer and every texture it makes; the only shared object is
// libplacebo's pl_cache, which is documented thread-safe and content-addressed. The renderer's own
// first use of the same shader then finds the translated SPIR-V / HLSL / DXBC in that cache and
// skips the seconds-long translation.
//
// What is warmed, from the real contracts rather than guesses: (1) the re-base passes themselves
// (SourceRebase::Module::Apply on a frame with the live plane sizes, the candidate factors and the
// identified kernel, for the main slot and the crossfade slot); (2) the main libplacebo render of the
// RE-BASED planes to the live target with the live render parameters (VP-owned hooks are private to
// the renderer's device and are not part of this; see README "Not warmed").
namespace VpRebaseWarmup
{
	namespace detail
	{
		// Sinks for VisitContract: the readable key text, and a cheap 64-bit signature (FNV-1a) used to
		// notice a changed contract without building the text every frame.
		struct StringSink
		{
			std::ostringstream out;
			void Text(const char* text) { out << '|' << text; }
			void Int(long long value) { out << '|' << value; }
			void Real(double value) { out << '|' << value; }
		};
		struct HashSink
		{
			unsigned long long hash = 14695981039346656037ULL;
			void Byte(unsigned char value) { hash ^= value; hash *= 1099511628211ULL; }
			void Mix(unsigned long long value) { for (int i = 0; i < 8; ++i) Byte(static_cast<unsigned char>(value >> (8 * i))); }
			void Text(const char* text) { for (; *text; ++text) Byte(static_cast<unsigned char>(*text)); Byte(0xFF); }
			void Int(long long value) { Mix(static_cast<unsigned long long>(value)); }
			void Real(double value) { unsigned long long bits = 0; std::memcpy(&bits, &value, sizeof(bits)); Mix(bits); }
		};

		// The CONTRACT part of a candidate: the revision (device / settings generation), the identified
		// kernel and the source / target frames' colour and plane contract. False when a frame cannot be
		// keyed (no plane texture, no luma plane).
		template <class Sink>
		inline bool VisitContract(unsigned long long revision, const pl_frame& source, const pl_frame& target, int kernel, Sink& sink)
		{
			sink.Text("rebase");
			sink.Int(static_cast<long long>(revision));
			sink.Int(kernel);
			for (const pl_frame* frame : { &source, &target })
			{
				if (frame->num_planes < 0 || frame->num_planes > PL_MAX_PLANES) return false;
				sink.Int(frame->num_planes);
				sink.Int(static_cast<long long>(frame->repr.sys));
				sink.Int(static_cast<long long>(frame->repr.levels));
				sink.Int(static_cast<long long>(frame->repr.alpha));
				sink.Int(frame->repr.bits.sample_depth);
				sink.Int(frame->repr.bits.color_depth);
				sink.Int(frame->repr.bits.bit_shift);
				sink.Int(static_cast<long long>(frame->color.primaries));
				sink.Int(static_cast<long long>(frame->color.transfer));
				sink.Int(static_cast<long long>(frame->rotation));
				sink.Int(static_cast<long long>(frame->field));
				sink.Int(frame->lut ? static_cast<long long>(frame->lut->signature) : 0LL);
				sink.Int(static_cast<long long>(frame->lut_type));
				sink.Int(frame->icc ? 1 : 0);
				sink.Int(static_cast<long long>(frame->film_grain.type));
				for (int i = 0; i < frame->num_planes; ++i)
				{
					const pl_plane& plane = frame->planes[i];
					if (!plane.texture) return false;
					const pl_tex_params& p = plane.texture->params;
					sink.Int(p.w);
					sink.Int(p.h);
					sink.Text(p.format ? p.format->name : "?");
					sink.Int(plane.components);
					sink.Int(plane.flipped ? 1 : 0);
					sink.Real(plane.shift_x);
					sink.Real(plane.shift_y);
					for (int mapping : plane.component_mapping) sink.Int(mapping);
				}
			}
			return source.num_planes > 0 && source.planes[0].texture;
		}
	}

	// A stable identity for the contract of one candidate (everything except the factors); empty when a
	// frame cannot be keyed.
	inline std::string MakeContractKey(unsigned long long revision, const pl_frame& source, const pl_frame& target, int kernel)
	{
		detail::StringSink sink;
		if (!detail::VisitContract(revision, source, target, kernel, sink)) return std::string();
		return sink.out.str();
	}

	// The candidate factors as the exact scaled plane size, so nothing is rounded twice or hard-coded.
	inline std::string TargetSuffix(const pl_frame& source, double factorX, double factorY)
	{
		const pl_tex luma = source.num_planes > 0 ? source.planes[0].texture : nullptr;
		if (!luma) return std::string();
		return "|to:" + std::to_string(SourceRebase::Scaled(luma->params.w, factorX)) + "x" +
			std::to_string(SourceRebase::Scaled(luma->params.h, factorY));
	}

	// A stable identity for one candidate: the contract key plus the candidate's plane size.
	inline std::string MakeKey(unsigned long long revision, const pl_frame& source, const pl_frame& target,
		double factorX, double factorY, int kernel)
	{
		const std::string contract = MakeContractKey(revision, source, target, kernel);
		return contract.empty() ? contract : contract + TargetSuffix(source, factorX, factorY);
	}

	inline std::string Label(const pl_frame& source, double factorX, double factorY)
	{
		const pl_tex luma = source.num_planes > 0 ? source.planes[0].texture : nullptr;
		if (!luma) return "?";
		return std::to_string(SourceRebase::Scaled(luma->params.w, factorX)) + "x" +
			std::to_string(SourceRebase::Scaled(luma->params.h, factorY));
	}

	// One request: CPU snapshots of the live source / target frames and the live render parameters
	// (hooks, callbacks and the info sink are dropped by the copy), the candidate factors and kernel.
	struct Job
	{
		NlsPipelinePreparation::Request base;
		double factorX = 1.0, factorY = 1.0;
		int kernel = -1;
		LUID adapter{};
		int featureLevel = 0;
		bool noCompute = false;

		// Throws (std::runtime_error / bad_alloc) when a frame carries state the snapshot refuses
		// (external ICC objects, acquire/release callbacks, H.274 grain): the caller then lets the
		// re-base through on demand, visibly, instead of holding it forever.
		Job(const pl_frame& source, const pl_frame& target, const pl_render_params& params)
			: base(source, target, params, std::string()) {}

		bool Compile(pl_cache cache)
		{
			struct Owned
			{
				pl_d3d11 device = nullptr;
				pl_renderer renderer = nullptr;
				pl_dispatch dispatch = nullptr;
				SourceRebase::Module module;
				Job& job;
				explicit Owned(Job& j) : job(j) {}
				~Owned()
				{
					if (!device) return;
					pl_gpu gpu = device->gpu;
					module.Release(gpu);
					job.base.source.Destroy(gpu);
					job.base.target.Destroy(gpu);
					if (dispatch) pl_dispatch_destroy(&dispatch);
					pl_renderer_destroy(&renderer);
					pl_d3d11_destroy(&device);
				}
			} owned(*this);

			pl_d3d11_params p{};
			p.adapter_luid = adapter;
			p.no_compute = noCompute;
			p.min_feature_level = p.max_feature_level = featureLevel;
			owned.device = pl_d3d11_create(nullptr, &p);
			if (!owned.device) return false;
			pl_gpu gpu = owned.device->gpu;
			pl_gpu_set_cache(gpu, cache);
			owned.renderer = pl_renderer_create(nullptr, gpu);
			owned.dispatch = pl_dispatch_create(nullptr, gpu);
			if (!owned.renderer || !owned.dispatch || !base.source.Create(gpu) || !base.target.Create(gpu)) return false;

			owned.module.SetKernel(kernel);
			// The main slot, then the crossfade slot (same passes; its own textures).
			pl_frame rebased{};
			if (!owned.module.Apply(gpu, owned.dispatch, base.source.value, factorX, factorY, rebased, 0)) return false;
			pl_frame crossfade{};
			if (!owned.module.Apply(gpu, owned.dispatch, base.source.value, factorX, factorY, crossfade,
				SourceRebase::Module::kSlotCrossfade)) return false;
			// The main render of the re-based planes (hooks were dropped by the snapshot).
			rebased.prev = rebased.next = nullptr;
			if (!pl_render_image(owned.renderer, &rebased, &base.target.value, &base.options->params)) return false;
			pl_gpu_finish(gpu);
			return true;
		}
	};

	// The renderer's live facts for one frame (references; nothing is kept).
	struct Context
	{
		const pl_frame& source;
		const pl_frame& target;
		const pl_render_params& params;
		ID3D11Device* device;          // the render device: the adapter and feature level of the worker's own
		pl_cache cache;                // the shared libplacebo cache the worker compiles into
		bool cacheEnabled;             // false: the render device does not use the cache, nothing to warm
		unsigned long long revision;   // device / settings generation
		bool noCompute;
		int kernel;                    // the identified kernel (SourceRebase::Module::Kernel())
	};

	// Hands one candidate to the worker. True when the worker took it (or recorded its failure).
	inline bool Submit(NlsPipelinePreparation& worker, const std::string& key, const std::string& label,
		const Context& context, double factorX, double factorY)
	{
		try
		{
			auto job = std::make_shared<Job>(context.source, context.target, context.params);
			Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
			Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC description{};
			if (!context.device || FAILED(context.device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()))) ||
				FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) || FAILED(adapter->GetDesc(&description)))
				throw std::runtime_error("preparation adapter unavailable");
			job->adapter = description.AdapterLuid;
			job->featureLevel = context.device->GetFeatureLevel();
			job->noCompute = context.noCompute;
			job->factorX = factorX;
			job->factorY = factorY;
			job->kernel = context.kernel;
			const pl_cache sharedCache = context.cache;
			return worker.Submit(key, [job, sharedCache, label]() {
				const auto start = std::chrono::steady_clock::now();
				const bool prepared = job->Compile(sharedCache);
				DebugLog::Log("VP re-base background warm-up: target=%s thread=%lu ready=%d elapsed_ms=%.2f",
					label.c_str(), GetCurrentThreadId(), prepared ? 1 : 0,
					std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
				return prepared;
			});
		}
		catch (const std::exception& error)
		{
			// A frame contract the snapshot refuses: recorded as a failed warm-up of this key; the gate
			// retries with back-off and, after kMaxFailures, lets the re-base through on demand (visibly).
			DebugLog::Log("Re-base shader warm-up rejected for %s (%s)", label.c_str(), error.what());
			return worker.Submit(key, [] { return false; });
		}
	}

	// The renderer-facing side of the warm-up: the gate, the worker, the cached key and the factors in
	// force, behind ONE call per frame (render thread only). The host keeps generic call sites.
	class Controller
	{
	public:
		struct Outcome
		{
			bool hold = false;               // stay on the current path this frame
			double x = 1.0, y = 1.0;         // when `hold`: the factors to keep (the ones in force, else native)
			bool fallbackChanged = false;    // sync `fallback` into the fallback registry (state change only)
			FallbackCatalog::Decision fallback;
		};

		// `applies`: a re-base may run this frame (not temporal, not paused, the slot runs, not the
		// Original pathway). candidate*: the factors wanted (1.0 on an axis that is not reduced);
		// previous*: the factors in force on the previous frame.
		Outcome Frame(bool applies, const Context& context, double candidateX, double candidateY,
			double previousX, double previousY)
		{
			Outcome out;
			if (!applies || !(candidateX < 1.0 || candidateY < 1.0))
			{
				Idle(out);
				inForce.Clear();
				return out;
			}
			const KeyInfo& k = Refresh(context, candidateX, candidateY);
			// The candidate equals the factors already in force on an unchanged contract: the re-base is
			// running and its shaders are compiled; nothing to ask the gate.
			if (inForce.Covers(candidateX, candidateY, k.signature))
			{
				Idle(out);
				return out;
			}
			const double now = Now();
			Step step;
			if (!context.cacheEnabled)
				step = gate.Skip("the shader cache is disabled by a diagnostic setting", k.label);
			else
				step = gate.Evaluate(true, k.key, k.label, k.key.empty() ? Status::Missing : Lookup(k.key), now);
			if (step.submit && now >= submitHoldoff && !Submit(worker, k.key, k.label, context, candidateX, candidateY))
				submitHoldoff = now + 0.25;   // refused (worker busy or stopping): do not rebuild the job every frame
			if (!step.log.empty())
				DebugLog::Log("%s", step.log.c_str());
			pending.store(step.pending, std::memory_order_release);
			Publish(out, step.status);
			if (step.allow)
			{
				inForce.Allowed(candidateX, candidateY, k.signature, step.status.empty());   // a bypass is not "warm"
				return out;
			}
			// Held: the factors in force while they were made for this same contract, else native (a source /
			// target / kernel change makes them stale for the new frames).
			out.hold = true;
			const FactorsInForce::Factors keep = inForce.Hold(previousX, previousY, k.signature);
			out.x = keep.x;
			out.y = keep.y;
			return out;
		}

		// The renderer's "Preparing shaders..." status flag (any thread).
		bool Pending() const { return pending.load(std::memory_order_acquire); }
		// Joins the worker; before the shared cache or this DLL goes away.
		void Stop() { worker.Stop(); }

	private:
		struct KeyInfo
		{
			bool valid = false;
			bool keyed = false;
			unsigned long long signature = 0;
			double factorX = 0.0, factorY = 0.0;
			std::string contract, key, label;
		};

		static double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

		// The key and label are rebuilt only when the contract signature or the factors change.
		const KeyInfo& Refresh(const Context& c, double factorX, double factorY)
		{
			detail::HashSink hash;
			const bool keyed = detail::VisitContract(c.revision, c.source, c.target, c.kernel, hash);
			const unsigned long long signature = keyed ? hash.hash : 0ULL;
			const bool contractChanged = !info.valid || keyed != info.keyed || signature != info.signature;
			if (contractChanged)
			{
				info.keyed = keyed;
				info.signature = signature;
				info.contract = keyed ? MakeContractKey(c.revision, c.source, c.target, c.kernel) : std::string();
			}
			if (contractChanged || factorX != info.factorX || factorY != info.factorY)
			{
				info.factorX = factorX;
				info.factorY = factorY;
				info.key = info.contract.empty() ? std::string() : info.contract + TargetSuffix(c.source, factorX, factorY);
				info.label = Label(c.source, factorX, factorY);
			}
			info.valid = true;
			return info;
		}

		Status Lookup(const std::string& key) const
		{
			switch (worker.Get(key))
			{
			case NlsPipelinePreparation::Status::Pending: return Status::Pending;
			case NlsPipelinePreparation::Status::Ready: return Status::Ready;
			case NlsPipelinePreparation::Status::Failed: return Status::Failed;
			default: return Status::Missing;
			}
		}

		// The fallback entry follows the status; the host is told only when it changes.
		void Publish(Outcome& out, const std::string& status)
		{
			const bool engagedNow = !status.empty();
			if (engagedNow == engaged && status == lastStatus)
				return;
			engaged = engagedNow;
			lastStatus = status;
			out.fallbackChanged = true;
			out.fallback = FallbackDecision(status);
		}

		void Idle(Outcome& out)
		{
			if (!engaged)
				return;
			pending.store(false, std::memory_order_release);
			gate.Evaluate(false, std::string(), std::string(), Status::Missing, 0.0);
			Publish(out, std::string());
		}

		NlsPipelinePreparation worker;
		Gate gate;
		std::atomic<bool> pending{ false };
		KeyInfo info;
		FactorsInForce inForce;
		bool engaged = false;
		std::string lastStatus;
		double submitHoldoff = 0.0;
	};
}
