// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Registered fix D8 (doc FIX-B6-D8 before/after table): detail_pulse_stabilise (DP) and texture_restore (TR) restart on the SHARED
// cut / fade verdict (U7 CutFadeJudge::ClassifyEvents over the two P4 records, read from the frame analysis store's scene group)
// instead of their retired private tile judges. The target is TRUTH (owner ruling 2026-11-15): labelled real cuts and fades, not
// the retired tile law. This harness measures, on real clips from VP_TEST_CLIP_MANIFEST and on a synthetic fade and a synthetic
// flash, how often the SHARED restart decision agrees with the truth, and gates it:
//   - per frame: decode (RealClipLibrary, 8-bit gray), P1 half luma + P4 source stats at the clip size (the lane's own functions),
//     U7 verdict for seq >= 2 (prev, cur, false, false, then the pair folded into the part's one CutFadeJudge::FadeRun, fade = pair
//     law OR run: exactly the skeleton's SceneFactsOf since the multi-frame fade law), published into a FrameAnalysis::Store
//     registered as the one SourceFrameTap store (the harness plays the skeleton's seal; the skeleton itself is not run);
//   - truth per frame of the part (TruthRestarts), scored on the part's frames AFTER its first (the first frame has no prior
//     frame, so no detector decision exists there; excluded for DP and TR alike, never an automatic agree): the manifest's cuts_s frames;
//     the manifest's fade_in / fade_out events (t0..t1 s): every frame whose pair with the previous frame spans part of the ramp,
//     i.e. t(k) > t0 and t(k-1) < t1 (the generator's ffmpeg fade filter); synthetic fade: FadeGain(k) != FadeGain(k-1);
//     synthetic flash: no restart (a flash is not a restart). Labels are never invented: a real clip needs cuts_s or cut_free.
//     tos_24p_3min carries cut labels only, so its rows measure cut truth only;
//   - the CPU models twice: the retired TileJudgeReference law (sceneReader null) and the shared verdict (ReadSceneFromStore);
//     the shared decision is compared with the truth (truth agree / miss / extra); the retired-law agreement, restarts and the
//     picture difference on the frames where the two decisions differ are information columns only;
//   - the GPU modules with the store on (Params::sceneReader null -> DefaultSceneReader -> ReadSceneFromStore): SceneMissing().
// Gate per row: truth agreement >= 99.5 % (kAgreementGatePct, the owner's value) for each module and SceneMissing == 0 for each
// GPU module. Rows whose law misses slow fades or calls a flash a cut are red until the U7 fixes land (102b); not loosened here.
// Rows go to the CSV at VP_TEST_D8_OUT (header written when the file is new) and to the test log.
// Skips VISIBLY (SKIP-NO-GPU / SKIP-NO-FOOTAGE marker, a failure under VP_TEST_REQUIRE_GPU / _FOOTAGE / _ALL) when the GPU is not
// the hardware adapter (VP_TEST_GPU=hardware), the device cannot be created, or the manifest / clip / ffmpeg is missing.
// Knobs: VP_TEST_D8_SCALE (integer box factor for the models and the GPU, default 2), VP_TEST_D8_MAX_FRAMES (cap per row).
// Diagnostic output (WAIVER-D8-FRAME-DUMP, additive, output only; no gate reads it): VP_D8_FRAME_DUMP=<dir> writes one CSV per
// row part, <dir>\d8-frames-<row>-part<N>of<M>.csv, one line per frame of the part (truth label, U7 verdict and its scores, the
// DP / TR restart decisions). Unset: nothing is written.
// Not covered: the skeleton's real seal timing, look-ahead (MeasureAhead), vector fields (cell-trust column is n/a).
// GpuHardware: long real-clip rows, split into parts so that each test method stays within one GPU chunk.

#include "pch.h"
#include "CppUnitTest.h"

#include <VP-V/tests/support/DetailPulseModel.h>
#include <VP-V/tests/support/ExtendedRangeGpuProbe.h>
#include <VP-V/tests/support/GrainReferenceModel.h>
#include <VP-V/tests/support/HardwarePrerequisites.h>
#include <VP-V/tests/support/RealClipLibrary.h>
#include <VP-V/tests/support/TextureRestoreModel.h>

#include <VP-V/core/FrameAnalysisStore.h>
#include <VP-V/modules/input/source_analysis/SourceFrameTap.h>
#include <VP-V/modules/input/source_analysis/lane/LaneCpuProducts.h>
#include <VP-V/modules/repair/texture_restore/DetailPulse.h>
#include <VP-V/modules/repair/texture_restore/SceneRestart.h>
#include <VP-V/modules/repair/texture_restore/TextureRestore.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace D8SceneJudgeAgreementNs
{
	namespace HP = HardwarePrerequisites;

	constexpr double kAgreementGatePct = 99.5;
	constexpr int kSyntheticFrames = 240;
	constexpr int kKeepInputTextures = 4;   // input textures kept alive after their Apply (the modules may still read them)

	enum class Synthetic { None, Fade, Flash };

	inline std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

	inline int EnvInt(const char* name, int fallback)
	{
		const std::string v = RealClip::Env(name);
		if (v.empty()) return fallback;
		const int n = std::atoi(v.c_str());
		return n > 0 ? n : fallback;
	}

	inline double MsSince(std::chrono::steady_clock::time_point t0)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}

	// The synthetic rows' picture: a real static clip (rate_24) with a fade to black and back, or single bright frames.
	inline double FadeGain(int k)
	{
		if (k < 40) return 1.0;
		if (k < 88) return 1.0 - (k - 40 + 1) / 48.0;
		if (k < 112) return 0.0;
		if (k < 160) return (k - 112 + 1) / 48.0;
		return 1.0;
	}
	inline bool FlashFrame(int k) { return k % 24 == 12; }

	inline void ApplySynthetic(Synthetic kind, int k, std::vector<uint8_t>& luma)
	{
		if (kind == Synthetic::Fade)
		{
			const double g = FadeGain(k);
			for (uint8_t& p : luma)
			{
				const double v = 16.0 + (static_cast<double>(p) - 16.0) * g;
				p = static_cast<uint8_t>((std::max)(0.0, (std::min)(255.0, std::floor(v + 0.5))));
			}
		}
		else if (kind == Synthetic::Flash && FlashFrame(k))
		{
			for (uint8_t& p : luma)
				p = static_cast<uint8_t>((std::min)(255, static_cast<int>(p) + 100));
		}
	}

	struct ModuleRow
	{
		int frames = 0, oldRestarts = 0, sharedRestarts = 0, agree = 0, differ = 0;
		int truthFrames = 0, truthAgree = 0, truthMiss = 0, truthExtra = 0;   // the shared decision against the truth (the gate)
		double diffMax = 0.0, diffSum = 0.0;
		double diffPixels = 0.0;
		unsigned long long sceneMissing = 0;
		double msOld = 0.0, msShared = 0.0, msGpu = 0.0;
		double AgreementPct() const { return frames > 0 ? 100.0 * agree / frames : 0.0; }   // retired law: information only
		// Over the scored frames only (the part's frames after its first: see TruthRestarts).
		double TruthAgreementPct() const { return truthFrames > 0 ? 100.0 * truthAgree / truthFrames : 0.0; }
		void CountTruth(bool shared, bool truth)
		{
			++truthFrames;
			if (shared == truth) ++truthAgree;
			else if (truth) ++truthMiss;
			else ++truthExtra;
		}
		double DiffMean() const { return diffPixels > 0.0 ? diffSum / diffPixels : 0.0; }
	};

	template <typename ImageT>
	void AddDiff(ModuleRow& r, const ImageT& a, const ImageT& b)
	{
		for (size_t i = 0; i < a.v.size() && i < b.v.size(); ++i)
		{
			const double d = std::fabs(a.v[i] - b.v[i]);
			r.diffMax = (std::max)(r.diffMax, d);
			r.diffSum += d;
		}
		r.diffPixels += static_cast<double>(a.v.size());
	}

	struct RowFacts
	{
		std::string row, part, device;
		std::string truthLabels;   // where the truth comes from (no commas: a CSV field)
		int planeW = 0, planeH = 0, u7Cuts = 0, u7Fades = 0, u7Flashes = 0, truthRestarts = 0;
		double decodeMs = 0.0;
	};

	// Truth restart per frame of the part [begin, end) (index k - begin): see the file comment. Index 0 (the part's first frame)
	// stays 0 and is NOT scored: it has no prior frame, so no detector decision exists there.
	inline std::vector<char> TruthRestarts(const RealClip::Clip& clip, Synthetic synthetic, int begin, int end, double hz,
		std::string& labels)
	{
		Assert::IsTrue(end > begin, L"D8 truth: the part has frames");
		std::vector<char> truth(static_cast<size_t>(end - begin), 0);
		if (synthetic == Synthetic::Fade)
		{
			labels = "synthetic fade gain";
			for (int k = begin + 1; k < end; ++k)
				if (FadeGain(k) != FadeGain(k - 1)) truth[static_cast<size_t>(k - begin)] = 1;
			return truth;
		}
		if (synthetic == Synthetic::Flash)
		{
			labels = "synthetic flash (no restart)";
			return truth;
		}
		Assert::IsTrue(clip.hasCuts || clip.cutFree,
			Wide("D8 truth: clip " + clip.id + " has neither cuts_s nor cut_free in the manifest; truth is not invented").c_str());
		Assert::IsTrue(hz > 0.0, Wide("D8 truth: clip " + clip.id + " has no rate").c_str());
		labels = clip.hasCuts ? "manifest cuts_s" : "manifest cut_free";
		for (double c : clip.cuts)
		{
			const int f = static_cast<int>(std::floor(c * hz + 0.5));
			if (f > begin && f < end) truth[static_cast<size_t>(f - begin)] = 1;
		}
		int fades = 0;
		for (const RealClip::Event& e : clip.events)
		{
			if (e.type != "fade_in" && e.type != "fade_out") continue;
			Assert::IsNotNull(e.json, L"D8 truth: the fade event's record");
			const double t0 = e.json->Num("t0", -1.0), t1 = e.json->Num("t1", -1.0);
			Assert::IsTrue(t0 >= 0.0 && t1 > t0, Wide("D8 truth: clip " + clip.id + " has a " + e.type + " event without t0 < t1").c_str());
			++fades;
			for (int k = begin + 1; k < end; ++k)
				if (k / hz > t0 && (k - 1) / hz < t1) truth[static_cast<size_t>(k - begin)] = 1;
		}
		labels += fades > 0 ? " + manifest fade events (" + std::to_string(fades) + ")" : " (cut labels only)";
		return truth;
	}

	inline void WriteRow(const RowFacts& f, const char* module, const ModuleRow& r)
	{
		char line[1024];
		snprintf(line, sizeof(line),
			"%s,%s,%s,%d,%d,%d,%.3f,%d,%.4f,%.4f,%llu,n/a,%dx%d,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f,%d,%d,%s,%s",
			f.row.c_str(), f.part.c_str(), module, r.frames, r.oldRestarts, r.sharedRestarts, r.AgreementPct(), r.differ,
			r.diffMax, r.DiffMean(), r.sceneMissing, f.planeW, f.planeH, f.u7Cuts, f.u7Fades, f.u7Flashes, f.truthRestarts,
			r.frames ? r.msOld / r.frames : 0.0, r.frames ? r.msShared / r.frames : 0.0, r.frames ? r.msGpu / r.frames : 0.0,
			r.frames ? f.decodeMs / r.frames : 0.0, r.TruthAgreementPct(), r.truthMiss, r.truthExtra, f.truthLabels.c_str(),
			f.device.c_str());
		Logger::WriteMessage((std::string("D8ROW ") + line + "\n").c_str());
		std::string path = RealClip::Env("VP_TEST_D8_OUT");
		if (path.empty())
		{
			char temp[MAX_PATH] = {};
			GetTempPathA(MAX_PATH, temp);
			path = std::string(temp) + "vp-d8-scene-judge.csv";
			Logger::WriteMessage(("D8 VP_TEST_D8_OUT not set; CSV at " + path + "\n").c_str());
		}
		FILE* file = nullptr;
		if (fopen_s(&file, path.c_str(), "ab") != 0 || !file)
		{
			Assert::Fail(Wide("D8 cannot open the CSV " + path).c_str());
			return;
		}
		std::fseek(file, 0, SEEK_END);
		if (std::ftell(file) == 0)
			std::fputs("clip,part,module,frames,old_judge_restarts,shared_judge_restarts,agreement_pct,frames_differing,"
				"picture_diff_max_code10,picture_diff_mean_code10,scene_missing,cell_trust_ms_per_frame,plane,u7_cuts,u7_fades,"
				"u7_flashes,truth_restarts,model_old_ms_per_frame,model_shared_ms_per_frame,gpu_ms_per_frame,decode_p4_ms_per_frame,"
				"truth_agree_pct,truth_miss,truth_extra,truth_labels,device\r\n",
				file);
		std::fputs(line, file);
		std::fputs("\r\n", file);
		std::fclose(file);
	}

	// WAIVER-D8-FRAME-DUMP: the per-frame diagnostic CSV (see the file comment). Output only; never read by an assertion.
	struct FrameDump
	{
		bool on = false;
		std::string path;
		std::vector<std::string> lines;
		void Open(const char* rowName, int part, int parts)
		{
			const std::string dir = RealClip::Env("VP_D8_FRAME_DUMP");
			if (dir.empty()) return;
			CreateDirectoryA(dir.c_str(), nullptr);
			path = dir;
			if (path.back() != '\\' && path.back() != '/') path += "\\";
			path += std::string("d8-frames-") + rowName + "-part" + std::to_string(part + 1) + "of" + std::to_string(parts) + ".csv";
			on = true;
		}
		void Add(const std::string& line) { if (on) lines.push_back(line); }
		void Write() const
		{
			if (!on) return;
			FILE* file = nullptr;
			if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
			{
				Logger::WriteMessage(("D8 frame dump: cannot open " + path + " (diagnostic only)\n").c_str());
				return;
			}
			std::fputs("clip,part,frame,seq,time_s,scored,truth_restart,truth_label,u7_cut,u7_fade,u7_flash,u7_cut_possible,"
				"u7_hist_distance,u7_hist_frac,mean_prev,mean_cur,spread_prev,spread_cur,dp_restart,tr_restart,dp_old_restart,"
				"tr_old_restart\r\n", file);
			for (const std::string& l : lines)
			{
				std::fputs(l.c_str(), file);
				std::fputs("\r\n", file);
			}
			std::fclose(file);
			Logger::WriteMessage(("D8 frame dump: " + std::to_string(lines.size()) + " frames to " + path + "\n").c_str());
		}
	};

	struct StoreGuard
	{
		const void* owner;
		VpDetailRestore::SceneReaderFn previous;
		StoreGuard(const void* o, FrameAnalysis::Store* store) : owner(o), previous(VpDetailRestore::DefaultSceneReader())
		{
			VpFrameAnalysis::SourceFrameTap::SetStore(owner, store);
			VpDetailRestore::DefaultSceneReader() = &VpDetailRestore::ReadSceneFromStore;
		}
		~StoreGuard()
		{
			VpDetailRestore::DefaultSceneReader() = previous;
			VpFrameAnalysis::SourceFrameTap::SetStore(owner, nullptr);
		}
	};

	struct Gpu
	{
		pl_log log = nullptr;
		pl_d3d11 d3d = nullptr;
		pl_gpu gpu = nullptr;
		pl_dispatch dispatch = nullptr;
		std::string device;
		bool Create()
		{
			pl_log_params logParams{};
			logParams.log_level = PL_LOG_NONE;
			log = pl_log_create(PL_API_VER, &logParams);
			const pl_d3d11_params deviceParams = ExtendedRangeGpuProbe::TestDeviceParams(&device);
			d3d = pl_d3d11_create(log, &deviceParams);
			if (!d3d) return false;
			gpu = d3d->gpu;
			dispatch = pl_dispatch_create(log, gpu);
			return dispatch != nullptr;
		}
		~Gpu()
		{
			if (dispatch) pl_dispatch_destroy(&dispatch);
			if (d3d) pl_d3d11_destroy(&d3d);
			if (log) pl_log_destroy(&log);
		}
		pl_tex Upload(const std::vector<uint16_t>& code10, int w, int h) const
		{
			std::vector<uint16_t> data(code10.size());
			for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint16_t>(code10[i] << 6);
			pl_tex_params p{};
			p.w = w; p.h = h; p.format = pl_find_named_fmt(gpu, "r16"); p.sampleable = true; p.initial_data = data.data();
			return pl_tex_create(gpu, &p);
		}
		static pl_frame LumaFrame(pl_tex tex)
		{
			pl_frame f{};
			f.num_planes = 1;
			f.planes[0].texture = tex; f.planes[0].components = 1; f.planes[0].component_mapping[0] = PL_CHANNEL_Y;
			f.repr.sys = PL_COLOR_SYSTEM_BT_709; f.repr.levels = PL_COLOR_LEVELS_LIMITED;
			f.repr.bits.sample_depth = 16; f.repr.bits.color_depth = 10; f.repr.bits.bit_shift = 6;
			f.color = pl_color_space_bt709;
			return f;
		}
	};

	// One table row (or one part of it): see the file comment.
	inline void RunRow(const char* test, const char* clipId, Synthetic synthetic, const char* rowName, int part, int parts)
	{
		Logger::WriteMessage((std::string("D8 ") + test + ": row " + rowName + " part " + std::to_string(part + 1) + "/" +
			std::to_string(parts) + "\n").c_str());
		if (!HP::RequireOrSkip(HP::Kind::Gpu, test, ExtendedRangeGpuProbe::OnHardwareGpu(),
				"VP_TEST_GPU is not 'hardware': the D8 agreement gate runs on the hardware adapter only (set VP_TEST_GPU=hardware, VP_TEST_ADAPTER=NVIDIA)"))
			return;
		const RealClip::Manifest manifest = RealClip::Manifest::Load();
		if (!HP::RequireOrSkip(HP::Kind::Footage, test, manifest.Loaded(), "clip manifest not loaded: " + manifest.error))
			return;
		if (!HP::RequireOrSkip(HP::Kind::Footage, test, RealClip::FileExists(manifest.ffmpeg), "ffmpeg not found at " + manifest.ffmpeg))
			return;
		const RealClip::Clip* clip = manifest.Find(clipId);
		if (!HP::RequireOrSkip(HP::Kind::Footage, test, clip != nullptr && RealClip::FileExists(manifest.FullPath(*clip)),
				std::string("clip ") + clipId + " not in the manifest or its file is missing (" + manifest.path + ")"))
			return;
		Gpu g;
		const bool created = g.Create();
		if (!HP::RequireOrSkip(HP::Kind::Gpu, test, created, "the libplacebo D3D11 device could not be created on " + g.device))
			return;
		Logger::WriteMessage(("D8 GPU: " + g.device + "\n").c_str());

		const int total = synthetic == Synthetic::None ? clip->frames : (std::min)(kSyntheticFrames, clip->frames);
		Assert::IsTrue(total >= 2 && parts >= 1 && part >= 0 && part < parts, L"D8: the row has frames");
		const int begin = static_cast<int>(static_cast<long long>(total) * part / parts);
		int end = static_cast<int>(static_cast<long long>(total) * (part + 1) / parts);
		const int cap = EnvInt("VP_TEST_D8_MAX_FRAMES", 0);
		if (cap > 0) end = (std::min)(end, begin + cap);
		const int scale = EnvInt("VP_TEST_D8_SCALE", 2);
		const int W = clip->width, H = clip->height;
		const int pw = W / scale, ph = H / scale;
		Assert::IsTrue(pw >= 32 && ph >= 32, L"D8: the model plane is large enough");
		const double hz = clip->RateHz() > 0.0 ? clip->RateHz() : 0.0;

		RealClip::Decoder decoder;
		std::string error;
		Assert::IsTrue(decoder.Open(manifest, *clip, "", error), Wide("D8 decode: " + error).c_str());

		FrameAnalysis::Store store(16);
		store.SetEnabled(true);
		static const char ownerTag = 0;
		const StoreGuard guard(&ownerTag, &store);
		Assert::IsTrue(VpFrameAnalysis::SourceFrameTap::SingleStore() == &store,
			L"D8: the harness store is the one registered frame analysis store (another owner is registered)");

		RowFacts facts;
		facts.row = rowName;
		facts.part = std::to_string(part + 1) + "/" + std::to_string(parts) + " [" + std::to_string(begin) + "-" + std::to_string(end) + ")";
		facts.device = g.device;
		facts.planeW = pw; facts.planeH = ph;
		const std::vector<char> truth = TruthRestarts(*clip, synthetic, begin, end, hz, facts.truthLabels);
		for (char t : truth) facts.truthRestarts += t ? 1 : 0;
		FrameDump dump;   // WAIVER-D8-FRAME-DUMP: diagnostic only
		dump.Open(rowName, part, parts);
		std::vector<char> truthCut(truth.size(), 0);   // which truth restarts are manifest cuts (the rest are fade frames)
		if (dump.on && synthetic == Synthetic::None && hz > 0.0)
			for (double c : clip->cuts)
			{
				const int f = static_cast<int>(std::floor(c * hz + 0.5));
				if (f > begin && f < end) truthCut[static_cast<size_t>(f - begin)] = 1;
			}

		// WAIVER-U7ACC-D8-DELIVERY: the U7 v2 sequence law delivers a confirmed cut at seq + 1 (owner ruling Q3, one frame late is
		// allowed), as the skeleton's seal does: frame seq + 1 is not sealed when frame seq is. The scored truth therefore expects a
		// manifest cut at f + 1 (a cut on the part's last frame is delivered after the part and is expected nowhere in it). Fade
		// truth is unchanged: the fade stays the v1 pair law at the frame itself.
		std::vector<char> expected = truth;
		if (synthetic == Synthetic::None && hz > 0.0)
			for (double c : clip->cuts)
			{
				const int f = static_cast<int>(std::floor(c * hz + 0.5));
				if (f <= begin || f >= end) continue;
				expected[static_cast<size_t>(f - begin)] = 0;
				if (f + 1 < end) expected[static_cast<size_t>(f + 1 - begin)] = 1;
			}
		VpUnit::CutFadeJudge::CutTracker tracker;   // the renderer's law, fed every P4 record of the part in order
		VpUnit::CutFadeJudge::FadeRun fadeRun;      // WAIVER-U7-MULTIFRAME-FADE: the part's stream, one run, every pair folded in order (the skeleton's seal)

		ModuleRow dp, tr;
		DetailPulseModel::Model dpOld, dpShared;
		DetailPulseModel::Settings sOld, sShared;
		sOld.sourceHz = sShared.sourceHz = hz;
		sOld.colorDepth = sShared.colorDepth = 10;
		sShared.sceneReader = &VpDetailRestore::ReadSceneFromStore;
		TextureRestoreModel::Model trOld, trShared;
		DetailPulse::Module dpGpu;
		TextureRestore::Module trGpu;
		std::deque<pl_tex> inputs;

		const VpLaneProducts::Dims dims{ static_cast<uint32_t>(W), static_cast<uint32_t>(H) };
		std::vector<uint8_t> luma, half(VpLaneProducts::HalfLumaBytes(dims)), stats(VpLaneProducts::kStatsBytes);
		std::vector<uint16_t> view(static_cast<size_t>(W) * H), plane(static_cast<size_t>(pw) * ph);
		VpLaneProducts::SourceStatsRecord prev, cur;
		bool havePrev = false;
		for (int k = 0; k < end; ++k)
		{
			const auto t0 = std::chrono::steady_clock::now();
			Assert::IsTrue(decoder.Next(luma), Wide("D8: the clip ended early at frame " + std::to_string(k) + " of " + clipId).c_str());
			const bool needed = k + 1 >= begin;   // the frame before the part (its P4 is the first pair's prev) and the part itself
			if (!needed) continue;
			ApplySynthetic(synthetic, k, luma);
			// P1 + P4 at the clip size from the 10-bit view (msbShift 6, as every lane tap view).
			for (size_t i = 0; i < view.size(); ++i) view[i] = static_cast<uint16_t>(luma[i] * 4u);
			VpSkeleton::SourceFrameView v;
			v.seq = static_cast<uint64_t>(k) + 1;
			v.luma = view.data(); v.width = dims.width; v.height = dims.height; v.strideSamples = dims.width; v.msbShift = 6;
			Assert::IsTrue(VpLaneProducts::ComputeHalfLuma(v, half.data(), half.size()), L"D8: P1");
			Assert::IsTrue(VpLaneProducts::ComputeSourceStats(half.data(), dims, stats.data(), stats.size()), L"D8: P4");
			Assert::IsTrue(VpLaneProducts::ReadSourceStats(stats.data(), stats.size(), &cur), L"D8: P4 read back");
			if (k < begin)
			{
				prev = cur;
				havePrev = true;
				continue;
			}
			// The part's own sequence (fresh models and store): seq 1 has no pair, as the skeleton's seal (seq < 2: no scene).
			const uint64_t seq = static_cast<uint64_t>(k - begin) + 1;
			FrameAnalysis::Identity id;
			id.seq = seq; id.width = W; id.height = H;
			Assert::IsTrue(store.Begin(id, FrameAnalysis::kScene) != 0, L"D8: the store is on and begins the record");
			VpLaneProducts::EventSignals ev;   // the frame dump's copy of the U7 verdict (seq 1: none)
			const VpLaneProducts::SourceStatsRecord dumpPrev = prev;
			// WAIVER-U7ACC-D8-DELIVERY: the confirmed verdict of seq - 1 arrives with this frame's record.
			const VpUnit::CutFadeJudge::CutVerdict confirmedPrev = tracker.Push(seq, cur);
			if (seq >= 2)
			{
				Assert::IsTrue(havePrev, L"D8: the pair's previous P4");
				const auto e = VpLaneProducts::ClassifyEvents(prev, cur, false, false);
				ev = e;
				const bool fade = VpUnit::CutFadeJudge::FoldFadeRun(prev, cur, e, fadeRun) || e.fade;   // WAIVER-U7-MULTIFRAME-FADE
				const VpUnit::CutFadeJudge::CutVerdict own = tracker.Provisional();
				const bool delivered = confirmedPrev.known && confirmedPrev.seq + 1u == seq && confirmedPrev.cut;
				FrameAnalysis::Scene scene;
				scene.cut = delivered; scene.cutLate = delivered; scene.fade = fade; scene.origin = FrameAnalysis::SceneOrigin::Vp;
				scene.cutHere = own.cut; scene.confirmed = false; scene.flash = own.flash; scene.cutPossible = own.candidate;
				scene.cutStrength = own.strength;
				Assert::IsTrue(store.PublishScene(seq, scene), L"D8: the scene group is published");
				facts.u7Cuts += delivered ? 1 : 0;
				facts.u7Fades += fade ? 1 : 0;
				facts.u7Flashes += (confirmedPrev.known && confirmedPrev.flash) ? 1 : 0;
			}
			prev = cur;
			havePrev = true;
			// The models' and the GPU's plane: box mean over scale x scale, 10-bit codes.
			for (int y = 0; y < ph; ++y)
				for (int x = 0; x < pw; ++x)
				{
					unsigned sum = 0;
					for (int j = 0; j < scale; ++j)
						for (int i = 0; i < scale; ++i)
							sum += view[static_cast<size_t>(y * scale + j) * W + (x * scale + i)];
					const unsigned n = static_cast<unsigned>(scale * scale);
					plane[static_cast<size_t>(y) * pw + x] = static_cast<uint16_t>((sum + n / 2) / n);
				}
			facts.decodeMs += MsSince(t0);
			GrainModel::Image dpImage(pw, ph);
			TextureRestoreModel::Image trImage(pw, ph);
			for (size_t i = 0; i < plane.size(); ++i) dpImage.v[i] = trImage.v[i] = plane[i];

			bool dumpDpShared = false, dumpDpOld = false, dumpTrShared = false, dumpTrOld = false;
			// DP: retired tile law vs the shared verdict.
			{
				const auto a = std::chrono::steady_clock::now();
				const DetailPulseModel::Frame fo = dpOld.Process(dpImage, seq, sOld);
				dp.msOld += MsSince(a);
				const auto b = std::chrono::steady_clock::now();
				const DetailPulseModel::Frame fs = dpShared.Process(dpImage, seq, sShared);
				dp.msShared += MsSince(b);
				++dp.frames;
				dp.oldRestarts += fo.restart ? 1 : 0;
				dp.sharedRestarts += fs.restart ? 1 : 0;
				dumpDpShared = fs.restart; dumpDpOld = fo.restart;
				if (k > begin) dp.CountTruth(fs.restart, expected[static_cast<size_t>(k - begin)] != 0);   // WAIVER-U7ACC-D8-DELIVERY
				if (fo.restart == fs.restart) ++dp.agree;
				else { ++dp.differ; AddDiff(dp, fo.out, fs.out); }
			}
			// TR: retired tile law vs the shared verdict.
			{
				TextureRestore::Params po;
				po.colorDepth = 10; po.sourceHz = hz; po.bitrateQuality = 1.0; po.sequence = seq;
				TextureRestore::Params ps = po;
				ps.sceneReader = &VpDetailRestore::ReadSceneFromStore;
				const auto a = std::chrono::steady_clock::now();
				const TextureRestoreModel::Image oo = trOld.Frame(trImage, po);
				tr.msOld += MsSince(a);
				const bool ro = trOld.LastWasCut();
				const auto b = std::chrono::steady_clock::now();
				const TextureRestoreModel::Image os = trShared.Frame(trImage, ps);
				tr.msShared += MsSince(b);
				const bool rs = trShared.LastWasCut();
				++tr.frames;
				tr.oldRestarts += ro ? 1 : 0;
				tr.sharedRestarts += rs ? 1 : 0;
				dumpTrShared = rs; dumpTrOld = ro;
				if (k > begin) tr.CountTruth(rs, expected[static_cast<size_t>(k - begin)] != 0);   // WAIVER-U7ACC-D8-DELIVERY
				if (ro == rs) ++tr.agree;
				else { ++tr.differ; AddDiff(tr, oo, os); }
			}
			if (dump.on)   // WAIVER-D8-FRAME-DUMP: one diagnostic line per frame of the part
			{
				const size_t idx = static_cast<size_t>(k - begin);
				const bool truthRestart = truth[idx] != 0;
				const char* label = !truthRestart ? "none" : truthCut[idx] ? "cut" : "fade";
				const uint32_t spreadPrev = dumpPrev.p99 > dumpPrev.p01 ? dumpPrev.p99 - dumpPrev.p01 : 0u;
				const uint32_t spreadCur = cur.p99 > cur.p01 ? cur.p99 - cur.p01 : 0u;
				const bool pair = seq >= 2;
				char line[512];
				snprintf(line, sizeof(line), "%s,%d/%d,%d,%llu,%.4f,%d,%d,%s,%d,%d,%d,%d,%u,%.4f,%lld,%lld,%u,%u,%d,%d,%d,%d",
					rowName, part + 1, parts, k, static_cast<unsigned long long>(seq), hz > 0.0 ? k / hz : 0.0, k > begin ? 1 : 0,
					truthRestart ? 1 : 0, label, ev.cut ? 1 : 0, ev.fade ? 1 : 0, ev.flash ? 1 : 0, ev.cutPossible ? 1 : 0,
					pair ? ev.histDistance : 0u, pair ? ev.histDistance / 65535.0 : 0.0,
					pair ? static_cast<long long>(dumpPrev.mean) : -1ll, static_cast<long long>(cur.mean),
					pair ? spreadPrev : 0u, spreadCur, dumpDpShared ? 1 : 0, dumpTrShared ? 1 : 0, dumpDpOld ? 1 : 0, dumpTrOld ? 1 : 0);
				dump.Add(line);
			}
			// GPU modules: the store's verdict through the default reader (frame_analysis_store on).
			{
				pl_tex tex = g.Upload(plane, pw, ph);
				Assert::IsNotNull(tex, L"D8: input texture");
				inputs.push_back(tex);
				const auto a = std::chrono::steady_clock::now();
				DetailPulse::Params p;
				p.colorDepth = 10; p.sourceHz = hz; p.sequence = seq;
				pl_frame out{};
				Assert::IsTrue(dpGpu.Apply(g.gpu, g.dispatch, Gpu::LumaFrame(tex), p, out), Wide("D8 DP GPU: " + dpGpu.LastError()).c_str());
				pl_gpu_finish(g.gpu);
				dp.msGpu += MsSince(a);
				const auto b = std::chrono::steady_clock::now();
				TextureRestore::Params q;
				q.colorDepth = 10; q.sourceHz = hz; q.bitrateQuality = 1.0; q.sequence = seq;
				pl_frame out2{};
				Assert::IsTrue(trGpu.Apply(g.gpu, g.dispatch, Gpu::LumaFrame(tex), q, out2), Wide("D8 TR GPU: " + trGpu.LastError()).c_str());
				pl_gpu_finish(g.gpu);
				tr.msGpu += MsSince(b);
				while (static_cast<int>(inputs.size()) > kKeepInputTextures)
				{
					pl_tex old = inputs.front();
					inputs.pop_front();
					pl_tex_destroy(g.gpu, &old);
				}
			}
		}
		decoder.Close();
		dp.sceneMissing = dpGpu.SceneMissing();
		tr.sceneMissing = trGpu.SceneMissing();
		dpGpu.Release(g.gpu);
		trGpu.Release(g.gpu);
		for (pl_tex& t : inputs) pl_tex_destroy(g.gpu, &t);
		inputs.clear();

		WriteRow(facts, "DP", dp);
		WriteRow(facts, "TR", tr);
		dump.Write();   // WAIVER-D8-FRAME-DUMP: diagnostic only, before the gate asserts so a red part still dumps
		Assert::AreEqual(end - begin, dp.frames, L"D8: every frame of the part was processed");
		char text[768];
		snprintf(text, sizeof(text), "D8 %s part %s, truth %d restarts (%s): DP truth agreement %.3f %% (miss %d, extra %d), "
			"SceneMissing %llu; TR truth agreement %.3f %% (miss %d, extra %d), SceneMissing %llu (gate: >= %.1f %% vs truth, 0 missing); "
			"retired-law agreement (information) DP %.3f %%, TR %.3f %%", rowName, facts.part.c_str(), facts.truthRestarts,
			facts.truthLabels.c_str(), dp.TruthAgreementPct(), dp.truthMiss, dp.truthExtra, dp.sceneMissing, tr.TruthAgreementPct(),
			tr.truthMiss, tr.truthExtra, tr.sceneMissing, kAgreementGatePct, dp.AgreementPct(), tr.AgreementPct());
		Logger::WriteMessage((std::string(text) + "\n").c_str());
		Assert::AreEqual(0ull, dp.sceneMissing, Wide(text).c_str());
		Assert::AreEqual(0ull, tr.sceneMissing, Wide(text).c_str());
		Assert::IsTrue(dp.TruthAgreementPct() >= kAgreementGatePct, Wide(text).c_str());
		Assert::IsTrue(tr.TruthAgreementPct() >= kAgreementGatePct, Wide(text).c_str());
	}
}

#define D8_ROW_METHOD(NAME, CLIP, SYNTH, ROW, PART, PARTS) \
	BEGIN_TEST_METHOD_ATTRIBUTE(NAME) \
		TEST_METHOD_ATTRIBUTE(L"Category", L"GpuHardware") \
	END_TEST_METHOD_ATTRIBUTE() \
	TEST_METHOD(NAME) \
	{ \
		D8SceneJudgeAgreementNs::RunRow(#NAME, CLIP, SYNTH, ROW, PART, PARTS); \
	}

namespace D8SceneJudgeAgreementNs
{
	TEST_CLASS(D8SceneJudgeAgreementTests)
	{
	public:
		// tos_24p_3min (4320 frames, 17 truth cuts): six parts.
		D8_ROW_METHOD(D8_Tos24p3min_Part1, "tos_24p_3min", Synthetic::None, "tos_24p_3min", 0, 6)
		D8_ROW_METHOD(D8_Tos24p3min_Part2, "tos_24p_3min", Synthetic::None, "tos_24p_3min", 1, 6)
		D8_ROW_METHOD(D8_Tos24p3min_Part3, "tos_24p_3min", Synthetic::None, "tos_24p_3min", 2, 6)
		D8_ROW_METHOD(D8_Tos24p3min_Part4, "tos_24p_3min", Synthetic::None, "tos_24p_3min", 3, 6)
		D8_ROW_METHOD(D8_Tos24p3min_Part5, "tos_24p_3min", Synthetic::None, "tos_24p_3min", 4, 6)
		D8_ROW_METHOD(D8_Tos24p3min_Part6, "tos_24p_3min", Synthetic::None, "tos_24p_3min", 5, 6)
		// Synthetic fade (rate_24 picture, fade to black and back; 240 frames).
		D8_ROW_METHOD(D8_SyntheticFade, "rate_24", Synthetic::Fade, "fade_synthetic", 0, 1)
		// pan_480pxs_23976p (719 frames).
		D8_ROW_METHOD(D8_Pan480pxs23976p, "pan_480pxs_23976p", Synthetic::None, "pan_480pxs_23976p", 0, 1)
		// Synthetic flash (rate_24 picture, one bright frame every 24; 240 frames).
		D8_ROW_METHOD(D8_SyntheticFlash, "rate_24", Synthetic::Flash, "flash_synthetic", 0, 1)
		// rate_24 (static, cut free; 960 frames): two parts.
		D8_ROW_METHOD(D8_Rate24_Part1, "rate_24", Synthetic::None, "rate_24", 0, 2)
		D8_ROW_METHOD(D8_Rate24_Part2, "rate_24", Synthetic::None, "rate_24", 1, 2)
		// pan_1440pxs_5994p (1798 frames): three parts.
		D8_ROW_METHOD(D8_Pan1440pxs5994p_Part1, "pan_1440pxs_5994p", Synthetic::None, "pan_1440pxs_5994p", 0, 3)
		D8_ROW_METHOD(D8_Pan1440pxs5994p_Part2, "pan_1440pxs_5994p", Synthetic::None, "pan_1440pxs_5994p", 1, 3)
		D8_ROW_METHOD(D8_Pan1440pxs5994p_Part3, "pan_1440pxs_5994p", Synthetic::None, "pan_1440pxs_5994p", 2, 3)
		// pan_480pxs_fade_23976p (719 frames, fade during motion).
		D8_ROW_METHOD(D8_Pan480pxsFade23976p, "pan_480pxs_fade_23976p", Synthetic::None, "pan_480pxs_fade_23976p", 0, 1)
	};
}
