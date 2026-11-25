// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// AB2#2 (repair/CameraPassFailSeam): unit verification of unarmed inertness, per-site isolation,
// countdown semantics, RAII ScopedFail restore across normal / nested / exception unwind paths,
// and mandatory same-input negative control proving raw FailNext leaks without ScopedFail.
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/repair/CameraPassFailSeam.h>

#include <stdexcept>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace Tests
{
	namespace CameraPassFailSeamNs
	{
		using namespace VpRepair::CameraPassFailSeam;

		// Test-level RAII safety guard so no site can remain armed even on assertion failure.
		struct AllSitesResetGuard
		{
			AllSitesResetGuard() = default;
			~AllSitesResetGuard()
			{
				for (int i = 0; i < static_cast<int>(Site::Count); ++i)
					FailNext(static_cast<Site>(i), 0);
			}
			AllSitesResetGuard(const AllSitesResetGuard&) = delete;
			AllSitesResetGuard& operator=(const AllSitesResetGuard&) = delete;
		};

		TEST_CLASS(CameraPassFailSeamTests)
		{
		public:
			TEST_METHOD(UnarmedTakeReturnsFalseForEverySite)
			{
				AllSitesResetGuard reset;
				const Site all[] = { Site::GrainMap, Site::TextureStats, Site::TextureRef, Site::VectorField, Site::Debander };
				Assert::AreEqual(static_cast<size_t>(Site::Count), sizeof(all) / sizeof(all[0]),
					L"test array must cover every CameraPassFailSeam::Site enum value");
				for (Site s : all)
				{
					Assert::AreEqual(0, Armed(s).load(), L"site must start unarmed");
					Assert::IsFalse(Take(s), L"unarmed Take must return false");
					Assert::IsFalse(Take(s), L"repeated unarmed Take must stay false");
				}
			}

			TEST_METHOD(FailNextCountdownAndStrictSiteIsolation)
			{
				AllSitesResetGuard reset;
				const Site others[] = { Site::TextureStats, Site::TextureRef, Site::VectorField, Site::Debander };

				FailNext(Site::GrainMap, 2);
				// Strict site isolation while GrainMap is armed.
				for (Site s : others)
					Assert::IsFalse(Take(s), L"arming GrainMap must never affect any other site");

				Assert::IsTrue(Take(Site::GrainMap), L"first Take consumes 1st armed failure");
				for (Site s : others)
					Assert::IsFalse(Take(s), L"other sites stay unarmed mid-countdown");

				Assert::IsTrue(Take(Site::GrainMap), L"second Take consumes 2nd armed failure");
				Assert::IsFalse(Take(Site::GrainMap), L"third Take returns false after count exhausted");
			}

			TEST_METHOD(ScopedFailRestoresOnNormalNestedAndExceptionUnwind)
			{
				AllSitesResetGuard reset;

				// 1. Normal scope exit disarms remaining count.
				{
					ScopedFail guard(Site::TextureStats, 2);
					Assert::IsTrue(Take(Site::TextureStats), L"ScopedFail active inside scope");
				}
				Assert::IsFalse(Take(Site::TextureStats), L"ScopedFail must disarm on normal scope exit");

				// 2. Nested ScopedFail saves and restores outer scope's remaining count.
				{
					ScopedFail outer(Site::TextureRef, 3);
					Assert::IsTrue(Take(Site::TextureRef), L"outer consumes 1 (2 remaining)");
					{
						ScopedFail inner(Site::TextureRef, 1);
						Assert::IsTrue(Take(Site::TextureRef), L"inner consumes its 1");
						Assert::IsFalse(Take(Site::TextureRef), L"inner exhausted");
					}
					// Outer's 2 remaining failures must be restored.
					Assert::IsTrue(Take(Site::TextureRef), L"restored outer failure 1 of 2");
					Assert::IsTrue(Take(Site::TextureRef), L"restored outer failure 2 of 2");
					Assert::IsFalse(Take(Site::TextureRef), L"outer exhausted");
				}
				Assert::IsFalse(Take(Site::TextureRef), L"outer scope exit restores unarmed 0");

				// 3. C++ exception unwind restores unarmed state cleanly.
				bool caught = false;
				try
				{
					ScopedFail guard(Site::Debander, 3);
					Assert::IsTrue(Take(Site::Debander), L"armed before throw");
					throw std::runtime_error("boom");
				}
				catch (const std::runtime_error&)
				{
					caught = true;
				}
				Assert::IsTrue(caught, L"exception must have been thrown and caught");
				Assert::IsFalse(Take(Site::Debander), L"ScopedFail must disarm cleanly on exception unwind");
			}

			TEST_METHOD(ScopedFailPreventsLeakVsRawFailNextNegativeControl)
			{
				AllSitesResetGuard reset;

				// Mandatory in-test negative control on the exact same site & count:
				// Raw FailNext(Site::VectorField, 2) followed by only 1 Take LEAKS a remaining armed failure!
				{
					FailNext(Site::VectorField, 2);
					Assert::IsTrue(Take(Site::VectorField), L"negative control: 1st Take consumed");
				}
				Assert::IsTrue(Take(Site::VectorField),
					L"negative control: raw FailNext without ScopedFail leaks remaining armed failure past scope");
				FailNext(Site::VectorField, 0);
				Assert::IsFalse(Take(Site::VectorField), L"cleanup disarmed raw FailNext");

				// Protected path on the exact same site & count: ScopedFail leaves ZERO leak on scope exit!
				{
					ScopedFail guard(Site::VectorField, 2);
					Assert::IsTrue(Take(Site::VectorField), L"ScopedFail: 1st Take consumed");
				}
				Assert::IsFalse(Take(Site::VectorField),
					L"ScopedFail must leave zero leaked failures after scope exit");
			}
		};
	}
}
