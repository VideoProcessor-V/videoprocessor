/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Slice 21 W21-0: the STEP of a declared depth on the 10-bit analysis axis. Only STEP-derived constants move with depth;
// LEVEL thresholds keep their axis meaning (AnalysisAxis10.h). CPU only. A new class in a new file: AnalysisLumaDomainTests
// is not edited (rule 28).
#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/input/source_analysis/AnalysisAxis10.h>

#include <cstdint>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Vpv::AnalysisDomain;

namespace Tests
{
	TEST_CLASS(AnalysisStepAxisTests)
	{
	public:
		TEST_METHOD(StepAxisOfDeclaredDepth)
		{
			// step on the axis = 2^(10 - bits), derived (never tabulated): 8 -> 4, 10 -> 1, 12 -> 1/4, 16 -> 1/64.
			Assert::AreEqual(4.0, AxisStepOfBits(8));
			Assert::AreEqual(1.0, AxisStepOfBits(10));
			Assert::AreEqual(0.25, AxisStepOfBits(12));
			Assert::AreEqual(1.0 / 64.0, AxisStepOfBits(16));
			// native codes per axis code = (codeMaximum + 1) / 1024, and the two agree: step * native = 1 for every depth.
			Assert::AreEqual(0.25, NativeCodesPerAxisCode(CodeMaximum(8)));
			Assert::AreEqual(1.0, NativeCodesPerAxisCode(CodeMaximum(10)));
			Assert::AreEqual(4.0, NativeCodesPerAxisCode(CodeMaximum(12)));
			Assert::AreEqual(64.0, NativeCodesPerAxisCode(CodeMaximum(16)));
			for (int bits = 8; bits <= 16; ++bits)
			{
				Assert::AreEqual(1.0, AxisStepOfBits(bits) * NativeCodesPerAxisCode(CodeMaximum(bits)));
				// one native code at this depth is exactly AxisStepOfBits(bits) axis codes (Shift law, in Q6).
				const uint32_t q6One = AxisQ6(1u, CodeMaximum(bits), AxisLaw::Shift);
				Assert::AreEqual(AxisStepOfBits(bits) * 64.0, static_cast<double>(q6One));
			}
		}
	};
}
