// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Red-Team adversarial verification for the 6 motion-adaptive hybrid scaler upgrades
// (designs/2026-11-23_post-core-motion-adaptive-scaler-blend.md):
//   1. Continuous confidence CPU/GPU parity (no f.z > 0.5 cliff; continuous c in motionMap.g).
//   2. Single motionStrength continuum (0 = standard blend, zero GPU motion-map cost, schema
//      "off"/"on"/0.0..1.0, visible strength status, verbatim missing-vector fallback).
//   3. P10g (kP10gGrainLevel) via Acquire + verbatim "upscale motion: no vectors (alias-safe weight)".
//   4. 4-neighbor motion-boundary shear + Option 4B crisp rigid cinema pan protection + whip-pan floor.
//   5. 2x2 bilinear temporal advection (ppos = pc / 16 - 0.5) + asymmetric fast-attack (tau / 4) /
//      smooth-release (tau) one-pole across 24 Hz and 60 Hz.
//   6. Full NLS coordinate mapping on Motion and GrainLevelSource (active valid NLS stays live and
//      maps AD_MOTION & AD_GRAIN_PUBLISHED via nls_coordinate; !nls.valid fails visibly to fallback),
//      plus BTB/WTW, 12-bit <= 1 LSB precision, and multi-band grain retention non-regression.
#include "pch.h"
#include "CppUnitTest.h"

#include <ConfigFile.h>
#include <MainConfigSchema.h>
#include <RendererProfileConfig.h>
#include <VP-V/core/OnePole.h>
#include <VP-V/core/Products.h>
#include <VP-V/core/Ramp.h>
#include <VP-V/modules/control/config/VpvRendererSettings.h>
#include <VP-V/modules/motion/estimation/P3FieldFill.h>
#include <VP-V/modules/output/dither/CustomHookChain.h>
#include <VP-V/modules/scaling/output/AdaptiveDownscaleStage.h>
#include <VP-V/modules/scaling/output/NlsMaskMapping.h>
#include <VP-V/modules/scaling/shared/HybridBlend.h>
#include <VP-V/tests/VpLutGpuTestSupport.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <windows.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace AdaptiveMotionProbeFallback
{
	struct NoMatch {};
	NoMatch MotionTerm(...);
	NoMatch MotionShearSpeed(...);
	NoMatch MotionAdvectBilinear(...);
	NoMatch MotionStepAsymmetric(...);
	NoMatch MotionAttackReleaseAlphas(...);
}

namespace VideoProcessorTest
{
	namespace AdaptiveMotionUpgradeDetail
	{
		struct Rank0 {};
		struct Rank1 : Rank0 {};
		struct Rank2 : Rank1 {};
		struct Rank3 : Rank2 {};
		struct Rank4 : Rank3 {};
		struct Rank5 : Rank4 {};
		struct Rank6 : Rank5 {};
		struct Rank7 : Rank6 {};
		struct Rank8 : Rank7 {};
		struct Rank9 : Rank8 {};

		inline std::wstring Wide(const std::string& s)
		{
			return std::wstring(s.begin(), s.end());
		}

		inline bool HasSubstring(const std::vector<std::string>& lines, const char* needle)
		{
			for (const std::string& l : lines)
				if (l.find(needle) != std::string::npos)
					return true;
			return false;
		}

		inline std::string JoinLines(const std::vector<std::string>& lines)
		{
			std::string out;
			for (size_t i = 0; i < lines.size(); ++i)
			{
				if (i > 0)
					out += " | ";
				out += lines[i];
			}
			return out;
		}

		// ---- Compile-time interface bridges (allow clean pre-fix runtime FAIL execution on 482fd5811
		//      while strictly requiring & invoking the exact locked public interfaces post-fix) --------

		template <typename S>
		auto HasMotionStrengthImpl(const S& s, Rank1) -> decltype((void)static_cast<float>(s.motionStrength), std::true_type{});
		template <typename S>
		std::false_type HasMotionStrengthImpl(const S&, Rank0);

		inline bool SettingsHasMotionStrength()
		{
			const AdaptiveDownscale::Settings s{};
			return decltype(HasMotionStrengthImpl(s, Rank1{}))::value;
		}

		template <typename S>
		auto GetMotionStrengthImpl(const S& s, Rank1) -> decltype(static_cast<float>(s.motionStrength))
		{
			return static_cast<float>(s.motionStrength);
		}
		template <typename S>
		float GetMotionStrengthImpl(const S&, Rank0)
		{
			return -1.0f;
		}

		inline float GetMotionStrength(const AdaptiveDownscale::Settings& s)
		{
			return GetMotionStrengthImpl(s, Rank1{});
		}

		template <typename S>
		auto SetMotionStrengthImpl(S& s, float v, Rank2) -> decltype((void)(s.motionStrength = v), true)
		{
			s.motionStrength = v;
			return true;
		}
		template <typename S>
		auto SetMotionStrengthImpl(S& s, float v, Rank1) -> decltype((void)(s.motionAdaptive = (v > 0.0f)), false)
		{
			// Pre-fix fallback ONLY when Settings::motionStrength does not exist yet so WARP GPU shaders
			// still execute on pre-fix code and expose real runtime shader failures for Red-Green proof.
			s.motionAdaptive = (v > 0.0f);
			return false;
		}
		template <typename S>
		bool SetMotionStrengthImpl(S&, float, Rank0)
		{
			return false;
		}

		inline bool SetMotionStrength(AdaptiveDownscale::Settings& s, float v)
		{
			return SetMotionStrengthImpl(s, v, Rank2{});
		}

		template <typename M>
		auto SetNlsImpl(M& obj, const NlsMaskMapping::Mapping& m, Rank1) -> decltype((void)(obj.nls = m), true)
		{
			obj.nls = m;
			return true;
		}
		template <typename M>
		bool SetNlsImpl(M&, const NlsMaskMapping::Mapping&, Rank0)
		{
			return false;
		}

		template <typename M>
		auto GetNlsImpl(const M& obj, Rank1) -> decltype(obj.nls)
		{
			return obj.nls;
		}
		template <typename M>
		NlsMaskMapping::Mapping GetNlsImpl(const M&, Rank0)
		{
			return NlsMaskMapping::Mapping{};
		}

		namespace HbProbe
		{
			using AdaptiveMotionProbeFallback::NoMatch;
			using namespace AdaptiveMotionProbeFallback;
			using namespace HybridBlend;

			template <typename T>
			auto Eval4(bool valid, T mapFactor, T floor, T strength, Rank1)
				-> typename std::enable_if<!std::is_same<decltype(MotionTerm(valid, mapFactor, floor, strength)), NoMatch>::value, bool>::type
			{
				(void)valid; (void)mapFactor; (void)floor; (void)strength;
				return true;
			}
			template <typename T>
			bool Eval4(bool, T, T, T, Rank0)
			{
				return false;
			}

			template <typename T>
			auto Call4(bool valid, T mapFactor, T floor, T strength, Rank1)
				-> typename std::enable_if<!std::is_same<decltype(MotionTerm(valid, mapFactor, floor, strength)), NoMatch>::value, float>::type
			{
				return static_cast<float>(MotionTerm(valid, mapFactor, floor, strength));
			}
			template <typename T>
			float Call4(bool valid, T mapFactor, T floor, T, Rank0)
			{
				return HybridBlend::MotionTerm(valid, mapFactor, floor);
			}
		}

		inline bool HasMotionTerm4()
		{
			return HbProbe::Eval4<float>(true, 0.5f, 0.0f, 0.5f, Rank1{});
		}

		inline float CallMotionTerm4(bool valid, float mapFactor, float floor, float strength)
		{
			return HbProbe::Call4<float>(valid, mapFactor, floor, strength, Rank1{});
		}

		namespace AdProbe
		{
			using AdaptiveMotionProbeFallback::NoMatch;
			using namespace AdaptiveMotionProbeFallback;
			using namespace AdaptiveDownscale;

			// ---- MotionTarget(bool frameTrusted, float speed, float shearSpeed, float confidence, const Settings& s) ----
			// Exact function-pointer target cast cleanly disambiguates even when BOTH the legacy
			// (bool, bool, float, float, const Settings&) and upgraded (bool, float, float, float, const Settings&)
			// overloads coexist, while failing substitution cleanly on pre-fix code with zero C4305 warnings.
			template <typename S>
			auto EvalNewTarget(const S&, Rank1)
				-> decltype(static_cast<float(*)(bool, float, float, float, const S&)>(&MotionTarget), std::true_type{});
			template <typename S>
			std::false_type EvalNewTarget(const S&, Rank0);

			template <typename S>
			auto CallTarget(bool ft, float spd, float shr, float conf, const S& s, Rank1)
				-> decltype(static_cast<float(*)(bool, float, float, float, const S&)>(&MotionTarget)(ft, spd, shr, conf, s))
			{
				return static_cast<float(*)(bool, float, float, float, const S&)>(&MotionTarget)(ft, spd, shr, conf, s);
			}
			template <typename S>
			float CallTarget(bool ft, float spd, float, float conf, const S& s, Rank0)
			{
				// Pre-fix 5-arg signature had bool blockTrusted (f.z > 0.5 cliff) and no shearSpeed.
				return static_cast<float>(AdaptiveDownscale::MotionTarget(ft, conf > 0.5f, spd, conf, s));
			}

			// ---- MotionAttackReleaseAlphas(frameSeconds, smoothMs, alphaAttack, alphaRelease) ----
			template <typename T>
			auto CallAlphas(T dt, T smoothMs, T& aAtk, T& aRel, Rank1)
				-> typename std::enable_if<!std::is_same<decltype(MotionAttackReleaseAlphas(dt, smoothMs, aAtk, aRel)), NoMatch>::value, bool>::type
			{
				MotionAttackReleaseAlphas(dt, smoothMs, aAtk, aRel);
				return true;
			}
			template <typename T>
			bool CallAlphas(T, T, T& aAtk, T& aRel, Rank0)
			{
				aAtk = -1.0f;
				aRel = -1.0f;
				return false;
			}

			// ---- MotionStepAsymmetric(prev, target, alphaAttack, alphaRelease, reset) ----
			template <typename T>
			auto CallStepAsym(T prev, T target, T aAtk, T aRel, bool reset, float& out, Rank1)
				-> typename std::enable_if<!std::is_same<decltype(MotionStepAsymmetric(prev, target, aAtk, aRel, reset)), NoMatch>::value, bool>::type
			{
				out = static_cast<float>(MotionStepAsymmetric(prev, target, aAtk, aRel, reset));
				return true;
			}
			template <typename T>
			bool CallStepAsym(T, T, T, T, bool, float& out, Rank0)
			{
				out = -1.0f;
				return false;
			}

			// ---- MotionShearSpeed(...) over a bw x bh RGBA32F field (vx, vy, conf, err) ----
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank9)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(fieldRgba.data(), bw, bh, bx, by, cropH, dt)), NoMatch>::value, bool>::type
			{
				out = static_cast<float>(MotionShearSpeed(fieldRgba.data(), bw, bh, bx, by, cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank8)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(bx, by, bw, bh, fieldRgba.data(), cropH, dt)), NoMatch>::value, bool>::type
			{
				out = static_cast<float>(MotionShearSpeed(bx, by, bw, bh, fieldRgba.data(), cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank7)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(fieldRgba, bw, bh, bx, by, cropH, dt)), NoMatch>::value, bool>::type
			{
				out = static_cast<float>(MotionShearSpeed(fieldRgba, bw, bh, bx, by, cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank6)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(typename Vec::value_type{}, typename Vec::value_type{}, fieldRgba.data(), fieldRgba.data(), fieldRgba.data(), cropH, dt)), NoMatch>::value, bool>::type
			{
				auto cell = [&](int x, int y) -> const float* {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
				};
				const float* c = cell(bx, by);
				const float* n[4] = { cell(bx - 1, by), cell(bx + 1, by), cell(bx, by - 1), cell(bx, by + 1) };
				const float nVx[4] = { n[0][0], n[1][0], n[2][0], n[3][0] };
				const float nVy[4] = { n[0][1], n[1][1], n[2][1], n[3][1] };
				const float nConf[4] = { n[0][2], n[1][2], n[2][2], n[3][2] };
				out = static_cast<float>(MotionShearSpeed(c[0], c[1], nVx, nVy, nConf, cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank5)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(typename Vec::value_type{}, typename Vec::value_type{}, std::declval<const typename Vec::value_type(&)[4][4]>(), cropH, dt)), NoMatch>::value, bool>::type
			{
				auto cell = [&](int x, int y) -> const float* {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
				};
				const float* c = cell(bx, by);
				const float* ns[4] = { cell(bx - 1, by), cell(bx + 1, by), cell(bx, by - 1), cell(bx, by + 1) };
				float n4[4][4] = {};
				for (int i = 0; i < 4; ++i)
					for (int k = 0; k < 4; ++k)
						n4[i][k] = ns[i][k];
				out = static_cast<float>(MotionShearSpeed(c[0], c[1], n4, cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank4)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(typename Vec::value_type{}, typename Vec::value_type{}, std::declval<const typename Vec::value_type(&)[4][3]>(), cropH, dt)), NoMatch>::value, bool>::type
			{
				auto cell = [&](int x, int y) -> const float* {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
				};
				const float* c = cell(bx, by);
				const float* ns[4] = { cell(bx - 1, by), cell(bx + 1, by), cell(bx, by - 1), cell(bx, by + 1) };
				float n3[4][3] = {};
				for (int i = 0; i < 4; ++i)
					for (int k = 0; k < 3; ++k)
						n3[i][k] = ns[i][k];
				out = static_cast<float>(MotionShearSpeed(c[0], c[1], n3, cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank3)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(fieldRgba.data(), fieldRgba.data(), fieldRgba.data(), fieldRgba.data(), fieldRgba.data(), cropH, dt)), NoMatch>::value, bool>::type
			{
				auto cell = [&](int x, int y) -> const float* {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
				};
				out = static_cast<float>(MotionShearSpeed(cell(bx, by), cell(bx - 1, by), cell(bx + 1, by), cell(bx, by - 1), cell(bx, by + 1), cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank2)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(typename Vec::value_type{}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, cropH, dt)), NoMatch>::value, bool>::type
			{
				auto cell = [&](int x, int y) -> const float* {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
				};
				const float* c = cell(bx, by);
				const float* l = cell(bx - 1, by);
				const float* r = cell(bx + 1, by);
				const float* u = cell(bx, by - 1);
				const float* d = cell(bx, by + 1);
				out = static_cast<float>(MotionShearSpeed(c[0], c[1], l[0], l[1], l[2], r[0], r[1], r[2], u[0], u[1], u[2], d[0], d[1], d[2], cropH, dt));
				return true;
			}
			template <typename Vec>
			auto CallShear(const Vec& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt, float& out, Rank1)
				-> typename std::enable_if<!std::is_same<decltype(MotionShearSpeed(typename Vec::value_type{}, 0.0f, 0.0f, 0.0f, 0.0f, cropH, dt)), NoMatch>::value, bool>::type
			{
				auto cell = [&](int x, int y) -> const float* {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
				};
				const float* c = cell(bx, by);
				const float* ns[4] = { cell(bx - 1, by), cell(bx + 1, by), cell(bx, by - 1), cell(bx, by + 1) };
				float best = 0.0f;
				for (int i = 0; i < 4; ++i)
					best = (std::max)(best, static_cast<float>(MotionShearSpeed(c[0], c[1], ns[i][0], ns[i][1], ns[i][2], cropH, dt)));
				out = best;
				return true;
			}
			template <typename Vec>
			bool CallShear(const Vec&, int, int, int, int, float, float, float& out, Rank0)
			{
				out = -1.0f;
				return false;
			}

			// ---- MotionAdvectBilinear(...) at pc = (bx + 0.5) * 16 - v ----
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec& rgbaMap, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank8)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(scalarMap.data(), bw, bh, 0.0f, 0.0f, 1)), NoMatch>::value, bool>::type
			{
				(void)rgbaMap;
				const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
				const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
				out = static_cast<float>(MotionAdvectBilinear(scalarMap.data(), bw, bh, pcX, pcY, 1));
				return true;
			}
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec& rgbaMap, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank7)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(scalarMap.data(), bw, bh, 0.0f, 0.0f)), NoMatch>::value, bool>::type
			{
				const float probe[8] = { 0.0f, 0.25f, 0.0f, 0.0f, 0.75f, 0.0f, 0.0f, 0.0f };
				const float p = static_cast<float>(MotionAdvectBilinear(probe, 2, 1, 24.0f, 8.0f));
				const bool rgbaStride = std::fabs(p - 0.75f) < std::fabs(p - 0.25f);
				const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
				const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
				out = static_cast<float>(MotionAdvectBilinear(rgbaStride ? rgbaMap.data() : scalarMap.data(), bw, bh, pcX, pcY));
				return true;
			}
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec& rgbaMap, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank6)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(0.0f, 0.0f, scalarMap.data(), bw, bh)), NoMatch>::value, bool>::type
			{
				const float probe[8] = { 0.0f, 0.25f, 0.0f, 0.0f, 0.75f, 0.0f, 0.0f, 0.0f };
				const float p = static_cast<float>(MotionAdvectBilinear(24.0f, 8.0f, probe, 2, 1));
				const bool rgbaStride = std::fabs(p - 0.75f) < std::fabs(p - 0.25f);
				const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
				const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
				out = static_cast<float>(MotionAdvectBilinear(pcX, pcY, rgbaStride ? rgbaMap.data() : scalarMap.data(), bw, bh));
				return true;
			}
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec& rgbaMap, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank5)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(scalarMap, bw, bh, 0.0f, 0.0f)), NoMatch>::value, bool>::type
			{
				const std::vector<float> probe = { 0.0f, 0.25f, 0.0f, 0.0f, 0.75f, 0.0f, 0.0f, 0.0f };
				const float p = static_cast<float>(MotionAdvectBilinear(probe, 2, 1, 24.0f, 8.0f));
				const bool rgbaStride = std::fabs(p - 0.75f) < std::fabs(p - 0.25f);
				const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
				const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
				out = static_cast<float>(MotionAdvectBilinear(rgbaStride ? rgbaMap : scalarMap, bw, bh, pcX, pcY));
				return true;
			}
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec& rgbaMap, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank4)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(scalarMap.data(), bw, bh, bx, by, vx, vy)), NoMatch>::value, bool>::type
			{
				const float probe[8] = { 0.0f, 0.25f, 0.0f, 0.0f, 0.75f, 0.0f, 0.0f, 0.0f };
				const float p = static_cast<float>(MotionAdvectBilinear(probe, 2, 1, 1, 0, 0.0f, 0.0f));
				const bool rgbaStride = std::fabs(p - 0.75f) < std::fabs(p - 0.25f);
				out = static_cast<float>(MotionAdvectBilinear(rgbaStride ? rgbaMap.data() : scalarMap.data(), bw, bh, bx, by, vx, vy));
				return true;
			}
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec&, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank3)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(bw, bh, typename Vec::value_type{}, typename Vec::value_type{}, std::declval<std::function<typename Vec::value_type(int, int)>>())), NoMatch>::value, bool>::type
			{
				const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
				const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
				auto fetch = [&](int x, int y) -> float {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return scalarMap[static_cast<size_t>(cy) * bw + cx];
				};
				out = static_cast<float>(MotionAdvectBilinear(bw, bh, pcX, pcY, fetch));
				return true;
			}
			template <typename Vec>
			auto CallAdvect(const Vec& scalarMap, const Vec&, int bw, int bh, int bx, int by, float vx, float vy, float& out, Rank2)
				-> typename std::enable_if<!std::is_same<decltype(MotionAdvectBilinear(typename Vec::value_type{}, typename Vec::value_type{}, bw, bh, std::declval<std::function<typename Vec::value_type(int, int)>>())), NoMatch>::value, bool>::type
			{
				const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
				const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
				auto fetch = [&](int x, int y) -> float {
					const int cx = (std::max)(0, (std::min)(bw - 1, x));
					const int cy = (std::max)(0, (std::min)(bh - 1, y));
					return scalarMap[static_cast<size_t>(cy) * bw + cx];
				};
				out = static_cast<float>(MotionAdvectBilinear(pcX, pcY, bw, bh, fetch));
				return true;
			}
			template <typename Vec>
			bool CallAdvect(const Vec&, const Vec&, int, int, int, int, float, float, float& out, Rank0)
			{
				out = -1.0f;
				return false;
			}
		}

		inline bool HasNewMotionTarget()
		{
			const AdaptiveDownscale::Settings s{};
			return decltype(AdProbe::EvalNewTarget(s, Rank1{}))::value;
		}

		inline float CallMotionTarget(bool frameTrusted, float speed, float shearSpeed, float confidence, const AdaptiveDownscale::Settings& s)
		{
			return AdProbe::CallTarget(frameTrusted, speed, shearSpeed, confidence, s, Rank1{});
		}

		// Independent physics / spec reference formulas (derived directly from the locked spec).
		inline float RefSmoothstep(float lo, float hi, float x)
		{
			if (!(hi > lo))
				return x < lo ? 0.0f : 1.0f;
			const float t = RampMath::Clamp01((x - lo) / (hi - lo));
			return t * t * (3.0f - 2.0f * t);
		}

		inline float RefOption4BTarget(bool frameTrusted, float speed, float shearSpeed, float confidence, const AdaptiveDownscale::Settings& s)
		{
			const float fl = RampMath::Clamp01(s.motionFloor);
			const float c = frameTrusted ? RampMath::Clamp01(confidence) : 0.0f;
			const float ramp = (std::max)(
				RefSmoothstep(s.motionSpeedLo, s.motionSpeedHi, shearSpeed),
				RefSmoothstep(2.0f * s.motionSpeedHi, 4.0f * s.motionSpeedHi, speed));
			return fl + (1.0f - fl) * c * (1.0f - ramp);
		}

		inline float RefShearSpeed(const std::vector<float>& fieldRgba, int bw, int bh, int bx, int by, float cropH, float dt)
		{
			if (!(cropH > 0.0f) || !(dt > 0.0f))
				return 0.0f;
			auto cell = [&](int x, int y) -> const float* {
				const int cx = (std::max)(0, (std::min)(bw - 1, x));
				const int cy = (std::max)(0, (std::min)(bh - 1, y));
				return &fieldRgba[(static_cast<size_t>(cy) * bw + cx) * 4];
			};
			const float* cb = cell(bx, by);
			const int dx[4] = { -1, 1, 0, 0 };
			const int dy[4] = { 0, 0, -1, 1 };
			float maxWeighted = 0.0f;
			for (int i = 0; i < 4; ++i)
			{
				const float* cn = cell(bx + dx[i], by + dy[i]);
				const float conf = RampMath::Clamp01(cn[2]);
				const float dvx = cn[0] - cb[0];
				const float dvy = cn[1] - cb[1];
				maxWeighted = (std::max)(maxWeighted, conf * std::sqrt(dvx * dvx + dvy * dvy));
			}
			return maxWeighted / (cropH * dt);
		}

		inline float RefAdvectBilinear(const std::vector<float>& scalarMap, int bw, int bh, int bx, int by, float vx, float vy)
		{
			const float pcX = (static_cast<float>(bx) + 0.5f) * 16.0f - vx;
			const float pcY = (static_cast<float>(by) + 0.5f) * 16.0f - vy;
			const float pposX = (std::max)(0.0f, (std::min)(static_cast<float>(bw - 1), pcX / 16.0f - 0.5f));
			const float pposY = (std::max)(0.0f, (std::min)(static_cast<float>(bh - 1), pcY / 16.0f - 0.5f));
			const int x0 = static_cast<int>(std::floor(pposX));
			const int y0 = static_cast<int>(std::floor(pposY));
			const int x1 = (std::min)(bw - 1, x0 + 1);
			const int y1 = (std::min)(bh - 1, y0 + 1);
			const float tx = pposX - static_cast<float>(x0);
			const float ty = pposY - static_cast<float>(y0);
			const float m00 = scalarMap[static_cast<size_t>(y0) * bw + x0];
			const float m10 = scalarMap[static_cast<size_t>(y0) * bw + x1];
			const float m01 = scalarMap[static_cast<size_t>(y1) * bw + x0];
			const float m11 = scalarMap[static_cast<size_t>(y1) * bw + x1];
			return (1.0f - tx) * (1.0f - ty) * m00 + tx * (1.0f - ty) * m10 +
				(1.0f - tx) * ty * m01 + tx * ty * m11;
		}

		struct TempCfgScope
		{
			std::string path;
			TempCfgScope()
			{
				char dir[MAX_PATH] = {};
				GetTempPathA(MAX_PATH, dir);
				char name[128] = {};
				std::snprintf(name, sizeof(name), "vp-adaptive-motion-upgrade-%lu-%lu.cfg",
					static_cast<unsigned long>(GetCurrentProcessId()),
					static_cast<unsigned long>(GetCurrentThreadId()));
				path = std::string(dir) + name;
			}
			~TempCfgScope()
			{
				if (!path.empty())
					DeleteFileA(path.c_str());
			}
			TempCfgScope(const TempCfgScope&) = delete;
			TempCfgScope& operator=(const TempCfgScope&) = delete;
		};
	}

	// =================================================================================================
	// 1. Pure CPU Contract & Same-Input Negative Control Suite
	// =================================================================================================
	TEST_CLASS(AdaptiveMotionUpgradeCpuTests)
	{
	public:
		// Upgrade 1 (CPU): Continuous confidence in AdaptiveDownscale::MotionTarget(frameTrusted, speed,
		// shearSpeed, confidence, s) across f.z = {0.25, 0.45, 0.55, 0.80}, with same-input negative
		// controls proving both hard-cliff variants (f.z > 0.5 ? 1 : fl and f.z > 0.5 ? fl+(1-fl)*c : fl)
		// violate linear slope continuity across the 0.5 threshold.
		TEST_METHOD(ContinuousConfidenceMirrorMatchesLawAndRejectsHardCliff)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			AdaptiveDownscale::Settings s;
			s.motionFloor = 0.20f;
			s.motionSpeedLo = 0.10f;
			s.motionSpeedHi = 0.40f;

			const float probes[4] = { 0.25f, 0.45f, 0.55f, 0.80f };
			float continuousGot[4] = {};
			float naiveBinaryCliff[4] = {};
			float naiveGatedLinearCliff[4] = {};

			for (int i = 0; i < 4; ++i)
			{
				const float fz = probes[i];
				continuousGot[i] = CallMotionTarget(true, 0.0f, 0.0f, fz, s);
				naiveBinaryCliff[i] = fz > 0.5f ? 1.0f : s.motionFloor;
				naiveGatedLinearCliff[i] = fz > 0.5f ? (s.motionFloor + (1.0f - s.motionFloor) * fz) : s.motionFloor;
			}

			// Negative controls on the exact same {0.25, 0.45, 0.55, 0.80} inputs:
			// A linear law with span (1 - fl) = 0.80 moves by 0.16 over [0.25, 0.45] and by 0.08 over [0.45, 0.55].
			// Both cliff baselines stay frozen (0.0) over [0.25, 0.45] and jump >= 0.44 over [0.45, 0.55].
			Assert::AreEqual(0.0f, naiveBinaryCliff[1] - naiveBinaryCliff[0], 1e-6f,
				L"negative control (binary cliff): frozen below 0.5");
			Assert::IsTrue(naiveBinaryCliff[2] - naiveBinaryCliff[1] > 0.70f,
				L"negative control (binary cliff): massive discontinuity across 0.45 -> 0.55");
			Assert::AreEqual(0.0f, naiveGatedLinearCliff[1] - naiveGatedLinearCliff[0], 1e-6f,
				L"negative control (gated linear cliff): frozen below 0.5");
			Assert::IsTrue(naiveGatedLinearCliff[2] - naiveGatedLinearCliff[1] > 0.40f,
				L"negative control (gated linear cliff): step jump across 0.45 -> 0.55 exceeds 5x true slope");

			// Positive contract assertions: exact 5-arg signature, linear continuity & clamping.
			Assert::IsTrue(HasNewMotionTarget(),
				L"AdaptiveDownscale::MotionTarget(bool, float, float, float, const Settings&) must exist");
			for (int i = 0; i < 4; ++i)
			{
				const float expected = RefOption4BTarget(true, 0.0f, 0.0f, probes[i], s);
				Assert::AreEqual(expected, continuousGot[i], 1e-6f,
					(L"MotionTarget mismatch at confidence " + std::to_wstring(probes[i])).c_str());
			}
			Assert::AreEqual(0.16f, continuousGot[1] - continuousGot[0], 1e-6f, L"0.25 -> 0.45 linear step");
			Assert::AreEqual(0.08f, continuousGot[2] - continuousGot[1], 1e-6f, L"0.45 -> 0.55 linear step (no 0.5 cliff)");
			Assert::AreEqual(0.20f, continuousGot[3] - continuousGot[2], 1e-6f, L"0.55 -> 0.80 linear step");

			// Frame untrusted forces c = 0 -> floor regardless of block confidence; out-of-range c clamps to [0, 1].
			Assert::AreEqual(0.20f, CallMotionTarget(false, 0.0f, 0.0f, 0.80f, s), 1e-6f, L"untrusted frame -> floor");
			Assert::AreEqual(0.20f, CallMotionTarget(true, 0.0f, 0.0f, -0.50f, s), 1e-6f, L"negative confidence -> 0 -> floor");
			Assert::AreEqual(1.00f, CallMotionTarget(true, 0.0f, 0.0f, 1.75f, s), 1e-6f, L"confidence > 1 clamps to 1");

			// HIGH-1: Non-finite (NaN / +Inf / -Inf) confidence, speed, shear, and motionStrength must sanitize safely
			// (never propagate NaN, never treat +Inf confidence as 1.0f trusted!).
			const float qnan = std::numeric_limits<float>::quiet_NaN();
			const float pinf = std::numeric_limits<float>::infinity();
			const float ninf = -pinf;
			for (float badConf : { qnan, pinf, ninf })
			{
				const float t = CallMotionTarget(true, 0.0f, 0.0f, badConf, s);
				Assert::IsTrue(std::isfinite(t), L"non-finite confidence must not yield NaN/Inf");
				Assert::AreEqual(0.20f, t, 1e-6f, L"non-finite confidence (including +Inf) must be treated as untrusted 0 -> floor");
			}
			for (float badSpd : { qnan, pinf, ninf })
			{
				const float tSpd = CallMotionTarget(true, badSpd, 0.0f, 1.0f, s);
				const float tShr = CallMotionTarget(true, 0.0f, badSpd, 1.0f, s);
				Assert::IsTrue(std::isfinite(tSpd) && std::isfinite(tShr), L"non-finite speed/shear must yield finite target");
				Assert::AreEqual(0.20f, tSpd, 1e-6f, L"non-finite speed must fail safe to motionFloor");
				Assert::AreEqual(0.20f, tShr, 1e-6f, L"non-finite shearSpeed must fail safe to motionFloor");
			}
			Assert::IsTrue(std::isfinite(CallMotionTerm4(true, qnan, 0.2f, 1.0f)), L"NaN mapFactor must sanitize");
			Assert::AreEqual(0.0f, CallMotionTerm4(true, qnan, 0.2f, 1.0f), 1e-6f, L"NaN mapFactor -> alias-safe 0");
			Assert::AreEqual(1.0f, CallMotionTerm4(true, 0.2f, 0.0f, qnan), 1e-6f, L"NaN motionStrength -> 0 (standard blend)");
			Assert::AreEqual(1.0f, CallMotionTerm4(true, 0.2f, 0.0f, pinf), 1e-6f, L"+Inf motionStrength -> sanitized 0 (standard blend)");
		}

		// Upgrades 2 & 3 (CPU): Single motionStrength continuum (HybridBlend::MotionTerm, Settings::motionStrength),
		// schema key adaptive_motion ("off"/"on"/0.0..1.0), VpvRendererSettings profile load + Original reset +
		// delta/fingerprint, P10g (kP10gGrainLevel) Acquire seam, visible strength readout, and verbatim
		// "upscale motion: no vectors (alias-safe weight)" fallback.
		TEST_METHOD(MotionStrengthContinuumSchemaProfileAndStageReadouts)
		{
			using namespace AdaptiveMotionUpgradeDetail;

			// 1. Schema validation: "off", "on", and numeric 0.0..1.0 valid; out-of-range / garbage rejected.
			auto schemaValid = [](const char* val) {
				std::string expected;
				return RendererProfileConfig::ValidateProfileSetting("input", "adaptive_motion", val, expected);
			};
			for (const char* ok : { "off", "on", "0", "0.0", "0.35", "0.5", "1", "1.0" })
				Assert::IsTrue(schemaValid(ok), (L"adaptive_motion must accept '" + Wide(ok) + L"'").c_str());
			for (const char* bad : { "-0.01", "1.01", "2.5", "auto", "zzz_invalid", "" })
				Assert::IsFalse(schemaValid(bad), (L"negative control: adaptive_motion must reject '" + Wide(bad) + L"'").c_str());

			// 2. Settings::motionStrength default (0.0f), equality, and HybridBlend::MotionTerm 4-arg continuum.
			Assert::IsTrue(SettingsHasMotionStrength(), L"AdaptiveDownscale::Settings::motionStrength must exist");
			Assert::IsTrue(HasMotionTerm4(), L"HybridBlend::MotionTerm 4-arg continuum overload must exist");
			{
				AdaptiveDownscale::Settings a{}, b{};
				Assert::AreEqual(0.0f, GetMotionStrength(a), 1e-7f, L"default motionStrength must be 0.0f");
				Assert::IsTrue(a == b);
				SetMotionStrength(b, 0.35f);
				Assert::IsFalse(a == b, L"Settings::operator== must compare motionStrength");
			}
			// Negative control on MotionTerm: a boolean toggle (strength > 0 ? base : 1) fails at strength = 0.5.
			const float naiveBooleanToggleAtHalf = (0.5f > 0.0f) ? 0.2f : 1.0f;
			const float continuumAtHalf = CallMotionTerm4(true, 0.2f, 0.0f, 0.5f);
			Assert::IsTrue(std::fabs(naiveBooleanToggleAtHalf - 0.6f) > 0.35f, L"negative control: boolean toggle misses midpoint 0.6");
			Assert::AreEqual(1.0f, CallMotionTerm4(true, 0.0f, 0.0f, 0.0f), 1e-6f, L"strength 0 (valid) -> 1.0 exactly");
			Assert::AreEqual(1.0f, CallMotionTerm4(false, 0.0f, 0.0f, 0.0f), 1e-6f, L"strength 0 (invalid vectors) -> 1.0 (no floor lean)");
			Assert::AreEqual(0.6f, continuumAtHalf, 1e-6f, L"strength 0.5, mapFactor 0.2 -> 1 - 0.5*(1 - 0.2) = 0.6");
			Assert::AreEqual(0.625f, CallMotionTerm4(false, 0.9f, 0.25f, 0.5f), 1e-6f, L"strength 0.5, invalid vectors, floor 0.25 -> 0.625");
			Assert::AreEqual(0.2f, CallMotionTerm4(true, 0.2f, 0.0f, 1.0f), 1e-6f, L"strength 1.0 -> mapFactor");
			Assert::AreEqual(0.25f, CallMotionTerm4(false, 0.9f, 0.25f, 1.0f), 1e-6f, L"strength 1.0, invalid -> floor");
			Assert::AreEqual(0.25f, HybridBlend::MotionTerm(false, 0.9f, 0.25f), 1e-6f, L"default 4th arg = 1.0f");
			Assert::AreEqual(1.0f, CallMotionTerm4(true, 0.2f, 0.0f, -0.4f), 1e-6f, L"strength < 0 clamps to 0");
			Assert::AreEqual(0.2f, CallMotionTerm4(true, 0.2f, 0.0f, 1.8f), 1e-6f, L"strength > 1 clamps to 1");

			// 3. Profile loading ("off" -> 0.0, "on" -> 1.0, "0.35" -> 0.35), invalid retention, Original pathway reset, Delta & Fingerprint.
			{
				TempCfgScope tmp;
				auto loadProfileValue = [&](const char* rawVal, VpvRenderer::Fields& fields) {
					{
						std::ofstream out(tmp.path, std::ios::trunc | std::ios::binary);
						out << "[general]\nrenderer: VideoProcessor Renderer (Alpha)\n[vprenderer.Default]\nquality: high\nadaptive_motion: "
							<< rawVal << "\n";
					}
					ConfigFile cfg;
					Assert::IsTrue(cfg.Load(tmp.path, ConfigFile::ReadPolicy::Fresh), L"temp cfg load failed");
					VpvRendererSettings::ReadProfile(cfg, "vprenderer.Default", "Default", false, fields);
				};

				VpvRenderer::Fields fields;
				Assert::AreEqual(0.0f, GetMotionStrength(fields.adaptive), 1e-6f, L"Fields default motionStrength == 0");

				loadProfileValue("on", fields);
				Assert::AreEqual(1.0f, GetMotionStrength(fields.adaptive), 1e-6f, L"adaptive_motion: on -> 1.0f");

				loadProfileValue("0.35", fields);
				Assert::AreEqual(0.35f, GetMotionStrength(fields.adaptive), 1e-6f, L"adaptive_motion: 0.35 -> 0.35f");
				{
					ConfigFile cfg;
					Assert::IsTrue(cfg.Load(tmp.path, ConfigFile::ReadPolicy::Fresh));
					std::string schemaErr;
					Assert::IsTrue(MainConfigSchema::Validate(cfg, schemaErr),
						(L"MainConfigSchema::Validate rejected adaptive_motion: 0.35: " + Wide(schemaErr)).c_str());
				}

				loadProfileValue("1.5", fields);   // out of [0, 1]: must keep 0.35f
				Assert::AreEqual(0.35f, GetMotionStrength(fields.adaptive), 1e-6f, L"invalid 1.5 must retain prior 0.35f");

				loadProfileValue("off", fields);
				Assert::AreEqual(0.0f, GetMotionStrength(fields.adaptive), 1e-6f, L"adaptive_motion: off -> 0.0f");

				// AppendDelta & Fingerprint sensitivity to fractional motionStrength.
				VpvRenderer::Fields f0, f35;
				SetMotionStrength(f0.adaptive, 0.0f);
				SetMotionStrength(f35.adaptive, 0.35f);
				std::vector<const char*> delta;
				VpvRendererSettings::AppendDelta(f0, f35, delta);
				bool foundDelta = false;
				for (const char* k : delta)
					if (std::strcmp(k, "adaptive_motion") == 0)
						foundDelta = true;
				Assert::IsTrue(foundDelta, L"AppendDelta must report adaptive_motion between 0.0 and 0.35");
				Assert::AreNotEqual(VpvRendererSettings::Fingerprint(f0, true), VpvRendererSettings::Fingerprint(f35, true),
					L"Fingerprint must distinguish motionStrength 0.0 vs 0.35");

				// BLOCKER-2: Describe() preserves checked-in golden format ("off" at <=0/non-finite, "on" at >=1, numeric in (0,1)).
				{
					const Dejudder::Settings m{};
					auto describeLine = [&](float s) {
						VpvRenderer::Fields f;
						SetMotionStrength(f.adaptive, s);
						return VpvRendererSettings::Describe(f, "", m, "Default");
					};
					Assert::IsTrue(describeLine(0.0f).find("\nadaptive_motion=off\n") != std::string::npos,
						L"Describe() at 0.0f must emit adaptive_motion=off (golden compatibility)");
					Assert::IsTrue(describeLine(1.0f).find("\nadaptive_motion=on\n") != std::string::npos,
						L"Describe() at 1.0f must emit adaptive_motion=on");
					Assert::IsTrue(describeLine(0.5f).find("\nadaptive_motion=0.5\n") != std::string::npos,
						L"Describe() at 0.5f must emit adaptive_motion=0.5");
					Assert::IsTrue(describeLine(std::numeric_limits<float>::quiet_NaN()).find("\nadaptive_motion=off\n") != std::string::npos,
						L"Describe() at NaN must emit adaptive_motion=off");
					Assert::IsTrue(describeLine(std::numeric_limits<float>::infinity()).find("\nadaptive_motion=off\n") != std::string::npos,
						L"Describe() at +Inf must emit adaptive_motion=off");
				}

				// Original == Bill: PostLoad on Original pathway forces motionStrength to 0.0f.
				f35.pipelineOriginalPathway = true;
				VpvRendererSettings::PostLoad(f35, false, nullptr);
				Assert::AreEqual(0.0f, GetMotionStrength(f35.adaptive), 1e-6f, L"Original pathway must reset motionStrength to 0.0f");
			}

			// 4. Stage::Register: P10g (kP10gGrainLevel) via Acquire, visible strength in StatusLines(),
			//    verbatim "upscale motion: no vectors (alias-safe weight)" when motionStrength > 0 & vectors missing,
			//    and zero fallback / zero grainMapWanted when motionStrength == 0.
			{
				AdaptiveDownscale::Module module;
				module.SetCutSource([](uint64_t, uint64_t) {
					AdaptiveDownscale::CutVerdict v;
					v.state = AdaptiveDownscale::CutVerdict::State::Known;
					return v;
				});
				AdaptiveDownscaleStage::Stage stage{ module };
				AdaptiveDownscale::Settings settings;
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				int publishedCount = 0;
				int p10gAsks = 0, p3Asks = 0, otherAsks = 0;
				static int dummyGrainTex = 0;
				static int dummyMotionTex = 0;
				bool vectorsLive = false;

				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
				host.ssimInChain = []() { return true; };
				host.publishStatus = [&]() { ++publishedCount; };
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey& key) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel && key.IsDefault())
					{
						++p10gAsks;
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyGrainTex;
					}
					else if (id == VpSkeleton::kP3AnalysisField && key.IsDefault())
					{
						++p3Asks;
						if (vectorsLive)
						{
							r.status = VpSkeleton::AcquireStatus::Reused;
							r.gpu.resource = &dummyMotionTex;
						}
						else
							r.cause = VpSkeleton::AcquireCause::OwnerDeclined;
					}
					else
					{
						++otherAsks;
					}
					return r;
				};
				host.motion = [&]() {
					AdaptiveDownscaleStage::MotionSource m;
					m.live = vectorsLive;
					m.field = vectorsLive ? reinterpret_cast<pl_tex>(&dummyMotionTex) : nullptr;
					m.frameTrust = vectorsLive ? reinterpret_cast<pl_tex>(&dummyMotionTex) : nullptr;
					return m;
				};

				auto makeFrame = [&](uint64_t seq) {
					AdaptiveDownscaleStage::Frame f;
					f.adaptive = &settings;
					f.adaptiveUpscale = true;
					f.lowBitrateOff = true;
					f.sourceSequence = seq;
					f.nominalSourceRateHz = 24.0;
					f.sourceCrop = { 0.0f, 0.0f, 1920.0f, 1080.0f };
					f.targetCrop = { 0.0f, 0.0f, 2560.0f, 1440.0f };
					return f;
				};

				// At motionStrength = 0.0f with missing vectors: NO vector fallback and NO "no vectors" status line!
				SetMotionStrength(settings, 0.0f);
				vectorsLive = false;
				{
					std::vector<const pl_hook*> hooks;
					stage.Register(makeFrame(1), host, { weight, grainWanted, noSsim }, hooks);
					Assert::AreEqual(1, p10gAsks, L"P10g must be acquired once per upscale frame via Acquire");
					Assert::AreEqual(1, p3Asks, L"P3 must be acquired once per 2-axis upscale frame via Acquire");
					Assert::AreEqual(0, otherAsks, L"no unexpected stage Acquire calls");
					Assert::IsFalse(grainWanted, L"motionStrength 0.0 must not request grain/motion map");
					const std::vector<std::string> lines0 = stage.StatusLines();
					Assert::IsFalse(HasSubstring(lines0, "upscale motion: no vectors (alias-safe weight)"),
						(L"motionStrength 0.0 must not emit missing-vector fallback: " + Wide(JoinLines(lines0))).c_str());
					Assert::IsTrue(stage.Fallback().reason.find("no vectors") == std::string::npos,
						L"motionStrength 0.0 must not engage no-vectors fallback");
				}

				// At motionStrength = 0.35f with missing vectors: verbatim fallback line + engaged Fallback() + visible "0.35" strength.
				SetMotionStrength(settings, 0.35f);
				vectorsLive = false;
				{
					std::vector<const pl_hook*> hooks;
					stage.Register(makeFrame(2), host, { weight, grainWanted, noSsim }, hooks);
					Assert::AreEqual(2, p10gAsks, L"P10g acquired once on frame 2");
					Assert::AreEqual(2, p3Asks, L"P3 acquired once on frame 2");
					Assert::AreEqual(0, otherAsks, L"no unexpected stage Acquire calls on frame 2");
					const std::vector<std::string> lines35 = stage.StatusLines();
					const std::string joined35 = JoinLines(lines35);
					Assert::IsTrue(HasSubstring(lines35, "upscale motion: no vectors (alias-safe weight)"),
						(L"missing verbatim 'upscale motion: no vectors (alias-safe weight)' in: " + Wide(joined35)).c_str());
					Assert::IsTrue(HasSubstring(lines35, "0.35"),
						(L"StatusLines() must visibly report motionStrength 0.35 in: " + Wide(joined35)).c_str());
					const FallbackCatalog::Decision fb = stage.Fallback();
					Assert::IsTrue(fb.engaged && fb.id == "output scaling" && fb.reason.find("no vectors") != std::string::npos,
						(L"Fallback() must engage output scaling no-vectors reason, got: " + Wide(fb.reason)).c_str());
				}

				// Changing motionStrength from 0.35f -> 0.75f with live vectors updates status readout and republishes.
				vectorsLive = true;
				const int pubBefore = publishedCount;
				SetMotionStrength(settings, 0.75f);
				{
					std::vector<const pl_hook*> hooks;
					stage.Register(makeFrame(3), host, { weight, grainWanted, noSsim }, hooks);
					Assert::AreEqual(3, p10gAsks, L"P10g acquired once on frame 3");
					Assert::AreEqual(3, p3Asks, L"P3 acquired once on frame 3");
					Assert::AreEqual(0, otherAsks, L"no unexpected stage Acquire calls on frame 3");
					const std::vector<std::string> lines75 = stage.StatusLines();
					const std::string joined75 = JoinLines(lines75);
					Assert::IsTrue(HasSubstring(lines75, "0.75") && !HasSubstring(lines75, "0.35"),
						(L"StatusLines() must update strength readout from 0.35 to 0.75, got: " + Wide(joined75)).c_str());
					Assert::IsFalse(HasSubstring(lines75, "upscale motion: no vectors (alias-safe weight)"),
						L"live consecutive vectors must clear the no-vectors status line");
					Assert::IsTrue(publishedCount > pubBefore, L"strength change in StatusLines() must trigger publishStatus");
				}
			}
		}

		// W23-B4 M1#1 (CPU): upscale motion acquires kP3AnalysisField (AcquireForm::Gpu), never silently falls back
		// to host.motion().field, pins the acquired handle while upscale is active and releases it on non-upscale,
		// fails visibly when frameTrust is absent, and abstains at zero cost on one-axis / vectorsRun=false / nlsUnknown.
		TEST_METHOD(UpscaleMotionAcquiresP3AnalysisFieldAndRejectsStaleHostMotionFallback)
		{
			using namespace AdaptiveMotionUpgradeDetail;

			AdaptiveDownscale::Module module;
			module.SetCutSource([](uint64_t, uint64_t) {
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				return v;
			});
			AdaptiveDownscaleStage::Stage stage{ module };
			AdaptiveDownscale::Settings settings;
			SetMotionStrength(settings, 0.5f);
			double weight = 1.0;
			bool grainWanted = false, noSsim = false;

			static int dummyGrainTex = 0;
			static int dummyHostField = 0;
			static int dummyP3Tex = 0;
			static int dummyFactsTrust = 0;
			static int dummyAuxTrust = 0;

			auto makeFrame = [&](uint64_t seq, float sw = 1920.0f, float sh = 1080.0f, float tw = 2560.0f, float th = 1440.0f) {
				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &settings;
				f.adaptiveUpscale = true;
				f.lowBitrateOff = true;
				f.sourceSequence = seq;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, sw, sh };
				f.targetCrop = { 0.0f, 0.0f, tw, th };
				return f;
			};

			// 1. Mandatory in-test negative control: host.motion has LIVE field + frameTrust, while
			//    Acquire(kP3AnalysisField) declines with OwnerDeclined. Stage MUST NOT fall back to host.motion().field.
			{
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
				host.ssimInChain = []() { return true; };
				host.publishStatus = []() {};
				host.motion = []() {
					AdaptiveDownscaleStage::MotionSource m;
					m.live = true;
					m.field = reinterpret_cast<pl_tex>(&dummyHostField);
					m.frameTrust = reinterpret_cast<pl_tex>(&dummyAuxTrust);
					return m;
				};
				host.acquire = [](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyGrainTex;
					}
					else if (id == VpSkeleton::kP3AnalysisField)
					{
						r.status = VpSkeleton::AcquireStatus::Unavailable;
						r.cause = VpSkeleton::AcquireCause::OwnerDeclined;
					}
					return r;
				};

				// Same-input naive negative control: reading host.motion() directly falsely reports live vectors!
				const AdaptiveDownscaleStage::MotionSource naiveMotion = host.motion();
				Assert::IsTrue(naiveMotion.live && naiveMotion.field != nullptr && naiveMotion.frameTrust != nullptr,
					L"negative control: naive host.motion()-only check falsely reports live == true on the exact same host");

				std::vector<const pl_hook*> hooks;
				stage.Register(makeFrame(10), host, { weight, grainWanted, noSsim }, hooks);
				Assert::IsFalse(stage.UpscaleMotionCause().empty(),
					L"OwnerDeclined on kP3AnalysisField must produce non-empty UpscaleMotionCause");
				Assert::IsTrue(stage.UpscaleMotionCause().find("kP3AnalysisField") != std::string::npos,
					(L"UpscaleMotionCause must name kP3AnalysisField, got: " + Wide(stage.UpscaleMotionCause())).c_str());
				const std::vector<std::string> lines = stage.StatusLines();
				const std::string joined = JoinLines(lines);
				Assert::IsTrue(HasSubstring(lines, "upscale motion: no vectors (alias-safe weight)"),
					(L"declined kP3AnalysisField must emit verbatim no-vectors status: " + Wide(joined)).c_str());
				Assert::IsFalse(HasSubstring(lines, "ExternalView"),
					(L"must never emit legacy ExternalView status line: " + Wide(joined)).c_str());
				const FallbackCatalog::Decision fb = stage.Fallback();
				Assert::IsTrue(fb.engaged && fb.id == "output scaling" && fb.reason.find("no vectors") != std::string::npos,
					(L"declined kP3AnalysisField must engage output scaling no-vectors fallback, got: " + Wide(fb.reason)).c_str());
				Assert::IsTrue(fb.reason.find("ExternalView") == std::string::npos,
					(L"Fallback() must not report ExternalView: " + Wide(fb.reason)).c_str());
			}

			// 2a. Positive Acquire proof via VerifiedField::trust in P3FieldFactsTag() + pin lifetime across non-upscale.
			{
				std::shared_ptr<int> pin = std::make_shared<int>(42);
				auto facts = std::make_shared<VpMotion::VerifiedField>();
				facts->fwd = reinterpret_cast<pl_tex>(&dummyP3Tex);
				facts->trust = reinterpret_cast<pl_tex>(&dummyFactsTrust);

				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
				host.ssimInChain = []() { return true; };
				host.publishStatus = []() {};
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource{}; };   // no auxiliary trust needed
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyGrainTex;
					}
					else if (id == VpSkeleton::kP3AnalysisField)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyP3Tex;
						r.gpu.textureSet = VpMotion::P3FieldFactsTag();
						r.gpu.keepAlive = std::static_pointer_cast<void>(facts);
						r.pin = pin;
					}
					return r;
				};

				std::vector<const pl_hook*> hooks;
				stage.Register(makeFrame(11), host, { weight, grainWanted, noSsim }, hooks);
				Assert::IsTrue(stage.UpscaleMotionCause().empty(),
					(L"live kP3AnalysisField with P3FieldFactsTag trust must clear UpscaleMotionCause, got: " + Wide(stage.UpscaleMotionCause())).c_str());
				const std::vector<std::string> lines = stage.StatusLines();
				Assert::IsFalse(HasSubstring(lines, "ExternalView"), L"Acquire P3 must emit no ExternalView line");
				Assert::IsFalse(HasSubstring(lines, "no vectors"), L"live Acquire P3 must emit no no-vectors line");
				const FallbackCatalog::Decision fb = stage.Fallback();
				Assert::IsTrue(fb.reason.find("ExternalView") == std::string::npos, L"live Acquire P3 must not engage ExternalView fallback");
				Assert::IsTrue(fb.reason.find("no vectors") == std::string::npos, L"live Acquire P3 must not engage no-vectors fallback");
				Assert::IsTrue(pin.use_count() >= 2, L"kP3AnalysisField pin must be held while upscale is active");

				// Transition to non-upscale frame (1:1 native): pin must be released!
				hooks.clear();
				AdaptiveDownscaleStage::Frame nonUp = makeFrame(12, 1920.0f, 1080.0f, 1920.0f, 1080.0f);
				stage.Register(nonUp, host, { weight, grainWanted, noSsim }, hooks);
				Assert::AreEqual(long(1), pin.use_count(), L"transitioning to non-upscale frame must release kP3AnalysisField pin");
			}

			// 2b. Positive Acquire proof with auxiliary host.motion().frameTrust (no P3FieldFactsTag) + pin release.
			{
				std::shared_ptr<int> pinAux = std::make_shared<int>(99);
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
				host.ssimInChain = []() { return true; };
				host.publishStatus = []() {};
				host.motion = []() {
					AdaptiveDownscaleStage::MotionSource m;
					m.live = false;
					m.field = nullptr;
					m.frameTrust = reinterpret_cast<pl_tex>(&dummyAuxTrust);
					return m;
				};
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyGrainTex;
					}
					else if (id == VpSkeleton::kP3AnalysisField)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyP3Tex;
						r.pin = pinAux;
					}
					return r;
				};

				std::vector<const pl_hook*> hooks;
				stage.Register(makeFrame(13), host, { weight, grainWanted, noSsim }, hooks);
				Assert::IsTrue(stage.UpscaleMotionCause().empty(),
					(L"live kP3AnalysisField with auxiliary frameTrust must succeed, got: " + Wide(stage.UpscaleMotionCause())).c_str());
				Assert::IsFalse(HasSubstring(stage.StatusLines(), "ExternalView"), L"no ExternalView with auxiliary frameTrust");
				Assert::IsFalse(HasSubstring(stage.StatusLines(), "no vectors"), L"no no-vectors line with auxiliary frameTrust");
				Assert::IsTrue(pinAux.use_count() >= 2, L"kP3AnalysisField pin must be held with auxiliary frameTrust");

				hooks.clear();
				AdaptiveDownscaleStage::Frame offFrame = makeFrame(14);
				offFrame.adaptiveUpscale = false;
				stage.Register(offFrame, host, { weight, grainWanted, noSsim }, hooks);
				Assert::AreEqual(long(1), pinAux.use_count(), L"disabling upscale must release kP3AnalysisField pin");
			}

			// 3. Missing frameTrust visible fallback: kP3AnalysisField returns GPU resource, but BOTH VerifiedField::trust
			//    and host.motion().frameTrust are null -> visible fallback naming "kP3AnalysisField has no frame trust".
			{
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
				host.ssimInChain = []() { return true; };
				host.publishStatus = []() {};
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource{}; };
				host.acquire = [](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyGrainTex;
					}
					else if (id == VpSkeleton::kP3AnalysisField)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyP3Tex;
					}
					return r;
				};

				std::vector<const pl_hook*> hooks;
				stage.Register(makeFrame(15), host, { weight, grainWanted, noSsim }, hooks);
				Assert::AreEqual(std::string("kP3AnalysisField has no frame trust"), stage.UpscaleMotionCause(),
					L"missing frameTrust must surface exact cause");
				const std::vector<std::string> lines = stage.StatusLines();
				const std::string joined = JoinLines(lines);
				Assert::IsTrue(HasSubstring(lines, "upscale motion: no vectors (alias-safe weight)")
					&& HasSubstring(lines, "kP3AnalysisField has no frame trust"),
					(L"missing frameTrust must emit verbatim no-vectors + cause in: " + Wide(joined)).c_str());
				const FallbackCatalog::Decision fb = stage.Fallback();
				Assert::IsTrue(fb.engaged && fb.id == "output scaling"
					&& fb.reason.find("kP3AnalysisField has no frame trust") != std::string::npos,
					(L"missing frameTrust must engage Fallback() with cause, got: " + Wide(fb.reason)).c_str());
			}

			// 4. Zero-cost abstention: oneAxis upscale, vectorsRun == false, and nlsUnknown MUST NOT acquire kP3AnalysisField!
			{
				int p3Asks = 0;
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { NlsMaskMapping::Mapping m{}; m.valid = false; return m; };
				host.ssimInChain = []() { return true; };
				host.publishStatus = []() {};
				host.motion = []() { return AdaptiveDownscaleStage::MotionSource{}; };
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					if (id == VpSkeleton::kP3AnalysisField)
						++p3Asks;
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = &dummyGrainTex;
					}
					return r;
				};

				std::vector<const pl_hook*> hooks;
				// (a) one-axis upscale (width-only: 1920x1440 -> 2560x1440)
				stage.Register(makeFrame(16, 1920.0f, 1440.0f, 2560.0f, 1440.0f), host, { weight, grainWanted, noSsim }, hooks);
				Assert::IsTrue(stage.OneAxisThisFrame(), L"premise: 1920x1440 -> 2560x1440 is one-axis");
				Assert::AreEqual(0, p3Asks, L"one-axis upscale must not Acquire kP3AnalysisField");

				// (b) vectorsRun == false
				hooks.clear();
				AdaptiveDownscaleStage::Frame noVec = makeFrame(17);
				noVec.vectorsRun = false;
				stage.Register(noVec, host, { weight, grainWanted, noSsim }, hooks);
				Assert::AreEqual(0, p3Asks, L"vectorsRun == false must not Acquire kP3AnalysisField");

				// (c) nlsUnknown (nlsSubmitted == true, !nlsMap.valid)
				hooks.clear();
				AdaptiveDownscaleStage::Frame badNls = makeFrame(18);
				badNls.nlsSubmitted = true;
				badNls.nlsFile = "UnknownNLS.glsl";
				stage.Register(badNls, host, { weight, grainWanted, noSsim }, hooks);
				Assert::AreEqual(0, p3Asks, L"nlsUnknown must not Acquire kP3AnalysisField");
			}
		}

		// Upgrade 4 (CPU): 4-neighbor shear speed (MotionShearSpeed) + Option 4B ramp in MotionTarget:
		//   - Crisp rigid cinema pan (speed = 0.5 heights/s, shearSpeed = 0) stays 100% sharp (target = 1.0),
		//     while the same-input shear-unaware baseline collapses to floor (0.0)!
		//   - Moving-object boundary softens on BOTH foreground and background sides, in all 4 cardinal directions,
		//     and weights each neighbor by Clamp01(f_n.z) so untrusted garbage vectors cannot poison clean neighbors.
		//   - Extreme whip pan (speed >= 4 * speed_hi = 1.6 heights/s) drops to floor even with zero shear.
		TEST_METHOD(FourNeighborShearOption4BLawAndRigidPanNegativeControl)
		{
			using namespace AdaptiveMotionUpgradeDetail;

			AdaptiveDownscale::Settings s;
			s.motionSpeedLo = 0.10f;
			s.motionSpeedHi = 0.40f;
			s.motionFloor = 0.15f;

			const float cropH = 120.0f;
			const float dt = 1.0f / 24.0f;   // cropH * dt = 5.0 source px per (height/s)
			const int bw = 7, bh = 5;

			auto makeGrid = [&](const std::function<void(int, int, float*)>& fill) {
				std::vector<float> grid(static_cast<size_t>(bw) * bh * 4, 0.0f);
				for (int y = 0; y < bh; ++y)
					for (int x = 0; x < bw; ++x)
						fill(x, y, &grid[(static_cast<size_t>(y) * bw + x) * 4]);
				return grid;
			};

			// 1. Crisp rigid cinema pan: 0.5 heights/s = 2.5 px/frame everywhere, f.z = 1.0 everywhere.
			const std::vector<float> rigidPan = makeGrid([](int, int, float* c) {
				c[0] = 2.5f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;
			});
			float shearRigid = -1.0f;
			Assert::IsTrue(AdProbe::CallShear(rigidPan, bw, bh, 3, 2, cropH, dt, shearRigid, Rank9{}),
				L"AdaptiveDownscale::MotionShearSpeed(...) CPU mirror must exist");
			Assert::AreEqual(0.0f, shearRigid, 1e-6f, L"rigid pan has zero neighbor velocity difference -> shearSpeed == 0");

			// Negative control on the exact same rigid pan (speed = 0.5 > speed_hi = 0.4):
			// Legacy shear-unaware ramp smoothstep(0.1, 0.4, 0.5) == 1.0 -> collapses to motionFloor (0.15)!
			const float legacyShearUnawareTarget = s.motionFloor + (1.0f - s.motionFloor) * (1.0f - RefSmoothstep(s.motionSpeedLo, s.motionSpeedHi, 0.5f));
			Assert::AreEqual(s.motionFloor, legacyShearUnawareTarget, 1e-6f,
				L"negative control: shear-unaware ramp blurs 0.5 heights/s rigid cinema pan all the way to floor");
			Assert::IsTrue(HasNewMotionTarget(), L"5-arg MotionTarget with shearSpeed required");
			const float rigidTarget = CallMotionTarget(true, 0.5f, shearRigid, 1.0f, s);
			Assert::AreEqual(1.0f, rigidTarget, 1e-6f,
				L"Option 4B: crisp rigid cinema pan at 0.5 heights/s (< 2*speed_hi = 0.8) with zero shear MUST stay 1.0 sharp");

			// 2. Moving-object boundary: x <= 2 stationary (0 px/f), x >= 3 moving at 2.5 px/f (0.5 heights/s).
			//    Both boundary columns x = 2 (background side) and x = 3 (foreground side) see shearSpeed = 0.5 >= 0.4 -> floor!
			const std::vector<float> verticalEdge = makeGrid([](int x, int, float* c) {
				c[0] = x >= 3 ? 2.5f : 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;
			});
			for (int x = 0; x < bw; ++x)
			{
				float sh = -1.0f;
				Assert::IsTrue(AdProbe::CallShear(verticalEdge, bw, bh, x, 2, cropH, dt, sh, Rank9{}));
				const float expectedSh = RefShearSpeed(verticalEdge, bw, bh, x, 2, cropH, dt);
				Assert::AreEqual(expectedSh, sh, 1e-5f, (L"shearSpeed mismatch at column " + std::to_wstring(x)).c_str());
				const float spd = x >= 3 ? 0.5f : 0.0f;
				const float tgt = CallMotionTarget(true, spd, sh, 1.0f, s);
				if (x == 2 || x == 3)
					Assert::AreEqual(s.motionFloor, tgt, 1e-6f, L"both sides of shear boundary (x=2 and x=3) must soften to floor");
				else
					Assert::AreEqual(1.0f, tgt, 1e-6f, L"interior background (x<=1) and interior rigid object (x>=4) stay 1.0 sharp");
			}

			// 3. All 4 cardinal neighbors vs diagonals + Clamp01(f_n.z) neighbor confidence weighting:
			//    Single moving block at (3, 2) with vx = 1.25 px/f (0.25 heights/s -> midpoint of [0.1, 0.4], smoothstep = 0.5).
			const std::vector<float> singleMover = makeGrid([](int x, int y, float* c) {
				c[0] = (x == 3 && y == 2) ? 1.25f : 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;
			});
			for (const auto& nb : { std::make_pair(2, 2), std::make_pair(4, 2), std::make_pair(3, 1), std::make_pair(3, 3) })
			{
				float sh = 0.0f;
				Assert::IsTrue(AdProbe::CallShear(singleMover, bw, bh, nb.first, nb.second, cropH, dt, sh, Rank9{}));
				Assert::AreEqual(0.25f, sh, 1e-5f, L"4-neighbor cardinal shear must equal 0.25 heights/s");
				Assert::AreEqual(s.motionFloor + 0.5f * (1.0f - s.motionFloor),
					CallMotionTarget(true, 0.0f, sh, 1.0f, s), 1e-5f, L"midpoint shear -> 0.5 ramp");
			}
			for (const auto& diag : { std::make_pair(2, 1), std::make_pair(4, 1), std::make_pair(2, 3), std::make_pair(4, 3) })
			{
				float sh = -1.0f;
				Assert::IsTrue(AdProbe::CallShear(singleMover, bw, bh, diag.first, diag.second, cropH, dt, sh, Rank9{}));
				Assert::AreEqual(0.0f, sh, 1e-6f, L"diagonal neighbor must not contribute to 4-neighbor shear");
			}

			// Untrusted neighbor (f_n.z = 0.0) with garbage huge vector (50 px/frame) MUST NOT poison adjacent static block;
			// half-confident neighbor (f_n.z = 0.5) with 2.5 px/frame (0.5 heights/s) scales shear to 0.25 heights/s!
			const std::vector<float> rogueUntrusted = makeGrid([](int x, int y, float* c) {
				const bool rogue = (x == 4 && y == 2);
				c[0] = rogue ? 50.0f : 0.0f; c[1] = 0.0f; c[2] = rogue ? 0.0f : 1.0f; c[3] = 0.0f;
			});
			float shRogue = -1.0f;
			Assert::IsTrue(AdProbe::CallShear(rogueUntrusted, bw, bh, 3, 2, cropH, dt, shRogue, Rank9{}));
			Assert::AreEqual(0.0f, shRogue, 1e-6f, L"untrusted neighbor (f_n.z=0) must contribute zero shear");

			const std::vector<float> halfConfNeighbor = makeGrid([](int x, int y, float* c) {
				const bool half = (x == 4 && y == 2);
				c[0] = half ? 2.5f : 0.0f; c[1] = 0.0f; c[2] = half ? 0.5f : 1.0f; c[3] = 0.0f;
			});
			float shHalf = -1.0f;
			Assert::IsTrue(AdProbe::CallShear(halfConfNeighbor, bw, bh, 3, 2, cropH, dt, shHalf, Rank9{}));
			Assert::AreEqual(0.25f, shHalf, 1e-5f, L"f_n.z=0.5 scales 0.5 heights/s velocity delta to 0.25 shearSpeed");

			// HIGH-1: Untrusted neighbor with +Inf velocity (0.0 * Inf would be NaN!) or NaN confidence must NOT produce NaN.
			const float qnan = std::numeric_limits<float>::quiet_NaN();
			const float pinf = std::numeric_limits<float>::infinity();
			const std::vector<float> infNeighbor = makeGrid([pinf, qnan](int x, int y, float* c) {
				if (x == 4 && y == 2) { c[0] = pinf; c[1] = 0.0f; c[2] = 0.0f; c[3] = 0.0f; }
				else if (x == 2 && y == 2) { c[0] = 2.5f; c[1] = 0.0f; c[2] = qnan; c[3] = 0.0f; }
				else if (x == 3 && y == 1) { c[0] = 2.5f; c[1] = 0.0f; c[2] = pinf; c[3] = 0.0f; }
				else { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; }
			});
			float shInf = -1.0f;
			Assert::IsTrue(AdProbe::CallShear(infNeighbor, bw, bh, 3, 2, cropH, dt, shInf, Rank9{}));
			Assert::IsTrue(std::isfinite(shInf), L"0*Inf or NaN/+Inf neighbor confidence must never produce NaN shearSpeed");
			Assert::AreEqual(0.0f, shInf, 1e-6f, L"non-finite neighbor confidence/velocity contributes 0 shear");

			// 4. Whip-pan roll-off [2*speed_hi, 4*speed_hi] = [0.8, 1.6] heights/s with shearSpeed = 0:
			Assert::AreEqual(1.0f, CallMotionTarget(true, 0.80f, 0.0f, 1.0f, s), 1e-6f, L"2*speed_hi (0.8): still 1.0");
			Assert::AreEqual(s.motionFloor + 0.5f * (1.0f - s.motionFloor),
				CallMotionTarget(true, 1.20f, 0.0f, 1.0f, s), 1e-6f, L"3*speed_hi (1.2): smoothstep midpoint 0.5");
			Assert::AreEqual(s.motionFloor, CallMotionTarget(true, 1.60f, 0.0f, 1.0f, s), 1e-6f, L"4*speed_hi (1.6): drops to floor");
			Assert::AreEqual(s.motionFloor, CallMotionTarget(true, 3.00f, 0.0f, 1.0f, s), 1e-6f, L">4*speed_hi: stays at floor");
		}

		// Upgrade 5 (CPU): 2x2 bilinear advection (MotionAdvectBilinear at ppos = pc / 16 - 0.5 clamped to grid)
		// + asymmetric fast-attack (tau / 4) / smooth-release (tau) one-pole across 24 Hz and 60 Hz, with
		// same-input nearest-neighbor and symmetric-tau negative controls.
		TEST_METHOD(BilinearAdvectionAndAsymmetricOnePoleFramerateInvariance)
		{
			using namespace AdaptiveMotionUpgradeDetail;

			// 1. Bilinear advection vs nearest-neighbor floor(pc / 16) negative control on 8-px half-cell motion.
			const int bw = 4, bh = 3;
			std::vector<float> scalar(static_cast<size_t>(bw) * bh, 0.0f);
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x)
					scalar[static_cast<size_t>(y) * bw + x] = (x >= 2 && y >= 1) ? 1.0f : 0.2f;
			std::vector<float> rgba(scalar.size() * 4, 0.0f);
			for (size_t i = 0; i < scalar.size(); ++i)
			{
				rgba[i * 4] = scalar[i];
				rgba[i * 4 + 3] = 1.0f;
			}

			// At block (2, 1), center c = (40, 24). With vx = +8.0, vy = 0.0 (half-cell shift):
			// pc = (32, 24) -> ppos = (1.5, 1.0) -> 0.5 * map(1, 1) + 0.5 * map(2, 1) = 0.5 * 0.2 + 0.5 * 1.0 = 0.60!
			// Negative control (nearest floor(pc / 16)): floor(32 / 16) = 2 -> map(2, 1) = 1.00 (0% advection)!
			const float pcX = 2.5f * 16.0f - 8.0f;
			const int nearestBx = (std::max)(0, (std::min)(bw - 1, static_cast<int>(std::floor(pcX / 16.0f))));
			const float naiveNearest = scalar[static_cast<size_t>(1) * bw + nearestBx];
			Assert::AreEqual(1.0f, naiveNearest, 1e-6f, L"negative control: floor(pc/16) snaps half-cell 8px shift back to self");

			float gotHalfX = -1.0f;
			Assert::IsTrue(AdProbe::CallAdvect(scalar, rgba, bw, bh, 2, 1, 8.0f, 0.0f, gotHalfX, Rank8{}),
				L"AdaptiveDownscale::MotionAdvectBilinear(...) CPU mirror must exist");
			Assert::AreEqual(0.60f, gotHalfX, 1e-6f, L"bilinear 8px horizontal shift -> 0.5*0.2 + 0.5*1.0 = 0.60");

			// 2D diagonal shift vx = +8.0, vy = +8.0 at (2, 1) -> ppos = (1.5, 0.5):
			// corners (1,0)=0.2, (2,0)=0.2, (1,1)=0.2, (2,1)=1.0 -> 0.75*0.2 + 0.25*1.0 = 0.40!
			float gotDiag = -1.0f;
			Assert::IsTrue(AdProbe::CallAdvect(scalar, rgba, bw, bh, 2, 1, 8.0f, 8.0f, gotDiag, Rank8{}));
			Assert::AreEqual(RefAdvectBilinear(scalar, bw, bh, 2, 1, 8.0f, 8.0f), gotDiag, 1e-6f, L"2x2 diagonal bilinear advection");
			Assert::AreEqual(0.40f, gotDiag, 1e-6f);

			// Out-of-bounds clamping at both low and high edges.
			float gotClampLo = -1.0f, gotClampHi = -1.0f;
			Assert::IsTrue(AdProbe::CallAdvect(scalar, rgba, bw, bh, 0, 0, 48.0f, 48.0f, gotClampLo, Rank8{}));
			Assert::IsTrue(AdProbe::CallAdvect(scalar, rgba, bw, bh, 3, 2, -48.0f, -48.0f, gotClampHi, Rank8{}));
			Assert::AreEqual(0.2f, gotClampLo, 1e-6f, L"clamped to (0, 0)");
			Assert::AreEqual(1.0f, gotClampHi, 1e-6f, L"clamped to (bw-1, bh-1)");

			// 2. Asymmetric attack (tau = smoothMs / 4) & release (tau = smoothMs) alphas and stepping.
			float aAtk24 = -1.0f, aRel24 = -1.0f, aAtk60 = -1.0f, aRel60 = -1.0f, aAtk0 = -1.0f, aRel0 = -1.0f;
			Assert::IsTrue(AdProbe::CallAlphas(1.0f / 24.0f, 250.0f, aAtk24, aRel24, Rank1{}),
				L"AdaptiveDownscale::MotionAttackReleaseAlphas(...) must exist");
			Assert::IsTrue(AdProbe::CallAlphas(1.0f / 60.0f, 250.0f, aAtk60, aRel60, Rank1{}));
			Assert::IsTrue(AdProbe::CallAlphas(1.0f / 24.0f, 0.0f, aAtk0, aRel0, Rank1{}));

			const float expRel24 = OnePole::AlphaForMs(1000.0f / 24.0f, 250.0f, OnePole::DtRule::AsGiven);
			const float expAtk24 = OnePole::AlphaForMs(1000.0f / 24.0f, 250.0f / 4.0f, OnePole::DtRule::AsGiven);
			Assert::AreEqual(expAtk24, aAtk24, 1e-6f, L"attack alpha must use tau = smoothMs / 4 (62.5 ms)");
			Assert::AreEqual(expRel24, aRel24, 1e-6f, L"release alpha must use tau = smoothMs (250 ms)");
			Assert::AreEqual(1.0f, aAtk0, 1e-6f, L"0 ms -> alphaAttack == 1.0");
			Assert::AreEqual(1.0f, aRel0, 1e-6f, L"0 ms -> alphaRelease == 1.0");

			float stepVal = -1.0f;
			Assert::IsTrue(AdProbe::CallStepAsym(1.0f, 0.0f, aAtk24, aRel24, false, stepVal, Rank1{}),
				L"AdaptiveDownscale::MotionStepAsymmetric(...) must exist");
			Assert::AreEqual(1.0f - aAtk24, stepVal, 1e-6f, L"target < prev must use alphaAttack");
			Assert::IsTrue(AdProbe::CallStepAsym(0.0f, 1.0f, aAtk24, aRel24, false, stepVal, Rank1{}));
			Assert::AreEqual(aRel24, stepVal, 1e-6f, L"target > prev must use alphaRelease");
			Assert::IsTrue(AdProbe::CallStepAsym(1.0f, 0.25f, aAtk24, aRel24, true, stepVal, Rank1{}));
			Assert::AreEqual(0.25f, stepVal, 1e-6f, L"reset == true snaps directly to target");

			// Framerate invariance over 250 ms (6 frames @ 24 Hz == 15 frames @ 60 Hz) + symmetric-tau negative control.
			float atk24 = 1.0f, atk60 = 1.0f, sym24 = 1.0f;
			float rel24 = 0.0f, rel60 = 0.0f;
			for (int i = 0; i < 6; ++i)
			{
				AdProbe::CallStepAsym(atk24, 0.0f, aAtk24, aRel24, false, atk24, Rank1{});
				AdProbe::CallStepAsym(rel24, 1.0f, aAtk24, aRel24, false, rel24, Rank1{});
				sym24 = AdaptiveDownscale::MotionStep(sym24, 0.0f, expRel24, false);
			}
			for (int i = 0; i < 15; ++i)
			{
				AdProbe::CallStepAsym(atk60, 0.0f, aAtk60, aRel60, false, atk60, Rank1{});
				AdProbe::CallStepAsym(rel60, 1.0f, aAtk60, aRel60, false, rel60, Rank1{});
			}
			// Negative control: symmetric 250 ms one-pole still holds exp(-1) = 0.368 of sharp weight after 250 ms of fast motion,
			// whereas 4x fast-attack drops to exp(-4) = 0.0183 (< 2%) at BOTH 24 Hz and 60 Hz!
			Assert::IsTrue(sym24 > 0.35f, L"negative control: symmetric tau leaves >35% sharp weight after 250 ms");
			Assert::AreEqual(std::exp(-4.0f), atk24, 1e-4f, L"24 Hz fast-attack after 250 ms == exp(-4)");
			Assert::AreEqual(atk24, atk60, 1e-4f, L"24 Hz vs 60 Hz fast-attack framerate invariance");
			Assert::AreEqual(1.0f - std::exp(-1.0f), rel24, 1e-4f, L"24 Hz smooth-release after 250 ms == 1 - exp(-1)");
			Assert::AreEqual(rel24, rel60, 1e-4f, L"24 Hz vs 60 Hz smooth-release framerate invariance");
		}
	};

	// =================================================================================================
	// 2. Real WARP GPU Verification Suite (AdaptiveDownscale::Module + AdaptiveDownscaleStage::Stage)
	// =================================================================================================
	TEST_CLASS(AdaptiveMotionUpgradeWarpGpuTests)
	{
		struct WarpRig
		{
			TargetLutGpuFixture fixture;
			pl_gpu gpu = nullptr;
			pl_fmt unorm16 = nullptr, rgba32f = nullptr, r32f = nullptr;
			std::vector<pl_tex> owned;

			~WarpRig()
			{
				for (pl_tex& t : owned)
					if (t)
						pl_tex_destroy(gpu, &t);
			}

			bool Create()
			{
				if (!fixture.Create())
					return false;
				gpu = fixture.Gpu();
				unorm16 = pl_find_fmt(gpu, PL_FMT_UNORM, 4, 16, 16,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				rgba32f = pl_find_fmt(gpu, PL_FMT_FLOAT, 4, 32, 32,
					static_cast<enum pl_fmt_caps>(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR | PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
				r32f = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
				return unorm16 && rgba32f && r32f;
			}

			pl_tex Track(pl_tex t)
			{
				Assert::IsNotNull(t, L"GPU texture creation failed");
				owned.push_back(t);
				return t;
			}

			// High-frequency textured picture so Lanczos/CNN sharp branch and EwaLanczosSharp/alias-safe branch
			// differ strongly across the entire frame (no flat/identical-branch tautologies).
			pl_tex TexturedUnorm16(int w, int h)
			{
				std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
					{
						const double v = 0.50 + 0.22 * std::sin(x * 1.15) * std::cos(y * 0.95) +
							0.14 * std::cos((x - y) * 1.45);
						const uint16_t c = static_cast<uint16_t>(std::lround((std::max)(0.0, (std::min)(1.0, v)) * 65535.0));
						uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
						p[0] = p[1] = p[2] = c;
						p[3] = 65535;
					}
				pl_tex_params tp{};
				tp.w = w; tp.h = h; tp.format = unorm16; tp.sampleable = true; tp.initial_data = px.data();
				return Track(pl_tex_create(gpu, &tp));
			}

			pl_tex FloatPicture(int w, int h, const std::function<void(int, int, float*)>& pixel)
			{
				std::vector<float> px(static_cast<size_t>(w) * h * 4, 1.0f);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x)
						pixel(x, y, &px[(static_cast<size_t>(y) * w + x) * 4]);
				pl_tex_params tp{};
				tp.w = w; tp.h = h; tp.format = rgba32f; tp.sampleable = true; tp.initial_data = px.data();
				return Track(pl_tex_create(gpu, &tp));
			}

			pl_tex Field(int bw, int bh, const std::function<void(int, int, float*)>& cell)
			{
				std::vector<float> v(static_cast<size_t>(bw) * bh * 4, 0.0f);
				for (int y = 0; y < bh; ++y)
					for (int x = 0; x < bw; ++x)
						cell(x, y, &v[(static_cast<size_t>(y) * bw + x) * 4]);
				pl_tex_params tp{};
				tp.w = bw; tp.h = bh; tp.format = rgba32f; tp.sampleable = true; tp.initial_data = v.data();
				return Track(pl_tex_create(gpu, &tp));
			}

			pl_tex Trust(float share)
			{
				pl_tex_params tp{};
				tp.w = 1; tp.h = 1; tp.format = r32f; tp.sampleable = true; tp.initial_data = &share;
				return Track(pl_tex_create(gpu, &tp));
			}

			std::vector<float> ReadRgba32f(pl_tex tex)
			{
				Assert::IsNotNull(tex, L"expected non-null GPU texture");
				std::vector<float> out(static_cast<size_t>(tex->params.w) * tex->params.h * 4, 0.0f);
				pl_tex_transfer_params dl{};
				dl.tex = tex;
				dl.ptr = out.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"RGBA32F GPU readback failed");
				return out;
			}

			std::vector<uint16_t> RenderUnorm16(pl_tex src, int ow, int oh, const std::vector<const pl_hook*>& hooks,
				const pl_filter_config* scaler = &pl_filter_bilinear, bool gammaLight = true)
			{
				pl_tex_params tp{};
				tp.w = ow; tp.h = oh; tp.format = unorm16; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_default_params;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
				params.fbo_float32 = true;
#endif
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.sigmoid_params = nullptr;
				params.disable_linear_scaling = gammaLight;
				params.upscaler = scaler;
				params.downscaler = scaler;
				params.antiringing_strength = 0.5f;
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params), L"pl_render_image failed");
				pl_gpu_finish(gpu);
				std::vector<uint16_t> px(static_cast<size_t>(ow) * oh * 4);
				pl_tex_transfer_params dl{};
				dl.tex = target;
				dl.ptr = px.data();
				Assert::IsTrue(pl_tex_download(gpu, &dl), L"UNORM16 target download failed");
				pl_tex_destroy(gpu, &target);
				return px;
			}

			std::vector<float> RenderFloat32(pl_tex src, int ow, int oh, const std::vector<const pl_hook*>& hooks,
				const pl_filter_config* scaler = &pl_filter_lanczos)
			{
				pl_tex_params tp{};
				tp.w = ow; tp.h = oh; tp.format = rgba32f; tp.renderable = true; tp.host_readable = true;
				pl_tex target = pl_tex_create(gpu, &tp);
				Assert::IsNotNull(target);
				pl_frame image = TargetLutGpuFixture::MakeRgbFrame(src);
				image.repr.levels = PL_COLOR_LEVELS_FULL;
				image.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_frame out = TargetLutGpuFixture::MakeRgbFrame(target);
				out.repr.levels = PL_COLOR_LEVELS_FULL;
				out.color.transfer = PL_COLOR_TRC_BT_1886;
				pl_render_params params = pl_render_default_params;
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
				params.fbo_float32 = true;
#endif
				params.dither_params = nullptr;
				params.error_diffusion = nullptr;
				params.sigmoid_params = nullptr;
				params.disable_linear_scaling = true;
				params.upscaler = scaler;
				params.downscaler = scaler;
				params.antiringing_strength = 0.5f;
				params.hooks = hooks.empty() ? nullptr : hooks.data();
				params.num_hooks = static_cast<int>(hooks.size());
				Assert::IsTrue(pl_render_image(fixture.Renderer(), &image, &out, &params), L"pl_render_image float32 failed");
				pl_gpu_finish(gpu);
				std::vector<float> px = ReadRgba32f(target);
				pl_tex_destroy(gpu, &target);
				return px;
			}
		};

		// Measures effective sharp-branch blend weight w in out = safe + w * (sharp - safe) over an interior ROI.
		static double MeasureSharpFraction(const std::vector<uint16_t>& out, const std::vector<uint16_t>& sharp,
			const std::vector<uint16_t>& safe, int ow, int x0, int x1, int y0, int y1, int& activeCount)
		{
			double num = 0.0, den = 0.0;
			activeCount = 0;
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x)
				{
					const size_t idx = (static_cast<size_t>(y) * ow + x) * 4;
					const double sh = static_cast<double>(sharp[idx]);
					const double sf = static_cast<double>(safe[idx]);
					const double span = sh - sf;
					if (std::fabs(span) >= 256.0)
					{
						++activeCount;
						num += (static_cast<double>(out[idx]) - sf) * span;
						den += span * span;
					}
				}
			return den > 0.0 ? num / den : -1.0;
		}

	public:
		// Upgrade 1 (WARP GPU): Continuous confidence f.z = {0.25, 0.45, 0.55, 0.80} on GPU MOTION_BODY
		// (both motionMap.r target and motionMap.g continuous confidence c) AND rendered pixel blend weights,
		// proving zero cliff at 0.5 and exact GPU/CPU parity, plus !frameTrusted -> c = 0.0 negative check.
		BEGIN_TEST_METHOD_ATTRIBUTE(ContinuousConfidenceGpuParityAcrossHalfThresholdWithoutCliff)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ContinuousConfidenceGpuParityAcrossHalfThresholdWithoutCliff)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			const int sw = 128, sh = 64, bw = sw / 16, bh = sh / 16, ow = 192, oh = 96;
			pl_tex src = rig.TexturedUnorm16(sw, sh);
			pl_tex trustOk = rig.Trust(1.0f);
			pl_tex trustFail = rig.Trust(0.10f);   // < minTrustedShare (0.25) -> !frameTrusted -> c = 0
			pl_tex cleanGrain = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; });

			const float confs[4] = { 0.25f, 0.45f, 0.55f, 0.80f };
			// 4 two-block-wide bands (columns 0..1, 2..3, 4..5, 6..7), vx = vy = 0 everywhere so shearSpeed == 0 everywhere.
			pl_tex bandedField = rig.Field(bw, bh, [&](int x, int, float* c) {
				c[0] = 0.0f; c[1] = 0.0f; c[2] = confs[(std::min)(3, x / 2)]; c[3] = 0.0f;
			});

			AdaptiveDownscale::Settings st;
			st.cnnResidualStabilise = false;
			st.ssimResidualStabilise = false;
			st.grainMap = false;
			st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;   // e = 0 everywhere so blend isolates motion term
			st.motionFloor = 0.20f;
			SetMotionStrength(st, 1.0f);

			// 1. Direct GPU MotionMap() inspection: .r == fl + (1-fl)*c and .g == continuous c (never binary 0/1!).
			{
				AdaptiveDownscale::Module module;
				module.hostReadableMap = true;
				module.SetFrame(st, 1, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true; up.wBase = 1.0f; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
				module.SetUpscale(up);
				AdaptiveDownscale::GrainLevelSource gl;
				gl.level = cleanGrain; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetGrainLevel(gl);
				AdaptiveDownscale::Motion m;
				m.field = bandedField; m.frameTrust = trustOk; m.minTrustedShare = 0.25f;
				m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetMotion(m);

				rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
				Assert::IsTrue(module.Active() && module.Failures() == 0 && module.LastMotionState() == AdaptiveDownscale::MotionState::Used);
				const std::vector<float> map = rig.ReadRgba32f(module.MotionMap());

				// Same-input negative control: legacy GPU MOTION_BODY had `trusted ? 1.0 : fl` (0.20, 0.20, 1.00, 1.00)
				// and `map.g = trusted ? 1.0 : 0.0` (0, 0, 1, 1).
				const float legacyCliffJump = (0.55f > 0.5f ? 1.0f : 0.20f) - (0.45f > 0.5f ? 1.0f : 0.20f);
				Assert::IsTrue(legacyCliffJump > 0.75f, L"negative control: legacy shader jumps 0.80 across 0.45 -> 0.55");

				for (int band = 0; band < 4; ++band)
				{
					const int bx = band * 2;
					const float* cell = &map[(static_cast<size_t>(1) * bw + bx) * 4];
					const float expectedR = RefOption4BTarget(true, 0.0f, 0.0f, confs[band], st);
					const float cpuMirrorR = CallMotionTarget(true, 0.0f, 0.0f, confs[band], st);
					Assert::AreEqual(expectedR, cell[0], 1e-4f,
						(L"GPU motionMap.r must be continuous at f.z=" + std::to_wstring(confs[band]) +
							L", got " + std::to_wstring(cell[0])).c_str());
					Assert::AreEqual(confs[band], cell[1], 1e-4f,
						(L"GPU motionMap.g must store continuous c=" + std::to_wstring(confs[band]) +
							L", got " + std::to_wstring(cell[1])).c_str());
					Assert::IsTrue(HasNewMotionTarget(), L"5-arg MotionTarget required");
					Assert::AreEqual(cpuMirrorR, cell[0], 1e-4f, L"GPU motionMap.r must match CPU MotionTarget");
				}

				// Untrusted frame (frameTrust 0.10 < 0.25) on the exact same bandedField: c = 0.0 and r = floor everywhere.
				AdaptiveDownscale::Settings instant = st;
				instant.motionSmoothMs = 0.0f;
				module.SetFrame(instant, 2, 1.0f / 24.0f);
				m.frameTrust = trustFail;
				module.SetMotion(m);
				rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
				const std::vector<float> untrustedMap = rig.ReadRgba32f(module.MotionMap());
				for (int bx = 0; bx < bw; ++bx)
				{
					const float* cell = &untrustedMap[(static_cast<size_t>(1) * bw + bx) * 4];
					Assert::AreEqual(st.motionFloor, cell[0], 1e-4f, L"!frameTrusted must drop motionMap.r to motionFloor");
					Assert::AreEqual(0.0f, cell[1], 1e-4f, L"!frameTrusted must write c = 0.0 to motionMap.g");
				}
				module.Release();
			}

			// 2. End-to-end rendered pixel blend fraction across {0.25, 0.45, 0.55, 0.80} with motionFloor = 0.0f.
			st.motionFloor = 0.0f;
			auto renderUniformConf = [&](float wBase, float strength, float conf) {
				AdaptiveDownscale::Settings sRun = st;
				SetMotionStrength(sRun, strength);
				AdaptiveDownscale::Module module;
				module.SetFrame(sRun, 1, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true; up.wBase = wBase; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
				module.SetUpscale(up);
				AdaptiveDownscale::GrainLevelSource gl;
				gl.level = cleanGrain; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetGrainLevel(gl);
				if (conf >= 0.0f)
				{
					pl_tex uf = rig.Field(bw, bh, [conf](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = conf; c[3] = 0.0f; });
					AdaptiveDownscale::Motion m;
					m.field = uf; m.frameTrust = trustOk; m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetMotion(m);
				}
				std::vector<uint16_t> px = rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
				module.Release();
				return px;
			};

			const std::vector<uint16_t> sharpRef = renderUniformConf(1.0f, 0.0f, -1.0f);
			const std::vector<uint16_t> safeRef = renderUniformConf(0.0f, 0.0f, -1.0f);
			int active = 0;
			double wGot[4] = {};
			for (int i = 0; i < 4; ++i)
			{
				const std::vector<uint16_t> out = renderUniformConf(1.0f, 1.0f, confs[i]);
				wGot[i] = MeasureSharpFraction(out, sharpRef, safeRef, ow, 8, ow - 8, 8, oh - 8, active);
				Assert::IsTrue(active > 500, L"precondition: sharp and alias-safe branches must differ across >500 pixels");
				Assert::AreEqual(static_cast<double>(confs[i]), wGot[i], 0.01,
					(L"rendered blend fraction at f.z=" + std::to_wstring(confs[i]) + L" got " + std::to_wstring(wGot[i])).c_str());
			}
			Assert::IsTrue(wGot[2] - wGot[1] < 0.13 && wGot[1] - wGot[0] > 0.17,
				L"rendered step across 0.45->0.55 (~0.10) must be smaller than 0.25->0.45 (~0.20): proves zero cliff at 0.5");

			// 3. HIGH-1 (WARP GPU): NaN/+Inf confidence & 0*Inf neighbor velocity must sanitize to floor/0 without latching NaN.
			{
				const float qnan = std::numeric_limits<float>::quiet_NaN();
				const float pinf = std::numeric_limits<float>::infinity();
				pl_tex poisonedField = rig.Field(bw, bh, [qnan, pinf](int x, int y, float* c) {
					if (x == 1 && y == 1) { c[0] = 0.0f; c[1] = 0.0f; c[2] = qnan; c[3] = 0.0f; }       // NaN confidence
					else if (x == 3 && y == 1) { c[0] = 0.0f; c[1] = 0.0f; c[2] = pinf; c[3] = 0.0f; }  // +Inf confidence (must NOT clamp to 1!)
					else if (x == 5 && y == 1) { c[0] = pinf; c[1] = 0.0f; c[2] = 0.0f; c[3] = 0.0f; }  // 0 * Inf neighbor hazard for (4,1) & (6,1)
					else if (x == 7 && y == 1) { c[0] = qnan; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; }  // NaN velocity with c=1
					else { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; }
				});
				pl_tex cleanField = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; });

				AdaptiveDownscale::Settings stNan = st;
				stNan.motionFloor = 0.20f;
				stNan.motionSmoothMs = 250.0f;
				SetMotionStrength(stNan, 1.0f);

				AdaptiveDownscale::Module module;
				module.hostReadableMap = true;
				auto runSeq = [&](uint64_t seq, pl_tex f) {
					module.SetFrame(stNan, seq, 1.0f / 24.0f);
					AdaptiveDownscale::Upscale up;
					up.on = true; up.wBase = 1.0f; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
					module.SetUpscale(up);
					AdaptiveDownscale::GrainLevelSource gl;
					gl.level = cleanGrain; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetGrainLevel(gl);
					AdaptiveDownscale::Motion m;
					m.field = f; m.frameTrust = trustOk;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetMotion(m);
					rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
					return rig.ReadRgba32f(module.MotionMap());
				};

				const std::vector<float> mPoison = runSeq(1, poisonedField);
				for (int i = 0; i < bw * bh; ++i)
				{
					const float* cell = &mPoison[static_cast<size_t>(i) * 4];
					Assert::IsTrue(std::isfinite(cell[0]) && std::isfinite(cell[1]),
						(L"GPU motionMap became non-finite at block " + std::to_wstring(i)).c_str());
				}
				const float* nanCell = &mPoison[(static_cast<size_t>(1) * bw + 1) * 4];
				const float* infConfCell = &mPoison[(static_cast<size_t>(1) * bw + 3) * 4];
				const float* adjInfVelCell = &mPoison[(static_cast<size_t>(1) * bw + 4) * 4];
				const float* nanVelCell = &mPoison[(static_cast<size_t>(1) * bw + 7) * 4];
				Assert::AreEqual(0.20f, nanCell[0], 1e-4f, L"GPU NaN confidence -> motionFloor");
				Assert::AreEqual(0.0f, nanCell[1], 1e-4f, L"GPU NaN confidence -> motionMap.g = 0");
				Assert::AreEqual(0.20f, infConfCell[0], 1e-4f, L"GPU +Inf confidence must sanitize to untrusted floor (NOT 1.0!)");
				Assert::AreEqual(0.0f, infConfCell[1], 1e-4f, L"GPU +Inf confidence -> motionMap.g = 0");
				Assert::AreEqual(1.0f, adjInfVelCell[0], 1e-4f, L"GPU neighbor with f_n.z=0, f_n.xy=+Inf must NOT poison adjacent clean block via 0*Inf=NaN");
				Assert::AreEqual(0.20f, nanVelCell[0], 1e-4f, L"GPU NaN velocity -> fail-safe motionFloor");

				// Frame 2 clean steady: every poisoned block must recover smoothly (zero NaN latching!).
				const std::vector<float> mRecover = runSeq(2, cleanField);
				const float alphaRel = OnePole::AlphaForMs(1000.0f / 24.0f, 250.0f, OnePole::DtRule::AsGiven);
				const float expectedRecover = 0.20f + (1.0f - 0.20f) * alphaRel;
				for (int bx : { 1, 3, 7 })
				{
					const float* cell = &mRecover[(static_cast<size_t>(1) * bw + bx) * 4];
					Assert::IsTrue(std::isfinite(cell[0]), L"frame 2 must remain finite (no NaN latch)");
					Assert::AreEqual(expectedRecover, cell[0], 1e-4f, L"frame 2 recovers smoothly from sanitized floor");
				}
				module.Release();
			}

			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Upgrades 2 & 3 (WARP GPU): motionStrength continuum {0.0, 0.5, 1.0} on BOTH upscale and downscale,
		// zero motion-map GPU allocation/dispatch at 0.0, and missing-vector floor scaling (1 - strength*(1 - floor))
		// with verbatim "upscale motion: no vectors (alias-safe weight)" status readout.
		BEGIN_TEST_METHOD_ATTRIBUTE(MotionStrengthContinuumZeroCostAndMissingVectorFallbackGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(MotionStrengthContinuumZeroCostAndMissingVectorFallbackGpu)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			const int sw = 128, sh = 64, bw = sw / 16, bh = sh / 16, upW = 192, upH = 96, dnW = 84, dnH = 42;
			pl_tex src = rig.TexturedUnorm16(sw, sh);
			pl_tex trust = rig.Trust(1.0f);
			// Whip-pan fast motion (vx = 32 px/frame = 12 heights/s >> 4*speed_hi = 1.6) so mapFactor == motionFloor (0.0f).
			pl_tex whipField = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 32.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; });
			pl_tex cleanGrain = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; });

			AdaptiveDownscale::Settings base;
			base.cnnResidualStabilise = false;
			base.ssimResidualStabilise = false;
			base.grainMap = false;
			base.edgeLo = 1.0e6f; base.edgeHi = 2.0e6f;
			base.grainLo = 1000.0f; base.grainHi = 2000.0f;   // scalar downscale grain = 1.0
			base.motionFloor = 0.0f;

			// 1. Upscale continuum through Stage::Register + Module on WARP GPU (both live whip-pan AND missing vectors!).
			auto runUpscaleStage = [&](double wBase, float strength, bool liveVectors,
				AdaptiveDownscale::MotionState& outState, bool& hasMotionTex, std::vector<std::string>& statusOut)
			{
				AdaptiveDownscale::Module module;
				module.SetCutSource([](uint64_t, uint64_t) {
					AdaptiveDownscale::CutVerdict v;
					v.state = AdaptiveDownscale::CutVerdict::State::Known;
					return v;
				});
				AdaptiveDownscaleStage::Stage stage{ module };
				AdaptiveDownscale::Settings st = base;
				SetMotionStrength(st, strength);
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = []() { return NlsMaskMapping::Mapping{}; };
				host.ssimInChain = []() { return false; };
				host.publishStatus = []() {};
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = const_cast<pl_tex_t*>(cleanGrain);
					}
					else if (id == VpSkeleton::kP3AnalysisField)
					{
						if (liveVectors)
						{
							r.status = VpSkeleton::AcquireStatus::Reused;
							r.gpu.resource = const_cast<pl_tex_t*>(whipField);
						}
						else
							r.cause = VpSkeleton::AcquireCause::OwnerDeclined;
					}
					return r;
				};
				host.motion = [&]() {
					AdaptiveDownscaleStage::MotionSource m;
					m.live = liveVectors;
					m.field = liveVectors ? whipField : nullptr;
					m.frameTrust = liveVectors ? trust : nullptr;
					return m;
				};

				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &st;
				f.adaptiveUpscale = true;
				f.upscaleWBase = wBase;
				f.upscaleSuperresScale = false;
				f.lowBitrateOff = true;
				f.sourceSequence = 1;
				f.nominalSourceRateHz = 24.0;
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(upW), static_cast<float>(upH) };

				std::vector<const pl_hook*> hooks;
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				std::vector<uint16_t> px = rig.RenderUnorm16(src, upW, upH, hooks);
				Assert::IsTrue(module.Active() && module.Failures() == 0);
				outState = module.LastMotionState();
				hasMotionTex = (module.MotionMap() != nullptr);
				statusOut = stage.StatusLines();
				module.Release();
				return px;
			};

			AdaptiveDownscale::MotionState st0 = AdaptiveDownscale::MotionState::Used, stHalf = st0, stFull = st0, stMiss = st0;
			bool tex0 = true, texHalf = false, texFull = false, texMiss = false;
			std::vector<std::string> lines0, linesHalf, linesFull, linesMissHalf, linesMissFull;

			const std::vector<uint16_t> safeUp = runUpscaleStage(0.0, 0.0f, true, st0, tex0, lines0);
			const std::vector<uint16_t> upStr0 = runUpscaleStage(1.0, 0.0f, true, st0, tex0, lines0);
			Assert::IsTrue(st0 == AdaptiveDownscale::MotionState::Off && !tex0,
				L"motionStrength == 0.0f must skip UpdateMotionMap (MotionState::Off, zero motion-map GPU allocation)");

			const std::vector<uint16_t> upStrHalf = runUpscaleStage(1.0, 0.5f, true, stHalf, texHalf, linesHalf);
			const std::vector<uint16_t> upStrFull = runUpscaleStage(1.0, 1.0f, true, stFull, texFull, linesFull);
			int active = 0;
			const double wUp0 = MeasureSharpFraction(upStr0, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active);
			const double wUpHalf = MeasureSharpFraction(upStrHalf, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active);
			const double wUpFull = MeasureSharpFraction(upStrFull, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active);
			Assert::IsTrue(active > 500);
			Assert::AreEqual(1.0, wUp0, 0.005, L"upscale strength 0.0 == standard sharp blend");
			Assert::AreEqual(0.5, wUpHalf, 0.015, L"upscale strength 0.5 on whip-pan == 0.5*(sharp + safe) continuum");
			Assert::AreEqual(0.0, wUpFull, 0.005, L"upscale strength 1.0 on whip-pan == alias-safe floor");

			// Missing vectors on upscale: verbatim status line + AD_MOTION_FLOOR respects motionStrength (0.5 vs 1.0)!
			const std::vector<uint16_t> upMiss0 = runUpscaleStage(1.0, 0.0f, false, stMiss, texMiss, lines0);
			Assert::AreEqual(1.0, MeasureSharpFraction(upMiss0, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active), 0.005,
				L"strength 0.0 with missing vectors must stay at standard blend (1.0), no floor lean");
			Assert::IsFalse(HasSubstring(lines0, "upscale motion: no vectors (alias-safe weight)"));

			const std::vector<uint16_t> upMissHalf = runUpscaleStage(1.0, 0.5f, false, stMiss, texMiss, linesMissHalf);
			const std::vector<uint16_t> upMissFull = runUpscaleStage(1.0, 1.0f, false, stMiss, texMiss, linesMissFull);
			Assert::IsTrue(HasSubstring(linesMissHalf, "upscale motion: no vectors (alias-safe weight)"),
				(L"missing verbatim fallback at strength 0.5: " + Wide(JoinLines(linesMissHalf))).c_str());
			Assert::IsTrue(HasSubstring(linesMissFull, "upscale motion: no vectors (alias-safe weight)"),
				(L"missing verbatim fallback at strength 1.0: " + Wide(JoinLines(linesMissFull))).c_str());
			Assert::AreEqual(0.5, MeasureSharpFraction(upMissHalf, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active), 0.015,
				L"missing vectors at motionStrength 0.5 must blend at 1 - 0.5*(1 - 0) = 0.5 (not boolean 0.0!)");
			Assert::AreEqual(0.0, MeasureSharpFraction(upMissFull, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active), 0.005,
				L"missing vectors at motionStrength 1.0 must reach alias-safe floor 0.0");

			// 2. Downscale continuum {0.0, 0.5, 1.0} on WARP GPU.
			auto runDownscale = [&](float wBase, float strength) {
				AdaptiveDownscale::Settings st = base;
				st.wBase = wBase;
				SetMotionStrength(st, strength);
				AdaptiveDownscale::Module module;
				module.SetFrame(st, 1, 1.0f / 24.0f);
				AdaptiveDownscale::Motion m;
				m.field = whipField; m.frameTrust = trust;
				m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetMotion(m);
				std::vector<uint16_t> px = rig.RenderUnorm16(src, dnW, dnH, { module.Measure(), module.Blend() });
				Assert::IsTrue(module.Active() && module.Failures() == 0);
				if (strength == 0.0f)
					Assert::IsTrue(module.LastMotionState() == AdaptiveDownscale::MotionState::Off && module.MotionMap() == nullptr);
				module.Release();
				return px;
			};
			const std::vector<uint16_t> dnSafe = runDownscale(0.0f, 0.0f);
			const std::vector<uint16_t> dnStr0 = runDownscale(1.0f, 0.0f);
			const std::vector<uint16_t> dnStrHalf = runDownscale(1.0f, 0.5f);
			const std::vector<uint16_t> dnStrFull = runDownscale(1.0f, 1.0f);
			Assert::AreEqual(0.5, MeasureSharpFraction(dnStrHalf, dnStr0, dnSafe, dnW, 6, dnW - 6, 6, dnH - 6, active), 0.015,
				L"downscale motionStrength 0.5 must sit halfway between standard blend and faithful floor");
			Assert::AreEqual(0.0, MeasureSharpFraction(dnStrFull, dnStr0, dnSafe, dnW, 6, dnW - 6, 6, dnH - 6, active), 0.005,
				L"downscale motionStrength 1.0 on whip-pan must match faithful branch");

			// 3. LOW-2: Live 0.5 -> 0.0 -> 0.5 motionStrength transition on a SINGLE AdaptiveDownscale::Module instance.
			{
				AdaptiveDownscale::Module liveMod;
				auto stepSingle = [&](uint64_t seq, float strength) {
					AdaptiveDownscale::Settings st = base;
					SetMotionStrength(st, strength);
					liveMod.SetFrame(st, seq, 1.0f / 24.0f);
					AdaptiveDownscale::Upscale up;
					up.on = true; up.wBase = 1.0f; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
					liveMod.SetUpscale(up);
					AdaptiveDownscale::GrainLevelSource gl;
					gl.level = cleanGrain; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					liveMod.SetGrainLevel(gl);
					AdaptiveDownscale::Motion m;
					m.field = whipField; m.frameTrust = trust;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					liveMod.SetMotion(m);
					return rig.RenderUnorm16(src, upW, upH, { liveMod.Measure(), liveMod.Blend() });
				};
				const std::vector<uint16_t> t1 = stepSingle(1, 0.5f);
				Assert::IsTrue(liveMod.LastMotionState() == AdaptiveDownscale::MotionState::Used && liveMod.MotionMap() != nullptr);
				Assert::AreEqual(0.5, MeasureSharpFraction(t1, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active), 0.015,
					L"live transition frame 1 (0.5): blends at 0.5");

				const std::vector<uint16_t> t2 = stepSingle(2, 0.0f);
				Assert::IsTrue(liveMod.LastMotionState() == AdaptiveDownscale::MotionState::Off && liveMod.MotionMap() == nullptr,
					L"live transition frame 2 (0.0): must drop to MotionState::Off and clear MotionMap()");
				Assert::AreEqual(1.0, MeasureSharpFraction(t2, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active), 0.005,
					L"live transition frame 2 (0.0): returns immediately to standard sharp blend");

				const std::vector<uint16_t> t3 = stepSingle(3, 0.5f);
				Assert::IsTrue(liveMod.LastMotionState() == AdaptiveDownscale::MotionState::Used && liveMod.MotionMap() != nullptr,
					L"live transition frame 3 (0.5): cleanly resumes motion map");
				Assert::AreEqual(0.5, MeasureSharpFraction(t3, upStr0, safeUp, upW, 8, upW - 8, 8, upH - 8, active), 0.015,
					L"live transition frame 3 (0.5): blends cleanly back at 0.5");
				liveMod.Release();
			}

			Assert::IsTrue(SettingsHasMotionStrength(), L"Settings::motionStrength required");
			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Upgrade 4 (WARP GPU): Option 4B 4-neighbor shear + crisp rigid cinema pan protection on GPU:
		//   - 0.5 heights/s uniform cinema pan stays 100% sharp (w = 1.0), whereas legacy shear-unaware shader blurs it to 0!
		//   - Foreground/background moving edge at the exact same 0.5 heights/s softens BOTH boundary columns (bx=3 and bx=4)
		//     while keeping interior static background (bx<=1) and interior rigid object (bx>=6) sharp!
		//   - Whip pan >= 1.6 heights/s softens the entire frame to floor!
		BEGIN_TEST_METHOD_ATTRIBUTE(ShearBoundarySoftensBothSidesWhileRigidCinemaPanStaysSharpGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ShearBoundarySoftensBothSidesWhileRigidCinemaPanStaysSharpGpu)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			// 128x96 source -> 8x6 blocks; at 24 Hz, crop_h * dt = 96 / 24 = 4.0 px per (height/s).
			// So 0.5 heights/s = 2.0 px/frame, and 1.6 heights/s (4 * speed_hi) = 6.4 px/frame.
			const int sw = 128, sh = 96, bw = sw / 16, bh = sh / 16, ow = 192, oh = 144;
			pl_tex src = rig.TexturedUnorm16(sw, sh);
			pl_tex trust = rig.Trust(1.0f);
			pl_tex cleanGrain = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; });

			pl_tex rigidCinemaPan = rig.Field(bw, bh, [](int, int, float* c) {
				c[0] = 2.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;   // 0.5 heights/s uniform rigid pan
			});
			pl_tex objectShear = rig.Field(bw, bh, [bw](int x, int, float* c) {
				c[0] = (x >= bw / 2) ? 2.0f : 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;   // boundary between bx=3 and bx=4
			});
			pl_tex whipPan = rig.Field(bw, bh, [](int, int, float* c) {
				c[0] = 6.8f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;   // 1.7 heights/s > 4*speed_hi (1.6)
			});
			pl_tex rogueUntrusted = rig.Field(bw, bh, [](int x, int y, float* c) {
				const bool rogue = (x == 4 && y == 3);
				c[0] = rogue ? 40.0f : 0.0f; c[1] = 0.0f; c[2] = rogue ? 0.0f : 1.0f; c[3] = 0.0f;
			});

			AdaptiveDownscale::Settings st;
			st.cnnResidualStabilise = false;
			st.ssimResidualStabilise = false;
			st.grainMap = false;
			st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;
			st.motionSpeedLo = 0.10f;
			st.motionSpeedHi = 0.40f;
			st.motionFloor = 0.0f;
			SetMotionStrength(st, 1.0f);

			auto runField = [&](float wBase, float strength, pl_tex field, std::vector<float>* mapOut = nullptr) {
				AdaptiveDownscale::Settings sRun = st;
				SetMotionStrength(sRun, strength);
				AdaptiveDownscale::Module module;
				module.hostReadableMap = (mapOut != nullptr);
				module.SetFrame(sRun, 1, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true; up.wBase = wBase; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
				module.SetUpscale(up);
				AdaptiveDownscale::GrainLevelSource gl;
				gl.level = cleanGrain; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetGrainLevel(gl);
				if (field)
				{
					AdaptiveDownscale::Motion m;
					m.field = field; m.frameTrust = trust;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetMotion(m);
				}
				std::vector<uint16_t> px = rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
				if (mapOut)
					*mapOut = rig.ReadRgba32f(module.MotionMap());
				module.Release();
				return px;
			};

			const std::vector<uint16_t> sharpRef = runField(1.0f, 0.0f, nullptr);
			const std::vector<uint16_t> safeRef = runField(0.0f, 0.0f, nullptr);

			// Negative control on same 0.5 heights/s speed: without Option 4B shear gating, ramp = smoothstep(0.1, 0.4, 0.5) = 1.0 (w = 0.0).
			Assert::AreEqual(1.0f, RefSmoothstep(st.motionSpeedLo, st.motionSpeedHi, 0.5f), 1e-6f,
				L"negative control: shear-unaware ramp destroys 0.5 heights/s cinema pan");

			// 1. Rigid cinema pan (0.5 heights/s, shear = 0): EVERY block on GPU MotionMap() == 1.0, rendered picture == sharpRef!
			std::vector<float> mapRigid, mapShear, mapWhip, mapRogue;
			const std::vector<uint16_t> outRigid = runField(1.0f, 1.0f, rigidCinemaPan, &mapRigid);
			for (int i = 0; i < bw * bh; ++i)
				Assert::AreEqual(1.0f, mapRigid[static_cast<size_t>(i) * 4], 1e-4f,
					L"GPU Option 4B: 0.5 heights/s rigid cinema pan must keep motionMap.r == 1.0 everywhere");
			int active = 0;
			Assert::AreEqual(1.0, MeasureSharpFraction(outRigid, sharpRef, safeRef, ow, 8, ow - 8, 8, oh - 8, active), 0.005,
				L"rendered rigid cinema pan must stay 100% sharp");

			// 2. Foreground/background shear at the exact same 0.5 heights/s speed:
			//    bx=3 (background side) and bx=4 (moving object side) BOTH drop to 0.0, while bx<=2 and bx>=5 stay 1.0!
			const std::vector<uint16_t> outShear = runField(1.0f, 1.0f, objectShear, &mapShear);
			for (int y = 0; y < bh; ++y)
				for (int x = 0; x < bw; ++x)
				{
					const float expected = (x == 3 || x == 4) ? 0.0f : 1.0f;
					Assert::AreEqual(expected, mapShear[(static_cast<size_t>(y) * bw + x) * 4], 1e-4f,
						(L"GPU shear map at bx=" + std::to_wstring(x)).c_str());
				}
			// Output x coordinates (1.5x upscale of 128 -> 192, block width = 24 output px):
			//   - Interior background (bx 0..1 -> x_out 6..36): w == 1.0
			//   - Background side of boundary (center of bx 3 -> x_out 80..92): w == 0.0
			//   - Foreground side of boundary (center of bx 4 -> x_out 100..112): w == 0.0
			//   - Interior moving object (bx 6..7 -> x_out 156..186): w == 1.0
			Assert::AreEqual(1.0, MeasureSharpFraction(outShear, sharpRef, safeRef, ow, 6, 36, 12, oh - 12, active), 0.01,
				L"interior static background stays sharp");
			Assert::IsTrue(MeasureSharpFraction(outShear, sharpRef, safeRef, ow, 80, 92, 12, oh - 12, active) < 0.08,
				L"background side of moving edge (bx=3) softens to alias-safe");
			Assert::IsTrue(MeasureSharpFraction(outShear, sharpRef, safeRef, ow, 100, 112, 12, oh - 12, active) < 0.08,
				L"foreground side of moving edge (bx=4) softens to alias-safe");
			Assert::AreEqual(1.0, MeasureSharpFraction(outShear, sharpRef, safeRef, ow, 156, 186, 12, oh - 12, active), 0.01,
				L"interior of 0.5 heights/s rigid moving object stays sharp");

			// 3. Untrusted neighbor (f_n.z = 0) with huge garbage vector does NOT poison neighbors on GPU.
			runField(1.0f, 1.0f, rogueUntrusted, &mapRogue);
			Assert::AreEqual(1.0f, mapRogue[(static_cast<size_t>(3) * bw + 3) * 4], 1e-4f,
				L"GPU MOTION_BODY must weight neighbor shear by clamp(f_n.z, 0, 1)");
			Assert::AreEqual(0.0f, mapRogue[(static_cast<size_t>(3) * bw + 4) * 4], 1e-4f,
				L"the untrusted block itself (f.z=0) drops to floor");

			// 4. Extreme whip pan (1.7 heights/s >= 4 * speed_hi = 1.6): drops entire frame to floor!
			const std::vector<uint16_t> outWhip = runField(1.0f, 1.0f, whipPan, &mapWhip);
			for (int i = 0; i < bw * bh; ++i)
				Assert::AreEqual(0.0f, mapWhip[static_cast<size_t>(i) * 4], 1e-4f, L"whip pan >= 4*speed_hi drops to floor");
			Assert::AreEqual(0.0, MeasureSharpFraction(outWhip, sharpRef, safeRef, ow, 8, ow - 8, 8, oh - 8, active), 0.005,
				L"rendered whip pan equals alias-safe branch");

			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Upgrade 5 (WARP GPU): 2x2 bilinear advection on half-cell motion (8 px/frame on 16-px grid, both 1D and 2D)
		// + asymmetric fast-attack (tau / 4) and smooth-release (tau) timing at BOTH 24 Hz and 60 Hz on GPU!
		BEGIN_TEST_METHOD_ATTRIBUTE(HalfCellBilinearAdvectionAndAsymmetricAttackRelease24And60HzGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(HalfCellBilinearAdvectionAndAsymmetricAttackRelease24And60HzGpu)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			// 128x256 source (8x16 blocks): at 24 Hz, 8 px/frame = 8*24/256 = 0.75 heights/s < 2*speed_hi (0.80),
			// and diagonal (8, 8) px/frame with speed_hi = 1.0 has speed = 1.06 < 2*1.0 = 2.0, so target == 1.0 everywhere!
			const int sw = 128, sh = 256, bw = sw / 16, bh = sh / 16, ow = 64, oh = 128;
			pl_tex src = rig.TexturedUnorm16(sw, sh);
			pl_tex trust = rig.Trust(1.0f);

			AdaptiveDownscale::Settings st;
			st.ssimResidualStabilise = false;
			st.grainMap = false;
			st.motionFloor = 0.0f;
			st.motionSpeedLo = 0.20f;
			st.motionSpeedHi = 1.00f;
			st.motionSmoothMs = 250.0f;
			SetMotionStrength(st, 1.0f);

			// 1. Half-cell (8 px) 2D corner advection on WARP GPU vs nearest-neighbor negative control:
			//    Frame 1: vx=vy=0 (zero shear), corner bx>=2 && by>=2 has f.z=1.0 (map1=1.0), else f.z=0.0 (map1=0.0).
			//    Frame 2: uniform (vx=+8.0, vy=+8.0, f.z=1.0) -> target=1.0 everywhere:
			//      - At (bx=3, by=2): pc=(48, 32) -> ppos=(2.5, 1.5) -> 1D-vertical boundary between row 1 (0.0) and row 2 (1.0) -> prev = 0.50!
			//      - At (bx=2, by=2): pc=(32, 32) -> ppos=(1.5, 1.5) -> 2x2 corner (0, 0, 0, 1) -> prev = 0.25!
			//      - Nearest-neighbor floor(pc / 16) negative control at BOTH (3, 2) and (2, 2) gives floor(2.0)=2 -> prev = 1.00!
			{
				pl_tex frame1Field = rig.Field(bw, bh, [](int x, int y, float* c) {
					c[0] = 0.0f; c[1] = 0.0f; c[2] = (x >= 2 && y >= 2) ? 1.0f : 0.0f; c[3] = 0.0f;
				});
				pl_tex halfShift2D = rig.Field(bw, bh, [](int, int, float* c) {
					c[0] = 8.0f; c[1] = 8.0f; c[2] = 1.0f; c[3] = 0.0f;
				});

				AdaptiveDownscale::Module module;
				module.hostReadableMap = true;
				auto stepGpu = [&](pl_tex f, uint64_t seq) {
					module.SetFrame(st, seq, 1.0f / 24.0f);
					AdaptiveDownscale::Motion m;
					m.field = f; m.frameTrust = trust;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetMotion(m);
					rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
					return rig.ReadRgba32f(module.MotionMap());
				};

				const std::vector<float> m1 = stepGpu(frame1Field, 1);
				Assert::AreEqual(0.0f, m1[(static_cast<size_t>(1) * bw + 1) * 4], 1e-4f);
				Assert::AreEqual(1.0f, m1[(static_cast<size_t>(2) * bw + 2) * 4], 1e-4f);

				const std::vector<float> m2 = stepGpu(halfShift2D, 2);
				const float alphaRel24 = OnePole::AlphaForMs(1000.0f / 24.0f, 250.0f, OnePole::DtRule::AsGiven);
				const float expectedEdge1D = 0.50f + (1.0f - 0.50f) * alphaRel24;
				const float expectedCorner2D = 0.25f + (1.0f - 0.25f) * alphaRel24;
				const float naiveNearestOut = 1.0f;   // floor(32/16) = 2 -> prev = 1.0 -> 1.0

				const float gotEdge1D = m2[(static_cast<size_t>(2) * bw + 3) * 4];
				const float gotCorner2D = m2[(static_cast<size_t>(2) * bw + 2) * 4];
				Assert::IsTrue(std::fabs(naiveNearestOut - expectedCorner2D) > 0.60f,
					L"negative control: nearest-neighbor floor(pc/16) misses 2x2 half-cell advection by >0.60");
				Assert::AreEqual(expectedEdge1D, gotEdge1D, 1e-4f,
					(L"GPU half-cell 8px 1D edge advection: expected " + std::to_wstring(expectedEdge1D) +
						L", got " + std::to_wstring(gotEdge1D)).c_str());
				Assert::AreEqual(expectedCorner2D, gotCorner2D, 1e-4f,
					(L"GPU half-cell 8px 2x2 corner advection: expected " + std::to_wstring(expectedCorner2D) +
						L", got " + std::to_wstring(gotCorner2D)).c_str());
				module.Release();
			}

			// 2. Asymmetric fast-attack (tau / 4 = 62.5 ms) vs smooth-release (tau = 250 ms) on WARP GPU at BOTH 24 Hz and 60 Hz!
			pl_tex steadyOne = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f; });
			pl_tex dropZero = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; c[1] = 0.0f; c[2] = 0.0f; c[3] = 0.0f; });

			auto runTrajectory = [&](float hz, bool attackDir, int steps, float& firstStepOut) {
				AdaptiveDownscale::Module module;
				module.hostReadableMap = true;
				uint64_t seq = 1;
				auto push = [&](pl_tex f) {
					module.SetFrame(st, seq++, 1.0f / hz);
					AdaptiveDownscale::Motion m;
					m.field = f; m.frameTrust = trust;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetMotion(m);
					rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
					return rig.ReadRgba32f(module.MotionMap())[(static_cast<size_t>(2) * bw + 2) * 4];
				};
				push(attackDir ? steadyOne : dropZero);   // Frame 1: initialize to 1.0 (for attack) or 0.0 (for release)
				float last = 0.0f;
				for (int i = 0; i < steps; ++i)
				{
					last = push(attackDir ? dropZero : steadyOne);
					if (i == 0)
						firstStepOut = last;
				}
				module.Release();
				return last;
			};

			float atk1_24 = 0.0f, atk1_60 = 0.0f, rel1_24 = 0.0f, rel1_60 = 0.0f;
			const float atk250ms_24 = runTrajectory(24.0f, true, 6, atk1_24);
			const float atk250ms_60 = runTrajectory(60.0f, true, 15, atk1_60);
			const float rel250ms_24 = runTrajectory(24.0f, false, 6, rel1_24);
			const float rel250ms_60 = runTrajectory(60.0f, false, 15, rel1_60);

			const float expAtk1_24 = 1.0f - OnePole::AlphaForMs(1000.0f / 24.0f, 62.5f, OnePole::DtRule::AsGiven);
			const float expRel1_24 = OnePole::AlphaForMs(1000.0f / 24.0f, 250.0f, OnePole::DtRule::AsGiven);
			const float naiveSymAtk1_24 = 1.0f - expRel1_24;   // ~0.8465 instead of ~0.5134

			Assert::IsTrue(naiveSymAtk1_24 - expAtk1_24 > 0.30f,
				L"negative control: symmetric 250ms tau lags fast-attack by >0.33 on frame 1 of motion onset");
			Assert::AreEqual(expAtk1_24, atk1_24, 1e-4f, L"GPU 24 Hz frame-1 fast-attack must use tau = smoothMs / 4");
			Assert::AreEqual(expRel1_24, rel1_24, 1e-4f, L"GPU 24 Hz frame-1 smooth-release must use tau = smoothMs");
			Assert::AreEqual(std::exp(-4.0f), atk250ms_24, 1e-3f, L"GPU 24 Hz attack after 250 ms == exp(-4)");
			Assert::AreEqual(atk250ms_24, atk250ms_60, 1e-3f, L"GPU 24 Hz vs 60 Hz fast-attack framerate invariance");
			Assert::AreEqual(1.0f - std::exp(-1.0f), rel250ms_24, 1e-3f, L"GPU 24 Hz release after 250 ms == 1 - exp(-1)");
			Assert::AreEqual(rel250ms_24, rel250ms_60, 1e-3f, L"GPU 24 Hz vs 60 Hz smooth-release framerate invariance");

			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Upgrade 6 (WARP GPU): Full NLS coordinate mapping on AdaptiveDownscale::Motion & GrainLevelSource:
		//   - On a 512x64 source (32x4 blocks) upscaled 1.5x to 768x96 under horizontal NLS (xScale = 1.5, curve = 2.0):
		//     * u = 0.75 (x_out = 576) maps via nls_coordinate to u_nls = 0.8125 (blocks 25..26, 32 px = 2 full blocks right of linear blocks 23..24!).
		//     * u = 0.25 (x_out = 192) maps via nls_coordinate to u_nls = 0.1875 (blocks 5..6, 32 px = 2 full blocks left of linear blocks 7..8!).
		//   - Proves Stage::Register keeps vectors ACTIVE under valid NLS (no discontinuity drop; center u=0.5 stays sharp!),
		//     maps BOTH AD_MOTION (at u=0.75) and AD_GRAIN_PUBLISHED (at u=0.25) to the stretched NLS source coordinates
		//     (same-input linear-UV negative control stays sharp at both positions!), AND when !nls.valid fails visibly
		//     to discontinuity/fallback ("upscale motion: no vectors (alias-safe weight)").
		BEGIN_TEST_METHOD_ATTRIBUTE(ActiveNlsMapsMotionAndPublishedGrainWhileInvalidNlsFallsBackGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(ActiveNlsMapsMotionAndPublishedGrainWhileInvalidNlsFallsBackGpu)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			const int sw = 512, sh = 64, bw = sw / 16, bh = sh / 16, ow = 768, oh = 96;
			pl_tex src = rig.TexturedUnorm16(sw, sh);
			pl_tex trust = rig.Trust(1.0f);

			const std::map<std::string, std::string> nlsParams = {
				{ "curve", "2.0" }, { "geometry", "0" }, { "center_protection", "0.0" }, { "strength", "1.0" }
			};
			const NlsMaskMapping::Mapping validNls = NlsMaskMapping::FromParameters(nlsParams, 1.5, false, false);
			Assert::IsTrue(validNls.valid && std::fabs(validNls.xScale - 1.5) < 1e-6);
			// Verify exact 2-block separation between linear UV and NLS UV at u = 0.75 and u = 0.25.
			Assert::AreEqual(0.8125, NlsMaskMapping::MapCoordinate(0.75, validNls.xScale, validNls.curve, validNls.geometry, validNls.xProtect), 1e-6);
			Assert::AreEqual(0.1875, NlsMaskMapping::MapCoordinate(0.25, validNls.xScale, validNls.curve, validNls.geometry, validNls.xProtect), 1e-6);

			// Motion map: untrusted/floor (0.0) ONLY at blocks bx = 25..26 (u_src ~ 0.8125); 1.0 everywhere else (including linear bx = 23..24!).
			// vx = vy = 0 everywhere so 4-neighbor shear is 0.0 everywhere and the spatial mask is exact!
			pl_tex offCenterMotion = rig.Field(bw, bh, [](int x, int, float* c) {
				c[0] = 0.0f; c[1] = 0.0f; c[2] = (x == 25 || x == 26) ? 0.0f : 1.0f; c[3] = 0.0f;
			});
			// Published kP10gGrainLevel: heavy grain (N_b = 10000 -> g = grainFloor = 0.0) ONLY at blocks bx = 5..6 (u_src ~ 0.1875);
			// clean (N_b = 0 -> g = 1.0) everywhere else (including linear bx = 7..8!).
			pl_tex offCenterGrain = rig.Field(bw, bh, [](int x, int, float* c) {
				c[0] = (x == 5 || x == 6) ? 10000.0f : 0.0f; c[1] = c[2] = c[3] = 0.0f;
			});

			AdaptiveDownscale::Settings st;
			st.cnnResidualStabilise = false;
			st.ssimResidualStabilise = false;
			st.grainMap = false;
			st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;
			st.grainFloor = 0.0f;
			st.grainLo = 2.0f; st.grainHi = 8.0f;
			st.motionFloor = 0.0f;
			SetMotionStrength(st, 1.0f);

			struct NlsOutcome
			{
				std::vector<uint16_t> px;
				AdaptiveDownscale::MotionState motionState = AdaptiveDownscale::MotionState::Off;
				bool floorUsed = false;
				std::string stageCause;
				std::vector<std::string> statusLines;
				FallbackCatalog::Decision fallback;
			};

			auto runStage = [&](double wBase, bool nlsSubmitted, const NlsMaskMapping::Mapping& nlsMap) -> NlsOutcome {
				AdaptiveDownscale::Module module;
				module.SetCutSource([](uint64_t, uint64_t) {
					AdaptiveDownscale::CutVerdict v;
					v.state = AdaptiveDownscale::CutVerdict::State::Known;
					return v;
				});
				AdaptiveDownscaleStage::Stage stage{ module };
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = [&]() { return nlsMap; };
				host.ssimInChain = []() { return false; };
				host.publishStatus = []() {};
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = const_cast<pl_tex_t*>(offCenterGrain);
					}
					else if (id == VpSkeleton::kP3AnalysisField)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = const_cast<pl_tex_t*>(offCenterMotion);
					}
					return r;
				};
				host.motion = [&]() {
					AdaptiveDownscaleStage::MotionSource m;
					m.live = true; m.field = offCenterMotion; m.frameTrust = trust;
					return m;
				};

				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &st;
				f.adaptiveUpscale = true;
				f.upscaleWBase = wBase;
				f.upscaleSuperresScale = false;
				f.lowBitrateOff = true;
				f.sourceSequence = 1;
				f.nominalSourceRateHz = 24.0;
				f.nlsSubmitted = nlsSubmitted;
				f.nlsFile = "NLS.glsl";
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };

				std::vector<const pl_hook*> hooks;
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				NlsOutcome out;
				out.px = rig.RenderUnorm16(src, ow, oh, hooks);
				Assert::IsTrue(module.Active() && module.Failures() == 0);
				out.motionState = module.LastMotionState();
				out.floorUsed = module.MotionFloorUsed();
				out.stageCause = stage.UpscaleMotionCause();
				out.statusLines = stage.StatusLines();
				out.fallback = stage.Fallback();
				module.Release();
				return out;
			};

			// Reference sharp (w=1 everywhere) and safe (w=0 everywhere) pictures.
			const std::vector<uint16_t> safeRef = runStage(0.0, false, NlsMaskMapping::Mapping{}).px;
			AdaptiveDownscale::Settings cleanEverywhere = st;
			SetMotionStrength(cleanEverywhere, 0.0f);
			std::vector<uint16_t> sharpRef;
			{
				pl_tex zeroGrain = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; });
				AdaptiveDownscale::Module module;
				module.SetFrame(cleanEverywhere, 1, 1.0f / 24.0f);
				AdaptiveDownscale::Upscale up;
				up.on = true; up.wBase = 1.0f; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
				module.SetUpscale(up);
				AdaptiveDownscale::GrainLevelSource gl;
				gl.level = zeroGrain; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				module.SetGrainLevel(gl);
				sharpRef = rig.RenderUnorm16(src, ow, oh, { module.Measure(), module.Blend() });
				module.Release();
			}

			// Same-input linear-UV negative control (nlsSubmitted = false on the exact same textures):
			// At x_out = 576 (u = 0.75, linear blocks 23..24) AND x_out = 192 (u = 0.25, linear blocks 7..8), linear UV stays 1.0 sharp!
			const NlsOutcome linearControl = runStage(1.0, false, NlsMaskMapping::Mapping{});
			int active = 0;
			Assert::AreEqual(1.0, MeasureSharpFraction(linearControl.px, sharpRef, safeRef, ow, 568, 584, 12, oh - 12, active), 0.02,
				L"negative control (unmapped linear UV): x_out=576 (u=0.75) reads blocks 23..24 (sharp 1.0)");
			Assert::AreEqual(1.0, MeasureSharpFraction(linearControl.px, sharpRef, safeRef, ow, 184, 200, 12, oh - 12, active), 0.02,
				L"negative control (unmapped linear UV): x_out=192 (u=0.25) reads blocks 7..8 (sharp 1.0)");

			// 1. Active VALID NLS: vectors stay LIVE (no fallback!), center x_out=384 stays sharp (1.0),
			//    x_out=576 maps AD_MOTION to blocks 25..26 (softens to 0.0!), and x_out=192 maps AD_GRAIN_PUBLISHED to blocks 5..6 (softens to 0.0!)!
			const NlsOutcome withValidNls = runStage(1.0, true, validNls);
			Assert::IsTrue(withValidNls.stageCause.empty(),
				(L"active valid NLS must NOT be treated as a motion discontinuity, got cause: " + Wide(withValidNls.stageCause)).c_str());
			Assert::IsTrue(!withValidNls.floorUsed && withValidNls.motionState == AdaptiveDownscale::MotionState::Used,
				L"active valid NLS must build and use the motion map (not drop to motionFloor)");
			Assert::IsFalse(HasSubstring(withValidNls.statusLines, "upscale motion: no vectors (alias-safe weight)"),
				L"active valid NLS must not report missing vectors");

			Assert::AreEqual(1.0, MeasureSharpFraction(withValidNls.px, sharpRef, safeRef, ow, 372, 396, 12, oh - 12, active), 0.02,
				L"center (u=0.5, x_out=384) must stay 1.0 sharp (proves frame did NOT drop globally to floor)");
			Assert::AreEqual(0.0, MeasureSharpFraction(withValidNls.px, sharpRef, safeRef, ow, 568, 584, 12, oh - 12, active), 0.03,
				L"AD_MOTION under valid NLS must map u=0.75 (x_out=576) to blocks 25..26 (alias-safe 0.0)");
			Assert::AreEqual(0.0, MeasureSharpFraction(withValidNls.px, sharpRef, safeRef, ow, 184, 200, 12, oh - 12, active), 0.03,
				L"AD_GRAIN_PUBLISHED under valid NLS must map u=0.25 (x_out=192) to blocks 5..6 (alias-safe 0.0)");

			// 2. Invalid NLS (!nls.valid) negative control: fails visibly to discontinuity / alias-safe floor!
			NlsMaskMapping::Mapping invalidNls{};
			invalidNls.valid = false;
			const NlsOutcome withInvalidNls = runStage(1.0, true, invalidNls);
			Assert::IsTrue(!withInvalidNls.stageCause.empty() && withInvalidNls.floorUsed,
				L"!nls.valid with nlsSubmitted must trigger motion discontinuity and floor fallback");
			Assert::IsTrue(HasSubstring(withInvalidNls.statusLines, "upscale motion: no vectors (alias-safe weight)"),
				(L"!nls.valid must emit verbatim no-vectors status: " + Wide(JoinLines(withInvalidNls.statusLines))).c_str());
			Assert::IsTrue(withInvalidNls.fallback.engaged && withInvalidNls.fallback.id == "output scaling",
				L"!nls.valid must engage output scaling fallback");
			Assert::AreEqual(0.0, MeasureSharpFraction(withInvalidNls.px, sharpRef, safeRef, ow, 372, 396, 12, oh - 12, active), 0.01,
				L"!nls.valid must drop even center pixels to motionFloor");

			// 2b. HIGH-2 (Rule 4): With motionStrength == 0.0f and !nls.valid, ReadGrain() must NOT silently bind
			//     unmapped linear AD_GRAIN_PUBLISHED — it must visibly fall back to grain_floor ("NLS map unknown")!
			{
				AdaptiveDownscale::Settings stGrainOnly = st;
				SetMotionStrength(stGrainOnly, 0.0f);
				AdaptiveDownscale::Module module;
				module.SetCutSource([](uint64_t, uint64_t) {
					AdaptiveDownscale::CutVerdict v;
					v.state = AdaptiveDownscale::CutVerdict::State::Known;
					return v;
				});
				AdaptiveDownscaleStage::Stage stage{ module };
				double weight = 1.0;
				bool grainWanted = false, noSsim = false;
				int p10gAsks = 0;
				AdaptiveDownscaleStage::Host host;
				host.fallbackFramePeriod = []() { return 1.0f / 24.0f; };
				host.bitrate = [](double& q) { q = 1.0; return false; };
				host.nlsMapping = [&]() { return invalidNls; };
				host.ssimInChain = []() { return false; };
				host.publishStatus = []() {};
				host.acquire = [&](VpSkeleton::ProductId id, uint64_t, const VpSkeleton::VariantKey&) {
					++p10gAsks;
					VpSkeleton::AcquireResult r;
					if (id == VpSkeleton::kP10gGrainLevel)
					{
						r.status = VpSkeleton::AcquireStatus::Reused;
						r.gpu.resource = const_cast<pl_tex_t*>(offCenterGrain);
					}
					return r;
				};
				host.motion = [&]() {
					AdaptiveDownscaleStage::MotionSource m;
					m.live = true; m.field = offCenterMotion; m.frameTrust = trust;
					return m;
				};
				AdaptiveDownscaleStage::Frame f;
				f.adaptive = &stGrainOnly;
				f.adaptiveUpscale = true;
				f.upscaleWBase = 1.0;
				f.upscaleSuperresScale = false;
				f.lowBitrateOff = true;
				f.sourceSequence = 1;
				f.nominalSourceRateHz = 24.0;
				f.nlsSubmitted = true;
				f.nlsFile = "CustomUnknownNLS.glsl";
				f.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				f.targetCrop = { 0.0f, 0.0f, static_cast<float>(ow), static_cast<float>(oh) };

				std::vector<const pl_hook*> hooks;
				stage.Register(f, host, { weight, grainWanted, noSsim }, hooks);
				const std::vector<uint16_t> grainFallbackPx = rig.RenderUnorm16(src, ow, oh, hooks);
				Assert::IsTrue(module.Active() && module.Failures() == 0);
				Assert::IsFalse(stage.GrainTerm().published, L"HIGH-2: !nls.valid must NOT publish unmapped linear kP10gGrainLevel");
				Assert::IsFalse(module.PublishedGrainUsed(), L"HIGH-2: module must use grain_floor instead of unmapped AD_GRAIN_PUBLISHED");
				Assert::IsTrue(stage.GrainTerm().cause.find("NLS") != std::string::npos,
					(L"HIGH-2: GrainTerm().cause must name NLS, got: " + Wide(stage.GrainTerm().cause)).c_str());
				const std::vector<std::string> gLines = stage.StatusLines();
				Assert::IsTrue(HasSubstring(gLines, "Adaptive upscale grain: no grain level") && HasSubstring(gLines, "NLS"),
					(L"HIGH-2: StatusLines must report visible grain fallback for unknown NLS: " + Wide(JoinLines(gLines))).c_str());
				const FallbackCatalog::Decision gFb = stage.Fallback();
				Assert::IsTrue(gFb.engaged && gFb.id == "output scaling" && gFb.reason.find("no grain level") != std::string::npos,
					(L"HIGH-2: Fallback() must engage output scaling grain fallback, got: " + Wide(gFb.reason)).c_str());
				Assert::AreEqual(0.0, MeasureSharpFraction(grainFallbackPx, sharpRef, safeRef, ow, 372, 396, 12, oh - 12, active), 0.01,
					L"HIGH-2: !nls.valid with motionStrength=0 must drop center pixels to grain_floor (0.0), not silently blend unmapped linear grain");
				module.Release();
			}

			// 3. Direct struct member contract check on AdaptiveDownscale::Motion::nls and GrainLevelSource::nls.
			AdaptiveDownscale::Motion mStruct{};
			AdaptiveDownscale::GrainLevelSource gStruct{};
			Assert::IsTrue(SetNlsImpl(mStruct, validNls, Rank1{}), L"AdaptiveDownscale::Motion::nls member must exist");
			Assert::IsTrue(SetNlsImpl(gStruct, validNls, Rank1{}), L"AdaptiveDownscale::GrainLevelSource::nls member must exist");
			Assert::IsTrue(GetNlsImpl(mStruct, Rank1{}).valid && GetNlsImpl(gStruct, Rank1{}).valid);

			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// Deliverable 6 (WARP GPU): BTB/WTW excursion survival, 12-bit precision (<= 1 LSB of 4095),
		// and two-sided grain retention (>= 0.95 flat/dark-flat, >= 0.90 texture) under active motion-adapted
		// blending (motionStrength = 0.5 and 1.0), with same-input clamping, 8-bit quantization, and unguarded
		// temporal-averaging negative controls.
		BEGIN_TEST_METHOD_ATTRIBUTE(BtbWtwTwelveBitPrecisionAndMultiBandGrainRetentionUnderMotionGpu)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(BtbWtwTwelveBitPrecisionAndMultiBandGrainRetentionUnderMotionGpu)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			// 1. BTB (< 0.0) & WTW (> 1.0) + 12-bit ramp precision (0..4095 within 1 LSB) on WARP GPU under motionStrength = 0.5f & 1.0f.
			{
				const int sw = 256, sh = 64, bw = sw / 16, bh = sh / 16, ow = 384, oh = 96;
				// 16 vertical uniform patches (each 16 source px wide -> interior immune to filter ringing):
				// patches 0..3: BTB (-0.045, -0.020) and WTW (1.035, 1.085);
				// patches 4..15: 12-bit codes spanning dark, mid, and highlight steps (1, 17, 65, 257, 513, 1025, 1537, 2049, 2561, 3073, 3585, 4094).
				const float patchValues[16] = {
					-0.045f, -0.020f, 1.035f, 1.085f,
					1.0f / 4095.0f, 17.0f / 4095.0f, 65.0f / 4095.0f, 257.0f / 4095.0f,
					513.0f / 4095.0f, 1025.0f / 4095.0f, 1537.0f / 4095.0f, 2049.0f / 4095.0f,
					2561.0f / 4095.0f, 3073.0f / 4095.0f, 3585.0f / 4095.0f, 4094.0f / 4095.0f
				};
				pl_tex rampSrc = rig.FloatPicture(sw, sh, [&](int x, int, float* p) {
					const float v = patchValues[(std::min)(15, x / 16)];
					p[0] = p[1] = p[2] = v;
					p[3] = 1.0f;
				});
				pl_tex trust = rig.Trust(1.0f);
				// Mixed motion across the patches: steady, partial confidence (0.55), shear boundary, and whip-pan!
				pl_tex mixedMotion = rig.Field(bw, bh, [](int x, int, float* c) {
					c[0] = (x % 4 == 3) ? 24.0f : ((x % 4 == 2) ? 2.0f : 0.0f);
					c[1] = 0.0f;
					c[2] = (x % 3 == 1) ? 0.55f : 1.0f;
					c[3] = 0.0f;
				});
				pl_tex grainLvl = rig.Field(bw, bh, [](int x, int, float* c) {
					c[0] = (x % 2 == 0) ? 0.0f : 25.0f;
				});

				for (float strength : { 0.5f, 1.0f })
				{
					AdaptiveDownscale::Settings st;
					st.float32Picture = true;
					st.cnnResidualStabilise = false;
					st.ssimResidualStabilise = false;
					st.grainMap = false;
					st.motionFloor = 0.20f;
					SetMotionStrength(st, strength);

					AdaptiveDownscale::Module module;
					module.SetFrame(st, 1, 1.0f / 24.0f);
					AdaptiveDownscale::Upscale up;
					up.on = true; up.wBase = 0.85f; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
					module.SetUpscale(up);
					AdaptiveDownscale::GrainLevelSource gl;
					gl.level = grainLvl; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetGrainLevel(gl);
					AdaptiveDownscale::Motion m;
					m.field = mixedMotion; m.frameTrust = trust;
					m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					module.SetMotion(m);

					const std::vector<float> out = rig.RenderFloat32(rampSrc, ow, oh, { module.Measure(), module.Blend() });
					Assert::IsTrue(module.Active() && module.Failures() == 0 && !module.HalfFloatPictureFallback());
					module.Release();

					// Negative controls on the exact same patchValues:
					//   - Naive [0, 1] clamp clips BTB (-0.045 -> 0.0, error 184 12-bit codes) and WTW (1.085 -> 1.0, error 348 codes).
					//   - Naive 8-bit bottleneck quantizes 17/4095 (0.00415) -> round(1.058)/255 = 1/255 = 16.06/4095, and 65/4095 -> 64.25,
					//     with worst 12-bit error >= 7 LSBs across odd 12-bit codes (e.g. 513/4095 -> round(31.95)/255 = 32/255 = 513.88, 257/4095 -> 16/255 = 256.94, 1/4095 -> 0 = 1 LSB, 7/4095 -> 0 = 7 LSBs).
					Assert::IsTrue(std::fabs(RampMath::Clamp01(patchValues[0]) - patchValues[0]) * 4095.0f > 150.0f,
						L"negative control: [0,1] clamp clips BTB");
					Assert::IsTrue(std::fabs(RampMath::Clamp01(patchValues[3]) - patchValues[3]) * 4095.0f > 300.0f,
						L"negative control: [0,1] clamp clips WTW");
					// LOW-3: Align 8-bit quantization negative control directly to patchValues[12] (2561/4095):
					// round(2561/4095 * 255) = 159 -> 159/255 * 4095 = 2553.35 (error > 7 LSBs!).
					Assert::IsTrue(std::fabs(std::round(patchValues[12] * 255.0f) / 255.0f * 4095.0f - patchValues[12] * 4095.0f) > 5.0f,
						L"negative control: 8-bit intermediate loses >5 12-bit LSBs on patchValues[12] (2561/4095)");

					// Verify center pixel of each 24-px-wide output patch (x = patch * 24 + 12, y = oh / 2).
					for (int p = 0; p < 16; ++p)
					{
						const int ox = p * 24 + 12, oy = oh / 2;
						const float* px = &out[(static_cast<size_t>(oy) * ow + ox) * 4];
						for (int c = 0; c < 3; ++c)
						{
							if (p < 4)
							{
								// On Beta libplacebo (PL_RENDER_PARAMS_FBO_FLOAT32), POST_KERNEL intermediates are rgba32f (2e-4f).
								// On Stable libplacebo-360, POST_KERNEL FBOs are rgba16f whose IEEE-754 binary16 1-ULP in [1, 2) is 1/1024.
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
								const float excursionTol = 2.0e-4f;
#else
								const float excursionTol = 1.0f / 1024.0f;
#endif
								Assert::AreEqual(patchValues[p], px[c], excursionTol,
									(L"BTB/WTW excursion clipped at patch " + std::to_wstring(p)).c_str());
							}
							else
							{
								const int expected12 = static_cast<int>(std::lround(patchValues[p] * 4095.0f));
								const int got12 = static_cast<int>(std::lround(px[c] * 4095.0f));
								// On Beta libplacebo (PL_RENDER_PARAMS_FBO_FLOAT32), POST_KERNEL FBOs are rgba32f (<= 1 LSB).
								// On Stable libplacebo-360, POST_KERNEL FBOs are rgba16f whose IEEE-754 binary16 1-ULP in [0.5, 1)
								// is 2^-11 = 1/2048 -> 4095/2048 = 2 12-bit LSBs (still 3x tighter than 8-bit >5 LSB negative control).
#if defined(PL_RENDER_PARAMS_FBO_FLOAT32)
								constexpr int kMax12BitLsb = 1;
#else
								constexpr int kMax12BitLsb = 2;
#endif
								Assert::IsTrue(std::abs(got12 - expected12) <= kMax12BitLsb,
									(L"12-bit precision > " + std::to_wstring(kMax12BitLsb) + L" LSB at code " +
										std::to_wstring(expected12) + L", got " + std::to_wstring(got12)).c_str());
							}
						}
					}
				}
			}

			// 2. MEDIUM-1: Multi-frame grain retention non-regression on WARP GPU across flat (512/1023), dark-flat (80/1023),
			//    and structured texture bands under BOTH motionStrength = 0.5f & 1.0f, with genuinely divergent sharp vs
			//    alias-safe branches (explicitly asserted!) AND an actual in-module residualGrainGuard = false negative control!
			{
				const int sw = 192, sh = 96, bw = sw / 16, bh = sh / 16, ow = 288, oh = 144;
				pl_tex trust = rig.Trust(1.0f);
				// Spatially varying motion across each band: top steady, middle rigid pan, bottom shear!
				pl_tex motionField = rig.Field(bw, bh, [](int, int y, float* c) {
					c[0] = (y >= 4) ? 2.0f : ((y >= 2) ? 1.5f : 0.0f);
					c[1] = 0.0f; c[2] = 0.75f; c[3] = 0.0f;
				});
				// Published kP10gGrainLevel reporting real grain sigma = 8 (10-bit codes -> N_b = 64, 8-bit energy ~7.11)
				// so residualGrainGuard's smoothstep(0.25, 0.75, grainy) fully protects film grain on the guarded path!
				pl_tex grainPublished = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 64.0f; });

				// Distinct crisp sharp branch (POSTKERNEL high-pass sharpening hook placed BEFORE module.Blend()) so sharp
				// and alias-safe branches genuinely diverge and carry non-zero high-frequency grain residual!
				static const char sharpHookSrc[] =
					"//!HOOK POSTKERNEL\n"
					"//!BIND HOOKED\n"
					"//!DESC test crisp sharp branch\n"
					"vec4 hook() {\n"
					"    vec4 c = HOOKED_tex(HOOKED_pos);\n"
					"    vec4 blur = 0.25 * (HOOKED_texOff(vec2(-1, 0)) + HOOKED_texOff(vec2(1, 0)) + HOOKED_texOff(vec2(0, -1)) + HOOKED_texOff(vec2(0, 1)));\n"
					"    return c + 1.5 * (c - blur);\n"
					"}\n";
				const pl_hook* sharpHook = pl_mpv_user_shader_parse(rig.gpu, sharpHookSrc, sizeof(sharpHookSrc) - 1);
				Assert::IsNotNull(sharpHook, L"sharp test hook must parse");

				auto baseAt = [sw](int x, int y) -> float {
					if (x < sw / 3)
						return 512.0f / 1023.0f;   // flat mid-grey
					if (x < 2 * sw / 3)
						return 80.0f / 1023.0f;    // dark flat
					return 512.0f / 1023.0f + 0.06f * std::sin(x * 0.45f) * std::cos(y * 0.41f);   // texture
				};
				auto grainSample = [](uint32_t seed, int x, int y) -> float {
					// Deterministic Box-Muller film grain, sigma = 8 / 1023 codes (exact match to published N_b = 64).
					uint32_t h1 = (static_cast<uint32_t>(y * 1024 + x) * 747796405u + seed * 2891336453u) ^ 0x9e3779b9u;
					h1 = ((h1 >> ((h1 >> 28) + 4u)) ^ h1) * 277803737u;
					uint32_t h2 = h1 * 1664525u + 1013904223u;
					const double u1 = (static_cast<double>((h1 >> 8) & 0xffffffu) + 0.5) / 16777216.0;
					const double u2 = (static_cast<double>((h2 >> 8) & 0xffffffu) + 0.5) / 16777216.0;
					return static_cast<float>((8.0 / 1023.0) * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2));
				};

				pl_tex cleanBaseTex = rig.FloatPicture(sw, sh, [&](int x, int y, float* p) {
					p[0] = p[1] = p[2] = baseAt(x, y); p[3] = 1.0f;
				});
				pl_tex noisyFrames[3] = {};
				for (int f = 0; f < 3; ++f)
				{
					const uint32_t seed = static_cast<uint32_t>(101 + f * 977);
					noisyFrames[f] = rig.FloatPicture(sw, sh, [&](int x, int y, float* p) {
						p[0] = p[1] = p[2] = baseAt(x, y) + grainSample(seed, x, y); p[3] = 1.0f;
					});
				}

				// Prove genuine sharp vs alias-safe branch divergence on noisyFrames[2] (non-tautological!).
				const std::vector<float> sharpBranchPx = rig.RenderFloat32(noisyFrames[2], ow, oh, { sharpHook }, &pl_filter_lanczos);
				const std::vector<float> safeBranchPx = rig.RenderFloat32(noisyFrames[2], ow, oh, {}, &pl_filter_ewa_lanczossharp);
				double maxBranchDiff = 0.0;
				for (int y = 10; y < oh - 10; ++y)
					for (int x = 10; x < ow - 10; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * ow + x) * 4;
						maxBranchDiff = (std::max)(maxBranchDiff, std::fabs(static_cast<double>(sharpBranchPx[idx]) - safeBranchPx[idx]));
					}
				Assert::IsTrue(maxBranchDiff > 0.02,
					(L"MEDIUM-1 precondition: sharp vs alias-safe branches must diverge strongly, maxDiff=" + std::to_wstring(maxBranchDiff)).c_str());

				auto render3Frames = [&](float strength, bool stabiliseOn, bool grainGuardOn, pl_tex f0, pl_tex f1, pl_tex f2) {
					AdaptiveDownscale::Settings st;
					st.float32Picture = true;
					st.faithfulOrtho = false;   // polar EWA LanczosSharp alias-safe partner (distinct from sharpened Lanczos sharp branch!)
					st.cnnResidualStabilise = stabiliseOn;
					st.residualGrainGuard = grainGuardOn;
					st.grainFloor = 0.40f;
					st.grainLo = 2.0f; st.grainHi = 8.0f;
					st.motionFloor = 0.25f;
					SetMotionStrength(st, strength);

					AdaptiveDownscale::Module module;
					const pl_tex seqTex[3] = { f0, f1, f2 };
					std::vector<float> last;
					for (int i = 0; i < 3; ++i)
					{
						module.SetFrame(st, static_cast<uint64_t>(i + 1), 1.0f / 24.0f);
						AdaptiveDownscale::Upscale up;
						up.on = true; up.wBase = 0.85f; up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);
						module.SetUpscale(up);
						AdaptiveDownscale::GrainLevelSource gl;
						gl.level = grainPublished; gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
						module.SetGrainLevel(gl);
						AdaptiveDownscale::Motion m;
						m.field = motionField; m.frameTrust = trust;
						m.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
						module.SetMotion(m);
						last = rig.RenderFloat32(seqTex[i], ow, oh, { module.Measure(), sharpHook, module.Blend() }, &pl_filter_lanczos);
					}
					Assert::IsTrue(module.Active() && module.Failures() == 0);
					if (stabiliseOn)
						Assert::IsTrue(module.StabilisedFrames() >= 2, L"residual stabilisation must actually execute on frames 2 & 3");
					module.Release();
					return last;
				};

				struct BandSpec { const wchar_t* name; int x0; int x1; double minRetention; };
				const BandSpec bands[3] = {
					{ L"flat", 10, ow / 3 - 10, 0.95 },
					{ L"dark-flat", ow / 3 + 10, 2 * ow / 3 - 10, 0.95 },
					{ L"texture", 2 * ow / 3 + 10, ow - 10, 0.90 }
				};

				for (float strength : { 0.5f, 1.0f })
				{
					// Single-frame unstabilised hybrid output of frame 2 (exact per-frame grain reference for this blend strength).
					const std::vector<float> refHybClean = render3Frames(strength, false, true, cleanBaseTex, cleanBaseTex, cleanBaseTex);
					const std::vector<float> refHybNoisy = render3Frames(strength, false, true, noisyFrames[0], noisyFrames[1], noisyFrames[2]);
					// Guarded 3-frame production path (residualGrainGuard = true).
					const std::vector<float> guardedClean = render3Frames(strength, true, true, cleanBaseTex, cleanBaseTex, cleanBaseTex);
					const std::vector<float> guardedNoisy = render3Frames(strength, true, true, noisyFrames[0], noisyFrames[1], noisyFrames[2]);
					// In-module negative control on the exact same 3 frames: residualGrainGuard = false!
					const std::vector<float> unguardedClean = render3Frames(strength, true, false, cleanBaseTex, cleanBaseTex, cleanBaseTex);
					const std::vector<float> unguardedNoisy = render3Frames(strength, true, false, noisyFrames[0], noisyFrames[1], noisyFrames[2]);

					for (const BandSpec& b : bands)
					{
						double ssGuard = 0.0, ssUnguard = 0.0, ssRef = 0.0;
						for (int y = 10; y < oh - 10; ++y)
							for (int x = b.x0; x < b.x1; ++x)
							{
								const size_t idx = (static_cast<size_t>(y) * ow + x) * 4;
								const double gg = static_cast<double>(guardedNoisy[idx]) - guardedClean[idx];
								const double gu = static_cast<double>(unguardedNoisy[idx]) - unguardedClean[idx];
								const double gr = static_cast<double>(refHybNoisy[idx]) - refHybClean[idx];
								ssGuard += gg * gg;
								ssUnguard += gu * gu;
								ssRef += gr * gr;
							}
						const double guardedRetention = std::sqrt(ssGuard / ssRef);
						const double unguardedRetention = std::sqrt(ssUnguard / ssRef);
						Assert::IsTrue(unguardedRetention < guardedRetention - 0.04,
							(std::wstring(L"in-module negative control (residualGrainGuard=false) must lose grain vs guarded in ") +
								b.name + L" at strength=" + std::to_wstring(strength) + L": unguarded=" +
								std::to_wstring(unguardedRetention) + L", guarded=" + std::to_wstring(guardedRetention)).c_str());
						Assert::IsTrue(guardedRetention >= b.minRetention && guardedRetention <= 1.05,
							(std::wstring(b.name) + L" grain retention out of two-sided bounds at strength=" +
								std::to_wstring(strength) + L": " + std::to_wstring(guardedRetention)).c_str());
					}
				}
				pl_mpv_user_shader_destroy(&sharpHook);
			}

			// Require single motionStrength continuum contract to pass this suite end-to-end.
			Assert::IsTrue(SettingsHasMotionStrength() && HasMotionTerm4(),
				L"Single motionStrength continuum (Settings::motionStrength + 4-arg HybridBlend::MotionTerm) must be implemented");

			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}

		// S4-H1 (WARP GPU): Cadence repeat / de-judder extra presentations of the same source frame (seq = 1)
		// after ClearMotion() / ClearGrainField() (the exact behaviour of AdaptiveDownscaleStage::Register() when
		// host.motion().live == false on a repeat/extra) MUST preserve BOTH the built map's sourceCrop (including
		// non-zero origin) AND its valid NlsMaskMapping::Mapping so neither motion_map nor grain_map collapses to
		// top-left (0, 0) or drops NLS on repeat presentations (zero strobing).
		BEGIN_TEST_METHOD_ATTRIBUTE(RepeatFramePreservesMotionAndGrainMapCropAndNls)
			TEST_METHOD_ATTRIBUTE(L"Category", L"GpuWarp")
		END_TEST_METHOD_ATTRIBUTE()
		TEST_METHOD(RepeatFramePreservesMotionAndGrainMapCropAndNls)
		{
			using namespace AdaptiveMotionUpgradeDetail;
			WarpRig rig;
			Assert::IsTrue(rig.Create(), L"Could not create WARP GPU test device");
			std::string log;
			rig.fixture.CaptureLog(&log);

			pl_tex trust = rig.Trust(1.0f);
			auto knownCut = [](uint64_t, uint64_t) {
				AdaptiveDownscale::CutVerdict v;
				v.state = AdaptiveDownscale::CutVerdict::State::Known;
				return v;
			};
			auto maxRgbDiff = [](const std::vector<float>& a, const std::vector<float>& b,
				int w, int x0, int x1, int y0, int y1) -> float
			{
				float worst = 0.0f;
				for (int y = y0; y < y1; ++y)
					for (int x = x0; x < x1; ++x)
					{
						const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
						for (int c = 0; c < 3; ++c)
							worst = (std::max)(worst, std::fabs(a[idx + c] - b[idx + c]));
					}
				return worst;
			};

			// Proven valid NLS mapping: u = 0.75 maps to u_nls = 0.8125 (2 full 16-px blocks right of linear UV on a 512-px source).
			const std::map<std::string, std::string> nlsParams = {
				{ "curve", "2.0" }, { "geometry", "0" }, { "center_protection", "0.0" }, { "strength", "1.0" }
			};
			const NlsMaskMapping::Mapping validNls = NlsMaskMapping::FromParameters(nlsParams, 1.5, false, false);
			Assert::IsTrue(validNls.valid && std::fabs(validNls.xScale - 1.5) < 1e-6,
				L"valid NLS mapping precondition");
			Assert::AreEqual(0.8125,
				NlsMaskMapping::MapCoordinate(0.75, validNls.xScale, validNls.curve, validNls.geometry, validNls.xProtect), 1e-6,
				L"NLS coordinate mapping precondition at u=0.75");

			// -----------------------------------------------------------------------------------------
			// 1A. Motion Map Repeat-Frame Parity (spatially distinct halves + non-zero-origin crop)
			// -----------------------------------------------------------------------------------------
			{
				const int sw = 128, sh = 64, ow = 192, oh = 96;
				// 10x6 block full-frame motion field (160x96 px) with non-zero active sourceCrop {16, 16, 144, 80}:
				//   - bx = 0..3 (incl. top-left (0,0) and crop left half bx=1..3): static (0, 0), conf=1 -> motionMap.r = 1.0 (sharp)
				//   - bx = 4: shear boundary adjacent to bx=5 -> motionMap.r = 0.0
				//   - bx = 5..9 (crop right half bx=5..8): fast moving (24, 0), conf=1 (9.0 heights/s >> 4*0.4) -> motionMap.r = 0.0 (alias-safe)
				const int bw = 10, bh = 6;
				const pl_rect2df activeCrop = { 16.0f, 16.0f, 144.0f, 80.0f };
				pl_tex src = rig.TexturedUnorm16(sw, sh);
				pl_tex cleanGrain = rig.Field(sw / 16, sh / 16, [](int, int, float* c) { c[0] = 0.0f; });
				pl_tex splitMotion = rig.Field(bw, bh, [](int x, int, float* c) {
					c[0] = (x >= 5) ? 24.0f : 0.0f;
					c[1] = 0.0f;
					c[2] = 1.0f;
					c[3] = 0.0f;
				});
				pl_tex staticOnlyMotion = rig.Field(bw, bh, [](int, int, float* c) {
					c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;
				});

				AdaptiveDownscale::Settings st;
				st.float32Picture = true;
				st.faithfulOrtho = false;
				st.cnnResidualStabilise = false;
				st.ssimResidualStabilise = false;
				st.grainMap = false;
				st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;
				st.motionSpeedLo = 0.10f; st.motionSpeedHi = 0.40f;
				st.motionFloor = 0.0f;
				SetMotionStrength(st, 1.0f);

				AdaptiveDownscale::Upscale up;
				up.on = true; up.wBase = 1.0f;
				up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);

				AdaptiveDownscale::GrainLevelSource gl;
				gl.level = cleanGrain;
				gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };

				// Static-only reference (exact same weight that EVERY pixel receives if mw_crop collapses to {0,0,0,0} -> (0,0)).
				std::vector<float> staticOnlyOut;
				{
					AdaptiveDownscale::Module refMod;
					refMod.SetCutSource(knownCut);
					refMod.SetFrame(st, 1, 1.0f / 24.0f);
					refMod.SetUpscale(up);
					refMod.SetGrainLevel(gl);
					AdaptiveDownscale::Motion mRef;
					mRef.field = staticOnlyMotion; mRef.frameTrust = trust;
					mRef.sourceCrop = activeCrop;
					refMod.SetMotion(mRef);
					staticOnlyOut = rig.RenderFloat32(src, ow, oh, { refMod.Measure(), refMod.Blend() }, &pl_filter_bilinear);
					Assert::IsTrue(refMod.Active() && refMod.Failures() == 0 &&
						refMod.LastMotionState() == AdaptiveDownscale::MotionState::Used);
					refMod.Release();
				}

				AdaptiveDownscale::Module module;
				module.SetCutSource(knownCut);
				// Presentation 1: live motion field with active non-zero-origin crop.
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetUpscale(up);
				module.SetGrainLevel(gl);
				AdaptiveDownscale::Motion m;
				m.field = splitMotion; m.frameTrust = trust;
				m.sourceCrop = activeCrop;
				module.SetMotion(m);
				const std::vector<float> firstOut = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMotionState() == AdaptiveDownscale::MotionState::Used && !module.MotionFloorUsed(),
					L"S4-H1 (1A): presentation 1 must build and use the motion map");

				// Presentation 2: cadence repeat / de-judder extra of the exact same source frame (seq = 1) after ClearMotion().
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetUpscale(up);
				module.SetGrainLevel(gl);
				module.ClearMotion();
				const std::vector<float> repeatOut = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMotionState() == AdaptiveDownscale::MotionState::Used && !module.MotionFloorUsed(),
					L"S4-H1 (1A): repeat presentation of seq=1 after ClearMotion() must keep MotionState::Used");
				module.Release();

				// Mandatory same-input negative control: static left half matches staticOnlyOut, while fast-moving right half
				// strongly diverges from staticOnlyOut (> 0.02), proving that collapsing mw_crop to {0,0,0,0} (sampling (0,0) = static)
				// causes a massive visible strobe on the right half!
				const float leftVsStatic = maxRgbDiff(firstOut, staticOnlyOut, ow, 10, 56, 10, oh - 10);
				const float rightVsStatic = maxRgbDiff(firstOut, staticOnlyOut, ow, 116, ow - 10, 10, oh - 10);
				Assert::IsTrue(leftVsStatic <= 1e-5f,
					(L"S4-H1 (1A) precondition: static left half must match staticOnlyOut, diff=" + std::to_wstring(leftVsStatic)).c_str());
				Assert::IsTrue(rightVsStatic > 0.02f,
					(L"S4-H1 (1A) negative control: moving right half must diverge strongly from staticOnlyOut (>0.02), diff=" +
						std::to_wstring(rightVsStatic)).c_str());

				// Positive repeat-frame parity across static left half, moving right half, AND full frame (catches origin loss at boundary!).
				const float leftRepeatDiff = maxRgbDiff(repeatOut, firstOut, ow, 10, 56, 10, oh - 10);
				const float rightRepeatDiff = maxRgbDiff(repeatOut, firstOut, ow, 116, ow - 10, 10, oh - 10);
				const float fullRepeatDiff = maxRgbDiff(repeatOut, firstOut, ow, 0, ow, 0, oh);
				Assert::IsTrue(leftRepeatDiff <= 1e-5f,
					(L"S4-H1 (1A): repeat frame static left half diverged from first presentation, diff=" + std::to_wstring(leftRepeatDiff)).c_str());
				Assert::IsTrue(rightRepeatDiff <= 1e-5f,
					(L"S4-H1 (1A): repeat frame fast-moving right half strobed (mw_crop lost on ClearMotion), diff=" + std::to_wstring(rightRepeatDiff)).c_str());
				Assert::IsTrue(fullRepeatDiff <= 1e-5f,
					(L"S4-H1 (1A): repeat frame full-frame parity failed (crop origin/extent drift), diff=" + std::to_wstring(fullRepeatDiff)).c_str());
			}

			// -----------------------------------------------------------------------------------------
			// 1B. Motion Map Repeat-Frame Parity under Active Valid NLS (catches caching crop without NLS!)
			// -----------------------------------------------------------------------------------------
			{
				const int sw = 512, sh = 64, bw = sw / 16, bh = sh / 16, ow = 768, oh = 96;
				pl_tex src = rig.TexturedUnorm16(sw, sh);
				pl_tex cleanGrain = rig.Field(bw, bh, [](int, int, float* c) { c[0] = 0.0f; });
				// Untrusted/alias-safe (0.0) ONLY at NLS-mapped blocks bx = 25..26 (u_nls = 0.8125 from u = 0.75 -> x_out = 576);
				// sharp (1.0) everywhere else including top-left (0,0) and linear u=0.75 blocks bx = 23..24!
				pl_tex nlsMotion = rig.Field(bw, bh, [](int x, int, float* c) {
					c[0] = 0.0f; c[1] = 0.0f; c[2] = (x == 25 || x == 26) ? 0.0f : 1.0f; c[3] = 0.0f;
				});

				AdaptiveDownscale::Settings st;
				st.float32Picture = true;
				st.faithfulOrtho = false;
				st.cnnResidualStabilise = false;
				st.ssimResidualStabilise = false;
				st.grainMap = false;
				st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;
				st.motionFloor = 0.0f;
				SetMotionStrength(st, 1.0f);

				AdaptiveDownscale::Upscale up;
				up.on = true; up.wBase = 1.0f;
				up.nativeWidth = static_cast<float>(sw); up.nativeHeight = static_cast<float>(sh);

				AdaptiveDownscale::GrainLevelSource gl;
				gl.level = cleanGrain;
				gl.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				gl.nls = validNls;

				// Same-input negative control without NLS (linear UV): at x_out=576 (u=0.75) reads linear blocks 23..24 (sharp 1.0).
				std::vector<float> linearMotionOut;
				{
					AdaptiveDownscale::Module linMod;
					linMod.SetCutSource(knownCut);
					linMod.SetFrame(st, 1, 1.0f / 24.0f);
					linMod.SetUpscale(up);
					linMod.SetGrainLevel(gl);
					AdaptiveDownscale::Motion mLin;
					mLin.field = nlsMotion; mLin.frameTrust = trust;
					mLin.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					linMod.SetMotion(mLin);
					linearMotionOut = rig.RenderFloat32(src, ow, oh, { linMod.Measure(), linMod.Blend() }, &pl_filter_bilinear);
					Assert::IsTrue(linMod.Active() && linMod.Failures() == 0);
					linMod.Release();
				}

				AdaptiveDownscale::Module module;
				module.SetCutSource(knownCut);
				// Presentation 1: valid NLS on motion map.
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetUpscale(up);
				module.SetGrainLevel(gl);
				AdaptiveDownscale::Motion mNls;
				mNls.field = nlsMotion; mNls.frameTrust = trust;
				mNls.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				mNls.nls = validNls;
				module.SetMotion(mNls);
				const std::vector<float> nlsFirstOut = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMotionState() == AdaptiveDownscale::MotionState::Used && !module.MotionFloorUsed(),
					L"S4-H1 (1B): NLS motion presentation 1 must use motion map");

				// Presentation 2: repeat seq = 1 after ClearMotion().
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetUpscale(up);
				module.SetGrainLevel(gl);
				module.ClearMotion();
				const std::vector<float> nlsRepeatOut = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMotionState() == AdaptiveDownscale::MotionState::Used && !module.MotionFloorUsed(),
					L"S4-H1 (1B): NLS motion repeat presentation must keep MotionState::Used");
				module.Release();

				// Negative control: valid NLS actively shifts x_out in [568, 584) onto blocks 25..26 (large diff vs linear UV).
				const float nlsVsLinear = maxRgbDiff(nlsFirstOut, linearMotionOut, ow, 568, 584, 12, oh - 12);
				Assert::IsTrue(nlsVsLinear > 0.02f,
					(L"S4-H1 (1B) negative control: valid NLS must diverge strongly from linear UV at x_out=576 (>0.02), diff=" +
						std::to_wstring(nlsVsLinear)).c_str());

				// Positive repeat-frame parity: ClearMotion() must preserve BOTH crop AND valid NLS on repeat!
				const float nlsBandRepeat = maxRgbDiff(nlsRepeatOut, nlsFirstOut, ow, 568, 584, 12, oh - 12);
				const float nlsFullRepeat = maxRgbDiff(nlsRepeatOut, nlsFirstOut, ow, 0, ow, 0, oh);
				Assert::IsTrue(nlsBandRepeat <= 1e-5f,
					(L"S4-H1 (1B): repeat frame dropped motion.nls mapping at x_out=576, diff=" + std::to_wstring(nlsBandRepeat)).c_str());
				Assert::IsTrue(nlsFullRepeat <= 1e-5f,
					(L"S4-H1 (1B): repeat frame full-frame NLS motion parity failed, diff=" + std::to_wstring(nlsFullRepeat)).c_str());
			}

			// -----------------------------------------------------------------------------------------
			// 2A. Grain Map Repeat-Frame Parity (downscale form with grainMap = true + non-zero-origin crop)
			// -----------------------------------------------------------------------------------------
			{
				const int sw = 256, sh = 128, ow = 160, oh = 80;
				// 18x10 block full-frame grain field (288x160 px) with non-zero active sourceCrop {16, 16, 272, 144}:
				//   - bx = 0..8 (incl. top-left (0,0) and crop left half bx=1..8): clean ge.r = 0.0 -> grain_scale = 1.0 (sharp)
				//   - bx = 9..17 (crop right half bx=9..16): heavy grain ge.r = 20.0 -> grain_scale = grainFloor = 0.0 (faithful)
				const int bw = 18, bh = 10;
				const pl_rect2df activeCrop = { 16.0f, 16.0f, 272.0f, 144.0f };
				pl_tex src = rig.TexturedUnorm16(sw, sh);
				pl_tex trustedField = rig.Field(bw, bh, [](int, int, float* c) {
					c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;
				});
				pl_tex splitGrainErr = rig.Field(bw, bh, [](int x, int, float* c) {
					c[0] = (x >= 9) ? 20.0f : 0.0f;
					c[1] = 127.5f;   // mid-grey luma -> shadow weight = 1.0
					c[2] = 0.0f;
					c[3] = 0.0f;
				});
				pl_tex cleanGrainErr = rig.Field(bw, bh, [](int, int, float* c) {
					c[0] = 0.0f; c[1] = 127.5f; c[2] = 0.0f; c[3] = 0.0f;
				});

				AdaptiveDownscale::Settings st;
				st.float32Picture = true;
				st.faithfulOrtho = false;
				st.wBase = 1.0f;
				st.grainMap = true;
				st.grainFloor = 0.0f;
				st.grainLo = 2.0f; st.grainHi = 8.0f;
				st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;
				st.cnnResidualStabilise = false;
				st.ssimResidualStabilise = false;
				SetMotionStrength(st, 0.0f);

				// Clean-only reference (exact same weight EVERY pixel receives if grain crop collapses to {0,0,0,0} -> (0,0)).
				std::vector<float> cleanOnlyOut;
				{
					AdaptiveDownscale::Module refMod;
					refMod.SetCutSource(knownCut);
					refMod.SetFrame(st, 1, 1.0f / 24.0f);
					refMod.SetSourceDownscales(true);
					AdaptiveDownscale::GrainField gfRef;
					gfRef.error = cleanGrainErr; gfRef.field = trustedField; gfRef.frameTrust = trust;
					gfRef.sourceCrop = activeCrop;
					refMod.SetGrainField(gfRef);
					cleanOnlyOut = rig.RenderFloat32(src, ow, oh, { refMod.Measure(), refMod.Blend() }, &pl_filter_bilinear);
					Assert::IsTrue(refMod.Active() && refMod.Failures() == 0 &&
						refMod.LastMapState() == AdaptiveDownscale::MapState::Used);
					refMod.Release();
				}

				AdaptiveDownscale::Module module;
				module.SetCutSource(knownCut);
				// Presentation 1: live grainField with active non-zero-origin crop.
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetSourceDownscales(true);
				AdaptiveDownscale::GrainField gf;
				gf.error = splitGrainErr; gf.field = trustedField; gf.frameTrust = trust;
				gf.sourceCrop = activeCrop;
				module.SetGrainField(gf);
				const std::vector<float> firstGrainOut = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMapState() == AdaptiveDownscale::MapState::Used,
					L"S4-H1 (2A): downscale grain presentation 1 must use grain map");

				// Presentation 2: repeat seq = 1 after ClearGrainField().
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetSourceDownscales(true);
				module.ClearGrainField();
				const std::vector<float> repeatGrainOut = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMapState() == AdaptiveDownscale::MapState::Used,
					L"S4-H1 (2A): downscale repeat presentation of seq=1 after ClearGrainField() must keep MapState::Used");
				module.Release();

				// Mandatory same-input negative control: clean left half matches cleanOnlyOut while heavy-grain right half
				// strongly diverges from cleanOnlyOut (> 0.02), proving spatial grain-map modulation is non-vacuous and that
				// collapsing crop to {0,0,0,0} (sampling clean (0,0)) causes a large visible strobe on the right half!
				const float grainLeftVsClean = maxRgbDiff(firstGrainOut, cleanOnlyOut, ow, 12, 68, 10, oh - 10);
				const float grainRightVsClean = maxRgbDiff(firstGrainOut, cleanOnlyOut, ow, 92, ow - 12, 10, oh - 10);
				Assert::IsTrue(grainLeftVsClean <= 1e-5f,
					(L"S4-H1 (2A) precondition: clean left half must match cleanOnlyOut, diff=" + std::to_wstring(grainLeftVsClean)).c_str());
				Assert::IsTrue(grainRightVsClean > 0.02f,
					(L"S4-H1 (2A) negative control: heavy-grain right half must diverge strongly from cleanOnlyOut (>0.02), diff=" +
						std::to_wstring(grainRightVsClean)).c_str());

				// Positive repeat-frame parity across clean left half, heavy-grain right half, AND full frame.
				const float grainLeftRepeat = maxRgbDiff(repeatGrainOut, firstGrainOut, ow, 12, 68, 10, oh - 10);
				const float grainRightRepeat = maxRgbDiff(repeatGrainOut, firstGrainOut, ow, 92, ow - 12, 10, oh - 10);
				const float grainFullRepeat = maxRgbDiff(repeatGrainOut, firstGrainOut, ow, 0, ow, 0, oh);
				Assert::IsTrue(grainLeftRepeat <= 1e-5f,
					(L"S4-H1 (2A): repeat frame clean left half diverged, diff=" + std::to_wstring(grainLeftRepeat)).c_str());
				Assert::IsTrue(grainRightRepeat <= 1e-5f,
					(L"S4-H1 (2A): repeat frame heavy-grain right half strobed (grainField.sourceCrop lost on ClearGrainField), diff=" +
						std::to_wstring(grainRightRepeat)).c_str());
				Assert::IsTrue(grainFullRepeat <= 1e-5f,
					(L"S4-H1 (2A): repeat frame full-frame grain map parity failed (crop origin/extent drift), diff=" +
						std::to_wstring(grainFullRepeat)).c_str());
			}

			// -----------------------------------------------------------------------------------------
			// 2B. Grain Map Repeat-Frame Parity under Active Valid NLS (catches caching crop without NLS!)
			// -----------------------------------------------------------------------------------------
			{
				const int sw = 512, sh = 128, bw = sw / 16, bh = sh / 16, ow = 320, oh = 80;
				pl_tex src = rig.TexturedUnorm16(sw, sh);
				pl_tex trustedField = rig.Field(bw, bh, [](int, int, float* c) {
					c[0] = 0.0f; c[1] = 0.0f; c[2] = 1.0f; c[3] = 0.0f;
				});
				// Heavy grain ONLY at NLS-mapped blocks bx = 25..26 (u_nls = 0.8125 from u = 0.75 -> x_out = 240);
				// clean everywhere else including top-left (0,0) and linear u=0.75 blocks bx = 23..24!
				pl_tex nlsGrainErr = rig.Field(bw, bh, [](int x, int, float* c) {
					c[0] = (x == 25 || x == 26) ? 20.0f : 0.0f;
					c[1] = 127.5f;
					c[2] = 0.0f;
					c[3] = 0.0f;
				});

				AdaptiveDownscale::Settings st;
				st.float32Picture = true;
				st.faithfulOrtho = false;
				st.wBase = 1.0f;
				st.grainMap = true;
				st.grainFloor = 0.0f;
				st.grainLo = 2.0f; st.grainHi = 8.0f;
				st.edgeLo = 1.0e6f; st.edgeHi = 2.0e6f;
				st.cnnResidualStabilise = false;
				st.ssimResidualStabilise = false;
				SetMotionStrength(st, 0.0f);

				// Same-input negative control without NLS (linear UV): x_out in [237, 244) reads linear blocks 23..24 (clean 1.0).
				std::vector<float> linearGrainOut;
				{
					AdaptiveDownscale::Module linMod;
					linMod.SetCutSource(knownCut);
					linMod.SetFrame(st, 1, 1.0f / 24.0f);
					linMod.SetSourceDownscales(true);
					AdaptiveDownscale::GrainField gfLin;
					gfLin.error = nlsGrainErr; gfLin.field = trustedField; gfLin.frameTrust = trust;
					gfLin.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
					linMod.SetGrainField(gfLin);
					linearGrainOut = rig.RenderFloat32(src, ow, oh, { linMod.Measure(), linMod.Blend() }, &pl_filter_bilinear);
					Assert::IsTrue(linMod.Active() && linMod.Failures() == 0);
					linMod.Release();
				}

				AdaptiveDownscale::Module module;
				module.SetCutSource(knownCut);
				// Presentation 1: valid NLS on grain map.
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetSourceDownscales(true);
				AdaptiveDownscale::GrainField gfNls;
				gfNls.error = nlsGrainErr; gfNls.field = trustedField; gfNls.frameTrust = trust;
				gfNls.sourceCrop = { 0.0f, 0.0f, static_cast<float>(sw), static_cast<float>(sh) };
				gfNls.nls = validNls;
				module.SetGrainField(gfNls);
				const std::vector<float> grainNlsFirst = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMapState() == AdaptiveDownscale::MapState::Used,
					L"S4-H1 (2B): NLS grain presentation 1 must use grain map");

				// Presentation 2: repeat seq = 1 after ClearGrainField().
				module.SetFrame(st, 1, 1.0f / 24.0f);
				module.SetSourceDownscales(true);
				module.ClearGrainField();
				const std::vector<float> grainNlsRepeat = rig.RenderFloat32(src, ow, oh, { module.Measure(), module.Blend() }, &pl_filter_bilinear);
				Assert::IsTrue(module.Active() && module.Failures() == 0 &&
					module.LastMapState() == AdaptiveDownscale::MapState::Used,
					L"S4-H1 (2B): NLS grain repeat presentation must keep MapState::Used");
				module.Release();

				// Negative control: valid NLS actively shifts x_out in [237, 244) onto heavy-grain blocks 25..26 (large diff vs linear UV).
				const float grainNlsVsLinear = maxRgbDiff(grainNlsFirst, linearGrainOut, ow, 237, 244, 10, oh - 10);
				Assert::IsTrue(grainNlsVsLinear > 0.02f,
					(L"S4-H1 (2B) negative control: valid NLS grain map must diverge strongly from linear UV at x_out=240 (>0.02), diff=" +
						std::to_wstring(grainNlsVsLinear)).c_str());

				// Positive repeat-frame parity: ClearGrainField() must preserve BOTH crop AND valid NLS on repeat!
				const float grainNlsBandRepeat = maxRgbDiff(grainNlsRepeat, grainNlsFirst, ow, 237, 244, 10, oh - 10);
				const float grainNlsFullRepeat = maxRgbDiff(grainNlsRepeat, grainNlsFirst, ow, 0, ow, 0, oh);
				Assert::IsTrue(grainNlsBandRepeat <= 1e-5f,
					(L"S4-H1 (2B): repeat frame dropped grainField.nls mapping at x_out=240, diff=" +
						std::to_wstring(grainNlsBandRepeat)).c_str());
				Assert::IsTrue(grainNlsFullRepeat <= 1e-5f,
					(L"S4-H1 (2B): repeat frame full-frame NLS grain map parity failed, diff=" +
						std::to_wstring(grainNlsFullRepeat)).c_str());
			}

			rig.fixture.CaptureLog(nullptr);
			AssertCleanLibplaceboLog(log);
		}
	};
}
