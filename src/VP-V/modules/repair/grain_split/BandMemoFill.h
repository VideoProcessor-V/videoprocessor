// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// repair/grain_split: the OWNER FILL of kBandMemo (P30 "band_memo"; C8 slice 8, coreplan-C8-8-U2, shared unit U2), GPU and CPU forms.
// One separable Gaussian band per (frame, BandKey): GPU runs two compute passes (across into an owner scratch R32F, down into the published R32F),
// CPU runs BandMemo::CpuBand<float> into tightly packed float32 bytes; taps and GLSL text come from BandMemo.h (BandFromBase::GlslFromTaps).
// GPU gets its device, dispatch and both textures ONLY from OwnerFillRequest::gpu (R2); inputs come through the memo (nested Acquire, no private upload).
// Served keys: plane 0, pipeline stage 0, Law::Taps or Law::Low. GPU serves Replicate edge (Full reads P13 source luma view; Half reads P1 GPU form).
// CPU serves both Replicate and Reflect edges with explicit width x height (Half reads P1 CPU form uint16 normalised by 65535; Full reads P13 CPU float32).
// Not served, each a VISIBLE decline with its cause (product.band_memo_unavailable): a key outside the codec, Reflect on GPU, plane > 0 or
// pipeline stage > 0 (REBASE-ON-CA: slot planes come with C6/C7), missing dimensions on CPU, or absent/mismatched input plane.
// Output units: the sampled value of the input view (normalised), matching what the private blurs of the consumers produced.
// Reserve (GPU): two R32F textures of the largest luma size the P13 view shows (BandMemoReserveBytes; rule 6, derived, never fixed).
// Registration: BandMemoModule below (observer of Entrance, wired in core/hub/HubBootstrap.cpp; GPU registered on Entrance size, CPU registered at load).
#include <VP-V/core/FallbackCatalog.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/core/hub/HostSlots.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/repair/grain_split/BandMemo.h>

#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace GrainBand
	{
		namespace RE = RendererExtension;
		static constexpr const char* kBandMemoUnit = "repair/grain_split";   // the catalog's kBandMemo owner
		static constexpr const char* kBandMemoFallbackId = "product.band_memo_unavailable";   // FallbackCatalog.h entry

		// Two R32F planes (scratch + output) at the source luma size; 0 = no size known yet.
		inline uint64_t BandMemoReserveBytes(VpSkeleton::Dims luma)
		{
			if (luma.width == 0 || luma.height == 0)
				return 0;
			return uint64_t(luma.width) * uint64_t(luma.height) * 4u * 2u;
		}

		// Fill/decline accounting surfaced on StatusKind::Fallbacks (Rule 4: no silent fallbacks).
		// lastDeclined is engaged on decline and released on the next successful fill (or when all hosts detach).
		struct Counters
		{
			std::atomic<uint64_t> fills{ 0 };
			std::atomic<uint64_t> declines{ 0 };
			std::atomic<bool> lastDeclined{ false };
			mutable std::mutex mutex;
			std::string lastDecline;
			std::string LastDecline() const { std::lock_guard<std::mutex> g(mutex); return lastDecline; }
		};

		inline std::string RegistrationText(VpSkeleton::SkeletonMemo::Registration r)
		{
			using R = VpSkeleton::SkeletonMemo::Registration;
			switch (r)
			{
			case R::Registered: return "registered";
			case R::Replaced: return "replaced";
			case R::NotOwner: return "refused: not the catalog's owning unit";
			case R::NotInCatalog: return "refused: not in the catalog";
			case R::AlreadyRegistered: return "refused: another fill is already registered";
			case R::BadDefaults: return "refused: bad defaults";
			}
			return "refused";
		}

		// Pure fallback status formatter routed through FallbackCatalog::ProductFallback("product.band_memo_unavailable", ...).
		// Empty when registration is healthy and no decline is currently active.
		inline std::string FallbackLine(const std::string& regProblem, uint64_t declines,
			const std::string& lastDecline, bool declineActive)
		{
			std::string why;
			if (!regProblem.empty())
				why = regProblem;
			if (declines != 0u && declineActive)
			{
				const std::string dec = (lastDecline.empty() ? std::string("fill declined") : lastDecline)
					+ " (" + std::to_string(declines) + (declines == 1u ? " decline)" : " declines)");
				why += (why.empty() ? std::string() : "; ") + dec;
			}
			if (why.empty())
				return std::string();
			const FallbackCatalog::Decision d = FallbackCatalog::ProductFallback(kBandMemoFallbackId, why);
			return d.id + ": " + d.reason;
		}

		namespace Detail
		{
			inline bool Decline(VpSkeleton::OwnerFillOutput& out, Counters* c, const std::string& why)
			{
				out.declineReason = "band_memo: " + why;
				if (c != nullptr)
				{
					c->declines.fetch_add(1, std::memory_order_relaxed);
					{
						std::lock_guard<std::mutex> g(c->mutex);
						c->lastDecline = out.declineReason;
					}
					c->lastDeclined.store(true, std::memory_order_release);
				}
				return false;
			}

			inline void NoteFill(Counters* c)
			{
				if (c == nullptr)
					return;
				c->fills.fetch_add(1, std::memory_order_relaxed);
				c->lastDeclined.store(false, std::memory_order_release);
			}

			// One compute pass: `body` over `in` (sampled, clamp, nearest) into `dst` (storage r32f).
			inline bool BandPass(const VpSkeleton::OwnerFillRequest& req, pl_tex in, pl_tex dst, const std::string& header, const char* body, const char* name)
			{
				const pl_dispatch dispatch = static_cast<pl_dispatch>(req.gpu->Dispatch());
				pl_shader_desc desc[2]{};
				desc[0].desc.name = "bm_src";
				desc[0].desc.type = PL_DESC_SAMPLED_TEX;
				desc[0].binding.object = in;
				desc[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
				desc[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
				desc[1].desc.name = "bm_out";
				desc[1].desc.type = PL_DESC_STORAGE_IMG;
				desc[1].desc.access = PL_DESC_ACCESS_WRITEONLY;
				desc[1].binding.object = dst;
				pl_custom_shader custom{};
				custom.description = name;
				custom.header = header.c_str();
				custom.body = body;
				custom.input = PL_SHADER_SIG_NONE;
				custom.output = PL_SHADER_SIG_NONE;
				custom.descriptors = desc;
				custom.num_descriptors = 2;
				custom.compute = true;
				custom.compute_group_size[0] = 16;
				custom.compute_group_size[1] = 16;
				pl_shader sh = pl_dispatch_begin(dispatch);
				if (!pl_shader_custom(sh, &custom))
				{
					pl_dispatch_abort(dispatch, &sh);
					return false;
				}
				pl_dispatch_compute_params compute{};
				compute.shader = &sh;
				compute.dispatch_size[0] = (dst->params.w + 15) / 16;
				compute.dispatch_size[1] = (dst->params.h + 15) / 16;
				compute.dispatch_size[2] = 1;
				return pl_dispatch_compute(dispatch, &compute);
			}
		}

		inline VpSkeleton::OwnerFill MakeBandMemoOwnerFill(uint64_t reserveBytes, std::shared_ptr<Counters> counters = nullptr)
		{
			using VK = VpSkeleton::VariantKey;
			VpSkeleton::OwnerFill f;
			f.variantAxes = uint32_t(VK::kAxisPlane) | uint32_t(VK::kAxisSize) | uint32_t(VK::kAxisScale) | uint32_t(VK::kAxisSigma)
				| uint32_t(VK::kAxisStage) | uint32_t(VK::kAxisParam0) | uint32_t(VK::kAxisParam1);
			f.reserveBytes = reserveBytes;
			f.frameScoped = true;
			f.fill = [counters](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				Counters* const c = counters.get();
				VpUnit::BandMemo::BandKey key;
				if (!VpUnit::BandMemo::FromVariant(req.variant, &key))
					return Detail::Decline(out, c, "the key is not a band key (outside the codec)");
				if (key.edge != VpUnit::BandFromBase::EdgeMode::Replicate)
					return Detail::Decline(out, c, "a Reflect edge has no GPU form");
				if (key.plane != 0 || key.pipelineStage != 0)
					return Detail::Decline(out, c, "only the source luma plane is served (slot planes and chroma wait for the C6/C7 planes)");
				if (req.gpu == nullptr || req.gpu->Gpu() == nullptr || req.gpu->Dispatch() == nullptr)
					return Detail::Decline(out, c, "no GPU context");
				const bool half = key.resolution == VpUnit::BandMemo::Stage::Half;
				if (const std::shared_ptr<const Capture::SourcePlaneSet> offered = Capture::SourcePlanes::Find(req.host, req.seq))
				{
					if (offered->rgb)
						return Detail::Decline(out, c, "RGB source has no luma plane");
				}
				const VpSkeleton::ProductId srcProduct = half ? VpSkeleton::kP1HalfLuma : VpSkeleton::kP13SourceRing;
				const VpSkeleton::AcquireResult src = memo.Acquire(srcProduct, req.seq, VpSkeleton::AcquireForm::Gpu);
				if (!src.Ok() || !src.gpu.Valid() || src.gpu.resource == nullptr)
				{
					const std::string cause = memo.LastCause(srcProduct);
					return Detail::Decline(out, c,
						(half ? std::string("no GPU P1 half-res plane (P1 has no GPU form yet)") : std::string("no source luma view"))
						+ (cause.empty() ? std::string() : " (" + cause + ")"));
				}
				if (!half)
				{
					const Capture::SourcePlaneSet* facts = Capture::SourcePlanes::FactsOf(src);
					if (facts != nullptr && facts->rgb)
						return Detail::Decline(out, c, "RGB source has no luma plane");
				}
				const pl_tex in = static_cast<pl_tex>(src.gpu.resource);
				if (in->params.w <= 0 || in->params.h <= 0)
					return Detail::Decline(out, c, "input plane has empty dimensions");
				VpSkeleton::TexShape shape;
				shape.w = uint32_t(in->params.w);
				shape.h = uint32_t(in->params.h);
				shape.fmt = VpSkeleton::TexFormat::R32F;
				if ((key.width != 0u || key.height != 0u) && (key.width != shape.w || key.height != shape.h))
				{
					return Detail::Decline(out, c, "requested size " + std::to_string(key.width) + "x" + std::to_string(key.height)
						+ " does not match input plane " + std::to_string(shape.w) + "x" + std::to_string(shape.h));
				}
				VpSkeleton::GpuMemoHandle scratch;
				std::string why;
				if (!req.gpu->Allocate(shape, scratch, why))
					return Detail::Decline(out, c, why);
				if (!req.gpu->Allocate(shape, out.gpu, why))
					return Detail::Decline(out, c, why);
				const pl_tex mid = static_cast<pl_tex>(scratch.resource);
				const pl_tex dst = static_cast<pl_tex>(out.gpu.resource);
				const std::string header = VpUnit::BandMemo::GlslOf(key);
				if (!Detail::BandPass(req, in, mid, header, VpUnit::BandMemo::AcrossBodyGlsl(), "VP-V band memo across")
					|| !Detail::BandPass(req, mid, dst, header, VpUnit::BandMemo::DownBodyGlsl(), "VP-V band memo down"))
				{
					out.gpu = VpSkeleton::GpuMemoHandle();
					return Detail::Decline(out, c, "shader or dispatch failed");
				}
				Detail::NoteFill(c);
				return true;   // `scratch` goes out of scope: its keepAlive releases it at the device's next GPU-legal phase
			};
			return f;
		}

		// CPU owner fill of kBandMemo (AcquireForm::Cpu): computes BandMemo::CpuBand<float> into tightly packed float32 bytes
		// (width * height * sizeof(float)). Serves both Replicate and Reflect edges on plane 0 / pipelineStage 0 with explicit
		// width x height. Stage::Half reads kP1HalfLuma CPU form (uint16 normalised by 65535 = (1<<16)-1); Stage::Full reads
		// kP13SourceRing CPU form (float32). Declines visibly on unsupported keys, missing dims, or absent/mismatched source planes.
		inline VpSkeleton::OwnerFill MakeBandMemoCpuOwnerFill(std::shared_ptr<Counters> counters = nullptr)
		{
			using VK = VpSkeleton::VariantKey;
			VpSkeleton::OwnerFill f;
			f.variantAxes = uint32_t(VK::kAxisPlane) | uint32_t(VK::kAxisSize) | uint32_t(VK::kAxisScale) | uint32_t(VK::kAxisSigma)
				| uint32_t(VK::kAxisStage) | uint32_t(VK::kAxisParam0) | uint32_t(VK::kAxisParam1);
			f.reserveBytes = 0;
			f.frameScoped = true;
			f.renderThreadOnly = false;
			f.fill = [counters](const VpSkeleton::OwnerFillRequest& req, VpSkeleton::SkeletonMemo& memo, VpSkeleton::OwnerFillOutput& out) {
				Counters* const c = counters.get();
				VpUnit::BandMemo::BandKey key;
				if (!VpUnit::BandMemo::FromVariant(req.variant, &key))
					return Detail::Decline(out, c, "the key is not a band key (outside the codec)");
				if (key.plane != 0 || key.pipelineStage != 0)
					return Detail::Decline(out, c, "only the source luma plane is served (slot planes and chroma wait for the C6/C7 planes)");
				if (key.width == 0u || key.height == 0u)
					return Detail::Decline(out, c, "CPU form requires explicit plane dimensions (width x height)");
				constexpr uint32_t kMaxDim = static_cast<uint32_t>(INT_MAX);
				if (key.width > kMaxDim || key.height > kMaxDim)
					return Detail::Decline(out, c, "CPU plane dimensions exceed INT_MAX");
				const uint64_t totalSamples = uint64_t(key.width) * uint64_t(key.height);
				if (totalSamples == 0u || totalSamples > uint64_t(SIZE_MAX / sizeof(float)))
					return Detail::Decline(out, c, "CPU plane sample count overflow");
				const size_t nSamples = static_cast<size_t>(totalSamples);
				const bool half = key.resolution == VpUnit::BandMemo::Stage::Half;
				std::vector<float> plane;
				if (half)
				{
					const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP1HalfLuma, req.seq, VpSkeleton::AcquireForm::Cpu);
					if (!src.Ok() || src.cpu == nullptr || src.cpuBytes == 0u)
					{
						const std::string cause = memo.LastCause(VpSkeleton::kP1HalfLuma);
						return Detail::Decline(out, c,
							std::string("no CPU P1 half-res plane") + (cause.empty() ? std::string() : " (" + cause + ")"));
					}
					const size_t halfBytes = nSamples * sizeof(uint16_t);
					const uint64_t qw = (uint64_t(key.width) + 1u) / 2u;
					const uint64_t qh = (uint64_t(key.height) + 1u) / 2u;
					const size_t totalP1Bytes = halfBytes + static_cast<size_t>(qw * qh) * sizeof(uint16_t);
					if (src.cpuBytes != totalP1Bytes && src.cpuBytes != halfBytes)
					{
						return Detail::Decline(out, c,
							"P1 CPU plane size (" + std::to_string(src.cpuBytes) + " B) does not match requested "
							+ std::to_string(key.width) + "x" + std::to_string(key.height));
					}
					std::vector<uint16_t> raw(nSamples);
					std::memcpy(raw.data(), src.cpu, halfBytes);
					plane.resize(nSamples);
					// 16-bit UNORM full-scale denominator: (1 << 16) - 1 = 65535 (matches R16 UNORM GPU sampling).
					constexpr float kInvU16Max = 1.0f / 65535.0f;
					for (size_t i = 0; i < nSamples; ++i)
						plane[i] = static_cast<float>(raw[i]) * kInvU16Max;
				}
				else
				{
					const VpSkeleton::AcquireResult src = memo.Acquire(VpSkeleton::kP13SourceRing, req.seq, VpSkeleton::AcquireForm::Cpu);
					if (!src.Ok() || src.cpu == nullptr || src.cpuBytes == 0u)
					{
						const std::string cause = memo.LastCause(VpSkeleton::kP13SourceRing);
						return Detail::Decline(out, c,
							std::string("no CPU source luma plane") + (cause.empty() ? std::string() : " (" + cause + ")"));
					}
					const size_t expectedBytes = nSamples * sizeof(float);
					if (src.cpuBytes != expectedBytes)
					{
						return Detail::Decline(out, c,
							"P13 CPU plane size (" + std::to_string(src.cpuBytes) + " B) does not match requested "
							+ std::to_string(key.width) + "x" + std::to_string(key.height) + " float32");
					}
					plane.resize(nSamples);
					std::memcpy(plane.data(), src.cpu, expectedBytes);
				}
				const std::vector<float> band = VpUnit::BandMemo::CpuBand<float>(
					plane, static_cast<int>(key.width), static_cast<int>(key.height), key);
				out.bytes.resize(band.size() * sizeof(float));
				if (!band.empty())
					std::memcpy(out.bytes.data(), band.data(), out.bytes.size());
				Detail::NoteFill(c);
				return true;
			};
			return f;
		}

		// Registers the kBandMemo GPU and CPU fills with the skeleton found by the hub; the GPU reserve follows the largest source luma size seen at
		// Entrance (as EdgeMapModule; re-registered only when it changes). Cost per frame: one short lock and a lookup.
		class BandMemoModule : public Hub::ISkeletonModule
		{
		public:
			BandMemoModule() : counters_(std::make_shared<Counters>()) {}
			const char* Name() const override { return "repair.grain_split.band_memo"; }
			void OnLifecycle(RE::LifecycleFacts& f) override
			{
				if (f.host && (f.kind == RE::Lifecycle::GpuReleased || f.kind == RE::Lifecycle::Shutdown))
					Forget(f.host);
			}
			bool OnFrame(RE::FrameContext& c) override
			{
				if (c.phase != RE::FramePhase::Entrance || c.image == nullptr || c.image->num_planes < 1
					|| c.image->planes[0].texture == nullptr || Hub::ExtensionHub::Instance().OriginalPathway(c.host))
					return false;
				if (c.image->planes[0].texture->params.w <= 0 || c.image->planes[0].texture->params.h <= 0)
					return false;
				VpSkeleton::Dims luma;
				luma.width = uint32_t(c.image->planes[0].texture->params.w);
				luma.height = uint32_t(c.image->planes[0].texture->params.h);
				if (!cpuRegistered_.load(std::memory_order_acquire))
					RegisterCpu(nullptr);
				Observe(c.host, luma);
				return false;
			}
			void OnStatus(const RE::Context& ctx, RE::StatusKind kind, std::vector<std::string>& lines, const char*, const char*) override
			{
				if (kind != RE::StatusKind::Fallbacks)
					return;
				if (ctx.host != nullptr && Hub::ExtensionHub::Instance().OriginalPathway(ctx.host))
					return;
				const std::vector<std::string> f = FallbackLines();
				lines.insert(lines.end(), f.begin(), f.end());
			}
			std::vector<std::string> FallbackLines() const
			{
				std::string regProblem;
				{
					std::lock_guard<std::mutex> g(mutex_);
					regProblem = regProblem_;
					if (!cpuRegProblem_.empty())
						regProblem += (regProblem.empty() ? std::string() : "; ") + cpuRegProblem_;
				}
				const std::string line = FallbackLine(
					regProblem,
					counters_->declines.load(std::memory_order_relaxed),
					counters_->LastDecline(),
					counters_->lastDeclined.load(std::memory_order_acquire));
				std::vector<std::string> out;
				if (!line.empty())
					out.push_back(line);
				return out;
			}
			bool RegisterCpu(Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				if (cpuRegistered_.load(std::memory_order_acquire))
					return true;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
				{
					cpuRegProblem_ = "CPU owner fill: no skeleton to register with (the skeleton module is not registered yet); retried at the next frame";
					return false;
				}
				using R = VpSkeleton::SkeletonMemo::Registration;
				const R r = demand->RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Cpu, kBandMemoUnit,
					MakeBandMemoCpuOwnerFill(counters_));
				if (r == R::Registered || r == R::Replaced || r == R::AlreadyRegistered)
				{
					cpuRegistered_.store(true, std::memory_order_release);
					cpuRegProblem_.clear();
					return true;
				}
				cpuRegProblem_ = "CPU owner fill " + RegistrationText(r);
				return false;
			}
			bool CpuRegistered() const { return cpuRegistered_.load(std::memory_order_acquire); }
			bool Observe(const void* host, VpSkeleton::Dims luma, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				bool found = false;
				for (auto& h : hosts_)
				{
					if (h.host == host)
					{
						if (h.luma.width != luma.width || h.luma.height != luma.height)
						{
							h.luma = luma;
							h.bytes = BandMemoReserveBytes(luma);
						}
						found = true;
						break;
					}
				}
				if (!found && host != nullptr && hosts_.size() < kMaxHosts)
				{
					HostSize h;
					h.host = host;
					h.luma = luma;
					h.bytes = BandMemoReserveBytes(luma);
					hosts_.push_back(h);
				}
				return UpdateLocked(demand);
			}
			void Forget(const void* host, Skeleton::IExternalDemand* demand = nullptr)
			{
				std::lock_guard<std::mutex> g(mutex_);
				for (size_t i = 0; i < hosts_.size(); ++i)
				{
					if (hosts_[i].host == host)
					{
						hosts_.erase(hosts_.begin() + std::ptrdiff_t(i));
						break;
					}
				}
				UpdateLocked(demand);
			}
			bool Registered() const { return registered_.load(std::memory_order_acquire); }
			uint64_t RegisteredReserveBytes() const
			{
				std::lock_guard<std::mutex> g(mutex_);
				return registered_.load(std::memory_order_acquire) ? reserve_ : uint64_t(0);
			}
			const Counters& Stats() const { return *counters_; }
		private:
			static constexpr size_t kMaxHosts = 16;   // the skeleton's renderer table (HostSlots)
			struct HostSize
			{
				const void* host = nullptr;
				VpSkeleton::Dims luma;
				uint64_t bytes = 0;
			};
			bool UpdateLocked(Skeleton::IExternalDemand* demand)
			{
				uint64_t want = 0;
				for (const HostSize& h : hosts_)
					if (h.bytes > want)
						want = h.bytes;
				const bool ok = registered_.load(std::memory_order_acquire);
				if (want == 0)
				{
					// No live size: nothing is owed. Reset tried_ so a previously refused size is retried (and shown
					// again) when a host returns; an active registration stays short-circuited via ok && want == reserve_.
					regProblem_.clear();
					tried_ = 0;
					counters_->lastDeclined.store(false, std::memory_order_release);
					return ok;
				}
				if (ok && want == reserve_)
				{
					regProblem_.clear();
					tried_ = reserve_;
					return true;
				}
				if (want == tried_)
					return ok;
				if (demand == nullptr)
					demand = static_cast<Skeleton::IExternalDemand*>(Hub::ExtensionHub::Instance().Query(Skeleton::kExternalDemandQueryId));
				if (demand == nullptr)
				{
					regProblem_ = "no skeleton to register with (the skeleton module is not registered yet); retried at the next frame";
					return ok;
				}
				tried_ = want;
				using R = VpSkeleton::SkeletonMemo::Registration;
				const R r = ok
					? demand->ReplaceOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu, kBandMemoUnit,
						MakeBandMemoOwnerFill(want, counters_))
					: demand->RegisterOwnerFill(VpSkeleton::kBandMemo, VpSkeleton::AcquireForm::Gpu, kBandMemoUnit,
						MakeBandMemoOwnerFill(want, counters_));
				const bool now = r == R::Registered || r == R::Replaced;
				if (now)
				{
					reserve_ = want;
					registered_.store(true, std::memory_order_release);
					regProblem_.clear();
					return true;
				}
				// Failed replace leaves the previously registered fill active on the skeleton: preserve registered_ and reserve_,
				// while keeping regProblem_ visibly populated with the failed replace reason and requested reserve (Rule 4).
				if (!ok)
				{
					reserve_ = uint64_t(0);
					registered_.store(false, std::memory_order_release);
				}
				regProblem_ = "owner fill " + RegistrationText(r) + " (reserve " + std::to_string(want) + " B; retried at the next size change)";
				return ok;
			}
			mutable std::mutex mutex_;
			std::vector<HostSize> hosts_;
			std::shared_ptr<Counters> counters_;
			uint64_t reserve_ = 0;
			uint64_t tried_ = 0;
			std::string regProblem_;
			std::string cpuRegProblem_;
			std::atomic<bool> registered_{ false };
			std::atomic<bool> cpuRegistered_{ false };
		};

		// Start-up only (core/hub/HubBootstrap.cpp, after the skeleton module); idempotent.
		inline void RegisterBandMemoModule()
		{
			static bool registered = false;
			if (registered)
				return;
			registered = true;
			std::unique_ptr<BandMemoModule> mod(new BandMemoModule());
			mod->RegisterCpu();
			Hub::ExtensionHub::Instance().AddModule(std::move(mod));
		}
	}
}
