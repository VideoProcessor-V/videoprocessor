/*
 * Copyright(C) 2026 VP-V contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
// Raw capture corpus (DV autotest Tier 3, 2026-10-17): the recorder writes a
// corpus the reader replays bit-exact through the tunnel path; any recorded
// corpus (the owner's cinema session, the LLDV-vs-tunnel runs, a hardware
// loopback run) replays through the same path. Captured pixels are never
// committed: the corpus lives outside the checkout (VP_TEST_RAW_CORPUS_DIR).
#include "pch.h"
#include "CppUnitTest.h"

#include <dv_loopback/DvLoopbackHarness.h>
#include <VP-V/modules/control/diagnostics/RawCaptureRecorder.h>
#include <VP-V/modules/control/diagnostics/RawCaptureCorpus.h>

#include <cstdio>
#include <map>
#include <tuple>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace DvLoopback;

namespace Tests
{
	namespace
	{
		std::string EnvString(const char* name)
		{
			char* v = nullptr;
			size_t n = 0;
			std::string out;
			if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
			std::free(v);
			return out;
		}

		std::string TempFolder(const char* name)
		{
			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			// A fresh folder per run (C++14 here: no std::filesystem).
			const std::string folder = std::string(temp) + name + "-" + std::to_string(GetTickCount64());
			CreateDirectoryA(folder.c_str(), nullptr);
			return folder;
		}

		uint64_t Fnv(const uint8_t* data, size_t n, uint64_t h = 1469598103934665603ULL)
		{
			for (size_t k = 0; k < n; ++k) h = (h ^ data[k]) * 1099511628211ULL;
			return h;
		}

		// Replays one segment through VP's tunnel path; returns one digest line
		// per frame (presentation, plane hash, metadata hash) and a summary.
		struct Replay
		{
			std::vector<std::string> digest;
			LoopReport report;
			std::vector<std::string> decodedCsv;
		};

		Replay ReplaySegment(const RawCorpusSegment& seg, bool tunnel, bool scalar = false)
		{
			Replay out;
			AnalyzerOptions options;
			options.keepFrames = false;
			options.scalarUnpack = scalar;
			DvLoopbackAnalyzer analyzer(options);
			const VideoStateComPtr state = seg.MakeState(tunnel);
			analyzer.OnState(state);
			out.decodedCsv.push_back("frame,counter,shown,valid,L1_min,L1_max,L1_avg,source_min_pq,source_max_pq,trims,l5_top,l5_bottom,l11_type,plane_hash");
			std::vector<uint8_t> bytes;
			for (size_t i = 0; i < seg.frames.size(); ++i)
			{
				if (!seg.LoadFrame(i, bytes))
					continue;
				VideoFrame frame(bytes.data(), seg.frames[i].counter ? seg.frames[i].counter : i + 1,
					seg.frames[i].timingTimestamp, nullptr, seg.frames[i].captureTimestamp);
				analyzer.OnFrame(frame, seg.frames[i].arrivalQpc);
				const DolbyVisionTunnelFrameMetadata& md = analyzer.LastMetadata();
				const Shown shown = analyzer.LastShown();
				const uint64_t planeHash = shown == Shown::Picture ?
					Fnv(reinterpret_cast<const uint8_t*>(analyzer.LastPlanes().data()), analyzer.LastPlanes().size() * 2) : 0;
				const FlatMetadata flat = Flatten(md);
				std::string meta;
				for (const auto& kv : flat) meta += kv.first + "=" + std::to_string(kv.second) + ";";
				char line[256];
				snprintf(line, sizeof(line), "%zu %s %016llx %016llx", i, ToString(shown), (unsigned long long)planeHash,
					(unsigned long long)Fnv(reinterpret_cast<const uint8_t*>(meta.data()), meta.size()));
				out.digest.push_back(line);
				char csv[512];
				snprintf(csv, sizeof(csv), "%zu,%llu,%s,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%016llx", i,
					(unsigned long long)seg.frames[i].counter, ToString(shown), md.valid ? 1 : 0,
					md.hasSceneMetadata ? md.sceneMinPq : 0, md.hasSceneMetadata ? md.sceneMaxPq : 0,
					md.hasSceneMetadata ? md.sceneAvgPq : 0, md.sourceMinPq, md.sourceMaxPq, md.trimCount,
					md.activeAreaTop, md.activeAreaBottom, md.l11ContentType, (unsigned long long)planeHash);
				out.decodedCsv.push_back(csv);
			}
			out.report = analyzer.Finish();
			return out;
		}

		// L1 (min, max, avg) tuples from a dovi_tool export JSON (Level1 blocks).
		std::set<std::tuple<int, int, int>> RpuL1Set(const std::string& path)
		{
			std::set<std::tuple<int, int, int>> out;
			std::ifstream in(path);
			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			const std::regex l1("\"Level1\"\\s*:\\s*\\{\\s*\"min_pq\"\\s*:\\s*(\\d+)\\s*,\\s*\"max_pq\"\\s*:\\s*(\\d+)\\s*,\\s*\"avg_pq\"\\s*:\\s*(\\d+)");
			for (auto it = std::sregex_iterator(text.begin(), text.end(), l1); it != std::sregex_iterator(); ++it)
				out.insert({ std::stoi((*it)[1]), std::stoi((*it)[2]), std::stoi((*it)[3]) });
			return out;
		}
	}

	TEST_CLASS(DvRawCorpusTests)
	{
	public:
		// The recorder writes a corpus (continuous mode, a capture-state change
		// mid-run, every-k decimation) that the reader replays bit-exact
		// through the tunnel path; the sidecar carries the capture state.
		TEST_METHOD(RecorderWritesACorpusTheReaderReplaysBitExact)
		{
			const std::string root = TempFolder("vp-raw-capture-test");
			RawCaptureOptions o;
			o.root = root;
			o.maxSeconds = 1000;
			o.minFreeGb = 0;
			o.watchFiles = false;
			o.note = "unit test";
			RawCaptureRecorder recorder(o);
			SequenceConfig config;
			config.width = 200;
			config.height = 108;
			// Each segment replays on a fresh formatter: start every picture with
			// a DM body (a no_md picture first would correctly show black).
			config.noMetadataEvery = 0;
			const PatternSequence sequence(config);
			// The capture card's view of the stream: frames through the simulated
			// loop into a sink that feeds the recorder.
			struct RecorderSink : IDvCaptureSink
			{
				RawCaptureRecorder& r;
				explicit RecorderSink(RawCaptureRecorder& rec) : r(rec) {}
				void OnState(const VideoStateComPtr& s) override { r.OnVideoState(*s); }
				void OnFrame(const VideoFrame& f, int64_t) override { r.OnFrame(f); }
			} sink(recorder);
			recorder.OnCardState("Simulated DeckLink", "RGB 4:4:4", "8-bit", { "test" });
			Assert::IsTrue(recorder.Start("test").find("recording started") != std::string::npos);
			LoopFaults faults;
			DvSimulatedLoop loop(sink, faults);
			loop.Begin(config.width, config.height, 24000, 1001);
			for (uint32_t i = 0; i < 20; ++i)
				loop.Send(sequence.Make(i), 0);
			loop.End();
			// A capture-state change (rate) starts a second segment.
			DvSimulatedLoop loop2(sink, faults);
			loop2.Begin(config.width, config.height, 60000, 1001);
			for (uint32_t i = 20; i < 30; ++i)
				loop2.Send(sequence.Make(i), 0);
			loop2.End();
			Assert::IsTrue(recorder.Stop("test").find("recording stopped") != std::string::npos);
			recorder.Flush();
			Assert::AreEqual<uint64_t>(30, recorder.FramesWritten());
			const std::vector<std::string> segments = FindRawCorpusSegments(root);
			Assert::AreEqual<size_t>(2, segments.size());
			size_t replayed = 0;
			for (const std::string& folder : segments)
			{
				RawCorpusSegment seg;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), std::wstring(error.begin(), error.end()).c_str());
				Assert::AreEqual(std::string("Simulated DeckLink"), seg.keys["card"]);
				Assert::AreEqual(std::string("8-bit"), seg.keys["card_input_bit_depth"]);
				Assert::AreEqual(std::string("unit test"), seg.keys["note"]);
				Assert::AreEqual(200u, seg.width);
				Assert::IsTrue(seg.encoding == VideoFrameEncoding::R210);
				AnalyzerOptions options;
				options.expected = [&sequence](uint32_t i, LoopFrame& f) { f = sequence.Make(i); return true; };
				DvLoopbackAnalyzer analyzer(options);
				analyzer.OnState(seg.MakeState(true));
				std::vector<uint8_t> bytes;
				for (size_t i = 0; i < seg.frames.size(); ++i)
				{
					Assert::IsTrue(seg.LoadFrame(i, bytes));
					VideoFrame frame(bytes.data(), seg.frames[i].counter, seg.frames[i].timingTimestamp, nullptr);
					analyzer.OnFrame(frame, seg.frames[i].arrivalQpc);
				}
				const LoopReport r = analyzer.Finish();
				Logger::WriteMessage((folder + ": " + r.Summary() + "\n").c_str());
				Assert::IsTrue(r.BitExact(), std::wstring(folder.begin(), folder.end()).c_str());
				Assert::AreEqual<uint64_t>(seg.frames.size(), r.pictures);
				replayed += seg.frames.size();
			}
			Assert::AreEqual<size_t>(30, replayed);
		}

		// Burst mode: N consecutive frames after the skip, per capture state.
		TEST_METHOD(RecorderBurstModeRecordsAfterTheSkip)
		{
			const std::string root = TempFolder("vp-raw-capture-burst");
			RawCaptureOptions o;
			o.root = root;
			o.burstFrames = 5;
			o.burstSkipFrames = 3;
			o.minFreeGb = 0;
			o.watchFiles = false;
			RawCaptureRecorder recorder(o);
			struct Sink : IDvCaptureSink
			{
				RawCaptureRecorder& r;
				explicit Sink(RawCaptureRecorder& rec) : r(rec) {}
				void OnState(const VideoStateComPtr& s) override { r.OnVideoState(*s); }
				void OnFrame(const VideoFrame& f, int64_t) override { r.OnFrame(f); }
			} sink(recorder);
			SequenceConfig config;
			config.width = 192;
			config.height = 108;
			const PatternSequence sequence(config);
			DvSimulatedLoop loop(sink, LoopFaults());
			loop.Begin(192, 108, 50, 1);
			for (uint32_t i = 0; i < 20; ++i)
				loop.Send(sequence.Make(i), 0);
			loop.End();
			recorder.Flush();
			Assert::AreEqual<uint64_t>(5, recorder.FramesWritten());
			RawCorpusSegment seg;
			std::string error;
			const std::vector<std::string> segments = FindRawCorpusSegments(root);
			Assert::AreEqual<size_t>(1, segments.size());
			Assert::IsTrue(LoadRawCorpusSegment(segments[0], seg, error));
			Assert::AreEqual<uint64_t>(4, seg.frames.front().counter, L"first frame after the 3-frame skip");
		}

		// A VideoProcessorStreamRecorder --raw folder (8-bit ARGB) is read and
		// re-contained losslessly as r210 for the tunnel formatter.
		TEST_METHOD(StreamRecorderArgbCorpusReplaysThroughTheTunnelPath)
		{
			const std::string root = TempFolder("vp-stream-recorder-corpus");
			const uint32_t w = 192, h = 108;
			SequenceConfig config;
			config.width = w;
			config.height = h;
			const PatternSequence sequence(config);
			{
				std::ofstream events(root + "\\events.txt");
				events << "recorder start; QPC frequency 10000000; hardware timestamps in 100 ns units\n";
				events << "frame 0: mode 192x108p23.98 192x108 23.97602 Hz 8BitARGB\n";
				std::ofstream frames(root + "\\frames.csv");
				frames << "index,hw_time_100ns,hw_duration_100ns,stream_time,stream_duration,callback_qpc,periods,buffered,flags,no_input,hash,repeat,callback_us\n";
				for (uint32_t i = 0; i < 6; ++i)
				{
					ContainerOptions argb;
					argb.encoding = VideoFrameEncoding::ARGB_8BIT;
					std::vector<uint8_t> bytes;
					WrapRgb8(sequence.Make(i).rgb8, w, h, argb, bytes);
					char name[32];
					snprintf(name, sizeof(name), "\\raw_%05u.bin", i);
					std::ofstream raw(root + name, std::ios::binary);
					raw.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
					events << "frame " << i << ": raw frame saved (" << w << "x" << h << ", " << w * 4 << " bytes per row, 8BitARGB)\n";
					frames << i << "," << i * 417083 << ",417083,0,0," << 1000 + i << ",1,0,0x0,0,0,0,0\n";
				}
			}
			RawCorpusSegment seg;
			std::string error;
			Assert::IsTrue(LoadRawCorpusSegment(root, seg, error), std::wstring(error.begin(), error.end()).c_str());
			Assert::AreEqual(24000u, seg.rateNumerator);
			Assert::AreEqual(1001u, seg.rateDenominator);
			const Replay replay = ReplaySegment(seg, true);
			Logger::WriteMessage((replay.report.Summary() + "\n").c_str());
			Assert::AreEqual<uint64_t>(6, replay.report.pictures);
		}

		// Any recorded corpus replays through VP's tunnel path (opt-in:
		// VP_TEST_RAW_CORPUS_DIR = a recording root). Writes vp-decoded.csv per
		// segment (per-frame presentation and metadata, for the comparison
		// tools); with vp-expected.txt present the digests must match (the
		// permanent regression corpus; VP_TEST_RAW_CORPUS_BLESS=1 writes it).
		// Invariants on real captures: no rejected frames, scalar and AVX2
		// planes identical, and - when the segment holds a dovi_tool export of
		// the same title (rpu.json) - every decoded L1 tuple occurs in the RPU.
		TEST_METHOD(RecordedCorpusReplaysThroughTheTunnelPath)
		{
			const std::string root = EnvString("VP_TEST_RAW_CORPUS_DIR");
			if (root.empty())
			{
				Logger::WriteMessage("SKIP-NO-CORPUS RecordedCorpusReplaysThroughTheTunnelPath (set VP_TEST_RAW_CORPUS_DIR)\n");
				return;
			}
			const bool bless = EnvString("VP_TEST_RAW_CORPUS_BLESS") == "1";
			const std::vector<std::string> segments = FindRawCorpusSegments(root);
			Assert::IsFalse(segments.empty(), L"no corpus segments under VP_TEST_RAW_CORPUS_DIR");
			for (const std::string& folder : segments)
			{
				RawCorpusSegment seg;
				std::string error;
				Assert::IsTrue(LoadRawCorpusSegment(folder, seg, error), std::wstring(error.begin(), error.end()).c_str());
				const bool tunnelCapable = seg.ReplayEncoding(true) == VideoFrameEncoding::R210 ||
					seg.ReplayEncoding(true) == VideoFrameEncoding::R10b || seg.ReplayEncoding(true) == VideoFrameEncoding::R10l;
				if (!tunnelCapable)
				{
					Logger::WriteMessage((folder + ": not a tunnel container (" + seg.keys["encoding"] + "); skipped here, used by the LLDV comparison\n").c_str());
					continue;
				}
				const Replay replay = ReplaySegment(seg, true);
				const Replay scalar = ReplaySegment(seg, true, true);
				Logger::WriteMessage((folder + ": " + replay.report.Summary() + "\n").c_str());
				Assert::AreEqual<uint64_t>(0, replay.report.rejected, std::wstring(folder.begin(), folder.end()).c_str());
				Assert::IsTrue(replay.digest == scalar.digest, L"scalar and AVX2 unpack differ on a real capture");
				{
					std::ofstream csv(folder + "\\vp-decoded.csv", std::ios::trunc);
					for (const std::string& line : replay.decodedCsv) csv << line << "\n";
				}
				const std::string expected = folder + "\\vp-expected.txt";
				if (bless)
				{
					std::ofstream out(expected, std::ios::trunc);
					for (const std::string& line : replay.digest) out << line << "\n";
				}
				else if (RawCorpusDetail::FileExists(expected))
				{
					std::ifstream in(expected);
					std::vector<std::string> want;
					std::string line;
					while (std::getline(in, line)) if (!line.empty()) want.push_back(line);
					Assert::AreEqual(want.size(), replay.digest.size(), L"frame count changed");
					for (size_t k = 0; k < want.size(); ++k)
						Assert::AreEqual(want[k], replay.digest[k], std::wstring(folder.begin(), folder.end()).c_str());
				}
				const std::string rpu = folder + "\\rpu.json";
				if (RawCorpusDetail::FileExists(rpu))
				{
					// Conformance anchor: dovi_tool's reading of the same title.
					const auto rpuL1 = RpuL1Set(rpu);
					size_t checked = 0, missing = 0;
					for (size_t k = 1; k < replay.decodedCsv.size(); ++k)
					{
						std::stringstream ss(replay.decodedCsv[k]);
						std::vector<std::string> f;
						std::string item;
						while (std::getline(ss, item, ',')) f.push_back(item);
						if (f.size() < 7 || f[3] != "1") continue;
						++checked;
						if (!rpuL1.count({ std::stoi(f[4]), std::stoi(f[5]), std::stoi(f[6]) })) ++missing;
					}
					Logger::WriteMessage(("rpu.json anchor: " + std::to_string(checked) + " frames, " + std::to_string(missing) + " L1 tuples not in the RPU\n").c_str());
					Assert::AreEqual<size_t>(0, missing, L"decoded L1 not found in dovi_tool's reading of the title");
				}
			}
		}

		// A hardware loopback run (opt-in: VP_TEST_DV_LOOPBACK_DIR = the folder
		// with the VPV sender's sent.csv and VP's recording of the capture).
		// The pattern is regenerated from the sender's header line, frames are
		// identified by their stamps, and the run must be bit-exact; latency
		// uses the QPC both processes logged (same PC).
		TEST_METHOD(HardwareLoopbackRecordingAnalyses)
		{
			const std::string root = EnvString("VP_TEST_DV_LOOPBACK_DIR");
			if (root.empty())
			{
				Logger::WriteMessage("SKIP-NO-LOOPBACK HardwareLoopbackRecordingAnalyses (set VP_TEST_DV_LOOPBACK_DIR)\n");
				return;
			}
			std::ifstream sent(root + "\\sent.csv");
			Assert::IsTrue(static_cast<bool>(sent), L"sent.csv missing (the VPV sender's log)");
			std::string header;
			std::getline(sent, header);
			// "# sequence width=W height=H sequence_id=N scene_frames=S sdr_start=A sdr_length=B sdr_eos=E rich=R qpc_frequency=F"
			SequenceConfig config;
			int64_t qpcFrequency = 0;
			unsigned sid = config.sequenceId, eos = 1, rich = 1;
			sscanf_s(header.c_str(), "# sequence width=%u height=%u sequence_id=%u scene_frames=%u sdr_start=%u sdr_length=%u sdr_eos=%u rich=%u qpc_frequency=%lld",
				&config.width, &config.height, &sid, &config.sceneFrames, &config.sdrStart, &config.sdrLength, &eos, &rich, &qpcFrequency);
			config.sequenceId = static_cast<uint8_t>(sid);
			const size_t periodAt = header.find(" period=");
			if (periodAt != std::string::npos)
				config.period = static_cast<uint32_t>(std::strtoul(header.c_str() + periodAt + 8, nullptr, 10));
			config.sdrEos = eos != 0;
			config.richMetadata = rich != 0;
			std::map<uint32_t, int64_t> sentAt;
			std::string line;
			std::getline(sent, line);   // column names
			while (std::getline(sent, line))
			{
				unsigned index = 0;
				long long qpc = 0;
				if (sscanf_s(line.c_str(), "%u,%lld", &index, &qpc) == 2)
					sentAt[index] = qpc;
			}
			const PatternSequence sequence(config);
			AnalyzerOptions options;
			options.sequenceId = config.sequenceId;
			options.keepFrames = false;
			options.expected = [&sequence, &sentAt](uint32_t i, LoopFrame& f) { if (!sentAt.count(i)) return false; f = sequence.Make(i); return true; };
			options.sentQpc = [&sentAt](uint32_t i) { const auto it = sentAt.find(i); return it == sentAt.end() ? int64_t(0) : it->second; };
			options.qpcFrequency = qpcFrequency;
			for (const std::string& folder : FindRawCorpusSegments(root))
			{
				RawCorpusSegment seg;
				std::string error;
				if (!LoadRawCorpusSegment(folder, seg, error))
					continue;
				DvLoopbackAnalyzer analyzer(options);
				analyzer.OnState(seg.MakeState(true));
				std::vector<uint8_t> bytes;
				for (size_t i = 0; i < seg.frames.size(); ++i)
				{
					if (!seg.LoadFrame(i, bytes)) continue;
					VideoFrame frame(bytes.data(), seg.frames[i].counter, seg.frames[i].timingTimestamp, nullptr);
					analyzer.OnFrame(frame, seg.frames[i].arrivalQpc);
				}
				const LoopReport r = analyzer.Finish();
				Logger::WriteMessage(("HARDWARE LOOP " + folder + ": " + r.Summary() + "\n").c_str());
				Assert::IsTrue(r.identified > 0, L"no stamped frame came back");
				Assert::IsTrue(r.BitExact(), std::wstring(folder.begin(), folder.end()).c_str());
			}
		}
	};
}
