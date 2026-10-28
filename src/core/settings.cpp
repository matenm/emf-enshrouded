#include "settings.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace emf::core {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
bool parse_number(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}
std::optional<bool> parse_bool(const std::string& s) {
    const std::string l = lower(s);
    if (l == "1" || l == "true" || l == "on" || l == "yes") return true;
    if (l == "0" || l == "false" || l == "off" || l == "no") return false;
    return std::nullopt;
}
bool fail(std::string* why, const std::string& text) {
    if (why) *why = text;
    return false;
}

std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.15g", v);
    return b;
}

// A number from the ini that had to change (out of range, off the step, not a number), for the log: a hand-edited 1
// on a slider in steps of 5 becomes 0, which can mean "off". Empty when the meaning stayed (1e1 for 10, yes for 1).
std::string moved_note(const SettingDef& d, const std::string& stored, const std::string& value) {
    if (d.type != SettingType::Int && d.type != SettingType::Float) return "";
    double was = 0.0, now = 0.0;
    if (parse_number(stored, was) && parse_number(value, now) && was == now) return "";
    std::string allowed;
    if (d.has_min && d.has_max) allowed = "from " + num(d.min) + " to " + num(d.max);
    if (d.step > 0) allowed += std::string(allowed.empty() ? "" : " ") + "in steps of " + num(d.step);
    return "setting \"" + d.key + "\": " + stored + " in the ini became " + value +
           (allowed.empty() ? "" : " (allowed: " + allowed + ")");
}

}  // namespace

std::optional<std::string> FileIniBackend::read(const std::string& mod, const std::string& key) {
    const std::wstring file = dir_ + L"\\" + wide(mod) + L".ini";
    wchar_t buf[1024];
    const DWORD n = GetPrivateProfileStringW(wide(mod).c_str(), wide(key).c_str(), L"\x01", buf, 1024, file.c_str());
    if (n == 1 && buf[0] == L'\x01') return std::nullopt;
    return narrow(std::wstring(buf, n));
}

namespace {
// WritePrivateProfileStringW writes the file's own encoding: an ANSI file loses characters outside the code page, a
// UTF-16 file (with its BOM) keeps them. Before the first such value the file becomes UTF-16 (its text kept).
void make_unicode(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string bytes;
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!bytes.empty()) ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
        bytes.resize(got);
        CloseHandle(h);
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
            return;  // already UTF-16
    }
    std::wstring text;
    if (!bytes.empty()) {
        const int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        text.resize(n);
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    }
    HANDLE w = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    const unsigned char bom[2] = {0xFF, 0xFE};
    WriteFile(w, bom, 2, &put, nullptr);
    if (!text.empty()) WriteFile(w, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &put, nullptr);
    CloseHandle(w);
}
}  // namespace

bool FileIniBackend::write(const std::string& mod, const std::string& key, const std::string& value) {
    CreateDirectoryW(dir_.c_str(), nullptr);
    const std::wstring file = dir_ + L"\\" + wide(mod) + L".ini";
    for (unsigned char c : value)
        if (c >= 0x80) {
            make_unicode(file);
            break;
        }
    return WritePrivateProfileStringW(wide(mod).c_str(), wide(key).c_str(), wide(value).c_str(), file.c_str()) != 0;
}

const SettingsStore::Entry* SettingsStore::find(const std::string& mod, const std::string& key) const {
    auto it = mods_.find(mod);
    if (it == mods_.end()) return nullptr;
    for (const auto& e : it->second)
        if (e.def.key == key) return &e;
    return nullptr;
}

bool SettingsStore::declare(const std::string& mod, SettingDef def, std::string* why, Declare mode) {
    if (!valid_mod_id(mod)) return fail(why, "\"" + mod + "\" is not a valid mod id");
    if (!def.problem.empty()) return fail(why, def.problem);  // the manifest found a mistake in this setting only
    std::string e = normalise_setting(def);
    if (!e.empty()) return fail(why, e);
    if (!def.problem.empty()) return fail(why, def.problem);
    if (def.type == SettingType::Key && !def.default_value.empty()) {  // refuses this setting only (the mod loads)
        auto k = setting_value(def, def.default_value, false, nullptr);
        if (!k) return fail(why, "setting \"" + def.key + "\": the default is not a key");
        def.default_value = *k;
    }
    const bool live = mode == Declare::Live;
    const std::string warning = def.warning;
    std::lock_guard<std::mutex> io(io_m_);
    std::optional<std::string> stored = backend_ ? backend_->read(mod, def.key) : std::nullopt;
    const std::string key = def.key;
    std::string value = def.default_value;
    std::optional<std::string> before;  // the value it had when redeclared
    std::string note;                   // a hand-edited value that had to change, for the log
    {
        std::lock_guard<std::mutex> l(m_);
        auto& list = mods_[mod];
        for (auto it = list.begin(); it != list.end(); ++it)
            if (it->def.key == def.key) {  // redeclared: Start keeps the current value if it still fits
                before = it->value;
                if (!live || !backend_) stored = it->value;
                list.erase(it);
                break;
            }
        if (stored)
            if (auto v = setting_value(def, *stored, true, nullptr)) value = *v;
        if (!live && stored && trim(*stored) != value) note = moved_note(def, trim(*stored), value);
        list.push_back({std::move(def), value});
        if (live && before && *before != value) changes_.emplace_back(mod, key);  // the value a mod reads moved
        ++version_;
    }
    // A value that had to be clamped or replaced is written back, so mods that read their ini themselves
    // (GetPrivateProfile*) see the value the Mods tab shows. Never for a live edit: it may be half-way.
    if (!live && stored && trim(*stored) != value && backend_) backend_->write(mod, key, value);
    if (why) *why = warning.empty() ? note : note.empty() ? warning : warning + "; " + note;  // worth a log line
    return true;
}

std::vector<SettingDef> SettingsStore::defs(const std::string& mod) const {
    std::lock_guard<std::mutex> l(m_);
    std::vector<SettingDef> out;
    auto it = mods_.find(mod);
    if (it != mods_.end())
        for (const auto& e : it->second) out.push_back(e.def);
    return out;
}

std::vector<std::string> SettingsStore::mods() const {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::string> out;
    for (const auto& [id, list] : mods_)
        if (!list.empty()) out.push_back(id);
    return out;
}

bool SettingsStore::has(const std::string& mod, const std::string& key) const {
    std::lock_guard<std::mutex> l(m_);
    return find(mod, key) != nullptr;
}

std::string SettingsStore::get(const std::string& mod, const std::string& key) const {
    std::lock_guard<std::mutex> l(m_);
    const Entry* e = find(mod, key);
    return e ? e->value : std::string();
}

int64_t SettingsStore::get_int(const std::string& mod, const std::string& key, int64_t fallback) const {
    const std::string v = get(mod, key);
    double x = 0.0;
    if (!parse_number(v, x)) return fallback;
    return static_cast<int64_t>(std::llround(x));
}

double SettingsStore::get_float(const std::string& mod, const std::string& key, double fallback) const {
    const std::string v = get(mod, key);
    double x = 0.0;
    return parse_number(v, x) ? x : fallback;
}

SettingsStore::SetResult SettingsStore::set(const std::string& mod, const std::string& key, const std::string& value,
                                            std::string* why) {
    std::lock_guard<std::mutex> io(io_m_);  // changes one at a time, in order; readers only wait for m_
    std::string v;
    {
        std::lock_guard<std::mutex> l(m_);
        const Entry* e = find(mod, key);
        if (!e) {
            if (why) *why = "no setting \"" + key + "\" in " + mod;
            return SetResult::UnknownSetting;
        }
        auto checked = setting_value(e->def, value, false, why);
        if (!checked) return SetResult::Invalid;
        if (*checked == e->value) return SetResult::Unchanged;
        v = std::move(*checked);
    }
    if (backend_ && !backend_->write(mod, key, v)) {  // the file write, outside m_
        if (why) *why = "could not write emf\\config\\" + mod + ".ini";
        return SetResult::WriteFailed;
    }
    std::lock_guard<std::mutex> l(m_);
    if (auto* e = const_cast<Entry*>(find(mod, key))) e->value = v;  // still there: entries change only under io_m_
    changes_.emplace_back(mod, key);
    ++version_;
    return SetResult::Ok;
}

bool SettingsStore::enabled(const std::string& mod, bool fallback) const {
    if (!backend_) return fallback;
    auto v = backend_->read(mod, "enabled");
    if (!v) return fallback;
    const std::string t = trim(*v);
    auto b = parse_bool(t);
    if (!b) return fallback;
    if (t != "1" && t != "0") {  // "true", "on": mods read 1/0
        std::lock_guard<std::mutex> io(io_m_);
        backend_->write(mod, "enabled", *b ? "1" : "0");
    }
    return *b;
}

bool SettingsStore::set_enabled(const std::string& mod, bool on) {
    std::lock_guard<std::mutex> io(io_m_);
    if (!backend_ || !backend_->write(mod, "enabled", on ? "1" : "0")) return false;
    std::lock_guard<std::mutex> l(m_);
    changes_.emplace_back(mod, "enabled");
    ++version_;
    return true;
}

std::vector<std::pair<std::string, std::string>> SettingsStore::take_changes() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(changes_);
    return out;
}

uint64_t SettingsStore::version() const {
    std::lock_guard<std::mutex> l(m_);
    return version_;
}

}  // namespace emf::core
