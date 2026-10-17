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

			// The dense grid (SourceResolution::PlaceTiles): one shot's sources at arbitrary tile
			// places (unclipped video / PiP, the native UI, the text with its coverage).
			struct PlacedShot
			{
				std::vector<SourceResolution::TilePlace> places;
				std::vector<ShotTile> tiles;
			};

			PlacedShot MakePlacedShot(const Layout& l, const std::vector<SourceResolution::TilePlace>& places, int shot)
			{
				const Scene videoScene(100u + 7u * static_cast<unsigned>(shot)), pipScene(300u + 7u * static_cast<unsigned>(shot));
				static const Scene uiScene(900u);
				PlacedShot out;
				out.places = places;
				for (size_t i = 0; i < places.size(); ++i)
				{
					const int ox = places[i].x0, oy = places[i].y0;
					ShotTile t;
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
					std::vector<double> native;
					if (needNative) native = Raw(uiScene, 1.0, ox, oy);
					for (size_t p = 0; p < t.map.size(); ++p)
					{
						switch (t.map[p])
						{
						case kNative: t.base[p] = Code(native[p]); break;
						case kBar: t.base[p] = static_cast<float>(SubtitleTextTestPattern::kWhite); break;
						case kVideo: t.base[p] = Code(t.video[p]); break;
						case kDimmed: t.base[p] = Code(t.video[p] * 0.35 + 0.04); break;
						case kPip: t.base[p] = Code(t.pip[p]); break;
						default: break;
						}
					}
					for (size_t k = 0; k < l.text.size(); ++k)
					{
						const Line& line = l.text[k];
						if (line.y0 + line.height + 12 < oy || line.y0 - 12 >= oy + kTile || line.x1 <= ox || line.x0 >= ox + kTile) continue;
						SubtitleTextTestPattern::Draw(t.base, kTile, kTile, line.x0 - ox, line.x1 - ox, line.y0 - oy, line.height,
							500u + static_cast<unsigned>(k), &t.coverage);
					}
					out.tiles.push_back(std::move(t));
				}
				return out;
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
						"first frame: tiles %d/%d text %d periodic %d ratio %.3f (v %.3f@%.3f h %.3f@%.3f floor v %.2f h %.2f) mixed %d picture %d overlay %d (%d rows x %d cols, ui %d, row most %d) overruled %.3f\n",
						g == 0 ? name : "", g == 0 ? "on" : "off", o.frames, o.informative, o.lower, o.mixed, o.declared, o.lowest, o.firstDeclared,
						r.informativeTiles, r.tiles, r.textTiles, r.periodicTiles, r.ratio, r.vertical.coherence, r.vertical.ratio, r.horizontal.coherence, r.horizontal.ratio,
						r.vertical.floor, r.horizontal.floor, r.mixed ? 1 : 0, r.mixedPictureTiles, r.mixedOverlayTiles, r.mixedOverlayRows, r.mixedOverlayColumns, r.mixedUiTiles, r.mixedRowMost, r.mixedRatio);
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
				// Text-only UI confined to one tile row: several lines per tile, not a subtitle band
				// (Thresholds::mixedRowGuard).
				Layout l;
				l.name = "menu: 50% preview (top half) + list text only";
				l.video = { { 0, 0, 3840, 1080 } };
				l.text = kListText;
				cases.push_back({ l, true });
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
			panelText.name = "info panel (text only, one tile row) over a 1080 film";
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

		// Intent review (2026-10-16): native UI confined to one tile row - a bottom-half EPG or info
		// panel over a 1080 film, a half-screen menu, a top info panel without artwork. The overlay
		// tiles never span two rows; they are mixed because they are not a subtitle band
		// (Thresholds::mixedRowGuard).
		TEST_METHOD(HalfScreenNativeUiOverUpscaleIsNeverReBased)
		{
			Layout epg = Film(0.5);
			epg.name = "bottom-half EPG (grid of entries, lines) over a 1080 film";
			epg.dimmed = { { 0, 1080, 3840, 2160 } };
			epg.bars = { { 0, 1470, 3840, 1473 }, { 0, 1630, 3840, 1633 } };   // grid rules
			epg.text = { { 200, 1150, 1500, 44 }, { 1300, 2250, 1500, 44 }, { 2350, 3300, 1500, 44 }, { 3350, 3800, 1500, 44 },
				{ 200, 1700, 1560, 36 }, { 1800, 3700, 1560, 36 },
				{ 200, 1150, 1660, 44 }, { 1300, 2250, 1660, 44 }, { 2350, 3300, 1660, 44 }, { 3350, 3800, 1660, 44 } };
			Layout menu = Film(0.5);
			menu.name = "half-screen list menu (bottom half) over a 1080 film";
			menu.dimmed = { { 0, 1080, 3840, 2160 } };
			menu.text = kListText;
			Layout info = Film(0.5);
			info.name = "info panel (text only, top tile row) over a 1080 film";
			info.dimmed = { { 0, 0, 3840, 900 } };
			info.text = { { 200, 3700, 250, 64 }, { 200, 3700, 420, 44 }, { 200, 3700, 500, 44 }, { 200, 3700, 580, 44 }, { 200, 3700, 700, 44 } };
			Layout side = Film(0.5);
			side.name = "right-half panel (text, one column of tiles per row) over a 1080 film";
			side.dimmed = { { 1920, 0, 3840, 2160 } };
			side.text = { { 2000, 3700, 430, 44 }, { 2000, 3700, 510, 44 }, { 2000, 3700, 590, 44 } };
			// Per case: never declared lower (strict), and at most one frame in twelve read lower. A
			// sporadic frame the guard misses (a panel tile's band share or detail at a threshold
			// on that frame's video) cannot re-base: any native frame in the tracker's window keeps
			// the candidate native (Tracker::Candidate is the window's highest reading).
			for (const Layout* l : { &epg, &menu, &info, &side })
			{
				Outcome on, off;
				Run({ { l, 6 } }, on, off);
				Report(l->name.c_str(), on, off);
				char line[256];
				snprintf(line, sizeof(line), "  %s: guard on - frames read lower %d of %d, mixed %d, lowest declared %.3f (guard off: read lower %d, lowest %.3f)\n",
					l->name.c_str(), on.lower, on.frames, on.mixed, on.lowest, off.lower, off.lowest);
				Logger::WriteMessage(line);
				const std::wstring name(l->name.begin(), l->name.end());
				Assert::AreEqual(1.0, on.lowest, (L"never declared lower: " + name).c_str());
				Assert::IsTrue(on.lower * 12 <= on.frames, (L"at most one frame in twelve reads lower: " + name).c_str());
			}
		}

		// The row rule's regression: subtitles - one or two lines in a band, top or bottom, wide,
		// in a player's translucent caption box - are still ignored (no mixed frame, declared as
		// without the guard).
		TEST_METHOD(SubtitleBandsAreStillIgnoredByTheRowRule)
		{
			std::vector<Layout> cases;
			auto add = [&](const char* name, std::vector<Line> text, std::vector<Rect> box = {}) {
				Layout l = Film(0.5);
				l.name = name;
				l.text = std::move(text);
				l.dimmed = std::move(box);
				cases.push_back(std::move(l));
			};
			add("subtitle: one line, bottom tile row", { { 900, 2940, 1610, 56 } });
			add("subtitle: two lines, bottom tile row", { { 1000, 2840, 1560, 56 }, { 900, 2940, 1640, 56 } });
			add("subtitle: two small lines (40 px)", { { 1100, 2740, 1580, 40 }, { 1000, 2840, 1640, 40 } });
			add("subtitle: one line, top tile row", { { 900, 2940, 520, 56 } });
			add("subtitle: one very wide line", { { 560, 3280, 1610, 56 } });
			add("subtitle: two lines in a translucent caption box", { { 1000, 2840, 1560, 56 }, { 900, 2940, 1640, 56 } }, { { 860, 1530, 2980, 1712 } });
			int failures = 0;
			for (const Layout& l : cases)
			{
				Outcome on, off;
				Run({ { &l, 6 } }, on, off);
				Report(l.name.c_str(), on, off);
				if (on.mixed > 0 || std::fabs(on.declared - off.declared) > 1e-9 || std::fabs(on.declared - 0.5) > 0.03)
				{
					++failures;
					Logger::WriteMessage("  FAILED: the subtitle changed the reading (mixed frames, or not declared 0.5)\n");
				}
			}
			Assert::AreEqual(0, failures, L"subtitle bands stay ignored with the row rule on");
		}

		// The switch: off is the old reading, frame for frame (also checked on every frame of
		// every scenario above), and the defaults are on.
		// The monitor's dense grid (gap 2): switching between the base 4 x 2 grid and the 16-tile
		// grid - here on every other frame, harsher than the monitor - is neither a cut nor a new
		// shot: the mean and signature come from the base tiles (Thresholds::signatureTiles), which
		// PlaceTiles puts first at the same places.
		TEST_METHOD(DenseGridSwitchIsNotACut)
		{
			int base = 0, baseDense = 0;
			const std::vector<SourceResolution::TilePlace> sparse = SourceResolution::PlaceTiles(0, 0, kWidth, kHeight, kTile, false, &base);
			const std::vector<SourceResolution::TilePlace> dense = SourceResolution::PlaceTiles(0, 0, kWidth, kHeight, kTile, true, &baseDense);
			Assert::AreEqual(8, static_cast<int>(sparse.size()));
			Assert::AreEqual(16, static_cast<int>(dense.size()));
			Assert::AreEqual(8, base);
			Assert::AreEqual(8, baseDense);
			for (size_t i = 0; i < sparse.size(); ++i)
			{
				int ox, oy;
				Origin(static_cast<int>(i), ox, oy);
				Assert::IsTrue(dense[i].x0 == sparse[i].x0 && dense[i].y0 == sparse[i].y0, L"the base tiles come first, at the same places");
				Assert::IsTrue(sparse[i].x0 == ox && sparse[i].y0 == oy, L"PlaceTiles is the monitor's 4 x 2 placement");
			}
			const Layout film = Film(0.5);
			SourceResolution::Thresholds tSparse, tDense, tDenseAll;
			SourceResolution::SetTileCentres(sparse, tSparse);
			tSparse.signatureTiles = base;
			SourceResolution::SetTileCentres(dense, tDense);
			tDense.signatureTiles = baseDense;
			tDenseAll = tDense;
			tDenseAll.signatureTiles = 0;   // the old signature over every tile (logged)
			SourceResolution::Tracker sparseOnly, switching, switchingAll;
			double t = 0.0;
			for (int shot = 0; shot < 6; ++shot)
			{
				const PlacedShot s = MakePlacedShot(film, dense, shot);
				for (int f = 0; f < 6; ++f, t += 0.5)
				{
					const double offset = (shot % 2 ? 0.2 : -0.2) + 3.0 * f / 1023.0;
					const std::vector<SourceResolution::Spectrum> all = AnalyseFrame(s.tiles, offset);
					const std::vector<SourceResolution::Spectrum> eight(all.begin(), all.begin() + 8);
					const SourceResolution::FrameReading r8 = SourceResolution::ReadFrame(eight, tSparse);
					const SourceResolution::FrameReading r16 = SourceResolution::ReadFrame(all, tDense);
					const SourceResolution::FrameReading r16all = SourceResolution::ReadFrame(all, tDenseAll);
					Assert::AreEqual(r8.mean, r16.mean, 0.0, L"the same frame has the same mean on either grid");
					Assert::AreEqual(r8.signature, r16.signature, 0.0, L"and the same signature");
					const bool useDense = f % 2 == 1;
					sparseOnly.Update(r8, t);
					switching.Update(useDense ? r16 : r8, t);
					switchingAll.Update(useDense ? r16all : r8, t);
				}
			}
			char line[200];
			snprintf(line, sizeof(line), "cuts: base grid only %llu (shots %d); switching every frame %llu (shots %d); switching without signatureTiles %llu\n",
				sparseOnly.Cuts(), sparseOnly.Shots(), switching.Cuts(), switching.Shots(), switchingAll.Cuts());
			Logger::WriteMessage(line);
			Assert::AreEqual(sparseOnly.Cuts(), switching.Cuts(), L"switching grids adds no cut");
			Assert::AreEqual(sparseOnly.Shots(), switching.Shots(), L"and no shot");
			// The declared value itself may differ in the last digits: the dense frames average 16
			// tiles' curves, not 8 (the sub-bin peak moves by ~1e-5). Within a thousandth (the
			// tracker's hysteresis is 0.03; the re-based height rounds to even lines).
			Assert::AreEqual(sparseOnly.Ratio(), switching.Ratio(), 1e-3, L"and declares the same (within 1e-3)");
		}

		// A small picture-in-picture between the base tiles: the base grid never sees it (the 1080
		// main reads 0.5 and would be re-based, softening the native PiP); the dense grid has a
		// tile inside it and the frame reads native. A two-line subtitle straddling the lower base
		// row and the bottom dense row stays ignored (the spread needs 0.3 of the height).
		TEST_METHOD(DenseGridCatchesASmallPipBetweenTheBaseTiles)
		{
			int base = 0;
			const std::vector<SourceResolution::TilePlace> sparse = SourceResolution::PlaceTiles(0, 0, kWidth, kHeight, kTile, false, &base);
			const std::vector<SourceResolution::TilePlace> dense = SourceResolution::PlaceTiles(0, 0, kWidth, kHeight, kTile, true);
			Layout pip = Film(0.5);
			pip.name = "1080 main + 880 x 495 native PiP between the base tiles";
			pip.pip = { { 1100, 820, 1980, 1315 } };
			pip.pipRatio = 1.0;
			const Rect& r = pip.pip.front();
			auto overlaps = [&](const SourceResolution::TilePlace& p) { return p.x0 < r.x1 && p.x0 + kTile > r.x0 && p.y0 < r.y1 && p.y0 + kTile > r.y0; };
			auto inside = [&](const SourceResolution::TilePlace& p) { return p.x0 >= r.x0 && p.x0 + kTile <= r.x1 && p.y0 >= r.y0 && p.y0 + kTile <= r.y1; };
			bool seenByBase = false, insideDense = false;
			for (const auto& p : sparse) seenByBase |= overlaps(p);
			for (const auto& p : dense) insideDense |= inside(p);
			Assert::IsFalse(seenByBase, L"fixture: no base tile touches the PiP");
			Assert::IsTrue(insideDense, L"fixture: a dense tile lies inside the PiP");
			SourceResolution::Thresholds tSparse, tDense;
			SourceResolution::SetTileCentres(sparse, tSparse);
			tSparse.signatureTiles = base;
			SourceResolution::SetTileCentres(dense, tDense);
			tDense.signatureTiles = base;
			Layout subtitle = Film(0.5);
			subtitle.text = { { 1000, 2840, 1690, 56 }, { 900, 2940, 1770, 56 } };
			for (int shot = 0; shot < 3; ++shot)
			{
				const PlacedShot s = MakePlacedShot(pip, dense, shot);
				std::vector<ShotTile> firstEight(s.tiles.begin(), s.tiles.begin() + base);
				const SourceResolution::FrameReading rBase = SourceResolution::ReadFrame(AnalyseFrame(firstEight, 0.0), tSparse);
				const SourceResolution::FrameReading rDense = SourceResolution::ReadFrame(AnalyseFrame(s.tiles, 0.0), tDense);
				const PlacedShot sub = MakePlacedShot(subtitle, dense, shot);
				const SourceResolution::FrameReading rSub = SourceResolution::ReadFrame(AnalyseFrame(sub.tiles, 0.0), tDense);
				char line[320];
				snprintf(line, sizeof(line), "shot %d: PiP base grid ratio %.3f mixed %d; dense ratio %.3f mixed %d (picture %d overlay %d, overruled %.3f); subtitle on the dense grid ratio %.3f mixed %d (overlay %d)\n",
					shot, rBase.ratio, rBase.mixed ? 1 : 0, rDense.ratio, rDense.mixed ? 1 : 0, rDense.mixedPictureTiles, rDense.mixedOverlayTiles, rDense.mixedRatio,
					rSub.ratio, rSub.mixed ? 1 : 0, rSub.mixedOverlayTiles);
				Logger::WriteMessage(line);
				Assert::IsTrue(!rBase.mixed && rBase.ratio < 0.9, L"the base grid misses the PiP and reads the main video's rate (the gap)");
				Assert::IsTrue(rDense.mixed && rDense.ratio == 1.0, L"the dense grid catches the PiP: native");
				Assert::IsFalse(rSub.mixed, L"a two-line subtitle across neighbouring dense rows is not mixed");
				Assert::AreEqual(0.5, rSub.ratio, 0.03, L"and the film still reads its rate");
			}
		}

		TEST_METHOD(GuardIsSwitchableAndOnByDefault)
		{
			const SourceResolution::Thresholds defaults;
			Assert::IsTrue(defaults.mixedGuard, L"on by default");
			// The fixture must give the guard something to overrule. The former one (a 50% preview
			// over a poster row) read native with the guard off as well - half its tiles are the
			// native posters - so the overrule check never ran (audit 2026-10-17). A 4K info panel
			// over a 1080 film reads the film's rate with the guard off on every shot (measured).
			Layout l = Film(0.5);
			l.name = "info panel (thumbnail + text) over the top of a 1080 film";
			l.dimmed = { { 0, 0, 3840, 900 } };
			l.native = { { 200, 200, 900, 800 } };
			std::vector<std::vector<float>> cache;
			SourceResolution::Thresholds off;
			off.mixedGuard = false;
			for (int shot = 0; shot < 3; ++shot)
			{
				const std::vector<ShotTile> tiles = MakeShot(l, shot, cache);
				const std::vector<SourceResolution::Spectrum> spectra = AnalyseFrame(tiles, 0.0);
				const SourceResolution::FrameReading rOn = SourceResolution::ReadFrame(spectra);
				const SourceResolution::FrameReading rOff = SourceResolution::ReadFrame(spectra, off);
				char line[200];
				snprintf(line, sizeof(line), "info panel over a film, shot %d: guard off ratio %.3f; on ratio %.3f mixed %d (picture %d overlay %d)\n",
					shot, rOff.ratio, rOn.ratio, rOn.mixed ? 1 : 0, rOn.mixedPictureTiles, rOn.mixedOverlayTiles);
				Logger::WriteMessage(line);
				Assert::IsFalse(rOff.mixed);
				Assert::AreEqual(0, rOff.mixedPictureTiles + rOff.mixedOverlayTiles, L"off: the guard does not run");
				// The precondition first: guard off reads the lower rate.
				Assert::IsTrue(rOff.ratio < 1.0, L"precondition: guard off reads the film's lower rate");
				Assert::IsTrue(rOn.mixed && rOn.ratio == 1.0, L"on: the lower reading is overruled");
			}
		}
	};
}
