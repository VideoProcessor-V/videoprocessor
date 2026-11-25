// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

// repair.grain_split: the kP10gGrainLevel OWNER FILL (GrainLevelGpu.h, P10g T3) on the CPU tier, no GPU object touched (textures
// are plain structs, the device is a test double that makes no texture):
//   1. the reserve is DERIVED from kP10gGrainLevel's declared size rule (one RGBA32F texel per 16-px block) at the source luma size;
//   2. the fill is Derived, frame-scoped, default variant; the module registers BOTH fills (the P10 variant owner once, P10g at the
//      largest live size) under the catalog's owning unit, re-registers P10g only on a size change, never the P10 owner twice;
//   3. the visible declines: no GPU context, no source plane (P13), the P10 split unavailable; each is counted for the status line;
//   4. the Fallbacks texts (the route that reaches Ctrl+I, Config status, Ctrl+Alt+D) say the single-frame law is in use, name the
//      declines with the last reason while the most recent fill declined (released after a successful fill), and show a missing
//      registration (again when a forgotten host's size returns); an AlreadyRegistered P10 owner (15b A1) is benign.
// The GPU parity test (fill vs GrainSplit's CPU blocks and the f^2 block mean) is part of the T5 GPU run (not run here: CODE ONLY).

#include <VP-V/core/GpuAllocator.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/modules/input/capture/SourcePlanes.h>
#include <VP-V/modules/repair/grain_split/GrainLevelGpu.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <VP-V/modules/control/skeleton/GpuProductTexBackend.h>
#include <VP-V/modules/repair/grain_split/GrainLevel.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	namespace
	{
		// Forwards the module's registrations to one real memo (the catalog's ownership rules apply) and counts them.
		struct GrainMemoDemand : Vpv::Skeleton::IExternalDemand
		{
			VpSkeleton::SkeletonMemo* memo = nullptr;
			int registers = 0;
			int unregisters = 0;
			int splitRegisters = 0;
			bool refuseSplit = false;   // answer the P10 owner NotOwner without forwarding (a refused split owner)
			VpSkeleton::OwnerFill lastLevelFill;   // a copy of the last P10g fill registered (the module's counters ride in it)
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				++registers;
				if (id == VpSkeleton::kP10gGrainLevel)
					lastLevelFill = fill;
				if (id == VpSkeleton::kP10GrainSplit)
				{
					++splitRegisters;
					if (refuseSplit)
						return VpSkeleton::SkeletonMemo::Registration::NotOwner;
				}
				return memo->RegisterOwnerFill(id, form, unit, fill);
			}
			bool UnregisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form, const std::string& unit) override
			{
				++unregisters;
				return memo->UnregisterOwnerFill(id, form, unit);
			}
		};

		// Non-null opaque handles (the fill only tests them before acquiring its inputs); makes no texture.
		struct GrainRefusingDevice : VpSkeleton::IOwnerGpuDevice
		{
			int gpuTag = 0;
			int dispatchTag = 0;
			int asked = 0;
			void* OwnerGpu() const override { return const_cast<int*>(&gpuTag); }
			void* OwnerDispatch() const override { return const_cast<int*>(&dispatchTag); }
			bool CreateOwnerTexture(const VpSkeleton::TexShape&, VpSkeleton::GpuMemoHandle&, std::string& why) override
			{
				++asked;
				why = "test device makes no texture";
				return false;
			}
		};

		VpSkeleton::Dims GrainDims(uint32_t w, uint32_t h)
		{
			VpSkeleton::Dims d;
			d.width = w;
			d.height = h;
			return d;
		}

		// CT-H7: WARP (or VP_TEST_GPU=hardware) libplacebo D3D11 device + host-readable owner GPU device + memo rig
		// that compiles and dispatches the LIVE kP10gGrainLevel GPU compute shader through GrainLevelModule.
		struct GrainLevelGpuDevice
		{
			pl_log log = nullptr;
			pl_d3d11 d3d11 = nullptr;
			std::string name;
			~GrainLevelGpuDevice()
			{
				if (d3d11) pl_d3d11_destroy(&d3d11);
				if (log) pl_log_destroy(&log);
			}
			GrainLevelGpuDevice() = default;
			GrainLevelGpuDevice(const GrainLevelGpuDevice&) = delete;
			GrainLevelGpuDevice& operator=(const GrainLevelGpuDevice&) = delete;
			bool Create()
			{
				pl_log_params lp{};
				lp.log_level = PL_LOG_ERR;
				lp.log_cb = [](void*, enum pl_log_level, const char* msg) {
					Logger::WriteMessage((std::string("libplacebo: ") + msg + "\n").c_str());
				};
				log = pl_log_create(PL_API_VER, &lp);
				if (!log)
					return false;
				const pl_d3d11_params params = ExtendedRangeGpuProbe::TestDeviceParams(&name);
				Logger::WriteMessage(("grain level GPU tests on: " + name + "\n").c_str());
				d3d11 = pl_d3d11_create(log, &params);
				return d3d11 != nullptr && d3d11->gpu != nullptr;
			}
			pl_gpu Gpu() const { return d3d11 ? d3d11->gpu : nullptr; }
		};

		// OwnerGpuDevice twin that marks created textures host-readable (for pl_tex_download parity verification)
		// and exposes OwnerMaxSharedMemory() required by GrainSplit::Gpu::Splitter::Compute(OwnerGpuContext&, ...).
		struct ReadableOwnerDevice : VpSkeleton::IOwnerGpuDevice
		{
			explicit ReadableOwnerDevice(pl_gpu g) : gpu(g), backend(g, true), dispatch(pl_dispatch_create(g->log, g)) {}
			~ReadableOwnerDevice() override
			{
				for (pl_tex& t : made)
					pl_tex_destroy(gpu, &t);
				if (dispatch != nullptr)
					pl_dispatch_destroy(&dispatch);
			}
			ReadableOwnerDevice(const ReadableOwnerDevice&) = delete;
			ReadableOwnerDevice& operator=(const ReadableOwnerDevice&) = delete;

			void* OwnerGpu() const override { return const_cast<void*>(static_cast<const void*>(gpu)); }
			void* OwnerDispatch() const override { return static_cast<void*>(dispatch); }
			uint64_t OwnerMaxSharedMemory() const override { return gpu != nullptr ? uint64_t(gpu->glsl.max_shmem_size) : 0u; }
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

		struct PlTexGuard
		{
			pl_gpu gpu = nullptr;
			pl_tex tex = nullptr;
			PlTexGuard(pl_gpu g, pl_tex t) : gpu(g), tex(t) {}
			~PlTexGuard()
			{
				if (gpu && tex)
					pl_tex_destroy(gpu, &tex);
			}
			PlTexGuard(const PlTexGuard&) = delete;
			PlTexGuard& operator=(const PlTexGuard&) = delete;
		};

		// Production-path memo rig: P13 SourcePlanes + GrainLevelModule (which registers both kP10GrainSplit VariantOwner
		// and kP10gGrainLevel MakeOwnerFill) backed by ReadableOwnerDevice. Destruction order releases memo spots and
		// SourcePlanes offers before ReadableOwnerDevice destroys owner textures and dispatch.
		struct GrainLevelGpuRig
		{
			pl_gpu gpu = nullptr;
			ReadableOwnerDevice device;
			VpSkeleton::SkeletonMemo memo;
			GrainMemoDemand demand;
			GrainSplit::LevelGpu::GrainLevelModule module;

			explicit GrainLevelGpuRig(pl_gpu g) : gpu(g), device(g)
			{
				Assert::IsNotNull(device.dispatch, L"ReadableOwnerDevice pl_dispatch created");
				demand.memo = &memo;
				memo.SetHost(this);
				memo.SetRenderThread(std::this_thread::get_id());
				memo.SetGpuDispatcherPresent(true);
				memo.SetGpuBudget(true, uint64_t(1) << 30);
				memo.SetGpuDevice(&device);
				Assert::IsTrue(memo.RegisterOwnerFill(VpSkeleton::kP13SourceRing, VpSkeleton::AcquireForm::Gpu,
					Vpv::Capture::SourcePlanes::kUnit, Vpv::Capture::SourcePlanes::MakeOwnerFill())
					== VpSkeleton::SkeletonMemo::Registration::Registered, L"P13 source-plane owner registered");
			}
			~GrainLevelGpuRig()
			{
				memo.ReleaseGpuSpots();
				Vpv::Capture::SourcePlanes::Forget(this);
			}
			GrainLevelGpuRig(const GrainLevelGpuRig&) = delete;
			GrainLevelGpuRig& operator=(const GrainLevelGpuRig&) = delete;

			void Observe(int w, int h)
			{
				Assert::IsTrue(module.Observe(this, GrainDims(static_cast<uint32_t>(w), static_cast<uint32_t>(h)), &demand),
					L"GrainLevelModule registers both P10 and P10g owner fills for the frame size");
			}

			void OfferLuma(uint64_t seq, pl_tex tex, int sampleDepth, int colorDepth, int bitShift)
			{
				Vpv::Capture::SourcePlaneSet set;
				set.seq = seq;
				set.planes = 1;
				set.tex[0] = tex;
				set.components[0] = 1;
				set.repr.sys = PL_COLOR_SYSTEM_BT_709;
				set.repr.levels = PL_COLOR_LEVELS_LIMITED;
				set.repr.bits.sample_depth = sampleDepth;
				set.repr.bits.color_depth = colorDepth;
				set.repr.bits.bit_shift = bitShift;
				set.origin = "grain level GPU parity test";
				Assert::IsTrue(Vpv::Capture::SourcePlanes::Offer(this, set), L"luma plane offered to P13");
			}

			void EndFrame(uint64_t seq)
			{
				memo.OnSeal(seq);
				Vpv::Capture::SourcePlanes::BeginFrame(this);
			}
		};

		constexpr double kGrainTestPi = 3.14159265358979323846;

		// Independent synthetic 3-zone luma scene in exact integer codes for `bits` (8, 10, or 12):
		//   left third:   flat pedestal (pure grain when sigma > 0; exact zero D when sigma == 0),
		//   middle third: high-frequency 2D sine texture (high Wiener share f -> high retained grain share),
		//   right third:  sharp step rectangle + diagonal tanh edge (populates multiple luma bins for NLF cap).
		std::vector<uint16_t> MakeSyntheticCodes(int w, int h, int bits, double sigma, uint32_t seed)
		{
			const double scale = GrainSplit::DepthScale(bits);
			// At 8-bit, sigma=1.5 codes is 6 ten-bit-equiv codes; lift the sine amplitude slightly so texture energy
			// stays well above 2*N_b while remaining safely inside [0, 255] without clipping.
			const double texScale = bits == 8 ? 0.55 : scale;
			const double maxCode = static_cast<double>((1 << bits) - 1);
			std::vector<uint16_t> codes(static_cast<size_t>(w) * static_cast<size_t>(h));
			std::mt19937 rng(seed);
			std::normal_distribution<double> noise(0.0, sigma > 0.0 ? sigma : 1.0);
			for (int y = 0; y < h; ++y)
				for (int x = 0; x < w; ++x)
				{
					double v = 0.0;
					if (x < w / 3)
					{
						v = 320.0 * scale;
					}
					else if (x < 2 * w / 3)
					{
						v = 360.0 * scale + texScale * (18.0 * std::sin(2.0 * kGrainTestPi * x / 5.0) * std::cos(2.0 * kGrainTestPi * y / 6.5)
							+ 12.0 * std::sin(2.0 * kGrainTestPi * (x + y) / 9.0));
					}
					else
					{
						const double rx0 = 0.72 * w, rx1 = 0.92 * w, ry0 = 0.2 * h, ry1 = 0.8 * h;
						const double inX = 0.5 * (std::tanh((x - rx0) / 0.7) - std::tanh((x - rx1) / 0.7));
						const double inY = 0.5 * (std::tanh((y - ry0) / 0.7) - std::tanh((y - ry1) / 0.7));
						v = (300.0 + 240.0 * inX * inY + 80.0 * std::tanh((x * 0.6 + y * 0.8 - 0.70 * w) / 0.9)) * scale;
					}
					if (sigma > 0.0)
						v += noise(rng);
					const long q = std::lround((std::min)(maxCode, (std::max)(0.0, v)));
					codes[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = static_cast<uint16_t>(q);
				}
			return codes;
		}

		GrainSplit::Plane CpuPlaneFromCodes(const std::vector<uint16_t>& codes, int w, int h)
		{
			GrainSplit::Plane p(w, h);
			for (size_t i = 0; i < codes.size(); ++i)
				p.v[i] = static_cast<double>(codes[i]);
			return p;
		}

		pl_tex UploadLumaPlane(pl_gpu gpu, const std::vector<uint16_t>& codes, int w, int h, int bits)
		{
			if (bits == 8)
			{
				std::vector<uint8_t> data(codes.size());
				for (size_t i = 0; i < codes.size(); ++i)
					data[i] = static_cast<uint8_t>(codes[i] & 0xFFu);
				pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 8, 8, PL_FMT_CAP_SAMPLEABLE);
				Assert::IsNotNull(fmt, L"no sampleable R8 UNORM format");
				pl_tex_params p{};
				p.w = w;
				p.h = h;
				p.format = fmt;
				p.sampleable = true;
				p.initial_data = data.data();
				pl_tex tex = pl_tex_create(gpu, &p);
				Assert::IsNotNull(tex, L"cannot create 8-bit luma texture");
				return tex;
			}
			const int bitShift = 16 - bits;
			std::vector<uint16_t> data(codes.size());
			for (size_t i = 0; i < codes.size(); ++i)
				data[i] = static_cast<uint16_t>(codes[i] << bitShift);
			pl_fmt fmt = pl_find_fmt(gpu, PL_FMT_UNORM, 1, 16, 16, PL_FMT_CAP_SAMPLEABLE);
			Assert::IsNotNull(fmt, L"no sampleable R16 UNORM format");
			pl_tex_params p{};
			p.w = w;
			p.h = h;
			p.format = fmt;
			p.sampleable = true;
			p.initial_data = data.data();
			pl_tex tex = pl_tex_create(gpu, &p);
			Assert::IsNotNull(tex, L"cannot create 16-bit luma texture");
			return tex;
		}

		std::vector<float> DownloadGrainLevel(pl_gpu gpu, pl_tex tex)
		{
			Assert::IsNotNull(tex, L"grain level output texture");
			pl_gpu_finish(gpu);
			std::vector<float> out(static_cast<size_t>(tex->params.w) * static_cast<size_t>(tex->params.h) * 4u, 0.0f);
			pl_tex_transfer_params tr{};
			tr.tex = tex;
			tr.ptr = out.data();
			Assert::IsTrue(pl_tex_download(gpu, &tr), L"grain level RGBA32F output texture must be host-readable");
			return out;
		}

		// CPU single-frame reference: GrainSplit::Compute(sourceBits=bits) -> GrainLevel::Estimator::Step(engineOn=false).
		GrainLevel::Result ComputeCpuGrainLevelRef(const std::vector<uint16_t>& codes, int w, int h, int bits, int& outBw, int& outBh)
		{
			GrainSplit::Params sp = GrainSplit::DefaultParams();
			sp.sourceBits = bits;
			const GrainSplit::Split split = GrainSplit::Compute(CpuPlaneFromCodes(codes, w, h), 1.0, 16, sp);
			outBw = split.bw;
			outBh = split.bh;
			GrainLevel::Params lp;
			lp.split = sp;
			GrainLevel::Estimator est(lp);
			GrainLevel::Context ctx;
			ctx.engineOn = false;
			const GrainLevel::Result r = est.Step(split, ctx);
			Assert::IsTrue(r.mode == GrainLevel::Mode::SingleFrame, L"CPU GrainLevel reference runs single-frame mode");
			return r;
		}

		// Parity bounds derived from GrainSplitV3GpuTests (|dN|/max(N, 1e-2*es) <= 1e-3, |df| <= 1/255):
		// |d(mf2*N)| <= mf2*|dN| + 2*|df|*N <= (1e-3 + 2/255)*max(N, 1e-2*es) < 1e-2*max(N, 1e-2*es).
		void AssertGrainLevelParity(const std::vector<float>& gpuRgba, const GrainLevel::Result& cpuRef,
			int bw, int bh, int bits, const char* label)
		{
			const size_t nBlocks = static_cast<size_t>(bw) * static_cast<size_t>(bh);
			Assert::AreEqual(nBlocks * 4u, gpuRgba.size(), L"RGBA32F block count");
			Assert::AreEqual(nBlocks, cpuRef.Nb.size(), L"CPU Nb block count");
			Assert::AreEqual(nBlocks, cpuRef.retained.size(), L"CPU retained block count");
			const double cs = GrainSplit::DepthScale(bits);
			const double floorE = 1e-2 * cs * cs;
			double worstN = 0.0, worstRet = 0.0;
			int badG = 0, badRho = 0;
			for (size_t i = 0; i < nBlocks; ++i)
			{
				const float gotN = gpuRgba[i * 4u + 0u];
				const float gotG = gpuRgba[i * 4u + 1u];
				const float gotRet = gpuRgba[i * 4u + 2u];
				const float gotRho = gpuRgba[i * 4u + 3u];
				const double denom = (std::max)(cpuRef.Nb[i], floorE);
				worstN = (std::max)(worstN, std::fabs(static_cast<double>(gotN) - cpuRef.Nb[i]) / denom);
				worstRet = (std::max)(worstRet, std::fabs(static_cast<double>(gotRet) - cpuRef.retained[i]) / denom);
				if (gotG != 0.0f) ++badG;
				if (gotRho != -1.0f) ++badRho;
			}
			char line[256];
			std::snprintf(line, sizeof line, "GRAIN_LEVEL_GPU_PARITY %s (%d-bit %dx%d blocks): worstN=%.3e worstRet=%.3e badG=%d badRho=%d\n",
				label, bits, bw, bh, worstN, worstRet, badG, badRho);
			Logger::WriteMessage(line);
			const std::string tagStr = std::string(label) + ": ";
			const std::wstring tag(tagStr.begin(), tagStr.end());
			Assert::IsTrue(worstN <= 1e-3, (tag + L"N_b (channel x) relative error must be <= 1e-3 vs CPU GrainLevel reference").c_str());
			Assert::AreEqual(0, badG, (tag + L"coherence gate g (channel y) must be exactly 0.0 in single-frame law").c_str());
			Assert::IsTrue(worstRet <= 1e-2, (tag + L"retained grain share f^2*N_b (channel z) relative error must be <= 1e-2 vs CPU GrainLevel reference").c_str());
			Assert::AreEqual(0, badRho, (tag + L"lag-1 correlation rho1 (channel w) must be exactly -1.0 when unmeasured").c_str());
		}
	}

	TEST_CLASS(GrainLevelFillTests)
	{
	public:
		TEST_METHOD(ReserveIsTheDeclaredBlockGridAtTheSourceLumaSize)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			const Dims sizes[3] = { GrainDims(3840, 2160), GrainDims(1920, 1080), GrainDims(1919, 1081) };
			for (const Dims& d : sizes)
			{
				TexShape s;
				s.w = (d.width + 15u) / 16u;
				s.h = (d.height + 15u) / 16u;
				s.fmt = TexFormat::RGBA32F;
				Assert::AreEqual(uint64_t(s.w) * s.h * 16u, L::ReserveBytes(d), L"one RGBA32F texel per 16-px block (ceil)");
				Assert::AreEqual(ShapeBytes(s), L::ReserveBytes(d), L"the reserve equals the texture the fill makes");
			}
			Assert::AreEqual(uint64_t(240) * 135u * 16u, L::ReserveBytes(GrainDims(3840, 2160)), L"518,400 B at 4K");
			Assert::AreEqual(uint64_t(0), L::ReserveBytes(GrainDims(0, 0)), L"no size known: no reserve");
			Assert::AreEqual(uint64_t(0), L::ReserveBytes(GrainDims(1920, 0)), L"an empty plane: no reserve");
		}

		TEST_METHOD(FillIsDerivedFrameScopedDefaultVariantUnderTheCatalogOwner)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(1920, 1080)), std::make_shared<L::Counters>());
			Assert::AreEqual(uint32_t(0), f.variantAxes, L"default variant only");
			Assert::IsTrue(f.frameScoped, L"a per-frame memo");
			Assert::IsTrue(f.jobClass == JobClass::Derived, L"Derived (LaneSpareWork StaticClassOf), never a job");
			Assert::AreEqual(L::ReserveBytes(GrainDims(1920, 1080)), f.reserveBytes, L"the reserve given");
			Assert::AreEqual(std::string(L::kOwnerUnit), SkeletonMemo::OwnerUnitOf(kP10gGrainLevel), L"the catalog's owner unit");
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, L::kOwnerUnit, f) == SkeletonMemo::Registration::Registered,
				L"registered by its unit");
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, "input/source_analysis/regions", f)
				== SkeletonMemo::Registration::NotOwner, L"another unit is refused");
		}

		TEST_METHOD(ModuleRegistersBothFillsAndTracksTheLargestLiveSize)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int hostA = 0, hostB = 0;

			Assert::IsFalse(module.Observe(&hostA, GrainDims(0, 0), &demand), L"no size: nothing registered");
			Assert::AreEqual(0, demand.registers, L"no registration without a size");
			Assert::IsFalse(module.SplitOwnerRegistered(), L"nor the P10 owner");

			Assert::IsTrue(module.Observe(&hostA, GrainDims(1920, 1080), &demand), L"registered on the first size");
			Assert::AreEqual(2, demand.registers, L"the P10 variant owner and the P10g fill");
			Assert::AreEqual(1, demand.splitRegisters, L"the P10 owner once");
			Assert::IsTrue(module.SplitOwnerRegistered(), L"P10 variant owner registered");
			Assert::IsTrue(m.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"the memo holds the P10 fill");
			Assert::IsTrue(m.HasOwnerFill(kP10gGrainLevel, AcquireForm::Gpu), L"the memo holds the P10g fill");
			Assert::AreEqual(L::ReserveBytes(GrainDims(1920, 1080)), module.RegisteredReserveBytes(), L"1080p reserve");
			Assert::AreEqual(L::SplitReserveBytes(GrainDims(1920, 1080)), module.SplitRegisteredReserveBytes(), L"1080p P10 reserve");
			Assert::IsFalse(module.SplitOwnerServedElsewhere(), L"the P10 owner is ours here");
			Assert::IsTrue(module.FallbackLines().empty(), L"both registered, no fill yet: no Fallbacks line");

			Assert::IsTrue(module.Observe(&hostA, GrainDims(1920, 1080), &demand), L"same size");
			Assert::AreEqual(2, demand.registers, L"same size: no re-registration");

			Assert::IsTrue(module.Observe(&hostB, GrainDims(3840, 2160), &demand), L"a larger host");
			Assert::AreEqual(2, demand.unregisters, L"old P10 (via default ReplaceOwnerFill) and old P10g fills removed");
			Assert::AreEqual(4, demand.registers, L"P10 replaced and P10g re-registered once for 4K");
			Assert::AreEqual(2, demand.splitRegisters, L"S2-M4: P10 owner reserve updated on upward 1080p -> 4K growth");
			Assert::AreEqual(L::ReserveBytes(GrainDims(3840, 2160)), module.RegisteredReserveBytes(), L"the largest live size");
			Assert::AreEqual(L::SplitReserveBytes(GrainDims(3840, 2160)), module.SplitRegisteredReserveBytes(), L"4K P10 reserve");
			Assert::IsTrue(module.SplitRegisteredReserveBytes() > L::SplitReserveBytes(GrainDims(1920, 1080)),
				L"4K P10 reserve strictly exceeds 1080p P10 reserve");

			module.Forget(&hostB, &demand);
			Assert::AreEqual(3, demand.unregisters, L"only P10g unregistered while hostA remains live");
			Assert::AreEqual(5, demand.registers, L"the largest host left: P10g re-registered for hostA");
			Assert::AreEqual(2, demand.splitRegisters, L"P10 high-water reserve kept while a live host remains");
			Assert::AreEqual(L::ReserveBytes(GrainDims(1920, 1080)), module.RegisteredReserveBytes(), L"the remaining host's size");
			Assert::AreEqual(L::SplitReserveBytes(GrainDims(3840, 2160)), module.SplitRegisteredReserveBytes(),
				L"P10 reserve stays at 4K until all hosts detach");
		}

		// S2-M4: GrainLevelModule must grow kP10GrainSplit's ahead-planner reserve atomically via ReplaceOwnerFill
		// when source resolution grows (e.g. 720p -> 4K, avoiding the ~9x reserve underestimate hazard), preserve the
		// active registration and report visibly on FallbackLines()/OnStatus(Fallbacks) if ReplaceOwnerFill fails
		// (Rule 4), and re-baseline cleanly to a smaller resolution after all hosts detach (Forget) while treating
		// a same-resolution return as a no-op.
		BEGIN_TEST_METHOD_ATTRIBUTE(SplitOwnerReserveGrowsAtomicallyFrom720pTo4KAndRebaselinesAfterDetach)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(SplitOwnerReserveGrowsAtomicallyFrom720pTo4KAndRebaselinesAfterDetach)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			namespace RE = RendererExtension;

			struct AtomicSplitDemand : Vpv::Skeleton::IExternalDemand
			{
				SkeletonMemo* memo = nullptr;
				int splitRegisters = 0;
				int splitReplaces = 0;
				int splitUnregisters = 0;
				bool refuseSplitReplace = false;
				SkeletonMemo::Registration refuseCode = SkeletonMemo::Registration::BadDefaults;
				uint64_t lastSplitReserveBytes = 0;

				void SetExternalDemand(const void*, const void*, const std::vector<ConsumerDecl>&,
					const std::vector<ProductId>&) override {}
				void ClearExternalDemand(const void*, const void*) override {}

				SkeletonMemo::Registration RegisterOwnerFill(ProductId id, AcquireForm form,
					const std::string& unit, OwnerFill fill) override
				{
					if (id == kP10GrainSplit)
					{
						++splitRegisters;
						lastSplitReserveBytes = fill.reserveBytes;
					}
					return memo->RegisterOwnerFill(id, form, unit, std::move(fill));
				}

				SkeletonMemo::Registration ReplaceOwnerFill(ProductId id, AcquireForm form,
					const std::string& unit, OwnerFill fill) override
				{
					if (id == kP10GrainSplit)
					{
						++splitReplaces;
						lastSplitReserveBytes = fill.reserveBytes;
						if (refuseSplitReplace)
							return refuseCode;
					}
					return memo->ReplaceOwnerFill(id, form, unit, std::move(fill));
				}

				bool UnregisterOwnerFill(ProductId id, AcquireForm form, const std::string& unit) override
				{
					if (id == kP10GrainSplit)
						++splitUnregisters;
					return memo->UnregisterOwnerFill(id, form, unit);
				}
			};

			auto memoSplitReserve = [](const SkeletonMemo& m) -> uint64_t {
				const AheadNeed need = m.MakeAheadNeed(QueueWindow{});
				for (const AheadCost& c : need.costs)
					if (c.id == uint16_t(kP10GrainSplit) && c.form == uint8_t(AcquireForm::Gpu))
						return c.bytesPerFrame;
				return uint64_t(0);
			};

			const Dims d720 = GrainDims(1280, 720);
			const Dims d1080 = GrainDims(1920, 1080);
			const Dims d4k = GrainDims(3840, 2160);
			const uint64_t r720 = L::SplitReserveBytes(d720);
			const uint64_t r1080 = L::SplitReserveBytes(d1080);
			const uint64_t r4k = L::SplitReserveBytes(d4k);

			Assert::IsTrue(r720 > uint64_t(0), L"720p split reserve is positive");
			Assert::IsTrue(r1080 > r720 && r4k > r1080, L"split reserve grows strictly with luma dimensions");

			// Same-input negative control (Rule 3): prove the pre-fix S2-M4 ~9x reserve-underestimate hazard is real.
			// Leaving kP10GrainSplit permanently registered at 1280x720 when the stream grows to 3840x2160 understates
			// SkeletonMemo's ahead-planner bytesPerFrame by more than 8x (~9x).
			GrainSplit::Gpu::VariantOwner baselineOwner;
			SkeletonMemo baselineMemo;
			Assert::IsTrue(baselineOwner.Register(baselineMemo, 1280, 720) == SkeletonMemo::Registration::Registered,
				L"negative control: baseline 720p P10 owner registers");
			const uint64_t baselineReserve = memoSplitReserve(baselineMemo);
			Assert::AreEqual(r720, baselineReserve, L"negative control: baseline memo carries 720p ahead-planner reserve");
			Assert::IsTrue(r720 * uint64_t(8) < r4k,
				L"negative control: SplitReserveBytes(1280x720) * 8 < SplitReserveBytes(3840x2160) (~9x underestimate hazard)");
			Assert::IsTrue(baselineReserve * uint64_t(8) < r4k,
				L"negative control: pre-fix 720p-pinned memo reserve violates 4K ahead-planner reserve by >8x");

			SkeletonMemo memo;
			AtomicSplitDemand demand;
			demand.memo = &memo;
			L::GrainLevelModule module;
			int host = 0;
			RE::Context ctx{};
			ctx.host = &host;

			// 1. Initial registration at 1280x720.
			Assert::IsTrue(module.Observe(&host, d720, &demand), L"initial 720p registration succeeds");
			Assert::IsTrue(module.SplitOwnerRegistered(), L"P10 owner registered at 720p");
			Assert::AreEqual(r720, module.SplitRegisteredReserveBytes(), L"SplitRegisteredReserveBytes matches 720p");
			Assert::IsTrue(memo.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"memo holds kP10GrainSplit fill at 720p");
			Assert::AreEqual(r720, memoSplitReserve(memo), L"memo ahead-planner reserve matches 720p");
			Assert::AreEqual(1, demand.splitRegisters, L"initial P10 registration calls RegisterOwnerFill once");
			Assert::AreEqual(0, demand.splitReplaces, L"initial P10 registration does not call ReplaceOwnerFill");
			Assert::AreEqual(0, demand.splitUnregisters, L"initial P10 registration never unregisters");
			Assert::IsTrue(module.FallbackLines().empty(), L"healthy 720p registration emits no fallback line");

			// Same-input negative control (Rule 3): naive RegisterOwnerFill on the active memo fails with AlreadyRegistered
			// and leaves the ahead-planner reserve stuck at 720p (proving atomic ReplaceOwnerFill is required).
			Assert::IsTrue(baselineOwner.Register(memo, 3840, 2160) == SkeletonMemo::Registration::AlreadyRegistered,
				L"negative control: real memo refuses second RegisterOwnerFill with AlreadyRegistered");
			Assert::AreEqual(r720, memoSplitReserve(memo), L"negative control: refused RegisterOwnerFill leaves 720p reserve");

			// 2. Failed ReplaceOwnerFill on upward growth 720p -> 4K must keep SplitOwnerRegistered() == true and
			//    the prior valid 720p reserve intact (both in module and memo), while visibly reporting on FallbackLines()
			//    and OnStatus(Fallbacks) (Rule 4) and latching against per-frame retries.
			demand.refuseSplitReplace = true;
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"P10g succeeds while P10 replace is refused");
			Assert::AreEqual(1, demand.splitRegisters, L"failed replace does not call RegisterOwnerFill");
			Assert::AreEqual(1, demand.splitReplaces, L"4K growth invokes ReplaceOwnerFill once");
			Assert::AreEqual(0, demand.splitUnregisters, L"failed replace never unregisters the prior P10 fill");
			Assert::AreEqual(r4k, demand.lastSplitReserveBytes, L"attempted replace carried 4K reserve");
			Assert::IsTrue(module.SplitOwnerRegistered(), L"SplitOwnerRegistered stays true after failed ReplaceOwnerFill");
			Assert::AreEqual(r720, module.SplitRegisteredReserveBytes(), L"prior valid 720p split reserve preserved on failure");
			Assert::IsTrue(memo.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"memo still holds prior 720p P10 fill");
			Assert::AreEqual(r720, memoSplitReserve(memo), L"memo ahead-planner reserve stays at prior 720p on failure");

			const std::string wantRefusal = std::string(L::kSplitUnregisteredId)
				+ ": P10 variant owner refused: bad defaults (reserve " + std::to_string(r4k)
				+ " B; retried at the next size change)";
			const std::vector<std::string> fbFail = module.FallbackLines();
			Assert::AreEqual(size_t(1), fbFail.size(), L"failed replace reports visibly on FallbackLines() (Rule 4)");
			Assert::AreEqual(wantRefusal, fbFail[0], L"failed replace line names refusal and requested 4K reserve");
			std::vector<std::string> statusLines;
			module.OnStatus(ctx, RE::StatusKind::Fallbacks, statusLines, nullptr, nullptr);
			Assert::AreEqual(size_t(1), statusLines.size(), L"OnStatus(Fallbacks) reports failed replace");
			Assert::AreEqual(wantRefusal, statusLines[0], L"OnStatus(Fallbacks) matches FallbackLines()");

			// Latch check: re-observing the same refused 4K size does not spam ReplaceOwnerFill every frame.
			demand.refuseSplitReplace = false;
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"latched 4K keeps P10g registered");
			Assert::AreEqual(1, demand.splitReplaces, L"refused 4K reserve is latched and not retried every frame");
			Assert::AreEqual(r720, module.SplitRegisteredReserveBytes(), L"reserve remains 720p while latched");
			Assert::AreEqual(size_t(1), module.FallbackLines().size(), L"failure line persists while latched at refused size");

			// Returning to the active 720p size clears splitProblem_ and resets splitTriedReserve_ without replacing.
			Assert::IsTrue(module.Observe(&host, d720, &demand), L"returning to active 720p succeeds");
			Assert::AreEqual(1, demand.splitReplaces, L"returning to active 720p needs no ReplaceOwnerFill");
			Assert::IsTrue(module.FallbackLines().empty(), L"returning to active 720p clears split failure line");

			// 3. Successful atomic growth from 1280x720 -> 3840x2160 (>8x reserve growth, zero unregister gap).
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"upward 720p -> 4K growth succeeds");
			Assert::AreEqual(1, demand.splitRegisters, L"growth never calls RegisterOwnerFill again");
			Assert::AreEqual(2, demand.splitReplaces, L"4K growth atomically invokes ReplaceOwnerFill");
			Assert::AreEqual(0, demand.splitUnregisters, L"atomic growth has zero unregister gap");
			Assert::IsTrue(module.SplitOwnerRegistered(), L"P10 owner remains registered at 4K");
			Assert::AreEqual(r4k, module.SplitRegisteredReserveBytes(), L"SplitRegisteredReserveBytes grows to 4K");
			Assert::IsTrue(module.SplitRegisteredReserveBytes() > r720 * uint64_t(8),
				L"4K P10 reserve exceeds 8x the 720p reserve (eliminates ~9x underestimate)");
			Assert::IsTrue(memo.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"memo holds kP10GrainSplit fill at 4K");
			Assert::AreEqual(r4k, memoSplitReserve(memo), L"memo ahead-planner reserve updated to 4K");
			Assert::IsTrue(module.FallbackLines().empty(), L"successful 4K growth leaves FallbackLines empty");

			// Same-size 4K repeat is a no-op.
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"same 4K size");
			Assert::AreEqual(2, demand.splitReplaces, L"steady 4K triggers no redundant ReplaceOwnerFill");

			// 4. Detach (want == 0) followed by returning at the EXACT same resolution (4K) is a no-op.
			module.Forget(&host, &demand);
			Assert::IsTrue(module.SplitOwnerRegistered(), L"P10 owner stays registered across detach");
			Assert::AreEqual(0, demand.splitUnregisters, L"detach does not unregister P10 owner");
			Assert::IsTrue(module.Observe(&host, d4k, &demand), L"returning at the same 4K resolution succeeds");
			Assert::AreEqual(2, demand.splitReplaces, L"returning at exact same resolution after detach is a no-op");
			Assert::AreEqual(r4k, module.SplitRegisteredReserveBytes(), L"same-resolution return keeps 4K reserve");
			Assert::AreEqual(r4k, memoSplitReserve(memo), L"memo ahead-planner reserve stays 4K on same-resolution return");

			// 5. Detach (want == 0) followed by a SMALLER returning resolution (1920x1080) re-baselines cleanly to 1080p.
			module.Forget(&host, &demand);
			Assert::IsTrue(module.Observe(&host, d1080, &demand), L"smaller 1080p session after detach succeeds");
			Assert::AreEqual(1, demand.splitRegisters, L"post-detach re-baseline uses ReplaceOwnerFill, not RegisterOwnerFill");
			Assert::AreEqual(3, demand.splitReplaces, L"post-detach smaller resolution atomically replaces P10 fill");
			Assert::AreEqual(0, demand.splitUnregisters, L"post-detach re-baseline has zero unregister gap");
			Assert::IsTrue(module.SplitOwnerRegistered(), L"P10 owner stays registered after re-baseline");
			Assert::AreEqual(r1080, module.SplitRegisteredReserveBytes(), L"SplitRegisteredReserveBytes re-baselines cleanly to 1080p");
			Assert::IsTrue(module.SplitRegisteredReserveBytes() < r4k, L"re-baselined 1080p reserve is strictly smaller than 4K");
			Assert::IsTrue(memo.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"memo holds re-baselined 1080p P10 fill");
			Assert::AreEqual(r1080, memoSplitReserve(memo), L"memo ahead-planner reserve re-baselines cleanly to 1080p");
			Assert::IsTrue(module.FallbackLines().empty(), L"re-baseline leaves FallbackLines empty");

			// Steady 1080p after re-baseline is a no-op.
			Assert::IsTrue(module.Observe(&host, d1080, &demand), L"steady 1080p after re-baseline");
			Assert::AreEqual(3, demand.splitReplaces, L"steady 1080p after re-baseline triggers no extra ReplaceOwnerFill");
		}

		TEST_METHOD(ModuleDoesNotRetryARefusedRegistrationEveryFrame)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, L::kOwnerUnit, L::MakeOwnerFill(64, std::make_shared<L::Counters>()))
				== SkeletonMemo::Registration::Registered, L"someone registered P10g first");
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int host = 0;
			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g refused: not registered");
			Assert::AreEqual(2, demand.registers, L"one P10 and one P10g attempt");
			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"the same size again");
			Assert::AreEqual(2, demand.registers, L"not retried every frame");
			Assert::AreEqual(uint64_t(0), module.RegisteredReserveBytes(), L"nothing of ours registered for P10g");
		}

		TEST_METHOD(ModuleShowsARefusedRegistrationOnTheFallbacksRoute)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			Assert::IsTrue(m.RegisterOwnerFill(kP10gGrainLevel, AcquireForm::Gpu, L::kOwnerUnit, L::MakeOwnerFill(64, std::make_shared<L::Counters>()))
				== SkeletonMemo::Registration::Registered, L"someone registered P10g first");
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int host = 0;
			Assert::IsFalse(module.Observe(&host, GrainDims(0, 0), &demand), L"no size yet");
			Assert::IsTrue(module.FallbackLines().empty(), L"no size seen: nothing owed, no line");

			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g refused");
			const std::vector<std::string> lines = module.FallbackLines();
			Assert::AreEqual(size_t(1), lines.size(), L"exactly the missing P10g registration (P10 is ours; no fill, no decline)");
			const std::string want = std::string(L::kUnregisteredId) + ": P10g owner fill refused: another fill is already registered (reserve "
				+ std::to_string(L::ReserveBytes(GrainDims(1920, 1080))) + " B; retried at the next size change)";
			Assert::AreEqual(want, lines[0], L"the refusal in words, on a Fallbacks line");

			module.Forget(&host, &demand);
			Assert::IsTrue(module.FallbackLines().empty(), L"the host left: no size, nothing owed");

			// The same size returns (GpuReleased, then the same source): still refused, so the line must be back (r2 MUST-1).
			Assert::AreEqual(2, demand.registers, L"before the size returns: one P10 and one P10g attempt");
			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"the same size returns: P10g still refused");
			Assert::AreEqual(3, demand.registers, L"the returned size retries the P10g registration once (the P10 owner is not retried)");
			Assert::AreEqual(1, demand.splitRegisters, L"the P10 owner is still registered once");
			const std::vector<std::string> again = module.FallbackLines();
			Assert::AreEqual(size_t(1), again.size(), L"the missing P10g registration is shown again");
			Assert::AreEqual(want, again[0], L"the same refusal line is back");
			Assert::IsFalse(module.Observe(&host, GrainDims(1920, 1080), &demand), L"the same size again");
			Assert::AreEqual(3, demand.registers, L"and not retried every frame after the return");
		}

		TEST_METHOD(ModuleShowsARefusedP10OwnerButNotAnAlreadyRegisteredOne)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			int host = 0;
			{
				SkeletonMemo m;
				GrainMemoDemand demand;
				demand.memo = &m;
				demand.refuseSplit = true;
				L::GrainLevelModule module;
				Assert::IsTrue(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g registered");
				Assert::IsFalse(module.SplitOwnerRegistered(), L"the P10 owner refused");
				Assert::IsFalse(module.SplitOwnerServedElsewhere(), L"a refusal is not 'served elsewhere'");
				const std::vector<std::string> lines = module.FallbackLines();
				Assert::AreEqual(size_t(1), lines.size(), L"exactly the refused P10 owner");
				Assert::AreEqual(std::string(L::kSplitUnregisteredId) + ": P10 variant owner refused: not the catalog's owning unit"
					" (P10 variant Acquires stay NoOwnerFill; not retried)", lines[0], L"the refusal in words");
				Assert::IsTrue(module.Observe(&host, GrainDims(3840, 2160), &demand), L"a size change");
				Assert::AreEqual(1, demand.splitRegisters, L"a refused P10 owner is not retried");
			}
			{
				// 15b A1 (or anyone) registered the P10 owner first: benign, nothing shown, never retried.
				SkeletonMemo m;
				GrainSplit::Gpu::VariantOwner a1;
				Assert::IsTrue(a1.Register(m) == SkeletonMemo::Registration::Registered, L"the other P10 owner first");
				GrainMemoDemand demand;
				demand.memo = &m;
				L::GrainLevelModule module;
				Assert::IsTrue(module.Observe(&host, GrainDims(1920, 1080), &demand), L"P10g still registered");
				Assert::AreEqual(1, demand.splitRegisters, L"one P10 attempt");
				Assert::IsFalse(module.SplitOwnerRegistered(), L"not ours");
				Assert::IsTrue(module.SplitOwnerServedElsewhere(), L"served by the other registration");
				Assert::IsTrue(m.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"P10 is served");
				Assert::IsTrue(module.FallbackLines().empty(), L"AlreadyRegistered is benign: no Fallbacks line");
				Assert::IsTrue(module.Observe(&host, GrainDims(3840, 2160), &demand), L"a size change");
				Assert::AreEqual(1, demand.splitRegisters, L"never retried");
			}
		}

		// Category Cpu: the name says GPU, the body is hardware-free (a refusing fake device, no pl_gpu).
		BEGIN_TEST_METHOD_ATTRIBUTE(FillDeclinesVisiblyWithoutAGpuContext)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(FillDeclinesVisiblyWithoutAGpuContext)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			const std::shared_ptr<L::Counters> c = std::make_shared<L::Counters>();
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(64, 48)), c);
			OwnerFillRequest req;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined");
			Assert::AreEqual(std::string("grain level: no GPU context"), out.declineReason, L"worded");
			Assert::IsFalse(out.gpu.Valid(), L"nothing made");
			Assert::AreEqual(uint64_t(1), c->declines.load(), L"counted for the status line");
			Assert::AreEqual(uint64_t(0), c->fills.load(), L"no fill");
			Assert::AreEqual(out.declineReason, c->LastDecline(), L"the last decline shown");
			Assert::IsTrue(c->lastDeclined.load(), L"the most recent fill declined: the declined line is engaged");
		}

		TEST_METHOD(FillDeclinesVisiblyWithoutASourcePlane)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			int host = 0;
			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(64, 48)), std::make_shared<L::Counters>());
			GrainRefusingDevice dev;
			OwnerGpuContext ctx(&dev, f.reserveBytes);
			OwnerFillRequest req;
			req.host = &host;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			req.gpu = &ctx;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"no P13 owner: declined");
			Assert::IsTrue(out.declineReason.find("grain level: no source plane for the frame (P13: ") == 0, L"worded with P13's cause");
			Assert::AreEqual(0, dev.asked, L"no texture asked for");
		}

		TEST_METHOD(FillDeclinesVisiblyWhenTheGrainSplitIsUnavailable)
		{
			using namespace VpSkeleton;
			using Vpv::Capture::SourcePlanes;
			namespace L = GrainSplit::LevelGpu;
			int host = 0;
			pl_tex_t luma{}, chroma{};
			luma.params.w = 64;
			luma.params.h = 48;
			chroma.params.w = 32;
			chroma.params.h = 24;
			pl_frame image{};
			image.num_planes = 2;
			image.planes[0].texture = &luma;
			image.planes[0].components = 1;
			image.planes[1].texture = &chroma;
			image.planes[1].components = 2;
			image.repr.sys = PL_COLOR_SYSTEM_BT_2020_NC;
			image.repr.levels = PL_COLOR_LEVELS_LIMITED;

			SkeletonMemo m;
			m.SetHost(&host);
			m.SetRenderThread(std::this_thread::get_id());
			Assert::IsTrue(m.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu, SourcePlanes::kUnit, SourcePlanes::MakeOwnerFill())
				== SkeletonMemo::Registration::Registered, L"P13 owner");
			Assert::IsTrue(SourcePlanes::OfferFrame(&host, 7, 1, image), L"the Entrance image offered");

			// No P10 variant owner registered: the nested Acquire is refused and P10g says so (never a private split).
			const std::shared_ptr<L::Counters> c = std::make_shared<L::Counters>();
			const OwnerFill f = L::MakeOwnerFill(L::ReserveBytes(GrainDims(64, 48)), c);
			GrainRefusingDevice dev;
			OwnerGpuContext ctx(&dev, f.reserveBytes);
			OwnerFillRequest req;
			req.host = &host;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			req.gpu = &ctx;
			OwnerFillOutput out;
			Assert::IsFalse(f.fill(req, m, out), L"declined");
			Assert::IsTrue(out.declineReason.find("grain level: grain split unavailable: ") == 0, L"worded with the split's cause");
			Assert::AreEqual(0, dev.asked, L"no output texture asked for before the split fits");
			Assert::AreEqual(uint64_t(1), c->declines.load(), L"counted");

			SourcePlanes::Forget(&host);
		}

		TEST_METHOD(FallbackLinesSayTheSingleFrameLawAndTheDeclines)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			Assert::IsTrue(L::FallbackLine(0).empty(), L"no Fallbacks line before any fill");
			Assert::IsTrue(L::FallbackLine(1).find(std::string(L::kFallbackId) + ": ") == 0, L"the Fallbacks id once in use");
			Assert::IsTrue(L::FallbackLine(12).find("single-frame law") != std::string::npos, L"the law in use");
			Assert::IsTrue(L::DeclineLine(0, "grain level: no GPU context").empty(), L"no decline: no line");
			Assert::AreEqual(std::string("grain_level_declined: grain level: no GPU context (3 declines)"),
				L::DeclineLine(3, "grain level: no GPU context"), L"the last decline and the count");
			Assert::AreEqual(std::string("grain_level_declined: grain level: RGB source (no luma plane to split) (1 decline)"),
				L::DeclineLine(1, "grain level: RGB source (no luma plane to split)"), L"one decline");
			Assert::AreEqual(std::string("grain_level_declined: a fill declined (2 declines)"), L::DeclineLine(2, std::string()),
				L"a count without a recorded reason still shows");
			Assert::IsTrue(L::UnregisteredLine(L::kUnregisteredId, std::string()).empty(), L"no problem: no line");
			Assert::AreEqual(std::string("grain_level_unregistered: why"), L::UnregisteredLine(L::kUnregisteredId, "why"), L"P10g id");
			Assert::AreEqual(std::string("grain_split_owner_unregistered: why"), L::UnregisteredLine(L::kSplitUnregisteredId, "why"),
				L"P10 owner id");
			Assert::AreEqual(std::string("refused: another fill is already registered"),
				L::RegistrationText(SkeletonMemo::Registration::AlreadyRegistered), L"worded");
			Assert::AreEqual(std::string("refused: not in the catalog"), L::RegistrationText(SkeletonMemo::Registration::NotInCatalog), L"worded");
		}

		// The module's own counters reach its Fallbacks lines: a fill made by the module declines (no GPU context) and shows.
		TEST_METHOD(ModuleShowsItsFillsDeclineOnTheFallbacksRoute)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			SkeletonMemo m;
			GrainMemoDemand demand;
			demand.memo = &m;
			L::GrainLevelModule module;
			int host = 0;
			Assert::IsTrue(module.Observe(&host, GrainDims(64, 48), &demand), L"registered");
			OwnerFillRequest req;
			req.id = kP10gGrainLevel;
			req.seq = 7;
			req.form = AcquireForm::Gpu;
			OwnerFillOutput out;
			Assert::IsFalse(demand.lastLevelFill.fill(req, m, out), L"the module's fill declines without a GPU context");
			const std::vector<std::string> lines = module.FallbackLines();
			Assert::AreEqual(size_t(1), lines.size(), L"exactly the decline (no fill ran)");
			Assert::AreEqual(std::string("grain_level_declined: grain level: no GPU context (1 decline)"), lines[0], L"on the Fallbacks route");
			Assert::AreEqual(L::DeclineLineOf(module.Stats()), lines[0], L"FallbackLines shows the counters' declined line while it is set");
		}

		// The declined line releases after a successful fill (r2 SHOULD-1, Engage/Release): NoteFill is the exact call the fill makes
		// after its pass dispatched (the pass itself needs a GPU: T5); the count and the last reason are kept for the next decline.
		TEST_METHOD(DeclinedLineReleasesAfterASuccessfulFill)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;
			Assert::IsTrue(L::DeclineLine(1, "grain level: no GPU context", false).empty(), L"released: no line although a decline was counted");
			Assert::IsTrue(L::DeclineLine(3, std::string(), false).empty(), L"released with no recorded reason: no line");

			L::Counters c;
			Assert::IsTrue(L::DeclineLineOf(c).empty(), L"nothing declined: no line");
			OwnerFillOutput out;
			Assert::IsFalse(L::Decline(out, &c, "no GPU context"), L"a decline");
			Assert::AreEqual(std::string("grain_level_declined: grain level: no GPU context (1 decline)"), L::DeclineLineOf(c),
				L"engaged while the most recent fill declined");

			L::NoteFill(&c);
			Assert::IsTrue(L::DeclineLineOf(c).empty(), L"a successful fill releases the declined line");
			Assert::IsFalse(c.lastDeclined.load(), L"lastDeclined cleared by the fill");
			Assert::AreEqual(uint64_t(1), c.fills.load(), L"the fill is counted");
			Assert::AreEqual(uint64_t(1), c.declines.load(), L"the cumulative decline count is kept");
			Assert::AreEqual(std::string("grain level: no GPU context"), c.LastDecline(), L"the last reason is kept");

			OwnerFillOutput out2;
			Assert::IsFalse(L::Decline(out2, &c, "RGB source (no luma plane to split)"), L"a later decline");
			Assert::AreEqual(std::string("grain_level_declined: grain level: RGB source (no luma plane to split) (2 declines)"), L::DeclineLineOf(c),
				L"re-engaged with the new reason and the cumulative count");
		}

		// CT-H7: compiles and dispatches the LIVE kP10gGrainLevel GPU compute shader through SkeletonMemo::Acquire
		// across 8-bit, 10-bit and 12-bit luma inputs (odd geometry exercising partial 16x16 edge workgroups),
		// verifies block-by-block parity against the CPU GrainLevel single-frame reference, checks independent
		// physical grain-variance / Wiener-retention / depth-scaling invariants, verifies live decline->release and
		// memo reuse, and runs an in-test zero-grain negative control on the exact same 3-zone scene.
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuFillMatchesCpuReferenceAcrossBitDepths)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuFillMatchesCpuReferenceAcrossBitDepths)
		{
			using namespace VpSkeleton;
			namespace L = GrainSplit::LevelGpu;

			GrainLevelGpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo D3D11 test device");
			const pl_gpu gpu = dev.Gpu();
			const int w = 257, h = 143;   // odd on both axes: partial right (1 px), bottom (15 px) and corner workgroups

			GrainLevelGpuRig rig(gpu);
			rig.Observe(w, h);

			// Live decline -> GPU fill release: acquiring seq 1 before any P13 frame is offered declines visibly.
			{
				const AcquireResult declined = rig.memo.Acquire(kP10gGrainLevel, 1, AcquireForm::Gpu);
				Assert::IsFalse(declined.Ok(), L"missing source plane declines");
				Assert::IsTrue(declined.cause == AcquireCause::OwnerDeclined, L"visible OwnerDeclined cause");
				Assert::AreEqual(uint64_t(1), rig.module.Stats().declines.load(), L"decline counted");
				Assert::IsTrue(rig.module.Stats().lastDeclined.load(), L"declined Fallbacks line engaged");
				const std::vector<std::string> fl = rig.module.FallbackLines();
				Assert::AreEqual(size_t(1), fl.size(), L"only the declined line before any live fill");
				Assert::IsTrue(fl[0].find(std::string(L::kDeclinedId) + ": ") == 0, L"declined line shown");
			}
			rig.EndFrame(1);

			struct DepthCase { int bits; int sampleDepth; int bitShift; double sigma; uint32_t seed; const char* label; };
			const DepthCase cases[3] = {
				{  8,  8, 0, 1.5, 501u, "8-bit r8" },
				{ 10, 16, 6, 2.0, 502u, "10-bit r16" },
				{ 12, 16, 4, 8.0, 502u, "12-bit r16" },
			};

			double meanN10 = 0.0, meanN12 = 0.0;
			uint64_t seq = 2;
			uint64_t expectedFills = 0;

			for (const DepthCase& dc : cases)
			{
				const std::vector<uint16_t> codes = MakeSyntheticCodes(w, h, dc.bits, dc.sigma, dc.seed);
				int cpuBw = 0, cpuBh = 0;
				const GrainLevel::Result cpuRef = ComputeCpuGrainLevelRef(codes, w, h, dc.bits, cpuBw, cpuBh);

				std::vector<float> gpuRgba;
				{
					PlTexGuard plane(gpu, UploadLumaPlane(gpu, codes, w, h, dc.bits));
					rig.OfferLuma(seq, plane.tex, dc.sampleDepth, dc.bits, dc.bitShift);
					{
						const AcquireResult first = rig.memo.Acquire(kP10gGrainLevel, seq, AcquireForm::Gpu);
						const std::string causeMsg = std::string(dc.label) + ": Acquire(kP10gGrainLevel) failed: "
							+ rig.memo.LastCause(kP10gGrainLevel);
						Assert::IsTrue(first.Ok(), std::wstring(causeMsg.begin(), causeMsg.end()).c_str());
						Assert::IsTrue(first.status == AcquireStatus::FilledAtRead, L"first Acquire runs the live GPU fill");
						const pl_tex outTex = static_cast<pl_tex>(first.gpu.resource);
						Assert::IsNotNull(outTex, L"live GPU fill produced RGBA32F output texture");
						Assert::AreEqual(cpuBw, outTex->params.w, L"output width equals block grid width");
						Assert::AreEqual(cpuBh, outTex->params.h, L"output height equals block grid height");
						Assert::AreEqual(L::ReserveBytes(GrainDims(uint32_t(w), uint32_t(h))), first.gpu.bytes,
							L"allocated bytes match declared reserve");

						++expectedFills;
						Assert::AreEqual(expectedFills, rig.module.Stats().fills.load(), L"fill counter incremented once");
						Assert::IsFalse(rig.module.Stats().lastDeclined.load(), L"live GPU fill releases prior declined line");

						// Memo reuse on the same frame: second Acquire reuses the published texture without recomputing.
						const AcquireResult second = rig.memo.Acquire(kP10gGrainLevel, seq, AcquireForm::Gpu);
						Assert::IsTrue(second.Ok() && second.status == AcquireStatus::Reused, L"second Acquire reuses memo spot");
						Assert::IsTrue(second.gpu.resource == first.gpu.resource, L"reused handle points to the same texture");
						Assert::AreEqual(expectedFills, rig.module.Stats().fills.load(), L"no second GPU dispatch on reuse");

						if (expectedFills == 1)
						{
							const std::vector<std::string> fl = rig.module.FallbackLines();
							Assert::AreEqual(size_t(1), fl.size(), L"declined line released; single-frame law line active");
							Assert::IsTrue(fl[0].find(std::string(L::kFallbackId) + ": ") == 0,
								L"Fallbacks shows single-frame law after live GPU fill");
						}

						gpuRgba = DownloadGrainLevel(gpu, outTex);
					}
					rig.EndFrame(seq);
				}
				++seq;

				// 1. Exact GPU-vs-CPU parity on all blocks (including partial right/bottom/corner edge blocks).
				AssertGrainLevelParity(gpuRgba, cpuRef, cpuBw, cpuBh, dc.bits, dc.label);

				// 2. Independent physical ground-truth invariants (zero oracle-only check):
				//    - flat-zone interior N_b tracks planted sigma^2,
				//    - textured-zone retained share (mf2 = retained / N_b) exceeds flat-zone mf2 by > 0.25.
				double sumNFlat = 0.0, sumMf2Flat = 0.0, sumMf2Tex = 0.0;
				int cntFlat = 0, cntTex = 0;
				for (int by = 1; by <= cpuBh - 2; ++by)
				{
					for (int bx = 1; bx <= 3; ++bx)
					{
						const size_t idx = (static_cast<size_t>(by) * static_cast<size_t>(cpuBw) + static_cast<size_t>(bx)) * 4u;
						const double n = gpuRgba[idx + 0u];
						const double ret = gpuRgba[idx + 2u];
						sumNFlat += n;
						sumMf2Flat += n > 0.0 ? ret / n : 0.0;
						++cntFlat;
					}
					for (int bx = 6; bx <= 9; ++bx)
					{
						const size_t idx = (static_cast<size_t>(by) * static_cast<size_t>(cpuBw) + static_cast<size_t>(bx)) * 4u;
						const double n = gpuRgba[idx + 0u];
						const double ret = gpuRgba[idx + 2u];
						sumMf2Tex += n > 0.0 ? ret / n : 0.0;
						++cntTex;
					}
				}
				Assert::IsTrue(cntFlat > 0 && cntTex > 0, L"interior flat and textured zones non-empty");
				const double meanNFlat = sumNFlat / static_cast<double>(cntFlat);
				const double meanMf2Flat = sumMf2Flat / static_cast<double>(cntFlat);
				const double meanMf2Tex = sumMf2Tex / static_cast<double>(cntTex);
				const double sigma2 = dc.sigma * dc.sigma;
				char physLine[256];
				std::snprintf(physLine, sizeof physLine,
					"GRAIN_LEVEL_PHYSICS %s: meanNFlat=%.3f (sigma^2=%.3f) mf2Flat=%.3f mf2Tex=%.3f\n",
					dc.label, meanNFlat, sigma2, meanMf2Flat, meanMf2Tex);
				Logger::WriteMessage(physLine);
				Assert::IsTrue(meanNFlat >= 0.25 * sigma2 && meanNFlat <= 2.5 * sigma2,
					L"GPU N_b in flat noisy zone must track analytically planted grain variance sigma^2");
				Assert::IsTrue(meanMf2Tex > meanMf2Flat + 0.25,
					L"GPU retained grain ratio mf2 must be significantly higher in textured blocks than in pure-grain blocks");

				if (dc.bits == 10) meanN10 = meanNFlat;
				if (dc.bits == 12) meanN12 = meanNFlat;
			}

			// 3. Bit-depth variance scaling physics: 12-bit codes = 4x 10-bit codes -> variance scales by 16x.
			Assert::IsTrue(meanN10 > 0.0, L"10-bit flat N_b positive");
			const double ratio12to10 = meanN12 / meanN10;
			Assert::IsTrue(ratio12to10 >= 12.0 && ratio12to10 <= 20.0,
				L"12-bit GPU N_b must scale by DepthScale(12)^2 = 16x relative to 10-bit on the same physical grain");

			// 4. Mandatory in-test negative control (Rule 3): feed the EXACT same 10-bit 3-zone scene with zero grain
			//    (sigma = 0.0) through the live GPU kP10gGrainLevel shader and prove flat-zone N_b collapses below 0.05 * 4.0,
			//    violating the noisy grain floor (>= 0.25 * 4.0 = 1.0) satisfied above.
			{
				const std::vector<uint16_t> cleanCodes = MakeSyntheticCodes(w, h, 10, 0.0, 502u);
				int cpuBw = 0, cpuBh = 0;
				const GrainLevel::Result cpuRef = ComputeCpuGrainLevelRef(cleanCodes, w, h, 10, cpuBw, cpuBh);
				std::vector<float> gpuZero;
				{
					PlTexGuard plane(gpu, UploadLumaPlane(gpu, cleanCodes, w, h, 10));
					rig.OfferLuma(seq, plane.tex, 16, 10, 6);
					{
						const AcquireResult res = rig.memo.Acquire(kP10gGrainLevel, seq, AcquireForm::Gpu);
						Assert::IsTrue(res.Ok(), L"zero-grain baseline Acquire succeeds");
						gpuZero = DownloadGrainLevel(gpu, static_cast<pl_tex>(res.gpu.resource));
					}
					rig.EndFrame(seq);
				}
				AssertGrainLevelParity(gpuZero, cpuRef, cpuBw, cpuBh, 10, "10-bit zero-grain negative control");

				double sumZeroFlat = 0.0;
				int cntFlat = 0;
				for (int by = 1; by <= cpuBh - 2; ++by)
					for (int bx = 1; bx <= 3; ++bx)
					{
						sumZeroFlat += gpuZero[(static_cast<size_t>(by) * static_cast<size_t>(cpuBw) + static_cast<size_t>(bx)) * 4u];
						++cntFlat;
					}
				const double meanZeroFlat = sumZeroFlat / static_cast<double>(cntFlat);
				char negLine[192];
				std::snprintf(negLine, sizeof negLine,
					"GRAIN_LEVEL_NEG_CONTROL_ZERO_GRAIN: meanZeroFlat=%.6f vs noisy meanN10=%.3f (floor 1.0)\n",
					meanZeroFlat, meanN10);
				Logger::WriteMessage(negLine);
				Assert::IsTrue(meanZeroFlat < 0.05 * (2.0 * 2.0),
					L"zero-grain negative control must collapse N_b near 0 and violate the noisy grain floor");
			}
		}

		// CT-H7 (letterbox crop + Rule 3 negative control): runs the LIVE kP10gGrainLevel GPU compute shader on
		// letterbox-cropped active-picture planes (odd 10-bit 257x115 and even 12-bit 192x96) against the CPU
		// GrainLevel reference, and proves on the EXACT SAME 10-bit letterbox frame that omitting the letterbox
		// crop (naive full-frame 257x179 evaluation with 32-px noiseless black bars) collapses N_b along the active-
		// picture top border blocks to ~0 via the 5x5 neighbourhood minimum while the cropped GPU pass preserves the
		// planted grain floor and matches the CPU reference.
		BEGIN_TEST_METHOD_ATTRIBUTE(GpuFillMatchesCpuReferenceOnLetterboxCropAndExposesUncroppedHazard)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(GpuFillMatchesCpuReferenceOnLetterboxCropAndExposesUncroppedHazard)
		{
			using namespace VpSkeleton;

			GrainLevelGpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create the libplacebo D3D11 test device");
			const pl_gpu gpu = dev.Gpu();

			const int cropW = 257, cropH = 115;   // odd active picture: 17x8 blocks (right col 1 px, bottom row 3 px)
			const int barTop = 32, barBot = 32;   // 32 px = 2 full 16-px block rows -> by_crop=0 aligns 1:1 with by_full=2
			const int fullW = cropW, fullH = cropH + barTop + barBot;   // 257x179
			const double sigma10 = 2.5;
			const double sigma10Sq = sigma10 * sigma10;   // 6.25 codes^2
			const uint16_t kBarBlack10 = 64u;             // limited-range 10-bit black (noiseless letterbox bar)

			// Build ONE active picture and embed the EXACT SAME pixels into the full letterbox frame.
			const std::vector<uint16_t> cropCodes = MakeSyntheticCodes(cropW, cropH, 10, sigma10, 901u);
			std::vector<uint16_t> fullCodes(static_cast<size_t>(fullW) * static_cast<size_t>(fullH), kBarBlack10);
			for (int y = 0; y < cropH; ++y)
				for (int x = 0; x < cropW; ++x)
					fullCodes[static_cast<size_t>(y + barTop) * static_cast<size_t>(fullW) + static_cast<size_t>(x)] =
						cropCodes[static_cast<size_t>(y) * static_cast<size_t>(cropW) + static_cast<size_t>(x)];

			GrainLevelGpuRig rig(gpu);

			// 1. Protected path: letterbox-cropped active picture (257x115, 10-bit).
			int cropBw = 0, cropBh = 0;
			const GrainLevel::Result cpuCrop = ComputeCpuGrainLevelRef(cropCodes, cropW, cropH, 10, cropBw, cropBh);
			std::vector<float> gpuCrop;
			{
				rig.Observe(cropW, cropH);
				PlTexGuard plane(gpu, UploadLumaPlane(gpu, cropCodes, cropW, cropH, 10));
				rig.OfferLuma(10, plane.tex, 16, 10, 6);
				{
					const AcquireResult res = rig.memo.Acquire(kP10gGrainLevel, 10, AcquireForm::Gpu);
					const std::string causeMsg = "cropped 10-bit Acquire failed: " + rig.memo.LastCause(kP10gGrainLevel);
					Assert::IsTrue(res.Ok(), std::wstring(causeMsg.begin(), causeMsg.end()).c_str());
					const pl_tex outTex = static_cast<pl_tex>(res.gpu.resource);
					Assert::AreEqual(cropBw, outTex->params.w, L"cropped block grid width");
					Assert::AreEqual(cropBh, outTex->params.h, L"cropped block grid height");
					gpuCrop = DownloadGrainLevel(gpu, outTex);
				}
				rig.EndFrame(10);
			}
			AssertGrainLevelParity(gpuCrop, cpuCrop, cropBw, cropBh, 10, "10-bit letterbox-cropped 257x115");

			// 2. Mandatory in-test negative control (Rule 3): naive UNCROPPED full-frame evaluation (257x179) on the
			//    EXACT same letterbox frame (also exercises live reserve re-registration on size growth).
			int fullBw = 0, fullBh = 0;
			const GrainLevel::Result cpuFull = ComputeCpuGrainLevelRef(fullCodes, fullW, fullH, 10, fullBw, fullBh);
			std::vector<float> gpuFull;
			{
				rig.Observe(fullW, fullH);
				PlTexGuard plane(gpu, UploadLumaPlane(gpu, fullCodes, fullW, fullH, 10));
				rig.OfferLuma(11, plane.tex, 16, 10, 6);
				{
					const AcquireResult res = rig.memo.Acquire(kP10gGrainLevel, 11, AcquireForm::Gpu);
					const std::string causeMsg = "uncropped 10-bit Acquire failed: " + rig.memo.LastCause(kP10gGrainLevel);
					Assert::IsTrue(res.Ok(), std::wstring(causeMsg.begin(), causeMsg.end()).c_str());
					const pl_tex outTex = static_cast<pl_tex>(res.gpu.resource);
					Assert::AreEqual(fullBw, outTex->params.w, L"uncropped block grid width");
					Assert::AreEqual(fullBh, outTex->params.h, L"uncropped block grid height");
					gpuFull = DownloadGrainLevel(gpu, outTex);
				}
				rig.EndFrame(11);
			}
			AssertGrainLevelParity(gpuFull, cpuFull, fullBw, fullBh, 10, "10-bit naive uncropped 257x179");

			// Prove the hazard is real on the exact same top active-picture border blocks (by_crop=0 vs by_full=2):
			// the cropped pass preserves N_b >= 0.25 * sigma^2 on every full-width block, while the naive uncropped
			// pass collapses every one of those same blocks below 0.05 * sigma^2 due to bar bleed into the 5x5 min.
			const int borderRowFull = barTop / 16;   // 2
			const double grainFloor = 0.25 * sigma10Sq;
			const double collapseCeil = 0.05 * sigma10Sq;
			double minCroppedBorder = 1e30, maxUncroppedBorder = 0.0;
			double sumCroppedBorder = 0.0, sumUncroppedBorder = 0.0;
			const int borderCols = cropBw - 2;       // all full 16x16 columns along the top active border (bx = 0..14)
			Assert::IsTrue(borderCols > 0, L"non-empty border columns");
			for (int bx = 0; bx < borderCols; ++bx)
			{
				const double nCrop = gpuCrop[static_cast<size_t>(bx) * 4u];
				const double nFull = gpuFull[(static_cast<size_t>(borderRowFull) * static_cast<size_t>(fullBw) + static_cast<size_t>(bx)) * 4u];
				minCroppedBorder = (std::min)(minCroppedBorder, nCrop);
				maxUncroppedBorder = (std::max)(maxUncroppedBorder, nFull);
				sumCroppedBorder += nCrop;
				sumUncroppedBorder += nFull;
			}
			const double meanCroppedBorder = sumCroppedBorder / static_cast<double>(borderCols);
			const double meanUncroppedBorder = sumUncroppedBorder / static_cast<double>(borderCols);
			char lbLine[256];
			std::snprintf(lbLine, sizeof lbLine,
				"GRAIN_LEVEL_LETTERBOX_CONTROL (sigma^2=%.2f): cropped border min=%.3f mean=%.3f vs naive uncropped max=%.6f mean=%.6f\n",
				sigma10Sq, minCroppedBorder, meanCroppedBorder, maxUncroppedBorder, meanUncroppedBorder);
			Logger::WriteMessage(lbLine);
			Assert::IsTrue(minCroppedBorder >= grainFloor,
				L"cropped GPU pass must preserve planted grain floor (>= 0.25 * sigma^2) on every active-picture border block");
			Assert::IsTrue(maxUncroppedBorder < collapseCeil,
				L"naive uncropped letterbox negative control must collapse active-picture border N_b below 0.05 * sigma^2");
			Assert::IsTrue(meanUncroppedBorder < 0.05 * meanCroppedBorder,
				L"naive uncropped letterbox pass must severely under-report active-picture border grain vs cropped pass");

			// 3. Even-multiple 12-bit letterbox-cropped frame (192x96 -> 12x6 full 16x16 blocks; tests reserve shrink).
			{
				const int cw12 = 192, ch12 = 96;
				const double sigma12 = 8.0;
				const std::vector<uint16_t> crop12 = MakeSyntheticCodes(cw12, ch12, 12, sigma12, 902u);
				int bw12 = 0, bh12 = 0;
				const GrainLevel::Result cpu12 = ComputeCpuGrainLevelRef(crop12, cw12, ch12, 12, bw12, bh12);
				std::vector<float> gpu12;
				{
					rig.Observe(cw12, ch12);
					PlTexGuard plane(gpu, UploadLumaPlane(gpu, crop12, cw12, ch12, 12));
					rig.OfferLuma(12, plane.tex, 16, 12, 4);
					{
						const AcquireResult res = rig.memo.Acquire(kP10gGrainLevel, 12, AcquireForm::Gpu);
						const std::string causeMsg = "cropped 12-bit Acquire failed: " + rig.memo.LastCause(kP10gGrainLevel);
						Assert::IsTrue(res.Ok(), std::wstring(causeMsg.begin(), causeMsg.end()).c_str());
						const pl_tex outTex = static_cast<pl_tex>(res.gpu.resource);
						Assert::AreEqual(bw12, outTex->params.w, L"12-bit cropped block width");
						Assert::AreEqual(bh12, outTex->params.h, L"12-bit cropped block height");
						gpu12 = DownloadGrainLevel(gpu, outTex);
					}
					rig.EndFrame(12);
				}
				AssertGrainLevelParity(gpu12, cpu12, bw12, bh12, 12, "12-bit letterbox-cropped 192x96");
				double sumN = 0.0;
				for (size_t i = 0; i < static_cast<size_t>(bw12) * static_cast<size_t>(bh12); ++i)
					sumN += gpu12[i * 4u];
				const double meanN = sumN / static_cast<double>(bw12 * bh12);
				Assert::IsTrue(meanN >= 0.25 * sigma12 * sigma12 && meanN <= 2.5 * sigma12 * sigma12,
					L"12-bit letterbox-cropped GPU N_b must track planted 12-bit grain variance");
			}
		}

		// W23-B3 M2#5 (15b A1): CPU unit contract for kP10cGrainSplitChroma variant key/reserve encoding,
		// GrainSplitOwnerModule dual P10 + P10c registration/atomic ReplaceOwnerFill resize/detach/refusal,
		// and benign handoff with GrainLevelModule (SplitOwnerServedElsewhere() == true, zero false alarms).
		BEGIN_TEST_METHOD_ATTRIBUTE(P10cChromaVariantOwnerContractAndModuleRegistration)
			TEST_METHOD_ATTRIBUTE(L"Category", L"Cpu")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(P10cChromaVariantOwnerContractAndModuleRegistration)
		{
			using namespace VpSkeleton;
			namespace G = GrainSplit::Gpu;
			namespace L = GrainSplit::LevelGpu;
			namespace RE = RendererExtension;

			// 1. ChromaVariantFor & ChromaReserveFor positive + negative controls.
			const VariantKey cbKey = G::ChromaVariantFor(1, 960, 540, 1.0, 10);
			const VariantKey crKey = G::ChromaVariantFor(2, 960, 540, 1.0, 10);
			Assert::AreEqual(uint8_t(1), cbKey.plane, L"plane 1 encodes Cb");
			Assert::AreEqual(uint8_t(2), crKey.plane, L"plane 2 encodes Cr");
			Assert::AreEqual(uint32_t(960), cbKey.width);
			Assert::AreEqual(uint32_t(540), cbKey.height);
			Assert::AreEqual(uint8_t(0), cbKey.scale, L"scale 1.0 encodes axis 0");
			Assert::AreEqual(uint8_t(10), cbKey.codeUnit);
			Assert::IsTrue((G::VariantOwner::ChromaServedAxes() & VariantKey::kAxisPlane) != 0u,
				L"P10c served axes include plane");
			Assert::IsTrue((G::VariantOwner::ServedAxes() & VariantKey::kAxisPlane) == 0u,
				L"negative control: P10 luma served axes exclude plane");

			const uint64_t cbReserve = G::VariantOwner::ChromaReserveFor(cbKey);
			const uint64_t crReserve = G::VariantOwner::ChromaReserveFor(crKey);
			const uint64_t lumaSameSizeReserve = G::VariantOwner::ReserveFor(G::VariantFor(960, 540, 1.0, 10));
			Assert::IsTrue(cbReserve > uint64_t(0), L"ChromaReserveFor(plane 1) > 0");
			Assert::AreEqual(cbReserve, crReserve, L"Cb and Cr at same dims share exact reserve");
			Assert::AreEqual(lumaSameSizeReserve, cbReserve, L"chroma reserve equals single-plane ContextShapesOf reserve");

			// Negative controls: plane 0, plane >= 3, negative plane, zero dims, and unrepresentable scale return 0.
			const VariantKey negPlane = G::ChromaVariantFor(-1, 960, 540, 1.0, 10);
			Assert::AreEqual(uint8_t(255), negPlane.plane, L"negative plane maps to sentinel 255, never wrapping into 1 or 2");
			Assert::AreEqual(uint64_t(0), G::VariantOwner::ChromaReserveFor(G::ChromaVariantFor(0, 960, 540, 1.0, 10)),
				L"negative control: plane 0 (luma) rejected with 0 reserve");
			Assert::AreEqual(uint64_t(0), G::VariantOwner::ChromaReserveFor(G::ChromaVariantFor(3, 960, 540, 1.0, 10)),
				L"negative control: plane >= 3 rejected with 0 reserve");
			Assert::AreEqual(uint64_t(0), G::VariantOwner::ChromaReserveFor(negPlane),
				L"negative control: negative plane rejected with 0 reserve");
			Assert::AreEqual(uint64_t(0), G::VariantOwner::ChromaReserveFor(G::ChromaVariantFor(1, 0, 540, 1.0, 10)),
				L"negative control: zero width rejected with 0 reserve");
			Assert::AreEqual(uint64_t(0), G::VariantOwner::ChromaReserveFor(G::ChromaVariantFor(1, 960, 540, 1.3, 10)),
				L"negative control: unrepresentable scale 1.3 rejected with 0 reserve");
			Assert::AreEqual(G::kScaleAxisUnrepresentable, G::ChromaVariantFor(1, 960, 540, 1.3, 10).scale,
				L"unrepresentable scale encodes kScaleAxisUnrepresentable");

			// 2. GrainSplitOwnerModule lifecycle: dual P10 + P10c registration, atomic resize, detach/re-attach, refusal.
			struct DualOwnerDemand : Vpv::Skeleton::IExternalDemand
			{
				SkeletonMemo* memo = nullptr;
				int lumaRegisters = 0, lumaReplaces = 0, lumaUnregisters = 0;
				int chromaRegisters = 0, chromaReplaces = 0, chromaUnregisters = 0;
				bool refuseChroma = false;
				bool refuseChromaReplace = false;

				void SetExternalDemand(const void*, const void*, const std::vector<ConsumerDecl>&,
					const std::vector<ProductId>&) override {}
				void ClearExternalDemand(const void*, const void*) override {}

				SkeletonMemo::Registration RegisterOwnerFill(ProductId id, AcquireForm form,
					const std::string& unit, OwnerFill fill) override
				{
					if (id == kP10GrainSplit) ++lumaRegisters;
					if (id == kP10cGrainSplitChroma)
					{
						++chromaRegisters;
						if (refuseChroma)
							return SkeletonMemo::Registration::NotOwner;
					}
					return memo->RegisterOwnerFill(id, form, unit, std::move(fill));
				}

				SkeletonMemo::Registration ReplaceOwnerFill(ProductId id, AcquireForm form,
					const std::string& unit, OwnerFill fill) override
				{
					if (id == kP10GrainSplit) ++lumaReplaces;
					if (id == kP10cGrainSplitChroma)
					{
						++chromaReplaces;
						if (refuseChromaReplace)
							return SkeletonMemo::Registration::BadDefaults;
					}
					return memo->ReplaceOwnerFill(id, form, unit, std::move(fill));
				}

				bool UnregisterOwnerFill(ProductId id, AcquireForm form, const std::string& unit) override
				{
					if (id == kP10GrainSplit) ++lumaUnregisters;
					if (id == kP10cGrainSplitChroma) ++chromaUnregisters;
					return memo->UnregisterOwnerFill(id, form, unit);
				}
			};

			auto memoReserveOf = [](const SkeletonMemo& m, ProductId pid) -> uint64_t {
				const AheadNeed need = m.MakeAheadNeed(QueueWindow{});
				for (const AheadCost& c : need.costs)
					if (c.id == uint16_t(pid) && c.form == uint8_t(AcquireForm::Gpu))
						return c.bytesPerFrame;
				return uint64_t(0);
			};

			SkeletonMemo memo;
			DualOwnerDemand demand;
			demand.memo = &memo;
			G::GrainSplitOwnerModule ownerMod;
			int host = 0;

			const Dims l720 = GrainDims(1280, 720), c360 = GrainDims(640, 360);
			const Dims l1080 = GrainDims(1920, 1080), c540 = GrainDims(960, 540);
			const Dims l4k = GrainDims(3840, 2160), c2k = GrainDims(1920, 1080);

			Assert::IsTrue(ownerMod.Observe(&host, l720, c360, &demand), L"Observe registers both P10 and P10c");
			Assert::IsTrue(ownerMod.LumaRegistered(), L"P10 registered");
			Assert::IsTrue(ownerMod.ChromaRegistered(), L"P10c registered");
			Assert::IsTrue(memo.HasOwnerFill(kP10GrainSplit, AcquireForm::Gpu), L"memo has P10 fill");
			Assert::IsTrue(memo.HasOwnerFill(kP10cGrainSplitChroma, AcquireForm::Gpu), L"memo has P10c fill");
			Assert::AreEqual(G::LumaReserveBytes(l720), ownerMod.LumaRegisteredReserveBytes());
			Assert::AreEqual(G::ChromaReserveBytes(c360), ownerMod.ChromaRegisteredReserveBytes());
			Assert::AreEqual(G::LumaReserveBytes(l720), memoReserveOf(memo, kP10GrainSplit));
			Assert::AreEqual(G::ChromaReserveBytes(c360), memoReserveOf(memo, kP10cGrainSplitChroma));
			Assert::AreEqual(1, demand.lumaRegisters);
			Assert::AreEqual(1, demand.chromaRegisters);
			Assert::AreEqual(0, demand.lumaUnregisters);
			Assert::AreEqual(0, demand.chromaUnregisters);
			Assert::IsTrue(ownerMod.FallbackLines().empty(), L"healthy dual registration has empty FallbackLines");

			// Negative control: naive duplicate RegisterChroma fails with AlreadyRegistered and leaves 360p reserve.
			G::VariantOwner naiveOwner;
			Assert::IsTrue(naiveOwner.RegisterChroma(memo, 1920, 1080) == SkeletonMemo::Registration::AlreadyRegistered,
				L"negative control: naive RegisterChroma on active memo fails with AlreadyRegistered");
			Assert::AreEqual(G::ChromaReserveBytes(c360), memoReserveOf(memo, kP10cGrainSplitChroma),
				L"negative control: refused RegisterChroma leaves prior chroma reserve unchanged");

			// Upward resize 720p/360p -> 4K/2K uses atomic ReplaceOwnerFill with zero unregister gap.
			Assert::IsTrue(ownerMod.Observe(&host, l4k, c2k, &demand), L"resize growth succeeds");
			Assert::AreEqual(1, demand.lumaRegisters, L"resize does not re-call RegisterOwnerFill for P10");
			Assert::AreEqual(1, demand.chromaRegisters, L"resize does not re-call RegisterOwnerFill for P10c");
			Assert::AreEqual(1, demand.lumaReplaces, L"resize calls ReplaceOwnerFill for P10");
			Assert::AreEqual(1, demand.chromaReplaces, L"resize calls ReplaceOwnerFill for P10c");
			Assert::AreEqual(0, demand.lumaUnregisters, L"zero unregister gap for P10");
			Assert::AreEqual(0, demand.chromaUnregisters, L"zero unregister gap for P10c");
			Assert::AreEqual(G::LumaReserveBytes(l4k), ownerMod.LumaRegisteredReserveBytes());
			Assert::AreEqual(G::ChromaReserveBytes(c2k), ownerMod.ChromaRegisteredReserveBytes());
			Assert::AreEqual(G::LumaReserveBytes(l4k), memoReserveOf(memo, kP10GrainSplit));
			Assert::AreEqual(G::ChromaReserveBytes(c2k), memoReserveOf(memo, kP10cGrainSplitChroma));

			// Detach (Forget) and re-attach at smaller 1080p/540p re-baselines atomically with zero gap.
			ownerMod.Forget(&host, &demand);
			Assert::IsTrue(ownerMod.LumaRegistered() && ownerMod.ChromaRegistered(), L"registrations stay live across detach");
			Assert::AreEqual(0, demand.lumaUnregisters);
			Assert::AreEqual(0, demand.chromaUnregisters);
			Assert::IsTrue(ownerMod.Observe(&host, l1080, c540, &demand), L"re-attach at smaller size succeeds");
			Assert::AreEqual(2, demand.lumaReplaces, L"re-baseline calls ReplaceOwnerFill for P10");
			Assert::AreEqual(2, demand.chromaReplaces, L"re-baseline calls ReplaceOwnerFill for P10c");
			Assert::AreEqual(0, demand.lumaUnregisters);
			Assert::AreEqual(0, demand.chromaUnregisters);
			Assert::AreEqual(G::LumaReserveBytes(l1080), ownerMod.LumaRegisteredReserveBytes());
			Assert::AreEqual(G::ChromaReserveBytes(c540), ownerMod.ChromaRegisteredReserveBytes());
			Assert::IsTrue(ownerMod.FallbackLines().empty());

			// 3. Handoff: GrainSplitOwnerModule registered first, GrainLevelModule observes second.
			{
				L::GrainLevelModule levelMod;
				Assert::IsTrue(levelMod.Observe(&host, l1080, &demand), L"GrainLevelModule registers P10g cleanly");
				Assert::IsTrue(levelMod.Registered(), L"P10g registered");
				Assert::IsFalse(levelMod.SplitOwnerRegistered(), L"P10 owned by GrainSplitOwnerModule, not GrainLevelModule");
				Assert::IsTrue(levelMod.SplitOwnerServedElsewhere(), L"GrainLevelModule recognizes P10 served elsewhere");
				Assert::IsTrue(levelMod.FallbackLines().empty(), L"benign handoff produces zero false alarms in GrainLevelModule");
				Assert::IsTrue(ownerMod.FallbackLines().empty(), L"benign handoff produces zero false alarms in GrainSplitOwnerModule");

				// Negative control: standalone GrainLevelModule without prior GrainSplitOwnerModule registers P10 itself.
				SkeletonMemo soloMemo;
				DualOwnerDemand soloDemand;
				soloDemand.memo = &soloMemo;
				L::GrainLevelModule soloLevel;
				Assert::IsTrue(soloLevel.Observe(&host, l1080, &soloDemand));
				Assert::IsTrue(soloLevel.SplitOwnerRegistered(), L"negative control: without ownerMod, GrainLevelModule owns P10");
				Assert::IsFalse(soloLevel.SplitOwnerServedElsewhere(), L"negative control: SplitOwnerServedElsewhere is false standalone");
			}

			// 4. Negative control (Rule 4): refused P10c registration visibly reports in FallbackLines() and OnStatus(Fallbacks).
			{
				SkeletonMemo refMemo;
				DualOwnerDemand refDemand;
				refDemand.memo = &refMemo;
				refDemand.refuseChroma = true;
				G::GrainSplitOwnerModule refMod;
				Assert::IsFalse(refMod.Observe(&host, l1080, c540, &refDemand), L"Observe returns false when P10c refuses");
				Assert::IsTrue(refMod.LumaRegistered(), L"P10 luma still registered");
				Assert::IsFalse(refMod.ChromaRegistered(), L"P10c chroma refused");
				const std::vector<std::string> fb = refMod.FallbackLines();
				Assert::AreEqual(size_t(1), fb.size(), L"exact single refusal line for P10c");
				Assert::IsTrue(fb[0].find(G::kP10cUnregisteredId) != std::string::npos, L"refusal line carries kP10cUnregisteredId");
				Assert::IsTrue(fb[0].find("not the catalog's owning unit") != std::string::npos, L"refusal line names NotOwner cause");
				RE::Context ctx{};
				ctx.host = &host;
				std::vector<std::string> statusLines;
				refMod.OnStatus(ctx, RE::StatusKind::Fallbacks, statusLines, nullptr, nullptr);
				Assert::AreEqual(fb.size(), statusLines.size());
				Assert::AreEqual(fb[0], statusLines[0], L"OnStatus(Fallbacks) surfaces exact refusal line");
			}
		}

		// W23-B3 M2#5 (15b A1): WARP GPU test for kP10cGrainSplitChroma owner fill acquire, single-compute reuse,
		// plane-axis separation (Cb vs Cr produce distinct grain statistics), and visible declines on invalid plane,
		// mismatched dimensions, and RGB P13 frames.
		BEGIN_TEST_METHOD_ATTRIBUTE(P10cChromaOwnerFillAcquireAndSingleCompute)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(P10cChromaOwnerFillAcquireAndSingleCompute)
		{
			using namespace VpSkeleton;
			namespace G = GrainSplit::Gpu;

			GrainLevelGpuDevice dev;
			Assert::IsTrue(dev.Create(), L"Could not create libplacebo D3D11 test device");
			const pl_gpu gpu = dev.Gpu();

			const int lw = 128, lh = 96;
			const int cw = 64, ch = 48;   // 4:2:0 chroma planes (4x3 16-px blocks)

			// Independent synthetic Y, Cb (quiet grain sigma=0.8), Cr (strong grain sigma=6.0) 10-bit planes.
			const std::vector<uint16_t> yCodes = MakeSyntheticCodes(lw, lh, 10, 1.5, 5101u);
			const std::vector<uint16_t> cbCodes = MakeSyntheticCodes(cw, ch, 10, 0.8, 5102u);
			const std::vector<uint16_t> crCodes = MakeSyntheticCodes(cw, ch, 10, 6.0, 5103u);

			PlTexGuard yTex(gpu, UploadLumaPlane(gpu, yCodes, lw, lh, 10));
			PlTexGuard cbTex(gpu, UploadLumaPlane(gpu, cbCodes, cw, ch, 10));
			PlTexGuard crTex(gpu, UploadLumaPlane(gpu, crCodes, cw, ch, 10));

			struct ChromaMemoRig
			{
				ReadableOwnerDevice device;
				SkeletonMemo memo;
				G::VariantOwner owner;
				explicit ChromaMemoRig(pl_gpu g) : device(g)
				{
					Assert::IsNotNull(device.dispatch, L"ReadableOwnerDevice pl_dispatch created");
					memo.SetHost(this);
					memo.SetRenderThread(std::this_thread::get_id());
					memo.SetGpuDispatcherPresent(true);
					memo.SetGpuBudget(true, uint64_t(1) << 30);
					memo.SetGpuDevice(&device);
					Assert::IsTrue(memo.RegisterOwnerFill(kP13SourceRing, AcquireForm::Gpu,
						Vpv::Capture::SourcePlanes::kUnit, Vpv::Capture::SourcePlanes::MakeOwnerFill())
						== SkeletonMemo::Registration::Registered, L"P13 registered");
					Assert::IsTrue(owner.RegisterChroma(memo, 64, 48) == SkeletonMemo::Registration::Registered,
						L"P10c chroma owner registered");
				}
				~ChromaMemoRig()
				{
					memo.ReleaseGpuSpots();
					Vpv::Capture::SourcePlanes::Forget(this);
				}
				ChromaMemoRig(const ChromaMemoRig&) = delete;
				ChromaMemoRig& operator=(const ChromaMemoRig&) = delete;
			};

			ChromaMemoRig rig(gpu);

			// Offer 3-plane YCbCr P13 SourcePlaneSet for seq = 41.
			const uint64_t seqYuv = 41u;
			{
				Vpv::Capture::SourcePlaneSet set;
				set.seq = seqYuv;
				set.planes = 3;
				set.tex[0] = yTex.tex;
				set.tex[1] = cbTex.tex;
				set.tex[2] = crTex.tex;
				set.components[0] = 1;
				set.components[1] = 1;
				set.components[2] = 1;
				set.rgb = false;
				set.repr.sys = PL_COLOR_SYSTEM_BT_709;
				set.repr.levels = PL_COLOR_LEVELS_LIMITED;
				set.repr.bits.sample_depth = 16;
				set.repr.bits.color_depth = 10;
				set.repr.bits.bit_shift = 6;
				set.origin = "p10c chroma acquire test";
				Assert::IsTrue(Vpv::Capture::SourcePlanes::Offer(&rig, set), L"3-plane YCbCr P13 offered");
			}

			auto meanBlockNb = [&](const G::Frame* f) -> double {
				Assert::IsNotNull(f, L"valid GrainSplit::Gpu::Frame");
				Assert::IsNotNull(f->blocks, L"frame->blocks texture");
				const std::vector<float> blk = DownloadGrainLevel(gpu, f->blocks);
				const size_t n = static_cast<size_t>(f->bw) * static_cast<size_t>(f->bh);
				Assert::AreEqual(n * 4u, blk.size());
				double sum = 0.0;
				for (size_t i = 0; i < n; ++i)
					sum += static_cast<double>(blk[i * 4u]);
				return sum / static_cast<double>(n);
			};

			// 1. Acquire plane=1 (Cb) twice -> single compute (ChromaComputes() == 1), identical Frame*.
			const VariantKey keyCb = G::ChromaVariantFor(1, cw, ch, 1.0, 10);
			const AcquireResult cb1 = rig.memo.Acquire(kP10cGrainSplitChroma, seqYuv, AcquireForm::Gpu, keyCb);
			const std::string cbErr = "Cb acquire failed: " + rig.memo.LastCause(kP10cGrainSplitChroma);
			Assert::IsTrue(cb1.Ok(), std::wstring(cbErr.begin(), cbErr.end()).c_str());
			const G::Frame* frameCb1 = G::FrameOf(cb1);
			Assert::IsNotNull(frameCb1, L"Cb FrameOf non-null");
			Assert::AreEqual(cw, frameCb1->w);
			Assert::AreEqual(ch, frameCb1->h);
			Assert::AreEqual(0, frameCb1->component);
			Assert::AreEqual(seqYuv, frameCb1->seq);
			Assert::AreEqual(uint64_t(1), rig.owner.ChromaComputes(), L"first Cb acquire runs one compute");

			const AcquireResult cb2 = rig.memo.Acquire(kP10cGrainSplitChroma, seqYuv, AcquireForm::Gpu, keyCb);
			Assert::IsTrue(cb2.Ok(), L"second Cb acquire hits memo");
			Assert::IsTrue(G::FrameOf(cb2) == frameCb1, L"second Cb acquire reuses exact same Frame*");
			Assert::AreEqual(uint64_t(1), rig.owner.ChromaComputes(), L"second Cb acquire does NOT recompute (single compute)");

			// 2. Negative control (a): acquiring plane=2 (Cr) increments ChromaComputes() == 2 and produces a distinct split.
			const VariantKey keyCr = G::ChromaVariantFor(2, cw, ch, 1.0, 10);
			const AcquireResult cr1 = rig.memo.Acquire(kP10cGrainSplitChroma, seqYuv, AcquireForm::Gpu, keyCr);
			const std::string crErr = "Cr acquire failed: " + rig.memo.LastCause(kP10cGrainSplitChroma);
			Assert::IsTrue(cr1.Ok(), std::wstring(crErr.begin(), crErr.end()).c_str());
			const G::Frame* frameCr1 = G::FrameOf(cr1);
			Assert::IsNotNull(frameCr1, L"Cr FrameOf non-null");
			Assert::IsTrue(frameCr1 != frameCb1, L"Cr split is distinct from Cb split");
			Assert::AreEqual(uint64_t(2), rig.owner.ChromaComputes(), L"plane=2 (Cr) increments ChromaComputes to 2");

			const double meanNbCb = meanBlockNb(frameCb1);
			const double meanNbCr = meanBlockNb(frameCr1);
			Assert::IsTrue(meanNbCr > meanNbCb * 4.0 && meanNbCr > 8.0,
				L"Cr (sigma=6.0) N_b strictly exceeds Cb (sigma=0.8) N_b by >4x, proving plane axis reads distinct chroma planes");

			// 3. Negative control (b): invalid plane (0 or 3) and mismatched size visibly decline with OwnerDeclined.
			{
				const AcquireResult rP0 = rig.memo.Acquire(kP10cGrainSplitChroma, seqYuv, AcquireForm::Gpu,
					G::ChromaVariantFor(0, cw, ch, 1.0, 10));
				Assert::IsFalse(rP0.Ok(), L"plane 0 must decline on P10c");
				Assert::IsTrue(rP0.cause == AcquireCause::OwnerDeclined, L"plane 0 returns OwnerDeclined");
				Assert::IsFalse(rig.owner.LastChromaError().empty(), L"plane 0 records non-empty LastChromaError()");

				const AcquireResult rP3 = rig.memo.Acquire(kP10cGrainSplitChroma, seqYuv, AcquireForm::Gpu,
					G::ChromaVariantFor(3, cw, ch, 1.0, 10));
				Assert::IsFalse(rP3.Ok(), L"plane 3 must decline on P10c");
				Assert::IsTrue(rP3.cause == AcquireCause::OwnerDeclined, L"plane 3 returns OwnerDeclined");
				Assert::IsFalse(rig.owner.LastChromaError().empty(), L"plane 3 records non-empty LastChromaError()");

				const AcquireResult rBadSize = rig.memo.Acquire(kP10cGrainSplitChroma, seqYuv, AcquireForm::Gpu,
					G::ChromaVariantFor(1, cw + 16, ch, 1.0, 10));
				Assert::IsFalse(rBadSize.Ok(), L"mismatched chroma size must decline");
				Assert::IsTrue(rBadSize.cause == AcquireCause::OwnerDeclined, L"mismatched size returns OwnerDeclined");
				Assert::IsTrue(rig.owner.LastChromaError().find("not the requested size") != std::string::npos,
					L"mismatched size error explains geometry mismatch");
			}

			rig.memo.OnSeal(seqYuv);
			Vpv::Capture::SourcePlanes::BeginFrame(&rig);

			// 4. Negative control (c): RGB P13 frame visibly declines on P10c.
			const uint64_t seqRgb = 42u;
			{
				Vpv::Capture::SourcePlaneSet rgbSet;
				rgbSet.seq = seqRgb;
				rgbSet.planes = 3;
				rgbSet.tex[0] = yTex.tex;
				rgbSet.tex[1] = cbTex.tex;
				rgbSet.tex[2] = crTex.tex;
				rgbSet.components[0] = 1;
				rgbSet.components[1] = 1;
				rgbSet.components[2] = 1;
				rgbSet.rgb = true;
				rgbSet.repr.sys = PL_COLOR_SYSTEM_RGB;
				rgbSet.repr.levels = PL_COLOR_LEVELS_FULL;
				rgbSet.repr.bits.sample_depth = 16;
				rgbSet.repr.bits.color_depth = 10;
				rgbSet.repr.bits.bit_shift = 6;
				rgbSet.origin = "p10c rgb negative control";
				Assert::IsTrue(Vpv::Capture::SourcePlanes::Offer(&rig, rgbSet), L"RGB P13 offered");

				const AcquireResult rRgb = rig.memo.Acquire(kP10cGrainSplitChroma, seqRgb, AcquireForm::Gpu, keyCb);
				Assert::IsFalse(rRgb.Ok(), L"RGB frame must decline on P10c");
				Assert::IsTrue(rRgb.cause == AcquireCause::OwnerDeclined, L"RGB frame returns OwnerDeclined");
				Assert::IsTrue(rig.owner.LastChromaError().find("RGB") != std::string::npos,
					L"RGB decline reason names RGB source");
				Assert::AreEqual(uint64_t(2), rig.owner.ChromaComputes(), L"declined acquires never increment ChromaComputes");

				rig.memo.OnSeal(seqRgb);
				Vpv::Capture::SourcePlanes::BeginFrame(&rig);
			}
		}
	};
}
