#include "nxm_handler.h"

#include <windows.h>
#include <shlobj.h>

#include <cwctype>
#include <vector>

#include "text.h"

namespace emf::app {

namespace {

const std::wstring kNxmKey = L"Software\\Classes\\nxm";
const std::wstring kShellKey = L"Software\\Classes\\nxm\\shell";
const std::wstring kOpenKey = L"Software\\Classes\\nxm\\shell\\open";
const std::wstring kCommandKey = L"Software\\Classes\\nxm\\shell\\open\\command";
const std::wstring kBackupKey = L"Software\\EMF\\NxmBackup";

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring file_part(const std::wstring& path) {
    const size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? path : path.substr(at + 1);
}

bool same_path(std::wstring a, std::wstring b) {
    for (auto& c : a)
        if (c == L'/') c = L'\\';
    for (auto& c : b)
        if (c == L'/') c = L'\\';
    return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::string hex(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::optional<std::string> unhex(const std::string& text) {
    if (text.size() % 2) return std::nullopt;
    const auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::string out;
    for (size_t i = 0; i < text.size(); i += 2) {
        const int a = v(text[i]), b = v(text[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        out += static_cast<char>(a * 16 + b);
    }
    return out;
}

// The kinds of value the backup keeps exactly.
bool kept_kind(unsigned long type) {
    return type == REG_NONE || type == REG_SZ || type == REG_EXPAND_SZ || type == REG_BINARY || type == REG_DWORD ||
           type == REG_MULTI_SZ || type == REG_QWORD;
}

// What EMF changes in HKCU\Software\Classes\nxm, as it is now.
struct Saved {
    bool had_key = false, had_shell = false, had_open = false, had_command = false;
    std::optional<RegValue> def, url_protocol, command;
};

Saved read_current(RegistryLayer& reg) {
    Saved s;
    s.had_key = reg.exists(RegistryLayer::Hive::User, kNxmKey);
    s.had_shell = reg.exists(RegistryLayer::Hive::User, kShellKey);
    s.had_open = reg.exists(RegistryLayer::Hive::User, kOpenKey);
    s.had_command = reg.exists(RegistryLayer::Hive::User, kCommandKey);
    s.def = reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L"");
    s.url_protocol = reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L"URL Protocol");
    s.command = reg.get_raw(RegistryLayer::Hive::User, kCommandKey, L"");
    return s;
}

// Why `s` cannot be put back exactly ("" when it can).
std::string not_exact(const Saved& s) {
    for (const auto* v : {&s.def, &s.url_protocol, &s.command}) {
        if (!*v) continue;
        if ((*v)->too_large) return "a value there is larger than EMF keeps";
        if (!kept_kind((*v)->type)) return "a value there is of a kind EMF does not know (type " + std::to_string((*v)->type) + ")";
    }
    return {};
}

bool backed_up(RegistryLayer& reg) { return reg.get(RegistryLayer::Hive::User, kBackupKey, L"had_key").has_value(); }

bool write_backup(RegistryLayer& reg, const Saved& s, const std::string& label) {
    bool ok = true;
    const auto flag = [&](const wchar_t* name, bool on) { ok = ok && reg.set(kBackupKey, name, on ? L"1" : L"0"); };
    flag(L"had_key", s.had_key);
    flag(L"had_shell", s.had_shell);
    flag(L"had_open", s.had_open);
    flag(L"had_command", s.had_command);
    const auto put = [&](const std::wstring& name, const std::optional<RegValue>& v) {
        flag((name + L"_present").c_str(), v.has_value());
        if (!v) return;
        ok = ok && reg.set(kBackupKey, name + L"_type", std::to_wstring(v->type));
        ok = ok && reg.set(kBackupKey, name + L"_data", wide(hex(v->bytes)));
    };
    put(L"default", s.def);
    put(L"url_protocol", s.url_protocol);
    put(L"command", s.command);
    ok = ok && reg.set(kBackupKey, L"previous_label", wide(label));
    return ok;
}

std::optional<Saved> read_backup(RegistryLayer& reg) {
    Saved s;
    const auto flag = [&](const wchar_t* name) { return reg.get(RegistryLayer::Hive::User, kBackupKey, name).value_or(L"0") == L"1"; };
    s.had_key = flag(L"had_key");
    s.had_shell = flag(L"had_shell");
    s.had_open = flag(L"had_open");
    s.had_command = flag(L"had_command");
    bool ok = true;
    const auto take = [&](const std::wstring& name) -> std::optional<RegValue> {
        if (!flag((name + L"_present").c_str())) return std::nullopt;
        const auto type = reg.get(RegistryLayer::Hive::User, kBackupKey, name + L"_type");
        const auto data = reg.get(RegistryLayer::Hive::User, kBackupKey, name + L"_data");
        const auto bytes = data ? unhex(utf8(*data)) : std::nullopt;
        if (!type || type->empty() || type->find_first_not_of(L"0123456789") != std::wstring::npos || type->size() > 10 || !bytes) {
            ok = false;
            return std::nullopt;
        }
        RegValue v;
        v.type = static_cast<unsigned long>(std::stoul(*type));
        v.bytes = *bytes;
        return v;
    };
    s.def = take(L"default");
    s.url_protocol = take(L"url_protocol");
    s.command = take(L"command");
    if (!ok) return std::nullopt;
    return s;
}

// Puts the nxm key back as `s` describes it: its values with their types and bytes, and without the keys EMF made.
bool restore(RegistryLayer& reg, const Saved& s) {
    if (!s.had_key) return reg.remove_tree(kNxmKey);
    bool ok = true;
    const auto put = [&](const std::wstring& key, const wchar_t* name, const std::optional<RegValue>& v) {
        ok = (v ? reg.set_raw(key, name, *v) : reg.remove_value(key, name)) && ok;
    };
    put(kNxmKey, L"", s.def);
    put(kNxmKey, L"URL Protocol", s.url_protocol);
    if (s.had_command) put(kCommandKey, L"", s.command);
    if (!s.had_shell) ok = reg.remove_tree(kShellKey) && ok;
    else if (!s.had_open) ok = reg.remove_tree(kOpenKey) && ok;
    else if (!s.had_command) ok = reg.remove_tree(kCommandKey) && ok;
    return ok;
}

std::string label_for(const std::wstring& program, const std::wstring& exe, bool& ours, bool& emf) {
    ours = emf = false;
    const std::wstring base = lower(file_part(program));
    if (same_path(program, exe)) {
        ours = emf = true;
        return "EMF";
    }
    if (base == L"emf.exe") {
        emf = true;
        return "another copy of EMF (" + utf8(program) + ")";
    }
    if (base == L"vortex.exe") return "Vortex";
    if (base == L"nxmhandler.exe" || base == L"modorganizer.exe") return "Mod Organizer 2";
    if (base == L"nexusclient.exe" || base == L"nexusclientcli.exe") return "Nexus Mod Manager";
    if (base == L"nexusmods.app.exe") return "the Nexus Mods app";
    return utf8(file_part(program));
}

// ---- the Windows registry ----
class WindowsRegistry final : public RegistryLayer {
public:
    explicit WindowsRegistry(bool tell_explorer) : tell_explorer_(tell_explorer) {}
    std::optional<RegValue> get_raw(Hive hive, const std::wstring& key, const std::wstring& name) override {
        HKEY k = nullptr;
        LSTATUS r = RegOpenKeyExW(root(hive), key.c_str(), 0, KEY_QUERY_VALUE, &k);
        if (r != ERROR_SUCCESS) {
            if (r != ERROR_FILE_NOT_FOUND) last_ = r;
            return std::nullopt;
        }
        const wchar_t* value = name.empty() ? nullptr : name.c_str();
        DWORD type = 0, size = 0;
        r = RegQueryValueExW(k, value, nullptr, &type, nullptr, &size);
        std::optional<RegValue> out;
        if (r == ERROR_SUCCESS) {
            RegValue v;
            v.type = type;
            if (size > kMaxRegValueBytes) {
                v.too_large = true;
            } else {
                v.bytes.resize(size);
                r = RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(v.bytes.data()), &size);
                if (r == ERROR_MORE_DATA) v.too_large = true;  // it grew in between
                v.bytes.resize(r == ERROR_SUCCESS ? size : 0);
            }
            if (r == ERROR_SUCCESS || v.too_large) out = std::move(v);
        }
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND && r != ERROR_MORE_DATA) last_ = r;
        RegCloseKey(k);
        return out;
    }
    bool exists(Hive hive, const std::wstring& key) override {
        HKEY h = nullptr;
        const LSTATUS r = RegOpenKeyExW(root(hive), key.c_str(), 0, KEY_READ, &h);
        if (r != ERROR_SUCCESS) return false;
        RegCloseKey(h);
        return true;
    }
    bool set_raw(const std::wstring& key, const std::wstring& name, const RegValue& v) override {
        HKEY h = nullptr;
        LSTATUS r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr);
        if (r != ERROR_SUCCESS) return failed(r);
        r = RegSetValueExW(h, name.empty() ? nullptr : name.c_str(), 0, v.type, reinterpret_cast<const BYTE*>(v.bytes.data()),
                           static_cast<DWORD>(v.bytes.size()));
        RegCloseKey(h);
        return r == ERROR_SUCCESS || failed(r);
    }
    bool remove_value(const std::wstring& key, const std::wstring& name) override {
        const LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, key.c_str(), name.empty() ? nullptr : name.c_str());
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || failed(r);
    }
    bool remove_tree(const std::wstring& key) override {
        LSTATUS r = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) return failed(r);
        r = RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND || failed(r);
    }
    void notify() override {
        if (tell_explorer_) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    long last_status() const override { return last_; }

private:
    static HKEY root(Hive h) { return h == Hive::User ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
    bool failed(LSTATUS r) {
        last_ = r;
        return false;
    }
    bool tell_explorer_;
    long last_ = 0;
};

}  // namespace

// ---- RegistryLayer's text helpers ----

std::optional<std::wstring> RegistryLayer::get(Hive hive, const std::wstring& key, const std::wstring& name) {
    const auto v = get_raw(hive, key, name);
    if (!v || v->too_large || (v->type != REG_SZ && v->type != REG_EXPAND_SZ)) return std::nullopt;
    std::wstring s(v->bytes.size() / sizeof(wchar_t), L'\0');
    if (!s.empty()) memcpy(s.data(), v->bytes.data(), s.size() * sizeof(wchar_t));
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

bool RegistryLayer::is_expand(Hive hive, const std::wstring& key, const std::wstring& name) {
    const auto v = get_raw(hive, key, name);
    return v && v->type == REG_EXPAND_SZ;
}

bool RegistryLayer::set(const std::wstring& key, const std::wstring& name, const std::wstring& value, bool expand) {
    RegValue v;
    v.type = expand ? REG_EXPAND_SZ : REG_SZ;
    v.bytes.assign(reinterpret_cast<const char*>(value.c_str()), (value.size() + 1) * sizeof(wchar_t));
    return set_raw(key, name, v);
}

// ---- MemoryRegistry ----

bool MemoryRegistry::refuse() {
    last_ = ERROR_ACCESS_DENIED;
    return false;
}

std::optional<RegValue> MemoryRegistry::get_raw(Hive h, const std::wstring& key, const std::wstring& name) {
    auto& keys = hive(h);
    const auto k = keys.find(lower(key));
    if (k == keys.end()) return std::nullopt;
    const auto v = k->second.find(lower(name));
    if (v == k->second.end()) return std::nullopt;
    RegValue out = v->second;
    if (out.bytes.size() > kMaxRegValueBytes) {  // as the real registry reads it
        out.bytes.clear();
        out.too_large = true;
    }
    return out;
}

bool MemoryRegistry::exists(Hive h, const std::wstring& key) {
    const std::wstring low = lower(key);
    for (const auto& [k, values] : hive(h))
        if (k == low || k.rfind(low + L"\\", 0) == 0) return true;
    return false;
}

bool MemoryRegistry::set_raw(const std::wstring& key, const std::wstring& name, const RegValue& value) {
    if (fail_writes) return refuse();
    user_[lower(key)][lower(name)] = value;
    return true;
}

bool MemoryRegistry::remove_value(const std::wstring& key, const std::wstring& name) {
    if (fail_writes) return refuse();
    const auto k = user_.find(lower(key));
    if (k != user_.end()) k->second.erase(lower(name));
    return true;
}

bool MemoryRegistry::remove_tree(const std::wstring& key) {
    if (fail_writes) return refuse();
    const std::wstring low = lower(key);
    for (auto it = user_.begin(); it != user_.end();) {
        if (it->first == low || it->first.rfind(low + L"\\", 0) == 0) it = user_.erase(it);
        else ++it;
    }
    return true;
}

void MemoryRegistry::set_machine(const std::wstring& key, const std::wstring& name, const std::wstring& value) {
    RegValue v;
    v.type = REG_SZ;
    v.bytes.assign(reinterpret_cast<const char*>(value.c_str()), (value.size() + 1) * sizeof(wchar_t));
    machine_[lower(key)][lower(name)] = v;
}

std::unique_ptr<RegistryLayer> windows_registry(bool tell_explorer) { return std::make_unique<WindowsRegistry>(tell_explorer); }

// ---- the handler ----

std::wstring nxm_command_for(const std::wstring& exe) { return L"\"" + exe + L"\" --nxm \"%1\""; }

std::wstring command_program(const std::wstring& command) {
    std::wstring c = command;
    while (!c.empty() && c.front() == L' ') c.erase(c.begin());
    if (c.empty()) return {};
    if (c.front() == L'"') {
        const size_t end = c.find(L'"', 1);
        return c.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    }
    const size_t exe = lower(c).find(L".exe");
    if (exe != std::wstring::npos) return c.substr(0, exe + 4);
    return c.substr(0, c.find(L' '));
}

NxmOwner nxm_owner(RegistryLayer& reg, const std::wstring& exe) {
    NxmOwner o;
    auto cmd = reg.get(RegistryLayer::Hive::User, kCommandKey, L"");
    if (!cmd || cmd->empty()) {
        cmd = reg.get(RegistryLayer::Hive::Machine, kCommandKey, L"");
        o.machine = cmd && !cmd->empty();
    }
    if (!cmd || cmd->empty()) {
        o.label = "nothing";
        o.machine = false;
        return o;
    }
    o.command = *cmd;
    o.program = command_program(*cmd);
    o.label = label_for(o.program, exe, o.ours, o.emf);
    return o;
}

std::string nxm_previous_label(RegistryLayer& reg) {
    if (!backed_up(reg)) return {};
    const std::string label = utf8(reg.get(RegistryLayer::Hive::User, kBackupKey, L"previous_label").value_or(L""));
    return label.empty() ? "nothing" : label;
}

NxmChange nxm_take(RegistryLayer& reg, const std::wstring& exe) {
    NxmChange c;
    if (exe.empty()) {
        c.error = "EMF does not know where EMF.exe is, so it cannot register it for nxm:// links.";
        return c;
    }
    const NxmOwner before = nxm_owner(reg, exe);
    // While EMF (this or another copy) has the links, the backup of the program before it is kept; otherwise (nothing
    // was backed up, or another program took the links since) the backup is made anew from what is there now.
    const bool fresh_backup = !(backed_up(reg) && before.emf && !before.machine);
    if (fresh_backup) {
        const Saved now = read_current(reg);
        if (const std::string why = not_exact(now); !why.empty()) {
            c.error = "EMF could not save the current nxm:// setting exactly (" + why +
                      "), so it changed nothing. The program that has the links keeps them.";
            return c;
        }
        if (!reg.remove_tree(kBackupKey) || !write_backup(reg, now, before.label)) {
            const long status = reg.last_status();
            reg.remove_tree(kBackupKey);
            c.error = "Windows did not let EMF save the current nxm:// setting, so EMF changed nothing: " + windows_error_text(status);
            return c;
        }
    }
    bool ok = true;
    if (!reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L"")) ok = reg.set(kNxmKey, L"", L"URL:NXM Protocol");
    ok = ok && reg.set(kNxmKey, L"URL Protocol", L"");
    ok = ok && reg.set(kCommandKey, L"", nxm_command_for(exe));
    if (!ok) {
        const long status = reg.last_status();
        if (fresh_backup) {
            if (const auto saved = read_backup(reg)) restore(reg, *saved);
            reg.remove_tree(kBackupKey);
        }
        c.error = "Windows did not let EMF register for nxm:// links: " + windows_error_text(status);
        return c;
    }
    reg.notify();
    c.ok = true;
    const std::string previous = nxm_previous_label(reg);
    if (before.ours) c.text = "EMF already opens Nexus's Mod Manager Download links.";
    else if (previous == "nothing" || previous.empty()) c.text = "EMF now opens Nexus's Mod Manager Download links.";
    else c.text = "EMF now opens Nexus's Mod Manager Download links instead of " + previous + ". You can give them back here at any time.";
    return c;
}

NxmChange nxm_give_back(RegistryLayer& reg, const std::wstring& exe) {
    NxmChange c;
    const NxmOwner now = nxm_owner(reg, exe);
    if (!backed_up(reg)) {
        if (!now.emf || now.machine) {
            c.error = "EMF does not open nxm:// links, so there is nothing to give back.";
            return c;
        }
        // EMF's command without a backup (it was lost): take EMF's command out, as if nothing had been there
        if (!reg.remove_tree(kNxmKey)) {
            c.error = "Windows did not let EMF change the nxm:// setting: " + windows_error_text(reg.last_status());
            return c;
        }
        reg.notify();
        c.ok = true;
        c.text = "EMF no longer opens nxm:// links.";
        return c;
    }
    const std::string previous = nxm_previous_label(reg);
    if (!now.emf || now.machine) {  // another program took the links since: leave them alone
        reg.remove_tree(kBackupKey);
        c.ok = true;
        c.text = now.command.empty() ? "Nothing opens nxm:// links now; EMF left that as it is."
                                     : now.label + " opens nxm:// links now; EMF left that as it is.";
        return c;
    }
    const auto saved = read_backup(reg);
    if (!saved) {
        c.error = "EMF's saved nxm:// setting is damaged, so EMF changed nothing. Open " +
                  (previous == "nothing" ? std::string("the program that should open the links") : previous) +
                  " to take the links, or delete HKCU\\Software\\Classes\\nxm.";
        return c;
    }
    if (!restore(reg, *saved)) {
        c.error = "Windows did not let EMF put the nxm:// setting back: " + windows_error_text(reg.last_status());
        return c;
    }
    reg.remove_tree(kBackupKey);
    reg.notify();
    c.ok = true;
    c.text = previous == "nothing" ? "EMF no longer opens nxm:// links (nothing opened them before EMF)."
                                   : previous + " opens Nexus's Mod Manager Download links again.";
    return c;
}

}  // namespace emf::app
