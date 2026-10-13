#include <pch.h>
#include "CustomHookChain.h"

#include <DebugLog.h>
#include <vprenderer/HookInstanceTag.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>

namespace CustomHookChain
{
    namespace
    {
        struct BuiltIn
        {
            const char* name;
            std::string value;
            bool known;
        };

        std::string FormatNumber(double value)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.6g", value);
            return buffer;
        }

        std::vector<BuiltIn> BuiltIns(const TemplateValues& values)
        {
            return {
                { "output_width", std::to_string(values.outputWidth), values.outputWidth > 0 },
                { "output_height", std::to_string(values.outputHeight), values.outputHeight > 0 },
                { "source_width", std::to_string(values.sourceWidth), values.sourceWidth > 0 },
                { "source_height", std::to_string(values.sourceHeight), values.sourceHeight > 0 },
                { "refresh_hz", FormatNumber(values.refreshHz), values.refreshHz > 0.0 },
                { "source_fps", FormatNumber(values.sourceFps), values.sourceFps > 0.0 },
            };
        }

        std::string Token(const std::string& name)
        {
            return "{{" + name + "}}";
        }

        bool ReplaceAll(std::string& source, const std::string& token,
            const std::string& value)
        {
            bool replaced = false;
            for (size_t at = source.find(token); at != std::string::npos;
                at = source.find(token, at + value.size()))
            {
                source.replace(at, token.size(), value);
                replaced = true;
            }
            return replaced;
        }

        bool ParseNumber(const std::string& text, double& value)
        {
            if (text.empty()) return false;
            char* end = nullptr;
            value = std::strtod(text.c_str(), &end);
            return end && *end == '\0' && std::isfinite(value);
        }

        template<class T>
        T Bounded(T value, T low, T high)
        {
            return std::max(low, std::min(value, high));
        }

        std::string HookDescription(const ConfiguredShaderRule& rule)
        {
            return "GLSL: " + rule.filename;
        }
    }

    std::vector<ConfiguredShaderRule> SelectRules(
        const std::vector<ConfiguredShaderRule>& selection)
    {
        std::vector<ConfiguredShaderRule> rules;
        for (const ConfiguredShaderRule& rule : selection)
            if (!rule.nls && !rule.none && !rule.filename.empty())
                rules.push_back(rule);
        std::stable_sort(rules.begin(), rules.end(),
            [](const ConfiguredShaderRule& a, const ConfiguredShaderRule& b)
            {
                if (a.profileSelectionOrder != b.profileSelectionOrder)
                    return a.profileSelectionOrder < b.profileSelectionOrder;
                return a.stageOrder < b.stageOrder;
            });
        return rules;
    }

    bool ApplyTemplates(std::string& source, const TemplateValues& values,
        const std::map<std::string, std::string>& parameters, std::string& reason)
    {
        for (const BuiltIn& builtIn : BuiltIns(values))
        {
            const std::string token = Token(builtIn.name);
            if (source.find(token) == std::string::npos) continue;
            if (!builtIn.known)
            {
                reason = std::string("template {{") + builtIn.name + "}} is not known yet";
                return false;
            }
            ReplaceAll(source, token, builtIn.value);
        }
        for (const auto& parameter : parameters)
            ReplaceAll(source, Token(parameter.first), parameter.second);
        // Only {{identifier}} is a template; GLSL array initialisers such as
        // {{0.0, 0.0}, {2.0, 0.0}} are ordinary code.
        static const std::regex leftover(R"(\{\{\s*[A-Za-z_][A-Za-z0-9_]*\s*\}\})");
        if (std::regex_search(source, leftover))
        {
            reason = "shader contains an unresolved {{parameter}} token";
            return false;
        }
        return true;
    }

    std::string TemplateKey(const std::string& source, const TemplateValues& values)
    {
        std::string key;
        for (const BuiltIn& builtIn : BuiltIns(values))
            if (source.find(Token(builtIn.name)) != std::string::npos)
                key += std::string(builtIn.name) + "=" + builtIn.value + ";";
        return key;
    }

    std::vector<std::string> ApplyHookParameters(const pl_hook* hook,
        const std::map<std::string, std::string>& parameters)
    {
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
                    std::llround(std::max(value, 0.0)),
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

    bool SetHookParameter(const pl_hook* hook, const char* name, double value)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.9g", value);
        const auto applied = ApplyHookParameters(hook, { { name, text } });
        return !applied.empty();
    }

    Chain::Chain(Loader loaderIn) : loader(std::move(loaderIn))
    {
    }

    Chain::~Chain()
    {
        Clear();
    }

    void Chain::DestroyHook(Entry& entry)
    {
        if (entry.hook && loader.destroy)
            loader.destroy(entry.hook);
        entry.hook = nullptr;
    }

    std::vector<uint64_t> Chain::Configure(const std::vector<ConfiguredShaderRule>& selection)
    {
        std::vector<uint64_t> runtimeFailed;
        for (Entry& entry : entries)
            if (entry.runtimeFailed && entry.hook)
                runtimeFailed.push_back(entry.hook->signature);
        Clear();
        for (const ConfiguredShaderRule& rule : SelectRules(selection))
        {
            Entry entry;
            entry.rule = rule;
            entries.push_back(std::move(entry));
        }
        return runtimeFailed;
    }

    void Chain::Clear()
    {
        for (Entry& entry : entries)
            DestroyHook(entry);
        entries.clear();
    }

    bool Chain::Ensure(const TemplateValues& values)
    {
        bool changed = false;
        for (Entry& entry : entries)
        {
            if (entry.runtimeFailed) continue;
            if (!entry.sourceRead)
            {
                entry.sourceRead = true;
                std::string reason;
                if (!loader.read || !loader.read(entry.rule.filename, entry.source, reason))
                {
                    entry.loadFailed = true;
                    DebugLog::Log("Custom hooks: \"%s\" file=%s not loaded: %s",
                        entry.rule.name.c_str(), entry.rule.filename.c_str(), reason.c_str());
                    continue;
                }
            }
            if (entry.source.empty()) continue;
            const std::string key = TemplateKey(entry.source, values);
            if (key == entry.key && (entry.hook || entry.loadFailed)) continue;
            if (entry.hook) changed = true;
            DestroyHook(entry);
            entry.key = key;
            entry.loadFailed = false;
            std::string text = entry.source;
            std::string reason;
            if (!ApplyTemplates(text, values, entry.rule.parameters, reason))
            {
                // An unknown built-in is transient (no output yet); retry next
                // frame without latching or logging.
                if (reason.find("not known yet") != std::string::npos)
                {
                    entry.key.clear();
                    continue;
                }
                entry.loadFailed = true;
                DebugLog::Log("Custom hooks: \"%s\" file=%s not loaded: %s",
                    entry.rule.name.c_str(), entry.rule.filename.c_str(), reason.c_str());
                continue;
            }
            entry.hook = loader.parse ? loader.parse(text) : nullptr;
            if (!entry.hook)
            {
                entry.loadFailed = true;
                DebugLog::Log("Custom hooks: \"%s\" file=%s not loaded: libplacebo rejected the shader",
                    entry.rule.name.c_str(), entry.rule.filename.c_str());
                continue;
            }
            if (entry.hook->stages == 0)
            {
                // libplacebo accepts an unknown //!HOOK name (e.g. PRE_OUTPUT for
                // PREOUTPUT) and the hook then never runs: report it instead.
                if (loader.destroy) loader.destroy(entry.hook);
                entry.hook = nullptr;
                entry.loadFailed = true;
                DebugLog::Log("Custom hooks: \"%s\" file=%s not loaded: no known //!HOOK stage",
                    entry.rule.name.c_str(), entry.rule.filename.c_str());
                continue;
            }
            changed = true;
            const auto applied = ApplyHookParameters(entry.hook, entry.rule.parameters);
            // A stateful shader's own validity tag (HookInstanceTag.h); no-op
            // for shaders without the parameter.
            SetHookParameter(entry.hook, "instance_tag", NextHookInstanceTag());
            std::string unused;
            for (const auto& parameter : entry.rule.parameters)
                if (std::find(applied.begin(), applied.end(), parameter.first) == applied.end() &&
                    entry.source.find(Token(parameter.first)) == std::string::npos)
                    unused += (unused.empty() ? "" : ",") + parameter.first;
            DebugLog::Log(
                "Custom hooks: loaded \"%s\" file=%s stages=0x%x output_stage=%d params_set=%zu templates=\"%s\"%s%s%s",
                entry.rule.name.c_str(), entry.rule.filename.c_str(),
                static_cast<unsigned>(entry.hook->stages), HooksOutput(entry.hook) ? 1 : 0,
                applied.size(), key.c_str(), entry.rule.cinemaOnly ? " cinema_only=1" : "",
                unused.empty() ? "" : " unused_params=", unused.c_str());
        }
        return changed;
    }

    bool Chain::Submitted(const Entry& entry, bool lowLatency) const
    {
        return entry.hook && !entry.loadFailed && !entry.runtimeFailed &&
            !(lowLatency && entry.rule.cinemaOnly);
    }

    void Chain::Collect(bool lowLatency, std::vector<const pl_hook*>& hooks) const
    {
        for (const Entry& entry : entries)
            if (Submitted(entry, lowLatency))
                hooks.push_back(entry.hook);
    }

    bool Chain::SuppressesDither(bool lowLatency) const
    {
        return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry)
            {
                return Submitted(entry, lowLatency) && HooksOutput(entry.hook);
            });
    }

    bool Chain::MarkRuntimeFailures(const uint64_t* signatures, int count)
    {
        bool changed = false;
        for (Entry& entry : entries)
        {
            if (!entry.hook || entry.runtimeFailed) continue;
            for (int i = 0; i < count; ++i)
            {
                if (signatures[i] != entry.hook->signature) continue;
                entry.runtimeFailed = true;
                changed = true;
                DebugLog::Log(
                    "Custom hooks: \"%s\" file=%s disabled by libplacebo at runtime; configured dither resumes until the shader selection changes",
                    entry.rule.name.c_str(), entry.rule.filename.c_str());
                break;
            }
        }
        return changed;
    }

    bool Chain::SubmitsFile(const std::string& fileName, bool lowLatency) const
    {
        for (const Entry& entry : entries)
        {
            if (!Submitted(entry, lowLatency))
                continue;
            const std::string& path = entry.rule.filename;
            const size_t slash = path.find_last_of("\\/");
            const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
            if (_stricmp(name.c_str(), fileName.c_str()) == 0)
                return true;
        }
        return false;
    }

    std::vector<std::string> Chain::ActiveDescriptions(bool lowLatency) const
    {
        std::vector<std::string> descriptions;
        for (const Entry& entry : entries)
            if (Submitted(entry, lowLatency))
                descriptions.push_back(HookDescription(entry.rule));
        return descriptions;
    }
}
