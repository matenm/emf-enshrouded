#include "servers.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <map>
#include <filesystem>
#include <optional>

#include "core/json.h"
#include "json_out.h"
#include "locate.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::wstring without_trailing_slashes(std::wstring dir) {
    for (auto& c : dir)
        if (c == L'/') c = L'\\';  // a folder as Windows writes it
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    return dir;
}

// A full path: "C:\..." or "\\server\share\...".
bool full_path_text(const std::wstring& p) {
    return (p.size() >= 3 && std::iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
           (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\');
}

int int_member(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::Number) && x->n >= 0 && x->n < 1e9 ? static_cast<int>(x->n) : 0;
}

bool bool_member(const Value& v, const char* key, bool* known = nullptr) {
    const Value* x = v.get(key);
    if (known) *known = x && x->is(VT::Bool);
    return x && x->is(VT::Bool) && x->b;
}

std::string text_member(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

// A password field: set when it has a value; its text (a string, or a number as written) goes to `secrets` only.
bool password_of(const Value* x, std::vector<std::string>* secrets) {
    if (!x || x->is(VT::Null)) return false;
    if (x->is(VT::String)) {
        if (x->s.empty()) return false;
        if (secrets) secrets->push_back(x->s);
        return true;
    }
    if (x->is(VT::Number) && secrets) secrets->push_back(json_number(x->n));
    return true;  // anything else in a password field still lets nobody in without it
}

// The values of "password" keys in JSON text that does not parse (a file being edited): what follows each
// "password": as a string (escapes honoured) or a bare token. Only for redact_secrets.
void scan_passwords(const std::string& text, std::vector<std::string>& secrets) {
    const std::string key = "\"password\"";
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + key.size())) {
        size_t i = at + key.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
        if (i >= text.size() || text[i] != ':') continue;
        ++i;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
        std::string v;
        if (i < text.size() && text[i] == '"') {
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                v += text[i];
            }
        } else {
            while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != '\r' && text[i] != '\n') v += text[i++];
            v = trim(v);
            if (v == "null") v.clear();
        }
        if (!v.empty()) secrets.push_back(v);
    }
}

// ---- ini lines ----
struct Line {
    std::string body, eol;  // eol: "\r\n", "\n" or "" (the last line without one)
};

std::vector<Line> split_lines(const std::string& text) {
    std::vector<Line> out;
    size_t from = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\n') continue;
        const bool cr = i > from && text[i - 1] == '\r';
        out.push_back({text.substr(from, i - from - (cr ? 1 : 0)), cr ? "\r\n" : "\n"});
        from = i + 1;
    }
    if (from < text.size()) out.push_back({text.substr(from), ""});
    return out;
}

std::string join_lines(const std::vector<Line>& lines) {
    std::string s;
    for (const auto& l : lines) s += l.body + l.eol;
    return s;
}

// "[name]" -> name (trimmed); nullopt for other lines.
std::optional<std::string> section_of(const std::string& body) {
    const std::string t = trim(body);
    if (t.size() < 2 || t.front() != '[') return std::nullopt;
    const size_t close = t.find(']');
    if (close == std::string::npos) return std::nullopt;
    return trim(t.substr(1, close - 1));
}

// "key=value" -> key (trimmed); nullopt for comments, blank lines and lines without '='.
std::optional<std::string> key_of(const std::string& body, std::string* value = nullptr) {
    const std::string t = trim(body);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return std::nullopt;
    const size_t eq = body.find('=');
    if (eq == std::string::npos) return std::nullopt;
    if (value) *value = trim(body.substr(eq + 1));
    return trim(body.substr(0, eq));
}

// A value as GetPrivateProfileString gives it: one pair of matching quotes around it dropped.
std::string unquote(std::string v) {
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
}

// The keys of the first [section] of an ini text, the first of each, values unquoted (as GetPrivateProfileString takes
// them: a second section of the same name is never read).
std::vector<std::pair<std::string, std::string>> section_values(const std::string& text, const std::string& section) {
    std::vector<std::pair<std::string, std::string>> out;
    bool in = false, done = false;
    for (const auto& l : split_lines(text)) {
        if (const auto s = section_of(l.body)) {
            if (in) done = true;
            in = !done && lower(*s) == lower(section);
            continue;
        }
        std::string value;
        if (in)
            if (const auto k = key_of(l.body, &value)) {
                bool seen = false;
                for (const auto& [key, v] : out) seen = seen || lower(key) == lower(*k);
                if (!seen) out.emplace_back(*k, unquote(value));
            }
    }
    return out;
}

// GetPrivateProfileInt's reading of a value: its leading digits (none: 0).
int ini_int(const std::string& v) { return std::atoi(v.c_str()); }

// emf_core.ini as text: UTF-16LE (BOM FF FE) as UTF-8 and back; anything else stays as its bytes.
struct IniText {
    std::string text;
    bool utf16 = false;
};

IniText decode_ini(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring w((bytes.size() - 2) / 2, L'\0');
        if (!w.empty()) memcpy(w.data(), bytes.data() + 2, w.size() * 2);
        return {utf8(w), true};
    }
    return {bytes, false};
}

std::string encode_ini(const IniText& t) {
    if (!t.utf16) return t.text;
    const std::wstring w = wide(t.text);
    std::string out = "\xFF\xFE";
    out.append(reinterpret_cast<const char*>(w.data()), w.size() * 2);
    return out;
}

bool hex_digit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

uint64_t account_value(std::string e) {
    if (e.size() > 2 && e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) e = e.substr(2);
    return std::strtoull(e.c_str(), nullptr, 16);
}

}  // namespace

// ---- the folders EMF.exe keeps ----

std::string folder_kind(const std::wstring& dir_in) {
    const std::wstring dir = without_trailing_slashes(dir_in);
    if (dir.empty()) return {};
    if (exists(dir + L"\\enshrouded.exe")) return "game";
    if (exists(dir + L"\\enshrouded_server.exe")) return "server";
    return {};
}

bool same_folder(const std::wstring& a, const std::wstring& b) { return same_place(a, b); }

FolderList parse_folder_list(const std::string& text) {
    FolderList out;
    const auto r = core::json::parse(text);
    if (!r.value || !r.value->is(VT::Object)) return out;
    const Value* list = r.value->get("folders");
    if (!list || !list->is(VT::Array)) return out;
    std::optional<FolderEntry> game;
    std::vector<FolderEntry> servers;
    for (const Value& v : list->arr) {
        if (!v.is(VT::Object)) continue;
        FolderEntry e;
        e.kind = text_member(v, "kind");
        e.path = without_trailing_slashes(wide(text_member(v, "path")));
        e.stop_on_close = bool_member(v, "stop_on_close");
        if ((e.kind != "game" && e.kind != "server") || !full_path_text(e.path)) continue;
        bool repeated = game && same_folder(game->path, e.path);
        for (const auto& s : servers) repeated = repeated || same_folder(s.path, e.path);
        if (repeated) continue;
        if (e.kind == "game") {
            if (!game) game = e;
        } else {
            servers.push_back(e);
        }
    }
    if (game) out.folders.push_back(*game);
    for (auto& s : servers)
        if (!game || !same_folder(game->path, s.path)) out.folders.push_back(std::move(s));
    return out;
}

std::string folder_list_text(const FolderList& list) {
    std::vector<std::string> items;
    for (const auto& f : list.folders)
        items.push_back(Obj().str("kind", f.kind).str("path", utf8(f.path)).boolean("stop_on_close", f.stop_on_close).text());
    return Obj().num("version", 1).raw("folders", json_list(items)).text() + "\n";
}

FolderList read_folder_list(const std::wstring& file) {
    std::string text;
    if (!read_file(file, text, 1u << 20)) return {};
    return parse_folder_list(text);
}

Change write_folder_list(const std::wstring& file, const FolderList& list) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    return write_file_whole(file, folder_list_text(list), "folders.json");
}

const FolderEntry* find_folder(const FolderList& list, const std::wstring& dir) {
    for (const auto& f : list.folders)
        if (same_folder(f.path, dir)) return &f;
    return nullptr;
}

FolderEntry* find_folder(FolderList& list, const std::wstring& dir) {
    for (auto& f : list.folders)
        if (same_folder(f.path, dir)) return &f;
    return nullptr;
}

bool remember_folder(FolderList& list, const std::wstring& dir_in, const std::string& kind) {
    const std::wstring dir = without_trailing_slashes(dir_in);
    if (dir.empty() || (kind != "game" && kind != "server")) return false;
    if (kind == "game") {
        if (!list.folders.empty() && list.folders[0].kind == "game") {
            if (same_folder(list.folders[0].path, dir)) return false;
            list.folders[0].path = dir;
        } else {
            list.folders.insert(list.folders.begin(), FolderEntry{"game", dir, false});
        }
        // the same folder kept as a server too would show twice
        for (size_t i = 1; i < list.folders.size(); ++i)
            if (same_folder(list.folders[i].path, dir)) list.folders.erase(list.folders.begin() + static_cast<long>(i--));
        return true;
    }
    if (find_folder(list, dir)) return false;
    list.folders.push_back(FolderEntry{"server", dir, false});
    return true;
}

bool forget_folder(FolderList& list, const std::wstring& dir) {
    for (size_t i = 0; i < list.folders.size(); ++i)
        if (same_folder(list.folders[i].path, dir)) {
            list.folders.erase(list.folders.begin() + static_cast<long>(i));
            return true;
        }
    return false;
}

// ---- server folders through Steam ----

std::vector<std::wstring> find_servers_in_steam(const std::wstring& steam_root) {
    std::vector<std::wstring> out;
    for (const auto& lib : steam_libraries(steam_root)) {
        std::string acf;
        if (!read_file(lib + L"\\steamapps\\" + kServerAppManifest, acf, 1u << 20)) continue;
        std::string dir = acf_installdir(acf);
        if (dir.empty()) dir = "EnshroudedServer";
        const std::wstring folder = lib + L"\\steamapps\\common\\" + wide(dir);
        if (!exists(folder + L"\\enshrouded_server.exe")) continue;
        bool listed = false;
        for (const auto& f : out) listed = listed || same_folder(f, folder);
        if (!listed) out.push_back(folder);
    }
    return out;
}

// ---- enshrouded_server.json ----

ServerConfig parse_server_config(const std::string& json_text, std::vector<std::string>* secrets) {
    ServerConfig c;
    c.found = true;
    const auto r = core::json::parse(json_text);
    if (!r.value) {
        c.error = "enshrouded_server.json is not valid JSON: " + r.error;
        if (secrets) scan_passwords(json_text, *secrets);  // still kept out of the logs EMF shows
        return c;
    }
    const Value& v = *r.value;
    if (!v.is(VT::Object)) {
        c.error = "enshrouded_server.json must hold an object ({...})";
        return c;
    }
    c.name = text_member(v, "name");
    c.ip = text_member(v, "ip");
    c.save_dir = text_member(v, "saveDirectory");
    c.log_dir = text_member(v, "logDirectory");
    c.slots = int_member(v, "slotCount");
    c.query_port = int_member(v, "queryPort");
    c.game_port = int_member(v, "gamePort");
    c.text_chat = bool_member(v, "enableTextChat", &c.text_chat_known);
    c.voice_chat = bool_member(v, "enableVoiceChat");
    c.legacy_password_set = password_of(v.get("password"), secrets);
    if (const Value* groups = v.get("userGroups"); groups && groups->is(VT::Array))
        for (const Value& g : groups->arr) {
            if (!g.is(VT::Object)) continue;
            ServerGroup sg;
            sg.name = text_member(g, "name");
            sg.password_set = password_of(g.get("password"), secrets);
            sg.can_kick_ban = bool_member(g, "canKickBan");
            sg.can_access_inventories = bool_member(g, "canAccessInventories");
            sg.can_edit_world = bool_member(g, "canEditWorld");
            sg.can_edit_base = bool_member(g, "canEditBase");
            sg.can_extend_base = bool_member(g, "canExtendBase");
            sg.reserved_slots = int_member(g, "reservedSlots");
            c.groups.push_back(std::move(sg));
        }
    if (secrets) {  // a key given twice keeps only its last value in the parsed object: the raw text has them all
        std::vector<std::string> raw;
        scan_passwords(json_text, raw);
        for (auto& s : raw)
            if (std::find(secrets->begin(), secrets->end(), s) == secrets->end()) secrets->push_back(std::move(s));
    }
    return c;
}

ServerConfig read_server_config(const std::wstring& folder, std::vector<std::string>* secrets) {
    std::string text;
    if (!read_file(without_trailing_slashes(folder) + L"\\enshrouded_server.json", text, 1u << 20)) return {};
    return parse_server_config(text, secrets);
}

std::string server_config_json(const ServerConfig& c) {
    std::vector<std::string> groups;
    for (const auto& g : c.groups)
        groups.push_back(Obj().str("name", g.name).boolean("password_set", g.password_set).boolean("can_kick_ban", g.can_kick_ban)
                             .boolean("can_access_inventories", g.can_access_inventories).boolean("can_edit_world", g.can_edit_world)
                             .boolean("can_edit_base", g.can_edit_base).boolean("can_extend_base", g.can_extend_base)
                             .num("reserved_slots", g.reserved_slots).text());
    return Obj()
        .boolean("found", c.found).str("error", c.error).str("name", c.name).str("ip", c.ip).num("slots", c.slots)
        .num("query_port", c.query_port).num("game_port", c.game_port).boolean("text_chat", c.text_chat)
        .boolean("text_chat_known", c.text_chat_known).boolean("voice_chat", c.voice_chat)
        .boolean("legacy_password_set", c.legacy_password_set).str("save_dir", c.save_dir).str("log_dir", c.log_dir)
        .raw("groups", json_list(groups))
        .text();
}

std::wstring server_log_path(const std::wstring& folder_in, const ServerConfig& c) {
    const std::wstring folder = without_trailing_slashes(folder_in);
    const std::wstring fallback = folder + L"\\logs\\enshrouded_server.log";
    std::wstring rel = wide(c.log_dir);
    for (auto& ch : rel)
        if (ch == L'/') ch = L'\\';
    while (rel.size() >= 2 && rel[0] == L'.' && rel[1] == L'\\') rel = rel.substr(2);
    while (!rel.empty() && rel.back() == L'\\') rel.pop_back();
    if (rel.empty() || rel == L"." || full_path_text(rel) || rel[0] == L'\\' || rel.find(L':') != std::wstring::npos) return fallback;
    const std::wstring dir = folder + L"\\" + rel;
    if (!path_inside(folder, dir)) return fallback;
    return dir + L"\\enshrouded_server.log";
}

std::string redact_secrets(const std::string& text_in, const std::vector<std::string>& secrets_in) {
    const std::string mask = "******";
    // the passwords themselves first, on the raw text and the longest first (one may contain another), so the word rule
    // below never leaves part of one standing
    std::vector<std::string> secrets;
    for (const auto& s : secrets_in)
        if (s.size() >= 4) secrets.push_back(s);
    std::sort(secrets.begin(), secrets.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::string text = text_in;
    for (const auto& s : secrets)
        for (size_t at = text.find(s); at != std::string::npos; at = text.find(s, at + mask.size())) text.replace(at, s.size(), mask);
    std::string out;
    out.reserve(text.size());
    const std::string low = lower(text);
    size_t i = 0;
    while (i < text.size()) {
        const size_t at = low.find("passw", i);
        if (at == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        size_t k = at + 5;  // the rest of the word ("password", "passwords", "password_hash")
        while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '_')) ++k;
        size_t j = k;
        if (j < text.size() && (text[j] == '"' || text[j] == '\'')) ++j;  // the key's closing quote
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        bool is_word = false;  // "password is x"
        if (j + 3 <= text.size() && low.compare(j, 3, "is ") == 0) {
            is_word = true;
            j += 2;
        } else if (j >= text.size() || (text[j] != ':' && text[j] != '=')) {
            out.append(text, i, k - i);
            i = k;
            continue;
        }
        ++j;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        char quote = 0;
        if (!is_word && j < text.size() && (text[j] == '"' || text[j] == '\'')) quote = text[j++];
        size_t end = j;
        if (quote) {  // to the closing quote, escaped quotes included
            while (end < text.size() && text[end] != quote && text[end] != '\n') end += (text[end] == '\\' && end + 1 < text.size()) ? 2 : 1;
            if (end > text.size()) end = text.size();
        } else {  // a bare value may hold spaces and commas: the rest of the line goes
            while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
        }
        out.append(text, i, j - i);
        if (end > j) out += mask;
        i = end;
    }
    return out;
}

std::string redact_log(const std::string& raw, const std::vector<std::string>& secrets) {
    std::vector<std::string> all;
    const auto add = [&](const std::string& v) {
        if (!v.empty() && std::find(all.begin(), all.end(), v) == all.end()) all.push_back(v);
    };
    for (const auto& s : secrets) {
        add(s);
        const std::wstring w = wide(s);  // as the ANSI code page writes it
        BOOL lossy = FALSE;
        const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, &lossy);
        if (n > 0 && !lossy) {
            std::string a(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), a.data(), n, nullptr, nullptr);
            add(a);
        }
        std::string escaped;  // as a JSON dump writes it
        for (char c : s) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        add(escaped);
    }
    std::string out;
    for (const auto& l : split_lines(redact_secrets(raw, all))) out += to_utf8_text(l.body) + l.eol;
    return out;
}

// ---- EMF's chat settings ----

ChatSettings parse_chat_settings(const std::string& ini_text) {
    ChatSettings s;
    for (const auto& [key, value] : section_values(ini_text, "chat")) {
        const std::string k = lower(key);
        if (k == "players") {
            s.players = ini_int(value) != 0;
            s.has_players_key = true;
        } else if (k == "everyone") {
            s.everyone = ini_int(value) != 0;
        } else if (k == "admins") {
            size_t from = 0;
            while (from <= value.size()) {
                size_t comma = value.find(',', from);
                if (comma == std::string::npos) comma = value.size();
                const std::string item = trim(value.substr(from, comma - from));
                if (!item.empty()) s.admins.push_back(item);
                from = comma + 1;
            }
        }
    }
    return s;
}

std::optional<std::string> ini_value(const std::string& text, const std::string& section, const std::string& key) {
    for (const auto& [k, v] : section_values(text, section))
        if (lower(k) == lower(key)) return v;
    return std::nullopt;
}

std::string admins_value(const std::vector<std::string>& admins) {
    std::string s;
    for (size_t i = 0; i < admins.size(); ++i) s += (i ? ", " : "") + admins[i];
    return s;
}

ChatSettings read_chat_settings(const std::wstring& folder) {
    std::string bytes;
    read_file(without_trailing_slashes(folder) + L"\\emf\\emf_core.ini", bytes, 1u << 20);
    return parse_chat_settings(decode_ini(bytes).text);
}

std::string set_ini_value(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<Line> lines = split_lines(text);
    std::string nl = "\r\n";
    for (const auto& l : lines)
        if (!l.eol.empty()) {
            nl = l.eol;
            break;
        }
    const std::string sec = lower(section), k = lower(key);
    std::optional<size_t> header;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto s = section_of(lines[i].body);
        if (!s) continue;
        if (header) break;  // the end of the section
        if (lower(*s) == sec) header = i;
    }
    if (!header) {
        if (!lines.empty()) {
            if (lines.back().eol.empty()) lines.back().eol = nl;
            if (!trim(lines.back().body).empty()) lines.push_back({"", nl});
        }
        lines.push_back({"[" + section + "]", nl});
        lines.push_back({key + "=" + value, nl});
        return join_lines(lines);
    }
    size_t last = *header;  // the section's last line that is not blank
    for (size_t i = *header + 1; i < lines.size(); ++i) {
        if (section_of(lines[i].body)) break;
        std::string old;
        if (const auto found = key_of(lines[i].body, &old); found && lower(*found) == k) {
            if (old == value) return text;
            lines[i].body = lines[i].body.substr(0, lines[i].body.find('=') + 1) + value;
            return join_lines(lines);
        }
        if (!trim(lines[i].body).empty()) last = i;
    }
    if (lines[last].eol.empty()) lines[last].eol = nl;
    lines.insert(lines.begin() + static_cast<long>(last) + 1, Line{key + "=" + value, nl});
    return join_lines(lines);
}

bool valid_account_hash(const std::string& entry) {
    std::string e = entry;
    if (e.size() > 2 && e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) e = e.substr(2);
    if (e.empty() || e.size() > 16) return false;
    for (char c : e)
        if (!hex_digit(c)) return false;
    return true;
}

AdminList parse_admin_lines(const std::string& text) {
    AdminList out;
    std::vector<uint64_t> seen;
    std::string token;
    const auto flush = [&] {
        if (token.empty()) return;
        if (!valid_account_hash(token)) {
            out.bad.push_back(token);
        } else {
            const uint64_t v = account_value(token);
            bool dup = false;
            for (uint64_t s : seen) dup = dup || s == v;
            if (!dup) {
                seen.push_back(v);
                out.ok.push_back(token);
            }
        }
        token.clear();
    };
    for (char c : text) {
        if (c == '\n' || c == '\r' || c == ',' || c == ' ' || c == '\t') flush();
        else token += c;
    }
    flush();
    return out;
}

Change save_chat_settings(const std::wstring& folder_in, const ChatSettings& s) {
    const std::wstring folder = without_trailing_slashes(folder_in);
    const std::wstring path = folder + L"\\emf\\emf_core.ini";
    const size_t max = 1u << 20;
    std::string bytes;
    const bool there = read_file(path, bytes, max + 1);
    if (!there && exists(path)) return {false, "Could not read emf\\emf_core.ini."};
    if (bytes.size() > max) return {false, "emf\\emf_core.ini is too large to edit here (over 1 MB)."};
    IniText ini = decode_ini(bytes);
    const ChatSettings before = parse_chat_settings(ini.text);
    ini.text = set_ini_value(ini.text, "chat", "admins", admins_value(s.admins));
    ini.text = set_ini_value(ini.text, "chat", "everyone", s.everyone ? "1" : "0");
    if (s.players || before.has_players_key) ini.text = set_ini_value(ini.text, "chat", "players", s.players ? "1" : "0");
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(folder + L"\\emf"), ec);
    return write_file_whole(path, encode_ini(ini), "emf\\emf_core.ini");
}

std::vector<RefusedAccount> refused_accounts(const std::string& log_text) {
    std::vector<RefusedAccount> out;
    const std::string marker = "chat command refused from account 0x";
    const std::string tail = " (to allow it, add the account to [chat] admins in emf\\emf_core.ini)";
    for (const auto& line : split_lines(log_text)) {
        const std::string& l = line.body;
        // "[11:06:10.000 t2] ": the time, a space, 't' and the thread's number
        if (l.size() < 20 || l[0] != '[' || l[3] != ':' || l[6] != ':' || l[9] != '.' || l[13] != ' ' || l[14] != 't') continue;
        const size_t close = l.find("] ", 15);
        if (close == std::string::npos) continue;
        bool digits = close > 15;
        for (size_t i = 15; i < close; ++i) digits = digits && std::isdigit(static_cast<unsigned char>(l[i]));
        if (!digits || l.compare(close + 2, marker.size(), marker) != 0) continue;
        const size_t hex = close + 2 + marker.size();
        if (l.size() < hex + 18 || l.compare(hex + 16, 2, ": ") != 0) continue;
        bool ok = true;
        for (size_t i = hex; i < hex + 16; ++i) ok = ok && hex_digit(l[i]);
        if (!ok) continue;
        RefusedAccount a;
        a.account = "0x" + l.substr(hex, 16);
        a.time = l.substr(1, 8);
        a.command = l.substr(hex + 18);
        if (a.command.size() >= tail.size() && a.command.compare(a.command.size() - tail.size(), tail.size(), tail) == 0)
            a.command.resize(a.command.size() - tail.size());
        for (size_t k = 0; k < out.size(); ++k)
            if (out[k].account == a.account) {
                out.erase(out.begin() + static_cast<long>(k));
                break;
            }
        out.push_back(std::move(a));
    }
    return out;
}

// ---- where a mod runs ----

SideNote server_side_note(const std::string& side, bool declared) {
    if (side == "client") return {"warn", "Its mod.json says it does nothing on a server."};
    if (side == "server") return {"info", "Its mod.json says it runs on the server; players do not need it."};
    if (declared) return {"info", "Its mod.json says players need it too."};
    return {"info", "Players may need this mod too: its mod.json does not say where it runs (\"side\")."};
}

void add_server_notes(FolderState& s) {
    std::vector<Warning> notes;
    for (auto& m : s.mods) {
        if (m.source != "emf" && m.source != "eml") continue;  // DLL mods and other loaders' mods: EMF cannot tell
        const SideNote n = server_side_note(m.side, m.side_declared);
        m.server_note = n.text;
        m.server_level = n.level;
        if (!m.side_error.empty()) notes.push_back({"warn", m.name + ": " + m.side_error + "."});
        if (m.side == "client" && m.enabled)
            notes.push_back({"warn", m.name + " does nothing on a server (its mod.json says \"side\": \"client\"): switch it off "
                                     "here; players keep it in their own games."});
    }
    s.warnings.insert(s.warnings.end(), notes.begin(), notes.end());
}

// ---- installing EMF into a server folder ----

namespace {

bool has_scripts(const std::wstring& dir) { return exists(dir + L"\\install.ps1") && exists(dir + L"\\emf_common.ps1"); }

std::wstring parent_dir(const std::wstring& dir) {
    const std::wstring d = without_trailing_slashes(dir);
    const size_t at = d.find_last_of(L"\\/");
    return at == std::wstring::npos ? std::wstring() : d.substr(0, at);
}

// A relative path from emf\installed.txt that is safe to copy: inside the folder, no drive, no "..", not the game's
// own exe or data, not the player's settings.
bool stageable(const std::string& rel) {
    if (rel.empty() || rel.size() > 400 || rel.find(':') != std::string::npos || rel[0] == '\\' || rel[0] == '/') return false;
    std::string low = lower(rel);
    for (auto& c : low)
        if (c == '/') c = '\\';
    if (low.find("..") != std::string::npos) return false;
    for (const char* file : {"dinput8.dll", "emf\\emf_core.dll", "emf\\emf.exe", "emf\\known_tools.txt"})
        if (low == file) return true;
    for (const char* folder : {"emf\\ui\\", "emf\\vk\\", "emf\\mods\\", "emf\\setup\\"}) {
        const size_t n = std::strlen(folder);
        if (low.size() > n && low.compare(0, n, folder) == 0) return true;
    }
    return false;  // the user's settings, EMF's records, the game's files: never
}

// The text a process printed: UTF-8 as it is, else the console's OEM code page (Windows PowerShell's default).
std::string console_text(const std::string& bytes) {
    if (bytes.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0) > 0)
        return bytes;
    const int n = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), static_cast<int>(bytes.size()), w.data(), n);
    return utf8(w);
}

// Runs `cmd` without a window in a job that ends with this process, collecting what it prints (stdout and stderr).
ScriptRun run_captured(const std::wstring& exe, std::wstring cmd, unsigned timeout_ms, const std::atomic<bool>* cancel) {
    ScriptRun r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        r.error = "EMF could not run PowerShell: " + windows_error_text(GetLastError());
        return r;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // the child inherits exactly its two standard handles, not every inheritable handle EMF.exe has
    HANDLE inherit[2] = {write_end, nul};
    const DWORD inherit_count = nul != INVALID_HANDLE_VALUE ? 2 : 1;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    const bool listed = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, inherit_count * sizeof(HANDLE),
                                                  nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.StartupInfo.hStdOutput = write_end;
    si.StartupInfo.hStdError = write_end;
    si.lpAttributeList = listed ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    }
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (listed ? EXTENDED_STARTUPINFO_PRESENT : 0);
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                                        reinterpret_cast<STARTUPINFOW*>(&si), &pi);
    const DWORD start_error = GetLastError();
    if (listed) DeleteProcThreadAttributeList(attrs);
    CloseHandle(write_end);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(read_end);
        if (job) CloseHandle(job);
        r.error = "EMF could not run PowerShell: " + windows_error_text(start_error);
        return r;
    }
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {  // then it is ended by itself on a timeout or a cancel
        CloseHandle(job);
        job = nullptr;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string out;
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    bool stopped = false;
    for (;;) {
        if ((cancel && *cancel) || GetTickCount64() >= until) {
            stopped = true;
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
        DWORD avail = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (ReadFile(read_end, buf, sizeof buf, &got, nullptr) && got > 0 && out.size() < (1u << 20)) out.append(buf, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
            // what is left in the pipe
            char buf[4096];
            DWORD got = 0;
            while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0 &&
                   ReadFile(read_end, buf, sizeof buf, &got, nullptr) && got > 0)
                if (out.size() < (1u << 20)) out.append(buf, got);
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(read_end);
    if (job) CloseHandle(job);
    for (const auto& l : split_lines(console_text(out)))
        if (!trim(l.body).empty()) r.lines.push_back(l.body);
    r.exit_code = static_cast<int>(code);
    r.ok = !stopped && code == 0;
    if (stopped) r.error = (cancel && *cancel) ? "Cancelled." : "The installer took longer than five minutes and was stopped.";
    else if (code != 0) {
        r.error = "The installer stopped: ";
        for (const auto& l : r.lines)  // PowerShell's error record starts with "<where> : <what>"
            if (l.find(" : ") != std::string::npos || l.rfind("Exception", 0) == 0) {
                r.error += trim(l.substr(l.find(" : ") == std::string::npos ? 0 : l.find(" : ") + 3));
                return r;
            }
        r.error += r.lines.empty() ? "it printed nothing (exit code " + std::to_string(code) + ")." : r.lines.back();
    }
    return r;
}

}  // namespace

EmfPackage find_emf_package(const std::wstring& exe_dir_in) {
    EmfPackage p;
    const std::wstring exe_dir = without_trailing_slashes(exe_dir_in);
    const std::wstring parent = parent_dir(exe_dir);
    if (has_scripts(exe_dir + L"\\setup")) p.scripts = exe_dir + L"\\setup";
    else if (!parent.empty() && has_scripts(parent_dir(parent))) p.scripts = parent_dir(parent);  // <package>\install.ps1
    const std::string by_hand = " Install EMF into the server with install.cmd from the EMF zip: install.cmd \"<server folder>\".";
    if (p.scripts.empty()) {
        p.error = "This EMF.exe has no EMF package with it (setup\\install.ps1 next to it)." + by_hand;
        return p;
    }
    if (parent.empty() || !exists(parent + L"\\dinput8.dll") || !exists(parent + L"\\emf\\emf_core.dll")) {
        p.error = "EMF's files are not next to this EMF.exe (dinput8.dll and emf\\emf_core.dll in " + utf8(parent) + ")." + by_hand;
        return p;
    }
    if (folder_kind(parent).empty()) {
        p.tree = parent;  // an unpacked package's game\ folder
    } else if (exists(parent + L"\\emf\\installed.txt")) {
        p.installed_in = parent;
    } else {
        p.error = "EMF's files in " + utf8(parent) + " are not listed in emf\\installed.txt (an install from before EMF 0.2)." + by_hand;
        return p;
    }
    p.version = emf_core_version(parent + L"\\emf\\emf_core.dll");
    // the mods it brings: the package's emf\mods, or those the game's installed.txt lists (not mods installed since)
    std::vector<std::wstring> manifests;
    if (!p.tree.empty()) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(std::filesystem::path(lp(parent + L"\\emf\\mods")), ec), end; !ec && it != end;
             it.increment(ec))
            if (it->is_directory(ec)) manifests.push_back(parent + L"\\emf\\mods\\" + it->path().filename().wstring() + L"\\mod.json");
    } else {
        std::string list;
        read_file(parent + L"\\emf\\installed.txt", list, 1u << 20);
        for (const auto& l : split_lines(list)) {
            const std::string rel = lower(trim(l.body));
            if (rel.rfind("emf\\mods\\", 0) == 0 && rel.size() > 18 && rel.compare(rel.size() - 9, 9, "\\mod.json") == 0 &&
                rel.find('\\', 9) == rel.size() - 9 && stageable(rel))
                manifests.push_back(parent + L"\\" + wide(trim(l.body)));
        }
    }
    for (const auto& path : manifests) {
        std::string text;
        if (!read_file(path, text, core::kMaxManifestBytes + 1)) continue;
        const auto r = core::parse_manifest(text);
        if (!r.manifest) continue;
        EmfPackage::Mod mod{r.manifest->id, r.manifest->name, "both", r.manifest->enabled_default};
        if (auto v = core::json::parse(text); v.value && v.value->is(VT::Object)) mod.side = read_mod_side(*v.value).side;
        p.mods.push_back(std::move(mod));
    }
    std::sort(p.mods.begin(), p.mods.end(), [](const EmfPackage::Mod& a, const EmfPackage::Mod& b) { return a.id < b.id; });
    p.ok = true;
    return p;
}

std::string stage_installed_emf(const std::wstring& game_in, const std::wstring& stage) {
    const std::wstring game = without_trailing_slashes(game_in);
    std::string list;
    if (!read_file(game + L"\\emf\\installed.txt", list, 1u << 20)) return "emf\\installed.txt could not be read.";
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(lp(stage)), ec);
    std::filesystem::create_directories(std::filesystem::path(lp(stage)), ec);
    if (ec) return "EMF could not make " + utf8(stage) + ": " + ec.message();
    size_t copied = 0;
    for (const auto& l : split_lines(list)) {
        const std::string rel = trim(l.body);
        if (!stageable(rel)) continue;
        const std::wstring from = game + L"\\" + wide(rel), to = stage + L"\\" + wide(rel);
        if (!path_inside(game, from) || !is_dir(parent_dir(from)) || !exists(from) || is_dir(from)) continue;
        std::filesystem::create_directories(std::filesystem::path(lp(parent_dir(to))), ec);
        if (!CopyFileW(lp(from).c_str(), lp(to).c_str(), FALSE)) return "EMF could not copy " + rel + ": " + windows_error_text(GetLastError());
        ++copied;
    }
    if (!exists(stage + L"\\dinput8.dll") || !exists(stage + L"\\emf\\emf_core.dll"))
        return "emf\\installed.txt does not list EMF's loader and core (dinput8.dll, emf\\emf_core.dll).";
    return copied ? std::string() : "emf\\installed.txt lists no files.";
}

ScriptRun install_emf_into(const EmfPackage& pkg, const std::wstring& server_in, const std::atomic<bool>* cancel) {
    ScriptRun r;
    if (!pkg.ok) {
        r.error = pkg.error;
        return r;
    }
    const std::wstring server = without_trailing_slashes(server_in);
    std::wstring tree = pkg.tree, stage;
    if (tree.empty()) {
        wchar_t tmp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        stage = std::wstring(tmp, n > 0 && n <= MAX_PATH ? n : 0) + L"emf-setup\\" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64());
        if (const std::string e = stage_installed_emf(pkg.installed_in, stage); !e.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(std::filesystem::path(lp(stage)), ec);
            r.error = e;
            return r;
        }
        tree = stage;
    }
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring ps = std::wstring(sys, n) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::wstring cmd = command_line_arg(ps) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                             command_line_arg(pkg.scripts + L"\\install.ps1") + L" -Game " + command_line_arg(server) +
                             L" -From " + command_line_arg(without_trailing_slashes(tree));
    r = run_captured(ps, cmd, 5 * 60 * 1000, cancel);
    if (!stage.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(lp(stage)), ec);
    }
    return r;
}

std::wstring command_line_arg(const std::wstring& arg) {
    std::wstring out = L"\"" + arg;
    size_t slashes = 0;  // backslashes right before the closing quote are doubled, so they stay backslashes
    for (size_t i = arg.size(); i > 0 && arg[i - 1] == L'\\'; --i) ++slashes;
    out.append(slashes, L'\\');
    return out + L"\"";
}

}  // namespace emf::app
