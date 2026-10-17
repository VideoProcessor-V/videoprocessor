#include "pch.h"
#include "CppUnitTest.h"

#include <SourceResolutionDetector.h>
#include "SubtitleTextTestPattern.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Owner 2026-10-16: mixed screens. A streaming app's menu at native 4K with a video preview
// playing inside it at a lower resolution, a 4K info panel or transport bar over a 1080 film,
// picture-in-picture. The re-base is whole-frame (one factor per axis), so VP must never re-base
// such a frame to the video's rate - the 4K UI would be softened. Subtitles (a text band over
// the video) must still be ignored as before.
//
// The frames are synthesised at 3840 x 2160 and sampled as the monitor does (4 x 2 tiles of 256
// px on the 16-px grid), at 2 Hz, in shots of 3 s; every scenario runs through ReadFrame and the
// Tracker with the mixed-screen guard on and off (off = the behaviour before the guard).
namespace VideoProcessorTest
{
	namespace
	{
		namespace MixedScreen
		{
			constexpr double kPi = 3.14159265358979323846;
			constexpr int kTile = 256;
			constexpr int kWidth = 3840, kHeight = 2160;
			constexpr int kColumns = 4, kRows = 2;   // the monitor's grid
			constexpr int kTiles = kColumns * kRows;

			// The synthetic scene of SourceResolutionDetectorTests.cpp (copied): a sum of
			// separable cosines, amplitude falling as 1/f, frequencies up to the master's Nyquist.
			struct Scene
			{
				struct Wave { double fx, fy, px, py, amplitude; };
				std::vector<Wave> waves;
				explicit Scene(unsigned seed, int count = 260)
				{
					std::mt19937 rng(seed);
					std::uniform_real_distribution<double> u(0.0, 1.0);
					for (int i = 0; i < count; ++i)
					{
						const double f = 0.004 * std::pow(0.49 / 0.004, u(rng));
						const double angle = u(rng) * kPi;
						Wave w{ f * std::cos(angle), f * std::sin(angle), u(rng) * 2 * kPi, u(rng) * 2 * kPi, 0.0 };
						w.amplitude = 0.012 / std::sqrt(f);
						waves.push_back(w);
					}
				}
				std::vector<double> Sample(int x0, int y0, int w, int h) const
				{
					std::vector<double> out(static_cast<size_t>(w) * h, 0.45);
					std::vector<double> cx(w), sx(w), cy(h), sy(h);
					for (const Wave& v : waves)
					{
						for (int x = 0; x < w; ++x) { const double a = 2 * kPi * v.fx * (x0 + x) + v.px; cx[x] = std::cos(a); sx[x] = std::sin(a); }
						for (int y = 0; y < h; ++y) { const double b = 2 * kPi * v.fy * (y0 + y) + v.py; cy[y] = std::cos(b); sy[y] = std::sin(b); }
						for (int y = 0; y < h; ++y)
						{
							double* row = &out[static_cast<size_t>(y) * w];
							const double c = v.amplitude * cy[y], s = v.amplitude * sy[y];
							for (int x = 0; x < w; ++x) row[x] += cx[x] * c - sx[x] * s;
						}
					}
					return out;
				}
			};

			using Kernel = std::function<double(double)>;
			double Sinc(double x) { return std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
			const Kernel kLanczos3 = [](double x) { return std::fabs(x) < 3.0 ? Sinc(x) * Sinc(x / 3.0) : 0.0; };

			// One tile of the scene at frame position (ox, oy): mastered at `ratio` of the native
			// resolution and upscaled with Lanczos-3 (one picture across the frame: every tile
			// samples the same master at its own place), or sampled natively at ratio 1. Unclipped.
			std::vector<double> Raw(const Scene& scene, double ratio, int ox, int oy)
			{
				if (ratio >= 1.0) return scene.Sample(ox, oy, kTile, kTile);
				const int margin = 4;
				const int mw = static_cast<int>(std::ceil(kTile * ratio)) + 2 * margin;
				const int mx0 = static_cast<int>(std::floor(ox * ratio)) - margin, my0 = static_cast<int>(std::floor(oy * ratio)) - margin;
				const std::vector<double> master = scene.Sample(mx0, my0, mw, mw);
				auto resample = [&](const double* src, int stride, double* dst, int dstStride, int origin, int m0) {
					for (int x = 0; x < kTile; ++x)
					{
						const double u = (origin + x + 0.5) * ratio - 0.5 - m0;
						const int c = static_cast<int>(std::floor(u));
						double sum = 0, wsum = 0;
						for (int k = c - 3; k <= c + 4; ++k)
						{
							if (k < 0 || k >= mw) continue;
							const double w = kLanczos3(u - k);
							sum += w * src[static_cast<size_t>(k) * stride];
							wsum += w;
						}
						dst[static_cast<size_t>(x) * dstStride] = wsum != 0 ? sum / wsum : 0;
					}
				};
				std::vector<double> rows(static_cast<size_t>(mw) * kTile), out(static_cast<size_t>(kTile) * kTile);
				for (int y = 0; y < mw; ++y) resample(&master[static_cast<size_t>(y) * mw], 1, &rows[static_cast<size_t>(y) * kTile], 1, ox, mx0);
				for (int x = 0; x < kTile; ++x) resample(&rows[x], kTile, &out[x], kTile, oy, my0);
				return out;
			}

			float Code(double v) { return static_cast<float>(std::round((std::min)(1.0, (std::max)(0.0, v)) * 1023.0) / 1023.0); }

			struct Rect
			{
				int x0, y0, x1, y1;
				bool Contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
			};
			struct Line { int x0, x1, y0, height; };

			// A screen: the lower-resolution video (in `video`, or everywhere), translucent UI panels
			// over it, native-resolution picture (poster artwork), a second video (picture-in-
			// picture at its own rate), flat solid UI shapes and UI text at the output resolution.
			struct Layout
			{
				std::string name;
				bool videoEverywhere = false;
				double videoRatio = 0.5, pipRatio = 0.5;
				std::vector<Rect> video, dimmed, native, pip, bars;
				std::vector<Line> text;
				double background = 0.12;
			};

			enum Source : unsigned char { kBackground, kVideo, kDimmed, kNative, kPip, kBar };

			Source At(const Layout& l, int x, int y)
			{
				for (const Rect& r : l.bars) if (r.Contains(x, y)) return kBar;
				for (const Rect& r : l.native) if (r.Contains(x, y)) return kNative;
				for (const Rect& r : l.pip) if (r.Contains(x, y)) return kPip;
				for (const Rect& r : l.dimmed) if (r.Contains(x, y)) return kDimmed;
				for (const Rect& r : l.video) if (r.Contains(x, y)) return kVideo;
				return l.videoEverywhere ? kVideo : kBackground;
			}

			// The monitor's tile placement (SourceResolutionMonitor: 4 x 2, centred in equal cells,
			// on the 16-px grid inside the picture).
			void Origin(int index, int& x0, int& y0)
			{
				auto place = [](double start, double span, int i, int count) {
					const double centre = start + (i + 0.5) * span / count;
					const double o = std::floor((centre - kTile / 2) / 16.0) * 16.0;
					const double lo = std::ceil(start / 16.0) * 16.0, hi = std::floor((start + span - kTile) / 16.0) * 16.0;
					return static_cast<int>((std::max)(lo, (std::min)(hi, o)));
				};
				x0 = place(0.0, kWidth, index % kColumns, kColumns);
				y0 = place(0.0, kHeight, index / kColumns, kRows);
			}

			// One shot's tiles: the sources each tile needs, and the composite of the static parts
			// (UI, native picture, text; the text's coverage keeps it over the moving video).
			struct ShotTile
			{
				std::vector<unsigned char> map, coverage;
				std::vector<double> video, pip;
				std::vector<float> base;
			};

			std::vector<ShotTile> MakeShot(const Layout& l, int shot, std::vector<std::vector<float>>& nativeCache)
			{
				const Scene videoScene(100u + 7u * static_cast<unsigned>(shot)), pipScene(300u + 7u * static_cast<unsigned>(shot));
				static const Scene uiScene(900u);
				std::vector<ShotTile> tiles(kTiles);
				if (nativeCache.size() != static_cast<size_t>(kTiles)) nativeCache.assign(kTiles, {});
				for (int i = 0; i < kTiles; ++i)
				{
					int ox, oy;
					Origin(i, ox, oy);
					ShotTile& t = tiles[static_cast<size_t>(i)];
					t.map.assign(static_cast<size_t>(kTile) * kTile, kBackground);
					t.coverage.assign(t.map.size(), 0);
					t.base.assign(t.map.size(), static_cast<float>(l.background));
					bool needVideo = false, needPip = false, needNative = false;
					for (int y = 0; y < kTile; ++y)
						for (int x = 0; x < kTile; ++x)
						{
							const Source s = At(l, ox + x, oy + y);
							t.map[static_cast<size_t>(y) * kTile + x] = s;
							needVideo |= s == kVideo || s == kDimmed;
							needPip |= s == kPip;
							needNative |= s == kNative;
						}
					if (needVideo) t.video = Raw(videoScene, l.videoRatio, ox, oy);
					if (needPip) t.pip = Raw(pipScene, l.pipRatio, ox, oy);
					if (needNative && nativeCache[static_cast<size_t>(i)].empty())
					{
						const std::vector<double> raw = Raw(uiScene, 1.0, ox, oy);
						nativeCache[static_cast<size_t>(i)].resize(raw.size());
						for (size_t p = 0; p < raw.size(); ++p) nativeCache[static_cast<size_t>(i)][p] = Code(raw[p]);
					}
					for (size_t p = 0; p < t.map.size(); ++p)
					{
						switch (t.map[p])
						{
						case kNative: t.base[p] = nativeCache[static_cast<size_t>(i)][p]; break;
						case kBar: t.base[p] = static_cast<float>(SubtitleTextTestPattern::kWhite); break;
						case kVideo: t.base[p] = Code(t.video[p]); break;
						case kDimmed: t.base[p] = Code(t.video[p] * 0.35 + 0.04); break;
						case kPip: t.base[p] = Code(t.pip[p]); break;
						default: break;
						}
					}
					// UI text / subtitles, drawn at the output resolution (glyphs placed in frame
					// coordinates, so a line continues from tile to tile).
					for (size_t k = 0; k < l.text.size(); ++k)
					{
						const Line& line = l.text[k];
						if (line.y0 + line.height + 12 < oy || line.y0 - 12 >= oy + kTile || line.x1 <= ox || line.x0 >= ox + kTile) continue;
						SubtitleTextTestPattern::Draw(t.base, kTile, kTile, line.x0 - ox, line.x1 - ox, line.y0 - oy, line.height,
							500u + static_cast<unsigned>(k), &t.coverage);
					}
				}
				return tiles;
			}

			// One analysed frame: the moving video at `offset` (a cut per shot, a small step per
			// frame so no frame repeats); the UI static.
			std::vector<SourceResolution::Spectrum> AnalyseFrame(const std::vector<ShotTile>& shot, double offset)
			{
				std::vector<SourceResolution::Spectrum> spectra(shot.size());
				std::vector<float> tile(static_cast<size_t>(kTile) * kTile);
				for (size_t i = 0; i < shot.size(); ++i)
				{
					const ShotTile& t = shot[i];
					for (size_t p = 0; p < tile.size(); ++p)
					{
						if (t.coverage[p]) { tile[p] = t.base[p]; continue; }
						switch (t.map[p])
						{
						case kVideo: tile[p] = Code(t.video[p] + offset); break;
						case kDimmed: tile[p] = Code((std::min)(1.0, (std::max)(0.0, t.video[p] + offset)) * 0.35 + 0.04); break;
						case kPip: tile[p] = Code(t.pip[p] + offset); break;
						default: tile[p] = t.base[p]; break;
						}
					}
					Assert::IsTrue(SourceResolution::Analyse(tile.data(), kTile, kTile, spectra[i], 1023));
				}
				return spectra;
			}

			struct Outcome
			{
				int frames = 0, informative = 0, lower = 0, mixed = 0;
				double declared = 1.0, lowest = 1.0, firstDeclared = -1.0;
				SourceResolution::FrameReading first;   // the first informative frame (logged)
			};

			struct Segment { const Layout* layout; int shots; };

			// The segments in order through one tracker per guard state. `onFrame` (optional) sees
			// every frame: segment index, frame within the segment, the two declared ratios.
			void Run(const std::vector<Segment>& segments, Outcome& on, Outcome& off,
				const std::function<void(size_t, int, double, double)>& onFrame = nullptr)
			{
				SourceResolution::Tracker trackerOn, trackerOff;
				SourceResolution::Thresholds tOn, tOff;
				tOff.mixedGuard = false;
				const int framesPerShot = 6;   // 3 s at the monitor's 2 Hz
				double t = 0.0;
				int shotIndex = 0;
				for (size_t s = 0; s < segments.size(); ++s)
				{
					std::vector<std::vector<float>> nativeCache;
					for (int shot = 0; shot < segments[s].shots; ++shot, ++shotIndex)
					{
						const std::vector<ShotTile> tiles = MakeShot(*segments[s].layout, shotIndex, nativeCache);
						for (int f = 0; f < framesPerShot; ++f, t += 0.5)
						{
							// +-0.2 per shot: the generator clips over half its pixels, so the cut must be large.
							const double offset = (shotIndex % 2 ? 0.2 : -0.2) + 3.0 * f / 1023.0;
							const std::vector<SourceResolution::Spectrum> spectra = AnalyseFrame(tiles, offset);
							const SourceResolution::FrameReading rOn = SourceResolution::ReadFrame(spectra, tOn);
							const SourceResolution::FrameReading rOff = SourceResolution::ReadFrame(spectra, tOff);
							// The guard only ever overrules a lower reading to native; off, it never acts.
							Assert::IsFalse(rOff.mixed, L"guard off: never mixed");
							if (rOn.mixed)
								Assert::IsTrue(rOn.ratio == 1.0 && rOn.ratioV == 1.0 && rOn.ratioH == 1.0 && rOn.mixedRatio < 1.0, L"a mixed frame reads native");
							else
								Assert::IsTrue(rOn.ratio == rOff.ratio && rOn.ratioV == rOff.ratioV && rOn.ratioH == rOff.ratioH, L"not mixed: the reading is unchanged");
							trackerOn.Update(rOn, t);
							trackerOff.Update(rOff, t);
							for (int g = 0; g < 2; ++g)
							{
								Outcome& o = g == 0 ? on : off;
								const SourceResolution::FrameReading& r = g == 0 ? rOn : rOff;
								const SourceResolution::Tracker& tr = g == 0 ? trackerOn : trackerOff;
								++o.frames;
								if (r.informative() && o.informative++ == 0) o.first = r;
								o.lower += r.informative() && r.ratio < 0.9 ? 1 : 0;
								o.mixed += r.mixed ? 1 : 0;
								o.declared = tr.Ratio();
								o.lowest = (std::min)(o.lowest, tr.Ratio());
								if (tr.Ratio() < 1.0 && o.firstDeclared < 0.0) o.firstDeclared = t;
							}
							if (onFrame) onFrame(s, shot * framesPerShot + f, trackerOn.Ratio(), trackerOff.Ratio());
						}
					}
				}
			}

			void Report(const char* name, const Outcome& on, const Outcome& off)
			{
				char line[512];
				for (int g = 0; g < 2; ++g)
				{
					const Outcome& o = g == 0 ? on : off;
					const SourceResolution::FrameReading& r = o.first;
					snprintf(line, sizeof(line),
						"%-58s guard %-3s: frames %d (informative %d, read lower %d, mixed %d); declared %.3f, lowest %.3f, first at %.1f s | "
						"first frame: tiles %d/%d text %d periodic %d ratio %.3f (v %.3f@%.3f h %.3f@%.3f floor v %.2f h %.2f) mixed %d picture %d overlay %d (%d rows x %d cols) overruled %.3f\n",
						g == 0 ? name : "", g == 0 ? "on" : "off", o.frames, o.informative, o.lower, o.mixed, o.declared, o.lowest, o.firstDeclared,
						r.informativeTiles, r.tiles, r.textTiles, r.periodicTiles, r.ratio, r.vertical.coherence, r.vertical.ratio, r.horizontal.coherence, r.horizontal.ratio,
						r.vertical.floor, r.horizontal.floor, r.mixed ? 1 : 0, r.mixedPictureTiles, r.mixedOverlayTiles, r.mixedOverlayRows, r.mixedOverlayColumns, r.mixedRatio);
					Logger::WriteMessage(line);
				}
			}

			// ---- The screens (3840 x 2160; the tiles sit at x 352 / 1312 / 2272 / 3232 and
			// y 400 / 1488, 256 px each) ----

			Layout Film(double ratio = 0.5)
			{
				Layout l;
				l.name = ratio >= 1.0 ? "native 4K film" : "1080 film, full frame";
				l.videoEverywhere = true;
				l.videoRatio = ratio;
				return l;
			}

			const std::vector<Rect> kPosterRow = { { 200, 1300, 900, 1900 }, { 1100, 1300, 1800, 1900 }, { 2000, 1300, 2700, 1900 }, { 2900, 1300, 3600, 1900 } };
			// A details panel beside a 25 % preview (title, two lines of description) - tiles 2 and 3.
			const std::vector<Line> kSidePanelText = { { 2100, 3700, 420, 64 }, { 2100, 3700, 520, 40 }, { 2100, 3700, 590, 40 } };
			// A list menu across the lower half (three rows of entries) - tiles 4..7.
			const std::vector<Line> kListText = { { 200, 3600, 1500, 48 }, { 200, 3600, 1580, 48 }, { 200, 3600, 1660, 48 } };
		}
	}

	using namespace MixedScreen;

	TEST_CLASS(MixedScreenSourceResolutionTests)
	{
	public:
		// The controls: the guard must leave plain films and subtitles exactly as before.
		TEST_METHOD(FilmsAndSubtitlesAreUnchanged)
		{
			Layout film = Film(0.5), native = Film(1.0);
			Layout subtitle = Film(0.5);
			subtitle.name = "1080 film + one subtitle line (bottom tile row)";
			subtitle.text = { { 900, 2940, 1610, 56 } };
			Layout twoLines = Film(0.5);
			twoLines.name = "1080 film + two subtitle lines";
			twoLines.text = { { 1000, 2840, 1560, 56 }, { 900, 2940, 1640, 56 } };
			Layout wide = Film(0.5);
			wide.name = "1080 film + a very wide subtitle line (touches all 4 columns)";
			wide.text = { { 560, 3280, 1610, 56 } };
			Layout top = Film(0.5);
			top.name = "1080 film + a top subtitle line (top tile row)";
			top.text = { { 900, 2940, 520, 56 } };
			for (const Layout* l : { &film, &subtitle, &twoLines, &wide, &top })
			{
				Outcome on, off;
				Run({ { l, 6 } }, on, off);
				Report(l->name.c_str(), on, off);
				Assert::AreEqual(0, on.mixed, (L"no mixed frame: " + std::wstring(l->name.begin(), l->name.end())).c_str());
				Assert::AreEqual(off.declared, on.declared, 1e-9, L"declared as without the guard");
				Assert::AreEqual(0.5, on.declared, 0.03, (L"the 1080 film is declared: " + std::wstring(l->name.begin(), l->name.end())).c_str());
			}
			Outcome on, off;
			Run({ { &native, 6 } }, on, off);
			Report(native.name.c_str(), on, off);
			Assert::AreEqual(1.0, on.lowest, L"native 4K stays native");
		}

		// (a) A streaming app's 4K menu with a 1080 preview playing in it.
		TEST_METHOD(MenuWithAVideoPreviewIsNeverReBased)
		{
			struct Case { Layout layout; bool guarded; };
			std::vector<Case> cases;
			{
				Layout l;
				l.name = "menu: 25% preview + details text + poster row";
				l.video = { { 0, 0, 1920, 1080 } };
				l.native = kPosterRow;
				l.text = kSidePanelText;
				cases.push_back({ l, true });
			}
			{
				Layout l;
				l.name = "menu: 25% preview + details text + list text";
				l.video = { { 0, 0, 1920, 1080 } };
				l.text = kSidePanelText;
				l.text.insert(l.text.end(), kListText.begin(), kListText.end());
				cases.push_back({ l, true });
			}
			{
				Layout l;
				l.name = "menu: 50% preview (top half) + poster row";
				l.video = { { 0, 0, 3840, 1080 } };
				l.native = kPosterRow;
				cases.push_back({ l, true });
			}
			{
				// Known limit: text-only UI confined to one tile row is a band like a subtitle.
				Layout l;
				l.name = "menu: 50% preview (top half) + list text only [limit]";
				l.video = { { 0, 0, 3840, 1080 } };
				l.text = kListText;
				cases.push_back({ l, false });
			}
			{
				// Known limit: the poster strip below a 75% preview reaches only the bottom 84 rows
				// of the lower tiles (the monitor samples no lower), each tile half video.
				Layout l;
				l.name = "menu: 75% preview + poster strip (straddles the tiles) [limit]";
				l.video = { { 0, 0, 3840, 1620 } };
				l.native = { { 200, 1660, 900, 2100 }, { 1100, 1660, 1800, 2100 }, { 2000, 1660, 2700, 2100 }, { 2900, 1660, 3600, 2100 } };
				cases.push_back({ l, false });
			}
			{
				Layout l;
				l.name = "menu: 75% preview (left 3/4) + side panel posters";
				l.video = { { 0, 0, 2880, 2160 } };
				l.native = { { 2960, 200, 3760, 1000 }, { 2960, 1300, 3760, 2100 } };
				cases.push_back({ l, true });
			}
			int failures = 0;
			for (const Case& c : cases)
			{
				Outcome on, off;
				Run({ { &c.layout, 6 } }, on, off);
				Report(c.layout.name.c_str(), on, off);
				// Per frame as well as declared: the tracker may lack shots on a menu (the preview's cuts
				// are diluted by the static UI), so the frame readings are the real check.
				if (c.guarded && (on.lowest < 1.0 || on.lower > 0))
				{
					++failures;
					Logger::WriteMessage("  FAILED: read or declared lower on a mixed screen with the guard on\n");
				}
				if (!c.guarded && on.lowest < 1.0) Logger::WriteMessage("  limit: declared lower with the guard on (see the case name)\n");
			}
			Assert::AreEqual(0, failures, L"a 4K menu with a lower-resolution preview is never declared lower (guard on)");
		}

		// (b) 4K UI drawn over a 1080 film: an info panel, a transport bar.
		TEST_METHOD(UiOverlayOverAFilmIsNeverReBased)
		{
			Layout panel = Film(0.5);
			panel.name = "info panel (thumbnail + text) over the top of a 1080 film";
			panel.dimmed = { { 0, 0, 3840, 900 } };
			panel.native = { { 200, 200, 900, 800 } };
			panel.text = { { 1100, 3700, 250, 64 }, { 1100, 3700, 420, 44 }, { 1100, 3700, 500, 44 }, { 1100, 3700, 580, 44 }, { 1100, 3700, 700, 44 } };
			Layout panelText = Film(0.5);
			panelText.name = "info panel (text only) over a 1080 film [limit]";
			panelText.dimmed = { { 0, 0, 3840, 900 } };
			panelText.text = { { 200, 3700, 250, 64 }, { 200, 3700, 420, 44 }, { 200, 3700, 500, 44 }, { 200, 3700, 580, 44 }, { 200, 3700, 700, 44 } };
			Layout transport = Film(0.5);
			transport.name = "transport bar in the lower tile row over a 1080 film [limit]";
			transport.dimmed = { { 0, 1450, 3840, 1800 } };
			transport.bars = { { 300, 1560, 3540, 1568 } };
			transport.text = { { 300, 800, 1600, 40 }, { 3040, 3540, 1600, 40 }, { 1200, 2640, 1660, 48 } };
			Layout transportLow = Film(0.5);
			transportLow.name = "transport bar at the frame's bottom (below the tiles) [limit]";
			transportLow.dimmed = { { 0, 1800, 3840, 2160 } };
			transportLow.bars = { { 300, 1900, 3540, 1908 } };
			transportLow.text = { { 300, 800, 1940, 40 }, { 3040, 3540, 1940, 40 }, { 1200, 2640, 2010, 48 } };
			{
				Outcome on, off;
				Run({ { &panel, 6 } }, on, off);
				Report(panel.name.c_str(), on, off);
				Assert::AreEqual(1.0, on.lowest, L"an info panel with native artwork over the film blocks the re-base");
				Assert::AreEqual(0, on.lower, L"no frame with the panel reads lower");
			}
			for (const Layout* l : { &panelText, &transport, &transportLow })
			{
				Outcome on, off;
				Run({ { l, 6 } }, on, off);
				Report(l->name.c_str(), on, off);
				if (on.lowest < 1.0) Logger::WriteMessage("  limit: declared lower with the guard on (a one-row band of UI reads like a subtitle, or the tiles never see it)\n");
			}
		}

		// The overlay appearing over a declared film: back to native on its first frame
		// (the tracker's "detail above: up at once"), and no re-base while it stays.
		TEST_METHOD(UiOverlayAppearingDisengagesAtOnce)
		{
			Layout film = Film(0.5);
			Layout panel = Film(0.5);
			panel.name = "info panel";
			panel.dimmed = { { 0, 0, 3840, 900 } };
			panel.native = { { 200, 200, 900, 800 } };
			panel.text = { { 1100, 3700, 250, 64 }, { 1100, 3700, 420, 44 }, { 1100, 3700, 580, 44 } };
			double beforeOn = 1.0, firstPanelOn = 0.0, firstPanelOff = 0.0, panelLowestOn = 1.0;
			Outcome on, off;
			Run({ { &film, 8 }, { &panel, 2 }, { &film, 2 } }, on, off, [&](size_t segment, int frame, double rOn, double rOff) {
				if (segment == 0) beforeOn = rOn;
				if (segment == 1 && frame == 0) { firstPanelOn = rOn; firstPanelOff = rOff; }
				if (segment == 1) panelLowestOn = (std::min)(panelLowestOn, rOn);
			});
			char line[200];
			snprintf(line, sizeof(line), "film declared %.3f; first info-panel frame: guard on %.3f, off %.3f; lowest during the panel (on) %.3f; at the end on %.3f off %.3f\n",
				beforeOn, firstPanelOn, firstPanelOff, panelLowestOn, on.declared, off.declared);
			Logger::WriteMessage(line);
			Assert::AreEqual(0.5, beforeOn, 0.03, L"the film is declared before the panel");
			Assert::AreEqual(1.0, firstPanelOn, L"native on the panel's first frame");
			Assert::AreEqual(1.0, panelLowestOn, L"native while the panel stays");
		}

		// (c) Picture-in-picture.
		TEST_METHOD(PictureInPictureIsNeverReBasedOverNativeContent)
		{
			Layout nativeMain = Film(1.0);
			nativeMain.name = "native 4K main + 1080 PiP (bottom-right quarter)";
			nativeMain.pip = { { 1920, 1080, 3840, 2160 } };
			nativeMain.pipRatio = 0.5;
			Layout nativePip = Film(0.5);
			nativePip.name = "1080 main + native 4K PiP (bottom-right quarter)";
			nativePip.pip = { { 1920, 1080, 3840, 2160 } };
			nativePip.pipRatio = 1.0;
			Layout smallPip = Film(0.5);
			smallPip.name = "1080 main + small native 4K PiP (one tile)";
			smallPip.pip = { { 3000, 1400, 3840, 1880 } };
			smallPip.pipRatio = 1.0;
			Layout lowerPip = Film(0.5);
			lowerPip.name = "1080 main + 720 PiP (bottom-right quarter)";
			lowerPip.pip = { { 1920, 1080, 3840, 2160 } };
			lowerPip.pipRatio = 720.0 / 2160.0;
			for (const Layout* l : { &nativeMain, &nativePip, &smallPip })
			{
				Outcome on, off;
				Run({ { l, 6 } }, on, off);
				Report(l->name.c_str(), on, off);
				Assert::AreEqual(1.0, on.lowest, (L"native content in the frame: never re-based - " + std::wstring(l->name.begin(), l->name.end())).c_str());
				Assert::AreEqual(0, on.lower, (L"no frame reads lower - " + std::wstring(l->name.begin(), l->name.end())).c_str());
			}
			Outcome on, off;
			Run({ { &lowerPip, 6 } }, on, off);
			Report(lowerPip.name.c_str(), on, off);
			// Nothing in the frame is native: re-basing to the main video's rate softens nothing,
			// but never below it (that would soften the main video).
			Assert::IsTrue(on.lowest >= 0.5 - 0.03, L"a lower-rate PiP never takes the frame below the main video's rate");
		}

		// The switch: off is the old reading, frame for frame (also checked on every frame of
		// every scenario above), and the defaults are on.
		TEST_METHOD(GuardIsSwitchableAndOnByDefault)
		{
			const SourceResolution::Thresholds defaults;
			Assert::IsTrue(defaults.mixedGuard, L"on by default");
			Layout l;
			l.name = "menu: 50% preview + poster row";
			l.video = { { 0, 0, 3840, 1080 } };
			l.native = kPosterRow;
			std::vector<std::vector<float>> cache;
			const std::vector<ShotTile> tiles = MakeShot(l, 0, cache);
			const std::vector<SourceResolution::Spectrum> spectra = AnalyseFrame(tiles, 0.0);
			SourceResolution::Thresholds off;
			off.mixedGuard = false;
			const SourceResolution::FrameReading rOn = SourceResolution::ReadFrame(spectra);
			const SourceResolution::FrameReading rOff = SourceResolution::ReadFrame(spectra, off);
			char line[200];
			snprintf(line, sizeof(line), "50%% menu, one frame: guard off ratio %.3f; on ratio %.3f mixed %d (picture %d overlay %d)\n",
				rOff.ratio, rOn.ratio, rOn.mixed ? 1 : 0, rOn.mixedPictureTiles, rOn.mixedOverlayTiles);
			Logger::WriteMessage(line);
			Assert::IsFalse(rOff.mixed);
			Assert::AreEqual(0, rOff.mixedPictureTiles + rOff.mixedOverlayTiles, L"off: the guard does not run");
			if (rOff.ratio < 1.0) Assert::IsTrue(rOn.mixed && rOn.ratio == 1.0, L"on: the lower reading is overruled");
		}
	};
}
