/*
 * Copyright(C) 2026 Bill Slack
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Synthetic DV tunnel validation (test/dv-synthetic, 2026-10-24): VP's tunnel decode
// against a corpus written by an independent Python implementation of the tunnel
// (tools/dv_synth/make_corpus.py: known pictures + authored DM, packed exactly as a
// DeckLink delivers an HDMI 1.4a 6.5.4 RGB8 tunnel in r210 / R10b / R10l, low8 /
// high8 / high8 with replicated bits, 1080p and 2160p, 24/30/60 Hz).
//
// VP_TEST_DV_SYNTH_CORPUS = the corpus root (tools/dv_synth/run_dv_synth.ps1 generates
// it; it is never committed). Without it the tests say SKIP-NO-CORPUS.
//
// What is checked, with numbers in the test output:
//   (a) decode accuracy: the formatter's planes (AVX2 and scalar) and the GPU unpack's
//       transcription (DolbyVisionTunnelUnpackReference.h, the shader's arithmetic)
//       are bit-exact with the generator's planes, every container and placement;
//   (b) metadata coverage: every authored DM field VP parses (L1, L2, L8, L5, L6,
//       L11, source range, matrices) comes back exactly; robustness: missing,
//       damaged, truncated, malformed and extreme metadata never shows packed pixels,
//       shows black / keeps the picture as documented and recovers;
//   (c) RPU <-> frame alignment: at every scene cut (same-frame and advance
//       metadata) each frame decodes its own scene's L1;
//   CPU unpack cost at 4K (and the GPU route's CPU share: the DM packet only).
#include "pch.h"
#include "CppUnitTest.h"

#include <dv_loopback/DvLoopbackHarness.h>
#include <VP-V/modules/control/diagnostics/RawCaptureCorpus.h>
#include <vprenderer/DolbyVisionTunnelUnpackReference.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace DvSynth
	{
		inline std::string Env(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string out;
			if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
			std::free(v);
			return out;
		}

		inline std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

		inline std::vector<int> Ints(const std::string& s)
		{
			std::vector<int> out;
			if (s.empty() || s == "-")
				return out;
			std::stringstream ss(s);
			std::string item;
			while (std::getline(ss, item, ','))
				out.push_back(std::atoi(item.c_str()));
			return out;
		}

		struct TruthFrame
		{
			size_t index = 0;
			std::string file, picture, expect, dm;
			int stamp = 0, scene = 0;
			std::map<std::string, std::string> keys;
		};

		struct Truth
		{
			std::map<std::string, std::string> segment;
			std::map<std::string, std::string> planes;   // picture -> planes file ("-" none)
			std::vector<TruthFrame> frames;
		};

		inline std::map<std::string, std::string> Words(const std::string& line)
		{
			std::map<std::string, std::string> out;
			std::stringstream ss(line);
			std::string w;
			while (ss >> w)
			{
				const size_t eq = w.find('=');
				if (eq != std::string::npos)
					out[w.substr(0, eq)] = w.substr(eq + 1);
			}
			return out;
		}

		inline bool LoadTruth(const std::string& folder, Truth& t)
		{
			std::ifstream in(folder + "\\truth.txt");
			if (!in)
				return false;
			std::string line;
			while (std::getline(in, line))
			{
				const auto words = Words(line);
				if (line.compare(0, 8, "segment ") == 0)
					t.segment = words;
				else if (line.compare(0, 8, "picture ") == 0)
					t.planes[words.at("id")] = words.at("planes");
				else if (line.compare(0, 6, "frame ") == 0)
				{
					TruthFrame f;
					f.index = static_cast<size_t>(std::atoi(words.at("index").c_str()));
					f.file = words.at("file");
					f.picture = words.at("picture");
					f.stamp = std::atoi(words.at("stamp").c_str());
					f.scene = std::atoi(words.at("scene").c_str());
					f.expect = words.at("expect");
					f.dm = words.at("dm");
					f.keys = words;
					t.frames.push_back(f);
				}
			}
			return !t.frames.empty();
		}

		inline std::vector<std::string> Segments(const std::string& prefix)
		{
			std::vector<std::string> out;
			const std::string root = Env("VP_TEST_DV_SYNTH_CORPUS");
			if (root.empty())
				return out;
			for (const std::string& folder : FindRawCorpusSegments(root))
			{
				const size_t slash = folder.find_last_of("\\/");
				const std::string name = slash == std::string::npos ? folder : folder.substr(slash + 1);
				if (name.compare(0, prefix.size(), prefix) == 0)
					out.push_back(folder);
			}
			return out;
		}

		inline bool SkipWithoutCorpus(const char* test)
		{
			if (!Env("VP_TEST_DV_SYNTH_CORPUS").empty())
				return false;
			Logger::WriteMessage((std::string("SKIP-NO-CORPUS ") + test +
				" (set VP_TEST_DV_SYNTH_CORPUS; tools/dv_synth/run_dv_synth.ps1)\n").c_str());
			return true;
		}

		inline bool ReadFile(const std::string& path, std::vector<uint8_t>& bytes)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return true;
		}

		// The stamp written by the generator: 32 cells across the stamp band (rows 48..96 at
		// 1080p, scaled), I = 3000 for a one, 600 for a zero; magic 0xA5 in the top byte.
		inline bool ReadStamp(const std::vector<uint16_t>& planes, uint32_t w, uint32_t h, uint32_t& word)
		{
			const double s = h / 1080.0;
			const uint32_t y = static_cast<uint32_t>((48 + 96) / 2 * s);
			word = 0;
			for (uint32_t bit = 0; bit < 32; ++bit)
			{
				const uint32_t x = (bit * w / 32 + (bit + 1) * w / 32) / 2;
				const uint16_t code = static_cast<uint16_t>(planes[static_cast<size_t>(y) * w + x] >> 4);
				word = (word << 1) | (code > 1800 ? 1u : 0u);
			}
			return (word >> 24) == 0xA5;
		}

		struct Decoded
		{
			Shown shown = Shown::Black;
			DolbyVisionTunnelFrameMetadata md;
			std::vector<uint16_t> planes;
		};

		// One segment through VP's formatter + the renderer's presentation rule.
		inline std::vector<Decoded> Replay(const RawCorpusSegment& seg, bool scalar, bool keepPlanes,
			std::vector<std::string>* log = nullptr)
		{
			std::vector<Decoded> out;
			AnalyzerOptions options;
			options.keepFrames = false;
			options.scalarUnpack = scalar;
			options.collectLog = log != nullptr;
			DvLoopbackAnalyzer analyzer(options);
			analyzer.OnState(seg.MakeState(true));
			std::vector<uint8_t> bytes;
			for (size_t i = 0; i < seg.frames.size(); ++i)
			{
				Decoded d;
				if (seg.LoadFrame(i, bytes))
				{
					VideoFrame frame(bytes.data(), seg.frames[i].counter, seg.frames[i].timingTimestamp, nullptr,
						seg.frames[i].captureTimestamp);
					analyzer.OnFrame(frame, seg.frames[i].arrivalQpc);
					d.shown = analyzer.LastShown();
					d.md = analyzer.LastMetadata();
					if (keepPlanes && d.shown == Shown::Picture)
						d.planes = analyzer.LastPlanes();
				}
				out.push_back(std::move(d));
			}
			if (log)
			{
				const LoopReport r = analyzer.Finish();
				*log = r.log;
			}
			return out;
		}

		inline std::string Note(const TruthFrame& f)
		{
			const auto it = f.keys.find("note");
			return it == f.keys.end() ? std::string() : it->second;
		}

		inline const char* ShownName(Shown s) { return DvLoopback::ToString(s); }

		// Decoded DM vs the authored one; returns the mismatching field names.
		inline std::string CompareDm(const DolbyVisionTunnelFrameMetadata& md, const TruthFrame& f)
		{
			std::string bad;
			auto check = [&bad](bool ok, const char* name) { if (!ok) { bad += name; bad += ' '; } };
			check(md.valid, "valid");
			const std::vector<int> src = Ints(f.keys.at("src"));
			check(src.size() == 2 && md.sourceMinPq == src[0] && md.sourceMaxPq == src[1], "source_range");
			const std::vector<int> l1 = Ints(f.keys.at("l1"));
			if (l1.empty())
				check(!md.hasSceneMetadata, "l1_absent");
			else
				check(md.hasSceneMetadata && md.sceneMinPq == l1[0] && md.sceneMaxPq == l1[1] && md.sceneAvgPq == l1[2], "l1");
			const int l2n = std::atoi(f.keys.at("l2n").c_str()), l8n = std::atoi(f.keys.at("l8n").c_str());
			const int maxTrims = DolbyVisionTunnelFrameMetadata::kMaxTrims;
			const int wantL2 = (std::min)(l2n, maxTrims);
			const int wantL8 = (std::min)(l8n, maxTrims - wantL2);
			int gotL2 = 0, gotL8 = 0;
			for (int k = 0; k < md.trimCount; ++k)
			{
				const auto& t = md.trims[k];
				if (t.level == 2)
				{
					const std::vector<int> want = Ints(f.keys.at("l2_" + std::to_string(gotL2)));
					check(want.size() >= 7 && t.targetMaxPq == want[0] && t.slope == want[1] && t.offset == want[2] &&
						t.power == want[3] && t.chromaWeight == want[4] && t.saturationGain == want[5] &&
						static_cast<uint16_t>(t.msWeight) == static_cast<uint16_t>(want[6]), "l2_fields");
					++gotL2;
				}
				else if (t.level == 8)
				{
					const std::vector<int> want = Ints(f.keys.at("l8_" + std::to_string(gotL8)));
					check(want.size() >= 7 && t.targetDisplayIndex == want[0] && t.slope == want[1] && t.offset == want[2] &&
						t.power == want[3] && t.chromaWeight == want[4] && t.saturationGain == want[5] &&
						static_cast<uint16_t>(t.msWeight) == static_cast<uint16_t>(want[6]), "l8_fields");
					// feat/dv-l8: the optional fields (mid contrast, clip trim, the vectors).
					if (want.size() >= 9)
						check(t.hasMidContrastAndClip && t.midContrast == want[7] && t.clipTrim == want[8], "l8_mid_clip");
					if (want.size() >= 15)
					{
						bool vec = t.hasSaturationVector;
						for (int v = 0; vec && v < 6; ++v) vec = t.saturationVector[v] == want[9 + v];
						check(vec, "l8_sat_vector");
					}
					if (want.size() >= 21)
					{
						bool vec = t.hasHueVector;
						for (int v = 0; vec && v < 6; ++v) vec = t.hueVector[v] == want[15 + v];
						check(vec, "l8_hue_vector");
					}
					++gotL8;
				}
			}
			check(gotL2 == wantL2, "l2_count");
			check(gotL8 == wantL8, "l8_count");
			check(md.trimOverflow == (l2n + l8n > maxTrims), "trim_overflow");
			// feat/dv-l8: L10 targets (corpora written before L10 carry no l10n key).
			if (f.keys.count("l10n"))
			{
				const int l10n = std::atoi(f.keys.at("l10n").c_str());
				bool l10 = md.targetDisplayCount == l10n;
				for (int k = 0; l10 && k < l10n; ++k)
				{
					const std::vector<int> want = Ints(f.keys.at("l10_" + std::to_string(k)));
					const auto& t = md.targetDisplays[k];
					l10 = want.size() == 4 && t.index == want[0] && t.maxPq == want[1] && t.minPq == want[2] && t.primaryIndex == want[3];
				}
				check(l10, "l10");
			}
			const std::vector<int> l5 = Ints(f.keys.at("l5"));
			check(l5.empty() ? !md.hasActiveArea : (md.hasActiveArea && md.activeAreaLeft == l5[0] && md.activeAreaRight == l5[1] &&
				md.activeAreaTop == l5[2] && md.activeAreaBottom == l5[3]), "l5");
			const std::vector<int> l6 = Ints(f.keys.at("l6"));
			check(l6.empty() ? !md.hasL6 : (md.hasL6 && md.l6MaxDisplayMasteringLuminance == l6[0] &&
				md.l6MinDisplayMasteringLuminance == l6[1]), "l6");
			const std::vector<int> l11 = Ints(f.keys.at("l11"));
			check(l11.empty() ? !md.hasL11 : (md.hasL11 && md.l11Raw[0] == l11[0] && md.l11Raw[1] == l11[1] &&
				md.l11Raw[2] == l11[2] && md.l11Raw[3] == l11[3]), "l11");
			const std::vector<int> ycc = Ints(f.keys.at("ycc")), off = Ints(f.keys.at("off")), lms = Ints(f.keys.at("lms"));
			bool matrices = ycc.size() == 9 && off.size() == 3 && lms.size() == 9;
			for (int r = 0; matrices && r < 3; ++r)
			{
				matrices = matrices && std::fabs(md.nonlinearOffset[r] - static_cast<float>(static_cast<uint32_t>(off[r]) / 268435456.0)) < 1e-6f;
				for (int c = 0; c < 3; ++c)
					matrices = matrices && md.nonlinear[r][c] == static_cast<float>(ycc[r * 3 + c]) / 8192.0f &&
						md.linear[r][c] == static_cast<float>(lms[r * 3 + c]) / 16384.0f;
			}
			check(matrices, "matrices");
			return bad;
		}
	}

	TEST_CLASS(DvSyntheticCorpusTests)
	{
	public:
		// Found by the synthetic corpus (2026-10-24), no corpus needed: a low8 capture whose
		// header rows are flat (black bars, a flat top) has a header CRC that is valid under
		// BOTH RGB8 placements (the packet bit is the parity of the whole pixel; the two bits a
		// placement drops are constant). High8 was tried first, so the formatter locked high8:
		// correct DM, garbage picture for as long as the top stayed flat. Fixed (dv_tunnel_fixes):
		// the placement votes decide. Checked with the fix on (exact planes, low8) and off (the
		// old failure reproduces, proving the test sees it); a black first picture that cannot
		// decide is followed by a picture that does.
		TEST_METHOD(FlatHeaderRowsDoNotLockTheWrongPlacement)
		{
			const uint32_t w = 1920, h = 1080;
			auto make = [&](bool content, uint8_t id, std::vector<uint8_t>& bytes, std::vector<uint16_t>& planes)
			{
				IptPicture picture;
				picture.Resize(w, h);
				for (uint32_t y = 0; y < h; ++y)
					for (uint32_t x = 0; x < w; ++x)
					{
						const size_t p = static_cast<size_t>(y) * w + x;
						const bool bars = y < 140 || y >= h - 140 || !content;   // a scope film's bars (or black)
						picture.i[p] = static_cast<uint16_t>(bars ? 64 : 300 + (x * 7 + y * 3) % 3000);
						picture.Chroma(p) = static_cast<uint16_t>(bars ? 2048 : 1500 + (x + 2 * y) % 1100);
					}
				DmFields dm = DmFields::Ipt();
				dm.ext.push_back(Ext::L1(0, 3079, 1500));
				PacketHeader header;
				header.currentId = header.affectedId = id;
				std::vector<uint8_t> rgb;
				IptPicture embedded;
				Assert::IsTrue(BuildTunnelRgb8(picture, dm, header, rgb, &embedded));
				ContainerOptions o;
				o.encoding = VideoFrameEncoding::R210;
				o.high8 = false;
				o.rowBytes = ((w + 63) / 64) * 256;
				WrapRgb8(rgb, w, h, o, bytes);
				FormatterPlanes(embedded, planes);
			};
			VideoStateComPtr state = new VideoState();
			state->valid = true;
			state->displayMode = std::make_shared<DisplayMode>(w, h, false, 24000, 1001);
			state->videoFrameEncoding = VideoFrameEncoding::R210;
			state->dolbyVisionTunnel = true;
			for (const bool fixes : { true, false })
			{
				AnalyzerOptions options;
				options.keepFrames = false;
				options.bugHuntFixes = fixes;
				options.collectLog = false;
				DvLoopbackAnalyzer analyzer(options);
				analyzer.OnState(state);
				std::vector<uint8_t> bytes;
				std::vector<uint16_t> want;
				// Picture 1: black (cannot decide); picture 2: letterboxed content.
				make(false, 0, bytes, want);
				analyzer.OnFrame(VideoFrame(bytes.data(), 1, 0, nullptr), 1);
				make(true, 0, bytes, want);
				analyzer.OnFrame(VideoFrame(bytes.data(), 2, 0, nullptr), 2);
				Assert::IsTrue(analyzer.LastShown() == Shown::Picture, L"picture shown");
				const bool exact = analyzer.LastPlanes() == want;
				const unsigned placement = analyzer.LastMetadata().rgbBitAlignment;
				Logger::WriteMessage((std::string("flat header rows, low8, dv_tunnel_fixes ") + (fixes ? "on" : "off") + ": placement " +
					(placement == 2 ? "low8" : placement == 1 ? "high8" : "?") + ", planes " + (exact ? "exact" : "WRONG") + "\n").c_str());
				if (fixes)
				{
					Assert::AreEqual(2u, placement, L"low8 placement");
					Assert::IsTrue(exact, L"planes exact with the fix");
				}
				else
					Assert::IsFalse(exact, L"without the fix the old failure reproduces (the test can see it)");
			}
		}

		// (a) Decode accuracy: bit-exact planes on every accuracy segment, for the AVX2 and
		// scalar CPU unpack and the GPU shader's transcription; the stamp read back.
		TEST_METHOD(SyntheticCorpusDecodesBitExact)
		{
			using namespace DvSynth;
			if (SkipWithoutCorpus("SyntheticCorpusDecodesBitExact")) return;
			const std::vector<std::string> segments = Segments("acc-");
			Assert::IsFalse(segments.empty(), L"no acc-* segments in the corpus");
			const std::vector<std::string> extra = Segments("btb-");
			std::vector<std::string> all = segments;
			all.insert(all.end(), extra.begin(), extra.end());
			size_t frames = 0, samples = 0;
			for (const std::string& folder : all)
			{
				RawCorpusSegment seg;
				Truth truth;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), W(error).c_str());
				Assert::IsTrue(LoadTruth(folder, truth), W(folder + ": truth.txt").c_str());
				const bool highAligned = truth.segment["placement"] != "low8";
				const size_t n = static_cast<size_t>(seg.width) * seg.height * 2;
				uint64_t avxBad = 0, scalarBad = 0, gpuBad = 0;
				size_t checked = 0, stampBad = 0;
				for (const bool scalar : { false, true })
				{
					const std::vector<Decoded> decoded = Replay(seg, scalar, true);
					for (const TruthFrame& f : truth.frames)
					{
						const std::string planesFile = truth.planes[f.picture];
						if (planesFile.empty() || planesFile == "-")
							continue;
						std::vector<uint8_t> raw;
						Assert::IsTrue(ReadFile(folder + "\\" + planesFile, raw), W(planesFile).c_str());
						Assert::AreEqual(n * 2, raw.size(), W(folder + ": planes size").c_str());
						const uint16_t* want = reinterpret_cast<const uint16_t*>(raw.data());
						const Decoded& d = decoded[f.index];
						Assert::IsTrue(d.shown == Shown::Picture, W(folder + " frame " + std::to_string(f.index) +
							": shown " + ShownName(d.shown)).c_str());
						uint64_t bad = 0;
						for (size_t k = 0; k < n; ++k)
							if (d.planes[k] != want[k])
							{
								if (bad == 0)
									Logger::WriteMessage((folder + (scalar ? " scalar" : " AVX2") + ": first mismatch at sample " + std::to_string(k) +
										" (plane " + std::to_string(k / (n / 2)) + ", x " + std::to_string((k % (n / 2)) % seg.width) + ", y " +
										std::to_string((k % (n / 2)) / seg.width) + "): got " + std::to_string(d.planes[k]) + " want " +
										std::to_string(want[k]) + "; rgbBitAlignment " + std::to_string(d.md.rgbBitAlignment) + "\n").c_str());
								++bad;
							}
						(scalar ? scalarBad : avxBad) += bad;
						if (!scalar)
						{
							++checked;
							uint32_t word = 0;
							if (!ReadStamp(d.planes, seg.width, seg.height, word) || ((word >> 8) & 0xFFFF) != static_cast<uint32_t>(f.stamp))
								++stampBad;
							// The GPU unpack's arithmetic (the shader, step by step) on the raw frame.
							std::vector<uint8_t> bytes;
							Assert::IsTrue(seg.LoadFrame(f.index, bytes));
							std::vector<uint16_t> gpu(n);
							DolbyVisionTunnelUnpackReference::UnpackFrame(bytes.data(), seg.rowBytes ? seg.rowBytes : seg.width * 4,
								static_cast<int>(seg.width), static_cast<int>(seg.height), seg.encoding, highAligned, gpu.data());
							for (size_t k = 0; k < n; ++k)
								gpuBad += gpu[k] != want[k] ? 1 : 0;
							samples += n;
						}
					}
				}
				frames += checked;
				char line[512];
				snprintf(line, sizeof(line), "DECODE %s: %zu frames, %ux%u %s %s: mismatching samples AVX2 %llu, scalar %llu, GPU transcription %llu; stamps unread %zu\n",
					truth.segment["name"].c_str(), checked, seg.width, seg.height, truth.segment["encoding"].c_str(),
					truth.segment["placement"].c_str(), (unsigned long long)avxBad, (unsigned long long)scalarBad,
					(unsigned long long)gpuBad, stampBad);
				Logger::WriteMessage(line);
				Assert::IsTrue(checked > 0, W(folder + ": no frame with planes").c_str());
				Assert::AreEqual<uint64_t>(0, avxBad, W(folder + ": AVX2 unpack differs").c_str());
				Assert::AreEqual<uint64_t>(0, scalarBad, W(folder + ": scalar unpack differs").c_str());
				Assert::AreEqual<uint64_t>(0, gpuBad, W(folder + ": GPU unpack transcription differs").c_str());
				Assert::AreEqual<size_t>(0, stampBad, W(folder + ": stamp").c_str());
			}
			Logger::WriteMessage(("DECODE total: " + std::to_string(frames) + " frames, " + std::to_string(samples) +
				" samples per route, bit-exact\n").c_str());
		}

		// (b) Metadata coverage: every authored field VP parses comes back exactly
		// (meta-, cuts-, policy- and acc- segments).
		TEST_METHOD(SyntheticCorpusMetadataMatchesTheAuthoredDm)
		{
			using namespace DvSynth;
			if (SkipWithoutCorpus("SyntheticCorpusMetadataMatchesTheAuthoredDm")) return;
			size_t checked = 0;
			for (const char* prefix : { "meta-", "cuts-", "policy-", "acc-", "btb-" })
				for (const std::string& folder : Segments(prefix))
				{
					RawCorpusSegment seg;
					Truth truth;
					std::string error;
					Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), W(error).c_str());
					Assert::IsTrue(LoadTruth(folder, truth));
					const std::vector<Decoded> decoded = Replay(seg, false, false);
					size_t bad = 0, frames = 0;
					std::string first;
					for (const TruthFrame& f : truth.frames)
					{
						if (f.dm != "valid")
							continue;
						++frames;
						const Decoded& d = decoded[f.index];
						std::string why = d.shown == Shown::Picture ? CompareDm(d.md, f) : std::string("shown ") + ShownName(d.shown);
						if (!why.empty())
						{
							++bad;
							if (first.empty())
								first = "frame " + std::to_string(f.index) + " (" + Note(f) + "): " + why;
						}
					}
					checked += frames;
					Logger::WriteMessage(("METADATA " + truth.segment["name"] + ": " + std::to_string(frames) + " DMs, " +
						std::to_string(bad) + " mismatching" + (first.empty() ? "" : "; first " + first) + "\n").c_str());
					Assert::AreEqual<size_t>(0, bad, W(folder + ": " + first).c_str());
				}
			Assert::IsTrue(checked > 0, L"no metadata checked");
		}

		// (b) Robustness: per frame, what the renderer would show; never packed pixels or a
		// crash; black where the DM is missing; the picture back (recovery) after every fault.
		TEST_METHOD(SyntheticCorpusRobustness)
		{
			using namespace DvSynth;
			if (SkipWithoutCorpus("SyntheticCorpusRobustness")) return;
			const std::vector<std::string> segments = Segments("robust-");
			Assert::IsFalse(segments.empty(), L"no robust-* segment");
			for (const std::string& folder : segments)
			{
				RawCorpusSegment seg;
				Truth truth;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), W(error).c_str());
				Assert::IsTrue(LoadTruth(folder, truth));
				for (const bool scalar : { false, true })
				{
					std::vector<std::string> log;
					const std::vector<Decoded> decoded = Replay(seg, scalar, false, &log);
					// Per label: frames, how they were shown, and the expectation outcome.
					struct Row { std::string label; size_t frames = 0, ok = 0; std::map<std::string, size_t> shown; };
					std::vector<Row> rows;
					size_t failures = 0;
					std::string firstFailure;
					for (const TruthFrame& f : truth.frames)
					{
						std::string label = f.keys.count("note") ? f.keys.at("note") : "";
						const Decoded& d = decoded[f.index];
						bool ok = true;
						if (f.expect == "picture")
						{
							ok = d.shown == Shown::Picture;
							if (ok && f.dm == "valid")
								ok = CompareDm(d.md, f).empty();
						}
						else if (f.expect == "black")
							ok = d.shown == Shown::Black;
						else if (f.expect == "sdr")
							ok = d.shown == Shown::Sdr;
						else if (f.expect == "hold")
							ok = d.shown == Shown::Rejected;   // the renderer keeps the previous picture
						else   // any: never packed pixels (SDR passthrough or the normal path) inside a DV stream
							ok = d.shown != Shown::Handoff && d.shown != Shown::TunnelOff;
						// DV open item (2026-10-24, no silent fallbacks): a picture shown with a DM that is
						// not its own (its DM rejected, the previous one reused) is flagged for the status.
						if (ok && d.shown == Shown::Picture && f.dm == "keep" && !d.md.reusedPreviousDm)
							ok = false;
						if (!ok)
						{
							++failures;
							if (firstFailure.empty())
								firstFailure = "frame " + std::to_string(f.index) + " [" + Note(f) + "] expected " + f.expect +
								", shown " + ShownName(d.shown);
						}
						if (rows.empty() || rows.back().label != Note(f))
						{
							rows.push_back(Row());
							rows.back().label = Note(f);
						}
						Row& row = rows.back();
						++row.frames;
						row.ok += ok ? 1 : 0;
						++row.shown[ShownName(d.shown)];
					}
					if (!scalar)
						for (const Row& row : rows)
						{
							std::string shown;
							for (const auto& kv : row.shown) shown += kv.first + " x" + std::to_string(kv.second) + ", ";
							Logger::WriteMessage(("ROBUST " + row.label + ": " + std::to_string(row.ok) + "/" + std::to_string(row.frames) +
								" as expected; shown " + shown + "\n").c_str());
						}
					Assert::AreEqual<size_t>(0, failures, W(std::string(scalar ? "scalar: " : "AVX2: ") + firstFailure).c_str());
				}
			}
		}

		// (c) RPU <-> frame alignment: every frame of the cut segments decodes the L1 of its own
		// scene (same-frame DM; advance DM announced one frame early; picture changes).
		TEST_METHOD(SyntheticCorpusRpuFrameAlignmentAtCuts)
		{
			using namespace DvSynth;
			if (SkipWithoutCorpus("SyntheticCorpusRpuFrameAlignmentAtCuts")) return;
			const std::vector<std::string> segments = Segments("cuts-");
			Assert::IsFalse(segments.empty(), L"no cuts-* segment");
			for (const std::string& folder : segments)
			{
				RawCorpusSegment seg;
				Truth truth;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), W(error).c_str());
				Assert::IsTrue(LoadTruth(folder, truth));
				const std::vector<Decoded> decoded = Replay(seg, false, true);
				size_t misaligned = 0, cutFrames = 0, early = 0, late = 0;
				std::string first;
				int previousScene = -1;
				std::map<int, std::vector<int>> sceneL1;
				for (const TruthFrame& f : truth.frames)
					sceneL1[f.scene] = Ints(f.keys.at("l1"));
				for (const TruthFrame& f : truth.frames)
				{
					const Decoded& d = decoded[f.index];
					const std::vector<int> want = Ints(f.keys.at("l1"));
					const bool cut = f.scene != previousScene && previousScene >= 0;
					cutFrames += cut ? 1 : 0;
					const bool ok = d.shown == Shown::Picture && d.md.hasSceneMetadata && want.size() == 3 &&
						d.md.sceneMaxPq == want[1] && d.md.sceneAvgPq == want[2] && d.md.sceneMinPq == want[0];
					// The picture's own stamp (scene id) agrees with the frame.
					uint32_t word = 0;
					const bool stampOk = d.shown == Shown::Picture && ReadStamp(d.planes, seg.width, seg.height, word) &&
						static_cast<int>(word & 0xFF) == f.scene && static_cast<int>((word >> 8) & 0xFFFF) == f.stamp;
					if (!ok || !stampOk)
					{
						++misaligned;
						auto matches = [&](int scene) { const auto it = sceneL1.find(scene); return it != sceneL1.end() && it->second.size() == 3 &&
							d.md.sceneMaxPq == it->second[1] && d.md.sceneAvgPq == it->second[2]; };
						if (matches(f.scene + 1)) ++early;
						if (matches(f.scene - 1)) ++late;
						if (first.empty())
							first = "frame " + std::to_string(f.index) + " scene " + std::to_string(f.scene) + ": decoded L1 max " +
							std::to_string(d.md.sceneMaxPq) + " avg " + std::to_string(d.md.sceneAvgPq) + ", want " +
							(want.size() == 3 ? std::to_string(want[1]) + "/" + std::to_string(want[2]) : "?") +
							(stampOk ? "" : " (stamp)");
					}
					previousScene = f.scene;
				}
				Logger::WriteMessage(("ALIGNMENT " + truth.segment["name"] + ": " + std::to_string(truth.frames.size()) + " frames, " +
					std::to_string(cutFrames) + " cuts, misaligned " + std::to_string(misaligned) + " (one frame early " +
					std::to_string(early) + ", one frame late " + std::to_string(late) + ")" + (first.empty() ? "" : "; first " + first) + "\n").c_str());
				Assert::AreEqual<size_t>(0, misaligned, W(folder + ": " + first).c_str());
			}
		}

		// CPU cost of the tunnel at 4K: the full CPU unpack (AVX2, scalar) and the GPU route's
		// CPU share (AnalyzeFrame: the DM packet only), per frame, on the 2160p corpus frames.
		TEST_METHOD(SyntheticCorpusCpuUnpackCost4K)
		{
			using namespace DvSynth;
			if (SkipWithoutCorpus("SyntheticCorpusCpuUnpackCost4K")) return;
			for (const std::string& folder : Segments("acc-2160p"))
			{
				RawCorpusSegment seg;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), W(error).c_str());
				std::vector<uint8_t> bytes;
				Assert::IsTrue(seg.LoadFrame(0, bytes));
				const VideoFrame frame(bytes.data(), 1, 0, nullptr);
				double best[3] = { 1e30, 1e30, 1e30 };
				for (int method = 0; method < 3; ++method)
				{
					CR210DolbyVisionTunnelFormatter f;
					if (method == 1)
						f.SetConversionMethod(CR210DolbyVisionTunnelFormatter::ConversionMethod::SCALAR);
					VideoStateComPtr state = seg.MakeState(true);
					f.OnVideoState(state);
					std::vector<BYTE> out(static_cast<size_t>(f.GetOutFrameSize()));
					for (int run = 0; run < 12; ++run)
					{
						VideoFrame fr(bytes.data(), static_cast<uint64_t>(run + 1), 0, nullptr);
						const auto t0 = std::chrono::steady_clock::now();
						if (method == 2)
							f.AnalyzeFrame(fr);
						else
							Assert::IsTrue(f.FormatVideoFrame(fr, out.data()));
						const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
						if (run >= 2)
							best[method] = (std::min)(best[method], ms);
					}
				}
				const double budget60 = 1000.0 * seg.rateDenominator / seg.rateNumerator;
				char line[512];
				snprintf(line, sizeof(line), "CPU COST %s (%ux%u %s): full CPU unpack AVX2 %.2f ms, scalar %.2f ms; GPU route's CPU share (DM packet only) %.3f ms; frame period %.2f ms\n",
					folder.substr(folder.find_last_of("\\/") + 1).c_str(), seg.width, seg.height, seg.keys["encoding"].c_str(),
					best[0], best[1], best[2], budget60);
				Logger::WriteMessage(line);
				Assert::IsTrue(best[2] < best[0], L"the GPU route's CPU share must be smaller than the CPU unpack");
			}
		}
	};
}
