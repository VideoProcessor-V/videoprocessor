// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only

// CameraDump (link/CameraDump.*): the VP_CAMERA_DUMP writer of camera K3. The .npy header and data, the file names (the contract of
// tools/eval/vector_source.py), the bounded queue that drops visibly, the writer thread that exists only when asked for, and the
// environment variable. Files go to a private temporary directory. CPU only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CppUnitTest.h"

#include <VP-V/modules/motion/estimation/global_model/link/CameraDump.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace CameraDumpTestsNs
{
	using namespace CameraLink;

	std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	void RemoveTree(const std::string& dir)
	{
		WIN32_FIND_DATAA f;
		HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &f);
		if (h != INVALID_HANDLE_VALUE)
		{
			do
			{
				const std::string name = f.cFileName;
				if (name == "." || name == "..")
					continue;
				if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					RemoveTree(dir + "\\" + name);
				else
					DeleteFileA((dir + "\\" + name).c_str());
			} while (FindNextFileA(h, &f));
			FindClose(h);
		}
		RemoveDirectoryA(dir.c_str());
	}

	struct TempDir
	{
		std::string path;
		TempDir()
		{
			static std::atomic<int> counter{ 0 };
			char dir[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, dir);
			path = std::string(dir) + "vp-camera-dump-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(counter++);
			RemoveTree(path);
		}
		~TempDir() { RemoveTree(path); }
	};

	struct EnvGuard
	{
		std::string name, old;
		bool had = false;
		EnvGuard(const char* n, const char* value) : name(n)
		{
			char* v = nullptr;
			size_t sz = 0;
			if (_dupenv_s(&v, &sz, n) == 0 && v)
			{
				had = true;
				old = v;
				free(v);
			}
			_putenv_s(n, value);
		}
		~EnvGuard() { _putenv_s(name.c_str(), had ? old.c_str() : ""); }
	};

	bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

	std::vector<uint8_t> ReadAll(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	std::string ReadText(const std::string& path)
	{
		const std::vector<uint8_t> b = ReadAll(path);
		return std::string(b.begin(), b.end());
	}

	// gridW x gridH field; forward vector of block i = (i + 0.5, -(i + 0.25)); backward = the negated forward.
	std::shared_ptr<FieldCopy> MakeField(uint64_t from, uint64_t to, int w, int h, bool backward)
	{
		std::shared_ptr<FieldCopy> f = std::make_shared<FieldCopy>();
		f->seq = to;
		f->pairFrom = from;
		f->pairTo = to;
		f->gridW = w;
		f->gridH = h;
		f->blockSize = 16;
		const size_t n = static_cast<size_t>(w) * h;
		for (size_t i = 0; i < n; ++i)
		{
			f->fwdX.push_back(static_cast<float>(i) + 0.5f);
			f->fwdY.push_back(-(static_cast<float>(i) + 0.25f));
			f->fwdTrust.push_back(1.0f);
			if (backward)
			{
				f->bwdX.push_back(-(static_cast<float>(i) + 0.5f));
				f->bwdY.push_back(static_cast<float>(i) + 0.25f);
				f->bwdTrust.push_back(1.0f);
			}
		}
		return f;
	}

	std::shared_ptr<CameraModel::CameraMotion> MakeRecord(const FieldCopy& f)
	{
		std::shared_ptr<CameraModel::CameraMotion> r = std::make_shared<CameraModel::CameraMotion>();
		r->seq = f.seq;
		r->pairFrom = f.pairFrom;
		r->pairTo = f.pairTo;
		r->pairSeconds = 1.0 / 24.0;
		r->valid = true;
		r->reason = CameraModel::Reason::Ok;
		r->confidence = 0.875;
		r->params.type = CameraModel::ModelType::Similarity;
		r->params.h[2] = 3.5;
		r->costMicros = 300;
		return r;
	}

	float FloatAt(const std::vector<uint8_t>& npy, size_t dataOffset, size_t index)
	{
		float v;
		std::memcpy(&v, &npy[dataOffset + index * sizeof(float)], sizeof(float));
		return v;
	}

	TEST_CLASS(CameraDumpTests)
	{
	public:
		TEST_METHOD(FileNamesFollowTheVectorSourceContract)
		{
			Assert::AreEqual(std::string("flow_000011_000012.npy"), FlowFileName(11, 12));
			Assert::AreEqual(std::string("flow_000012_000011.npy"), FlowFileName(12, 11));
			Assert::AreEqual(std::string("camera_000012.json"), CameraFileName(12));
			Assert::AreEqual(std::string("flow_1234567_000001.npy"), FlowFileName(1234567, 1), L"wider than 6 digits is not truncated");
		}

		TEST_METHOD(TheNpyHeaderAndDataAreExact)
		{
			const int w = 3, h = 2;
			const std::shared_ptr<FieldCopy> f = MakeField(1, 2, w, h, false);
			const std::vector<uint8_t> npy = BuildNpy(f->fwdX, f->fwdY, w, h);
			Assert::IsTrue(npy.size() > 10);
			const uint8_t magic[8] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0 };
			Assert::AreEqual(0, std::memcmp(npy.data(), magic, 8));
			const size_t headerLength = npy[8] | (static_cast<size_t>(npy[9]) << 8);
			Assert::AreEqual<size_t>(0, (10 + headerLength) % 64, L"the prefix is a multiple of 64");
			const std::string header(npy.begin() + 10, npy.begin() + 10 + headerLength);
			Assert::IsTrue(header.find("'descr': '<f4'") != std::string::npos);
			Assert::IsTrue(header.find("'fortran_order': False") != std::string::npos);
			Assert::IsTrue(header.find("'shape': (2, 3, 2)") != std::string::npos, L"(H, W, 2): channel 0 = dx, 1 = dy");
			Assert::AreEqual('\n', header.back());
			const size_t data = 10 + headerLength;
			Assert::AreEqual<size_t>(data + static_cast<size_t>(w) * h * 2 * sizeof(float), npy.size());
			for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
			{
				Assert::AreEqual(f->fwdX[i], FloatAt(npy, data, i * 2), L"dx");
				Assert::AreEqual(f->fwdY[i], FloatAt(npy, data, i * 2 + 1), L"dy");
			}
			Assert::IsTrue(BuildNpy(f->fwdX, f->fwdY, w + 1, h).empty(), L"planes that do not fit the shape give nothing");
			Assert::IsTrue(BuildNpy(f->fwdX, f->fwdY, 0, h).empty());
		}

		TEST_METHOD(TheJsonCarriesTheModeAndTheRecord)
		{
			const std::shared_ptr<FieldCopy> f = MakeField(11, 12, 3, 2, true);
			const std::shared_ptr<CameraModel::CameraMotion> r = MakeRecord(*f);
			const std::string with = CameraJson(r.get(), *f, "engine_camera");
			Assert::IsTrue(with.find("\"mode\": \"engine_camera\"") != std::string::npos);
			Assert::IsTrue(with.find("\"record\": true") != std::string::npos);
			Assert::IsTrue(with.find("\"reason\": \"ok\"") != std::string::npos);
			Assert::IsTrue(with.find("\"confidence\": 0.875") != std::string::npos);
			Assert::IsTrue(with.find("\"pairFrom\": 11") != std::string::npos && with.find("\"pairTo\": 12") != std::string::npos);
			Assert::IsTrue(with.find("\"backward\": true") != std::string::npos);
			Assert::IsTrue(with.find("\"params\": [1, 0, 3.5, 0, 1, 0, 0, 0, 1]") != std::string::npos, W(with).c_str());
			const std::string without = CameraJson(nullptr, *f, "../../evil");
			Assert::IsTrue(without.find("\"record\": false") != std::string::npos);
			Assert::IsTrue(without.find("\"mode\": \".._.._evil\"") != std::string::npos, W(without).c_str());
			Assert::IsTrue(without.find('/') == std::string::npos, L"the mode is one safe token");
		}

		TEST_METHOD(AFullQueueDropsAndCountsEveryDrop)
		{
			TempDir dir;
			DumpConfig c;
			c.directory = dir.path;
			c.queueCapacity = 2;
			CameraDump dump(c, false);
			Assert::IsFalse(dump.HasThread(), L"no thread unless asked for");
			int accepted = 0;
			for (int i = 0; i < 5; ++i)
				accepted += dump.Offer(MakeField(i, i + 1, 2, 2, false), nullptr) ? 1 : 0;
			Assert::AreEqual(2, accepted);
			const DumpStats s = dump.Stats();
			Assert::AreEqual<uint64_t>(5, s.offered);
			Assert::AreEqual<uint64_t>(3, s.dropped, L"the new items are dropped, counted");
			Assert::AreEqual<size_t>(2, s.queued);
			Assert::AreEqual<uint64_t>(0, s.written);
			Assert::IsFalse(dump.Offer(nullptr, nullptr), L"nothing to write");
			Assert::AreEqual<uint64_t>(4, dump.Stats().dropped);
			// Draining frees the queue again.
			Assert::AreEqual<size_t>(2, dump.DrainOnce());
			Assert::IsTrue(dump.Offer(MakeField(9, 10, 2, 2, false), nullptr));
		}

		TEST_METHOD(DrainOnceWritesBothDirectionsAndTheJson)
		{
			TempDir dir;
			DumpConfig c;
			c.directory = dir.path;
			c.clip = "myclip";
			c.mode = "engine_camera";
			CameraDump dump(c, false);
			const std::shared_ptr<FieldCopy> f = MakeField(11, 12, 3, 2, true);
			Assert::IsTrue(dump.Offer(f, MakeRecord(*f)));
			Assert::AreEqual<size_t>(1, dump.DrainOnce());
			const std::string base = dir.path + "\\myclip\\";
			Assert::AreEqual(base.substr(0, base.size() - 1), dump.ClipDirectory());
			Assert::IsTrue(Exists(base + "flow_000011_000012.npy"), L"forward  a -> b");
			Assert::IsTrue(Exists(base + "flow_000012_000011.npy"), L"backward b -> a");
			Assert::IsTrue(Exists(base + "camera_000012.json"));
			Assert::IsFalse(Exists(base + "flow_000011_000012.npy.tmp"), L"no temporary left behind");
			const std::vector<uint8_t> fwd = ReadAll(base + "flow_000011_000012.npy");
			const std::vector<uint8_t> bwd = ReadAll(base + "flow_000012_000011.npy");
			Assert::IsTrue(fwd == BuildNpy(f->fwdX, f->fwdY, 3, 2));
			Assert::IsTrue(bwd == BuildNpy(f->bwdX, f->bwdY, 3, 2));
			Assert::IsTrue(ReadText(base + "camera_000012.json").find("\"mode\": \"engine_camera\"") != std::string::npos);
			const DumpStats s = dump.Stats();
			Assert::AreEqual<uint64_t>(1, s.written);
			Assert::AreEqual<uint64_t>(0, s.dropped + s.failed);
		}

		TEST_METHOD(AForwardOnlyPairWritesNoBackwardFile)
		{
			TempDir dir;
			DumpConfig c;
			c.directory = dir.path;
			CameraDump dump(c, false);
			Assert::IsTrue(dump.Offer(MakeField(4, 5, 2, 2, false), nullptr));
			Assert::AreEqual<size_t>(1, dump.DrainOnce());
			const std::string base = dir.path + "\\clip\\";
			Assert::IsTrue(Exists(base + "flow_000004_000005.npy"));
			Assert::IsFalse(Exists(base + "flow_000005_000004.npy"));
			Assert::IsTrue(ReadText(base + "camera_000005.json").find("\"record\": false") != std::string::npos);
		}

		TEST_METHOD(AWriteThatCannotHappenIsCountedNotLost)
		{
			TempDir parent;
			CreateDirectoryA(parent.path.c_str(), nullptr);
			const std::string file = parent.path + "\\not_a_directory";
			{ std::ofstream out(file, std::ios::binary); out << "x"; }
			DumpConfig c;
			c.directory = file;   // a file: no directory can be made beneath it
			CameraDump dump(c, false);
			Assert::IsTrue(dump.Offer(MakeField(1, 2, 2, 2, false), nullptr));
			Assert::AreEqual<size_t>(1, dump.DrainOnce());
			Assert::AreEqual<uint64_t>(1, dump.Stats().failed);
			Assert::AreEqual<uint64_t>(0, dump.Stats().written);
		}

		TEST_METHOD(TheClipNameIsOneSafePathComponent)
		{
			DumpConfig c;
			c.directory = "D:\\x";
			c.clip = "..\\..\\evil clip";
			CameraDump dump(c, false);
			const std::string clip = dump.Config().clip;
			Assert::IsTrue(clip.find('\\') == std::string::npos && clip.find('/') == std::string::npos && clip.find(' ') == std::string::npos, W(clip).c_str());
			DumpConfig d;
			d.directory = "D:\\x";
			d.clip = "..";
			Assert::AreEqual(std::string("clip"), CameraDump(d, false).Config().clip);
		}

		TEST_METHOD(TheWriterThreadWritesAndIsJoinedOnDestruction)
		{
			TempDir dir;
			DumpConfig c;
			c.directory = dir.path;
			c.queueCapacity = 8;
			{
				CameraDump dump(c, true);
				Assert::IsTrue(dump.HasThread());
				for (uint64_t i = 1; i <= 3; ++i)
					Assert::IsTrue(dump.Offer(MakeField(i, i + 1, 2, 2, true), nullptr));
				// the destructor writes what is queued before it joins
			}
			const std::string base = dir.path + "\\clip\\";
			for (uint64_t i = 1; i <= 3; ++i)
			{
				Assert::IsTrue(Exists(base + FlowFileName(i, i + 1)), W(FlowFileName(i, i + 1)).c_str());
				Assert::IsTrue(Exists(base + FlowFileName(i + 1, i)));
				Assert::IsTrue(Exists(base + CameraFileName(i + 1)));
			}
		}

		TEST_METHOD(TheWriterKeepsUpWhileTheProducerRuns)
		{
			TempDir dir;
			DumpConfig c;
			c.directory = dir.path;
			c.queueCapacity = 4;
			CameraDump dump(c, true);
			for (uint64_t i = 1; i <= 4; ++i)
				dump.Offer(MakeField(i, i + 1, 2, 2, false), nullptr);
			const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (dump.Stats().written + dump.Stats().failed < 4 && std::chrono::steady_clock::now() < limit)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			const DumpStats s = dump.Stats();
			Assert::AreEqual<uint64_t>(4, s.written);
			Assert::AreEqual<uint64_t>(0, s.dropped + s.failed);
			Assert::AreEqual<size_t>(0, s.queued);
		}

		TEST_METHOD(TheEnvironmentVariableNamesTheDirectory)
		{
			{
				EnvGuard g("VP_CAMERA_DUMP", "");
				Assert::AreEqual(std::string(), CameraDump::DirectoryFromEnvironment(), L"unset: no dump");
			}
			{
				EnvGuard g("VP_CAMERA_DUMP", "1");
				Assert::AreEqual(std::string(), CameraDump::DirectoryFromEnvironment(), L"=1 is the status log, not a directory");
			}
			{
				EnvGuard g("VP_CAMERA_DUMP", "D:\\dumps\\run1");
				Assert::AreEqual(std::string("D:\\dumps\\run1"), CameraDump::DirectoryFromEnvironment());
			}
			{
				EnvGuard g("VP_CAMERA_DUMP_CLIP", "");
				Assert::AreEqual(std::string("clip"), CameraDump::ClipFromEnvironment());
			}
			{
				EnvGuard g("VP_CAMERA_DUMP_CLIP", "crowd run");
				Assert::AreEqual(std::string("crowd_run"), CameraDump::ClipFromEnvironment());
			}
		}
	};
}
