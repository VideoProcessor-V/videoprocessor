// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
// Built without the test project's precompiled header: keep windows.h from defining min/max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "CppUnitTest.h"

// K7-1: OutputDomainMap (CPU). Source-plane points and vectors to output pixels: identity, crop / letterbox, the P0 re-base
// plane, the NLS stretch (the hook's law, inverted), block vectors, determinism. The shader mapping is a WARP test for K7-2.

#include <VP-V/modules/output/dither/OutputDomainMap.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace OutputDomainMapTestsNs
{
	using namespace OutputDomainMap;

	inline void Require(bool ok, const wchar_t* what) { Assert::IsTrue(ok, what); }
	inline bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

	inline Box MakeBox(double l, double t, double r, double b) { Box x; x.left = l; x.top = t; x.right = r; x.bottom = b; return x; }

	// 1920x1080 raster, 2.39 letterbox active picture rows 140..940, scaled 2x into a 3840x2160 output with bars.
	inline Config Letterbox()
	{
		Config c = Config::Identity(1920, 1080);
		c.active = MakeBox(0, 140, 1920, 940);
		c.output = MakeBox(0, 280, 3840, 1880);
		return c;
	}

	inline Config WithNls(double ratio, double curve, int geometry, double protection, bool vertical)
	{
		Config c = Config::Identity(1920, 1080);
		c.nls.active = true;
		c.nls.ratio = ratio;
		c.nls.curve = curve;
		c.nls.geometry = geometry;
		c.nls.centerProtection = protection;
		c.nls.vertical = vertical;
		return c;
	}

	TEST_CLASS(OutputDomainMapTests)
	{
	public:
		TEST_METHOD(IdentityMapsPointsAndVectorsUnchanged)
		{
			const Config c = Config::Identity(1920, 1080);
			Require(IsValid(c) && IsIdentity(c), L"identity config");
			for (double x : { 0.0, 1.5, 960.0, 1919.0 })
				for (double y : { 0.0, 7.25, 540.0, 1079.0 })
				{
					const Result p = MapPoint(c, x, y);
					Require(p.x == x && p.y == y && p.inside && p.valid, L"point unchanged");
					const Result v = MapVector(c, x, y, -3.5, 2.0);
					Require(Near(v.x, -3.5, 1e-9) && Near(v.y, 2.0, 1e-9) && v.valid, L"vector unchanged (a raster-edge start or end may be flagged outside)");
				}
			Config bad;   // zero raster: not valid
			Require(!IsValid(bad) && IsIdentity(bad), L"an invalid map is the identity");
			const Result b = MapPoint(bad, 12.0, 34.0);
			Require(!b.valid && b.x == 12.0 && b.y == 34.0, L"invalid: input returned, flagged");
		}

		TEST_METHOD(CropAndOutputScale)
		{
			const Config c = Letterbox();
			Require(IsValid(c) && !IsIdentity(c), L"valid, not identity");
			Result p = MapPoint(c, 960.0, 540.0);
			Require(Near(p.x, 1920.0, 1e-9) && Near(p.y, 1080.0, 1e-9) && p.inside, L"active centre lands at the output centre");
			p = MapPoint(c, 0.0, 140.0);
			Require(Near(p.x, 0.0, 1e-9) && Near(p.y, 280.0, 1e-9), L"top-left of the active picture lands at the letterbox corner");
			p = MapPoint(c, 1920.0, 940.0);
			Require(Near(p.x, 3840.0, 1e-9) && Near(p.y, 1880.0, 1e-9), L"bottom-right");
			p = MapPoint(c, 960.0, 100.0);
			Require(!p.inside && Near(p.y, 200.0, 1e-9), L"a point in the bar is outside and extends linearly");
			const Result v = MapVector(c, 400.0, 500.0, 20.0, -4.0);
			Require(Near(v.x, 40.0, 1e-9) && Near(v.y, -8.0, 1e-9) && v.inside, L"a vector scales by the output scale");
			const Result out = MapVector(c, 400.0, 150.0, 0.0, -30.0);
			Require(!out.inside, L"a vector that leaves the active picture is flagged");
			// A presentation crop left and right: only the inner picture is shown.
			Config pc = c;
			pc.active = MakeBox(48, 140, 1872, 940);
			p = MapPoint(pc, 48.0, 140.0);
			Require(Near(p.x, 0.0, 1e-9), L"presentation crop: the crop edge is the output edge");
			p = MapPoint(pc, 10.0, 540.0);
			Require(!p.inside, L"cropped-off columns are outside");
			// Default output = the active picture's size at the origin.
			Config d = Config::Identity(1920, 1080);
			d.active = MakeBox(0, 140, 1920, 940);
			p = MapPoint(d, 100.0, 240.0);
			Require(Near(p.x, 100.0, 1e-9) && Near(p.y, 100.0, 1e-9), L"no output box: crop only shifts the origin");
		}

		TEST_METHOD(RebaseChangesThePlaneNotTheOutputPosition)
		{
			Config c = Letterbox();
			const Result before = MapPoint(c, 700.0, 300.0);
			c.rebase.factorX = 0.5;
			c.rebase.factorY = 0.5;
			const Result after = MapPoint(c, 700.0, 300.0);
			Require(Near(before.x, after.x, 1e-9) && Near(before.y, after.y, 1e-9), L"the re-base cancels in the output position");
			const Result plane = PlanePoint(c, 700.0, 300.0);
			Require(Near(plane.x, 350.0, 1e-9) && Near(plane.y, 150.0, 1e-9), L"the working plane is the raster scaled by the factor");
			double pw, ph, sx, sy;
			PlaneSize(c, pw, ph);
			OutputScale(c, sx, sy);
			Require(Near(pw, 960.0, 1e-9) && Near(ph, 400.0, 1e-9), L"plane size");
			Require(Near(sx, 4.0, 1e-9) && Near(sy, 4.0, 1e-9), L"the scaler applies 4x on the half-size plane");
			Require(!after.rebaseFading, L"settled");
			c.rebase.fadeWeight = 0.4;
			Require(MapPoint(c, 1.0, 141.0).rebaseFading && MapVector(c, 1.0, 141.0, 1.0, 1.0).rebaseFading, L"a crossfading re-base is reported");
			c.rebase.factorX = 0.0;
			Require(!IsValid(c), L"a zero factor is invalid");
			// Per-axis factors.
			Config a = Letterbox();
			a.rebase.factorX = 0.5;
			a.rebase.factorY = 1.0;
			double w, h;
			PlaneSize(a, w, h);
			OutputScale(a, sx, sy);
			Require(Near(w, 960.0, 1e-9) && Near(h, 800.0, 1e-9) && Near(sx, 4.0, 1e-9) && Near(sy, 2.0, 1e-9), L"per-axis re-base");
		}

		TEST_METHOD(NlsEndpointsCentreAndInverse)
		{
			const double ratio = 1.4, curve = 2.0;
			for (int geometry = 0; geometry < 2; ++geometry)
			{
				const Config c = WithNls(ratio, curve, geometry, geometry ? 0.2 : 0.0, false);
				Require(IsValid(c) && !IsIdentity(c), L"NLS active is not identity");
				Require(Near(MapPoint(c, 0.0, 500.0).x, 0.0, 1e-6) && Near(MapPoint(c, 1920.0, 500.0).x, 1920.0, 1e-6), L"edges are fixed");
				Require(Near(MapPoint(c, 960.0, 500.0).x, 960.0, 1e-6), L"centre is fixed");
				double prev = -1.0;
				for (int i = 0; i <= 1920; i += 16)
				{
					const Result p = MapPoint(c, static_cast<double>(i), 500.0);
					Require(p.x > prev, L"strictly monotone");
					prev = p.x;
					// The inverse: pushing the output coordinate through the hook's forward law returns the source coordinate.
					const double back = detail::NlsForward(p.x / 1920.0, ratio, curve, geometry, geometry ? 0.2 : 0.0) * 1920.0;
					Require(Near(back, static_cast<double>(i), 1e-6), L"source -> output is the inverse of the hook's map");
					Require(Near(p.y, 500.0, 1e-9), L"the other axis is linear");
				}
			}
		}

		TEST_METHOD(NlsScalesVectorsByTheLocalSlope)
		{
			const Config c = WithNls(1.4, 2.0, 0, 0.0, false);
			// geometry 0, ratio 1.4, curve 2: hook slope at the centre 1.4 -> output slope 1/1.4; at the edge 1.4 - 0.4 * 2 = 0.6 -> 1/0.6.
			const Result centre = MapVector(c, 960.0 - 0.5, 540.0, 1.0, 0.0);
			Require(Near(centre.x, 1.0 / 1.4, 2e-4), L"centre: vectors shrink by the ratio");
			const Result edge = MapVector(c, 1919.0 - 0.5, 540.0, 1.0, 0.0);
			Require(Near(edge.x, 1.0 / 0.6, 0.02), L"edge: vectors grow by 1 / edge slope");
			const Result vert = MapVector(c, 960.0, 540.0, 0.0, 5.0);
			Require(Near(vert.y, 5.0, 1e-9), L"the unwarped axis is untouched");
			// Vertical warp.
			const Config v = WithNls(1.4, 2.0, 0, 0.0, true);
			const Result vc = MapVector(v, 960.0, 540.0 - 0.5, 0.0, 1.0);
			Require(Near(vc.y, 1.0 / 1.4, 2e-4) && Near(vc.x, 0.0, 1e-9), L"vertical axis warped");
		}

		TEST_METHOD(NlsInactiveAndStrengthAndClamps)
		{
			Config c = WithNls(1.4, 2.0, 0, 0.0, false);
			c.nls.active = false;
			Require(IsIdentity(c) && MapPoint(c, 300.0, 200.0).x == 300.0, L"inactive NLS is linear");
			c.nls.active = true;
			c.nls.strength = 0.0;
			Require(IsIdentity(c), L"strength 0 is linear");
			c.nls.strength = 1.0;
			c.nls.ratio = 1.0;
			Require(IsIdentity(c), L"ratio 1 is linear");
			Config big = WithNls(3.0, 2.0, 0, 0.0, false), cap = WithNls(1.5, 2.0, 0, 0.0, false);
			Require(Near(MapPoint(big, 400.0, 1.0).x, MapPoint(cap, 400.0, 1.0).x, 1e-9), L"the ratio is clamped to the hook's 1.5");
			Config half = WithNls(1.4, 2.0, 0, 0.0, false), full = WithNls(1.2, 2.0, 0, 0.0, false);
			half.nls.strength = 0.5;
			Require(Near(MapPoint(half, 400.0, 1.0).x, MapPoint(full, 400.0, 1.0).x, 1e-9), L"strength blends the ratio toward 1");
			// Outside the picture the map extends linearly and continuously.
			const Config c2 = WithNls(1.4, 2.0, 0, 0.0, false);
			const double a = MapPoint(c2, -1.0, 100.0).x, b = MapPoint(c2, 0.0, 100.0).x, d = MapPoint(c2, -2.0, 100.0).x;
			Require(!MapPoint(c2, -1.0, 100.0).inside && Near(b - a, a - d, 1e-6), L"linear extension left of the edge");
		}

		TEST_METHOD(BlockVectorsOfAPan)
		{
			// A pan of (+20, 0) px on every block, mapped through the letterbox: 2x, blocks outside the picture are dropped.
			const Config c = Letterbox();
			CameraModel::Rect pic;
			pic.left = 0; pic.top = 140; pic.right = 1920; pic.bottom = 940;
			const int gw = 120, gh = 50;
			std::vector<float> mx(static_cast<size_t>(gw) * gh, 20.0f), my(static_cast<size_t>(gw) * gh, 0.0f), ox, oy;
			std::vector<uint8_t> inside;
			Require(MapBlockVectors(c, pic, gw, gh, 16.0, mx, my, ox, oy, inside), L"mapped");
			Require(Near(ox[static_cast<size_t>(25) * gw + 60], 40.0, 1e-4) && Near(oy[static_cast<size_t>(25) * gw + 60], 0.0, 1e-4), L"mid block");
			Require(inside[static_cast<size_t>(25) * gw + 60] == 1, L"mid block inside");
			// The last column's vector ends beyond the right edge: flagged and zeroed.
			Require(inside[static_cast<size_t>(25) * gw + 119] == 0 && Near(ox[static_cast<size_t>(25) * gw + 119], 40.0, 1e-3), L"edge block leaving the picture: flagged, vector kept");
			// Invalid map: zeros and false.
			Config bad;
			Require(!MapBlockVectors(bad, pic, gw, gh, 16.0, mx, my, ox, oy, inside) && ox[0] == 0.0f, L"invalid map: no vectors");
			// Picture-centred points agree with raster points.
			const Result a = MapPictureCentred(c, pic, 0.0, 0.0), b = MapPoint(c, 960.0, 540.0);
			Require(a.x == b.x && a.y == b.y, L"picture centre is the raster point");
		}

		TEST_METHOD(NlsMonotoneAtTheParameterCorners)
		{
			for (double ratio : { 1.05, 1.5 })
				for (double curve : { 1.0, 2.9 })
					for (double prot : { 0.0, 0.2, 0.45 })
						for (int geometry = 0; geometry < 2; ++geometry)
						{
							double prev = -1.0;
							for (int i = 0; i <= 1000; ++i)
							{
								const double f = detail::NlsForward(i / 1000.0, ratio, curve, geometry, prot);
								Require(f >= prev - 1e-12, L"hook map is monotone at the corners");
								prev = f;
							}
						}
			Require(!MapPoint(Config::Identity(100, 100), std::nan(""), 1.0).valid, L"non-finite input: valid = false");
		}

		TEST_METHOD(Deterministic)
		{
			const Config c = WithNls(1.37, 2.4, 1, 0.15, false);
			for (double x : { 3.3, 411.7, 1500.2 })
			{
				const Result a = MapPoint(c, x, 77.0), b = MapPoint(c, x, 77.0);
				Require(std::memcmp(&a.x, &b.x, sizeof(double)) == 0, L"bit-identical");
			}
		}
	};
}
