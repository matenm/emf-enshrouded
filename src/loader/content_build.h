#pragma once
// One build through emf_content.dll (content_abi.h), the way the licence rules want it: load it, check its ABI, call it
// once, check that no thread it started still runs (module_threads.h), unload it, and check that it is gone. Every
// outcome becomes a code and a message, never an exception. Called on one thread from start to end (the DLL wants the
// build and FreeLibrary on the same thread).
//   codes 0-5   the DLL's own (content_abi.h); 4 (the reply did not fit) means the result is unknown
//   below 0     the loader's: the DLL could not be loaded, has no exports, another ABI, a reply that is not JSON...
// A thread still running in the DLL after the build keeps it loaded (unloading would pull its code away): `unloaded`
// is false then, and the loader starts neither the chained DLLs nor EMF's core.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "content_abi.h"
#include "content_io.h"
#include "core/json.h"
#include "module_threads.h"

namespace emf::loader {

constexpr int kBuildNoDll = -1;         // LoadLibrary failed
constexpr int kBuildNoExport = -2;      // the two exports are not there
constexpr int kBuildWrongAbi = -3;      // emf_content_abi() is not 1
constexpr int kBuildBadReply = -4;      // the reply is not the JSON object it should be
constexpr int kBuildError = -5;         // the loader itself failed (out of memory)
constexpr int kBuildShadowFailed = -6;  // the shadow copy could not be made, so no build ran (content_start.h)
constexpr int kBuildRunning = -7;       // the state while a build runs: a crash or a kill leaves it, never current
constexpr int kBuildTimedOut = -8;      // the build did not come back in time (content_start.h)
constexpr int kBuildAbandoned = -9;     // the build was given up on before emf_content.dll was loaded

constexpr size_t kReplyCapacity = 1u << 20;

struct BuildOutcome {
    int code = kBuildError;
    std::string message;
    std::vector<std::string> mods;    // "<id> <version>"
    std::vector<std::string> errors;  // "<path>: <error>" for each mod the DLL could not read
    std::vector<std::string> warnings;  // "<id>: <warning>" for each warning a mod's Lua gave while it built
    uint64_t ms = 0;
    bool unloaded = true;    // the DLL is not in the process any more (or never was)
    bool truncated = false;  // the reply was cut (or did not fit at all): the result is unknown
    // Only this outcome may send the game to the shadow copy.
    bool usable() const { return code == content_abi::EMF_CONTENT_OK && unloaded && !truncated; }
};

using BuildLog = std::function<void(int level, const std::string& line)>;

// The request: always a forced build of `game_dir` (the shadow is made fresh from the base for every build).
inline std::string content_request(const std::wstring& game_dir, const std::string& file_name, bool server) {
    return "{\"game_dir\": " + cio::json_string(cio::to_utf8(game_dir)) + ", \"file_name\": " +
           cio::json_string(file_name) + ", \"force\": true, \"server\": " + (server ? "true" : "false") + "}";
}

namespace detail {
struct LogContext {
    const BuildLog* log = nullptr;
};

// The DLL's log callback: nothing may leave it, so whatever the loader's log throws stays here.
inline void build_log_line(void* ctx, int level, const char* line) noexcept {
    try {
        const auto* c = static_cast<const LogContext*>(ctx);
        if (c && c->log && *c->log && line) (*c->log)(level, line);
    } catch (...) {
    }
}

// Reads the reply into `out` (everything but the code, which the call returned); false when it is not the object.
inline bool read_reply(const std::string& text, BuildOutcome& out) {
    using T = emf::core::json::Value::Type;
    const auto parsed = emf::core::json::parse(text);
    if (!parsed.value || !parsed.value->is(T::Object)) return false;
    const auto& v = *parsed.value;
    if (const auto* m = v.get("message"); m && m->is(T::String)) out.message = m->s;
    if (const auto* ms = v.get("ms"); ms && ms->is(T::Number) && ms->n >= 0) out.ms = static_cast<uint64_t>(ms->n);
    if (const auto* t = v.get("truncated"); t && t->is(T::Bool)) out.truncated = t->b;
    if (const auto* mods = v.get("mods"); mods && mods->is(T::Array))
        for (const auto& m : mods->arr) {
            const auto* id = m.get("id");
            const auto* ver = m.get("version");
            if (id && id->is(T::String))
                out.mods.push_back(id->s + (ver && ver->is(T::String) ? " " + ver->s : std::string()));
        }
    if (const auto* errs = v.get("mod_errors"); errs && errs->is(T::Array))
        for (const auto& e : errs->arr) {
            const auto* path = e.get("path");
            const auto* error = e.get("error");
            out.errors.push_back((path && path->is(T::String) ? path->s : std::string("?")) + ": " +
                                 (error && error->is(T::String) ? error->s : std::string("?")));
        }
    if (const auto* warns = v.get("mod_warnings"); warns && warns->is(T::Array))
        for (const auto& w : warns->arr) {
            const auto* id = w.get("id");
            const auto* warning = w.get("warning");
            out.warnings.push_back((id && id->is(T::String) ? id->s : std::string("?")) + ": " +
                                   (warning && warning->is(T::String) ? warning->s : std::string("?")));
        }
    return true;
}

inline void add_note(BuildOutcome& out, const std::string& note) {
    out.message += (out.message.empty() ? "" : "; ") + note;
}

// FreeLibrary, but only when no thread of the DLL's own still runs; `unloaded` says whether it is gone.
inline void unload(HMODULE module, const std::wstring& dll, BuildOutcome& out) {
    const auto threads = threads_started_in(module);
    if (!threads.empty()) {
        out.unloaded = false;
        add_note(out, std::to_string(threads.size()) + (threads.size() == 1 ? " thread" : " threads") +
                          " started by emf_content.dll still run, so it stays loaded");
        return;
    }
    FreeLibrary(module);
    out.unloaded = GetModuleHandleW(dll.c_str()) == nullptr;
    if (!out.unloaded) add_note(out, "emf_content.dll is still loaded after FreeLibrary");
}
}  // namespace detail

// `reply_capacity`: the reply buffer (tests make it small). `abandoned`: set when the held start gave up waiting for
// this build (its time was up and the game goes on): then the DLL is never loaded, so it can never run while the game
// starts Steam.
inline BuildOutcome run_content_build(const std::wstring& dll, const std::string& request, const BuildLog& log,
                                      size_t reply_capacity = kReplyCapacity,
                                      const std::atomic<bool>* abandoned = nullptr) {
    namespace abi = content_abi;
    BuildOutcome out;
    HMODULE module = nullptr;
    try {
        if (abandoned && abandoned->load()) {
            out.code = kBuildAbandoned;
            out.message = "the held start gave up waiting before emf_content.dll was loaded";
            return out;
        }
        module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module) {
            out.code = kBuildNoDll;
            out.message = "could not load " + cio::to_utf8(dll) + " (Windows error " + std::to_string(GetLastError()) + ")";
            return out;
        }
        const auto version = reinterpret_cast<abi::emf_content_abi_fn>(GetProcAddress(module, "emf_content_abi"));
        const auto build = reinterpret_cast<abi::emf_content_build_fn>(GetProcAddress(module, "emf_content_build"));
        if (!version || !build) {
            out.code = kBuildNoExport;
            out.message = "emf_content.dll does not have the exports emf_content_abi and emf_content_build";
        } else if (const uint32_t v = version(); v != abi::kAbiVersion) {
            out.code = kBuildWrongAbi;
            out.message = "emf_content.dll has ABI " + std::to_string(v) + "; this EMF needs ABI " +
                          std::to_string(abi::kAbiVersion) + " (install the emf_content.dll that came with this EMF)";
        } else {
            std::vector<char> reply(reply_capacity ? reply_capacity : 1, '\0');
            size_t length = 0;
            detail::LogContext ctx{&log};
            const ULONGLONG t0 = GetTickCount64();
            const int32_t rc = build(request.c_str(), &detail::build_log_line, &ctx, reply.data(), reply.size(), &length);
            const uint64_t ms = GetTickCount64() - t0;
            out.code = rc;
            if (rc == abi::EMF_CONTENT_REPLY_TOO_SMALL) {
                out.truncated = true;
                out.message = "the build's reply did not fit, so its result is unknown";
            } else {
                // without the NUL; a cut reply is shorter than *reply_len (which counts the whole one): up to its NUL
                const size_t n = length > 0 && length <= reply.size() ? length - 1 : strnlen(reply.data(), reply.size());
                if (!detail::read_reply(std::string(reply.data(), n), out)) {
                    out.code = kBuildBadReply;
                    out.message = "emf_content.dll's reply (code " + std::to_string(rc) + ") is not the JSON it should be";
                } else if (out.truncated) {
                    detail::add_note(out, "the build's reply was cut, so its result is unknown");
                }
            }
            if (!out.ms) out.ms = ms;
        }
    } catch (...) {
        out.code = kBuildError;
        out.message = "the build stopped on an error in the loader (out of memory?)";
    }
    if (module) detail::unload(module, dll, out);
    return out;
}

}  // namespace emf::loader
