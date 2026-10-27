// The nxm:// handler setting giving the links back exactly: the keys EMF made are removed (an empty HKCU key would hide
// an all-users handler), values come back with their raw types and bytes, values EMF cannot keep exactly are refused,
// and Windows' own words for a refused write. Against the memory registry, and against the real registry API with
// HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE remapped (RegOverridePredefKey) to a scratch key
// HKCU\Software\EMF-test-<guid>, which is removed afterwards. The real HKCU\Software\Classes\nxm and
// HKCU\Software\EMF are never touched: the remap is proven with a probe key before anything else is written.
#include <doctest.h>

#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

#include "app/nxm_handler.h"
#include "app/text.h"

using namespace emf::app;

namespace {
const std::wstring kNxmKey = L"Software\\Classes\\nxm";
const std::wstring kShellKey = L"Software\\Classes\\nxm\\shell";
const std::wstring kCommandKey = L"Software\\Classes\\nxm\\shell\\open\\command";
const std::wstring kExe = L"C:\\Games\\Enshrouded\\emf\\EMF.exe";
const std::wstring kVortex = L"\"%LOCALAPPDATA%\\Programs\\Vortex\\Vortex.exe\" -d \"%1\"";

RegValue sz(const std::wstring& s, unsigned long type = REG_SZ) {
    RegValue v;
    v.type = type;
    v.bytes.assign(reinterpret_cast<const char*>(s.c_str()), (s.size() + 1) * sizeof(wchar_t));
    return v;
}

bool same(const std::optional<RegValue>& a, const RegValue& b) { return a && a->type == b.type && a->bytes == b.bytes; }

// Vortex's registration, with a REG_EXPAND_SZ command, a REG_BINARY "URL Protocol" (odd, but kept) and an icon.
void install_vortex(RegistryLayer& reg) {
    REQUIRE(reg.set_raw(kNxmKey, L"", sz(L"URL:NXM Protocol")));
    REQUIRE(reg.set_raw(kNxmKey, L"URL Protocol", RegValue{REG_BINARY, std::string("\x01\x02", 2)}));
    REQUIRE(reg.set_raw(kNxmKey + L"\\DefaultIcon", L"", sz(L"Vortex.exe,0")));
    REQUIRE(reg.set_raw(kCommandKey, L"", sz(kVortex, REG_EXPAND_SZ)));
}

void check_vortex_back(RegistryLayer& reg) {
    CHECK(same(reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L""), sz(L"URL:NXM Protocol")));
    CHECK(same(reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L"URL Protocol"), RegValue{REG_BINARY, std::string("\x01\x02", 2)}));
    CHECK(same(reg.get_raw(RegistryLayer::Hive::User, kNxmKey + L"\\DefaultIcon", L""), sz(L"Vortex.exe,0")));
    CHECK(same(reg.get_raw(RegistryLayer::Hive::User, kCommandKey, L""), sz(kVortex, REG_EXPAND_SZ)));
    CHECK_FALSE(reg.exists(RegistryLayer::Hive::User, L"Software\\EMF\\NxmBackup"));
}

// HKCU and HKLM remapped to HKCU\Software\EMF-test-<guid>\{User,Machine} for this process, while it lives.
struct ScratchRegistry {
    std::wstring name;
    HKEY root = nullptr, user = nullptr, machine = nullptr;
    bool ok = false;
    ScratchRegistry() {
        GUID g{};
        CoCreateGuid(&g);
        wchar_t buf[64];
        StringFromGUID2(g, buf, 64);
        name = std::wstring(L"Software\\EMF-test-") + buf;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS)
            return;
        if (RegCreateKeyExW(root, L"User", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr) != ERROR_SUCCESS) return;
        if (RegCreateKeyExW(root, L"Machine", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr) != ERROR_SUCCESS) return;
        if (RegOverridePredefKey(HKEY_CURRENT_USER, user) != ERROR_SUCCESS) return;
        if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) != ERROR_SUCCESS) return;
        // prove the remap: a key made through HKEY_CURRENT_USER must appear under the scratch key
        HKEY probe = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"EMF-remap-probe", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &probe, nullptr) != ERROR_SUCCESS)
            return;
        RegCloseKey(probe);
        HKEY seen = nullptr;
        ok = RegOpenKeyExW(user, L"EMF-remap-probe", 0, KEY_READ, &seen) == ERROR_SUCCESS;
        if (seen) RegCloseKey(seen);
        RegDeleteKeyW(HKEY_CURRENT_USER, L"EMF-remap-probe");
    }
    ~ScratchRegistry() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        for (HKEY k : {user, machine, root})
            if (k) RegCloseKey(k);
        if (!name.empty()) {
            RegDeleteTreeW(HKEY_CURRENT_USER, name.c_str());
            RegDeleteKeyW(HKEY_CURRENT_USER, name.c_str());
        }
    }
    // A value in the remapped "HKLM" (the test seeds an all-users handler there; EMF never writes HKLM).
    void seed_machine(const std::wstring& key, const std::wstring& value) const {
        HKEY k = nullptr;
        REQUIRE(RegCreateKeyExW(machine, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS);
        RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
};
}  // namespace

// ---- the memory registry ----

TEST_CASE("app nxm registry: give back removes the keys EMF made inside a key that was there") {
    MemoryRegistry reg;
    REQUIRE(reg.set_raw(kNxmKey + L"\\DefaultIcon", L"", sz(L"tool.exe,0")));  // nxm is there, without shell\open\command
    REQUIRE(nxm_take(reg, kExe).ok);
    CHECK(reg.exists(RegistryLayer::Hive::User, kCommandKey));
    REQUIRE(nxm_give_back(reg, kExe).ok);
    CHECK_FALSE(reg.exists(RegistryLayer::Hive::User, kShellKey));
    CHECK(reg.exists(RegistryLayer::Hive::User, kNxmKey + L"\\DefaultIcon"));
    CHECK_FALSE(reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L"URL Protocol"));
    CHECK_FALSE(reg.get_raw(RegistryLayer::Hive::User, kNxmKey, L""));
}

TEST_CASE("app nxm registry: values come back with their raw types and bytes") {
    MemoryRegistry reg;
    install_vortex(reg);
    REQUIRE(nxm_take(reg, kExe).ok);
    REQUIRE(nxm_give_back(reg, kExe).ok);
    check_vortex_back(reg);
}

TEST_CASE("app nxm registry: a value EMF could not put back exactly is refused, and nothing changes") {
    MemoryRegistry reg;
    install_vortex(reg);
    REQUIRE(reg.set_raw(kNxmKey, L"URL Protocol", RegValue{REG_RESOURCE_LIST, std::string("abc")}));  // a kind EMF does not know
    NxmChange c = nxm_take(reg, kExe);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("exactly") != std::string::npos);
    CHECK(nxm_owner(reg, kExe).label == "Vortex");
    CHECK_FALSE(reg.exists(RegistryLayer::Hive::User, L"Software\\EMF\\NxmBackup"));

    install_vortex(reg);
    REQUIRE(reg.set_raw(kNxmKey, L"", sz(std::wstring(40000, L'x'))));  // 80 KB
    c = nxm_take(reg, kExe);
    CHECK_FALSE(c.ok);
    CHECK(nxm_owner(reg, kExe).label == "Vortex");
}

TEST_CASE("app nxm registry: a refused write is explained in Windows' own words") {
    MemoryRegistry reg;
    install_vortex(reg);
    reg.fail_writes = true;
    const NxmChange c = nxm_take(reg, kExe);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find(windows_error_text(ERROR_ACCESS_DENIED)) != std::string::npos);
}

// ---- the real registry API, remapped to a scratch key ----

TEST_CASE("app nxm registry (real API): nothing was there; take and give back leave no key behind") {
    ScratchRegistry scratch;
    REQUIRE(scratch.ok);
    const auto reg = windows_registry(false);
    CHECK(nxm_owner(*reg, kExe).label == "nothing");
    REQUIRE(nxm_take(*reg, kExe).ok);
    CHECK(nxm_owner(*reg, kExe).ours);
    CHECK(reg->get(RegistryLayer::Hive::User, kNxmKey, L"URL Protocol").has_value());
    REQUIRE(nxm_give_back(*reg, kExe).ok);
    CHECK_FALSE(reg->exists(RegistryLayer::Hive::User, kNxmKey));
    CHECK_FALSE(reg->exists(RegistryLayer::Hive::User, L"Software\\EMF\\NxmBackup"));
}

TEST_CASE("app nxm registry (real API): Vortex's values, types and icon come back byte for byte") {
    ScratchRegistry scratch;
    REQUIRE(scratch.ok);
    const auto reg = windows_registry(false);
    install_vortex(*reg);
    CHECK(nxm_owner(*reg, kExe).label == "Vortex");
    REQUIRE(nxm_take(*reg, kExe).ok);
    CHECK(reg->get_raw(RegistryLayer::Hive::User, kCommandKey, L"")->type == REG_SZ);  // EMF's own command
    REQUIRE(nxm_give_back(*reg, kExe).ok);
    check_vortex_back(*reg);
}

TEST_CASE("app nxm registry (real API): an nxm key with only an icon gets its shell key taken out again") {
    ScratchRegistry scratch;
    REQUIRE(scratch.ok);
    const auto reg = windows_registry(false);
    REQUIRE(reg->set_raw(kNxmKey + L"\\DefaultIcon", L"", sz(L"tool.exe,0")));
    REQUIRE(nxm_take(*reg, kExe).ok);
    REQUIRE(nxm_give_back(*reg, kExe).ok);
    CHECK(reg->exists(RegistryLayer::Hive::User, kNxmKey + L"\\DefaultIcon"));
    CHECK_FALSE(reg->exists(RegistryLayer::Hive::User, kShellKey));
    CHECK_FALSE(reg->get_raw(RegistryLayer::Hive::User, kNxmKey, L"URL Protocol"));
}

TEST_CASE("app nxm registry (real API): an all-users handler shows again after give back") {
    ScratchRegistry scratch;
    REQUIRE(scratch.ok);
    scratch.seed_machine(kCommandKey, L"\"C:\\NMM\\NexusClient.exe\" \"%1\"");
    const auto reg = windows_registry(false);
    NxmOwner o = nxm_owner(*reg, kExe);
    CHECK(o.machine);
    CHECK(o.label == "Nexus Mod Manager");
    REQUIRE(nxm_take(*reg, kExe).ok);
    CHECK(nxm_owner(*reg, kExe).ours);
    REQUIRE(nxm_give_back(*reg, kExe).ok);
    CHECK_FALSE(reg->exists(RegistryLayer::Hive::User, kNxmKey));  // no empty HKCU key that would hide it in HKCR
    o = nxm_owner(*reg, kExe);
    CHECK(o.machine);
    CHECK(o.label == "Nexus Mod Manager");
}

TEST_CASE("app nxm registry (real API): a value of a kind EMF does not know is refused") {
    ScratchRegistry scratch;
    REQUIRE(scratch.ok);
    const auto reg = windows_registry(false);
    install_vortex(*reg);
    REQUIRE(reg->set_raw(kNxmKey, L"URL Protocol", RegValue{REG_RESOURCE_LIST, std::string("abc")}));
    CHECK_FALSE(nxm_take(*reg, kExe).ok);
    CHECK(same(reg->get_raw(RegistryLayer::Hive::User, kCommandKey, L""), sz(kVortex, REG_EXPAND_SZ)));
    CHECK_FALSE(reg->exists(RegistryLayer::Hive::User, L"Software\\EMF\\NxmBackup"));
}
