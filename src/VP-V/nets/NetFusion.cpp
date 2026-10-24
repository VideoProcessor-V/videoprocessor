#include "NetFusion.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace VpNets
{
	namespace
	{
		struct ParsedPass
		{
			bool ok = false;
			std::string biasText;
			std::array<float, 4> bias{};
			struct Tap { std::string mat; std::string tex; int dx = 0, dy = 0; };
			std::vector<Tap> taps;
			bool relu = false;
			std::string skipTex;
			std::string gate;
			int gateTile = 0;
		};

		std::vector<float> Numbers(const std::string& literal)
		{
			std::vector<float> v;
			const size_t open = literal.find('(');
			std::string inner = literal.substr(open + 1, literal.rfind(')') - open - 1);
			std::stringstream ss(inner);
			std::string item;
			while (std::getline(ss, item, ','))
				v.push_back(static_cast<float>(std::strtod(item.c_str(), nullptr)));
			return v;
		}

		ParsedPass ParseConvPass(const NetPass& p)
		{
			static const std::regex kTap(R"(^\s*r \+= (mat4\([^)]*\)) \* \(texelFetch\((\w+)_raw, mi\((?:ivec2\(\w+_pos \* \w+_size\)|P) \+ ivec2\((-?\d+), (-?\d+)\), ivec2\(\w+_size\)\), 0\) \* \w+_mul\);\s*$)");
			static const std::regex kBias(R"(^\s*vec4 r = (vec4\([^;]*\));\s*$)");
			static const std::regex kRet(R"(^\s*(?:return (.*)|imageStore\(out_image, P, (.*)\));\s*$)");
			static const std::regex kSkip(R"(^\(texelFetch\((\w+)_raw, mi\((?:ivec2\(\w+_pos \* \w+_size\)|P) \+ ivec2\(0, 0\), ivec2\(\w+_size\)\), 0\) \* \w+_mul\) \+ max\(r, vec4\(0\.0\)\)$)");
			static const std::regex kGate(R"(^\s*if \(texelFetch\((\w+)_raw, P / (\d+), 0\)\.x \* \w+_mul <= 0\.0\) \{ imageStore\(out_image, P, vec4\(0\.0\)\); return; \}\s*$)");
			ParsedPass r;
			std::istringstream in(p.body);
			std::string line;
			bool sawRet = false, sawBias = false;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				std::smatch m;
				const std::string t = line;
				if (t.find_first_not_of(" \t") == std::string::npos) continue;
				if (t.compare(0, 9, "ivec2 mi(") == 0 || t == "vec4 hook() {" || t == "void hook() {" || t == "}") continue;
				if (t == "    ivec2 P = ivec2(gl_GlobalInvocationID.xy);" || t == "    if (any(greaterThanEqual(P, imageSize(out_image)))) return;") continue;
				if (std::regex_match(t, m, kGate)) { r.gate = m[1]; r.gateTile = std::atoi(m[2].str().c_str()); continue; }
				if (std::regex_match(t, m, kBias))
				{
					r.biasText = m[1];
					const std::vector<float> b = Numbers(r.biasText);
					if (b.size() != 4) return ParsedPass();
					for (int k = 0; k < 4; ++k) r.bias[k] = b[k];
					sawBias = true;
					continue;
				}
				if (std::regex_match(t, m, kTap))
				{
					ParsedPass::Tap tap;
					tap.mat = m[1];
					tap.tex = m[2];
					tap.dx = std::atoi(m[3].str().c_str());
					tap.dy = std::atoi(m[4].str().c_str());
					if (tap.dx < -1 || tap.dx > 1 || tap.dy < -1 || tap.dy > 1 || Numbers(tap.mat).size() != 16) return ParsedPass();
					r.taps.push_back(tap);
					continue;
				}
				if (std::regex_match(t, m, kRet))
				{
					const std::string act = m[1].matched ? m[1].str() : m[2].str();
					std::smatch s;
					if (act == "max(r, vec4(0.0))") r.relu = true;
					else if (act == "r") r.relu = false;
					else if (std::regex_match(act, s, kSkip)) { r.relu = true; r.skipTex = s[1]; }
					else return ParsedPass();
					sawRet = true;
					continue;
				}
				return ParsedPass();   // anything else (conditioning bias, features, pools): not a plain conv group
			}
			r.ok = sawBias && sawRet;
			return r;
		}

		bool SplitSave(const std::string& save, std::string& prefix, int& index)
		{
			size_t i = save.size();
			while (i > 0 && std::isdigit(static_cast<unsigned char>(save[i - 1]))) --i;
			if (i == save.size() || i == 0) return false;
			prefix = save.substr(0, i);
			index = std::atoi(save.c_str() + i);
			return true;
		}

		std::string F(double v)
		{
			char b[48];
			std::snprintf(b, sizeof b, "%.9g", static_cast<float>(v));
			std::string s = b;
			if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
			return s;
		}
	}

	std::vector<ConvLayer> ExtractConvLayers(const NetPackage& pkg)
	{
		std::vector<ConvLayer> layers;
		ConvLayer cur;
		std::string curPrefix;
		auto flush = [&]()
		{
			if (!cur.passes.empty()) layers.push_back(cur);
			cur = ConvLayer();
			curPrefix.clear();
		};
		for (size_t i = 0; i < pkg.passes.size(); ++i)
		{
			const NetPass& p = pkg.passes[i];
			std::string prefix;
			int index = -1;
			const ParsedPass pp = p.save.empty() || !SplitSave(p.save, prefix, index) ? ParsedPass() : ParseConvPass(p);
			if (!pp.ok || p.components != 4)
			{
				flush();
				continue;
			}
			std::vector<std::string> inputs;
			for (const std::string& b : p.binds) if (b != pp.gate) inputs.push_back(b);
			const bool continues = !cur.passes.empty() && prefix == curPrefix && index == static_cast<int>(cur.outputs.size()) &&
				inputs == cur.inputs && pp.relu == cur.relu && (!pp.skipTex.empty()) == cur.skip && pp.gate == cur.gate &&
				p.width == cur.width && p.height == cur.height && p.hook == cur.hook;
			if (!continues)
			{
				flush();
				if (index != 0) continue;
				cur.hook = p.hook;
				cur.width = p.width;
				cur.height = p.height;
				cur.inputs = inputs;
				cur.relu = pp.relu;
				cur.skip = !pp.skipTex.empty();
				cur.gate = pp.gate;
				cur.gateTile = pp.gateTile;
				curPrefix = prefix;
			}
			const size_t o = cur.outputs.size();
			if (cur.skip && (o >= cur.inputs.size() || pp.skipTex != cur.inputs[o]))
			{
				flush();
				continue;
			}
			cur.outputs.push_back(p.save);
			cur.bias.push_back(pp.bias);
			cur.biasText.push_back(pp.biasText);
			cur.matText.resize((o + 1) * cur.inputs.size() * 9);
			cur.mat.resize((o + 1) * cur.inputs.size() * 9, std::array<float, 16>{});
			bool good = true;
			for (const ParsedPass::Tap& t : pp.taps)
			{
				size_t g = 0;
				while (g < cur.inputs.size() && cur.inputs[g] != t.tex) ++g;
				if (g == cur.inputs.size()) { good = false; break; }
				const size_t idx = cur.Index(o, g, (t.dy + 1) * 3 + (t.dx + 1));
				if (!cur.matText[idx].empty()) { good = false; break; }
				cur.matText[idx] = t.mat;
				const std::vector<float> v = Numbers(t.mat);
				for (int k = 0; k < 16; ++k) cur.mat[idx][k] = v[k];
			}
			if (!good)
			{
				cur.outputs.pop_back();
				flush();
				continue;
			}
			cur.passes.push_back(i);
		}
		flush();
		return layers;
	}

	FusedShader FusedLayerGlsl(const ConvLayer& L, FusedKind kind, bool fp16Shared)
	{
		FusedShader fs;
		const size_t G = L.inputs.size(), O = L.outputs.size();
		const size_t texel = fp16Shared ? 8 : 16;
		int tx = 16, ty = 16;
		while (G * static_cast<size_t>((tx + 2) * (ty + 2)) * texel > 32768 && ty > 4)
		{
			if (ty >= tx) ty /= 2; else tx /= 2;
		}
		fs.tileX = tx;
		fs.tileY = ty;
		fs.groupX = kind == FusedKind::Winograd ? tx / 2 : tx;
		fs.groupY = kind == FusedKind::Winograd ? ty / 2 : ty;
		const int W = tx + 2, H = ty + 2;
		fs.sharedBytes = G * static_cast<size_t>(W * H) * texel;
		std::ostringstream s;
		s << "ivec2 mi(ivec2 p, ivec2 s) { p = abs(p); return (s - 1) - abs((s - 1) - p); }\n";
		s << "#define NF_W " << W << "\n#define NF_TS " << W * H << "\n";
		if (fp16Shared)
		{
			s << "shared uvec2 nf_t[" << G * W * H << "];\n";
			s << "void nf_st(int i, vec4 v) { nf_t[i] = uvec2(packHalf2x16(v.xy), packHalf2x16(v.zw)); }\n";
			s << "vec4 nf_ld(int i) { uvec2 u = nf_t[i]; return vec4(unpackHalf2x16(u.x), unpackHalf2x16(u.y)); }\n";
		}
		else
		{
			s << "shared vec4 nf_t[" << G * W * H << "];\n";
			s << "void nf_st(int i, vec4 v) { nf_t[i] = v; }\nvec4 nf_ld(int i) { return nf_t[i]; }\n";
		}
		s << "void hook() {\n";
		s << "    ivec2 base = ivec2(gl_WorkGroupID.xy) * ivec2(" << tx << ", " << ty << ") - ivec2(1);\n";
		s << "    for (int i = int(gl_LocalInvocationIndex); i < NF_TS; i += " << fs.groupX * fs.groupY << ") {\n";
		s << "        ivec2 p = mi(base + ivec2(i % NF_W, i / NF_W), nf_size);\n";
		for (size_t g = 0; g < G; ++g)
			s << "        nf_st(" << g << " * NF_TS + i, texelFetch(NFI" << g << "_raw, p, 0));\n";
		s << "    }\n    barrier();\n";

		auto act = [&](size_t o, const std::string& r, const std::string& centre) -> std::string
		{
			std::string a = L.relu ? "max(" + r + ", vec4(0.0))" : r;
			if (L.skip) a = "nf_ld(" + std::to_string(o) + " * NF_TS + " + centre + ") + " + a;
			return a;
		};
		auto store = [&](size_t o, const std::string& P, const std::string& value)
		{
			std::string v = value;
			if (!L.gate.empty())
				v = "(texelFetch(NFG_raw, (" + P + ") / " + std::to_string(L.gateTile) + ", 0).x > 0.0 ? " + value + " : vec4(0.0))";
			return "imageStore(NFO" + std::to_string(o) + ", " + P + ", " + v + ");";
		};

		if (kind == FusedKind::Direct)
		{
			s << "    ivec2 l = ivec2(gl_LocalInvocationID.xy);\n    ivec2 P = base + ivec2(1) + l;\n";
			s << "    if (P.x >= nf_size.x || P.y >= nf_size.y) return;\n";
			s << "    int c = (l.y + 1) * NF_W + (l.x + 1);\n";
			for (size_t o = 0; o < O; ++o) s << "    vec4 r" << o << " = " << L.biasText[o] << ";\n";
			for (size_t g = 0; g < G; ++g)
			{
				s << "    {\n";
				for (int k = 0; k < 9; ++k)
				{
					const int dy = k / 3 - 1, dx = k % 3 - 1;
					s << "        vec4 t" << k << " = nf_ld(" << g << " * NF_TS + c + (" << dy << ") * NF_W + (" << dx << "));\n";
				}
				for (size_t o = 0; o < O; ++o)
					for (int k = 0; k < 9; ++k)
						if (!L.matText[L.Index(o, g, k)].empty())
							s << "        r" << o << " += " << L.matText[L.Index(o, g, k)] << " * t" << k << ";\n";
				s << "    }\n";
			}
			for (size_t o = 0; o < O; ++o)
				s << "    " << store(o, "P", act(o, "r" + std::to_string(o), "c")) << "\n";
			s << "}\n";
			fs.glsl = s.str();
			return fs;
		}

		// Winograd F(2x2, 3x3). U = G g G^T per (output group, input group); literals are mat4s
		// over (input channel column, output channel row), one per transform position (a, b).
		static const double Gm[4][3] = { { 1, 0, 0 }, { 0.5, 0.5, 0.5 }, { 0.5, -0.5, 0.5 }, { 0, 0, 1 } };
		auto uLiteral = [&](size_t o, size_t g, int a, int b, bool& nonzero)
		{
			std::string lit = "mat4(";
			nonzero = false;
			for (int c = 0; c < 4; ++c)
				for (int r = 0; r < 4; ++r)
				{
					double u = 0.0;
					for (int dy = 0; dy < 3; ++dy)
						for (int dx = 0; dx < 3; ++dx)
							u += Gm[a][dy] * L.mat[L.Index(o, g, dy * 3 + dx)][c * 4 + r] * Gm[b][dx];
					if (u != 0.0) nonzero = true;
					lit += (c || r ? ", " : "") + F(u);
				}
			return lit + ")";
		};
		s << "    ivec2 l = ivec2(gl_LocalInvocationID.xy) * 2;\n    ivec2 P0 = base + ivec2(1) + l;\n";
		const size_t chunk = 2;   // output groups per pass over the inputs (register pressure)
		for (size_t o0 = 0; o0 < O; o0 += chunk)
		{
			const size_t o1 = std::min(O, o0 + chunk);
			s << "    {\n";
			for (size_t o = o0; o < o1; ++o)
				for (int k = 0; k < 16; ++k) s << "        vec4 m" << o << "_" << k << " = vec4(0.0);\n";
			for (size_t g = 0; g < G; ++g)
			{
				bool any = false;
				for (size_t o = o0; o < o1 && !any; ++o)
					for (int k = 0; k < 9 && !any; ++k) any = !L.matText[L.Index(o, g, k)].empty();
				if (!any) continue;
				s << "        {\n";
				for (int i = 0; i < 4; ++i)
					for (int j = 0; j < 4; ++j)
						s << "            vec4 d" << i << j << " = nf_ld(" << g << " * NF_TS + (l.y + " << i << ") * NF_W + l.x + " << j << ");\n";
				// t = B^T d (rows), V = t B (columns)
				for (int j = 0; j < 4; ++j)
				{
					s << "            vec4 t0" << j << " = d0" << j << " - d2" << j << ", t1" << j << " = d1" << j << " + d2" << j
						<< ", t2" << j << " = d2" << j << " - d1" << j << ", t3" << j << " = d1" << j << " - d3" << j << ";\n";
				}
				for (int i = 0; i < 4; ++i)
				{
					s << "            vec4 v" << i << "0 = t" << i << "0 - t" << i << "2, v" << i << "1 = t" << i << "1 + t" << i << "2, v"
						<< i << "2 = t" << i << "2 - t" << i << "1, v" << i << "3 = t" << i << "1 - t" << i << "3;\n";
				}
				for (size_t o = o0; o < o1; ++o)
					for (int a = 0; a < 4; ++a)
						for (int b = 0; b < 4; ++b)
						{
							bool nz = false;
							const std::string lit = uLiteral(o, g, a, b, nz);
							if (nz) s << "            m" << o << "_" << a * 4 + b << " += " << lit << " * v" << a << b << ";\n";
						}
				s << "        }\n";
			}
			// Y = A^T M A, A^T = [[1, 1, 1, 0], [0, 1, -1, -1]]
			for (size_t o = o0; o < o1; ++o)
			{
				const std::string m = "m" + std::to_string(o) + "_";
				for (int j = 0; j < 4; ++j)
					s << "        vec4 q0" << o << "_" << j << " = " << m << j << " + " << m << 4 + j << " + " << m << 8 + j << ", q1" << o << "_" << j
						<< " = " << m << 4 + j << " - " << m << 8 + j << " - " << m << 12 + j << ";\n";
				for (int a = 0; a < 2; ++a)
				{
					const std::string q = "q" + std::to_string(a) + std::to_string(o) + "_";
					s << "        vec4 y" << o << "_" << a << "0 = " << q << "0 + " << q << "1 + " << q << "2 + " << L.biasText[o] << ";\n";
					s << "        vec4 y" << o << "_" << a << "1 = " << q << "1 - " << q << "2 - " << q << "3 + " << L.biasText[o] << ";\n";
				}
				for (int a = 0; a < 2; ++a)
					for (int b = 0; b < 2; ++b)
					{
						const std::string P = "P0 + ivec2(" + std::to_string(b) + ", " + std::to_string(a) + ")";
						const std::string centre = "(l.y + " + std::to_string(a + 1) + ") * NF_W + l.x + " + std::to_string(b + 1);
						s << "        if ((P0.x + " << b << ") < nf_size.x && (P0.y + " << a << ") < nf_size.y) "
							<< store(o, P, act(o, "y" + std::to_string(o) + "_" + std::to_string(a) + std::to_string(b), centre)) << "\n";
					}
			}
			s << "    }\n";
		}
		s << "}\n";
		fs.glsl = s.str();
		return fs;
	}
}
