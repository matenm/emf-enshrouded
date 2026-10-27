// A fake EmfApi and a temporary mods folder for the Lua host tests (test_lua_host.cpp, test_lua_bindings.cpp).
#pragma once
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "emf/emf_api.h"
#include "test_scratch.h"

namespace {

struct Fake {
    std::vector<std::string> log, notes, replies;
    std::map<std::string, std::string> hud;  // id -> text
    std::map<std::string, std::pair<uint32_t, uint32_t>> hud_style;  // id -> (place, rgba) of its last hud_set
    struct Cmd {
        std::string name, help;
        EmfCommandCallback cb;
        void* user;
        uint32_t flags = 0;  // register_command_ex's EMF_COMMAND_* (v17)
    };
    std::vector<Cmd> commands;
    std::vector<EmfModInfo> mods;
    EmfCommandSender sender{};  // what command_sender reports (size 0: outside a command)
    std::map<std::string, std::string> settings;  // "mod.key" -> value text
    std::vector<std::pair<std::string, std::pair<EmfSettingCallback, void*>>> setting_watchers;
    // worlds, components, items
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> components;  // (hash, entity) -> data
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> templates;   // (hash, entity) -> template half
    std::vector<uint32_t> entities;
    struct Give {
        uint32_t player, item, count;
    };
    std::vector<Give> gives;            // the drops made
    std::set<uint32_t> give_fails;      // give_item at these entities fails (gone: no transform), as in the core
    std::vector<EmfItemsRequest> puts, takes;
    struct Spawn {
        uint8_t guid[16];
        double x, y, z;
    };
    std::vector<Spawn> spawns;
    uint32_t next_spawn_id = 1234;  // spawn gives 1234, 1235, ...
    bool spawn_fails = false;       // spawn answers 0, as the core does for a template the game does not have
    std::set<std::vector<uint8_t>> missing_templates;  // these templates (game byte order) only: a pack not installed
    std::vector<uint32_t> destroyed;
    uint32_t tick = 0;  // what world_tick answers (60 per second of game time)
    struct Sub {
        uint8_t kind;
        std::string name;
        EmfWorldEventCallback cb;
        void* user;
        uint32_t expected = 0;  // the expected_size given to on_event
    };
    std::vector<Sub> subs;
    struct Program {
        std::string name;
        uint8_t kind;
        std::vector<std::string> out;
        EmfProgramCallback fn;
        void* user;
    };
    std::vector<Program> programs;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> emitted;  // name, event bytes
    struct Wheel {
        std::string title;
        std::vector<std::string> items;
        uint32_t hold;
        EmfWheelCallback cb;
        void* user;
    };
    std::vector<Wheel> wheels;
    // world loads (on_load) and the hosted session number (server_session)
    std::vector<std::pair<EmfSaveCallback, void*>> load_cbs;
    uint32_t session = 1;
    // the BasicPlayerInfos singleton (empty: none): 0xA0-byte entries, entity id at +0, account at +8
    std::vector<uint8_t> player_infos;
    std::map<std::string, std::vector<uint8_t>> singletons;  // other singletons by name (e.g. g38_daytime::Daytime)
    std::map<uint32_t, std::string> item_names;  // more items for item_info (0x11 "Wood" is always known)
    // v18 attributes: (entity, attribute id) -> value; what attribute_add and damage were asked to do
    std::map<std::pair<uint32_t, uint32_t>, int32_t> attrs;
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> attr_adds;  // entity, id, delta
    std::vector<std::tuple<uint32_t, uint32_t, int32_t>> damages;    // attacker, target, amount
    // v19 chat and saves: what chat_tell / chat_broadcast were asked to send, the names player_name knows, the on_chat
    // listeners, how often request_save was called
    std::vector<std::pair<uint32_t, std::string>> tells;  // player, text
    std::vector<std::string> broadcasts;
    std::map<uint32_t, std::string> names;  // player entity -> name (absent: player_name answers false)
    std::vector<std::pair<EmfChatCallback, void*>> chat_cbs;
    int saves = 0;
    bool chat_ok = true;  // what chat_tell / chat_broadcast / on_chat / request_save answer
    // v20 teleport: the requests made (answer them with their cb/user as the core would)
    std::vector<EmfTeleport> teleports;
    bool teleport_ok = true;
    void player_info(uint32_t index, uint32_t entity, uint64_t account) {
        if (player_infos.size() < (index + 1) * 0xA0) player_infos.resize((index + 1) * 0xA0, 0);
        std::memcpy(player_infos.data() + index * 0xA0, &entity, 4);
        std::memcpy(player_infos.data() + index * 0xA0 + 8, &account, 8);
    }
};
Fake* g = nullptr;

void* f_get_component(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g->components.find({hash, entity});
    if (it == g->components.end()) {
        if (size) *size = 0;
        return nullptr;
    }
    if (size) *size = it->second.size();
    return it->second.data();
}

void* f_get_template_component(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    auto it = g->templates.find({hash, entity});
    if (it == g->templates.end()) {
        if (size) *size = 0;
        return nullptr;
    }
    if (size) *size = it->second.size();
    return it->second.data();
}
void f_for_each(EmfWorld*, EmfEntityCallback cb, void* user) {
    for (uint32_t e : g->entities) cb(e, user);
}
uint32_t f_find_item(EmfWorld*, const char* name) {
    if (std::strcmp(name, "Wood") == 0) return 0x11u;
    for (const auto& [id, n] : g->item_names)
        if (n == name) return id;
    return 0u;
}
bool f_item_info(EmfWorld*, uint32_t id, EmfItemInfo* out) {
    if (auto it = g->item_names.find(id); it != g->item_names.end()) {
        out->item_id = id;
        out->max_stack = 100;
        std::snprintf(out->name, sizeof out->name, "%s", it->second.c_str());
        return true;
    }
    if (id != 0x11) return false;
    out->item_id = id;
    out->max_stack = 250;
    std::snprintf(out->name, sizeof out->name, "Wood");
    std::memset(out->guid, 0xAB, 16);
    return true;
}
uint32_t f_give(EmfWorld*, uint32_t player, uint32_t item, uint32_t count) {
    if (g->give_fails.count(player)) return 0;
    g->gives.push_back({player, item, count});
    return 900;
}
bool g_put_ok = true;  // what put_items_ex answers (false: the request was refused)
bool f_put_ex(EmfWorld*, const EmfItemsRequest* r) {
    g->puts.push_back(*r);
    return g_put_ok;
}
bool f_take(EmfWorld*, const EmfItemsRequest* r) {
    g->takes.push_back(*r);
    return true;
}
uint32_t f_spawn(EmfWorld*, const uint8_t guid[16], double x, double y, double z) {
    if (g->spawn_fails || g->missing_templates.count(std::vector<uint8_t>(guid, guid + 16))) return 0;
    Fake::Spawn s{};
    std::memcpy(s.guid, guid, 16);
    s.x = x;
    s.y = y;
    s.z = z;
    g->spawns.push_back(s);
    // the new entity stands where it was spawned (CurrentTransform: the position in fixed point, 1 m = 2^32, at offset 0)
    std::vector<uint8_t> t(56, 0);
    const int64_t fixed[3] = {static_cast<int64_t>(x * 4294967296.0), static_cast<int64_t>(y * 4294967296.0),
                              static_cast<int64_t>(z * 4294967296.0)};
    std::memcpy(t.data(), fixed, sizeof fixed);
    g->components[{0x027ac564u /* CurrentTransform */, g->next_spawn_id}] = t;
    return g->next_spawn_id++;
}
bool f_destroy(EmfWorld*, uint32_t e) {
    g->destroyed.push_back(e);
    return true;
}
bool f_on_event(uint8_t kind, const char* name, uint32_t expected, EmfWorldEventCallback cb, void* user) {
    g->subs.push_back({kind, name, cb, user, expected});
    return true;
}
bool f_program_def(const EmfProgramDef* d) {
    Fake::Program p{d->name, d->world_kind, {}, d->fn, d->user};
    for (const char* const* e = d->events_out; e && *e; ++e) p.out.push_back(*e);
    g->programs.push_back(p);
    return true;
}
void* f_emit(void*, const char* name, uint32_t size) {
    g->emitted.push_back({name, std::vector<uint8_t>(size, 0)});
    return g->emitted.back().second.data();
}
bool f_wheel(const EmfWheel* w) {
    Fake::Wheel x{w->title ? w->title : "", {}, w->hold_key, w->cb, w->user};
    for (uint32_t i = 0; i < w->count; ++i) x.items.push_back(w->items[i]);
    g->wheels.push_back(x);
    return true;
}

void f_log(const char* mod, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g->log.push_back(std::string("[") + mod + "] " + b);
}
void f_notify(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g->notes.push_back(b);
}
void f_reply(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g->replies.push_back(b);
}
bool f_hud(const char* id, const char* text, uint32_t place, uint32_t rgba) {
    if (!text || !*text) g->hud.erase(id);
    else g->hud[id] = text;
    g->hud_style[id] = {place, rgba};
    return true;
}
bool f_register_command(const char* name, const char* help, EmfCommandCallback cb, void* user) {
    for (const auto& c : g->commands)
        if (_stricmp(c.name.c_str(), name) == 0) return false;  // as the core: unique ignoring case
    g->commands.push_back({name, help ? help : "", cb, user});
    return true;
}
bool f_register_command_ex(const EmfCommandDef* d) {
    for (auto& c : g->commands)
        if (_stricmp(c.name.c_str(), d->name) == 0) {
            if (c.cb != d->cb || c.user != d->user) return false;  // as the core: only the same owner updates
            c.help = d->help ? d->help : "";
            c.flags = d->flags;
            return true;
        }
    g->commands.push_back({d->name, d->help ? d->help : "", d->cb, d->user, d->flags});
    return true;
}
bool f_mod_info_at(uint32_t i, EmfModInfo* out) {
    if (i >= g->mods.size()) return false;
    *out = g->mods[i];
    return true;
}
int64_t f_setting_int(const char* mod, const char* key, int64_t fallback) {
    auto it = g->settings.find(std::string(mod) + "." + key);
    return it == g->settings.end() ? fallback : std::atoll(it->second.c_str());
}
double f_setting_float(const char* mod, const char* key, double fallback) {
    auto it = g->settings.find(std::string(mod) + "." + key);
    return it == g->settings.end() ? fallback : std::atof(it->second.c_str());
}
uint32_t f_setting_text(const char* mod, const char* key, char* buf, uint32_t size) {
    auto it = g->settings.find(std::string(mod) + "." + key);
    if (it == g->settings.end()) {
        if (size) buf[0] = 0;
        return 0;
    }
    std::snprintf(buf, size, "%s", it->second.c_str());
    return static_cast<uint32_t>(it->second.size());
}
bool f_setting_set(const char* mod, const char* key, const char* value) {
    g->settings[std::string(mod) + "." + key] = value;
    return true;
}
void f_on_setting_changed(const char* mod, EmfSettingCallback cb, void* user) {
    g->setting_watchers.push_back({mod ? mod : "", {cb, user}});
}
bool f_command_sender(EmfCommandSender* out) {
    if (!g->sender.size || !out || out->size < sizeof(EmfCommandSender)) return false;
    *out = g->sender;
    return true;
}
uint8_t f_kind(EmfWorld* w) { return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(w) & 3); }
void f_on_load(EmfSaveCallback cb, void* user) { g->load_cbs.push_back({cb, user}); }
void* f_singleton(EmfWorld*, const char* name) {
    if (auto it = g->singletons.find(name); it != g->singletons.end()) return it->second.data();
    if (std::strcmp(name, "BasicPlayerInfos") != 0 || g->player_infos.empty()) return nullptr;
    return g->player_infos.data();
}
bool f_attr_get(EmfWorld*, uint32_t e, uint32_t id, int32_t* out) {
    auto it = g->attrs.find({e, id});
    if (it == g->attrs.end()) return false;
    *out = it->second;
    return true;
}
bool f_attr_add(EmfWorld*, uint32_t e, uint32_t id, int32_t delta) {
    auto it = g->attrs.find({e, id});
    if (it == g->attrs.end()) return false;
    g->attr_adds.emplace_back(e, id, delta);
    const int64_t v = static_cast<int64_t>(it->second) + delta;  // the core floors at 0
    it->second = static_cast<int32_t>(v < 0 ? 0 : v);
    return true;
}
bool f_damage(EmfWorld*, uint32_t attacker, uint32_t target, int32_t amount) {
    g->damages.emplace_back(attacker, target, amount);
    return amount > 0;
}
void* f_resolve(const char* name) {
    static int fn = 0;
    return std::strcmp(name, "AttrFind") == 0 || std::strcmp(name, "AttrAdd") == 0 ? &fn : nullptr;
}
// The v18 attribute functions of a core on a game build where they were found (fake_api leaves them out, as a core
// on a build without the signatures would answer: emf.kill then takes the old path).
void with_attributes(EmfApi& a) {
    a.attribute_get = f_attr_get;
    a.attribute_add = f_attr_add;
    a.damage = f_damage;
    a.resolve = f_resolve;
}
bool f_teleport(EmfWorld*, const EmfTeleport* t) {
    g->teleports.push_back(*t);
    return g->teleport_ok;
}
// A v20 core (fake_api is older for the Lua host's teleport: without it emf.teleport answers false).
void with_teleport(EmfApi& a) { a.teleport = f_teleport; }
uint32_t f_session() { return g->session; }
uint32_t f_tick(EmfWorld*) { return g->tick; }
bool f_chat_tell(EmfWorld*, uint32_t player, const char* text) {
    if (!g->chat_ok) return false;
    g->tells.emplace_back(player, text ? text : "");
    return true;
}
bool f_chat_broadcast(const char* text) {
    if (!g->chat_ok) return false;
    g->broadcasts.emplace_back(text ? text : "");
    return true;
}
bool f_player_name(EmfWorld*, uint32_t player, char* buf, uint32_t size) {
    if (size) buf[0] = 0;
    auto it = g->names.find(player);
    if (it == g->names.end() || !size) return false;
    std::snprintf(buf, size, "%s", it->second.c_str());
    return true;
}
bool f_on_chat(EmfChatCallback cb, void* user) {
    if (!g->chat_ok || !cb) return false;
    g->chat_cbs.emplace_back(cb, user);
    return true;
}
bool f_request_save() {
    if (!g->chat_ok) return false;
    ++g->saves;
    return true;
}
// A player's chat line reaches the server (every on_chat listener hears it on `world`).
void fire_chat(EmfWorld* world, uint64_t account, uint32_t entity, const char* text) {
    const auto cbs = g->chat_cbs;
    for (auto& [cb, user] : cbs) cb(world, account, entity, text, user);
}
// The host loaded world save `slot` (every on_load listener hears it).
void fire_load(uint16_t slot) {
    for (auto& [cb, user] : g->load_cbs) cb(slot, user);
}

EmfApi fake_api() {
    EmfApi a{};
    a.version = EMF_API_VERSION;
    a.size = sizeof a;
    a.log = f_log;
    a.notify = f_notify;
    a.command_reply = f_reply;
    a.hud_set = f_hud;
    a.register_command = f_register_command;
    a.register_command_ex = f_register_command_ex;
    a.mod_info_at = f_mod_info_at;
    a.setting_int = f_setting_int;
    a.setting_float = f_setting_float;
    a.setting_text = f_setting_text;
    a.setting_set = f_setting_set;
    a.on_setting_changed = f_on_setting_changed;
    a.world_kind = f_kind;
    a.get_component = f_get_component;
    a.get_template_component = f_get_template_component;
    a.for_each_entity = f_for_each;
    a.find_item = f_find_item;
    a.item_info = f_item_info;
    a.give_item = f_give;
    a.put_items_ex = f_put_ex;
    a.take_items = f_take;
    a.spawn = f_spawn;
    a.destroy = f_destroy;
    a.on_event = f_on_event;
    a.register_program_def = f_program_def;
    a.emit_event = f_emit;
    a.wheel_open = f_wheel;
    a.command_sender = f_command_sender;
    a.on_load = f_on_load;
    a.server_session = f_session;
    a.singleton = f_singleton;
    a.world_tick = f_tick;
    a.chat_tell = f_chat_tell;  // v19
    a.chat_broadcast = f_chat_broadcast;
    a.player_name = f_player_name;
    a.on_chat = f_on_chat;
    a.request_save = f_request_save;
    return a;
}

// %TEMP%\emf_lua_test_<pid>_<name>. Rigs with the same name in one run share it (a restart that finds what the first
// one saved), so it is removed when the run ends, not with the Rig.
std::wstring temp_dir(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"emf_lua_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
    CreateDirectoryW(d.c_str(), nullptr);
    test_scratch::remove_at_exit(d);
    return d;
}
void write(const std::wstring& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

// One script mod `id` with main.lua = `code` (and a mod.json with `settings_json` settings).
void add_script(const std::wstring& dir, const char* id, const std::string& code, const std::string& settings_json = "[]") {
    const std::wstring folder = dir + L"\\" + std::wstring(id, id + std::strlen(id));
    CreateDirectoryW(folder.c_str(), nullptr);
    write(folder + L"\\main.lua", code);
    write(folder + L"\\mod.json", std::string(R"({"id": ")") + id + R"(", "version": "1", "main": "main.lua", "settings": )" +
                                      settings_json + "}");
    EmfModInfo info{};
    info.size = sizeof info;
    info.status = EMF_MOD_SCRIPT;
    std::snprintf(info.id, sizeof info.id, "%s", id);
    std::snprintf(info.name, sizeof info.name, "%s", id);
    std::snprintf(info.source, sizeof info.source, "%s\\", id);
    std::snprintf(info.main, sizeof info.main, "main.lua");
    g->mods.push_back(info);
}
bool logged(const char* needle) {
    for (const auto& l : g->log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
EmfWorld* const kServer = reinterpret_cast<EmfWorld*>(0x10000);  // kind 0
EmfWorld* const kClient = reinterpret_cast<EmfWorld*>(0x10002);  // kind 2

struct Rig {
    Fake fake;
    EmfApi api;
    std::wstring dir;
    explicit Rig(const wchar_t* name) {
        g = &fake;
        api = fake_api();
        dir = temp_dir(name) + L"\\mods";  // <test>\mods, so <test>\config is beside it as in the game
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    ~Rig() { g = nullptr; }
    void run_command(const std::string& line) {
        std::vector<std::string> words;
        size_t p = 0;
        while (p < line.size()) {
            size_t e = line.find(' ', p);
            if (e == std::string::npos) e = line.size();
            words.push_back(line.substr(p, e - p));
            p = e + 1;
        }
        std::vector<const char*> argv;
        for (auto& w : words) argv.push_back(w.c_str());
        for (auto& c : fake.commands)
            if (c.name == words[0]) c.cb(kServer, static_cast<int>(argv.size()), argv.data(), c.user);
    }
};
}  // namespace
