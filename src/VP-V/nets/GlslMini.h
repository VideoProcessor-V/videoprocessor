#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// GlslMini: a small interpreter for the GLSL subset the TrainKit exporter writes, so the CPU
// reference executor (CpuNetExecutor.h) can run the SAME pass bodies the GPU will run.
//
// Why an interpreter and not a hand port: the point of the CPU executor is to prove that VP reads
// the exported package correctly (weights, pass order, sizes, bindings) before any GPU wiring. A
// hand port would re-derive the maths from the Python and could agree with the Python while the
// shader text says something else. Interpreting the text tests what will actually ship.
//
// Supported: float/int/bool scalars, vec2-4, ivec2-4 (uvec as ivec), bvec2-4, mat4; const arrays
// (`const vec4 KV[5] = vec4[5](...)`); user functions; if/else, for, while, break, continue,
// return, ?:, && || ! (short-circuit); the arithmetic/compare operators incl. % on ints and
// compound assignment (also into swizzles: `c.x += ...`); swizzles .xyzw/.rgba; texelFetch,
// imageSize/imageStore, and the mpv binding macros NAME_raw/_pos/_size/_pt/_mul and
// NAME_tex()/NAME_texOff() (bilinear, clamp to edge, as libplacebo's sh_bind with LINEAR/CLAMP).
// Constant sub-expressions (the mat4 weight literals) are folded at parse time.
//
// Float maths is 32-bit (float), like the GPU. CPU-only, slow, for tests. C++14.
namespace VpNets
{
	namespace Glsl
	{
		enum class Base : uint8_t { Void, Float, Int, Bool, Sampler, Image };

		struct Value
		{
			Base base = Base::Void;
			uint8_t n = 0;                 // components: 1..4, or 16 for mat4 (column-major)
			float f[16] = {};              // Float components
			int i[4] = {};                 // Int/Bool components; Sampler/Image: binding index in i[0]
			std::shared_ptr<std::vector<Value>> arr;   // arrays (const tables)

			static Value Float(float x) { Value v; v.base = Base::Float; v.n = 1; v.f[0] = x; return v; }
			static Value Int(int x) { Value v; v.base = Base::Int; v.n = 1; v.i[0] = x; return v; }
			static Value Bool(bool x) { Value v; v.base = Base::Bool; v.n = 1; v.i[0] = x ? 1 : 0; return v; }
			static Value Vec(float x, float y, float z, float w)
			{
				Value v; v.base = Base::Float; v.n = 4; v.f[0] = x; v.f[1] = y; v.f[2] = z; v.f[3] = w; return v;
			}
			float F(int k) const { return base == Base::Float ? f[k] : static_cast<float>(i[k]); }
			int I(int k) const { return base == Base::Float ? static_cast<int>(f[k]) : i[k]; }
		};

		// A texture the interpreter reads: 4 floats per texel, row-major.
		struct TexView
		{
			int width = 0, height = 0;
			const float* rgba = nullptr;
		};

		// The compute pass's out_image.
		struct ImageView
		{
			int width = 0, height = 0;
			float* rgba = nullptr;
			int components = 4;
		};

		// Names the shader uses that are not its own: the executor supplies their values.
		enum class ExternKind : uint8_t
		{
			Plain,     // gl_FragCoord, gl_GlobalInvocationID, PARAM labels, ...
			Raw,       // NAME_raw: a sampler (binding = NAME)
			Pos,       // NAME_pos: per-pixel normalised position (vec2)
			Size,      // NAME_size: vec2 texture size
			Pt,        // NAME_pt: 1 / size
			Mul,       // NAME_mul: the normalisation factor (1.0 for float textures)
			Image,     // out_image
		};

		struct Extern
		{
			std::string name;      // as written
			ExternKind kind = ExternKind::Plain;
			std::string binding;   // NAME for the macro kinds
		};

		struct Program;

		// Per-invocation state the executor fills: one Value per extern (Program::externs order),
		// the textures (indexed by Value::i[0] of the Raw externs) and the compute target.
		struct Context
		{
			std::vector<Value> externs;
			std::vector<TexView> textures;
			ImageView image;
			std::string error;      // a runtime error stops the pass
		};

		struct Program
		{
			Program();
			~Program();
			Program(const Program&) = delete;
			Program& operator=(const Program&) = delete;

			// Parses a pass body (helper functions, constants, hook()). False + *error on failure.
			bool Parse(const std::string& source, std::string* error);

			// The externs the body references (resolved after Parse).
			const std::vector<Extern>& Externs() const;

			// Calls a function with no arguments (the pass entry "hook"). The result is Void for a
			// void function. False + ctx.error on a runtime error.
			bool Call(const std::string& function, Context& ctx, Value& result) const;

			bool HasFunction(const std::string& function) const;

		private:
			struct Impl;
			std::unique_ptr<Impl> impl;
		};
	}
}
