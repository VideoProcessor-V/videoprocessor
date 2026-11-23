// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// 15c C6 (owner answer 3, 2026-11-15): the decode owner (AnalysisLumaSource + its VP-V face AnalysisLumaDomain.h) now
// carries the frame's shared luma plane and the studio map that VpFrameAnalysis used to compute itself (checker site D6).
// This pins the move as BIT-IDENTICAL: the producer's former private decode, frozen below as the ORACLE (verbatim from
// VpFrameAnalysis.cpp at ab32bc33b: SamplerCodesAreFullRange, StudioLumaMap, NeedsDecode, DecodeSharedLuma), against the
// extended owner, on synthetic fixtures (R210, R12B, R12L, BGRA, full-range P010 / decoded DV 4:2:2 / v210, and every
// other SyntheticAnalysisFrames kind), VP-V and Original pathway, BT.709 and BT.2020, two frames:
//   (1) the same formats need a plane; (2) the plane's codes are byte-equal; (3) the 1024-entry studio map is equal;
//   (4) VpFrameAnalysis::Reduce (the production path, reading the owner) gives a Measured record exactly equal (every
//       histogram bin, every double, every count) to the oracle's decode + map through the same Reducer.
// The oracle lives ONLY in this test. CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

#include <AnalysisLumaSource.h>
#include <VP-V/modules/repair/grain_split/GrainSplit.h>
#include <VP-V/modules/input/source_analysis/VpFrameAnalysis.h>
#include <VP-V/tests/SyntheticAnalysisFrames.h>
#include <VP-V/modules/input/source_analysis/tests/DvWideReference.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace DecodeOwnerIdentityTestsNs
{
	using namespace SyntheticAnalysisFrames;

	// ---- ORACLE: the producer's private decode as it stood at ab32bc33b (VpFrameAnalysis.cpp 17-21, 23-28, 43-64,
	// 116-146). Frozen; do not "fix" it: it is the reference the owner must equal bit for bit. ----
	namespace Oracle
	{
		bool PlaneFormat(const AnalysisLumaSource& source)
		{
			return source.format == AnalysisLumaFormat::P010 || source.format == AnalysisLumaFormat::P210 ||
				source.format == AnalysisLumaFormat::DecodedDolbyVision422;
		}

		int RoundDivide(int64_t numerator, int64_t denominator)
		{
			return static_cast<int>(numerator >= 0 ? (numerator + denominator / 2) / denominator :
				-((-numerator + denominator / 2) / denominator));
		}

		bool SamplerCodesAreFullRange(const AnalysisLumaSource& source)
		{
			return source.CodesFullRange();
		}

		std::array<uint16_t, 1024> StudioLumaMap(const AnalysisLumaSource& source)
		{
			std::array<uint16_t, 1024> map{};
			const bool full = SamplerCodesAreFullRange(source);
			for (int code = 0; code < 1024; ++code)
				map[code] = static_cast<uint16_t>(full ?
					(std::max)(0, (std::min)(1023, 64 + RoundDivide(static_cast<int64_t>(code) * 876, 1023))) : code);
			return map;
		}

		bool NeedsDecode(const AnalysisLumaSource& source)
		{
			return source.IsValid() && !(PlaneFormat(source) && !source.fullRangeYuv);
		}

		bool DecodeSharedLuma(const AnalysisLumaSource& source)
		{
			AnalysisLumaPlane* plane = source.sharedLuma.get();
			if (!plane || !source.IsValid() || plane->Ready() || !plane->TryClaim())
				return false;
			const int width = source.width, height = source.height;
			plane->Bind(source);
			plane->codes.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
			for (int y = 0; y < height; ++y)
			{
				uint16_t* row = plane->codes.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
				if (source.SampleLumaRow(0, y, width, row))
					continue;
				for (int x = 0; x < width; ++x)
				{
					AnalysisLumaSample sample;
					if (!source.Sample(x, y, sample))
					{
						plane->codes.clear();
						plane->codes.shrink_to_fit();
						return false;
					}
					row[x] = sample.luma;
				}
			}
			plane->Publish();
			return true;
		}

		// The former Reduce over the oracle's plane / map: the same row reader and Reducer (not part of the moved decode).
		bool Reduce(const AnalysisLumaSource& source, const FrameAnalysis::Rect& picture, VpFrameAnalysis::Measured& out)
		{
			VpFrameAnalysis::Reducer reducer(picture, StudioLumaMap(source));
			VpFrameAnalysis::LumaRowReader reader(source);
			for (int y = picture.top; y < picture.bottom; ++y)
			{
				const uint16_t* row = reader.Row(y, picture.left, picture.Width());
				if (!row) return false;
				reducer.AddRow(y, row);
			}
			out = reducer.Finish();
			return true;
		}
	}

	std::wstring Name(Kind kind, bool limited, bool bt2020, int frame, const wchar_t* what)
	{
		return std::wstring(KindName(kind)) + (limited ? L" vp-v" : L" original") + (bt2020 ? L" bt2020" : L" bt709") +
			L" frame " + std::to_wstring(frame) + L": " + what;
	}

	void SameMeasured(const VpFrameAnalysis::Measured& a, const VpFrameAnalysis::Measured& b, const std::wstring& name)
	{
		Assert::IsTrue(a.luma.histogram == b.luma.histogram, (name + L" histogram").c_str());
		Assert::IsTrue(a.luma.samples == b.luma.samples, (name + L" samples").c_str());
		Assert::IsTrue(a.luma.mean == b.luma.mean, (name + L" mean").c_str());
		Assert::IsTrue(a.luma.apl == b.luma.apl, (name + L" apl").c_str());
		Assert::IsTrue(a.luma.minimum == b.luma.minimum && a.luma.maximum == b.luma.maximum, (name + L" extremes").c_str());
		Assert::IsTrue(a.luma.p1 == b.luma.p1 && a.luma.p50 == b.luma.p50 && a.luma.p99 == b.luma.p99, (name + L" percentiles").c_str());
		Assert::IsTrue(a.range.btb == b.range.btb && a.range.wtw == b.range.wtw, (name + L" btb/wtw").c_str());
		Assert::IsTrue(a.range.nearBlack == b.range.nearBlack && a.range.clipped == b.range.clipped, (name + L" near-black/clipped").c_str());
		// No grain comparison: jb2 (c359c921d, unit-5 grain split) retired the Reducer's CPU grain estimate, so Measured has
		// no grain field; grain is the grain split unit's product, not this reducer's. SameGrain below asserts it on that path.
	}

	// Grain on jb2's replacement path (coordinator ruling on WAIVER-C5-15C6-GRAIN-RETIRED): the grain split laws over the
	// owner's plane equal the same laws over the oracle's plane, every band, the Wiener share, the clean / grain planes and
	// every block level and energy, exactly. Both the production CPU law (Compute, v3) and the V2 A/B baseline (ComputeV2).
	GrainSplit::Plane GrainInput(const std::vector<uint16_t>& codes)
	{
		GrainSplit::Plane y(kWidth, kHeight);
		for (size_t i = 0; i < codes.size() && i < y.v.size(); ++i)
			y.v[i] = static_cast<double>(codes[i]);
		return y;
	}

	void SameSplit(const GrainSplit::Split& a, const GrainSplit::Split& b, const std::wstring& name)
	{
		Assert::IsTrue(a.w == b.w && a.h == b.h && a.bw == b.bw && a.bh == b.bh && a.B == b.B, (name + L" grain geometry").c_str());
		const GrainSplit::Plane* pa[] = { &a.base, &a.d1, &a.d2, &a.d3, &a.f, &a.s1, &a.s2, &a.clean, &a.grain };
		const GrainSplit::Plane* pb[] = { &b.base, &b.d1, &b.d2, &b.d3, &b.f, &b.s1, &b.s2, &b.clean, &b.grain };
		for (size_t i = 0; i < sizeof(pa) / sizeof(pa[0]); ++i)
			Assert::IsTrue(pa[i]->w == pb[i]->w && pa[i]->h == pb[i]->h && pa[i]->v == pb[i]->v,
				(name + L" grain plane " + std::to_wstring(i)).c_str());
		Assert::IsTrue(a.N == b.N, (name + L" grain level N").c_str());
		Assert::IsTrue(a.e1 == b.e1 && a.e2 == b.e2 && a.e3 == b.e3, (name + L" grain block energies").c_str());
		Assert::IsFalse(a.N.empty(), (name + L" grain has blocks").c_str());
	}

	void SameGrain(const std::vector<uint16_t>& oracle, const std::vector<uint16_t>& owner, const std::wstring& name)
	{
		const GrainSplit::Plane yo = GrainInput(oracle), yw = GrainInput(owner);
		SameSplit(GrainSplit::Compute(yo, 1.0, 16), GrainSplit::Compute(yw, 1.0, 16), name + L" (v3)");
		SameSplit(GrainSplit::ComputeV2(yo, 1.0, 16, GrainSplit::ParamsV2()), GrainSplit::ComputeV2(yw, 1.0, 16, GrainSplit::ParamsV2()),
			name + L" (V2)");
	}

	TEST_CLASS(DecodeOwnerIdentityTests)
	{
	public:
		TEST_METHOD(OwnerDecodeAndMeasuredAreBitIdenticalToTheFormerPrivateDecode)
		{
			// The brief's fixtures first (R210 / R12B / R12L / BGRA / full-range YUV), then every other kind.
			const Kind named[] = { Kind::R210, Kind::R12B, Kind::R12L, Kind::Bgra, Kind::P010Full, Kind::DecodedDv422Full,
				Kind::V210Full };
			std::vector<Kind> kinds(named, named + sizeof(named) / sizeof(named[0]));
			for (int k = 0; k < static_cast<int>(Kind::Count); ++k)
			{
				bool listed = false;
				for (Kind n : named) listed = listed || n == static_cast<Kind>(k);
				if (!listed) kinds.push_back(static_cast<Kind>(k));
			}
			int decoded = 0, fullMaps = 0;
			for (Kind kind : kinds)
				for (int pathway = 0; pathway < 2; ++pathway)
					for (int matrix = 0; matrix < 2; ++matrix)
						for (int frame : { 0, 7 })
						{
							const bool limited = pathway == 0, bt2020 = matrix == 1;
							Frame fixture = Make(kind, frame);
							fixture.source.limitedDomain = limited;
							if (bt2020) fixture.source.colorspace = ColorSpace::BT_2020;
							const AnalysisLumaSource base = fixture.source;
							Assert::IsTrue(base.IsValid(), Name(kind, limited, bt2020, frame, L"valid").c_str());

							// (1) the same formats need a plane.
							const bool needs = Oracle::NeedsDecode(base);
							Assert::AreEqual(needs, Vpv::AnalysisDomain::NeedsLumaPlane(base),
								Name(kind, limited, bt2020, frame, L"needs a plane").c_str());

							// (3) the studio map, every code.
							const auto oracleMap = Oracle::StudioLumaMap(base);
							Assert::IsTrue(oracleMap == Vpv::AnalysisDomain::StudioLumaMap(base),
								Name(kind, limited, bt2020, frame, L"studio map").c_str());
							Assert::IsTrue(oracleMap == VpFrameAnalysis::StudioLumaMap(base),
								Name(kind, limited, bt2020, frame, L"producer's studio map").c_str());
							fullMaps += Oracle::SamplerCodesAreFullRange(base) ? 1 : 0;

							AnalysisLumaSource oracle = base;
							AnalysisLumaSource owner = base;
							if (needs)
							{
								// (2) byte-equal planes.
								oracle.sharedLuma = std::make_shared<AnalysisLumaPlane>();
								owner.sharedLuma = std::make_shared<AnalysisLumaPlane>();
								const bool a = Oracle::DecodeSharedLuma(oracle);
								const bool b = Vpv::AnalysisDomain::DecodeSharedLuma(owner);
								Assert::AreEqual(a, b, Name(kind, limited, bt2020, frame, L"decode result").c_str());
								Assert::IsTrue(a, Name(kind, limited, bt2020, frame, L"decoded").c_str());
								const std::vector<uint16_t>& x = oracle.sharedLuma->codes;
								const std::vector<uint16_t>& y = owner.sharedLuma->codes;
								Assert::IsTrue(x.size() == y.size() && x.size() == static_cast<size_t>(kWidth) * kHeight,
									Name(kind, limited, bt2020, frame, L"plane size").c_str());
								Assert::IsTrue(std::memcmp(x.data(), y.data(), x.size() * sizeof(uint16_t)) == 0,
									Name(kind, limited, bt2020, frame, L"plane bytes").c_str());
								SameGrain(x, y, Name(kind, limited, bt2020, frame, L"grain split"));
								Assert::IsNotNull(owner.SharedLumaRow(0), Name(kind, limited, bt2020, frame, L"published").c_str());
								Assert::IsFalse(Vpv::AnalysisDomain::DecodeSharedLuma(owner),
									Name(kind, limited, bt2020, frame, L"decoded once").c_str());
								++decoded;
							}

							// (4) Measured through the production path equals the oracle's, letterboxed and full raster.
							const FrameAnalysis::Rect pictures[] = { { 0, kBar, kWidth, kHeight - kBar }, { 0, 0, kWidth, kHeight } };
							for (const FrameAnalysis::Rect& picture : pictures)
							{
								VpFrameAnalysis::Measured want, got;
								// WAIVER-S21-O3: the packed DV tunnel is measured on its 12-bit tap (DvTunnelTapLaw code12 = Intensity >> 4,
								// tap16 = tap12 << 4, a 12-bit sample is Q6 on the 10-bit axis); the frozen ten-bit oracle does not apply to it.
								if (kind == Kind::PackedDvR210)
									Assert::IsTrue(DvWideReference::Reduce12(oracle, picture, !DvWideReference::MapIsIdentity(oracleMap), want.luma, want.range),
										Name(kind, limited, bt2020, frame, L"oracle reduce").c_str());
								else
									Assert::IsTrue(Oracle::Reduce(oracle, picture, want), Name(kind, limited, bt2020, frame, L"oracle reduce").c_str());
								Assert::IsTrue(VpFrameAnalysis::Reduce(owner, picture, got), Name(kind, limited, bt2020, frame, L"reduce").c_str());
								SameMeasured(want, got, Name(kind, limited, bt2020, frame, L"measured"));
							}
						}
			// The fixtures exercise both laws: decoded planes and the full -> studio map (Original pathway RGB).
			Assert::IsTrue(decoded > 0);
			Assert::IsTrue(fullMaps > 0);
		}

		TEST_METHOD(StudioMapIsTheOneFullToLimitedLawForEveryCode)
		{
			// The owner's map under CodesFullRange is FullLumaToLimited(code, 1) for all 1024 codes, equal to the oracle's
			// own expression; with studio codes it is the identity.
			Frame original = Make(Kind::Bgra, 0);
			original.source.limitedDomain = false;
			Assert::IsTrue(original.source.CodesFullRange());
			const auto map = Vpv::AnalysisDomain::StudioLumaMap(original.source);
			const auto oracle = Oracle::StudioLumaMap(original.source);
			for (int code = 0; code < 1024; ++code)
			{
				Assert::AreEqual(oracle[code], map[code]);
				Assert::AreEqual(Vpv::AnalysisDomain::FullLumaToLimited(code, 1), map[code]);
			}
			Assert::AreEqual(64, static_cast<int>(map[0]));
			Assert::AreEqual(940, static_cast<int>(map[1023]));
			const Frame vp = Make(Kind::Bgra, 0);
			Assert::IsFalse(vp.source.CodesFullRange());
			const auto identity = Vpv::AnalysisDomain::StudioLumaMap(vp.source);
			for (int code = 0; code < 1024; ++code)
				Assert::AreEqual(static_cast<uint16_t>(code), identity[code]);
		}

		TEST_METHOD(UnreadableSourceIsNeverPublished)
		{
			// Same failure behaviour as the oracle: an invalid source neither needs nor gets a plane.
			Frame fixture = Make(Kind::R210, 0);
			AnalysisLumaSource broken = fixture.source;
			broken.dataBytes = 16;   // far too small for the raster
			Assert::AreEqual(Oracle::NeedsDecode(broken), Vpv::AnalysisDomain::NeedsLumaPlane(broken));
			broken.sharedLuma = std::make_shared<AnalysisLumaPlane>();
			AnalysisLumaSource broken2 = broken;
			broken2.sharedLuma = std::make_shared<AnalysisLumaPlane>();
			Assert::AreEqual(Oracle::DecodeSharedLuma(broken2), Vpv::AnalysisDomain::DecodeSharedLuma(broken));
			Assert::IsFalse(broken.sharedLuma->Ready());
		}
	};
}
