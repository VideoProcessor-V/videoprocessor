// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Milestone 0 Deterministic Synthetic Physics Sports Testbed for Small Fast Objects & Trajectory Study.
// Pure C++14/17, deterministic SplitMix64 + Box-Muller noise, 16x temporal sub-frame oversampling
// for true physical motion blur (both camera pan and curved/bouncing object trajectories), opaque
// painter's-order compositing, half-res Y/Cb/Cr downsampling, P8/CE-WATER guard masks, ground-truth
// per-frame centroids/velocities/masks/trajectory types, and unprotected baselines (2-frame residual
// & full-range 16x16 SAD block matcher).
#pragma once

#include <VP-V/modules/motion/estimation/fast_objects/FastObjectOverlay.h>
#include <VP-V/modules/motion/estimation/fast_objects/FastObjectTracker.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace VideoProcessorTest
{
	namespace FastObjectSynth
	{
		constexpr int kSubframes = 16; // Mandatory 16x temporal sub-frame oversampling
		constexpr double kPi = 3.14159265358979323846;

		// Deterministic SplitMix64 PRNG + Box-Muller Gaussian generator (zero std::rand / OS state).
		struct DetRng
		{
			uint64_t state = 0x9E3779B97F4A7C15ull;
			bool hasSpare = false;
			double spare = 0.0;

			explicit DetRng(uint64_t seed = 1ull)
				: state(seed ? seed : 0x9E3779B97F4A7C15ull)
			{
			}

			uint64_t NextU64()
			{
				uint64_t z = (state += 0x9E3779B97F4A7C15ull);
				z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
				z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
				return z ^ (z >> 31);
			}

			double Uniform01()
			{
				return (static_cast<double>(NextU64() >> 11) + 0.5) * (1.0 / 9007199254740992.0);
			}

			double UniformRange(double lo, double hi)
			{
				return lo + (hi - lo) * Uniform01();
			}

			double Normal01()
			{
				if (hasSpare)
				{
					hasSpare = false;
					return spare;
				}
				// Exact Marsaglia polar Gaussian generator (zero sin/cos calls).
				double u = 0.0, v = 0.0, s = 0.0;
				do
				{
					u = 2.0 * Uniform01() - 1.0;
					v = 2.0 * Uniform01() - 1.0;
					s = u * u + v * v;
				} while (!(s > 1e-15 && s < 1.0));
				const double mul = std::sqrt(-2.0 * std::log(s) / s);
				spare = v * mul;
				hasSpare = true;
				return u * mul;
			}
		};

		enum class SurfaceKind : uint8_t
		{
			SoccerGrass = 0,
			TennisCourt = 1,
			HockeyIce = 2,
			HardwoodCourt = 3,
			IsoLumaCourt = 4,
			WaterSurface = 5
		};

		// Precomputed world-plane background canvas (Y in [0,1], Cb/Cr in [-0.5, 0.5]) + optional
		// pre-integrated 16-tap camera-pan shutter canvas (mathematically identical to 16x per-pixel
		// background integration when zoom == 1, while accelerating 1080p rendering by >10x).
		struct WorldBackdrop
		{
			int width = 0;
			int height = 0;
			double originX = 0.0;
			double originY = 0.0;
			std::vector<float> y;
			std::vector<float> cb;
			std::vector<float> cr;

			bool hasPanBlur = false;
			std::vector<float> blurY;
			std::vector<float> blurCb;
			std::vector<float> blurCr;

			void Build(
				int w, int h, double ox, double oy,
				SurfaceKind kind, uint64_t seed, float fixedIsoLuma,
				bool preintegrateLinearPan, double shutterPanX, double shutterPanY)
			{
				width = w;
				height = h;
				originX = ox;
				originY = oy;
				const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
				y.resize(n);
				cb.resize(n);
				cr.resize(n);

				DetRng rng(seed);
				struct Wave { double kx, ky, ph, amp; };
				constexpr int kNumWaves = 12;
				Wave waves[kNumWaves];
				double ampSum2 = 0.0;
				for (int i = 0; i < kNumWaves; ++i)
				{
					// Multi-scale stadium turf/ice/court texture spanning 4.5 px blades up to 80 px stripes.
					const double wl = 4.5 * std::pow(80.0 / 4.5, static_cast<double>(i) / static_cast<double>(kNumWaves - 1));
					const double ang = 2.0 * kPi * rng.Uniform01();
					waves[i].kx = (2.0 * kPi / wl) * std::cos(ang);
					waves[i].ky = (2.0 * kPi / wl) * std::sin(ang);
					waves[i].ph = 2.0 * kPi * rng.Uniform01();
					waves[i].amp = 1.0;
					ampSum2 += 1.0;
				}
				const double invNorm = 1.0 / std::sqrt(ampSum2);
				for (int i = 0; i < kNumWaves; ++i)
					waves[i].amp *= invNorm;

				std::vector<double> tex(n, 0.0);
				std::vector<double> sx(static_cast<size_t>(w)), cx(static_cast<size_t>(w));
				std::vector<double> sy(static_cast<size_t>(h)), cy(static_cast<size_t>(h));
				for (int wi = 0; wi < kNumWaves; ++wi)
				{
					const Wave& wv = waves[wi];
					for (int ix = 0; ix < w; ++ix)
					{
						const double wx = static_cast<double>(ix) + ox;
						sx[static_cast<size_t>(ix)] = std::sin(wv.kx * wx);
						cx[static_cast<size_t>(ix)] = std::cos(wv.kx * wx);
					}
					for (int iy = 0; iy < h; ++iy)
					{
						const double wy = static_cast<double>(iy) + oy;
						sy[static_cast<size_t>(iy)] = std::sin(wv.ky * wy + wv.ph);
						cy[static_cast<size_t>(iy)] = std::cos(wv.ky * wy + wv.ph);
					}
					for (int iy = 0; iy < h; ++iy)
					{
						double* row = &tex[static_cast<size_t>(iy) * static_cast<size_t>(w)];
						const double a = wv.amp * cy[static_cast<size_t>(iy)];
						const double b = wv.amp * sy[static_cast<size_t>(iy)];
						for (int ix = 0; ix < w; ++ix)
							row[ix] += sx[static_cast<size_t>(ix)] * a + cx[static_cast<size_t>(ix)] * b;
					}
				}

				const double midY = 0.5 * static_cast<double>(h) + oy;
				const double midX = 0.5 * static_cast<double>(w) + ox;

				for (int iy = 0; iy < h; ++iy)
				{
					const double wy = static_cast<double>(iy) + oy;
					for (int ix = 0; ix < w; ++ix)
					{
						const double wx = static_cast<double>(ix) + ox;
						const size_t idx = static_cast<size_t>(iy) * static_cast<size_t>(w) + static_cast<size_t>(ix);
						const double t = tex[idx];

						double luma = 0.30;
						double cBlue = -0.06;
						double cRed = -0.04;

						if (kind == SurfaceKind::SoccerGrass)
						{
							const double stripe = 0.04 * std::sin(wx * (2.0 * kPi / 120.0));
							luma = 0.30 + stripe + 0.14 * t;
							cBlue = -0.10 + 0.015 * t;
							cRed = -0.08 - 0.010 * t;

							const double dLine1 = std::fabs(wy - midY);
							const double dLine2 = std::fabs(wx - midX);
							const double dDiag = std::fabs(0.6 * (wx - (midX - 180.0)) - 0.8 * (wy - midY));
							const double lineAlpha = std::fmax(
								std::fmax(std::fmax(0.0, 2.5 - dLine1), std::fmax(0.0, 2.5 - dLine2)),
								std::fmax(0.0, 2.0 - dDiag));
							const double a = std::fmin(1.0, lineAlpha);
							luma = luma * (1.0 - a) + 0.92 * a;
							cBlue *= (1.0 - a);
							cRed *= (1.0 - a);
						}
						else if (kind == SurfaceKind::TennisCourt)
						{
							luma = 0.34 + 0.12 * t;
							cBlue = 0.09 + 0.015 * t;
							cRed = -0.05 + 0.015 * t;

							const double dBase = std::fabs(wy - (midY + 90.0));
							const double dCenter = std::fabs(wx - midX);
							const double a = std::fmin(1.0, std::fmax(std::fmax(0.0, 2.5 - dBase), std::fmax(0.0, 2.0 - dCenter)));
							luma = luma * (1.0 - a) + 0.93 * a;
							cBlue *= (1.0 - a);
							cRed *= (1.0 - a);
						}
						else if (kind == SurfaceKind::HockeyIce)
						{
							luma = 0.80 + 0.11 * t;
							cBlue = 0.025 + 0.01 * t;
							cRed = -0.015 + 0.01 * t;

							const double aBlue1 = std::fmin(1.0, std::fmax(0.0, 3.5 - std::fabs(wx - (midX - 220.0))));
							const double aRed = std::fmin(1.0, std::fmax(0.0, 3.5 - std::fabs(wx - midX)));
							const double aBlue2 = std::fmin(1.0, std::fmax(0.0, 3.5 - std::fabs(wx - (midX + 220.0))));
							if (aBlue1 > 0.0 || aBlue2 > 0.0)
							{
								const double ab = std::fmax(aBlue1, aBlue2);
								luma = luma * (1.0 - ab) + 0.42 * ab;
								cBlue = cBlue * (1.0 - ab) + 0.22 * ab;
								cRed = cRed * (1.0 - ab) - 0.10 * ab;
							}
							if (aRed > 0.0)
							{
								luma = luma * (1.0 - aRed) + 0.44 * aRed;
								cBlue = cBlue * (1.0 - aRed) - 0.08 * aRed;
								cRed = cRed * (1.0 - aRed) + 0.26 * aRed;
							}
						}
						else if (kind == SurfaceKind::HardwoodCourt)
						{
							const double plank = 0.03 * std::sin(wy * (2.0 * kPi / 18.0));
							luma = 0.46 + plank + 0.11 * t;
							cBlue = -0.08 + 0.01 * t;
							cRed = 0.07 + 0.01 * t;
							const double aKey = std::fmin(1.0, std::fmax(0.0, 2.5 - std::fabs(wy - midY)));
							luma = luma * (1.0 - aKey) + 0.90 * aKey;
						}
						else if (kind == SurfaceKind::IsoLumaCourt)
						{
							luma = static_cast<double>(fixedIsoLuma);
							cBlue = 0.08 + 0.025 * t;
							cRed = -0.06 - 0.020 * t;
						}
						else // WaterSurface
						{
							luma = 0.26 + 0.09 * t;
							cBlue = 0.14 + 0.02 * t;
							cRed = -0.09 + 0.01 * t;
						}

						y[idx] = static_cast<float>(std::fmin(1.0, std::fmax(0.0, luma)));
						cb[idx] = static_cast<float>(std::fmin(0.5, std::fmax(-0.5, cBlue)));
						cr[idx] = static_cast<float>(std::fmin(0.5, std::fmax(-0.5, cRed)));
					}
				}

				hasPanBlur = preintegrateLinearPan;
				if (preintegrateLinearPan)
				{
					blurY.resize(n);
					blurCb.resize(n);
					blurCr.resize(n);
					constexpr float kInvSub = 1.0f / static_cast<float>(kSubframes);
					double dxTap[kSubframes], dyTap[kSubframes];
					for (int s = 0; s < kSubframes; ++s)
					{
						const double u = (static_cast<double>(s) + 0.5) / static_cast<double>(kSubframes) - 0.5;
						dxTap[s] = u * shutterPanX;
						dyTap[s] = u * shutterPanY;
					}
					for (int iy = 0; iy < h; ++iy)
					{
						const double wy = static_cast<double>(iy) + oy;
						for (int ix = 0; ix < w; ++ix)
						{
							const double wx = static_cast<double>(ix) + ox;
							float ayAcc = 0.0f, acbAcc = 0.0f, acrAcc = 0.0f;
							for (int s = 0; s < kSubframes; ++s)
							{
								float sy0 = 0.0f, scb0 = 0.0f, scr0 = 0.0f;
								SampleRawBilinear(wx + dxTap[s], wy + dyTap[s], sy0, scb0, scr0);
								ayAcc += sy0;
								acbAcc += scb0;
								acrAcc += scr0;
							}
							const size_t idx = static_cast<size_t>(iy) * static_cast<size_t>(w) + static_cast<size_t>(ix);
							blurY[idx] = ayAcc * kInvSub;
							blurCb[idx] = acbAcc * kInvSub;
							blurCr[idx] = acrAcc * kInvSub;
						}
					}
				}
			}

			inline void SamplePlanes(
				const std::vector<float>& py,
				const std::vector<float>& pcb,
				const std::vector<float>& pcr,
				double wx, double wy,
				float& outY, float& outCb, float& outCr) const
			{
				double sx = wx - originX;
				double sy = wy - originY;
				if (sx < 0.0) sx = 0.0;
				else if (sx > static_cast<double>(width - 1)) sx = static_cast<double>(width - 1);
				if (sy < 0.0) sy = 0.0;
				else if (sy > static_cast<double>(height - 1)) sy = static_cast<double>(height - 1);

				const int x0 = static_cast<int>(std::floor(sx));
				const int y0 = static_cast<int>(std::floor(sy));
				const int x1 = (x0 + 1 < width) ? (x0 + 1) : (width - 1);
				const int y1 = (y0 + 1 < height) ? (y0 + 1) : (height - 1);
				const float fx = static_cast<float>(sx - static_cast<double>(x0));
				const float fy = static_cast<float>(sy - static_cast<double>(y0));
				const size_t r0 = static_cast<size_t>(y0) * static_cast<size_t>(width);
				const size_t r1 = static_cast<size_t>(y1) * static_cast<size_t>(width);
				const float w00 = (1.0f - fx) * (1.0f - fy);
				const float w10 = fx * (1.0f - fy);
				const float w01 = (1.0f - fx) * fy;
				const float w11 = fx * fy;
				const size_t i00 = r0 + static_cast<size_t>(x0);
				const size_t i10 = r0 + static_cast<size_t>(x1);
				const size_t i01 = r1 + static_cast<size_t>(x0);
				const size_t i11 = r1 + static_cast<size_t>(x1);
				outY = py[i00] * w00 + py[i10] * w10 + py[i01] * w01 + py[i11] * w11;
				outCb = pcb[i00] * w00 + pcb[i10] * w10 + pcb[i01] * w01 + pcb[i11] * w11;
				outCr = pcr[i00] * w00 + pcr[i10] * w10 + pcr[i01] * w01 + pcr[i11] * w11;
			}

			inline void SampleRawBilinear(double wx, double wy, float& outY, float& outCb, float& outCr) const
			{
				SamplePlanes(y, cb, cr, wx, wy, outY, outCb, outCr);
			}

			inline void SamplePanBlurred(double wx, double wy, float& outY, float& outCb, float& outCr) const
			{
				SamplePlanes(blurY, blurCb, blurCr, wx, wy, outY, outCb, outCr);
			}
		};

		struct GroundTruthBall
		{
			uint32_t id = 1;
			bool visible = true;
			bool occludedByPlayer = false;
			bool crossingAmbiguous = false;
			FastObjects::TrajectoryType expectedType = FastObjects::TrajectoryType::Unknown;
			float cx = 0.0f;
			float cy = 0.0f;
			float vx = 0.0f;
			float vy = 0.0f;
			float radiusPx = 0.0f;
			float streakLenPx = 0.0f;
			bool pairBracketsImpulse = false;
			float impulseImgX = 0.0f;
			float impulseImgY = 0.0f;
		};

		struct SynthFrame
		{
			uint64_t frameIndex = 0;
			double timestampSec = 0.0;
			double dtFromPrevSec = 0.0;

			int srcW = 0;
			int srcH = 0;
			int halfW = 0;
			int halfH = 0;

			std::vector<float> fullY;
			std::vector<float> halfY;
			std::vector<float> halfCb;
			std::vector<float> halfCr;

			CameraModel::Params camWorldToImg;
			CameraModel::Params camPrevToCur;
			bool camPrevUsable = true;
			double camSigmaPx = 0.15;
			CameraModel::Rect picture;

			int guardGridW = 0;
			int guardGridH = 0;
			double guardBlockSrcPx = 16.0;
			std::vector<uint8_t> blockLabels;
			std::vector<uint8_t> blockExcluded;
			std::vector<float> waterProb;
			std::vector<float> grainVarMap;

			std::vector<float> gtBallAlpha;
			std::vector<float> gtFlowX;
			std::vector<float> gtFlowY;
			std::vector<GroundTruthBall> gtBalls;
		};

		struct SynthSequence
		{
			int srcW = 1920;
			int srcH = 1080;
			double fps = 60.0;
			double shutterFraction = 0.5;
			double grainSigma = 0.004;
			int bitDepth = 10;
			std::vector<SynthFrame> frames;
		};

		inline CameraModel::Params MakePanZoomHomography(double camPanX, double camPanY, double zoomScale = 1.0)
		{
			CameraModel::Params p;
			p.type = (std::fabs(zoomScale - 1.0) > 1e-9)
				? CameraModel::ModelType::Similarity
				: CameraModel::ModelType::Translation;
			p.h[0] = zoomScale;
			p.h[1] = 0.0;
			p.h[2] = -camPanX * zoomScale;
			p.h[3] = 0.0;
			p.h[4] = zoomScale;
			p.h[5] = -camPanY * zoomScale;
			p.h[6] = 0.0;
			p.h[7] = 0.0;
			p.h[8] = 1.0;
			return p;
		}

		inline void WorldToImage(
			const CameraModel::Params& hWorldToImg,
			const CameraModel::Rect& pic,
			double wx, double wy,
			double& ix, double& iy)
		{
			const double cx = pic.CentreX();
			const double cy = pic.CentreY();
			double ox = 0.0, oy = 0.0;
			CameraModel::Apply(hWorldToImg, wx - cx, wy - cy, &ox, &oy);
			ix = ox + cx;
			iy = oy + cy;
		}

		struct BallSpec
		{
			uint32_t id = 1;
			FastObjects::TrajectoryType expectedType = FastObjects::TrajectoryType::LinearGlide;

			double x0 = 300.0, y0 = 500.0;
			double vx0 = 1800.0, vy0 = 0.0;
			double ax = 0.0, ay = 0.0;
			double dragDecel = 0.0;

			bool hasBounce = false;
			double tBounceSec = 0.0;
			double vxPost = 0.0, vyPost = 0.0;

			bool isPerspective = false;
			double kPerspPerSec = 0.0;
			double perspCenterX = 960.0;
			double perspCenterY = 540.0;

			double radiusPx = 6.0;
			float baseY = 0.92f;
			float baseCb = -0.02f;
			float baseCr = 0.02f;
			bool textured = true;
			double tStartSec = -1e9;
			double tEndSec = 1e9;

			void EvalWorldAt(double tSec, double& outWx, double& outWy, double& outRadius, double& outSpd) const
			{
				outRadius = radiusPx;
				if (isPerspective)
				{
					const double denom = std::fmax(0.15, 1.0 - kPerspPerSec * tSec);
					outRadius = radiusPx / denom;
					outWx = perspCenterX + ((x0 - perspCenterX) + vx0 * tSec) / denom;
					outWy = perspCenterY + ((y0 - perspCenterY) + vy0 * tSec) / denom + 0.5 * ay * tSec * tSec;
					const double eps = 1e-4;
					const double d2 = std::fmax(0.15, 1.0 - kPerspPerSec * (tSec + eps));
					const double wx2 = perspCenterX + ((x0 - perspCenterX) + vx0 * (tSec + eps)) / d2;
					const double wy2 = perspCenterY + ((y0 - perspCenterY) + vy0 * (tSec + eps)) / d2 + 0.5 * ay * (tSec + eps) * (tSec + eps);
					outSpd = std::hypot(wx2 - outWx, wy2 - outWy) / eps;
					return;
				}
				if (hasBounce && tSec > tBounceSec)
				{
					const double tb = tBounceSec;
					const double bx = x0 + vx0 * tb + 0.5 * ax * tb * tb;
					const double by = y0 + vy0 * tb + 0.5 * ay * tb * tb;
					const double dt = tSec - tb;
					outWx = bx + vxPost * dt + 0.5 * ax * dt * dt;
					outWy = by + vyPost * dt + 0.5 * ay * dt * dt;
					outSpd = std::hypot(vxPost + ax * dt, vyPost + ay * dt);
					return;
				}
				if (dragDecel > 0.0)
				{
					const double v0Mag = std::hypot(vx0, vy0);
					const double ux = (v0Mag > 1e-9) ? (vx0 / v0Mag) : 1.0;
					const double uy = (v0Mag > 1e-9) ? (vy0 / v0Mag) : 0.0;
					const double tStop = v0Mag / dragDecel;
					const double te = std::fmin(std::fmax(tSec, 0.0), tStop);
					const double dist = v0Mag * te - 0.5 * dragDecel * te * te;
					outWx = x0 + ux * dist;
					outWy = y0 + uy * dist;
					outSpd = std::fmax(0.0, v0Mag - dragDecel * te);
					return;
				}
				outWx = x0 + vx0 * tSec + 0.5 * ax * tSec * tSec;
				outWy = y0 + vy0 * tSec + 0.5 * ay * tSec * tSec;
				outSpd = std::hypot(vx0 + ax * tSec, vy0 + ay * tSec);
			}
		};

		struct PlayerBoxSpec
		{
			bool enabled = false;
			double wx0 = 0.0, wy0 = 0.0, wx1 = 0.0, wy1 = 0.0;
			double vx = 0.0, vy = 0.0;
			float luma = 0.16f;
			float cb = 0.05f;
			float cr = -0.02f;

			void BoundsAt(double tSec, double& l, double& t, double& r, double& b) const
			{
				l = wx0 + vx * tSec;
				r = wx1 + vx * tSec;
				t = wy0 + vy * tSec;
				b = wy1 + vy * tSec;
			}
		};

		struct OverlaySpec
		{
			bool enabled = false;
			CameraModel::Rect box;
			bool markInP8 = true;
			double scrollSpeedPxSec = 240.0;
			bool blinking = false;
		};

		struct ClutterParticle
		{
			enum class Kind : uint8_t { RandomWalk, Oscillatory, BlinkingStatic };
			Kind kind = Kind::RandomWalk;
			double baseWx = 0.0, baseWy = 0.0;
			double ampX = 18.0, ampY = 18.0;
			double freqHz = 20.0;
			double phase = 0.0;
			double radiusPx = 4.5;
			float luma = 0.92f;
			float cb = 0.0f;
			float cr = 0.0f;
			uint64_t seed = 1ull;
			bool screenFixed = false;

			void EvalPos(int frameIdx, double subT, double& ox, double& oy, bool& visible) const
			{
				visible = true;
				if (kind == Kind::RandomWalk)
				{
					DetRng r(seed ^ (static_cast<uint64_t>(frameIdx + 17) * 0x9E3779B97F4A7C15ull));
					ox = baseWx + r.UniformRange(-ampX, ampX);
					oy = baseWy + r.UniformRange(-ampY, ampY);
				}
				else if (kind == Kind::Oscillatory)
				{
					const double s = std::sin(2.0 * kPi * freqHz * subT + phase);
					ox = baseWx + ampX * s;
					oy = baseWy + ampY * std::cos(2.0 * kPi * freqHz * subT + phase);
				}
				else
				{
					// Stationary jittering/pulsing stadium LED ad-board element (< 2*r_est net motion).
					ox = baseWx + 2.0 * std::sin(2.0 * kPi * 18.0 * subT + phase);
					oy = baseWy + 1.5 * std::cos(2.0 * kPi * 14.0 * subT + phase);
					visible = true;
				}
			}

			float EvalLuma(int frameIdx) const
			{
				if (kind == Kind::BlinkingStatic)
				{
					// Pulsing LED ad-board contrast above background on every frame so D_- and D_+ share polarity
					// relative to shifting background or brightness step, testing Stage B stationary rejection + 64x36 hotspot!
					return ((frameIdx % 2) == 0) ? luma : (0.78f * luma);
				}
				return luma;
			}
		};

		struct SceneRecipe
		{
			int srcW = 1920;
			int srcH = 1080;
			int numFrames = 10;
			double fps = 60.0;
			double shutterFraction = 0.5;
			SurfaceKind surface = SurfaceKind::SoccerGrass;
			float isoLumaValue = 0.52f;
			double camPanVxPxFrame = 6.0;
			double camPanVyPxFrame = 0.0;
			double camZoomRatePerFrame = 0.0;
			double camEstJitterPx = 0.0;
			double camReportedSigmaPx = 0.2;
			double grainSigma = 0.004;
			int bitDepth = 10;
			uint64_t seed = 20261126ull;

			std::vector<BallSpec> balls;
			PlayerBoxSpec player;
			OverlaySpec overlay;
			std::vector<ClutterParticle> clutter;
			CameraModel::Rect waterRegion;
		};

		inline SynthSequence RenderSequence(const SceneRecipe& rec)
		{
			SynthSequence seq;
			seq.srcW = rec.srcW;
			seq.srcH = rec.srcH;
			seq.fps = rec.fps;
			seq.shutterFraction = rec.shutterFraction;
			seq.grainSigma = rec.grainSigma;
			seq.bitDepth = rec.bitDepth;

			const int w = rec.srcW;
			const int h = rec.srcH;
			const int hw = w / 2;
			const int hh = h / 2;
			const double dt = 1.0 / rec.fps;
			const bool isPureTranslationPan = (std::fabs(rec.camZoomRatePerFrame) <= 1e-12);

			const double maxPanX = std::fabs(rec.camPanVxPxFrame) * static_cast<double>(rec.numFrames + 4) + 96.0;
			const double maxPanY = std::fabs(rec.camPanVyPxFrame) * static_cast<double>(rec.numFrames + 4) + 96.0;
			const int worldW = w + static_cast<int>(2.0 * maxPanX);
			const int worldH = h + static_cast<int>(2.0 * maxPanY);
			WorldBackdrop backdrop;
			backdrop.Build(
				worldW, worldH, -maxPanX, -maxPanY,
				rec.surface, rec.seed ^ 0xA5A5A5A5ull, rec.isoLumaValue,
				isPureTranslationPan,
				rec.camPanVxPxFrame * rec.shutterFraction,
				rec.camPanVyPxFrame * rec.shutterFraction);

			CameraModel::Rect pic;
			pic.left = 0;
			pic.top = 0;
			pic.right = w;
			pic.bottom = h;

			auto camAtTime = [&](double tSec) -> CameraModel::Params
			{
				const double f = tSec * rec.fps;
				const double panX = rec.camPanVxPxFrame * f;
				const double panY = rec.camPanVyPxFrame * f;
				const double zoom = isPureTranslationPan ? 1.0 : std::pow(1.0 + rec.camZoomRatePerFrame, f);
				return MakePanZoomHomography(panX, panY, zoom);
			};

			std::vector<float> fullCb(static_cast<size_t>(w) * static_cast<size_t>(h));
			std::vector<float> fullCr(static_cast<size_t>(w) * static_cast<size_t>(h));

			seq.frames.resize(static_cast<size_t>(rec.numFrames));
			const double quantLevels = static_cast<double>((1u << std::min(std::max(rec.bitDepth, 8), 16)) - 1u);
			const double invQuantLevels = 1.0 / quantLevels;

			for (int fi = 0; fi < rec.numFrames; ++fi)
			{
				SynthFrame& fr = seq.frames[static_cast<size_t>(fi)];
				fr.frameIndex = static_cast<uint64_t>(fi);
				fr.timestampSec = static_cast<double>(fi) * dt;
				fr.dtFromPrevSec = dt;
				fr.srcW = w;
				fr.srcH = h;
				fr.halfW = hw;
				fr.halfH = hh;
				fr.picture = pic;
				fr.camSigmaPx = rec.camReportedSigmaPx;

				const CameraModel::Params hCurTrue = camAtTime(fr.timestampSec);
				fr.camWorldToImg = hCurTrue;
				const CameraModel::Params hPrevTrue = camAtTime(static_cast<double>(fi - 1) * dt);
				fr.camPrevToCur = FastObjects::ComposeHomography(hCurTrue, FastObjects::InvertHomography(hPrevTrue));
				if (rec.camEstJitterPx > 0.0)
				{
					DetRng jrng(rec.seed ^ (static_cast<uint64_t>(fi + 101) * 0x517CC1B727220A95ull));
					fr.camPrevToCur.h[2] += jrng.Normal01() * rec.camEstJitterPx;
					fr.camPrevToCur.h[5] += jrng.Normal01() * rec.camEstJitterPx;
				}
				fr.camPrevUsable = true;

				const size_t nFull = static_cast<size_t>(w) * static_cast<size_t>(h);
				fr.fullY.assign(nFull, 0.0f);
				fullCb.assign(nFull, 0.0f);
				fullCr.assign(nFull, 0.0f);
				fr.gtBallAlpha.assign(nFull, 0.0f);

				// Only allocate full-res gtFlowX/Y on frames that have visible balls (saves memory/time on clutter scenes).
				const bool hasTrueBalls = !rec.balls.empty();
				if (hasTrueBalls)
				{
					fr.gtFlowX.assign(nFull, 0.0f);
					fr.gtFlowY.assign(nFull, 0.0f);
				}

				struct SubBallState
				{
					bool active = false;
					double ix = 0.0, iy = 0.0, r = 0.0;
				};
				CameraModel::Params subInvCam[kSubframes];
				std::vector<SubBallState> subBalls(static_cast<size_t>(kSubframes) * rec.balls.size());
				double subPlayerBox[kSubframes][4] = {};
				bool subPlayerOn[kSubframes] = {};
				double subTime[kSubframes] = {};

				for (int s = 0; s < kSubframes; ++s)
				{
					const double u = (static_cast<double>(s) + 0.5) / static_cast<double>(kSubframes) - 0.5;
					const double tSub = fr.timestampSec + u * rec.shutterFraction * dt;
					subTime[s] = tSub;
					const CameraModel::Params hSub = camAtTime(tSub);
					subInvCam[s] = FastObjects::InvertHomography(hSub);

					for (size_t bi = 0; bi < rec.balls.size(); ++bi)
					{
						const BallSpec& bs = rec.balls[bi];
						SubBallState& sb = subBalls[static_cast<size_t>(s) * rec.balls.size() + bi];
						if (tSub < bs.tStartSec || tSub > bs.tEndSec)
							continue;
						double wx = 0.0, wy = 0.0, rad = 0.0, spd = 0.0;
						bs.EvalWorldAt(tSub, wx, wy, rad, spd);
						WorldToImage(hSub, pic, wx, wy, sb.ix, sb.iy);
						sb.active = true;
						sb.r = rad;
					}

					if (rec.player.enabled)
					{
						double wl = 0.0, wt = 0.0, wr = 0.0, wb = 0.0;
						rec.player.BoundsAt(tSub, wl, wt, wr, wb);
						double il = 0.0, it = 0.0, ir = 0.0, ib = 0.0;
						WorldToImage(hSub, pic, wl, wt, il, it);
						WorldToImage(hSub, pic, wr, wb, ir, ib);
						subPlayerBox[s][0] = std::fmin(il, ir);
						subPlayerBox[s][1] = std::fmin(it, ib);
						subPlayerBox[s][2] = std::fmax(il, ir);
						subPlayerBox[s][3] = std::fmax(it, ib);
						subPlayerOn[s] = true;
					}
				}

				struct SubClutterState
				{
					bool active = false;
					double ix = 0.0, iy = 0.0, r = 0.0;
					float y = 0.0f, cb = 0.0f, cr = 0.0f;
				};
				std::vector<SubClutterState> subClutter(static_cast<size_t>(kSubframes) * rec.clutter.size());
				for (int s = 0; s < kSubframes; ++s)
				{
					const CameraModel::Params hSub = camAtTime(subTime[s]);
					for (size_t ci = 0; ci < rec.clutter.size(); ++ci)
					{
						const ClutterParticle& cp = rec.clutter[ci];
						SubClutterState& sc = subClutter[static_cast<size_t>(s) * rec.clutter.size() + ci];
						double px = 0.0, py = 0.0;
						bool vis = false;
						cp.EvalPos(fi, subTime[s], px, py, vis);
						if (!vis)
							continue;
						if (cp.screenFixed)
						{
							sc.ix = px;
							sc.iy = py;
						}
						else
						{
							WorldToImage(hSub, pic, px, py, sc.ix, sc.iy);
						}
						sc.active = true;
						sc.r = cp.radiusPx;
						sc.y = cp.EvalLuma(fi);
						sc.cb = cp.cb;
						sc.cr = cp.cr;
					}
				}

				// Build an exact 16x16 source-pixel foreground occupancy grid so scattered clutter/balls only run
				// the 16x painter's loop on tiles actually touched by a foreground element (100% bit-identical pixels!).
				constexpr int kTile = 16;
				const int tileW = (w + kTile - 1) / kTile;
				const int tileH = (h + kTile - 1) / kTile;
				std::vector<uint8_t> fgTile(static_cast<size_t>(tileW) * static_cast<size_t>(tileH), 0u);
				auto markFgBox = [&](double x0, double y0, double x1, double y1)
				{
					const int tx0 = std::max(0, (static_cast<int>(std::floor(x0)) - 2) / kTile);
					const int ty0 = std::max(0, (static_cast<int>(std::floor(y0)) - 2) / kTile);
					const int tx1 = std::min(tileW - 1, (static_cast<int>(std::ceil(x1)) + 2) / kTile);
					const int ty1 = std::min(tileH - 1, (static_cast<int>(std::ceil(y1)) + 2) / kTile);
					for (int ty = ty0; ty <= ty1; ++ty)
						for (int tx = tx0; tx <= tx1; ++tx)
							fgTile[static_cast<size_t>(ty) * static_cast<size_t>(tileW) + static_cast<size_t>(tx)] = 1u;
				};
				for (const auto& sb : subBalls)
					if (sb.active) markFgBox(sb.ix - sb.r, sb.iy - sb.r, sb.ix + sb.r, sb.iy + sb.r);
				for (int s = 0; s < kSubframes; ++s)
					if (subPlayerOn[s]) markFgBox(subPlayerBox[s][0], subPlayerBox[s][1], subPlayerBox[s][2], subPlayerBox[s][3]);
				for (const auto& sc : subClutter)
					if (sc.active) markFgBox(sc.ix - sc.r, sc.iy - sc.r, sc.ix + sc.r, sc.iy + sc.r);
				if (rec.overlay.enabled && !rec.overlay.box.Empty())
					markFgBox(rec.overlay.box.left, rec.overlay.box.top, rec.overlay.box.right, rec.overlay.box.bottom);

				const CameraModel::Params hNextTrue = camAtTime(static_cast<double>(fi + 1) * dt);
				const CameraModel::Params hCurToNextTrue = FastObjects::ComposeHomography(hNextTrue, FastObjects::InvertHomography(hCurTrue));
				const double picCx = pic.CentreX();
				const double picCy = pic.CentreY();
				constexpr float kInvSub = 1.0f / static_cast<float>(kSubframes);
				const double framePanX = rec.camPanVxPxFrame * static_cast<double>(fi);
				const double framePanY = rec.camPanVyPxFrame * static_cast<double>(fi);

				for (int py = 0; py < h; ++py)
				{
					const double sy = static_cast<double>(py) + 0.5;
					const size_t tileRow = static_cast<size_t>(py / kTile) * static_cast<size_t>(tileW);
					for (int px = 0; px < w; ++px)
					{
						const double sx = static_cast<double>(px) + 0.5;
						const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(w) + static_cast<size_t>(px);

						if (hasTrueBalls)
						{
							double camDx = 0.0, camDy = 0.0;
							CameraModel::Displacement(hCurToNextTrue, sx - picCx, sy - picCy, &camDx, &camDy);
							fr.gtFlowX[idx] = static_cast<float>(camDx);
							fr.gtFlowY[idx] = static_cast<float>(camDy);
						}

						const bool pxHasFg = (fgTile[tileRow + static_cast<size_t>(px / kTile)] != 0u);
						if (!pxHasFg && backdrop.hasPanBlur)
						{
							backdrop.SamplePanBlurred(sx + framePanX, sy + framePanY, fr.fullY[idx], fullCb[idx], fullCr[idx]);
							continue;
						}

						float accY = 0.0f, accCb = 0.0f, accCr = 0.0f;
						for (int s = 0; s < kSubframes; ++s)
						{
							double bwx = 0.0, bwy = 0.0;
							CameraModel::Apply(subInvCam[s], sx - picCx, sy - picCy, &bwx, &bwy);
							bwx += picCx;
							bwy += picCy;

							float sY = 0.0f, sCb = 0.0f, sCr = 0.0f;
							backdrop.SampleRawBilinear(bwx, bwy, sY, sCb, sCr);

							if (pxHasFg)
							{
								for (size_t ci = 0; ci < rec.clutter.size(); ++ci)
								{
									const SubClutterState& sc = subClutter[static_cast<size_t>(s) * rec.clutter.size() + ci];
									if (!sc.active)
										continue;
									const double cdx = sx - sc.ix;
									const double cdy = sy - sc.iy;
									const double rLim = sc.r + 0.5;
									if (std::fabs(cdx) >= rLim || std::fabs(cdy) >= rLim)
										continue;
									const double dist = std::hypot(cdx, cdy);
									const float a = static_cast<float>(std::fmin(1.0, std::fmax(0.0, rLim - dist)));
									if (a > 0.0f)
									{
										sY = sY * (1.0f - a) + sc.y * a;
										sCb = sCb * (1.0f - a) + sc.cb * a;
										sCr = sCr * (1.0f - a) + sc.cr * a;
									}
								}

								for (size_t bi = 0; bi < rec.balls.size(); ++bi)
								{
									const SubBallState& sb = subBalls[static_cast<size_t>(s) * rec.balls.size() + bi];
									if (!sb.active)
										continue;
									const double dx = sx - sb.ix;
									const double dy = sy - sb.iy;
									const double rLim = sb.r + 0.5;
									if (std::fabs(dx) >= rLim || std::fabs(dy) >= rLim)
										continue;
									const double dist = std::hypot(dx, dy);
									const float a = static_cast<float>(std::fmin(1.0, std::fmax(0.0, rLim - dist)));
									if (a > 0.0f)
									{
										const BallSpec& bs = rec.balls[bi];
										float bY = bs.baseY;
										if (bs.textured)
										{
											const double nx = dx / std::fmax(sb.r, 1.0);
											const double ny = dy / std::fmax(sb.r, 1.0);
											const double seam = 0.14 * std::cos(3.2 * nx + 1.1) * std::sin(3.2 * ny - 0.7)
												- 0.08 * (nx * nx + ny * ny);
											bY = static_cast<float>(std::fmin(1.0, std::fmax(0.0, static_cast<double>(bs.baseY) + seam)));
										}
										sY = sY * (1.0f - a) + bY * a;
										sCb = sCb * (1.0f - a) + bs.baseCb * a;
										sCr = sCr * (1.0f - a) + bs.baseCr * a;
									}
								}

								if (subPlayerOn[s] &&
									sx >= subPlayerBox[s][0] && sx < subPlayerBox[s][2] &&
									sy >= subPlayerBox[s][1] && sy < subPlayerBox[s][3])
								{
									const float jerseyStripe = 0.04f * static_cast<float>(std::sin(sx * 0.35));
									sY = rec.player.luma + jerseyStripe;
									sCb = rec.player.cb;
									sCr = rec.player.cr;
								}

								if (rec.overlay.enabled &&
									px >= rec.overlay.box.left && px < rec.overlay.box.right &&
									py >= rec.overlay.box.top && py < rec.overlay.box.bottom)
								{
									const double scrollX = sx + rec.overlay.scrollSpeedPxSec * subTime[s];
									const bool glyph = (std::sin(scrollX * 0.55) * std::cos(sy * 0.65) > 0.25);
									const float blink = (!rec.overlay.blinking || ((fi & 1) == 0)) ? 1.0f : 0.25f;
									sY = glyph ? (0.92f * blink) : 0.12f;
									sCb = 0.0f;
									sCr = 0.0f;
								}
							}

							accY += sY;
							accCb += sCb;
							accCr += sCr;
						}

						fr.fullY[idx] = accY * kInvSub;
						fullCb[idx] = accCb * kInvSub;
						fullCr[idx] = accCr * kInvSub;
					}
				}

				double plL = 0.0, plT = 0.0, plR = 0.0, plB = 0.0;
				if (rec.player.enabled)
				{
					double wl = 0.0, wt = 0.0, wr = 0.0, wb = 0.0;
					rec.player.BoundsAt(fr.timestampSec, wl, wt, wr, wb);
					double il = 0.0, it = 0.0, ir = 0.0, ib = 0.0;
					WorldToImage(hCurTrue, pic, wl, wt, il, it);
					WorldToImage(hCurTrue, pic, wr, wb, ir, ib);
					plL = std::fmin(il, ir);
					plT = std::fmin(it, ib);
					plR = std::fmax(il, ir);
					plB = std::fmax(it, ib);
				}

				for (size_t bi = 0; bi < rec.balls.size(); ++bi)
				{
					const BallSpec& bs = rec.balls[bi];
					if (fr.timestampSec < bs.tStartSec || fr.timestampSec > bs.tEndSec)
						continue;

					double wx0 = 0.0, wy0 = 0.0, rad0 = 0.0, spd0 = 0.0;
					double wx1 = 0.0, wy1 = 0.0, rad1 = 0.0, spd1 = 0.0;
					bs.EvalWorldAt(fr.timestampSec, wx0, wy0, rad0, spd0);
					bs.EvalWorldAt(fr.timestampSec + dt, wx1, wy1, rad1, spd1);

					double ix0 = 0.0, iy0 = 0.0, ix1 = 0.0, iy1 = 0.0;
					WorldToImage(hCurTrue, pic, wx0, wy0, ix0, iy0);
					WorldToImage(hNextTrue, pic, wx1, wy1, ix1, iy1);

					GroundTruthBall gt;
					gt.id = bs.id;
					gt.expectedType = bs.expectedType;
					gt.cx = static_cast<float>(ix0);
					gt.cy = static_cast<float>(iy0);
					gt.vx = static_cast<float>(ix1 - ix0);
					gt.vy = static_cast<float>(iy1 - iy0);
					gt.radiusPx = static_cast<float>(rad0);
					gt.streakLenPx = static_cast<float>(spd0 * dt * rec.shutterFraction);

					if (rec.player.enabled && ix0 >= plL && ix0 <= plR && iy0 >= plT && iy0 <= plB)
					{
						gt.occludedByPlayer = true;
						gt.visible = false;
					}

					if (bs.hasBounce && bs.tBounceSec > fr.timestampSec && bs.tBounceSec < fr.timestampSec + dt)
					{
						gt.pairBracketsImpulse = true;
						double bwx = 0.0, bwy = 0.0, br = 0.0, bspd = 0.0;
						bs.EvalWorldAt(bs.tBounceSec, bwx, bwy, br, bspd);
						const CameraModel::Params hBounce = camAtTime(bs.tBounceSec);
						double bix = 0.0, biy = 0.0;
						WorldToImage(hBounce, pic, bwx, bwy, bix, biy);
						gt.impulseImgX = static_cast<float>(bix);
						gt.impulseImgY = static_cast<float>(biy);
					}

					fr.gtBalls.push_back(gt);

					if (gt.visible && hasTrueBalls)
					{
						const int x0 = std::max(0, static_cast<int>(std::floor(ix0 - rad0 - 1.0)));
						const int x1 = std::min(w, static_cast<int>(std::ceil(ix0 + rad0 + 1.0)));
						const int y0 = std::max(0, static_cast<int>(std::floor(iy0 - rad0 - 1.0)));
						const int y1 = std::min(h, static_cast<int>(std::ceil(iy0 + rad0 + 1.0)));
						for (int y = y0; y < y1; ++y)
						{
							for (int x = x0; x < x1; ++x)
							{
								if (rec.player.enabled && (x + 0.5) >= plL && (x + 0.5) < plR && (y + 0.5) >= plT && (y + 0.5) < plB)
									continue;
								const double d = std::hypot((x + 0.5) - ix0, (y + 0.5) - iy0);
								const float a = static_cast<float>(std::fmin(1.0, std::fmax(0.0, rad0 + 0.5 - d)));
								const size_t pidx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
								if (a > fr.gtBallAlpha[pidx])
								{
									fr.gtBallAlpha[pidx] = a;
									if (a >= 0.5f)
									{
										fr.gtFlowX[pidx] = gt.vx;
										fr.gtFlowY[pidx] = gt.vy;
									}
								}
							}
						}
					}
				}

				for (size_t i = 0; i < fr.gtBalls.size(); ++i)
				{
					for (size_t j = i + 1; j < fr.gtBalls.size(); ++j)
					{
						const double sep = std::hypot(
							fr.gtBalls[i].cx - fr.gtBalls[j].cx,
							fr.gtBalls[i].cy - fr.gtBalls[j].cy);
						if (sep <= std::fmax(fr.gtBalls[i].radiusPx, fr.gtBalls[j].radiusPx))
						{
							fr.gtBalls[i].crossingAmbiguous = true;
							fr.gtBalls[j].crossingAmbiguous = true;
						}
					}
				}

				DetRng noiseRng(rec.seed ^ (static_cast<uint64_t>(fi + 1) * 0xD1B54A32D192ED03ull));
				if (rec.grainSigma > 0.0 || rec.bitDepth > 0)
				{
					for (size_t i = 0; i < nFull; ++i)
					{
						double v = static_cast<double>(fr.fullY[i]);
						if (rec.grainSigma > 0.0)
							v += noiseRng.Normal01() * rec.grainSigma;
						v = std::fmin(1.0, std::fmax(0.0, v));
						v = std::round(v * quantLevels) * invQuantLevels;
						fr.fullY[i] = static_cast<float>(v);
					}
				}

				const size_t nHalf = static_cast<size_t>(hw) * static_cast<size_t>(hh);
				fr.halfY.resize(nHalf);
				fr.halfCb.resize(nHalf);
				fr.halfCr.resize(nHalf);
				for (int hy = 0; hy < hh; ++hy)
				{
					const size_t r0 = static_cast<size_t>(hy * 2) * static_cast<size_t>(w);
					const size_t r1 = static_cast<size_t>(hy * 2 + 1) * static_cast<size_t>(w);
					for (int hx = 0; hx < hw; ++hx)
					{
						const size_t c0 = static_cast<size_t>(hx * 2);
						const size_t hidx = static_cast<size_t>(hy) * static_cast<size_t>(hw) + static_cast<size_t>(hx);
						fr.halfY[hidx] = 0.25f * (fr.fullY[r0 + c0] + fr.fullY[r0 + c0 + 1] + fr.fullY[r1 + c0] + fr.fullY[r1 + c0 + 1]);

						double mCb = 0.25 * (fullCb[r0 + c0] + fullCb[r0 + c0 + 1] + fullCb[r1 + c0] + fullCb[r1 + c0 + 1]);
						double mCr = 0.25 * (fullCr[r0 + c0] + fullCr[r0 + c0 + 1] + fullCr[r1 + c0] + fullCr[r1 + c0 + 1]);
						if (rec.grainSigma > 0.0)
						{
							mCb += noiseRng.Normal01() * (0.5 * rec.grainSigma);
							mCr += noiseRng.Normal01() * (0.5 * rec.grainSigma);
						}
						mCb = std::round((std::fmin(0.5, std::fmax(-0.5, mCb)) + 0.5) * quantLevels) * invQuantLevels - 0.5;
						mCr = std::round((std::fmin(0.5, std::fmax(-0.5, mCr)) + 0.5) * quantLevels) * invQuantLevels - 0.5;
						fr.halfCb[hidx] = static_cast<float>(mCb);
						fr.halfCr[hidx] = static_cast<float>(mCr);
					}
				}

				fr.guardBlockSrcPx = 16.0;
				fr.guardGridW = (w + 15) / 16;
				fr.guardGridH = (h + 15) / 16;
				const size_t gCount = static_cast<size_t>(fr.guardGridW) * static_cast<size_t>(fr.guardGridH);
				fr.blockLabels.assign(gCount, static_cast<uint8_t>(CameraModel::Label::Camera));
				fr.blockExcluded.assign(gCount, 0u);
				fr.waterProb.assign(gCount, 0.0f);
				const float halfGrainVar = static_cast<float>(0.25 * rec.grainSigma * rec.grainSigma);
				fr.grainVarMap.assign(gCount, halfGrainVar);

				for (int by = 0; by < fr.guardGridH; ++by)
				{
					const double cy = (static_cast<double>(by) + 0.5) * 16.0;
					for (int bx = 0; bx < fr.guardGridW; ++bx)
					{
						const double cx = (static_cast<double>(bx) + 0.5) * 16.0;
						const size_t gidx = static_cast<size_t>(by) * static_cast<size_t>(fr.guardGridW) + static_cast<size_t>(bx);

						if (rec.player.enabled && cx >= plL - 4.0 && cx <= plR + 4.0 && cy >= plT - 4.0 && cy <= plB + 4.0)
							fr.blockLabels[gidx] = static_cast<uint8_t>(CameraModel::Label::Object);

						if (!rec.waterRegion.Empty() &&
							cx >= rec.waterRegion.left && cx < rec.waterRegion.right &&
							cy >= rec.waterRegion.top && cy < rec.waterRegion.bottom)
						{
							fr.waterProb[gidx] = 0.85f;
						}

						if (rec.overlay.enabled && rec.overlay.markInP8 &&
							cx >= rec.overlay.box.left - 4.0 && cx < rec.overlay.box.right + 4.0 &&
							cy >= rec.overlay.box.top - 4.0 && cy < rec.overlay.box.bottom + 4.0)
						{
							fr.blockLabels[gidx] = static_cast<uint8_t>(CameraModel::Label::Overlay);
							fr.blockExcluded[gidx] = 1u;
						}
					}
				}
			}

			return seq;
		}

		inline FastObjects::ResidualInput BuildBidirectionalInput(const SynthSequence& seq, int fi, bool useGrainMap = true)
		{
			const SynthFrame& prev = seq.frames[static_cast<size_t>(fi - 1)];
			const SynthFrame& cur = seq.frames[static_cast<size_t>(fi)];
			const SynthFrame& next = seq.frames[static_cast<size_t>(fi + 1)];

			FastObjects::ResidualInput in;
			in.lumaPrev = prev.halfY.data();
			in.lumaCur = cur.halfY.data();
			in.lumaNext = next.halfY.data();
			in.width = cur.halfW;
			in.height = cur.halfH;
			in.sourceScale = 2.0f;

			in.cbPrev = prev.halfCb.data();
			in.cbCur = cur.halfCb.data();
			in.cbNext = next.halfCb.data();
			in.crPrev = prev.halfCr.data();
			in.crCur = cur.halfCr.data();
			in.crNext = next.halfCr.data();

			in.camPrevToCur = cur.camPrevToCur;
			in.camCurToNext = next.camPrevToCur;
			in.camPrevUsable = cur.camPrevUsable;
			in.camNextUsable = next.camPrevUsable;
			in.camSigmaPx = cur.camSigmaPx;
			in.picture = cur.picture;

			if (useGrainMap)
			{
				in.grainVarMap = cur.grainVarMap.data();
				in.grainGridW = cur.guardGridW;
				in.grainGridH = cur.guardGridH;
				in.grainBlockSrcPx = cur.guardBlockSrcPx;
			}

			in.blockLabels = cur.blockLabels.data();
			in.blockExcluded = cur.blockExcluded.data();
			in.waterProb = cur.waterProb.data();
			in.guardGridW = cur.guardGridW;
			in.guardGridH = cur.guardGridH;
			in.guardBlockSrcPx = cur.guardBlockSrcPx;
			return in;
		}

		inline FastObjects::ResidualInput BuildCausalTwoBackInput(const SynthSequence& seq, int fi)
		{
			const SynthFrame& prev2 = seq.frames[static_cast<size_t>(fi - 2)];
			const SynthFrame& prev1 = seq.frames[static_cast<size_t>(fi - 1)];
			const SynthFrame& cur = seq.frames[static_cast<size_t>(fi)];

			FastObjects::ResidualInput in = BuildBidirectionalInput(seq, fi - 1);
			in.lumaPrev = prev1.halfY.data();
			in.lumaCur = cur.halfY.data();
			in.lumaNext = prev2.halfY.data();
			in.cbPrev = prev1.halfCb.data();
			in.cbCur = cur.halfCb.data();
			in.cbNext = prev2.halfCb.data();
			in.crPrev = prev1.halfCr.data();
			in.crCur = cur.halfCr.data();
			in.crNext = prev2.halfCr.data();

			in.camPrevToCur = cur.camPrevToCur;
			in.camCurToNext = prev1.camPrevToCur;
			in.camPrevUsable = cur.camPrevUsable;
			in.camNextUsable = prev1.camPrevUsable;
			in.camSigmaPx = cur.camSigmaPx;
			in.picture = cur.picture;
			in.causalTwoBack = true;

			in.grainVarMap = cur.grainVarMap.data();
			in.blockLabels = cur.blockLabels.data();
			in.blockExcluded = cur.blockExcluded.data();
			in.waterProb = cur.waterProb.data();
			return in;
		}

		inline FastObjects::TrackerFrameInput BuildTrackerFrameInput(const SynthSequence& seq, int fi, bool useGrainMap = true)
		{
			FastObjects::TrackerFrameInput tfi;
			const SynthFrame& cur = seq.frames[static_cast<size_t>(fi)];
			tfi.frameIndex = static_cast<uint64_t>(fi - 1);
			tfi.timestampSec = cur.timestampSec;
			tfi.dtFromPrevSec = cur.dtFromPrevSec;
			tfi.resyncGeneration = 1ull;
			tfi.residual = BuildBidirectionalInput(seq, fi, useGrainMap);
			return tfi;
		}

		inline FastObjects::TrackerRefinePlanes BuildRefinePlanes(const SynthSequence& seq, int fi)
		{
			const SynthFrame& cur = seq.frames[static_cast<size_t>(fi)];
			const SynthFrame& next = seq.frames[static_cast<size_t>(fi + 1)];
			FastObjects::TrackerRefinePlanes rp;
			rp.lumaA = cur.fullY.data();
			rp.lumaB = next.fullY.data();
			rp.width = cur.srcW;
			rp.height = cur.srcH;
			rp.planeScale = 1.0f;
			rp.camAToB = next.camPrevToCur;
			rp.camUsable = next.camPrevUsable;
			rp.picture = cur.picture;
			return rp;
		}

		// 1. Single-Pair 2-Frame Residual Negative Control:
		inline uint32_t CountTwoFrameSinglePairResidualBlobs(
			const FastObjects::Config& cfg,
			const FastObjects::ResidualInput& in)
		{
			const int w = in.width;
			const int h = in.height;
			const double scale = static_cast<double>(in.sourceScale);
			CameraModel::Rect pic = in.picture;
			if (pic.Empty())
			{
				pic.left = 0; pic.top = 0;
				pic.right = static_cast<int>(w * scale);
				pic.bottom = static_cast<int>(h * scale);
			}
			const double picCx = pic.CentreX();
			const double picCy = pic.CentreY();
			const CameraModel::Params mapToPrev = FastObjects::InvertHomography(in.camPrevToCur);
			const double sigmaQ = cfg.QuantizationSigma();
			const double twoSigmaQ2 = 2.0 * sigmaQ * sigmaQ;
			const double sigmaHSamp = std::fmax(0.0, in.camSigmaPx) / scale;

			std::vector<uint8_t> mask(static_cast<size_t>(w) * static_cast<size_t>(h), 0u);
			for (int hy = 1; hy < h - 1; ++hy)
			{
				const double sy = (hy + 0.5) * scale;
				for (int hx = 1; hx < w - 1; ++hx)
				{
					const double sx = (hx + 0.5) * scale;
					double pxC = 0.0, pyC = 0.0;
					CameraModel::Apply(mapToPrev, sx - picCx, sy - picCy, &pxC, &pyC);
					const double rpx = (pxC + picCx) / scale - 0.5;
					const double rpy = (pyC + picCy) / scale - 0.5;
					if (rpx < 0.0 || rpx >= w - 1 || rpy < 0.0 || rpy >= h - 1)
						continue;

					const int x0 = static_cast<int>(std::floor(rpx));
					const int y0 = static_cast<int>(std::floor(rpy));
					const double fx = rpx - x0, fy = rpy - y0;
					const double p00 = in.lumaPrev[static_cast<size_t>(y0) * w + x0];
					const double p10 = in.lumaPrev[static_cast<size_t>(y0) * w + x0 + 1];
					const double p01 = in.lumaPrev[static_cast<size_t>(y0 + 1) * w + x0];
					const double p11 = in.lumaPrev[static_cast<size_t>(y0 + 1) * w + x0 + 1];
					const double prevY = (p00 * (1.0 - fx) + p10 * fx) * (1.0 - fy) + (p01 * (1.0 - fx) + p11 * fx) * fy;
					const size_t idx = static_cast<size_t>(hy) * w + hx;
					const double dm = std::fabs(static_cast<double>(in.lumaCur[idx]) - prevY);

					double gv = 0.0001;
					if (in.grainVarMap && in.grainGridW > 0 && in.grainGridH > 0)
					{
						const int gbx = std::min(std::max(static_cast<int>(sx / in.grainBlockSrcPx), 0), in.grainGridW - 1);
						const int gby = std::min(std::max(static_cast<int>(sy / in.grainBlockSrcPx), 0), in.grainGridH - 1);
						gv = in.grainVarMap[static_cast<size_t>(gby) * in.grainGridW + gbx];
					}
					const double gx = 0.5 * (in.lumaCur[idx + 1] - in.lumaCur[idx - 1]);
					const double gy = 0.5 * (in.lumaCur[idx + w] - in.lumaCur[idx - w]);
					const double reg = sigmaHSamp * std::hypot(gx, gy);
					const double sig = std::sqrt(2.0 * gv + twoSigmaQ2 + reg * reg);
					const double z = dm / std::fmax(sig, 1e-9);
					if (z >= cfg.seedSigma)
						mask[idx] = 2u;
					else if (z >= cfg.growSigma)
						mask[idx] = 1u;
				}
			}

			std::vector<uint8_t> vis(mask.size(), 0u);
			std::vector<int> q;
			uint32_t blobs = 0;
			const int dx8[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
			const int dy8[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
			for (int y = 1; y < h - 1; ++y)
			{
				for (int x = 1; x < w - 1; ++x)
				{
					const size_t sidx = static_cast<size_t>(y) * w + x;
					if (vis[sidx] || mask[sidx] < 2u)
						continue;
					q.clear();
					vis[sidx] = 1u;
					q.push_back(static_cast<int>(sidx));
					size_t head = 0;
					while (head < q.size())
					{
						const int cur = q[head++];
						const int cx = cur % w, cy = cur / w;
						for (int k = 0; k < 8; ++k)
						{
							const int nx = cx + dx8[k], ny = cy + dy8[k];
							if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
							const size_t nidx = static_cast<size_t>(ny) * w + nx;
							if (!vis[nidx] && mask[nidx] >= 1u)
							{
								vis[nidx] = 1u;
								q.push_back(static_cast<int>(nidx));
							}
						}
					}
					if (static_cast<double>(q.size()) >= cfg.minBlobAreaHalfPx)
						++blobs;
				}
			}
			return blobs;
		}

		// 2. Unprotected Naive Full-Range 16x16 SAD Block Matcher (with 4 px context collar matching VP's 24x24
		// support window in DejudderWarp / CameraGuidanceGpu.h:117, Spec s1.4 #1):
		inline double MeasureNaive16x16BlockMatcherBallEpe(
			const SynthFrame& frameA,
			const SynthFrame& frameB,
			int searchRangePx,
			double* outVelocityAtCentroidEpe = nullptr)
		{
			const int w = frameA.srcW;
			const int h = frameA.srcH;
			constexpr int kBlock = 16;
			constexpr int kMargin = 4; // Standard 2 half-px (= 4 source px) matcher support margin (Spec s1.4 #1)

			auto evalBlockVector = [&](int bx0, int by0, float& outVx, float& outVy)
			{
				const int sx0 = std::max(0, bx0 - kMargin);
				const int sy0 = std::max(0, by0 - kMargin);
				const int sx1 = std::min(w, bx0 + kBlock + kMargin);
				const int sy1 = std::min(h, by0 + kBlock + kMargin);
				const int sw = sx1 - sx0;
				const int sh = sy1 - sy0;

				double bestSad = 1e30;
				int bestDx = 0, bestDy = 0;
				for (int dy = -searchRangePx; dy <= searchRangePx; ++dy)
				{
					if (sy0 + dy < 0 || sy0 + sh + dy > h)
						continue;
					for (int dx = -searchRangePx; dx <= searchRangePx; ++dx)
					{
						if (sx0 + dx < 0 || sx0 + sw + dx > w)
							continue;
						double sad = 0.0;
						for (int y = 0; y < sh; ++y)
						{
							const float* rA = &frameA.fullY[static_cast<size_t>(sy0 + y) * w + sx0];
							const float* rB = &frameB.fullY[static_cast<size_t>(sy0 + y + dy) * w + (sx0 + dx)];
							for (int x = 0; x < sw; ++x)
								sad += std::fabs(static_cast<double>(rA[x]) - static_cast<double>(rB[x]));
						}
						if (sad < bestSad)
						{
							bestSad = sad;
							bestDx = dx;
							bestDy = dy;
						}
					}
				}
				outVx = static_cast<float>(bestDx);
				outVy = static_cast<float>(bestDy);
			};

			double sumEpe = 0.0;
			size_t ballPixels = 0;
			const int gw = w / kBlock;
			const int gh = h / kBlock;
			for (int gby = 0; gby < gh; ++gby)
			{
				for (int gbx = 0; gbx < gw; ++gbx)
				{
					const int bx0 = gbx * kBlock;
					const int by0 = gby * kBlock;
					bool hasBallPx = false;
					for (int y = by0; y < by0 + kBlock && !hasBallPx; ++y)
						for (int x = bx0; x < bx0 + kBlock; ++x)
							if (frameA.gtBallAlpha[static_cast<size_t>(y) * w + x] >= 0.5f)
								hasBallPx = true;
					if (!hasBallPx)
						continue;

					float bvx = 0.0f, bvy = 0.0f;
					evalBlockVector(bx0, by0, bvx, bvy);
					for (int y = by0; y < by0 + kBlock; ++y)
					{
						for (int x = bx0; x < bx0 + kBlock; ++x)
						{
							const size_t pidx = static_cast<size_t>(y) * w + x;
							if (frameA.gtBallAlpha[pidx] >= 0.5f)
							{
								sumEpe += std::hypot(bvx - frameA.gtFlowX[pidx], bvy - frameA.gtFlowY[pidx]);
								++ballPixels;
							}
						}
					}
				}
			}

			if (outVelocityAtCentroidEpe && !frameA.gtBalls.empty())
			{
				const GroundTruthBall& gt = frameA.gtBalls[0];
				const int bx0 = std::min(std::max((static_cast<int>(gt.cx) / kBlock) * kBlock, 0), w - kBlock);
				const int by0 = std::min(std::max((static_cast<int>(gt.cy) / kBlock) * kBlock, 0), h - kBlock);
				float cvx = 0.0f, cvy = 0.0f;
				evalBlockVector(bx0, by0, cvx, cvy);
				*outVelocityAtCentroidEpe = std::hypot(cvx - gt.vx, cvy - gt.vy);
			}

			return (ballPixels > 0) ? (sumEpe / static_cast<double>(ballPixels)) : 0.0;
		}

		inline double MeasureTrackedSmallFastPixelEpe(
			const SynthFrame& frameA,
			const SynthFrame& frameB,
			const FastObjects::FastObjectTrackSet& trackSet)
		{
			const int w = frameA.srcW;
			const int h = frameA.srcH;
			const double picCx = frameA.picture.CentreX();
			const double picCy = frameA.picture.CentreY();
			double sumEpe = 0.0;
			size_t ballPixels = 0;

			for (int y = 0; y < h; ++y)
			{
				for (int x = 0; x < w; ++x)
				{
					const size_t pidx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
					if (frameA.gtBallAlpha[pidx] < 0.5f)
						continue;

					const float sx = static_cast<float>(x) + 0.5f;
					const float sy = static_cast<float>(y) + 0.5f;
					float vx = 0.0f, vy = 0.0f;
					bool disocc = false;
					if (!FastObjects::SampleObjectVector(trackSet, sx, sy, 0.0f, vx, vy, disocc))
					{
						double cdx = 0.0, cdy = 0.0;
						CameraModel::Displacement(frameB.camPrevToCur, sx - picCx, sy - picCy, &cdx, &cdy);
						vx = static_cast<float>(cdx);
						vy = static_cast<float>(cdy);
					}
					sumEpe += std::hypot(vx - frameA.gtFlowX[pidx], vy - frameA.gtFlowY[pidx]);
					++ballPixels;
				}
			}
			return (ballPixels > 0) ? (sumEpe / static_cast<double>(ballPixels)) : 0.0;
		}

		// =====================================================================
		// Milestone 0 Standard Scene Builders (1080p Broadcast Physics Scale!)
		// =====================================================================

		// 1. Soccer Parabolic Lob + Camera Pan (60p & 30p): 12 px ball, gravity arc (ay > 0), 6 px/frame pan + grass + crisp white lines.
		inline SynthSequence MakeSoccerParabolicLobScene(double fps = 60.0, int numFrames = 11)
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080; // d_max = 32.4 px, v_max = 3840 px/s
			r.numFrames = numFrames;
			r.fps = fps;
			r.shutterFraction = 0.25;
			r.surface = SurfaceKind::SoccerGrass;
			r.camPanVxPxFrame = 6.0; // Exact spec: 6 px/frame camera pan!
			r.camPanVyPxFrame = 0.0;
			r.grainSigma = 0.004;
			r.seed = 1001ull + static_cast<uint64_t>(fps);

			BallSpec b;
			b.id = 1;
			b.expectedType = FastObjects::TrajectoryType::Ballistic;
			b.radiusPx = 6.0; // 12 px diameter soccer ball
			b.x0 = (fps >= 50.0) ? 420.0 : 360.0;
			b.y0 = 560.0;
			b.vx0 = (fps >= 50.0) ? 1860.0 : 1380.0;
			b.vy0 = (fps >= 50.0) ? -780.0 : -450.0;
			b.ax = 0.0;
			b.ay = (fps >= 50.0) ? 4500.0 : 1900.0; // Downward gravity arc
			b.baseY = 0.92f;
			b.textured = true;
			r.balls.push_back(b);
			return RenderSequence(r);
		}

		// 2. Tennis Serve + Court Bounce + Motion Blur (60p): 7 px ball at 56 px/frame, 180-deg shutter (L = 28 px), sharp court bounce!
		inline SynthSequence MakeTennisServeBounceScene(int numFrames = 11)
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.5; // Mandatory 180-deg shutter -> L = 28 px streak!
			r.surface = SurfaceKind::TennisCourt;
			r.camPanVxPxFrame = 4.0;
			r.camPanVyPxFrame = 0.0;
			r.grainSigma = 0.004;
			r.seed = 2002ull;

			BallSpec b;
			b.id = 1;
			b.expectedType = FastObjects::TrajectoryType::Impulse;
			b.radiusPx = 3.5; // 7 px diameter tennis ball
			b.x0 = 480.0;
			b.y0 = 468.0;
			b.vx0 = 2640.0;   // 44 px/frame X
			b.vy0 = 2040.0;   // 34 px/frame Y -> speed = 55.6 px/frame (3336 px/s <= 3840 v_max, L = 27.8 px!)
			b.hasBounce = true;
			b.tBounceSec = 4.5 / 60.0; // Bounces cleanly at y = 621 on court baseline between frames 4 and 5!
			b.vxPost = 2160.0;         // 36 px/frame X
			b.vyPost = -1560.0;        // -26 px/frame Y -> speed = 44.4 px/frame (L = 22.2 px!)
			b.baseY = 0.95f;
			b.baseCb = -0.18f;
			b.baseCr = 0.12f;
			b.textured = true;
			r.balls.push_back(b);
			return RenderSequence(r);
		}

		// 3. Hockey Slapshot Linear Glide (60p): Dark 7 px puck on bright white ice + rink lines, 52 px/frame with tangential drag.
		inline SynthSequence MakeHockeySlapshotGlideScene(int numFrames = 11)
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.25;
			r.surface = SurfaceKind::HockeyIce;
			r.camPanVxPxFrame = 6.0;
			r.camPanVyPxFrame = 0.0;
			r.grainSigma = 0.004;
			r.seed = 3003ull;

			BallSpec b;
			b.id = 1;
			b.expectedType = FastObjects::TrajectoryType::LinearGlide;
			b.radiusPx = 3.5; // 7 px dark puck
			b.x0 = 520.0;
			b.y0 = 560.0;
			b.vx0 = 3120.0;   // 52 px/frame (<= 64 px/frame v_max!)
			b.vy0 = -480.0;
			b.dragDecel = 3600.0;
			b.baseY = 0.05f;
			b.textured = true;
			r.balls.push_back(b);
			return RenderSequence(r);
		}

		// 4. 2.5D Approaching Perspective Ball (60p): Apparent radius grows r(t) = r0 / (1 - k*t).
		inline SynthSequence MakePerspectiveApproachingBallScene(int numFrames = 10)
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.20;
			r.surface = SurfaceKind::SoccerGrass;
			r.camPanVxPxFrame = 4.0;
			r.grainSigma = 0.004;
			r.seed = 4004ull;

			BallSpec b;
			b.id = 1;
			b.expectedType = FastObjects::TrajectoryType::LinearGlide;
			b.isPerspective = true;
			b.radiusPx = 3.5;     // Grows from 3.5 px to ~7.8 px across 10 frames!
			b.kPerspPerSec = 3.6;
			b.perspCenterX = 960.0;
			b.perspCenterY = 540.0;
			b.x0 = 810.0;
			b.y0 = 470.0;
			b.vx0 = -480.0;
			b.vy0 = -210.0;
			b.baseY = 0.93f;
			b.textured = true;
			r.balls.push_back(b);
			return RenderSequence(r);
		}

		// 5. Chroma-Only Optic-Yellow Ball on Iso-Luma Court (|Delta Y| == 0, strong Cb/Cr contrast + grain).
		inline SynthSequence MakeChromaOnlyIsoLumaBallScene(int numFrames = 10, double grainSigma = 0.005)
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.25;
			r.surface = SurfaceKind::IsoLumaCourt;
			r.isoLumaValue = 0.52f;
			r.camPanVxPxFrame = 4.0;
			r.grainSigma = grainSigma;
			r.seed = 5005ull;

			BallSpec b;
			b.id = 1;
			b.expectedType = FastObjects::TrajectoryType::LinearGlide;
			b.radiusPx = 5.0;  // 10 px optic-yellow ball
			b.x0 = 500.0;
			b.y0 = 520.0;
			b.vx0 = 1920.0;    // 32 px/frame (well within 64 px/frame v_max!)
			b.vy0 = 240.0;
			b.baseY = 0.52f;   // IDENTICAL luma to IsoLumaCourt (|Delta Y| == 0.0)!
			b.baseCb = -0.22f;
			b.baseCr = 0.16f;
			b.textured = false;
			r.balls.push_back(b);
			return RenderSequence(r);
		}

		// 6. Basketball Pass with 3-Frame Player Occlusion (60p): 16 px ball crosses behind large Label::Object player.
		inline SynthSequence MakeBasketballOcclusionScene()
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080;
			r.numFrames = 14;
			r.fps = 60.0;
			r.shutterFraction = 0.20;
			r.surface = SurfaceKind::HardwoodCourt;
			r.camPanVxPxFrame = 4.0;
			r.grainSigma = 0.004;
			r.seed = 6006ull;

			BallSpec b;
			b.id = 1;
			b.expectedType = FastObjects::TrajectoryType::LinearGlide;
			b.radiusPx = 7.5; // 15 px basketball
			b.x0 = 540.0;
			b.y0 = 520.0;
			b.vx0 = 1680.0; // +28 px/frame world X
			b.vy0 = -60.0;
			b.baseY = 0.89f;
			b.baseCb = -0.14f;
			b.baseCr = 0.20f;
			b.textured = true;
			r.balls.push_back(b);

			// Player spans [666, 750] x [400, 640] (84 x 240 = 20,160 px^2 >> (32.4)^2 = 1,050 px^2 large object!):
			// Occludes synth frames 5, 6, 7 (3 frames: world X = 680, 708, 736)!
			r.player.enabled = true;
			r.player.wx0 = 666.0;
			r.player.wx1 = 750.0;
			r.player.wy0 = 400.0;
			r.player.wy1 = 640.0;
			return RenderSequence(r);
		}

		// 7. Crossing Balls Ambiguity (60p): Two balls crossing within <= 0.4 blob radius at crossing frame!
		inline SynthSequence MakeCrossingBallsScene()
		{
			SceneRecipe r;
			r.srcW = 1920;
			r.srcH = 1080;
			r.numFrames = 12;
			r.fps = 60.0;
			r.shutterFraction = 0.20;
			r.surface = SurfaceKind::SoccerGrass;
			r.camPanVxPxFrame = 4.0;
			r.grainSigma = 0.004;
			r.seed = 7007ull;

			BallSpec b1;
			b1.id = 1;
			b1.expectedType = FastObjects::TrajectoryType::LinearGlide;
			b1.radiusPx = 5.5;
			b1.x0 = 550.0;
			b1.y0 = 419.0;
			b1.vx0 = 2100.0; // +35 px/frame X
			b1.vy0 = 1200.0; // +20 px/frame Y -> (760, 539) at synth frame 6 (t = 0.10s)
			b1.baseY = 0.92f;
			b1.textured = true;

			BallSpec b2 = b1;
			b2.id = 2;
			b2.x0 = 550.0;
			b2.y0 = 661.0;
			b2.vx0 = 2100.0;
			b2.vy0 = -1200.0; // -> (760, 541) at synth frame 6 (2.0 px apart < 1 radius!)
			r.balls.push_back(b1);
			r.balls.push_back(b2);
			return RenderSequence(r);
		}

		// =====================================================================
		// Milestone 0 Mandatory Negative-Control Scene Builders (0.00% False Locks!)
		// =====================================================================

		inline SynthSequence MakeCrowdConfettiClutterScene(int numFrames = 18)
		{
			SceneRecipe r;
			r.srcW = 960;
			r.srcH = 540;
			r.numFrames = numFrames;
			r.fps = 30.0;
			r.shutterFraction = 0.30;
			r.surface = SurfaceKind::SoccerGrass;
			r.camPanVxPxFrame = 4.0;
			r.grainSigma = 0.005;
			r.seed = 8001ull;

			for (int i = 0; i < 10; ++i)
			{
				ClutterParticle cp;
				cp.kind = ClutterParticle::Kind::RandomWalk;
				cp.baseWx = 160.0 + 65.0 * i;
				cp.baseWy = 140.0 + 30.0 * (i % 5);
				cp.ampX = 24.0;
				cp.ampY = 24.0;
				cp.radiusPx = 4.0;
				cp.luma = 0.94f;
				cp.seed = 9100ull + static_cast<uint64_t>(i) * 37ull;
				r.clutter.push_back(cp);
			}
			for (int i = 0; i < 6; ++i)
			{
				ClutterParticle hand;
				hand.kind = ClutterParticle::Kind::Oscillatory;
				hand.baseWx = 180.0 + 95.0 * i;
				hand.baseWy = 390.0;
				hand.ampX = 16.0;
				hand.ampY = 12.0;
				hand.freqHz = 10.0;
				hand.phase = 0.9 * i;
				hand.radiusPx = 5.0;
				hand.luma = 0.89f;
				r.clutter.push_back(hand);
			}
			return RenderSequence(r);
		}

		inline SynthSequence MakeRipplingWaterGlintsScene(int numFrames = 18)
		{
			SceneRecipe r;
			r.srcW = 960;
			r.srcH = 540;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.25;
			r.surface = SurfaceKind::WaterSurface;
			r.camPanVxPxFrame = 4.0;
			r.grainSigma = 0.005;
			r.seed = 8002ull;

			r.waterRegion.left = 0;
			r.waterRegion.top = 0;
			r.waterRegion.right = 960;
			r.waterRegion.bottom = 280;

			for (int i = 0; i < 8; ++i)
			{
				ClutterParticle glint;
				glint.kind = (i < 4) ? ClutterParticle::Kind::RandomWalk : ClutterParticle::Kind::Oscillatory;
				glint.baseWx = 150.0 + 85.0 * i;
				glint.baseWy = (i < 4) ? 140.0 : 390.0;
				glint.ampX = 14.0;
				glint.ampY = 10.0;
				glint.freqHz = 22.0;
				glint.phase = 0.7 * i;
				glint.radiusPx = 4.5;
				glint.luma = 0.96f;
				glint.seed = 9200ull + static_cast<uint64_t>(i);
				r.clutter.push_back(glint);
			}
			return RenderSequence(r);
		}

		inline SynthSequence MakeScoreboardAndLedClutterScene(int numFrames = 20)
		{
			SceneRecipe r;
			r.srcW = 960;
			r.srcH = 540;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.25;
			r.surface = SurfaceKind::SoccerGrass;
			r.camPanVxPxFrame = 0.0; // Stationary/slow-pan stadium shot with scrolling ticker + pulsing LED ribbon boards
			r.grainSigma = 0.005;
			r.seed = 8003ull;

			r.overlay.enabled = true;
			r.overlay.box.left = 48;
			r.overlay.box.top = 464;
			r.overlay.box.right = 912;
			r.overlay.box.bottom = 512;
			r.overlay.markInP8 = true;
			r.overlay.scrollSpeedPxSec = 320.0;

			// Unmasked oscillatory/jittering LED sideline ad boards in fixed 64x36 grid cells:
			// Every frame moves back-and-forth by +/-12 px (disjoint from prior footprint so Stage A detects blobs every frame,
			// while Stage B rejects oscillatory/stationary clutter and heats the 64x36 hotspot auto-mask >= 3.0!).
			for (int i = 0; i < 3; ++i)
			{
				ClutterParticle led;
				led.kind = ClutterParticle::Kind::Oscillatory;
				led.screenFixed = true;
				led.baseWx = 180.0 + 260.0 * i;
				led.baseWy = 72.0;
				led.ampX = 6.0;
				led.ampY = 0.0;
				led.freqHz = 30.0; // Alternates left/right inside the same 15x15 hotspot cell!
				led.phase = 0.4;
				led.radiusPx = 4.0;
				led.luma = 0.97f;
				r.clutter.push_back(led);
			}
			return RenderSequence(r);
		}

		// Heavy Panning Film Grain (sigma_g = 0.03 in [0.02, 0.04]) + Crisp High-Contrast Sub-Pixel Panning Pitch Lines!
		inline SynthSequence MakeHeavyGrainPanningLinesScene(int numFrames = 18)
		{
			SceneRecipe r;
			r.srcW = 960;
			r.srcH = 540;
			r.numFrames = numFrames;
			r.fps = 60.0;
			r.shutterFraction = 0.15;
			r.surface = SurfaceKind::SoccerGrass;
			r.camPanVxPxFrame = 7.5;  // Fractional half-luma pan across crisp white pitch lines!
			r.camPanVyPxFrame = 1.25;
			r.camEstJitterPx = 0.25;
			r.camReportedSigmaPx = 0.45;
			r.grainSigma = 0.030;
			r.seed = 8004ull;
			return RenderSequence(r);
		}
	}
}
