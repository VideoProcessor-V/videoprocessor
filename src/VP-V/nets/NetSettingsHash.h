#pragma once

#include "NetJson.h"
#include "NetPackage.h"
#include "NetSha256.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

// VP's computation of the TrainKit's settings hashes (U3, coordinator 2026-10-24).
//
// Why: each export's fingerprint records hashes of the pipeline settings that fed the net
// (trainkit/main.py fingerprint()): h(obj) = sha256(json.dumps(obj, sort_keys=True))[:16].
// VP compares them with ITS running settings, so it must produce byte-identical hashes: the
// Python json.dumps text (", " and ": " separators, sorted keys, ASCII escapes, Python float
// repr, int vs float kept apart) and the same objects:
//   rebase_settings        h({rebase_kernel_match, source_resolution_latch_fixes, per_axis_inverse,
//                             separable_passes, intermediate, one_d_kernel})
//   grain_split            h({operator, eps_codes, r2160, pad})
//   stage_order            h(the stage order text)
//   scaler_blend_settings  h({common, down, up, adapter} of the render settings; {} when absent)
//   bounds                 h({"bound": {b0, b1, b2, bmax}, "scale_bound": {c0, c1, cmax}})
//   fingerprint            h(every recorded field except "fingerprint" and "stage_order_text")
// Definitions read from the round-1 kit (D:\nn-corpus\VP-NN-TrainKit\trainkit\main.py, read only);
// the defaults below are the kit's values, so a default struct hashes to the recorded value.
// Header-only, C++14.
namespace VpNets
{
	namespace KitHash
	{
		// A value with Python's JSON types (int and float differ: 2 vs 2.0).
		struct Value
		{
			enum class Kind { Null, Bool, Int, Float, String, Array, Object } kind = Kind::Null;
			bool b = false;
			long long i = 0;
			double f = 0.0;
			std::string s;
			std::vector<Value> items;
			std::map<std::string, Value> fields;   // sorted, as sort_keys=True

			static Value Str(const std::string& v) { Value x; x.kind = Kind::String; x.s = v; return x; }
			static Value Int(long long v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
			static Value Float(double v) { Value x; x.kind = Kind::Float; x.f = v; return x; }
			static Value Bool(bool v) { Value x; x.kind = Kind::Bool; x.b = v; return x; }
			static Value Obj() { Value x; x.kind = Kind::Object; return x; }
			Value& Set(const std::string& k, const Value& v) { kind = Kind::Object; fields[k] = v; return *this; }
		};

		// Python repr(float): the shortest digits that round-trip, fixed notation for
		// 1e-4 <= |x| < 1e16 (with ".0" when integral), else d.ddde+XX.
		inline std::string PyFloat(double x)
		{
			if (std::isnan(x)) return "NaN";
			if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
			if (x == 0.0) return std::signbit(x) ? "-0.0" : "0.0";
			char buf[64];
			int prec = 1;
			for (; prec <= 17; ++prec)
			{
				std::snprintf(buf, sizeof buf, "%.*e", prec - 1, x);
				if (std::strtod(buf, nullptr) == x) break;
			}
			std::string t = buf;   // [-]d.ddde[+-]XX
			const bool neg = t[0] == '-';
			if (neg) t = t.substr(1);
			const size_t e = t.find('e');
			const int exp10 = std::atoi(t.c_str() + e + 1);
			std::string digits;
			for (size_t k = 0; k < e; ++k) if (t[k] != '.') digits += t[k];
			while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
			std::string out;
			const int n = static_cast<int>(digits.size());
			if (exp10 >= -4 && exp10 < 16)
			{
				if (exp10 >= n - 1) out = digits + std::string(static_cast<size_t>(exp10 - (n - 1)), '0') + ".0";
				else if (exp10 >= 0) out = digits.substr(0, static_cast<size_t>(exp10 + 1)) + "." + digits.substr(static_cast<size_t>(exp10 + 1));
				else out = "0." + std::string(static_cast<size_t>(-exp10 - 1), '0') + digits;
			}
			else
			{
				out = digits.substr(0, 1);
				if (n > 1) out += "." + digits.substr(1);
				char eb[16];
				std::snprintf(eb, sizeof eb, "e%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
				out += eb;
			}
			return neg ? "-" + out : out;
		}

		// json.dumps string escaping (ensure_ascii=True): UTF-8 in, \uXXXX (surrogate pairs) out.
		inline std::string PyString(const std::string& s)
		{
			std::string o = "\"";
			char eb[16];
			auto u = [&](unsigned cp) { std::snprintf(eb, sizeof eb, "\\u%04x", cp); o += eb; };
			for (size_t i = 0; i < s.size();)
			{
				unsigned char c = static_cast<unsigned char>(s[i]);
				unsigned cp = c;
				size_t len = 1;
				if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu); len = 4; }
				else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); len = 3; }
				else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); len = 2; }
				i += len;
				switch (cp)
				{
				case '"': o += "\\\""; continue;
				case '\\': o += "\\\\"; continue;
				case '\n': o += "\\n"; continue;
				case '\r': o += "\\r"; continue;
				case '\t': o += "\\t"; continue;
				case '\b': o += "\\b"; continue;
				case '\f': o += "\\f"; continue;
				default: break;
				}
				if (cp >= 0x20 && cp < 0x7F) o += static_cast<char>(cp);   // printable ASCII as is
				else if (cp < 0x10000) u(cp);                               // controls, DEL, BMP
				else { cp -= 0x10000; u(0xD800 + (cp >> 10)); u(0xDC00 + (cp & 0x3FF)); }
			}
			return o + "\"";
		}

		// json.dumps(v, sort_keys=True).
		inline std::string Dumps(const Value& v)
		{
			switch (v.kind)
			{
			case Value::Kind::Null: return "null";
			case Value::Kind::Bool: return v.b ? "true" : "false";
			case Value::Kind::Int: return std::to_string(v.i);
			case Value::Kind::Float: return PyFloat(v.f);
			case Value::Kind::String: return PyString(v.s);
			case Value::Kind::Array:
			{
				std::string o = "[";
				for (size_t k = 0; k < v.items.size(); ++k) o += (k ? ", " : "") + Dumps(v.items[k]);
				return o + "]";
			}
			default:
			{
				std::string o = "{";
				bool first = true;
				for (const auto& kv : v.fields) { o += (first ? "" : ", ") + PyString(kv.first) + ": " + Dumps(kv.second); first = false; }
				return o + "}";
			}
			}
		}

		// The kit's h(): the first 16 hex digits of sha256(json.dumps(obj, sort_keys=True)).
		inline std::string H(const Value& v) { return Sha256Hex(Dumps(v)).substr(0, 16); }

		// A parsed JSON value as a Python value (a number with '.', 'e' or 'E' is a float, as
		// Python's json.loads reads it).
		inline Value FromJson(const Json& j)
		{
			switch (j.kind)
			{
			case Json::Kind::Bool: return Value::Bool(j.boolean);
			case Json::Kind::Number:
				if (j.text.find_first_of(".eE") != std::string::npos || j.text == "NaN" || j.text == "Infinity") return Value::Float(j.number);
				return Value::Int(std::strtoll(j.text.c_str(), nullptr, 10));
			case Json::Kind::String: return Value::Str(j.text);
			case Json::Kind::Array: { Value a; a.kind = Value::Kind::Array; for (const Json& x : j.items) a.items.push_back(FromJson(x)); return a; }
			case Json::Kind::Object: { Value o = Value::Obj(); for (const auto& kv : j.fields) o.fields[kv.first] = FromJson(kv.second); return o; }
			default: return Value();
			}
		}
	}

	// ------------------------------------------------------------------ the settings VP hashes
	// VP fills these from what it runs; the defaults are the kit's planned values (round 1/2).
	struct RebaseHashSettings
	{
		std::string rebaseKernelMatch = "on";
		std::string latchFixes = "on";              // source_resolution_latch_fixes
		bool perAxisInverse = true;
		std::string separablePasses = "width then height";
		std::string intermediate = "16-bit UNORM (r16, SourceRebase)";
		std::string oneDKernel = "pl_filter_lanczos r3, antiring 0.5";
	};

	struct GrainSplitHashSettings
	{
		std::string op = "self-guided filter";
		double epsCodes = 6.0;   // float in the kit config
		long long r2160 = 2;     // int in the kit config
		std::string pad = "reflect";
	};

	struct BoundsHashSettings
	{
		double b0 = 1.5, b1 = 2.0, b2 = 0.35, bmax = 24.0;   // "bound"
		double c0 = 1.0, c1 = 0.25, cmax = 32.0;             // "scale_bound"
	};

	// The kit's planned stage order text (round 1/2).
	constexpr const char* kKitStageOrderR1 =
		"dv_reconstruction(nets never see raw DV IPT)>detect_on_original(kernel, labels)>crop>A_on_original_delivered_frame>"
		"per_axis[1D_rebase_pass(kernel-matched 2:1 or 1D Lanczos, r16)>C]>per_axis[x2_pass>B(H r16 hand-off)]>hybrid_ssim_downscale(w0.2, lanczos base, ortho)>D>rest_of_VP";

	inline std::string RebaseSettingsHash(const RebaseHashSettings& r)
	{
		using KitHash::Value;
		Value o = Value::Obj();
		o.Set("rebase_kernel_match", Value::Str(r.rebaseKernelMatch)).Set("source_resolution_latch_fixes", Value::Str(r.latchFixes))
			.Set("per_axis_inverse", Value::Bool(r.perAxisInverse)).Set("separable_passes", Value::Str(r.separablePasses))
			.Set("intermediate", Value::Str(r.intermediate)).Set("one_d_kernel", Value::Str(r.oneDKernel));
		return KitHash::H(o);
	}

	inline std::string GrainSplitHash(const GrainSplitHashSettings& g)
	{
		using KitHash::Value;
		Value o = Value::Obj();
		o.Set("operator", Value::Str(g.op)).Set("eps_codes", Value::Float(g.epsCodes)).Set("r2160", Value::Int(g.r2160)).Set("pad", Value::Str(g.pad));
		return KitHash::H(o);
	}

	inline std::string StageOrderHash(const std::string& orderText) { return KitHash::H(KitHash::Value::Str(orderText)); }

	inline std::string BoundsHash(const BoundsHashSettings& b)
	{
		using KitHash::Value;
		Value bound = Value::Obj(), scale = Value::Obj(), o = Value::Obj();
		bound.Set("b0", Value::Float(b.b0)).Set("b1", Value::Float(b.b1)).Set("b2", Value::Float(b.b2)).Set("bmax", Value::Float(b.bmax));
		scale.Set("c0", Value::Float(b.c0)).Set("c1", Value::Float(b.c1)).Set("cmax", Value::Float(b.cmax));
		o.Set("bound", bound).Set("scale_bound", scale);
		return KitHash::H(o);
	}

	// `renderSettings`: the render settings object (scaling_manifest.json "settings"); only
	// common/down/up/adapter enter the hash. A non-object (no manifest) hashes as {}, as the kit.
	inline std::string ScalerBlendHash(const Json& renderSettings)
	{
		KitHash::Value o = KitHash::Value::Obj();
		if (renderSettings.IsObject())
			for (const char* k : { "common", "down", "up", "adapter" })
				if (renderSettings.Has(k)) o.fields[k] = KitHash::FromJson(renderSettings[k]);
		return KitHash::H(o);
	}

	// The settingsHashes map NetRuntimeContext wants, from VP's values.
	inline std::map<std::string, std::string> SettingsHashes(const RebaseHashSettings& r, const GrainSplitHashSettings& g,
		const std::string& stageOrderText, const Json& renderSettings, const BoundsHashSettings& b)
	{
		return {
			{ "rebase_settings", RebaseSettingsHash(r) },
			{ "grain_split", GrainSplitHash(g) },
			{ "stage_order", StageOrderHash(stageOrderText) },
			{ "scaler_blend_settings", ScalerBlendHash(renderSettings) },
			{ "bounds", BoundsHash(b) },
		};
	}

	// Recomputes an export's fingerprint id from its recorded fields: proves the fields are the
	// ones the kit hashed (nothing edited after export, no field missing). A kit that records a
	// field after computing the id makes this false: the caller then treats the id as unverified.
	inline bool FingerprintIntact(const Json& recordedFingerprint, std::string* computed = nullptr)
	{
		if (!recordedFingerprint.IsObject()) return false;
		KitHash::Value o = KitHash::Value::Obj();
		for (const auto& kv : recordedFingerprint.fields)
		{
			if (kv.first == "fingerprint" || kv.first == "stage_order_text") continue;
			o.fields[kv.first] = KitHash::FromJson(kv.second);
		}
		const std::string id = KitHash::H(o);
		if (computed) *computed = id;
		return id == recordedFingerprint["fingerprint"].text;
	}
}
