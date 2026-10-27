// bulk_craft (Bulk Craft): an EMF mod. Craft one by hand, then craft many.
//   - The host looks at a crafting station (or at nothing: hand crafting) and presses the key (default U): a wheel with
//     the recipes they crafted there by hand, then x5 / x10 / x25 / max.
//   - /craft <n|max> [part of the name] does the same for every player on the server (API v17 player commands), at the
//     station they look at or the nearest within `range`; /craft.stop, /craft.list.
// The game crafts: the mod sends the game's own CraftRecipeEvent with the player as the operator, so the game's craft
// core takes the materials from the player and puts the output into them, all or nothing per craft. The mod never
// moves an item itself. What the event does not check (seen in game, docs/engine_parts/inventory_crafting.md
// section 4), the mod does: only recipes the player crafted by hand through the game's own crafting window (which
// offers only unlocked recipes), a station within reach with the recipe's workshop (never a factory, the altar or an
// upgrade), and a pace never faster than the recipe's crafting time. The rules are bc_logic.h; the game side bc_glue.h.
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "bc_glue.h"
#include "bc_logic.h"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/keys.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"

namespace gen = emf::gen;
namespace bc = bulkcraft;

namespace {
constexpr const char* kMod = "bulk_craft";
constexpr size_t kWheelRecipes = 7;

// ---- settings (the change callback may run on any world's thread) ----
struct Config {
    bc::Settings s;
    uint32_t key_vk = 'U';
    std::string key_name = "U";
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};

// The mod's own switch, as the Mods tab saved it (emf\config\bulk_craft.ini, [bulk_craft] enabled; off by default).
bool switch_on() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring ini(exe);
    ini = ini.substr(0, ini.find_last_of(L"\\/")) + L"\\emf\\config\\bulk_craft.ini";
    return GetPrivateProfileIntW(L"bulk_craft", L"enabled", 0, ini.c_str()) != 0;
}
Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}
void read_settings() {
    Config c;
    const std::string key = emf::setting_text(kMod, "key", "U");
    if (const uint32_t vk = emf::keys::parse_key(std::wstring(key.begin(), key.end()), &c.key_name)) {
        c.key_vk = vk;
    } else {
        c.key_vk = 0;  // no key (cleared): /craft still works
        c.key_name = "(no key)";
    }
    c.s.pace = bc::pace_of(emf::setting_text(kMod, "pace", "paced"));
    c.s.floor_s = emf::setting_float(kMod, "seconds", 0.5);
    const int64_t max_amount = emf::setting_int(kMod, "max_amount", 100);
    c.s.max_amount = static_cast<uint32_t>((std::min)((std::max)(max_amount, int64_t{0}), int64_t{100000}));
    c.s.range = static_cast<double>(emf::setting_int(kMod, "range", 5));
    c.s.notes = emf::setting_bool(kMod, "notes", true);
    c.s = bc::clamped(c.s);  // mod.json's limits, whatever the ini says
    std::lock_guard<std::mutex> l(g_config_lock);
    g_config = c;
}

// ---- state: the server world's thread, except the send queue and OwnSends (also the program's worker thread) ----
emf::WorldFile g_file(kMod);
bc::Learned g_learned;
bc::Runner g_run;
uint32_t g_session = 0;
emf::keys::Edge g_key;
std::atomic<bool> g_blind{false};  // the game refused an event watch: no orders
std::set<std::pair<uint32_t, uint32_t>> g_blocked;  // (workshop, recipe) whose materials came from outside the bag
std::map<uint32_t, std::string> g_names;

struct Send {
    uint32_t op = 0, station = 0, recipe = 0;
    uint16_t amount = 1;
    uint32_t session = 0;
    bc::Choices choices{};
};
std::mutex g_send_lock;
std::vector<Send> g_sends;
bc::OwnSends g_own;  // under g_send_lock

std::string debug_name(emf::World w, uint32_t item) {
    auto it = g_names.find(item);
    if (it != g_names.end()) return it->second;
    EmfItemInfo info{};
    info.size = sizeof info;
    if (!emf::api()->item_info(w.raw(), item, &info) || !info.name[0]) {
        char b[24];
        snprintf(b, sizeof b, "item %08x", item);
        return b;  // not known yet: asked again next time
    }
    return g_names.emplace(item, info.name).first->second;
}
std::string label(emf::World w, uint32_t item) { return bc::pretty_item(debug_name(w, item)); }
std::string recipe_label(emf::World w, const bc::Recipe& r) {
    if (r.outputs.empty()) return "recipe " + std::to_string(r.id);
    const auto& o = r.outputs[0];
    return label(w, o.item) + (o.count > 1 ? " x" + std::to_string(o.count) : "");
}

std::string output_label(emf::World w, const bc::Order& o) {
    return o.recipe.outputs.empty() ? std::string("items") : label(w, o.recipe.outputs[0].item);
}

bool save_learned() { return g_file.save(g_learned.text()); }

void new_session() {
    g_session = emf::server_session();
    g_run.clear();
    {
        std::lock_guard<std::mutex> l(g_send_lock);
        g_sends.clear();
        g_own.clear();
    }
    g_learned.clear();  // the new world's file brings its own (poll)
    g_blocked.clear();
}

// Tells a player something: the host on screen (with notes on), a chat player in the chat.
void say(emf::World w, uint32_t player, bool chat, const std::string& text, const bc::Settings& s) {
    EMF_LOG(kMod, "player %u: %s", player, text.c_str());
    if (chat) {
        if (!emf::tell(w, player, text) && player == emf::local_player() && s.notes) EMF_NOTIFY("%s", text.c_str());
    } else if (s.notes) {
        EMF_NOTIFY("%s", text.c_str());
    }
}

// ---- starting an order ----
// Returns what to tell the player (the start note, or why not). `forced`: the recipe the wheel picked.
std::string start_order(emf::World w, uint32_t player, bool chat, const bc::Amount& amount, const std::string& filter,
                        uint32_t forced = 0) {
    if (g_switched_off) return "Bulk craft is switched off";
    if (g_blind) return bc::blind_text();
    if (emf::server_session() != g_session) new_session();
    const Config c = config();
    if (!player || !emf::position(w, player)) return "Bulk craft: EMF could not tell where you are; try again in a moment";
    const uint64_t account = emf::player_account(w, player);
    if (!account) return "Bulk craft: EMF could not tell who you are; try again in a moment";
    if (const auto* o = g_run.running(player)) {
        if (o->account == account)
            return "Bulk craft: still crafting (" + std::to_string(o->made) + " of " + std::to_string(o->want) +
                   "); /craft.stop first";
        g_run.stop(player);  // a player who left this slot: theirs ends, without a word to the newcomer
        EMF_LOG(kMod, "player %u: the order of the account that had this slot before ended", player);
    }
    if (bc::station_recipes(w, 0).empty()) return "Bulk craft: the game's recipes are not loaded yet; try again in a moment";
    const auto st = bc::station_for(w, player, c.s.range, true);
    auto name_of = [w](uint32_t item) { return debug_name(w, item); };
    auto list = bc::candidates(w, g_learned.of(account), st, filter, name_of);
    if (forced) {
        std::vector<bc::Recipe> only;
        for (auto& r : list)
            if (r.id == forced) only.push_back(r);
        list.swap(only);
    }
    if (list.empty()) {
        // Explain: a recipe of this station (or by hand) matches but was never crafted by hand, or nothing does.
        if (!filter.empty()) {
            std::vector<bc::Recipe> any = st ? bc::station_recipes(w, st->workshop) : std::vector<bc::Recipe>{};
            for (auto& r : bc::station_recipes(w, 0)) any.push_back(r);
            std::string best;  // the shortest matching name: "stone" means the plain stone block, not a dispenser
            uint32_t best_id = 0;
            for (const auto& r : any)
                if (!r.outputs.empty() && r.outputs[0].item && bc::name_matches(debug_name(w, r.outputs[0].item), filter)) {
                    const std::string n = label(w, r.outputs[0].item);
                    if (best.empty() || n.size() < best.size()) {
                        best = n;
                        best_id = r.id;
                    }
                }
            if (!best.empty()) {
                EMF_LOG(kMod, "player %u asked for '%s': %s is recipe %08x, not crafted by hand (craft.teach %08x)", player,
                        filter.c_str(), best.c_str(), best_id, best_id);
                return bc::refusal_text(bc::Refusal::NotLearned, best);
            }
            return "Bulk craft: nothing called '" + filter + "' is made " + (st ? "at this station or " : "") + "by hand";
        }
        return std::string("Bulk craft repeats what you crafted by hand: craft one in the game's crafting window ") +
               (st ? "at this station" : "(or stand at a station)") + " first";
    }
    const bc::Recipe found = list.front();
    const bool hand = found.workshop == 0;
    const uint32_t station = hand ? player : st->id;
    const std::string name = recipe_label(w, found);
    const bc::Refusal why = bc::check_recipe(found, hand ? 0 : st->workshop,
                                             !hand && w.get<gen::ecs::FactoryStation>(station), g_learned.knows(account, found.id));
    if (why != bc::Refusal::None) return bc::refusal_text(why, name);
    if (g_blocked.count({found.workshop, found.id})) return bc::refusal_text(bc::Refusal::Blocked, name);
    // The crafting window's category choice, from the hand craft this was learned from; without one, refused.
    const bc::Choices choices = g_learned.choices_of(account, found.id).value_or(bc::Choices{});
    const bc::Resolved res = bc::resolve(found, choices, bc::category_lookup(w));
    if (res.result != bc::Resolve::Ok) {
        EMF_LOG(kMod, "player %u: recipe %08x has category inputs and %s", player, found.id,
                res.result == bc::Resolve::NoChoice ? "no saved choice" : "a saved choice the category does not have");
        return bc::refusal_text(bc::Refusal::NoChoice, name);
    }
    const bc::Recipe& r = res.recipe;
    const auto have = bc::carried(w, player);
    const uint32_t pay = bc::can_pay(r, have);
    const uint32_t n = bc::order_size(amount, pay, c.s.max_amount);
    if (n == 0) {
        const uint32_t missing = bc::missing_input(r, have);
        return "Bulk craft: you have too few " + (missing ? label(w, missing) : std::string("materials")) + " for " + name;
    }
    bc::Order o;
    o.player = player;
    o.account = account;
    o.station = station;
    o.recipe = r;
    o.choices = choices;
    o.want = n;
    o.settings = c.s;
    o.chat = chat;
    o.session = g_session;
    if (!g_run.start(o, GetTickCount64())) return "Bulk craft: could not start (see the log)";
    g_learned.learn(account, r.id);  // used: the newest (kept in memory; the file is written when a recipe is new)
    char eta[64] = "";
    const double secs = bc::eta_seconds(r, c.s, n);
    if (secs >= 1.0) snprintf(eta, sizeof eta, ", about %.0f s", secs);
    std::string text = "Bulk craft: " + std::to_string(n) + "x " + name + (hand ? " by hand" : " at this station");
    if (amount.max || n < amount.n) text += " (all you have materials for)";
    text += eta;
    text += chat ? "; /craft.stop stops it" : "; press " + c.key_name + " to stop";
    EMF_LOG(kMod, "player %u: order %u x %08x (%s) at %u%s", player, n, r.id, name.c_str(), station, hand ? " (hand)" : "");
    return text;
}

// ---- the program: sends the queued crafts (game worker thread) ----
void craft_program(void* ctx) {
    std::lock_guard<std::mutex> l(g_send_lock);
    for (const auto& s : g_sends) {
        if (s.session != emf::server_session()) continue;  // an old world's: its ids are other entities now
        if (auto* e = emf::emit<gen::ecs::CraftRecipeEvent>(ctx)) {
            e->craftingOperatorId.id = s.op;
            e->craftingStationId.id = s.station;
            e->recipeId.value = s.recipe;
            e->recipeAmount = s.amount;
            for (size_t i = 0; i < s.choices.size(); ++i) {  // the crafting window's choice, as it sent it
                e->inputCategorySelections.selections[i].id.value = s.choices[i].category;
                e->inputCategorySelections.selections[i].selectedIndex = s.choices[i].index;
            }
            g_own.sent(s.op, s.recipe);
        }
    }
    g_sends.clear();
}

// Before each send: is the order still sound? (bc_glue.h order_check: the account, the station, the materials)
bc::Stop check_order(emf::World w, const bc::Order& o) {
    if (g_switched_off) return bc::Stop{"Bulk craft was switched off"};
    return bc::order_check(w, o, [w](uint32_t item) { return debug_name(w, item); });
}

// ---- the key and the wheels (the host) ----
void amount_wheel(emf::World w, uint32_t player, const bc::Recipe& r, uint32_t hold_vk) {
    const Config c = config();
    const uint64_t account = emf::player_account(w, player);
    const bc::Resolved res =
        bc::resolve(r, g_learned.choices_of(account, r.id).value_or(bc::Choices{}), bc::category_lookup(w));
    if (res.result != bc::Resolve::Ok) {
        say(w, player, false, bc::refusal_text(bc::Refusal::NoChoice, recipe_label(w, r)), c.s);
        return;
    }
    const uint32_t pay = bc::can_pay(res.recipe, bc::carried(w, player));
    const uint32_t max = bc::order_size(bc::parse_amount("max"), pay, c.s.max_amount);
    const std::string title = recipe_label(w, r) + ": how many?";
    const std::vector<std::string> items = {"x5", "x10", "x25", "Max (" + std::to_string(max) + ")"};
    const uint32_t id = r.id;
    const bool ok = emf::wheel(title.c_str(), items, hold_vk, EMF_WORLD_SERVER, [player, id](emf::World w2, int choice) {
        static const char* amounts[] = {"5", "10", "25", "max"};
        if (choice < 0 || choice > 3) return;
        const std::string text = start_order(w2, player, false, bc::parse_amount(amounts[choice]), "", id);
        say(w2, player, false, text, config().s);
    });
    if (!ok) say(w, player, false, "Bulk craft: the wheel is busy (is the console open?); /craft works too", c.s);
}

void on_key(emf::World w) {
    const uint32_t player = emf::local_player();
    const Config c = config();
    if (!player) return;
    if (const auto stopped = g_run.stop(player)) {  // a running order: the key stops it
        say(w, player, false, bc::finish_text(*stopped, bc::Outcome::Stopped, "you pressed " + c.key_name,
                                              output_label(w, *stopped)), c.s);
        return;
    }
    if (emf::server_session() != g_session) new_session();
    const uint64_t account = emf::player_account(w, player);
    const auto st = bc::station_for(w, player, c.s.range, true);
    auto list = bc::candidates(w, g_learned.of(account), st, "", [w](uint32_t item) { return debug_name(w, item); });
    if (list.empty()) {
        say(w, player, false, start_order(w, player, false, bc::parse_amount("5"), ""), c.s);  // says why
        return;
    }
    if (list.size() == 1) {
        amount_wheel(w, player, list[0], c.key_vk);
        return;
    }
    if (list.size() > kWheelRecipes) list.resize(kWheelRecipes);
    std::vector<std::string> items;
    for (const auto& r : list) items.push_back(recipe_label(w, r) + (r.workshop ? "" : " (by hand)"));
    const bool ok = emf::wheel("Bulk craft", items, c.key_vk, EMF_WORLD_SERVER, [player, list](emf::World w2, int choice) {
        if (choice < 0 || choice >= static_cast<int>(list.size())) return;
        amount_wheel(w2, player, list[static_cast<size_t>(choice)], 0);  // the key is up by now: click to pick
    });
    if (!ok) say(w, player, false, "Bulk craft: the wheel is busy (is the console open?); /craft works too", c.s);
}

// ---- the server tick ----
void on_tick(emf::World w) {
    if (!w.is_server()) return;
    if (emf::server_session() != g_session) new_session();
    g_file.poll([](const std::string& text) {
        g_learned.parse(text);
        save_learned();  // what this session learned before the file was read goes in too
    });
    const Config c = config();
    if (g_switched_off) {
        g_run.clear();
        return;
    }
    if (c.key_vk && g_key.pressed(emf::keys::down(c.key_vk))) on_key(w);
    if (g_run.empty()) return;
    const auto acts = g_run.step(GetTickCount64(), w.tick(), [w](const bc::Order& o) { return check_order(w, o); },
                                 [w](const bc::Order& o) { return bc::carried(w, o.player); });
    for (const auto& a : acts) {
        if (a.kind == bc::Action::Send) {
            std::lock_guard<std::mutex> l(g_send_lock);
            g_sends.push_back({a.player, a.station, a.recipe, static_cast<uint16_t>(a.amount), g_session, a.choices});
            continue;
        }
        const std::string text = bc::finish_text(a.order, a.outcome, a.why, output_label(w, a.order));
        if (a.foreign) {  // the game took the materials from elsewhere (base storage?): never again here this session
            g_blocked.insert({a.order.recipe.workshop, a.order.recipe.id});
            EMF_LOG(kMod, "player %u: recipe %08x at workshop %08x took its materials from outside the bag; refused there "
                    "from now on (this world session)", a.player, a.order.recipe.id, a.order.recipe.workshop);
        }
        if (a.quiet) EMF_LOG(kMod, "player %u: %s (%s; not told)", a.player, text.c_str(), a.why.c_str());
        else say(w, a.player, a.order.chat, text, a.order.settings);
    }
}

// ---- commands ----
uint32_t sender_player(bool* chat) {
    EmfCommandSender s{};
    s.size = sizeof s;
    const bool from_chat = emf::command_sender(&s) && s.from_chat;
    if (chat) *chat = from_chat;
    return from_chat ? s.entity : emf::local_player();
}

void cmd_craft(emf::World w, const std::vector<std::string>& a) {
    const auto args = bc::parse_craft_args(a);
    if (!args.ok) {
        EMF_REPLY("%s", args.error.c_str());
        return;
    }
    bool chat = false;
    const uint32_t player = sender_player(&chat);
    if (!player) {
        EMF_REPLY("Bulk craft: EMF could not tell which player you are (on a dedicated server, type it in the game's chat)");
        return;
    }
    EMF_REPLY("%s", start_order(w, player, chat, args.amount, args.filter).c_str());
}

void cmd_stop(emf::World w, const std::vector<std::string>&) {
    const uint32_t player = sender_player(nullptr);
    const auto* running = g_run.running(player);
    const auto o = running && running->account == emf::player_account(w, player) ? g_run.stop(player)
                                                                              : std::optional<bc::Order>{};
    if (!o) {
        EMF_REPLY("Bulk craft: you have no order running");
        return;
    }
    EMF_REPLY("%s", bc::finish_text(*o, bc::Outcome::Stopped, "you asked", output_label(w, *o)).c_str());
}

void cmd_list(emf::World w, const std::vector<std::string>&) {
    if (emf::server_session() != g_session) new_session();
    const uint32_t player = sender_player(nullptr);
    const uint64_t account = emf::player_account(w, player);
    if (!player || !account) {
        EMF_REPLY("Bulk craft: EMF could not tell who you are");
        return;
    }
    const auto st = bc::station_for(w, player, config().s.range, true);
    const auto list = bc::candidates(w, g_learned.of(account), st, "", [w](uint32_t item) { return debug_name(w, item); });
    if (list.empty()) {
        EMF_REPLY("Bulk craft: nothing to repeat here yet: craft something by hand %s first",
                  st ? "at this station" : "(or stand at a station)");
        return;
    }
    std::string text;
    for (size_t i = 0; i < list.size() && i < 10; ++i)
        text += (i ? ", " : "") + recipe_label(w, list[i]) + (list[i].workshop ? "" : " (by hand)");
    EMF_REPLY("Bulk craft here: %s%s", text.c_str(), list.size() > 10 ? ", ..." : "");
}

// craft.teach <recipe hex> [player entity] (host): counts as crafted by hand (a recipe crafted before the mod came).
void cmd_teach(emf::World w, const std::vector<std::string>& a) {
    if (bc::sender_from_chat()) {  // the host's console or emf\commands.txt only, as the README says
        EMF_REPLY("craft.teach is for the host (the console or emf\\commands.txt), not the chat");
        return;
    }
    if (emf::server_session() != g_session) new_session();
    if (a.size() < 2 || a.size() > 3) {
        EMF_REPLY("usage: craft.teach <recipe id hex> [player entity]: let a player bulk-craft a recipe as if crafted by hand");
        return;
    }
    const uint32_t id = static_cast<uint32_t>(std::strtoul(a[1].c_str(), nullptr, 16));
    const uint32_t player = a.size() == 3 ? static_cast<uint32_t>(std::strtoul(a[2].c_str(), nullptr, 10))
                                          : sender_player(nullptr);
    const auto r = bc::recipe_by_id(w, id);
    const uint64_t account = bc::is_player(w, player) ? emf::player_account(w, player) : 0;
    if (!r || !account) {
        EMF_REPLY("%s", !r ? "no such recipe (hex id, e.g. 278240d5)" : "that is not a player of this world");
        return;
    }
    g_learned.learn(account, id);  // a choice seen by hand stays; a category recipe without one is still refused
    const bool saved = save_learned();
    EMF_REPLY("player %u may bulk-craft %s now%s", player, recipe_label(w, *r).c_str(), saved ? "" : " (not saved: no world file yet)");
    EMF_LOG(kMod, "craft.teach: player %u, recipe %08x", player, id);
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
    g_file.attach();
    if (!emf::add_program("bulk_craft", EMF_WORLD_SERVER, {}, {"CraftRecipeEvent"}, craft_program)) {
        EMF_LOG(kMod, "could not add the bulk_craft program: Bulk craft stays off");
        return false;
    }
    // The game's own crafts teach recipes: a CraftRecipeEvent with a player as the operator that this mod did not send
    // came from the game's crafting window (player_crafting), which offers only what the player has unlocked; its
    // category choice is kept and sent back. (Another mod sending such an event with a player as the operator would
    // teach too; none of the shipped ones does.)
    const bool watching = bc::watch_events(
        [](emf::World w, const gen::ecs::CraftRecipeEvent& e) {
            const uint32_t op = e.craftingOperatorId.id, recipe = e.recipeId.value;
            {
                std::lock_guard<std::mutex> l(g_send_lock);
                if (g_own.take(op, recipe)) return;
            }
            if (!bc::is_player(w, op)) return;  // villager jobs and other mods' chests
            if (emf::server_session() != g_session) new_session();
            bc::Choices choices{};
            for (size_t i = 0; i < choices.size(); ++i)
                choices[i] = {e.inputCategorySelections.selections[i].id.value,
                              e.inputCategorySelections.selections[i].selectedIndex};
            const bc::LearnResult r = g_learned.learn(emf::player_account(w, op), recipe, &choices);
            if (r == bc::LearnResult::New || r == bc::LearnResult::Choice) {  // not on a mere recency change
                save_learned();
                EMF_LOG(kMod, "player %u crafted %08x by hand%s: it can be bulk-crafted now", op, recipe,
                        r == bc::LearnResult::Choice ? " with other items" : "");
            }
        },
        [](emf::World, const gen::ecs::PickupEvent& e) {
            g_run.on_pickup(e.pickedUpByEntityId.id, e.itemId.value, e.itemCount, e.pickedUpFromEntityId.id);
        });
    g_blind = !watching;
    emf::on_tick(on_tick);
    const auto command = [](const char* name, const char* help, void (*f)(emf::World, const std::vector<std::string>&),
                            uint32_t flags) {
        if (!emf::add_command(name, help, f, flags)) EMF_LOG(kMod, "the command %s could not be added (see the core log)", name);
    };
    command("craft", "<n|max> [name]: more of what you crafted by hand here", cmd_craft,
            EMF_COMMAND_PLAYERS);
    command("craft.stop", ": stop your order", cmd_stop, EMF_COMMAND_PLAYERS);
    command("craft.list", ": what you can bulk-craft here", cmd_list, EMF_COMMAND_PLAYERS);
    command("craft.teach", "<recipe hex> [player]: as if crafted by hand (host)", cmd_teach, 0);
    g_switched_off = !switch_on();
    const Config c = config();
    EMF_LOG(kMod, "ready: %s or /craft repeats what a player crafted by hand (%s, at most %u per order, stations within %.0f m)",
            c.key_name.c_str(), c.s.pace == bc::Pace::Paced ? "paced" : "instant", c.s.max_amount, c.s.range);
    return true;
}
