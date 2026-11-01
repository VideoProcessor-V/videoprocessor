// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Light-level map settings (`input.light_level`, DRAFT). Header-only, C++14, no Windows or libplacebo headers.
//
// Two VP-owned keys (Specs(), like ToneMappingTuning::Specs) plus READS of existing keys. Every read follows the
// renderer's own parse rules; an invalid or missing value falls back to the spec default and an invalid one adds a
// line to Settings::notes (never a silent fallback: the host surfaces the notes in the Light level status line).
//
//   VP-owned: light_level_weber, light_level_ambient_nits, light_level_display_white_nits,
//             light_level_display_black_nits (each display key: `auto` = sdr_target_nits / sdr_black_nits, or a number)
//   existing: sdr_target_nits, sdr_black_nits, hdr_tone_map_target_gamma, tone_mapping, peak_detection, quality,
//             the ToneMappingTuning keys (ToneMappingTuning::Read), sdr_tone_shape, sdr_white_headroom,
//             sdr_knee_start, sdr_knee_strength.
//
// Entry: `template<class Getter> Settings Read(Getter get)` with `bool get(const std::string& key, std::string& raw)`
// (true when the key is present in the effective configuration).
#pragma once

#include <array>
#include <cctype>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "../../../../../VideoProcessor-Lib/HdrTargetLuminance.h"
#include "../../../../../VideoProcessor-Lib/ToneMappingTuning.h"
#include "../../../colour/sdr_tone_shape/SdrToneShape.h"

namespace LightLevel
{
	// ---- VP-owned keys ----
	enum SpecIndex { Weber, AmbientNits, DisplayWhiteNits, DisplayBlackNits, SpecCount };
	struct Spec
	{
		const char* key;
		const char* label;
		double minimum, maximum, defaultValue;
		const char* unit;
		const char* help;
		bool autoAllowed;   // the value may be the token `auto` (resolved from the encoding target in Read)
	};
	inline const std::array<Spec, SpecCount>& Specs()
	{
		static const std::array<Spec, SpecCount> specs = {{
			// PROVISIONAL DEFAULT: 0.02 is the usual Weber fraction for a luminance step visible on a mid-grey
			// field (about 1 to 2 percent); to be tuned against the owner's i1Pro measurements.
			{"light_level_weber", "Weber fraction", 0.001, 0.2, 0.02, "fraction",
				"Relative luminance step the viewer just notices; one visibility unit V = one such step.", false},
			// PROVISIONAL DEFAULT: 0 (dark room, projector); the floor added to the display black for V.
			{"light_level_ambient_nits", "Ambient floor", 0.0, 1000.0, 0.0, "nits",
				"Room light reflected from the screen, added to the display black when weighing visibility.", false},
			// PROVISIONAL: auto = sdr_target_nits. The PHYSICAL white of the display the viewer looks at; the
			// encoding target (sdr_target_nits) is what libplacebo tone maps to. Range 1 to 10000 nits.
			{"light_level_display_white_nits", "Display white", 1.0, 10000.0, 0.0, "nits",
				"Physical peak white of the display (auto = sdr_target_nits); the light the viewer sees at signal 1.", true},
			// PROVISIONAL: auto = sdr_black_nits. The PHYSICAL black of the display (native contrast, no
			// ambient). Range 0 to 100 nits; must stay below the display white.
			{"light_level_display_black_nits", "Display black", 0.0, 100.0, 0.0, "nits",
				"Physical black of the display (auto = sdr_black_nits); the light the viewer sees at signal 0.", true}
		}};
		return specs;
	}

	enum class Transfer { Bt1886, Srgb, Power };
	enum class PeakDetection { Auto, Off, Standard, HighQuality };

	struct Settings
	{
		double weber = Specs()[Weber].defaultValue;
		double ambientNits = Specs()[AmbientNits].defaultValue;

		// sdr_target_nits: renderer default PL_COLOR_SDR_WHITE = 203 nits (LibplaceboVideoRenderer.cpp:1036,
		// 3rdparty/libplacebo/include/libplacebo/colorspace.h:280). Parsed by HdrTargetLuminance::ParseWhite
		// (LibplaceboVideoRenderer.cpp:3803-3812).
		double sdrTargetNits = 203.0;
		// sdr_black_nits: "auto" and omission resolve to HdrTargetLuminance::DefaultBlack = 0
		// (HdrTargetLuminance.h:13; LibplaceboVideoRenderer.cpp:3814-3820 via ParseBlack, HdrTargetLuminance.h:42-55).
		// The renderer resets to the default before parsing (LibplaceboVideoRenderer.cpp:3814) and again when the
		// resolved black conflicts with white (NormalizeSdrBlackLevel, LibplaceboVideoRenderer.cpp:2330-2337).
		// Zero stays zero here; libplacebo's boundary maps it to HdrTargetLuminance::BlackFloor.
		double sdrBlackNits = HdrTargetLuminance::DefaultBlack;

		// ENCODING target = sdr_target_nits / sdr_black_nits (what libplacebo tone maps to and encodes with the inverse
		// of the target transfer). PHYSICAL display = the two resolved values below (what the viewer's screen emits at
		// signal 1 and 0): light_level_display_white_nits / light_level_display_black_nits, `auto` = the encoding
		// target. Read() always leaves white > black here.
		double displayWhiteNits = 203.0;
		double displayBlackNits = HdrTargetLuminance::DefaultBlack;

		// hdr_tone_map_target_gamma (LibplaceboVideoRenderer.cpp:3907-3908): bt1886 | srgb | 1.8 .. 2.8; empty = bt1886.
		Transfer transfer = Transfer::Bt1886;
		double gammaPower = 2.4;        // used when transfer == Power

		std::string toneMapping = "auto";               // auto | spline | bt2390 | st2094-40 | reinhard
		PeakDetection peakDetection = PeakDetection::Auto;
		std::string quality = "high";                   // fast | balanced | high: selects the libplacebo preset
		ToneMappingTuning::Overrides tuning{};          // auto = the ToneMappingTuning::Specs() default

		SdrToneShape::Settings shape;                   // sdr_tone_shape / headroom / knee start / knee strength
		std::vector<std::string> notes;                 // one entry per invalid value that fell back
	};

	// The power the SDR TONE SHAPE uses. BT.1886 = 2.4 by definition; sRGB is approximated by 2.2 here only (the
	// renderer's LedDisplayGamma(target.color.transfer) is the source for the shape, LibplaceboVideoRenderer.cpp:23865).
	// The display model (Display / Display::Inverse) uses the exact piecewise sRGB.
	inline double EffectivePower(const Settings& s)
	{
		return s.transfer == Transfer::Bt1886 ? 2.4 : s.transfer == Transfer::Srgb ? 2.2 : s.gammaPower;
	}

	// Whether libplacebo peak detection (dynamic tone mapping) is active for these settings.
	// `peak_detection: auto` keeps the quality preset's own value
	// (src/VideoProcessor-Lib/vprenderer/LibplaceboRenderParameters.cpp:317-326; the preset is chosen by QualityPreset,
	// :45-50, default quality "high", LibplaceboVideoRenderer.cpp:1045). libplacebo's fast preset has no peak
	// detection, the balanced (default) and high-quality presets have it, so auto is OFF for quality=fast and ON
	// otherwise. standard / high_quality force it on, off forces it off (:312-316).
	inline bool PeakDetectionOn(const Settings& s)
	{
		if (s.peakDetection == PeakDetection::Off) return false;
		if (s.peakDetection == PeakDetection::Auto) return s.quality != "fast";
		return true;
	}

	namespace detail
	{
		inline std::string Normalize(const std::string& text)
		{
			const auto first = text.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) return std::string();
			std::string t = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
			for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return t;
		}
		inline bool ParseNumber(const std::string& text, double& value)
		{
			std::istringstream in(Normalize(text)); in.imbue(std::locale::classic());
			double v = 0.0;
			if (!(in >> v) || !std::isfinite(v)) return false;
			in >> std::ws;
			if (!in.eof()) return false;
			value = v; return true;
		}
		inline std::string Num(double v)
		{
			std::ostringstream o; o.imbue(std::locale::classic()); o << v; return o.str();
		}
		inline void Note(Settings& s, const std::string& key, const std::string& raw, const std::string& fallback)
		{
			s.notes.push_back(key + " '" + raw + "' invalid; using " + fallback);
		}
		inline void ReadRanged(Settings& s, const char* key, double minimum, double maximum, double& target,
			const std::string& raw)
		{
			double v = 0.0;
			if (ParseNumber(raw, v) && v >= minimum && v <= maximum) target = v;
			else Note(s, key, raw, Num(target));
		}
	}

	template<class Getter>
	inline Settings Read(Getter get)
	{
		Settings s;
		std::string raw;
		double displayWhite = -1.0, displayBlack = -1.0;   // < 0 = auto (resolved after the encoding target is known)

		for (int i = 0; i < SpecCount; ++i)
		{
			const Spec& spec = Specs()[i];
			raw.clear();
			if (!get(spec.key, raw)) continue;
			double v = 0.0;
			if (spec.autoAllowed)
			{
				if (detail::Normalize(raw) == "auto") continue;
				if (detail::ParseNumber(raw, v) && v >= spec.minimum && v <= spec.maximum)
					(i == DisplayWhiteNits ? displayWhite : displayBlack) = v;
				else
					detail::Note(s, spec.key, raw, "auto");
				continue;
			}
			if (detail::ParseNumber(raw, v) && v >= spec.minimum && v <= spec.maximum)
				(i == Weber ? s.weber : s.ambientNits) = v;
			else
				detail::Note(s, spec.key, raw, detail::Num(spec.defaultValue));
		}

		raw.clear();
		if (get("sdr_target_nits", raw))
		{
			double v = 0.0;
			if (HdrTargetLuminance::ParseWhite(detail::Normalize(raw), v)) s.sdrTargetNits = v;
			else detail::Note(s, "sdr_target_nits", raw, detail::Num(s.sdrTargetNits));
		}
		raw.clear();
		if (get("sdr_black_nits", raw))
		{
			double v = 0.0;
			if (HdrTargetLuminance::ParseBlack(detail::Normalize(raw), s.sdrTargetNits, v)) s.sdrBlackNits = v;
			else
			{
				detail::Note(s, "sdr_black_nits", raw, "default black " + detail::Num(HdrTargetLuminance::DefaultBlack));
				s.sdrBlackNits = HdrTargetLuminance::DefaultBlack;
			}
		}
		if (!HdrTargetLuminance::ValidBlack(s.sdrBlackNits, s.sdrTargetNits))
		{
			// The renderer's NormalizeSdrBlackLevel (LibplaceboVideoRenderer.cpp:2330-2337) after white and black resolve.
			s.notes.push_back("sdr_black_nits conflicts with sdr_target_nits; using default black");
			s.sdrBlackNits = HdrTargetLuminance::DefaultBlack;
		}

		// Physical display: auto = the encoding target; white must stay above black or both fall back to auto.
		s.displayWhiteNits = displayWhite >= 0.0 ? displayWhite : s.sdrTargetNits;
		s.displayBlackNits = displayBlack >= 0.0 ? displayBlack : s.sdrBlackNits;
		if (!(s.displayWhiteNits > s.displayBlackNits))
		{
			s.notes.push_back("light_level_display_white_nits must exceed light_level_display_black_nits; using auto "
				"(sdr_target_nits / sdr_black_nits)");
			s.displayWhiteNits = s.sdrTargetNits;
			s.displayBlackNits = s.sdrBlackNits;
		}

		raw.clear();
		if (get("hdr_tone_map_target_gamma", raw))
		{
			const std::string t = detail::Normalize(raw);
			double v = 0.0;
			if (t.empty() || t == "bt1886") s.transfer = Transfer::Bt1886;
			else if (t == "srgb") s.transfer = Transfer::Srgb;
			else if (detail::ParseNumber(t, v) && v >= 1.8 && v <= 2.8) { s.transfer = Transfer::Power; s.gammaPower = v; }
			else detail::Note(s, "hdr_tone_map_target_gamma", raw, "bt1886");
		}

		raw.clear();
		if (get("tone_mapping", raw))
		{
			const std::string t = detail::Normalize(raw);
			if (t == "auto" || t == "spline" || t == "bt2390" || t == "st2094-40" || t == "reinhard") s.toneMapping = t;
			else detail::Note(s, "tone_mapping", raw, "auto");
		}
		raw.clear();
		if (get("peak_detection", raw))
		{
			// Same token set as LibplaceboVideoRenderer.cpp:2476-2495 (display rule reader).
			const std::string t = detail::Normalize(raw);
			if (t == "auto") s.peakDetection = PeakDetection::Auto;
			else if (t == "off" || t == "false") s.peakDetection = PeakDetection::Off;
			else if (t == "default" || t == "on" || t == "true") s.peakDetection = PeakDetection::Standard;
			else if (t == "high_quality") s.peakDetection = PeakDetection::HighQuality;
			else detail::Note(s, "peak_detection", raw, "auto");
		}

		raw.clear();
		if (get("quality", raw))
		{
			// Same token set as LibplaceboVideoRenderer.cpp:2521 / 3836.
			const std::string t = detail::Normalize(raw);
			if (t == "fast" || t == "balanced" || t == "high") s.quality = t;
			else detail::Note(s, "quality", raw, "high");
		}

		// ToneMappingTuning::Read skips invalid values silently; report them here.
		ToneMappingTuning::Read(get, s.tuning);
		for (int i = 0; i < ToneMappingTuning::Count; ++i)
		{
			raw.clear();
			if (!get(ToneMappingTuning::Specs()[i].key, raw)) continue;
			ToneMappingTuning::Override probe;
			if (!ToneMappingTuning::Parse(i, raw, probe))
				detail::Note(s, ToneMappingTuning::Specs()[i].key, raw, "auto");
		}

		raw.clear();
		if (get("sdr_tone_shape", raw))
		{
			const std::string t = detail::Normalize(raw);
			if (t == "off" || t == "knee") s.shape.knee = t == "knee";
			else detail::Note(s, "sdr_tone_shape", raw, s.shape.knee ? "knee" : "off");
		}
		raw.clear();
		if (get("sdr_white_headroom", raw)) detail::ReadRanged(s, "sdr_white_headroom", 0.0, 0.5, s.shape.headroom, raw);
		raw.clear();
		if (get("sdr_knee_start", raw)) detail::ReadRanged(s, "sdr_knee_start", 0.5, 0.99, s.shape.kneeStart, raw);
		raw.clear();
		if (get("sdr_knee_strength", raw)) detail::ReadRanged(s, "sdr_knee_strength", 0.0, 1.0, s.shape.kneeStrength, raw);
		return s;
	}
}
