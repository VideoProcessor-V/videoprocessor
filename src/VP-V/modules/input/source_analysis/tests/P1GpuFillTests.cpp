// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// P1 GPU fill (L-B4): the float emulation of the R16 unorm kernel equals the lane's CPU half-plane law bit for bit at 8/10/12/16
// bits, odd sizes included; the Float32 variant equals the accessor's mean. CPU only (the kernel's GPU run is the coordinator's).
// P1GpuModuleAheadTests (GF-P1 MUST-1): the production module's declared ahead depth, driven through the hub.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <VP-V/modules/input/source_analysis/lane/P1GpuFill.h>
#include <VP-V/modules/input/source_analysis/lane/shared/HalfLuma.h>
#include <VP-V/core/AheadCeiling.h>
#include <VP-V/core/SkeletonAcquire.h>
#include <VP-V/core/hub/CurrentFrame.h>
#include <VP-V/core/hub/ExtensionHub.h>
#include <VP-V/modules/control/skeleton/SkeletonFacts.h>
#include <libplacebo/gpu.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "CppUnitTest.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(P1GpuFillTests)
	{
	public:
		TEST_METHOD(Unorm16KernelEqualsTheCpuLawAtEveryBitDepth)
		{
			using namespace VpUnit::HalfLuma;
			uint32_t rng = 12345u;
			auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
			const int depths[4] = { 8, 10, 12, 16 };
			for (int bits : depths)
			{
				const uint32_t maxCode = (1u << bits) - 1u;
				for (uint32_t w : { 7u, 8u, 33u })
				{
					const uint32_t h = w + 1;
					std::vector<uint32_t> code(size_t(w) * h);
					for (auto& c : code) c = next() % (maxCode + 1u);
					code[0] = 0; code[1] = maxCode;
					const uint32_t hw = HalfDim(w), hh = HalfDim(h);
					for (uint32_t y = 0; y < hh; ++y)
						for (uint32_t x = 0; x < hw; ++x)
						{
							const uint32_t x0 = Tap0(x, w), x1 = Tap1(x, w), y0 = Tap0(y, h), y1 = Tap1(y, h);
							const uint32_t shift = 16u - uint32_t(bits);
							const uint32_t sum = Box4(code[y0 * w + x0] << shift, code[y0 * w + x1] << shift, code[y1 * w + x0] << shift, code[y1 * w + x1] << shift);
							const uint16_t cpu = FinishP1(sum);
							auto norm = [&](uint32_t c) { return float(c) / float(maxCode); };
							const uint16_t gpu = VpFrameAnalysis::P1Gpu::EmulateUnorm16(norm(code[y0 * w + x0]), norm(code[y0 * w + x1]),
								norm(code[y1 * w + x0]), norm(code[y1 * w + x1]), bits);
							Assert::AreEqual(unsigned(cpu), unsigned(gpu));
						}
				}
			}
		}

		TEST_METHOD(Float32VariantIsTheAccessorMean)
		{
			Assert::AreEqual(VpUnit::HalfLuma::MeanF<float>(0.1f, 0.2f, 0.3f, 0.4f),
				VpFrameAnalysis::P1Gpu::EmulateFloat32(0.1f, 0.2f, 0.3f, 0.4f));
			Assert::IsTrue(std::string(VpFrameAnalysis::P1Gpu::FillBody()).find("vpu_half_luma(src, p)") != std::string::npos);
		}
	};

	// WAIVER-C5-GF-T6 (GF-P1 MUST-1, gating): the production P1 GPU module declares the matcher's structural read ahead
	// (kMatcherStructuralAhead = 2: the render pair B = N + 1 and the EstimateAhead pair C = N + 2) ONLY while the matcher is an
	// active consumer: 2 from a matcher offer, 0 once no offer has come for more than kAheadCeiling Entrance frames (owner OD
	// 2026-11-14: ahead cap 6, derived from active consumers), 2 again at the next offer. The skeleton is a fake IExternalDemand that
	// captures RegisterOwnerFill; the module is the production one (RegisterP1GpuModule), driven through the hub's NoteP1GpuSourceLuma
	// and Entrance phase. The body needs no GPU object (the Entrance image names a plain texture struct); the category puts it on
	// the GPU gate with the other P1 GPU lines.
	namespace P1GpuModuleAheadSupport
	{
		struct Registration
		{
			VpSkeleton::ProductId id = VpSkeleton::kNoProduct;
			VpSkeleton::AcquireForm form = VpSkeleton::AcquireForm::Cpu;
			std::string unit;
			VpSkeleton::OwnerFill fill;
		};

		struct CapturingDemand : Vpv::Skeleton::IExternalDemand
		{
			std::vector<Registration> registrations;
			int unregistrations = 0;
			void SetExternalDemand(const void*, const void*, const std::vector<VpSkeleton::ConsumerDecl>&,
				const std::vector<VpSkeleton::ProductId>&) override {}
			void ClearExternalDemand(const void*, const void*) override {}
			VpSkeleton::SkeletonMemo::Registration RegisterOwnerFill(VpSkeleton::ProductId id, VpSkeleton::AcquireForm form,
				const std::string& unit, VpSkeleton::OwnerFill fill) override
			{
				Registration r;
				r.id = id;
				r.form = form;
				r.unit = unit;
				r.fill = std::move(fill);
				registrations.push_back(std::move(r));
				return VpSkeleton::SkeletonMemo::Registration::Registered;
			}
			bool UnregisterOwnerFill(VpSkeleton::ProductId, VpSkeleton::AcquireForm, const std::string&) override
			{
				++unregistrations;
				return true;
			}
		};

		struct FakeSkeleton : Vpv::Hub::ISkeletonModule
		{
			CapturingDemand demand;
			const char* Name() const override { return "test.p1_gpu_module_ahead.fake_skeleton"; }
			void* Query(const char* id) override
			{
				return id != nullptr && std::string(id) == Vpv::Skeleton::kExternalDemandQueryId
					? static_cast<Vpv::Skeleton::IExternalDemand*>(&demand) : nullptr;
			}
		};

		// The hub with the fake skeleton first and the production P1 module after it (HubBootstrap's order).
		struct HubRig
		{
			int host = 0;
			CapturingDemand* demand = nullptr;   // owned by the hub's module
			pl_tex_t luma{};
			HubRig()
			{
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
				std::unique_ptr<FakeSkeleton> skeleton(new FakeSkeleton());
				demand = &skeleton->demand;
				Vpv::Hub::ExtensionHub::Instance().AddModule(std::move(skeleton));
				Vpv::FrameAnalysis::RegisterP1GpuModule();
				luma.params.w = int(kWidth);
				luma.params.h = int(kHeight);
			}
			~HubRig()
			{
				demand = nullptr;
				Vpv::Hub::ForgetCurrentFrame(&host);
				Vpv::Hub::ExtensionHub::Instance().ResetModulesForTesting();
			}
			HubRig(const HubRig&) = delete;
			HubRig& operator=(const HubRig&) = delete;
			static constexpr uint32_t kWidth = 1920;
			static constexpr uint32_t kHeight = 1080;
			void Offer() { Vpv::FrameAnalysis::NoteP1GpuSourceLuma(&host, kWidth, kHeight); }
			// One render frame's Entrance (the module observes the size and counts the frame).
			void Entrance(int64_t seq)
			{
				pl_frame image{};
				image.num_planes = 1;
				image.planes[0].texture = &luma;
				image.planes[0].components = 1;
				RendererExtension::FrameContext c;
				c.host = &host;
				c.phase = RendererExtension::FramePhase::Entrance;
				c.seq = seq;
				c.image = &image;
				Vpv::Hub::ExtensionHub::Instance().OnFrame(c);
			}
			VpSkeleton::Dims Dims() const
			{
				VpSkeleton::Dims d;
				d.width = kWidth;
				d.height = kHeight;
				return d;
			}
		};
	}

	TEST_CLASS(P1GpuModuleAheadTests)
	{
	public:
		// A matcher offer registers the (kP1HalfLuma, Gpu) fill with the structural ahead (2) and the per-frame working set as its
		// reserve.
		BEGIN_TEST_METHOD_ATTRIBUTE(MatcherOfferDeclaresTheStructuralAhead)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MatcherOfferDeclaresTheStructuralAhead)
		{
			P1GpuModuleAheadSupport::HubRig rig;
			Assert::IsTrue(rig.demand != nullptr && rig.demand->registrations.empty(), L"nothing is registered before a size is known");
			rig.Offer();
			Assert::AreEqual(size_t(1), rig.demand->registrations.size(), L"the offer registered the fill once");
			const P1GpuModuleAheadSupport::Registration& r = rig.demand->registrations.back();
			Assert::IsTrue(r.id == VpSkeleton::kP1HalfLuma && r.form == VpSkeleton::AcquireForm::Gpu, L"the (kP1HalfLuma, Gpu) fill");
			Assert::AreEqual(unsigned(VpFrameAnalysis::P1Gpu::kMatcherStructuralAhead), unsigned(r.fill.aheadDepth), L"aheadDepth is the structural ahead");
			Assert::AreEqual(unsigned(2), unsigned(r.fill.aheadDepth), L"the structural ahead is the render pair B and the ahead pair C: 2");
			Assert::IsTrue(r.fill.reserveBytes > 0, L"a reserve");
			Assert::AreEqual(VpFrameAnalysis::P1Gpu::MatcherPyramidReserveBytes(rig.Dims()), r.fill.reserveBytes,
				L"reserveBytes = MatcherPyramidReserveBytes of the observed luma");
			Assert::AreEqual(0, rig.demand->unregistrations, L"the first registration unregisters nothing");
		}

		// Entrance frames with no offers: after kAheadCeiling + 1 of them the fill is re-registered with 0, once; a new offer
		// restores 2.
		BEGIN_TEST_METHOD_ATTRIBUTE(QuietMatcherDeclaresZero)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(QuietMatcherDeclaresZero)
		{
			P1GpuModuleAheadSupport::HubRig rig;
			rig.Offer();
			Assert::AreEqual(size_t(1), rig.demand->registrations.size(), L"registered with the structural ahead");
			Assert::AreEqual(unsigned(2), unsigned(rig.demand->registrations.back().fill.aheadDepth), L"declared 2 while the matcher is active");
			const int ceiling = int(VpSkeleton::kAheadCeiling);
			int64_t seq = 1;
			for (int i = 0; i < ceiling; ++i)
				rig.Entrance(seq++);
			Assert::AreEqual(size_t(1), rig.demand->registrations.size(), L"within kAheadCeiling Entrances of the last offer: still active, no re-registration");
			rig.Entrance(seq++);   // the (kAheadCeiling + 1)th Entrance with no offer
			Assert::AreEqual(size_t(2), rig.demand->registrations.size(), L"after kAheadCeiling + 1 Entrances: re-registered");
			Assert::AreEqual(unsigned(0), unsigned(rig.demand->registrations.back().fill.aheadDepth), L"with aheadDepth 0");
			Assert::AreEqual(1, rig.demand->unregistrations, L"the old fill was unregistered first");
			for (int i = 0; i < 5; ++i)
				rig.Entrance(seq++);
			Assert::AreEqual(size_t(2), rig.demand->registrations.size(), L"exactly once: no re-register on each later frame");
			Assert::AreEqual(1, rig.demand->unregistrations, L"and no further unregister");
			rig.Offer();
			Assert::AreEqual(size_t(3), rig.demand->registrations.size(), L"a new offer re-registers");
			Assert::AreEqual(unsigned(2), unsigned(rig.demand->registrations.back().fill.aheadDepth), L"and restores 2");
			Assert::AreEqual(2, rig.demand->unregistrations, L"the quiet fill was unregistered first");
		}
	};
}
