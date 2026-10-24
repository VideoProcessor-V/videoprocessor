#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/ExtendedRange.h>

#include <libplacebo/colorspace.h>

#include <cmath>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Audit 2026-10-16: the pure math of the widened BTB/WTW carry
// (ExtendedRange.h, 1.-4.): the transfers' odd extensions and analytic
// continuation above 1, the calibration LUT's edge extrapolation on synthetic
// LUTs, the linear-light remainder through a primaries matrix, and Decide()
// per case with each switch on and off. GPU behaviour is not covered here.
namespace VideoProcessorTest
{
	namespace
	{
		namespace ErCarry
		{
			using ExtendedRange::Transfer;
			using ExtendedRange::TransferKind;

			std::vector<Transfer> Transfers()
			{
				return {
					{ TransferKind::Power, 1.8f }, { TransferKind::Power, 2.2f }, { TransferKind::Power, 2.4f },
					{ TransferKind::Power, 2.8f }, { TransferKind::Bt1886, 0.0f }, { TransferKind::Bt1886, 0.0596f },
					{ TransferKind::Bt1886, 0.2f }, { TransferKind::Srgb, 0.0f }, { TransferKind::Identity, 0.0f } };
			}

			std::wstring Name(const Transfer& t)
			{
				const std::string n = ExtendedRange::TransferName(t);
				return std::wstring(n.begin(), n.end()) + L" " + std::to_wstring(t.param);
			}

			pl_color_space Space(enum pl_color_primaries primaries, enum pl_color_transfer transfer)
			{
				pl_color_space c{};
				c.primaries = primaries;
				c.transfer = transfer;
				return c;
			}

			// A synthetic LUT: size n per axis, out = f(r, g, b).
			template <typename F>
			std::vector<float> MakeLut(int n, F f)
			{
				std::vector<float> data(static_cast<size_t>(n) * n * n * 3);
				for (int b = 0; b < n; ++b)
					for (int g = 0; g < n; ++g)
						for (int r = 0; r < n; ++r)
						{
							float out[3];
							f(r / float(n - 1), g / float(n - 1), b / float(n - 1), out);
							const size_t i = ((static_cast<size_t>(b) * n + g) * n + r) * 3;
							data[i] = out[0];
							data[i + 1] = out[1];
							data[i + 2] = out[2];
						}
				return data;
			}

			ExtendedRange::LutView View(const std::vector<float>& data, int n)
			{
				ExtendedRange::LutView v;
				v.size[0] = v.size[1] = v.size[2] = n;
				v.data = data.data();
				return v;
			}

			// An affine LUT with cross-talk: out = A p + t.
			void Affine(float r, float g, float b, float out[3])
			{
				out[0] = 0.02f + 0.90f * r + 0.05f * g + 0.01f * b;
				out[1] = 0.01f + 0.03f * r + 0.92f * g + 0.02f * b;
				out[2] = 0.03f + 0.02f * r + 0.04f * g + 0.88f * b;
			}

			pl_custom_lut PlainLut(const std::vector<float>& data, int n)
			{
				pl_custom_lut lut{};
				lut.signature = 0x1234;
				lut.size[0] = lut.size[1] = lut.size[2] = n;
				lut.data = data.data();
				return lut;
			}
		}
	}

	TEST_CLASS(ExtendedRangeCarryTests)
	{
	public:
		// 1: f(-x) = -f(x) and FromLinear(ToLinear(v)) = v from -0.5 to 1.6.
		TEST_METHOD(TransferOddExtensionsRoundTrip)
		{
			using namespace ErCarry;
			for (const Transfer& t : Transfers())
				for (int i = -500; i <= 1600; ++i)
				{
					const double v = i / 1000.0;
					const double l = ExtendedRange::ToLinear(t, v);
					Assert::AreEqual(-l, ExtendedRange::ToLinear(t, -v), 1e-12, Name(t).c_str());
					Assert::AreEqual(v, ExtendedRange::FromLinear(t, l), 1e-9, Name(t).c_str());
					Assert::AreEqual(-ExtendedRange::FromLinear(t, l), ExtendedRange::FromLinear(t, -l), 1e-12, Name(t).c_str());
				}
		}

		// 1: E(0) = 0, E(1) = 1 (relative linear light).
		TEST_METHOD(TransferEndpointsAreRelative)
		{
			using namespace ErCarry;
			for (const Transfer& t : Transfers())
			{
				Assert::AreEqual(0.0, ExtendedRange::ToLinear(t, 0.0), 1e-12, Name(t).c_str());
				Assert::AreEqual(1.0, ExtendedRange::ToLinear(t, 1.0), 1e-9, Name(t).c_str());
			}
		}

		// 1: WTW strictly increasing above 1 (and BTB below 0), continuous and
		// C1 across 1 (the analytic form continued: equal one-sided slopes).
		TEST_METHOD(TransferMonotonicAboveOneAndBelowZero)
		{
			using namespace ErCarry;
			for (const Transfer& t : Transfers())
			{
				double previous = ExtendedRange::ToLinear(t, -0.5);
				for (int i = -499; i <= 2000; ++i)
				{
					const double l = ExtendedRange::ToLinear(t, i / 1000.0);
					Assert::IsTrue(l > previous, Name(t).c_str());
					previous = l;
				}
				const double h = 1e-6;
				const double left = (ExtendedRange::ToLinear(t, 1.0) - ExtendedRange::ToLinear(t, 1.0 - h)) / h;
				const double right = (ExtendedRange::ToLinear(t, 1.0 + h) - ExtendedRange::ToLinear(t, 1.0)) / h;
				Assert::AreEqual(left, right, 1e-3 * left, Name(t).c_str());
				// The inverse is monotonic as well.
				double previousV = ExtendedRange::FromLinear(t, -0.5);
				for (int i = -499; i <= 3000; ++i)
				{
					const double v = ExtendedRange::FromLinear(t, i / 1000.0);
					Assert::IsTrue(v > previousV, Name(t).c_str());
					previousV = v;
				}
			}
		}

		// 1: BT.1886 and sRGB are C1 across 0 too (odd extension of a curve
		// with a finite non-zero slope at 0).
		TEST_METHOD(PiecewiseTransfersAreC1AtZero)
		{
			using namespace ErCarry;
			for (const Transfer& t : { Transfer{ TransferKind::Bt1886, 0.0596f }, Transfer{ TransferKind::Srgb, 0.0f } })
			{
				const double h = 1e-7;
				const double right = ExtendedRange::ToLinear(t, h) / h;
				const double left = -ExtendedRange::ToLinear(t, -h) / h;
				Assert::IsTrue(right > 0.0, Name(t).c_str());
				Assert::AreEqual(right, left, 1e-9, Name(t).c_str());
			}
		}

		// 1: in range, the relative curves are libplacebo's own (its
		// linearize, black-scaled: (L(v) - L(0)) / (L(1) - L(0))).
		TEST_METHOD(InRangeMatchesLibplaceboLinearize)
		{
			using namespace ErCarry;
			for (enum pl_color_transfer trc : { PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_SRGB, PL_COLOR_TRC_GAMMA22,
				PL_COLOR_TRC_GAMMA24 })
			{
				pl_color_space c = Space(PL_COLOR_PRIM_BT_709, trc);
				c.hdr.min_luma = PL_COLOR_SDR_WHITE / 1000.0f;
				c.hdr.max_luma = PL_COLOR_SDR_WHITE;
				Transfer t;
				Assert::IsTrue(ExtendedRange::DescribeTransfer(c, true, t));
				float lo[3] = { 0, 0, 0 }, hi[3] = { 1, 1, 1 };
				pl_color_linearize(&c, lo);
				pl_color_linearize(&c, hi);
				for (int i = 0; i <= 100; ++i)
				{
					float x[3] = { i / 100.0f, i / 100.0f, i / 100.0f };
					pl_color_linearize(&c, x);
					const double relative = (x[0] - lo[0]) / double(hi[0] - lo[0]);
					Assert::AreEqual(relative, ExtendedRange::ToLinear(t, i / 100.0), 2e-5, Name(t).c_str());
				}
			}
		}

		TEST_METHOD(PiecewiseSwitchGatesDescribeTransfer)
		{
			using namespace ErCarry;
			Transfer t;
			for (enum pl_color_transfer trc : { PL_COLOR_TRC_BT_1886, PL_COLOR_TRC_SRGB })
			{
				Assert::IsFalse(ExtendedRange::DescribeTransfer(Space(PL_COLOR_PRIM_BT_709, trc), false, t));
				Assert::IsTrue(ExtendedRange::DescribeTransfer(Space(PL_COLOR_PRIM_BT_709, trc), true, t));
			}
			Assert::IsTrue(ExtendedRange::DescribeTransfer(Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), false, t));
			Assert::IsTrue(t.kind == TransferKind::Power);
			Assert::AreEqual(2.4f, t.param);
			Assert::IsFalse(ExtendedRange::DescribeTransfer(Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), true, t));
			Assert::IsFalse(ExtendedRange::DescribeTransfer(Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_HLG), true, t));
		}

		// 3: an affine LUT is reproduced exactly outside 0..1 (the edge slope
		// of a linear LUT is its matrix), and inside it is the LUT.
		TEST_METHOD(LutExtrapolationIsExactForAnAffineLut)
		{
			using namespace ErCarry;
			for (int n : { 2, 9, 17, 33 })
			{
				const std::vector<float> data = MakeLut(n, Affine);
				const ExtendedRange::LutView lut = View(data, n);
				const float probes[][3] = { { 1.08f, 0.5f, 0.2f }, { -0.07f, 0.3f, 0.9f }, { 1.1f, 1.05f, -0.06f },
					{ -0.1f, -0.1f, -0.1f }, { 1.09f, 1.09f, 1.09f }, { 0.25f, 0.75f, 0.5f }, { 0.0f, 1.0f, 0.5f } };
				for (const auto& v : probes)
				{
					float expected[3], got[3];
					Affine(v[0], v[1], v[2], expected);
					ExtendedRange::LutExtrapolate(lut, v, got);
					for (int k = 0; k < 3; ++k)
						Assert::AreEqual(expected[k], got[k], 1e-5f);
				}
			}
		}

		// 3: a non-linear monotonic LUT: continuous at the edge, the slope is
		// the edge cell's, BTB/WTW monotonic, the in-range part untouched.
		TEST_METHOD(LutExtrapolationContinuesTheEdgeCell)
		{
			using namespace ErCarry;
			const int n = 9;
			const auto curve = [](float r, float g, float b, float out[3])
			{
				out[0] = std::pow(r, 1.0f / 1.1f) * 0.95f + 0.02f * g;
				out[1] = std::pow(g, 1.05f) * 0.97f + 0.01f * b;
				out[2] = std::pow(b, 0.9f) * 0.93f + 0.015f * r;
			};
			const std::vector<float> data = MakeLut(n, curve);
			const ExtendedRange::LutView lut = View(data, n);
			const float g = 0.5f, b = 0.25f;
			// Continuity at both edges.
			for (float edge : { 0.0f, 1.0f })
			{
				const float at[3] = { edge, g, b };
				const float nearEdge[3] = { edge + (edge > 0.5f ? 1e-5f : -1e-5f), g, b };
				float a[3], c[3];
				ExtendedRange::LutSample(lut, at, a);
				ExtendedRange::LutExtrapolate(lut, nearEdge, c);
				for (int k = 0; k < 3; ++k)
					Assert::AreEqual(a[k], c[k], 1e-4f);
			}
			// The slope above 1 is the last cell's difference along R.
			{
				const float top[3] = { 1.0f, g, b }, inner[3] = { 1.0f - 1.0f / (n - 1), g, b };
				float lt[3], li[3], slope[3];
				ExtendedRange::LutSample(lut, top, lt);
				ExtendedRange::LutSample(lut, inner, li);
				ExtendedRange::LutEdgeSlope(lut, top, 0, true, slope);
				for (int k = 0; k < 3; ++k)
					Assert::AreEqual((lt[k] - li[k]) * (n - 1), slope[k], 1e-4f);
				const float over[3] = { 1.1f, g, b };
				float out[3];
				ExtendedRange::LutExtrapolate(lut, over, out);
				for (int k = 0; k < 3; ++k)
					Assert::AreEqual(lt[k] + 0.1f * slope[k], out[k], 1e-5f);
			}
			// Monotonic in R from -0.1 to 1.1 (the LUT is increasing in R).
			float previous = -1e9f;
			for (int i = -100; i <= 1100; ++i)
			{
				const float v[3] = { i / 1000.0f, g, b };
				float out[3];
				ExtendedRange::LutExtrapolate(lut, v, out);
				Assert::IsTrue(out[0] > previous);
				previous = out[0];
			}
			// In range: exactly the trilinear sample.
			const float inside[3] = { 0.3f, 0.6f, 0.9f };
			float s[3], e[3];
			ExtendedRange::LutSample(lut, inside, s);
			ExtendedRange::LutExtrapolate(lut, inside, e);
			for (int k = 0; k < 3; ++k)
				Assert::AreEqual(s[k], e[k]);
		}

		TEST_METHOD(LutCarryableOnlyForPlain3dLuts)
		{
			using namespace ErCarry;
			const std::vector<float> data = MakeLut(5, Affine);
			pl_custom_lut lut = PlainLut(data, 5);
			Assert::IsTrue(ExtendedRange::LutCarryable(&lut));
			Assert::IsFalse(ExtendedRange::LutCarryable(nullptr));
			pl_custom_lut oneD = lut;
			oneD.size[1] = oneD.size[2] = 0;
			Assert::IsFalse(ExtendedRange::LutCarryable(&oneD));
			pl_custom_lut shaper = lut;
			shaper.shaper_in.m[0][0] = 1.0f;
			Assert::IsFalse(ExtendedRange::LutCarryable(&shaper));
			pl_custom_lut empty = lut;
			empty.data = nullptr;
			Assert::IsFalse(ExtendedRange::LutCarryable(&empty));
		}

		// 2: the linear remainder through the matrix, added in the output's
		// linear light after the in-range part was converted, gives the whole
		// value's conversion (when the gamut mapping leaves the clipped part
		// alone): the remainder path is in linear light where M applies.
		TEST_METHOD(LinearRemainderThroughTheMatrixRebuildsTheConversion)
		{
			using namespace ErCarry;
			const Transfer t{ TransferKind::Bt1886, 0.0596f };
			const pl_matrix3x3 m = pl_get_color_mapping_matrix(pl_raw_primaries_get(PL_COLOR_PRIM_BT_2020),
				pl_raw_primaries_get(PL_COLOR_PRIM_BT_709), PL_INTENT_RELATIVE_COLORIMETRIC);
			const double values[][3] = { { 1.06, 0.4, -0.05 }, { -0.07, 0.2, 0.3 }, { 1.09, 1.09, 1.09 }, { 0.5, 1.04, 0.1 } };
			for (const auto& v : values)
			{
				double full[3], clippedLinear[3], remainder[3];
				for (int k = 0; k < 3; ++k)
				{
					const double c = v[k] < 0.0 ? 0.0 : v[k] > 1.0 ? 1.0 : v[k];
					full[k] = ExtendedRange::ToLinear(t, v[k]);
					clippedLinear[k] = ExtendedRange::ToLinear(t, c);
					remainder[k] = full[k] - clippedLinear[k];
				}
				for (int row = 0; row < 3; ++row)
				{
					double expected = 0.0, inRange = 0.0, extra = 0.0;
					for (int column = 0; column < 3; ++column)
					{
						expected += m.m[row][column] * full[column];
						inRange += m.m[row][column] * clippedLinear[column];
						extra += m.m[row][column] * remainder[column];
					}
					// libplacebo's encoded in-range result, then the late restore.
					const double encoded = ExtendedRange::FromLinear(t, inRange);
					const double restored = ExtendedRange::FromLinear(t, ExtendedRange::ToLinear(t, encoded) + extra);
					Assert::AreEqual(ExtendedRange::FromLinear(t, expected), restored, 1e-6);
				}
			}
		}

		// Decide: the old cases with every switch off are the old decisions.
		TEST_METHOD(DecideAllSwitchesOffKeepsTheOldEligibility)
		{
			using namespace ErCarry;
			const ExtendedRange::Settings off{ false, false, false, false };
			using R = ExtendedRange::Reason;
			struct Case { pl_color_space source, output; bool lut; R expected; };
			const std::vector<float> data = MakeLut(5, Affine);
			const pl_custom_lut lut = PlainLut(data, 5);
			const Case cases[] = {
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), false, R::Active },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA22), false, R::Active },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886), Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), false, R::SourceTransfer },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_SRGB), false, R::OutputTransfer },
				{ Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA22), false, R::SourcePrimaries },
				{ Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA22), false, R::Active },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_BT_601_525, PL_COLOR_TRC_GAMMA22), false, R::OutputPrimaries },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24), Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), true, R::CalibrationLut },
				{ Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), false, R::HdrSource },
				{ Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), false, R::HdrSource },
				{ Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ), false, R::HdrOutput },
			};
			int index = 0;
			for (const Case& c : cases)
			{
				const ExtendedRange::Decision d = ExtendedRange::Decide(true, off, c.source, c.output,
					c.lut ? &lut : nullptr, PL_LUT_NORMALIZED);
				Assert::IsTrue(d.reason == c.expected, (L"case " + std::to_wstring(index)).c_str());
				if (d.Active())
					Assert::IsFalse(d.LateRestore() || d.libplaceboMapsPrimaries || d.gamutClip || d.hdrPassthrough);
				++index;
			}
			Assert::IsTrue(ExtendedRange::Decide(false, ExtendedRange::Settings{}, cases[0].source, cases[0].output,
				nullptr, PL_LUT_UNKNOWN).reason == R::Off);
		}

		// 1 on: BT.1886 / sRGB active, transfers described, gamma converted
		// only when the two differ.
		TEST_METHOD(DecidePiecewiseTransfers)
		{
			using namespace ErCarry;
			const ExtendedRange::Settings on{};
			ExtendedRange::Settings off{};
			off.piecewiseTransfers = false;
			const pl_color_space s1886 = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_BT_1886);
			const pl_color_space o24 = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
			const pl_color_space oSrgb = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_SRGB);
			ExtendedRange::Decision d = ExtendedRange::Decide(true, on, s1886, o24, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsTrue(d.source.kind == TransferKind::Bt1886);
			Assert::IsTrue(d.output.kind == TransferKind::Power);
			Assert::IsTrue(d.convertGamma);
			Assert::IsFalse(d.convertPrimaries);
			Assert::IsTrue(d.ScaledRestore());
			Assert::IsFalse(d.LateRestore());
			d = ExtendedRange::Decide(true, on, s1886, s1886, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsFalse(d.convertGamma);
			d = ExtendedRange::Decide(true, on, o24, oSrgb, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsTrue(d.output.kind == TransferKind::Srgb);
			Assert::IsTrue(ExtendedRange::Decide(true, off, s1886, o24, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::SourceTransfer);
			Assert::IsTrue(ExtendedRange::Decide(true, off, o24, oSrgb, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::OutputTransfer);
		}

		// 2 on: a contained gamut converts in the carry; a wider one keeps
		// libplacebo's gamut mapping (late linear restore, no SCALED restore);
		// with a LUT it clips in the carry instead.
		TEST_METHOD(DecideWideSourcePrimaries)
		{
			using namespace ErCarry;
			const ExtendedRange::Settings on{};
			ExtendedRange::Settings off{};
			off.wideSourcePrimaries = false;
			const pl_color_space p3 = Space(PL_COLOR_PRIM_DISPLAY_P3, PL_COLOR_TRC_GAMMA24);
			const pl_color_space ebu = Space(PL_COLOR_PRIM_BT_601_625, PL_COLOR_TRC_GAMMA24);
			const pl_color_space bt2020 = Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24);
			const pl_color_space bt709 = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);

			// Contained (not BT.709): BT.601-625 (EBU) inside BT.2020.
			ExtendedRange::Decision d = ExtendedRange::Decide(true, on, ebu, bt2020, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsTrue(d.convertPrimaries);
			Assert::IsFalse(d.libplaceboMapsPrimaries);
			Assert::IsTrue(ExtendedRange::Decide(true, off, ebu, bt2020, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::SourcePrimaries);
			// Audit 2026-10-16 (CPU run): Display P3 is NOT inside BT.2020 for
			// libplacebo - P3 red (0.680, 0.320) lies about 0.0017 above
			// BT.2020's red-green edge (y 0.318 at x 0.680), beyond
			// pl_primaries_superset's 1e-6 tolerance - so libplacebo gamut-maps
			// P3 -> BT.2020 itself and the carry must leave that to it (wide).
			d = ExtendedRange::Decide(true, on, p3, bt2020, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsFalse(d.convertPrimaries);
			Assert::IsTrue(d.libplaceboMapsPrimaries);
			Assert::IsTrue(ExtendedRange::Decide(true, off, p3, bt2020, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::SourcePrimaries);

			d = ExtendedRange::Decide(true, on, bt2020, bt709, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsFalse(d.convertPrimaries);
			Assert::IsTrue(d.libplaceboMapsPrimaries);
			Assert::IsTrue(d.sourcePrimaries == PL_COLOR_PRIM_BT_2020);
			Assert::IsTrue(d.LateRestore());
			Assert::IsFalse(d.ScaledRestore());
			// The matrix is BT.2020 -> BT.709 (column-major): pure 2020 green
			// has a negative red in BT.709.
			Assert::IsTrue(d.matrix[3 * 1 + 0] < 0.0f);
			Assert::IsTrue(ExtendedRange::Decide(true, off, bt2020, bt709, nullptr, PL_LUT_UNKNOWN).reason ==
				ExtendedRange::Reason::SourcePrimaries);

			const std::vector<float> data = MakeLut(5, Affine);
			const pl_custom_lut lut = PlainLut(data, 5);
			d = ExtendedRange::Decide(true, on, bt2020, bt709, &lut, PL_LUT_NORMALIZED);
			Assert::IsTrue(d.Active());
			Assert::IsTrue(d.convertPrimaries);
			Assert::IsTrue(d.gamutClip);
			Assert::IsFalse(d.libplaceboMapsPrimaries);
			Assert::IsTrue(d.LateRestore());
			Assert::IsTrue(d.ScaledRestore());

			// Prepare: the wide source keeps its primaries for libplacebo, no
			// SCALED restore, the late restore first, the carry last.
			ExtendedRange::Hooks hooks;
			d = ExtendedRange::Decide(true, on, bt2020, bt709, nullptr, PL_LUT_UNKNOWN);
			pl_color_space source = bt2020;
			std::vector<const pl_hook*> list;
			ExtendedRange::Prepare(d, hooks, source, bt709, list);
			Assert::IsTrue(source.primaries == PL_COLOR_PRIM_BT_2020);
			Assert::IsTrue(source.transfer == PL_COLOR_TRC_GAMMA22);
			Assert::AreEqual(size_t(2), list.size());
			Assert::IsTrue(list.front() == hooks.LateRestore());
			Assert::IsTrue(list.back() == hooks.Carry());
		}

		// 3 on / off.
		TEST_METHOD(DecideCalibrationLut)
		{
			using namespace ErCarry;
			const ExtendedRange::Settings on{};
			ExtendedRange::Settings off{};
			off.calibrationLut = false;
			const pl_color_space s = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
			const pl_color_space o = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			const std::vector<float> data = MakeLut(5, Affine);
			const pl_custom_lut lut = PlainLut(data, 5);
			ExtendedRange::Decision d = ExtendedRange::Decide(true, on, s, o, &lut, PL_LUT_NORMALIZED);
			Assert::IsTrue(d.Active());
			Assert::IsTrue(d.lut == &lut);
			Assert::IsTrue(d.LateRestore());
			Assert::IsTrue(d.ScaledRestore());
			Assert::IsTrue(ExtendedRange::Decide(true, off, s, o, &lut, PL_LUT_NORMALIZED).reason ==
				ExtendedRange::Reason::CalibrationLut);
			Assert::IsTrue(ExtendedRange::Decide(true, on, s, o, &lut, PL_LUT_CONVERSION).reason ==
				ExtendedRange::Reason::LutUnsupported);
			pl_custom_lut shaper = lut;
			shaper.shaper_out.m[1][1] = 1.0f;
			Assert::IsTrue(ExtendedRange::Decide(true, on, s, o, &shaper, PL_LUT_NORMALIZED).reason ==
				ExtendedRange::Reason::LutUnsupported);
			// The old form has no LUT data: still off with a LUT.
			Assert::IsTrue(ExtendedRange::Decide(true, s, o, true).reason == ExtendedRange::Reason::CalibrationLut);
			Assert::IsTrue(ExtendedRange::Decide(true, s, o, false).Active());

			ExtendedRange::Hooks hooks;
			pl_color_space source = s;
			std::vector<const pl_hook*> list;
			ExtendedRange::Prepare(d, hooks, source, o, list);
			Assert::AreEqual(size_t(3), list.size());
			Assert::IsTrue(list[0] == hooks.LateRestore());
			Assert::IsTrue(list[1] == hooks.Restore(true));
			Assert::IsTrue(list[2] == hooks.Carry());
			Assert::IsTrue(hooks.Owns(hooks.LateRestore()->signature));
		}

		// 4 on / off: only an identity mapping (equal colour spaces) carries.
		TEST_METHOD(DecideHdrPassthrough)
		{
			using namespace ErCarry;
			const ExtendedRange::Settings on{};
			ExtendedRange::Settings off{};
			off.hdrPassthrough = false;
			using R = ExtendedRange::Reason;
			pl_color_space pq = Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_PQ);
			pq.hdr.max_luma = 1000.0f;
			pq.hdr.min_luma = 0.005f;
			ExtendedRange::Decision d = ExtendedRange::Decide(true, on, pq, pq, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(d.Active());
			Assert::IsTrue(d.hdrPassthrough);
			Assert::IsFalse(d.convertGamma || d.convertPrimaries || d.libplaceboMapsPrimaries);
			Assert::IsTrue(d.source.kind == TransferKind::Identity);
			Assert::IsTrue(ExtendedRange::Decide(true, off, pq, pq, nullptr, PL_LUT_UNKNOWN).reason == R::HdrSource);
			// Dynamic tone mapping (peak detection) maps even equal colour spaces.
			ExtendedRange::Settings dynamic{};
			dynamic.hdrDynamicToneMapping = true;
			Assert::IsTrue(ExtendedRange::Decide(true, dynamic, pq, pq, nullptr, PL_LUT_UNKNOWN).reason == R::HdrMapped);
			pl_color_space display = pq;
			display.hdr.max_luma = 600.0f;   // tone mapping
			Assert::IsTrue(ExtendedRange::Decide(true, on, pq, display, nullptr, PL_LUT_UNKNOWN).reason == R::HdrMapped);
			const pl_color_space hlg = Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_HLG);
			Assert::IsTrue(ExtendedRange::Decide(true, on, hlg, hlg, nullptr, PL_LUT_UNKNOWN).Active());
			Assert::IsTrue(ExtendedRange::Decide(true, on, pq, Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22),
				nullptr, PL_LUT_UNKNOWN).reason == R::HdrSource);
			Assert::IsTrue(ExtendedRange::Decide(true, on, Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22), pq,
				nullptr, PL_LUT_UNKNOWN).reason == R::HdrOutput);
			// Prepare leaves the (equal) colour space alone.
			ExtendedRange::Hooks hooks;
			pl_color_space source = pq;
			std::vector<const pl_hook*> list;
			ExtendedRange::Prepare(d, hooks, source, pq, list);
			Assert::IsTrue(pl_color_space_equal(&source, &pq));
			Assert::AreEqual(size_t(2), list.size());
		}

		// Hunt 2 (08 #1): through the temporal renderer every frame is converted
		// once. Deinterlacing alone keeps every mode; libplacebo frame mixing
		// keeps the carry + SCALED restore (inside each frame's pass) and turns
		// a late restore off (it would read the last-rendered frame, not the mix).
		TEST_METHOD(ForTemporalKeepsPerFrameModesAndDropsLateRestoreUnderMixing)
		{
			using namespace ErCarry;
			using R = ExtendedRange::Reason;
			const ExtendedRange::Settings on{};
			const pl_color_space s = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA24);
			const pl_color_space o = Space(PL_COLOR_PRIM_BT_709, PL_COLOR_TRC_GAMMA22);
			const ExtendedRange::Decision scaled = ExtendedRange::Decide(true, on, s, o, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(scaled.Active() && !scaled.LateRestore());
			Assert::IsTrue(ExtendedRange::ForTemporal(scaled, false).Active());
			Assert::IsTrue(ExtendedRange::ForTemporal(scaled, true).Active());

			const std::vector<float> data = MakeLut(5, Affine);
			const pl_custom_lut lut = PlainLut(data, 5);
			const ExtendedRange::Decision withLut = ExtendedRange::Decide(true, on, s, o, &lut, PL_LUT_NORMALIZED);
			Assert::IsTrue(withLut.Active() && withLut.LateRestore());
			Assert::IsTrue(ExtendedRange::ForTemporal(withLut, false).Active());
			Assert::IsTrue(ExtendedRange::ForTemporal(withLut, true).reason == R::TemporalMix);

			// Wide source gamut: libplacebo maps the primaries, linear remainder restored late.
			const pl_color_space wide = Space(PL_COLOR_PRIM_BT_2020, PL_COLOR_TRC_GAMMA24);
			const ExtendedRange::Decision mapped = ExtendedRange::Decide(true, on, wide, o, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(mapped.Active() && mapped.libplaceboMapsPrimaries);
			Assert::IsTrue(ExtendedRange::ForTemporal(mapped, false).Active());
			Assert::IsTrue(ExtendedRange::ForTemporal(mapped, true).reason == R::TemporalMix);

			// An inactive decision keeps its reason; the off text names the cause.
			const ExtendedRange::Decision off = ExtendedRange::Decide(false, on, s, o, nullptr, PL_LUT_UNKNOWN);
			Assert::IsTrue(ExtendedRange::ForTemporal(off, true).reason == R::Off);
			Assert::IsTrue(ExtendedRange::Describe(ExtendedRange::Decision{ R::TemporalMix }).find("frame mixing") != std::string::npos);

			// Under mixing a TemporalMix decision prepares nothing: libplacebo keeps the source label.
			ExtendedRange::Hooks hooks;
			pl_color_space source = s;
			std::vector<const pl_hook*> list;
			ExtendedRange::Prepare(ExtendedRange::ForTemporal(withLut, true), hooks, source, o, list);
			Assert::IsTrue(pl_color_space_equal(&source, &s));
			Assert::AreEqual(size_t(0), list.size());
		}

		// Efficiency audit 2026-10-24 #7: the one-pass carry is the old body
		// verbatim up to the clamp, then exactly the old ER_REMAINDER
		// expressions (stored) and the old clipped assignment. Byte identity
		// on the GPU: docs/design/efficiency-byte-identical-tests.md (#7).
		TEST_METHOD(OnePassCarryKeepsBothPassesExpressions)
		{
			const std::string old = ExtendedRange::CarryBody();
			const std::string one = ExtendedRange::CarryOnePassBody();
			const size_t split = old.find("#ifdef ER_REMAINDER");
			Assert::IsTrue(split != std::string::npos);
			Assert::AreEqual(old.substr(0, split), one.substr(0, split));
			const std::string tail = one.substr(split);
			Assert::IsTrue(tail.find("#ifdef") == std::string::npos && tail.find("#else") == std::string::npos);
			for (const char* expr : { "er_to_linear(color.rgb, er_out_kind, er_out_param) -",
				"er_to_linear(er_clipped, er_out_kind, er_out_param)", "color.rgb - er_clipped",
				"if (er_linear_remainder > 0.5)" })
			{
				Assert::IsTrue(old.find(expr, split) != std::string::npos);
				Assert::IsTrue(tail.find(expr) != std::string::npos);
			}
			Assert::IsTrue(tail.find("imageStore(er_remainder_out, ivec2(gl_FragCoord.xy), vec4(er_rem, 1.0));") != std::string::npos);
			// The clipped colour is the last statement, as in the old clipped pass.
			const std::string last = "color.rgb = er_clipped;\n";
			Assert::IsTrue(tail.size() >= last.size() && tail.compare(tail.size() - last.size(), last.size(), last) == 0);

			ExtendedRange::Hooks hooks;
			Assert::IsTrue(hooks.CarryPasses() == ExtendedRange::CarryPassReason::None);
			Assert::IsTrue(hooks.CarryPassesDescribe().find("one pass") != std::string::npos);
			Assert::IsTrue(std::string(ExtendedRange::CarryPassReasonText(ExtendedRange::CarryPassReason::OnePassFailed)).find("two passes") == 0);
		}
	};
}
