#pragma once
/*
 * Copyright(C) 2026 Victor McPherson
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// DV comparison capture (feat/dv-l8, 2026-10-24; diagnostic, OFF by default).
//
// The owner plays the same scene twice from the Apple TV - once as LLDV (the certified Dolby
// mapping applied by the player) and once as the DV tunnel (VP maps it) - and VP records a
// short labelled clip of each. tools/dv_compare/dv_pair_compare.py pairs the two clips by a
// picture fingerprint (tolerant of timing offsets, pauses and the different tone curves) and
// reports PSNR, Delta E ITP and the mean luminance difference in stops per pair and scene.
//
// Opt-in per process, like VP_RAW_CAPTURE_PATH / VP_SUBTITLE_CAPTURE_PATH (never active in
// an ordinary launch; clear the variables afterwards):
//   VP_DV_COMPARE_PATH      root folder; enables the capture (a large drive, NEVER a source
//                           checkout: captured film frames are never committed)
//   VP_DV_COMPARE_SECONDS   clip length (default 4 s of capture time, at the measured rate)
//   VP_DV_COMPARE_NOTE      free text stored with every clip (e.g. "LLDV 100 nits EDID")
//   VP_DV_COMPARE_MIN_FREE_GB  no clip below this much free space (default 10)
// Start a clip: create the file RECORD in the root folder (delete and create it again for
// the next clip); a STOP file ends one early. Each clip is one RawCaptureRecorder session
// (<root>\<yyyymmdd-hhmmss>-rec\segNN\: capture.txt, frames.csv, frame_NNNNNN.bin - the raw
// capture bytes, so the compare tool reads them as it reads raw captures) plus, beside the
// segments, dv-frames.jsonl: a clip header line, then one line per rendered picture (source
// counter, the decoded DM: L1/L2/L8/L9/L10, matrices, the DM hold phase, and the trim VP
// chose: level, reason, target nits, the parameters).
//
// This object lives in the renderer (it sees the decoded DM and the trim choice). The frame
// bytes are the raw capture frame the render already holds (capture_path gpu); without one
// (CPU ingest) only the metadata lines are written and the header says so. The copy runs on
// the render thread while a clip records (a few ms per 4K frame) - a diagnostic cost.
#include <VP-V/modules/control/diagnostics/RawCaptureRecorder.h>
#include <VideoFrame.h>
#include <VideoState.h>
#include <EOTF.h>
#include <VP-V/modules/colour/sdr_tone_shape/DvTrimSelection.h>
#include <VP-V/modules/input/signal/DvMetadataHold.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace DvCompareCapture
{
	inline std::string Env(const char* name)
	{
		char* value = nullptr;
		size_t size = 0;
		std::string out;
		if (_dupenv_s(&value, &size, name) == 0 && value)
			out = value;
		std::free(value);
		return out;
	}

	inline double EnvNumber(const char* name, double fallback)
	{
		const std::string text = Env(name);
		if (text.empty())
			return fallback;
		char* end = nullptr;
		const double v = std::strtod(text.c_str(), &end);
		return end != text.c_str() && v > 0.0 ? v : fallback;
	}

	struct Options
	{
		std::string root;
		double seconds = 4.0;
		double minFreeGb = 10.0;
		std::string note;

		static Options FromEnvironment()
		{
			Options o;
			o.root = Env("VP_DV_COMPARE_PATH");
			o.seconds = EnvNumber("VP_DV_COMPARE_SECONDS", 4.0);
			o.minFreeGb = EnvNumber("VP_DV_COMPARE_MIN_FREE_GB", 10.0);
			o.note = Env("VP_DV_COMPARE_NOTE");
			return o;
		}
	};

	// What the render knows about one picture.
	struct FrameInfo
	{
		uint64_t counter = 0;
		const char* mode = "";                               // "tunnel", "pq" (LLDV / HDR10), "sdr"
		const DolbyVisionTunnelFrameMetadata* dm = nullptr;  // tunnel only
		bool trimsEnabled = false;
		const DvTrimSelection::Selection* trims = nullptr;   // null: not evaluated this picture
		double targetNits = 0.0;                             // the tone-mapping target (display peak)
		bool frameBytes = false;                             // the raw frame was handed to the recorder
	};

	inline const char* ModeOf(const VideoState& state, const DolbyVisionTunnelFrameMetadata* dm)
	{
		if (state.dolbyVisionTunnel && dm && dm->valid)
			return "tunnel";
		if (state.dolbyVisionTunnel)
			return "tunnel-no-dm";
		return state.eotf == EOTF::PQ ? "pq" : "sdr";
	}

	inline void JsonEscape(std::ostringstream& s, const std::string& text)
	{
		s << '"';
		for (const char c : text)
		{
			if (c == '"' || c == '\\') s << '\\' << c;
			else if (static_cast<unsigned char>(c) < 0x20) s << ' ';
			else s << c;
		}
		s << '"';
	}

	inline void JsonParams(std::ostringstream& s, const DvTrimSelection::ExtParams& p)
	{
		char text[640];
		std::snprintf(text, sizeof(text),
			"{\"slope\":%.6g,\"offset\":%.6g,\"power\":%.6g,\"chroma_weight\":%.6g,\"saturation_gain\":%.6g,"
			"\"mid_contrast\":%.6g,\"clip_trim\":%.6g,\"sat_vector\":[%.6g,%.6g,%.6g,%.6g,%.6g,%.6g],"
			"\"hue_vector_deg\":[%.6g,%.6g,%.6g,%.6g,%.6g,%.6g]}",
			p.base.slope, p.base.offset, p.base.power, p.base.chromaWeight, p.base.saturationGain,
			p.midContrast, p.clipTrim, p.saturation[0], p.saturation[1], p.saturation[2], p.saturation[3],
			p.saturation[4], p.saturation[5], p.hueDegrees[0], p.hueDegrees[1], p.hueDegrees[2],
			p.hueDegrees[3], p.hueDegrees[4], p.hueDegrees[5]);
		s << text;
	}

	// The decoded DM as JSON (the fields the compare tool and a reader need).
	inline std::string DmJson(const DolbyVisionTunnelFrameMetadata& md)
	{
		std::ostringstream s;
		s.precision(9);
		s << "{\"valid\":" << (md.valid ? 1 : 0) << ",\"full_range\":" << (md.fullRange ? 1 : 0)
			<< ",\"source_pq\":[" << md.sourceMinPq << ',' << md.sourceMaxPq << ']';
		if (md.hasSceneMetadata)
			s << ",\"l1\":[" << md.sceneMinPq << ',' << md.sceneMaxPq << ',' << md.sceneAvgPq << ']';
		else
			s << ",\"l1\":null";
		s << ",\"nonlinear\":[";
		for (int k = 0; k < 9; ++k) s << (k ? "," : "") << md.nonlinear[k / 3][k % 3];
		s << "],\"nonlinear_offset\":[";
		for (int k = 0; k < 3; ++k) s << (k ? "," : "") << md.nonlinearOffset[k];
		s << "],\"linear\":[";
		for (int k = 0; k < 9; ++k) s << (k ? "," : "") << md.linear[k / 3][k % 3];
		s << "],\"trims\":[";
		for (int i = 0; i < md.trimCount && i < DolbyVisionTunnelFrameMetadata::kMaxTrims; ++i)
		{
			const auto& t = md.trims[i];
			s << (i ? "," : "") << "{\"level\":" << static_cast<int>(t.level);
			if (t.level == 2) s << ",\"target_max_pq\":" << t.targetMaxPq;
			else s << ",\"target_index\":" << static_cast<int>(t.targetDisplayIndex);
			s << ",\"codes\":[" << t.slope << ',' << t.offset << ',' << t.power << ',' << t.chromaWeight << ','
				<< t.saturationGain << ',' << t.msWeight << ']';
			if (t.hasMidContrastAndClip) s << ",\"mid_contrast\":" << t.midContrast << ",\"clip_trim\":" << t.clipTrim;
			if (t.hasSaturationVector)
			{
				s << ",\"sat_vector\":[";
				for (int k = 0; k < 6; ++k) s << (k ? "," : "") << static_cast<int>(t.saturationVector[k]);
				s << ']';
			}
			if (t.hasHueVector)
			{
				s << ",\"hue_vector\":[";
				for (int k = 0; k < 6; ++k) s << (k ? "," : "") << static_cast<int>(t.hueVector[k]);
				s << ']';
			}
			s << '}';
		}
		s << "],\"l10\":[";
		for (int i = 0; i < md.targetDisplayCount && i < DolbyVisionTunnelFrameMetadata::kMaxTargetDisplays; ++i)
		{
			const auto& t = md.targetDisplays[i];
			s << (i ? "," : "") << "{\"index\":" << static_cast<int>(t.index) << ",\"max_pq\":" << t.maxPq
				<< ",\"min_pq\":" << t.minPq << ",\"primary_index\":" << static_cast<int>(t.primaryIndex) << '}';
		}
		s << "],\"l9\":";
		if (md.hasL9) s << static_cast<int>(md.l9SourcePrimaryIndex); else s << "null";
		s << ",\"reused_previous_dm\":" << (md.reusedPreviousDm ? 1 : 0)
			<< ",\"hold_phase\":\"" << DvMetadataHold::PhaseName(static_cast<DvMetadataHold::Phase>(md.dmHoldPhase))
			<< "\",\"rejected_pictures\":" << md.dmRejectedPictures << '}';
		return s.str();
	}

	// One dv-frames.jsonl line for a picture.
	inline std::string FrameJson(const FrameInfo& f)
	{
		std::ostringstream s;
		s.precision(9);
		s << "{\"type\":\"frame\",\"counter\":" << f.counter << ",\"mode\":";
		JsonEscape(s, f.mode);
		s << ",\"frame_bytes\":" << (f.frameBytes ? 1 : 0) << ",\"target_nits\":" << f.targetNits;
		s << ",\"dm\":" << (f.dm ? DmJson(*f.dm) : std::string("null"));
		s << ",\"trims\":";
		if (!f.trimsEnabled)
			s << "{\"enabled\":0}";
		else if (!f.trims)
			s << "{\"enabled\":1,\"level\":\"none\",\"apply\":0,\"reason\":\"not evaluated\"}";
		else
		{
			s << "{\"enabled\":1,\"level\":\"" << DvTrimSelection::LevelName(f.trims->level) << "\",\"apply\":"
				<< (f.trims->apply ? 1 : 0) << ",\"reason\":";
			JsonEscape(s, f.trims->reason);
			s << ",\"weight\":" << f.trims->weight << ",\"display_pq\":" << f.trims->displayPq
				<< ",\"l8_unresolved\":" << f.trims->l8Unresolved << ",\"params\":";
			JsonParams(s, f.trims->params);
			s << '}';
		}
		s << '}';
		return s.str();
	}

	inline std::string ClipJson(const Options& o, const VideoState& state, const char* mode, double targetNits, bool frameBytes)
	{
		std::ostringstream s;
		s.precision(9);
		std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
		s << "{\"type\":\"clip\",\"format\":\"vp-dv-compare-1\",\"started\":\"" << stamp << "\",\"mode\":";
		JsonEscape(s, mode);
		if (state.displayMode)
			s << ",\"width\":" << state.displayMode->FrameWidth() << ",\"height\":" << state.displayMode->FrameHeight()
				<< ",\"rate_hz\":" << state.displayMode->RefreshRateHz();
		s << ",\"target_nits\":" << targetNits << ",\"frames_recorded\":" << (frameBytes ? 1 : 0) << ",\"note\":";
		JsonEscape(s, o.note);
		if (!frameBytes)
			s << ",\"warning\":\"no raw frame in this render path (capture_path cpu): metadata only\"";
		s << ",\"never_commit\":\"captured film frames\"}";
		return s.str();
	}

	class Recorder
	{
	public:
		explicit Recorder(const Options& options) : m_options(options), m_environmentRoot(options.root)
		{
			Rebuild();
		}

		bool Enabled() const { std::lock_guard<std::mutex> guard(m_mutex); return m_raw != nullptr; }
		bool Recording() const { std::lock_guard<std::mutex> guard(m_mutex); return m_raw && m_raw->Recording(); }

		// WIP (feat/dv-l8 follow-up, unverified): the Config Diagnostics folder
		// (dv_compare_capture_folder; empty = VP_DV_COMPARE_PATH, or off). Cheap when unchanged.
		void Configure(const std::string& folder)
		{
			const std::string root = folder.empty() ? m_environmentRoot : folder;
			std::lock_guard<std::mutex> guard(m_mutex);
			if (root == m_options.root)
				return;
			m_options.root = root;
			m_raw.reset();
			m_session.clear();
			m_stateSignature.clear();
			RebuildLocked();
		}

		// WIP: the dv_compare_capture shortcut - start a clip (it stops by itself after the clip
		// length), or stop one early. Returns the line to show on screen.
		std::string Toggle()
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			if (!m_raw)
				return "DV comparison capture: off - set Config > Diagnostics > DV comparison capture folder "
					"(dv_compare_capture_folder) or VP_DV_COMPARE_PATH";
			const std::string status = m_raw->Recording() ? m_raw->Stop("dv_compare_capture key") :
				m_raw->Start("dv_compare_capture key");
			return "DV comparison capture: " + status;
		}

		// Once per rendered picture (render thread). raw = the capture frame (may be null).
		void OnFrame(const VideoState& state, const VideoFrame* raw, FrameInfo info)
		{
			std::lock_guard<std::mutex> guard(m_mutex);
			if (!m_raw)
				return;
			const std::string signature = StateSignature(state);
			if (signature != m_stateSignature)
			{
				m_stateSignature = signature;
				m_raw->OnVideoState(state);
			}
			m_raw->SetProcessingNote(std::string("dv-compare mode=") + info.mode);
			// The recorder reads its RECORD / STOP files and its time limit from frames; a
			// render path without a raw frame still drives it with a header-only frame.
			info.frameBytes = raw && raw->GetData();
			if (info.frameBytes)
				m_raw->OnFrame(*raw);
			else
			{
				VideoFrame probe(nullptr, info.counter, 0, nullptr);
				m_raw->OnFrame(probe);
			}
			if (!m_raw->Recording())
			{
				m_session.clear();
				return;
			}
			const std::string session = m_raw->SessionFolder();
			if (session.empty())
				return;
			if (session != m_session)
			{
				m_session = session;
				Append(ClipJson(m_options, state, info.mode, info.targetNits, info.frameBytes));
			}
			Append(FrameJson(info));
		}

		// The renderer's one hook line: everything it knows about the picture just rendered.
		void OnRender(const VideoState& state, const VideoFrame* raw, uint64_t counter,
			const DolbyVisionTunnelFrameMetadata* dm, bool trimsEnabled, const DvTrimSelection::Selection* trims,
			double targetNits)
		{
			FrameInfo info;
			info.counter = counter;
			info.mode = ModeOf(state, dm);
			info.dm = dm && dm->valid ? dm : nullptr;
			info.trimsEnabled = trimsEnabled;
			info.trims = trims;
			info.targetNits = targetNits;
			OnFrame(state, raw, info);
		}

	private:
		void Rebuild() { std::lock_guard<std::mutex> guard(m_mutex); RebuildLocked(); }
		void RebuildLocked()
		{
			if (m_options.root.empty())
				return;
			RawCaptureOptions raw;
			raw.root = m_options.root;
			raw.maxSeconds = m_options.seconds;
			raw.minFreeGb = m_options.minFreeGb;
			raw.note = "dv-compare" + (m_options.note.empty() ? std::string() : ": " + m_options.note);
			raw.watchFiles = true;
			m_raw.reset(new RawCaptureRecorder(raw));
		}

		static std::string StateSignature(const VideoState& s)
		{
			std::ostringstream o;
			o << s.valid << '|' << static_cast<int>(s.videoFrameEncoding) << '|' << static_cast<int>(s.eotf) << '|'
				<< static_cast<int>(s.colorspace) << '|' << s.dolbyVisionTunnel;
			if (s.displayMode)
				o << '|' << s.displayMode->FrameWidth() << 'x' << s.displayMode->FrameHeight() << '@'
				<< s.displayMode->RefreshRateHz();
			return o.str();
		}

		void Append(const std::string& line)
		{
			std::ofstream out(m_session + "\\dv-frames.jsonl", std::ios::app | std::ios::binary);
			out << line << '\n';
		}

		Options m_options;
		std::string m_environmentRoot;
		mutable std::mutex m_mutex;
		std::unique_ptr<RawCaptureRecorder> m_raw;
		std::string m_stateSignature;
		std::string m_session;
	};
}
