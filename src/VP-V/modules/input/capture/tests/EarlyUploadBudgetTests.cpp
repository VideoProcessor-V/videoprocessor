// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Early-upload byte budget rule (CPU only, no GPU).
#include "pch.h"
#include "CppUnitTest.h"
#include <VP-V/modules/input/capture/EarlyUploadBudget.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace VideoProcessorTest
{
	TEST_CLASS(EarlyUploadBudgetTests)
	{
	public:
		TEST_METHOD(SlotsAreMadeOnlyWithinTheCap)
		{
			const size_t cap = 1000, frame = 100;
			Assert::IsTrue(EarlyUploadBudget::MayMakeSlot(0, frame, cap));
			Assert::IsTrue(EarlyUploadBudget::MayMakeSlot(900, frame, cap), L"exactly fills the cap");
			Assert::IsFalse(EarlyUploadBudget::MayMakeSlot(901, frame, cap), L"one byte over");
			Assert::IsFalse(EarlyUploadBudget::MayMakeSlot(1000, frame, cap), L"old-size textures fill the cap");
			Assert::IsFalse(EarlyUploadBudget::MayMakeSlot(0, cap + 1, cap), L"a frame bigger than the cap");
			Assert::IsTrue(EarlyUploadBudget::MayMakeSlot(0, cap, cap));
			Assert::IsFalse(EarlyUploadBudget::MayMakeSlot(1, cap, cap));
		}
	};
}
