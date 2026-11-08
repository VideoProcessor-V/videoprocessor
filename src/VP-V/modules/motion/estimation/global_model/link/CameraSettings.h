// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Camera K3: the camera model's switches, section [vpv.camera] of VideoProcessor.cfg. Every key defaults to OFF and the
// Original pathway forces every key off (Effective(), the accessors below). Pure, header-only, C++14: parser, validators and
// a serialiser; no state, no threads. The H1 adapter (CameraExtension) reads them at GpuCreated and every SettingsApplied.
//
//   camera_model                 on | off                              the camera model (P7 fit) and its diagnostics
//   camera_model_overlay         off | labels | residual | vectors     diagnostic overlay (never default)
//   camera_labels_gpu            on | off                              P8 labels computed on the GPU (compute mode)
//   camera_meas_<name>           on | off   (10 keys)                  the measurement switches, one per
//                                                                      CameraModel::MeasurementSwitches member
//   camera_use_<consumer>        on | off   (grain, detail, dither, deband, filler, dejudder)
//   camera_ahead_fit             on | off                              the fit reads the AHEAD NVOF pair (IVectorEngine::FlowOf) and
//                                                                      publishes P7 on time when it can (link/README.md "Ahead fit");
//                                                                      a LINK key: not on the Camera model card yet (kCameraLinkKeys)
//
// The switches and the consumers need camera_model = on: with it off they are inert, and the status says so. An invalid value
// keeps the default and is listed in `problems` (shown under Fallbacks, never log-only). Keys of the section this parser does
// not know are listed in `unknownKeys` (kept in the file, never rewritten, shown as a note: later steps add keys).
//
// The validators (Validate) mirror the profile-key validators of control/config/VpvSchemaKeys.h (Verdict, IsChoice) so they can
// be registered there unchanged when these keys become per-profile; see link/README.md "K3".

#include <ConfigFile.h>
#include <VP-V/modules/motion/estimation/global_model/GlobalModelMeasurement.h>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Vpv
{
	namespace Camera
	{
		constexpr const char* kSection = "vpv.camera";

		enum class OverlayMode { Off, Labels, Residual, Vectors };

		inline const char* OverlayModeText(OverlayMode m)
		{
			switch (m)
			{
			case OverlayMode::Labels: return "labels";
			case OverlayMode::Residual: return "residual";
			case OverlayMode::Vectors: return "vectors";
			case OverlayMode::Off: break;
			}
			return "off";
		}

		// One flag per consumer of the camera (camera_use_<name>).
		struct UseSwitches
		{
			bool grain = false, detail = false, dither = false, deband = false, filler = false, dejudder = false;
			bool Any() const { return grain || detail || dither || deband || filler || dejudder; }
			bool operator==(const UseSwitches& o) const
			{
				return grain == o.grain && detail == o.detail && dither == o.dither && deband == o.deband &&
					filler == o.filler && dejudder == o.dejudder;
			}
		};

		// Every key of the section, in file order. A key is documented in the module README (estimation/README.md, Settings).
		static const char* const kCameraKeys[] = {
			"camera_model", "camera_model_overlay", "camera_labels_gpu",
			"camera_meas_model_global", "camera_meas_guided_candidate", "camera_meas_narrow_window", "camera_meas_e1_skip",
			"camera_meas_static_gate", "camera_meas_cut_skip", "camera_meas_fade_normalised", "camera_meas_flash_bridge",
			"camera_meas_temporal_seed", "camera_meas_tiny_mover_roi",
			"camera_use_grain", "camera_use_detail", "camera_use_dither", "camera_use_deband", "camera_use_filler",
			"camera_use_dejudder" };
		constexpr size_t kCameraKeyCount = sizeof(kCameraKeys) / sizeof(kCameraKeys[0]);
		// Keys of the section that steer the camera LINK (how the fit is fed), not the model: parsed and validated here like the others,
		// but not on the Config card (Config placement: none yet; see the module README). Not in kCameraKeys / KeyValues on purpose
		// (that list is the card's).
		static const char* const kCameraLinkKeys[] = { "camera_ahead_fit" };
		constexpr size_t kCameraLinkKeyCount = sizeof(kCameraLinkKeys) / sizeof(kCameraLinkKeys[0]);

		enum class Verdict { NotMine, Valid, Invalid };

		struct CameraSettings
		{
			bool model = false;                              // camera_model
			OverlayMode overlay = OverlayMode::Off;          // camera_model_overlay
			bool labelsGpu = false;                          // camera_labels_gpu
			CameraModel::MeasurementSwitches meas;           // camera_meas_*  (`objects` has no key: it stays off)
			UseSwitches use;                                 // camera_use_*
			bool aheadFit = false;                           // camera_ahead_fit (link key)
			bool originalPathway = false;                    // [vpv] pipeline_original_pathway = on (set by the caller)
			std::vector<std::string> problems;               // invalid values (kept at the default)
			std::vector<std::string> unknownKeys;            // keys of [vpv.camera] this version does not know (kept in the file)

			// True when any key is set on (before the Original pathway and the model switch are applied).
			bool AnySet() const { return model || overlay != OverlayMode::Off || labelsGpu || meas.Any() || use.Any() || aheadFit; }
			// The camera model runs (or would, once a vector engine exists).
			bool ModelWanted() const { return !originalPathway && model; }
			// Something is set that the status must show (a set switch under a model that is off is shown as inert).
			bool Wanted() const { return !originalPathway && AnySet(); }

			// What the consumers are allowed to read: everything off unless the model is wanted.
			CameraModel::MeasurementSwitches Measurement() const
			{
				return ModelWanted() ? meas : CameraModel::MeasurementSwitches();
			}
			UseSwitches Use() const { return ModelWanted() ? use : UseSwitches(); }
			OverlayMode Overlay() const { return ModelWanted() ? overlay : OverlayMode::Off; }
			bool LabelsGpu() const { return ModelWanted() && labelsGpu; }
			// The ahead fit runs only with the model (it changes how the fit is fed, nothing else).
			bool AheadFit() const { return ModelWanted() && aheadFit; }
			// The same settings with every key forced off when the Original pathway is on (problems are dropped: nothing is shown).
			CameraSettings Effective() const
			{
				if (!originalPathway)
					return *this;
				CameraSettings off;
				off.originalPathway = true;
				return off;
			}
		};

		namespace Detail
		{
			// The boolean slot of a key, or nullptr when the key is not a boolean key of this section.
			inline bool* BoolSlot(CameraSettings& s, const std::string& key)
			{
				if (key == "camera_model") return &s.model;
				if (key == "camera_labels_gpu") return &s.labelsGpu;
				if (key == "camera_meas_model_global") return &s.meas.modelGlobal;
				if (key == "camera_meas_guided_candidate") return &s.meas.guidedCandidate;
				if (key == "camera_meas_narrow_window") return &s.meas.narrowWindow;
				if (key == "camera_meas_e1_skip") return &s.meas.e1Skip;
				if (key == "camera_meas_static_gate") return &s.meas.staticGate;
				if (key == "camera_meas_cut_skip") return &s.meas.cutSkip;
				if (key == "camera_meas_fade_normalised") return &s.meas.fadeNormalised;
				if (key == "camera_meas_flash_bridge") return &s.meas.flashBridge;
				if (key == "camera_meas_temporal_seed") return &s.meas.temporalSeed;
				if (key == "camera_meas_tiny_mover_roi") return &s.meas.tinyMoverRoi;
				if (key == "camera_use_grain") return &s.use.grain;
				if (key == "camera_use_detail") return &s.use.detail;
				if (key == "camera_use_dither") return &s.use.dither;
				if (key == "camera_use_deband") return &s.use.deband;
				if (key == "camera_use_filler") return &s.use.filler;
				if (key == "camera_use_dejudder") return &s.use.dejudder;
				if (key == "camera_ahead_fit") return &s.aheadFit;
				return nullptr;
			}
			inline const bool* BoolSlot(const CameraSettings& s, const std::string& key)
			{
				return BoolSlot(const_cast<CameraSettings&>(s), key);
			}
		}

		// `key` and `value` as the file holds them (any case, surrounding blanks). NotMine = not a key of this section.
		inline Verdict Validate(const std::string& keyIn, const std::string& valueIn)
		{
			const std::string key = ConfigFile::NormalizeName(keyIn);
			const std::string value = ConfigFile::NormalizeName(valueIn);
			if (key == "camera_model_overlay")
				return (value == "off" || value == "labels" || value == "residual" || value == "vectors") ? Verdict::Valid : Verdict::Invalid;
			CameraSettings scratch;
			if (!Detail::BoolSlot(scratch, key))
				return Verdict::NotMine;
			return (value == "on" || value == "off") ? Verdict::Valid : Verdict::Invalid;
		}

		// The settings of the [vpv.camera] section; `originalPathway` is left false (the caller knows the pathway).
		inline CameraSettings Parse(const ConfigFile& config)
		{
			CameraSettings s;
			const std::map<std::string, std::string>* values = config.GetSectionValues(kSection);
			if (!values)
				return s;
			for (const std::pair<const std::string, std::string>& kv : *values)
			{
				const std::string key = ConfigFile::NormalizeName(kv.first);
				const std::string value = ConfigFile::NormalizeName(kv.second);
				switch (Validate(key, value))
				{
				case Verdict::NotMine:
					s.unknownKeys.push_back(key);
					break;
				case Verdict::Invalid:
					s.problems.push_back(key + " '" + kv.second + "' is not valid (" +
						(key == "camera_model_overlay" ? "off|labels|residual|vectors" : "on|off") + "); kept off");
					break;
				case Verdict::Valid:
					if (key == "camera_model_overlay")
						s.overlay = value == "labels" ? OverlayMode::Labels : value == "residual" ? OverlayMode::Residual :
							value == "vectors" ? OverlayMode::Vectors : OverlayMode::Off;
					else if (bool* slot = Detail::BoolSlot(s, key))
						*slot = value == "on";
					break;
				}
			}
			return s;
		}

		// A loader of the configuration file (the same type as Skeleton::ConfigLoader; this header stays free of the skeleton so the
		// Config tests can include it). The caller passes the skeleton's loader, or Skeleton::DefaultLoader when it has none.
		// An unreadable file (or no loader) gives defaults (everything off).
		using ConfigLoader = std::function<bool(ConfigFile&)>;
		inline CameraSettings Load(const ConfigLoader& loader)
		{
			ConfigFile config;
			if (!loader || !loader(config))
				return CameraSettings();
			return Parse(config);
		}

		// key / value pairs of every key in file order (the round trip of Parse).
		inline std::vector<std::pair<std::string, std::string>> KeyValues(const CameraSettings& s)
		{
			std::vector<std::pair<std::string, std::string>> out;
			for (size_t i = 0; i < kCameraKeyCount; ++i)
			{
				const std::string key = kCameraKeys[i];
				if (key == "camera_model_overlay")
					out.emplace_back(key, OverlayModeText(s.overlay));
				else if (const bool* slot = Detail::BoolSlot(s, key))
					out.emplace_back(key, *slot ? "on" : "off");
			}
			return out;
		}

		// "[vpv.camera]\nkey: value\n..." (the file format).
		inline std::string ToConfigText(const CameraSettings& s)
		{
			std::string text = std::string("[") + kSection + "]\n";
			for (const std::pair<std::string, std::string>& kv : KeyValues(s))
				text += kv.first + ": " + kv.second + "\n";
			return text;
		}

		inline bool operator==(const CameraSettings& a, const CameraSettings& b)
		{
			return KeyValues(a) == KeyValues(b) && a.aheadFit == b.aheadFit;
		}
	}
}
