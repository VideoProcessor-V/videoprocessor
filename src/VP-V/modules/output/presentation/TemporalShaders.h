#pragma once

// mpv-format shaders whose output changes per render even for the same input (WP-24 audit M2,
// VP-ONLY, pure). libplacebo steps an mpv shader's `frame` counter and its PRNG (`random`) on every
// pass invocation, and `//!BUFFER` storage persists across frames. A reused repeat would freeze such
// a shader (and leave its counter behind, so later full renders differ from a run with reuse off),
// so these hooks are never treated as static: the loaders note each parsed shader here, and the
// repeat reuse classes a noted hook as opaque.
//
// Thread-safe (the loaders may run on the settings thread, the classification on the render thread).

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace VpRepeatReuse
{
	// True when the shader text uses `frame` or `random` as identifiers, or declares a //!BUFFER
	// (storage) block. Comments are skipped; any other use of the words counts (conservative).
	inline bool ShaderTextIsTemporal(const char* text, size_t length)
	{
		if (!text)
			return false;
		const std::string s(text, length);
		if (s.find("//!BUFFER") != std::string::npos)
			return true;
		auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
		size_t i = 0;
		while (i < s.size())
		{
			if (s.compare(i, 2, "//") == 0)
			{
				const size_t end = s.find('\n', i);
				if (end == std::string::npos) break;
				i = end + 1;
				continue;
			}
			if (s.compare(i, 2, "/*") == 0)
			{
				const size_t end = s.find("*/", i + 2);
				if (end == std::string::npos) break;
				i = end + 2;
				continue;
			}
			if (word(s[i]) && (i == 0 || !word(s[i - 1])))
			{
				size_t j = i;
				while (j < s.size() && word(s[j]))
					++j;
				const std::string token = s.substr(i, j - i);
				if (token == "frame" || token == "random")
					return true;
				i = j;
				continue;
			}
			++i;
		}
		return false;
	}

	class TemporalShaderRegistry
	{
	public:
		static TemporalShaderRegistry& Global()
		{
			static TemporalShaderRegistry registry;
			return registry;
		}
		// After each pl_mpv_user_shader_parse: notes the hook when its text is temporal, and moves the
		// shader-set generation (audit L4: a hook re-allocated at the same address is still seen).
		void Note(const void* hook, const char* text, size_t length)
		{
			const bool temporal = ShaderTextIsTemporal(text, length);
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_generation;
			if (temporal)
				m_temporal.insert(hook);
			else
				m_temporal.erase(hook);
		}
		void Forget(const void* hook)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			++m_generation;
			m_temporal.erase(hook);
		}
		bool IsTemporal(const void* hook) const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_temporal.count(hook) != 0;
		}
		uint64_t Generation() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_generation;
		}
	private:
		mutable std::mutex m_mutex;
		std::set<const void*> m_temporal;
		uint64_t m_generation = 0;
	};
}
