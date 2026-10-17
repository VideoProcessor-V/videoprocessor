#include "pch.h"
#include "CppUnitTest.h"

#include <vprenderer/PulldownDetector.h>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Pulldown default-on corpus (docs/audit/pulldown-review-2026-10-16.md, "Test
// needed before default-on"). pulldown_fixes stays off by default until this
// passes with the fixes on.
//
// Real pictures: the native 120p clips (decimated by 5 to 24p, by 4 to 30p, by
// 2 to 60p video) and the 50p y4m clips (by 2 to 25p, native 50p video) in
// VP_TEST_REAL_FOOTAGE (default D:\hfr). Only the 64 rows Pulldown::Signature
// samples are read (every 8th sample kept), so the real grain decides what
// repeats. Each clip is capped at 3 s; shots follow each other (cuts), looping.
// Cadences come from exact rate ratios - no rate, cadence or resolution is
// fixed. Signals carry the film the way a source does (each signal frame shows
// the latest film frame due) with scenario events, then are replayed through
// Pulldown::Detector exactly as LibplaceboVideoRenderer feeds it, with
// pulldown_fixes on and off. Off must reproduce the 2026-10-14 behaviour: it is
// checked decision for decision against a copy of that detector (427e2c1e).
//
// Criteria (the review's):
//  C1 no new picture ever dropped
//  C2 zero locks in video, stills, twos or random content
//  C3 lock within FramesToLock + one cycle of a film segment's start
//  C4 no unlock through fades, black, cuts or subtitles
//  C5 locked (to the right cadence) >= 98% of film time
//  C6 wrong-time frames per phase event <= 2
//  C7 film clock monotonic, never early, spaced 1/f within +-1 ms under +-3 ms jitter
//  C8 every lock change marks the next kept frame (D1)

namespace VideoProcessorTest
{
	namespace PulldownCorpus
	{
	namespace
	{
		// ---- The 2026-10-14 detector (427e2c1e), verbatim logic: the reference
		// that pulldown_fixes off must reproduce. ----
		namespace Reference
		{
			struct Decision
			{
				bool drop = false, locked = false, relocked = false, update = false;
				double filmHz = 0.0;
				int cycle = 0, unique = 0;
				uint64_t filmIndex = 0;
			};

			inline bool IsEvenPattern(const std::string& pattern)
			{
				int unique = 0;
				for (char c : pattern)
					unique += c == 'N' ? 1 : 0;
				const int length = static_cast<int>(pattern.size());
				if (unique < 1 || unique >= length)
					return false;
				const int shortHold = length / unique, longHold = (length + unique - 1) / unique;
				for (int i = 0; i < length; ++i)
				{
					if (pattern[i] != 'N')
						continue;
					int hold = 1;
					while (pattern[(i + hold) % length] != 'N')
						++hold;
					if (hold < shortHold || hold > longHold)
						return false;
				}
				return true;
			}

			class Detector
			{
			public:
				static constexpr int kCyclesToLock = 3;
				static constexpr double kSecondsToLock = 2.0;
				static constexpr int kContradictionsToUnlock = 3;
				static constexpr double kContradictionSeconds = 1.0;
				static constexpr int kHistory = 2048;
				static constexpr double kMinFilmHz = 15.0;

				static int MaxCycle(double signalHz)
				{
					const int second = signalHz > 0.0 ? static_cast<int>(std::ceil(signalHz)) : 2;
					const int limit = kHistory / kCyclesToLock;
					return second < limit ? second : limit;
				}
				static int FramesToLock(int cycle, double signalHz)
				{
					int need = cycle * kCyclesToLock;
					const int seconds = signalHz > 0.0 ? static_cast<int>(std::ceil(signalHz * kSecondsToLock)) : 0;
					need = seconds > need ? seconds : need;
					return need < kHistory ? need : kHistory;
				}
				void Reset()
				{
					m_history.clear();
					Unlock();
					m_signalHz = 0.0;
				}
				Decision Next(bool repeat, bool gap, double signalHz)
				{
					if (gap || (m_signalHz > 0.0 && signalHz > 0.0 && std::fabs(signalHz - m_signalHz) > m_signalHz * 0.005))
						Reset();
					if (signalHz > 0.0 && m_signalHz <= 0.0)
						m_signalHz = signalHz;
					++m_frames;
					m_history.push_back(repeat ? 'R' : 'N');
					if (static_cast<int>(m_history.size()) > kHistory)
						m_history.pop_front();
					Decision decision;
					if (m_locked)
					{
						m_phase = (m_phase + 1) % static_cast<int>(m_pattern.size());
						const char expected = m_pattern[m_phase];
						const bool contradiction = expected == 'R' && !repeat;
						const uint64_t window = static_cast<uint64_t>(std::ceil((signalHz > 0.0 ? signalHz : 60.0) * kContradictionSeconds));
						while (!m_contradictions.empty() && m_frames - m_contradictions.front() >= window)
							m_contradictions.pop_front();
						if (contradiction)
							m_contradictions.push_back(m_frames);
						if (static_cast<int>(m_contradictions.size()) >= kContradictionsToUnlock)
							Unlock();
						else
							return Locked(decision, signalHz, expected == 'R' && repeat, false, contradiction);
					}
					const int size = static_cast<int>(m_history.size());
					const int maxCycle = MaxCycle(signalHz);
					for (int cycle = 2; cycle <= maxCycle; ++cycle)
					{
						const int need = FramesToLock(cycle, signalHz);
						if (size < need)
							break;
						bool periodic = true;
						for (int i = size - need + cycle; i < size && periodic; ++i)
							periodic = m_history[i] == m_history[i - cycle];
						if (!periodic)
							continue;
						const std::string pattern(m_history.end() - cycle, m_history.end());
						int unique = 0;
						for (char c : pattern)
							unique += c == 'N' ? 1 : 0;
						const double filmHz = signalHz * unique / cycle;
						if (!IsEvenPattern(pattern) || !(filmHz >= kMinFilmHz))
							break;
						m_locked = true;
						m_pattern = pattern;
						m_phase = cycle - 1;
						m_unique = unique;
						m_filmIndex = 0;
						m_contradictions.clear();
						return Locked(decision, signalHz, repeat && pattern[m_phase] == 'R', true, false);
					}
					return decision;
				}
				bool Locked() const { return m_locked; }

			private:
				Decision Locked(Decision& decision, double signalHz, bool drop, bool relocked, bool update)
				{
					decision.locked = true;
					decision.relocked = relocked;
					decision.update = update;
					decision.cycle = static_cast<int>(m_pattern.size());
					decision.unique = m_unique;
					decision.filmHz = signalHz * m_unique / decision.cycle;
					decision.drop = drop;
					if (!drop && !update)
						++m_filmIndex;
					decision.filmIndex = m_filmIndex;
					return decision;
				}
				void Unlock()
				{
					m_locked = false;
					m_pattern.clear();
					m_phase = 0;
					m_unique = 0;
					m_filmIndex = 0;
					m_contradictions.clear();
				}
				std::deque<char> m_history;
				std::deque<uint64_t> m_contradictions;
				uint64_t m_frames = 0;
				std::string m_pattern;
				bool m_locked = false;
				int m_phase = 0;
				int m_unique = 0;
				uint64_t m_filmIndex = 0;
				double m_signalHz = 0.0;
			};

			class FilmClock
			{
			public:
				static constexpr double kWindowSeconds = 2.0;
				void Reset() { m_offsets.clear(); }
				double Time(uint64_t filmIndex, double arrival, double filmHz)
				{
					if (!(filmHz > 0.0))
						return arrival;
					const double ideal = static_cast<double>(filmIndex) / filmHz;
					m_offsets.push_back({ ideal, arrival - ideal });
					while (m_offsets.size() > 1 && ideal - m_offsets.front().first > kWindowSeconds)
						m_offsets.pop_front();
					double anchor = m_offsets.front().second;
					for (const auto& offset : m_offsets)
						anchor = offset.second > anchor ? offset.second : anchor;
					return anchor + ideal;
				}

			private:
				std::deque<std::pair<double, double>> m_offsets;
			};
		}

		// ---- Helpers ----
		struct Rate
		{
			long long num = 0, den = 1;
			double Hz() const { return static_cast<double>(num) / static_cast<double>(den); }
		};

		std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

		std::string RateText(const Rate& rate)
		{
			char text[32];
			std::snprintf(text, sizeof(text), "%.3f", rate.Hz());
			return text;
		}

		uint64_t Mix(uint64_t x)
		{
			x += 0x9E3779B97F4A7C15ull;
			x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
			x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
			return x ^ (x >> 31);
		}

		std::string EnvValue(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string value;
			if (_dupenv_s(&v, &n, name) == 0 && v) value = v;
			std::free(v);
			return value;
		}

		double Seconds(std::chrono::steady_clock::time_point since)
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
		}

		// ---- Real footage: the sampled rows of each clip ----
		constexpr int kRows = 64;          // Pulldown::Signature's default
		constexpr int kColumnStep = 8;     // every 8th sample of a sampled row kept
		constexpr double kClipSeconds = 3.0;

		struct Clip
		{
			std::string name;
			double hz = 0.0;
			int width = 0, height = 0, bytesPerSample = 1;
			int black = 16, white = 235;
			int samples = 0;   // kept per row
			std::vector<std::vector<uint16_t>> frames;   // by source index; empty = not read
		};

		struct Library
		{
			std::vector<Clip> clips;
			std::map<long long, std::vector<std::pair<int, int>>> reels;   // picture mHz -> (clip, source)
			double loadSeconds = 0.0;
			mutable std::unordered_map<uint64_t, uint64_t> cache;

			static long long Key(double pictureHz) { return std::llround(pictureHz * 1000.0); }
			const std::vector<std::pair<int, int>>& Reel(double pictureHz) const
			{
				static const std::vector<std::pair<int, int>> none;
				const auto it = reels.find(Key(pictureHz));
				return it == reels.end() ? none : it->second;
			}
		};

		// The picture a signal frame carries: a source frame of a clip, faded
		// toward black (gain per mille) and with a device subtitle drawn over.
		struct Pic
		{
			int clip = 0, source = 0, gain = 1000, subtitle = 0;
			bool operator==(const Pic& o) const { return clip == o.clip && source == o.source && gain == o.gain && subtitle == o.subtitle; }
		};

		uint64_t Sign(const Library& library, const Pic& pic)
		{
			const uint64_t key = (static_cast<uint64_t>(pic.clip) << 56) ^ (static_cast<uint64_t>(pic.source) << 32) ^
				(static_cast<uint64_t>(pic.gain) << 16) ^ static_cast<uint64_t>(pic.subtitle);
			const auto hit = library.cache.find(key);
			if (hit != library.cache.end())
				return hit->second;
			const Clip& clip = library.clips[static_cast<size_t>(pic.clip)];
			const std::vector<uint16_t>& base = clip.frames[static_cast<size_t>(pic.source)];
			std::vector<uint16_t> rows(base.size());
			for (size_t i = 0; i < base.size(); ++i)
				rows[i] = static_cast<uint16_t>(clip.black + (static_cast<int>(base[i]) - clip.black) * pic.gain / 1000);
			if (pic.subtitle != 0)
			{
				// A text-like block over the sampled rows of the subtitle band
				// (rows 52..59 of 64: about 82-93% of the picture height).
				for (int r = 52; r < 60; ++r)
					for (int c = clip.samples / 4; c < clip.samples * 3 / 4; ++c)
						rows[static_cast<size_t>(r) * clip.samples + c] = static_cast<uint16_t>(
							(Mix(static_cast<uint64_t>(pic.subtitle) * 977u + static_cast<uint64_t>(r * 4099 + c / 6)) & 1) ? clip.white : clip.black);
			}
			const uint64_t signature = Pulldown::Signature(rows.data(), static_cast<size_t>(clip.samples) * sizeof(uint16_t), kRows);
			library.cache[key] = signature;
			return signature;
		}

		std::vector<std::string> ListFiles(const std::string& folder, const char* pattern)
		{
			std::vector<std::string> names;
			WIN32_FIND_DATAA data;
			HANDLE find = FindFirstFileA((folder + "\\" + pattern).c_str(), &data);
			if (find == INVALID_HANDLE_VALUE)
				return names;
			do
			{
				if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					names.push_back(data.cFileName);
			} while (FindNextFileA(find, &data));
			FindClose(find);
			std::sort(names.begin(), names.end());
			return names;
		}

		// Reads the footage once per process. The picture rates wanted decide
		// which source frames are read (a source rate that is a whole multiple).
		const Library* LoadFootage(std::string& folderOut)
		{
			static std::unique_ptr<Library> library;
			static bool tried = false;
			static std::string folder;
			folderOut = folder;
			if (tried)
				return library.get();
			tried = true;
			folder = EnvValue("VP_TEST_REAL_FOOTAGE");
			if (folder.empty())
				folder = "D:\\hfr";
			folderOut = folder;
			const auto start = std::chrono::steady_clock::now();
			const double pictureRates[] = { 24.0, 25.0, 30.0, 50.0, 60.0 };
			auto lib = std::unique_ptr<Library>(new Library());
			std::vector<std::string> files = ListFiles(folder, "*.yuv");
			for (const std::string& y4m : ListFiles(folder, "*.y4m"))
				files.push_back(y4m);
			for (const std::string& file : files)
			{
				Clip clip;
				clip.name = file;
				FILE* f = nullptr;
				if (fopen_s(&f, (folder + "\\" + file).c_str(), "rb") != 0 || !f)
					continue;
				long long header = 0, frameBytes = 0, frameHeader = 0;
				const bool y4m = file.size() > 4 && file.substr(file.size() - 4) == ".y4m";
				if (y4m)
				{
					std::string line;
					int ch;
					while ((ch = fgetc(f)) != EOF && ch != '\n')
						line += static_cast<char>(ch);
					header = static_cast<long long>(line.size()) + 1;
					std::istringstream tokens(line);
					std::string token;
					int fn = 0, fd = 1;
					while (tokens >> token)
					{
						if (token[0] == 'W') clip.width = std::atoi(token.c_str() + 1);
						else if (token[0] == 'H') clip.height = std::atoi(token.c_str() + 1);
						else if (token[0] == 'F' && sscanf_s(token.c_str() + 1, "%d:%d", &fn, &fd) == 2 && fd > 0) clip.hz = static_cast<double>(fn) / fd;
					}
					clip.bytesPerSample = 1;
					frameHeader = 6;   // "FRAME\n"
					frameBytes = static_cast<long long>(clip.width) * clip.height * 3 / 2 + frameHeader;
				}
				else
				{
					// "<name>_<W>x<H>_<fps>fps_420_<bits>bit_YUV.yuv"
					for (size_t p = 0; p < file.size(); ++p)
					{
						int w = 0, h = 0, fps = 0;
						if (sscanf_s(file.c_str() + p, "_%dx%d_%dfps", &w, &h, &fps) == 3)
						{
							clip.width = w; clip.height = h; clip.hz = fps;
							break;
						}
					}
					clip.bytesPerSample = file.find("_10bit") != std::string::npos ? 2 : 1;
					frameBytes = static_cast<long long>(clip.width) * clip.height * 3 / 2 * clip.bytesPerSample;
				}
				if (clip.width <= 0 || clip.height <= 0 || !(clip.hz > 0.0))
				{
					fclose(f);
					continue;
				}
				if (clip.bytesPerSample == 2) { clip.black = 64; clip.white = 940; }
				_fseeki64(f, 0, SEEK_END);
				const long long available = (_ftelli64(f) - header) / frameBytes;
				const int count = static_cast<int>((std::min)(available, std::llround(clip.hz * kClipSeconds)));
				std::vector<int> steps;
				for (double rate : pictureRates)
				{
					const double step = clip.hz / rate;
					if (step >= 1.0 - 1e-9 && std::fabs(step - std::round(step)) < 1e-9)
						steps.push_back(static_cast<int>(std::llround(step)));
				}
				if (steps.empty() || count <= 0)
				{
					fclose(f);
					continue;
				}
				clip.samples = clip.width / kColumnStep;
				clip.frames.resize(static_cast<size_t>(count));
				const size_t rowBytes = static_cast<size_t>(clip.width) * clip.bytesPerSample;
				std::vector<uint8_t> row(rowBytes);
				bool ok = true;
				for (int index = 0; index < count && ok; ++index)
				{
					bool wanted = false;
					for (int step : steps)
						wanted = wanted || index % step == 0;
					if (!wanted)
						continue;
					std::vector<uint16_t>& rows = clip.frames[static_cast<size_t>(index)];
					rows.resize(static_cast<size_t>(kRows) * clip.samples);
					const long long base = header + index * frameBytes + frameHeader;
					for (int r = 0; r < kRows && ok; ++r)
					{
						// The rows Pulldown::Signature samples of the full picture.
						const long long y = static_cast<long long>(2 * r + 1) * clip.height / (kRows * 2);
						ok = _fseeki64(f, base + y * static_cast<long long>(rowBytes), SEEK_SET) == 0 && fread(row.data(), 1, rowBytes, f) == rowBytes;
						for (int c = 0; ok && c < clip.samples; ++c)
						{
							const size_t x = static_cast<size_t>(c) * kColumnStep;
							rows[static_cast<size_t>(r) * clip.samples + c] = clip.bytesPerSample == 2 ?
								static_cast<uint16_t>(row[x * 2] | (row[x * 2 + 1] << 8)) : row[x];
						}
					}
				}
				fclose(f);
				if (!ok)
					continue;
				const int clipIndex = static_cast<int>(lib->clips.size());
				for (double rate : pictureRates)
				{
					const double step = clip.hz / rate;
					if (step < 1.0 - 1e-9 || std::fabs(step - std::round(step)) >= 1e-9)
						continue;
					const int s = static_cast<int>(std::llround(step));
					for (int index = 0; index < count; index += s)
						lib->reels[Library::Key(rate)].push_back({ clipIndex, index });
				}
				lib->clips.push_back(std::move(clip));
			}
			lib->loadSeconds = Seconds(start);
			if (!lib->clips.empty())
				library = std::move(lib);
			return library.get();
		}

		// ---- A signal with its ground truth ----
		enum class Kind { Film, Video, Still, Twos, Random };

		enum Tag : unsigned
		{
			TagFade = 1, TagBlack = 2, TagCut = 4, TagSubtitle = 8, TagEdit = 16, TagMiss = 32, TagRestart = 64,
			TagSlip = 128, TagSpike = 256, TagSegment = 512
		};

		const char* TagName(unsigned bit)
		{
			switch (bit)
			{
			case TagFade: return "fade";
			case TagBlack: return "black";
			case TagCut: return "cut";
			case TagSubtitle: return "subtitle";
			case TagEdit: return "edit";
			case TagMiss: return "miss";
			case TagRestart: return "restart";
			case TagSlip: return "slip";
			case TagSpike: return "spike";
			case TagSegment: return "segment change";
			default: return "unexplained";
			}
		}

		struct Frame
		{
			Pic pic;
			Kind kind = Kind::Film;
			bool cadenceNew = false;    // truth: the first signal frame of a film frame
			bool subtitleOnly = false;  // truth: only the device subtitle changed
			bool event = false;         // a phase event starts here (edit, miss, restart, subtitle)
			bool spike = false;
			bool slip = false;          // the source's own cadence slips here (a rate mismatch)
			unsigned tags = 0;
			int missedBefore = 0;       // counted capture misses just before it
			int segment = 0;
			double arrival = 0.0;
		};

		struct Segment
		{
			size_t start = 0, end = 0;
			Kind kind = Kind::Film;
			int unique = 0, cycle = 0;   // the cadence a lock should take (film)
			std::string name;
		};

		class Tape
		{
		public:
			Tape(const Library& library, Rate signal) : m_library(library), m_signal(signal) {}

			double SignalHz() const { return m_signal.Hz(); }
			const std::vector<Frame>& Frames() const { return m_frames; }
			const std::vector<Segment>& Segments() const { return m_segments; }

			// Film carried `offset` signal frames into its cadence (any phase).
			// restart: the same programme resumed after a pause (an event, not
			// a new segment). twos: animation drawn on twos.
			void StartFilm(Rate film, double pictureHz, long long offset, bool restart = false, bool twos = false)
			{
				m_film = film;
				m_reel = &m_library.Reel(pictureHz);
				m_position = offset;
				m_lastFilmFrame = -1;
				m_twos = twos;
				m_cadence.clear();
				m_lastSlip = -1000000;
				// The shortest cycle within 0.2% of film / signal: where the
				// source's own cadence breaks it, it slips.
				const double ratio = film.Hz() / m_signal.Hz();
				m_cycle = 0;
				m_unique = 0;
				for (int q = 1; q <= Pulldown::Detector::MaxCycle(m_signal.Hz()) && m_cycle == 0; ++q)
					if (std::fabs(std::round(ratio * q) / q - ratio) <= ratio * 0.002)
					{
						m_cycle = q;
						m_unique = static_cast<int>(std::llround(ratio * q));
					}
				if (restart)
				{
					m_shot += 37;   // resumes elsewhere in the programme
					m_eventPending = true;
					m_pendingTags |= TagRestart;
				}
				else
				{
					m_shot += 101;
					NewSegment(twos ? Kind::Twos : Kind::Film, (twos ? "twos " : "film ") + RateText(film));
				}
			}

			void Film(double seconds, int gainFrom = 1000, int gainTo = 1000, unsigned tags = 0)
			{
				const long long count = std::llround(seconds * m_signal.Hz());
				for (long long i = 0; i < count; ++i)
				{
					const long long n = (m_position++ * m_film.num * m_signal.den) / (m_film.den * m_signal.num);
					const bool filmNew = n != m_lastFilmFrame;
					m_lastFilmFrame = n;
					m_cadence.push_back(filmNew);
					const long long at = static_cast<long long>(m_cadence.size()) - 1;
					if (m_cycle > 0 && at - m_cycle >= 1 && m_cadence[static_cast<size_t>(at)] != m_cadence[static_cast<size_t>(at - m_cycle)] && at - m_lastSlip > m_cycle)
					{
						m_slipPending = true;
						m_lastSlip = at;
					}
					// Twos: the drawing changes every second film frame.
					const long long drawn = m_twos ? n - (n & 1) : n;
					const bool cadenceNew = filmNew && drawn != m_lastDrawn;
					m_lastDrawn = drawn;
					if (filmNew)
					{
						if (m_cutPending)
						{
							m_shot += 53;
							m_cutPending = false;
							m_pendingTags |= TagCut;
						}
						m_gain = gainFrom + static_cast<int>(std::lround((gainTo - gainFrom) * static_cast<double>(i) / static_cast<double>(count)));
					}
					Frame frame;
					frame.kind = m_twos ? Kind::Twos : Kind::Film;
					frame.cadenceNew = cadenceNew;
					frame.pic = ReelPic(drawn + m_shot);
					frame.pic.gain = m_gain;
					frame.tags = tags;
					// A shot change in the reel is a cut.
					if (cadenceNew && m_lastClip >= 0 && (frame.pic.clip != m_lastClip || frame.pic.source < m_lastSource))
						frame.tags |= TagCut;
					if (cadenceNew)
					{
						m_lastClip = frame.pic.clip;
						m_lastSource = frame.pic.source;
					}
					if (m_subtitlePending && !filmNew)
					{
						++m_subtitle;
						frame.subtitleOnly = true;
						frame.event = true;
						frame.tags |= TagSubtitle;
						m_subtitlePending = false;
					}
					frame.pic.subtitle = m_subtitle;
					if (m_dropPending > 0)
					{
						--m_dropPending;   // an edit took this frame out of the programme
						m_eventPending = true;
						m_pendingTags |= TagEdit;
						continue;
					}
					if (m_missPending > 0)
					{
						--m_missPending;   // lost in capture: the renderer counts it
						++m_missed;
						m_eventPending = true;
						m_pendingTags |= TagMiss;
						continue;
					}
					Push(frame);
				}
			}

			void Cut() { m_cutPending = true; }
			void Subtitle() { m_subtitlePending = true; }
			void EditDrop() { ++m_dropPending; }
			void Miss(int frames = 1) { m_missPending += frames; }

			// An edit that repeats the last signal frame once.
			void EditDuplicate()
			{
				Frame frame = m_frames.back();
				frame.cadenceNew = false;
				frame.subtitleOnly = false;
				frame.missedBefore = 0;
				frame.spike = false;
				frame.slip = false;
				frame.event = true;
				frame.tags = TagEdit;
				m_frames.push_back(frame);
			}

			// Paused: the last picture held.
			void Pause(double seconds)
			{
				const long long count = std::llround(seconds * m_signal.Hz());
				for (long long i = 0; i < count; ++i)
				{
					Frame frame = m_frames.back();
					frame.kind = Kind::Still;
					frame.cadenceNew = false;
					frame.subtitleOnly = false;
					frame.event = false;
					frame.slip = false;
					frame.spike = false;
					frame.tags = 0;
					frame.missedBefore = 0;
					m_frames.push_back(frame);
				}
			}

			// Native video at `pictureHz` (= the signal rate): every frame new.
			// holdMax > 1: random content, each picture held 1..holdMax frames.
			void Video(double seconds, double pictureHz, int holdMax = 1, uint64_t seed = 1)
			{
				m_reel = &m_library.Reel(pictureHz);
				m_shot += 211;
				NewSegment(holdMax > 1 ? Kind::Random : Kind::Video, holdMax > 1 ? "random" : "video " + RateText(m_signal));
				const long long count = std::llround(seconds * m_signal.Hz());
				uint64_t state = seed;
				long long picture = 0;
				int held = 0, hold = 1;
				for (long long i = 0; i < count; ++i)
				{
					if (held >= hold)
					{
						++picture;
						held = 0;
						state = Mix(state);
						hold = holdMax > 1 ? 1 + static_cast<int>(state % static_cast<uint64_t>(holdMax)) : 1;
					}
					++held;
					Frame frame;
					frame.kind = holdMax > 1 ? Kind::Random : Kind::Video;
					frame.cadenceNew = held == 1;
					frame.pic = ReelPic(picture + m_shot);
					frame.pic.subtitle = m_subtitle;
					Push(frame);
				}
			}

			// Arrival times: one signal period per slot (counted misses take
			// their slots), +-jitterMs uniform, one late spike.
			void Finish(uint64_t seed, double jitterMs, double spikeSeconds = -1.0, double spikeMs = 0.0)
			{
				uint64_t state = seed;
				long long slot = 0;
				const size_t spikeFrame = spikeSeconds >= 0.0 ? static_cast<size_t>(spikeSeconds * m_signal.Hz()) : static_cast<size_t>(-1);
				for (size_t i = 0; i < m_frames.size(); ++i)
				{
					slot += 1 + m_frames[i].missedBefore;
					state = Mix(state);
					const double u = static_cast<double>(state >> 11) / 9007199254740992.0;
					double jitter = (u * 2.0 - 1.0) * jitterMs * 1e-3;
					if (i == spikeFrame)
					{
						jitter += spikeMs * 1e-3;
						m_frames[i].spike = true;
						m_frames[i].tags |= TagSpike;
					}
					m_frames[i].arrival = 10.0 + static_cast<double>(slot) * static_cast<double>(m_signal.den) / static_cast<double>(m_signal.num) + jitter;
				}
				if (!m_segments.empty())
					m_segments.back().end = m_frames.size();
			}

		private:
			Pic ReelPic(long long index) const
			{
				const auto& reel = *m_reel;
				const auto& entry = reel[static_cast<size_t>(((index % static_cast<long long>(reel.size())) + static_cast<long long>(reel.size())) % static_cast<long long>(reel.size()))];
				Pic pic;
				pic.clip = entry.first;
				pic.source = entry.second;
				return pic;
			}

			void NewSegment(Kind kind, const std::string& name)
			{
				if (!m_segments.empty())
				{
					m_segments.back().end = m_frames.size();
					m_pendingTags |= TagSegment;
					m_eventPending = true;   // the content's cadence restarts at its own phase
				}
				Segment segment;
				segment.start = m_frames.size();
				segment.kind = kind;
				segment.unique = m_unique;
				segment.cycle = m_cycle;
				segment.name = name;
				m_segments.push_back(segment);
			}

			void Push(Frame frame)
			{
				frame.missedBefore = m_missed;
				m_missed = 0;
				frame.slip = m_slipPending;
				if (m_slipPending)
					frame.tags |= TagSlip;
				m_slipPending = false;
				if (m_eventPending)
				{
					frame.event = true;
					m_eventPending = false;
				}
				frame.tags |= m_pendingTags;
				m_pendingTags = 0;
				frame.segment = static_cast<int>(m_segments.size()) - 1;
				m_frames.push_back(frame);
			}

			const Library& m_library;
			Rate m_signal;
			Rate m_film{ 24, 1 };
			const std::vector<std::pair<int, int>>* m_reel = nullptr;
			long long m_position = 0, m_lastFilmFrame = -1, m_lastDrawn = -1, m_shot = 0;
			int m_cycle = 0, m_unique = 0;
			bool m_twos = false;
			std::vector<bool> m_cadence;
			long long m_lastSlip = -1000000;
			bool m_slipPending = false;
			int m_gain = 1000, m_subtitle = 0;
			int m_lastClip = -1, m_lastSource = 0;
			bool m_cutPending = false, m_subtitlePending = false, m_eventPending = false;
			unsigned m_pendingTags = 0;
			int m_dropPending = 0, m_missPending = 0, m_missed = 0;
			std::vector<Frame> m_frames;
			std::vector<Segment> m_segments;
		};

		// ---- The renderer's use of the detector ----
		struct Out
		{
			bool drop = false, locked = false, relocked = false, update = false, rephased = false, lockChanged = false;
			int cycle = 0, unique = 0;
			double filmHz = 0.0;
			uint64_t filmIndex = 0;
			bool repeat = false;
			bool kept = false;
			bool marked = false;   // a kept frame queued with gapBefore (no motion pair across it)
			bool timed = false;
			double filmTime = 0.0;
		};

		// As LibplaceboVideoRenderer::OnCapture feeds it (733220a3): repeat =
		// signature equal to the previous captured frame; with the fixes a
		// counted miss passes its count, without them any gap resets; a lock
		// change sets gapBefore and resets the clock; a dropped repeat returns
		// early (with the fixes its gap is marked for the next kept frame);
		// every kept frame consumes the pending discontinuity (used only with
		// the fixes); an update keeps its film frame's time.
		std::vector<Out> Replay(const Library& library, const std::vector<Frame>& frames, double signalHz, bool fixes)
		{
			Pulldown::Detector detector;
			Pulldown::Settings settings;
			settings.keepLockAcrossMisses = settings.rephaseInPlace = settings.cadenceAwareClock = fixes;
			detector.SetSettings(settings);
			Pulldown::FilmClock clock;
			clock.SetRebaseOnRateChange(true);   // timing_continuity_fixes default
			std::vector<Out> out(frames.size());
			uint64_t previous = 0;
			double lastFilmTime = 0.0;
			bool haveFilmTime = false;
			for (size_t i = 0; i < frames.size(); ++i)
			{
				const Frame& frame = frames[i];
				const uint64_t signature = Sign(library, frame.pic);
				Out& o = out[i];
				o.repeat = signature != 0 && signature == previous;
				previous = signature;
				const bool gap = frame.missedBefore > 0;
				const bool wasLocked = detector.Locked();
				const Pulldown::Decision d = detector.Next(o.repeat, gap, signalHz, gap && fixes ? frame.missedBefore : -1);
				o.drop = d.drop; o.locked = d.locked; o.relocked = d.relocked; o.update = d.update; o.rephased = d.rephased;
				o.cycle = d.cycle; o.unique = d.unique; o.filmHz = d.filmHz; o.filmIndex = d.filmIndex;
				o.lockChanged = d.lockChanged;
				bool gapBefore = gap;
				if (d.locked != wasLocked || d.relocked)
				{
					gapBefore = true;
					clock.Reset();
				}
				if (d.drop)
				{
					if (gapBefore && fixes)
						detector.MarkDiscontinuity();
					continue;
				}
				o.kept = true;
				if (detector.ConsumeDiscontinuity() && fixes)
					gapBefore = true;
				o.marked = gapBefore;
				if (d.locked)
				{
					if (d.update && haveFilmTime)
						o.filmTime = lastFilmTime;
					else
					{
						o.timed = true;
						o.filmTime = fixes ? clock.Time(d.filmIndex, frame.arrival, d.filmHz, d.cadenceLateness, d.cadenceLatenessMax) :
							clock.Time(d.filmIndex, frame.arrival, d.filmHz);
						lastFilmTime = o.filmTime;
						haveFilmTime = true;
					}
				}
				else
					haveFilmTime = false;
			}
			return out;
		}

		// The 2026-10-14 renderer: the reference detector and clock; a lock
		// change marks only the frame it lands on.
		std::vector<Out> ReplayReference(const Library& library, const std::vector<Frame>& frames, double signalHz)
		{
			Reference::Detector detector;
			Reference::FilmClock clock;
			std::vector<Out> out(frames.size());
			uint64_t previous = 0;
			double lastFilmTime = 0.0;
			bool haveFilmTime = false;
			for (size_t i = 0; i < frames.size(); ++i)
			{
				const Frame& frame = frames[i];
				const uint64_t signature = Sign(library, frame.pic);
				Out& o = out[i];
				o.repeat = signature != 0 && signature == previous;
				previous = signature;
				const bool gap = frame.missedBefore > 0;
				const bool wasLocked = detector.Locked();
				const Reference::Decision d = detector.Next(o.repeat, gap, signalHz);
				o.drop = d.drop; o.locked = d.locked; o.relocked = d.relocked; o.update = d.update;
				o.cycle = d.cycle; o.unique = d.unique; o.filmHz = d.filmHz; o.filmIndex = d.filmIndex;
				o.lockChanged = d.locked != wasLocked || d.relocked;
				bool gapBefore = gap;
				if (o.lockChanged)
				{
					gapBefore = true;
					clock.Reset();
				}
				if (d.drop)
					continue;
				o.kept = true;
				o.marked = gapBefore;
				if (d.locked)
				{
					if (d.update && haveFilmTime)
						o.filmTime = lastFilmTime;
					else
					{
						o.timed = true;
						o.filmTime = clock.Time(d.filmIndex, frame.arrival, d.filmHz);
						lastFilmTime = o.filmTime;
						haveFilmTime = true;
					}
				}
				else
					haveFilmTime = false;
			}
			return out;
		}

		// Decision for decision (and the film times) equal: the switch off is
		// today's behaviour. Returns the first differing frame, or -1.
		long long FirstDifference(const std::vector<Out>& a, const std::vector<Out>& b)
		{
			for (size_t i = 0; i < a.size(); ++i)
			{
				const Out& x = a[i];
				const Out& y = b[i];
				if (x.repeat != y.repeat || x.drop != y.drop || x.locked != y.locked || x.relocked != y.relocked || x.update != y.update ||
					x.cycle != y.cycle || x.unique != y.unique || x.filmIndex != y.filmIndex || x.filmHz != y.filmHz ||
					x.lockChanged != y.lockChanged || x.marked != y.marked || x.timed != y.timed || std::fabs(x.filmTime - y.filmTime) > 1e-9)
					return static_cast<long long>(i);
			}
			return -1;
		}

		// ---- The criteria ----
		struct Result
		{
			int newDropped = 0;                                  // C1
			int falseLocks = 0;                                  // C2
			int segmentsChecked = 0, lateLocks = 0;              // C3
			int unlocksForbidden = 0, unlocksOther = 0;          // C4 (fade/black/cut/subtitle; the rest reported)
			std::map<std::string, int> unlockCauses;
			long long filmFrames = 0, filmLocked = 0;            // C5
			int events = 0, worstEventWrong = 0, strayWrong = 0, totalWrong = 0;   // C6
			int clockBackwards = 0, clockEarly = 0, spacingChecked = 0, spacingOff = 0;   // C7
			double worstSpacingMs = 0.0;
			int lockChanges = 0, onDrops = 0, unmarked = 0, rephases = 0;   // C8
			std::string wrongDetail;   // the wrong-time frames (first 24): where and what
			// Cadence changes (24p <-> 30p segments): frames still locked to the
			// previous cadence, the wrong-time ones among them, the longest hold.
			int oldCadenceFrames = 0, oldCadenceWrong = 0, oldCadenceLongest = 0;

			double LockedShare() const { return filmFrames ? static_cast<double>(filmLocked) / static_cast<double>(filmFrames) : 1.0; }
			bool Pass(int c) const
			{
				switch (c)
				{
				case 1: return newDropped == 0;
				case 2: return falseLocks == 0;
				case 3: return lateLocks == 0;
				case 4: return unlocksForbidden == 0;
				case 5: return filmFrames == 0 || filmLocked * 100 >= filmFrames * 98;
				case 6: return worstEventWrong <= 2 && strayWrong == 0;
				case 7: return clockBackwards == 0 && clockEarly == 0 && spacingOff == 0;
				case 8: return unmarked == 0;
				default: return false;
				}
			}
			std::string Causes() const
			{
				std::string text;
				for (const auto& cause : unlockCauses)
					text += (text.empty() ? "" : ",") + cause.first + ":" + std::to_string(cause.second);
				return text.empty() ? "-" : text;
			}
			std::string Text() const
			{
				char text[512];
				std::snprintf(text, sizeof(text),
					"C1 newDropped %d | C2 falseLocks %d | C3 late %d/%d | C4 unlocks fade/black/cut/sub %d other %d [%s] | C5 locked %.2f%% | "
					"C6 events %d worst %d stray %d total %d | C7 back %d early %d spacing %d/%d worst %.3f ms | C8 changes %d (on drops %d) unmarked %d | rephases %d",
					newDropped, falseLocks, lateLocks, segmentsChecked, unlocksForbidden, unlocksOther, Causes().c_str(), 100.0 * LockedShare(),
					events, worstEventWrong, strayWrong, totalWrong, clockBackwards, clockEarly, spacingOff, spacingChecked, worstSpacingMs,
					lockChanges, onDrops, unmarked, rephases);
				std::string result = text;
				if (oldCadenceFrames > 0)
					result += " | cadence change: " + std::to_string(oldCadenceFrames) + " frames on the old lock (" + std::to_string(oldCadenceWrong) +
						" wrong-time), longest to frame " + std::to_string(oldCadenceLongest) + " of its segment";
				return result;
			}
		};

		// A frame decided at the wrong film time while locked: a new film frame
		// shown as an update; a subtitle change taken for a new film frame; a
		// repeat slot taken for a new film frame; a film frame's own slot dropped.
		bool WrongTime(const Frame& frame, const Out& o)
		{
			if (!o.locked || frame.kind != Kind::Film)
				return false;
			if (frame.subtitleOnly)
				return !o.update;
			if (!o.repeat)
				return o.update;
			if (!frame.cadenceNew)
				return !o.drop && !o.update;
			return o.drop;
		}

		Result Evaluate(const Tape& tape, const std::vector<Out>& run)
		{
			Result r;
			const std::vector<Frame>& frames = tape.Frames();
			const std::vector<Segment>& segments = tape.Segments();
			const double signalHz = tape.SignalHz();
			const size_t n = frames.size();
			const size_t second = static_cast<size_t>(std::ceil(signalHz));

			// C1, C8, unlocks.
			bool expectMark = false;
			std::vector<long long> segmentLock(segments.size(), -1);   // first right-cadence lock in the segment
			for (size_t i = 0; i < n; ++i)
			{
				const Out& o = run[i];
				const Frame& frame = frames[i];
				const Segment& segment = segments[static_cast<size_t>(frame.segment)];
				if (o.drop && !o.repeat)
					++r.newDropped;
				if (o.lockChanged)
				{
					++r.lockChanges;
					r.onDrops += o.drop ? 1 : 0;
					expectMark = true;
				}
				if (o.kept)
				{
					if (expectMark && !o.marked)
						++r.unmarked;
					expectMark = false;
				}
				r.rephases += o.rephased ? 1 : 0;
				const bool film = segment.kind == Kind::Film;
				const bool right = o.locked && o.cycle == segment.cycle && o.unique == segment.unique;
				if (film && right && segmentLock[static_cast<size_t>(frame.segment)] < 0)
					segmentLock[static_cast<size_t>(frame.segment)] = static_cast<long long>(i);
				// C2: a lock taken inside non-film content, or video / random still
				// locked a second after it began.
				if (!film)
				{
					if (o.relocked)
						++r.falseLocks;
					if ((segment.kind == Kind::Video || segment.kind == Kind::Random) && o.locked && i >= segment.start + second)
						++r.falseLocks;
				}
				// C4: an unlock inside a film segment once it had its lock, with its cause.
				if (i > 0 && run[i - 1].locked && !o.locked && film && segmentLock[static_cast<size_t>(frame.segment)] >= 0)
				{
					const size_t window = (std::max)(static_cast<size_t>(3 * (segment.cycle > 0 ? segment.cycle : 1) + 3), second + 3);
					// Every tag in the window; the structural events first (a natural
					// cut every few seconds must not take the blame for an edit).
					unsigned seen = 0;
					for (size_t k = i + 1; k-- > 0 && i - k <= window;)
						seen |= frames[k].tags;
					static const unsigned priority[] = { TagSegment, TagEdit, TagMiss, TagRestart, TagSlip, TagSubtitle, TagSpike, TagFade, TagBlack, TagCut };
					unsigned cause = 0;
					for (unsigned bit : priority)
						if (!cause && (seen & bit))
							cause = bit;
					const std::string name = cause ? TagName(cause) : "unexplained";
					r.unlockCauses[name] += 1;
					if (cause & (TagFade | TagBlack | TagCut | TagSubtitle))
						++r.unlocksForbidden;
					else
						++r.unlocksOther;
				}
			}

			// C3 and C5 per film segment: acquisition from the segment start or the
			// source's last slip before the lock (a slip restarts the exact window).
			for (size_t s = 0; s < segments.size(); ++s)
			{
				const Segment& segment = segments[s];
				if (segment.kind != Kind::Film || segment.cycle <= 0)
					continue;
				const long long need = Pulldown::Detector::FramesToLock(segment.cycle, signalHz) + segment.cycle;
				long long acquireFrom = static_cast<long long>(segment.start);
				const long long lockAt = segmentLock[s];
				for (long long i = acquireFrom; i < (lockAt >= 0 ? lockAt : static_cast<long long>(segment.end)) && i < acquireFrom + need; ++i)
					acquireFrom = frames[static_cast<size_t>(i)].slip ? i : acquireFrom;
				if (static_cast<long long>(segment.end) - acquireFrom < need)
					continue;   // too short to judge
				++r.segmentsChecked;
				if (lockAt < 0 || lockAt + 1 > acquireFrom + need)
					++r.lateLocks;
				for (size_t i = static_cast<size_t>(acquireFrom + need); i < segment.end; ++i)
				{
					if (frames[i].kind != Kind::Film)
						continue;
					++r.filmFrames;
					r.filmLocked += run[i].locked && run[i].cycle == segment.cycle && run[i].unique == segment.unique ? 1 : 0;
				}
			}

			// C6: a wrong-time frame belongs to the latest phase event at most 3
			// frames after it and 3 cycles + 3 frames before it.
			std::vector<long long> events;
			for (size_t i = 0; i < n; ++i)
				if (frames[i].event || frames[i].spike || frames[i].slip)
					events.push_back(static_cast<long long>(i));
			r.events = static_cast<int>(events.size());
			std::vector<int> perEvent(events.size(), 0);
			for (size_t i = 0; i < n; ++i)
			{
				const Segment& segment = segments[static_cast<size_t>(frames[i].segment)];
				// Still locked to the previous content's cadence (a cadence change,
				// 24p <-> 30p): not a phase event - measured on its own, bounded by
				// the 3-contradictions-in-1-s release.
				if (run[i].locked && segment.kind == Kind::Film && (run[i].cycle != segment.cycle || run[i].unique != segment.unique))
				{
					++r.oldCadenceFrames;
					r.oldCadenceWrong += WrongTime(frames[i], run[i]) ? 1 : 0;
					r.oldCadenceLongest = (std::max)(r.oldCadenceLongest, static_cast<int>(i - segment.start) + 1);
					continue;
				}
				if (!WrongTime(frames[i], run[i]))
					continue;
				++r.totalWrong;
				if (r.totalWrong <= 24)
				{
					char text[200];
					std::snprintf(text, sizeof(text), "  wrong %zu (%s +%zu): repeat %d new %d sub %d -> drop %d update %d locked %d/%d rephased %d\n",
						i, segment.name.c_str(), i - segment.start, run[i].repeat ? 1 : 0, frames[i].cadenceNew ? 1 : 0, frames[i].subtitleOnly ? 1 : 0,
						run[i].drop ? 1 : 0, run[i].update ? 1 : 0, run[i].unique, run[i].cycle, run[i].rephased ? 1 : 0);
					r.wrongDetail += text;
				}
				const long long window = 3LL * (segment.cycle > 0 ? segment.cycle : 1) + 3;
				int owner = -1;
				for (size_t e = 0; e < events.size(); ++e)
					if (events[e] <= static_cast<long long>(i) + 3 && static_cast<long long>(i) <= events[e] + window)
						owner = static_cast<int>(e);
				if (owner >= 0)
					++perEvent[static_cast<size_t>(owner)];
				else
					++r.strayWrong;
			}
			for (int count : perEvent)
				r.worstEventWrong = (std::max)(r.worstEventWrong, count);

			// C7: never backwards, never before arrival; in quiet stretches (no
			// event, lock change, miss or segment change for the clock window +
			// 0.5 s) spaced exactly 1/f per film frame within 1 ms.
			const double settle = Pulldown::FilmClock::kWindowSeconds + 0.5;
			double lastDisturbance = -1e9, lastTime = 0.0;
			uint64_t lastIndex = 0;
			bool haveLast = false;
			for (size_t i = 0; i < n; ++i)
			{
				const Out& o = run[i];
				if (frames[i].event || frames[i].spike || frames[i].slip || o.lockChanged || frames[i].missedBefore > 0 || (frames[i].tags & TagSegment))
					lastDisturbance = frames[i].arrival;
				if (o.relocked || !o.locked)
					haveLast = false;
				if (!o.timed)
					continue;
				const double t = o.filmTime;
				if (t < frames[i].arrival - 1e-9)
					++r.clockEarly;
				if (haveLast)
				{
					if (t <= lastTime)
						++r.clockBackwards;
					if (frames[i].arrival - lastDisturbance >= settle && o.filmIndex > lastIndex)
					{
						++r.spacingChecked;
						const double errorMs = std::fabs((t - lastTime) - static_cast<double>(o.filmIndex - lastIndex) / o.filmHz) * 1e3;
						r.worstSpacingMs = (std::max)(r.worstSpacingMs, errorMs);
						r.spacingOff += errorMs > 1.0 ? 1 : 0;
					}
				}
				lastTime = t;
				lastIndex = o.filmIndex;
				haveLast = true;
			}
			return r;
		}

		// Per-criterion tally across streams.
		struct Tally
		{
			int streams = 0;
			int fails[9] = {};
			void Add(const Result& r)
			{
				++streams;
				for (int c = 1; c <= 8; ++c)
					fails[c] += r.Pass(c) ? 0 : 1;
			}
			std::string Text(const char* label) const
			{
				std::string text = std::string(label) + " (" + std::to_string(streams) + " streams) failing:";
				for (int c = 1; c <= 8; ++c)
					text += " C" + std::to_string(c) + "=" + std::to_string(fails[c]);
				return text;
			}
		};

		struct Run
		{
			Result on, off;
		};

		// Replays a tape with the fixes on and off; off checked against the
		// 2026-10-14 reference decision for decision.
		Run ReplayBoth(const Library& library, const Tape& tape, const std::string& name, Tally& onTally, Tally& offTally)
		{
			Run run;
			const auto on = Replay(library, tape.Frames(), tape.SignalHz(), true);
			const auto off = Replay(library, tape.Frames(), tape.SignalHz(), false);
			const auto reference = ReplayReference(library, tape.Frames(), tape.SignalHz());
			const long long difference = FirstDifference(off, reference);
			Assert::AreEqual(-1LL, difference, Wide(name + ": pulldown_fixes off reproduces the 2026-10-14 behaviour (first differing frame)").c_str());
			run.on = Evaluate(tape, on);
			run.off = Evaluate(tape, off);
			onTally.Add(run.on);
			offTally.Add(run.off);
			Logger::WriteMessage((name + " [on ] " + run.on.Text() + "\n").c_str());
			Logger::WriteMessage((name + " [off] " + run.off.Text() + "\n").c_str());
			if (!run.on.Pass(6))
				Logger::WriteMessage((name + " [on ] wrong-time frames:\n" + run.on.wrongDetail).c_str());
			return run;
		}

		void AssertCriteria(const Result& r, const std::string& name, const int* criteria, size_t count)
		{
			static const wchar_t* text[] = { L"", L"C1 no new picture dropped", L"C2 zero false locks", L"C3 lock within FramesToLock + one cycle",
				L"C4 no unlock through fades, black, cuts or subtitles", L"C5 locked >= 98% of film time", L"C6 <= 2 wrong-time frames per phase event",
				L"C7 film clock monotonic, spaced 1/f within 1 ms", L"C8 every lock change marks the next kept frame" };
			for (size_t i = 0; i < count; ++i)
				Assert::IsTrue(r.Pass(criteria[i]), (Wide(name) + L" (fixes on): " + text[criteria[i]]).c_str());
		}

		const Library* RequireFootage()
		{
			std::string folder;
			const Library* library = LoadFootage(folder);
			if (!library)
			{
				Logger::WriteMessage(("SKIP-NO-FOOTAGE: no readable clips in " + folder + " (VP_TEST_REAL_FOOTAGE)\n").c_str());
				Assert::IsTrue(EnvValue("VP_TEST_REQUIRE_FOOTAGE") != "1", L"VP_TEST_REQUIRE_FOOTAGE=1 but no footage");
				return nullptr;
			}
			static bool reported = false;
			if (!reported)
			{
				reported = true;
				char text[256];
				std::snprintf(text, sizeof(text), "footage: %d clips from %s read in %.2f s (sampled rows only)\n",
					static_cast<int>(library->clips.size()), folder.c_str(), library->loadSeconds);
				Logger::WriteMessage(text);
			}
			return library;
		}

		struct Carrier { Rate film, signal; double pictureHz; const char* name; };

		bool HasReel(const Library& library, double pictureHz) { return !library.Reel(pictureHz).empty(); }

		// ---- Apple TV recordings (StreamRecorder frames.csv) ----
		void FindRecordings(const std::string& folder, int depth, std::vector<std::string>& found)
		{
			WIN32_FIND_DATAA data;
			HANDLE find = FindFirstFileA((folder + "\\*").c_str(), &data);
			if (find == INVALID_HANDLE_VALUE)
				return;
			do
			{
				const std::string name = data.cFileName;
				if (name == "." || name == "..")
					continue;
				const std::string path = folder + "\\" + name;
				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					if (depth > 0)
						FindRecordings(path, depth - 1, found);
				}
				else if (_stricmp(name.c_str(), "frames.csv") == 0)
				{
					std::ifstream csv(path);
					std::string header;
					std::getline(csv, header);
					if (header.find("exact_repeat") != std::string::npos)
						found.push_back(path);
				}
			} while (FindNextFileA(find, &data));
			FindClose(find);
		}
	}
	}

	using namespace PulldownCorpus;

	TEST_CLASS(PulldownCorpusTests)
	{
	public:
		// Every carrier (3:2, 3:2 at 59.94, 2:2 at 50 and 60, 24 in 30, 24 in 50,
		// 23.976 in 60) from real pictures at two start phases, through: fade to
		// black, a black hold and back; a cut; two subtitle changes on repeat
		// slots; an edit dropping and one duplicating a signal frame; a counted
		// capture miss; a pause restarting at a random phase; +-3 ms jitter and a
		// 15 ms spike. Fixes on: all criteria. Off: today's behaviour, reported.
		TEST_METHOD(RealFootageFilmThroughEvents)
		{
			const auto start = std::chrono::steady_clock::now();
			const Library* library = RequireFootage();
			if (!library) return;
			const Carrier carriers[] = {
				{ { 24, 1 }, { 60, 1 }, 24.0, "3:2" }, { { 24000, 1001 }, { 60000, 1001 }, 24.0, "3:2 NTSC" },
				{ { 25, 1 }, { 50, 1 }, 25.0, "2:2 25-in-50" }, { { 30, 1 }, { 60, 1 }, 30.0, "2:2 30-in-60" },
				{ { 24, 1 }, { 30, 1 }, 24.0, "24-in-30" }, { { 24, 1 }, { 50, 1 }, 24.0, "24-in-50" },
				{ { 24000, 1001 }, { 60, 1 }, 24.0, "23.976-in-60" } };
			Tally onTally, offTally;
			uint64_t seed = 11;
			for (const Carrier& c : carriers)
			{
				if (!HasReel(*library, c.pictureHz)) { Logger::WriteMessage((std::string(c.name) + ": no clip for its picture rate, skipped\n").c_str()); continue; }
				for (long long phase : { 0LL, 3LL })
				{
					++seed;
					Tape tape(*library, c.signal);
					tape.StartFilm(c.film, c.pictureHz, phase);
					tape.Film(4.0);
					tape.Film(1.0, 1000, 0, TagFade);    // fade to black
					tape.Film(1.0, 0, 0, TagBlack);      // black hold
					tape.Film(1.0, 0, 1000, TagFade);    // and back
					tape.Film(2.0);
					tape.Cut();
					tape.Film(2.0);
					tape.Subtitle();                     // on the next repeat slot
					tape.Film(2.0);
					tape.Subtitle();                     // changes on another
					tape.Film(2.0);
					tape.EditDrop();
					tape.Film(3.0);
					tape.EditDuplicate();
					tape.Film(3.0);
					tape.Miss();
					tape.Film(3.0);
					tape.Pause(1.5);
					tape.StartFilm(c.film, c.pictureHz, static_cast<long long>(Mix(seed) % 97), true);
					tape.Film(4.0);
					tape.Finish(seed, 3.0, 20.3, 15.0);
					const std::string name = std::string(c.name) + " " + RateText(c.film) + " in " + RateText(c.signal) + " phase " + std::to_string(phase);
					const Run run = ReplayBoth(*library, tape, name, onTally, offTally);
					const int all[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
					AssertCriteria(run.on, name, all, 8);
					Assert::IsTrue(run.on.segmentsChecked > 0 && run.on.spacingChecked > 0, Wide(name + ": criteria exercised").c_str());
				}
			}
			Logger::WriteMessage((onTally.Text("events, fixes on ") + "\n").c_str());
			Logger::WriteMessage((offTally.Text("events, fixes off") + "\n").c_str());
			char text[96];
			std::snprintf(text, sizeof(text), "RealFootageFilmThroughEvents: %.2f s\n", Seconds(start));
			Logger::WriteMessage(text);
		}

		// D5: rate mismatches (frame-rate matching off) slip one film frame
		// every ~40 s; 100 s each, so at least two natural slips.
		TEST_METHOD(RealFootageRateMismatchSlips)
		{
			const auto start = std::chrono::steady_clock::now();
			const Library* library = RequireFootage();
			if (!library) return;
			const Carrier carriers[] = {
				{ { 24000, 1001 }, { 60, 1 }, 24.0, "23.976-in-60" }, { { 24, 1 }, { 60000, 1001 }, 24.0, "24-in-59.94" },
				{ { 25, 1 }, { 60000, 1001 }, 25.0, "25-in-59.94" } };
			Tally onTally, offTally;
			for (const Carrier& c : carriers)
			{
				if (!HasReel(*library, c.pictureHz)) continue;
				Tape tape(*library, c.signal);
				tape.StartFilm(c.film, c.pictureHz, 1);
				tape.Film(100.0);
				tape.Finish(77, 3.0, 50.1, 15.0);
				int slips = 0;
				for (const Frame& frame : tape.Frames())
					slips += frame.slip ? 1 : 0;
				const std::string name = std::string(c.name) + " (" + std::to_string(slips) + " slips)";
				Assert::IsTrue(slips >= 2, Wide(name + ": the stream slips").c_str());
				const Run run = ReplayBoth(*library, tape, name, onTally, offTally);
				const int all[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
				AssertCriteria(run.on, name, all, 8);
			}
			Logger::WriteMessage((onTally.Text("slips, fixes on ") + "\n").c_str());
			Logger::WriteMessage((offTally.Text("slips, fixes off") + "\n").c_str());
			char text[96];
			std::snprintf(text, sizeof(text), "RealFootageRateMismatchSlips: %.2f s\n", Seconds(start));
			Logger::WriteMessage(text);
		}

		// Segments inside film: native video (60p / 50p / 30p from the clips), a
		// 30p segment (film-class: relocks as 30p), animation on twos, then film
		// again at another phase. Video unlocks within a second and never locks;
		// twos never takes a lock; each film segment locks in time and holds.
		TEST_METHOD(RealFootageVideo30pAndTwosSegments)
		{
			const auto start = std::chrono::steady_clock::now();
			const Library* library = RequireFootage();
			if (!library) return;
			Tally onTally, offTally;
			struct Family { Rate signal, film, film30; double videoHz, filmPictures; bool has30; };
			const Family families[] = {
				{ { 60, 1 }, { 24, 1 }, { 30, 1 }, 60.0, 24.0, true },
				{ { 60000, 1001 }, { 24000, 1001 }, { 30000, 1001 }, 60.0, 24.0, true },
				{ { 50, 1 }, { 25, 1 }, { 24, 1 }, 50.0, 25.0, true },     // the "other film" segment: 24p in 50
				{ { 30, 1 }, { 24, 1 }, { 0, 1 }, 30.0, 24.0, false } };
			uint64_t seed = 300;
			for (const Family& f : families)
			{
				if (!HasReel(*library, f.videoHz) || !HasReel(*library, f.filmPictures)) continue;
				Tape tape(*library, f.signal);
				tape.StartFilm(f.film, f.filmPictures, 3);
				tape.Film(5.0);
				tape.Video(3.0, f.videoHz);
				tape.StartFilm(f.film, f.filmPictures, 1);
				tape.Film(5.0);
				if (f.has30)
				{
					const double pictures = f.film30.Hz() > 29.0 ? 30.0 : 24.0;
					if (HasReel(*library, pictures))
					{
						tape.StartFilm(f.film30, pictures, 0);
						tape.Film(4.0);
					}
				}
				tape.StartFilm(f.film, f.filmPictures, 4);
				tape.Film(5.0);
				tape.StartFilm(f.film, f.filmPictures, 2, false, true);   // animation on twos
				tape.Film(3.0);
				tape.StartFilm(f.film, f.filmPictures, 1);
				tape.Film(5.0);
				tape.Finish(++seed, 3.0);
				const std::string name = "segments in " + RateText(f.signal);
				const Run run = ReplayBoth(*library, tape, name, onTally, offTally);
				const int all[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
				AssertCriteria(run.on, name, all, 8);
				// A cadence change releases the old lock within the time a lock needs
				// (C3 then bounds the new lock from the segment start).
				Assert::IsTrue(run.on.oldCadenceLongest <= static_cast<int>(std::ceil(f.signal.Hz() * Pulldown::Detector::kSecondsToLock)),
					Wide(name + ": the old cadence released within the lock time").c_str());
			}
			Logger::WriteMessage((onTally.Text("segments, fixes on ") + "\n").c_str());
			Logger::WriteMessage((offTally.Text("segments, fixes off") + "\n").c_str());
			char text[96];
			std::snprintf(text, sizeof(text), "RealFootageVideo30pAndTwosSegments: %.2f s\n", Seconds(start));
			Logger::WriteMessage(text);
		}

		// C2 alone: real video, a held still, animation on twos (24p and 25p
		// film drawn on twos, carried) and random holds never lock, fixes on or
		// off. 30p drawn on twos (exactly 15 fps, the floor) is reported only.
		TEST_METHOD(RealFootageNeverLocksOnVideoStillsTwosOrRandom)
		{
			const auto start = std::chrono::steady_clock::now();
			const Library* library = RequireFootage();
			if (!library) return;
			struct Case { Rate signal; double videoHz; Rate film; double filmPictures; };
			const Case cases[] = {
				{ { 60, 1 }, 60.0, { 24, 1 }, 24.0 }, { { 60000, 1001 }, 60.0, { 24000, 1001 }, 24.0 },
				{ { 50, 1 }, 50.0, { 25, 1 }, 25.0 }, { { 50, 1 }, 50.0, { 24, 1 }, 24.0 }, { { 30, 1 }, 30.0, { 24, 1 }, 24.0 } };
			int streams = 0;
			for (const Case& c : cases)
			{
				for (int kind = 0; kind < 4; ++kind)
				{
					const double pictures = kind == 2 ? c.filmPictures : c.videoHz;
					if (!HasReel(*library, pictures) || (kind == 1 && !HasReel(*library, c.filmPictures))) continue;
					Tape tape(*library, c.signal);
					std::string what;
					if (kind == 0) { tape.Video(10.0, c.videoHz); what = "video"; }
					else if (kind == 1) { tape.StartFilm(c.film, c.filmPictures, 0); tape.Film(0.2); tape.Pause(10.0); what = "still"; }
					else if (kind == 2) { tape.StartFilm(c.film, c.filmPictures, 1, false, true); tape.Film(10.0); what = "twos of " + RateText(c.film); }
					else { tape.Video(10.0, c.videoHz, 3, 99); what = "random holds"; }
					tape.Finish(5, 3.0);
					for (int fixes = 0; fixes < 2; ++fixes)
					{
						const auto run = Replay(*library, tape.Frames(), tape.SignalHz(), fixes == 1);
						int locked = 0, newDropped = 0;
						// The still's first 0.2 s is film: judged from the pause on.
						const size_t from = kind == 1 ? static_cast<size_t>(std::llround(0.2 * c.signal.Hz())) : 0;
						for (size_t i = from; i < run.size(); ++i)
						{
							locked += run[i].locked ? 1 : 0;
							newDropped += run[i].drop && !run[i].repeat ? 1 : 0;
						}
						const std::string name = what + " in " + RateText(c.signal) + (fixes ? " [on ]" : " [off]");
						char text[200];
						std::snprintf(text, sizeof(text), "%s: locked frames %d new dropped %d\n", name.c_str(), locked, newDropped);
						Logger::WriteMessage(text);
						Assert::AreEqual(0, locked, Wide(name + ": C2 no lock").c_str());
						Assert::AreEqual(0, newDropped, Wide(name + ": C1 nothing new dropped").c_str());
						++streams;
					}
				}
			}
			// Reported, not asserted: 30p drawn on twos in 60 Hz is exactly
			// kMinFilmHz (15 fps) and the floor is inclusive.
			if (HasReel(*library, 30.0))
			{
				for (const Rate& signal : { Rate{ 60, 1 }, Rate{ 60000, 1001 } })
				{
					Tape tape(*library, signal);
					tape.StartFilm(signal.den == 1 ? Rate{ 30, 1 } : Rate{ 30000, 1001 }, 30.0, 0, false, true);
					tape.Film(10.0);
					tape.Finish(6, 3.0);
					const auto run = Replay(*library, tape.Frames(), tape.SignalHz(), true);
					int locked = 0;
					double filmHz = 0.0;
					for (const Out& o : run) { locked += o.locked ? 1 : 0; filmHz = o.locked ? o.filmHz : filmHz; }
					char text[200];
					std::snprintf(text, sizeof(text), "INFO 30p on twos (15 fps) in %s: locked frames %d of %d (film %.3f)\n",
						RateText(signal).c_str(), locked, static_cast<int>(run.size()), filmHz);
					Logger::WriteMessage(text);
				}
			}
			char text[96];
			std::snprintf(text, sizeof(text), "RealFootageNeverLocksOnVideoStillsTwosOrRandom: %d streams, %.2f s\n", streams, Seconds(start));
			Logger::WriteMessage(text);
			Assert::IsTrue(streams > 0, L"streams checked");
		}

		// Corpus finding (D3): 24p carried in 50 Hz after a 25p 2:2 lock beats
		// against the short pattern - a re-phase every half second, two
		// contradictions a second, under the release rule - and held the 25p
		// lock (and its clock) for as long as it lasted. A second re-phase
		// within FramesToLock now drops the lock; the 24-in-50 lock follows in
		// time. Rate-mismatch slips (seconds apart) still re-phase. No footage.
		TEST_METHOD(ShortPatternBeatingCadenceReleases)
		{
			auto carried = [](Rate film, Rate signal, long long from, long long count)
			{
				std::string pattern;
				long long previous = from > 0 ? ((from - 1) * film.num * signal.den) / (film.den * signal.num) : -1;
				for (long long i = from; i < from + count; ++i)
				{
					const long long n = (i * film.num * signal.den) / (film.den * signal.num);
					pattern += n == previous ? 'R' : 'N';
					previous = n;
				}
				return pattern;
			};
			struct Case { Rate before, after, signal; int cycle, unique; };
			const Case cases[] = {
				{ { 25, 1 }, { 24, 1 }, { 50, 1 }, 25, 12 },
				{ { 30, 1 }, { 24, 1 }, { 60, 1 }, 5, 2 },
				{ { 30000, 1001 }, { 24000, 1001 }, { 60000, 1001 }, 5, 2 } };
			for (const Case& c : cases)
			{
				const long long before = std::llround(4.0 * c.signal.Hz()), after = std::llround(6.0 * c.signal.Hz());
				// Every phase the new content can start at.
				for (int offset = 0; offset < c.cycle; ++offset)
				for (int fixes = 0; fixes < 2; ++fixes)
				{
					const std::string frames = carried(c.before, c.signal, 0, before) + carried(c.after, c.signal, offset, after);
					Pulldown::Detector detector;
					Pulldown::Settings settings;
					settings.keepLockAcrossMisses = settings.rephaseInPlace = settings.cadenceAwareClock = fixes == 1;
					detector.SetSettings(settings);
					long long lockAt = -1;
					for (long long i = 0; i < static_cast<long long>(frames.size()); ++i)
					{
						const Pulldown::Decision d = detector.Next(frames[static_cast<size_t>(i)] == 'R', false, c.signal.Hz());
						if (i >= before && lockAt < 0 && d.locked && d.cycle == c.cycle && d.unique == c.unique)
							lockAt = i;
					}
					const long long need = Pulldown::Detector::FramesToLock(c.cycle, c.signal.Hz()) + c.cycle;
					char text[160];
					std::snprintf(text, sizeof(text), "%s -> %s in %s offset %d [%s]: new lock %lld frames after the change (bound %lld)\n",
						RateText(c.before).c_str(), RateText(c.after).c_str(), RateText(c.signal).c_str(), offset, fixes ? "on " : "off", lockAt - before, need);
					if (offset == 0 || lockAt < before || lockAt + 1 - before > need)
						Logger::WriteMessage(text);
					Assert::IsTrue(lockAt >= before && lockAt + 1 - before <= need, Wide(text).c_str());
					Assert::IsTrue(detector.Locked(), L"holds the new lock");
				}
			}
		}

		// The owner's Apple TV recordings (StreamRecorder frames.csv with
		// exact_repeat; VP_PULLDOWN_RECORDINGS = a folder searched 4 levels
		// deep), replayed with the fixes on: >= 98% locked after the first
		// acquisition, zero false locks (the locked span's repeat share must match
		// the cadence; with labels.csv "first,last,film|video|still" beside it, no
		// lock in a non-film stretch), and every unlock with its cause.
		TEST_METHOD(AppleTvRecordingsReplay)
		{
			const auto start = std::chrono::steady_clock::now();
			const std::string folder = EnvValue("VP_PULLDOWN_RECORDINGS");
			std::vector<std::string> recordings;
			if (!folder.empty())
				FindRecordings(folder, 4, recordings);
			if (recordings.empty())
			{
				Logger::WriteMessage(("SKIP-NO-RECORDINGS: no frames.csv with exact_repeat" + (folder.empty() ? std::string(" (VP_PULLDOWN_RECORDINGS not set)") : " under " + folder) + "\n").c_str());
				Assert::IsTrue(folder.empty(), L"VP_PULLDOWN_RECORDINGS is set but holds no recording");
				return;
			}
			for (const std::string& path : recordings)
			{
				struct Row { long long hw = 0, duration = 0; int periods = 1, noInput = 0, repeat = 0; };
				std::vector<Row> rows;
				{
					std::ifstream csv(path);
					std::string line;
					std::getline(csv, line);
					std::vector<std::string> names;
					{
						std::istringstream header(line);
						std::string name;
						while (std::getline(header, name, ',')) names.push_back(name);
					}
					auto column = [&](const char* name) { for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return static_cast<int>(i); return -1; };
					const int cHw = column("hw_time_100ns"), cDuration = column("hw_duration_100ns"), cPeriods = column("periods_since_previous"),
						cNoInput = column("no_input"), cRepeat = column("exact_repeat");
					Assert::IsTrue(cHw >= 0 && cDuration >= 0 && cPeriods >= 0 && cRepeat >= 0, Wide(path + ": frames.csv columns").c_str());
					while (std::getline(csv, line))
					{
						std::vector<std::string> cells;
						std::istringstream cellStream(line);
						std::string cell;
						while (std::getline(cellStream, cell, ',')) cells.push_back(cell);
						if (static_cast<int>(cells.size()) <= cRepeat) continue;
						Row row;
						row.hw = std::atoll(cells[static_cast<size_t>(cHw)].c_str());
						row.duration = std::atoll(cells[static_cast<size_t>(cDuration)].c_str());
						row.periods = std::atoi(cells[static_cast<size_t>(cPeriods)].c_str());
						row.noInput = cNoInput >= 0 ? std::atoi(cells[static_cast<size_t>(cNoInput)].c_str()) : 0;
						row.repeat = std::atoi(cells[static_cast<size_t>(cRepeat)].c_str());
						rows.push_back(row);
					}
				}
				// Optional labels beside it.
				std::vector<std::pair<std::pair<long long, long long>, std::string>> labels;
				{
					std::ifstream file(path.substr(0, path.find_last_of('\\')) + "\\labels.csv");
					std::string line;
					while (std::getline(file, line))
					{
						long long a = 0, b = 0;
						char kind[32] = {};
						if (sscanf_s(line.c_str(), "%lld,%lld,%31s", &a, &b, kind, static_cast<unsigned>(sizeof(kind))) == 3)
							labels.push_back({ { a, b }, kind });
					}
				}
				auto labelAt = [&](long long i) { for (const auto& l : labels) if (i >= l.first.first && i <= l.first.second) return l.second; return std::string("film"); };

				Pulldown::Detector detector;
				uint64_t locked = 0, judged = 0;
				int falseLocks = 0, unlocks = 0, unexplained = 0;
				std::map<std::string, int> causes;
				long long firstLock = -1, lockStart = -1, lockRepeats = 0, lockFrames = 0;
				int lockCycle = 0, lockUnique = 0;
				std::deque<long long> contradictionsSeen;
				double previousHz = 0.0;
				bool wasLocked = false;
				auto closeLock = [&](long long end)
				{
					if (lockStart < 0 || lockFrames < 2LL * lockCycle) return;
					const double expected = 1.0 - static_cast<double>(lockUnique) / lockCycle;
					const double share = static_cast<double>(lockRepeats) / static_cast<double>(lockFrames);
					// A real lock shows the cadence's repeat share (stills only add repeats).
					if (share + 0.05 < expected)
					{
						++falseLocks;
						char text[160];
						std::snprintf(text, sizeof(text), "  false lock %lld..%lld: repeat share %.3f, cadence wants %.3f\n", lockStart, end, share, expected);
						Logger::WriteMessage(text);
					}
				};
				for (size_t i = 0; i < rows.size(); ++i)
				{
					const Row& row = rows[i];
					const double hz = row.duration > 0 ? 1e7 / static_cast<double>(row.duration) : 0.0;
					const int missed = row.periods > 1 ? row.periods - 1 : 0;
					const Pulldown::Decision d = detector.Next(row.repeat != 0, missed > 0, hz, missed > 0 ? missed : -1);
					const bool contradiction = d.locked && d.update;
					if (contradiction) contradictionsSeen.push_back(static_cast<long long>(i));
					while (!contradictionsSeen.empty() && static_cast<long long>(i) - contradictionsSeen.front() > static_cast<long long>(std::ceil(hz > 0 ? hz : 60.0)))
						contradictionsSeen.pop_front();
					if (d.locked && firstLock < 0) firstLock = static_cast<long long>(i);
					if (d.locked && (!wasLocked || d.relocked))
					{
						closeLock(static_cast<long long>(i));
						lockStart = static_cast<long long>(i); lockRepeats = 0; lockFrames = 0; lockCycle = d.cycle; lockUnique = d.unique;
					}
					if (d.locked) { ++lockFrames; lockRepeats += row.repeat ? 1 : 0; }
					if (d.locked && labelAt(static_cast<long long>(i)) != "film" && (!wasLocked || d.relocked)) ++falseLocks;
					if (wasLocked && !d.locked)
					{
						++unlocks;
						closeLock(static_cast<long long>(i));
						lockStart = -1;
						std::string cause;
						if (missed > 0) cause = "capture gap (" + std::to_string(missed) + " missed)";
						else if (row.noInput) cause = "no input";
						else if (previousHz > 0.0 && std::fabs(hz - previousHz) > previousHz * 0.005) cause = "signal rate change";
						else if (contradictionsSeen.size() + 1 >= 3) cause = "new pictures on repeat slots (" + std::to_string(contradictionsSeen.size() + 1) + " in 1 s): content change";
						else { cause = "unexplained"; ++unexplained; }
						causes[cause] += 1;
						char text[200];
						std::snprintf(text, sizeof(text), "  unlock at %zu: %s\n", i, cause.c_str());
						Logger::WriteMessage(text);
					}
					if (firstLock >= 0 && labelAt(static_cast<long long>(i)) == "film" && !row.noInput)
					{
						++judged;
						locked += d.locked ? 1 : 0;
					}
					wasLocked = d.locked;
					previousHz = hz > 0.0 ? hz : previousHz;
				}
				closeLock(static_cast<long long>(rows.size()));
				const double share = judged ? static_cast<double>(locked) / static_cast<double>(judged) : 0.0;
				char text[300];
				std::snprintf(text, sizeof(text), "%s: %zu frames, first lock %lld, locked %.2f%%, false locks %d, unlocks %d (unexplained %d)\n",
					path.c_str(), rows.size(), firstLock, 100.0 * share, falseLocks, unlocks, unexplained);
				Logger::WriteMessage(text);
				const std::wstring w = Wide(path);
				Assert::IsTrue(firstLock >= 0, (w + L": locks").c_str());
				Assert::IsTrue(share >= 0.98, (w + L": >= 98% locked").c_str());
				Assert::AreEqual(0, falseLocks, (w + L": zero false locks").c_str());
				Assert::AreEqual(0, unexplained, (w + L": every unlock has its cause").c_str());
			}
			char text[96];
			std::snprintf(text, sizeof(text), "AppleTvRecordingsReplay: %.2f s\n", Seconds(start));
			Logger::WriteMessage(text);
		}
	};
}
