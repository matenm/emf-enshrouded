// EMF C++ convenience layer over the C API (header-only). Compile it into each mod.
//
//   extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
//       if (!emf::init(api)) return false;
//       emf::on_tick([](emf::World w) { if (w.is_client()) { ... } });
//       return true;
//   }
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "emf_api.h"

namespace emf {

inline const EmfApi*& api_ref() {
    static const EmfApi* a = nullptr;
    return a;
}
inline const EmfApi* api() { return api_ref(); }

// The table a mod uses when the core's (or another host's) is smaller than this header's: a copy, zero-padded.
inline EmfApi& padded_api() {
    static EmfApi t{};
    return t;
}

// Call first in emf_mod_load. Fails if the core is older than the API this mod was built against.
// min_version (since 0.6.10): the lowest EMF_API_VERSION the mod needs, for a mod that checks emf::has_vNN() before it
// calls newer functions. On an older core or another host with a smaller table the mod then starts, with a zero-padded
// copy of the table: its newer functions are null and has_vNN() is false for them.
inline bool init(const EmfApi* a, uint32_t min_version = EMF_API_VERSION) {
    if (!a || a->version < 1) return false;
    if (min_version < 17) min_version = 17;  // the wrappers of v1-v16 call their functions without a has_vNN() check
    if (min_version >= EMF_API_VERSION) {
        if (a->size < sizeof(EmfApi)) {
            if (a->log)
                a->log("emf", "this mod was built for EMF API v%d; the core is v%u: update EMF (the mod stays off)",
                       EMF_API_VERSION, a->version);
            return false;
        }
    } else if (a->version < min_version || a->size < offsetof(EmfApi, log) + sizeof(a->log)) {
        if (a->log)
            a->log("emf", "this mod needs EMF API v%u; the core is v%u: update EMF (the mod stays off)", min_version,
                   a->version);
        return false;
    }
    if (a->size >= sizeof(EmfApi)) {
        api_ref() = a;
        return true;
    }
    EmfApi& t = padded_api();  // an older table: copy what it has, the rest stays null
    t = EmfApi{};
    std::memcpy(&t, a, a->size);
    api_ref() = &t;
    return true;
}

#define EMF_LOG(mod, ...) (::emf::api() ? ::emf::api()->log((mod), __VA_ARGS__) : (void)0)

class World {
public:
    World(EmfWorld* w = nullptr) : w_(w) {}
    explicit operator bool() const { return w_ != nullptr; }
    EmfWorld* raw() const { return w_; }

    uint8_t kind() const { return api()->world_kind(w_); }
    bool is_server() const { return kind() == EMF_WORLD_SERVER; }
    bool is_client() const { return kind() == EMF_WORLD_CLIENT; }
    uint32_t tick() const { return api()->world_tick(w_); }
    int64_t ecs_time_ns() const { return api()->ecs_time_ns(w_); }
    void* singleton(const char* name) const { return api()->singleton(w_, name); }

    // Component of type T (a generated struct with kHash). NULL if missing or the size differs.
    // Split components (e.g. ActiveNpcState): the per-entity half is its own type keyed by its
    // own hash, so read it as get<DynamicActiveNpcState>(id); the template half is not per entity.
    template <class T>
    T* get(uint32_t entity) const {
        uint64_t size = 0;
        void* p = api()->get_component(w_, T::kHash, entity, &size);
        return (p && size == sizeof(T)) ? static_cast<T*>(p) : nullptr;
    }
    // The entity's template data of type T: shared by every entity of the template, read-only (change a field with
    // emf::write_value). NULL if missing or if the game's size differs from T (a game update changed the layout).
    template <class T>
    const T* get_template(uint32_t entity) const {
        if (!api()->get_template_component) return nullptr;
        uint64_t size = 0;
        const void* p = api()->get_template_component(w_, T::kHash, entity, &size);
        return (p && size == sizeof(T)) ? static_cast<const T*>(p) : nullptr;
    }
    void* get_raw(uint32_t hash, uint32_t entity, uint64_t* size = nullptr) const {
        return api()->get_component(w_, hash, entity, size);
    }
    template <class F>
    void for_each_entity(F&& f) const {
        api()->for_each_entity(w_, [](uint32_t id, void* u) { (*static_cast<std::remove_reference_t<F>*>(u))(id); }, &f);
    }

private:
    EmfWorld* w_;
};

inline World server_world() { return World(api()->server_world()); }
inline World client_world() { return World(api()->client_world()); }
inline uint32_t local_player() { return api()->local_player(); }

// f(World) after every world step. The functor lives for the whole process.
template <class F>
void on_tick(F f) {
    auto* p = new F(std::move(f));
    api()->on_tick([](EmfWorld* w, void* u) { (*static_cast<F*>(u))(World(w)); }, p);
}

// f(World) once, after the next step of a world of `kind`. Callable from any thread.
template <class F>
void run_on_world(uint8_t kind, F f) {
    auto* p = new F(std::move(f));
    api()->run_on_world(kind, [](EmfWorld* w, void* u) {
        auto* fn = static_cast<F*>(u);
        (*fn)(World(w));
        delete fn;
    }, p);
}

// ---- v3: programs with events ----
// Adds a mod ECS program (call from emf_mod_load). f(program_context) runs once per tick on a game worker
// thread, on worlds of `world_kind`; inside it use emit<E>() and read<E>(). The functor lives forever.
template <class F>
bool add_program(const char* name, uint8_t world_kind, std::initializer_list<const char*> events_in,
                 std::initializer_list<const char*> events_out, F f) {
    std::vector<const char*> in(events_in), out(events_out);
    in.push_back(nullptr);
    out.push_back(nullptr);
    auto* p = new F(std::move(f));
    EmfProgramDef def{sizeof(EmfProgramDef), name, world_kind, [](void* ctx, void* u) { (*static_cast<F*>(u))(ctx); },
                      p, in.data(), out.data()};
    if (api()->register_program_def(&def)) return true;
    delete p;
    return false;
}

// v8: the same, plus the singletons the program reads ("g38_items::ItemRegistry") and the components it reads on
// OTHER entities ("CurrentTransform"); inside the callback use program_singleton() and read_other<T>().
template <class F>
bool add_program(const char* name, uint8_t world_kind, std::initializer_list<const char*> events_in,
                 std::initializer_list<const char*> events_out, std::initializer_list<const char*> singletons,
                 std::initializer_list<const char*> other_read, F f) {
    std::vector<const char*> in(events_in), out(events_out), single(singletons), other(other_read);
    for (auto* v : {&in, &out, &single, &other}) v->push_back(nullptr);
    auto* p = new F(std::move(f));
    EmfProgramDef def{sizeof(EmfProgramDef), name, world_kind, [](void* ctx, void* u) { (*static_cast<F*>(u))(ctx); },
                      p, in.data(), out.data(), single.data(), other.data()};
    if (api()->register_program_def(&def)) return true;
    delete p;
    return false;
}

// Sends event E (a generated struct with kName) from inside a program callback. Fill the returned
// struct before the callback returns. NULL if refused (see emit_event in emf_api.h).
template <class E>
E* emit(void* program_context) {
    return static_cast<E*>(api()->emit_event(program_context, E::kName, static_cast<uint32_t>(sizeof(E))));
}

// Calls f(const E&) for each event E queued for this tick, from inside a program callback.
template <class E, class F>
uint32_t read(void* program_context, F&& f) {
    using Fn = std::remove_reference_t<F>;
    return api()->read_events(program_context, E::kName, static_cast<uint32_t>(sizeof(E)),
                              [](const void* e, void* u) { (*static_cast<Fn*>(u))(*static_cast<const E*>(e)); }, &f);
}

// ---- v10 ----
// Opens a radial wheel; f(World, int choice) runs after the next step of a world of `kind` (choice -1 = cancelled).
// hold_vk: the key held while it is open (its release picks), 0 = click to pick. False if refused.
template <class F>
bool wheel(const char* title, const std::vector<std::string>& items, uint32_t hold_vk, uint8_t kind, F f) {
    std::vector<const char*> ptrs;
    for (const auto& s : items) ptrs.push_back(s.c_str());
    auto* holder = new F(std::move(f));
    EmfWheel w{};
    w.size = sizeof(EmfWheel);
    w.title = title;
    w.items = ptrs.data();
    w.count = static_cast<uint32_t>(ptrs.size());
    w.hold_key = hold_vk;
    w.world_kind = kind;
    w.cb = [](EmfWorld* world, int choice, void* u) {
        F* fn = static_cast<F*>(u);
        try {
            (*fn)(World(world), choice);
        } catch (const std::exception& e) {
            EMF_LOG("emf::wheel", "the wheel callback threw: %s", e.what());
        } catch (...) {
            EMF_LOG("emf::wheel", "the wheel callback threw an unknown C++ exception");
        }
        delete fn;
    };
    w.user = holder;
    if (api()->wheel_open(&w)) return true;
    delete holder;
    return false;
}

// ---- v10: notes ----
inline bool has_v10() { return api() && api()->size >= offsetof(EmfApi, notify) + sizeof(api()->notify); }
// printf-style note at the top of the screen for a few seconds (also in the console and the log). Any thread.
// The first argument is a format: show text from players or the game as EMF_NOTIFY("%s", text), never
// EMF_NOTIFY(text) (a '%' in it would read garbage or crash). The same holds for EMF_LOG and EMF_REPLY.
#define EMF_NOTIFY(...) (::emf::has_v10() ? ::emf::api()->notify(__VA_ARGS__) : (void)0)

// ---- v9 ----
// The hosted session number (see server_session in emf_api.h): compare it each tick to notice a reload or new game.
inline uint32_t server_session() { return api()->server_session(); }

// ---- v8: singletons and other entities inside a program ----
// A singleton declared in the program's EmfProgramDef.singletons (e.g. "g38_items::ItemRegistry"), or NULL.
inline void* program_singleton(void* program_context, const char* name) {
    return api()->program_singleton(program_context, name);
}
// Another entity's component T (a generated struct), declared by its short name in EmfProgramDef.other_read, from
// inside a program callback; NULL if the entity has none or the size of T differs from the game's.
template <class T>
const T* read_other(void* program_context, const char* component, uint32_t entity) {
    return static_cast<const T*>(
        api()->program_read_other(program_context, component, entity, static_cast<uint32_t>(sizeof(T))));
}

// ---- v4: gameplay event callbacks ----
// f(World, const E&) for every event E (a generated struct with kName) in worlds of `kind`, after the world's
// simulation step on its thread (components and spawn/destroy allowed). Call from emf_mod_load.
template <class E, class F>
bool on_event(uint8_t kind, F f) {
    auto* p = new F(std::move(f));
    const bool ok = api()->on_event(kind, E::kName, static_cast<uint32_t>(sizeof(E)),
                                    [](EmfWorld* w, const void* e, uint32_t, void* u) {
                                        (*static_cast<F*>(u))(World(w), *static_cast<const E*>(e));
                                    },
                                    p);
    if (!ok) delete p;
    return ok;
}

// ---- v7: item definitions ----
// Raises or lowers an item's maximum stack size in every live world. See set_item_max_stack in emf_api.h.
inline bool set_item_max_stack(uint32_t item_id, uint16_t max) {
    return api()->set_item_max_stack && api()->set_item_max_stack(item_id, max);
}

// Writes one value into game data (a field of get_template<T> or item_data), which the game keeps read-only.
template <class T>
bool write_value(const T* dst, const T& value) {
    return api()->write_memory && api()->write_memory(const_cast<T*>(dst), &value, sizeof(T));
}

// ---- v6: developer commands ----
namespace detail {
template <class F>
void command_trampoline(EmfWorld* w, int argc, const char* const* argv, void* u) {
    // A C++ exception (e.g. std::stoi on a typo) becomes a reply; only a real fault disables the command.
    const char* cmd = argc > 0 && argv[0] ? argv[0] : "command";
    try {
        std::vector<std::string> args(argv, argv + argc);
        (*static_cast<F*>(u))(World(w), args);
    } catch (const std::exception& e) {
        api()->command_reply("%s: %s", cmd, e.what());
    } catch (...) {
        api()->command_reply("%s: unknown C++ exception", cmd);
    }
}
}  // namespace detail

// Adds a command for the EMF console / emf/commands.txt. f(World, args) with args[0] = the command name. Runs on the
// host's server world after its step. Reply with EMF_REPLY("...", ...).
template <class F>
bool add_command(const char* name, const char* help, F f) {
    auto* p = new F(std::move(f));
    const bool ok = api()->register_command(name, help, &detail::command_trampoline<F>, p);
    if (!ok) delete p;
    return ok;
}
#define EMF_REPLY(...) (::emf::api() ? ::emf::api()->command_reply(__VA_ARGS__) : (void)0)

// ---- v17: commands any player may run ----
inline bool has_v17() {
    return api() && api()->size >= offsetof(EmfApi, register_command_ex) + sizeof(api()->register_command_ex);
}
// add_command with EMF_COMMAND_* flags: EMF_COMMAND_PLAYERS lets every player run it from the game's chat (not only the
// [chat] admins). Act on the sender (emf::command_sender), not the host. On a core older than v17 the command is added
// for the admins only (logged).
template <class F>
bool add_command(const char* name, const char* help, F f, uint32_t flags) {
    if (!has_v17()) {
        if (flags && api() && api()->log)
            api()->log("emf", "add_command('%s'): commands for players need EMF API v17; the core is v%u: admins only",
                       name ? name : "?", api()->version);
        return add_command(name, help, std::move(f));
    }
    auto* p = new F(std::move(f));
    EmfCommandDef d{};
    d.size = sizeof d;
    d.flags = flags;
    d.name = name;
    d.help = help;
    d.cb = &detail::command_trampoline<F>;
    d.user = p;
    const bool ok = api()->register_command_ex(&d);
    if (!ok) delete p;
    return ok;
}

// ---- v5: items ----
// The game's ItemId of an item GUID (FNV-1a-32 over the 16 bytes as stored); 0 for a null GUID.
inline uint32_t item_id(const uint8_t guid[16]) {
    bool zero = true;
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 16; ++i) {
        zero = zero && guid[i] == 0;
        h = (h ^ guid[i]) * 0x01000193u;
    }
    return zero ? 0u : h;
}

// f(const EmfItemInfo&) for every item the game knows (from the world's item registry).
template <class F>
void for_each_item(World w, F&& f) {
    using Fn = std::remove_reference_t<F>;
    api()->for_each_item(w.raw(), [](const EmfItemInfo* i, void* u) { (*static_cast<Fn*>(u))(*i); },
                         const_cast<void*>(static_cast<const void*>(&f)));
}

// Gives count x the item with this debug name (see find_item) to `player` as a pickup dropped 1 m above them (the
// game collects it when they are in range and auto-collect is on). Same rules as spawn. 0 if the name is unknown or
// the game refused. Looks the name up on every call: keep the id (find_item) for frequent use.
inline uint32_t give_item(World w, uint32_t player, const char* item_name, uint32_t count) {
    const uint32_t id = api()->find_item(w.raw(), item_name);
    return id ? api()->give_item(w.raw(), player, id, count) : 0u;
}

// ---- v8: items straight into an inventory ----
// Puts count x the item with this debug name into `entity`'s inventory on the next server tick (host only); then
// f(World, uint32_t added) runs on the server thread (added < count when it did not all fit). False when refused at
// once (unknown name, not the server world, ...; f is not called).
template <class F>
bool put_items(World w, uint32_t entity, const char* item_name, uint32_t count, F f) {
    const uint32_t id = api()->find_item(w.raw(), item_name);
    if (!id) return false;
    auto* holder = new F(std::move(f));
    const bool ok = api()->put_items(w.raw(), entity, id, count,
                                     [](EmfWorld* world, uint32_t, uint32_t, uint32_t, uint32_t added, void* u) {
                                         F* fn = static_cast<F*>(u);
                                         (*fn)(World(world), added);
                                         delete fn;
                                     },
                                     holder);
    if (!ok) delete holder;
    return ok;
}

// ---- v11 ----
// True when the core has the v11 functions (an older core: the wrappers below return false).
inline bool has_v11() { return api() && api()->size >= offsetof(EmfApi, take_items) + sizeof(api()->take_items); }

namespace detail {
template <class F>
EmfItemsRequest items_request(uint32_t entity, uint32_t item, uint32_t count, F* holder) {
    EmfItemsRequest r{};
    r.size = sizeof(EmfItemsRequest);
    r.entity = entity;
    r.item = item;
    r.count = count;
    r.cb = [](EmfWorld* world, uint32_t, uint32_t, uint32_t, uint32_t done, void* u) {
        F* fn = static_cast<F*>(u);
        try {
            (*fn)(World(world), done);
        } catch (...) {
            EMF_LOG("emf::items", "an items callback threw a C++ exception");
        }
        delete fn;
    };
    r.user = holder;
    return r;
}
}  // namespace detail

// Takes `count` of item `item` from `entity` (slot = the slot to take from first, 0 = any; exact_slot: only from that
// slot, refused when it no longer holds the item); f(World, uint32_t taken) runs after the server step. All or
// nothing. Host only. See take_items in emf_api.h (gear loses its pide).
template <class F>
bool take_items(World w, uint32_t entity, uint32_t item, uint32_t count, uint64_t slot, F f, bool exact_slot = false) {
    if (!has_v11()) return false;
    auto* holder = new F(std::move(f));
    EmfItemsRequest r = detail::items_request(entity, item, count, holder);
    r.slot = slot;
    if (exact_slot) r.flags |= EMF_ITEMS_EXACT_SLOT;
    if (api()->take_items(w.raw(), &r)) return true;
    delete holder;
    return false;
}

// put_items with an item level (0 = default) and rarity (0-4, 0xFF = default) for gear; f(World, uint32_t added).
template <class F>
bool put_items_ex(World w, uint32_t entity, uint32_t item, uint32_t count, uint32_t level, uint8_t rarity, F f) {
    if (!has_v11()) return false;
    auto* holder = new F(std::move(f));
    EmfItemsRequest r = detail::items_request(entity, item, count, holder);
    r.level = level;
    if (rarity != 0xFF) {
        r.flags |= EMF_ITEMS_RARITY;
        r.rarity = rarity;
    }
    if (api()->put_items_ex(w.raw(), &r)) return true;
    delete holder;
    return false;
}

// ---- v12 ----
inline bool has_v12() { return api() && api()->size >= offsetof(EmfApi, hud_set) + sizeof(api()->hud_set); }
// Keeps `text` on screen as line `id` (empty text removes it); see hud_set in emf_api.h. False on an older core.
inline bool hud(const char* id, const std::string& text, uint32_t place = EMF_HUD_TOP_RIGHT, uint32_t rgba = 0) {
    return has_v12() && api()->hud_set(id, text.c_str(), place, rgba);
}

// ---- v13: settings and mod info ----
inline bool has_v13() { return api() && api()->size >= offsetof(EmfApi, mod_info_at) + sizeof(api()->mod_info_at); }
// A setting of `mod` (declared in its mod.json or with setting_declare), or `fallback` when it is not declared (also on
// an older core). setting_text gives an empty value (a cleared key: no key) as "" (a core of EMF 0.2.0 or older: the
// fallback).
inline int64_t setting_int(const char* mod, const char* key, int64_t fallback) {
    return has_v13() ? api()->setting_int(mod, key, fallback) : fallback;
}
inline double setting_float(const char* mod, const char* key, double fallback) {
    return has_v13() ? api()->setting_float(mod, key, fallback) : fallback;
}
inline bool setting_bool(const char* mod, const char* key, bool fallback) {
    return setting_int(mod, key, fallback ? 1 : 0) != 0;
}
inline std::string setting_text(const char* mod, const char* key, const std::string& fallback = "") {
    if (!has_v13()) return fallback;
    char first[128];
    first[1] = 1;  // a declared empty value sets it to 0 (see setting_text in emf_api.h)
    const uint32_t n = api()->setting_text(mod, key, first, sizeof first);
    if (n == 0) return first[1] == 0 ? std::string() : fallback;  // empty, or not declared
    if (n < sizeof first) return std::string(first, n);
    std::string big(n + 1, '\0');
    api()->setting_text(mod, key, big.data(), n + 1);
    big.resize(n);
    return big;
}
// A "color" setting ("#rrggbb", EMF 0.3.0 and newer) as 0xRRGGBBAA with full alpha, ready for emf::hud; `fallback` when
// the setting is not declared or holds no colour. A "select" setting reads with setting_text (its option's value), a
// "keybind" with setting_text and emf::keys::parse_key, a "slider" with setting_int or setting_float.
inline uint32_t setting_rgba(const char* mod, const char* key, uint32_t fallback) {
    const std::string t = setting_text(mod, key, "");
    if (t.size() != 7 || t[0] != '#') return fallback;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = t[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return fallback;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    return (v << 8) | 0xFFu;
}
// Changes a setting (validated and saved like a change in the Mods tab). False when refused (the log says why).
inline bool set_setting(const char* mod, const char* key, const std::string& value) {
    return has_v13() && api()->setting_set(mod, key, value.c_str());
}
// f(World, key) after each change of one of `mod`'s settings (key "enabled" = its on/off switch), after the next step
// of any world, on its thread. The functor lives for the whole process. A C++ exception it throws is logged under
// `mod` (or "emf::on_setting_changed" when watching every mod) and the functor stays on.
template <class F>
bool on_setting_changed(const char* mod, F f) {
    if (!has_v13()) return false;
    struct Holder {
        F f;
        std::string mod;
    };
    auto* p = new Holder{std::move(f), mod ? mod : ""};
    api()->on_setting_changed(mod, [](EmfWorld* w, const char* changed, const char* key, void* u) {
        auto* h = static_cast<Holder*>(u);
        const char* who = h->mod.empty() ? "emf::on_setting_changed" : h->mod.c_str();
        try {
            h->f(World(w), std::string(key ? key : ""));
        } catch (const std::exception& e) {
            EMF_LOG(who, "the on_setting_changed callback threw on %s.%s: %s", changed ? changed : "?", key ? key : "?",
                    e.what());
        } catch (...) {
            EMF_LOG(who, "the on_setting_changed callback threw an unknown C++ exception on %s.%s", changed ? changed : "?",
                    key ? key : "?");
        }
    }, p);
    return true;
}
// ---- v14: patterns, hooks, build info ----
inline bool has_v14() { return api() && api()->size >= offsetof(EmfApi, game_build) + sizeof(api()->game_build); }
// The address `bytes` finds in the game exe's .text (or `section`): exactly one match, plus `offset`; with
// follow_rel32 the int32 there is followed (see EmfPattern). 0 when not found (the log says why) or on an older core.
inline uintptr_t find_pattern(const char* bytes, int32_t offset = 0, bool follow_rel32 = false, int32_t rel32_tail = 0,
                              const char* section = nullptr) {
    if (!has_v14()) return 0;
    EmfPattern p{};
    p.size = sizeof p;
    p.bytes = bytes;
    p.section = section;
    p.offset = offset;
    p.follow = follow_rel32 ? EMF_FOLLOW_REL32 : EMF_FOLLOW_NONE;
    p.rel32_tail = rel32_tail;
    return api()->find_pattern(&p);
}
// Hooks the function `bytes` finds (see hook_pattern in emf_api.h; hooks on the same function chain).
inline bool hook_pattern(const char* name, const char* bytes, void* detour, void** original, int32_t offset = 0,
                         bool follow_rel32 = false) {
    if (!has_v14()) return false;
    EmfPattern p{};
    p.size = sizeof p;
    p.bytes = bytes;
    p.offset = offset;
    p.follow = follow_rel32 ? EMF_FOLLOW_REL32 : EMF_FOLLOW_NONE;
    return api()->hook_pattern(name, &p, detour, original);
}
inline bool hook_address(const char* name, void* address, void* detour, void** original) {
    return has_v14() && api()->hook_address(name, address, detour, original);
}
// Which game build runs (number, sha256, known, server), or nothing on an older core.
inline std::optional<EmfGameBuild> game_build() {
    if (!has_v14()) return std::nullopt;
    EmfGameBuild b{};
    b.size = sizeof b;
    if (!api()->game_build(&b)) return std::nullopt;
    return b;
}

// What the loader found for mod `id` (installed, switched off, rejected...), or nothing.
inline std::optional<EmfModInfo> mod_info(const char* id) {
    if (!has_v13()) return std::nullopt;
    EmfModInfo info{};
    info.size = sizeof info;
    if (!api()->mod_info(id, &info)) return std::nullopt;
    return info;
}

// Command words with coordinates written many ways, as plain words: "3 4 5", "3, 4,5", "(3, 4, 5)", "[3 4 5]",
// "<3> <4> <5>", "3;4;5" and a pasted `pos` line "player 1 at (3.0, 4.0, 5.0) m" all give "3" "4" "5". Other words
// (#k, ground, closest) stay as they are.
inline std::vector<std::string> coordinate_words(const std::vector<std::string>& words) {
    std::string all;
    for (const auto& w : words) all += w + ' ';
    for (char& ch : all)
        if (ch == ',' || ch == ';' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '<' || ch == '>') ch = ' ';
    std::vector<std::string> out;
    std::string cur;
    for (char ch : all) {
        if (ch == ' ' || ch == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    auto is = [](const std::string& s, const char* w) {
        if (s.size() != std::strlen(w)) return false;
        for (size_t i = 0; i < s.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(s[i])) != w[i]) return false;
        return true;
    };
    if (out.size() >= 6 && is(out[0], "player") && is(out[2], "at")) {  // a pasted pos line (then maybe "ground")
        out.erase(out.begin(), out.begin() + 3);
        if (out.size() > 3 && is(out[3], "m")) out.erase(out.begin() + 3);
    }
    return out;
}

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> 16 bytes. ms_layout (default) = Windows GUID layout, which is
// how the game stores template GUIDs (first three groups little-endian); otherwise bytes in text order.
inline bool parse_guid(std::string_view s, uint8_t out[16], bool ms_layout = true) {
    if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    uint8_t b[16];
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '-') continue;
        const int hi = hex(s[i]), lo = (i + 1 < s.size()) ? hex(s[i + 1]) : -1;
        if (hi < 0 || lo < 0 || n >= 16) return false;
        b[n++] = static_cast<uint8_t>(hi * 16 + lo);
        ++i;
    }
    if (n != 16) return false;
    if (ms_layout) {
        std::reverse(b, b + 4);
        std::reverse(b + 4, b + 6);
        std::reverse(b + 6, b + 8);
    }
    std::copy(b, b + 16, out);
    return true;
}

// 16 bytes -> "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (the inverse of parse_guid with the same layout).
inline std::string guid_to_text(const uint8_t g[16], bool ms_layout = true) {
    uint8_t b[16];
    std::copy(g, g + 16, b);
    if (ms_layout) {
        std::reverse(b, b + 4);
        std::reverse(b + 4, b + 6);
        std::reverse(b + 6, b + 8);
    }
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[b[i] >> 4];
        s += hex[b[i] & 15];
    }
    return s;
}

// Address of a resolved engine function by signature name, cast to a function pointer type.
template <class Fn>
Fn resolve(const char* sig_name) {
    return reinterpret_cast<Fn>(api()->resolve(sig_name));
}

// ---- v16: writes the players see ----
inline bool has_v16() {
    return api() && api()->size >= offsetof(EmfApi, program_write_other) + sizeof(api()->program_write_other);
}
// Another entity's component T, declared in EmfProgramDef.other_write, to write into now (inside the program
// callback); the game marks it changed as its own programs do (meant for the players' copies and the save; not
// verified with joined players). NULL if refused (see program_write_other) or on an older core.
template <class T>
T* write_other(void* program_context, const char* component, uint32_t entity) {
    if (!has_v16()) return nullptr;
    return static_cast<T*>(
        api()->program_write_other(program_context, component, entity, static_cast<uint32_t>(sizeof(T))));
}
// v16: add_program with the components the program writes on OTHER entities (write_other<T>()). False (logged) when
// other_write is not empty and the core is older than v16: it would ignore the list and every write would give NULL.
template <class F>
bool add_program(const char* name, uint8_t world_kind, std::initializer_list<const char*> events_in,
                 std::initializer_list<const char*> events_out, std::initializer_list<const char*> singletons,
                 std::initializer_list<const char*> other_read, std::initializer_list<const char*> other_write, F f) {
    if (other_write.size() && !has_v16()) {
        if (api() && api()->log)
            api()->log("emf", "add_program('%s'): other_write needs EMF API v16; the core is v%u (update EMF)",
                       name ? name : "?", api()->version);
        return false;
    }
    std::vector<const char*> in(events_in), out(events_out), single(singletons), oread(other_read), owrite(other_write);
    for (auto* v : {&in, &out, &single, &oread, &owrite}) v->push_back(nullptr);
    auto* p = new F(std::move(f));
    EmfProgramDef def{sizeof(EmfProgramDef), name, world_kind, [](void* ctx, void* u) { (*static_cast<F*>(u))(ctx); },
                      p, in.data(), out.data(), single.data(), oread.data(), owrite.data()};
    if (api()->register_program_def(&def)) return true;
    delete p;
    return false;
}

// ---- helpers (any core): the time of day, killing without a hit ----
namespace detail {
constexpr size_t kDaytimeTimeOfDay = 0x48;  // g38_daytime::Daytime: i64 ns since midnight (docs/ENGINE_MAP.md 2.6)
constexpr int64_t kDayNs = 24ll * 3600ll * 1000000000ll;
// The components kill reads, by their types.json hash and size (emf/generated/components.hpp has them too; the tests
// check they agree): Health = attribute header + int32 dataStorage[8] at +8; NetworkHealth = {u32 health, u32 healthMax}.
constexpr uint32_t kHealthHash = 0xdc101091u, kHealthSize = 40;
constexpr uint32_t kNetworkHealthHash = 0x216eb5f9u, kNetworkHealthSize = 8;
constexpr uint32_t kPlayerStateHash = 0x6ff967bbu;

inline std::optional<double> time_of_day_with(const EmfApi* a, EmfWorld* w) {
    if (!a || !w || !a->singleton) return std::nullopt;
    const auto* daytime = static_cast<const uint8_t*>(a->singleton(w, "g38_daytime::Daytime"));
    if (!daytime) return std::nullopt;
    int64_t ns = -1;
    std::memcpy(&ns, daytime + kDaytimeTimeOfDay, sizeof ns);
    if (ns < 0 || ns >= kDayNs) return std::nullopt;  // not a time of day: the layout changed (a game update?)
    return static_cast<double>(ns) / 3600e9;
}

// The index of the Health attribute slot that holds `current` (the replicated NetworkHealth.health), -1 if none does.
inline int current_health_slot(const void* storage, uint32_t count, int32_t current) {
    for (uint32_t i = 0; i < count && i < 8; ++i) {
        int32_t v = 0;
        std::memcpy(&v, static_cast<const uint8_t*>(storage) + 4 * i, 4);
        if (v == current) return static_cast<int>(i);
    }
    return -1;
}

// The core has the v18 attribute functions and they work on this game build (their signatures resolved).
inline bool attributes_with(const EmfApi* a) {
    return a && a->size >= offsetof(EmfApi, damage) + sizeof(a->damage) && a->attribute_get && a->attribute_add &&
           a->resolve && a->resolve("AttrFind") && a->resolve("AttrAdd");
}

// The current and maximum health of `entity`: through the game's attribute registry (v18, a world this game hosts), else
// the replicated NetworkHealth (also on a client of someone else's game). False when neither is there.
inline bool health_with(const EmfApi* a, EmfWorld* w, uint32_t entity, int32_t* cur, int32_t* max) {
    if (!a || !w || !entity) return false;
    if (a->size >= offsetof(EmfApi, damage) + sizeof(a->damage) && a->attribute_get && a->world_kind &&
        a->world_kind(w) == EMF_WORLD_SERVER) {
        int32_t c = 0, m = 0;
        if (a->attribute_get(w, entity, EMF_ATTR_HEALTH, &c) && a->attribute_get(w, entity, EMF_ATTR_MAX_HEALTH, &m)) {
            if (cur) *cur = c;
            if (max) *max = m;
            return true;
        }
    }
    uint64_t size = 0;
    const auto* net = a->get_component ? static_cast<const uint8_t*>(a->get_component(w, kNetworkHealthHash, entity, &size))
                                       : nullptr;
    if (!net || size != kNetworkHealthSize) return false;
    uint32_t v[2] = {};
    std::memcpy(v, net, sizeof v);
    if (cur) *cur = static_cast<int32_t>(v[0]);
    if (max) *max = static_cast<int32_t>(v[1]);
    return true;
}

inline bool kill_with(const EmfApi* a, EmfWorld* w, uint32_t entity, std::string* why = nullptr) {
    auto no = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (!a || !w || !entity || !a->get_component || !a->world_kind) return no("no world or entity");
    if (a->world_kind(w) != EMF_WORLD_SERVER) return no("server world only (Health exists only on the host)");
    if (a->get_component(w, kPlayerStateHash, entity, nullptr)) return no("players are never killed this way");
    // v18: the current-health attribute through the game's registry and attrAdd (the attribute the game's death program
    // reads, wherever its slot is)
    if (attributes_with(a)) {
        int32_t current = 0;
        if (!a->attribute_get(w, entity, EMF_ATTR_HEALTH, &current)) return no("the entity has no current-health attribute");
        if (current <= 0) return no("already dead");
        // the most negative delta: the core's clamp floors it at 0 whatever the health is by then
        if (!a->attribute_add(w, entity, EMF_ATTR_HEALTH, INT32_MIN)) return no("attribute_add refused (see the log)");
        return true;
    }
    // without them (an older core, or a game build where they were not found): Health slot 0
    uint64_t hs = 0, ns = 0;
    auto* health = static_cast<uint8_t*>(a->get_component(w, kHealthHash, entity, &hs));
    const auto* net = static_cast<const uint8_t*>(a->get_component(w, kNetworkHealthHash, entity, &ns));
    if (!health || hs != kHealthSize || !net || ns != kNetworkHealthSize) return no("the entity has no Health");
    uint32_t current = 0;
    std::memcpy(&current, net, 4);
    if (current == 0) return no("already dead");
    // Slot 0 only: the current-health slot confirmed in game (a mini-boss held 1498,0,1498,0,0,0,0,1498). Another slot
    // with the same value is the maximum on a creature at full health, never written.
    if (current_health_slot(health + 8, 1, static_cast<int32_t>(current)) != 0)
        return no("the Health slot 0 does not hold the current health (it is changing: try again next step)");
    const int32_t zero = 0;
    std::memcpy(health + 8, &zero, 4);
    return true;
}
}  // namespace detail

// ---- v18: attributes and damage ----
inline bool has_v18() { return api() && api()->size >= offsetof(EmfApi, damage) + sizeof(api()->damage); }
// Current and maximum health (the attribute registry in a world this game hosts, else NetworkHealth). World callback.
inline bool health(World w, uint32_t entity, int32_t* cur, int32_t* max) {
    return detail::health_with(api(), w.raw(), entity, cur, max);
}
// An attribute (EMF_ATTR_HEALTH, EMF_ATTR_MAX_HEALTH, EMF_ATTR_STAMINA, EMF_ATTR_MANA, ...) of `entity` in a world this
// game hosts, as the game reads it; nothing when the entity has none, on a client world, or on an older core.
inline std::optional<int32_t> attribute(World w, uint32_t entity, uint32_t attr) {
    int32_t v = 0;
    if (!has_v18() || !api()->attribute_get || !api()->attribute_get(w.raw(), entity, attr, &v)) return std::nullopt;
    return v;
}
// Adds delta to an attribute (clamped: 0..max for health). No hit: no kill credit, reaction or killing blow (see
// attribute_add in emf_api.h). False when refused (the log says why) or on an older core.
inline bool attribute_add(World w, uint32_t entity, uint32_t attr, int32_t delta) {
    return has_v18() && api()->attribute_add && api()->attribute_add(w.raw(), entity, attr, delta);
}
// v22: a player's XP and level are attributes too (EMF_ATTR_XP_PENDING, _XP, _XP_REQUIRED, _LEVEL in emf_api.h), and
// the game's XP curve, level cap and a level setter are functions.
inline bool has_v22() {
    return has_v18() && api()->version >= 22 &&
           api()->size >= offsetof(EmfApi, player_level_set) + sizeof(api()->player_level_set);
}
// Gives `amount` (> 0) XP to a player the way the game's own XP sources do: as pending XP, which the game counts on its
// next steps and levels the player up from (with the level-up event and the skill points). Saved with the character.
// False when refused, for amount <= 0, or on an older core.
inline bool give_xp(World w, uint32_t player, int32_t amount) {
    return amount > 0 && has_v22() && attribute_add(w, player, EMF_ATTR_XP_PENDING, amount);
}
// The XP `level` needs before the next one (the game's curve); 0 outside 1..cap, on a client or an older core.
inline uint32_t xp_required(World w, uint32_t level) {
    return has_v22() && api()->player_xp_required ? api()->player_xp_required(w.raw(), level) : 0u;
}
// The highest level a player can reach; 0 when unknown (a client, an older core).
inline uint32_t level_cap(World w) { return has_v22() && api()->player_level_cap ? api()->player_level_cap(w.raw()) : 0u; }
// Sets a player's level (1..cap) with `xp` towards the next one, raising or lowering, without level-up events (to level
// up the game's way, give_xp). The skill points follow the level. False when refused (the log says why) or on an older
// core; true from outside a world callback means queued for the server world's next step.
inline bool set_level(World w, uint32_t player, uint32_t level, uint32_t xp = 0) {
    return has_v22() && api()->player_level_set && api()->player_level_set(w.raw(), player, level, xp);
}

// ---- v23: add a component to an entity, or remove one (change_component in emf_api.h; only the listed ones) ----
inline bool has_v23() {
    return api() && api()->version >= 23 &&
           api()->size >= offsetof(EmfApi, change_component) + sizeof(api()->change_component);
}
namespace detail {
inline bool change_component(World w, uint32_t entity, uint32_t op, uint32_t hash, const void* data, uint32_t size,
                             uint32_t flags, EmfComponentCallback cb, void* user) {
    if (!has_v23() || !api()->change_component) return false;
    EmfComponentChange c{};
    c.size = sizeof c;
    c.op = op;
    c.entity = entity;
    c.hash = hash;
    c.data = data;
    c.data_size = size;
    c.flags = flags;
    c.cb = cb;
    c.user = user;
    return api()->change_component(w.raw(), &c);
}
template <class F>
void component_done(EmfWorld* w, uint32_t, uint32_t, uint32_t status, const char* why, void* user) {
    auto* f = static_cast<F*>(user);
    try {
        (*f)(World(w), status, why ? why : "");
    } catch (...) {  // nothing may escape into EMF
    }
    delete f;
}
template <class F>
bool change_component_cb(World w, uint32_t entity, uint32_t op, uint32_t hash, const void* data, uint32_t size,
                         uint32_t flags, F f) {
    auto* p = new F(std::move(f));
    const bool ok = change_component(w, entity, op, hash, data, size, flags, &component_done<F>, p);
    if (!ok) delete p;  // refused at once: the callback is never called
    return ok;
}
}  // namespace detail
// Adds `value` (a generated component struct: emf::gen::ecs::ScaleComponent{1.25f}) to `entity`, after the server
// world's next step. flags: EMF_COMPONENT_PERSISTENT for one the world save keeps. False when refused (logged).
template <class T>
bool add_component(World w, uint32_t entity, const T& value, uint32_t flags = 0) {
    return detail::change_component(w, entity, EMF_COMPONENT_ADD, T::kHash, &value, static_cast<uint32_t>(sizeof(T)),
                                    flags, nullptr, nullptr);
}
// The same with f(World, uint32_t status, const char* why) once it is applied (EMF_COMPONENT_DONE / _ALREADY /
// _FAILED), on the server world's thread. The World is empty (false) when the world ended first.
template <class T, class F>
bool add_component(World w, uint32_t entity, const T& value, uint32_t flags, F f) {
    return detail::change_component_cb(w, entity, EMF_COMPONENT_ADD, T::kHash, &value, static_cast<uint32_t>(sizeof(T)),
                                       flags, std::move(f));
}
// Removes component T from `entity` (one EMF lets mods remove, or one this mod added in this session).
template <class T>
bool remove_component(World w, uint32_t entity, uint32_t flags = 0) {
    return detail::change_component(w, entity, EMF_COMPONENT_REMOVE, T::kHash, nullptr, 0, flags, nullptr, nullptr);
}
template <class T, class F>
bool remove_component(World w, uint32_t entity, uint32_t flags, F f) {
    return detail::change_component_cb(w, entity, EMF_COMPONENT_REMOVE, T::kHash, nullptr, 0, flags, std::move(f));
}
// `amount` of true damage from attacker (a player gets the kill credit and XP; 0 = none) to target as a game hit,
// applied on the next step with a HitEvent, a light hit reaction and aggro (see damage in emf_api.h). False when refused
// or on an older core.
inline bool damage(World w, uint32_t attacker, uint32_t target, int32_t amount) {
    return has_v18() && api()->damage && api()->damage(w.raw(), attacker, target, amount);
}

// The time of day in hours (0 <= h < 24) from the world's g38_daytime::Daytime, the clock the clock mod shows; nothing
// when the world has none (or the value is no time of day). The server and the client world each have one (a client
// follows the host's). World callback.
inline std::optional<double> time_of_day(World w) { return detail::time_of_day_with(api(), w.raw()); }

// Kills `entity` without a hit: its current-health attribute becomes 0 and the game's own death program sends
// EntityDiedEvent on the next step; the corpse keeps its loot. There is no attacker, so no kill credit (emf::damage with
// more than its health gives one). On a v18 core it goes through the attribute registry and attrAdd (attribute_add);
// on an older core, or a game build where those were not found, it writes Health slot 0 when that holds
// NetworkHealth.health (confirmed in game). Server world, inside a world callback. False: not the server world, no
// Health, already dead, a player (never), or (old path) the current health is changing this step (try again).
inline bool kill(World w, uint32_t entity) { return detail::kill_with(api(), w.raw(), entity); }

// ---- v15: commands from players ----
inline bool has_v15() {
    return api() && api()->size >= offsetof(EmfApi, command_sender) + sizeof(api()->command_sender);
}
// Inside a command callback: who typed it (from_chat 1: a player in the game's chat). False outside commands or on an
// older core.
inline bool command_sender(EmfCommandSender* out) {
    if (!has_v15() || !out) return false;
    out->size = sizeof(EmfCommandSender);
    return api()->command_sender(out);
}

// ---- v19: chat lines and saves on a server (see emf_api.h) ----
inline bool has_v19() { return api() && api()->size >= offsetof(EmfApi, request_save) + sizeof(api()->request_save); }
inline bool has_v30();
// A chat line only `player` sees (their game shows it under their own name; system (v30): with no name in front, as the
// game shows a player joining); the host of a game you host gets a notify. tell takes no format: pass text from players
// as it is. False when refused (logged) or on an older core.
inline bool tell(World w, uint32_t player, const std::string& text, bool system = false) {
    if (system && has_v30() && api()->chat_tell_ex)
        return api()->chat_tell_ex(w.raw(), player, text.c_str(), EMF_CHAT_SYSTEM);
    return has_v19() && api()->chat_tell && api()->chat_tell(w.raw(), player, text.c_str());
}
// The same line for every connected player.
inline bool broadcast(const std::string& text, bool system = false) {
    if (system && has_v30() && api()->chat_broadcast_ex) return api()->chat_broadcast_ex(text.c_str(), EMF_CHAT_SYSTEM);
    return has_v19() && api()->chat_broadcast && api()->chat_broadcast(text.c_str());
}
// The player's name as the game knows it; nothing when not known (then say "player #<id>").
inline std::optional<std::string> player_name(World w, uint32_t player) {
    char buf[256] = {};
    if (!has_v19() || !api()->player_name || !api()->player_name(w.raw(), player, buf, sizeof buf) || !buf[0])
        return std::nullopt;
    return std::string(buf);
}
// f(World, account, entity, text) for every player's chat line that is no EMF command, on the server world's thread
// after its next step (read-only). The functor lives for the whole process; a C++ exception it throws is logged under
// `mod` and it stays on. False when players' chat is off here (logged) or on an older core.
template <class F>
bool on_chat(const char* mod, F f) {
    if (!has_v19() || !api()->on_chat) return false;
    struct Holder {
        F f;
        std::string mod;
    };
    auto* p = new Holder{std::move(f), mod ? mod : "emf::on_chat"};
    const bool ok = api()->on_chat([](EmfWorld* w, uint64_t account, uint32_t entity, const char* text, void* u) {
        auto* h = static_cast<Holder*>(u);
        try {
            h->f(World(w), account, entity, std::string(text ? text : ""));
        } catch (const std::exception& e) {
            EMF_LOG(h->mod.c_str(), "the on_chat callback threw: %s", e.what());
        } catch (...) {
            EMF_LOG(h->mod.c_str(), "the on_chat callback threw an unknown C++ exception");
        }
    }, p);
    if (!ok) delete p;
    return ok;
}
// ---- v28: player events ----
inline bool has_v28() { return api() && api()->size >= offsetof(EmfApi, on_player) + sizeof(api()->on_player); }
// f(World, const EmfPlayerEvent&) for each player who joins, leaves or dies on the server world (EMF_PLAYER_JOINED,
// _LEFT, _DIED; its thread, after its step). Call it in emf_mod_load for "died" too. The functor lives for the whole
// process; a C++ exception it throws is logged under `mod`. False on an older core.
template <class F>
bool on_player(const char* mod, F f) {
    if (!has_v28() || !api()->on_player) return false;
    struct Holder {
        F f;
        std::string mod;
    };
    auto* p = new Holder{std::move(f), mod ? mod : "emf::on_player"};
    const bool ok = api()->on_player([](EmfWorld* w, const EmfPlayerEvent* e, void* u) {
        auto* h = static_cast<Holder*>(u);
        try {
            h->f(World(w), *e);
        } catch (const std::exception& ex) {
            EMF_LOG(h->mod.c_str(), "the on_player callback threw: %s", ex.what());
        } catch (...) {
            EMF_LOG(h->mod.c_str(), "the on_player callback threw an unknown C++ exception");
        }
    }, p);
    if (!ok) delete p;
    return ok;
}

// ---- v29: the game's item names and language (the game exe only) ----
inline bool has_v29() { return api() && api()->size >= offsetof(EmfApi, game_language) + sizeof(api()->game_language); }
// The item's name as the game shows it, in the player's language: for EMF's screens (windows, wheels, HUD lines;
// English where they cannot draw Japanese, Korean, Chinese or Thai), or with game_text for the game's own chat (the
// player's language always). Empty on the dedicated server, before the game's text is known, for an item without a
// name, or on an older core: use the debug name then. (Named so it does not clash with mods' own item_name helpers.)
inline std::string shown_item_name(World w, uint32_t item, bool game_text = false) {
    if (!has_v29() || !api()->item_name) return {};
    char buf[256];
    return api()->item_name(w.raw(), item, game_text ? EMF_ITEM_NAME_GAME_TEXT : 0u, buf, sizeof buf) ? std::string(buf)
                                                                                                      : std::string();
}
// The language the game shows text in ("en_us", "de_de", ...); empty on the dedicated server or an older core.
inline std::string game_language() {
    if (!has_v29() || !api()->game_language) return {};
    char code[16];
    return api()->game_language(code, sizeof code) ? std::string(code) : std::string();
}

// ---- v30: character names and lines without a speaker (tell / broadcast above take `system`) ----
inline bool has_v30() {
    return api() && api()->size >= offsetof(EmfApi, chat_broadcast_ex) + sizeof(api()->chat_broadcast_ex);
}
// The player's character name (the Social tab's), server world only: may already be there at the join or come a few
// seconds later, and may change; read it when you need it, never cache it, key data by the account. Nothing when not
// known or on an older core.
inline std::optional<std::string> character_name(World w, uint32_t player) {
    char buf[260] = {};
    if (!has_v30() || !api()->player_character_name || !api()->player_character_name(w.raw(), player, buf, sizeof buf) ||
        !buf[0])
        return std::nullopt;
    return std::string(buf);
}

// ---- v31: hits (rules here; on_hit / on_hit_before through api() with EmfHitCallback / EmfHitBeforeCallback) ----
inline bool has_v31() {
    return api() && api()->size >= offsetof(EmfApi, hit_rule_remove) + sizeof(api()->hit_rule_remove);
}
// A rule with nothing set yet (matches every damage hit, scale 1): set what it matches and does, then add_hit_rule.
inline EmfHitRule hit_rule() {
    EmfHitRule r{};
    r.size = sizeof r;
    r.scale = 1.0f;
    return r;
}
// Its id, or 0 when refused (logged) or on an older core.
inline uint32_t add_hit_rule(const EmfHitRule& r) { return has_v31() && api()->hit_rule_add ? api()->hit_rule_add(&r) : 0; }
inline bool remove_hit_rule(uint32_t id) { return has_v31() && api()->hit_rule_remove && api()->hit_rule_remove(id); }

// ---- v32: buffs (buff_types, buff_type, buff_text, buffs_of, buff_apply, buff_remove, on_buff through api()) ----
inline bool has_v32() { return api() && api()->size >= offsetof(EmfApi, on_buff) + sizeof(api()->on_buff); }

// ---- v33: markers (marker_set ... marker_unsend through api()) ----
inline bool has_v33() { return api() && api()->size >= offsetof(EmfApi, marker_unsend) + sizeof(api()->marker_unsend); }

// ---- v34: the map's mouse, map actions, the ground (map_cursor ... ground_at through api()) ----
inline bool has_v34() { return api() && api()->size >= offsetof(EmfApi, ground_at) + sizeof(api()->ground_at); }
// v35: player_view, fog_at, map_markers, canvas_set, canvas_draw (a minimap's parts)
inline bool has_v35() { return api() && api()->size >= offsetof(EmfApi, canvas_draw) + sizeof(api()->canvas_draw); }
// v36: remove_building (a placed object removed like a pick-up)
inline bool has_v36() { return api() && api()->size >= offsetof(EmfApi, remove_building) + sizeof(api()->remove_building); }
// v37: path_find, path_cancel (paths on the game's own path grid)
inline bool has_v37() { return api() && api()->size >= offsetof(EmfApi, path_cancel) + sizeof(api()->path_cancel); }
// v38: door_state, door_set, door_lock
inline bool has_v38() { return api() && api()->size >= offsetof(EmfApi, door_lock) + sizeof(api()->door_lock); }
// v39: enemy_order, enemy_order_cancel, enemy_target
inline bool has_v39() { return api() && api()->size >= offsetof(EmfApi, enemy_target) + sizeof(api()->enemy_target); }
// v40: enemy_move, enemy_move_cancel, enemy_moving (move orders)
inline bool has_v40() { return api() && api()->size >= offsetof(EmfApi, enemy_moving) + sizeof(api()->enemy_moving); }
// v41 (0.6.10, the version only): EmfMoveOrder.stuck_seconds and EMF_MOVE_PEACEFUL_ALL are understood
inline bool has_v41() { return has_v40() && api()->version >= 41; }
// v42 (0.6.11, the version only): EmfPathResult.doors (the door entity of each door point); check result->size too
inline bool has_v42() { return has_v40() && api()->version >= 42; }
// v43: player_permissions (the game's own permissions of a player)
inline bool has_v43() { return api() && api()->size >= offsetof(EmfApi, player_permissions) + sizeof(api()->player_permissions); }
// The player's permissions as the game keeps them (EMF_PERM_*), on the server world; nothing for a non-player, on a
// client world or an older core. Read it when you need it (a host can change it; a reconnect is a new entity).
inline std::optional<uint32_t> player_permissions(World w, uint32_t player) {
    uint32_t perms = 0;
    if (!has_v43() || !api()->player_permissions || !api()->player_permissions(w.raw(), player, &perms)) return std::nullopt;
    return perms;
}
// The game counts the player as an admin (EMF_PERM_KICK_BAN: a dedicated server's Admin group, a host's admins).
inline bool game_admin(World w, uint32_t player) {
    const auto p = player_permissions(w, player);
    return p && (*p & EMF_PERM_KICK_BAN);
}
// v44: breaking player-built blocks, never terrain (break_blocks, block_at, find_wall)
inline bool has_v44() { return api() && api()->size >= offsetof(EmfApi, find_wall) + sizeof(api()->find_wall); }
// API v45 (EmfBlockInfo.world, EMF_BREAK_WORLD: the world's own blocks, its ruins, are never broken unless asked)
inline bool has_v45() { return has_v44() && api()->version >= 45; }
// v46: villager moves (villager_move, villager_move_cancel, villager_moving)
inline bool has_v46() { return api() && api()->size >= offsetof(EmfApi, villager_moving) + sizeof(api()->villager_moving); }
// Walks villager order.enemy through its points inside its own area (order.size is set here): the move's id, or 0 with
// order.why. hold_seconds / EMF_MOVE_HOLD keep it at the last point after the arrival callback.
inline uint32_t villager_move(World w, EmfMoveOrder& order, EmfMoveCallback cb = nullptr, void* user = nullptr) {
    order.size = sizeof order;
    if (!has_v46() || !api()->villager_move) {
        static const char kText[] = "this EMF core is too old (villager_move needs API v46)";
        static_assert(sizeof kText <= sizeof order.why, "the text fits");
        std::memcpy(order.why, kText, sizeof kText);
        return 0;
    }
    return api()->villager_move(w.raw(), &order, cb, user);
}
inline bool villager_move_cancel(uint32_t id) { return has_v46() && api()->villager_move_cancel && api()->villager_move_cancel(id); }
inline uint32_t villager_moving(World w, uint32_t villager) {
    return has_v46() && api()->villager_moving ? api()->villager_moving(w.raw(), villager) : 0;
}
// Queues the break (req.size is set here): its id, or 0 with req.why. cb gets the counts after the server step.
inline uint32_t break_blocks(World w, EmfBreakBlocks& req, EmfBreakCallback cb = nullptr, void* user = nullptr) {
    req.size = sizeof req;
    if (!has_v44() || !api()->break_blocks) {
        static const char kText[] = "this EMF core is too old (break_blocks needs API v44)";
        static_assert(sizeof kText <= sizeof req.why, "the text fits");
        std::memcpy(req.why, kText, sizeof kText);
        return 0;
    }
    return api()->break_blocks(w.raw(), &req, cb, user);
}
// What is at a point (inside a world callback on the server world); nothing when refused or on an older core.
inline std::optional<EmfBlockInfo> block_at(World w, float x, float y, float z) {
    EmfBlockInfo b{};
    b.size = sizeof b;
    b.world = -1;  // a v44 core writes 40 bytes: world stays "not known"
    if (!has_v44() || !api()->block_at || !api()->block_at(w.raw(), x, y, z, &b)) return std::nullopt;
    return b;
}
// The first building block on the line (at most 64 m of it); nothing when the line is clear or when refused.
inline std::optional<EmfBlockInfo> find_wall(World w, const float from[3], const float to[3]) {
    EmfBlockInfo b{};
    b.size = sizeof b;
    b.world = -1;  // a v44 core writes 40 bytes: world stays "not known"
    if (!has_v44() || !api()->find_wall || !api()->find_wall(w.raw(), from, to, &b)) return std::nullopt;
    return b;
}
// v25: E prompts (offer_add, offer_remove); v26: pictures in windows (offer_active ends it); v27: mod messages
inline bool has_v25() { return api() && api()->size >= offsetof(EmfApi, offer_remove) + sizeof(api()->offer_remove); }
inline bool has_v26() { return api() && api()->size >= offsetof(EmfApi, offer_active) + sizeof(api()->offer_active); }
inline bool has_v27() { return api() && api()->size >= offsetof(EmfApi, net_peer) + sizeof(api()->net_peer); }

// Asks the game to save the world now (on_save follows once written). False when refused (logged) or on an older core.
inline bool request_save() { return has_v19() && api()->request_save && api()->request_save(); }

// ---- v21: world settings (docs/modding/world_settings.md) ----
inline bool has_v21() {
    return api() && api()->size >= offsetof(EmfApi, world_setting_info) + sizeof(api()->world_setting_info);
}
// The running world's value (durations in minutes); nothing for an unknown key, no server world or an older core.
inline std::optional<double> world_setting(const char* key) {
    double v = 0;
    if (!has_v21() || !api()->world_setting_get || !api()->world_setting_get(key, &v)) return std::nullopt;
    return v;
}
// Queued for the next server step; false (logged with the allowed range) when refused. persist: keep it for this world.
inline bool set_world_setting(const char* key, double value, bool persist = false) {
    return has_v21() && api()->world_setting_set && api()->world_setting_set(key, value, persist ? EMF_WS_PERSIST : 0u);
}
// Back to the world's own value; nullptr: every setting. persist: forget the kept value too.
inline bool reset_world_setting(const char* key = nullptr, bool persist = false) {
    return has_v21() && api()->world_setting_reset && api()->world_setting_reset(key, persist ? EMF_WS_PERSIST : 0u);
}

// ---- v24: mod windows (reference.md "v24") ----
inline bool has_v24() {
    return api() && api()->version >= 24 && api()->size >= offsetof(EmfApi, window_close) + sizeof(api()->window_close);
}
namespace ui {
// A window event: kind EMF_UI_CLICK / _RIGHT_CLICK (key: the widget's) or EMF_UI_CLOSED (reason: EMF_UI_BY_*; the last
// one; with flags EMF_UI_NO_WORLD the World is empty).
struct Event {
    uint32_t kind = 0;
    std::string window, key;
    uint32_t reason = 0;
    uint32_t flags = 0;
};
// A window built in order, top to bottom: `same_line()` puts the next widget on the line of the one before.
//   emf::ui::Window w("mymod.stable", "Beasts");
//   w.heading("Active").slot("active.1", "Wolf").same_line().slot("active.2", "Slot 2", 0, EMF_UI_EMPTY);
//   w.open(EMF_WORLD_SERVER, [](emf::World world, const emf::ui::Event& e) { ... });
// Later: w.clear(), add the new contents, w.update() (false when the player closed it).
class Window {
public:
    Window(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {}
    Window& heading(std::string text) { return add(EMF_UI_HEADING, {}, std::move(text), 0); }
    Window& text(std::string text, uint32_t rgba = 0) {
        add(EMF_UI_TEXT, {}, std::move(text), 0);
        items_.back().w.rgba = rgba;
        return *this;
    }
    Window& button(std::string key, std::string text, uint32_t flags = 0) {
        return add(EMF_UI_BUTTON, std::move(key), std::move(text), flags);
    }
    Window& row(std::string key, std::string text, uint32_t flags = 0) {
        return add(EMF_UI_ROW, std::move(key), std::move(text), flags);
    }
    Window& slot(std::string key, std::string text, uint32_t count = 0, uint32_t flags = 0) {
        add(EMF_UI_SLOT, std::move(key), std::move(text), flags);
        items_.back().w.count = count;
        return *this;
    }
    Window& bar(std::string text, float fraction) {
        add(EMF_UI_BAR, {}, std::move(text), 0);
        const float f = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        items_.back().w.value = static_cast<uint32_t>(f * 1000.0f + 0.5f);
        return *this;
    }
    Window& separator() { return add(EMF_UI_SEPARATOR, {}, {}, 0); }
    Window& same_line() {
        same_line_ = true;
        return *this;
    }
    Window& tooltip(std::string text) {  // for the widget added last
        if (!items_.empty()) items_.back().tooltip = std::move(text);
        return *this;
    }
    Window& place(uint32_t p) {
        place_ = p;
        return *this;
    }
    Window& width(float text_heights) {
        width_ = text_heights;
        return *this;
    }
    Window& close_key(uint32_t vk) {
        close_key_ = vk;
        return *this;
    }
    Window& clear() {  // the widgets only (id, title, place, width and close key stay)
        items_.clear();
        same_line_ = false;
        return *this;
    }
    const std::string& id() const { return id_; }

    // Opens it; f(World, const Event&) runs after the next step of a world of `kind` for each event, and is deleted
    // after CLOSED. False when refused (logged) or on an older core.
    template <class F>
    bool open(uint8_t kind, F f) const {
        if (!has_v24() || !api()->window_open) return false;
        std::vector<EmfUiWidget> widgets;
        EmfWindowDef d = def(widgets);
        d.world_kind = kind;
        auto* holder = new F(std::move(f));
        d.cb = [](EmfWorld* world, const EmfUiEvent* e, void* u) {
            F* fn = static_cast<F*>(u);
            Event ev;
            ev.kind = e->kind;
            ev.window = e->window ? e->window : "";
            ev.key = e->key ? e->key : "";
            ev.reason = e->reason;
            ev.flags = e->flags;
            try {
                (*fn)(World(world), ev);
            } catch (const std::exception& x) {
                EMF_LOG("emf::ui", "a window callback threw: %s", x.what());
            } catch (...) {
                EMF_LOG("emf::ui", "a window callback threw an unknown C++ exception");
            }
            if (e->kind == EMF_UI_CLOSED) delete fn;
        };
        d.user = holder;
        if (api()->window_open(&d)) return true;
        delete holder;
        return false;
    }
    // The current contents for the open window (its callback stays). False when it is not open.
    bool update() const {
        if (!has_v24() || !api()->window_update) return false;
        std::vector<EmfUiWidget> widgets;
        const EmfWindowDef d = def(widgets);
        return api()->window_update(&d);
    }

private:
    struct Item {
        EmfUiWidget w{};
        std::string key, text, tooltip;
    };
    Window& add(uint32_t type, std::string key, std::string text, uint32_t flags) {
        Item it;
        it.w.type = type;
        it.w.flags = flags | (same_line_ ? EMF_UI_SAME_LINE : 0u);
        same_line_ = false;
        it.key = std::move(key);
        it.text = std::move(text);
        items_.push_back(std::move(it));
        return *this;
    }
    EmfWindowDef def(std::vector<EmfUiWidget>& out) const {
        out.clear();
        for (const auto& it : items_) {
            EmfUiWidget w = it.w;
            w.key = it.key.empty() ? nullptr : it.key.c_str();
            w.text = it.text.c_str();
            w.tooltip = it.tooltip.empty() ? nullptr : it.tooltip.c_str();
            out.push_back(w);
        }
        EmfWindowDef d{};
        d.size = sizeof d;
        d.id = id_.c_str();
        d.title = title_.c_str();
        d.widgets = out.data();
        d.count = static_cast<uint32_t>(out.size());
        d.widget_size = sizeof(EmfUiWidget);
        d.close_key = close_key_;
        d.place = place_;
        d.width = width_;
        return d;
    }
    std::string id_, title_;
    std::vector<Item> items_;
    bool same_line_ = false;
    uint32_t place_ = EMF_UI_PLACE_CENTRE;
    float width_ = 0.0f;
    uint32_t close_key_ = 0;
};
// Closes the window `id` (its callback gets CLOSED, BY_MOD). False when it is not open or on an older core.
inline bool close(const char* id) { return has_v24() && api()->window_close && api()->window_close(id); }
}  // namespace ui

}  // namespace emf
