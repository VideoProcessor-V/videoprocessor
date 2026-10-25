#include "NetPackage.h"
#include "NetSettingsHash.h"
#include "NetSha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

// Parser for the TrainKit exporter's mpv-format shader text (see NetPackage.h for why).
// The format, as export_glsl.py writes it:
//   header comments: "// VP-NN <name>: <what>", "// <desc>", notes, then
//     "// VPNN pipeline fingerprint: <id> input_stage=<...>" and "//   <key>: <value>" lines;
//   //!PARAM blocks (//!TYPE, //!MINIMUM, //!MAXIMUM, then the default value on its own line);
//   pass blocks: //!DESC, //!HOOK, //!BIND..., [//!COMPUTE x y], [//!SAVE], [//!WIDTH/HEIGHT],
//     //!COMPONENTS, a blank line, then the GLSL body up to the next block.
namespace VpNets
{
	namespace
	{
		std::string Trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
			return s.substr(a, b - a);
		}

		bool StartsWith(const std::string& s, const char* p)
		{
			const size_t n = std::char_traits<char>::length(p);
			return s.size() >= n && s.compare(0, n, p) == 0;
		}

		bool Contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// "rgba32f" / "rgba16f" mentioned as the net intermediates in texture_formats.
		TexFormat IntermediateFrom(const std::string& textureFormats)
		{
			const std::string t = Lower(textureFormats);
			const size_t at = t.find("intermediates");
			if (at == std::string::npos) return TexFormat::Unknown;
			const std::string tail = t.substr(at, 40);
			if (Contains(tail, "rgba32f")) return TexFormat::Rgba32f;
			if (Contains(tail, "rgba16f") || Contains(tail, "fp16")) return TexFormat::Rgba16f;
			return TexFormat::Unknown;
		}

		TexFormat HandoffFrom(const std::string& textureFormats)
		{
			const std::string t = Lower(textureFormats);
			if (Contains(t, "pass outputs r16") || Contains(t, "hand-off r16")) return TexFormat::R16Unorm;
			return TexFormat::Unknown;
		}

		// The typed fingerprint members from the verbatim fields.
		void DeriveFingerprint(NetFingerprint& fp)
		{
			fp.id = fp.Field("fingerprint");
			fp.labelSchema = fp.Field("label_schema");
			if (fp.labelSchema == "none") fp.labelSchema.clear();   // a stamp's "no labels"
			const std::string tf = fp.Field("texture_formats");
			fp.intermediate = IntermediateFrom(tf);
			fp.handoff = HandoffFrom(tf);
			// An explicit field (exporters after 2026-10-24, or a stamp) wins. Round 1/2 state the pass
			// order only for the separable nets, in b_structure / c_structure ("passes H then V"). The
			// shared texture_formats wording ("x2 H->V hand-off") is about B and must not give A or D
			// a pass order (audit 2026-10-24).
			fp.passOrder = fp.Field("pass_order");
			if (fp.passOrder == "n/a") fp.passOrder.clear();
			if (fp.passOrder.empty())
			{
				for (const char* key : { "b_structure", "c_structure" })
				{
					const std::string v = fp.Field(key);
					if (Contains(v, "H then V") || Contains(v, "H->V"))
					{
						fp.passOrder = "H>V";
						break;
					}
				}
			}
			if (fp.Field("b_structure").empty() && fp.Field("c_structure").empty() && !fp.Has("handoff_format"))
				fp.handoff = TexFormat::Unknown;   // the r16 hand-off belongs to the separable chain only
			if (fp.Field("handoff_format") == "r16") fp.handoff = TexFormat::R16Unorm;
			if (fp.Field("handoff_format") == "n/a") fp.handoff = TexFormat::Unknown;
			// D's hybrid motion mode (round 2: "NO VECTORS: ..."): typed for the check.
			const std::string hm = fp.Field("hybrid_motion_mode");
			fp.hybridMotionMode = hm.empty() ? std::string() : Contains(hm, "NO VECTORS") ? "no_vectors" : Lower(hm);
			const std::string explicitFormat = Lower(fp.Field("intermediate_format"));
			if (explicitFormat == "rgba32f") fp.intermediate = TexFormat::Rgba32f;
			else if (explicitFormat == "rgba16f") fp.intermediate = TexFormat::Rgba16f;
		}

		NetRole RoleFromTitle(const std::string& title, const std::string& inputStage)
		{
			const std::string t = Lower(title);
			if (Contains(t, "stage a repair on the delivered frame")) return NetRole::RepairDelivered;
			if (Contains(t, "encoded") || Contains(t, "bounded codec-artifact repair")) return NetRole::RepairEncoded;
			if (Contains(t, "re-base refine")) return NetRole::RebaseRefine;
			if (Contains(t, "re-base restore x2")) return NetRole::X2Restore;
			if (Contains(t, "bounded scaling repair")) return NetRole::ScaleRefine;
			if (Contains(t, "decision net")) return NetRole::Decision;
			if (StartsWith(inputStage, "A:")) return NetRole::RepairDelivered;
			if (StartsWith(inputStage, "C:")) return NetRole::RebaseRefine;
			if (StartsWith(inputStage, "D:")) return NetRole::ScaleRefine;
			return NetRole::Unknown;
		}

		size_t CountFloatLiterals(const std::string& body)
		{
			size_t n = 0;
			for (size_t i = 0; i < body.size(); ++i)
			{
				const char c = body[i];
				if (c == '.' && i > 0 && std::isdigit(static_cast<unsigned char>(body[i - 1])) &&
					(i == 1 || !std::isalpha(static_cast<unsigned char>(body[i - 2]))))
					++n;
			}
			return n;
		}

		std::string Compact(const Json& j)
		{
			switch (j.kind)
			{
			case Json::Kind::Null: return "null";
			case Json::Kind::Bool: return j.boolean ? "true" : "false";
			case Json::Kind::Number: return j.text;
			case Json::Kind::String: return j.text;
			case Json::Kind::Array:
			{
				std::string s = "[";
				for (size_t i = 0; i < j.items.size(); ++i) s += (i ? "," : "") + Compact(j.items[i]);
				return s + "]";
			}
			default:
			{
				std::string s = "{";
				bool first = true;
				for (const auto& kv : j.fields) { s += (first ? "" : ",") + kv.first + ":" + Compact(kv.second); first = false; }
				return s + "}";
			}
			}
		}
	}

	const char* RoleName(NetRole role)
	{
		switch (role)
		{
		case NetRole::RepairDelivered: return "repair (delivered frame)";
		case NetRole::RepairEncoded: return "repair (encoded plane)";
		case NetRole::RebaseRefine: return "re-base refine";
		case NetRole::X2Restore: return "x2 restore";
		case NetRole::ScaleRefine: return "scaler refine (D)";
		case NetRole::Decision: return "decision";
		default: return "unknown";
		}
	}

	PipelineSlots::Id SlotOf(NetRole role)
	{
		using PipelineSlots::Id;
		switch (role)
		{
		case NetRole::RepairDelivered: case NetRole::RepairEncoded: case NetRole::Decision: return Id::RepairEncoding;
		case NetRole::RebaseRefine: return Id::ScalingRebase;
		case NetRole::X2Restore: return Id::ScalingRoute4k;
		case NetRole::ScaleRefine: return Id::ScalingOutput;
		default: return Id::ControlNetRuntime;
		}
	}

	bool ReadTextFile(const std::string& path, std::string& out)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f) return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	ParseResult ParseShaderText(const std::string& text, const std::string& name)
	{
		ParseResult r;
		NetPackage& pkg = r.package;
		pkg.name = name;
		pkg.shaderSha256 = Sha256Hex(text);

		std::vector<std::string> lines;
		{
			std::string line;
			std::istringstream in(text);
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				lines.push_back(line);
			}
		}

		enum class Block { Header, Param, Pass } block = Block::Header;
		NetPass* pass = nullptr;
		NetParam* param = nullptr;
		bool paramValueSeen = false;
		bool passBodyStarted = false;
		std::string fpHeaderId;
		auto fail = [&](size_t ln, const std::string& why)
		{
			r.ok = false;
			r.error = name + ": line " + std::to_string(ln + 1) + ": " + why;
			return r;
		};

		for (size_t ln = 0; ln < lines.size(); ++ln)
		{
			const std::string& line = lines[ln];
			const bool directive = StartsWith(line, "//!");
			if (!directive)
			{
				if (block == Block::Header)
				{
					if (StartsWith(line, "//"))
					{
						const std::string c = line.substr(2);
						if (pkg.title.empty() && !Trim(c).empty()) pkg.title = Trim(c);
						const std::string fpTag = " VPNN pipeline fingerprint: ";
						if (StartsWith(c, fpTag.c_str()))
						{
							std::string rest = c.substr(fpTag.size());
							fpHeaderId = rest.substr(0, rest.find(' '));
							const size_t is = rest.find("input_stage=");
							if (is != std::string::npos && !pkg.fingerprint.fields.count("input_stage"))
								pkg.fingerprint.fields["input_stage"] = rest.substr(is + 12);
						}
						else if (StartsWith(c, "   ") && Contains(c, ":"))
						{
							// "//   key: value" (value may be empty: "//   upstream_nets: ")
							const std::string kv = c.substr(3);
							const size_t colon = kv.find(':');
							std::string value = kv.substr(colon + 1);
							if (!value.empty() && value[0] == ' ') value = value.substr(1);
							pkg.fingerprint.fields[Trim(kv.substr(0, colon))] = value;
						}
					}
					continue;
				}
				if (block == Block::Param)
				{
					if (!paramValueSeen && !Trim(line).empty())
					{
						param->defaultValue = std::strtod(Trim(line).c_str(), nullptr);
						paramValueSeen = true;
					}
					continue;
				}
				// Pass body (the blank separator line included; harmless in GLSL).
				if (passBodyStarted || !Trim(line).empty())
					passBodyStarted = true;
				if (passBodyStarted)
					pass->body += line + "\n";
				continue;
			}

			// A directive. A new block starts at //!DESC or //!PARAM, or at //!HOOK after a body.
			const std::string d = line.substr(3);
			const size_t sp = d.find(' ');
			const std::string key = d.substr(0, sp);
			const std::string arg = sp == std::string::npos ? std::string() : Trim(d.substr(sp + 1));
			const bool newPass = key == "DESC" || (key == "HOOK" && (block != Block::Pass || passBodyStarted || !pass->hook.empty()));
			if (key == "PARAM")
			{
				pkg.params.push_back(NetParam());
				param = &pkg.params.back();
				param->name = arg;
				block = Block::Param;
				paramValueSeen = false;
				continue;
			}
			if (newPass)
			{
				pkg.passes.push_back(NetPass());
				pass = &pkg.passes.back();
				block = Block::Pass;
				passBodyStarted = false;
			}
			if (block == Block::Param)
			{
				if (key == "TYPE") param->isInt = Contains(arg, "int");
				else if (key == "MINIMUM") param->minimum = std::strtod(arg.c_str(), nullptr);
				else if (key == "MAXIMUM") param->maximum = std::strtod(arg.c_str(), nullptr);
				else if (key == "DESC") {}
				else return fail(ln, "unknown PARAM directive //!" + key);
				continue;
			}
			if (block != Block::Pass)
				return fail(ln, "directive //!" + key + " outside a pass");
			if (passBodyStarted)
				return fail(ln, "directive //!" + key + " inside a pass body");
			if (key == "DESC") pass->desc = arg;
			else if (key == "HOOK") pass->hook = arg;
			else if (key == "BIND") pass->binds.push_back(arg);
			else if (key == "SAVE") pass->save = arg;
			else if (key == "COMPONENTS") pass->components = std::atoi(arg.c_str());
			else if (key == "WIDTH") pass->width = arg;
			else if (key == "HEIGHT") pass->height = arg;
			else if (key == "COMPUTE")
			{
				std::istringstream a(arg);
				a >> pass->computeX >> pass->computeY;
				if (pass->computeX <= 0 || pass->computeY <= 0) return fail(ln, "bad //!COMPUTE size");
			}
			else return fail(ln, "unsupported directive //!" + key + " (the runner implements the exporter's subset)");
		}

		if (pkg.passes.empty())
			return fail(lines.size(), "no passes");

		// Derived: textures, binding contract, role, fingerprint.
		std::set<std::string> saved;
		std::vector<std::string> inputs;
		auto addInput = [&](const std::string& n) { if (std::find(inputs.begin(), inputs.end(), n) == inputs.end()) inputs.push_back(n); };
		static const std::regex texSample("[A-Za-z_][A-Za-z0-9_]*_tex\\(");
		for (size_t i = 0; i < pkg.passes.size(); ++i)
		{
			const NetPass& p = pkg.passes[i];
			if (p.hook.empty()) return fail(lines.size(), "pass '" + p.desc + "' has no //!HOOK");
			if (p.components < 1 || p.components > 4) return fail(lines.size(), "pass '" + p.desc + "' has bad COMPONENTS");
			if (p.width.empty() != p.height.empty()) return fail(lines.size(), "pass '" + p.desc + "' sets only one of WIDTH/HEIGHT");
			for (const std::string& b : p.binds)
				if (!saved.count(b))
					addInput(b);
			if (!p.save.empty())
			{
				if (saved.insert(p.save).second)
					pkg.savedTextures.push_back(p.save);
			}
			if (std::regex_search(p.body, texSample)) pkg.contract.usesLinearSampling = true;
			if (p.IsCompute()) pkg.contract.tileGated = true;
			pkg.weightLiterals += CountFloatLiterals(p.body);
		}
		const NetPass& last = pkg.passes.back();
		if (!last.save.empty())
			return fail(lines.size(), "the last pass saves '" + last.save + "' instead of writing the hooked stage (no net output)");
		pkg.contract.outputStage = last.hook;
		pkg.contract.outputComponents = last.components;
		pkg.contract.inputs = inputs;
		for (const NetParam& p : pkg.params) pkg.contract.labels.push_back(p.name);
		pkg.contract.neighbours = std::find(inputs.begin(), inputs.end(), "VPNN_PREV") != inputs.end();
		if (saved.count("VPNNGATE")) pkg.contract.tileGated = true;
		pkg.contract.site = (StartsWith(last.hook, "SCALER_")) ? RunSite::PostKernelHook : RunSite::FrontEnd;

		NetFingerprint& fp = pkg.fingerprint;
		if (!fpHeaderId.empty()) fp.fields["fingerprint"] = fpHeaderId;
		DeriveFingerprint(fp);
		// The id must recompute from the header's own fields (the export json, when merged, re-checks
		// it over its own, complete fields).
		{
			Json obj;
			obj.kind = Json::Kind::Object;
			for (const auto& kv : fp.fields)
			{
				Json v;
				v.kind = Json::Kind::String;
				v.text = kv.second;
				obj.fields[kv.first] = v;
			}
			fp.idVerified = !fp.id.empty() && FingerprintIntact(obj);
		}
		pkg.role = RoleFromTitle(pkg.title, fp.Field("input_stage"));

		r.ok = true;
		return r;
	}

	bool MergeExportFingerprint(NetPackage& package, const Json& exportJson, std::string* error)
	{
		const Json& fj = exportJson["fingerprint"];
		if (!fj.IsObject())
		{
			if (error) *error = package.name + ": export json has no fingerprint object";
			return false;
		}
		NetFingerprint& fp = package.fingerprint;
		for (const auto& kv : fj.fields)
		{
			const std::string value = Compact(kv.second);
			const auto it = fp.fields.find(kv.first);
			// The header stamps scalars on one line; a nested json value has no header twin.
			if (it != fp.fields.end() && kv.second.kind != Json::Kind::Object && kv.second.kind != Json::Kind::Array &&
				Lower(Trim(it->second)) != Lower(Trim(value)))   // Python prints True, json true
			{
				if (error) *error = package.name + ": fingerprint field '" + kv.first + "' differs between the shader header and the export json";
				return false;
			}
			fp.fields[kv.first] = value;
		}
		fp.idVerified = FingerprintIntact(fj);
		const Json& f16 = exportJson["fp16_check"];
		fp.fp16Allowed = f16.IsObject() && f16["fp16_within_1_lsb"].kind == Json::Kind::Bool && f16["fp16_within_1_lsb"].boolean;
		DeriveFingerprint(fp);
		if (package.role == NetRole::Unknown)
			package.role = RoleFromTitle(package.title, fp.Field("input_stage"));
		return true;
	}

	bool MergeStampSidecar(NetPackage& package, const Json& sc, const std::string& shaderSha256, std::string* error)
	{
		auto fail = [&](const std::string& why) { if (error) *error = package.name + ": stamp sidecar: " + why; return false; };
		if (sc["schema"].text != "vp-net-stamp/1") return fail("unknown schema '" + sc["schema"].text + "'");
		NetFingerprint& fp = package.fingerprint;
		if (sc["export_fingerprint"].text != fp.id) return fail("made for fingerprint " + sc["export_fingerprint"].text + ", the export is " + fp.id);
		bool shaderListed = false;
		for (const auto& kv : sc["shaders"].fields)
			if (kv.second.text == shaderSha256) shaderListed = true;
		if (!shaderListed) return fail("does not list this shader's sha256 (a different export)");
		const Json& f = sc["fields"];
		if (!f.IsObject()) return fail("no fields");
		for (const auto& kv : f.fields)
		{
			const std::string value = Compact(kv.second);
			if (fp.Has(kv.first))
			{
				if (Trim(fp.Field(kv.first)) != Trim(value))
					return fail("field '" + kv.first + "' disagrees with the export");
				continue;
			}
			fp.fields[kv.first] = value;
			fp.stamped.push_back(kv.first);
		}
		DeriveFingerprint(fp);
		return true;
	}

	bool LoadExport(const std::string& exportJsonPath, const std::string& shaderDir,
		std::vector<NetPackage>& out, std::string* error, const std::vector<std::string>& sidecarDirs)
	{
		std::string text;
		if (!ReadTextFile(exportJsonPath, text))
		{
			if (error) *error = "cannot read " + exportJsonPath;
			return false;
		}
		Json j;
		std::string jerr;
		if (!ParseJson(text, j, &jerr))
		{
			if (error) *error = exportJsonPath + ": " + jerr;
			return false;
		}
		std::string dir = shaderDir;
		if (dir.empty())
		{
			const size_t slash = exportJsonPath.find_last_of("/\\");
			dir = (slash == std::string::npos ? std::string(".") : exportJsonPath.substr(0, slash)) + "/shaders";
		}
		// The stamp sidecar, if any.
		const size_t slash2 = exportJsonPath.find_last_of("/\\");
		std::string jsonStem = slash2 == std::string::npos ? exportJsonPath : exportJsonPath.substr(slash2 + 1);
		if (jsonStem.size() > 5 && jsonStem.compare(jsonStem.size() - 5, 5, ".json") == 0) jsonStem = jsonStem.substr(0, jsonStem.size() - 5);
		std::vector<std::string> sidecarCandidates;
		for (const std::string& d : sidecarDirs) sidecarCandidates.push_back(d + "/" + jsonStem + ".vpstamp.json");
		sidecarCandidates.push_back((slash2 == std::string::npos ? std::string(".") : exportJsonPath.substr(0, slash2)) + "/" + jsonStem + ".vpstamp.json");
		Json sidecar;
		bool haveSidecar = false;
		for (const std::string& c : sidecarCandidates)
		{
			std::string st;
			if (!ReadTextFile(c, st)) continue;
			std::string serr;
			if (!ParseJson(st, sidecar, &serr))
			{
				if (error) *error = c + ": " + serr;
				return false;
			}
			haveSidecar = true;
			break;
		}
		const Json& shaders = j["shaders"];
		if (shaders.kind != Json::Kind::Array || shaders.items.empty())
		{
			if (error) *error = exportJsonPath + ": no shaders listed";
			return false;
		}
		for (const Json& s : shaders.items)
		{
			const std::string file = dir + "/" + s.text;
			std::string body;
			if (!ReadTextFile(file, body))
			{
				if (error) *error = "cannot read " + file;
				return false;
			}
			std::string stem = s.text;
			const size_t dot = stem.rfind('.');
			if (dot != std::string::npos) stem = stem.substr(0, dot);
			ParseResult pr = ParseShaderText(body, stem);
			if (!pr.ok)
			{
				if (error) *error = pr.error;
				return false;
			}
			if (!MergeExportFingerprint(pr.package, j, error))
				return false;
			if (haveSidecar && !MergeStampSidecar(pr.package, sidecar, Sha256Hex(body), error))
				return false;
			out.push_back(std::move(pr.package));
		}
		return true;
	}
}
