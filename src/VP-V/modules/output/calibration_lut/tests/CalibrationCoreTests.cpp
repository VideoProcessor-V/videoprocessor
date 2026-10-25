#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/modules/output/calibration_lut/calibration/CalibrationOutputs.h>
#include <VP-V/modules/output/calibration_lut/calibration/DisplayModel.h>
#include <VP-V/modules/output/calibration_lut/calibration/LedCharacterize.h>
#include <VP-V/modules/output/calibration_lut/calibration/PatternPlan.h>
#include <VP-V/modules/output/calibration_lut/calibration/SimulatedDisplay.h>
#include <VP-V/modules/output/calibration_lut/calibration/SimulatedMeter.h>
#include <ConfigEditorCore.h>
#include <led/LedDriveConfig.h>
#include <vprenderer/ExtendedRange.h>
#include <vprenderer/LibplaceboDisplayLut.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Calibration;

// Automatic calibration, the pure core (docs/VP-auto-calibration-design.md,
// "Tests"): a simulated display measured by a simulated meter through the
// real patch plan; the fit, target, inverse, LUT, .cube (through VP's own
// loader), LutExtrapolate end slopes, LED fits, outputs and key-only config
// edits. CPU only.
namespace
{
	std::vector<Measurement> Measure(const std::vector<Patch>& plan, const SimulatedDisplay& display,
		SimulatedMeter& meter)
	{
		std::vector<Measurement> out;
		for (const Patch& p : plan)
		{
			if (p.space != PatchSpace::Device || !p.usedForFit) continue;
			Measurement m;
			m.device = { p.rgb[0], p.rgb[1], p.rgb[2] };
			m.kind = p.kind;
			m.xyz = meter.Read(display.Emit(m.device)).xyz;
			out.push_back(m);
		}
		return out;
	}

	SimulatedMeterParams MeterParams()
	{
		SimulatedMeterParams p;
		p.matrixError = SimulatedMeter::MatrixError(0.01, 7u);
		p.proportionalNoise = 0.001;   // an i1Display-class repeatability
		p.seed = 11u;
		return p;
	}

	struct Calibrated
	{
		SimulatedDisplay display;
		SimulatedMeter meter;
		DisplayModel model;
		CalibrationTarget target[3];
		Lut3D lut[3];
		double lutSeconds[3] = { 0, 0, 0 };

		explicit Calibrated(const SimulatedDisplayParams& params, PlanKind plan = PlanKind::Standard, int lutSize = 33)
			: display(params), meter(MeterParams())
		{
			const std::vector<Measurement> m = Measure(BuildPlan(plan), display, meter);
			std::string error;
			Assert::IsTrue(model.Fit(m, ForwardModelOptions(), &error), std::wstring(error.begin(), error.end()).c_str());
			for (int s = 0; s < 3; ++s)
			{
				TargetOptions options;
				options.primaries = SlotPrimaries(static_cast<LutSlot>(s));
				options.transfer = TargetTransfer::Bt1886;
				Assert::IsTrue(target[s].Build(model, options, &error));
				LutOptions lo;
				lo.size = lutSize;
				const auto t0 = std::chrono::steady_clock::now();
				lut[s] = BuildLut(model, target[s], lo);
				lutSeconds[s] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			}
		}

		// What the meter would read without noise.
		Vec3 Ideal(const Vec3& device) const { return meter.Ideal(display.Emit(device)); }
	};

	const Calibrated& Standard()
	{
		static std::unique_ptr<Calibrated> c(new Calibrated(SimulatedDisplayParams()));
		return *c;
	}

	double DeltaE(const Vec3& a, const Vec3& b, const Vec3& white)
	{
		return DeltaE2000(XyzToLab(a, white), XyzToLab(b, white));
	}

	ExtendedRange::LutView View(const Lut3D& lut)
	{
		ExtendedRange::LutView v;
		v.size[0] = v.size[1] = v.size[2] = lut.size;
		v.data = lut.data.data();
		return v;
	}

	std::string TempPath(const char* prefix)
	{
		char directory[MAX_PATH] = {};
		GetTempPathA(ARRAYSIZE(directory), directory);
		char path[MAX_PATH] = {};
		GetTempFileNameA(directory, prefix, 0, path);
		return path;
	}

	std::vector<std::string> SplitLines(const std::string& text)
	{
		std::vector<std::string> lines;
		size_t start = 0;
		while (start <= text.size())
		{
			const size_t end = text.find("\r\n", start);
			if (end == std::string::npos) { if (start < text.size()) lines.push_back(text.substr(start)); break; }
			lines.push_back(text.substr(start, end - start));
			start = end + 2;
		}
		return lines;
	}
}

namespace VideoProcessorTest
{
	TEST_CLASS(CalibrationCoreTests)
	{
	public:
		TEST_METHOD(DeltaE2000MatchesSharmaReferencePairs)
		{
			struct Pair { Vec3 a, b; double de; };
			const Pair pairs[] = {
				{ { 50.0, 2.6772, -79.7751 }, { 50.0, 0.0, -82.7485 }, 2.0425 },
				{ { 50.0, 0.0, 0.0 }, { 50.0, -1.0, 2.0 }, 2.3669 },
				{ { 50.0, 2.5, 0.0 }, { 73.0, 25.0, -18.0 }, 27.1492 },
				{ { 60.2574, -34.0099, 36.2677 }, { 60.4626, -34.1751, 39.4387 }, 1.2644 },
				{ { 22.7233, 20.0904, -46.6940 }, { 23.0331, 14.9730, -42.5619 }, 2.0373 },
				{ { 2.0776, 0.0795, -1.1350 }, { 0.9033, -0.0636, -0.5514 }, 0.9082 },
			};
			for (const Pair& p : pairs)
			{
				Assert::AreEqual(p.de, DeltaE2000(p.a, p.b), 1e-4);
				Assert::AreEqual(p.de, DeltaE2000(p.b, p.a), 1e-4);
			}
		}

		TEST_METHOD(PatternPlanContainsTheDesignSets)
		{
			const std::vector<Patch> grey = GreyscalePatches();
			int btb = 0, wtw = 0, nearBlack = 0;
			for (const Patch& p : grey)
			{
				if (p.kind == PatchKind::Btb) { ++btb; Assert::IsTrue(p.rgb[0] < 0.0); }
				if (p.kind == PatchKind::Wtw) { ++wtw; Assert::IsTrue(p.rgb[0] > 1.0); }
				if (p.kind == PatchKind::NearBlack) ++nearBlack;
			}
			Assert::AreEqual(4, btb);
			Assert::AreEqual(2, wtw);
			Assert::AreEqual(8, nearBlack);   // 1..9% without 5%
			Assert::AreEqual(254.0, SignalToLimitedCode(grey.back().rgb[0]), 1e-9);
			Assert::AreEqual(size_t(24), PrimarySecondaryPatches().size());
			for (int n : { 5, 9, 13, 17 })
				Assert::AreEqual(static_cast<size_t>(n) * n * n, CubePatches(n).size());
			const std::vector<Patch> verify = VerificationPatches();
			Assert::IsTrue(verify.size() >= 55 && verify.size() <= 65);
			Assert::AreEqual(size_t(24), ColorChecker24().size());
			for (const Patch& p : verify)
			{
				Assert::IsTrue(p.space == PatchSpace::TargetSignal);
				Assert::IsFalse(p.usedForFit);
			}
			// The random verification colours avoid the 9^3 fit lattice.
			for (const Patch& p : verify)
				if (p.kind == PatchKind::VerifyRandom)
				{
					bool allOnLattice = true;
					for (int k = 0; k < 3; ++k)
						if (std::fabs(p.rgb[k] * 8 - std::floor(p.rgb[k] * 8 + 0.5)) > 1.0 / 3.0) allOnLattice = false;
					Assert::IsFalse(allOnLattice);
				}
			// Drift references: about one per 50.
			const std::vector<Patch> plan = BuildPlan(PlanKind::Standard);
			int drift = 0;
			for (const Patch& p : plan) if (p.kind == PatchKind::DriftReference) { ++drift; Assert::IsFalse(p.usedForFit); }
			const double fraction = static_cast<double>(drift) / (plan.size() - drift);
			Assert::IsTrue(fraction > 0.018 && fraction < 0.025);
		}

		TEST_METHOD(TimeEstimatesAreSane)
		{
			const double settle = 0.5;
			const PlanEstimate quick = EstimateTime(BuildPlan(PlanKind::Quick), settle);
			const PlanEstimate c5 = EstimateTime(BuildPlan(PlanKind::Cube5), settle);
			const PlanEstimate standard = EstimateTime(BuildPlan(PlanKind::Standard), settle);
			const PlanEstimate c13 = EstimateTime(BuildPlan(PlanKind::Cube13), settle);
			const PlanEstimate thorough = EstimateTime(BuildPlan(PlanKind::Thorough), settle);
			const PlanEstimate verify = EstimateTime(BuildPlan(PlanKind::Verify), settle);
			Logger::WriteMessage(("quick " + std::to_string(quick.TotalMinutes()) + " min, standard " +
				std::to_string(standard.TotalMinutes()) + " min, thorough " + std::to_string(thorough.TotalMinutes()) +
				" min, verify " + std::to_string(verify.TotalMinutes()) + " min\n").c_str());
			Assert::IsTrue(quick.TotalMinutes() > 1.0 && quick.TotalMinutes() < 15.0);
			Assert::IsTrue(standard.TotalMinutes() > 10.0 && standard.TotalMinutes() < 60.0);
			Assert::IsTrue(thorough.TotalMinutes() > 60.0 && thorough.TotalMinutes() < 240.0);
			Assert::IsTrue(verify.TotalMinutes() > 1.0 && verify.TotalMinutes() < 10.0);
			Assert::IsTrue(quick.TotalSeconds() < c5.TotalSeconds() && c5.TotalSeconds() < standard.TotalSeconds() &&
				standard.TotalSeconds() < c13.TotalSeconds() && c13.TotalSeconds() < thorough.TotalSeconds());
			// Roughly linear in the patch count once the cube dominates.
			const double perPatchStandard = standard.TotalSeconds() / standard.patches;
			const double perPatchThorough = thorough.TotalSeconds() / thorough.patches;
			Assert::IsTrue(perPatchThorough > 0.7 * perPatchStandard && perPatchThorough < 1.3 * perPatchStandard);
			// Settle scales the total; a dark patch integrates longer than a bright one.
			Assert::IsTrue(EstimateTime(BuildPlan(PlanKind::Standard), 2.0).TotalSeconds() > standard.TotalSeconds());
			const MeterTimingModel meter;
			Assert::IsTrue(meter.IntegrationSeconds(0.05) > 3.0 * meter.IntegrationSeconds(50.0));
			Assert::AreEqual(meter.maxSeconds + meter.overheadSeconds, meter.IntegrationSeconds(0.0), 1e-12);
		}

		TEST_METHOD(ForwardFitRecoversTheSimulatedDisplay)
		{
			const Calibrated& c = Standard();
			const Vec3 white = c.Ideal({ 1, 1, 1 });
			std::mt19937 rng(5u);
			std::uniform_real_distribution<double> u(0.0, 1.0);
			double sum = 0.0, worst = 0.0;
			const int count = 400;
			for (int i = 0; i < count; ++i)
			{
				const Vec3 d = { u(rng), u(rng), u(rng) };
				const double de = DeltaE(c.model.Forward(d), c.Ideal(d), white);
				sum += de;
				worst = (std::max)(worst, de);
			}
			Logger::WriteMessage(("forward fit dE2000 mean " + std::to_string(sum / count) + " max " + std::to_string(worst) +
				", additivity " + std::to_string(c.model.AdditivityError()) + ", grid " +
				std::to_string(c.model.ResidualGrid()) + "\n").c_str());
			Assert::IsTrue(sum / count < 0.3, L"forward mean");
			Assert::IsTrue(worst < 1.0, L"forward max");
			// Black, the WTW extension and the primaries' chromaticities.
			Assert::AreEqual(c.Ideal({ 0, 0, 0 })[1], c.model.Black()[1], 0.05 * c.Ideal({ 0, 0, 0 })[1]);
			const double wtw = LimitedCodeToSignal(254);
			Assert::AreEqual(c.Ideal({ wtw, wtw, wtw })[1], c.model.Forward({ wtw, wtw, wtw })[1],
				0.01 * c.Ideal({ wtw, wtw, wtw })[1]);
			Assert::AreEqual(LimitedCodeToSignal(4), c.model.MinDevice(), 1e-9);
			Assert::AreEqual(wtw, c.model.MaxDevice(), 1e-9);
			Assert::IsTrue(c.model.ChannelResponds(1, 1.0, wtw));     // WTW passes
			Assert::IsFalse(c.model.ChannelResponds(1, LimitedCodeToSignal(4), 0.0));   // BTB is clipped
			// The quick plan (no cube): no residual field, still a usable fit.
			SimulatedMeter meter(MeterParams());
			DisplayModel quick;
			Assert::IsTrue(quick.Fit(Measure(BuildPlan(PlanKind::Quick), c.display, meter)));
			Assert::AreEqual(0, quick.ResidualGrid());
			double quickSum = 0.0;
			for (int i = 0; i < count; ++i)
			{
				const Vec3 d = { u(rng), u(rng), u(rng) };
				quickSum += DeltaE(quick.Forward(d), c.Ideal(d), white);
			}
			Logger::WriteMessage(("quick fit mean " + std::to_string(quickSum / count) + "\n").c_str());
			Assert::IsTrue(quickSum / count < 3.0, L"quick fit");
			// The inverse inverts the forward model.
			for (int i = 0; i < 50; ++i)
			{
				const Vec3 d = { 0.05 + 0.9 * u(rng), 0.05 + 0.9 * u(rng), 0.05 + 0.9 * u(rng) };
				Vec3 back;
				const bool converged = c.model.Inverse(c.model.Forward(d), back);
				if (!converged || std::fabs(back[0] - d[0]) > 1e-5 || std::fabs(back[1] - d[1]) > 1e-5 || std::fabs(back[2] - d[2]) > 1e-5)
					Logger::WriteMessage(("inverse " + std::to_string(converged) + " d " + std::to_string(d[0]) + "," + std::to_string(d[1]) + "," +
						std::to_string(d[2]) + " back " + std::to_string(back[0]) + "," + std::to_string(back[1]) + "," + std::to_string(back[2]) + "\n").c_str());
				Assert::IsTrue(converged, L"inverse converged");
				for (int k = 0; k < 3; ++k) Assert::AreEqual(d[k], back[k], 1e-5, L"inverse value");
			}
		}

		TEST_METHOD(LutRoundTripIsUnderHalfDeltaEInGamut)
		{
			const Calibrated& c = Standard();
			std::vector<Vec3> probes;
			std::mt19937 rng(9u);
			std::uniform_real_distribution<double> u(0.0, 1.0);
			for (int i = 0; i < 400; ++i) probes.push_back({ u(rng), u(rng), u(rng) });
			for (const Patch& p : VerificationPatches()) probes.push_back({ p.rgb[0], p.rgb[1], p.rgb[2] });
			for (int s = 0; s < 2; ++s)   // BT.709 and P3: inside this display's gamut
			{
				double sum = 0.0, worst = 0.0;
				int n = 0;
				for (const Vec3& v : probes)
				{
					Vec3 direct;
					bool clipped = false;
					MapTargetToDevice(c.model, c.target[s], v, direct, &clipped);
					if (clipped) continue;
					const Vec3 device = c.lut[s].Sample(v);
					const double de = DeltaE(c.Ideal(device), c.target[s].Xyz(v), c.target[s].White());
					sum += de; worst = (std::max)(worst, de); ++n;
				}
				Logger::WriteMessage(("slot " + std::to_string(s) + ": " + std::to_string(n) + " in gamut, dE2000 mean " +
					std::to_string(sum / n) + " max " + std::to_string(worst) + ", clipped nodes " +
					std::to_string(c.lut[s].clippedNodes) + ", failed " + std::to_string(c.lut[s].failedNodes) + ", built in " + std::to_string(c.lutSeconds[s]) + " s\n").c_str());
				Assert::IsTrue(n > 300);
				Assert::IsTrue(worst < 0.5);
				Assert::AreEqual(0, c.lut[s].failedNodes);
			}
			// BT.2020 on this display: out-of-gamut nodes are clipped, never failed,
			// and a clipped colour keeps its lightness and hue.
			Assert::IsTrue(c.lut[2].clippedNodes > 0);
			Logger::WriteMessage(("slot 2: clipped nodes " + std::to_string(c.lut[2].clippedNodes) + ", built in " + std::to_string(c.lutSeconds[2]) + " s"+std::string(1,(char)10)).c_str());
			Assert::AreEqual(0, c.lut[2].failedNodes);
			const Vec3 green = { 0.0, 1.0, 0.0 };
			Vec3 device;
			bool clipped = false;
			Assert::IsTrue(MapTargetToDevice(c.model, c.target[2], green, device, &clipped));
			Assert::IsTrue(clipped);
			const Vec3 want = XyzToLab(c.target[2].Xyz(green), c.target[2].White());
			const Vec3 got = XyzToLab(c.model.Forward(device), c.target[2].White());
			Assert::AreEqual(want[0], got[0], 0.05);
			Assert::AreEqual(std::atan2(want[2], want[1]), std::atan2(got[2], got[1]), 0.01);
			Assert::IsTrue(std::hypot(got[1], got[2]) < std::hypot(want[1], want[2]));
		}

		TEST_METHOD(NeutralAxisIsMonotoneAndExact)
		{
			const Calibrated& c = Standard();
			for (int s = 0; s < 3; ++s)
			{
				const Lut3D& lut = c.lut[s];
				double previousY = -1.0;
				for (int i = 0; i < lut.size; ++i)
				{
					const Vec3 d = lut.At(i, i, i);
					if (i > 0)
					{
						const Vec3 p = lut.At(i - 1, i - 1, i - 1);
						for (int k = 0; k < 3; ++k) Assert::IsTrue(d[k] >= p[k], L"channel monotone");
					}
					const Vec3 xyz = c.Ideal(d);
					Assert::IsTrue(xyz[1] > previousY, L"luminance rising");
					previousY = xyz[1];
					// The neutral axis is never chroma-clipped: it hits the target grey.
					const double v = static_cast<double>(i) / (lut.size - 1);
					const double de = DeltaE(xyz, c.target[s].Xyz({ v, v, v }), c.target[s].White());
					if (de >= 0.5) Logger::WriteMessage(("neutral slot " + std::to_string(s) + " i " + std::to_string(i) + " dE " + std::to_string(de) + "\n").c_str());
					Assert::IsTrue(de < 0.5, L"neutral exact");
				}
			}
			// The target white is D65 at the largest reachable level: one channel at 1.
			const Vec3 top = c.lut[0].At(32, 32, 32);
			Assert::AreEqual(1.0, (std::max)((std::max)(top[0], top[1]), top[2]), 1e-5);
			const Chromaticity w = XyzToXy(c.target[0].White());
			Assert::AreEqual(0.3127, w.x, 1e-6);
			Assert::AreEqual(0.3290, w.y, 1e-6);
		}

		TEST_METHOD(CubePassesTheExistingParser)
		{
			const Calibrated& c = Standard();
			for (int s = 0; s < 3; ++s)
			{
				const std::string path = TempPath("vpc");
				std::string error;
				Assert::IsTrue(WriteCube(c.lut[s], path, "VP calibration test",
					{ "VideoProcessor automatic calibration", "slot test" }, &error));
				std::ifstream in(path, std::ios::binary);
				const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				in.close();
				Assert::IsTrue(text.find("DOMAIN") == std::string::npos);
				LibplaceboDisplayLut::LoadResult loaded = LibplaceboDisplayLut::Load(nullptr, path);
				Assert::IsTrue(loaded.status == LibplaceboDisplayLut::Status::ACTIVE);
				Assert::IsNotNull(loaded.lut);
				for (int k = 0; k < 3; ++k) Assert::AreEqual(33, loaded.lut->size[k]);
				for (size_t i = 0; i < c.lut[s].data.size(); ++i)
					Assert::AreEqual(c.lut[s].data[i], loaded.lut->data[i], 1e-6f);
				pl_lut_free(&loaded.lut);
				DeleteFileA(path.c_str());
			}
			// The largest size the loader accepts, and sizes beyond it clamp.
			Lut3D big;
			big.size = 65;
			big.data.resize(65u * 65u * 65u * 3u);
			for (int b = 0; b < 65; ++b)
				for (int g = 0; g < 65; ++g)
					for (int r = 0; r < 65; ++r)
						big.Set(r, g, b, { r / 64.0, g / 64.0, b / 64.0 });
			const std::string path = TempPath("vpc");
			Assert::IsTrue(WriteCube(big, path, "identity 65"));
			LibplaceboDisplayLut::LoadResult loaded = LibplaceboDisplayLut::Load(nullptr, path);
			Assert::IsTrue(loaded.status == LibplaceboDisplayLut::Status::ACTIVE);
			Assert::AreEqual(65, loaded.lut->size[0]);
			pl_lut_free(&loaded.lut);
			DeleteFileA(path.c_str());
		}

		TEST_METHOD(EndSlopesAgreeWithLutExtrapolate)
		{
			const Calibrated& c = Standard();
			for (int s = 0; s < 3; ++s)
			{
				const Lut3D& lut = c.lut[s];
				const ExtendedRange::LutView view = View(lut);
				// WTW passes on this display: the high end is fitted to it.
				Assert::IsTrue(lut.endSlopes.fittedHigh);
				// BTB is clipped: the low end keeps the inverse's own slope.
				Assert::IsFalse(lut.endSlopes.fittedLow);
				const Vec3 top = lut.At(32, 32, 32), bottom = lut.At(0, 0, 0);
				for (const double delta : { 0.01, 0.03, 0.05 })
				{
					const float hi[3] = { static_cast<float>(1 + delta), static_cast<float>(1 + delta), static_cast<float>(1 + delta) };
					float out[3];
					ExtendedRange::LutExtrapolate(view, hi, out);
					for (int k = 0; k < 3; ++k)
						Assert::AreEqual(top[k] + delta * lut.endSlopes.slopeHigh[k], static_cast<double>(out[k]), 2e-5);
					// The light the display gives for the extrapolated WTW codes is the target's.
					const Vec3 device = { out[0], out[1], out[2] };
					const double want = c.target[s].Xyz({ 1 + delta, 1 + delta, 1 + delta })[1];
					Assert::AreEqual(want, c.Ideal(device)[1], 0.01 * want);
					// The slope from the true display (not the model) agrees.
					const float lo[3] = { static_cast<float>(-delta), static_cast<float>(-delta), static_cast<float>(-delta) };
					ExtendedRange::LutExtrapolate(view, lo, out);
					for (int k = 0; k < 3; ++k)
					{
						Assert::AreEqual(bottom[k] - delta * lut.endSlopes.naturalLow[k], static_cast<double>(out[k]), 2e-5);
						Assert::IsTrue(out[k] < bottom[k] + 1e-6);   // BTB codes still pass below black
					}
				}
			}

			// A display that shows BTB (black set high): the low end is fitted too.
			SimulatedDisplayParams params;
			params.btbClipCode = 1;
			params.blackLift = 0.05;
			const Calibrated lifted(params, PlanKind::Standard, 17);
			const Lut3D& lut = lifted.lut[0];
			Assert::IsTrue(lut.endSlopes.fittedLow);
			const ExtendedRange::LutView view = View(lut);
			const Vec3 bottom = lut.At(0, 0, 0);
			for (const double delta : { 0.01, 0.02 })
			{
				const float lo[3] = { static_cast<float>(-delta), static_cast<float>(-delta), static_cast<float>(-delta) };
				float out[3];
				ExtendedRange::LutExtrapolate(view, lo, out);
				for (int k = 0; k < 3; ++k)
					Assert::AreEqual(bottom[k] - delta * lut.endSlopes.slopeLow[k], static_cast<double>(out[k]), 2e-5);
				const double want = lifted.target[0].Xyz({ -delta, -delta, -delta })[1];
				const double got = lifted.Ideal({ out[0], out[1], out[2] })[1];
				Assert::AreEqual(want, got, 0.03 * lifted.target[0].Black()[1]);
			}
		}

		TEST_METHOD(VerificationReportsDeltaE)
		{
			// Synthetic: known differences in L* only.
			const Vec3 white = XyToXyz(Gamuts::D65(), 100.0);
			std::vector<VerifyEntry> entries;
			for (int i = 0; i < 4; ++i)
			{
				VerifyEntry e;
				e.label = "patch " + std::to_string(i);
				e.targetXyz = LabToXyz({ 50.0 - 0.5 * i, 0.0, 0.0 }, white);
				e.measuredXyz = LabToXyz({ 50.0 + 0.5 * i, 0.0, 0.0 }, white);
				entries.push_back(e);
			}
			VerifyReport r = BuildVerifyReport(entries, white, { 0, 0, 0 });
			Assert::AreEqual(3.0, r.maxDeltaE, 1e-6);   // dL = 3 around L 50: SL = 1
			Assert::AreEqual(1.5, r.meanDeltaE, 1e-6);
			Assert::AreEqual(std::string("patch 3"), r.worstLabel);
			Assert::IsFalse(r.pass);   // mean 1.5 is not < 1.5
			entries.pop_back();
			r = BuildVerifyReport(entries, white, { 0, 0, 0 });
			Assert::IsTrue(r.pass);

			// The simulated run: verify patches through the LUT, measured again.
			const Calibrated& c = Standard();
			SimulatedMeter meter(MeterParams());
			std::vector<VerifyEntry> after, before;
			std::vector<std::pair<double, Vec3>> greys;
			for (const Patch& p : VerificationPatches())
			{
				const Vec3 v = { p.rgb[0], p.rgb[1], p.rgb[2] };
				VerifyEntry e;
				e.label = p.label;
				e.kind = p.kind;
				Vec3 device;
				MapTargetToDevice(c.model, c.target[0], v, device);
				e.targetXyz = c.model.Forward(device);   // the clipped target the LUT aims for
				e.measuredXyz = meter.Read(c.display.Emit(c.lut[0].Sample(v))).xyz;
				after.push_back(e);
				e.measuredXyz = meter.Read(c.display.Emit(v)).xyz;   // uncalibrated: the signal straight to the display
				before.push_back(e);
				if (p.kind == PatchKind::VerifyGrey) greys.push_back(std::make_pair(p.rgb[0], after.back().measuredXyz));
			}
			const VerifyReport good = BuildVerifyReport(after, c.target[0].White(), c.target[0].Black(), greys);
			const VerifyReport bad = BuildVerifyReport(before, c.target[0].White(), c.target[0].Black());
			Logger::WriteMessage(("verify after: mean " + std::to_string(good.meanDeltaE) + " max " +
				std::to_string(good.maxDeltaE) + " (" + good.worstLabel + "); before: mean " + std::to_string(bad.meanDeltaE) +
				" max " + std::to_string(bad.maxDeltaE) + "\n").c_str());
			Assert::IsTrue(good.pass);
			Assert::IsTrue(good.meanDeltaE < 1.0);
			Assert::IsFalse(bad.pass);
			Assert::IsTrue(good.entries.size() >= 55);
			Assert::AreEqual(greys.size(), good.greys.size());
			for (const GreyStep& g : good.greys)
				if (g.signal > 0.2 && g.signal < 0.9)
				{
					Assert::IsTrue(g.gamma > 2.0 && g.gamma < 2.8);
					Assert::IsTrue(std::fabs(g.cct - 6504.0) < 150.0);
				}
			const std::string json = FormatVerifyJson(good);
			Assert::IsTrue(json.find("\"mean_de2000\"") != std::string::npos);
			Assert::IsTrue(json.find("\"pass\": true") != std::string::npos);
			Assert::IsTrue(json.find("CC dark skin") != std::string::npos);
		}

		TEST_METHOD(LedFluxTableIsRecoveredWithHysteresis)
		{
			SimulatedDisplayParams params;
			const SimulatedDisplay display(params);
			std::mt19937 rng(3u);
			std::normal_distribution<double> noise(0.0, 1.0);
			const double white = 48.0;
			std::vector<FluxStep> steps;
			// LPW1 60 -> 255 and back in steps of 5, 3 s dwell, a colorimeter-rate reading.
			const double dwell = 3.0, rate = 4.0;
			double t = 0.0;
			bool previousFromAbove = false;
			int previous = 60;
			std::vector<int> sweep;
			for (int l = 60; l <= 255; l += 5) sweep.push_back(l);
			for (int l = 250; l >= 60; l -= 5) sweep.push_back(l);
			for (const int level : sweep)
			{
				const bool fromAbove = level < previous;
				const double from = display.RelativeFlux(previous, previousFromAbove);
				const double to = display.RelativeFlux(level, fromAbove);
				FluxStep step;
				step.lpw = level;
				step.direction = fromAbove ? StepDirection::Down : StepDirection::Up;
				for (TimedSample s : display.SampleLedStep(from, to, t, t, dwell, rate, 0.0, 0.0, rng))
				{
					s.value = s.value * white * (1.0 + 0.002 * noise(rng));
					step.samples.push_back(s);
				}
				steps.push_back(step);
				t += dwell;
				previous = level;
				previousFromAbove = fromAbove;
			}
			const FluxFit fit = FitFluxTable(steps);
			Assert::IsTrue(fit.ok);
			Assert::AreEqual(size_t(40), fit.points.size());
			for (const FluxPoint& p : fit.points)
			{
				if (p.haveUp) Assert::AreEqual(display.RelativeFlux(p.lpw, false), p.up, 0.005);
				if (p.haveDown) Assert::AreEqual(display.RelativeFlux(p.lpw, true), p.down, 0.005);
				if (p.haveUp && p.haveDown)
				{
					const double truth = (display.RelativeFlux(p.lpw, true) - display.RelativeFlux(p.lpw, false)) / p.flux;
					Assert::AreEqual(truth, p.hysteresis, 0.006);
				}
			}
			Assert::IsTrue(fit.maxHysteresis > 0.005);
			Assert::AreEqual(1.0, fit.table.back().second, 1e-9);
			// The config value passes the LED drive's own parser.
			std::vector<std::pair<double, double>> parsed;
			Assert::IsTrue(LedDrive::ParseFluxTable(fit.ConfigValue(), parsed));
			Assert::AreEqual(fit.table.size(), parsed.size());
		}

		TEST_METHOD(LedLatencyAndLagAreRecovered)
		{
			struct Case { double dead, tau, ramp; bool up; };
			const Case cases[] = {
				{ 0.080, 0.120, 0.0, true }, { 0.080, 0.120, 0.0, false }, { 0.045, 0.300, 0.0, true },
				{ 0.150, 0.060, 0.0, false }, { 0.080, 0.100, 0.200, true }, { 0.060, 0.150, 0.300, false },
			};
			std::mt19937 rng(17u);
			std::vector<StepResponseFit> plain;
			for (const Case& k : cases)
			{
				SimulatedDisplayParams params;
				params.ledDeadTimeSeconds = k.dead;
				params.ledTauSeconds = k.tau;
				params.ledRampSeconds = k.ramp;
				const SimulatedDisplay display(params);
				const double from = k.up ? 0.4 : 1.0, to = k.up ? 1.0 : 0.4;
				// A photodiode on the ESP32 ADC at 1 kHz.
				const std::vector<TimedSample> samples =
					display.SampleLedStep(from, to, 0.5, 0.0, 0.5 + k.dead + k.ramp + 8 * k.tau, 1000.0, 0.005, 0.002, rng);
				const StepResponseFit fit = FitStepResponse(samples, 0.5);
				Logger::WriteMessage(("dead " + std::to_string(fit.deadTimeSeconds) + " tau " + std::to_string(fit.tauSeconds) +
					" ramp " + std::to_string(fit.rampSeconds) + (fit.rampDetected ? " (ramp)" : "") + "\n").c_str());
				Assert::IsTrue(fit.ok);
				Assert::AreEqual(k.dead, fit.deadTimeSeconds, 0.010);
				Assert::AreEqual(k.tau, fit.tauSeconds, 0.10 * k.tau);
				Assert::AreEqual(k.ramp > 0.0, fit.rampDetected);
				if (k.ramp > 0.0) Assert::AreEqual(k.ramp, fit.rampSeconds, 0.15 * k.ramp);
				Assert::AreEqual(from, fit.from, 0.01);
				Assert::AreEqual(to, fit.to, 0.01);
				if (k.ramp == 0.0 && k.dead == 0.080) plain.push_back(fit);
			}
			const LatencySummary summary = SummarizeSteps(plain);
			Assert::IsTrue(summary.ok);
			Assert::AreEqual(80.0, static_cast<double>(summary.latencyMs), 10.0);
			Assert::AreEqual(120.0, static_cast<double>(summary.settleMs), 12.0);
			// A picture step against an LPW1 step.
			StepResponseFit picture, led;
			picture.deadTimeSeconds = 0.034; led.deadTimeSeconds = 0.080;
			Assert::AreEqual(0.046, LedAfterPictureSeconds(picture, led), 1e-12);
		}

		TEST_METHOD(CntrCurveGivesDmdClipAndMaxUsable)
		{
			SimulatedDisplay display;
			std::mt19937 rng(23u);
			std::normal_distribution<double> noise(0.0, 1.0);
			std::vector<CntrSample> samples;
			for (int cntr = 50; cntr <= 74; ++cntr)
			{
				display.SetCntr(cntr);
				for (const int code : { 128, 235, 254 })
				{
					CntrSample s;
					s.cntr = cntr;
					s.code = code;
					s.luminance = display.EmitCode8(code, code, code)[1] * (1.0 + 0.002 * noise(rng));
					samples.push_back(s);
				}
			}
			const CntrFit fit = FitCntrCurve(samples);
			const SimulatedDisplayParams& p = display.Params();
			// Code 235 (signal 1) reaches the DMD maximum where gain(CNTR) = 1.
			const double clip = p.cntrReference + (1.0 / p.gainAtReference - 1.0) / p.gainPerStep;
			Logger::WriteMessage(("clip " + std::to_string(fit.clipExact) + " (truth " + std::to_string(clip) + "), usable " +
				std::to_string(fit.usableExact) + "\n").c_str());
			Assert::IsTrue(fit.ok);
			Assert::IsTrue(fit.clipReached);
			Assert::AreEqual(clip, fit.clipExact, 0.6);
			Assert::AreEqual(static_cast<double>(std::lround(clip)), static_cast<double>(fit.dmdClipCntr), 1.0);
			Assert::AreEqual(p.gainSaturatesAt, fit.cntrMaxUsable);
			// The values pass the LED drive's own range rules.
			int parsed = 0;
			Assert::IsTrue(LedDrive::ParseDmdClipCntr(std::to_string(fit.dmdClipCntr), parsed));
			// No clip within range: -1 (= cntr_max_usable).
			std::vector<CntrSample> low;
			for (const CntrSample& s : samples) if (s.cntr <= 58) low.push_back(s);
			const CntrFit unclipped = FitCntrCurve(low);
			Assert::IsTrue(unclipped.ok);
			Assert::IsFalse(unclipped.clipReached);
			Assert::AreEqual(-1, unclipped.dmdClipCntr);
		}

		TEST_METHOD(RunFolderAndDisplayProfile)
		{
			RunTimestamp ts;
			ts.year = 2026; ts.month = 10; ts.day = 17; ts.hour = 9; ts.minute = 5; ts.second = 3;
			const RunFolder f = BuildRunFolder("C:\\Videoprocessor\\vp", "Barco FL35 / main", ts);
			Assert::AreEqual(std::string("C:\\Videoprocessor\\vp\\luts\\calibration\\index.json"), f.indexJson);
			Assert::AreEqual(std::string("C:\\Videoprocessor\\vp\\luts\\calibration\\Barco_FL35___main\\20261017-090503"), f.runDir);
			Assert::AreEqual(std::string("luts/calibration/Barco_FL35___main/20261017-090503/calibration_p3_d65.cube"),
				f.cubeConfigValue[1]);
			Assert::AreEqual(f.runDir + "\\raw.jsonl", f.rawJournal);
			Assert::AreEqual(f.runDir + "\\display_profile.cfg", f.displayProfile);
			Assert::AreEqual(f.runDir + "\\before\\VideoProcessor.cfg", BeforeCopyPath(f, "C:\\Videoprocessor\\vp\\VideoProcessor.cfg"));
			Assert::AreEqual(std::string("display"), SanitizeDisplayName("..\\.."));
			Assert::AreEqual(std::string("display"), SanitizeDisplayName(""));

			const Calibrated& c = Standard();
			const DisplayProfileValues v = ProfileFromModel(c.model, c.target[0], 8, "1920x1080@23.976 RGB 8-bit limited");
			Logger::WriteMessage((std::string("eotf ") + ::ToString(v.eotf) + " gamut " + ::ToString(v.gamut) + " gamma " + std::to_string(EffectiveGamma(c.model)) + "\n").c_str());
			Assert::IsTrue(v.eotf == DisplayEotf::Bt1886 || v.eotf == DisplayEotf::Gamma22 || v.eotf == DisplayEotf::Gamma24 || v.eotf == DisplayEotf::Srgb);
			Assert::IsTrue(v.gamut == DisplayGamut::Bt2020);   // the simulated narrow-band primaries
			const double gamma = EffectiveGamma(c.model);
			Assert::IsTrue(gamma > 2.1 && gamma < 2.4);
			const std::string text = FormatDisplayProfile("FL35", v);
			for (const char* key : { "[display.FL35]", "peak_nits: ", "black_nits: ", "eotf: ", "gamut: bt2020",
				"link_bit_depth: 8", "dynamic_mapping: off", "output_fingerprint: 1920x1080@23.976" })
				Assert::IsTrue(text.find(key) != std::string::npos);
			// It parses as a VP configuration document.
			ConfigEditorCore::ConfigDocument doc;
			doc.LoadBytes(L"display_profile.cfg", text);
			Assert::AreEqual(std::string("8"), doc.Get("display.FL35", "link_bit_depth"));
		}

		TEST_METHOD(ConfigEditsAreKeyOnly)
		{
			const std::string original =
				"# VideoProcessor configuration\r\n"
				"[general]\r\n"
				"renderer: VP Renderer   # keep this comment\r\n"
				"\r\n"
				"[vprenderer.color.Rec709]\r\n"
				"calibration_lut_enabled: false\r\n"
				"calibration_lut_bt709: none # slot comment\r\n"
				"calibration_lut_p3_d65:    none\r\n"
				"\r\n"
				"; trailing comment block\r\n"
				"[projector.Default]\r\n"
				"port: COM3\r\n"
				"led_latency_ms: 80   # measured once with a camera\r\n"
				"cntr_max_usable: 74\r\n"
				"\r\n"
				"[led_drive.default]\r\n"
				"mode: compensate\r\n"
				"compensation_eotf: 2.41\r\n"
				"\r\n"
				"[unknown.section]\r\n"
				"something_unknown: keep me\r\n";
			ConfigEditorCore::ConfigDocument doc;
			doc.LoadBytes(L"VideoProcessor.cfg", original);

			FluxFit flux;
			flux.ok = true;
			flux.table = { { 60, 0.5 }, { 160, 0.8 }, { 255, 1.0 } };
			LatencySummary led;
			led.ok = true; led.latencyMs = 92; led.settleMs = 140;
			CntrFit cntr;
			cntr.ok = true; cntr.cntrMaxUsable = 70; cntr.dmdClipCntr = 61;
			LedCalibrationResults results;
			results.flux = &flux;
			results.led = &led;
			results.cntrCurve = &cntr;
			results.compensationEotf = 2.36;
			std::vector<ConfigEdit> edits = LedEdits(results, "projector.Default");
			RunTimestamp ts;
			const RunFolder folder = BuildRunFolder("C:\\vp", "FL35", ts);
			const bool slots[3] = { true, true, false };
			const std::vector<ConfigEdit> lutEdits = LutSlotEdits("vprenderer.color.Rec709", folder, slots);
			edits.insert(edits.end(), lutEdits.begin(), lutEdits.end());

			const ConfigDiff diff = ApplyKeyOnlyEdits(doc, edits);
			Assert::IsTrue(diff.ok);
			Assert::IsTrue(diff.Changed());
			const std::string text = diff.Format();
			Logger::WriteMessage(text.c_str());
			Assert::IsTrue(text.find("[projector.Default] led_latency_ms: 80 -> 92") != std::string::npos);
			Assert::IsTrue(text.find("[projector.Default] flux_table: (absent) -> 60:0.500000") != std::string::npos);

			const std::string updated = doc.Serialize();
			const std::vector<std::string> before = SplitLines(original), after = SplitLines(updated);
			// Every original line is still there, in order, byte-identical unless
			// it holds an edited key; new lines are only the added keys.
			std::vector<std::string> changedKeys, addedKeys;
			for (const ConfigDiffEntry& e : diff.entries)
				(e.existed ? changedKeys : addedKeys).push_back(e.key);
			size_t j = 0;
			int added = 0, changed = 0;
			for (size_t i = 0; i < before.size(); ++i)
			{
				while (j < after.size() && after[j] != before[i])
				{
					// Either an edited line (the same key, comment kept) or an added key.
					bool isChanged = false;
					for (const std::string& key : changedKeys)
						if (before[i].compare(0, key.size() + 1, key + ":") == 0 && after[j].compare(0, key.size() + 1, key + ":") == 0)
						{
							const size_t hash = before[i].find('#');
							if (hash != std::string::npos)
								Assert::IsTrue(after[j].find(before[i].substr(hash)) != std::string::npos);
							isChanged = true;
						}
					if (isChanged) { ++changed; break; }
					bool isAdded = false;
					for (const std::string& key : addedKeys)
						if (after[j].compare(0, key.size() + 1, key + ":") == 0) isAdded = true;
					Assert::IsTrue(isAdded, (L"unexpected line: " + std::wstring(after[j].begin(), after[j].end())).c_str());
					++added;
					++j;
				}
				Assert::IsTrue(j < after.size());
				++j;
			}
			for (; j < after.size(); ++j)
			{
				bool isAdded = false;
				for (const std::string& key : addedKeys)
					if (after[j].compare(0, key.size() + 1, key + ":") == 0) isAdded = true;
				Assert::IsTrue(isAdded);
				++added;
			}
			Assert::AreEqual(static_cast<int>(addedKeys.size()), added);
			Assert::AreEqual(static_cast<int>(changedKeys.size()), changed);
			// The line ending is kept; no lone LF.
			for (size_t i = 0; i < updated.size(); ++i)
				if (updated[i] == '\n') Assert::IsTrue(i > 0 && updated[i - 1] == '\r');
			Assert::AreEqual(std::string("92"), doc.Get("projector.Default", "led_latency_ms"));
			Assert::AreEqual(std::string("luts/calibration/FL35/19700101-000000/calibration_bt709.cube"),
				doc.Get("vprenderer.color.Rec709", "calibration_lut_bt709"));
			Assert::AreEqual(std::string("keep me"), doc.Get("unknown.section", "something_unknown"));
			// The produced values pass the LED drive's parser.
			std::vector<std::pair<double, double>> table;
			Assert::IsTrue(LedDrive::ParseFluxTable(doc.Get("projector.Default", "flux_table"), table));

			// Applying the same edits again changes nothing.
			const ConfigDiff again = ApplyKeyOnlyEdits(doc, edits);
			Assert::IsTrue(again.ok);
			Assert::IsFalse(again.Changed());
			Assert::AreEqual(updated, doc.Serialize());
		}
	};
}
