// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// output.dither / VP_INCLUDE (W2-C8): a whole line `// VP_INCLUDE <id>` in a custom GLSL hook (a plain comment, never a
// //! directive, so libplacebo itself would ignore it) is replaced by the GLSL the owning module publishes under <id>,
// before templates and parsing. Known ids: motion.output_domain_search (VpMotion::OutputDomainSearch::Glsl, the motion
// engine's output-domain search; Temporal sigma-delta.glsl includes it). The inserted text takes the line ends of the
// including line (CRLF or LF), so an expanded file equals the file the passes were cut from. Header-only: the custom
// hook chain, the GPU tests' shader loaders and the Beta host share this one expansion.
#pragma once

#include <VP-V/modules/motion/estimation/fallback/OutputDomainSearchGlsl.h>

#include <algorithm>
#include <string>

namespace CustomHookChain
{
    // The text published under `id`; false for an unknown id.
    inline bool VpIncludeText(const std::string& id, std::string& text)
    {
        if (id == VpMotion::OutputDomainSearch::kIncludeId)
        {
            text = VpMotion::OutputDomainSearch::Glsl();
            // LF by contract (a raw string literal of a CRLF checkout may carry CRs on some compilers): normalise first, so the
            // including line's own line ends decide.
            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
            return true;
        }
        return false;
    }

    // Expands every `// VP_INCLUDE <id>` line of `source` in place. False (reason set) for an unknown id; true when
    // there is nothing to expand (the source is then untouched).
    inline bool ExpandVpIncludes(std::string& source, std::string& reason)
    {
        static const char kPrefix[] = "// VP_INCLUDE ";
        const size_t prefixLength = sizeof(kPrefix) - 1;
        std::string out;
        bool expanded = false;
        size_t start = 0;
        while (start < source.size())
        {
            size_t end = source.find('\n', start);
            const bool lastLine = end == std::string::npos;
            if (lastLine) end = source.size();
            const size_t next = lastLine ? end : end + 1;   // the start of the following line
            size_t contentEnd = end;
            const bool crlf = contentEnd > start && source[contentEnd - 1] == '\r';
            if (crlf) --contentEnd;
            if (contentEnd - start > prefixLength && source.compare(start, prefixLength, kPrefix) == 0)
            {
                std::string id = source.substr(start + prefixLength, contentEnd - start - prefixLength);
                while (!id.empty() && (id.back() == ' ' || id.back() == '\t')) id.pop_back();
                std::string text;
                if (!VpIncludeText(id, text))
                {
                    reason = "unknown VP_INCLUDE id \"" + id + "\"";
                    return false;
                }
                if (!expanded)
                {
                    out.reserve(source.size() + text.size() * 2);
                    out.assign(source, 0, start);
                    expanded = true;
                }
                if (crlf)
                    for (size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at + 2))
                        text.insert(at, 1, '\r');
                out += text;
                out.append(source, contentEnd, next - contentEnd);   // the including line's own line end
            }
            else if (expanded)
                out.append(source, start, next - start);
            start = next;
        }
        if (expanded)
            source.swap(out);
        return true;
    }
}
