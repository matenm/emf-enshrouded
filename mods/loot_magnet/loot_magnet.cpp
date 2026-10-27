// loot_magnet (Loot Magnet): an EMF mod. On the host or a dedicated server, for every player whose magnet is on, the
// corpses of creatures killed near them in the last seconds are looted into them: the game's own loot event
// {guest = the player, host = the corpse, lootAll, bypassInteraction} (seen in game: the corpse's stacks moved into the
// player, a PickupEvent each). Players who joined need no EMF: it runs where the world runs. Each player switches their
// own magnet with /loot.toggle (a command for every player, API v17); the host's default and key are settings. Never: a
// corpse twice, a player's or an ally's body, anything owned or tame, anything inside a base. The rules (tested without
// the game) are magnet_logic.h.
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/inventory.hpp"
#include "emf/keys.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "magnet_glue.h"
#include "magnet_logic.h"

namespace gen = emf::gen::ecs;

namespace {
constexpr const char* kMod = "loot_magnet";
constexpr double kFixed = 4294967296.0;

// ---- settings (the change callback may run on any world's thread) ----
struct Config {
    magnet::Rules rules;
    bool default_on = true;
    std::string key;         // the host's toggle key ("" = none)
    bool debug_log = true;   // one log line per death noted and per corpse decided
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};  // the mod's switch went off in the Mods tab (the DLL stays until restart)

// The mod's own switch, as the Mods tab saved it (emf\config\loot_magnet.ini; the API has no getter).
bool switch_on() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\loot_magnet.ini";
    return GetPrivateProfileIntW(L"loot_magnet", L"enabled", 1, ini.c_str()) != 0;
}

Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}
void read_settings() {
    Config c;
    c.rules.radius = emf::setting_float(kMod, "radius", 8.0);
    c.rules.delay_ms = static_cast<uint64_t>(emf::setting_float(kMod, "delay", 1.0) * 1000.0);
    // how long after the death it keeps looking (at least 8 s: the game adds the loot 1-3 s after the death), and at
    // least 2 s past the delay
    const double window = (std::max)(emf::setting_float(kMod, "window", 15.0), 8.0);
    c.rules.window_ms = (std::max)(static_cast<uint64_t>(window * 1000.0), c.rules.delay_ms + 2000);
    c.rules.materials_only = emf::setting_text(kMod, "categories", "materials") == "materials";
    c.rules.include_ammo = emf::setting_bool(kMod, "include_ammo", true);
    c.rules.base_radius = emf::setting_float(kMod, "base_radius", 30.0);
    c.default_on = emf::setting_bool(kMod, "default_on", true);
    c.key = emf::setting_text(kMod, "toggle_key", "");
    c.debug_log = emf::setting_bool(kMod, "debug_log", true);
    std::lock_guard<std::mutex> l(g_config_lock);
    g_config = c;
}

// ---- loot events for the program (server tick -> a game worker thread) ----
struct Request {
    uint32_t guest, host, session;
};
std::mutex g_requests_lock;
std::vector<Request> g_requests;

void magnet_program(void* ctx) {
    std::vector<Request> mine;
    {
        std::lock_guard<std::mutex> l(g_requests_lock);
        mine.swap(g_requests);
    }
    const uint32_t session = emf::server_session();
    for (const auto& r : mine)
        if (r.session == session)
            if (auto* e = emf::emit<gen::LootInteractionEvent>(ctx)) magnet::fill_loot_event(*e, r.guest, r.host);
}

// ---- server tick state ----
magnet::Magnet g_magnet;
magnet::Toggles g_toggles;
emf::WorldFile g_file(kMod);
std::vector<magnet::Vec3> g_bases;
uint64_t g_last_ms = 0, g_bases_ms = 0;
uint32_t g_session = 0;
std::map<uint32_t, std::string> g_item_names;  // item id -> debug name (the game's names do not change)
emf::keys::Edge g_key;
int g_looted = 0;  // corpses looted this session (for loot.status)

std::optional<magnet::Vec3> pos_of(emf::World w, uint32_t id) {
    auto* t = w.get<gen::CurrentTransform>(id);
    if (!t) return std::nullopt;
    return magnet::Vec3{t->transform.position.x / kFixed, t->transform.position.y / kFixed,
                        t->transform.position.z / kFixed};
}

const std::string& item_name(emf::World w, uint32_t item) {
    auto it = g_item_names.find(item);
    if (it != g_item_names.end()) return it->second;
    EmfItemInfo info{};
    info.size = sizeof info;
    std::string name = emf::api()->item_info(w.raw(), item, &info) ? info.name : "";
    if (name.empty()) {  // not known yet: the corpse waits (materials only), and it is asked again next time
        static const std::string unknown;
        return unknown;
    }
    return g_item_names.emplace(item, name).first->second;
}

magnet::Body body_of(emf::World w, uint32_t id) {
    return magnet::read_body(w, id, [w](uint32_t item) { return item_name(w, item); });
}

double nearest_base(const magnet::Vec3& p) {
    double best = -1;
    for (const auto& b : g_bases) {
        const double d = magnet::horizontal_distance(p, b);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

// The decision log: what became of each corpse the magnet watched (looted lines always; the rest with debug_log).
void log_decision(const magnet::Decision& d, const Config& c) {
    if (d.why == magnet::Why::Ok) {
        EMF_LOG(kMod, "corpse %u looted into player %u (%.1f m; interaction offer %s)", d.corpse, d.player, d.distance,
                d.offer ? "yes" : "no");
        return;
    }
    if (!c.debug_log) return;
    if (d.why == magnet::Why::TooLate) {
        if (d.waited == magnet::Why::NoLoot)
            EMF_LOG(kMod, "corpse %u left: no loot appeared within %.0f s of its death (no Health or no Inventory)", d.corpse,
                    c.rules.window_ms / 1000.0);
        else if (d.waited == magnet::Why::TooFar && d.distance >= 0)
            EMF_LOG(kMod, "corpse %u left: its time ran out while %s (the nearest player with a magnet was %.1f m away, reach %.0f m)",
                    d.corpse, magnet::why_text(d.waited), d.distance, c.rules.radius);
        else
            EMF_LOG(kMod, "corpse %u left: its time ran out while waiting (%s)", d.corpse, magnet::why_text(d.waited));
        return;
    }
    EMF_LOG(kMod, "corpse %u skipped: %s", d.corpse, magnet::why_text(d.why));
}

void refresh_bases(emf::World w) {
    g_bases.clear();
    w.for_each_entity([&](uint32_t id) {
        if (!w.get<gen::BaseIdComponent>(id)) return;
        if (const auto p = pos_of(w, id)) g_bases.push_back(*p);
    });
}

bool magnet_on(emf::World w, uint32_t player, const Config& c) {
    const uint64_t account = emf::player_account(w, player);
    return account ? g_toggles.on(account, c.default_on) : c.default_on;
}

// Switches `player`'s magnet (to `want`, or the other way); false when their account is not known.
bool toggle(emf::World w, uint32_t player, int want, bool* now_on) {
    const uint64_t account = emf::player_account(w, player);
    if (!account) return false;
    const bool on = want < 0 ? !g_toggles.on(account, config().default_on) : want != 0;
    g_toggles.set(account, on);
    if (!g_file.save(g_toggles.text())) EMF_LOG(kMod, "the switch could not be saved yet (no world file); it lasts this session");
    *now_on = on;
    return true;
}

void on_tick(emf::World w) {
    if (!w.is_server() || g_switched_off) return;  // switched off in the Mods tab: no more looting from now on
    const Config c = config();
    g_file.poll([](const std::string& text) { g_toggles.parse(text); });
    const uint32_t session = emf::server_session();
    if (session != g_session) {
        g_session = session;
        g_bases_ms = 0;
        g_looted = 0;
        if (!g_file.ready()) g_toggles.clear();
    }
    const uint64_t now = GetTickCount64();
    if (!c.key.empty()) {  // the host's own key
        const uint32_t vk = emf::keys::parse_key(std::wstring(c.key.begin(), c.key.end()));
        if (g_key.pressed(emf::keys::down(vk)) && emf::local_player()) {
            bool on = false;
            if (toggle(w, emf::local_player(), -1, &on)) EMF_NOTIFY("Loot Magnet %s", on ? "on" : "off");
        }
    }
    if (now - g_last_ms < 500) return;
    g_last_ms = now;
    if (now - g_bases_ms >= 10000) {
        g_bases_ms = now;
        refresh_bases(w);
    }
    {
        std::lock_guard<std::mutex> lock(g_requests_lock);
        if (g_requests.size() >= 192) return;  // the program has not sent the last ones yet: decide nothing now
    }
    std::vector<magnet::Looter> looters;
    for (uint32_t id : emf::players(w))
        if (const auto p = pos_of(w, id)) looters.push_back({id, *p, magnet_on(w, id, c)});
    std::vector<magnet::Decision> decisions;
    const auto loots =
        g_magnet.tick(now, session, looters, [w](uint32_t id) { return body_of(w, id); }, g_bases, c.rules, &decisions);
    for (const auto& d : decisions) log_decision(d, c);
    for (const auto& l : loots) {
        {
            std::lock_guard<std::mutex> lock(g_requests_lock);
            if (g_requests.size() < 256) g_requests.push_back({l.player, l.corpse, session});
        }
        g_magnet.on_sent(l.corpse, l.player, now, session);
        ++g_looted;
    }
    // nothing came from the corpse within 2 s and it still holds loot: send again (it may become lootable a moment
    // after it died), at most 5 times; then the player has no room (or the game refused), and the loot stays there
    for (const auto& late : g_magnet.overdue(now)) {
        if (emf::inventory_stacks(w, late.corpse).empty()) continue;  // it came (the confirmation was another stack's)
        if (g_magnet.should_retry(late)) {
            if (c.debug_log)
                EMF_LOG(kMod, "corpse %u: nothing came from the loot event yet (try %d of %d): sent again", late.corpse,
                        late.tries, magnet::Magnet::kMaxTries);
            std::lock_guard<std::mutex> lock(g_requests_lock);
            g_requests.push_back({late.player, late.corpse, session});
            g_magnet.on_sent(late.corpse, late.player, now, session, late.tries + 1);
            continue;
        }
        EMF_LOG(kMod, "corpse %u still holds loot after %d loot events: player %u has no room, or the game refused (the loot "
                      "stays there)", late.corpse, late.tries, late.player);
        if (g_magnet.should_tell_full(late.player, session) && late.player == emf::local_player())
            EMF_NOTIFY("Loot Magnet: no room in your inventory; the loot stays in the corpse");
    }
}

// The player a command means: a chat sender's entity, else the host.
uint32_t sender_player() {
    EmfCommandSender s{};
    if (emf::command_sender(&s) && s.from_chat) return s.entity;
    return emf::local_player();
}

void cmd_toggle(emf::World w, const std::vector<std::string>& a) {
    int want = -1;
    if (a.size() > 1) {
        if (a[1] == "on") want = 1;
        else if (a[1] == "off") want = 0;
        else {
            EMF_REPLY("usage: loot.toggle [on|off]");
            return;
        }
    }
    const uint32_t player = sender_player();
    bool on = false;
    if (!player || !toggle(w, player, want, &on)) {
        EMF_REPLY("Loot Magnet: EMF could not tell which player you are; try again in a moment");
        return;
    }
    const Config c = config();
    EMF_REPLY("Loot Magnet is %s for you (corpses within %.0f m, %s)", on ? "on" : "off", c.rules.radius,
              c.rules.materials_only ? "materials only" : "everything");
}

void cmd_status(emf::World w, const std::vector<std::string>&) {
    const Config c = config();
    const uint32_t player = sender_player();
    if (!player || !emf::player_account(w, player)) {
        EMF_REPLY("Loot Magnet: EMF could not tell which player you are; try again in a moment");
        return;
    }
    EMF_REPLY("Loot Magnet is %s for you: corpses up to %.0f s after the kill within %.0f m, from %.1f s after it, %s; never in a base",
              magnet_on(w, player, c) ? "on" : "off", c.rules.window_ms / 1000.0, c.rules.radius,
              c.rules.delay_ms / 1000.0, c.rules.materials_only ? (c.rules.include_ammo ? "materials and ammo only" : "materials only")
                                                                : "everything");
    EMF_REPLY("%d corpse(s) looted this session; /loot.toggle switches yours", g_looted);
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v17()) {
        EMF_LOG(kMod, "needs EMF API v17 or newer");
        return false;
    }
    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string& key) {
        if (key == "enabled") g_switched_off = !switch_on();
        read_settings();
    });
    if (!emf::add_program("loot_magnet", EMF_WORLD_SERVER, {}, {"LootInteractionEvent"}, magnet_program)) {
        EMF_LOG(kMod, "could not add the loot_magnet program: off");
        return false;
    }
    emf::on_event<gen::EntityDiedEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::EntityDiedEvent& e) {
        if (g_switched_off) return;
        const uint32_t id = e.deadEntityId.id;
        const auto p = pos_of(w, id);
        if (!p) {
            if (config().debug_log) EMF_LOG(kMod, "death of %u: it has no position any more; not watched", id);
            return;
        }
        g_magnet.on_death(id, *p, GetTickCount64(), emf::server_session(), magnet::template_of(w, id));
        if (config().debug_log)
            EMF_LOG(kMod, "death of %u noted at (%.1f, %.1f, %.1f), %.0f m from the nearest flame altar", id, p->x, p->y,
                    p->z, nearest_base(*p));
    });
    emf::on_event<gen::PickupEvent>(EMF_WORLD_SERVER, [](emf::World, const gen::PickupEvent& e) {
        g_magnet.on_pickup(e.pickedUpByEntityId.id, e.pickedUpFromEntityId.id);
    });
    g_file.attach();
    if (!emf::add_command("loot.toggle", "[on|off]: your Loot Magnet (corpses of creatures killed near you come to you)",
                          cmd_toggle, EMF_COMMAND_PLAYERS))
        EMF_LOG(kMod, "the command loot.toggle could not be added (see the core log)");
    if (!emf::add_command("loot.status", ": is your Loot Magnet on, and its rules", cmd_status, EMF_COMMAND_PLAYERS))
        EMF_LOG(kMod, "the command loot.status could not be added (see the core log)");
    emf::on_tick(on_tick);
    const Config c = config();
    EMF_LOG(kMod, "ready: corpses within %.0f m come to players whose magnet is on (default %s); /loot.toggle", c.rules.radius,
            c.default_on ? "on" : "off");
    return true;
}
