#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <libplacebo/shaders/custom.h>
#include <microsoft_directshow/MadVRShaderLoader.h>

// Custom GLSL hooks for the VP Renderer: [shader.standard.*] rules with a
// glsl_file. NLS keeps its own geometry-coupled lifecycle in the renderer; this
// chain owns every other selected hook. Entries load lazily on the render
// thread, reload when a template value they use changes, and drop out (one log
// line) on read, template, parse or runtime failure so rendering continues
// with libplacebo's configured dither.
namespace CustomHookChain
{
    // Built-in {{name}} templates available to every custom GLSL rule.
    struct TemplateValues
    {
        int outputWidth = 0;
        int outputHeight = 0;
        int sourceWidth = 0;
        int sourceHeight = 0;
        double refreshHz = 0.0;
        double sourceFps = 0.0;
        // Modularity 2026-10-25 (efficiency audit #8a): {{output_width_q4}} / {{output_width_b16}}
        // (and heights) are the sigma-delta's quarter / block grids only with efficiency_fixes
        // on; off (the default, the Original pathway) they are the full output size as before.
        bool sigmaDeltaGrids = false;
    };

    // Rules this chain owns, in execution order. Like the madVR path, the
    // profile list order leads and the per-rule stage order breaks ties.
    std::vector<ConfiguredShaderRule> SelectRules(
        const std::vector<ConfiguredShaderRule>& selection);

    // Replaces {{name}} with built-ins first, then rule parameters. Fails when a
    // {{identifier}} token remains or a used built-in is not yet known (zero).
    // Efficiency audit 2026-10-24 #8a: ceil((n + 1.5) / 4) and ceil((n + 7.5) / 16) in integers.
    inline int QuarterGrid(int n) { return n > 0 ? (2 * n + 10) / 8 : 0; }
    inline int BlockGrid(int n) { return n > 0 ? (2 * n + 46) / 32 : 0; }

    bool ApplyTemplates(std::string& source, const TemplateValues& values,
        const std::map<std::string, std::string>& parameters, std::string& reason);

    // Identifies the built-in template values a shader source depends on, so
    // unrelated changes (for example source size for an OUTPUT hook) never
    // rebuild it and discard its storage textures.
    std::string TemplateKey(const std::string& source, const TemplateValues& values);

    // Sets //!PARAM values from rule parameters by name, clamped to the
    // parameter's declared range. Returns the names that were applied.
    std::vector<std::string> ApplyHookParameters(const pl_hook* hook,
        const std::map<std::string, std::string>& parameters);

    // Sets one //!PARAM by name, clamped to its range, without reparsing (the
    // per-frame pattern NLS uses for stretch_ratio). False if it is absent.
    bool SetHookParameter(const pl_hook* hook, const char* name, double value);

    inline bool HooksOutput(const pl_hook* hook)
    {
        return hook && (hook->stages & PL_HOOK_OUTPUT) != 0;
    }

    // Audit 2026-10-16: a hook that quantises the output itself (replaces
    // libplacebo's dither). Only such a hook suppresses the dither; any other
    // OUTPUT-stage hook keeps it. Marked by this token anywhere in the shader
    // source (a plain comment line, never a //! directive, which libplacebo
    // would reject), or by the sigma-delta's file name for older copies.
    constexpr const char* OutputQuantizerMarker = "VP_OUTPUT_QUANTIZER";
    constexpr const char* SigmaDeltaFileName = "Temporal sigma-delta.glsl";
    bool IsOutputQuantizer(const std::string& filename, const std::string& source);

    struct Loader
    {
        std::function<bool(const std::string& filename, std::string& source,
            std::string& reason)> read;
        std::function<const pl_hook*(const std::string& source)> parse;
        std::function<void(const pl_hook*& hook)> destroy;
    };

    class Chain
    {
    public:
        explicit Chain(Loader loader);
        ~Chain();
        Chain(const Chain&) = delete;
        Chain& operator=(const Chain&) = delete;

        // Replaces the configured rules and destroys every loaded hook. Returns
        // the signatures of hooks libplacebo had disabled at runtime so the
        // renderer can clear its own record of them.
        std::vector<uint64_t> Configure(const std::vector<ConfiguredShaderRule>& selection);
        void Clear();

        // Loads entries that are new or whose template key changed. Returns
        // true when the set of loaded hooks changed.
        bool Ensure(const TemplateValues& values);

        // Hooks to submit this frame. cinema_only entries are skipped when
        // lowLatency is true.
        void Collect(bool lowLatency, std::vector<const pl_hook*>& hooks) const;

        // True when a hook submitted this frame quantises the OUTPUT stage
        // (IsOutputQuantizer, audit 2026-10-16); the renderer then clears
        // libplacebo's dither for that frame. Other OUTPUT hooks keep it.
        bool SuppressesDither(bool lowLatency) const;

        // Audit 2026-10-16: true when `hook` is a loaded output quantiser of
        // this chain, so the renderer can confirm one is still in the frame's
        // hook list (it drops the sigma-delta on an unsupported code domain).
        bool IsQuantizerHook(const pl_hook* hook) const;

        // Latches entries whose hook libplacebo disabled at runtime. Returns
        // true when at least one entry newly dropped out.
        bool MarkRuntimeFailures(const uint64_t* signatures, int count);

        // True when a submitted rule loads this file (by name, any folder,
        // case-insensitive). Stage 8 needs the SSIM downscaler in the chain.
        bool SubmitsFile(const std::string& fileName, bool lowLatency) const;

        // One description per submitted hook, for the stats overlay.
        std::vector<std::string> ActiveDescriptions(bool lowLatency) const;

        size_t ConfiguredCount() const { return entries.size(); }

    private:
        struct Entry
        {
            ConfiguredShaderRule rule;
            bool sourceRead = false;
            std::string source;
            std::string key;
            const pl_hook* hook = nullptr;
            bool quantizer = false;   // IsOutputQuantizer (audit 2026-10-16)
            bool loadFailed = false;
            bool runtimeFailed = false;
        };

        bool Submitted(const Entry& entry, bool lowLatency) const;
        void DestroyHook(Entry& entry);

        Loader loader;
        std::vector<Entry> entries;
    };
}
