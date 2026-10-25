/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// WP-14 / audit M-1 (2026-10-25): VP's DM additions as an IDolbyVisionMetadataExtension
// (DvTunnelMetadataExtension.h) instead of inline code in Bill's formatter. The extension alone:
// L9 / L10 consumed (other levels left to the formatter), the hold at the measured rate, static
// metadata for shown pictures past the hold, the transport-damage cap, an unsupported body cleared
// once the hold expires, everything off with the fixes switch off, and the base interface being a
// no-op (no extension = the formatter's own behaviour). The formatter end to end (through the hook)
// stays covered by DvSignalL8HoldTests::FormatterParsesL8AndL10AndHoldsThenStatics once
// m2-dv-formatter-hook.patch is applied.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/signal/DvTunnelMetadataExtension.h>

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace DvExtension
	{
		inline DolbyVisionTunnelFrameMetadata Dm()
		{
			DolbyVisionTunnelFrameMetadata md;
			md.valid = true;
			md.hasSceneMetadata = true;
			md.sceneMaxPq = 3000;
			md.trimCount = 1;
			md.trims[0].level = 2;
			md.trims[0].targetMaxPq = 2081;
			return md;
		}

		inline IDolbyVisionMetadataExtension::Picture Rejected(uint64_t counter, bool bodyUnsupported = false, bool fixes = true)
		{
			IDolbyVisionMetadataExtension::Picture p;
			p.fixesOn = fixes;
			p.tunnelConfirmed = true;
			p.acceptedMetadata = false;
			p.haveMetadata = true;
			p.bodyUnsupported = bodyUnsupported;
			p.counter = counter;
			return p;
		}

		inline IDolbyVisionMetadataExtension::Picture Accepted(uint64_t counter)
		{
			IDolbyVisionMetadataExtension::Picture p = Rejected(counter);
			p.acceptedMetadata = true;
			return p;
		}

		inline DolbyVisionTunnelFrameMetadata Shown(const DvTunnelMetadataExtension& e, bool reused, bool validHeader = true)
		{
			DolbyVisionTunnelFrameMetadata md = Dm();
			md.reusedPreviousDm = reused;
			e.Decorate(md, validHeader);
			return md;
		}
	}

	TEST_CLASS(DvTunnelMetadataExtensionTests)
	{
	public:
		TEST_METHOD(ConsumesL9AndL10OnlyAndTheBaseInterfaceDoesNothing)
		{
			DvTunnelMetadataExtension e;
			DolbyVisionTunnelFrameMetadata md;
			const uint8_t l9[1] = { 2 };
			Assert::IsTrue(e.ParseExtensionBlock(9, l9, 1, md));
			Assert::IsTrue(md.hasL9 && md.l9SourcePrimaryIndex == 2);
			const uint8_t odd[3] = { 1, 2, 3 };
			Assert::IsTrue(e.ParseExtensionBlock(10, odd, 3, md), L"L10 is the extension's level even when the block is bad");
			Assert::AreEqual<int>(0, md.targetDisplayCount);
			Assert::IsFalse(e.ParseExtensionBlock(6, l9, 1, md), L"other levels stay the formatter's");
			Assert::IsFalse(e.ParseExtensionBlock(254, l9, 1, md));

			IDolbyVisionMetadataExtension none;   // the defaults: the formatter's own behaviour
			DolbyVisionTunnelFrameMetadata untouched = DvExtension::Dm();
			untouched.reusedPreviousDm = true;
			Assert::IsFalse(none.ParseExtensionBlock(10, odd, 3, untouched));
			Assert::IsFalse(none.KeepDmPastLossLimit(true, true, true));
			Assert::IsFalse(none.OnPicture(DvExtension::Rejected(1)));
			none.Decorate(untouched, true);
			Assert::AreEqual<int>(0, untouched.dmHoldPhase);
			Assert::IsTrue(untouched.hasSceneMetadata && untouched.trimCount == 1);
		}

		TEST_METHOD(HoldsThenStaticsThenCapsTransportDamage)
		{
			using namespace DvExtension;
			DvTunnelMetadataExtension e;
			e.OnSignal(24000.0 / 1001.0, 2.0);   // 6 held, cap 48 (as the formatter test)
			Assert::AreEqual(6u, e.Hold().HoldPictures());
			Assert::AreEqual(48u, e.Hold().StaticCapPictures());
			Assert::IsTrue(e.KeepDmPastLossLimit(true, true, true));
			Assert::IsFalse(e.KeepDmPastLossLimit(false, true, true), L"fixes off: the old clearing");
			Assert::IsFalse(e.KeepDmPastLossLimit(true, false, true), L"no valid header: cleared");
			uint64_t c = 0;
			for (int k = 0; k < 3; ++k)
				Assert::IsFalse(e.OnPicture(Accepted(++c)));
			Assert::AreEqual<int>(0, Shown(e, false).dmHoldPhase);
			for (int k = 1; k <= 6; ++k)
			{
				Assert::IsFalse(e.OnPicture(Rejected(++c)));
				const DolbyVisionTunnelFrameMetadata md = Shown(e, true);
				Assert::IsTrue(md.dmHoldPhase == 1 && md.hasSceneMetadata && md.trimCount == 1, L"held whole");
			}
			Assert::IsFalse(e.OnPicture(Rejected(++c)));
			DolbyVisionTunnelFrameMetadata md = Shown(e, true);
			Assert::IsTrue(md.dmHoldPhase == 2 && !md.hasSceneMetadata && md.trimCount == 0, L"static for a shown picture");
			Assert::AreEqual(7u, md.dmRejectedPictures);
			md = Shown(e, true, false);
			Assert::IsTrue(md.dmHoldPhase == 2 && md.hasSceneMetadata, L"no valid header: not stripped (the picture is black)");
			for (int span = 8; span <= 48; ++span)
				Assert::IsFalse(e.OnPicture(Rejected(++c)), L"static up to the cap");
			Assert::IsTrue(e.OnPicture(Rejected(++c)), L"past the cap: cleared");
			Assert::IsFalse(e.OnPicture(Accepted(++c)));
			Assert::AreEqual<int>(0, Shown(e, false).dmHoldPhase, L"fresh DM: own again");
		}

		TEST_METHOD(UnsupportedBodyIsClearedOnceTheHoldExpires)
		{
			using namespace DvExtension;
			DvTunnelMetadataExtension e;
			e.OnSignal(24000.0 / 1001.0, 2.0);
			uint64_t c = 0;
			Assert::IsFalse(e.OnPicture(Accepted(++c)));
			for (int k = 1; k <= 6; ++k)
				Assert::IsFalse(e.OnPicture(Rejected(++c, true)), L"held briefly");
			Assert::IsTrue(e.OnPicture(Rejected(++c, true)), L"unsupported past the hold: cleared, never the old matrices");
		}

		TEST_METHOD(FixesOffLeavesEverythingAlone)
		{
			using namespace DvExtension;
			DvTunnelMetadataExtension e;
			e.OnSignal(24000.0 / 1001.0, 2.0);
			uint64_t c = 0;
			for (int k = 0; k < 100; ++k)
				Assert::IsFalse(e.OnPicture(Rejected(++c, k % 2 == 0, false)));
			const DolbyVisionTunnelFrameMetadata md = Shown(e, true);
			Assert::IsTrue(md.dmHoldPhase == 0 && md.hasSceneMetadata && md.dmRejectedPictures == 0);
		}
	};
}
