// EMF.exe's card "EML content" (content_panel.h; docs/engine_parts/content_loader.md) through the page bridge.
// Page -> host: "content" | "content_rebuild" | "content_remove" | "content_mode" {mode} | "content_take_over" |
// "content_give_back"; host -> page {type: "content", cmd, ok, why, text, panel} (panel: content_panel_json, null
// without a game folder).
// - Build again at the next start: deletes emf\content\client\emf_content_state.json (the loader then builds).
// - Remove the copy: deletes emf\content\client\ with the loader's remove_tree (a link is deleted as a link, never
//   opened, so the game's files behind the shadow's hard links stay whole), only while no folder on the way is a
//   junction (shadow_path_safe).
// - When to apply: [content] mode=auto|on|off in emf\emf_core.ini.
// - Let EMF load EML content (E5): EML's dbghelp.dll (never ShroudForge's) is renamed to eml-dbghelp.dll and recorded
//   in emf\moved.txt first, so EMF's uninstall puts it back; the give-back renames it back and drops the record.
// Every action is refused while the game (or a dedicated server) runs from the folder; in a dedicated server's folder
// they act on its copy, emf\content\server. Nothing here writes a game file, and nothing loads emf_content.dll.
#include <windows.h>

#include "bridge.h"
#include "content_panel.h"
#include "core/json.h"
#include "json_out.h"
#include "locate.h"
#include "server_bridge.h"
#include "loader/content_io.h"
#include "loader/content_shadow.h"
#include "loader/content_state.h"
#include "loader/file_redirect.h"
#include "saves/backup.h"  // size_text
#include "text.h"

namespace emf::app {

namespace {

namespace ld = emf::loader;
using core::json::Value;
using VT = Value::Type;

struct Outcome {
    bool ok = false;
    std::string why, text;
};

Outcome refuse(const std::string& why) { return {false, why, {}}; }
Outcome done(const std::string& text) { return {true, {}, text}; }

bool is_file(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(lp(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The copy's folder: emf\content\client, or emf\content\server in a dedicated server's folder (no enshrouded.exe).
std::wstring shadow_of(const std::wstring& game) {
    const bool server = !is_file(game + L"\\enshrouded.exe") && is_file(game + L"\\enshrouded_server.exe");
    return game + L"\\" + std::wstring(ld::content_target(server).subdir);
}

const std::string kJunction =
    "emf\\content (or a folder in it) is a junction or link, so EMF changes nothing there: it would act somewhere else";

Outcome rebuild(const std::wstring& game) {
    const std::wstring shadow = shadow_of(game);
    if (!ld::shadow_path_safe(game, shadow)) return refuse(kJunction + ".");
    const std::wstring state = shadow + L"\\" + ld::kStateFile;
    if (!ld::cio::stat(state).exists) return done("There is no record of an earlier build: EMF builds at the next start anyway.");
    std::string why;
    if (!ld::shadow_detail::delete_name(state, &why)) return refuse(why + ".");
    return done("EMF builds the EML content again at the next start.");
}

Outcome remove_copy(const std::wstring& game) {
    const std::wstring shadow = shadow_of(game);
    if (!ld::shadow_path_safe(game, shadow)) return refuse(kJunction + ".");
    if (!ld::cio::stat(shadow).exists) return done("There is no copy to remove.");
    std::string why;
    if (!ld::shadow_detail::remove_tree(shadow, &why)) return refuse(why + ". Close anything that has a file of it open, then try again.");
    RemoveDirectoryW((game + L"\\emf\\content").c_str());  // only when empty
    return done("The copy is removed. When EMF applies EML content, the next start builds it again.");
}

Outcome set_mode(const std::wstring& game, const Value& m) {
    const Value* v = m.get("mode");
    const std::string mode = v && v->is(VT::String) ? v->s : std::string();
    if (mode != "auto" && mode != "on" && mode != "off") return refuse("EMF knows the modes auto, on or off.");
    if (!is_dir(game + L"\\emf")) return refuse("EMF is not installed in this folder.");
    if (!WritePrivateProfileStringW(L"content", L"mode", wide(mode).c_str(), (game + L"\\emf\\emf_core.ini").c_str()))
        return refuse("Could not write emf\\emf_core.ini (" + windows_error_text(GetLastError()) + ").");
    return done("Saved. It applies from the next game start.");
}

// moved.txt with `line` added (a line break first when its last line has none).
std::string with_line(std::string text, const std::string& line) {
    if (!text.empty() && text.back() != '\n') text += "\r\n";
    return text + line + "\r\n";
}

// moved.txt without the set-aside lines for `name`.
std::string without_set_aside(const std::string& text, const std::string& name) {
    std::string out;
    size_t from = 0;
    while (from < text.size()) {
        const size_t to = text.find('\n', from);
        const size_t end = to == std::string::npos ? text.size() : to + 1;
        const std::string line = text.substr(from, end - from);
        from = end;
        std::string bare = line;
        while (!bare.empty() && (bare.back() == '\n' || bare.back() == '\r')) bare.pop_back();
        if (bare.rfind("set-aside\t" + name + "\t", 0) == 0) continue;
        out += line;
    }
    return out;
}

Outcome take_over(const std::wstring& game) {
    const std::wstring dbghelp = game + L"\\dbghelp.dll";
    for (const auto& name : content_set_aside_names(game))
        if (is_file(game + L"\\" + wide(name)))
            return refuse("EMF has set EML's dbghelp.dll aside already (it is " + name + " now): give that one back first.");
    if (const std::string why = content_take_over_problem(game); !why.empty()) return refuse(why);  // as the card offers it
    std::wstring name;
    for (int i = 1; i < 100 && name.empty(); ++i) {
        const std::wstring n = i == 1 ? L"eml-dbghelp.dll" : L"eml-dbghelp-" + std::to_wstring(i) + L".dll";
        if (!exists(game + L"\\" + n)) name = n;
    }
    if (name.empty()) return refuse("There are too many eml-dbghelp files in the game folder already.");
    // the record first, so even an interrupted take-over leaves one that EMF's uninstall can use
    const std::wstring record = game + L"\\emf\\moved.txt";
    std::string before;
    const bool had = read_file(record, before, 1u << 20);
    if (!had && exists(record)) return refuse("Could not read emf\\moved.txt. Is another program using it?");
    const Change w = write_file_whole(record, with_line(before, "set-aside\t" + utf8(name) + "\tdbghelp.dll"), "emf\\moved.txt");
    if (!w.ok) return refuse(w.error);
    if (!MoveFileExW(lp(dbghelp).c_str(), lp(game + L"\\" + name).c_str(), 0)) {
        const DWORD e = GetLastError();
        if (had) write_file_whole(record, before, "emf\\moved.txt");
        else DeleteFileW(lp(record).c_str());
        return refuse("Could not rename dbghelp.dll (" + windows_error_text(e) + "). Is another program using it?");
    }
    return done("EML's dbghelp.dll is now " + utf8(name) + ", so EMF builds the EML content at the next start. Give it "
                "back here, or uninstall EMF, and EML loads the EML mods again.");
}

Outcome give_back(const std::wstring& game) {
    std::string name;
    for (const auto& n : content_set_aside_names(game))
        if (name.empty() && is_file(game + L"\\" + wide(n))) name = n;
    if (name.empty()) return refuse("EMF has not set EML's dbghelp.dll aside, so there is nothing to give back.");
    const std::wstring dbghelp = game + L"\\dbghelp.dll";
    if (exists(dbghelp))
        return refuse("There is a dbghelp.dll in the game folder again, and EMF never replaces it. EML's earlier one stays " +
                      name + ": delete that yourself if you do not want it.");
    if (!MoveFileExW(lp(game + L"\\" + wide(name)).c_str(), lp(dbghelp).c_str(), 0))
        return refuse("Could not rename " + name + " back to dbghelp.dll (" + windows_error_text(GetLastError()) + ").");
    const std::wstring record = game + L"\\emf\\moved.txt";
    std::string text;
    if (read_file(record, text, 1u << 20)) {
        const std::string rest = without_set_aside(text, name);
        if (rest.find_first_not_of("\r\n \t") == std::string::npos) DeleteFileW(lp(record).c_str());
        else write_file_whole(record, rest, "emf\\moved.txt");
    }
    return done("EML's dbghelp.dll is back, so EML applies the EML content again at the next start.");
}

}  // namespace

std::string content_panel_json(const ContentPanel& p) {
    std::vector<std::string> mods, left, loses;
    for (const auto& m : p.mods)
        mods.push_back(Obj().str("folder", m.folder).str("id", m.id).str("name", m.name).str("version", m.version)
                           .boolean("built", m.built).text());
    for (const auto& l : p.left_out) left.push_back(Obj().str("folder", l.folder).str("why", l.why).text());
    for (const auto& l : p.take_over_loses) loses.push_back(Obj().str("folder", l.folder).str("why", l.why).text());
    std::string last = "null";
    if (p.last_build) {
        const auto& b = *p.last_build;
        last = Obj().num("code", b.code).str("message", b.message).strs("errors", b.errors)
                   .num("errors_more", static_cast<double>(b.errors_more)).strs("warnings", b.warnings)
                   .num("warnings_more", static_cast<double>(b.warnings_more)).strs("mods", b.mods)
                   .num("ms", static_cast<double>(b.ms)).str("built_at", b.built_at).text();
    }
    return Obj()
        .str("status", content_status_name(p.status)).str("headline", p.headline).str("detail", p.detail).str("why", p.why)
        .str("mode", p.mode)
        .raw("dll", Obj().boolean("present", p.dll.present).num("size", static_cast<double>(p.dll.size))
                        .str("size_text", saves::size_text(p.dll.size)).num("time", static_cast<double>(p.dll.time)).text())
        .raw("mods", json_list(mods)).raw("left_out", json_list(left)).raw("last_build", last)
        .boolean("copy_there", p.copy_there).num("copy_bytes", static_cast<double>(p.copy_bytes))
        .str("copy_text", saves::size_text(p.copy_bytes)).boolean("patched_in_place", p.patched_in_place)
        .str("take_over", take_over_name(p.take_over)).str("other_tool", p.other_tool).str("set_aside", p.set_aside)
        .raw("take_over_loses", json_list(loses))
        .text();
}

std::optional<std::vector<std::string>> Bridge::handle_content(const std::string& cmd, const core::json::Value& m) {
    if (cmd != "content" && cmd != "content_rebuild" && cmd != "content_remove" && cmd != "content_mode" &&
        cmd != "content_take_over" && cmd != "content_give_back")
        return std::nullopt;
    const std::wstring game = host_.game();
    // nothing may end EMF.exe from here (the UI thread): out of memory, say, is an answer like any other
    try {
        const bool found = !game.empty() && is_game_folder(game);
        const bool server = found && !exists(game + L"\\enshrouded.exe");
        const Value* meant = m.get("game");  // the folder the page showed when the player clicked
        Outcome out{true, {}, {}};
        if (!found) {
            out = refuse("Choose the game's folder first.");
        } else if (cmd != "content") {
            const std::string running = host_.running_reason();
            if (!running.empty())
                out = refuse("Close the game first: EMF changes nothing for EML content while it runs (" + running + ").");
            else if (busy_ || desk_->installing())  // as the profile commands: a switch, an install or an unpack runs
                out = refuse("EMF is busy (switching, installing or unpacking). Wait a moment, then try again.");
            else if (meant && meant->is(VT::String) && !(path_inside(game, wide(meant->s)) && path_inside(wide(meant->s), game)))
                out = refuse("The folder changed since the card was drawn: look at it again first.");
            else if (cmd == "content_rebuild")
                out = rebuild(game);
            else if (cmd == "content_remove")
                out = remove_copy(game);
            else if (cmd == "content_mode")
                out = set_mode(game, m);
            else if (cmd == "content_take_over")
                out = take_over(game);
            else
                out = give_back(game);
        }
        const std::string panel = found ? content_panel_json(content_panel(game, server)) : std::string("null");
        std::vector<std::string> replies{Obj().str("type", "content").str("cmd", cmd).str("game", utf8(game))
                                             .boolean("ok", out.ok).str("why", out.why).str("text", out.text)
                                             .raw("panel", panel).text()};
        if (out.ok && cmd != "content")
            if (auto s = state_if_changed(false)) replies.push_back(*s);  // the Mods tab may read differently now
        return replies;
    } catch (const std::exception&) {
        return std::vector<std::string>{Obj().str("type", "content").str("cmd", cmd).str("game", utf8(game)).boolean("ok", false)
                                            .str("why", "EMF could not look at the EML content (out of memory?). Try again.")
                                            .str("text", "").raw("panel", "null").text()};
    }
}

}  // namespace emf::app
