// Copyright(C) 2026 VP-V contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// output.dither / //!PARAM access on a parsed libplacebo hook (namespace CustomHookChain). MFC-free: it needs only
// libplacebo's custom.h and the standard library, so a translation unit without MFC (the Beta test host, through
// DitherNeedGpu.h's HookGuard) uses the same code as the renderer. CustomHookChain.h includes this header, so every
// includer of the chain still sees these functions. The bodies were moved here verbatim from CustomHookChain.cpp
// (GRANT #11b, TQ-CD-M6); no behaviour change.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <libplacebo/shaders/custom.h>

namespace CustomHookChain
{
    namespace HookParameterDetail
    {
        inline bool ParseNumber(const std::string& text, double& value)
        {
            if (text.empty()) return false;
            char* end = nullptr;
            value = std::strtod(text.c_str(), &end);
            return end && *end == '\0' && std::isfinite(value);
        }

        template<class T>
        T Bounded(T value, T low, T high)
        {
            return (std::max)(low, (std::min)(value, high));
        }
    }

    // Sets //!PARAM values from rule parameters by name, clamped to the
    // parameter's declared range. Returns the names that were applied.
    inline std::vector<std::string> ApplyHookParameters(const pl_hook* hook,
        const std::map<std::string, std::string>& parameters)
    {
        using HookParameterDetail::Bounded;
        using HookParameterDetail::ParseNumber;
        std::vector<std::string> applied;
        if (!hook) return applied;
        for (int i = 0; i < hook->num_parameters; ++i)
        {
            const pl_hook_par& parameter = hook->parameters[i];
            if (!parameter.name || !parameter.data) continue;
            const auto configured = parameters.find(parameter.name);
            double value = 0.0;
            if (configured == parameters.end() || !ParseNumber(configured->second, value))
                continue;
            switch (parameter.type)
            {
            case PL_VAR_FLOAT:
                parameter.data->f = static_cast<float>(Bounded(value,
                    static_cast<double>(parameter.minimum.f),
                    static_cast<double>(parameter.maximum.f)));
                break;
            case PL_VAR_SINT:
                parameter.data->i = static_cast<int>(Bounded(std::llround(value),
                    static_cast<long long>(parameter.minimum.i),
                    static_cast<long long>(parameter.maximum.i)));
                break;
            case PL_VAR_UINT:
                parameter.data->u = static_cast<unsigned>(Bounded(
                    std::llround((std::max)(value, 0.0)),
                    static_cast<long long>(parameter.minimum.u),
                    static_cast<long long>(parameter.maximum.u)));
                break;
            default:
                continue;
            }
            applied.push_back(parameter.name);
        }
        return applied;
    }

    // Sets one //!PARAM by name, clamped to its range, without reparsing (the
    // per-frame pattern NLS uses for stretch_ratio). False if it is absent.
    inline bool SetHookParameter(const pl_hook* hook, const char* name, double value)
    {
        using HookParameterDetail::Bounded;
        if (!std::isfinite(value) || value != std::floor(value) || std::fabs(value) >= 1e9)
        {
            // Non-integral or large values keep the "%.9g" text round trip.
            char text[64];
            std::snprintf(text, sizeof(text), "%.9g", value);
            const auto applied = ApplyHookParameters(hook, { { name, text } });
            return !applied.empty();
        }
        if (!hook || !name) return false;
        // Integral and below 1e9: the "%.9g" text parses back to exactly this value.
        bool applied = false;
        for (int i = 0; i < hook->num_parameters; ++i)
        {
            const pl_hook_par& parameter = hook->parameters[i];
            if (!parameter.name || !parameter.data || std::strcmp(parameter.name, name) != 0)
                continue;
            switch (parameter.type)
            {
            case PL_VAR_FLOAT:
            {
                const float next = static_cast<float>(Bounded(value,
                    static_cast<double>(parameter.minimum.f),
                    static_cast<double>(parameter.maximum.f)));
                if (parameter.data->f != next) parameter.data->f = next;
                break;
            }
            case PL_VAR_SINT:
            {
                const int next = static_cast<int>(Bounded(std::llround(value),
                    static_cast<long long>(parameter.minimum.i),
                    static_cast<long long>(parameter.maximum.i)));
                if (parameter.data->i != next) parameter.data->i = next;
                break;
            }
            case PL_VAR_UINT:
            {
                const unsigned next = static_cast<unsigned>(Bounded(
                    std::llround((std::max)(value, 0.0)),
                    static_cast<long long>(parameter.minimum.u),
                    static_cast<long long>(parameter.maximum.u)));
                if (parameter.data->u != next) parameter.data->u = next;
                break;
            }
            default:
                continue;
            }
            applied = true;
        }
        return applied;
    }

    inline bool HooksOutput(const pl_hook* hook)
    {
        return hook && (hook->stages & PL_HOOK_OUTPUT) != 0;
    }
}
