// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// VP-V GPU hook tests (moved out of Bill's LibplaceboLutParserTests.cpp; test names unchanged).
#include "pch.h"
#include <VP-V/tests/VpLutGpuTestSupport.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>
#include <VP-V/modules/input/source_analysis/regions/BusyEdge.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapDispatch.h>
#include <VP-V/modules/input/source_analysis/regions/EdgeMapFill.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/control/skeleton/GpuProductTexBackend.h>
#include <VP-V/modules/control/skeleton/SkeletonAccess.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/HubQuery.h>
#include <VP-V/tests/SettingsTestSupport.h>
#include <libplacebo/dispatch.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <set>

namespace VideoProcessorTest
{
	namespace
	{
		// Counts real libplacebo shader dispatches inside a wrapped pl_hook via pl_dispatch_callback
		// (RAII restore on scope exit) and records each dispatched pass's description/steps and signature.
		struct HookDispatchRecord
		{
			int count = 0;
			std::vector<std::string> descriptions;
			std::vector<uint64_t> signatures;

			void Reset()
			{
				count = 0;
				descriptions.clear();
				signatures.clear();
			}

			bool HasPassContaining(const char* needle) const
			{
				if (!needle || !needle[0])
					return false;
				std::string lowNeedle(needle);
				for (char& ch : lowNeedle)
					ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
				for (size_t i = 0; i < descriptions.size(); ++i)
				{
					std::string low = descriptions[i];
					for (char& ch : low)
						ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
					if (low.find(lowNeedle) != std::string::npos)
						return true;
				}
				return false;
			}
		};

		struct HookDispatchProxy
		{
			const pl_hook* inner = nullptr;
			HookDispatchRecord* record = nullptr;
			pl_hook hook{};

			void Init(const pl_hook* target, HookDispatchRecord* rec)
			{
				inner = target;
				record = rec;
				hook = *target;
				hook.priv = this;
				hook.hook = &HookDispatchProxy::Run;
				hook.reset = target->reset ? &HookDispatchProxy::ResetHook : nullptr;
			}

			static void OnDispatch(void* priv, const pl_dispatch_info* info)
			{
				HookDispatchRecord* rec = static_cast<HookDispatchRecord*>(priv);
				if (!rec)
					return;
				++rec->count;
				std::string desc;
				if (info && info->shader)
				{
					if (info->shader->description)
						desc = info->shader->description;
					for (int i = 0; i < info->shader->num_steps; ++i)
					{
						if (info->shader->steps[i])
						{
							if (!desc.empty())
								desc += " | ";
							desc += info->shader->steps[i];
						}
					}
				}
				rec->descriptions.push_back(desc);
				rec->signatures.push_back(info ? info->signature : 0ull);
			}

			static void ResetHook(void* priv)
			{
				HookDispatchProxy* self = static_cast<HookDispatchProxy*>(priv);
				if (self && self->inner && self->inner->reset)
					self->inner->reset(self->inner->priv);
			}

			static pl_hook_res Run(void* priv, const pl_hook_params* params)
			{
				HookDispatchProxy* self = static_cast<HookDispatchProxy*>(priv);
				struct CallbackGuard
				{
					pl_dispatch dp;
					CallbackGuard(pl_dispatch d, HookDispatchRecord* rec) : dp(d)
					{
						if (dp)
							pl_dispatch_callback(dp, rec, &HookDispatchProxy::OnDispatch);
					}
					~CallbackGuard()
					{
						if (dp)
							pl_dispatch_callback(dp, nullptr, nullptr);
					}
					CallbackGuard(const CallbackGuard&) = delete;
					CallbackGuard& operator=(const CallbackGuard&) = delete;
				} guard(params ? params->dispatch : nullptr, self->record);
				return self->inner->hook(self->inner->priv, params);
			}
		};

		// Owner-fill GPU device for skeleton memo kEdgeMap serving tests (owns its own pl_dispatch and product textures).
		struct AdaptiveMemoOwnerDevice : VpSkeleton::IOwnerGpuDevice
		{
			explicit AdaptiveMemoOwnerDevice(pl_gpu g)
				: gpu(g), backend(g, false), dispatch(pl_dispatch_create(g->log, g))
			{}
			~AdaptiveMemoOwnerDevice() override
			{
				for (pl_tex& t : made)
					pl_tex_destroy(gpu, &t);
				if (dispatch != nullptr)
					pl_dispatch_destroy(&dispatch);
			}
			AdaptiveMemoOwnerDevice(const AdaptiveMemoOwnerDevice&) = delete;
			AdaptiveMemoOwnerDevice& operator=(const AdaptiveMemoOwnerDevice&) = delete;

			void* OwnerGpu() const override { return const_cast<void*>(static_cast<const void*>(gpu)); }
			void* OwnerDispatch() const override { return static_cast<void*>(dispatch); }
			bool CreateOwnerTexture(const VpSkeleton::TexShape& shape, VpSkeleton::GpuMemoHandle& out, std::string& why) override
			{
				out = VpSkeleton::GpuMemoHandle();
				const pl_tex tex = backend.Create(shape);
				if (tex == nullptr)
				{
					why = backend.lastError;
					return false;
				}
				made.push_back(tex);
				out.resource = const_cast<void*>(static_cast<const void*>(tex));
				out.width = shape.w;
				out.height = shape.h;
				out.format = uint32_t(shape.fmt);
				out.bytes = VpSkeleton::ShapeBytes(shape);
				out.keepAlive = std::shared_ptr<void>(out.resource, [](void*) {});
				return true;
			}

			pl_gpu gpu;
			VpLaneProducts::GpuTexBackend backend;
			pl_dispatch dispatch;
			std::vector<pl_tex> made;
		};

		// RAII skeleton-memo host wiring Vpv::Hub::QuerySlot() + CurrentFrame without mutating ExtensionHub::modules_.
		struct ScopedEdgeMapMemoHost
		{
			struct DemandImpl : Vpv::Skeleton::IExternalDemand
			{
				const void* expectedHost = nullptr;
				std::shared_ptr<VpSkeleton::SkeletonMemo> memo;
				const void* gpuPtr = nullptr;

				void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
					const std::vector<VpSkeleton::ProductId>&) override {}
				void ClearExternalDemand(const void*, const void*) override {}
				Vpv::Skeleton::FrameMemo MemoFor(const void* h, uint64_t seq) override
				{
					Vpv::Skeleton::FrameMemo fm;
					if (h == expectedHost && memo)
					{
						fm.memo = memo;
						fm.host = h;
						fm.seq = seq;
						fm.gpu = gpuPtr;
					}
					return fm;
				}
			};

			static Vpv::Skeleton::IExternalDemand*& ActiveDemandSlot()
			{
				static Vpv::Skeleton::IExternalDemand* ptr = nullptr;
				return ptr;
			}
			static Vpv::Hub::QueryFn& PrevQuerySlot()
			{
				static Vpv::Hub::QueryFn prev = nullptr;
				return prev;
			}
			static void* QueryDispatch(const char* iid)
			{
				if (iid != nullptr && std::strcmp(iid, Vpv::Skeleton::kExternalDemandQueryId) == 0 && ActiveDemandSlot() != nullptr)
					return ActiveDemandSlot();
				const Vpv::Hub::QueryFn prev = PrevQuerySlot();
				return prev != nullptr ? prev(iid) : nullptr;
			}

			int hostTag = 0;
			AdaptiveMemoOwnerDevice dev;
			std::shared_ptr<VpSkeleton::SkeletonMemo> memo;
			DemandImpl demand;
			Vpv::Hub::QueryFn savedPrev = nullptr;

			ScopedEdgeMapMemoHost(pl_gpu gpu, VpSkeleton::OwnerFill edgeMapFill)
				: dev(gpu), memo(std::make_shared<VpSkeleton::SkeletonMemo>())
			{
				Vpv::Hub::ResetThisThreadFrameForTesting();
				memo->SetHost(&hostTag);
				memo->SetRenderThread(std::this_thread::get_id());
				memo->SetGpuDispatcherPresent(true);
				memo->SetGpuBudget(true, uint64_t(1) << 30);
				memo->SetGpuDevice(&dev);
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu,
					Vpv::Capture::SourcePlanes::kUnit, Vpv::Capture::SourcePlanes::MakeOwnerFill())
					== VpSkeleton::SkeletonMemo::Registration::Registered,
					L"ScopedEdgeMapMemoHost: kP13SourceRing registration failed");
				Assert::IsTrue(memo->RegisterOwnerFill(VpSkeleton::kEdgeMap, VpSkeleton::AcquireForm::Gpu,
					Vpv::Regions::kEdgeMapUnit, std::move(edgeMapFill))
					== VpSkeleton::SkeletonMemo::Registration::Registered,
					L"ScopedEdgeMapMemoHost: kEdgeMap registration failed");
				demand.expectedHost = &hostTag;
				demand.memo = memo;
				demand.gpuPtr = gpu;
				ActiveDemandSlot() = &demand;
				savedPrev = Vpv::Hub::QuerySlot().exchange(&ScopedEdgeMapMemoHost::QueryDispatch, std::memory_order_acq_rel);
				PrevQuerySlot() = savedPrev;
			}

			~ScopedEdgeMapMemoHost()
			{
				Vpv::Hub::QuerySlot().store(savedPrev, std::memory_order_release);
				ActiveDemandSlot() = nullptr;
				PrevQuerySlot() = nullptr;
				Vpv::Capture::SourcePlanes::Forget(&hostTag);
				Vpv::Hub::ForgetCurrentFrame(&hostTag);
				Vpv::Hub::ResetThisThreadFrameForTesting();
				if (memo)
				{
					memo->SetGpuDevice(nullptr);
					memo->Clear();
				}
			}

			ScopedEdgeMapMemoHost(const ScopedEdgeMapMemoHost&) = delete;
			ScopedEdgeMapMemoHost& operator=(const ScopedEdgeMapMemoHost&) = delete;

			void BeginFrame(uint64_t seq)
			{
				memo->OnSeal(seq);
				RendererExtension::FrameContext fc;
				fc.host = &hostTag;
				fc.phase = RendererExtension::FramePhase::Begin;
				fc.seq = static_cast<int64_t>(seq);
				Vpv::Hub::RecordCurrentFrame(fc);
			}
		};

		// Tiny RAII guards for WARP GPU test log capture and device handles so an early Assert::* throw
		// never causes UAF on the stack log string or leaks pl_tex / pl_hook / pl_dispatch / Module state.
		struct ScopedLogCapture
		{
			TargetLutGpuFixture* fixture = nullptr;
			ScopedLogCapture(TargetLutGpuFixture& f, std::string& sink) : fixture(&f)
			{
				fixture->CaptureLog(&sink);
			}
			~ScopedLogCapture() { Release(); }
			void Release()
			{
				if (fixture)
				{
					fixture->CaptureLog(nullptr);
					fixture = nullptr;
				}
			}
			ScopedLogCapture(const ScopedLogCapture&) = delete;
			ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
		};

		struct ScopedTex
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			ScopedTex() = default;
			ScopedTex(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			~ScopedTex() { Reset(); }
			void Reset(pl_tex t = nullptr)
			{
				if (gpu && tex)
					pl_tex_destroy(gpu, &tex);
				tex = t;
			}
			operator pl_tex() const { return tex; }
			ScopedTex(const ScopedTex&) = delete;
			ScopedTex& operator=(const ScopedTex&) = delete;
			ScopedTex(ScopedTex&& o) noexcept : gpu(o.gpu), tex(o.tex) { o.gpu = nullptr; o.tex = nullptr; }
			ScopedTex& operator=(ScopedTex&& o) noexcept
			{
				if (this != &o)
				{
					Reset();
					gpu = o.gpu;
					tex = o.tex;
					o.gpu = nullptr;
					o.tex = nullptr;
				}
				return *this;
			}
		};

		struct ScopedHook
		{
			const pl_hook* hook = nullptr;
			ScopedHook() = default;
			explicit ScopedHook(const pl_hook* h) : hook(h) {}
			~ScopedHook() { Reset(); }
			void Reset(const pl_hook* h = nullptr)
			{
				if (hook)
					pl_mpv_user_shader_destroy(&hook);
				hook = h;
			}
			operator const pl_hook*() const { return hook; }
			ScopedHook(const ScopedHook&) = delete;
			ScopedHook& operator=(const ScopedHook&) = delete;
		};

		struct ScopedDispatch
		{
			pl_dispatch dispatch = nullptr;
			ScopedDispatch() = default;
			explicit ScopedDispatch(pl_dispatch d) : dispatch(d) {}
			~ScopedDispatch() { Reset(); }
			void Reset(pl_dispatch d = nullptr)
			{
				if (dispatch)
					pl_dispatch_destroy(&dispatch);
				dispatch = d;
			}
			operator pl_dispatch() const { return dispatch; }
			ScopedDispatch(const ScopedDispatch&) = delete;
			ScopedDispatch& operator=(const ScopedDispatch&) = delete;
		};

		struct ScopedModule
		{
			AdaptiveDownscale::Module mod;
			ScopedModule() = default;
			~ScopedModule() { mod.Release(); }
			AdaptiveDownscale::Module& operator*() { return mod; }
			const AdaptiveDownscale::Module& operator*() const { return mod; }
			AdaptiveDownscale::Module* operator->() { return &mod; }
			const AdaptiveDownscale::Module* operator->() const { return &mod; }
			ScopedModule(const ScopedModule&) = delete;
			ScopedModule& operator=(const ScopedModule&) = delete;
		};
	}

	TEST_CLASS(AdaptiveScalingHookTests)
	{
	public:


		// Stage 8-2: the adaptive downscale blend (SSIM / faithful, luma only)
		// on a 1.5x downscale. Hard edges come out as the faithful branch,
		// low-contrast texture as SSIM; the grain term falls to the floor on
		// noise, recovers smoothly, snaps on a cut; nothing happens on an
		// upscale; both light domains give the same weight map; and the Stage
		// 10 carry's BTB/WTW codes survive the measure, SSIM and the blend.
		TEST_METHOD(AdaptiveDownscaleBlendsEdgesTextureAndGrain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				Assert::IsFalse(source.empty());
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int sw = 384, sh = 240;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			auto makeSource = [&](auto code)
			{
				std::vector<uint16_t> px(static_cast<size_t>(sw) * sh * 4);
				for (int y = 0; y < sh; ++y)
					for (int x = 0; x < sw; ++x)
					{
						const double c = std::min(255.0, std::max(0.0, static_cast<double>(code(x, y))));
						const uint16_t v = static_cast<uint16_t>(std::lround(c * 257.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			// Hard stripes on the left, low-contrast fine texture on the right.
			pl_tex scene = makeSource([](int x, int y)
			{
				if (x < sw / 2)
					return ((x / 12) % 2) ? 220.0 : 30.0;
				return 128.0 + 5.0 * std::sin(x * 1.7) * std::cos(y * 1.3);
			});
			uint32_t noiseState = 12345u;
			auto noise = [&noiseState]()
			{
				noiseState = noiseState * 1664525u + 1013904223u;
				return (static_cast<double>(noiseState >> 8) / 16777216.0 - 0.5) * 2.0;
			};
			std::vector<double> noiseField(static_cast<size_t>(sw) * sh);
			for (double& n : noiseField) n = noise() * 7.0;
			pl_tex grainy = makeSource([&](int x, int y) { return 128.0 + noiseField[static_cast<size_t>(y) * sw + x]; });
			pl_tex clean = makeSource([](int, int) { return 128.0; });
			pl_tex dark = makeSource([](int, int) { return 20.0; });
			pl_tex brighter = makeSource([&](int x, int y) { return 200.0 + noiseField[static_cast<size_t>(y) * sw + x]; });

			AdaptiveDownscale::Module module;
			uint64_t sequence = 0;
			// FIX-X5-CUT-P4: the cut event is the shared one (P4 through U7), not a private mean luma step. This harness has no
			// skeleton memo, so it states the cuts of its own scenes (dark -> grain, grain -> brighter) as the P4 verdict.
			std::set<uint64_t> cutSequences;
			module.SetCutSource([&cutSequences](uint64_t, uint64_t seq)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = cutSequences.count(seq) != 0;
				return v;
			});
			auto render = [&](pl_tex src, int ow, int oh, bool withModule, const AdaptiveDownscale::Settings& st,
				bool gammaLight, bool advance)
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = gammaLight;
				std::vector<const pl_hook*> hooks;
				if (advance) ++sequence;
				if (withModule)
				{
					module.SetFrame(st, sequence, 1.0f / 24.0f);
					hooks.push_back(module.Measure());
				}
				hooks.push_back(ssim);
				if (withModule)
					hooks.push_back(module.Blend());
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(ow) * oh);
				pl_tex_transfer_params d{};
				d.tex = target;
				d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			// The stats of the last advancing frame (polled by the next render).
			auto grainNow = [&](pl_tex src, const AdaptiveDownscale::Settings& st)
			{
				render(src, 256, 160, true, st, false, false);
				render(src, 256, 160, true, st, false, false);
				return module.LastStats();
			};
			std::string report;
			char line[200];

			// 1. Edges faithful, texture SSIM (grain term held at 1).
			AdaptiveDownscale::Settings noGrain;
			noGrain.grainLo = 63.0f;
			noGrain.grainHi = 64.0f;
			AdaptiveDownscale::Settings faithfulOnly = noGrain;
			faithfulOnly.wBase = 0.0f;
			// With edges switched off (thresholds above any gradient) w is 1
			// everywhere: the blend must be the SSIM picture (grey content: the
			// luma blend is the whole picture).
			AdaptiveDownscale::Settings allSsim = noGrain;
			allSsim.edgeLo = 254.0f;
			allSsim.edgeHi = 255.0f;
			const auto ssimOnly = render(scene, 256, 160, false, noGrain, false, true);
			const auto faithfulPx = render(scene, 256, 160, true, faithfulOnly, false, true);
			const auto asSsim = render(scene, 256, 160, true, allSsim, false, true);
			const auto blended = render(scene, 256, 160, true, noGrain, false, true);
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"The blend did not run on a downscale");
			int ssimMax = 0, edgeMax = 0, edgeSsimVsFaithful = 0;
			int texMax = 0, texBlendedVsFaithful = 0, texSsimVsFaithful = 0;
			for (int y = 20; y < 140; ++y)
				for (int x = 4; x < 252; ++x)
				{
					const size_t i = static_cast<size_t>(y) * 256 + x;
					ssimMax = std::max(ssimMax, std::abs(asSsim[i].g - ssimOnly[i].g));
					const int sx = static_cast<int>((x + 0.5) * 1.5);
					if (x < 120 && (sx % 12 == 0 || sx % 12 == 11))   // stripe transitions
					{
						edgeMax = std::max(edgeMax, std::abs(blended[i].g - faithfulPx[i].g));
						edgeSsimVsFaithful = std::max(edgeSsimVsFaithful, std::abs(ssimOnly[i].g - faithfulPx[i].g));
					}
					if (x >= 136)   // low-contrast texture half: must follow SSIM, not faithful
					{
						texMax = std::max(texMax, std::abs(blended[i].g - ssimOnly[i].g));
						texBlendedVsFaithful = std::max(texBlendedVsFaithful, std::abs(blended[i].g - faithfulPx[i].g));
						texSsimVsFaithful = std::max(texSsimVsFaithful, std::abs(ssimOnly[i].g - faithfulPx[i].g));
					}
				}
			std::snprintf(line, sizeof(line),
				"w=1 vs SSIM max %d; edges vs faithful max %d (SSIM vs faithful %d); texture vs SSIM max %d (blended vs faithful %d, SSIM vs faithful %d)\n",
				ssimMax, edgeMax, edgeSsimVsFaithful, texMax, texBlendedVsFaithful, texSsimVsFaithful);
			report += line;
			// Edges: the edge map is interpolated between source pixels at a
			// transition, so up to 2 codes of SSIM remain there.
			// Texture half: blended must stay within 1 code of SSIM and closer to SSIM than faithful.
			Assert::IsTrue(ssimMax <= 1 && edgeMax <= 2 && edgeSsimVsFaithful >= 4 &&
				texMax <= 1 && (texSsimVsFaithful <= 1 || texMax < texBlendedVsFaithful),
				ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 2. Grain: after a cut (dark -> grey noise) it snaps to the floor;
			// a clean field of the same mean recovers smoothly; a luma jump
			// (grey -> bright noise) snaps again.
			AdaptiveDownscale::Settings grain;
			render(dark, 256, 160, true, grain, false, true);
			cutSequences.insert(sequence + 1);
			render(grainy, 256, 160, true, grain, false, true);
			const auto noisy = grainNow(grainy, grain);
			render(clean, 256, 160, true, grain, false, true);
			const auto oneClean = grainNow(clean, grain);
			for (int i = 0; i < 120; ++i)
				render(clean, 256, 160, true, grain, false, true);
			const auto manyClean = grainNow(clean, grain);
			cutSequences.insert(sequence + 1);
			render(brighter, 256, 160, true, grain, false, true);
			const auto afterCut = grainNow(brighter, grain);
			std::snprintf(line, sizeof(line), "grain: noise %.3f (valid %d, luma %.1f, w %.3f), clean 1 frame %.3f, 120 frames %.3f, cut to noise %.3f (luma %.1f)\n",
				noisy.grainScale, noisy.valid ? 1 : 0, noisy.meanLuma, noisy.meanW, oneClean.grainScale,
				manyClean.grainScale, afterCut.grainScale, afterCut.meanLuma);
			report += line;
			Assert::IsTrue(noisy.valid && noisy.grainScale <= grain.grainFloor + 0.05f, ExtendedRangeGpuProbe::ToWide(report).c_str());
			Assert::IsTrue(oneClean.grainScale < 0.5f, ExtendedRangeGpuProbe::ToWide("no smoothing: " + report).c_str());
			Assert::IsTrue(manyClean.grainScale > 0.95f, ExtendedRangeGpuProbe::ToWide("no recovery: " + report).c_str());
			Assert::IsTrue(afterCut.grainScale <= grain.grainFloor + 0.05f, ExtendedRangeGpuProbe::ToWide("no snap: " + report).c_str());

			// 3. No effect on an upscale (the SSIM shader's own condition).
			const auto upPlain = render(scene, 576, 360, false, noGrain, false, true);
			const auto upModule = render(scene, 576, 360, true, noGrain, false, true);
			Assert::IsFalse(module.Active(), L"The blend ran on an upscale");
			for (size_t i = 0; i < upPlain.size(); ++i)
				Assert::IsTrue(upPlain[i].r == upModule[i].r && upPlain[i].g == upModule[i].g, L"An upscale changed");
			// 3b. The source's own height decides, as SSIM's `NATIVE_CROPPED.h >
			// POSTKERNEL.h`: a picture a prescaler made taller than the output,
			// from a source that is not, is left alone (audit stage8-rs232 item 3).
			module.SetSourceDownscales(false);
			render(scene, 256, 160, true, noGrain, false, true);
			Assert::IsFalse(module.Active(), L"The blend ran although the source is not downscaled");
			module.SetSourceDownscales(true);

			// 4. Both light domains, the same weight map (edges from the source).
			// State the brighter -> scene cut for P4 (FIX-X5-CUT-P4: matches Step 2's injected cut verdicts).
			cutSequences.insert(sequence + 1);
			render(scene, 256, 160, true, noGrain, false, true);
			const auto linearStats = grainNow(scene, noGrain);
			// Bug hunt 34 item 1: on D3D11 the statistics come through the non-blocking staging
			// ring (libplacebo's buffer readback blocks there), and still arrive one frame later.
			Assert::IsTrue(module.NativeStatsReadback(), L"D3D11: the statistics must be read back without waiting");
			Assert::AreEqual<uint64_t>(0, module.StatsDropped(), L"statistics frames dropped by the staging ring");
			render(scene, 256, 160, true, noGrain, true, true);
			render(scene, 256, 160, true, noGrain, true, false);
			render(scene, 256, 160, true, noGrain, true, false);
			const auto gammaStats = module.LastStats();
			std::snprintf(line, sizeof(line), "mean w linear %.4f gamma %.4f\n", linearStats.meanW, gammaStats.meanW);
			report += line;
			Assert::IsTrue(std::abs(linearStats.meanW - gammaStats.meanW) < 0.02f, ExtendedRangeGpuProbe::ToWide(report).c_str());

			// 5. The carry through measure, SSIM and blend (BT.709 -> P3, down 0.5).
			{
				using namespace ExtendedRangeGpuProbe;
				const std::vector<int> codes = { 4, 16, 40, 64, 100, 502, 900, 940, 980, 1019 };
				Case c;
				c.scale = 0.5;
				c.mitchell = true;
				module.SetFrame(noGrain, ++sequence, 1.0f / 24.0f);
				c.extraHooks = { module.Measure(), ssim, module.Blend() };
				// With the SSIM downscaler (the owner's 4K chain) every code is
				// kept, below black too, from the first carry frame on. (The
				// restore renders its own texture: as an inline colour step it was
				// disabled after any texture-output hook at POST_KERNEL.)
				auto check = [&](const std::vector<int>& got, const char* what)
				{
					for (size_t i = 0; i < codes.size(); ++i)
						Assert::IsTrue(std::abs(got[i] - codes[i]) <= (codes[i] > 940 || codes[i] < 64 ? 2 : 1),
							ToWide(std::string(what) + " changed the picture\n" + report).c_str());
				};
				const Result first = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				const auto firstGot = Columns(first, static_cast<int>(codes.size()), 0.5, 64);
				report += "first carry frame after SSIM without it: " + Join(firstGot) + "\n";
				check(firstGot, "the first carry frame");
				{
					Case plain;
					plain.scale = 0.5;
					plain.mitchell = true;
					plain.extraHooks = { ssim };
					const auto ssimGot = Columns(Render(gpu, fixture.Renderer(), Greys(codes, 64), plain),
						static_cast<int>(codes.size()), 0.5, 64);
					report += "SSIM alone with the carry: " + Join(ssimGot) + "\n";
					check(ssimGot, "SSIM with the carry");
				}
				module.SetFrame(noGrain, ++sequence, 1.0f / 24.0f);
				const Result r = Render(gpu, fixture.Renderer(), Greys(codes, 64), c);
				Assert::IsTrue(r.ok);
				const auto got = Columns(r, static_cast<int>(codes.size()), 0.5, 64);
				report += "carry through the blend: " + Join(got) + "\n";
				check(got, "the blend");
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			for (pl_tex* t : { &scene, &grainy, &clean, &dark, &brighter })
				pl_tex_destroy(gpu, t);
			module.Release();
			pl_mpv_user_shader_destroy(&ssim);
			// Only shader-compiler timing notes may appear (a loaded machine).
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Stage 8b (docs/VP-stage8b-upscale-blend.md), with a nearest-neighbour
		// luma doubler standing in for the CNN (its 2x2 box average is the
		// source exactly, so the faithful branch must equal libplacebo's own
		// ewa_lanczossharp render). The grain term is forced by the ramp:
		// grain_lo 1000 (clean, g = 1) or grain_hi 0.001 (grain, g = floor 0.4).
		// CF-2 B1 (HybridBlend.h): w = clamp(w_base (1 - e) g) x m, out = sharp + (1 - w)(alias_safe - sharp)
		// on luma. The identity / grain assertions run with the edge ramp forced to e = 0 on every pixel
		// (unchanged tolerances); the edge term is then checked with the ramp forced to e = 1 and e = 0.5.
		TEST_METHOD(AdaptiveUpscaleBlendsCnnAndFaithfulByGrain)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const int width = 128, height = 64;
			// Neutral grey: the blend is luma-only, so the branches are compared
			// exactly where R = G = B.
			auto chroma = [](int, int, int) { return static_cast<uint16_t>(32768); };
			const DejudderFrame frame = MakeDejudderFrame(width, height,
				[](int x, int y) { return DejudderTexture(x, y); }, chroma);
			DejudderGpuPlanes planes;
			UploadDejudderFrame(gpu, frame, planes);
			static const char doubler[] =
				"//!HOOK LUMA\n"
				"//!BIND HOOKED\n"
				"//!WIDTH HOOKED.w 2 *\n"
				"//!HEIGHT HOOKED.h 2 *\n"
				"//!WHEN OUTPUT.h HOOKED.h / 1.300 >\n"
				"//!DESC test nearest luma doubler\n"
				"vec4 hook() { return HOOKED_tex((floor(HOOKED_pos * HOOKED_size) + 0.5) / HOOKED_size); }\n";
			const pl_hook* cnn = pl_mpv_user_shader_parse(gpu, doubler, sizeof(doubler) - 1);
			Assert::IsNotNull(cnn);
			pl_fmt rgba16 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsNotNull(rgba16);
			// CF-2 switch-over (WAIVER): the upscale form's grain term is kP10gGrainLevel only (no private scalar): a stand-in
			// level, N_b = 1 in every 16 x 16 source block (y/z/w unused by the blend), handed to the module as the stage would.
			// sqrt(1) x GrainLevelEnergyScale() (1.0, FIX-UPS-grain) = 1 sits inside every ramp below exactly where the old
			// private energy did: above 0.001 and -1, below 1000. Without the level the term is grain_floor (checked below).
			pl_fmt rgba32f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(rgba32f);
			const int levelW = (width + 15) / 16, levelH = (height + 15) / 16;
			std::vector<float> levelTexels(static_cast<size_t>(levelW) * levelH * 4, 0.0f);
			for (size_t i = 0; i < levelTexels.size(); i += 4)
				levelTexels[i] = 1.0f;
			pl_tex_params lp{};
			lp.w = levelW;
			lp.h = levelH;
			lp.format = rgba32f;
			lp.sampleable = true;
			lp.initial_data = levelTexels.data();
			pl_tex grainLevelTex = pl_tex_create(gpu, &lp);
			Assert::IsNotNull(grainLevelTex);
			bool publishGrainLevel = true;
			auto render = [&](int ow, int oh, std::vector<const pl_hook*> hooks, const pl_filter_config* upscaler)
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = rgba16;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target);
				pl_frame image{};
				image.num_planes = 2;
				image.planes[0].texture = planes.planes.luma;
				image.planes[0].components = 1;
				image.planes[0].component_mapping[0] = PL_CHANNEL_Y;
				image.planes[1].texture = planes.planes.chroma;
				image.planes[1].components = 2;
				image.planes[1].component_mapping[0] = PL_CHANNEL_CB;
				image.planes[1].component_mapping[1] = PL_CHANNEL_CR;
				image.repr.sys = PL_COLOR_SYSTEM_BT_709;
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.primaries = PL_COLOR_PRIM_BT_709;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);
				pl_frame t = TargetLutGpuFixture::MakeRgbFrame(target);
				t.repr.levels = PL_COLOR_LEVELS_FULL;
				t.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_fast_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				// upscale_light gamma for the exact comparisons: with linear light
				// libplacebo scales a doubler's 2x output DOWN to 1.5x in linear
				// light but upscales the plain source in gamma light (no linear
				// upscaling without sigmoid), so the references would differ in
				// domain; in gamma light every path is gamma.
				params.sigmoid_params = nullptr;
				params.disable_linear_scaling = true;
				params.upscaler = upscaler;
				params.antiringing_strength = 0.5f;   // as the module's faithful branch
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &t, &params));
				pl_gpu_finish(gpu);
				std::vector<uint16_t> px(static_cast<size_t>(ow) * oh * 4);
				pl_tex_transfer_params d{};
				d.tex = target;
				d.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &d));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			AdaptiveDownscale::Settings settings;
			settings.grainMap = false;
			// e = 0 everywhere: smoothstep(1e6, 2e6, grad) = 0 for any 8-bit gradient. The ramps below force g
			// through the stand-in level (energy 1: > 0.001, and far below 1000).
			const float edgeOffLo = 1.0e6f, edgeOffHi = 2.0e6f;
			settings.edgeLo = edgeOffLo;
			settings.edgeHi = edgeOffHi;
			auto withModule = [&](AdaptiveDownscale::Module& module, float grainLo, float grainHi, float wBase,
				float nativeH, int ow, int oh, std::vector<const pl_hook*> hooks, bool superresAlpha = false,
				size_t blendAt = static_cast<size_t>(-1))
			{
				AdaptiveDownscale::Settings s2 = settings;
				s2.grainLo = grainLo;
				s2.grainHi = grainHi;
				s2.grainSmoothMs = 0.0f;
				// A new source frame per call: the grain state advances once per frame.
				static uint64_t sequence = 0;
				module.SetFrame(s2, ++sequence, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true;   // the module's own gate must refuse native and downscaled frames
				up.wBase = wBase;
				up.superresAlpha = superresAlpha;
				up.nativeHeight = nativeH;
				module.SetUpscale(up);
				module.ClearGrainLevel();
				if (publishGrainLevel)
				{
					AdaptiveDownscale::GrainLevelSource level;
					level.level = grainLevelTex;
					level.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetGrainLevel(level);
				}
				hooks.insert(hooks.begin(), module.Measure());
				if (blendAt < hooks.size())
					hooks.insert(hooks.begin() + static_cast<std::ptrdiff_t>(blendAt) + 1, module.Blend());
				else
					hooks.push_back(module.Blend());
				return render(ow, oh, hooks, &pl_filter_ewa_lanczossharp);
			};
			auto maxDiff = [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b)
			{
				int m = 0;
				for (size_t i = 0; i < a.size(); ++i)
					if (i % 4 != 3)
						m = std::max(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
				return m;
			};
			const int ow = width * 3 / 2, oh = height * 3 / 2;   // 1.5x: the doubler runs (2x > 1.3x)
			const std::vector<uint16_t> cnnBranch = render(ow, oh, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::vector<uint16_t> faithfulRef = render(ow, oh, {}, &pl_filter_ewa_lanczossharp);
			Assert::IsTrue(maxDiff(cnnBranch, faithfulRef) > 1000, L"the stand-in CNN should differ from the faithful upscale");
			{
				// The stand-in at exactly 2x: the nearest-neighbour source (diagnostic).
				const std::vector<uint16_t> twice = render(width * 2, height * 2, { cnn }, &pl_filter_ewa_lanczossharp);
				int worst = 0, wx = 0, wy = 0;
				for (int y = 4; y < height * 2 - 4; ++y)
					for (int x = 4; x < width * 2 - 4; ++x)
					{
						const int d = std::abs(static_cast<int>(twice[(static_cast<size_t>(y) * width * 2 + x) * 4]) -
							static_cast<int>(frame.luma[static_cast<size_t>(y / 2) * width + x / 2]));
						if (d > worst) { worst = d; wx = x; wy = y; }
					}
				Assert::IsTrue(worst <= 64, ExtendedRangeGpuProbe::ToWide("doubler at 2x vs nearest source: max " + std::to_string(worst) + " at " +
					std::to_string(wx) + "," + std::to_string(wy) + ": got " +
					std::to_string(twice[(static_cast<size_t>(wy) * width * 2 + wx) * 4]) + ", source " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2]) + ", left " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2 - 1]) + ", right " +
					std::to_string(frame.luma[static_cast<size_t>(wy / 2) * width + wx / 2 + 1])).c_str());
			}

			// Clean (g = 1, e = 0, w = 1): exactly the CNN branch, edges included.
			AdaptiveDownscale::Module module;
			const std::vector<uint16_t> clean = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			Assert::AreEqual(2, module.LastUpscaleRatio(), L"the doubler's output should be averaged to the source");
			Assert::IsTrue(module.PublishedGrainUsed(), L"the blend read the stand-in grain level");
			const int cleanDiff = maxDiff(clean, cnnBranch);
			// w_base 0: the luma of the faithful branch (chroma: the same scaler in both).
			const std::vector<uint16_t> faithful = withModule(module, 1000.0f, 2000.0f, 0.0f, static_cast<float>(height), ow, oh, { cnn });
			const int faithfulDiff = maxDiff(faithful, faithfulRef);
			std::wstring where;
			{
				double sum = 0.0;
				int mx = 0, at = 0;
				for (size_t i = 0; i < faithful.size(); i += 4)
				{
					const int d = std::abs(static_cast<int>(faithful[i]) - static_cast<int>(faithfulRef[i]));
					sum += d;
					if (d > mx) { mx = d; at = static_cast<int>(i / 4); }
				}
				where = L" (mean " + std::to_wstring(sum / (faithful.size() / 4)) + L", max at " + std::to_wstring(at % ow) +
					L"," + std::to_wstring(at / ow) + L": " + std::to_wstring(faithful[static_cast<size_t>(at) * 4]) + L" vs " +
					std::to_wstring(faithfulRef[static_cast<size_t>(at) * 4]) + L", CNN " + std::to_wstring(cnnBranch[static_cast<size_t>(at) * 4]) + L")";
			}
			// Grain (g = 0.4): luma = CNN + 0.6 (faithful - CNN).
			const std::vector<uint16_t> grainy = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			int grainDiff = 0;
			for (size_t i = 0; i < grainy.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double expected = cnnBranch[i] + 0.6 * (static_cast<double>(faithful[i]) - cnnBranch[i]);
				grainDiff = std::max(grainDiff, static_cast<int>(std::lround(std::fabs(grainy[i] - expected))));
			}
			// No grain level (the switch-over's alias-safe lean): g = grain_floor whatever the ramp: the clean ramp
			// (g = 1 if published) gives the grain picture.
			publishGrainLevel = false;
			const std::vector<uint16_t> unpublished = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh, { cnn });
			publishGrainLevel = true;
			Assert::IsFalse(module.PublishedGrainUsed(), L"no level: not published");
			int floorDiff = 0;
			for (size_t i = 0; i < unpublished.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double expected = cnnBranch[i] + 0.6 * (static_cast<double>(faithful[i]) - cnnBranch[i]);
				floorDiff = (std::max)(floorDiff, static_cast<int>(std::lround(std::fabs(unpublished[i] - expected))));
			}
			Assert::IsTrue(floorDiff <= 96, (L"no grain level vs CNN + 0.6 (faithful - CNN): " + std::to_wstring(floorDiff)).c_str());
			// Without the doubler the base is the source picture itself.
			const std::vector<uint16_t> plain = withModule(module, 1000.0f, 2000.0f, 0.0f, static_cast<float>(height), ow, oh, {});
			const int plainRatio = module.LastUpscaleRatio();
			const int plainDiff = maxDiff(plain, faithfulRef);
			// Gate: native size and a downscale are left alone.
			const std::vector<uint16_t> native = withModule(module, 0.0f, 0.001f, 0.0f, static_cast<float>(height), width, height, { cnn });
			const std::vector<uint16_t> nativeRef = render(width, height, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::vector<uint16_t> down = withModule(module, 0.0f, 0.001f, 0.0f, static_cast<float>(height), width / 2, height / 2, { cnn });
			const std::vector<uint16_t> downRef = render(width / 2, height / 2, { cnn }, &pl_filter_ewa_lanczossharp);
			const std::wstring message = L"clean vs CNN " + std::to_wstring(cleanDiff) + L", faithful vs ewa " +
				std::to_wstring(faithfulDiff) + where + L", grain vs CNN + 0.6 (faithful - CNN) " + std::to_wstring(grainDiff) +
				L", no doubler: ratio " + std::to_wstring(plainRatio) + L" vs ewa " + std::to_wstring(plainDiff) +
				L" (16-bit codes); log: " + std::wstring(log.begin(), log.end());
			// The blend's output and the faithful branch are 16-bit float
			// textures: about 1/2048 of the value (32 of 65535 near white).
			Assert::IsTrue(cleanDiff <= 32, message.c_str());
			Assert::IsTrue(faithfulDiff <= 96 && plainDiff <= 96 && plainRatio == 1, message.c_str());
			Assert::IsTrue(grainDiff <= 96, message.c_str());
			Assert::AreEqual(0, maxDiff(native, nativeRef), (L"native size must be untouched; " + message).c_str());
			Assert::AreEqual(0, maxDiff(down, downRef), (L"a downscale must be untouched; " + message).c_str());

			// CF-2 B1 edge term (FIX-UPS-edge change (a)): edge pixels move from the sharp branch (here the CNN)
			// toward the alias-safe branch (`faithful`, w_base 0) by 1 - w, w = clamp(w_base (1 - e) g) x m (m = 1:
			// no vectors, no floor). The ramp forces e on every pixel:
			//   e = 1:   smoothstep(-2, -1, grad) = 1 for any gradient >= 0, so w = 0 whatever g: out = alias-safe.
			//   e = 0.5: smoothstep(-1e6, 1e6, grad) = 0.5 within 3e-4 (16-bit float edge texture: within 1e-3).
			// The stand-in level's energy 1: grain ramp 1000..2000 gives g = 1, -2..-1 gives g = the floor 0.4
			// (switch-over WAIVER: was the private energy 0 with no flat pixel). Predicted 1 - w (w_base 1): e = 1 -> 1 (g = 1 and g = 0.4);
			// e = 0.5 -> 0.5 (g = 1), 0.8 (g = 0.4). With the edge term removed these would be 0, 0.6, 0 and 0.6.
			{
				auto withEdge = [&](float edgeLo, float edgeHi, float grainLo, float grainHi)
				{
					settings.edgeLo = edgeLo;
					settings.edgeHi = edgeHi;
					const std::vector<uint16_t> out = withModule(module, grainLo, grainHi, 1.0f, static_cast<float>(height), ow, oh, { cnn });
					settings.edgeLo = edgeOffLo;
					settings.edgeHi = edgeOffHi;
					return out;
				};
				// out against sharp + toward (alias_safe - sharp), every colour channel (the luma difference moves all three).
				auto checkToward = [&](const std::vector<uint16_t>& out, double toward, int tolerance, const wchar_t* label)
				{
					int worst = 0, wrongWay = 0, overshoot = 0, moving = 0;
					double num = 0.0, den = 0.0;
					for (size_t i = 0; i < out.size(); ++i)
					{
						if (i % 4 == 3)
							continue;
						const double sharp = cnnBranch[i], safe = faithful[i];
						const double moved = out[i] - sharp, span = safe - sharp;
						worst = std::max(worst, static_cast<int>(std::lround(std::fabs(out[i] - (sharp + toward * span)))));
						num += moved * span;
						den += span * span;
						if (std::fabs(span) >= 512.0)
						{
							++moving;
							if (moved * span <= 0.0)
								++wrongWay;       // not moved, or moved away from the alias-safe branch
							if (std::fabs(moved) > std::fabs(span) + tolerance)
								++overshoot;      // moved past the alias-safe branch
						}
					}
					const double fraction = den > 0.0 ? num / den : 0.0;
					const std::wstring m = std::wstring(label) + L": predicted 1 - w " + std::to_wstring(toward) + L", measured " +
						std::to_wstring(fraction) + L", max vs prediction " + std::to_wstring(worst) + L" (tolerance " +
						std::to_wstring(tolerance) + L"), moving channels " + std::to_wstring(moving) + L", wrong way " +
						std::to_wstring(wrongWay) + L", overshoot " + std::to_wstring(overshoot) + L" (16-bit codes)";
					Assert::IsTrue(moving > 0 && den > 0.0, m.c_str());
					Assert::IsTrue(worst <= tolerance, m.c_str());
					Assert::IsTrue(std::fabs(fraction - toward) < 0.02, m.c_str());
					Assert::AreEqual(0, wrongWay, m.c_str());
					Assert::AreEqual(0, overshoot, m.c_str());
				};
				// e = 1, w = 0 (as w_base 0): the alias-safe branch, at the identity tolerance.
				checkToward(withEdge(-2.0f, -1.0f, 1000.0f, 2000.0f), 1.0, 32, L"e = 1, g = 1");
				checkToward(withEdge(-2.0f, -1.0f, -2.0f, -1.0f), 1.0, 32, L"e = 1, g = 0.4");
				// e = 0.5: a mixed expectation, at the grain assertion's tolerance.
				checkToward(withEdge(-1.0e6f, 1.0e6f, 1000.0f, 2000.0f), 0.5, 96, L"e = 0.5, g = 1 (w = 0.5)");
				checkToward(withEdge(-1.0e6f, 1.0e6f, -2.0f, -1.0f), 0.8, 96, L"e = 0.5, g = 0.4 (w = 0.2)");
			}

			// SuperRes strength follows the grain term (its vp_grain_strength,
			// alpha from the blend): with g = 0.4 its correction is scaled by 0.4.
			std::string superSource = LoadBundledShader("SSimSuperRes.glsl");
			Assert::IsFalse(superSource.empty());
			const pl_hook* superres = pl_mpv_user_shader_parse(gpu, superSource.data(), superSource.size());
			Assert::IsNotNull(superres);
			// On the plain upscale (no doubler), where SuperRes has work to do;
			// the blend sits before it (both POST_KERNEL).
			const std::vector<uint16_t> blendOnly = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh, {});
			const int superAlone = maxDiff(render(ow, oh, { superres }, &pl_filter_ewa_lanczossharp), faithfulRef);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 0.0));
			const std::vector<uint16_t> fullSuper = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, false, 0);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 1.0));
			const std::vector<uint16_t> scaledSuper = withModule(module, 0.0f, 0.001f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, true, 0);
			double num = 0.0, den = 0.0;
			for (size_t i = 0; i < blendOnly.size(); ++i)
			{
				if (i % 4 == 3)
					continue;
				const double full = static_cast<double>(fullSuper[i]) - blendOnly[i];
				const double scaled = static_cast<double>(scaledSuper[i]) - blendOnly[i];
				num += full * scaled;
				den += full * full;
			}
			const double ratio = den > 0.0 ? num / den : 0.0;
			// Clean (g = 1): the scaled SuperRes is the full one.
			const std::vector<uint16_t> cleanScaled = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, true, 0);
			Assert::IsTrue(CustomHookChain::SetHookParameter(superres, "vp_grain_strength", 0.0));
			const std::vector<uint16_t> cleanFull = withModule(module, 1000.0f, 2000.0f, 1.0f, static_cast<float>(height), ow, oh,
				{ superres }, false, 0);
			// SuperRes after a doubler is covered by the chain matrix, not here
			// (planner 2026-10-12, not a gap): on these synthetic pictures its
			// correction after a doubler is at half-float rounding level (31 of
			// 65535 even after a sharpening doubler, without the blend), so it
			// cannot be measured; the order and the alpha handoff are checked
			// above on the plain upscale, and the matrix's refinements axis runs
			// real FSRCNNX + SSimSuperRes with the blend at the upscale scales.
			pl_mpv_user_shader_destroy(&superres);
			pl_mpv_user_shader_destroy(&cnn);
			pl_tex_destroy(gpu, &grainLevelTex);
			const std::wstring superMessage = L"SuperRes correction with g = 0.4: " + std::to_wstring(ratio) +
				L" of the full one (correction energy " + std::to_wstring(den) + L"; SuperRes alone changes the plain upscale by " +
				std::to_wstring(superAlone) + L"); clean scaled vs full " +
				std::to_wstring(maxDiff(cleanScaled, cleanFull)) + L"; log: " + std::wstring(log.begin(), log.end());
			Assert::IsTrue(den > 0.0 && std::fabs(ratio - 0.4) < 0.05, superMessage.c_str());
			Assert::IsTrue(maxDiff(cleanScaled, cleanFull) <= 1, superMessage.c_str());
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		TEST_METHOD(BitrateScaleRampAndAntiring)
		{
			// Delta 15 S8-2 / 2c: clean (0.8 and above) is today's output exactly.
			Assert::AreEqual(1.0, AdaptiveDownscale::BitrateScale(1.0, 0.4));
			Assert::AreEqual(1.0, AdaptiveDownscale::BitrateScale(0.8, 0.4));
			Assert::AreEqual(0.4, AdaptiveDownscale::BitrateScale(0.0, 0.4), 1e-9);
			Assert::AreEqual(0.4, AdaptiveDownscale::BitrateScale(0.2, 0.4), 1e-9);
			Assert::AreEqual(0.7, AdaptiveDownscale::BitrateScale(0.5, 0.4), 1e-9, L"halfway");
			Assert::AreEqual(0.7, AdaptiveDownscale::BitrateScale(0.51, 0.4), 1e-9, L"steps of 0.05: no churn");
			Assert::AreEqual(0.5f, AdaptiveDownscale::BitrateAntiring(0.5f, 1.0, 0.4), L"clean: anti-ringing unchanged");
			Assert::AreEqual(0.75f, AdaptiveDownscale::BitrateAntiring(0.5f, 0.4, 0.4), 1e-6f, L"floor: halfway to 1");
		}

		TEST_METHOD(AdaptiveDownscaleGrainMapFromTheVectors)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create());
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);
			const pl_hook* ssim = nullptr;
			{
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				Assert::IsNotNull(ssim);
			}
			static constexpr int width = 256, height = 128, dx = 8, dy = 4;
			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt && dstFmt);
			Dejudder::MotionWarp warp(fixture.Log(), gpu, DejudderTestSettings());
			AdaptiveDownscale::Module module;
			module.hostReadableMap = true;
			AdaptiveDownscale::Settings st;
			st.grainSmoothMs = 0.0f;       // the scalar follows each frame
			st.grainMapSmoothMs = 100.0f;  // the map settles in a few frames
			// The picture as RGB (grey from the luma), the render's source.
			auto rgbOf = [&](const DejudderFrame& f)
			{
				std::vector<uint16_t> px(static_cast<size_t>(width) * height * 4);
				for (size_t i = 0; i < f.luma.size(); ++i)
				{
					px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = f.luma[i];
					px[i * 4 + 3] = 65535;
				}
				pl_tex_params tp{};
				tp.w = width; tp.h = height; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				return pl_tex_create(gpu, &tp);
			};
			auto render = [&](pl_tex src, uint64_t sequence, bool withField, int mode = 1)
			{
				// mode 1: measure + SSIM + blend; 0: SSIM alone.
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				module.ClearGrainField();
				if (withField)
				{
					AdaptiveDownscale::GrainField gf;
					gf.error = warp.GrainError();
					gf.field = warp.Field();
					gf.frameTrust = warp.FrameTrust();
					gf.minTrustedShare = warp.GetSettings().minTrustedShare;
					gf.sourceCrop = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
					module.SetGrainField(gf);
				}
				pl_tex_params tp{};
				tp.w = width / 2; tp.h = height / 2; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				const pl_hook* ssimOnly[1] = { ssim };
				params.hooks = mode == 1 ? hooks : ssimOnly;
				params.num_hooks = mode == 1 ? 3 : 1;
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params));
				pl_gpu_finish(gpu);
				std::vector<RgbaPixel> px(static_cast<size_t>(width / 2) * (height / 2));
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl));
				pl_tex_destroy(gpu, &target);
				return px;
			};
			std::string report;
			// Frames 0..7; each pair (f, f+1) estimated, frame f rendered with its field.
			for (int f = 0; f < 8; ++f)
			{
				const DejudderFrame a = GrainFrame(width, height, f, dx, dy, 0);
				const DejudderFrame b = GrainFrame(width, height, f + 1, dx, dy, 0);
				DejudderGpuPlanes planesA, planesB;
				UploadDejudderFrame(gpu, a, planesA);
				UploadDejudderFrame(gpu, b, planesB);
				Assert::IsTrue(warp.Estimate(planesA.planes, planesB.planes, -1, true));
				pl_tex src = rgbOf(a);
				render(src, static_cast<uint64_t>(f + 1), true);
				render(src, static_cast<uint64_t>(f + 1), true);   // a repeat: the map stands
				pl_tex_destroy(gpu, &src);
			}
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"the blend did not run");
			Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Used, L"the map was not used");
			pl_tex map = module.GrainMap();
			Assert::IsNotNull(map);
			std::vector<float> texels(static_cast<size_t>(map->params.w) * map->params.h * 4);
			pl_tex_transfer_params d{};
			d.tex = map;
			d.ptr = texels.data();
			Assert::IsTrue(pl_tex_download(gpu, &d), L"map not readable");
			double clean = 0.0, noisy = 0.0;
			int nClean = 0, nNoisy = 0;
			const int bw = map->params.w, bh = map->params.h;
			for (int by = 1; by < bh - 1; ++by)
				for (int bx = 1; bx < bw - 1; ++bx)
				{
					if (bx == bw / 2 - 1 || bx == bw / 2)
						continue;
					const float* t = &texels[(static_cast<size_t>(by) * bw + bx) * 4];
					if (t[1] < 0.5f)
						continue;   // untrusted
					if (bx < bw / 2) { clean += t[0]; ++nClean; }
					else { noisy += t[0]; ++nNoisy; }
				}
			Assert::IsTrue(nClean > 10 && nNoisy > 10, L"too few trusted blocks in the map");
			clean /= nClean;
			noisy /= nNoisy;
			char line[200];
			std::snprintf(line, sizeof(line), "map: texture %.3f (%d blocks), noise %.3f (%d blocks), floor %.2f\n",
				clean, nClean, noisy, nNoisy, st.grainFloor);
			report += line;
			Assert::IsTrue(clean > 0.9 && noisy < st.grainFloor + 0.1, ExtendedRangeGpuProbe::ToWide(report).c_str());
			// The blend follows the map where the output shows it: on the
			// texture half (map 1) it is SSIM except at edges; on the noise
			// half (map at the floor) it moves towards faithful. Faithful is the
			// blend with w_base 0. Measured as the mean distance to SSIM per
			// half, against the scalar-only blend of the same frame.
			{
				const DejudderFrame a = GrainFrame(width, height, 7, dx, dy, 0);
				pl_tex src = rgbOf(a);
				const auto withMap = render(src, 8, true);          // the map stands for frame 8
				const auto ssimPx = render(src, 8, true, 0);
				AdaptiveDownscale::Settings saved = st;
				st.grainMap = false;
				const auto scalarPx = render(src, 8, true);
				st = saved;
				pl_tex_destroy(gpu, &src);
				double mapTex = 0.0, scalarTex = 0.0, mapNoise = 0.0, scalarNoise = 0.0;
				int nTex = 0, nNoise = 0;
				const int ow = width / 2, oh = height / 2;
				for (int y = 8; y < oh - 8; ++y)
					for (int x = 8; x < ow - 8; ++x)
					{
						if (x >= ow / 2 - 12 && x < ow / 2 + 12)
							continue;   // the boundary (bilinear between blocks)
						const size_t i = static_cast<size_t>(y) * ow + x;
						const double m = std::abs(withMap[i].g - ssimPx[i].g), sc = std::abs(scalarPx[i].g - ssimPx[i].g);
						if (x < ow / 2) { mapTex += m; scalarTex += sc; ++nTex; }
						else { mapNoise += m; scalarNoise += sc; ++nNoise; }
					}
				mapTex /= nTex; scalarTex /= nTex; mapNoise /= nNoise; scalarNoise /= nNoise;
				std::snprintf(line, sizeof(line), "blend vs SSIM: texture map %.3f scalar %.3f; noise map %.3f scalar %.3f (scalar g %.3f)\n",
					mapTex, scalarTex, mapNoise, scalarNoise, module.LastStats().grainScale);
				report += line;
				// Texture: the map keeps SSIM at least as much as the scalar;
				// noise: it leans away from SSIM at least as much; and the two
				// halves differ (the map is not one number).
				Assert::IsTrue(mapTex <= scalarTex + 0.05 && mapNoise >= scalarNoise - 0.05 && mapNoise > mapTex + 0.2,
					ExtendedRangeGpuProbe::ToWide(report).c_str());
			}
			// No field for the next source frame: the scalar.
			{
				const DejudderFrame a = GrainFrame(width, height, 8, dx, dy, 0);
				pl_tex src = rgbOf(a);
				render(src, 9, false);
				pl_tex_destroy(gpu, &src);
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::NoField, L"no field: not the scalar");
			}
			// Map off: the scalar, nothing else.
			st.grainMap = false;
			{
				const DejudderFrame a = GrainFrame(width, height, 9, dx, dy, 0);
				pl_tex src = rgbOf(a);
				render(src, 10, true);
				pl_tex_destroy(gpu, &src);
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Off);
			}
			fixture.CaptureLog(nullptr);
			Logger::WriteMessage(report.c_str());
			module.Release();
			pl_mpv_user_shader_destroy(&ssim);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// ---- X5-GRAIN-P10 (gate cb-u1 r1 MUST 4 / 5 / 6, SHOULD 5): the shared grain source on the adaptive downscale ----------
		// A downscale through the real hook chain (measure, SSIM, blend) with white-noise sources, so the private Laplacian
		// energy and the P10-derived energy have a known common value. The grain ramp is turned into an energy readout: the
		// smoothing is off (grain_smooth_ms 1) and a cut (dark -> noise) snaps, so grain_scale = the ramp's target and the
		// energy is recovered by inverting grain_scale = floor + (1 - floor) (1 - smoothstep(lo, hi, energy)).
		struct GrainRig
		{
			static constexpr int kSw = 384, kSh = 240, kBw = 24, kBh = 15;   // source size and its 16 px block grid
			TargetLutGpuFixture fixture;
			pl_gpu gpu = nullptr;
			const pl_hook* ssim = nullptr;
			pl_fmt srcFmt = nullptr, dstFmt = nullptr, gridFmt = nullptr;
			std::vector<pl_tex> owned;

			~GrainRig()
			{
				for (pl_tex& t : owned)
					pl_tex_destroy(gpu, &t);
				if (ssim)
					pl_mpv_user_shader_destroy(&ssim);
			}

			bool Init()
			{
				if (!fixture.Create())
					return false;
				gpu = fixture.Gpu();
				const std::string source = LoadBundledShader("SSimDownscaler.glsl");
				if (source.empty())
					return false;
				ssim = pl_mpv_user_shader_parse(gpu, source.data(), source.size());
				srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
				dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				gridFmt = pl_find_named_fmt(gpu, "rgba32f");
				return ssim && srcFmt && dstFmt && gridFmt;
			}

			pl_tex Grey(const std::vector<double>& codes)
			{
				std::vector<uint16_t> px(static_cast<size_t>(kSw) * kSh * 4);
				for (size_t i = 0; i < codes.size(); ++i)
				{
					const double c = (std::min)(255.0, (std::max)(0.0, codes[i]));
					const uint16_t v = static_cast<uint16_t>(std::lround(c * 257.0));
					px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
					px[i * 4 + 3] = 65535;
				}
				pl_tex_params tp{};
				tp.w = kSw; tp.h = kSh; tp.format = srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				pl_tex t = pl_tex_create(gpu, &tp);
				owned.push_back(t);
				return t;
			}

			pl_tex Grid(const std::vector<float>& rgba, int w, int h)
			{
				pl_tex_params tp{};
				tp.w = w; tp.h = h; tp.format = gridFmt; tp.sampleable = true; tp.initial_data = rgba.data();
				pl_tex t = pl_tex_create(gpu, &tp);
				owned.push_back(t);
				return t;
			}

			// Per block: N (the grain variance in 8-bit codes squared, P10 .x), meanY, coverage x off-edge share.
			AdaptiveDownscale::GrainSource Source(double sigma, int w = kBw, int h = kBh, int s4w = kBw, int s4h = kBh)
			{
				const size_t n = static_cast<size_t>(w) * h;
				std::vector<float> p10(n * 4, 0.0f), s0(n * 4, 0.0f), s4(static_cast<size_t>(s4w) * s4h * 4, 0.0f);
				for (size_t i = 0; i < n; ++i)
				{
					p10[i * 4] = static_cast<float>(sigma * sigma);
					s0[i * 4] = 128.0f;
				}
				for (size_t i = 0; i < static_cast<size_t>(s4w) * s4h; ++i)
					s4[i * 4] = s4[i * 4 + 1] = 1.0f;
				AdaptiveDownscale::GrainSource g;
				g.p10Blocks = Grid(p10, w, h);
				g.p11Stats0 = Grid(s0, w, h);
				g.p11Stats4 = Grid(s4, s4w, s4h);
				g.blockPx = 16;
				g.codeToEightBit = 1.0f;
				g.sourceCrop = pl_rect2df{ 0.0f, 0.0f, static_cast<float>(kSw), static_cast<float>(kSh) };
				return g;
			}

			static AdaptiveDownscale::Settings Ramp()
			{
				AdaptiveDownscale::Settings st;
				st.edgeLo = 250.0f; st.edgeHi = 255.0f;   // every pixel is flat: the private energy is the mean |Laplacian|
				st.shadowBias = 0.0f;                     // no shadow weighting in either estimate
				st.grainLo = 2.0f; st.grainHi = 16.0f; st.grainFloor = 0.4f;
				st.grainSmoothMs = 1.0f;
				return st;
			}

			void Render(AdaptiveDownscale::Module& module, uint64_t& sequence, pl_tex src, const AdaptiveDownscale::Settings& st,
				bool gammaLight, bool advance, const AdaptiveDownscale::GrainSource* grain)
			{
				pl_tex_params tp{};
				tp.w = 256; tp.h = 160; tp.format = dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = gammaLight;   // true: the hook sees the gamma code domain (mode 0)
				if (advance)
					++sequence;
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				if (grain)
					module.SetGrainSource(*grain);
				else
					module.ClearGrainSource();
				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;
				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				pl_tex_destroy(gpu, &target);
				Assert::IsTrue(ok, L"the render ran");
			}

			struct Result
			{
				AdaptiveDownscale::Stats stats;
				AdaptiveDownscale::GrainSourceUse use = AdaptiveDownscale::GrainSourceUse::NotSupplied;
				bool fromShared = false;
			};

			// dark -> noise (a cut: grain_scale snaps to the target), two more frames for the read-back to arrive.
			Result Measure(pl_tex dark, pl_tex noisy, const AdaptiveDownscale::Settings& st, bool gammaLight,
				const AdaptiveDownscale::GrainSource* grain)
			{
				AdaptiveDownscale::Module module;
				uint64_t sequence = 0;
				Render(module, sequence, dark, st, gammaLight, true, grain);
				Render(module, sequence, noisy, st, gammaLight, true, grain);
				Render(module, sequence, noisy, st, gammaLight, false, grain);
				Render(module, sequence, noisy, st, gammaLight, false, grain);
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"the blend ran without a GPU failure");
				Assert::IsTrue(module.LastStats().valid, L"the statistics arrived");
				Result r;
				r.stats = module.LastStats();
				r.use = module.LastGrainSourceUse();
				r.fromShared = module.LastGrainFromSharedProducts();
				module.Release();
				Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"no hook was disabled");
				return r;
			}
		};

		// Seeded white Gaussian noise around 128 codes (Box-Muller on an LCG); sigmaOut = the field's own standard deviation.
		static std::vector<double> WhiteNoiseField(double sigma, uint32_t seed, double& sigmaOut)
		{
			std::vector<double> v(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh);
			uint32_t state = seed;
			auto uniform = [&state]() { state = state * 1664525u + 1013904223u; return (static_cast<double>(state >> 8) + 0.5) / 16777216.0; };
			double sum = 0.0;
			for (double& x : v)
			{
				x = 128.0 + sigma * std::sqrt(-2.0 * std::log(uniform())) * std::cos(6.283185307179586 * uniform());
				sum += x;
			}
			const double mean = sum / static_cast<double>(v.size());
			double ss = 0.0;
			for (double x : v) ss += (x - mean) * (x - mean);
			sigmaOut = std::sqrt(ss / static_cast<double>(v.size()));
			return v;
		}

		// The energy the grain ramp saw: grain_scale = floor + (1 - floor) (1 - smoothstep(lo, hi, energy)), inverted.
		static double EnergyOfScale(float scale, const AdaptiveDownscale::Settings& st)
		{
			const double u = (std::min)(1.0, (std::max)(0.0, 1.0 - (static_cast<double>(scale) - st.grainFloor) / (1.0 - st.grainFloor)));
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 60; ++i)
			{
				const double t = 0.5 * (lo + hi);
				if (t * t * (3.0 - 2.0 * t) < u) lo = t; else hi = t;
			}
			return st.grainLo + (st.grainHi - st.grainLo) * 0.5 * (lo + hi);
		}

		TEST_METHOD(AdaptiveGrainSourceUseIsReportedForEveryCase)
		{
			using AdaptiveDownscale::GrainSourceUse;
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			double sigma = 0.0;
			pl_tex noisy = rig.Grey(WhiteNoiseField(2.0, 12345u, sigma));
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const AdaptiveDownscale::GrainSource good = rig.Source(sigma);

			// NotSupplied: no source at all (the design when the host offers none).
			const GrainRig::Result own = rig.Measure(dark, noisy, st, true, nullptr);
			Assert::IsTrue(own.use == GrainSourceUse::NotSupplied && !own.fromShared, L"no source: NotSupplied");

			// Shared: a valid source in the gamma code domain.
			const GrainRig::Result shared = rig.Measure(dark, noisy, st, true, &good);
			Assert::IsTrue(shared.use == GrainSourceUse::Shared && shared.fromShared, L"a valid source in the code domain: Shared");

			// NoUsableBlock: bound, but every block is outside the crop (an empty crop) ...
			AdaptiveDownscale::GrainSource outside = good;
			outside.sourceCrop = pl_rect2df{};
			const GrainRig::Result outsideResult = rig.Measure(dark, noisy, st, true, &outside);
			Assert::IsTrue(outsideResult.use == GrainSourceUse::NoUsableBlock && !outsideResult.fromShared,
				L"every block outside the crop: NoUsableBlock, never Shared");
			Assert::IsTrue(std::fabs(outsideResult.stats.grainScale - own.stats.grainScale) < 1e-3f,
				L"no usable block: the energy is the private estimate (the same grain scale as the unsupplied run)");
			// ... or every block vetoed (zero flat coverage).
			AdaptiveDownscale::GrainSource vetoed = rig.Source(sigma);
			{
				const std::vector<float> zero(static_cast<size_t>(GrainRig::kBw) * GrainRig::kBh * 4, 0.0f);
				vetoed.p11Stats4 = rig.Grid(zero, GrainRig::kBw, GrainRig::kBh);
			}
			const GrainRig::Result vetoedResult = rig.Measure(dark, noisy, st, true, &vetoed);
			Assert::IsTrue(vetoedResult.use == GrainSourceUse::NoUsableBlock && !vetoedResult.fromShared,
				L"every block vetoed (no flat coverage): NoUsableBlock, never Shared");
			Assert::IsTrue(std::fabs(vetoedResult.stats.grainScale - own.stats.grainScale) < 1e-3f,
				L"all blocks vetoed: the energy is the private estimate");

			// MeasureDomain: a valid source, but the measure domain is linear light (P10 N is in source codes).
			const GrainRig::Result ownLinear = rig.Measure(dark, noisy, st, false, nullptr);
			const GrainRig::Result linear = rig.Measure(dark, noisy, st, false, &good);
			Assert::IsTrue(ownLinear.use == GrainSourceUse::NotSupplied, L"linear light, no source: NotSupplied");
			Assert::IsTrue(linear.use == GrainSourceUse::MeasureDomain && !linear.fromShared,
				L"a valid source in a linear measure domain is refused: MeasureDomain");
			Assert::IsTrue(std::fabs(linear.stats.grainScale - ownLinear.stats.grainScale) < 1e-3f,
				L"the refused source leaves the private estimate in force");

			// Grid mismatch: the three textures do not share one block grid. Valid() is false and the module ignores it.
			const AdaptiveDownscale::GrainSource mismatched = rig.Source(sigma, GrainRig::kBw, GrainRig::kBh, GrainRig::kBw / 2, GrainRig::kBh / 2);
			Assert::IsFalse(mismatched.SameGrid(), L"the coverage texture has another block grid");
			Assert::IsFalse(mismatched.Valid(), L"a grid mismatch is not a valid source");
			const GrainRig::Result mismatchedResult = rig.Measure(dark, noisy, st, true, &mismatched);
			Assert::IsTrue(mismatchedResult.use == GrainSourceUse::NotSupplied && !mismatchedResult.fromShared,
				L"a source with mismatched grids is not bound: NotSupplied");
			Assert::IsTrue(std::fabs(mismatchedResult.stats.grainScale - own.stats.grainScale) < 1e-3f,
				L"a grid mismatch leaves the private estimate in force");
		}

		TEST_METHOD(AdaptiveGrainSourceNonFiniteBlocksAreVetoed)
		{
			// A NaN, an infinity or a negative level in a P10 block, or a NaN in P11's mean luma or coverage, vetoes that block
			// (the energy is built from the other blocks); it never reaches the ramp. The planted blocks lie inside the crop.
			using AdaptiveDownscale::GrainSourceUse;
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			double sigma = 0.0;
			pl_tex noisy = rig.Grey(WhiteNoiseField(2.0, 777u, sigma));
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const double level = 1.5 * sigma;   // a level that differs from the private estimate, so the P10 path is what is read
			const size_t nb = static_cast<size_t>(GrainRig::kBw) * GrainRig::kBh;
			auto block = [](int bx, int by) { return (static_cast<size_t>(by) * GrainRig::kBw + bx) * 4; };

			const AdaptiveDownscale::GrainSource control = rig.Source(level);
			const GrainRig::Result own = rig.Measure(dark, noisy, st, true, nullptr);
			const GrainRig::Result clean = rig.Measure(dark, noisy, st, true, &control);
			Assert::IsTrue(clean.use == GrainSourceUse::Shared, L"control: the P10 path is read");
			Assert::IsTrue(std::fabs(clean.stats.grainScale - own.stats.grainScale) > 0.02f, L"control: the P10 level changes the energy (the path is really used)");

			const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
			std::vector<float> p10(nb * 4, 0.0f), s0(nb * 4, 0.0f), s4(nb * 4, 0.0f);
			for (size_t i = 0; i < nb; ++i)
			{
				p10[i * 4] = static_cast<float>(level * level);
				s0[i * 4] = 128.0f;
				s4[i * 4] = s4[i * 4 + 1] = 1.0f;
			}
			p10[block(3, 3)] = nan;              // NaN grain level
			p10[block(4, 4)] = inf;              // +inf grain level
			p10[block(5, 5)] = -inf;             // -inf grain level
			p10[block(6, 6)] = -4.0f;            // negative variance
			s0[block(7, 7)] = nan;               // NaN mean luma
			s0[block(8, 8)] = inf;               // +inf mean luma
			s4[block(9, 9)] = nan;               // NaN coverage
			s4[block(10, 10) + 1] = -inf;        // -inf off-edge share
			// Gate cb-u1 r2 SHOULD 1: the P11-planted blocks carry a distinct P10 level (4 x the level, 16 x the variance), so a
			// block whose mean-luma or coverage veto failed would move the energy by far more than the 1e-4 tolerance below
			// (with shadow_bias 0 a uniform level made a non-vetoed P11 block invisible).
			for (const size_t b : { block(7, 7), block(8, 8), block(9, 9), block(10, 10) })
				p10[b] = static_cast<float>(16.0 * level * level);
			AdaptiveDownscale::GrainSource planted = control;
			planted.p10Blocks = rig.Grid(p10, GrainRig::kBw, GrainRig::kBh);
			planted.p11Stats0 = rig.Grid(s0, GrainRig::kBw, GrainRig::kBh);
			planted.p11Stats4 = rig.Grid(s4, GrainRig::kBw, GrainRig::kBh);
			Assert::IsTrue(planted.Valid(), L"the planted source is a valid-shaped source");
			const GrainRig::Result r = rig.Measure(dark, noisy, st, true, &planted);
			Assert::IsTrue(r.use == GrainSourceUse::Shared, L"the unplanted blocks are still usable: Shared");
			Assert::IsTrue(std::isfinite(r.stats.grainScale) && r.stats.grainScale >= st.grainFloor - 1e-4f && r.stats.grainScale <= 1.0f + 1e-4f,
				L"the grain scale stays finite and inside the ramp");
			Assert::IsTrue(std::fabs(r.stats.grainScale - clean.stats.grainScale) < 1e-4f,
				L"the vetoed blocks do not move the energy (a uniform level over the other blocks; the P11-vetoed ones carry 4 x the level)");
		}

		TEST_METHOD(AdaptiveGrainSourceEnergyMatchesThePrivateEstimateOnWhiteNoise)
		{
			// FIX-X5-GRAIN-P10: on i.i.d. Gaussian noise of standard deviation s the 4-neighbour Laplacian 4e - (b + d + f + h)
			// has variance 20 s^2 and E|x| = sqrt(2/pi) std, so the private mean |Laplacian| is kEdgeEnergyPerSigma x s
			// (3.5682482 s). The P10 path is that constant times N^(1/2); with N = s^2 both must read the same energy.
			// Tolerance from the derivation: the relative standard error of a mean of |L| is sqrt(pi/2 - 1) / sqrt(N_eff) with
			// N_eff >= N / 5 (each Laplacian shares its 5 taps with at most 4 neighbours) = 0.755 / sqrt(92160 / 5) = 0.56%,
			// so 5 standard errors = 2.8% < 3%. The P10 path itself is exact up to float rounding of the readout (0.5%).
			using AdaptiveDownscale::GrainSourceUse;
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			for (double target : { 2.0, 3.0 })
			{
				double sigma = 0.0;
				pl_tex noisy = rig.Grey(WhiteNoiseField(target, static_cast<uint32_t>(1000.0 * target) + 7u, sigma));
				const double expected = AdaptiveDownscale::kEdgeEnergyPerSigma * sigma;
				const AdaptiveDownscale::GrainSource source = rig.Source(sigma);
				const GrainRig::Result own = rig.Measure(dark, noisy, st, true, nullptr);
				const GrainRig::Result shared = rig.Measure(dark, noisy, st, true, &source);
				Assert::IsTrue(shared.use == GrainSourceUse::Shared, L"the P10 path was read");
				const double ePrivate = EnergyOfScale(own.stats.grainScale, st), eShared = EnergyOfScale(shared.stats.grainScale, st);
				const std::string report = "sigma " + std::to_string(sigma) + ": expected " + std::to_string(expected) + ", private " +
					std::to_string(ePrivate) + ", shared " + std::to_string(eShared);
				Logger::WriteMessage(report.c_str());
				Assert::IsTrue(expected > st.grainLo + 2.0 && expected < st.grainHi - 2.0, L"the energy sits inside the ramp (readable)");
				Assert::IsTrue(std::fabs(ePrivate - expected) <= 0.03 * expected, ExtendedRangeGpuProbe::ToWide("private Laplacian energy vs sqrt(20) sqrt(2/pi) sigma: " + report).c_str());
				Assert::IsTrue(std::fabs(eShared - expected) <= 0.005 * expected, ExtendedRangeGpuProbe::ToWide("P10 energy vs the constant: " + report).c_str());
				Assert::IsTrue(std::fabs(eShared - ePrivate) <= 0.03 * expected, ExtendedRangeGpuProbe::ToWide("P10 energy vs private energy: " + report).c_str());
			}
		}

		TEST_METHOD(AdaptiveGrainSourceStageStatusAndFallbackForEveryCase)
		{
			// Gate cb-u1 r2 MUST 4 (r1 MUST 5 / SHOULD 6): the stage, driven as the host drives it (Stage::Register with a Host, the
			// OutputGlueStagesGpuTests pattern), shows exactly one grain status line and the GrainSourceFallback decision of each
			// case after a real scalar grain pass: no callback, a valid source, an empty crop (no usable block), a grid
			// mismatch, and a valid source in a linear measure domain. The grain decision owns the bare id (a classical cause);
			// the temporal-detail read is a separate, cause-keyed entry (gate cb-u1 r3 MUST 1).
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");
			double sigma = 0.0;
			pl_tex noisy = rig.Grey(WhiteNoiseField(2.0, 4242u, sigma));
			pl_tex dark = rig.Grey(std::vector<double>(static_cast<size_t>(GrainRig::kSw) * GrainRig::kSh, 20.0));
			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const AdaptiveDownscale::GrainSource good = rig.Source(sigma);
			AdaptiveDownscale::GrainSource outside = good;
			outside.sourceCrop = pl_rect2df{};
			const AdaptiveDownscale::GrainSource mismatched = rig.Source(sigma, GrainRig::kBw, GrainRig::kBh, GrainRig::kBw / 2, GrainRig::kBh / 2);
			Assert::IsFalse(mismatched.Valid(), L"precondition: the mismatched source is not valid");

			struct Outcome
			{
				std::vector<std::string> grainLines;
				FallbackCatalog::Decision grain, td;
				bool active = false;
				bool ran = false;   // the scalar grain pass ran without a failure (r3 SHOULD 1: no read-back needed)
				AdaptiveDownscale::GrainSourceUse use = AdaptiveDownscale::GrainSourceUse::NotSupplied;
			};
			// dark -> noise (a cut), then two repeats of the noisy frame for the read-back, as GrainRig::Measure.
			// frameCount 1 (r3 SHOULD 1): one bound frame only, so no read-back of a bound frame exists yet (Pending).
			auto run = [&](const std::function<AdaptiveDownscale::GrainSource()>& source, bool gammaLight, int frameCount = 4) -> Outcome
			{
				AdaptiveDownscale::Module module;
				AdaptiveDownscaleStage::Stage stage{ module };
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
				host.ssimInChain = []() { return true; };
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
				host.publishStatus = []() {};
				host.grainSource = source;   // empty = the host offers no shared products
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				const pl_tex inputs[4] = { dark, noisy, noisy, noisy };
				const uint64_t sequences[4] = { 1, 2, 2, 2 };
				for (int i = 0; i < frameCount && i < 4; ++i)
				{
					pl_tex_params tp{};
					tp.w = 256; tp.h = 160; tp.format = rig.dstFmt; tp.renderable = true; tp.host_readable = true;
					pl_tex target = pl_tex_create(rig.gpu, &tp);
					pl_frame image = TargetLutGpuFixture::MakeRgbFrame(inputs[i]);
					image.color.transfer = PL_COLOR_TRC_GAMMA22;
					pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
					out.color.transfer = PL_COLOR_TRC_GAMMA22;
					pl_render_params params = pl_render_default_params;
					params.dither_params = nullptr;
					params.error_diffusion = nullptr;
					params.downscaler = &pl_filter_mitchell;
					params.disable_linear_scaling = gammaLight;   // true: the hook sees the gamma code domain (mode 0)
					AdaptiveDownscaleStage::Frame f;
					f.adaptiveDownscale = true;
					f.adaptive = &st;
					f.lowBitrateOff = true;
					f.fboFloat32 = 0;
					f.sourceSequence = sequences[i];
					f.nominalSourceRateHz = 24.0;
					f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(GrainRig::kSw), static_cast<float>(GrainRig::kSh) };
					f.targetCrop = { 0.0f, 0.0f, 256.0f, 160.0f };
					std::vector<const pl_hook*> list = { rig.ssim };
					stage.Register(f, host, { weight, grainWanted, noSsim }, list);
					Assert::IsTrue(list.size() == 3 && list.front() == module.Measure() && list.back() == module.Blend(),
						L"the stage registered measure, SSIM, blend");
					params.hooks = list.data();
					params.num_hooks = static_cast<int>(list.size());
					const bool ok = pl_render_image(rig.fixture.Renderer(), &image, &out, &params);
					pl_gpu_finish(rig.gpu);
					pl_tex_destroy(rig.gpu, &target);
					Assert::IsTrue(ok, L"the render ran");
				}
				Outcome o;
				o.active = module.Active() && module.Failures() == 0 && module.LastStats().valid;
				o.ran = module.Active() && module.Failures() == 0;
				o.use = module.LastGrainSourceUse();
				for (const std::string& line : stage.StatusLines())
					if (line.rfind("Adaptive grain:", 0) == 0) o.grainLines.push_back(line);
				o.grain = stage.GrainSourceFallback();
				o.td = stage.TemporalDetailFallback();
				module.Release();
				Assert::AreEqual(0, pl_renderer_get_errors(rig.fixture.Renderer()).num_disabled_hooks, L"no hook was disabled");
				return o;
			};
			struct Case
			{
				const char* name;
				std::function<AdaptiveDownscale::GrainSource()> source;
				bool gammaLight;
				const char* line;
				bool engaged;
				const char* reason;
			};
			const std::vector<Case> cases = {
				{ "no callback", nullptr, true,
					"Adaptive grain: own estimate (the shared grain products are not supplied to the scaler)", false, "" },
				{ "valid", [&]() { return good; }, true,
					"Adaptive grain: from the shared grain split (P10) and region statistics (P11)", false, "" },
				{ "empty crop", [&]() { return outside; }, true,
					"Adaptive grain: own estimate (no usable shared grain block on the last read-back frame)", true,
					"grain: no usable shared block" },
				{ "grid mismatch", [&]() { return mismatched; }, true,
					"Adaptive grain: own estimate (the shared grain products are not supplied to the scaler)", true,
					"grain: the offered shared products are invalid (missing or grid mismatch)" },
				{ "linear measure domain", [&]() { return good; }, false,
					"Adaptive grain: own estimate (measure domain is linear / HDR; the shared products are in source codes)", true,
					"grain: measure domain is linear / HDR" },
			};
			for (const Case& c : cases)
			{
				const Outcome o = run(c.source, c.gammaLight);
				const std::wstring name = ExtendedRangeGpuProbe::ToWide(c.name);
				Assert::IsTrue(o.active, (name + L": the scalar grain pass ran and its statistics were read back").c_str());
				Assert::AreEqual(size_t(1), o.grainLines.size(), (name + L": exactly one grain status line").c_str());
				Assert::AreEqual(std::string(c.line), o.grainLines[0], (name + L": the grain status line").c_str());
				Assert::AreEqual(std::string("product.adaptive_downscale"), o.grain.id, (name + L": the fallback id").c_str());
				Assert::IsTrue(c.engaged == o.grain.engaged, (name + L": GrainSourceFallback engaged / released").c_str());
				Assert::AreEqual(std::string(c.reason), o.grain.reason, (name + L": the GrainSourceFallback reason").c_str());
				// Two entries: the temporal-detail read is not engaged in reset 1, and it never uses the bare id (r3 MUST 1).
				Assert::IsFalse(o.td.engaged, (name + L": the temporal-detail read path is not engaged").c_str());
			}
			{
				// r3 SHOULD 1: a valid source on the FIRST frame. The products are bound but no frame that bound them has been read
				// back yet (PollStats runs before the pass; a fresh module has no statistics): Pending, never a transient
				// NoUsableBlock, and no downgrade is claimed. Before f41a2c1c5 this read NoUsableBlock and engaged the fallback.
				const Outcome o = run([&]() { return good; }, true, 1);
				Assert::IsTrue(o.ran, L"valid, first frame: the scalar grain pass ran");
				Assert::IsTrue(o.use == AdaptiveDownscale::GrainSourceUse::Pending, L"valid, first frame: LastGrainSourceUse is Pending");
				Assert::AreEqual(size_t(1), o.grainLines.size(), L"valid, first frame: exactly one grain status line");
				Assert::AreEqual(std::string("Adaptive grain: shared grain split (P10) bound; waiting for the first read-back of a bound frame"),
					o.grainLines[0], L"valid, first frame: the Pending status line");
				Assert::AreEqual(std::string("product.adaptive_downscale"), o.grain.id, L"valid, first frame: the fallback id");
				Assert::IsFalse(o.grain.engaged, L"valid, first frame: GrainSourceFallback is Released (nothing dropped yet)");
			}
			Assert::IsTrue(TdProducts::FallbackKey(TdProducts::Consumer::AdaptiveScaler, TdProducts::kCauseAbsent) != std::string("product.adaptive_downscale"),
				L"the temporal-detail read is keyed by cause, never under the grain decision's bare id");
		}

		TEST_METHOD(UpscaleDoesNotEmitDownscaleGrainSourceFallbackOrStatusLine)
		{
			// S4-H2: the upscale form's grain term comes from kP10gGrainLevel via ReadGrain (never the downscale scalar
			// GrainSource P10/P11 seam). Even when the host always supplies host.grainSource (as LibplaceboVideoRenderer does),
			// an active upscale frame must NOT invoke host.grainSource, must NOT engage GrainSourceFallback()
			// ("product.adaptive_downscale"), and must NOT emit downscale's "Adaptive grain: ..." status line.
			GrainRig rig;
			Assert::IsTrue(rig.Init(), L"the WARP test device, formats and SSimDownscaler.glsl are available");

			static constexpr int kUpSw = 64, kUpSh = 64, kUpDw = 128, kUpDh = 128;
			static constexpr int kDownDw = 256, kDownDh = 160;
			pl_tex upSrc = nullptr;
			{
				std::vector<uint16_t> px(static_cast<size_t>(kUpSw) * kUpSh * 4);
				for (int y = 0; y < kUpSh; ++y)
					for (int x = 0; x < kUpSw; ++x)
					{
						const uint16_t v = static_cast<uint16_t>(std::lround((110.0 + ((x + y) % 32)) * 257.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * kUpSw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = kUpSw; tp.h = kUpSh; tp.format = rig.srcFmt; tp.sampleable = true; tp.initial_data = px.data();
				upSrc = pl_tex_create(rig.gpu, &tp);
				Assert::IsNotNull(upSrc, L"precondition: upscale source texture created");
				rig.owned.push_back(upSrc);
			}
			double sigma = 0.0;
			pl_tex downSrc = rig.Grey(WhiteNoiseField(2.0, 9001u, sigma));
			Assert::IsNotNull(downSrc, L"precondition: downscale source texture created");

			const AdaptiveDownscale::Settings st = GrainRig::Ramp();
			const AdaptiveDownscale::GrainSource invalidSource{};
			const AdaptiveDownscale::GrainSource validSource = rig.Source(sigma);
			Assert::IsFalse(invalidSource.Valid(), L"precondition: empty GrainSource is invalid");
			Assert::IsTrue(validSource.Valid(), L"precondition: shared GrainSource is valid");

			auto countDownscaleGrainLines = [](const std::vector<std::string>& lines) -> size_t
			{
				size_t count = 0;
				for (const std::string& line : lines)
					if (line.rfind("Adaptive grain:", 0) == 0)
						++count;
				return count;
			};

			AdaptiveDownscale::Module module;
			AdaptiveDownscaleStage::Stage stage{ module };
			int grainSourceCalls = 0;
			bool returnValidSource = false;
			AdaptiveDownscaleStage::Host host;
			host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
			host.bitrate = [](double& q) { q = 1.0; return false; };
			host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
			host.ssimInChain = []() { return true; };
			host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
			host.publishStatus = []() {};
			host.grainSource = [&]() -> AdaptiveDownscale::GrainSource
			{
				++grainSourceCalls;
				return returnValidSource ? validSource : invalidSource;
			};

			double weight = 1.0;
			bool grainWanted = false, noSsim = false;
			uint64_t sequence = 0;
			auto renderFrame = [&](pl_tex src, int sw, int sh, int dw, int dh, bool adaptiveUpscale, bool adaptiveDownscale)
			{
				AdaptiveDownscaleStage::Frame f;
				f.adaptiveUpscale = adaptiveUpscale;
				f.adaptiveDownscale = adaptiveDownscale;
				f.adaptive = &st;
				f.lowBitrateOff = true;
				f.fboFloat32 = 0;
				f.sourceSequence = ++sequence;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh) };

				std::vector<const pl_hook*> list;
				if (!adaptiveUpscale && adaptiveDownscale)
					list.push_back(rig.ssim);
				stage.Register(f, host, { weight, grainWanted, noSsim }, list);
				if (adaptiveUpscale)
					Assert::IsTrue(list.size() == 2 && list.front() == module.Measure() && list.back() == module.Blend(),
						L"upscale registered measure and blend");
				else
					Assert::IsTrue(list.size() == 3 && list.front() == module.Measure() && list.back() == module.Blend(),
						L"downscale registered measure, SSIM, blend");

				pl_tex_params tp{};
				tp.w = dw; tp.h = dh; tp.format = rig.dstFmt; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(rig.gpu, &tp);
				Assert::IsNotNull(target, L"target texture created");
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_GAMMA22;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.upscaler = &pl_filter_ewa_lanczossharp;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;
				params.sigmoid_params = nullptr;
				params.hooks = list.data();
				params.num_hooks = static_cast<int>(list.size());
				const bool ok = pl_render_image(rig.fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(rig.gpu);
				pl_tex_destroy(rig.gpu, &target);
				Assert::IsTrue(ok, L"the WARP render ran");
			};

			// 1. Upscale frame (64x64 -> 128x128) with host.grainSource returning an invalid/empty GrainSource.
			returnValidSource = false;
			grainSourceCalls = 0;
			renderFrame(upSrc, kUpSw, kUpSh, kUpDw, kUpDh, true, false);
			Assert::IsTrue(stage.UpscaleThisFrame(), L"upscale (invalid offered GrainSource): stage must be in upscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"upscale (invalid offered GrainSource): module must be active with zero failures");
			Assert::AreEqual(1, module.LastUpscaleRatio(), L"upscale (invalid offered GrainSource): prescale ratio must be 1");
			Assert::AreEqual(0, grainSourceCalls, L"upscale (invalid offered GrainSource): host.grainSource must not be invoked on upscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"upscale (invalid offered GrainSource): fallback id");
				Assert::IsFalse(d.engaged, L"upscale (invalid offered GrainSource): downscale GrainSourceFallback must NOT engage on upscale");
				Assert::AreEqual(std::string(), d.reason, L"upscale (invalid offered GrainSource): released fallback reason must be empty");
				Assert::AreEqual(size_t(0), countDownscaleGrainLines(stage.StatusLines()),
					L"upscale (invalid offered GrainSource): StatusLines must contain zero downscale 'Adaptive grain:' lines");
			}

			// 2. Upscale frame (64x64 -> 128x128) with host.grainSource returning a valid GrainSource.
			returnValidSource = true;
			grainSourceCalls = 0;
			renderFrame(upSrc, kUpSw, kUpSh, kUpDw, kUpDh, true, false);
			Assert::IsTrue(stage.UpscaleThisFrame(), L"upscale (valid offered GrainSource): stage must be in upscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"upscale (valid offered GrainSource): module must be active with zero failures");
			Assert::AreEqual(0, grainSourceCalls, L"upscale (valid offered GrainSource): host.grainSource must not be invoked on upscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"upscale (valid offered GrainSource): fallback id");
				Assert::IsFalse(d.engaged, L"upscale (valid offered GrainSource): downscale GrainSourceFallback must NOT engage on upscale");
				Assert::AreEqual(std::string(), d.reason, L"upscale (valid offered GrainSource): released fallback reason must be empty");
				Assert::AreEqual(size_t(0), countDownscaleGrainLines(stage.StatusLines()),
					L"upscale (valid offered GrainSource): StatusLines must contain zero downscale 'Adaptive grain:' lines");
			}

			// 3. MANDATORY IN-TEST NEGATIVE CONTROL (exact same stage, module, and host with the exact same invalid
			// host.grainSource callback, switched to downscale 384x240 -> 256x160 with SSIM in chain): proves the downscale
			// GrainSourceFallback detector and "Adaptive grain:" status line are genuinely active and NOT disabled globally.
			returnValidSource = false;
			grainSourceCalls = 0;
			renderFrame(downSrc, GrainRig::kSw, GrainRig::kSh, kDownDw, kDownDh, false, true);
			Assert::IsFalse(stage.UpscaleThisFrame(), L"negative control (downscale): stage must be in downscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"negative control (downscale): module must be active with zero failures");
			Assert::IsTrue(grainSourceCalls > 0, L"negative control (downscale): host.grainSource must be queried on downscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"negative control (downscale): fallback id");
				Assert::IsTrue(d.engaged, L"negative control (downscale): invalid offered GrainSource must engage GrainSourceFallback on downscale");
				Assert::AreEqual(std::string("grain: the offered shared products are invalid (missing or grid mismatch)"), d.reason,
					L"negative control (downscale): expected invalid shared products fallback reason");
				Assert::AreEqual(size_t(1), countDownscaleGrainLines(stage.StatusLines()),
					L"negative control (downscale): StatusLines must contain exactly one 'Adaptive grain:' line on downscale");
			}

			// 4. Downscale -> Upscale transition on the exact same stage & module (both adaptiveUpscale and adaptiveDownscale
			// enabled, as in typical user settings): switching back to upscale must clear the downscale fallback and status line.
			returnValidSource = false;
			grainSourceCalls = 0;
			renderFrame(upSrc, kUpSw, kUpSh, kUpDw, kUpDh, true, true);
			Assert::IsTrue(stage.UpscaleThisFrame(), L"downscale->upscale transition: stage must be in upscale mode");
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"downscale->upscale transition: module must be active with zero failures");
			Assert::AreEqual(0, grainSourceCalls, L"downscale->upscale transition: host.grainSource must not be invoked on upscale");
			{
				const FallbackCatalog::Decision d = stage.GrainSourceFallback();
				Assert::AreEqual(std::string("product.adaptive_downscale"), d.id, L"downscale->upscale transition: fallback id");
				Assert::IsFalse(d.engaged, L"downscale->upscale transition: stale downscale fallback must be released on upscale");
				Assert::AreEqual(size_t(0), countDownscaleGrainLines(stage.StatusLines()),
					L"downscale->upscale transition: stale 'Adaptive grain:' status line must disappear on upscale");
			}

			module.Release();
			Assert::AreEqual(0, pl_renderer_get_errors(rig.fixture.Renderer()).num_disabled_hooks, L"no hook was disabled");
		}

		// Q#1 (GEM-069 / ADS-STATE): PQ inverse luminance (GuardLinear / MeasureLumaCode) must stay finite and
		// non-negative for all valid [0, 1] codes AND for overshoot WTW / ringing codes above 1.0, including
		// codes past the SMPTE ST 2084 rational pole c_pole = (18.8515625 / 18.6875)^78.84375 ~= 1.99206008
		// where e = c^(1 / 78.84375) crosses 18.8515625 / 18.6875 ~= 1.00877926 and the unclamped denominator
		// goes negative (pow(negative, 1 / m1) -> NaN).
		TEST_METHOD(GuardLinearIsFiniteAbovePqOne)
		{
			pl_color_space pqSpace{};
			pqSpace.primaries = PL_COLOR_PRIM_BT_2020;
			pqSpace.transfer = PL_COLOR_TRC_PQ;
			const AdaptiveDownscale::GuardTransferParams pq = AdaptiveDownscale::GuardTransfer(pqSpace);
			Assert::AreEqual(1.0f, pq.mode, 1e-6f, L"precondition: PQ GuardTransfer must select mode 1");
			const AdaptiveDownscale::MeasureDomain domain =
				AdaptiveDownscale::MeasureDomainFor(pqSpace, PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020);
			Assert::AreEqual(2.0f, domain.mode, 1e-6f, L"precondition: PQ MeasureDomainFor must select HDR decode mode 2");

			auto unclampedPqLinear = [](double c) -> double
			{
				const double e = std::pow(c, 1.0 / 78.84375);
				const double denom = 18.8515625 - 18.6875 * e;
				const double n = (std::max)(e - 0.8359375, 0.0) / denom;
				return std::pow(n, 1.0 / 0.1593017578125) * (10000.0 / 203.0);
			};

			// 1. Valid PQ domain [0, 1] must match exact SMPTE ST 2084 EOTF (clamping must not disturb valid codes).
			const double validCodes[] = { 0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 };
			for (double c : validCodes)
			{
				const double got = AdaptiveDownscale::GuardLinear(pq, c);
				const double expected = c <= 0.0 ? 0.0 : unclampedPqLinear(c);
				const std::wstring msg = L"valid PQ code c=" + std::to_wstring(c) +
					L": got=" + std::to_wstring(got) + L", expected=" + std::to_wstring(expected);
				Assert::IsTrue(std::isfinite(got), msg.c_str());
				Assert::IsTrue(std::fabs(got - expected) <= 1e-9 * (1.0 + expected), msg.c_str());
			}
			const double atOne = AdaptiveDownscale::GuardLinear(pq, 1.0);
			Assert::AreEqual(10000.0 / 203.0, atOne, 1e-6, L"PQ c=1.0 must equal 10000/203 relative linear");
			const double btb = AdaptiveDownscale::GuardLinear(pq, -0.05);
			Assert::IsTrue(std::isfinite(btb) && btb == 0.0, L"negative BTB code must clamp cleanly to 0");

			// 2. Overshoot WTW codes above PQ 1.0 — both mild (1.005..1.5) and pole-crossing (2.0..4.0, past
			// c_pole ~= 1.99206008) — must stay finite, non-negative, and >= peak PQ white in GuardLinear and MeasureLumaCode.
			const double overshootCodes[] = { 1.005, 1.01, 1.02, 1.05, 1.2, 1.5, 2.0, 2.05, 2.5, 4.0 };
			for (double c : overshootCodes)
			{
				const double got = AdaptiveDownscale::GuardLinear(pq, c);
				const std::wstring msg = L"overshoot PQ code c=" + std::to_wstring(c) + L": GuardLinear=" + std::to_wstring(got);
				Assert::IsTrue(std::isfinite(got), (L"GuardLinear must stay finite: " + msg).c_str());
				Assert::IsTrue(got >= atOne - 1e-6, (L"GuardLinear above 1.0 must not drop below peak PQ: " + msg).c_str());

				const double rgb[3] = { c, c, c };
				const double lumaCode = AdaptiveDownscale::MeasureLumaCode(domain, rgb);
				Assert::IsTrue(std::isfinite(lumaCode) && lumaCode >= 0.0,
					(L"MeasureLumaCode must stay finite and non-negative for overshoot PQ code c=" + std::to_wstring(c)).c_str());
			}

			// 3. MANDATORY IN-TEST NEGATIVE CONTROL on the exact same pole-crossing overshoot codes (> c_pole ~= 1.99206):
			// the unclamped ST 2084 formula crosses denominator 0 and produces NaN in both double and float32, proving
			// the overshoot hazard is real and the assertions above are non-vacuous.
			const double poleCrossingCodes[] = { 2.0, 2.05, 2.5, 4.0 };
			for (double c : poleCrossingCodes)
			{
				const double e = std::pow(c, 1.0 / 78.84375);
				const double denom = 18.8515625 - 18.6875 * e;
				const double unclamped = unclampedPqLinear(c);
				Assert::IsTrue(denom < 0.0,
					(L"negative control: PQ inverse denominator must be negative at c=" + std::to_wstring(c)).c_str());
				Assert::IsFalse(std::isfinite(unclamped),
					(L"negative control (double): unclamped PQ inverse must produce NaN at c=" + std::to_wstring(c)).c_str());

				const float cf = static_cast<float>(c);
				const float ef = std::pow(cf, 1.0f / 78.84375f);
				const float unclampedF = std::pow((std::max)(ef - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * ef),
					1.0f / 0.1593017578125f);
				Assert::IsFalse(std::isfinite(unclampedF),
					(L"negative control (float32): unclamped PQ inverse must produce NaN at c=" + std::to_wstring(c)).c_str());
			}
		}

		// Q#1 (GEM-069 / ADS-STATE, WARP GPU):
		//  0. Direct WARP GPU test of MeasureDomainGlsl.h (ad_lin / ad_luma via DispatchEdgeMapStage) on overshootTex
		//     containing valid PQ 1.0 and pole-crossing PQ codes {2.05, 2.50, 4.00}: every output RG32F texel must be
		//     strictly finite and every overshoot pixel's luma must be >= MeasureLumaCode(pqDomain, {1, 1, 1}) - 1e-2f
		//     (defeats FXC SM5.0 smoothstep(NaN)->0.0 masking).
		//  1. An RGBA32F PL_COLOR_TRC_PQ frame with a high-grain background plus overshoot pixels above PQ 1.0 and past
		//     the ST 2084 pole (c = 1.00, 1.05, 1.20, 1.50, 2.05, 2.50, 4.00) rendered through AdaptiveDownscale::Module
		//     + SSimDownscaler must keep both persistent grain state (LastStats) and every float32 output pixel strictly
		//     finite AND drive stats2.grainScale < 0.90f (catching DXBC smoothstep(NaN) -> 0.0 -> target = 1.0).
		//  2. Poisoned-state recovery on WARP: both end-to-end (clean -> NaN -> clean) AND direct WARP dispatch of the
		//     real production FINAL_BODY_B from AdaptiveDownscale.cpp on a 1x1 RGBA32F state_io texture pre-uploaded with
		//     { quiet_NaN(), 0, 0, 1 } (cut_in = 0, smooth_alpha = 0.1) must snap cleanly to finite target (with an
		//     in-test WARP negative control proving pre-fix ordered prev.r < 0.0 stays NaN), plus structural guards on
		//     FINAL_BODY_B, MAP_BODY, and STABILISE_GUARD_DEFINE's rs_lin.
		TEST_METHOD(MeasureStateStaysFiniteOnAnOvershootPixel)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			ScopedLogCapture logGuard(fixture, log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			ScopedHook ssim(pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size()));
			Assert::IsNotNull(ssim.hook, L"SSimDownscaler.glsl must parse");

			ScopedDispatch dispatch(pl_dispatch_create(fixture.Log(), gpu));
			Assert::IsNotNull(dispatch.dispatch, L"pl_dispatch_create failed on WARP");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rg32fFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rgba32fRwFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && rg32fFmt != nullptr && rgba32fRwFmt != nullptr,
				L"WARP must provide sampleable, storable, renderable, and host-readable RG32F/RGBA32F");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			auto makeFloatTex = [&](const std::vector<float>& rgba) -> pl_tex
			{
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex tex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(tex, L"RGBA32F source texture creation failed");
				return tex;
			};

			// Genuinely smooth, grain-free surface (energy << grainLo = 2.0 in both PQ and gamma22) so
			// Frame 1 initializes state.r = 1.0 (catching DXBC smoothstep(NaN) -> 0.0 -> target = 1.0).
			std::vector<float> cleanData(static_cast<size_t>(sw) * sh * 4, 0.0f);
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					const float v = 0.40f + 0.002f * std::sin(x * 0.15f) * std::cos(y * 0.15f);
					float* p = &cleanData[(static_cast<size_t>(y) * sw + x) * 4];
					p[0] = p[1] = p[2] = v;
					p[3] = 1.0f;
				}
			}

			// High-grain deterministic PQ background (energy ~= 19.4 >> grainHi = 8.0 -> clean target = grainFloor = 0.40)
			// plus valid PQ peak (1.0), mild WTW (> 1.0), and pole-crossing (> c_pole ~= 1.99206) PQ overshoot pixels.
			const float overshootValues[] = { 1.00f, 1.05f, 1.20f, 1.50f, 2.05f, 2.50f, 4.00f };
			static constexpr size_t kNumOvershootValues = sizeof(overshootValues) / sizeof(overshootValues[0]);
			std::vector<float> overshootData(static_cast<size_t>(sw) * sh * 4, 0.0f);
			{
				uint32_t grainSeed = 12345u;
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						grainSeed = grainSeed * 1664525u + 1013904223u;
						const float n = (static_cast<float>(grainSeed >> 8) / 16777216.0f - 0.5f) * 2.0f;
						const float v = 0.40f + 0.015f * n;
						float* p = &overshootData[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 1.0f;
					}
				}
			}
			for (size_t k = 0; k < kNumOvershootValues; ++k)
			{
				const int px = sw / 2 - 3 + static_cast<int>(k);
				const int py = sh / 2;
				float* p = &overshootData[(static_cast<size_t>(py) * sw + px) * 4];
				p[0] = p[1] = p[2] = overshootValues[k];
				p[3] = 1.0f;
			}

			std::vector<float> nanData = cleanData;
			{
				const float qnan = std::numeric_limits<float>::quiet_NaN();
				float* p = &nanData[(static_cast<size_t>(sh / 2) * sw + (sw / 2)) * 4];
				p[0] = p[1] = p[2] = qnan;
				p[3] = 1.0f;
			}

			ScopedTex cleanTex(gpu, makeFloatTex(cleanData));
			ScopedTex overshootTex(gpu, makeFloatTex(overshootData));
			ScopedTex nanTex(gpu, makeFloatTex(nanData));

			auto assertAllFinite = [](const std::vector<float>& px, const wchar_t* label)
			{
				size_t nonFiniteCount = 0;
				size_t firstBad = 0;
				for (size_t i = 0; i < px.size(); ++i)
				{
					if (!std::isfinite(px[i]))
					{
						if (nonFiniteCount == 0)
							firstBad = i;
						++nonFiniteCount;
					}
				}
				const std::wstring msg = std::wstring(label) + L": found " + std::to_wstring(nonFiniteCount) +
					L" non-finite output components (first at index " + std::to_wstring(firstBad) + L")";
				Assert::AreEqual(size_t(0), nonFiniteCount, msg.c_str());
			};

			// 0. Direct WARP GPU test of MeasureDomainGlsl.h (ad_lin / ad_luma) via DispatchEdgeMapStage on overshootTex.
			//    No smoothstep sits between ad_lin and em_out, so pre-fix unclamped ad_lin writes literal NaN texels
			//    on WARP even under FXC SM5.0, and even a flush-to-zero bug would fail gotLuma >= pq1Luma - 1e-2f.
			{
				pl_tex_params edgeTp{};
				edgeTp.w = sw;
				edgeTp.h = sh;
				edgeTp.format = rg32fFmt;
				edgeTp.sampleable = true;
				edgeTp.storable = true;
				edgeTp.host_readable = true;
				ScopedTex edgeDst(gpu, pl_tex_create(gpu, &edgeTp));
				Assert::IsNotNull(edgeDst.tex, L"RG32F edge-map output texture creation failed");

				pl_color_space pqColor{};
				pqColor.primaries = PL_COLOR_PRIM_BT_2020;
				pqColor.transfer = PL_COLOR_TRC_PQ;
				std::string dispatchWhy;
				const bool dispatched = Vpv::Regions::DispatchEdgeMapStage(dispatch.dispatch, overshootTex.tex, pqColor,
					PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020, edgeDst.tex, dispatchWhy);
				Assert::IsTrue(dispatched,
					(L"DispatchEdgeMapStage(PQ) failed on WARP: " + std::wstring(dispatchWhy.begin(), dispatchWhy.end())).c_str());
				pl_gpu_finish(gpu);

				std::vector<float> edgeRg(static_cast<size_t>(sw) * sh * 2, 0.0f);
				pl_tex_transfer_params edgeDl{};
				edgeDl.tex = edgeDst.tex;
				edgeDl.ptr = edgeRg.data();
				Assert::IsTrue(pl_tex_download(gpu, &edgeDl), L"RG32F edge-map download failed");
				edgeDst.Reset();

				assertAllFinite(edgeRg, L"Direct MeasureDomainGlsl (DispatchEdgeMapStage PQ) RG32F output");

				const VpMeasure::MeasureDomain pqDomain = VpMeasure::MeasureDomainFor(pqColor,
					PL_COLOR_SYSTEM_BT_2020_NC, PL_COLOR_PRIM_BT_2020);
				const double pqWhiteRgb[3] = { 1.0, 1.0, 1.0 };
				const float pq1Luma = static_cast<float>(VpMeasure::MeasureLumaCode(pqDomain, pqWhiteRgb));

				for (size_t k = 0; k < kNumOvershootValues; ++k)
				{
					const int px = sw / 2 - 3 + static_cast<int>(k);
					const int py = sh / 2;
					const size_t idx = (static_cast<size_t>(py) * sw + px) * 2;
					const float gotSobel = edgeRg[idx + 0];
					const float gotLuma = edgeRg[idx + 1];
					Assert::IsTrue(std::isfinite(gotSobel) && std::isfinite(gotLuma),
						(L"Direct MeasureDomainGlsl: non-finite RG32F texel at PQ c=" + std::to_wstring(overshootValues[k])).c_str());
					Assert::IsTrue(gotLuma >= pq1Luma - 1e-2f,
						(L"Direct MeasureDomainGlsl: PQ overshoot luma must be >= PQ 1.0 luma (" + std::to_wstring(pq1Luma) +
							L") at c=" + std::to_wstring(overshootValues[k]) + L", got " + std::to_wstring(gotLuma)).c_str());
					if (k == 0)
					{
						Assert::IsTrue(std::fabs(gotLuma - pq1Luma) <= 1.0f,
							(L"Direct MeasureDomainGlsl: valid PQ 1.0 pixel must match CPU MeasureLumaCode (" +
								std::to_wstring(pq1Luma) + L"), got " + std::to_wstring(gotLuma)).c_str());
					}
				}
			}

			AdaptiveDownscale::Settings st;
			st.grainMap = false;
			st.grainSmoothMs = 500.0f;   // alpha < 1: subsequent frames genuinely mix prev.r unless reset

			auto renderFrame = [&](AdaptiveDownscale::Module& module, uint64_t& sequence, pl_tex src,
				enum pl_color_transfer trc, enum pl_color_primaries prim, bool advance, std::vector<float>* outPx)
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				ScopedTex target(gpu, pl_tex_create(gpu, &tp));
				Assert::IsNotNull(target.tex, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target.tex);
				image.color.transfer = out.color.transfer = trc;
				image.color.primaries = out.color.primaries = prim;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;   // keep PRE_KERNEL in source PQ/gamma domain so ad_lin runs on PQ

				if (advance)
					++sequence;
				module.SetFrame(st, sequence, 1.0f / 24.0f);
				const pl_hook* hooks[3] = { module.Measure(), ssim.hook, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				if (outPx != nullptr)
				{
					outPx->assign(static_cast<size_t>(dw) * dh * 4, 0.0f);
					pl_tex_transfer_params dl{};
					dl.tex = target.tex;
					dl.ptr = outPx->data();
					Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F target download failed");
				}
			};

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			// 1. PQ overshoot test: smooth clean frame (state.r = 1.0) -> high-grain PQ overshoot frames
			//    (with c = 1.00..4.00 past c_pole ~= 1.99206) -> clean frame (no cut).
			{
				ScopedModule module;
				module->SetCutSource(noCutSource);
				uint64_t sequence = 0;
				std::vector<float> out2, out3;

				// Frame 1: initialize state cleanly near 1.0 on the smooth PQ surface.
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, true, nullptr);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				const AdaptiveDownscale::Stats stats1 = module->LastStats();
				Assert::IsTrue(stats1.valid && std::isfinite(stats1.grainScale) && stats1.grainScale > 0.99f,
					(L"clean PQ Frame 1 must initialize state grainScale near 1.0, got " + std::to_wstring(stats1.grainScale)).c_str());

				// Frames 2..9: high-grain PQ background + overshoot PQ pixels (including 2.05, 2.50, 4.00 past c_pole).
				// Over 8 advancing frames (alpha ~= 0.08 toward grainFloor = 0.40), clean grainScale drops to ~0.71 < 0.90;
				// pre-fix unclamped ad_lin poisons t.x -> NaN -> DXBC smoothstep(NaN) == 0 -> target = 1.0 (stuck at 1.0!).
				for (int f = 0; f < 8; ++f)
					renderFrame(*module, sequence, overshootTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, true, f == 7 ? &out2 : nullptr);
				renderFrame(*module, sequence, overshootTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				renderFrame(*module, sequence, overshootTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				const AdaptiveDownscale::Stats stats2 = module->LastStats();
				Assert::IsTrue(module->Active() && module->Failures() == 0, L"PQ overshoot frame: module must stay active with zero failures");
				Assert::IsTrue(stats2.valid, L"PQ overshoot frame: grain state statistics must be read back");
				Assert::IsTrue(std::isfinite(stats2.grainScale) && stats2.grainScale >= st.grainFloor - 1e-4f && stats2.grainScale <= 1.0f + 1e-4f,
					(L"PQ overshoot frame: state grainScale must stay finite and in [grainFloor, 1], got " + std::to_wstring(stats2.grainScale)).c_str());
				Assert::IsTrue(stats2.grainScale < 0.90f,
					(L"PQ overshoot frame: high-grain background must pull grainScale < 0.90 (catches DXBC smoothstep(NaN)->0->target=1), got " +
						std::to_wstring(stats2.grainScale)).c_str());
				Assert::IsTrue(std::isfinite(stats2.meanW) && stats2.meanW >= 0.0f && stats2.meanW <= st.wBase + 1e-4f,
					(L"PQ overshoot frame: state meanW must stay finite and in [0, wBase], got " + std::to_wstring(stats2.meanW)).c_str());
				assertAllFinite(out2, L"PQ overshoot frame output");

				// Subsequent clean PQ frame (no cut) must stay finite and uncorrupted.
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, true, &out3);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_PQ, PL_COLOR_PRIM_BT_2020, false, nullptr);
				const AdaptiveDownscale::Stats stats3 = module->LastStats();
				Assert::IsTrue(module->Active() && module->Failures() == 0, L"post-overshoot clean PQ frame: module must stay active with zero failures");
				Assert::IsTrue(stats3.valid, L"post-overshoot clean PQ frame: statistics must be read back");
				Assert::IsTrue(std::isfinite(stats3.grainScale) && stats3.grainScale >= st.grainFloor - 1e-4f && stats3.grainScale <= 1.0f + 1e-4f,
					(L"post-overshoot clean PQ frame: state grainScale must stay finite, got " + std::to_wstring(stats3.grainScale)).c_str());
				Assert::IsTrue(std::isfinite(stats3.meanW) && stats3.meanW >= 0.0f && stats3.meanW <= st.wBase + 1e-4f,
					(L"post-overshoot clean PQ frame: state meanW must stay finite, got " + std::to_wstring(stats3.meanW)).c_str());
				assertAllFinite(out3, L"post-overshoot clean PQ frame output");
			}

			// 2. Poisoned-state recovery on WARP:
			// (a) End-to-end AdaptiveDownscale::Module: clean frame -> NaN-injected frame -> clean frame with cut = false.
			{
				ScopedModule module;
				module->SetCutSource(noCutSource);
				uint64_t sequence = 0;
				std::vector<float> recoveredOut;

				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, true, nullptr);
				renderFrame(*module, sequence, nanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, true, nullptr);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, true, &recoveredOut);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, false, nullptr);
				renderFrame(*module, sequence, cleanTex, PL_COLOR_TRC_GAMMA22, PL_COLOR_PRIM_BT_709, false, nullptr);
				const AdaptiveDownscale::Stats recovered = module->LastStats();
				Assert::IsTrue(module->Active() && module->Failures() == 0, L"NaN recovery frame: module must stay active with zero failures");
				Assert::IsTrue(recovered.valid, L"NaN recovery frame: statistics must be read back");
				Assert::IsTrue(std::isfinite(recovered.grainScale) && recovered.grainScale >= st.grainFloor - 1e-4f && recovered.grainScale <= 1.0f + 1e-4f,
					(L"NaN recovery frame: FINAL_BODY_B must recover finite grainScale, got " + std::to_wstring(recovered.grainScale)).c_str());
				Assert::IsTrue(std::isfinite(recovered.meanW) && recovered.meanW >= 0.0f && recovered.meanW <= st.wBase + 1e-4f,
					(L"NaN recovery frame: meanW must recover finite, got " + std::to_wstring(recovered.meanW)).c_str());
				assertAllFinite(recoveredOut, L"NaN recovery clean frame output");
			}

			// (b) Direct WARP GPU poisoned-state proof of the real production FINAL_BODY_B extracted from
			//     AdaptiveDownscale.cpp, plus structural guards on FINAL_BODY_B, MAP_BODY, and rs_lin, plus an
			//     in-test WARP GPU negative control proving pre-fix ordered prev.r < 0.0 stays NaN on the same input.
			{
				const std::string adsSource = SettingsTestSupport::ReadFile(
					SettingsTestSupport::RepoRoot() + "src\\VP-V\\modules\\scaling\\output\\AdaptiveDownscale.cpp");
				Assert::IsFalse(adsSource.empty(), L"Failed to read AdaptiveDownscale.cpp for shader guard verification");

				auto extractGlslRaw = [&](const char* marker) -> std::string
				{
					const size_t pos = adsSource.find(marker);
					Assert::IsTrue(pos != std::string::npos,
						(L"AdaptiveDownscale.cpp missing shader literal: " + std::wstring(marker, marker + std::strlen(marker))).c_str());
					const size_t open = adsSource.find("R\"GLSL(", pos);
					Assert::IsTrue(open != std::string::npos, L"Missing R\"GLSL( opening delimiter in AdaptiveDownscale.cpp");
					const size_t start = open + 7;
					const size_t close = adsSource.find(")GLSL\"", start);
					Assert::IsTrue(close != std::string::npos, L"Missing )GLSL\" closing delimiter in AdaptiveDownscale.cpp");
					return adsSource.substr(start, close - start);
				};

				std::string nonfiniteGlsl;
				{
					const size_t nfPos = adsSource.find("const char* const NONFINITE_GLSL =");
					if (nfPos != std::string::npos)
					{
						const size_t q0 = adsSource.find('"', nfPos);
						const size_t q1 = q0 != std::string::npos ? adsSource.find('"', q0 + 1) : std::string::npos;
						Assert::IsTrue(q0 != std::string::npos && q1 != std::string::npos, L"Malformed NONFINITE_GLSL literal");
						std::string raw = adsSource.substr(q0 + 1, q1 - q0 - 1);
						size_t esc = 0;
						while ((esc = raw.find("\\n", esc)) != std::string::npos)
						{
							raw.replace(esc, 2, "\n");
							++esc;
						}
						nonfiniteGlsl = raw;
					}
				}

				const std::string finalHeader = extractGlslRaw("const char* const FINAL_HEADER =");
				const std::string finalBodyA = extractGlslRaw("const char* const FINAL_BODY_A =");
				const std::string finalBodyB = extractGlslRaw("const char* const FINAL_BODY_B =");
				const std::string mapBody = extractGlslRaw("const char* const MAP_BODY =");
				const std::string stabiliseGuard = extractGlslRaw("const char* const STABILISE_GUARD_DEFINE =");

				// Structural invariants: unordered !(prev.r >= 0.0), zero isnan() (FXC folds it), clamped rs_lin pole.
				Assert::IsTrue(finalBodyB.find("!(prev.r >= 0.0)") != std::string::npos,
					L"FINAL_BODY_B must guard state.r with !(prev.r >= 0.0) so NaN triggers immediate self-heal");
				Assert::IsTrue(finalBodyB.find("isnan(") == std::string::npos,
					L"FINAL_BODY_B must not use isnan() (D3DCompile/FXC can fold it)");
				Assert::IsTrue(mapBody.find("!(prev.r >= 0.0)") != std::string::npos,
					L"MAP_BODY must guard prev.r with !(prev.r >= 0.0) so NaN triggers immediate self-heal");
				Assert::IsTrue(mapBody.find("isnan(") == std::string::npos,
					L"MAP_BODY must not use isnan() (D3DCompile/FXC can fold it)");
				Assert::IsTrue(stabiliseGuard.find("max(18.8515625 - 18.6875 * e, 1e-6)") != std::string::npos,
					L"STABILISE_GUARD_DEFINE rs_lin must clamp ST 2084 PQ denominator via max(18.8515625 - 18.6875 * e, 1e-6)");
				Assert::IsTrue(stabiliseGuard.find("isnan(") == std::string::npos,
					L"STABILISE_GUARD_DEFINE must not use isnan()");

				// Direct WARP dispatch of production FINAL_BODY_B against 1x1 RGBA32F state_io = { quiet_NaN(), 0, 0, 1 }.
				const float partialInit[4] = { 100.0f, 10.0f, 10.0f, 0.0f };   // energy = 10.0 > grainHi (8.0) -> target = grainFloor (0.40)
				pl_tex_params partialTp{};
				partialTp.w = 1;
				partialTp.h = 1;
				partialTp.format = srcFmt;
				partialTp.sampleable = true;
				partialTp.initial_data = partialInit;
				ScopedTex partialTex(gpu, pl_tex_create(gpu, &partialTp));
				Assert::IsNotNull(partialTex.tex, L"1x1 RGBA32F partial_in texture creation failed");

				pl_tex_params stateTp{};
				stateTp.w = 1;
				stateTp.h = 1;
				stateTp.format = rgba32fRwFmt;
				stateTp.sampleable = true;
				stateTp.storable = true;
				stateTp.host_writable = true;
				stateTp.host_readable = true;
				ScopedTex stateTex(gpu, pl_tex_create(gpu, &stateTp));
				Assert::IsNotNull(stateTex.tex, L"1x1 RGBA32F state_io texture creation failed");

				pl_tex_params statsTp{};
				statsTp.w = 1;
				statsTp.h = 1;
				statsTp.format = rgba32fRwFmt;
				statsTp.sampleable = true;
				statsTp.storable = true;
				statsTp.host_readable = true;
				ScopedTex statsTex(gpu, pl_tex_create(gpu, &statsTp));
				Assert::IsNotNull(statsTex.tex, L"1x1 RGBA32F ad_stats_img texture creation failed");

				auto dispatchFinalOnPoisonedState = [&](const std::string& headerText, const std::string& bodyText,
					const char* desc, float outState[4], float outStats[4])
				{
					float poisonedState[4] = { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f };
					pl_tex_transfer_params up{};
					up.tex = stateTex.tex;
					up.ptr = poisonedState;
					Assert::IsTrue(pl_tex_upload(gpu, &up), L"RGBA32F poisoned state upload failed");

					pl_shader_desc d[3]{};
					d[0].desc.name = "partial_in";
					d[0].desc.type = PL_DESC_SAMPLED_TEX;
					d[0].binding.object = partialTex.tex;
					d[0].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
					d[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
					d[1].desc.name = "state_io";
					d[1].desc.type = PL_DESC_STORAGE_IMG;
					d[1].desc.access = PL_DESC_ACCESS_READWRITE;
					d[1].binding.object = stateTex.tex;
					d[2].desc.name = "ad_stats_img";
					d[2].desc.type = PL_DESC_STORAGE_IMG;
					d[2].desc.access = PL_DESC_ACCESS_WRITEONLY;
					d[2].binding.object = statsTex.tex;

					const float writeStatsF = 1.0f;
					const float advanceF = 1.0f;
					const float cutInF = 0.0f;
					const float totalPixelsF = 10.0f;
					const float smoothAlphaF = 0.1f;
					const pl_shader_var v[8] = {
						{ pl_var_float("write_stats"), &writeStatsF, true },
						{ pl_var_float("advance"), &advanceF, true },
						{ pl_var_float("cut_in"), &cutInF, true },
						{ pl_var_float("total_pixels"), &totalPixelsF, true },
						{ pl_var_float("grain_lo"), &st.grainLo, true },
						{ pl_var_float("grain_hi"), &st.grainHi, true },
						{ pl_var_float("grain_floor"), &st.grainFloor, true },
						{ pl_var_float("smooth_alpha"), &smoothAlphaF, true } };

					pl_custom_shader custom{};
					custom.description = desc;
					custom.header = headerText.c_str();
					custom.body = bodyText.c_str();
					custom.input = PL_SHADER_SIG_NONE;
					custom.output = PL_SHADER_SIG_NONE;
					custom.descriptors = d;
					custom.num_descriptors = 3;
					custom.variables = v;
					custom.num_variables = 8;
					custom.compute = true;
					custom.compute_group_size[0] = 16;
					custom.compute_group_size[1] = 16;

					pl_shader sh = pl_dispatch_begin(dispatch.dispatch);
					const bool customOk = pl_shader_custom(sh, &custom);
					if (!customOk)
						pl_dispatch_abort(dispatch.dispatch, &sh);
					Assert::IsTrue(customOk, L"pl_shader_custom failed for poisoned-state FINAL_BODY_B test");

					pl_dispatch_compute_params compute{};
					compute.shader = &sh;
					compute.dispatch_size[0] = 1;
					compute.dispatch_size[1] = 1;
					compute.dispatch_size[2] = 1;
					Assert::IsTrue(pl_dispatch_compute(dispatch.dispatch, &compute), L"pl_dispatch_compute failed for poisoned-state FINAL_BODY_B test");
					pl_gpu_finish(gpu);

					pl_tex_transfer_params stateDl{};
					stateDl.tex = stateTex.tex;
					stateDl.ptr = outState;
					Assert::IsTrue(pl_tex_download(gpu, &stateDl), L"RGBA32F state_io download failed");

					pl_tex_transfer_params statsDl{};
					statsDl.tex = statsTex.tex;
					statsDl.ptr = outStats;
					Assert::IsTrue(pl_tex_download(gpu, &statsDl), L"RGBA32F ad_stats_img download failed");
				};

				const std::string prodHeader = "#define AD_STATS_IMG 1\n" + nonfiniteGlsl + finalHeader;
				const std::string prodBody = finalBodyA + finalBodyB;
				float healedState[4] = {};
				float healedStats[4] = {};
				dispatchFinalOnPoisonedState(prodHeader, prodBody, "Q#1 production FINAL_BODY_B NaN self-heal", healedState, healedStats);
				Assert::IsTrue(std::isfinite(healedState[0]) && std::fabs(healedState[0] - st.grainFloor) < 1e-4f,
					(L"Production FINAL_BODY_B on WARP must snap poisoned prev.r=NaN cleanly to target=" +
						std::to_wstring(st.grainFloor) + L", got " + std::to_wstring(healedState[0])).c_str());
				Assert::IsTrue(healedState[3] > 1.5f,
					(L"Production FINAL_BODY_B on WARP must set state.a=2.0 (cut/self-heal flag), got " +
						std::to_wstring(healedState[3])).c_str());
				Assert::IsTrue(std::isfinite(healedStats[0]) && std::fabs(healedStats[0] - st.grainFloor) < 1e-4f,
					(L"Production FINAL_BODY_B on WARP must publish finite healed stats=" +
						std::to_wstring(st.grainFloor) + L", got " + std::to_wstring(healedStats[0])).c_str());

				// In-test WARP GPU negative control on the exact same poisoned { quiet_NaN(), 0, 0, 1 } texture:
				// pre-fix ordered comparison (prev.r < 0.0) returns false for NaN, leaving state.r stuck at NaN!
				const std::string naiveHeader = "#define AD_STATS_IMG 1\n" + finalHeader;
				const std::string naiveBody = finalBodyA + R"GLSL(    bool cut = prev.a < 0.5 || cut_in > 0.5 || prev.r < 0.0;
    float scale = cut ? target : mix(prev.r, target, smooth_alpha);
    imageStore(state_io, ivec2(0), vec4(scale, free_mean, 0.0, cut ? 2.0 : 1.0));
    if (write_stats > 0.5) {
        imageStore(ad_stats_img, ivec2(0), vec4(scale, free_mean, 0.0, stats_tag));
    }
}
)GLSL";
				float unhealedState[4] = {};
				float unhealedStats[4] = {};
				dispatchFinalOnPoisonedState(naiveHeader, naiveBody, "Q#1 negative control pre-fix ordered prev.r < 0.0", unhealedState, unhealedStats);
				Assert::IsFalse(std::isfinite(unhealedState[0]),
					(L"WARP negative control: pre-fix ordered prev.r < 0.0 must fail to heal NaN on WARP, got " +
						std::to_wstring(unhealedState[0])).c_str());
				Assert::IsTrue(unhealedState[3] < 1.5f,
					(L"WARP negative control: pre-fix ordered prev.r < 0.0 must not signal cut on NaN, got state.a=" +
						std::to_wstring(unhealedState[3])).c_str());
			}

			// 3. MANDATORY IN-TEST NEGATIVE CONTROLS (CPU float32 math):
			// (a) On the exact pole-crossing overshoot pixels uploaded into overshootTex (2.05, 2.50, 4.00), the
			//     unclamped float32 PQ inverse formula genuinely produces NaN (proving the GPU test input is hazardous).
			for (float c : { 2.05f, 2.50f, 4.00f })
			{
				const float e = std::pow(c, 1.0f / 78.84375f);
				const float unclamped = std::pow((std::max)(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e),
					1.0f / 0.1593017578125f);
				Assert::IsFalse(std::isfinite(unclamped),
					(L"negative control: unclamped PQ inverse must produce NaN for overshootTex pixel c=" + std::to_wstring(c)).c_str());
			}
			// (b) On NaN, !(nan >= 0.0) is true (triggers FINAL_BODY_B state reset) whereas ordered (nan < 0.0) is false
			//     (would fail to detect poisoned state, leaving mix(prev.r, target, alpha) stuck at NaN).
			const volatile float vNan = std::numeric_limits<float>::quiet_NaN();
			const float nan = vNan;
			Assert::IsTrue(!(nan >= 0.0f), L"!(NaN >= 0.0) must be true to detect and reset poisoned state");
			Assert::IsFalse(nan < 0.0f, L"negative control: ordered comparison (NaN < 0.0) is false and fails to detect NaN");
			Assert::IsFalse(std::isfinite(nan + 0.1f * (1.0f - nan)),
				L"negative control: unguarded one-pole mix(NaN, target, alpha) remains non-finite");

			dispatch.Reset();
			nanTex.Reset();
			overshootTex.Reset();
			cleanTex.Reset();
			ssim.Reset();
			logGuard.Release();
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#2 (GEM-070 / ADS-STATE, WARP GPU):
		// Residual stabilisation must stop holding Frame 1's luma residual across a confirmed scene cut
		// (SetCutSource -> CutVerdict::State::Known, cut = true), for both plain (residualGrainGuard = false)
		// and guarded (residualGrainGuard = true) paths, including on a repeat of the cut frame.
		// Mandatory in-test negative control: the exact same Frame 1 -> Frame 2 inputs with cut = false
		// MUST hold Frame 1's residual (> 0.01 delta vs fresh Frame 2), proving the hazard is real.
		TEST_METHOD(ResidualHoldStopsAtAConfirmedCut)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt fieldFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt trustFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && fieldFmt != nullptr && trustFmt != nullptr,
				L"WARP must provide float32 source, target, motion field, and frame-trust formats");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			static constexpr int bw = sw / 16, bh = sh / 16;

			auto makeSourceTex = [&](const std::function<float(int, int)>& pattern) -> pl_tex
			{
				std::vector<float> rgba(static_cast<size_t>(sw) * sh * 4, 0.0f);
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						const float v = pattern(x, y);
						float* p = &rgba[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 1.0f;
					}
				}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex tex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(tex, L"RGBA32F source texture creation failed");
				return tex;
			};

			// Frame 1: high-contrast 2x2 checkerboard (left) + 2-px vertical stripes (right) so SSIM and
			// faithful ewa_lanczossharp diverge strongly at 2x downscale (large Frame-1 residual).
			pl_tex frame1Tex = makeSourceTex([](int x, int y) -> float
			{
				if (x < sw / 2)
					return (((x / 2) + (y / 2)) % 2) != 0 ? 0.90f : 0.10f;
				return ((x / 2) % 2) != 0 ? 0.90f : 0.10f;
			});

			// Frame 2: completely different scene — uniform flat 0.50 (top half, zero own residual)
			// plus horizontal stripes (bottom half, orthogonal non-zero own residual).
			pl_tex frame2Tex = makeSourceTex([](int, int y) -> float
			{
				if (y < sh / 2)
					return 0.50f;
				return ((y / 2) % 2) != 0 ? 0.72f : 0.28f;
			});

			// Valid zero-motion field: v = (0, 0), trust = 1.0 on every block; frameTrust = 1.0.
			std::vector<float> fieldData(static_cast<size_t>(bw) * bh * 4, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
			{
				fieldData[i * 4 + 0] = 0.0f;
				fieldData[i * 4 + 1] = 0.0f;
				fieldData[i * 4 + 2] = 1.0f;
				fieldData[i * 4 + 3] = 0.0f;
			}
			pl_tex_params fp{};
			fp.w = bw;
			fp.h = bh;
			fp.format = fieldFmt;
			fp.sampleable = true;
			fp.initial_data = fieldData.data();
			pl_tex fieldTex = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(fieldTex, L"motion field texture creation failed");

			const float trustVal = 1.0f;
			pl_tex_params trp{};
			trp.w = 1;
			trp.h = 1;
			trp.format = trustFmt;
			trp.sampleable = true;
			trp.initial_data = &trustVal;
			pl_tex trustTex = pl_tex_create(gpu, &trp);
			Assert::IsNotNull(trustTex, L"frame trust texture creation failed");

			AdaptiveDownscale::Motion motion;
			motion.field = fieldTex;
			motion.frameTrust = trustTex;
			motion.minTrustedShare = 0.25f;
			motion.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };

			auto renderFrame = [&](AdaptiveDownscale::Module& module, const AdaptiveDownscale::Settings& st,
				uint64_t seq, pl_tex src, bool withMotion) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;

				module.SetFrame(st, seq, 1.0f / 24.0f);
				if (withMotion)
					module.SetMotion(motion);
				else
					module.ClearMotion();

				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				std::vector<float> px(static_cast<size_t>(dw) * dh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F target download failed");
				pl_tex_destroy(gpu, &target);
				return px;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"output frame sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"output pixels must remain finite");
					if (i % 4 == 3)
						continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst)
						worst = d;
				}
				return worst;
			};

			auto makeSettings = [](bool stabilise, bool grainGuard)
			{
				AdaptiveDownscale::Settings st;
				st.wBase = 1.0f;
				// Hold e = 0 and g = 1 everywhere regardless of high-contrast stripes/checkerboard
				// so the only difference between Frame 2 runs is the residual hold.
				st.edgeLo = 10000.0f;
				st.edgeHi = 20000.0f;
				st.grainLo = 10000.0f;
				st.grainHi = 20000.0f;
				st.shadowBias = 0.0f;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = grainGuard;
				st.residualCodingNoiseCodes = 0.0f;
				st.float32Picture = true;
				return st;
			};

			auto constantCutSource = [](bool cutOnSeq2)
			{
				return [cutOnSeq2](uint64_t, uint64_t seq)
				{
					AdaptiveDownscale::CutVerdict v;
					v.state = AdaptiveDownscale::CutVerdict::State::Known;
					v.cut = cutOnSeq2 && (seq == 2);
					return v;
				};
			};

			// Precondition: verify Frame 1 has a large SSIM-vs-faithful residual (> 0.05).
			{
				AdaptiveDownscale::Module preMod;
				preMod.SetCutSource(constantCutSource(false));
				AdaptiveDownscale::Settings ssimSt = makeSettings(false, false);
				AdaptiveDownscale::Settings faithfulSt = ssimSt;
				faithfulSt.wBase = 0.0f;
				const std::vector<float> f1Ssim = renderFrame(preMod, ssimSt, 1, frame1Tex, false);
				const std::vector<float> f1Faithful = renderFrame(preMod, faithfulSt, 2, frame1Tex, false);
				const float f1Residual = maxRgbAbsDiff(f1Ssim, f1Faithful);
				Assert::IsTrue(f1Residual > 0.05f,
					(L"precondition: Frame 1 must have strong SSIM-vs-faithful residual (> 0.05), got " +
						std::to_wstring(f1Residual)).c_str());
				preMod.Release();
			}

			for (bool grainGuard : { false, true })
			{
				const std::wstring modeLabel = grainGuard ? L"[residualGrainGuard=true] " : L"[residualGrainGuard=false] ";
				const AdaptiveDownscale::Settings stStab = makeSettings(true, grainGuard);
				const AdaptiveDownscale::Settings stNoStab = makeSettings(false, grainGuard);

				// Fresh non-stabilised Frame 2 reference.
				std::vector<float> freshFrame2;
				{
					AdaptiveDownscale::Module refMod;
					refMod.SetCutSource(constantCutSource(false));
					freshFrame2 = renderFrame(refMod, stNoStab, 1, frame2Tex, false);
					Assert::IsTrue(refMod.Active() && refMod.Failures() == 0,
						(modeLabel + L"fresh Frame 2 reference must render cleanly").c_str());
					refMod.Release();
				}

				// 1. Confirmed cut on Frame 2 (seq = 2): Frame 1's residual must NOT bleed into Frame 2
				//    (nor into a repeat presentation of Frame 2).
				{
					AdaptiveDownscale::Module cutMod;
					cutMod.SetCutSource(constantCutSource(true));
					renderFrame(cutMod, stStab, 1, frame1Tex, true);
					const std::vector<float> cutFrame2 = renderFrame(cutMod, stStab, 2, frame2Tex, true);
					const std::vector<float> cutFrame2Repeat = renderFrame(cutMod, stStab, 2, frame2Tex, true);
					Assert::IsTrue(cutMod.Active() && cutMod.Failures() == 0,
						(modeLabel + L"confirmed-cut module must stay active with zero failures").c_str());

					const float cutDiff = maxRgbAbsDiff(cutFrame2, freshFrame2);
					const float repeatDiff = maxRgbAbsDiff(cutFrame2Repeat, freshFrame2);
					Assert::IsTrue(cutDiff <= 1e-4f,
						(modeLabel + L"confirmed cut must stop Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(cutDiff) + L")").c_str());
					Assert::IsTrue(repeatDiff <= 1e-4f,
						(modeLabel + L"repeat of confirmed-cut frame must have zero Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(repeatDiff) + L")").c_str());
					cutMod.Release();
				}

				// 2. MANDATORY IN-TEST NEGATIVE CONTROL (exact same Frame 1 -> Frame 2 inputs with cut = false):
				//    Frame 2 MUST hold Frame 1's residual (diverging from fresh Frame 2 by > 0.01), proving
				//    residual hold is genuinely active and only stopped by the confirmed cut.
				{
					AdaptiveDownscale::Module noCutMod;
					noCutMod.SetCutSource(constantCutSource(false));
					renderFrame(noCutMod, stStab, 1, frame1Tex, true);
					const std::vector<float> noCutFrame2 = renderFrame(noCutMod, stStab, 2, frame2Tex, true);
					Assert::IsTrue(noCutMod.Active() && noCutMod.Failures() == 0,
						(modeLabel + L"no-cut negative control module must stay active with zero failures").c_str());
					Assert::IsTrue(noCutMod.StabilisedFrames() >= 1ull,
						(modeLabel + L"no-cut negative control must actually stabilise Frame 2").c_str());

					const float noCutDiff = maxRgbAbsDiff(noCutFrame2, freshFrame2);
					Assert::IsTrue(noCutDiff > 0.01f,
						(modeLabel + L"negative control (cut=false): Frame 2 must hold Frame-1 residual (> 0.01), got " +
							std::to_wstring(noCutDiff)).c_str());
					noCutMod.Release();
				}
			}

			for (pl_tex* t : { &frame1Tex, &frame2Tex, &fieldTex, &trustTex })
				pl_tex_destroy(gpu, t);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#2 (GEM-070 / ADS-STATE, WARP GPU):
		// Residual stabilisation must invalidate the previous residual when motion.sourceCrop shifts
		// even if the output dimensions (dw x dh) stay unchanged — both on a pure crop-origin shift
		// ({0, 0, 128, 96} -> {16, 16, 144, 112}) and on a crop-extent shift ({0, 0, 160, 128} -> {0, 0, 128, 96}),
		// including on a subsequent repeat of Frame 2.
		// Mandatory in-test negative control: exact same Frame 1 -> Frame 2 with UNCHANGED sourceCrop
		// MUST blend Frame 1's residual (> 0.01 difference from fresh Frame 2).
		TEST_METHOD(ResidualHoldStopsOnASourceCropShift)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt fieldFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt trustFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && fieldFmt != nullptr && trustFmt != nullptr,
				L"WARP must provide float32 source, target, motion field, and frame-trust formats");

			static constexpr int sw = 160, sh = 128, dw = 64, dh = 48;
			static constexpr int bw = sw / 16, bh = sh / 16;

			const pl_rect2df cropBase  = { 0.0f,  0.0f, 128.0f,  96.0f };
			const pl_rect2df cropShift = { 16.0f, 16.0f, 144.0f, 112.0f };
			const pl_rect2df cropZoom  = { 0.0f,  0.0f, 160.0f, 128.0f };

			auto makeSourceTex = [&](const std::function<float(int, int)>& pattern) -> pl_tex
			{
				std::vector<float> rgba(static_cast<size_t>(sw) * sh * 4, 0.0f);
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						const float v = pattern(x, y);
						float* p = &rgba[(static_cast<size_t>(y) * sw + x) * 4];
						p[0] = p[1] = p[2] = v;
						p[3] = 1.0f;
					}
				}
				pl_tex_params tp{};
				tp.w = sw;
				tp.h = sh;
				tp.format = srcFmt;
				tp.sampleable = true;
				tp.initial_data = rgba.data();
				pl_tex tex = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(tex, L"RGBA32F source texture creation failed");
				return tex;
			};

			pl_tex frame1Tex = makeSourceTex([](int x, int y) -> float
			{
				if (x < sw / 2)
					return (((x / 2) + (y / 2)) % 2) != 0 ? 0.90f : 0.10f;
				return ((x / 2) % 2) != 0 ? 0.90f : 0.10f;
			});

			pl_tex frame2Tex = makeSourceTex([](int, int y) -> float
			{
				if (y < sh / 2)
					return 0.50f;
				return ((y / 2) % 2) != 0 ? 0.72f : 0.28f;
			});

			std::vector<float> fieldData(static_cast<size_t>(bw) * bh * 4, 0.0f);
			for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
			{
				fieldData[i * 4 + 0] = 0.0f;
				fieldData[i * 4 + 1] = 0.0f;
				fieldData[i * 4 + 2] = 1.0f;
				fieldData[i * 4 + 3] = 0.0f;
			}
			pl_tex_params fp{};
			fp.w = bw;
			fp.h = bh;
			fp.format = fieldFmt;
			fp.sampleable = true;
			fp.initial_data = fieldData.data();
			pl_tex fieldTex = pl_tex_create(gpu, &fp);
			Assert::IsNotNull(fieldTex, L"motion field texture creation failed");

			const float trustVal = 1.0f;
			pl_tex_params trp{};
			trp.w = 1;
			trp.h = 1;
			trp.format = trustFmt;
			trp.sampleable = true;
			trp.initial_data = &trustVal;
			pl_tex trustTex = pl_tex_create(gpu, &trp);
			Assert::IsNotNull(trustTex, L"frame trust texture creation failed");

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			auto renderCrop = [&](AdaptiveDownscale::Module& module, const AdaptiveDownscale::Settings& st,
				uint64_t seq, pl_tex src, const pl_rect2df& crop, bool withMotion) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.crop = crop;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;

				module.SetFrame(st, seq, 1.0f / 24.0f);
				if (withMotion)
				{
					AdaptiveDownscale::Motion m;
					m.field = fieldTex;
					m.frameTrust = trustTex;
					m.minTrustedShare = 0.25f;
					m.sourceCrop = crop;
					module.SetMotion(m);
				}
				else
				{
					module.ClearMotion();
				}

				const pl_hook* hooks[3] = { module.Measure(), ssim, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				std::vector<float> px(static_cast<size_t>(dw) * dh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F target download failed");
				pl_tex_destroy(gpu, &target);
				return px;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"output frame sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"output pixels must remain finite");
					if (i % 4 == 3)
						continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst)
						worst = d;
				}
				return worst;
			};

			auto makeSettings = [](bool stabilise, bool grainGuard)
			{
				AdaptiveDownscale::Settings st;
				st.wBase = 1.0f;
				st.edgeLo = 10000.0f;
				st.edgeHi = 20000.0f;
				st.grainLo = 10000.0f;
				st.grainHi = 20000.0f;
				st.shadowBias = 0.0f;
				st.grainSmoothMs = 0.0f;
				st.grainMap = false;
				st.ssimResidualStabilise = stabilise;
				st.residualGrainGuard = grainGuard;
				st.residualCodingNoiseCodes = 0.0f;
				st.float32Picture = true;
				return st;
			};

			for (bool grainGuard : { false, true })
			{
				const std::wstring modeLabel = grainGuard ? L"[residualGrainGuard=true] " : L"[residualGrainGuard=false] ";
				const AdaptiveDownscale::Settings stStab = makeSettings(true, grainGuard);
				const AdaptiveDownscale::Settings stNoStab = makeSettings(false, grainGuard);

				std::vector<float> freshAtBase;
				std::vector<float> freshAtShift;
				{
					AdaptiveDownscale::Module refMod;
					refMod.SetCutSource(noCutSource);
					freshAtBase = renderCrop(refMod, stNoStab, 1, frame2Tex, cropBase, false);
					freshAtShift = renderCrop(refMod, stNoStab, 2, frame2Tex, cropShift, false);
					Assert::IsTrue(refMod.Active() && refMod.Failures() == 0,
						(modeLabel + L"fresh Frame 2 crop references must render cleanly").c_str());
					refMod.Release();
				}

				// 1. Pure origin shift (same crop WxH 128x96 and same output WxH 64x48):
				//    {0, 0, 128, 96} -> {16, 16, 144, 112} with cut = false.
				{
					AdaptiveDownscale::Module shiftMod;
					shiftMod.SetCutSource(noCutSource);
					renderCrop(shiftMod, stStab, 1, frame1Tex, cropBase, true);
					const std::vector<float> shiftedFrame2 = renderCrop(shiftMod, stStab, 2, frame2Tex, cropShift, true);
					const std::vector<float> shiftedFrame2Repeat = renderCrop(shiftMod, stStab, 2, frame2Tex, cropShift, true);
					Assert::IsTrue(shiftMod.Active() && shiftMod.Failures() == 0,
						(modeLabel + L"crop-shift module must stay active with zero failures").c_str());

					const float shiftDiff = maxRgbAbsDiff(shiftedFrame2, freshAtShift);
					const float repeatDiff = maxRgbAbsDiff(shiftedFrame2Repeat, freshAtShift);
					Assert::IsTrue(shiftDiff <= 1e-4f,
						(modeLabel + L"sourceCrop origin shift must stop Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(shiftDiff) + L")").c_str());
					Assert::IsTrue(repeatDiff <= 1e-4f,
						(modeLabel + L"repeat after sourceCrop shift must have zero Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(repeatDiff) + L")").c_str());
					shiftMod.Release();
				}

				// 2. Crop extent/zoom shift (same origin (0, 0) and same output WxH 64x48):
				//    {0, 0, 160, 128} -> {0, 0, 128, 96} with cut = false.
				{
					AdaptiveDownscale::Module zoomMod;
					zoomMod.SetCutSource(noCutSource);
					renderCrop(zoomMod, stStab, 1, frame1Tex, cropZoom, true);
					const std::vector<float> zoomedFrame2 = renderCrop(zoomMod, stStab, 2, frame2Tex, cropBase, true);
					Assert::IsTrue(zoomMod.Active() && zoomMod.Failures() == 0,
						(modeLabel + L"crop-extent shift module must stay active with zero failures").c_str());

					const float zoomDiff = maxRgbAbsDiff(zoomedFrame2, freshAtBase);
					Assert::IsTrue(zoomDiff <= 1e-4f,
						(modeLabel + L"sourceCrop extent shift must stop Frame-1 residual bleed (expected <= 1e-4, got " +
							std::to_wstring(zoomDiff) + L")").c_str());
					zoomMod.Release();
				}

				// 3. MANDATORY IN-TEST NEGATIVE CONTROL (exact same Frame 1 -> Frame 2, cut = false,
				//    UNCHANGED sourceCrop = cropBase): Frame 2 MUST blend in Frame 1's residual (> 0.01).
				{
					AdaptiveDownscale::Module steadyMod;
					steadyMod.SetCutSource(noCutSource);
					renderCrop(steadyMod, stStab, 1, frame1Tex, cropBase, true);
					const std::vector<float> steadyFrame2 = renderCrop(steadyMod, stStab, 2, frame2Tex, cropBase, true);
					Assert::IsTrue(steadyMod.Active() && steadyMod.Failures() == 0,
						(modeLabel + L"unchanged-crop negative control must stay active with zero failures").c_str());
					Assert::IsTrue(steadyMod.StabilisedFrames() >= 1ull,
						(modeLabel + L"unchanged-crop negative control must stabilise Frame 2").c_str());

					const float steadyDiff = maxRgbAbsDiff(steadyFrame2, freshAtBase);
					Assert::IsTrue(steadyDiff > 0.01f,
						(modeLabel + L"negative control (unchanged sourceCrop): Frame 2 must hold Frame-1 residual (> 0.01), got " +
							std::to_wstring(steadyDiff)).c_str());
					steadyMod.Release();
				}
			}

			for (pl_tex* t : { &frame1Tex, &frame2Tex, &fieldTex, &trustTex })
				pl_tex_destroy(gpu, t);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#2 absorbed W1B MUST-6 test (WARP GPU):
		// When 32-bit float picture precision is requested (float32Picture = true) and the GPU has no renderable
		// 32-bit float format, ScalerPass falls back to 16-bit float, AdaptiveDownscale::Module::HalfFloatPictureFallback()
		// turns true, and AdaptiveDownscaleStage::Stage surfaces the fallback visibly in BOTH StatusLines() and
		// Fallback() ("output scaling" catalog entry).
		// Mandatory negative controls:
		//  (a) Switching the feature off (requested = false, either adaptiveDownscale = false or slotRuns = false)
		//      while module.HalfFloatPictureFallback() is still latched true suppresses BOTH StatusLines() and Fallback()
		//      (no stale fallback).
		//  (b) Explicitly requesting 16-bit float (float32Picture = false) does not raise the fallback.
		//  (c) Restoring renderable 32-bit float on the GPU clears HalfFloatPictureFallback(), StatusLines(), and Fallback().
		TEST_METHOD(HalfFloatPictureFallbackIsVisible)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			fixture.CaptureLog(&log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			const pl_hook* ssim = pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size());
			Assert::IsNotNull(ssim, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 8, 8,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr, L"WARP must provide rgba16 source and rgba8 target formats");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			std::vector<uint16_t> srcPixels(static_cast<size_t>(sw) * sh * 4);
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					const uint16_t v = static_cast<uint16_t>(((x + y) % 2) != 0 ? 48000u : 16000u);
					uint16_t* p = &srcPixels[(static_cast<size_t>(y) * sw + x) * 4];
					p[0] = p[1] = p[2] = v;
					p[3] = 65535u;
				}
			}
			pl_tex_params stp{};
			stp.w = sw;
			stp.h = sh;
			stp.format = srcFmt;
			stp.sampleable = true;
			stp.initial_data = srcPixels.data();
			pl_tex srcTex = pl_tex_create(gpu, &stp);
			Assert::IsNotNull(srcTex, L"source texture creation failed");

			struct ScopedDisableFloat32Renderable
			{
				struct Saved { struct pl_fmt_t* fmt; enum pl_fmt_caps caps; };
				std::vector<Saved> saved;
				explicit ScopedDisableFloat32Renderable(pl_gpu g)
				{
					for (int i = 0; i < g->num_formats; ++i)
					{
						struct pl_fmt_t* f = const_cast<struct pl_fmt_t*>(g->formats[i]);
						if (f && f->type == PL_FMT_FLOAT && f->component_depth[0] >= 32 &&
							(f->caps & PL_FMT_CAP_RENDERABLE) != 0)
						{
							Saved entry;
							entry.fmt = f;
							entry.caps = f->caps;
							saved.push_back(entry);
							f->caps = static_cast<enum pl_fmt_caps>(
								static_cast<unsigned>(f->caps) &
								~static_cast<unsigned>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_BLENDABLE));
						}
					}
				}
				void Restore()
				{
					for (size_t i = 0; i < saved.size(); ++i)
						saved[i].fmt->caps = saved[i].caps;
					saved.clear();
				}
				~ScopedDisableFloat32Renderable() { Restore(); }
			};

			const std::string kExpectedStatusLine =
				"Adaptive scaler: 16-bit float picture (32-bit float not available)";
			const std::string kExpectedFallbackReason =
				"adaptive scaler: 16-bit float picture (32-bit float not available)";

			auto hasStatusLine = [](const std::vector<std::string>& lines, const std::string& target) -> bool
			{
				return std::find(lines.begin(), lines.end(), target) != lines.end();
			};

			AdaptiveDownscale::Module module;
			// Known cut source so CutAvailable() is true and never shadows HalfFloatPictureFallback() in Fallback().
			module.SetCutSource([](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			});

			AdaptiveDownscaleStage::Stage stage{ module };
			AdaptiveDownscaleStage::Host host;
			host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
			host.bitrate = [](double& q) { q = 1.0; return false; };
			host.nlsMapping = []() { return NlsMaskMapping::Mapping(); };
			host.ssimInChain = []() { return true; };
			host.motion = []() { return AdaptiveDownscaleStage::MotionSource(); };
			host.publishStatus = []() {};

			double weight = 1.0;
			bool grainWanted = false, noSsim = false;
			uint64_t sequence = 0;

			auto runStageFrame = [&](const AdaptiveDownscale::Settings& st, bool adaptiveDownscale,
				bool slotRuns, int fboFloat32, bool render)
			{
				AdaptiveDownscaleStage::Frame f;
				f.adaptiveDownscale = adaptiveDownscale;
				f.adaptiveUpscale = false;
				f.slotRuns = slotRuns;
				f.adaptive = &st;
				f.lowBitrateOff = true;
				f.fboFloat32 = fboFloat32;
				f.sourceSequence = ++sequence;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh) };

				std::vector<const pl_hook*> hooks = { ssim };
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				if (!render)
				{
					Assert::AreEqual(size_t(1), hooks.size(), L"feature off: stage must not register adaptive hooks");
					return;
				}
				Assert::IsTrue(hooks.size() == 3 && hooks.front() == module.Measure() && hooks.back() == module.Blend(),
					L"downscale stage must register measure, SSIM, blend");

				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target, L"target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(srcTex);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				pl_tex_destroy(gpu, &target);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");
			};

			AdaptiveDownscale::Settings stF32;
			stF32.float32Picture = true;
			stF32.grainMap = false;
			stF32.ssimResidualStabilise = false;

			{
				ScopedDisableFloat32Renderable hideFloat32(gpu);
				Assert::IsFalse(hideFloat32.saved.empty(),
					L"precondition: WARP must initially expose at least one renderable 32-bit float format");

				// 1. Positive case: 32-bit float requested, GPU lacks renderable 32-bit float -> ScalerPass
				//    falls back to 16-bit float -> module.HalfFloatPictureFallback() is true -> visible in
				//    both StatusLines() and Fallback() ("output scaling").
				runStageFrame(stF32, true, true, 1, true);
				Assert::IsTrue(module.Active() && module.Failures() == 0,
					L"half-float fallback frame must succeed with zero failures");
				Assert::IsTrue(module.CutAvailable(), L"precondition: cut verdict must remain available");
				Assert::IsTrue(module.HalfFloatPictureFallback(),
					L"module.HalfFloatPictureFallback() must be true when 32-bit float is unavailable");
				Assert::IsTrue(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"StatusLines() must emit the 16-bit float picture fallback line when requested");
				{
					const FallbackCatalog::Decision fb = stage.Fallback();
					Assert::IsTrue(fb.engaged, L"Fallback() must be engaged when HalfFloatPictureFallback() is true and requested");
					Assert::AreEqual(std::string("output scaling"), fb.id, L"Fallback() catalog id must be 'output scaling'");
					Assert::AreEqual(kExpectedFallbackReason, fb.reason, L"Fallback() reason must match half-float picture fallback");
				}

				// 2. MANDATORY NEGATIVE CONTROL (requested = false, feature off while module flag is still true):
				//    neither StatusLines() nor Fallback() may emit a stale half-float fallback.
				runStageFrame(stF32, false, true, 1, false);
				Assert::IsTrue(module.HalfFloatPictureFallback(),
					L"precondition: module flag remains latched until next measure (proving stage gating is necessary)");
				Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"negative control (adaptiveDownscale=false): StatusLines() must not emit stale half-float fallback");
				Assert::IsFalse(stage.Fallback().engaged,
					L"negative control (adaptiveDownscale=false): Fallback() must not emit stale half-float fallback");

				// Also check slotRuns = false (requested = false while adaptiveDownscale = true).
				runStageFrame(stF32, true, false, 1, false);
				Assert::IsTrue(module.HalfFloatPictureFallback(),
					L"precondition: module flag still latched when slot is off");
				Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"negative control (slotRuns=false): StatusLines() must not emit stale half-float fallback");
				Assert::IsFalse(stage.Fallback().engaged,
					L"negative control (slotRuns=false): Fallback() must not emit stale half-float fallback");

				// 3. Negative control: explicitly requesting 16-bit float (float32Picture = false) must NOT trigger fallback.
				AdaptiveDownscale::Settings stF16 = stF32;
				stF16.float32Picture = false;
				runStageFrame(stF16, true, true, 0, true);
				Assert::IsTrue(module.Active() && module.Failures() == 0, L"explicit 16-bit float frame must succeed");
				Assert::IsFalse(module.HalfFloatPictureFallback(),
					L"negative control (float32Picture=false): explicit 16-bit float request is not a fallback");
				Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
					L"negative control (float32Picture=false): StatusLines() must not emit fallback line");
				Assert::IsFalse(stage.Fallback().engaged,
					L"negative control (float32Picture=false): Fallback() must not be engaged");

				// Restore renderable 32-bit float formats on WARP.
				hideFloat32.Restore();
			}

			// 4. Negative control: with renderable 32-bit float available on WARP, requesting 32-bit float succeeds
			//    without fallback and clears StatusLines() and Fallback().
			runStageFrame(stF32, true, true, 1, true);
			Assert::IsTrue(module.Active() && module.Failures() == 0, L"32-bit float frame on WARP must succeed");
			Assert::IsFalse(module.HalfFloatPictureFallback(),
				L"negative control (32-bit float available): HalfFloatPictureFallback() must be false");
			Assert::IsFalse(hasStatusLine(stage.StatusLines(), kExpectedStatusLine),
				L"negative control (32-bit float available): StatusLines() must not emit fallback line");
			Assert::IsFalse(stage.Fallback().engaged,
				L"negative control (32-bit float available): Fallback() must not be engaged");

			module.Release();
			pl_tex_destroy(gpu, &srcTex);
			pl_mpv_user_shader_destroy(&ssim);
			fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#3 (GEM-071 / ADS-STATE, WARP GPU):
		// UpdateMap must restart the per-block temporal grain map on a source sequence gap
		// (sequence != mapSequence + 1), snapping cleanly to the new frame's target instead of
		// smoothing against the stale pre-gap map when cut = false.
		// Mandatory in-test negative control: exact same Frame 1 -> Frame 2 inputs with CONSECUTIVE
		// sequence numbers (seq = 1 -> seq = 2, cut = false) MUST smooth slowly (grainMapSmoothMs = 500 ms)
		// from ~0.40 toward 1.0 (landing well below 0.70), proving smoothing is genuinely active and that
		// the snap on seq = 1 -> seq = 5 (and backward gap seq = 5 -> seq = 3) is caused strictly by the gap.
		TEST_METHOD(GrainMapRestartsAfterASequenceGap)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			ScopedLogCapture logGuard(fixture, log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			ScopedHook ssim(pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size()));
			Assert::IsNotNull(ssim.hook, L"SSimDownscaler.glsl must parse");

			pl_fmt srcFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt dstFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt gridFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt trustFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(srcFmt != nullptr && dstFmt != nullptr && gridFmt != nullptr && trustFmt != nullptr,
				L"WARP must provide float32 source, target, block-grid, and frame-trust formats");

			static constexpr int sw = 128, sh = 96, dw = 64, dh = 48;
			static constexpr int bw = sw / 16, bh = sh / 16;

			// Uniform clean mid-grey source texture so scalar grain state stays healthy (state.a == 1.0 after frame 1).
			std::vector<float> srcData(static_cast<size_t>(sw) * sh * 4, 0.5f);
			for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i)
				srcData[i * 4 + 3] = 1.0f;
			pl_tex_params stp{};
			stp.w = sw;
			stp.h = sh;
			stp.format = srcFmt;
			stp.sampleable = true;
			stp.initial_data = srcData.data();
			ScopedTex srcTex(gpu, pl_tex_create(gpu, &stp));
			Assert::IsNotNull(srcTex.tex, L"RGBA32F source texture creation failed");

			auto makeUniformGrid = [&](float r, float g, float b, float a) -> pl_tex
			{
				std::vector<float> data(static_cast<size_t>(bw) * bh * 4, 0.0f);
				for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
				{
					data[i * 4 + 0] = r;
					data[i * 4 + 1] = g;
					data[i * 4 + 2] = b;
					data[i * 4 + 3] = a;
				}
				pl_tex_params gp{};
				gp.w = bw;
				gp.h = bh;
				gp.format = gridFmt;
				gp.sampleable = true;
				gp.initial_data = data.data();
				pl_tex tex = pl_tex_create(gpu, &gp);
				Assert::IsNotNull(tex, L"RGBA32F block-grid texture creation failed");
				return tex;
			};

			// Zero-motion trusted field (f.z = 1.0 > 0.5), frameTrust = 1.0 >= minTrustedShare (0.25).
			ScopedTex fieldTex(gpu, makeUniformGrid(0.0f, 0.0f, 1.0f, 0.0f));
			// Heavy-grain error (ge.r = 20.0 -> energy ~= 126.6 >> grainHi = 8.0 -> target = grainFloor = 0.40).
			ScopedTex heavyErrorTex(gpu, makeUniformGrid(20.0f, 127.5f, 0.0f, 1.0f));
			// Clean error (ge.r = 0.0 -> energy = 0.0 < grainLo = 2.0 -> target = 1.0).
			ScopedTex cleanErrorTex(gpu, makeUniformGrid(0.0f, 127.5f, 0.0f, 1.0f));

			const float trustVal = 1.0f;
			pl_tex_params trp{};
			trp.w = 1;
			trp.h = 1;
			trp.format = trustFmt;
			trp.sampleable = true;
			trp.initial_data = &trustVal;
			ScopedTex trustTex(gpu, pl_tex_create(gpu, &trp));
			Assert::IsNotNull(trustTex.tex, L"frame trust texture creation failed");

			AdaptiveDownscale::Settings st;
			st.grainMap = true;
			st.grainMapSmoothMs = 500.0f;   // slow smoothing (~8% per 24 fps frame) so blended vs snapped is unmistakable
			st.grainLo = 2.0f;
			st.grainHi = 8.0f;
			st.grainFloor = 0.4f;
			st.ssimResidualStabilise = false;

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			auto renderFrame = [&](AdaptiveDownscale::Module& module, uint64_t seq, pl_tex errorTex)
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = dstFmt;
				tp.renderable = true;
				tp.host_readable = true;
				ScopedTex target(gpu, pl_tex_create(gpu, &tp));
				Assert::IsNotNull(target.tex, L"RGBA32F target texture creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(srcTex.tex);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target.tex);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;

				module.SetFrame(st, seq, 1.0f / 24.0f);
				AdaptiveDownscale::GrainField gf;
				gf.error = errorTex;
				gf.field = fieldTex.tex;
				gf.frameTrust = trustTex.tex;
				gf.minTrustedShare = 0.25f;
				gf.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetGrainField(gf);

				const pl_hook* hooks[3] = { module.Measure(), ssim.hook, module.Blend() };
				params.hooks = hooks;
				params.num_hooks = 3;

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				target.Reset();
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");
				Assert::IsTrue(module.Active() && module.Failures() == 0,
					L"AdaptiveDownscale module must be active with zero failures");
				Assert::IsTrue(module.CutAvailable(), L"precondition: cut verdict must stay Known (cut = false)");
				Assert::IsTrue(module.LastMapState() == AdaptiveDownscale::MapState::Used,
					L"grain map must be Used when grainMap = true and a valid GrainField is supplied");
			};

			auto checkGrainMapRange = [&](const AdaptiveDownscale::Module& module, float minAllowed, float maxAllowed,
				const wchar_t* label)
			{
				pl_tex map = module.GrainMap();
				Assert::IsNotNull(map, (std::wstring(label) + L": GrainMap() must be non-null").c_str());
				Assert::AreEqual(bw, map->params.w, (std::wstring(label) + L": GrainMap width mismatch").c_str());
				Assert::AreEqual(bh, map->params.h, (std::wstring(label) + L": GrainMap height mismatch").c_str());

				std::vector<float> texels(static_cast<size_t>(bw) * bh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = map;
				dl.ptr = texels.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), (std::wstring(label) + L": GrainMap download failed").c_str());

				float minR = (std::numeric_limits<float>::max)();
				float maxR = -(std::numeric_limits<float>::max)();
				for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
				{
					const float r = texels[i * 4 + 0];
					const float g = texels[i * 4 + 1];
					Assert::IsTrue(std::isfinite(r) && std::isfinite(g),
						(std::wstring(label) + L": non-finite grainMap texel at block " + std::to_wstring(i)).c_str());
					Assert::IsTrue(g > 0.5f,
						(std::wstring(label) + L": expected trusted block (g > 0.5) at block " + std::to_wstring(i) +
							L", got " + std::to_wstring(g)).c_str());
					if (r < minR) minR = r;
					if (r > maxR) maxR = r;
				}
				const std::wstring msg = std::wstring(label) + L": grainMap.r range [" + std::to_wstring(minR) +
					L", " + std::to_wstring(maxR) + L"], expected within [" + std::to_wstring(minAllowed) +
					L", " + std::to_wstring(maxAllowed) + L"]";
				Assert::IsTrue(minR >= minAllowed && maxR <= maxAllowed, msg.c_str());
			};

			// 1. Sequence-gap case (seq = 1 -> seq = 5, plus backward jump seq = 5 -> seq = 3; cut = false throughout):
			//    Frame 1 initializes grainMap.r at grainFloor (~0.40); after the sequence gap to seq = 5 with a clean
			//    grainField, UpdateMap must treat it as first_map = 1 and snap cleanly to 1.0 (>= 0.99), NOT smooth from 0.40.
			{
				ScopedModule gapMod;
				gapMod->hostReadableMap = true;
				gapMod->SetCutSource(noCutSource);

				renderFrame(*gapMod, 1, heavyErrorTex.tex);
				checkGrainMapRange(*gapMod, 0.35f, 0.45f, L"sequence-gap Frame seq=1 (heavy grain)");

				renderFrame(*gapMod, 5, cleanErrorTex.tex);
				checkGrainMapRange(*gapMod, 0.99f, 1.001f, L"sequence-gap Frame seq=5 (clean after 1->5 gap must snap to 1.0)");

				// Backward sequence discontinuity (5 -> 3, 3 != 5 + 1, cut = false): must also snap cleanly back to floor.
				renderFrame(*gapMod, 3, heavyErrorTex.tex);
				checkGrainMapRange(*gapMod, 0.35f, 0.45f, L"sequence-gap Frame seq=3 (heavy grain after 5->3 backward gap must snap to floor)");
			}

			// 2. MANDATORY IN-TEST NEGATIVE CONTROL (exact same Frame 1 -> Frame 2 inputs with CONSECUTIVE seq = 1 -> seq = 2,
			//    cut = false): with grainMapSmoothMs = 500 ms (~8% alpha at 24 fps), Frame 2 MUST smooth slowly from 0.40
			//    toward 1.0 (~0.448, strictly > 0.41 and < 0.70), NOT snap — proving smoothing is active and the snap in
			//    Step 1 is caused strictly by the sequence gap.
			{
				ScopedModule consecMod;
				consecMod->hostReadableMap = true;
				consecMod->SetCutSource(noCutSource);

				renderFrame(*consecMod, 1, heavyErrorTex.tex);
				checkGrainMapRange(*consecMod, 0.35f, 0.45f, L"negative control Frame seq=1 (heavy grain)");

				renderFrame(*consecMod, 2, cleanErrorTex.tex);
				checkGrainMapRange(*consecMod, st.grainFloor + 0.01f, 0.70f,
					L"negative control Frame seq=2 (consecutive clean must smooth slowly from 0.40, not snap)");
			}

			trustTex.Reset();
			cleanErrorTex.Reset();
			heavyErrorTex.Reset();
			fieldTex.Reset();
			srcTex.Reset();
			ssim.Reset();
			logGuard.Release();
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#10 (GEM-077 / ADS-PERF, WARP GPU):
		// Verify AdaptiveDownscale::Module's 18x18 shared-memory tiled MEASURE_BODY is bit-identical
		// (<= 1e-6f) to an independent un-tiled direct-texelFetch reference AND matches an independent
		// CPU reference across:
		//  (A) Full 128x96 (8x6 16x16 workgroups) exercising every internal workgroup seam
		//      (x = 15, 16, 31, 32, ..., 111, 112; y = 15, 16, ..., 79, 80) and all 4 texture borders
		//      (x = 0, 127; y = 0, 95);
		//  (B) Odd non-zero sub-crop {13, 11, 110, 84} (97x73 -> 48x36) exercising non-zero origin,
		//      partial edge workgroups, and cross-crop halo reads into the full em texture.
		// Mandatory in-test negative control (exact same 128x96 input): omitting the 1-pixel halo
		// (clamping tile neighbor reads to local [0, 15] instead of [0, 17]) introduces workgroup-seam
		// Laplacian errors (> 1e-3) and shifts grainScale, meanW, and blended output pixels (> 1e-3).
		TEST_METHOD(MeasureTileIsBitIdentical)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			ScopedLogCapture logGuard(fixture, log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			ScopedHook ssim(pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size()));
			Assert::IsNotNull(ssim.hook, L"SSimDownscaler.glsl must parse");

			pl_fmt rgba32fSample = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt rgba32fTarget = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rgba32fStoreRead = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rg32fStore = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE));
			pl_fmt r16fEdge = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 16, 16,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_LINEAR));
			Assert::IsTrue(rgba32fSample && rgba32fTarget && rgba32fStoreRead && rg32fStore && r16fEdge,
				L"WARP must provide float32/float16 formats for measure-tile parity test");

			static constexpr int sw = 128, sh = 96;
			// Deterministic 128x96 image exercising hard edges (left x < 32 and top/bottom borders y = 0, 95),
			// flat/textured pixels with crisp 5-code steps across EVERY 16x16 workgroup boundary (x >= 32),
			// fine per-pixel texture, and a vertical luma ramp exercising shadowBias = 0.5 and border clamping.
			std::vector<float> srcRgba(static_cast<size_t>(sw) * sh * 4, 0.0f);
			uint32_t rng = 0x51A7F00Du;
			auto nextNoise = [&rng]() -> float
			{
				rng = rng * 1664525u + 1013904223u;
				return (static_cast<float>(rng >> 8) / 16777216.0f - 0.5f) * 3.0f;   // [-1.5, +1.5] codes
			};
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					float code = 128.0f;
					if (x < 32)
					{
						// Hard vertical stripes crossing x = 0, 15/16, 31/32 plus sharp top/bottom border edges.
						code = ((x / 12) % 2) != 0 ? 220.0f : 30.0f;
						if (y == 0 || y == sh - 1)
							code = (code > 128.0f) ? 40.0f : 220.0f;
					}
					else
					{
						// Alternating 5-code 16x16 tile checkerboard: Sobel grad ~5 < edgeLo (8), so flat_px == 1
						// right on every workgroup seam (x = 31/32, 47/48, ..., 111/112; y = 15/16, ..., 79/80).
						const float tileStep = (((x / 16) + (y / 16)) & 1) != 0 ? 129.0f : 124.0f;
						const float vertRamp = -45.0f + 90.0f * (static_cast<float>(y) / static_cast<float>(sh - 1));
						code = tileStep + vertRamp + nextNoise();
					}
					code = (std::max)(0.0f, (std::min)(255.0f, code));
					const float norm = code / 255.0f;
					float* px = &srcRgba[(static_cast<size_t>(y) * sw + x) * 4];
					px[0] = px[1] = px[2] = norm;
					px[3] = 1.0f;
				}
			}

			pl_tex_params stp{};
			stp.w = sw;
			stp.h = sh;
			stp.format = rgba32fSample;
			stp.sampleable = true;
			stp.initial_data = srcRgba.data();
			ScopedTex srcTex(gpu, pl_tex_create(gpu, &stp));
			Assert::IsNotNull(srcTex.tex, L"RGBA32F source texture creation failed");

			AdaptiveDownscale::Settings st;
			st.wBase = 1.0f;
			st.edgeLo = 8.0f;
			st.edgeHi = 40.0f;
			st.grainLo = 2.0f;
			st.grainHi = 8.0f;
			st.grainFloor = 0.4f;
			st.grainSmoothMs = 0.0f;
			st.shadowBias = 0.5f;
			st.grainMap = false;
			st.ssimResidualStabilise = false;
			st.float32Picture = true;

			auto smoothstepCpu = [](float lo, float hi, float x) -> float
			{
				const float t = (std::max)(0.0f, (std::min)(1.0f, (x - lo) / (hi - lo)));
				return t * t * (3.0f - 2.0f * t);
			};

			// Independent pure-C++ CPU reference of the measure pass (exact Sobel + un-tiled 4-neighbor Laplacian)
			// plus a CPU workgroup-seam audit comparing true halo vs no-halo [0, 15] local clamping.
			struct CpuMeasureRef
			{
				float grainScale = 1.0f;
				float meanW = 0.0f;
				float maxSeamLapErr = 0.0f;
			};
			auto computeCpuRef = [&](int ox, int oy, int rw, int rh) -> CpuMeasureRef
			{
				pl_color_space cs{};
				cs.primaries = PL_COLOR_PRIM_BT_709;
				cs.transfer = PL_COLOR_TRC_GAMMA22;
				const VpMeasure::MeasureDomain dom = VpMeasure::MeasureDomainFor(cs, PL_COLOR_SYSTEM_BT_709, PL_COLOR_PRIM_BT_709);
				std::vector<float> luma(static_cast<size_t>(sw) * sh, 0.0f);
				for (int i = 0; i < sw * sh; ++i)
				{
					const double rgb[3] = { srcRgba[i * 4 + 0], srcRgba[i * 4 + 1], srcRgba[i * 4 + 2] };
					luma[static_cast<size_t>(i)] = static_cast<float>(VpMeasure::MeasureLumaCode(dom, rgb));
				}
				const std::vector<float> sobel = RegionEvidence::BusyEdge::SobelMagnitude(luma.data(), sw, sh);
				auto atClamped = [&](int x, int y) -> float
				{
					const int cx = (std::max)(0, (std::min)(sw - 1, x));
					const int cy = (std::max)(0, (std::min)(sh - 1, y));
					return luma[static_cast<size_t>(cy) * sw + cx];
				};
				double sumLap = 0.0, sumFlat = 0.0, sumFree = 0.0;
				float maxSeamErr = 0.0f;
				for (int qy = 0; qy < rh; ++qy)
				{
					for (int qx = 0; qx < rw; ++qx)
					{
						const int px = (std::max)(0, (std::min)(sw - 1, ox + qx));
						const int py = (std::max)(0, (std::min)(sh - 1, oy + qy));
						const float e = luma[static_cast<size_t>(py) * sw + px];
						const float b = atClamped(ox + qx, oy + qy - 1);
						const float d = atClamped(ox + qx - 1, oy + qy);
						const float f = atClamped(ox + qx + 1, oy + qy);
						const float h = atClamped(ox + qx, oy + qy + 1);
						const float grad = 0.25f * sobel[static_cast<size_t>(py) * sw + px];
						const float ed = smoothstepCpu(st.edgeLo, st.edgeHi, grad);
						const float lap = std::fabs(4.0f * e - b - d - f - h);
						const float flatPx = grad < st.edgeLo ? 1.0f : 0.0f;
						const float eNorm = (std::max)(0.0f, (std::min)(1.0f, e / 255.0f));
						const float wgt = (std::max)(1.0f + (2.0f * (1.0f - eNorm) - 1.0f) * st.shadowBias, 0.0f);
						sumLap += static_cast<double>(lap * wgt * flatPx);
						sumFlat += static_cast<double>(flatPx);
						sumFree += static_cast<double>(1.0f - ed);

						// Seam audit on internal 16x16 workgroup borders (x = 15, 16, 31, 32, ...).
						const int lx = qx & 15;
						const int ly = qy & 15;
						if (flatPx > 0.5f && (lx == 0 || lx == 15) && qx > 0 && qx < rw - 1)
						{
							const int wgX = (qx / 16) * 16;
							const int wgY = (qy / 16) * 16;
							const float bNH = atClamped(ox + wgX + lx, oy + wgY + (std::max)(ly - 1, 0));
							const float dNH = atClamped(ox + wgX + (std::max)(lx - 1, 0), oy + wgY + ly);
							const float fNH = atClamped(ox + wgX + (std::min)(lx + 1, 15), oy + wgY + ly);
							const float hNH = atClamped(ox + wgX + lx, oy + wgY + (std::min)(ly + 1, 15));
							const float lapNH = std::fabs(4.0f * e - bNH - dNH - fNH - hNH);
							maxSeamErr = (std::max)(maxSeamErr, std::fabs(lap - lapNH));
						}
					}
				}
				const float energy = static_cast<float>(sumLap / (std::max)(sumFlat, 1.0));
				const float freeMean = static_cast<float>(sumFree / (std::max)(static_cast<double>(rw) * rh, 1.0));
				const float gs = st.grainFloor + (1.0f - st.grainFloor) * (1.0f - smoothstepCpu(st.grainLo, st.grainHi, energy));
				CpuMeasureRef out;
				out.grainScale = gs;
				out.meanW = st.wBase * freeMean * gs;
				out.maxSeamLapErr = maxSeamErr;
				return out;
			};

			// Independent GPU reference hook pair:
			//  - omitHalo == false: pure un-tiled direct texelFetch measure pass (zero shared tile).
			//  - omitHalo == true:  negative-control 16x16 shared tile omitting the 1-pixel halo (clamped to [0, 15]).
			struct ReferenceMeasureHook
			{
				pl_gpu gpu = nullptr;
				AdaptiveDownscale::Settings settings{};
				bool omitHalo = false;
				ScalerPass::Module faithfulScaler;
				pl_tex faithful = nullptr;
				pl_tex em = nullptr;
				pl_tex edge = nullptr;
				pl_tex partial = nullptr;
				pl_tex state = nullptr;
				pl_fmt rg32fFmt = nullptr;
				pl_fmt r16fFmt = nullptr;
				pl_fmt rgba32fFmt = nullptr;
				pl_rect2df srcRect{};
				pl_rect2d dstRect{};
				float edgeOrigin[2] = {};
				VpMeasure::MeasureDomain domain{};
				pl_hook measureHook{};
				pl_hook blendHook{};

				void Init(pl_gpu g, const AdaptiveDownscale::Settings& s, bool noHalo,
					pl_fmt rg32f, pl_fmt r16f, pl_fmt rgba32f, uint64_t sigBase)
				{
					gpu = g;
					settings = s;
					omitHalo = noHalo;
					rg32fFmt = rg32f;
					r16fFmt = r16f;
					rgba32fFmt = rgba32f;
					measureHook.stages = PL_HOOK_PRE_KERNEL;
					measureHook.input = PL_HOOK_SIG_TEX;
					measureHook.priv = this;
					measureHook.hook = &ReferenceMeasureHook::RunMeasure;
					measureHook.signature = sigBase;
					blendHook.stages = PL_HOOK_POST_KERNEL;
					blendHook.input = PL_HOOK_SIG_TEX;
					blendHook.priv = this;
					blendHook.hook = &ReferenceMeasureHook::RunBlend;
					blendHook.signature = sigBase + 1ull;
				}

				void Release()
				{
					if (gpu)
					{
						for (pl_tex* t : { &em, &edge, &partial, &state })
							if (*t) pl_tex_destroy(gpu, t);
						faithfulScaler.Release(gpu);
					}
					faithful = em = edge = partial = state = nullptr;
				}
				~ReferenceMeasureHook() { Release(); }

				static pl_shader_desc SampledDesc(const char* name, pl_tex tex, bool linear)
				{
					pl_shader_desc d{};
					d.desc.name = name;
					d.desc.type = PL_DESC_SAMPLED_TEX;
					d.binding.object = tex;
					d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
					d.binding.sample_mode = linear ? PL_TEX_SAMPLE_LINEAR : PL_TEX_SAMPLE_NEAREST;
					return d;
				}
				static pl_shader_desc StorageDesc(const char* name, pl_tex tex)
				{
					pl_shader_desc d{};
					d.desc.name = name;
					d.desc.type = PL_DESC_STORAGE_IMG;
					d.desc.access = PL_DESC_ACCESS_WRITEONLY;
					d.binding.object = tex;
					return d;
				}
				static bool RunCompute(pl_dispatch dp, const char* desc, const char* hdr, const char* body,
					const pl_shader_desc* descs, int nDescs, const pl_shader_var* vars, int nVars, int gx, int gy)
				{
					pl_custom_shader custom{};
					custom.description = desc;
					custom.header = hdr;
					custom.body = body;
					custom.input = PL_SHADER_SIG_NONE;
					custom.output = PL_SHADER_SIG_NONE;
					custom.descriptors = descs;
					custom.num_descriptors = nDescs;
					custom.variables = vars;
					custom.num_variables = nVars;
					custom.compute = true;
					custom.compute_group_size[0] = 16;
					custom.compute_group_size[1] = 16;
					pl_shader sh = pl_dispatch_begin(dp);
					if (!pl_shader_custom(sh, &custom))
					{
						pl_dispatch_abort(dp, &sh);
						return false;
					}
					pl_dispatch_compute_params cp{};
					cp.shader = &sh;
					cp.dispatch_size[0] = gx;
					cp.dispatch_size[1] = gy;
					cp.dispatch_size[2] = 1;
					return pl_dispatch_compute(dp, &cp);
				}

				static pl_hook_res RunMeasure(void* priv, const pl_hook_params* p)
				{
					ReferenceMeasureHook* self = static_cast<ReferenceMeasureHook*>(priv);
					pl_hook_res unchanged{};
					unchanged.output = PL_HOOK_SIG_NONE;
					const float sx0 = std::fmin(p->src_rect.x0, p->src_rect.x1);
					const float sx1 = std::fmax(p->src_rect.x0, p->src_rect.x1);
					const float sy0 = std::fmin(p->src_rect.y0, p->src_rect.y1);
					const float sy1 = std::fmax(p->src_rect.y0, p->src_rect.y1);
					const int dw = std::abs(p->dst_rect.x1 - p->dst_rect.x0);
					const int dh = std::abs(p->dst_rect.y1 - p->dst_rect.y0);
					const int ox = static_cast<int>(std::floor(sx0));
					const int oy = static_cast<int>(std::floor(sy0));
					const int w = (std::max)(1, static_cast<int>(std::ceil(sx1)) - ox);
					const int h = (std::max)(1, static_cast<int>(std::ceil(sy1)) - oy);
					const int gx = (w + 15) / 16, gy = (h + 15) / 16;

					auto ensureTex = [&](pl_tex& tex, int tw, int th, pl_fmt fmt, bool hostRead) -> bool
					{
						if (tex && tex->params.w == tw && tex->params.h == th)
							return true;
						pl_tex_params tp{};
						tp.w = tw;
						tp.h = th;
						tp.format = fmt;
						tp.sampleable = true;
						tp.storable = true;
						tp.host_readable = hostRead;
						return pl_tex_recreate(self->gpu, &tex, &tp);
					};
					Assert::IsTrue(ensureTex(self->em, p->tex->params.w, p->tex->params.h, self->rg32fFmt, false) &&
						ensureTex(self->edge, w, h, self->r16fFmt, false) &&
						ensureTex(self->partial, gx, gy, self->rgba32fFmt, false) &&
						ensureTex(self->state, 1, 1, self->rgba32fFmt, true),
						L"reference measure texture creation failed");

					// Faithful branch via ScalerPass (identical settings to AdaptiveDownscale::Module).
					ScalerPass::Settings fs;
					fs.axis = ScalerPass::Axis::Both;
					fs.order = ScalerPass::Order::DownFirst;
					fs.antiring = self->settings.antiring;
					fs.precision = ScalerPass::Precision::F32;
					fs.filter.kind = ScalerKernels::Kind::EwaLanczosSharp;
					ScalerPass::Source fsrc;
					fsrc.tex = p->tex;
					fsrc.x0 = sx0; fsrc.y0 = sy0; fsrc.x1 = sx1; fsrc.y1 = sy1;
					Assert::IsTrue(self->faithfulScaler.Apply(self->gpu, p->dispatch, fsrc, dw, dh, fs, self->faithful, 0),
						L"reference faithful scaler failed");

					const pl_color_system sys = p->orig_repr ? p->orig_repr->sys : p->repr.sys;
					const pl_color_primaries prim = p->orig_color ? p->orig_color->primaries : p->color.primaries;
					self->domain = VpMeasure::MeasureDomainFor(p->color, sys, prim);
					std::string why;
					Assert::IsTrue(Vpv::Regions::DispatchEdgeMapStage(p->dispatch, p->tex, p->color, sys, prim, self->em, why),
						L"reference DispatchEdgeMapStage failed");

					static const char* const kUntiledHeader = "shared vec4 ref_sum[256];\n";
					static const char* const kUntiledBody = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
uint li = gl_LocalInvocationIndex;
vec4 v = vec4(0.0);
if (q.x < int(region.x) && q.y < int(region.y)) {
    ivec2 p = ivec2(origin) + q;
    ivec2 lim = textureSize(em, 0) - ivec2(1);
    vec2 m = texelFetch(em, clamp(p, ivec2(0), lim), 0).xy;
    float e = m.y;
    float b = texelFetch(em, clamp(p + ivec2(0, -1), ivec2(0), lim), 0).y;
    float d = texelFetch(em, clamp(p + ivec2(-1, 0), ivec2(0), lim), 0).y;
    float f = texelFetch(em, clamp(p + ivec2(1, 0), ivec2(0), lim), 0).y;
    float h = texelFetch(em, clamp(p + ivec2(0, 1), ivec2(0), lim), 0).y;
    float grad = 0.25 * m.x;
    float ed = smoothstep(edge_lo, edge_hi, grad);
    imageStore(edge_out, q, vec4(ed));
    float lap = abs(4.0 * e - b - d - f - h);
    float flat_px = grad < edge_lo ? 1.0 : 0.0;
    float wgt = max(mix(1.0, 2.0 * (1.0 - clamp(e / 255.0, 0.0, 1.0)), shadow_bias), 0.0);
    v = vec4(lap * wgt * flat_px, flat_px, 1.0 - ed, 0.0);
}
ref_sum[li] = v;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        ref_sum[li] += ref_sum[li + s];
    barrier();
}
if (li == 0u)
    imageStore(partial_out, ivec2(gl_WorkGroupID.xy), ref_sum[0]);
)GLSL";

					static const char* const kNoHaloHeader =
						"shared float nh_tile[16][16];\n"
						"shared vec4 ref_sum[256];\n";
					static const char* const kNoHaloBody = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 lid = ivec2(gl_LocalInvocationID.xy);
uint li = gl_LocalInvocationIndex;
ivec2 lim = textureSize(em, 0) - ivec2(1);
ivec2 p = ivec2(origin) + q;
vec2 m = texelFetch(em, clamp(p, ivec2(0), lim), 0).xy;
nh_tile[lid.y][lid.x] = m.y;
barrier();
vec4 v = vec4(0.0);
if (q.x < int(region.x) && q.y < int(region.y)) {
    float e = nh_tile[lid.y][lid.x];
    float b = nh_tile[max(lid.y - 1, 0)][lid.x];
    float d = nh_tile[lid.y][max(lid.x - 1, 0)];
    float f = nh_tile[lid.y][min(lid.x + 1, 15)];
    float h = nh_tile[min(lid.y + 1, 15)][lid.x];
    float grad = 0.25 * m.x;
    float ed = smoothstep(edge_lo, edge_hi, grad);
    imageStore(edge_out, q, vec4(ed));
    float lap = abs(4.0 * e - b - d - f - h);
    float flat_px = grad < edge_lo ? 1.0 : 0.0;
    float wgt = max(mix(1.0, 2.0 * (1.0 - clamp(e / 255.0, 0.0, 1.0)), shadow_bias), 0.0);
    v = vec4(lap * wgt * flat_px, flat_px, 1.0 - ed, 0.0);
}
ref_sum[li] = v;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        ref_sum[li] += ref_sum[li + s];
    barrier();
}
if (li == 0u)
    imageStore(partial_out, ivec2(gl_WorkGroupID.xy), ref_sum[0]);
)GLSL";

					const float origin[2] = { static_cast<float>(ox), static_cast<float>(oy) };
					const float region[2] = { static_cast<float>(w), static_cast<float>(h) };
					const pl_shader_desc md[3] = {
						SampledDesc("em", self->em, false),
						StorageDesc("edge_out", self->edge),
						StorageDesc("partial_out", self->partial) };
					const pl_shader_var mv[5] = {
						{ pl_var_vec2("origin"), origin, true },
						{ pl_var_vec2("region"), region, true },
						{ pl_var_float("edge_lo"), &self->settings.edgeLo, true },
						{ pl_var_float("edge_hi"), &self->settings.edgeHi, true },
						{ pl_var_float("shadow_bias"), &self->settings.shadowBias, true } };
					Assert::IsTrue(RunCompute(p->dispatch,
						self->omitHalo ? "ref measure (no halo negative control)" : "ref measure (untiled)",
						self->omitHalo ? kNoHaloHeader : kUntiledHeader,
						self->omitHalo ? kNoHaloBody : kUntiledBody,
						md, 3, mv, 5, gx, gy),
						L"reference measure compute dispatch failed");

					static const char* const kFinalHeader = "shared vec4 ref_total[256];\n";
					static const char* const kFinalBody = R"GLSL(
uint li = gl_LocalInvocationIndex;
ivec2 n = textureSize(partial_in, 0);
vec4 acc = vec4(0.0);
for (int idx = int(li); idx < n.x * n.y; idx += 256)
    acc += texelFetch(partial_in, ivec2(idx % n.x, idx / n.x), 0);
ref_total[li] = acc;
barrier();
for (uint s = 128u; s > 0u; s >>= 1u) {
    if (li < s)
        ref_total[li] += ref_total[li + s];
    barrier();
}
if (li == 0u) {
    vec4 t = ref_total[0];
    float energy = t.x / max(t.y, 1.0);
    float free_mean = t.z / max(total_pixels, 1.0);
    float target = mix(grain_floor, 1.0, 1.0 - smoothstep(grain_lo, grain_hi, energy));
    imageStore(state_out, ivec2(0), vec4(target, free_mean, 0.0, 2.0));
}
)GLSL";
					const float totalPx = static_cast<float>(w) * static_cast<float>(h);
					const pl_shader_desc fd[2] = {
						SampledDesc("partial_in", self->partial, false),
						StorageDesc("state_out", self->state) };
					const pl_shader_var fv[4] = {
						{ pl_var_float("total_pixels"), &totalPx, true },
						{ pl_var_float("grain_lo"), &self->settings.grainLo, true },
						{ pl_var_float("grain_hi"), &self->settings.grainHi, true },
						{ pl_var_float("grain_floor"), &self->settings.grainFloor, true } };
					Assert::IsTrue(RunCompute(p->dispatch, "ref grain final", kFinalHeader, kFinalBody,
						fd, 2, fv, 4, 1, 1),
						L"reference final compute dispatch failed");

					self->srcRect = { sx0, sy0, sx1, sy1 };
					self->dstRect = p->dst_rect;
					self->edgeOrigin[0] = origin[0];
					self->edgeOrigin[1] = origin[1];
					return unchanged;
				}

				static pl_hook_res RunBlend(void* priv, const pl_hook_params* p)
				{
					ReferenceMeasureHook* self = static_cast<ReferenceMeasureHook*>(priv);
					const int w = std::abs(p->dst_rect.x1 - p->dst_rect.x0);
					const int h = std::abs(p->dst_rect.y1 - p->dst_rect.y0);
					pl_tex out = p->get_tex(p->priv, w, h);
					Assert::IsNotNull(out, L"reference blend get_tex failed");
					const float dw = static_cast<float>(std::abs(self->dstRect.x1 - self->dstRect.x0));
					const float dh = static_cast<float>(std::abs(self->dstRect.y1 - self->dstRect.y0));
					const float srcOrigin[2] = { self->srcRect.x0, self->srcRect.y0 };
					const float srcPerDst[2] = {
						dw > 0.0f ? (self->srcRect.x1 - self->srcRect.x0) / dw : 1.0f,
						dh > 0.0f ? (self->srcRect.y1 - self->srcRect.y0) / dh : 1.0f };
					const std::string hdr = "#define REF_KW " + VpMeasure::LumaWeightsGlsl(self->domain) + "\n";
					static const char* const kBlendBody = R"GLSL(
ivec2 q = ivec2(gl_GlobalInvocationID.xy);
ivec2 size = imageSize(dst);
if (q.x < size.x && q.y < size.y) {
    vec4 fa = texelFetch(faithful, q, 0);
    vec4 ss = texelFetch(hooked, q, 0);
    vec2 s = src_origin + (vec2(q) + vec2(0.5)) * src_per_dst;
    float e = texture(edge, (s - edge_origin) / vec2(textureSize(edge, 0))).r;
    float g = texelFetch(state, ivec2(0), 0).r;
    float w = clamp(w_base * (1.0 - e) * g, 0.0, 1.0);
    const vec3 k = REF_KW;
    vec4 o;
    o.rgb = fa.rgb + vec3(w * (dot(ss.rgb, k) - dot(fa.rgb, k)));
    o.a = fa.a;
    imageStore(dst, q, o);
}
)GLSL";
					const pl_shader_desc bd[5] = {
						SampledDesc("hooked", p->tex, false),
						SampledDesc("faithful", self->faithful, false),
						SampledDesc("edge", self->edge, true),
						SampledDesc("state", self->state, false),
						StorageDesc("dst", out) };
					const pl_shader_var bv[4] = {
						{ pl_var_vec2("src_origin"), srcOrigin, true },
						{ pl_var_vec2("src_per_dst"), srcPerDst, true },
						{ pl_var_vec2("edge_origin"), self->edgeOrigin, true },
						{ pl_var_float("w_base"), &self->settings.wBase, true } };
					Assert::IsTrue(RunCompute(p->dispatch, "ref blend", hdr.c_str(), kBlendBody,
						bd, 5, bv, 4, (w + 15) / 16, (h + 15) / 16),
						L"reference blend compute dispatch failed");
					pl_hook_res res{};
					res.output = PL_HOOK_SIG_TEX;
					res.tex = out;
					res.repr = p->repr;
					res.color = p->color;
					res.components = p->components;
					res.rect = p->rect;
					return res;
				}
			};

			auto renderHooks = [&](const pl_hook* h0, const pl_hook* h1, const pl_hook* h2,
				const pl_rect2df& crop, int dw, int dh) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = rgba32fTarget;
				tp.renderable = true;
				tp.host_readable = true;
				ScopedTex target(gpu, pl_tex_create(gpu, &tp));
				Assert::IsNotNull(target.tex, L"RGBA32F target creation failed");
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(srcTex.tex);
				image.crop = crop;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target.tex);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;
				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;
				const pl_hook* hooks[3] = { h0, h1, h2 };
				params.hooks = hooks;
				params.num_hooks = 3;
				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");
				std::vector<float> px(static_cast<size_t>(dw) * dh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target.tex;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"target download failed");
				return px;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"pixel buffer sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"pixels must remain finite");
					if (i % 4 == 3) continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst) worst = d;
				}
				return worst;
			};

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			struct CropCase { const wchar_t* label; pl_rect2df crop; int ox, oy, rw, rh, dw, dh; };
			const CropCase cases[2] = {
				{ L"[full 128x96 (8x6 tiles)] ", { 0.0f, 0.0f, 128.0f, 96.0f }, 0, 0, 128, 96, 64, 48 },
				{ L"[odd sub-crop 97x73 @ (13,11)] ", { 13.0f, 11.0f, 110.0f, 84.0f }, 13, 11, 97, 73, 48, 36 }
			};

			for (int ci = 0; ci < 2; ++ci)
			{
				const CropCase& cc = cases[ci];
				const CpuMeasureRef cpuRef = computeCpuRef(cc.ox, cc.oy, cc.rw, cc.rh);
				Assert::IsTrue(cpuRef.grainScale > 0.45f && cpuRef.grainScale < 0.95f,
					(std::wstring(cc.label) + L"precondition: grainScale must sit in active ramp interior, got " +
						std::to_wstring(cpuRef.grainScale)).c_str());

				// Production AdaptiveDownscale::Module (tiled ad_tile[18][18] measure pass).
				ScopedModule mod;
				mod->SetCutSource(noCutSource);
				mod->SetFrame(st, 1, 1.0f / 24.0f);
				const std::vector<float> modPx = renderHooks(mod->Measure(), ssim.hook, mod->Blend(), cc.crop, cc.dw, cc.dh);
				Assert::IsTrue(mod->Active() && mod->Failures() == 0,
					(std::wstring(cc.label) + L"AdaptiveDownscale::Module must succeed").c_str());
				// Poll stats on non-advancing repeats (seq stays 1 -> advance == 0).
				for (int rep = 0; rep < 3; ++rep)
					renderHooks(mod->Measure(), ssim.hook, mod->Blend(), cc.crop, cc.dw, cc.dh);
				const AdaptiveDownscale::Stats modStats = mod->LastStats();
				Assert::IsTrue(modStats.valid, (std::wstring(cc.label) + L"LastStats must become valid").c_str());

				// Independent un-tiled direct-texelFetch GPU reference on exact same input & crop.
				ReferenceMeasureHook untiledRef;
				untiledRef.Init(gpu, st, false, rg32fStore, r16fEdge, rgba32fStoreRead, 0x5650544553543100ull + uint64_t(ci) * 4ull);
				const std::vector<float> refPx = renderHooks(&untiledRef.measureHook, ssim.hook, &untiledRef.blendHook, cc.crop, cc.dw, cc.dh);
				float refStateVals[4] = {};
				pl_tex_transfer_params sdl{};
				sdl.tex = untiledRef.state;
				sdl.ptr = refStateVals;
				Assert::IsTrue(pl_tex_download(gpu, &sdl), L"reference state download failed");
				const float refGrainScale = refStateVals[0];
				const float refMeanW = st.wBase * refStateVals[1] * refStateVals[0];

				// 1. Cross-check against independent pure-C++ CPU reference (<= 1e-5f).
				Assert::IsTrue(std::fabs(modStats.grainScale - cpuRef.grainScale) <= 1e-5f,
					(std::wstring(cc.label) + L"grainScale vs CPU reference mismatch: mod=" +
						std::to_wstring(modStats.grainScale) + L" cpu=" + std::to_wstring(cpuRef.grainScale)).c_str());
				Assert::IsTrue(std::fabs(modStats.meanW - cpuRef.meanW) <= 1e-5f,
					(std::wstring(cc.label) + L"meanW vs CPU reference mismatch: mod=" +
						std::to_wstring(modStats.meanW) + L" cpu=" + std::to_wstring(cpuRef.meanW)).c_str());

				// 2. Bit-identical parity (<= 1e-6f) against independent un-tiled GPU reference.
				const float gsDiff = std::fabs(modStats.grainScale - refGrainScale);
				const float mwDiff = std::fabs(modStats.meanW - refMeanW);
				const float pxDiff = maxRgbAbsDiff(modPx, refPx);
				Assert::IsTrue(gsDiff <= 1e-6f,
					(std::wstring(cc.label) + L"grainScale vs un-tiled GPU ref must be bit-identical (<= 1e-6), got diff " +
						std::to_wstring(gsDiff)).c_str());
				Assert::IsTrue(mwDiff <= 1e-6f,
					(std::wstring(cc.label) + L"meanW vs un-tiled GPU ref must be bit-identical (<= 1e-6), got diff " +
						std::to_wstring(mwDiff)).c_str());
				Assert::IsTrue(pxDiff <= 1e-6f,
					(std::wstring(cc.label) + L"blended pixels vs un-tiled GPU ref must be bit-identical (<= 1e-6), got diff " +
						std::to_wstring(pxDiff)).c_str());

				// 3. MANDATORY IN-TEST NEGATIVE CONTROL (exact same input): omitting the 1-pixel halo
				//    (clamping local tile reads to [0, 15] instead of [0, 17]) MUST introduce measurable
				//    workgroup-seam errors at x = 15, 16, 31, 32, ... (> 1e-3) and shift grainScale & meanW (> 1e-3).
				if (ci == 0)
				{
					Assert::IsTrue(cpuRef.maxSeamLapErr > 1e-3f,
						(L"negative control: omitting halo must break workgroup-seam Laplacian (> 1e-3), got " +
							std::to_wstring(cpuRef.maxSeamLapErr)).c_str());

					ReferenceMeasureHook noHaloRef;
					noHaloRef.Init(gpu, st, true, rg32fStore, r16fEdge, rgba32fStoreRead, 0x5650544553543200ull);
					const std::vector<float> noHaloPx = renderHooks(&noHaloRef.measureHook, ssim.hook, &noHaloRef.blendHook, cc.crop, cc.dw, cc.dh);
					float nhStateVals[4] = {};
					pl_tex_transfer_params nhdl{};
					nhdl.tex = noHaloRef.state;
					nhdl.ptr = nhStateVals;
					Assert::IsTrue(pl_tex_download(gpu, &nhdl), L"no-halo negative control state download failed");
					const float nhGrainScale = nhStateVals[0];
					const float nhMeanW = st.wBase * nhStateVals[1] * nhStateVals[0];
					const float nhGsErr = std::fabs(nhGrainScale - modStats.grainScale);
					const float nhMwErr = std::fabs(nhMeanW - modStats.meanW);
					const float nhPxErr = maxRgbAbsDiff(noHaloPx, modPx);
					Assert::IsTrue(nhGsErr > 1e-3f,
						(L"negative control (no-halo [0,15] clamp): grainScale error must exceed 1e-3, got " +
							std::to_wstring(nhGsErr)).c_str());
					Assert::IsTrue(nhMwErr > 1e-3f,
						(L"negative control (no-halo [0,15] clamp): meanW error must exceed 1e-3, got " +
							std::to_wstring(nhMwErr)).c_str());
					Assert::IsTrue(nhPxErr > 1e-4f,
						(L"negative control (no-halo [0,15] clamp): blended output error must exceed 1e-4 (100x parity tol), got " +
							std::to_wstring(nhPxErr)).c_str());
					noHaloRef.Release();
				}

				untiledRef.Release();
				mod->Release();
			}

			srcTex.Reset();
			ssim.Reset();
			logGuard.Release();
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#10 (GEM-077 / ADS-PERF, WARP GPU):
		// On a one-axis upscale (upscale.on = true, upscale.oneAxis = true, cnnResidualStabilise = false,
		// 128x96 -> 192x96 with motionStrength = 1.0 & grainMap = true), AdaptiveDownscale::Module must NOT
		// dispatch the measure/edge/state/grain-map/motion-map compute passes (zero measure/edge/state/motion
		// dispatches; dispatch count drops by >= 4 vs two-axis negative control), keep Failures() == 0 &
		// Active() == true, and match the faithful branch within <= 1e-5f.
		// Mandatory in-test negative control: exact same module, settings, Motion, and GrainField with
		// upscale.oneAxis = false (two-axis 128x96 -> 192x144) MUST dispatch those measure/edge/state/motion
		// passes (twoAxisDispatches >= oneAxisDispatches + 4, strictly >= +3).
		TEST_METHOD(OneAxisUpscaleRunsNoMeasurePasses)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			ScopedLogCapture logGuard(fixture, log);

			pl_fmt rgba32fSample = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt rgba32fTarget = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt gridFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			pl_fmt trustFmt = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(rgba32fSample && rgba32fTarget && gridFmt && trustFmt,
				L"WARP must provide float32 source, target, grid, and trust formats");

			static constexpr int sw = 128, sh = 96;
			static constexpr int bw = sw / 16, bh = sh / 16;

			// Neutral-grey sharp stripes + fine oscillation so Lanczos3 faithful and bilinear main scaler differ strongly.
			std::vector<float> srcData(static_cast<size_t>(sw) * sh * 4, 0.0f);
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					const float stripe = ((x / 4) % 2) != 0 ? 0.85f : 0.15f;
					const float wave = 0.08f * std::sin(static_cast<float>(x) * 1.3f) * std::cos(static_cast<float>(y) * 0.9f);
					const float v = (std::max)(0.0f, (std::min)(1.0f, stripe + wave));
					float* p = &srcData[(static_cast<size_t>(y) * sw + x) * 4];
					p[0] = p[1] = p[2] = v;
					p[3] = 1.0f;
				}
			}
			pl_tex_params stp{};
			stp.w = sw;
			stp.h = sh;
			stp.format = rgba32fSample;
			stp.sampleable = true;
			stp.initial_data = srcData.data();
			ScopedTex srcTex(gpu, pl_tex_create(gpu, &stp));
			Assert::IsNotNull(srcTex.tex, L"source texture creation failed");

			auto makeUniformGrid = [&](float r, float g, float b, float a) -> pl_tex
			{
				std::vector<float> data(static_cast<size_t>(bw) * bh * 4, 0.0f);
				for (size_t i = 0; i < static_cast<size_t>(bw) * bh; ++i)
				{
					data[i * 4 + 0] = r;
					data[i * 4 + 1] = g;
					data[i * 4 + 2] = b;
					data[i * 4 + 3] = a;
				}
				pl_tex_params gp{};
				gp.w = bw;
				gp.h = bh;
				gp.format = gridFmt;
				gp.sampleable = true;
				gp.initial_data = data.data();
				pl_tex tex = pl_tex_create(gpu, &gp);
				Assert::IsNotNull(tex, L"grid texture creation failed");
				return tex;
			};
			ScopedTex fieldTex(gpu, makeUniformGrid(0.0f, 0.0f, 1.0f, 0.0f));
			ScopedTex errorTex(gpu, makeUniformGrid(10.0f, 127.5f, 0.0f, 1.0f));
			const float trustVal = 1.0f;
			pl_tex_params trp{};
			trp.w = 1;
			trp.h = 1;
			trp.format = trustFmt;
			trp.sampleable = true;
			trp.initial_data = &trustVal;
			ScopedTex trustTex(gpu, pl_tex_create(gpu, &trp));
			Assert::IsNotNull(trustTex.tex, L"frame trust texture creation failed");

			AdaptiveDownscale::Settings st;
			st.motionStrength = 1.0f;
			st.grainMap = true;
			st.cnnResidualStabilise = false;
			st.ssimResidualStabilise = false;
			st.faithfulAuto = true;
			st.float32Picture = true;
			st.antiring = 0.5f;

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"output sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"output pixels must remain finite");
					if (i % 4 == 3) continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst) worst = d;
				}
				return worst;
			};

			auto renderUpscale = [&](AdaptiveDownscale::Module* mod, uint64_t seq, int ow, int oh,
				bool oneAxis, float wBase, HookDispatchRecord* measureRec, HookDispatchRecord* blendRec) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = ow;
				tp.h = oh;
				tp.format = rgba32fTarget;
				tp.renderable = true;
				tp.host_readable = true;
				ScopedTex target(gpu, pl_tex_create(gpu, &tp));
				Assert::IsNotNull(target.tex, L"RGBA32F target creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(srcTex.tex);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target.tex);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.sigmoid_params = nullptr;
				params.upscaler = &pl_filter_bilinear;
				params.disable_linear_scaling = true;

				HookDispatchProxy mProxy, bProxy;
				const pl_hook* hooks[2] = {};
				if (mod)
				{
					mod->SetFrame(st, seq, 1.0f / 24.0f);
					mod->SetSourceDownscales(false);
					AdaptiveDownscale::Upscale up;
					up.on = true;
					up.wBase = wBase;
					up.nativeWidth = static_cast<float>(sw);
					up.nativeHeight = static_cast<float>(sh);
					up.oneAxis = oneAxis;
					mod->SetUpscale(up);

					AdaptiveDownscale::Motion mot;
					mot.field = fieldTex.tex;
					mot.frameTrust = trustTex.tex;
					mot.minTrustedShare = 0.25f;
					mot.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					mod->SetMotion(mot);

					AdaptiveDownscale::GrainField gf;
					gf.error = errorTex.tex;
					gf.field = fieldTex.tex;
					gf.frameTrust = trustTex.tex;
					gf.minTrustedShare = 0.25f;
					gf.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					mod->SetGrainField(gf);

					if (measureRec && blendRec)
					{
						measureRec->Reset();
						blendRec->Reset();
						mProxy.Init(mod->Measure(), measureRec);
						bProxy.Init(mod->Blend(), blendRec);
						hooks[0] = &mProxy.hook;
						hooks[1] = &bProxy.hook;
					}
					else
					{
						hooks[0] = mod->Measure();
						hooks[1] = mod->Blend();
					}
					params.hooks = hooks;
					params.num_hooks = 2;
				}

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				std::vector<float> px(static_cast<size_t>(ow) * oh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target.tex;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"target download failed");
				return px;
			};

			// Reference faithful-only output (oneAxis = false, wBase = 0.0f) and unblended bilinear baseline on 128x96 -> 192x96.
			const std::vector<float> bilinearOnlyPx = renderUpscale(nullptr, 0, 192, 96, false, 0.0f, nullptr, nullptr);
			std::vector<float> faithfulOnlyPx;
			{
				ScopedModule refMod;
				refMod->SetCutSource(noCutSource);
				faithfulOnlyPx = renderUpscale(&(*refMod), 1, 192, 96, false, 0.0f, nullptr, nullptr);
				Assert::IsTrue(refMod->Active() && refMod->Failures() == 0, L"faithful-only reference must succeed");
			}

			// 1. Fresh AdaptiveDownscale::Module on one-axis upscale (128x96 -> 192x96, oneAxis = true, cnnResidualStabilise = false):
			//    MUST skip all measure/edge/state/grain-map/motion-map passes (only faithful ScalerPass in Measure() + Blend()).
			ScopedModule mod;
			mod->SetCutSource(noCutSource);
			HookDispatchRecord oneAxisMeasure, oneAxisBlend;
			const std::vector<float> oneAxisPx = renderUpscale(&(*mod), 1, 192, 96, true, 1.0f, &oneAxisMeasure, &oneAxisBlend);
			Assert::IsTrue(mod->Active() && mod->Failures() == 0,
				L"one-axis upscale must remain active with zero failures");
			Assert::AreEqual(1, mod->LastUpscaleRatio(), L"one-axis upscale must operate at 1x base ratio");
			Assert::AreEqual(1, oneAxisMeasure.count,
				(L"one-axis upscale Measure() must dispatch ONLY the faithful scaler pass (1), got " +
					std::to_wstring(oneAxisMeasure.count)).c_str());
			Assert::IsFalse(oneAxisMeasure.HasPassContaining("edge map") ||
				oneAxisMeasure.HasPassContaining("upscale: edge") ||
				oneAxisMeasure.HasPassContaining("measure") ||
				oneAxisMeasure.HasPassContaining("cut state") ||
				oneAxisMeasure.HasPassContaining("grain") ||
				oneAxisMeasure.HasPassContaining("motion-adapted"),
				L"one-axis upscale must not dispatch any edge/measure/state/grain/motion pass");

			const float faithfulDiff = maxRgbAbsDiff(oneAxisPx, faithfulOnlyPx);
			Assert::IsTrue(faithfulDiff <= 1e-5f,
				(L"one-axis upscale output must match faithful branch within <= 1e-5, got " +
					std::to_wstring(faithfulDiff)).c_str());
			const float vsBilinearDiff = maxRgbAbsDiff(oneAxisPx, bilinearOnlyPx);
			Assert::IsTrue(vsBilinearDiff > 0.01f,
				(L"precondition: one-axis faithful output must visibly differ from bilinear main scaler (> 0.01), got " +
					std::to_wstring(vsBilinearDiff)).c_str());

			// 2. MANDATORY IN-TEST NEGATIVE CONTROL (exact same module, settings, Motion, and GrainField with
			//    upscale.oneAxis = false, two-axis 128x96 -> 192x144): measure/edge/state/motion passes MUST dispatch.
			HookDispatchRecord twoAxisMeasure, twoAxisBlend;
			renderUpscale(&(*mod), 2, 192, 144, false, 1.0f, &twoAxisMeasure, &twoAxisBlend);
			Assert::IsTrue(mod->Active() && mod->Failures() == 0,
				L"two-axis negative control must remain active with zero failures");
			Assert::IsTrue(mod->LastMotionState() == AdaptiveDownscale::MotionState::Used,
				L"two-axis negative control must build and use the motion map");

			const int oneAxisTotal = oneAxisMeasure.count + oneAxisBlend.count;
			const int twoAxisTotal = twoAxisMeasure.count + twoAxisBlend.count;
			Assert::IsTrue(twoAxisMeasure.count >= oneAxisMeasure.count + 4,
				(L"negative control (two-axis vs one-axis): Measure() dispatches must drop by >= 4 on one-axis (twoAxis=" +
					std::to_wstring(twoAxisMeasure.count) + L", oneAxis=" + std::to_wstring(oneAxisMeasure.count) + L")").c_str());
			Assert::IsTrue(twoAxisTotal >= oneAxisTotal + 4,
				(L"negative control (two-axis vs one-axis): total Measure()+Blend() dispatches must drop by >= 4 (twoAxis=" +
					std::to_wstring(twoAxisTotal) + L", oneAxis=" + std::to_wstring(oneAxisTotal) + L")").c_str());
			Assert::IsTrue(twoAxisMeasure.HasPassContaining("edge") &&
				twoAxisMeasure.HasPassContaining("cut state") &&
				twoAxisMeasure.HasPassContaining("motion-adapted"),
				L"negative control (two-axis): Measure() must dispatch edge, cut state, and motion-adapted passes");

			mod->Release();
			trustTex.Reset();
			errorTex.Reset();
			fieldTex.Reset();
			srcTex.Reset();
			logGuard.Release();
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}

		// Q#10 (GEM-077 / ADS-PERF, WARP GPU):
		// Verify skeleton-memo kEdgeMap serving skips the private Sobel edge fill (DispatchEdgeMapStage / edgeLocal):
		//  (a) With skeleton memo serving kEdgeMap: EdgeMapStatus().empty() is true, Failures() == 0,
		//      memo kEdgeMap ownerFillCalls == 1, Measure() dispatches ZERO private "VP-V edge map" passes
		//      on p->dispatch, blended output is bit-identical (<= 1e-6f) to the non-memo path, AND serving
		//      a synthetic inverted kEdgeMap proves the blend strictly follows the served edge map.
		//  (b) Mandatory in-test negative control — without skeleton memo: !EdgeMapStatus().empty()
		//      ("no skeleton memo"), private "VP-V edge map" IS dispatched (+1 Measure() dispatch vs memo path),
		//      and sharp stripe edges vs zero-Sobel texture produce distinct blend weights (maxEdgeDiff > 0.02f).
		TEST_METHOD(ServedEdgeSkipsThePrivateEdge)
		{
			TargetLutGpuFixture fixture;
			Assert::IsTrue(fixture.Create(), L"Could not create the libplacebo WARP test device");
			pl_gpu gpu = fixture.Gpu();
			std::string log;
			ScopedLogCapture logGuard(fixture, log);

			const std::string ssimSource = LoadBundledShader("SSimDownscaler.glsl");
			Assert::IsFalse(ssimSource.empty(), L"SSimDownscaler.glsl must load");
			ScopedHook ssim(pl_mpv_user_shader_parse(gpu, ssimSource.data(), ssimSource.size()));
			Assert::IsNotNull(ssim.hook, L"SSimDownscaler.glsl must parse");

			pl_fmt rgba32fSample = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR));
			pl_fmt rgba32fTarget = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
				static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
			pl_fmt rg32fSample = pl_find_fmt(gpu, PL_FMT_FLOAT, 2, 32, 32, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsTrue(rgba32fSample && rgba32fTarget && rg32fSample,
				L"WARP must provide float32 source, target, and RG32F edge-map formats");

			// 144x96 -> 96x64 (exact 1.5x downscale, 9x6 16x16 workgroups):
			//  - Left half (x < 72): sharp vertical stripes (220 vs 30, period 12 px -> grad ~190 >> edgeHi = 100).
			//  - Right half (x >= 72): period-3 sine texture (128 + 20*sin(2*pi*x/3) -> max grad ~34.6 < edgeLo = 50,
			//    so ed == 0 everywhere, while 1.5x downscale maps period-3 source to period-2 destination where SSIM
			//    and faithful EWA Lanczos differ strongly).
			static constexpr int sw = 144, sh = 96, dw = 96, dh = 64;
			std::vector<float> srcData(static_cast<size_t>(sw) * sh * 4, 0.0f);
			constexpr float kTwoPiOver3 = 2.0943951023931953f;
			for (int y = 0; y < sh; ++y)
			{
				for (int x = 0; x < sw; ++x)
				{
					const float code = (x < sw / 2)
						? ((((x / 12) % 2) != 0) ? 220.0f : 30.0f)
						: (128.0f + 20.0f * std::sin(kTwoPiOver3 * static_cast<float>(x)));
					const float norm = code / 255.0f;
					float* p = &srcData[(static_cast<size_t>(y) * sw + x) * 4];
					p[0] = p[1] = p[2] = norm;
					p[3] = 1.0f;
				}
			}
			pl_tex_params stp{};
			stp.w = sw;
			stp.h = sh;
			stp.format = rgba32fSample;
			stp.sampleable = true;
			stp.initial_data = srcData.data();
			ScopedTex srcTex(gpu, pl_tex_create(gpu, &stp));
			Assert::IsNotNull(srcTex.tex, L"source texture creation failed");

			// High grain thresholds so grainScale == 1.0 (isolating the edge term w = 1 - e cleanly).
			// edgeLo = 50, edgeHi = 100 cleanly separates right-half period-3 texture (grad <= 34.6 -> e = 0)
			// from left-half hard stripes (grad ~ 190 -> e = 1).
			AdaptiveDownscale::Settings st;
			st.wBase = 1.0f;
			st.edgeLo = 50.0f;
			st.edgeHi = 100.0f;
			st.grainLo = 400.0f;
			st.grainHi = 500.0f;
			st.grainSmoothMs = 0.0f;
			st.grainMap = false;
			st.ssimResidualStabilise = false;
			st.float32Picture = true;

			auto noCutSource = [](uint64_t, uint64_t)
			{
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				v.cut = false;
				return v;
			};

			auto renderDownscale = [&](AdaptiveDownscale::Module* mod, const AdaptiveDownscale::Settings& s,
				uint64_t seq, HookDispatchRecord* measureRec, HookDispatchRecord* blendRec) -> std::vector<float>
			{
				pl_tex_params tp{};
				tp.w = dw;
				tp.h = dh;
				tp.format = rgba32fTarget;
				tp.renderable = true;
				tp.host_readable = true;
				ScopedTex target(gpu, pl_tex_create(gpu, &tp));
				Assert::IsNotNull(target.tex, L"RGBA32F target creation failed");

				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(srcTex.tex);
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target.tex);
				image.color.transfer = out.color.transfer = PL_COLOR_TRC_GAMMA22;
				image.color.primaries = out.color.primaries = PL_COLOR_PRIM_BT_709;

				pl_render_params params = pl_render_default_params;
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.downscaler = &pl_filter_mitchell;
				params.disable_linear_scaling = true;

				HookDispatchProxy mProxy, bProxy;
				std::vector<const pl_hook*> hooks;
				if (mod)
				{
					mod->SetFrame(s, seq, 1.0f / 24.0f);
					mod->SetSourceDownscales(true);
					if (measureRec && blendRec)
					{
						measureRec->Reset();
						blendRec->Reset();
						mProxy.Init(mod->Measure(), measureRec);
						bProxy.Init(mod->Blend(), blendRec);
						hooks.push_back(&mProxy.hook);
						hooks.push_back(ssim.hook);
						hooks.push_back(&bProxy.hook);
					}
					else
					{
						hooks.push_back(mod->Measure());
						hooks.push_back(ssim.hook);
						hooks.push_back(mod->Blend());
					}
				}
				else
				{
					hooks.push_back(ssim.hook);
				}
				params.hooks = hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());

				const bool ok = pl_render_image(fixture.Renderer(), &image, &out, &params);
				pl_gpu_finish(gpu);
				Assert::IsTrue(ok, L"pl_render_image failed on WARP");

				std::vector<float> px(static_cast<size_t>(dw) * dh * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = target.tex;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"target download failed");
				return px;
			};

			auto maxRgbAbsDiff = [](const std::vector<float>& a, const std::vector<float>& b) -> float
			{
				Assert::AreEqual(a.size(), b.size(), L"output sizes must match");
				float worst = 0.0f;
				for (size_t i = 0; i < a.size(); ++i)
				{
					Assert::IsTrue(std::isfinite(a[i]) && std::isfinite(b[i]), L"output pixels must remain finite");
					if (i % 4 == 3) continue;
					const float d = std::fabs(a[i] - b[i]);
					if (d > worst) worst = d;
				}
				return worst;
			};

			// Ensure no stale thread-local frame memo before starting.
			Vpv::Hub::ResetThisThreadFrameForTesting();
			Assert::IsFalse(Vpv::Skeleton::CurrentFrameMemo().Valid(), L"precondition: no active frame memo initially");

			const std::vector<float> ssimOnlyPx = renderDownscale(nullptr, st, 0, nullptr, nullptr);
			std::vector<float> faithfulOnlyPx;
			{
				AdaptiveDownscale::Settings faithfulSt = st;
				faithfulSt.wBase = 0.0f;
				ScopedModule refMod;
				refMod->SetCutSource(noCutSource);
				faithfulOnlyPx = renderDownscale(&(*refMod), faithfulSt, 1, nullptr, nullptr);
				Assert::IsTrue(refMod->Active() && refMod->Failures() == 0, L"faithful-only baseline must succeed");
			}

			VpSkeleton::Dims dims;
			dims.width = static_cast<uint32_t>(sw);
			dims.height = static_cast<uint32_t>(sh);
			const uint64_t reserve = Vpv::Regions::EdgeMapFillReserveBytes(dims);

			// (a1) With skeleton memo serving real kEdgeMap (MakeEdgeMapOwnerFill):
			//      EdgeMapStatus().empty() is true, Failures() == 0, ownerFillCalls == 1,
			//      and private "VP-V edge map" is NOT dispatched on p->dispatch.
			HookDispatchRecord memoMeasureRec, memoBlendRec;
			std::vector<float> memoBlendedPx;
			{
				ScopedEdgeMapMemoHost memoHost(gpu, Vpv::Regions::MakeEdgeMapOwnerFill(reserve));
				memoHost.BeginFrame(1);
				Assert::IsTrue(Vpv::Skeleton::CurrentFrameMemo().Valid(),
					L"precondition: skeleton memo must be active for memo-served test");

				ScopedModule memoMod;
				memoMod->SetCutSource(noCutSource);
				memoBlendedPx = renderDownscale(&(*memoMod), st, 1, &memoMeasureRec, &memoBlendRec);
				Assert::IsTrue(memoMod->Active() && memoMod->Failures() == 0,
					L"memo-served kEdgeMap frame must succeed with zero failures");
				Assert::IsTrue(memoMod->EdgeMapStatus().empty(),
					(L"memo-served kEdgeMap must clear EdgeMapStatus(), got: " +
						ExtendedRangeGpuProbe::ToWide(memoMod->EdgeMapStatus())).c_str());
				Assert::AreEqual(uint64_t(1), memoHost.memo->CountersOf(VpSkeleton::kEdgeMap).ownerFillCalls,
					L"skeleton memo kEdgeMap owner fill must run once");
				Assert::IsFalse(memoMeasureRec.HasPassContaining("VP-V edge map"),
					L"memo-served kEdgeMap must skip the module's private DispatchEdgeMapStage pass");
			}

			// (a2) Synthetic inverted kEdgeMap served via skeleton memo:
			//      left half (hard stripes) served as em.r = 0 -> e = 0 -> w = 1 (SSIM);
			//      right half (period-3 texture) served as em.r = 600 -> 0.25*600 = 150 > edgeHi (100) -> e = 1 -> w = 0 (faithful).
			//      Proves blended output strictly obeys the served edge map, not a recomputed private map.
			{
				std::vector<float> synthEm(static_cast<size_t>(sw) * sh * 2, 0.0f);
				for (int y = 0; y < sh; ++y)
				{
					for (int x = 0; x < sw; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * sw + x) * 2;
						synthEm[idx + 0] = (x < sw / 2) ? 0.0f : 600.0f;
						synthEm[idx + 1] = 128.0f;
					}
				}
				pl_tex_params ep{};
				ep.w = sw;
				ep.h = sh;
				ep.format = rg32fSample;
				ep.sampleable = true;
				ep.initial_data = synthEm.data();
				ScopedTex synthEmTex(gpu, pl_tex_create(gpu, &ep));
				Assert::IsNotNull(synthEmTex.tex, L"synthetic RG32F edge map creation failed");

				pl_tex synthEmRaw = synthEmTex.tex;
				VpSkeleton::OwnerFill synthFill = Vpv::Regions::MakeEdgeMapOwnerFill(reserve);
				synthFill.fill = [synthEmRaw, reserve](const VpSkeleton::OwnerFillRequest&, VpSkeleton::SkeletonMemo&,
					VpSkeleton::OwnerFillOutput& out) {
					out.gpu.resource = const_cast<void*>(static_cast<const void*>(synthEmRaw));
					out.gpu.width = static_cast<uint32_t>(synthEmRaw->params.w);
					out.gpu.height = static_cast<uint32_t>(synthEmRaw->params.h);
					out.gpu.format = static_cast<uint32_t>(VpSkeleton::TexFormat::RG32F);
					out.gpu.bytes = reserve;
					out.gpu.keepAlive = std::shared_ptr<void>(out.gpu.resource, [](void*) {});
					return true;
				};

				std::vector<float> synthBlendedPx;
				{
					ScopedEdgeMapMemoHost synthHost(gpu, std::move(synthFill));
					synthHost.BeginFrame(1);
					ScopedModule synthMod;
					synthMod->SetCutSource(noCutSource);
					HookDispatchRecord sMeasure, sBlend;
					synthBlendedPx = renderDownscale(&(*synthMod), st, 1, &sMeasure, &sBlend);
					Assert::IsTrue(synthMod->Active() && synthMod->Failures() == 0,
						L"synthetic served kEdgeMap frame must succeed");
					Assert::IsTrue(synthMod->EdgeMapStatus().empty(),
						L"synthetic served kEdgeMap must report empty EdgeMapStatus()");
					Assert::AreEqual(uint64_t(1), synthHost.memo->CountersOf(VpSkeleton::kEdgeMap).ownerFillCalls,
						L"synthetic kEdgeMap fill must be served through the skeleton memo");
					Assert::IsFalse(sMeasure.HasPassContaining("VP-V edge map"),
						L"synthetic served kEdgeMap must not dispatch private VP-V edge map");
				}
				synthEmTex.Reset();

				float leftVsSsim = 0.0f, leftVsFaithful = 0.0f;
				float rightVsFaithful = 0.0f, rightVsSsim = 0.0f;
				for (int y = 8; y < dh - 8; ++y)
				{
					for (int x = 4; x < dw / 2 - 6; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * dw + x) * 4 + 1;
						leftVsSsim = (std::max)(leftVsSsim, std::fabs(synthBlendedPx[i] - ssimOnlyPx[i]));
						leftVsFaithful = (std::max)(leftVsFaithful, std::fabs(synthBlendedPx[i] - faithfulOnlyPx[i]));
					}
					for (int x = dw / 2 + 6; x < dw - 4; ++x)
					{
						const size_t i = (static_cast<size_t>(y) * dw + x) * 4 + 1;
						rightVsFaithful = (std::max)(rightVsFaithful, std::fabs(synthBlendedPx[i] - faithfulOnlyPx[i]));
						rightVsSsim = (std::max)(rightVsSsim, std::fabs(synthBlendedPx[i] - ssimOnlyPx[i]));
					}
				}
				Assert::IsTrue(leftVsSsim <= 1e-3f && leftVsFaithful > 0.02f,
					(L"served kEdgeMap (left em.r=0 -> SSIM): leftVsSsim=" + std::to_wstring(leftVsSsim) +
						L", leftVsFaithful=" + std::to_wstring(leftVsFaithful)).c_str());
				Assert::IsTrue(rightVsFaithful <= 1e-3f && rightVsSsim > 0.02f,
					(L"served kEdgeMap (right em.r=600 -> faithful): rightVsFaithful=" + std::to_wstring(rightVsFaithful) +
						L", rightVsSsim=" + std::to_wstring(rightVsSsim)).c_str());
			}

			// (b) MANDATORY IN-TEST NEGATIVE CONTROL — without skeleton memo (!EdgeMapStatus().empty()):
			//     private edge map IS computed (+1 Measure() dispatch), matches memo-served output bit-for-bit,
			//     and sharp edges vs sub-threshold texture produce distinct blend weights (maxEdgeDiff > 0.02).
			Assert::IsFalse(Vpv::Skeleton::CurrentFrameMemo().Valid(),
				L"negative control precondition: skeleton memo must be inactive after RAII scope exit");
			HookDispatchRecord noMemoMeasureRec, noMemoBlendRec;
			std::vector<float> noMemoBlendedPx;
			{
				ScopedModule noMemoMod;
				noMemoMod->SetCutSource(noCutSource);
				noMemoBlendedPx = renderDownscale(&(*noMemoMod), st, 1, &noMemoMeasureRec, &noMemoBlendRec);
				Assert::IsTrue(noMemoMod->Active() && noMemoMod->Failures() == 0,
					L"no-memo negative control frame must succeed with zero failures");
				Assert::IsFalse(noMemoMod->EdgeMapStatus().empty(),
					L"negative control (no skeleton memo): EdgeMapStatus() must be non-empty");
				Assert::IsTrue(noMemoMod->EdgeMapStatus().find("no skeleton memo") != std::string::npos,
					(L"negative control (no skeleton memo): unexpected EdgeMapStatus: " +
						ExtendedRangeGpuProbe::ToWide(noMemoMod->EdgeMapStatus())).c_str());
				Assert::IsTrue(noMemoMeasureRec.HasPassContaining("VP-V edge map"),
					L"negative control (no skeleton memo): private DispatchEdgeMapStage pass must dispatch");
				Assert::AreEqual(memoMeasureRec.count + 1, noMemoMeasureRec.count,
					(L"no-memo Measure() must dispatch exactly 1 more pass (private edge map) than memo-served (memo=" +
						std::to_wstring(memoMeasureRec.count) + L", noMemo=" + std::to_wstring(noMemoMeasureRec.count) + L")").c_str());
			}

			// Real memo-served kEdgeMap output vs private no-memo edge-map output must be bit-identical (<= 1e-6f).
			const float parityDiff = maxRgbAbsDiff(memoBlendedPx, noMemoBlendedPx);
			Assert::IsTrue(parityDiff <= 1e-6f,
				(L"memo-served kEdgeMap and private no-memo blended output must be bit-identical (<= 1e-6), got " +
					std::to_wstring(parityDiff)).c_str());

			// Sharp stripe edges vs sub-threshold period-3 texture must produce distinct blend weights on the non-memo path.
			float maxEdgeDiff = 0.0f, maxEdgeVsFaithful = 0.0f;
			float maxTexVsSsim = 0.0f, maxTexVsFaithful = 0.0f;
			for (int y = 8; y < dh - 8; ++y)
			{
				for (int x = 4; x < dw / 2 - 4; ++x)
				{
					const int sx = static_cast<int>((x + 0.5) * 1.5);
					if (sx % 12 == 0 || sx % 12 == 11)
					{
						const size_t i = (static_cast<size_t>(y) * dw + x) * 4 + 1;
						maxEdgeDiff = (std::max)(maxEdgeDiff, std::fabs(noMemoBlendedPx[i] - ssimOnlyPx[i]));
						maxEdgeVsFaithful = (std::max)(maxEdgeVsFaithful, std::fabs(noMemoBlendedPx[i] - faithfulOnlyPx[i]));
					}
				}
				for (int x = dw / 2 + 6; x < dw - 4; ++x)
				{
					const size_t i = (static_cast<size_t>(y) * dw + x) * 4 + 1;
					maxTexVsSsim = (std::max)(maxTexVsSsim, std::fabs(noMemoBlendedPx[i] - ssimOnlyPx[i]));
					maxTexVsFaithful = (std::max)(maxTexVsFaithful, std::fabs(noMemoBlendedPx[i] - faithfulOnlyPx[i]));
				}
			}
			Assert::IsTrue(maxEdgeDiff > 0.02f && maxEdgeVsFaithful <= 0.02f,
				(L"negative control (no memo): sharp edges must select faithful over SSIM (maxEdgeDiff=" +
					std::to_wstring(maxEdgeDiff) + L", maxEdgeVsFaithful=" + std::to_wstring(maxEdgeVsFaithful) + L")").c_str());
			Assert::IsTrue(maxTexVsSsim <= 0.005f && maxTexVsFaithful > 0.02f,
				(L"negative control (no memo): sub-threshold texture must select SSIM over faithful (maxTexVsSsim=" +
					std::to_wstring(maxTexVsSsim) + L", maxTexVsFaithful=" + std::to_wstring(maxTexVsFaithful) + L")").c_str());

			srcTex.Reset();
			ssim.Reset();
			logGuard.Release();
			AssertCleanLibplaceboLog(log);
			Assert::AreEqual(0, pl_renderer_get_errors(fixture.Renderer()).num_disabled_hooks, L"A hook was disabled");
		}
	};
}
