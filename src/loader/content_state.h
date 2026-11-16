#pragma once
// <shadow>\emf_content_state.json: what the last build of the EML content did, written after every build (as .tmp,
// then renamed). The next start compares its fingerprint (content_fingerprint.h) with the one recorded here: equal,
// with a build that worked, means the shadow copy is current and emf_content.dll is not loaded. A crash in the middle
// of a build leaves the old state, or none, or one without code 0: never current, so the next start builds again. EMF's
// core reads it too, to tell the player how the content went (src/core/content_status.h).
//   {"fingerprint": "sha256:...", "code": 0, "message": "...", "mods": ["<id> <version>"], "errors": ["..."],
//    "ms": 28000, "built_at": "2026-11-01T20:00:00Z", "unloaded": true, "truncated": false}
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "content_io.h"
#include "core/json.h"

namespace emf::loader {

constexpr const wchar_t* kStateFile = L"emf_content_state.json";

struct ContentState {
    std::string fingerprint;
    int code = -5;  // content_build.h's codes
    std::string message;
    std::vector<std::string> mods, errors;
    std::vector<std::string> warnings;  // a good build's mods that warned ("<id>: <warning>"): said again at each start
    uint64_t ms = 0;
    std::string built_at;    // UTC, "YYYY-MM-DDTHH:MM:SSZ"
    bool unloaded = true;    // emf_content.dll was unloaded after the build
    bool truncated = false;  // its reply was cut: the result is unknown
    std::vector<std::string> chain_checked;  // "<dll> <size> <time>": chained DLLs read at this build, not EML's
    int attempts = 0;  // a build that is running (kBuildRunning): how many starts in a row began it
    bool reported = false;  // EMF's core was told of this outcome (and backed up the saves after a failure)
};

inline std::string state_json(const ContentState& s) {
    const auto list = [](const std::vector<std::string>& v) {
        std::string out = "[";
        for (size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + cio::json_string(v[i]);
        return out + "]";
    };
    return "{\"fingerprint\": " + cio::json_string(s.fingerprint) + ", \"code\": " + std::to_string(s.code) +
           ", \"message\": " + cio::json_string(s.message) + ", \"mods\": " + list(s.mods) + ", \"errors\": " +
           list(s.errors) + ", \"warnings\": " + list(s.warnings) + ", \"ms\": " + std::to_string(s.ms) +
           ", \"built_at\": " + cio::json_string(s.built_at) +
           ", \"unloaded\": " + (s.unloaded ? "true" : "false") + ", \"truncated\": " + (s.truncated ? "true" : "false") +
           ", \"chain_checked\": " + list(s.chain_checked) + ", \"attempts\": " + std::to_string(s.attempts) +
           ", \"reported\": " + (s.reported ? "true" : "false") + "}\r\n";
}

// nullopt for anything but a state object with a fingerprint and a code.
inline std::optional<ContentState> parse_state(std::string_view text) {
    using T = emf::core::json::Value::Type;
    const auto parsed = emf::core::json::parse(text);
    if (!parsed.value || !parsed.value->is(T::Object)) return std::nullopt;
    const auto& v = *parsed.value;
    const auto* fp = v.get("fingerprint");
    const auto* code = v.get("code");
    if (!fp || !fp->is(T::String) || !code || !code->is(T::Number)) return std::nullopt;
    ContentState s;
    s.fingerprint = fp->s;
    s.code = static_cast<int>(code->n);
    if (const auto* m = v.get("message"); m && m->is(T::String)) s.message = m->s;
    for (const auto& [key, list] : {std::pair<const char*, std::vector<std::string>*>{"mods", &s.mods}, {"errors", &s.errors},
                                {"warnings", &s.warnings}, {"chain_checked", &s.chain_checked}})
        if (const auto* a = v.get(key); a && a->is(T::Array))
            for (const auto& x : a->arr)
                if (x.is(T::String)) list->push_back(x.s);
    if (const auto* ms = v.get("ms"); ms && ms->is(T::Number) && ms->n >= 0) s.ms = static_cast<uint64_t>(ms->n);
    if (const auto* at = v.get("built_at"); at && at->is(T::String)) s.built_at = at->s;
    if (const auto* u = v.get("unloaded"); u && u->is(T::Bool)) s.unloaded = u->b;
    if (const auto* t = v.get("truncated"); t && t->is(T::Bool)) s.truncated = t->b;
    if (const auto* a = v.get("attempts"); a && a->is(T::Number) && a->n >= 0) s.attempts = static_cast<int>(a->n);
    if (const auto* r = v.get("reported"); r && r->is(T::Bool)) s.reported = r->b;
    return s;
}

inline std::optional<ContentState> read_state(const std::wstring& path) {
    const auto text = cio::read_file(path, 4u << 20);
    if (!text) return std::nullopt;
    return parse_state(*text);
}

inline bool write_state(const std::wstring& path, const ContentState& s) { return cio::write_file_atomic(path, state_json(s)); }

// The shadow copy may be used as it is: the last build worked (code 0, its reply whole, the DLL unloaded) and nothing it
// depends on changed since.
inline bool state_current(const std::optional<ContentState>& s, const std::string& fingerprint) {
    return s && s->code == 0 && s->unloaded && !s->truncated && !s->fingerprint.empty() && s->fingerprint == fingerprint;
}

}  // namespace emf::loader
