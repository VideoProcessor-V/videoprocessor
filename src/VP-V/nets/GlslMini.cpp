#include "GlslMini.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

// The GlslMini interpreter (see GlslMini.h for why and what is supported).
// Layout: tokenizer -> recursive-descent parser building node/statement pools with locals
// resolved to frame slots at parse time -> a tree-walking evaluator.
namespace VpNets
{
	namespace Glsl
	{
		namespace
		{
			// ------------------------------------------------------------------ types
			struct Type
			{
				Base base = Base::Void;
				uint8_t n = 0;       // 1..4, 16 = mat4
				int arrayLen = 0;    // > 0 for arrays
			};

			bool TypeFromName(const std::string& s, Type& t)
			{
				static const std::map<std::string, Type> kTypes = {
					{ "void", { Base::Void, 0, 0 } },
					{ "float", { Base::Float, 1, 0 } }, { "vec2", { Base::Float, 2, 0 } }, { "vec3", { Base::Float, 3, 0 } }, { "vec4", { Base::Float, 4, 0 } },
					{ "int", { Base::Int, 1, 0 } }, { "ivec2", { Base::Int, 2, 0 } }, { "ivec3", { Base::Int, 3, 0 } }, { "ivec4", { Base::Int, 4, 0 } },
					{ "uint", { Base::Int, 1, 0 } }, { "uvec2", { Base::Int, 2, 0 } }, { "uvec3", { Base::Int, 3, 0 } }, { "uvec4", { Base::Int, 4, 0 } },
					{ "bool", { Base::Bool, 1, 0 } }, { "bvec2", { Base::Bool, 2, 0 } }, { "bvec3", { Base::Bool, 3, 0 } }, { "bvec4", { Base::Bool, 4, 0 } },
					{ "mat4", { Base::Float, 16, 0 } },
				};
				const auto it = kTypes.find(s);
				if (it == kTypes.end()) return false;
				t = it->second;
				return true;
			}

			// ------------------------------------------------------------------ tokens
			enum class Tok { End, Ident, Float, Int, Op };
			struct Token
			{
				Tok kind = Tok::End;
				std::string text;
				double number = 0.0;
				int line = 1;
			};

			bool Tokenize(const std::string& s, std::vector<Token>& out, std::string* error)
			{
				static const char* kOps3[] = { "<<=", ">>=" };
				static const char* kOps2[] = { "++", "--", "+=", "-=", "*=", "/=", "%=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>" };
				int line = 1;
				size_t i = 0;
				while (i < s.size())
				{
					const char c = s[i];
					if (c == '\n') { ++line; ++i; continue; }
					if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
					if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
					{
						while (i < s.size() && s[i] != '\n') ++i;
						continue;
					}
					if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
					{
						i += 2;
						while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') ++line; ++i; }
						i += 2;
						continue;
					}
					if (c == '#')
					{
						if (error) *error = "line " + std::to_string(line) + ": preprocessor directives are not supported";
						return false;
					}
					Token t;
					t.line = line;
					if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
					{
						const size_t a = i;
						while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
						t.kind = Tok::Ident;
						t.text = s.substr(a, i - a);
						out.push_back(t);
						continue;
					}
					if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))))
					{
						const size_t a = i;
						bool isFloat = false;
						while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
						if (i < s.size() && s[i] == '.') { isFloat = true; ++i; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i; }
						if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
						{
							isFloat = true;
							++i;
							if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
							while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
						}
						t.text = s.substr(a, i - a);
						if (i < s.size() && (s[i] == 'f' || s[i] == 'F')) { isFloat = true; ++i; }
						else if (i < s.size() && (s[i] == 'u' || s[i] == 'U')) ++i;
						t.kind = isFloat ? Tok::Float : Tok::Int;
						t.number = std::strtod(t.text.c_str(), nullptr);
						out.push_back(t);
						continue;
					}
					t.kind = Tok::Op;
					bool matched = false;
					for (const char* op : kOps3)
						if (s.compare(i, 3, op) == 0) { t.text = op; i += 3; matched = true; break; }
					if (!matched)
						for (const char* op : kOps2)
							if (s.compare(i, 2, op) == 0) { t.text = op; i += 2; matched = true; break; }
					if (!matched)
					{
						if (std::strchr("+-*/%=<>!&|^?:;,.(){}[]~", c) == nullptr)
						{
							if (error) *error = "line " + std::to_string(line) + ": unexpected character '" + std::string(1, c) + "'";
							return false;
						}
						t.text = std::string(1, c);
						++i;
					}
					out.push_back(t);
				}
				Token end;
				end.line = line;
				out.push_back(end);
				return true;
			}

			// ------------------------------------------------------------------ AST
			enum class NK : uint8_t
			{
				Lit, Local, Global, Ext, Unary, Binary, LogicAnd, LogicOr, Ternary, Assign,
				PreInc, PostInc, Call, Builtin, Construct, ArrayConstruct, Swizzle, Index, TexSample, TexOff, Comma,
			};

			enum class BI : uint8_t
			{
				Abs, Sign, Floor, Ceil, Fract, Round, RoundEven, Trunc, Min, Max, Clamp, Mix, Step, Smoothstep,
				Sqrt, InverseSqrt, Exp, Exp2, Log, Log2, Pow, Sin, Cos, Tan, Tanh, Atan, Mod,
				Dot, Length, Distance, Normalize, Any, All, Not,
				GreaterThan, GreaterThanEqual, LessThan, LessThanEqual, Equal, NotEqual,
				TexelFetch, Texture, ImageSize, ImageStore, IsNan, IsInf, Fma,
			};

			struct Node
			{
				NK kind = NK::Lit;
				std::string op;            // operator text (Unary/Binary/Assign/Inc)
				int slot = -1;             // Local/Global slot, Ext index, Call function, Builtin id, TexSample binding ext
				int slot2 = -1;            // TexOff: the Pt extern; Assign: the target's declared type index
				int mulExt = -1;           // TexSample/TexOff: the NAME_mul extern (mpv's NAME_tex includes it)
				Type type;                 // Construct/ArrayConstruct target
				uint8_t swz[4] = {};       // Swizzle indices
				uint8_t swzN = 0;
				std::vector<int> kids;
				Value lit;
				int line = 0;
			};

			enum class SK : uint8_t { Expr, Decl, If, For, While, Return, Block, Break, Continue, Empty };

			struct Stmt
			{
				SK kind = SK::Empty;
				int expr = -1;             // Expr/Return value/If cond/For cond
				int expr2 = -1;            // For step
				int init = -1;             // For init statement
				int body = -1, elseBody = -1;
				std::vector<int> stmts;    // Block
				// Decl: (slot, init node or -1)
				std::vector<std::pair<int, int>> decls;
			};

			struct Function
			{
				std::string name;
				Type ret;
				std::vector<int> paramSlots;
				int body = -1;
				int nSlots = 0;
				std::vector<Type> slotTypes;
				bool defined = false;
			};

			enum class Flow { Normal, Break, Continue, Return };

			// ------------------------------------------------------------------ value helpers
			Value Convert(const Value& v, const Type& t)
			{
				if (t.base == Base::Void || t.arrayLen > 0 || v.arr) return v;
				if (v.base == t.base && v.n == t.n) return v;
				Value r;
				r.base = t.base;
				r.n = t.n;
				if (t.n == 16)
				{
					if (v.n == 1) { for (int k = 0; k < 4; ++k) r.f[k * 5] = v.F(0); }
					else for (int k = 0; k < 16 && k < v.n; ++k) r.f[k] = v.f[k];
					return r;
				}
				for (int k = 0; k < t.n; ++k)
				{
					const int src = v.n == 1 ? 0 : k;
					if (src >= v.n) break;
					if (t.base == Base::Float) r.f[k] = v.F(src);
					else if (t.base == Base::Int) r.i[k] = v.base == Base::Float ? static_cast<int>(v.f[src]) : v.i[src];
					else r.i[k] = v.base == Base::Float ? (v.f[src] != 0.0f) : (v.i[src] != 0);
				}
				return r;
			}

			bool Truthy(const Value& v) { return v.base == Base::Float ? v.f[0] != 0.0f : v.i[0] != 0; }

			int SwizzleIndex(char c)
			{
				switch (c)
				{
				case 'x': case 'r': case 's': return 0;
				case 'y': case 'g': case 't': return 1;
				case 'z': case 'b': case 'p': return 2;
				case 'w': case 'a': case 'q': return 3;
				default: return -1;
				}
			}
		}

		// ====================================================================== Impl
		struct Program::Impl
		{
			std::vector<Node> nodes;
			std::vector<Stmt> stmts;
			std::vector<Function> functions;
			std::map<std::string, int> functionIndex;
			std::vector<Extern> externs;
			std::map<std::string, int> externIndex;
			std::vector<Value> globals;
			std::vector<Type> globalTypes;
			std::map<std::string, int> globalIndex;
			std::vector<std::pair<int, int>> globalInits;   // (slot, node)

			// parser state
			std::vector<Token> toks;
			size_t p = 0;
			std::string err;
			Function* cur = nullptr;
			std::vector<std::map<std::string, int>> scopes;

			// ---------------------------------------------------------------- parse helpers
			const Token& Peek(size_t k = 0) const { return toks[std::min(p + k, toks.size() - 1)]; }
			bool IsOp(const char* op, size_t k = 0) const { const Token& t = Peek(k); return t.kind == Tok::Op && t.text == op; }
			bool IsIdent(const char* id) const { const Token& t = Peek(); return t.kind == Tok::Ident && t.text == id; }
			bool Fail(const std::string& why)
			{
				if (err.empty()) err = "line " + std::to_string(Peek().line) + ": " + why;
				return false;
			}
			bool Expect(const char* op)
			{
				if (!IsOp(op)) return Fail(std::string("expected '") + op + "' near '" + Peek().text + "'");
				++p;
				return true;
			}
			int NewNode(NK k)
			{
				Node n;
				n.kind = k;
				n.line = Peek().line;
				nodes.push_back(n);
				return static_cast<int>(nodes.size()) - 1;
			}
			int NewStmt(SK k)
			{
				Stmt s;
				s.kind = k;
				stmts.push_back(s);
				return static_cast<int>(stmts.size()) - 1;
			}
			bool PeekType(Type& t, size_t k = 0) const
			{
				const Token& tk = Peek(k);
				return tk.kind == Tok::Ident && TypeFromName(tk.text, t);
			}
			void SkipQualifiers()
			{
				while (Peek().kind == Tok::Ident && (Peek().text == "const" || Peek().text == "highp" || Peek().text == "mediump" ||
					Peek().text == "lowp" || Peek().text == "in" || Peek().text == "precise"))
					++p;
			}
			bool IsDeclStart() const
			{
				size_t k = 0;
				while (Peek(k).kind == Tok::Ident && (Peek(k).text == "const" || Peek(k).text == "highp" || Peek(k).text == "mediump" ||
					Peek(k).text == "lowp" || Peek(k).text == "precise"))
					++k;
				Type t;
				return PeekType(t, k) && Peek(k + 1).kind == Tok::Ident;
			}

			int Declare(const std::string& name, const Type& t)
			{
				const int slot = cur->nSlots++;
				cur->slotTypes.push_back(t);
				scopes.back()[name] = slot;
				return slot;
			}

			int ExternFor(const std::string& name)
			{
				const auto it = externIndex.find(name);
				if (it != externIndex.end()) return it->second;
				Extern e;
				e.name = name;
				auto suffix = [&](const char* s, ExternKind k)
				{
					const size_t n = std::strlen(s);
					if (name.size() > n && name.compare(name.size() - n, n, s) == 0)
					{
						e.kind = k;
						e.binding = name.substr(0, name.size() - n);
						return true;
					}
					return false;
				};
				if (name == "out_image") e.kind = ExternKind::Image;
				else if (!(suffix("_raw", ExternKind::Raw) || suffix("_pos", ExternKind::Pos) || suffix("_size", ExternKind::Size) ||
					suffix("_pt", ExternKind::Pt) || suffix("_mul", ExternKind::Mul)))
					e.kind = ExternKind::Plain;
				externs.push_back(e);
				const int idx = static_cast<int>(externs.size()) - 1;
				externIndex[name] = idx;
				return idx;
			}

			// ---------------------------------------------------------------- constant folding
			bool IsLit(int n) const { return nodes[n].kind == NK::Lit; }
			int Fold(int n)
			{
				Node& nd = nodes[n];
				bool foldable = false;
				switch (nd.kind)
				{
				case NK::Unary: case NK::Binary: case NK::Construct: case NK::ArrayConstruct: case NK::Swizzle: case NK::Ternary:
				case NK::LogicAnd: case NK::LogicOr:
					foldable = true;
					break;
				case NK::Builtin:
				{
					const BI b = static_cast<BI>(nd.slot);
					foldable = b != BI::TexelFetch && b != BI::Texture && b != BI::ImageSize && b != BI::ImageStore;
					break;
				}
				default:
					break;
				}
				if (!foldable) return n;
				for (int k : nd.kids)
					if (!IsLit(k)) return n;
				Context dummy;
				std::vector<Value> frame;
				Value v = Eval(n, dummy, frame);
				if (!dummy.error.empty()) return n;
				Node& again = nodes[n];
				again.kind = NK::Lit;
				again.kids.clear();
				again.lit = v;
				return n;
			}

			// ---------------------------------------------------------------- expressions
			int ParseExpr() { return ParseComma(); }

			int ParseComma()
			{
				int a = ParseAssign();
				return a;   // the comma operator is not used by the exporter (for-steps use one expression)
			}

			int ParseAssign()
			{
				const int lhs = ParseTernary();
				if (lhs < 0) return -1;
				static const char* kAssign[] = { "=", "+=", "-=", "*=", "/=", "%=" };
				for (const char* op : kAssign)
					if (IsOp(op))
					{
						++p;
						const int rhs = ParseAssign();
						if (rhs < 0) return -1;
						const int n = NewNode(NK::Assign);
						nodes[n].op = op;
						nodes[n].kids = { lhs, rhs };
						return n;
					}
				return lhs;
			}

			int ParseTernary()
			{
				const int c = ParseOr();
				if (c < 0) return -1;
				if (!IsOp("?")) return c;
				++p;
				const int a = ParseAssign();
				if (a < 0 || !Expect(":")) return -1;
				const int b = ParseAssign();
				if (b < 0) return -1;
				const int n = NewNode(NK::Ternary);
				nodes[n].kids = { c, a, b };
				return Fold(n);
			}

			int ParseOr()
			{
				int a = ParseAnd();
				while (a >= 0 && IsOp("||"))
				{
					++p;
					const int b = ParseAnd();
					if (b < 0) return -1;
					const int n = NewNode(NK::LogicOr);
					nodes[n].kids = { a, b };
					a = Fold(n);
				}
				return a;
			}

			int ParseAnd()
			{
				int a = ParseEquality();
				while (a >= 0 && IsOp("&&"))
				{
					++p;
					const int b = ParseEquality();
					if (b < 0) return -1;
					const int n = NewNode(NK::LogicAnd);
					nodes[n].kids = { a, b };
					a = Fold(n);
				}
				return a;
			}

			template <typename Next>
			int ParseBinaryLevel(std::initializer_list<const char*> ops, Next next)
			{
				int a = (this->*next)();
				for (;;)
				{
					if (a < 0) return -1;
					const char* hit = nullptr;
					for (const char* op : ops) if (IsOp(op)) { hit = op; break; }
					if (!hit) return a;
					++p;
					const int b = (this->*next)();
					if (b < 0) return -1;
					const int n = NewNode(NK::Binary);
					nodes[n].op = hit;
					nodes[n].kids = { a, b };
					a = Fold(n);
				}
			}

			int ParseEquality() { return ParseBinaryLevel({ "==", "!=" }, &Impl::ParseRelational); }
			int ParseRelational() { return ParseBinaryLevel({ "<=", ">=", "<", ">" }, &Impl::ParseAdditive); }
			int ParseAdditive() { return ParseBinaryLevel({ "+", "-" }, &Impl::ParseMultiplicative); }
			int ParseMultiplicative() { return ParseBinaryLevel({ "*", "/", "%" }, &Impl::ParseUnary); }

			int ParseUnary()
			{
				if (IsOp("-") || IsOp("+") || IsOp("!"))
				{
					const std::string op = Peek().text;
					++p;
					const int a = ParseUnary();
					if (a < 0) return -1;
					if (op == "+") return a;
					const int n = NewNode(NK::Unary);
					nodes[n].op = op;
					nodes[n].kids = { a };
					return Fold(n);
				}
				if (IsOp("++") || IsOp("--"))
				{
					const std::string op = Peek().text;
					++p;
					const int a = ParseUnary();
					if (a < 0) return -1;
					const int n = NewNode(NK::PreInc);
					nodes[n].op = op;
					nodes[n].kids = { a };
					return n;
				}
				return ParsePostfix();
			}

			int ParsePostfix()
			{
				int a = ParsePrimary();
				for (;;)
				{
					if (a < 0) return -1;
					if (IsOp("."))
					{
						++p;
						if (Peek().kind != Tok::Ident) { Fail("expected a swizzle"); return -1; }
						const std::string s = Peek().text;
						++p;
						if (s.size() > 4) { Fail("bad swizzle ." + s); return -1; }
						const int n = NewNode(NK::Swizzle);
						for (size_t k = 0; k < s.size(); ++k)
						{
							const int idx = SwizzleIndex(s[k]);
							if (idx < 0) { Fail("bad swizzle ." + s); return -1; }
							nodes[n].swz[k] = static_cast<uint8_t>(idx);
						}
						nodes[n].swzN = static_cast<uint8_t>(s.size());
						nodes[n].kids = { a };
						a = Fold(n);
						continue;
					}
					if (IsOp("["))
					{
						++p;
						const int idx = ParseExpr();
						if (idx < 0 || !Expect("]")) return -1;
						const int n = NewNode(NK::Index);
						nodes[n].kids = { a, idx };
						a = n;
						continue;
					}
					if (IsOp("++") || IsOp("--"))
					{
						const int n = NewNode(NK::PostInc);
						nodes[n].op = Peek().text;
						++p;
						nodes[n].kids = { a };
						a = n;
						continue;
					}
					return a;
				}
			}

			bool ParseArgs(std::vector<int>& args)
			{
				if (!Expect("(")) return false;
				if (IsOp(")")) { ++p; return true; }
				for (;;)
				{
					const int a = ParseAssign();
					if (a < 0) return false;
					args.push_back(a);
					if (IsOp(",")) { ++p; continue; }
					return Expect(")");
				}
			}

			static bool BuiltinFromName(const std::string& s, BI& b)
			{
				static const std::map<std::string, BI> kB = {
					{ "abs", BI::Abs }, { "sign", BI::Sign }, { "floor", BI::Floor }, { "ceil", BI::Ceil }, { "fract", BI::Fract },
					{ "round", BI::Round }, { "roundEven", BI::RoundEven }, { "trunc", BI::Trunc }, { "min", BI::Min }, { "max", BI::Max },
					{ "clamp", BI::Clamp }, { "mix", BI::Mix }, { "step", BI::Step }, { "smoothstep", BI::Smoothstep }, { "sqrt", BI::Sqrt },
					{ "inversesqrt", BI::InverseSqrt }, { "exp", BI::Exp }, { "exp2", BI::Exp2 }, { "log", BI::Log }, { "log2", BI::Log2 },
					{ "pow", BI::Pow }, { "sin", BI::Sin }, { "cos", BI::Cos }, { "tan", BI::Tan }, { "tanh", BI::Tanh }, { "atan", BI::Atan },
					{ "mod", BI::Mod }, { "dot", BI::Dot }, { "length", BI::Length }, { "distance", BI::Distance }, { "normalize", BI::Normalize },
					{ "any", BI::Any }, { "all", BI::All }, { "not", BI::Not }, { "greaterThan", BI::GreaterThan },
					{ "greaterThanEqual", BI::GreaterThanEqual }, { "lessThan", BI::LessThan }, { "lessThanEqual", BI::LessThanEqual },
					{ "equal", BI::Equal }, { "notEqual", BI::NotEqual }, { "texelFetch", BI::TexelFetch }, { "texture", BI::Texture },
					{ "textureLod", BI::Texture }, { "imageSize", BI::ImageSize }, { "imageStore", BI::ImageStore }, { "isnan", BI::IsNan },
					{ "isinf", BI::IsInf }, { "fma", BI::Fma },
				};
				const auto it = kB.find(s);
				if (it == kB.end()) return false;
				b = it->second;
				return true;
			}

			static bool EndsWith(const std::string& s, const char* suf)
			{
				const size_t n = std::strlen(suf);
				return s.size() > n && s.compare(s.size() - n, n, suf) == 0;
			}

			int ParsePrimary()
			{
				const Token t = Peek();
				if (t.kind == Tok::Float || t.kind == Tok::Int)
				{
					++p;
					const int n = NewNode(NK::Lit);
					nodes[n].lit = t.kind == Tok::Float ? Value::Float(static_cast<float>(t.number)) : Value::Int(static_cast<int>(t.number));
					return n;
				}
				if (IsOp("("))
				{
					++p;
					const int e = ParseExpr();
					if (e < 0 || !Expect(")")) return -1;
					return e;
				}
				if (t.kind != Tok::Ident) { Fail("unexpected '" + t.text + "'"); return -1; }
				if (t.text == "true" || t.text == "false")
				{
					++p;
					const int n = NewNode(NK::Lit);
					nodes[n].lit = Value::Bool(t.text == "true");
					return n;
				}
				Type ty;
				if (TypeFromName(t.text, ty))
				{
					++p;
					if (IsOp("["))   // array constructor T[N](...)
					{
						++p;
						int len = 0;
						if (Peek().kind == Tok::Int) { len = static_cast<int>(Peek().number); ++p; }
						if (!Expect("]")) return -1;
						std::vector<int> args;
						if (!ParseArgs(args)) return -1;
						const int n = NewNode(NK::ArrayConstruct);
						nodes[n].type = ty;
						nodes[n].type.arrayLen = len ? len : static_cast<int>(args.size());
						nodes[n].kids = args;
						return Fold(n);
					}
					std::vector<int> args;
					if (!ParseArgs(args)) return -1;
					const int n = NewNode(NK::Construct);
					nodes[n].type = ty;
					nodes[n].kids = args;
					return Fold(n);
				}
				++p;
				if (IsOp("("))   // a call
				{
					std::vector<int> args;
					if (!ParseArgs(args)) return -1;
					const auto fit = functionIndex.find(t.text);
					if (fit != functionIndex.end())
					{
						const int n = NewNode(NK::Call);
						nodes[n].slot = fit->second;
						nodes[n].kids = args;
						return n;
					}
					BI b;
					if (BuiltinFromName(t.text, b))
					{
						const int n = NewNode(NK::Builtin);
						nodes[n].slot = static_cast<int>(b);
						nodes[n].kids = args;
						return Fold(n);
					}
					if (EndsWith(t.text, "_texOff"))
					{
						const std::string name = t.text.substr(0, t.text.size() - 7);
						if (args.size() != 1) { Fail(t.text + " takes one argument"); return -1; }
						const int n = NewNode(NK::TexOff);
						nodes[n].slot = ExternFor(name + "_raw");
						nodes[n].slot2 = ExternFor(name + "_pt");
						nodes[n].kids = { args[0] };
						nodes[n].lit = Value::Int(ExternFor(name + "_pos"));
						nodes[n].mulExt = ExternFor(name + "_mul");
						return n;
					}
					if (EndsWith(t.text, "_tex"))
					{
						const std::string name = t.text.substr(0, t.text.size() - 4);
						if (args.size() != 1) { Fail(t.text + " takes one argument"); return -1; }
						const int n = NewNode(NK::TexSample);
						nodes[n].slot = ExternFor(name + "_raw");
						nodes[n].kids = { args[0] };
						nodes[n].mulExt = ExternFor(name + "_mul");
						return n;
					}
					Fail("unknown function '" + t.text + "'");
					return -1;
				}
				// An identifier: local, global, or extern.
				for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
				{
					const auto f = it->find(t.text);
					if (f != it->end())
					{
						const int n = NewNode(NK::Local);
						nodes[n].slot = f->second;
						return n;
					}
				}
				const auto g = globalIndex.find(t.text);
				if (g != globalIndex.end())
				{
					const int n = NewNode(NK::Global);
					nodes[n].slot = g->second;
					return n;
				}
				const int n = NewNode(NK::Ext);
				nodes[n].slot = ExternFor(t.text);
				return n;
			}

			// ---------------------------------------------------------------- statements
			int ParseDecl()
			{
				SkipQualifiers();
				Type t;
				if (!PeekType(t)) { Fail("expected a type"); return -1; }
				++p;
				const int s = NewStmt(SK::Decl);
				for (;;)
				{
					if (Peek().kind != Tok::Ident) { Fail("expected a name"); return -1; }
					const std::string name = Peek().text;
					++p;
					Type vt = t;
					if (IsOp("["))
					{
						++p;
						if (Peek().kind == Tok::Int) { vt.arrayLen = static_cast<int>(Peek().number); ++p; }
						else vt.arrayLen = -1;
						if (!Expect("]")) return -1;
					}
					int init = -1;
					if (IsOp("="))
					{
						++p;
						init = ParseAssign();
						if (init < 0) return -1;
					}
					// The name is visible only after its initialiser (GLSL scoping).
					const int slot = Declare(name, vt);
					stmts[s].decls.push_back({ slot, init });
					if (IsOp(",")) { ++p; continue; }
					break;
				}
				return s;
			}

			int ParseStatement()
			{
				if (IsOp("{"))
				{
					++p;
					scopes.emplace_back();
					const int s = NewStmt(SK::Block);
					while (!IsOp("}"))
					{
						if (Peek().kind == Tok::End) { Fail("unterminated block"); return -1; }
						const int c = ParseStatement();
						if (c < 0) return -1;
						stmts[s].stmts.push_back(c);
					}
					++p;
					scopes.pop_back();
					return s;
				}
				if (IsOp(";")) { ++p; return NewStmt(SK::Empty); }
				if (IsIdent("if"))
				{
					++p;
					if (!Expect("(")) return -1;
					const int c = ParseExpr();
					if (c < 0 || !Expect(")")) return -1;
					scopes.emplace_back();
					const int body = ParseStatement();
					scopes.pop_back();
					if (body < 0) return -1;
					int eb = -1;
					if (IsIdent("else"))
					{
						++p;
						scopes.emplace_back();
						eb = ParseStatement();
						scopes.pop_back();
						if (eb < 0) return -1;
					}
					const int s = NewStmt(SK::If);
					stmts[s].expr = c;
					stmts[s].body = body;
					stmts[s].elseBody = eb;
					return s;
				}
				if (IsIdent("for"))
				{
					++p;
					if (!Expect("(")) return -1;
					scopes.emplace_back();
					int init = -1;
					if (!IsOp(";"))
					{
						if (IsDeclStart()) init = ParseDecl();
						else
						{
							const int e = ParseExpr();
							if (e < 0) return -1;
							init = NewStmt(SK::Expr);
							stmts[init].expr = e;
						}
						if (init < 0) return -1;
					}
					if (!Expect(";")) return -1;
					int cond = -1;
					if (!IsOp(";")) { cond = ParseExpr(); if (cond < 0) return -1; }
					if (!Expect(";")) return -1;
					int step = -1;
					if (!IsOp(")")) { step = ParseExpr(); if (step < 0) return -1; }
					if (!Expect(")")) return -1;
					const int body = ParseStatement();
					scopes.pop_back();
					if (body < 0) return -1;
					const int s = NewStmt(SK::For);
					stmts[s].init = init;
					stmts[s].expr = cond;
					stmts[s].expr2 = step;
					stmts[s].body = body;
					return s;
				}
				if (IsIdent("while"))
				{
					++p;
					if (!Expect("(")) return -1;
					const int c = ParseExpr();
					if (c < 0 || !Expect(")")) return -1;
					scopes.emplace_back();
					const int body = ParseStatement();
					scopes.pop_back();
					if (body < 0) return -1;
					const int s = NewStmt(SK::While);
					stmts[s].expr = c;
					stmts[s].body = body;
					return s;
				}
				if (IsIdent("return"))
				{
					++p;
					const int s = NewStmt(SK::Return);
					if (!IsOp(";"))
					{
						const int e = ParseExpr();
						if (e < 0) return -1;
						stmts[s].expr = e;
					}
					if (!Expect(";")) return -1;
					return s;
				}
				if (IsIdent("break") || IsIdent("continue"))
				{
					const bool brk = Peek().text == "break";
					++p;
					if (!Expect(";")) return -1;
					return NewStmt(brk ? SK::Break : SK::Continue);
				}
				if (IsDeclStart())
				{
					const int s = ParseDecl();
					if (s < 0 || !Expect(";")) return -1;
					return s;
				}
				const int e = ParseExpr();
				if (e < 0 || !Expect(";")) return -1;
				const int s = NewStmt(SK::Expr);
				stmts[s].expr = e;
				return s;
			}

			bool ParseTop()
			{
				while (Peek().kind != Tok::End)
				{
					SkipQualifiers();
					Type t;
					if (!PeekType(t)) return Fail("expected a declaration near '" + Peek().text + "'");
					if (Peek(1).kind != Tok::Ident) return Fail("expected a name");
					const std::string name = Peek(1).text;
					if (Peek(2).kind == Tok::Op && Peek(2).text == "(")
					{
						p += 3;
						Function fn;
						fn.name = name;
						fn.ret = t;
						functions.push_back(fn);
						const int fi = static_cast<int>(functions.size()) - 1;
						cur = &functions[fi];
						scopes.clear();
						scopes.emplace_back();
						if (IsIdent("void") && IsOp(")", 1)) ++p;
						while (!IsOp(")"))
						{
							SkipQualifiers();
							Type pt;
							if (!PeekType(pt)) return Fail("expected a parameter type");
							++p;
							if (Peek().kind != Tok::Ident) return Fail("expected a parameter name");
							cur->paramSlots.push_back(Declare(Peek().text, pt));
							++p;
							if (IsOp(",")) ++p;
							else if (!IsOp(")")) return Fail("expected ',' or ')'");
						}
						++p;
						if (IsOp(";")) { ++p; continue; }   // a prototype
						functionIndex[name] = fi;            // visible inside its own body (no recursion at run time)
						const int body = ParseStatement();
						cur = &functions[fi];
						if (body < 0) return false;
						cur->body = body;
						cur->defined = true;
						cur = nullptr;
						continue;
					}
					// A global variable / constant table.
					++p;
					for (;;)
					{
						if (Peek().kind != Tok::Ident) return Fail("expected a global name");
						const std::string gname = Peek().text;
						++p;
						Type gt = t;
						if (IsOp("["))
						{
							++p;
							if (Peek().kind == Tok::Int) { gt.arrayLen = static_cast<int>(Peek().number); ++p; }
							if (!Expect("]")) return false;
						}
						int init = -1;
						if (IsOp("="))
						{
							++p;
							Function scratch;
							cur = &scratch;
							scopes.clear();
							scopes.emplace_back();
							init = ParseAssign();
							cur = nullptr;
							if (init < 0) return false;
						}
						const int slot = static_cast<int>(globals.size());
						globals.push_back(Value());
						globalTypes.push_back(gt);
						globalIndex[gname] = slot;
						globalInits.push_back({ slot, init });
						if (IsOp(",")) { ++p; continue; }
						break;
					}
					if (!Expect(";")) return false;
				}
				return true;
			}

			// ---------------------------------------------------------------- evaluation
			bool RtFail(Context& ctx, const Node& n, const std::string& why) const
			{
				if (ctx.error.empty()) ctx.error = "line " + std::to_string(n.line) + ": " + why;
				return false;
			}

			static float Comp(const Value& v, int k) { return v.F(v.n == 1 ? 0 : k); }

			Value Arith(const std::string& op, const Value& a, const Value& b, Context& ctx, const Node& nd) const
			{
				const char o = op[0];
				if (o == '*' && a.n == 16 && b.n == 4)
				{
					Value r = Value::Vec(0, 0, 0, 0);
					for (int row = 0; row < 4; ++row)
					{
						float s = 0.0f;
						for (int col = 0; col < 4; ++col) s += a.f[col * 4 + row] * b.F(col);
						r.f[row] = s;
					}
					return r;
				}
				if (o == '*' && a.n == 4 && b.n == 16)
				{
					Value r = Value::Vec(0, 0, 0, 0);
					for (int col = 0; col < 4; ++col)
					{
						float s = 0.0f;
						for (int row = 0; row < 4; ++row) s += a.F(row) * b.f[col * 4 + row];
						r.f[col] = s;
					}
					return r;
				}
				if (a.n == 16 || b.n == 16)
				{
					if (a.n == 16 && b.n == 16 && (o == '+' || o == '-'))
					{
						Value r = a;
						for (int k = 0; k < 16; ++k) r.f[k] = o == '+' ? a.f[k] + b.f[k] : a.f[k] - b.f[k];
						return r;
					}
					if ((a.n == 16 && b.n == 1) || (a.n == 1 && b.n == 16))
					{
						const Value& m = a.n == 16 ? a : b;
						const float s = a.n == 16 ? b.F(0) : a.F(0);
						Value r = m;
						for (int k = 0; k < 16; ++k)
							r.f[k] = o == '*' ? m.f[k] * s : (o == '/' && a.n == 16) ? m.f[k] / s : o == '+' ? m.f[k] + s : m.f[k] - s;
						return r;
					}
					RtFail(ctx, nd, "unsupported matrix operation " + op);
					return Value();
				}
				if (a.n != b.n && a.n != 1 && b.n != 1)
				{
					RtFail(ctx, nd, "component count mismatch in " + op);
					return Value();
				}
				const int n = std::max(a.n, b.n);
				Value r;
				r.n = static_cast<uint8_t>(n);
				const bool fl = a.base == Base::Float || b.base == Base::Float;
				r.base = fl ? Base::Float : Base::Int;
				for (int k = 0; k < n; ++k)
				{
					if (fl)
					{
						const float x = Comp(a, k), y = Comp(b, k);
						float z = 0.0f;
						switch (o)
						{
						case '+': z = x + y; break;
						case '-': z = x - y; break;
						case '*': z = x * y; break;
						case '/': z = x / y; break;
						default: RtFail(ctx, nd, "'%' on floats"); return Value();
						}
						r.f[k] = z;
					}
					else
					{
						const int x = a.i[a.n == 1 ? 0 : k], y = b.i[b.n == 1 ? 0 : k];
						int z = 0;
						switch (o)
						{
						case '+': z = x + y; break;
						case '-': z = x - y; break;
						case '*': z = x * y; break;
						case '/': if (y == 0) { RtFail(ctx, nd, "integer division by zero"); return Value(); } z = x / y; break;
						case '%': if (y == 0) { RtFail(ctx, nd, "integer modulo by zero"); return Value(); } z = x % y; break;
						default: break;
						}
						r.i[k] = z;
					}
				}
				return r;
			}

			Value Binary(const Node& nd, const Value& a, const Value& b, Context& ctx) const
			{
				const std::string& op = nd.op;
				if (op == "==" || op == "!=")
				{
					bool eq = a.n == b.n;
					const bool fl = a.base == Base::Float || b.base == Base::Float;
					for (int k = 0; eq && k < a.n; ++k)
						eq = fl ? a.F(k) == b.F(k) : a.i[k] == b.i[k];
					return Value::Bool(op == "==" ? eq : !eq);
				}
				if (op == "<" || op == ">" || op == "<=" || op == ">=")
				{
					if (a.n != 1 || b.n != 1) { RtFail(ctx, nd, "relational operator on vectors"); return Value(); }
					const bool fl = a.base == Base::Float || b.base == Base::Float;
					bool res;
					if (fl)
					{
						const float x = a.F(0), y = b.F(0);
						res = op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y;
					}
					else
					{
						const int x = a.i[0], y = b.i[0];
						res = op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y;
					}
					return Value::Bool(res);
				}
				return Arith(op, a, b, ctx, nd);
			}

			Value Construct(const Node& nd, const std::vector<Value>& args, Context& ctx) const
			{
				const Type& t = nd.type;
				if (args.size() == 1 && args[0].n == 1 && t.n != 16)
				{
					Value r;
					r.base = t.base;
					r.n = t.n;
					for (int k = 0; k < t.n; ++k)
					{
						if (t.base == Base::Float) r.f[k] = args[0].F(0);
						else if (t.base == Base::Int) r.i[k] = args[0].base == Base::Float ? static_cast<int>(args[0].f[0]) : args[0].i[0];
						else r.i[k] = Truthy(args[0]) ? 1 : 0;
					}
					return r;
				}
				if (args.size() == 1 && args[0].n >= t.n && t.n != 16 && args[0].n != 16)
				{
					Type tt = t;
					Value src = args[0];
					src.n = t.n;   // truncating constructor: vec2(vec4)
					return Convert(src, tt);
				}
				Value r;
				r.base = t.base;
				r.n = t.n;
				if (t.n == 16 && args.size() == 1 && args[0].n == 1)
				{
					for (int k = 0; k < 4; ++k) r.f[k * 5] = args[0].F(0);
					return r;
				}
				int k = 0;
				for (const Value& a : args)
					for (int c = 0; c < a.n; ++c)
					{
						if (k >= t.n) { RtFail(ctx, nd, "too many constructor components"); return Value(); }
						if (t.base == Base::Float) r.f[k] = a.F(c);
						else if (t.base == Base::Int) r.i[k] = a.base == Base::Float ? static_cast<int>(a.f[c]) : a.i[c];
						else r.i[k] = (a.base == Base::Float ? a.f[c] != 0.0f : a.i[c] != 0) ? 1 : 0;
						++k;
					}
				if (k != t.n) { RtFail(ctx, nd, "wrong constructor component count"); return Value(); }
				return r;
			}

			static Value Fetch(const TexView& tv, int x, int y)
			{
				x = std::min(std::max(x, 0), tv.width - 1);
				y = std::min(std::max(y, 0), tv.height - 1);
				const float* px = tv.rgba + (static_cast<size_t>(y) * tv.width + x) * 4;
				return Value::Vec(px[0], px[1], px[2], px[3]);
			}

			// textureLod(tex, pos, 0) with LINEAR filtering and CLAMP addressing (libplacebo sh_bind).
			static Value Bilinear(const TexView& tv, float u, float v)
			{
				const float tx = u * tv.width - 0.5f, ty = v * tv.height - 0.5f;
				const float fx0 = std::floor(tx), fy0 = std::floor(ty);
				const float ax = tx - fx0, ay = ty - fy0;
				const int x0 = static_cast<int>(fx0), y0 = static_cast<int>(fy0);
				const Value a = Fetch(tv, x0, y0), b = Fetch(tv, x0 + 1, y0), c = Fetch(tv, x0, y0 + 1), d = Fetch(tv, x0 + 1, y0 + 1);
				Value r = Value::Vec(0, 0, 0, 0);
				for (int k = 0; k < 4; ++k)
				{
					const float top = a.f[k] + (b.f[k] - a.f[k]) * ax;
					const float bot = c.f[k] + (d.f[k] - c.f[k]) * ax;
					r.f[k] = top + (bot - top) * ay;
				}
				return r;
			}

			const TexView* Texture(const Value& sampler, Context& ctx, const Node& nd) const
			{
				if (sampler.base != Base::Sampler || sampler.i[0] < 0 || sampler.i[0] >= static_cast<int>(ctx.textures.size()) ||
					!ctx.textures[sampler.i[0]].rgba)
				{
					RtFail(ctx, nd, "texture not bound");
					return nullptr;
				}
				return &ctx.textures[sampler.i[0]];
			}

			Value CallBuiltin(const Node& nd, const std::vector<Value>& a, Context& ctx) const
			{
				const BI b = static_cast<BI>(nd.slot);
				auto need = [&](size_t n) { if (a.size() != n) { RtFail(ctx, nd, "wrong argument count"); return false; } return true; };
				auto unaryF = [&](float (*fn)(float)) -> Value
				{
					if (!need(1)) return Value();
					Value r = Convert(a[0], { Base::Float, a[0].n, 0 });
					for (int k = 0; k < r.n; ++k) r.f[k] = fn(r.f[k]);
					return r;
				};
				auto binaryF = [&](float (*fn)(float, float)) -> Value
				{
					if (!need(2)) return Value();
					const int n = std::max(a[0].n, a[1].n);
					Value r;
					r.base = Base::Float;
					r.n = static_cast<uint8_t>(n);
					for (int k = 0; k < n; ++k) r.f[k] = fn(Comp(a[0], k), Comp(a[1], k));
					return r;
				};
				auto compare = [&](int mode) -> Value
				{
					if (!need(2)) return Value();
					Value r;
					r.base = Base::Bool;
					r.n = a[0].n;
					const bool fl = a[0].base == Base::Float || a[1].base == Base::Float;
					for (int k = 0; k < r.n; ++k)
					{
						bool res;
						if (fl)
						{
							const float x = a[0].F(k), y = a[1].F(k);
							res = mode == 0 ? x > y : mode == 1 ? x >= y : mode == 2 ? x < y : mode == 3 ? x <= y : mode == 4 ? x == y : x != y;
						}
						else
						{
							const int x = a[0].i[k], y = a[1].i[k];
							res = mode == 0 ? x > y : mode == 1 ? x >= y : mode == 2 ? x < y : mode == 3 ? x <= y : mode == 4 ? x == y : x != y;
						}
						r.i[k] = res ? 1 : 0;
					}
					return r;
				};
				switch (b)
				{
				case BI::Abs:
					if (!need(1)) return Value();
					if (a[0].base == Base::Int) { Value r = a[0]; for (int k = 0; k < r.n; ++k) r.i[k] = std::abs(r.i[k]); return r; }
					return unaryF([](float x) { return std::fabs(x); });
				case BI::Sign:
					if (!need(1)) return Value();
					if (a[0].base == Base::Int) { Value r = a[0]; for (int k = 0; k < r.n; ++k) r.i[k] = (r.i[k] > 0) - (r.i[k] < 0); return r; }
					return unaryF([](float x) { return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f; });
				case BI::Floor: return unaryF([](float x) { return std::floor(x); });
				case BI::Ceil: return unaryF([](float x) { return std::ceil(x); });
				case BI::Fract: return unaryF([](float x) { return x - std::floor(x); });
				case BI::Round: return unaryF([](float x) { return std::nearbyint(x); });       // GPUs round half to even
				case BI::RoundEven: return unaryF([](float x) { return std::nearbyint(x); });
				case BI::Trunc: return unaryF([](float x) { return std::trunc(x); });
				case BI::Sqrt: return unaryF([](float x) { return std::sqrt(x); });
				case BI::InverseSqrt: return unaryF([](float x) { return 1.0f / std::sqrt(x); });
				case BI::Exp: return unaryF([](float x) { return std::exp(x); });
				case BI::Exp2: return unaryF([](float x) { return std::exp2(x); });
				case BI::Log: return unaryF([](float x) { return std::log(x); });
				case BI::Log2: return unaryF([](float x) { return std::log2(x); });
				case BI::Sin: return unaryF([](float x) { return std::sin(x); });
				case BI::Cos: return unaryF([](float x) { return std::cos(x); });
				case BI::Tan: return unaryF([](float x) { return std::tan(x); });
				case BI::Tanh: return unaryF([](float x) { return std::tanh(x); });
				case BI::Atan:
					if (a.size() == 2) return binaryF([](float y, float x) { return std::atan2(y, x); });
					return unaryF([](float x) { return std::atan(x); });
				case BI::Pow: return binaryF([](float x, float y) { return std::pow(x, y); });
				case BI::Mod: return binaryF([](float x, float y) { return x - y * std::floor(x / y); });
				case BI::Step: return binaryF([](float e, float x) { return x < e ? 0.0f : 1.0f; });
				case BI::Min: case BI::Max:
				{
					if (!need(2)) return Value();
					const bool isMin = b == BI::Min;
					if (a[0].base != Base::Float && a[1].base != Base::Float)
					{
						const int n = std::max(a[0].n, a[1].n);
						Value r;
						r.base = Base::Int;
						r.n = static_cast<uint8_t>(n);
						for (int k = 0; k < n; ++k)
						{
							const int x = a[0].i[a[0].n == 1 ? 0 : k], y = a[1].i[a[1].n == 1 ? 0 : k];
							r.i[k] = isMin ? std::min(x, y) : std::max(x, y);
						}
						return r;
					}
					return isMin ? binaryF([](float x, float y) { return y < x ? y : x; }) : binaryF([](float x, float y) { return x < y ? y : x; });
				}
				case BI::Clamp:
				{
					if (!need(3)) return Value();
					if (a[0].base != Base::Float && a[1].base != Base::Float && a[2].base != Base::Float)
					{
						Value r = a[0];
						for (int k = 0; k < r.n; ++k)
							r.i[k] = std::min(std::max(r.i[k], a[1].i[a[1].n == 1 ? 0 : k]), a[2].i[a[2].n == 1 ? 0 : k]);
						return r;
					}
					Value r = Convert(a[0], { Base::Float, a[0].n, 0 });
					for (int k = 0; k < r.n; ++k) r.f[k] = std::min(std::max(r.f[k], Comp(a[1], k)), Comp(a[2], k));
					return r;
				}
				case BI::Mix:
				{
					if (!need(3)) return Value();
					const int n = std::max(a[0].n, a[1].n);
					Value r;
					r.base = Base::Float;
					r.n = static_cast<uint8_t>(n);
					for (int k = 0; k < n; ++k)
					{
						const float x = Comp(a[0], k), y = Comp(a[1], k), t = Comp(a[2], k);
						r.f[k] = a[2].base == Base::Bool ? (t != 0.0f ? y : x) : x * (1.0f - t) + y * t;
					}
					return r;
				}
				case BI::Smoothstep:
				{
					if (!need(3)) return Value();
					Value r = Convert(a[2], { Base::Float, a[2].n, 0 });
					for (int k = 0; k < r.n; ++k)
					{
						const float e0 = Comp(a[0], k), e1 = Comp(a[1], k);
						float t = std::min(std::max((r.f[k] - e0) / (e1 - e0), 0.0f), 1.0f);
						r.f[k] = t * t * (3.0f - 2.0f * t);
					}
					return r;
				}
				case BI::Fma:
				{
					if (!need(3)) return Value();
					Value r = Convert(a[0], { Base::Float, a[0].n, 0 });
					for (int k = 0; k < r.n; ++k) r.f[k] = r.f[k] * Comp(a[1], k) + Comp(a[2], k);
					return r;
				}
				case BI::Dot:
				{
					if (!need(2)) return Value();
					float s = 0.0f;
					for (int k = 0; k < a[0].n; ++k) s += a[0].F(k) * a[1].F(k);
					return Value::Float(s);
				}
				case BI::Length:
				{
					if (!need(1)) return Value();
					float s = 0.0f;
					for (int k = 0; k < a[0].n; ++k) s += a[0].F(k) * a[0].F(k);
					return Value::Float(std::sqrt(s));
				}
				case BI::Distance:
				{
					if (!need(2)) return Value();
					float s = 0.0f;
					for (int k = 0; k < a[0].n; ++k) { const float d = a[0].F(k) - a[1].F(k); s += d * d; }
					return Value::Float(std::sqrt(s));
				}
				case BI::Normalize:
				{
					if (!need(1)) return Value();
					float s = 0.0f;
					for (int k = 0; k < a[0].n; ++k) s += a[0].F(k) * a[0].F(k);
					Value r = Convert(a[0], { Base::Float, a[0].n, 0 });
					const float inv = 1.0f / std::sqrt(s);
					for (int k = 0; k < r.n; ++k) r.f[k] *= inv;
					return r;
				}
				case BI::Any: case BI::All:
				{
					if (!need(1)) return Value();
					bool res = b == BI::All;
					for (int k = 0; k < a[0].n; ++k)
					{
						const bool c = a[0].base == Base::Float ? a[0].f[k] != 0.0f : a[0].i[k] != 0;
						res = b == BI::Any ? (res || c) : (res && c);
					}
					return Value::Bool(res);
				}
				case BI::Not:
				{
					if (!need(1)) return Value();
					Value r = a[0];
					for (int k = 0; k < r.n; ++k) r.i[k] = r.i[k] ? 0 : 1;
					return r;
				}
				case BI::IsNan: case BI::IsInf:
				{
					if (!need(1)) return Value();
					Value r;
					r.base = Base::Bool;
					r.n = a[0].n;
					for (int k = 0; k < r.n; ++k) r.i[k] = b == BI::IsNan ? std::isnan(a[0].F(k)) : std::isinf(a[0].F(k));
					return r;
				}
				case BI::GreaterThan: return compare(0);
				case BI::GreaterThanEqual: return compare(1);
				case BI::LessThan: return compare(2);
				case BI::LessThanEqual: return compare(3);
				case BI::Equal: return compare(4);
				case BI::NotEqual: return compare(5);
				case BI::TexelFetch:
				{
					if (a.size() < 2) { RtFail(ctx, nd, "texelFetch needs a sampler and a position"); return Value(); }
					const TexView* tv = Texture(a[0], ctx, nd);
					if (!tv) return Value();
					return Fetch(*tv, a[1].I(0), a[1].I(1));
				}
				case BI::Texture:
				{
					if (a.size() < 2) { RtFail(ctx, nd, "texture needs a sampler and a position"); return Value(); }
					const TexView* tv = Texture(a[0], ctx, nd);
					if (!tv) return Value();
					return Bilinear(*tv, a[1].F(0), a[1].F(1));
				}
				case BI::ImageSize:
				{
					if (!need(1)) return Value();
					if (!ctx.image.rgba) { RtFail(ctx, nd, "imageSize without an out_image (not a compute pass)"); return Value(); }
					Value r;
					r.base = Base::Int;
					r.n = 2;
					r.i[0] = ctx.image.width;
					r.i[1] = ctx.image.height;
					return r;
				}
				case BI::ImageStore:
				{
					if (!need(3)) return Value();
					if (!ctx.image.rgba) { RtFail(ctx, nd, "imageStore without an out_image (not a compute pass)"); return Value(); }
					const int x = a[1].I(0), y = a[1].I(1);
					if (x < 0 || y < 0 || x >= ctx.image.width || y >= ctx.image.height) return Value();   // GPU: ignored
					float* px = ctx.image.rgba + (static_cast<size_t>(y) * ctx.image.width + x) * 4;
					static const float kDefault[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
					for (int k = 0; k < 4; ++k)
						px[k] = k < ctx.image.components ? a[2].F(std::min<int>(k, a[2].n - 1)) : kDefault[k];
					return Value();
				}
				}
				RtFail(ctx, nd, "unsupported builtin");
				return Value();
			}

			Value Assign(int n, Context& ctx, std::vector<Value>& frame, const Function* fn) const
			{
				const Node& nd = nodes[n];
				const int target = nd.kids[0];
				const Node& tn = nodes[target];
				Value rhs = Eval(nd.kids[1], ctx, frame);
				if (!ctx.error.empty()) return Value();
				const std::string op = nd.op == "=" ? std::string() : nd.op.substr(0, 1);
				if (tn.kind == NK::Local)
				{
					Value& v = frame[tn.slot];
					Value nv = op.empty() ? rhs : Binary(MakeOpNode(op, nd.line), v, rhs, ctx);
					if (!ctx.error.empty()) return Value();
					const Type& t = fn->slotTypes[tn.slot];
					v = Convert(nv, t);
					return v;
				}
				if (tn.kind == NK::Swizzle && nodes[tn.kids[0]].kind == NK::Local)
				{
					Value& base = frame[nodes[tn.kids[0]].slot];
					Value curVal;
					curVal.base = base.base;
					curVal.n = tn.swzN;
					for (int k = 0; k < tn.swzN; ++k) { curVal.f[k] = base.f[tn.swz[k]]; curVal.i[k] = base.i[tn.swz[k]]; }
					Value nv = op.empty() ? rhs : Binary(MakeOpNode(op, nd.line), curVal, rhs, ctx);
					if (!ctx.error.empty()) return Value();
					nv = Convert(nv, { base.base, tn.swzN, 0 });
					for (int k = 0; k < tn.swzN; ++k)
					{
						if (base.base == Base::Float) base.f[tn.swz[k]] = nv.f[nv.n == 1 ? 0 : k];
						else base.i[tn.swz[k]] = nv.i[nv.n == 1 ? 0 : k];
					}
					return nv;
				}
				RtFail(ctx, nd, "unsupported assignment target");
				return Value();
			}

			static Node MakeOpNode(const std::string& op, int line)
			{
				Node t;
				t.kind = NK::Binary;
				t.op = op;
				t.line = line;
				return t;
			}

			// The function whose frame is active (for declared slot types in assignments).
			mutable const Function* active = nullptr;

			Value Eval(int n, Context& ctx, std::vector<Value>& frame) const
			{
				const Node& nd = nodes[n];
				switch (nd.kind)
				{
				case NK::Lit: return nd.lit;
				case NK::Local: return frame[nd.slot];
				case NK::Global: return globals[nd.slot];
				case NK::Ext:
					if (nd.slot >= static_cast<int>(ctx.externs.size())) { RtFail(ctx, nd, "unbound name '" + externs[nd.slot].name + "'"); return Value(); }
					return ctx.externs[nd.slot];
				case NK::Unary:
				{
					Value a = Eval(nd.kids[0], ctx, frame);
					if (nd.op == "!") return Value::Bool(!Truthy(a));
					for (int k = 0; k < a.n; ++k) a.f[k] = -a.f[k];
					for (int k = 0; k < a.n && k < 4; ++k) a.i[k] = -a.i[k];
					return a;
				}
				case NK::Binary:
				{
					const Value a = Eval(nd.kids[0], ctx, frame);
					const Value b = Eval(nd.kids[1], ctx, frame);
					if (!ctx.error.empty()) return Value();
					return Binary(nd, a, b, ctx);
				}
				case NK::LogicAnd:
					return Value::Bool(Truthy(Eval(nd.kids[0], ctx, frame)) && Truthy(Eval(nd.kids[1], ctx, frame)));
				case NK::LogicOr:
					return Value::Bool(Truthy(Eval(nd.kids[0], ctx, frame)) || Truthy(Eval(nd.kids[1], ctx, frame)));
				case NK::Ternary:
					return Truthy(Eval(nd.kids[0], ctx, frame)) ? Eval(nd.kids[1], ctx, frame) : Eval(nd.kids[2], ctx, frame);
				case NK::Assign:
					return Assign(n, ctx, frame, active);
				case NK::PreInc: case NK::PostInc:
				{
					const Node& tn = nodes[nd.kids[0]];
					if (tn.kind != NK::Local) { RtFail(ctx, nd, "++/-- on a non-variable"); return Value(); }
					Value& v = frame[tn.slot];
					const Value old = v;
					const int d = nd.op == "++" ? 1 : -1;
					for (int k = 0; k < v.n; ++k) { if (v.base == Base::Float) v.f[k] += static_cast<float>(d); else v.i[k] += d; }
					return nd.kind == NK::PreInc ? v : old;
				}
				case NK::Call:
				{
					const Function& fn = functions[nd.slot];
					if (!fn.defined) { RtFail(ctx, nd, "call of undefined function " + fn.name); return Value(); }
					std::vector<Value> callee(fn.nSlots);
					for (size_t k = 0; k < nd.kids.size() && k < fn.paramSlots.size(); ++k)
						callee[fn.paramSlots[k]] = Convert(Eval(nd.kids[k], ctx, frame), fn.slotTypes[fn.paramSlots[k]]);
					if (!ctx.error.empty()) return Value();
					return Invoke(fn, ctx, callee);
				}
				case NK::Builtin:
				{
					std::vector<Value> args;
					args.reserve(nd.kids.size());
					for (int k : nd.kids) args.push_back(Eval(k, ctx, frame));
					if (!ctx.error.empty()) return Value();
					return CallBuiltin(nd, args, ctx);
				}
				case NK::Construct:
				{
					std::vector<Value> args;
					args.reserve(nd.kids.size());
					for (int k : nd.kids) args.push_back(Eval(k, ctx, frame));
					if (!ctx.error.empty()) return Value();
					return Construct(nd, args, ctx);
				}
				case NK::ArrayConstruct:
				{
					Value r;
					r.base = nd.type.base;
					r.n = nd.type.n;
					r.arr = std::make_shared<std::vector<Value>>();
					for (int k : nd.kids) r.arr->push_back(Convert(Eval(k, ctx, frame), { nd.type.base, nd.type.n, 0 }));
					return r;
				}
				case NK::Swizzle:
				{
					const Value a = Eval(nd.kids[0], ctx, frame);
					Value r;
					r.base = a.base;
					r.n = nd.swzN;
					for (int k = 0; k < nd.swzN; ++k)
					{
						if (nd.swz[k] >= a.n) { RtFail(ctx, nd, "swizzle out of range"); return Value(); }
						r.f[k] = a.f[nd.swz[k]];
						r.i[k] = a.i[nd.swz[k]];
					}
					return r;
				}
				case NK::Index:
				{
					const Value a = Eval(nd.kids[0], ctx, frame);
					const int idx = Eval(nd.kids[1], ctx, frame).I(0);
					if (!ctx.error.empty()) return Value();
					if (a.arr)
					{
						if (idx < 0 || idx >= static_cast<int>(a.arr->size())) { RtFail(ctx, nd, "array index out of range"); return Value(); }
						return (*a.arr)[idx];
					}
					if (a.n == 16)
					{
						if (idx < 0 || idx > 3) { RtFail(ctx, nd, "matrix column out of range"); return Value(); }
						return Value::Vec(a.f[idx * 4], a.f[idx * 4 + 1], a.f[idx * 4 + 2], a.f[idx * 4 + 3]);
					}
					if (idx < 0 || idx >= a.n) { RtFail(ctx, nd, "vector index out of range"); return Value(); }
					Value r;
					r.base = a.base;
					r.n = 1;
					r.f[0] = a.f[idx];
					r.i[0] = a.i[idx];
					return r;
				}
				case NK::TexSample:
				{
					const Value pos = Eval(nd.kids[0], ctx, frame);
					if (!ctx.error.empty()) return Value();
					const TexView* tv = Texture(ctx.externs[nd.slot], ctx, nd);
					if (!tv) return Value();
					Value r = Bilinear(*tv, pos.F(0), pos.F(1));
					const float mul = ctx.externs[nd.mulExt].F(0);
					for (int k = 0; k < 4; ++k) r.f[k] *= mul;
					return r;
				}
				case NK::TexOff:
				{
					const Value off = Eval(nd.kids[0], ctx, frame);
					if (!ctx.error.empty()) return Value();
					const TexView* tv = Texture(ctx.externs[nd.slot], ctx, nd);
					if (!tv) return Value();
					const Value& pos = ctx.externs[nd.lit.i[0]];
					const Value& pt = ctx.externs[nd.slot2];
					Value r = Bilinear(*tv, pos.F(0) + pt.F(0) * Comp(off, 0), pos.F(1) + pt.F(1) * Comp(off, 1));
					const float mul = ctx.externs[nd.mulExt].F(0);
					for (int k = 0; k < 4; ++k) r.f[k] *= mul;
					return r;
				}
				case NK::Comma:
					break;
				}
				RtFail(ctx, nd, "unsupported expression");
				return Value();
			}

			Value Invoke(const Function& fn, Context& ctx, std::vector<Value>& frame) const
			{
				const Function* saved = active;
				active = &fn;
				Value ret;
				Exec(fn.body, ctx, frame, ret);
				active = saved;
				return fn.ret.base == Base::Void ? Value() : Convert(ret, fn.ret);
			}

			Flow Exec(int s, Context& ctx, std::vector<Value>& frame, Value& ret) const
			{
				const Stmt& st = stmts[s];
				switch (st.kind)
				{
				case SK::Empty: return Flow::Normal;
				case SK::Expr: Eval(st.expr, ctx, frame); break;
				case SK::Decl:
					for (const auto& d : st.decls)
					{
						const Type& t = active->slotTypes[d.first];
						frame[d.first] = d.second >= 0 ? Convert(Eval(d.second, ctx, frame), t) : Convert(Value::Int(0), t);
					}
					break;
				case SK::Block:
					for (int c : st.stmts)
					{
						const Flow f = Exec(c, ctx, frame, ret);
						if (f != Flow::Normal || !ctx.error.empty()) return f;
					}
					break;
				case SK::If:
				{
					const bool c = Truthy(Eval(st.expr, ctx, frame));
					if (!ctx.error.empty()) return Flow::Return;
					if (c) return Exec(st.body, ctx, frame, ret);
					if (st.elseBody >= 0) return Exec(st.elseBody, ctx, frame, ret);
					break;
				}
				case SK::For:
				case SK::While:
				{
					if (st.init >= 0) Exec(st.init, ctx, frame, ret);
					for (int guard = 0; ; ++guard)
					{
						if (guard > (1 << 24)) { ctx.error = "loop limit exceeded"; return Flow::Return; }
						if (st.expr >= 0 && !Truthy(Eval(st.expr, ctx, frame))) break;
						if (!ctx.error.empty()) return Flow::Return;
						const Flow f = Exec(st.body, ctx, frame, ret);
						if (!ctx.error.empty()) return Flow::Return;
						if (f == Flow::Return) return f;
						if (f == Flow::Break) break;
						if (st.expr2 >= 0) Eval(st.expr2, ctx, frame);
					}
					break;
				}
				case SK::Return:
					if (st.expr >= 0) ret = Eval(st.expr, ctx, frame);
					return Flow::Return;
				case SK::Break: return Flow::Break;
				case SK::Continue: return Flow::Continue;
				}
				return ctx.error.empty() ? Flow::Normal : Flow::Return;
			}
		};

		Program::Program() : impl(new Impl()) {}
		Program::~Program() {}

		bool Program::Parse(const std::string& source, std::string* error)
		{
			impl.reset(new Impl());
			Impl& m = *impl;
			if (!Tokenize(source, m.toks, error)) return false;
			if (!m.ParseTop())
			{
				if (error) *error = m.err;
				return false;
			}
			// Global constants: evaluated once (literals and constructors of literals only).
			for (const auto& g : m.globalInits)
			{
				if (g.second < 0) { m.globals[g.first] = Convert(Value::Int(0), m.globalTypes[g.first]); continue; }
				Context c;
				std::vector<Value> frame(64);
				m.globals[g.first] = Convert(m.Eval(g.second, c, frame), m.globalTypes[g.first]);
				if (!c.error.empty())
				{
					if (error) *error = "global initialiser: " + c.error;
					return false;
				}
			}
			return true;
		}

		const std::vector<Extern>& Program::Externs() const { return impl->externs; }

		bool Program::HasFunction(const std::string& function) const
		{
			const auto it = impl->functionIndex.find(function);
			return it != impl->functionIndex.end() && impl->functions[it->second].defined;
		}

		bool Program::Call(const std::string& function, Context& ctx, Value& result) const
		{
			const auto it = impl->functionIndex.find(function);
			if (it == impl->functionIndex.end())
			{
				ctx.error = "no function " + function;
				return false;
			}
			const auto& fn = impl->functions[it->second];
			std::vector<Value> frame(fn.nSlots);
			result = impl->Invoke(fn, ctx, frame);
			return ctx.error.empty();
		}
	}
}
