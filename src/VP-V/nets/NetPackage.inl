#pragma once

// Template part of NetPackage.h (the RPN size evaluator). Mirrors libplacebo's eval_shexpr
// (src/shaders/custom_mpv.c): float stack; + - * / % > < = and unary !; NAME.w / NAME.h (also
// .width / .height); the result rounded with roundf (half away from zero).

#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace VpNets
{
	template <typename Lookup>
	bool EvalSizeExpr(const std::string& expr, Lookup lookup, int& result, std::string* error)
	{
		std::istringstream in(expr);
		std::string tok;
		std::vector<float> st;
		auto fail = [&](const std::string& why) { if (error) *error = "size expression '" + expr + "': " + why; return false; };
		while (in >> tok)
		{
			if (tok.size() == 1 && std::string("+-*/%><=").find(tok[0]) != std::string::npos)
			{
				if (st.size() < 2) return fail("stack underflow");
				const float b = st.back(); st.pop_back();
				const float a = st.back(); st.pop_back();
				float r = 0.0f;
				switch (tok[0])
				{
				case '+': r = a + b; break;
				case '-': r = a - b; break;
				case '*': r = a * b; break;
				case '/': r = a / b; break;
				case '%': r = std::fmod(a, b); break;
				case '>': r = a > b ? 1.0f : 0.0f; break;
				case '<': r = a < b ? 1.0f : 0.0f; break;
				default: r = std::fabs(a - b) <= 1e-6f * std::fmax(a, b) ? 1.0f : 0.0f; break;
				}
				if (!std::isfinite(r)) return fail("illegal operation");
				st.push_back(r);
				continue;
			}
			if (tok == "!")
			{
				if (st.empty()) return fail("stack underflow");
				st.back() = st.back() == 0.0f ? 1.0f : 0.0f;
				continue;
			}
			const size_t dot = tok.rfind('.');
			char* end = nullptr;
			const float num = std::strtof(tok.c_str(), &end);
			if (end && *end == '\0')
			{
				st.push_back(num);
				continue;
			}
			if (dot == std::string::npos || dot == 0)
				return fail("unknown token '" + tok + "'");
			const std::string name = tok.substr(0, dot), comp = tok.substr(dot + 1);
			char wh = 0;
			if (comp == "w" || comp == "width") wh = 'w';
			else if (comp == "h" || comp == "height") wh = 'h';
			else return fail("unknown component '" + comp + "'");
			int v = 0;
			if (!lookup(name, wh, v)) return fail("unknown texture '" + name + "'");
			st.push_back(static_cast<float>(v));
		}
		if (st.size() != 1) return fail("malformed stack");
		result = static_cast<int>(std::round(st[0]));   // roundf: half away from zero
		return true;
	}
}
