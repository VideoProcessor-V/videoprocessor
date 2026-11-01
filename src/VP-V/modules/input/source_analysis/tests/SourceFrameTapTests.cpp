// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// The source frame tap (SourceFrameTap.h + VpFrameAnalysis::Host::Owner / OnArrival / Process): with no
// sink nothing changes; a registered sink gets the decoded luma plane as a SourceFrameView (msbShift 6,
// keepAlive holding the pooled plane); formats read in place are decoded privately only for a sink; the
// Host registers its store under its owner key and removes it with itself. CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysisHost.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace SourceFrameTapTestsNs
{
	using namespace SyntheticAnalysisFrames;
	namespace Tap = VpFrameAnalysis::SourceFrameTap;
	using VpSkeleton::SourceFrameView;

	// The luma of every pixel equals Sample().luma, through the view's own geometry.
	void AssertViewMatches(const SourceFrameView& v, const AnalysisLumaSource& source)
	{
		Assert::AreEqual<uint32_t>(static_cast<uint32_t>(source.width), v.width);
		Assert::AreEqual<uint32_t>(static_cast<uint32_t>(source.height), v.height);
		Assert::AreEqual<uint32_t>(v.width, v.strideSamples);
		Assert::AreEqual<int>(6, v.msbShift, L"10-bit codes in the low bits of 16");
		Assert::IsTrue(v.Valid());
		for (int y = 0; y < source.height; ++y)
			for (int x = 0; x < source.width; ++x)
			{
				AnalysisLumaSample sample;
				Assert::IsTrue(source.Sample(x, y, sample));
				if (v.luma[static_cast<size_t>(y) * v.strideSamples + x] != sample.luma)
					Assert::Fail(L"the view's luma differs from Sample().luma");
			}
	}

	TEST_CLASS(SourceFrameTapTests)
	{
	public:
		TEST_METHOD(NoSinkChangesNothing)
		{
			VpFrameAnalysis::Host host;
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Assert::IsFalse(Tap::AnyWants());
			Assert::IsFalse(Tap::Wants(&host));
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma == nullptr, L"no sink and no reader: no plane");
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual(uint64_t(0), host.Worker().Processed(), L"no job: nothing wants the frame");
			Assert::IsTrue(Tap::StoreFor(&host) == &host.Store(), L"Owner registers the store for the camera extension");
			host.Configure(false);
		}

		TEST_METHOD(AHostWithoutAnOwnerIsNeverTapped)
		{
			VpFrameAnalysis::Host host;   // Owner() never called
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Configure(true, false, false);
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(0, got.size());
			Assert::AreEqual(uint64_t(0), host.Worker().Processed());
			Tap::Unregister(&host);
			host.Configure(false);
		}

		TEST_METHOD(ASinkGetsTheDecodedPlaneAndHoldsItAlive)
		{
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			Assert::IsTrue(Tap::AnyWants() && Tap::Wants(&host));
			host.Owner(&host).Configure(true, false, false);   // no other reader wants the plane
			host.Worker().Stop();
			Frame frame = Make(Kind::Uyvy, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma != nullptr, L"a registered sink attaches the shared plane");
			host.OnArrival(7, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(1, got.size());
			Assert::AreEqual<uint64_t>(7, got[0].seq);
			Assert::IsTrue(got[0].keepAlive != nullptr);
			AssertViewMatches(got[0], frame.source);
			Assert::AreEqual<uint64_t>(1, Tap::Delivered(&host));

			// The view holds the pooled plane: it is not recycled for a later frame while the view lives.
			const void* held = got[0].keepAlive.get();
			frame.source.sharedLuma.reset();
			Frame next = Make(Kind::Uyvy, 2);
			VpFrameAnalysis::Host::AttachIfEnabled(next.source);
			Assert::IsTrue(next.source.sharedLuma != nullptr && next.source.sharedLuma.get() != held,
				L"a held plane is never handed out again");
			got.clear();   // the last holder lets go: the pool may recycle it now
			next.source.sharedLuma.reset();
			Frame third = Make(Kind::Uyvy, 3);
			VpFrameAnalysis::Host::AttachIfEnabled(third.source);
			Assert::IsTrue(third.source.sharedLuma != nullptr);

			Tap::Unregister(&host);
			Assert::IsFalse(Tap::AnyWants());
			host.Configure(false);
		}

		TEST_METHOD(PlaneFormatsAreDecodedPrivatelyForASink)
		{
			VpFrameAnalysis::Host host;
			std::vector<SourceFrameView> got;
			Tap::Register(&host, [&got](const SourceFrameView& v) { got.push_back(v); });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Frame frame = Make(Kind::P010, 1);
			VpFrameAnalysis::Host::AttachIfEnabled(frame.source);
			Assert::IsTrue(frame.source.sharedLuma == nullptr, L"a plane format is read in place: no shared plane");
			host.OnArrival(3, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(1, got.size());
			AssertViewMatches(got[0], frame.source);
			Tap::Unregister(&host);
			host.Configure(false);
		}

		TEST_METHOD(OnlyTheOwnersSinkIsDelivered)
		{
			VpFrameAnalysis::Host host;
			int other = 0;
			std::vector<SourceFrameView> gotOther;
			Tap::Register(&other, [&gotOther](const SourceFrameView& v) { gotOther.push_back(v); });
			host.Owner(&host).Configure(true, false, false);
			host.Worker().Stop();
			Assert::IsTrue(Tap::AnyWants());
			Assert::IsFalse(Tap::Wants(&host), L"another renderer's sink is not this one's");
			Frame frame = Make(Kind::Uyvy, 1);
			host.OnArrival(1, frame.source, FrameAnalysis::Rect{}, 0, false);
			host.Worker().Drain();
			Assert::AreEqual<size_t>(0, gotOther.size());
			Assert::AreEqual(uint64_t(0), host.Worker().Processed());
			Tap::Unregister(&other);
			host.Configure(false);
		}

		TEST_METHOD(AHostTakesItsStoreAndSinkWithIt)
		{
			const void* key = nullptr;
			{
				VpFrameAnalysis::Host host;
				key = &host;
				host.Owner(&host);
				Tap::Register(&host, [](const SourceFrameView&) {});
				Assert::IsTrue(Tap::StoreFor(key) != nullptr);
				Assert::IsTrue(Tap::Wants(key));
			}
			Assert::IsTrue(Tap::StoreFor(key) == nullptr, L"the store pointer never outlives the Host");
			Assert::IsFalse(Tap::Wants(key), L"nor its sink");
		}

		TEST_METHOD(ASinkMayRunWithoutTheRegistryLock)
		{
			// A sink that unregisters itself from inside the call must not deadlock.
			int key = 0;
			bool ran = false;
			Tap::Register(&key, [&](const SourceFrameView&) { ran = true; Tap::Unregister(&key); });
			SourceFrameView v;
			Assert::IsTrue(Tap::Deliver(&key, v));
			Assert::IsTrue(ran);
			Assert::IsFalse(Tap::Wants(&key));
			Assert::IsFalse(Tap::Deliver(&key, v));
		}
	};
}
