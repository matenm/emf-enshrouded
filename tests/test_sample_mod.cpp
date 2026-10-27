// Loads the real sample_mod.dll with a fake API table and drives its tick callback.
#include <doctest.h>
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "emf/emf_api.h"

namespace {
std::vector<std::string> g_log;
std::vector<std::pair<EmfWorldCallback, void*>> g_ticks;
EmfWorld* const kClient = reinterpret_cast<EmfWorld*>(0x7000);
int64_t g_transform[7] = {int64_t(5) << 32, int64_t(3) << 31, -(int64_t(2) << 32), 0, 0, 0, 0};
// a villager: entity 7 with DynamicActiveNpcState (136 bytes: state, previousState, decision first)
uint8_t g_npc_state[136] = {7, 5, 3};
int64_t g_npc_transform[7] = {int64_t(10) << 32, int64_t(20) << 32, int64_t(30) << 32, 0, 0, 0, 0};

void fake_log(const char* mod, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log.push_back(std::string("[") + mod + "] " + buf);
}
uint8_t fake_kind(EmfWorld* w) { return w == kClient ? EMF_WORLD_CLIENT : EMF_WORLD_SERVER; }
uint32_t fake_local_player() { return 42; }
void* fake_get_component(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    if (hash == 0x027ac564u && entity == 42) {
        if (size) *size = 56;
        return g_transform;
    }
    if (hash == 0x027ac564u && entity == 7) {
        if (size) *size = 56;
        return g_npc_transform;
    }
    if (hash == 0x9e65507au && entity == 7) {  // DynamicActiveNpcState's own hash
        if (size) *size = sizeof(g_npc_state);
        return g_npc_state;
    }
    if (size) *size = 0;
    return nullptr;
}
void fake_on_tick(EmfWorldCallback cb, void* user) { g_ticks.push_back({cb, user}); }
EmfSaveCallback g_on_save = nullptr;
EmfSaveCallback g_on_load = nullptr;
void fake_on_save(EmfSaveCallback cb, void*) { g_on_save = cb; }
void fake_on_load(EmfSaveCallback cb, void*) { g_on_load = cb; }
std::string g_registered_program;
EmfProgramCallback g_registered_fn = nullptr;
uint8_t g_registered_kind = 0xFF;
bool fake_register_program(const char* name, uint8_t kind, EmfProgramCallback fn, void*) {
    g_registered_program = name;
    g_registered_kind = kind;
    g_registered_fn = fn;
    return true;
}
std::string g_hooked_program;
EmfProgramCallback g_program_pre = nullptr;
bool fake_hook_program(const char* name, EmfProgramCallback pre, EmfProgramCallback, void*) {
    g_hooked_program = name;
    g_program_pre = pre;
    return true;
}
void fake_for_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    cb(42, user);
    cb(7, user);
}

std::string g_sm_cmd_name;
EmfCommandCallback g_sm_cmd_cb = nullptr;
void* g_sm_cmd_user = nullptr;
std::vector<std::string> g_sm_replies;
bool fake_register_command(const char* name, const char*, EmfCommandCallback cb, void* user) {
    g_sm_cmd_name = name;
    g_sm_cmd_cb = cb;
    g_sm_cmd_user = user;
    return true;
}
void fake_command_reply(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_sm_replies.push_back(buf);
}

std::wstring sample_mod_path() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/")) + L"\\dist\\emf\\mods\\sample_mod\\sample_mod.dll";
}

bool has(const std::string& needle) {
    for (auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

TEST_CASE("sample_mod loads, subscribes to ticks and logs the local player's position") {
    static EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    api.log = fake_log;
    api.world_kind = fake_kind;
    api.local_player = fake_local_player;
    api.get_component = fake_get_component;
    api.on_tick = fake_on_tick;
    api.for_each_entity = fake_for_each;
    api.hook_program = fake_hook_program;
    api.register_program = fake_register_program;
    api.on_save = fake_on_save;
    api.on_load = fake_on_load;
    api.register_command = fake_register_command;
    api.command_reply = fake_command_reply;

    HMODULE m = LoadLibraryW(sample_mod_path().c_str());
    REQUIRE(m != nullptr);
    auto load = reinterpret_cast<EmfModLoadFn>(GetProcAddress(m, "emf_mod_load"));
    REQUIRE(load != nullptr);
    REQUIRE(load(&api));
    CHECK(has("[sample_mod] loaded"));
    REQUIRE_FALSE(g_ticks.empty());
    CHECK(g_hooked_program == "client_player_state");
    REQUIRE(g_program_pre != nullptr);
    g_program_pre(nullptr, nullptr);
    g_program_pre(nullptr, nullptr);
    for (auto& [cb, user] : g_ticks) cb(kClient, user);
    CHECK(has("player 42 at (5.00, 1.50, -2.00) m"));
    CHECK(has("client_player_state ran 2 times"));

    CHECK(g_registered_program == "sample_mod_server_tick");
    CHECK(g_registered_kind == EMF_WORLD_SERVER);
    REQUIRE(g_registered_fn != nullptr);
    for (int i = 0; i < 3; ++i) g_registered_fn(nullptr, nullptr);
    for (auto& [cb, user] : g_ticks) cb(reinterpret_cast<EmfWorld*>(0x8000), user);  // a server world tick
    CHECK(has("sample_mod_server_tick ran 3 times"));
    CHECK(has("villager 7 state=7 previous=5 decision=3 at (10.0, 20.0, 30.0) m"));
    CHECK(has("villagers: 1"));

    REQUIRE(g_on_save != nullptr);
    REQUIRE(g_on_load != nullptr);
    g_on_save(0x0001, nullptr);
    g_on_load(0x0101, nullptr);
    CHECK(has("world saved: slot index 0, Local"));
    CHECK(has("world loaded: slot index 0, Steam"));

    // the sample command lists the villagers of the world it runs on
    CHECK(g_sm_cmd_name == "sample.villagers");
    REQUIRE(g_sm_cmd_cb != nullptr);
    const char* argv[] = {"sample.villagers"};
    g_sm_cmd_cb(reinterpret_cast<EmfWorld*>(0x8000), 1, argv, g_sm_cmd_user);
    bool listed = false;
    for (const auto& r : g_sm_replies) listed |= r.find("villager 7") != std::string::npos;
    CHECK(listed);
}
