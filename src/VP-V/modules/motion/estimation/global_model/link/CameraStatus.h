// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Camera K3: the visible state of the camera model, as text. Pure and deterministic (no clock, no thread, no GPU), so one
// builder serves the Ctrl+I line, the Config status, the Fallbacks list and the Ctrl+Alt+D section (CameraExtension feeds it
// from the settings, the link core and the dump writer). The rules:
//   * everything off (or the Original pathway): no line, no fallback;
//   * a switch that is set but cannot act is SHOWN (camera_model off, camera_link off, no vector engine yet, an overlay that
//     cannot draw): never silent, never log-only;
//   * a fit that exists: "camera: <model> <confidence>, fit <ms> ms; users <list>; reason <text>".
// Header-only, C++14.
#pragma once

#include "CameraDump.h"
#include "CameraFitStatus.h"
#include "CameraSettings.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Camera
	{
		inline const char* ModelTypeText(CameraModel::ModelType t)
		{
			switch (t)
			{
			case CameraModel::ModelType::Translation: return "translation";
			case CameraModel::ModelType::Similarity: return "similarity";
			case CameraModel::ModelType::Affine: return "affine";
			case CameraModel::ModelType::Homography: return "homography";
			case CameraModel::ModelType::None: break;
			}
			return "none";
		}

		enum class CameraState
		{
			Off,                // nothing set, or the Original pathway
			InertSwitches,      // switches set while camera_model is off
			LinkOff,            // camera_model on, camera_link off: the link is not attached
			LinkUnavailable,    // camera_model on, camera_link = report, but the link could not be built
			WaitingForEngine,   // the link is attached, no vector engine feeds it (P5a pending): no fit
			NoFitYet,           // the fit runs, no record yet
			Fitting             // a record exists
		};

		// One consumer row for the Ctrl+Alt+D section: weight < 0 = not available (no fit).
		struct ConsumerFact
		{
			std::string name;
			bool on = false;
			double weight = -1;
		};

		struct StatusInput
		{
			CameraSettings cam;                 // originalPathway already set by the caller
			bool linkReport = false;            // camera_link = report
			bool coreAlive = false;             // a CameraLinkCore exists for the renderer
			bool engineAvailable = false;       // the core allocated its fit (a vector engine feeds it)
			std::string linkProblem;            // why there is no core (store / seal)
			CameraLink::LastFitSummary fit;
			uint64_t fitted = 0, published = 0, late = 0;
			std::string measurementLine;        // CameraModel::MeasurementDiagnostics::StatusLine(); "" = none
			std::vector<ConsumerFact> consumers;   // optional per-consumer weights (a consumer without a fact shows n/a)
			bool dumpOn = false;                // VP_CAMERA_DUMP names a directory
			std::string dumpDirectory;
			CameraLink::DumpStats dump;
		};

		struct StatusText
		{
			CameraState state = CameraState::Off;
			std::vector<std::string> lines;       // "camera: ..." Ctrl+I and Config status (K3's lines)
			std::vector<std::string> fallbacks;   // "id: reason", for the Fallbacks list
			std::vector<std::string> detail;      // Ctrl+Alt+D: per-consumer weight, deadline misses, measurement diagnostics
		};

		namespace Detail
		{
			inline std::string Join(const std::vector<std::string>& parts, const char* sep)
			{
				std::string out;
				for (const std::string& p : parts)
					out += (out.empty() ? "" : sep) + p;
				return out;
			}

			// Names of the keys set on (camera_model itself excluded), the overlay by its value.
			inline std::vector<std::string> SetKeys(const CameraSettings& c)
			{
				std::vector<std::string> out;
				for (const std::pair<std::string, std::string>& kv : KeyValues(c))
					if (kv.second != "off" && kv.first != "camera_model")
						out.push_back(kv.first == "camera_model_overlay" ? kv.first + "=" + kv.second : kv.first);
				return out;
			}

			inline std::vector<ConsumerFact> UseRows(const StatusInput& in)
			{
				static const char* const names[] = { "grain", "detail", "dither", "deband", "filler", "dejudder" };
				const bool flags[] = { in.cam.use.grain, in.cam.use.detail, in.cam.use.dither, in.cam.use.deband, in.cam.use.filler,
					in.cam.use.dejudder };
				std::vector<ConsumerFact> rows;
				for (size_t i = 0; i < 6; ++i)
				{
					ConsumerFact f;
					f.name = names[i];
					f.on = flags[i];
					for (const ConsumerFact& given : in.consumers)
						if (given.name == names[i])
							f.weight = given.weight;
					rows.push_back(f);
				}
				return rows;
			}
		}

		// The one builder. The caller sets `in.cam.originalPathway`; the Original pathway gives an empty result.
		inline StatusText BuildStatus(const StatusInput& in)
		{
			StatusText t;
			const CameraSettings& c = in.cam;
			if (c.originalPathway)
				return t;

			// The dump's progress and drops are shown whatever else is set (a dump with no [vpv.camera] key must not hide its drops).
			auto addDump = [&]() {
				if (in.dumpOn)
				{
					char b[192];
					std::snprintf(b, sizeof(b), "camera dump: written %llu, dropped %llu, failed %llu, queued %llu",
						static_cast<unsigned long long>(in.dump.written), static_cast<unsigned long long>(in.dump.dropped),
						static_cast<unsigned long long>(in.dump.failed), static_cast<unsigned long long>(in.dump.queued));
					t.detail.push_back(std::string(b) + " (" + in.dumpDirectory + ")");
					if (in.dump.dropped > 0 || in.dump.failed > 0)
					{
						std::snprintf(b, sizeof(b), "camera_dump: %llu item(s) dropped (the writer is behind), %llu failed to write",
							static_cast<unsigned long long>(in.dump.dropped), static_cast<unsigned long long>(in.dump.failed));
						t.fallbacks.push_back(b);
					}
				}
			};

			// Invalid values and unknown keys: shown even when nothing else is set.
			std::vector<std::string> keyProblems = c.problems;
			for (const std::string& k : c.unknownKeys)
				keyProblems.push_back("unknown key '" + k + "' in [" + kSection + "] ignored");
			if (!keyProblems.empty())
				t.fallbacks.push_back("camera_settings: " + Detail::Join(keyProblems, "; "));

			if (!c.AnySet())
			{
				addDump();
				return t;
			}

			const std::vector<std::string> setKeys = Detail::SetKeys(c);
			std::string users;
			{
				std::vector<std::string> names;
				for (const ConsumerFact& f : Detail::UseRows(in))
					if (f.on)
						names.push_back(f.name);
				users = names.empty() ? std::string("none") : Detail::Join(names, "+");
			}

			if (!c.model)
			{
				t.state = CameraState::InertSwitches;
				const std::string why = "camera_model is off, so " + Detail::Join(setKeys, ", ") + " have no effect";
				t.lines.push_back("camera: model off; " + why);
				t.fallbacks.push_back("camera_model: " + why);
				addDump();
				return t;
			}

			// camera_model on.
			std::string fit;
			if (!in.linkReport)
			{
				t.state = CameraState::LinkOff;
				fit = "camera: model on; the camera link is not attached (camera_link is off): nothing is fitted, no consumer uses the camera";
				t.fallbacks.push_back("camera_model: camera_link is off: the camera link is not attached, so no fit runs and no consumer uses the camera");
			}
			else if (!in.coreAlive)
			{
				t.state = CameraState::LinkUnavailable;
				fit = "camera: model on; the camera link is unavailable (" + (in.linkProblem.empty() ? std::string("starting") : in.linkProblem) + ")";
			}
			else if (!in.engineAvailable)
			{
				t.state = CameraState::WaitingForEngine;
				fit = "camera: model on; waiting for the vector engine (P5a pending): no fit runs, no consumer uses the camera";
				t.fallbacks.push_back("camera_model: waiting for the vector engine (P5a pending): no fit runs, no consumer uses the camera");
			}
			else if (!in.fit.have)
			{
				t.state = CameraState::NoFitYet;
				fit = "camera: model on; no fit published yet; users " + users + "; reason none yet";
			}
			else
			{
				t.state = CameraState::Fitting;
				char b[96];
				std::snprintf(b, sizeof(b), "camera: %s %.2f, fit %.1f ms; users ", ModelTypeText(in.fit.type), in.fit.confidence, in.fit.fitMs);
				fit = std::string(b) + users + "; reason " + CameraModel::ReasonTextOf(in.fit.reason, in.fit.verdictFlags);
				if (!in.fit.published)
					fit += " (late: not published before the frame)";
			}
			t.lines.push_back(fit);

			if (c.overlay != OverlayMode::Off)
			{
				const std::string why = std::string(OverlayModeText(c.overlay)) +
					" requested, but it cannot draw yet (the overlay stage is not wired to the fitted labels): nothing is drawn";
				t.lines.push_back("camera overlay: " + why);
				t.fallbacks.push_back("camera_model_overlay: " + why);
			}

			// Ctrl+Alt+D.
			t.detail.push_back(std::string("camera labels: ") + (c.labelsGpu ? "computed on the GPU (camera_labels_gpu on)" : "uploaded from the fit (camera_labels_gpu off)"));
			if (in.engineAvailable)
			{
				char b[128];
				std::snprintf(b, sizeof(b), "camera deadline misses: %llu late (fitted %llu, published %llu)",
					static_cast<unsigned long long>(in.late), static_cast<unsigned long long>(in.fitted), static_cast<unsigned long long>(in.published));
				t.detail.push_back(b);
			}
			t.detail.push_back(in.measurementLine.empty()
				? std::string(c.meas.Any() ? "camera guidance: no pair guided yet" : "camera guidance: off (no measurement switch is on)")
				: in.measurementLine);
			for (const ConsumerFact& f : Detail::UseRows(in))
			{
				char b[96];
				if (f.weight < 0)
					std::snprintf(b, sizeof(b), "camera use %s: %s, weight n/a (no fit)", f.name.c_str(), f.on ? "on" : "off");
				else
					std::snprintf(b, sizeof(b), "camera use %s: %s, weight %.2f", f.name.c_str(), f.on ? "on" : "off", f.weight);
				t.detail.push_back(b);
			}

			addDump();
			return t;
		}
	}
}
