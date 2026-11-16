#pragma once
// What the core tells the player about EML content at this start, from what the loader told it in the environment
// variable EMF_CONTENT_STATUS (src/loader/content_status_env.h; docs/engine_parts/content_loader.md):
//   built / current   one line in the console ("EML content: 3 mods (built in 28 s)"); when the message holds a mod's
//                     warning (a creature pack that could not add a creature), it goes into that line and on screen
//                     once a world loads
//   failed            a line in the console and on screen once a world loads, and a save backup before that world
//                     (mod items vanish when a world loads without their mod): the backup ignores min_minutes
//   off               only with [content] mode=on (the loader sends it only then): why, in the console
// Header-only and pure but for reading the variable.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "loader/content_status_env.h"

namespace emf::core {

struct ContentLines {
    std::string console;  // the in-game console and emf_core.log; "" for none
    std::string notice;   // on screen once, when the first world loads; "" for none
    bool backup = false;  // back up the saves now, whatever min_minutes says
    const char* backup_label = nullptr;  // that backup's label (kContentBackupLabel or kContentPartBackupLabel)
};

constexpr const char* kContentBackupLabel = "before a start without EML content";
constexpr const char* kContentPartBackupLabel = "before a start with part of the EML content missing";

inline std::string content_seconds(uint64_t ms) {
    if (ms < 10000) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(ms) / 1000.0);
        return buf;
    }
    return std::to_string((ms + 500) / 1000) + " s";
}

// `backups_on`: [backup] on_start (a backup is promised only when one will be made).
inline ContentLines content_lines(const std::optional<emf::loader::ContentStatus>& s, bool backups_on) {
    ContentLines l;
    if (!s) return l;
    const std::string mods = std::to_string(s->mods) + (s->mods == 1 ? " mod" : " mods");
    if (s->kind == "built" || s->kind == "current") {
        l.console = "EML content: " + mods +
                    (s->kind == "built" ? " (built in " + content_seconds(s->ms) + ")" : " (unchanged since the last build)");
        // a good build whose mods warned (a creature pack that could not add a creature): its message says which
        if (!s->message.empty()) {
            std::string what = s->message;
            while (!what.empty() && (what.back() == '.' || what.back() == ' ')) what.pop_back();
            l.console += "; a mod reported a problem: " + what;
            l.notice = "An EML mod reported a problem while its content was built: " + what +
                       ". The rest of the content is in; emf\\content.log has every line.";
            // the missing part may be a placed object's template, which a loading world drops: a backup first, once
            // (a current start has the same content as the start that made it, as failed_again)
            if (s->kind == "built") {
                if (backups_on) l.notice += " Your saves were backed up.";
                l.backup = true;
                l.backup_label = kContentPartBackupLabel;
            }
        }
    } else if (s->kind == "failed" || s->kind == "failed_again") {
        std::string why = s->message.empty() ? std::string("an unknown error") : s->message;
        // a Lua error from EML: the mod and its message, not the stack traceback players cannot use (content.log
        // keeps all of it)
        if (const size_t tb = why.find("stack traceback"); tb != std::string::npos) why.erase(tb);
        const std::string runtime = "runtime error: ", chunk = "[string \"mods.";
        if (why.rfind(runtime, 0) == 0) why.erase(0, runtime.size());
        if (why.rfind(chunk, 0) == 0) {
            const size_t end = why.find("\"]:", chunk.size());
            if (end != std::string::npos) {
                std::string rest = why.substr(end + 3);  // "<line>: <message>"
                const size_t colon = rest.find(": ");
                if (colon != std::string::npos) rest = "line " + rest.substr(0, colon) + ": " + rest.substr(colon + 2);
                why = "the mod " + why.substr(chunk.size(), end - chunk.size()) + ": " + rest;
            }
        }
        while (!why.empty() && (why.back() == '.' || why.back() == ' ')) why.pop_back();
        l.console = "EML content could not be built: " + why + ". The game runs without it.";
        if (s->kind == "failed_again") {
            l.console += " EMF tries again when a mod, the game or emf_content.dll changes.";
        } else {
            if (backups_on) l.console += " Your saves were backed up.";
            l.backup = true;
            l.backup_label = kContentBackupLabel;
        }
        l.notice = l.console;
    } else if (s->kind == "off") {
        std::string why = s->message;
        if (why.rfind("off ", 0) == 0) why = why.substr(4);  // "off (...)": the decision's own words
        l.console = "EML content is off " + why;
    }
    return l;
}

// The status in the variable's text (UTF-16, as the environment holds it).
inline std::optional<emf::loader::ContentStatus> content_status_from(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), s.data(), n, nullptr, nullptr);
    return emf::loader::parse_content_status(s);
}

// This process's EMF_CONTENT_STATUS, set by the loader before the core started; nullopt when it set none. Read whole
// (a long "failed" message carries the save backup), then cleared, so nothing the game starts later inherits it.
inline std::optional<emf::loader::ContentStatus> read_content_status() {
    const wchar_t* name = emf::loader::kContentStatusVariable;
    const DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return std::nullopt;
    std::wstring text(need, L'\0');
    const DWORD n = GetEnvironmentVariableW(name, text.data(), need);
    SetEnvironmentVariableW(name, nullptr);
    if (n == 0 || n >= need) return std::nullopt;
    text.resize(n);
    return content_status_from(text);
}

}  // namespace emf::core
