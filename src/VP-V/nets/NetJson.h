#pragma once

#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

// A minimal JSON reader for the TrainKit's export_<net>.json files and the test reference cases.
//
// Why its own: VP has no JSON library, and the export files are small, machine-written and
// trusted only as data (strings, numbers, objects, arrays, true/false/null). Nothing here is
// used for writing. Header-only, C++14, no Windows.
namespace VpNets
{
	struct Json
	{
		enum class Kind { Null, Bool, Number, String, Array, Object };
		Kind kind = Kind::Null;
		bool boolean = false;
		double number = 0.0;
		std::string text;                     // String; for Number, the literal as written
		std::vector<Json> items;              // Array
		std::map<std::string, Json> fields;   // Object

		bool IsObject() const { return kind == Kind::Object; }
		bool Has(const std::string& key) const { return kind == Kind::Object && fields.count(key) != 0; }
		const Json& operator[](const std::string& key) const
		{
			static const Json kNull;
			if (kind != Kind::Object) return kNull;
			const auto it = fields.find(key);
			return it == fields.end() ? kNull : it->second;
		}
		// A scalar as text: strings as is, numbers as written, bools "true"/"false".
		std::string AsText() const
		{
			switch (kind)
			{
			case Kind::String: case Kind::Number: return text;
			case Kind::Bool: return boolean ? "true" : "false";
			default: return std::string();
			}
		}
	};

	namespace JsonDetail
	{
		struct Reader
		{
			const std::string& s;
			size_t i = 0;
			std::string error;
			explicit Reader(const std::string& src) : s(src) {}

			void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
			bool Fail(const char* what) { if (error.empty()) error = std::string(what) + " at offset " + std::to_string(i); return false; }

			static void PutUtf8(std::string& out, unsigned cp)
			{
				if (cp < 0x80) out += static_cast<char>(cp);
				else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
				else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
			}

			bool String(std::string& out)
			{
				if (i >= s.size() || s[i] != '"') return Fail("expected a string");
				++i;
				while (i < s.size() && s[i] != '"')
				{
					char c = s[i++];
					if (c != '\\') { out += c; continue; }
					if (i >= s.size()) return Fail("bad escape");
					c = s[i++];
					switch (c)
					{
					case 'n': out += '\n'; break;
					case 't': out += '\t'; break;
					case 'r': out += '\r'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'u':
					{
						if (i + 4 > s.size()) return Fail("bad \\u escape");
						const unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
						i += 4;
						PutUtf8(out, cp);
						break;
					}
					default: out += c; break;   // \" \\ \/
					}
				}
				if (i >= s.size()) return Fail("unterminated string");
				++i;
				return true;
			}

			bool Value(Json& v, int depth)
			{
				if (depth > 64) return Fail("nesting too deep");
				Ws();
				if (i >= s.size()) return Fail("unexpected end");
				const char c = s[i];
				if (c == '{')
				{
					v.kind = Json::Kind::Object;
					++i; Ws();
					if (i < s.size() && s[i] == '}') { ++i; return true; }
					for (;;)
					{
						Ws();
						std::string key;
						if (!String(key)) return false;
						Ws();
						if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
						++i;
						Json child;
						if (!Value(child, depth + 1)) return false;
						v.fields[key] = std::move(child);
						Ws();
						if (i < s.size() && s[i] == ',') { ++i; continue; }
						if (i < s.size() && s[i] == '}') { ++i; return true; }
						return Fail("expected ',' or '}'");
					}
				}
				if (c == '[')
				{
					v.kind = Json::Kind::Array;
					++i; Ws();
					if (i < s.size() && s[i] == ']') { ++i; return true; }
					for (;;)
					{
						Json child;
						if (!Value(child, depth + 1)) return false;
						v.items.push_back(std::move(child));
						Ws();
						if (i < s.size() && s[i] == ',') { ++i; continue; }
						if (i < s.size() && s[i] == ']') { ++i; return true; }
						return Fail("expected ',' or ']'");
					}
				}
				if (c == '"') { v.kind = Json::Kind::String; return String(v.text); }
				if (s.compare(i, 4, "true") == 0) { v.kind = Json::Kind::Bool; v.boolean = true; i += 4; return true; }
				if (s.compare(i, 5, "false") == 0) { v.kind = Json::Kind::Bool; i += 5; return true; }
				if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
				if (s.compare(i, 9, "-Infinity") == 0) { v.kind = Json::Kind::Number; v.text = "-Infinity"; v.number = -std::numeric_limits<double>::infinity(); i += 9; return true; }
				if (c == '-' || (c >= '0' && c <= '9'))
				{
					const size_t start = i;
					++i;
					while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
						++i;
					v.kind = Json::Kind::Number;
					v.text = s.substr(start, i - start);
					v.number = std::strtod(v.text.c_str(), nullptr);
					return true;
				}
				// Python's json.dump writes NaN/Infinity for non-finite floats.
				if (s.compare(i, 3, "NaN") == 0) { v.kind = Json::Kind::Number; v.text = "NaN"; v.number = std::strtod("nan", nullptr); i += 3; return true; }
				if (s.compare(i, 8, "Infinity") == 0) { v.kind = Json::Kind::Number; v.text = "Infinity"; v.number = std::numeric_limits<double>::infinity(); i += 8; return true; }
				if (s.compare(i, 9, "-Infinity") == 0) { v.kind = Json::Kind::Number; v.text = "-Infinity"; v.number = -std::numeric_limits<double>::infinity(); i += 9; return true; }
				return Fail("unexpected character");
			}
		};
	}

	// Parses `text`; on failure returns false and sets *error.
	inline bool ParseJson(const std::string& text, Json& out, std::string* error)
	{
		JsonDetail::Reader r(text);
		out = Json();
		if (!r.Value(out, 0))
		{
			if (error) *error = r.error;
			return false;
		}
		r.Ws();
		if (r.i != text.size())
		{
			if (error) *error = "trailing characters at offset " + std::to_string(r.i);
			return false;
		}
		return true;
	}
}
