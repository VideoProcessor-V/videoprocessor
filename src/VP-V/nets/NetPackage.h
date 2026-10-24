#pragma once

#include "NetJson.h"

#include <VP-V/core/PipelineSlots.h>

#include <map>
#include <string>
#include <vector>

// NetPackage: one exported net as a TYPED description (barrier B1, owner 2026-10-24).
//
// Why: the TrainKit exporter (trainkit/export_glsl.py) writes mpv-format user-shader text with
// the weights embedded and a fingerprint in the header comments, plus export_<net>.json. libplacebo
// can only run that text as hooks inside pl_render, which is the wrong place for VP's nets (B1:
// a LUMA hook runs after deband and texture restore and never on RGB capture). VP therefore runs
// the passes itself (NetRunner.h). This file turns the text into data the runner, the fingerprint
// check (NetFingerprint.h) and the tests can reason about: the passes, the textures they make, the
// binding contract (what VP must supply and what comes out), the PARAM labels and the declared
// stage contract.
//
// It does NOT interpret the GLSL bodies (GlslMini.h does, for the CPU reference executor; the
// pl_dispatch runner compiles them). C++14, no libplacebo, no Windows.
namespace VpNets
{
	// Texture formats the runner and the fingerprints talk about.
	enum class TexFormat { Unknown, R16Unorm, R16f, R32f, Rgba16f, Rgba32f };

	inline const char* FormatName(TexFormat f)
	{
		switch (f)
		{
		case TexFormat::R16Unorm: return "r16";
		case TexFormat::R16f: return "r16f";
		case TexFormat::R32f: return "r32f";
		case TexFormat::Rgba16f: return "rgba16f";
		case TexFormat::Rgba32f: return "rgba32f";
		default: return "unknown";
		}
	}

	// What the net is, from the exporter's title and input_stage. Decides the slot it serves and
	// where it runs.
	enum class NetRole
	{
		Unknown,
		RepairDelivered,   // A: on the ORIGINAL delivered frame, HOOK LUMA (A_single / A_motion)
		RepairEncoded,     // A (round 2): on the re-based (encoded-size) plane, + a state flag
		RebaseRefine,      // C: after each 1-D re-base pass (C_h, C_v)
		X2Restore,         // B: re-base restore x2 per axis
		ScaleRefine,       // D: inside the hybrid scaler (SCALER_SRC + SCALER_OUT)
		Decision,          // decision net: a control map for classical stages
	};
	const char* RoleName(NetRole role);

	// Where a net's passes run in VP.
	enum class RunSite
	{
		FrontEnd,         // VP-owned textures in the pre-libplacebo entrance (A, C, B, the 4K refine)
		PostKernelHook,   // inside AdaptiveDownscale's POST_KERNEL hook (D), via pl_hook_params' dispatch
	};

	// The slot a role serves (PipelineSlots ids; ADR-0021 order).
	PipelineSlots::Id SlotOf(NetRole role);

	// One //!PARAM block (A's labels: vpnn_ry, vpnn_rx, vpnn_kernel, vpnn_codec, vpnn_fgs, ...).
	struct NetParam
	{
		std::string name;
		bool isInt = false;
		double minimum = 0.0, maximum = 0.0, defaultValue = 0.0;
	};

	// One pass (one //!DESC block).
	struct NetPass
	{
		std::string desc;
		std::string hook;                  // LUMA, SCALER_OUT, SCALER_SRC
		std::vector<std::string> binds;    // HOOKED, saved textures, external inputs
		std::string save;                  // empty: the pass writes the hooked stage (the net output)
		int components = 4;
		std::string width, height;         // RPN size expressions ("HOOKED.w 2 /"); empty = hooked size
		int computeX = 0, computeY = 0;    // //!COMPUTE block size; 0 = fragment pass
		std::string body;                  // the GLSL after the directives
		bool IsCompute() const { return computeX > 0; }
	};

	// The binding contract: what VP must supply, and what the net gives back.
	struct BindingContract
	{
		std::string outputStage;                 // the hook stage the last pass replaces (LUMA / SCALER_OUT)
		std::vector<std::string> inputs;         // external textures: HOOKED (or the stage) + VPNN_PREV/NEXT/MASK, SCALER_SRC
		std::vector<std::string> labels;         // PARAM names, in file order
		bool neighbours = false;                 // needs motion-aligned VPNN_PREV / VPNN_NEXT / VPNN_MASK
		bool tileGated = false;                  // has a repair gate (closed tiles: exact copy)
		bool usesLinearSampling = false;         // some pass samples bilinearly (NAME_tex at a non-texel position)
		RunSite site = RunSite::FrontEnd;
		int outputComponents = 1;
	};

	// The fingerprint the exporter stamped (header comments and/or export json; the json wins and
	// the two must agree). `fields` keeps every key verbatim; the typed members are derived.
	struct NetFingerprint
	{
		std::string id;                          // "fingerprint": the kit's hash of everything below
		std::map<std::string, std::string> fields;
		std::string labelSchema;                 // "vpnn-labels-v1" or empty (no labels)
		std::string passOrder;                   // "H>V" when the export declares it
		TexFormat intermediate = TexFormat::Unknown;
		TexFormat handoff = TexFormat::Unknown;  // the re-base / x2 hand-off between axes
		bool fp16Allowed = false;                // export json fp16_check.fp16_within_1_lsb
		std::string Field(const std::string& key) const
		{
			const auto it = fields.find(key);
			return it == fields.end() ? std::string() : it->second;
		}
		bool Has(const std::string& key) const { return fields.count(key) != 0; }   // declared (maybe empty)
		std::vector<std::string> stamped;        // fields that came from a VP stamp sidecar, not the export
		bool idVerified = false;                 // the id recomputes from the recorded fields (kit h())
		std::string hybridMotionMode;            // D: "no_vectors" (round 2's record), "" = not recorded
		std::string upstreamNets;                // "slot=fingerprint|..." of the nets the training data came through
	};

	struct NetPackage
	{
		std::string name;                        // file stem, e.g. "C_rebase_refine_h"
		std::string title;                       // the first header line
		NetRole role = NetRole::Unknown;
		std::vector<NetParam> params;
		std::vector<NetPass> passes;
		std::vector<std::string> savedTextures;  // intermediates, in creation order
		BindingContract contract;
		NetFingerprint fingerprint;
		size_t weightLiterals = 0;               // float literals in the bodies (a size/cost hint)
		std::string shaderSha256;                // of the exported shader text (the weights are in it)

		// The net's identity for upstream-net records: the kit's fingerprint id is the PIPELINE's
		// stamp and is shared by nets of one round (A_single and A_motion have the same id), so a
		// specific net is named by its file stem and the sha256 of its shader text (16 hex).
		std::string Identity() const { return name + "@" + shaderSha256.substr(0, 16); }

		const NetParam* FindParam(const std::string& p) const
		{
			for (const NetParam& x : params) if (x.name == p) return &x;
			return nullptr;
		}
	};

	struct ParseResult
	{
		bool ok = false;
		std::string error;
		NetPackage package;
	};

	// Parses one exported shader's text. `name` is the file stem.
	ParseResult ParseShaderText(const std::string& text, const std::string& name);

	// Merges an export json's fingerprint (an object) into a parsed package: json fields win; a
	// header field that disagrees with the json makes the package invalid (returns false + reason).
	bool MergeExportFingerprint(NetPackage& package, const Json& exportJson, std::string* error);

	// Merges a VP stamp sidecar (tools/net_stamp.py, schema vp-net-stamp/1) into a package: only
	// fields the export lacks are taken; a field the export has must agree. The sidecar must name the
	// package's fingerprint id and the sha256 of its shader text. False + reason otherwise.
	bool MergeStampSidecar(NetPackage& package, const Json& sidecar, const std::string& shaderSha256, std::string* error);

	// Loads export_<net>.json and every shader it lists (from `shaderDir`, or <dir of json>/shaders).
	// One package per shader (C has C_h and C_v). A stamp sidecar <json stem>.vpstamp.json is looked
	// for in `sidecarDirs`, then next to the json; when found it is merged (a bad one fails the load).
	// On any failure returns false and *error.
	bool LoadExport(const std::string& exportJsonPath, const std::string& shaderDir,
		std::vector<NetPackage>& out, std::string* error, const std::vector<std::string>& sidecarDirs = std::vector<std::string>());

	// File helpers (binary read; false when the file is missing).
	bool ReadTextFile(const std::string& path, std::string& out);

	// Evaluates an mpv RPN size expression ("HOOKED.w 2 / 0.4375 +"). `lookup(name, 'w'|'h')`
	// returns a texture's size (false if unknown). The result is rounded to the nearest integer as
	// libplacebo does. Returns false on a malformed expression or unknown name.
	template <typename Lookup>
	bool EvalSizeExpr(const std::string& expr, Lookup lookup, int& result, std::string* error);
}

#include "NetPackage.inl"
