#pragma once

#include "DisplayRuleExpression.h"

#include <string>
#include <utility>

// VP Renderer DirectShow filter (2026-10-17): the profile rules' `source`
// variable says which host feeds the processing chain - `capture` for
// VideoProcessor.exe (always) and `file` for the DirectShow filter's FileHost
// (MPC-HC / MPC-BE playing a local file). A text variable like `content`;
// rules that do not name it evaluate exactly as before.
namespace SourceRuleVariable
{
	static const char* const kName = "source";
	static const char* const kCapture = "capture";
	static const char* const kFile = "file";

	// Answers `source` with `value` and forwards every other name to `inner`.
	inline DisplayRuleExpression::ValueLookup WithSource(
		DisplayRuleExpression::ValueLookup inner, const char* value)
	{
		const std::string source(value ? value : "");
		return [inner, source](const std::string& name, std::string& result)
		{
			if (name == kName)
			{
				if (source.empty()) return false;
				result = source;
				return true;
			}
			return inner ? inner(name, result) : false;
		};
	}
}
