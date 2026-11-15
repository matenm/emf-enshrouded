// The C API table handed to mods. Every function is safe to call with a null/stale world:
// reads are fault-guarded and return 0/NULL.
#include "core/game_text.h"
#include "core/item_text.h"
#include "core/player_strings.h"
#include "api.h"
#include "api_sizes.h"
#include "common/pattern_find.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <set>
#include <vector>

#include "attributes.h"
#include "common/log_limiter.h"
#include "common/hooks.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "common/world_offsets.h"
#include "events.h"
#include "inventory.h"
#include "items.h"
#include "program_access.h"
#include "doors.h"
#include "enemy_orders.h"
#include "move_orders.h"
#include "path_glue.h"
#include "remove_building.h"
#include "spawn.h"
#include "teleport.h"
#include "component_service.h"
#include "offers.h"
#include "net/net_service.h"
#include "player_events.h"
#include "overlay/window_model.h"
#include "world_settings_service.h"
#include "emf/generated/components.hpp"

namespace emf::core {
// v31 hits (defined after the game reads they use, near the damage pre-hook)
bool api_on_hit(EmfHitCallback cb, void* user);
bool api_on_hit_before(EmfHitBeforeCallback cb, void* user);
uint32_t api_hit_rule_add(const EmfHitRule* rule);
bool api_hit_rule_remove(uint32_t id);

namespace {

CoreState* g_state = nullptr;

uint8_t* raw(EmfWorld* w) { return reinterpret_cast<uint8_t*>(w); }

void core_log(const std::string& line) {
    if (g_state && g_state->log_line) g_state->log_line(line);
}

// World-changing calls are only safe on the world's own thread, after its sim step: i.e. inside an
// on_tick/run_on_world callback, on the world passed to it.
bool world_call_allowed(EmfWorld* w, const char* what) {
    if (w && w == dispatching_world()) return true;
    core_log(std::string(what) + " refused: call it from an on_tick/run_on_world callback, on the world passed to it");
    return false;
}
uint64_t now() { return g_state && g_state->now_ms ? g_state->now_ms() : GetTickCount64(); }

// Logs a line once per hosted session (mod mistakes inside per-tick callbacks would flood the log otherwise); a new
// server world logs it again, as the same refusal in a later world is news.
void log_once(const std::string& line) {
    static std::mutex m;
    static std::set<std::string> seen;
    static uint32_t seen_session = 0;
    const uint32_t session = g_state ? g_state->server_session.load() : 0u;
    {
        std::lock_guard<std::mutex> lock(m);
        if (session != seen_session) {
            seen.clear();
            seen_session = session;
        }
        if (!seen.insert(line).second) return;
    }
    core_log(line);
}

void api_log(const char* mod, const char* fmt, ...) {
    if (!g_state || !g_state->log_line || !fmt) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    g_state->log_line(std::string("[") + (mod ? mod : "?") + "] " + msg);
}

EmfWorld* api_server_world() { return g_state ? g_state->tracker.latest(EMF_WORLD_SERVER, now()) : nullptr; }
EmfWorld* api_client_world() { return g_state ? g_state->tracker.latest(EMF_WORLD_CLIENT, now()) : nullptr; }
uint8_t api_world_kind(EmfWorld* w) { return w ? WorldView(raw(w)).kind() : 0xFF; }
// False (logged once) when the world's tick fields are not confirmed on this game build.
bool tick_fields_ok() {
    if (!g_state || g_state->tick_offsets_ok) return true;
    if (!g_state->tick_fields_warned.exchange(true))
        g_state->log_line("world_tick / ecs_time_ns give 0: the world's tick fields are not confirmed on this game build");
    return false;
}

uint32_t api_world_tick(EmfWorld* w) { return w && tick_fields_ok() ? WorldView(raw(w)).tick() : 0; }

int64_t api_ecs_time_ns(EmfWorld* w) {
    if (!w || !tick_fields_ok()) return 0;
    int64_t period = 0;
    uint32_t tick = 0;
    const WorldOffsets& wo = world_offsets();
    if (!seh_read(raw(w) + wo.tick_period_ns, &period, 8) || !seh_read(raw(w) + wo.tick, &tick, 4)) return 0;
    return period * static_cast<int64_t>(tick);
}

void* api_singleton(EmfWorld* w, const char* name) {
    return (w && name) ? WorldView(raw(w)).singleton(name) : nullptr;
}

uint32_t api_local_player() {
    EmfWorld* client = api_client_world();
    if (!client) return 0;
    const uint8_t* lpd = static_cast<const uint8_t*>(api_singleton(client, "LocalPlayerData"));
    uint32_t id = 0;
    if (!lpd || !seh_read(lpd + off::kLocalPlayerId, &id, 4)) return 0;
    return id;
}

void* api_get_template_component(EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size_out) {
    if (size_out) *size_out = 0;
    if (!g_state || !w) return nullptr;
    void* p = get_template_component(g_state->get_template_component, raw(w), hash, entity);
    if (p && size_out) *size_out = WorldView(raw(w)).template_component_size(hash).value_or(0);
    return p;
}

void* api_get_component(EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size_out) {
    if (size_out) *size_out = 0;
    if (!w || !g_state) return nullptr;
    ComponentRef c = get_component(g_state->get_component, raw(w), hash, entity);
    if (size_out) *size_out = c.ptr ? c.size : 0;
    return c.ptr;
}

void api_for_each_entity(EmfWorld* w, EmfEntityCallback cb, void* user) {
    if (!w || !cb) return;
    for (uint32_t id : WorldView(raw(w)).entity_ids()) cb(id, user);
}

void api_on_tick(EmfWorldCallback cb, void* user) {
    if (g_state) g_state->dispatcher.subscribe_tick(cb, user);
}

void api_run_on_world(uint8_t kind, EmfWorldCallback cb, void* user) {
    if (g_state) g_state->dispatcher.post(kind, cb, user);
}

const Resolved* usable_sig(const char* name) {
    if (!g_state || !name) return nullptr;
    const SigDef* def = find_signature(name);
    if (!def) return nullptr;
    const Resolved& r = g_state->sigs[static_cast<size_t>(def->id)];
    return is_usable(r, g_state->known_build) ? &r : nullptr;
}

void* api_resolve(const char* name) {
    const Resolved* r = usable_sig(name);
    return r ? reinterpret_cast<void*>(r->addr) : nullptr;
}

// The file name (no extension) of the module that holds `address`: the mod calling the API.
std::string module_name_of(const void* address) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &m))
        return "unknown";
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    std::string s(path);
    s = s.substr(s.find_last_of("\\/") + 1);
    const size_t dot = s.find_last_of('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

bool install_hook(const std::string& owner, const std::string& name, uintptr_t target, void* detour, void** original,
                  const char* api) {
    if (!g_state || !g_state->hooks) {
        core_log(std::string(api) + "('" + name + "') refused: hooks are not available yet");
        return false;
    }
    std::string why;
    const auto r = g_state->hooks->install(owner, name, target, detour, original, &why);
    if (r == HookTable::Result::Ok) {
        core_log("hook '" + name + "' by " + owner + " installed");
        return true;
    }
    core_log(std::string(api) + "('" + name + "') refused: " + why);
    return false;
}

bool api_hook(const char* name, void* detour, void** original) {
    const Resolved* r = usable_sig(name);
    if (!r || !detour) return false;
    return install_hook(module_name_of(_ReturnAddress()), name, r->addr, detour, original, "hook");
}

// ---- v14: patterns, hooks, build info ----
uintptr_t find_in_exe(const EmfPattern* p, const char* api) {
    if (!g_state) return 0;
    if (!p || p->size < kPatternV14Size || !p->bytes) {
        core_log(std::string(api) + " refused: no pattern, or its size field is too small");
        return 0;
    }
    const std::string section = p->section ? p->section : ".text";
    const emf::Section* sec = nullptr;
    for (const auto& s : g_state->exe.sections)
        if (s.name == section) sec = &s;
    if (!sec) {
        core_log(std::string(api) + "(\"" + p->bytes + "\") refused: the game exe has no section " + section);
        return 0;
    }
    if (p->follow > EMF_FOLLOW_REL32) {
        core_log(std::string(api) + "(\"" + p->bytes + "\") refused: unknown follow");
        return 0;
    }
    emf::PatternQuery q;
    q.bytes = p->bytes;
    q.offset = p->offset;
    q.follow_rel32 = p->follow == EMF_FOLLOW_REL32;
    q.rel32_tail = p->rel32_tail;
    const auto r = emf::find_unique({g_state->exe.scan_bytes(*sec), sec->size}, sec->start, g_state->exe.base,
                                    g_state->exe.size, q);
    if (!r.address) core_log(std::string(api) + "(\"" + p->bytes + "\") in " + section + ": " + r.error);
    return r.address;
}

uintptr_t api_find_pattern(const EmfPattern* p) { return find_in_exe(p, "find_pattern"); }

bool api_hook_pattern(const char* name, const EmfPattern* p, void* detour, void** original) {
    const std::string n = name ? name : "unnamed";
    const uintptr_t at = find_in_exe(p, ("hook_pattern('" + n + "')").c_str());
    if (!at) return false;
    return install_hook(module_name_of(_ReturnAddress()), n, at, detour, original, "hook_pattern");
}

bool api_hook_address(const char* name, void* address, void* detour, void** original) {
    return install_hook(module_name_of(_ReturnAddress()), name ? name : "unnamed", reinterpret_cast<uintptr_t>(address),
                        detour, original, "hook_address");
}

bool api_game_build(EmfGameBuild* out) {
    if (!g_state || !out || out->size < kGameBuildV14Size) return false;
    const auto& e = g_state->exe;
    out->number = g_state->known_build ? e.build_number : 0;
    const size_t n = (std::min)(e.sha256.size(), sizeof(out->sha256) - 1);
    std::memcpy(out->sha256, e.sha256.data(), n);
    out->sha256[n] = '\0';
    out->known = g_state->known_build ? 1 : 0;
    out->server = e.server ? 1 : 0;
    out->pad = 0;
    out->module_base = e.base;
    out->text_rva = 0;
    out->text_size = 0;
    for (const auto& s : e.sections)
        if (s.name == ".text") {
            out->text_rva = static_cast<uint32_t>(s.start - e.base);
            out->text_size = static_cast<uint32_t>(s.size);
        }
    return true;
}

bool api_command_sender(EmfCommandSender* out) {
    if (!out || out->size < kCommandSenderV15Size) return false;
    const CommandSender* s = current_sender();
    if (!s) return false;
    out->entity = s->entity;
    out->account = s->account;
    out->from_chat = s->from_chat ? 1 : 0;
    out->admin = s->restricted ? 0 : 1;
    std::memset(out->pad, 0, sizeof out->pad);
    return true;
}

bool api_hook_program(const char* name, EmfProgramCallback pre, EmfProgramCallback post, void* user) {
    if (!g_state || !g_state->programs || !name) return false;
    for (const auto& reserved : g_state->reserved_programs)
        if (reserved == name) {
            core_log(std::string("hook_program('") + name +
                     "') refused: EMF's core hooks this program itself (for damage, API v18); one hook per program");
            return false;
        }
    const auto r = g_state->programs->request_hook(name, pre, post, user);
    return r == ProgramRegistry::Result::Installed || r == ProgramRegistry::Result::Pending;
}

bool api_register_program(const char* name, uint8_t world_kind, EmfProgramCallback fn, void* user) {
    if (!g_state || !name) return false;
    if (g_state->system_programs.registration_closed()) {
        core_log(std::string("register_program('") + name +
                 "') refused: call it from emf_mod_load, before the first world is set up (or mod programs are unavailable)");
        return false;
    }
    if (g_state->system_programs.add(name, world_kind, fn, user)) return true;
    core_log(std::string("register_program('") + name +
             "') refused: empty/duplicate name, null callback, world kind not server/client, or more than 16 programs");
    return false;
}

// Mods built against v3..v7 pass the smaller definition; v8 fields are read only when the size covers them.
constexpr uint32_t kProgramDefV3Size = static_cast<uint32_t>(offsetof(EmfProgramDef, singletons));

bool api_register_program_def(const EmfProgramDef* def) {
    if (!g_state || !def || def->size < kProgramDefV3Size || !def->name) return false;
    const std::string name = def->name;
    if (g_state->system_programs.registration_closed()) {
        core_log("register_program_def('" + name +
                 "') refused: call it from emf_mod_load, before the first world is set up (or mod programs are unavailable)");
        return false;
    }
    ProgramSpec spec{name, def->world_kind, def->fn, def->user, {}, {}};
    size_t total = 0;
    for (auto p = def->events_in; p && *p && total <= kMaxProgramEvents; ++p, ++total) spec.events_in.emplace_back(*p);
    for (auto p = def->events_out; p && *p && total <= kMaxProgramEvents; ++p, ++total) spec.events_out.emplace_back(*p);
    if (def->size >= offsetof(EmfProgramDef, other_read) + sizeof(def->other_read)) {
        size_t n = 0;
        for (auto p = def->singletons; p && *p && n <= kMaxProgramAccessList; ++p, ++n) spec.singletons_read.emplace_back(*p);
        n = 0;
        for (auto p = def->other_read; p && *p && n <= kMaxProgramAccessList; ++p, ++n) spec.other_read.emplace_back(*p);
    }
    if (def->size >= offsetof(EmfProgramDef, other_write) + sizeof(def->other_write)) {  // v16
        size_t n = 0;
        for (auto p = def->other_write; p && *p && n <= kMaxProgramAccessList; ++p, ++n) spec.other_write.emplace_back(*p);
    }
    if (g_state->system_programs.add(spec)) return true;
    core_log("register_program_def('" + name +
             "') refused: empty/duplicate name, null callback, world kind not server/client, more than 16 programs, "
             "an event name that is not 1-64 letters/digits/_, a name listed twice, more than 24 events, a singleton "
             "name that is not 1-64 of [A-Za-z0-9_:], an unknown component in other_read/other_write, or more than 16 in a "
             "list");
    return false;
}

// The current program call if `ctx` is the program context of the callback running on this thread.
const ProgramCall* call_for(void* ctx, const char* fn, const char* name) {
    const ProgramCall* call = current_program_call();
    if (call && ctx && call->ctx == ctx) return call;
    log_once(std::string(fn) + "('" + name +
             "') refused: call it inside your program's callback, with the program_context passed to it");
    return nullptr;
}

std::optional<EventType> declared_event(const ProgramCall& call, const char* fn, const char* name, bool out,
                                        uint32_t expected_size, size_t* iter_slot) {
    const auto e = program_event(call, name, out, iter_slot);
    if (!e) {
        log_once(std::string(fn) + "('" + name + "') refused: '" + name + "' is not in the program's " +
                 (out ? "events_out" : "events_in"));
        return std::nullopt;
    }
    if (expected_size && expected_size != e->size) {
        log_once(std::string(fn) + "('" + name + "') refused: the game's '" + name + "' is " + std::to_string(e->size) +
                 " bytes, the mod expects " + std::to_string(expected_size) + " (game update? rebuild the mod)");
        return std::nullopt;
    }
    return e;
}

void* api_emit_event(void* ctx, const char* name, uint32_t expected_size) {
    if (!g_state || !name) return nullptr;
    const ProgramCall* call = call_for(ctx, "emit_event", name);
    if (!call) return nullptr;
    const auto e = declared_event(*call, "emit_event", name, true, expected_size, nullptr);
    if (!e) return nullptr;
    auto fn = reinterpret_cast<EmitEventFn>(api_resolve("EmitEvent"));
    if (!fn) {
        log_once("emit_event refused: the EmitEvent signature is not usable on this game build");
        return nullptr;
    }
    void* job = nullptr;
    void* ev = seh_read(ctx, &job, 8) ? emit_from_job(fn, job, e->hash, e->size, e->has_timestamp) : nullptr;
    if (!ev) log_once(std::string("emit_event('") + name + "'): the game did not take the event (event buffer full?)");
    return ev;
}

uint32_t api_read_events(void* ctx, const char* name, uint32_t expected_size, EmfEventCallback cb, void* user) {
    if (!g_state || !name || !cb) return 0;
    const ProgramCall* call = call_for(ctx, "read_events", name);
    if (!call) return 0;
    size_t slot = 0;
    const auto e = declared_event(*call, "read_events", name, false, expected_size, &slot);
    if (!e) return 0;
    if (e->size % 8 != 0) {
        log_once(std::string("read_events('") + name + "') refused: the queue layout of " + std::to_string(e->size) +
                 "-byte events is not known yet");
        return 0;
    }
    if (!call->iter || slot >= kProgramIterQwords) {
        log_once("read_events refused: the IterBegin signature is not usable on this game build");
        return 0;
    }
    return walk_event_queue(reinterpret_cast<const void*>(call->iter[slot]), e->size, cb, user);
}

void* api_program_singleton(void* ctx, const char* name) {
    if (!g_state || !name) return nullptr;
    const ProgramCall* call = call_for(ctx, "program_singleton", name);
    if (!call) return nullptr;
    const auto slot = program_singleton_slot(*call, name);
    if (!slot) {
        log_once(std::string("program_singleton('") + name + "') refused: '" + name +
                 "' is not in the program's singletons");
        return nullptr;
    }
    if (!call->iter || *slot >= kProgramIterQwords) {
        log_once("program_singleton refused: the IterBegin signature is not usable on this game build");
        return nullptr;
    }
    void* p = reinterpret_cast<void*>(call->iter[*slot]);
    void* expected = call->world ? WorldView(call->world).singleton(name) : nullptr;
    if (expected && p != expected) {
        log_once(std::string("program_singleton('") + name +
                 "') refused: the engine's pointer does not match the world's singleton (iterator layout?)");
        return nullptr;
    }
    return p;
}

void* api_program_read_other(void* ctx, const char* component, uint32_t entity, uint32_t expected_size) {
    if (!g_state || !component) return nullptr;
    const ProgramCall* call = call_for(ctx, "program_read_other", component);
    if (!call) return nullptr;
    const auto access = program_other(*call, component);
    if (!access) {
        log_once(std::string("program_read_other('") + component + "') refused: '" + component +
                 "' is not in the program's other_read (or other_write)");
        return nullptr;
    }
    auto read = reinterpret_cast<ReadOtherFn>(api_resolve("ReadOther"));
    if (!read || !call->iter || access->iter_slot >= kProgramIterQwords) {
        log_once("program_read_other refused: the ReadOther or IterBegin signature is not usable on this game build");
        return nullptr;
    }
    std::string problem;
    const OtherRead r = read_other(*call, *access, component, entity, read, g_state->get_component, &problem);
    if (!problem.empty()) log_once(std::string("program_read_other('") + component + "'): " + problem);
    if (!r.ptr) return nullptr;
    if (expected_size && r.size && expected_size != r.size) {
        log_once(std::string("program_read_other('") + component + "') refused: the game's '" + component + "' is " +
                 std::to_string(r.size) + " bytes, the mod expects " + std::to_string(expected_size) +
                 " (game update? rebuild the mod)");
        return nullptr;
    }
    return r.ptr;
}

void* api_program_write_other(void* ctx, const char* component, uint32_t entity, uint32_t expected_size) {
    if (!g_state || !component) return nullptr;
    const ProgramCall* call = call_for(ctx, "program_write_other", component);
    if (!call) return nullptr;
    const auto access = program_other(*call, component, true);
    if (!access || !access->write) {
        log_once(std::string("program_write_other('") + component + "') refused: '" + component +
                 "' is not in the program's other_write");
        return nullptr;
    }
    auto write = reinterpret_cast<WriteOtherFn>(api_resolve("WriteOther"));
    auto read = reinterpret_cast<ReadOtherFn>(api_resolve("ReadOther"));
    if (!write || !read || !call->iter || access->iter_slot >= kProgramIterQwords) {
        log_once("program_write_other refused: the WriteOther, ReadOther or IterBegin signature is not usable on this "
                 "game build");
        return nullptr;
    }
    std::string problem;
    const OtherRead r = write_other(*call, *access, component, entity, read, write, g_state->get_component, &problem);
    if (!problem.empty()) log_once(std::string("program_write_other('") + component + "'): " + problem);
    if (!r.ptr) return nullptr;
    if (expected_size && r.size && expected_size != r.size) {
        log_once(std::string("program_write_other('") + component + "') refused: the game's '" + component + "' is " +
                 std::to_string(r.size) + " bytes, the mod expects " + std::to_string(expected_size) +
                 " (game update? rebuild the mod)");
        return nullptr;
    }
    return r.ptr;
}

const uint8_t* item_registry(EmfWorld* w);

bool api_put_items(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t count, EmfPutItemsCallback cb, void* user) {
    if (!g_state) return false;
    auto refuse = [](const std::string& why) {
        std::string line = "put_items refused: " + why;
        if (g_state) {
            std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
            line = g_state->put_refusals.pass(line, g_state->now_ms ? g_state->now_ms() : GetTickCount64());
        }
        if (!line.empty()) core_log(line);
        return false;
    };
    if (!g_state->inventory)
        return refuse("the inventory transaction is not available on this game build (its signatures are not usable)");
    if (!w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return refuse("not a server world (host only)");
    if (!item_by_id(item_registry(w), item)) return refuse("unknown item " + std::to_string(item));
    std::string why;
    if (!g_state->inventory->request({w, entity, item, count, cb, user}, &why)) return refuse(why);
    return true;
}

// v11: put_items_ex / take_items share the validation of put_items.
bool api_items_request(EmfWorld* w, const EmfItemsRequest* req, bool take) {
    if (!g_state) return false;
    const char* fn = take ? "take_items" : "put_items_ex";
    auto refuse = [fn](const std::string& why) {
        std::string line = std::string(fn) + " refused: " + why;
        {
            std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
            line = g_state->put_refusals.pass(line, g_state->now_ms ? g_state->now_ms() : GetTickCount64());
        }
        if (!line.empty()) core_log(line);
        return false;
    };
    if (!req || req->size < kItemsRequestV11Size) return refuse("no request, or its size field is too small");
    if (!g_state->inventory)
        return refuse("the inventory transaction is not available on this game build (its signatures are not usable)");
    if (!w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return refuse("not a server world (host only)");
    if (!item_by_id(item_registry(w), req->item)) return refuse("unknown item " + std::to_string(req->item));
    if (req->flags & ~(EMF_ITEMS_EXACT_SLOT | EMF_ITEMS_RARITY)) return refuse("unknown flags (a newer mod?)");
    if ((req->flags & EMF_ITEMS_RARITY) && req->rarity > 4) return refuse("rarity must be 0-4");
    PutRequest r{w, req->entity, req->item, req->count, req->cb, req->user};
    r.take = take;
    r.slot = req->slot;
    r.level = req->level;
    r.rarity = (req->flags & EMF_ITEMS_RARITY) ? req->rarity : uint8_t{0xFF};
    r.exact_slot = take && (req->flags & EMF_ITEMS_EXACT_SLOT);
    std::string why;
    if (!g_state->inventory->request(r, &why)) return refuse(why);
    return true;
}
bool api_put_items_ex(EmfWorld* w, const EmfItemsRequest* req) { return api_items_request(w, req, false); }
bool api_take_items(EmfWorld* w, const EmfItemsRequest* req) { return api_items_request(w, req, true); }

// v12: a line of text kept on screen.
bool api_hud_set(const char* id, const char* text, uint32_t place, uint32_t rgba) {
    if (!g_state) return false;
    const auto r = g_state->hud.set(id ? id : "", text ? text : "", place, rgba);
    if (r == HudModel::Result::Ok || r == HudModel::Result::Removed) return true;
    static const char* const kWhy[] = {"", "", "the id must be 1-48 of A-Z a-z 0-9 _ . -", "place must be 0-2",
                                       "32 lines are already shown"};
    std::string line = std::string("hud_set refused: ") + kWhy[static_cast<int>(r)] + " (" + (id ? id : "null") + ")";
    {
        std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
        line = g_state->put_refusals.pass(line, g_state->now_ms ? g_state->now_ms() : GetTickCount64());
    }
    if (!line.empty()) core_log(line);
    return false;
}

uint32_t api_server_session() { return g_state ? g_state->server_session.load() : 0u; }

bool api_wheel_open(const EmfWheel* w) {
    if (!g_state) return false;
    auto refuse = [](const std::string& why) {
        core_log("wheel_open refused: " + why);
        return false;
    };
    if (!w || w->size < kWheelV10Size) return refuse("no wheel definition, or its size field is too small");
    if (!w->items || w->count == 0 || w->count > kMaxWheelItems) return refuse("it needs 1 to 64 entries");
    if (!w->cb) return refuse("no callback");
    if (w->world_kind != EMF_WORLD_SERVER && w->world_kind != EMF_WORLD_CLIENT)
        return refuse("world_kind must be EMF_WORLD_SERVER or EMF_WORLD_CLIENT");
    const uint32_t hk = w->hold_key;
    if (hk > 0xFF || (hk >= VK_LBUTTON && hk <= VK_XBUTTON2) || (hk >= VK_LSHIFT && hk <= VK_RMENU))
        return refuse("hold_key must be 0 or a keyboard key's generic virtual-key code (VK_SHIFT, not VK_LSHIFT; no "
                      "mouse buttons)");
    WheelRequest r;
    if (w->title) r.title = w->title;
    for (uint32_t i = 0; i < w->count; ++i) {
        if (!w->items[i]) return refuse("entry " + std::to_string(i) + " is NULL");
        r.items.emplace_back(w->items[i]);
    }
    r.hold_vk = static_cast<uint16_t>(w->hold_key & 0xFFu);
    r.world_kind = w->world_kind;
    r.cb = w->cb;
    r.user = w->user;
    if (!g_state->open_wheel) return refuse("the in-game overlay is off");
    return g_state->open_wheel(r);
}

void api_notify(const char* fmt, ...) {
    if (!g_state || !fmt) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_state->notify)
        g_state->notify(buf);
    else
        core_log(buf);
}

// v28 on_player. The first call also asks the event relay for EntityDiedEvent and HitEvent on the server world
// ("died" and its killer) while it is still open (before the first world starts); later, joined and left only.
void player_died_event(EmfWorld*, const void* event, uint32_t size, void*) {
    if (!g_state || !g_state->players || size < sizeof(gen::ecs::EntityDiedEvent)) return;
    gen::ecs::EntityDiedEvent e{};
    std::memcpy(&e, event, sizeof e);
    g_state->players->died(e.deadEntityId.id);
}
void player_hit_event(EmfWorld*, const void* event, uint32_t size, void*) {
    if (!g_state || !g_state->players || size < sizeof(gen::ecs::HitEvent)) return;
    gen::ecs::HitEvent e{};
    std::memcpy(&e, event, sizeof e);
    g_state->players->hit(e.targetId.id, e.rootSourceId.id);
}

bool api_on_player(EmfPlayerCallback cb, void* user) {
    if (!g_state || !g_state->players || !cb) return false;
    static std::atomic<bool> asked{false};
    if (!asked.exchange(true)) {
        std::string why;
        const bool died = g_state->relay.subscribe(EMF_WORLD_SERVER, "EntityDiedEvent", sizeof(gen::ecs::EntityDiedEvent),
                                                   player_died_event, nullptr, &why) &&
                          g_state->relay.subscribe(EMF_WORLD_SERVER, "HitEvent", sizeof(gen::ecs::HitEvent),
                                                   player_hit_event, nullptr, &why);
        if (!died)
            core_log("on_player (" + module_name_of(reinterpret_cast<const void*>(cb)) +
                     "): no \"died\" events (" + why + "); joined and left come");
    }
    return g_state->players->watch(cb, user);
}

bool guarded_player_call(EmfPlayerCallback cb, EmfWorld* w, const EmfPlayerEvent* e, void* user) {
    __try {
        cb(w, e, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool api_on_event(uint8_t kind, const char* name, uint32_t expected_size, EmfWorldEventCallback cb, void* user) {
    if (!g_state || !name) return false;
    std::string why;
    if (g_state->relay.subscribe(kind, name, expected_size, cb, user, &why)) return true;
    core_log(std::string("on_event('") + name + "') refused: " + why);
    return false;
}

const uint8_t* item_registry(EmfWorld* w) {
    return w ? static_cast<const uint8_t*>(WorldView(raw(w)).singleton("g38_items::ItemRegistry")) : nullptr;
}

void fill_item_info(const ItemRecord& r, EmfItemInfo* out) {
    out->item_id = r.id;
    std::memcpy(out->guid, r.guid, 16);
    out->max_stack = r.max_stack;
    const size_t n = r.name.size() < sizeof(out->name) - 1 ? r.name.size() : sizeof(out->name) - 1;
    std::memcpy(out->name, r.name.data(), n);
    out->name[n] = '\0';
}

void api_for_each_item(EmfWorld* w, EmfItemCallback cb, void* user) {
    if (!cb) return;
    for (const auto& r : list_items(item_registry(w))) {
        EmfItemInfo info{};
        info.size = sizeof(info);
        fill_item_info(r, &info);
        cb(&info, user);
    }
}

uint32_t api_find_item(EmfWorld* w, const char* name) {
    if (!name) return 0;
    const auto r = item_by_name(item_registry(w), name);
    return r ? r->id : 0;
}

// v29: the game's item names and language (game_text.h, item_text.h)
uint32_t copy_out(const std::string& s, char* buf, uint32_t size) {
    if (buf && size) {
        size_t n = s.size() < size - 1 ? s.size() : size - 1;
        while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // whole characters
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(s.size());
}

uint32_t api_item_name(EmfWorld* w, uint32_t item, uint32_t flags, char* buf, uint32_t size) {
    if (buf && size) buf[0] = 0;
    const uint8_t* reg = item_registry(w);
    if (!reg) return 0;
    const auto name = item_names().name(reg, item, (flags & EMF_ITEM_NAME_GAME_TEXT) ? NameFor::Game : NameFor::Screen);
    return name ? copy_out(*name, buf, size) : 0;
}

// v30: the character name from the server's table of custom player strings (core/player_strings.h).
bool api_player_character_name(EmfWorld* w, uint32_t player, char* buf, uint32_t size) {
    if (buf && size) buf[0] = '\0';
    if (!g_state || !w || !buf || size < 2 || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return false;
    const ComponentRef c = get_component(g_state->get_component, raw(w), kCustomPlayerStringHash, player);
    uint64_t id = 0;
    if (!c.ptr || c.size < sizeof id || !seh_read(c.ptr, &id, sizeof id) || id == 0) return false;
    const auto* sys = static_cast<const uint8_t*>(WorldView(raw(w)).singleton(kCustomStringsSingleton));
    const auto name = custom_player_string(sys, id);
    if (!name) return false;
    const size_t n = (std::min)(name->size(), static_cast<size_t>(size - 1));
    std::memcpy(buf, name->data(), n);
    buf[n] = '\0';
    return true;
}

uint32_t api_game_language(char* code_out, uint32_t size) {
    if (code_out && size) code_out[0] = 0;
    const uint32_t lang = game_text().language();
    if (lang) copy_out(language_code(lang), code_out, size);
    return lang;
}

bool api_item_info(EmfWorld* w, uint32_t id, EmfItemInfo* out) {
    if (!out || out->size < kItemInfoV5Size) return false;
    const auto r = item_by_id(item_registry(w), id);
    if (!r) return false;
    fill_item_info(*r, out);
    return true;
}

void* api_item_data(EmfWorld* w, uint32_t id, uint64_t* size_out) {
    if (size_out) *size_out = 0;
    if (!g_state || !w) return nullptr;
    uint8_t* info = item_data_ptr(item_registry(w), id);
    if (info && size_out) *size_out = off::kItemInfoSize;
    return info;
}

bool api_set_item_max_stack(uint32_t id, uint16_t max) {
    if (!g_state || !max) return false;
    std::vector<uint8_t*> done;
    // A world running our callback on this thread is live even if a long pass kept it from stamping a tick.
    EmfWorld* server = api_server_world();
    EmfWorld* client = api_client_world();
    if (EmfWorld* d = dispatching_world()) {
        const uint8_t kind = WorldView(raw(d)).kind();
        if (kind == EMF_WORLD_SERVER) server = d;
        if (kind == EMF_WORLD_CLIENT) client = d;
    }
    for (EmfWorld* w : {server, client}) {
        uint8_t* info = w ? item_data_ptr(item_registry(w), id) : nullptr;
        if (!info || std::find(done.begin(), done.end(), info) != done.end()) continue;
        if (write_game_memory(info + off::kItemInfoMaxStack, &max, sizeof(max))) done.push_back(info);
    }
    return !done.empty();
}

bool api_write_memory(void* dst, const void* src, uint64_t n) {
    if (!dst || !src || !n || n > (uint64_t(1) << 30)) return false;
    return write_game_memory(dst, src, static_cast<size_t>(n));
}

uint32_t api_give_item(EmfWorld* w, uint32_t player, uint32_t item_id, uint32_t count) {
    if (!world_call_allowed(w, "give_item")) return 0;
    auto fn = reinterpret_cast<SpawnWrapperFn>(api_resolve("SpawnWrapper"));
    if (!fn) {
        core_log("give_item refused: the SpawnWrapper signature is not usable on this game build");
        return 0;
    }
    const auto item = item_by_id(item_registry(w), item_id);
    if (!item) {
        core_log("give_item: unknown item " + std::to_string(item_id) + " (no such id in the item registry)");
        return 0;
    }
    const ComponentRef t = g_state ? get_component(g_state->get_component, raw(w), off::kHashCurrentTransform, player)
                                   : ComponentRef{};
    int64_t pos[3] = {};
    if (!t.ptr || t.size < 24 || !seh_read(t.ptr, pos, sizeof(pos))) {
        core_log("give_item: player " + std::to_string(player) + " has no transform on this world");
        return 0;
    }
    const double k = 4294967296.0;
    std::string why;
    const uint32_t id = give_pickup(fn, raw(w), *item, count, pos[0] / k, pos[1] / k + 1.0, pos[2] / k, &why);
    if (!id) core_log("give_item refused: " + why);
    return id;
}

uint32_t api_spawn_ex(EmfWorld* w, const uint8_t guid[16], double x, double y, double z, const EmfComponentInit* inits,
                      uint32_t count) {
    if (!world_call_allowed(w, "spawn_ex")) return 0;
    auto fn = reinterpret_cast<SpawnWrapperFn>(api_resolve("SpawnWrapper"));
    if (!fn) {
        core_log("spawn_ex refused: the SpawnWrapper signature is not usable on this game build");
        return 0;
    }
    if (count > kMaxExtraSpawnComponents || (count && !inits)) {
        core_log("spawn_ex refused: at most 15 extra components");
        return 0;
    }
    std::vector<SpawnComponent> comps;
    for (uint32_t i = 0; i < count; ++i) comps.push_back({inits[i].hash, 0, inits[i].data, inits[i].size});
    std::string why;
    const uint32_t id = spawn_entity_ex(fn, raw(w), guid, x, y, z, comps.data(), comps.size(), &why);
    if (!id) core_log("spawn_ex refused: " + why);
    return id;
}

bool api_register_command(const char* name, const char* help, EmfCommandCallback cb, void* user) {
    if (!g_state || !name) return false;
    std::string why;
    if (g_state->commands.add(name, help ? help : "", cb, user, &why)) return true;
    core_log(std::string("register_command('") + name + "') refused: " + why);
    return false;
}

// v17: a command with options (EMF_COMMAND_PLAYERS: any player may run it from chat).
bool api_register_command_ex(const EmfCommandDef* def) {
    if (!g_state) return false;
    if (!def || def->size < kCommandDefV17Size || !def->name) {
        core_log("register_command_ex refused: no definition or name, or its size field is too small");
        return false;
    }
    if (def->flags & ~static_cast<uint32_t>(EMF_COMMAND_PLAYERS)) {
        core_log(std::string("register_command_ex('") + def->name + "') refused: unknown flags (a newer mod?)");
        return false;
    }
    static_assert(EMF_COMMAND_PLAYERS == kCommandForPlayers, "the core's command flags follow EMF_COMMAND_*");
    std::string why;
    if (g_state->commands.add(def->name, def->help ? def->help : "", def->cb, def->user, &why, false, def->flags, true))
        return true;
    core_log(std::string("register_command_ex('") + def->name + "') refused: " + why);
    return false;
}

void api_command_reply(const char* fmt, ...) {
    if (!fmt) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    command_reply_line(msg);
}

uint32_t api_spawn(EmfWorld* w, const uint8_t guid[16], double x, double y, double z) {
    if (!world_call_allowed(w, "spawn")) return 0;
    auto fn = reinterpret_cast<SpawnWrapperFn>(api_resolve("SpawnWrapper"));
    if (!fn) {
        core_log("spawn refused: the SpawnWrapper signature is not usable on this game build");
        return 0;
    }
    const uint32_t id = spawn_entity(fn, raw(w), guid, x, y, z);
    if (!id) core_log("spawn: no entity created (not a server world, unknown template GUID, or the call faulted)");
    return id;
}

bool api_destroy(EmfWorld* w, uint32_t entity) {
    if (!world_call_allowed(w, "destroy")) return false;
    auto fn = reinterpret_cast<DestroyEntityFn>(api_resolve("DestroyEntity"));
    if (!fn) {
        core_log("destroy refused: the DestroyEntity signature is not usable on this game build");
        return false;
    }
    constexpr uint32_t kPlayerStateHash = 0x6ff967bbu;  // PlayerState (emf/generated/components.hpp; the test uses that)
    if (api_get_component(w, kPlayerStateHash, entity, nullptr)) {  // never a player (reference.md)
        core_log("destroy refused: entity " + std::to_string(entity) + " is a player");
        return false;
    }
    const bool ok = destroy_entity(fn, raw(w), entity);
    if (!ok) core_log("destroy: refused (not a server world, entity 0, or the call faulted)");
    if (ok && g_state->offers) g_state->offers->forget(entity);  // its id may soon be another entity's (E prompts)
    return ok;
}

// v36: a placed object removed like a pick-up (remove_building.h decides; destroy and the refund here).
namespace {
struct RefundLater {
    uint32_t session;
    double pos[3];
};
// Drops one `item` at pos (0.5 m up) as a loose pickup; false (logged) when it could not be spawned.
bool drop_refund(EmfWorld* w, uint32_t item, const double pos[3]) {
    auto fn = reinterpret_cast<SpawnWrapperFn>(api_resolve("SpawnWrapper"));
    const auto rec = fn ? item_by_id(item_registry(w), item) : std::nullopt;
    std::string why = fn ? "unknown item" : "the SpawnWrapper signature is not usable on this game build";
    if (rec && give_pickup(fn, raw(w), *rec, 1, pos[0], pos[1] + 0.5, pos[2], &why)) return true;
    core_log("remove_building: the item could not be dropped (" + why + ")");
    return false;
}
// force: what it held drops at its spot, a pickup per stack side by side (item, count, level, rarity; the owner saw a
// forced chest's items lost). False when a stack could not be dropped (logged).
bool drop_contents(EmfWorld* w, const RemoveFacts& f) {
    auto fn = reinterpret_cast<SpawnWrapperFn>(api_resolve("SpawnWrapper"));
    const uint8_t* reg = item_registry(w);
    size_t dropped = 0;
    for (size_t i = 0; i < f.contents.size(); ++i) {
        const ContentStack& c = f.contents[i];
        const auto rec = fn ? item_by_id(reg, c.item) : std::nullopt;
        const double a = 2.39996 * static_cast<double>(i);  // the golden angle: stacks spread round the spot
        const double r = 0.3 + 0.15 * std::sqrt(static_cast<double>(i));
        std::string why = fn ? "unknown item" : "the SpawnWrapper signature is not usable on this game build";
        bool ok = rec.has_value();
        for (uint32_t left = c.count; ok && left > 0;) {  // a pickup holds at most 65535 (a stack-size mod's stacks)
            const uint32_t n = (std::min)(left, 65535u);
            ok = give_pickup(fn, raw(w), *rec, n, f.pos[0] + r * std::cos(a), f.pos[1] + 0.5, f.pos[2] + r * std::sin(a), &why,
                             c.level, c.rarity) != 0;
            left -= ok ? n : 0;
        }
        if (ok)
            ++dropped;
        else
            core_log("remove_building: a stack of item " + std::to_string(c.item) + " could not be dropped (" + why + ")");
    }
    core_log("remove_building: dropped " + std::to_string(dropped) + " of " + std::to_string(f.contents.size()) +
             " stack(s) it held");
    return dropped == f.contents.size();
}
void refund_put_done(EmfWorld* w, uint32_t, uint32_t item, uint32_t, uint32_t added, void* user) {
    std::unique_ptr<RefundLater> r(static_cast<RefundLater*>(user));
    if (added) return;
    if (!w || r->session != api_server_session()) {  // another session: that spot means nothing now
        core_log("remove_building: the refund did not fit and the session ended; nothing dropped");
        return;
    }
    drop_refund(w, item, r->pos);  // the inventory was full: at the object's spot instead
}
}  // namespace

bool api_remove_building(EmfWorld* w, EmfRemoveBuilding* req) {
    if (!g_state || !req || req->size < kRemoveBuildingV36Size) {
        core_log("remove_building refused: no request, or its size field is not sizeof(EmfRemoveBuilding)");
        return false;
    }
    req->refunded = 0;
    req->why[0] = 0;
    auto refuse = [req](const std::string& why) {
        std::snprintf(req->why, sizeof(req->why), "%s", why.c_str());
        std::string line = "remove_building refused (entity " + std::to_string(req->entity) + "): " + why;
        if (g_state) {  // a mod retrying every step: the same line once per 10 s (the caller has req->why)
            std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
            line = g_state->put_refusals.pass(line, now());
        }
        if (!line.empty()) core_log(line);
        return false;
    };
    if (!w || w != dispatching_world())
        return refuse("call it from a world callback (on_tick, run_on_world, on_event) on the server world passed to it");
    if (WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return refuse("not a server world (host only)");
    auto fn = reinterpret_cast<DestroyEntityFn>(api_resolve("DestroyEntity"));
    if (!fn) return refuse("the DestroyEntity signature is not usable on this game build");
    constexpr uint32_t kPlayerStateHash = 0x6ff967bbu;  // PlayerState
    const bool player_ok = req->player && api_get_component(w, kPlayerStateHash, req->player, nullptr);
    const ComponentGetter get = [w](uint32_t hash, uint32_t entity, uint64_t* size) -> const void* {
        return api_get_component(w, hash, entity, size);
    };
    const uint8_t* reg = item_registry(w);
    const ItemInfoGetter info = [reg](uint32_t item) -> const uint8_t* { return item_data_ptr(reg, item); };
    const RemoveFacts facts = remove_facts(req->entity, get, info);
    const RemovePlan plan = plan_remove(facts, req->refund, player_ok, req->flags);
    if (!plan.ok) return refuse(plan.why);
    // The same object twice in one step pays once (the game's destroy may leave its components until the step ends).
    auto& removed = g_state->removed_this_step;
    const uint32_t tick = api_world_tick(w);
    const uint64_t step = tick ? tick : (now() / 100) | (uint64_t(1) << 63);  // no tick on this build: 0.1 s
    if (removed.session != api_server_session() || removed.step != step) {
        removed.session = api_server_session();
        removed.step = step;
        removed.ids.clear();
    }
    if (std::find(removed.ids.begin(), removed.ids.end(), req->entity) != removed.ids.end())
        return refuse("it was removed already");
    if (plan.refund_item && (!api_resolve("SpawnWrapper") || !item_by_id(reg, plan.refund_item)))
        return refuse("its item cannot be given back on this game build");  // never removed without its item
    if (req->flags & EMF_REMOVE_FORCE)  // never removed with what it holds lost
        for (const ContentStack& c : facts.contents)
            if (!api_resolve("SpawnWrapper") || !item_by_id(reg, c.item))
                return refuse("what it holds cannot be dropped on this game build (item " + std::to_string(c.item) + ")");
    if (!destroy_entity(fn, raw(w), req->entity)) return refuse("the game's destroy call failed (see the log)");
    removed.ids.push_back(req->entity);
    if (g_state && g_state->offers) g_state->offers->forget(req->entity);
    if ((req->flags & EMF_REMOVE_FORCE) && !facts.contents.empty()) drop_contents(w, facts);
    if (!plan.refund_item) return true;
    req->refunded = plan.refund_item;
    if (req->refund == EMF_REFUND_PLAYER) {
        auto later = std::make_unique<RefundLater>(RefundLater{api_server_session(), {facts.pos[0], facts.pos[1], facts.pos[2]}});
        if (api_put_items(w, req->player, plan.refund_item, 1, refund_put_done, later.get())) {
            later.release();  // refund_put_done frees it
            return true;
        }
    }
    if (!drop_refund(w, plan.refund_item, facts.pos)) req->refunded = 0;
    return true;
}

// v37: paths on the game's path grid (path_service.h / path_glue.h).
uint32_t api_path_find(EmfWorld* w, const EmfPathQuery* q, EmfPathCallback cb, void* user) {
    auto refuse = [](const std::string& why) {
        std::string line = "path_find refused: " + why;
        if (g_state) {  // a mod retrying every step: the same line once per 10 s
            std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
            line = g_state->put_refusals.pass(line, now());
        }
        if (!line.empty()) core_log(line);
        return 0u;
    };
    PathService* paths = path_service();
    if (!paths) return refuse("not available on this game build (its signatures are not usable, or a game call faulted)");
    if (!q || q->size < kPathQueryV37Size) return refuse("no query, or its size field is not sizeof(EmfPathQuery)");
    if (!w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER)
        return refuse("not a server world (a joined game has no path grid: ask on the host or the dedicated server)");
    EmfPathQuery copy{};  // what v37 knows of a later version's larger struct
    std::memcpy(&copy, q, (std::min)(static_cast<size_t>(q->size), sizeof copy));
    copy.size = sizeof copy;
    std::string why = path_query_check(copy);  // the cheap refusals before the owner lookup
    if (why.empty() && paths->open() >= kMaxOpenPaths) why = "32 path queries are open already (wait for answers)";
    if (!why.empty()) return refuse(why);
    uint32_t owner = copy.owner;
    if (!owner) owner = api_local_player();  // the host's own player: no scan
    if (!owner)
        for (const auto& p : read_players(api_table(), w))
            if (p.entity) {
                owner = p.entity;
                break;
            }
    const uint32_t id = paths->request(copy, owner, cb, user, now(), &why);
    return id ? id : refuse(why);
}

bool api_path_cancel(uint32_t id) {
    PathService* paths = path_service();
    return paths && paths->cancel(id);
}

// v38: doors (doors.h).
bool door_refused(const std::string& what, const std::string& why) {
    std::string line = what + " refused: " + why;
    if (g_state) {  // a mod retrying every step: the same line once per 10 s
        std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
        line = g_state->put_refusals.pass(line, now());
    }
    if (!line.empty()) core_log(line);
    return false;
}

uint32_t api_door_state(EmfWorld* w, uint32_t entity) {
    DoorService* d = g_state ? g_state->doors : nullptr;
    return d && w ? d->state(raw(w), entity) : EMF_DOOR_NONE;
}

bool api_door_set(EmfWorld* w, uint32_t entity, uint32_t open, uint32_t side) {
    DoorService* d = g_state ? g_state->doors : nullptr;
    if (!d) return door_refused("door_set", "doors are off on this game build (see the log)");
    if (side > 2) return door_refused("door_set", "side is 0, 1 (front) or 2 (back)");
    std::string why;
    if (d->request(raw(w), entity, open ? DoorOrderKind::Open : DoorOrderKind::Close, static_cast<uint8_t>(side), &why))
        return true;
    return door_refused("door_set (entity " + std::to_string(entity) + ")", why);
}

// v39 enemy orders (enemy_orders.h).
uint32_t api_enemy_order(EmfWorld* w, EmfEnemyOrder* req) {
    if (!g_state || !req || req->size < kEnemyOrderV39Size) {
        core_log("enemy_order refused: no order, or its size field is not sizeof(EmfEnemyOrder)");
        return 0;
    }
    req->why[0] = 0;
    auto refuse = [req](const std::string& why) {
        std::snprintf(req->why, sizeof(req->why), "%s", why.c_str());
        std::string line = "enemy_order refused (enemy " + std::to_string(req->enemy) + "): " + why;
        if (g_state) {  // a mod retrying every step: the same line once per 10 s
            std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
            line = g_state->put_refusals.pass(line, now());
        }
        if (!line.empty()) core_log(line);
        return 0u;
    };
    if (!w || w != dispatching_world())
        return refuse("call it from a world callback (on_tick, run_on_world, on_event) on the server world passed to it");
    if (WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return refuse("not a server world (host only)");
    EnemyOrders* o = g_state->orders;
    if (!o) return refuse("enemy orders are off");
    std::string why;
    const uint32_t id = o->add(w, req->enemy, req->target, req->seconds, req->damage, req->flags, &why);
    if (!id) return refuse(why);
    if (g_state->moves) g_state->moves->cancel_enemy(w, req->enemy);  // a move on it ends (v40), once the order holds
    return id;
}

bool api_enemy_order_cancel(uint32_t id) {
    EnemyOrders* o = g_state ? g_state->orders : nullptr;
    if (!o) return false;
    EmfWorld* w = dispatching_world();
    const bool now = w && WorldView(raw(w)).kind() == EMF_WORLD_SERVER;  // else at the next step
    return o->cancel(id, now);
}

uint32_t api_enemy_target(EmfWorld* w, uint32_t enemy) {
    EnemyOrders* o = g_state ? g_state->orders : nullptr;
    return o && w ? o->target_of(w, enemy) : 0;
}

// v40 move orders (move_orders.h).
bool guarded_move_call(EmfMoveCallback cb, EmfWorld* w, uint32_t id, uint32_t status, void* user) {
    __try {
        cb(w, id, status, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t api_enemy_move(EmfWorld* w, EmfMoveOrder* req, EmfMoveCallback cb, void* user) {
    if (!g_state || !req || req->size < kMoveOrderV40Size) {
        core_log("enemy_move refused: no order, or its size field is not sizeof(EmfMoveOrder)");
        return 0;
    }
    req->why[0] = 0;
    auto refuse = [req](const std::string& why) {
        std::snprintf(req->why, sizeof(req->why), "%s", why.c_str());
        std::string line = "enemy_move refused (enemy " + std::to_string(req->enemy) + "): " + why;
        if (g_state) {  // a mod retrying every step: the same line once per 10 s
            std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
            line = g_state->put_refusals.pass(line, now());
        }
        if (!line.empty()) core_log(line);
        return 0u;
    };
    if (!w || w != dispatching_world())
        return refuse("call it from a world callback (on_tick, run_on_world, on_event) on the server world passed to it");
    if (WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return refuse("not a server world (host only)");
    MoveOrders* m = g_state->moves;
    if (!m) return refuse("move orders are off (see the log)");
    if (!req->points || req->count == 0 || req->count > kMovePointsMax) return refuse("1 to 16 points");
    std::vector<MoveOrders::Point> points;
    for (uint32_t i = 0; i < req->count; ++i)
        points.push_back({req->points[i].x, req->points[i].y, req->points[i].z});
    const double arrive = req->arrive == 0.0f ? 2.0 : req->arrive;
    const double stuck = req->size >= offsetof(EmfMoveOrder, stuck_seconds) + sizeof(req->stuck_seconds)
                             ? static_cast<double>(req->stuck_seconds)
                             : 0.0;  // a mod built with 0.6.8's or 0.6.9's header
    MoveOrders::Callback done;
    if (cb)
        done = [cb, user](EmfWorld* world, uint32_t id, uint32_t status) {
            if (!guarded_move_call(cb, world, id, status, user))
                core_log("enemy_move: a mod's callback faulted (move " + std::to_string(id) + ")");
        };
    std::string why;
    const uint32_t id = m->add(w, req->enemy, points, arrive, req->seconds, req->flags, std::move(done), &why, stuck);
    if (!id) return refuse(why);
    if (g_state->orders) g_state->orders->cancel_enemy(w, req->enemy);  // an enemy order on it ends (v39), once the move holds
    return id;
}

bool api_enemy_move_cancel(uint32_t id) {
    MoveOrders* m = g_state ? g_state->moves : nullptr;
    if (!m) return false;
    EmfWorld* w = dispatching_world();
    const bool now = w && WorldView(raw(w)).kind() == EMF_WORLD_SERVER;  // else at the next step
    return m->cancel(id, now);
}

uint32_t api_enemy_moving(EmfWorld* w, uint32_t enemy) {
    MoveOrders* m = g_state ? g_state->moves : nullptr;
    return m && w ? m->moving(w, enemy) : 0;
}

bool api_door_lock(EmfWorld* w, uint32_t entity, uint32_t locked) {
    DoorService* d = g_state ? g_state->doors : nullptr;
    if (!d) return door_refused("door_lock", "doors are off on this game build (see the log)");
    std::string why;
    if (d->request(raw(w), entity, locked ? DoorOrderKind::Lock : DoorOrderKind::Unlock, 0, &why)) return true;
    return door_refused("door_lock (entity " + std::to_string(entity) + ")", why);
}

void api_on_save(EmfSaveCallback cb, void* user) {
    if (g_state) g_state->save_events.subscribe(cb, user);
}

void api_on_load(EmfSaveCallback cb, void* user) {
    if (g_state) g_state->load_events.subscribe(cb, user);
}

// ---- v13: settings and mod info ----
std::string text_of(const char* s) { return s ? s : ""; }

bool api_setting_declare(const char* mod, const EmfSettingDef* def) {
    if (!g_state || !g_state->settings) return false;
    if (!def || def->size < kSettingDefV13Size || !def->key) {
        core_log("setting_declare refused: no definition, or its size field is too small");
        return false;
    }
    if (def->type > EMF_SETTING_TEXT) {
        core_log("setting_declare refused (" + text_of(mod) + "." + def->key + "): unknown type");
        return false;
    }
    SettingDef d;
    d.key = def->key;
    d.type = static_cast<SettingType>(def->type);
    d.label = text_of(def->label);
    d.help = text_of(def->help);
    d.default_value = text_of(def->default_value);
    d.has_min = (def->flags & EMF_SETTING_HAS_MIN) != 0;
    d.has_max = (def->flags & EMF_SETTING_HAS_MAX) != 0;
    d.min = def->min;
    d.max = def->max;
    d.live = (def->flags & EMF_SETTING_LIVE) != 0;
    if (def->choices) {
        std::string all = def->choices, part;
        for (char c : all) {
            if (c == '|') {
                d.choices.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        d.choices.push_back(part);
    }
    std::string why;
    if (!g_state->settings->declare(text_of(mod), d, &why)) {
        core_log("setting_declare refused (" + text_of(mod) + "): " + why);
        return false;
    }
    if (!why.empty()) core_log("setting_declare (" + text_of(mod) + "): " + why);  // declared, with a note
    return true;
}

int64_t api_setting_int(const char* mod, const char* key, int64_t fallback) {
    if (!g_state || !g_state->settings || !g_state->settings->has(text_of(mod), text_of(key))) return fallback;
    return g_state->settings->get_int(text_of(mod), text_of(key), fallback);
}

double api_setting_float(const char* mod, const char* key, double fallback) {
    if (!g_state || !g_state->settings || !g_state->settings->has(text_of(mod), text_of(key))) return fallback;
    return g_state->settings->get_float(text_of(mod), text_of(key), fallback);
}

uint32_t api_setting_text(const char* mod, const char* key, char* buf, uint32_t size) {
    if (buf && size) buf[0] = '\0';
    if (!g_state || !g_state->settings || !g_state->settings->has(text_of(mod), text_of(key))) return 0;
    const std::string v = g_state->settings->get(text_of(mod), text_of(key));
    if (buf && size) {
        const size_t n = (std::min)(v.size(), static_cast<size_t>(size - 1));
        std::memcpy(buf, v.data(), n);
        buf[n] = '\0';
        if (v.empty() && size >= 2) buf[1] = '\0';  // declared but empty (an undeclared one leaves buf[1] alone)
    }
    return static_cast<uint32_t>(v.size());
}

bool api_setting_set(const char* mod, const char* key, const char* value) {
    if (!g_state || !g_state->settings) return false;
    std::string why;
    const auto r = g_state->settings->set(text_of(mod), text_of(key), text_of(value), &why);
    if (r == SettingsStore::SetResult::Ok || r == SettingsStore::SetResult::Unchanged) return true;
    core_log("setting_set refused (" + text_of(mod) + "." + text_of(key) + " = " + text_of(value) + "): " + why);
    return false;
}

void api_on_setting_changed(const char* mod, EmfSettingCallback cb, void* user) {
    if (!g_state || !cb) return;
    std::lock_guard<std::mutex> l(g_state->setting_watchers_lock);
    g_state->setting_watchers.push_back({text_of(mod), cb, user, true});
}

void copy_text(char* dst, size_t cap, const std::string& s) {
    const size_t n = (std::min)(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

bool fill_mod_info(const ModRecord& r, EmfModInfo* out) {
    if (!out || out->size < kModInfoV13Size) return false;
    static_assert(EMF_MOD_SCRIPT == static_cast<uint32_t>(ModStatus::Script), "EMF_MOD_* follow ModStatus");
    out->status = static_cast<uint32_t>(r.status);
    copy_text(out->id, sizeof out->id, r.id);
    copy_text(out->name, sizeof out->name, r.name);
    copy_text(out->version, sizeof out->version, r.version);
    copy_text(out->source, sizeof out->source, r.source);
    copy_text(out->main, sizeof out->main, r.main);
    return true;
}

bool api_mod_info(const char* id, EmfModInfo* out) {
    if (!g_state) return false;
    auto r = g_state->mods.find(text_of(id));
    return r && fill_mod_info(*r, out);
}

bool api_mod_info_at(uint32_t index, EmfModInfo* out) {
    if (!g_state) return false;
    const auto all = g_state->mods.list();
    return index < all.size() && fill_mod_info(all[index], out);
}

// ---- v18: attributes and damage (docs/engine_parts/re_attributes_menu.md) ----
namespace ecs = emf::gen::ecs;
constexpr const char* kAttributeRegistry = "impact::AttributeRegistry";
constexpr const char* kImpactSystem = "impact::ImpactSystem";

// The same refusal line at most every 10 s (a mod calling every tick would flood the log otherwise).
void refuse_limited(const std::string& line) {
    if (!g_state) return;
    std::string out;
    {
        std::lock_guard<std::mutex> l(g_state->put_refusals_lock);
        out = g_state->put_refusals.pass(line, now());
    }
    if (!out.empty()) core_log(out);
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08x", v);
    return b;
}

// Where attribute `id` of `entity` lives: the world's registry and the attribute component whose rootId is the
// attribute's. The status says why not.
struct AttrHome {
    uint8_t* comp = nullptr;
    uint64_t size = 0;
    const void* registry = nullptr;
};
AttrStatus attribute_home(EmfWorld* w, uint32_t entity, uint32_t id, AttrHome* home, std::string* why) {
    home->registry = WorldView(raw(w)).singleton(kAttributeRegistry);
    if (!home->registry) {
        *why = "the world has no attribute registry (yet)";
        return AttrStatus::Fault;
    }
    uint32_t root = 0;
    const AttrStatus s = attr_root(g_state->attr_find, home->registry, id, &root);
    if (s != AttrStatus::Ok) {
        *why = attr_status_text(s);
        return s;
    }
    bool any = false;
    for (const auto& c : attribute_components()) {
        const ComponentRef r = get_component(g_state->get_component, raw(w), c.hash, entity);
        uint32_t comp_root = 0;
        if (!r.ptr || r.size < 8 || !seh_read(r.ptr, &comp_root, 4)) continue;
        any = true;
        if (comp_root != root) continue;
        home->comp = static_cast<uint8_t*>(r.ptr);
        home->size = r.size;
        return AttrStatus::Ok;
    }
    const AttrStatus none = any ? AttrStatus::OtherComponent : AttrStatus::NoComponent;
    *why = any ? "none of its attribute components holds that attribute" : attr_status_text(none);
    return none;
}

// The checks every v18 call starts with: the signatures it needs, a server world. False after logging why (a read on
// another world is only a false, like get_component's NULL).
bool attributes_ready(EmfWorld* w, const char* fn, bool need_add) {
    if (!g_state) return false;
    if (!g_state->attr_find || (need_add && !g_state->attr_add)) {
        log_once(std::string(fn) + " is off: the " + (g_state->attr_find ? "AttrAdd" : "AttrFind") +
                 " signature is not usable on this game build");
        return false;
    }
    if (!w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) {
        if (need_add)
            log_once(std::string(fn) + " refused: server world only (a joined client has no Health; read NetworkHealth)");
        return false;
    }
    return true;
}

bool api_attribute_get(EmfWorld* w, uint32_t entity, uint32_t id, int32_t* out) {
    if (!out || !attributes_ready(w, "attribute_get", false)) return false;
    AttrHome home;
    std::string why;
    if (attribute_home(w, entity, id, &home, &why) != AttrStatus::Ok) return false;
    int32_t v = 0;
    if (attr_read(g_state->attr_find, home.comp, home.size, home.registry, id, &v) != AttrStatus::Ok) return false;
    *out = v;
    return true;
}

// attribute_add on the world's own thread after its step (no program runs).
bool attribute_add_now(EmfWorld* w, uint32_t entity, uint32_t id, int32_t delta) {
    const std::string what = "attribute_add(" + std::to_string(entity) + ", " + hex32(id) + ", " + std::to_string(delta) + ")";
    AttrHome home;
    std::string why;
    if (attribute_home(w, entity, id, &home, &why) != AttrStatus::Ok) {
        refuse_limited(what + " refused: " + why);
        return false;
    }
    std::optional<int32_t> max;  // health, stamina and mana have one; a heal without it is refused (attr_add)
    if (const auto max_id = max_attribute_of(id)) {
        int32_t m = 0;
        if (attr_read(g_state->attr_find, home.comp, home.size, home.registry, *max_id, &m) == AttrStatus::Ok) max = m;
    }
    if (attribute_write_rule(id) == AttrWrite::GiveOnly) max = INT32_MAX;  // pending XP has no maximum of its own
    int32_t applied = 0, after = 0;
    const AttrStatus s = attr_add(g_state->attr_find, g_state->attr_add, home.comp, home.size, home.registry, id, delta,
                                  max, &applied, &after);
    if (s != AttrStatus::Ok) {
        refuse_limited(what + " refused: " + attr_status_text(s));
        return false;
    }
    return true;
}

// A call made outside the world's callbacks, run after the server world's next step.
struct DeferredCall {
    EmfWorld* world;
    uint32_t session;
    uint32_t a, b, c;
    int32_t value;
};
// Runs `fn` on the server world after its next step when that is still the world (and session) it was meant for.
void defer_to_server(const DeferredCall& call, void (*fn)(EmfWorld*, const DeferredCall&), const char* what) {
    struct Job {
        DeferredCall call;
        void (*fn)(EmfWorld*, const DeferredCall&);
        const char* what;
    };
    auto* job = new Job{call, fn, what};
    g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* w, void* u) {
        auto* j = static_cast<Job*>(u);
        const Job copy = *j;
        delete j;
        if (!g_state) return;
        if (w != copy.call.world) {
            refuse_limited(std::string(copy.what) + ": not run (the world it was meant for is gone)");
            return;
        }
        if (g_state->server_session.load() != copy.call.session) {
            refuse_limited(std::string(copy.what) + ": not run (the session it was meant for is over)");
            return;
        }
        copy.fn(w, copy.call);
    }, job);
}

bool api_attribute_add(EmfWorld* w, uint32_t entity, uint32_t id, int32_t delta) {
    if (!attributes_ready(w, "attribute_add", true)) return false;
    // XP, XP required and level are the game's to keep consistent; XP is given through pending XP, never taken
    const AttrWrite rule = attribute_write_rule(id);
    if (rule == AttrWrite::ReadOnly || (rule == AttrWrite::GiveOnly && delta <= 0)) {
        refuse_limited("attribute_add(" + std::to_string(entity) + ", " + hex32(id) + ", " + std::to_string(delta) +
                       ") refused: " +
                       (rule == AttrWrite::ReadOnly ? "that attribute is read-only (give XP with EMF_ATTR_XP_PENDING; the "
                                                      "game levels up itself)"
                                                    : "XP is only given (a positive amount of EMF_ATTR_XP_PENDING)"));
        return false;
    }
    if (dispatching_world() == w) return attribute_add_now(w, entity, id, delta);
    defer_to_server({w, g_state->server_session.load(), entity, id, 0, delta},
                    [](EmfWorld* world, const DeferredCall& c) { attribute_add_now(world, c.a, c.b, c.value); },
                    "attribute_add");
    return true;
}

// ---- v22: a player's level (docs/research xp_levels; reference.md "v22: XP and level") ----

constexpr const char* kBalancingRegistry = "BalancingRegistry";

// The game's pure balancing functions, called on their table (a fault is "unknown": 0).
uint32_t seh_xp_required(uint32_t (*fn)(uint32_t, const void*), uint32_t level, const void* bal) {
    __try {
        return fn(level, bal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uint32_t seh_level_cap(uint32_t (*fn)(const void*), const void* bal) {
    __try {
        return fn(bal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

const void* balancing_of(EmfWorld* w) {
    if (!g_state || !w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return nullptr;
    return WorldView(raw(w)).singleton(kBalancingRegistry);
}

uint32_t api_player_level_cap(EmfWorld* w) {
    const void* bal = balancing_of(w);
    return bal && g_state->level_cap ? seh_level_cap(g_state->level_cap, bal) : 0u;
}

uint32_t api_player_xp_required(EmfWorld* w, uint32_t level) {
    const void* bal = balancing_of(w);
    if (!bal || !g_state->xp_required || level < 1) return 0;
    const uint32_t cap = api_player_level_cap(w);
    if (!cap || level > cap) return 0;  // 1..cap only: with the cap unknown, no level is known to exist
    return seh_xp_required(g_state->xp_required, level, bal);
}

// Why player_level_set refuses `level` here ("" when it does not): the game's functions, the level within 1..cap.
std::string level_set_refusal(EmfWorld* w, uint32_t level) {
    if (!g_state->xp_required) return "the game's xpRequired is not found on this build";
    const uint32_t cap = api_player_level_cap(w);
    if (!cap) return "the game's level cap is not found on this build";
    if (level < 1 || level > cap) return "the level must be 1.." + std::to_string(cap);
    return {};
}

// player_level_set on the server world's own thread after its step: the game's admin "set level", with the
// requirement player_level keeps (xpRequired(level), never 0) and XP below it.
bool player_level_set_now(EmfWorld* w, uint32_t player, uint32_t level, uint32_t xp) {
    const std::string what = "player_level_set(" + std::to_string(player) + ", " + std::to_string(level) + ")";
    if (const std::string why = level_set_refusal(w, level); !why.empty()) {
        refuse_limited(what + " refused: " + why);
        return false;
    }
    const uint32_t need = api_player_xp_required(w, level);
    if (need == 0 || need > static_cast<uint32_t>(INT32_MAX)) {
        refuse_limited(what + " refused: the game gave no XP requirement for that level");
        return false;
    }
    AttrHome lvl, exp;
    std::string why;
    if (attribute_home(w, player, kAttrLevel, &lvl, &why) != AttrStatus::Ok ||
        attribute_home(w, player, kAttrXp, &exp, &why) != AttrStatus::Ok) {
        refuse_limited(what + " refused: " + why);
        return false;
    }
    int32_t cur_level = 0, cur_xp = 0, cur_pending = 0, cur_need = 0;
    if (attr_read(g_state->attr_find, lvl.comp, lvl.size, lvl.registry, kAttrLevel, &cur_level) != AttrStatus::Ok ||
        attr_read(g_state->attr_find, exp.comp, exp.size, exp.registry, kAttrXp, &cur_xp) != AttrStatus::Ok ||
        attr_read(g_state->attr_find, exp.comp, exp.size, exp.registry, kAttrXpPending, &cur_pending) != AttrStatus::Ok ||
        attr_read(g_state->attr_find, exp.comp, exp.size, exp.registry, kAttrXpRequired, &cur_need) != AttrStatus::Ok) {
        refuse_limited(what + " refused: the player's level or XP could not be read");
        return false;
    }
    const int32_t want_xp = static_cast<int32_t>((std::min)(xp, need - 1));
    // the requirement first, so no step ever sees a requirement of 0 (a free level) or the old one with the new XP
    struct Write {
        AttrHome* home;
        uint32_t id;
        int64_t delta;
    } const writes[] = {
        {&exp, kAttrXpRequired, static_cast<int64_t>(need) - cur_need},
        {&exp, kAttrXp, static_cast<int64_t>(want_xp) - cur_xp},
        {&exp, kAttrXpPending, -static_cast<int64_t>(cur_pending)},
        {&lvl, kAttrLevel, static_cast<int64_t>(level) - cur_level},
    };
    if (std::all_of(std::begin(writes), std::end(writes), [](const Write& wr) { return wr.delta == 0; }))
        return true;  // already so: nothing written, nothing logged (a mod may hold a level each step)
    for (const auto& wr : writes) {
        if (wr.delta == 0) continue;
        int32_t applied = 0;
        const AttrStatus s = attr_add(g_state->attr_find, g_state->attr_add, wr.home->comp, wr.home->size,
                                      wr.home->registry, wr.id, static_cast<int32_t>(wr.delta), INT32_MAX, &applied, nullptr);
        if (s != AttrStatus::Ok) {
            refuse_limited(what + " stopped half-way: " + attr_status_text(s));
            return false;
        }
    }
    core_log(what + ": level " + std::to_string(cur_level) + " -> " + std::to_string(level) + ", " +
             std::to_string(want_xp) + " of " + std::to_string(need) + " XP");
    return true;
}

bool api_player_level_set(EmfWorld* w, uint32_t player, uint32_t level, uint32_t xp) {
    if (!attributes_ready(w, "player_level_set", true)) return false;
    if (dispatching_world() == w) return player_level_set_now(w, player, level, xp);
    // checked at once too (the cap and the curve are pure reads): only the player's own state is checked later
    if (const std::string why = level_set_refusal(w, level); !why.empty()) {
        refuse_limited("player_level_set(" + std::to_string(player) + ", " + std::to_string(level) + ") refused: " + why);
        return false;
    }
    const int32_t x = static_cast<int32_t>((std::min)(xp, static_cast<uint32_t>(INT32_MAX)));
    defer_to_server({w, g_state->server_session.load(), player, level, 0, x},
                    [](EmfWorld* world, const DeferredCall& c) {
                        player_level_set_now(world, c.a, c.b, static_cast<uint32_t>(c.value));
                    },
                    "player_level_set");
    return true;
}

// The position of an entity in metres (CurrentTransform: fixed point, 1 m = 2^32), if it has one.
bool entity_position(EmfWorld* w, uint32_t entity, double out[3], uint8_t* transform56) {
    const ComponentRef t = get_component(g_state->get_component, raw(w), ecs::CurrentTransform::kHash, entity);
    uint8_t buf[56];
    if (!t.ptr || t.size < sizeof buf || !seh_read(t.ptr, buf, sizeof buf)) return false;
    int64_t p[3];
    std::memcpy(p, buf, sizeof p);
    for (int i = 0; i < 3; ++i) out[i] = static_cast<double>(p[i]) / 4294967296.0;
    if (transform56) std::memcpy(transform56, buf, sizeof buf);
    return true;
}

// Fills a damage request on the server world's thread (the target's components) and queues it for the pre-hook.
bool damage_now(EmfWorld* w, uint32_t attacker, uint32_t target, int32_t amount) {
    const std::string what = "damage(" + std::to_string(attacker) + " -> " + std::to_string(target) + ", " +
                             std::to_string(amount) + ")";
    // An entity without CombatFlags takes hits (the game's applyHitToEntity); with them, only while handleHits is on.
    const ComponentRef flags = get_component(g_state->get_component, raw(w), ecs::CombatFlags::kHash, target);
    if (flags.ptr) {
        ecs::CombatFlags cf{};
        if (flags.size != sizeof cf || !seh_read(flags.ptr, &cf, sizeof cf) || !cf.handleHits) {
            refuse_limited(what + " refused: the target takes no hits (CombatFlags.handleHits is off)");
            return false;
        }
        if (cf.immune) {
            refuse_limited(what + " refused: the target is immune (CombatFlags.immune)");
            return false;
        }
    }
    // Health, or a placed object (Destructible, on the entity or its template): the game gives it Health on the first
    // hit and the destructible program removes it at 0 (private research hit_hooks_2026-11-11 section 5).
    const bool has_health = get_component(g_state->get_component, raw(w), ecs::Health::kHash, target).ptr != nullptr;
    const bool placed = get_component(g_state->get_component, raw(w), ecs::Destructible::kHash, target).ptr != nullptr ||
                        (g_state->get_template_component &&
                         get_template_component(g_state->get_template_component, raw(w), ecs::Destructible::kHash, target));
    if (!has_health && !placed) {
        refuse_limited(what + " refused: the target has no Health and is not a placed object (Destructible)");
        return false;
    }
    HitSpec s;
    s.world = w;
    s.session = g_state->server_session.load();
    s.attacker = attacker;
    s.target = target;
    s.amount = amount;
    double at[3] = {}, from[3] = {};
    if (entity_position(w, target, at, s.target_transform)) {
        s.has_transform = true;
        for (int i = 0; i < 3; ++i) s.position[i] = static_cast<float>(at[i]);
        if (attacker && entity_position(w, attacker, from, nullptr)) {
            const double d[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-3)
                for (int i = 0; i < 3; ++i) s.direction[i] = static_cast<float>(d[i] / len);
        }
    }
    const ComponentRef faction = get_component(g_state->get_component, raw(w), ecs::Faction::kHash, target);
    if (faction.ptr && faction.size >= 1) seh_read(faction.ptr, &s.faction, 1);
    s.impact_id = g_state->damage_queue.next_impact_id();
    s.queued_tick = WorldView(raw(w)).tick();
    std::string why;
    if (!g_state->damage_queue.push(s, &why)) {
        refuse_limited(what + " refused: " + why);
        return false;
    }
    return true;
}

bool api_damage(EmfWorld* w, uint32_t attacker, uint32_t target, int32_t amount) {
    if (!g_state) return false;
    if (!g_state->alloc_pending_hit || !g_state->damage_hooked.load()) {
        log_once("damage is off: not on this game build (the AllocPendingHit signature, or a build whose layouts EMF has "
                 "not checked), or impact_hit_conclusion's hook is not installed (yet)");
        return false;
    }
    if (!w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) {
        log_once("damage refused: server world only");
        return false;
    }
    if (!target || amount <= 0) {
        refuse_limited("damage refused: it needs a target and an amount above 0");
        return false;
    }
    if (dispatching_world() == w) return damage_now(w, attacker, target, amount);
    defer_to_server({w, g_state->server_session.load(), attacker, target, 0, amount},
                    [](EmfWorld* world, const DeferredCall& c) { damage_now(world, c.a, c.b, c.value); }, "damage");
    return true;
}

// ---- v19: chat lines and saves on a server (docs/modding/server.md) ----
bool guarded_request_save(CoreState::RequestSaveFn fn, void* inst) {
    __try {
        fn(inst);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool chat_on() { return g_state->chat_on ? g_state->chat_on() : server_chat_on(); }
bool chat_tell_line(uint32_t handle, uint64_t account, const std::string& line, bool system) {
    return g_state->chat_tell_line ? g_state->chat_tell_line(handle, account, line, system)
                                   : queue_chat_tell(handle, account, line, system);
}
bool chat_everyone_line(const std::string& line, bool system) {
    return g_state->chat_everyone_line ? g_state->chat_everyone_line(line, system) : queue_chat_everyone(line, system);
}
std::string hex_flags(uint32_t flags) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", flags);
    return b;
}
// A game this process hosts with a local player (not the dedicated server): the host's own game does not show the
// server's chat lines, so a line for the host is a notify.
bool hosted_with_local_player(uint32_t* local) {
    if (!g_state || g_state->exe.server || !api_server_world()) return false;  // a joined client hosts nothing
    const uint32_t id = api_local_player();
    if (local) *local = id;
    return id != 0;
}
void notify_lines(const std::vector<std::string>& lines) {
    for (const auto& l : lines) {
        if (g_state->notify) g_state->notify(l);
        else core_log("[note] " + l);
    }
}

// Counts `lines` against `mod`'s budget; false (logged, limited) when over it. Lines that are not sent after all are
// given back (chat_budget_return), so only lines queued or shown count.
bool chat_budget_ok(const std::string& mod, const char* fn, size_t lines) {
    const int limit = chat_line_limit(mod);
    if (g_state->chat_budget.take(mod, static_cast<int>(lines), now(), limit)) return true;
    refuse_limited(std::string(fn) + " refused: " + mod + " sent more than " + std::to_string(limit) + " chat lines in 10 s");
    return false;
}
void chat_budget_return(const std::string& mod, size_t taken, size_t sent) {
    if (sent < taken) g_state->chat_budget.give_back(mod, static_cast<int>(taken - sent));
}

// The player's BasicPlayerInfos entry (on the world's thread), if it is theirs and holds an account.
std::optional<PlayerInfo> player_info_of(EmfWorld* w, uint32_t player, uint8_t* entry_out = nullptr) {
    if (player < 1 || player > kPlayerInfoCount) return std::nullopt;
    const auto* infos = static_cast<const uint8_t*>(WorldView(raw(w)).singleton("BasicPlayerInfos"));
    uint8_t entry[kPlayerInfoSize];
    if (!infos || !seh_read(infos + static_cast<size_t>(player - 1) * kPlayerInfoSize, entry, sizeof entry))
        return std::nullopt;
    if (entry_out) std::memcpy(entry_out, entry, sizeof entry);
    return read_player_info(entry, player);
}

// v43: the PlayerPermissions byte of the player's BasicPlayerInfos entry (server world).
bool api_player_permissions(EmfWorld* w, uint32_t player, uint32_t* perms) {
    if (perms) *perms = 0;
    if (!perms || !w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return false;
    uint8_t entry[kPlayerInfoSize];
    if (!player_info_of(w, player, entry)) return false;
    *perms = entry[kPlayerInfoPermissions] & 0x3Fu;
    return true;
}

// chat_tell on the server world's thread: the host's notify, or the outbox for the receive loop. How many lines went.
size_t chat_tell_now(EmfWorld* w, uint32_t player, const std::vector<std::string>& lines, bool system) {
    const std::string what = "chat_tell(" + std::to_string(player) + ")";
    uint32_t local = 0;
    if (hosted_with_local_player(&local) && local == player) {
        notify_lines(lines);
        return lines.size();
    }
    if (!chat_on()) {
        refuse_limited(what + " refused: players' chat is off here (in a game you host: [chat] players=1 in "
                              "emf\\emf_core.ini), or the server's chat functions were not found on this game build");
        return 0;
    }
    const auto info = player_info_of(w, player);
    if (!info) {
        refuse_limited(what + " refused: no such player in this world (or the game holds no account for them)");
        return 0;
    }
    size_t sent = 0;
    for (const auto& l : lines) {
        if (!chat_tell_line(info->handle, info->account, l, system)) {
            refuse_limited(what + " refused: too many chat lines are waiting for the server");
            return sent;
        }
        core_log("chat to player " + std::to_string(player) + ": " + l);
        ++sent;
    }
    return sent;
}

// chat_tell and chat_tell_ex (v30: system, a line without a speaker's name) for the mod `mod`.
bool chat_tell_from(EmfWorld* w, uint32_t player, const char* text, bool system, const std::string& mod) {
    if (!w || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) {
        refuse_limited("chat_tell refused: not a server world (" + mod + ")");
        return false;
    }
    const auto lines = chat_lines(text ? text : "", EMF_CHAT_MAX_LINES);
    if (lines.empty()) {
        refuse_limited("chat_tell refused: no text (" + mod + ")");
        return false;
    }
    if (!chat_budget_ok(mod, "chat_tell", lines.size())) return false;
    if (dispatching_world() == w) {
        const size_t sent = chat_tell_now(w, player, lines, system);
        chat_budget_return(mod, lines.size(), sent);
        return sent == lines.size();
    }
    struct Job {
        EmfWorld* world;
        uint32_t session, player;
        std::vector<std::string> lines;
        std::string mod;
        bool system;
    };
    auto* job = new Job{w, g_state->server_session.load(), player, lines, mod, system};
    g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* world, void* u) {
        std::unique_ptr<Job> j(static_cast<Job*>(u));
        if (!g_state) return;
        if (world != j->world || g_state->server_session.load() != j->session) {
            refuse_limited("chat_tell(" + std::to_string(j->player) + "): not sent (the world it was meant for is gone)");
            chat_budget_return(j->mod, j->lines.size(), 0);
            return;
        }
        chat_budget_return(j->mod, j->lines.size(), chat_tell_now(world, j->player, j->lines, j->system));
    }, job);
    return true;
}

bool api_chat_tell(EmfWorld* w, uint32_t player, const char* text) {
    if (!g_state) return false;
    return chat_tell_from(w, player, text, false, module_name_of(_ReturnAddress()));
}

bool api_chat_tell_ex(EmfWorld* w, uint32_t player, const char* text, uint32_t flags) {
    if (!g_state) return false;
    const std::string mod = module_name_of(_ReturnAddress());
    if (flags & ~EMF_CHAT_SYSTEM) {
        refuse_limited("chat_tell_ex refused: unknown flags " + hex_flags(flags & ~EMF_CHAT_SYSTEM) + " (" + mod + ")");
        return false;
    }
    return chat_tell_from(w, player, text, (flags & EMF_CHAT_SYSTEM) != 0, mod);
}

// chat_broadcast and chat_broadcast_ex (v30: system) for the mod `mod`.
bool chat_broadcast_from(const char* text, bool system, const std::string& mod) {
    if (!g_state->exe.server && !api_server_world()) {
        refuse_limited("chat_broadcast refused: this game hosts no world (a client of someone else's game) (" + mod + ")");
        return false;
    }
    const auto lines = chat_lines(text ? text : "", EMF_CHAT_MAX_LINES);
    if (lines.empty()) {
        refuse_limited("chat_broadcast refused: no text (" + mod + ")");
        return false;
    }
    if (!chat_budget_ok(mod, "chat_broadcast", lines.size())) return false;
    const bool host = hosted_with_local_player(nullptr);
    if (host) notify_lines(lines);
    if (!chat_on()) {
        if (!host) {
            refuse_limited("chat_broadcast refused: players' chat is off here (in a game you host: [chat] players=1 in "
                           "emf\\emf_core.ini), or the server's chat functions were not found on this game build");
            chat_budget_return(mod, lines.size(), 0);
            return false;
        }
        log_once("chat_broadcast: shown to the host only; other players get chat lines with [chat] players=1 in "
                 "emf\\emf_core.ini");
        return true;
    }
    size_t sent = 0;
    for (const auto& l : lines) {
        if (!chat_everyone_line(l, system)) {
            refuse_limited("chat_broadcast refused: too many chat lines are waiting for the server");
            if (!host) chat_budget_return(mod, lines.size(), sent);  // the host's notes were shown: they count
            return host;
        }
        core_log("chat to everyone: " + l);
        ++sent;
    }
    return true;
}

bool api_chat_broadcast(const char* text) {
    if (!g_state) return false;
    return chat_broadcast_from(text, false, module_name_of(_ReturnAddress()));
}

bool api_chat_broadcast_ex(const char* text, uint32_t flags) {
    if (!g_state) return false;
    const std::string mod = module_name_of(_ReturnAddress());
    if (flags & ~EMF_CHAT_SYSTEM) {
        refuse_limited("chat_broadcast_ex refused: unknown flags " + hex_flags(flags & ~EMF_CHAT_SYSTEM) + " (" + mod + ")");
        return false;
    }
    return chat_broadcast_from(text, (flags & EMF_CHAT_SYSTEM) != 0, mod);
}

bool api_player_name(EmfWorld* w, uint32_t player, char* buf, uint32_t size) {
    if (buf && size) buf[0] = '\0';
    if (!g_state || !w || !buf || size < 2 || WorldView(raw(w)).kind() != EMF_WORLD_SERVER) return false;
    const auto info = player_info_of(w, player);
    if (!info) return false;
    std::vector<uint8_t> records;
    if (g_state->read_connections) {
        if (!g_state->read_connections(&records)) return false;
    } else {
        uint8_t* server = chat_server();  // known once the server's receive loop ran (players' chat on)
        records.assign(static_cast<size_t>(server_layout::kConnectionSize) * server_layout::kConnectionCount, 0);
        if (!server || !seh_read(server + server_layout::kConnections, records.data(), records.size())) return false;
    }
    if (records.size() < static_cast<size_t>(server_layout::kConnectionSize) * server_layout::kConnectionCount) return false;
    const auto name = connection_name(records.data(), info->handle, info->account,
                                      [](const void* src, void* dst, size_t n) { return seh_read(src, dst, n); });
    if (!name || name->empty()) return false;
    const size_t n = (std::min)(name->size(), static_cast<size_t>(size - 1));
    std::memcpy(buf, name->data(), n);
    buf[n] = '\0';
    return true;
}

bool api_on_chat(EmfChatCallback cb, void* user) {
    if (!g_state || !cb) return false;
    if (!chat_on()) {
        core_log("on_chat refused (" + module_name_of(reinterpret_cast<const void*>(cb)) +
                 "): players' chat is off here (in a game you host: [chat] players=1 in emf\\emf_core.ini), or the "
                 "server's chat functions were not found on this game build");
        return false;
    }
    std::lock_guard<std::mutex> l(g_state->chat_watchers_lock);
    g_state->chat_watchers.push_back({cb, user, true});
    g_state->has_chat_watchers = true;
    return true;
}

// requestServerSave on the server world's thread, for the instance of this session's load.
bool request_save_now() {
    void* inst = g_state->save_instance.load();
    if (!inst || g_state->save_instance_session.load() != g_state->server_session.load()) {
        refuse_limited("request_save refused: no world save was loaded in this session (EMF knows the game's server "
                       "from its load)");
        return false;
    }
    if (!guarded_request_save(g_state->request_save, inst)) {
        refuse_limited("request_save: the game's save request faulted");
        return false;
    }
    core_log("request_save: the game was asked to save the world");
    return true;
}

bool api_request_save() {
    if (!g_state) return false;
    if (!g_state->request_save) {
        log_once("request_save is off: the RequestServerSave signature is not usable on this game build");
        return false;
    }
    EmfWorld* d = dispatching_world();
    if (d && WorldView(raw(d)).kind() == EMF_WORLD_SERVER) return request_save_now();
    if (!api_server_world()) {
        refuse_limited("request_save refused: this game hosts no world");
        return false;
    }
    const uint32_t session = g_state->server_session.load();
    g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld*, void* u) {
        const auto session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(u));
        if (!g_state) return;
        if (g_state->server_session.load() != session) {
            refuse_limited("request_save: not asked (the session it was meant for is over)");
            return;
        }
        request_save_now();
    }, reinterpret_cast<void*>(static_cast<uintptr_t>(session)));
    return true;
}

// ---- v20: teleport (docs/engine_parts/re_teleport.md; the work is TeleportService's, core/teleport.h) ----
static_assert(EMF_TELEPORT_FIND_GROUND == kTpFindGround && EMF_TELEPORT_FACE_YAW == kTpFaceYaw &&
                  EMF_TELEPORT_CHECKPOINT == kTpCheckpoint && EMF_TELEPORT_NOTIFY_READY == kTpNotifyReady,
              "the API's teleport flags are the core's");
static_assert(EMF_TELEPORT_FAILED == kTpFailed && EMF_TELEPORT_MOVED == kTpMoved && EMF_TELEPORT_READY == kTpReady &&
                  EMF_TELEPORT_OFF_TARGET == kTpOffTarget && EMF_TELEPORT_RAW_TARGET == kTpRawTarget,
              "the API's teleport results are the core's");

bool guarded_component_call(EmfComponentCallback cb, EmfWorld* w, uint32_t entity, uint32_t hash, uint32_t status,
                            const char* why, void* user) {
    __try {
        cb(w, entity, hash, status, why, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_offer_call(EmfOfferCallback cb, EmfWorld* w, uint32_t player, uint32_t entity, void* user) {
    __try {
        cb(w, player, entity, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool guarded_window_call(EmfUiCallback cb, EmfWorld* w, const EmfUiEvent* e, void* user) {
    __try {
        cb(w, e, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// v24: a mod's window (window_request.h; the overlay draws it, overlay.cpp).
bool refuse_window(const std::string& mod, const char* what, const std::string& why) {
    refuse_limited(std::string(what) + " refused (" + mod + "): " + why);
    return false;
}

bool api_window_open(const EmfWindowDef* def) {
    if (!g_state) return false;
    WindowRequest r;
    r.mod = module_name_of(_ReturnAddress());
    std::string why;
    if (!copy_window_def(def, false, &r, &why)) return refuse_window(r.mod, "window_open", why);
    if (!g_state->open_window) return refuse_window(r.mod, "window_open", "the in-game overlay is off (or no screen)");
    if (!g_state->tracker.latest(r.world_kind, now()))
        return refuse_window(r.mod, "window_open",
                             r.world_kind == EMF_WORLD_SERVER
                                 ? "no server world runs (in someone else's game pass EMF_WORLD_CLIENT): its events "
                                   "would never come"
                                 : "no client world runs: its events would never come");
    const std::string mod = r.mod;
    std::optional<WindowRequest> replaced;
    if (!g_state->open_window(std::move(r), &replaced, &why)) return refuse_window(mod, "window_open", why);
    if (replaced) post_window_event(*replaced, EMF_UI_CLOSED, "", EMF_UI_BY_MOD);
    return true;
}

bool api_window_update(const EmfWindowDef* def) {
    if (!g_state) return false;
    WindowRequest r;
    r.mod = module_name_of(_ReturnAddress());
    std::string why;
    if (!copy_window_def(def, true, &r, &why)) return refuse_window(r.mod, "window_update", why);
    return g_state->update_window && g_state->update_window(r);  // not open: false, no log (the player closed it)
}

bool api_window_close(const char* id) {
    if (!g_state || !id || !g_state->close_window) return false;
    std::optional<WindowRequest> closed;
    if (!g_state->close_window(id, &closed)) return false;
    if (closed) post_window_event(*closed, EMF_UI_CLOSED, "", EMF_UI_BY_MOD);
    return true;
}

bool api_offer_active(EmfWorld*, uint32_t entity) {
    if (!g_state || !g_state->offers || !entity) return false;
    return g_state->offers->active(entity, module_name_of(_ReturnAddress()));
}

// ---- v27 mod messages ----

bool guarded_net_call(EmfNetCallback cb, EmfWorld* w, const EmfNetFrom* f, const void* d, uint32_t n, void* user) {
    __try {
        cb(w, f, d, n, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A mod's C callback as the service's handler (a fault is caught and logged once; that callback is not called again).
net::NetHandler net_handler(EmfNetCallback cb, void* user, const std::string& channel) {
    if (!cb) return nullptr;
    auto broken = std::make_shared<std::atomic<bool>>(false);
    return [cb, user, channel, broken](EmfWorld* w, const net::NetFrom& f, const std::string& data) {
        if (broken->load()) return;
        EmfNetFrom from{};
        from.size = sizeof from;
        from.player = f.player;
        from.account = f.account;
        from.peer_version = f.peer_version;
        from.admin = f.admin ? 1 : 0;
        from.local = f.local ? 1 : 0;
        if (!guarded_net_call(cb, w, &from, data.data(), static_cast<uint32_t>(data.size()), user)) {
            broken->store(true);
            core_log("a net callback for " + channel + " in " + module_name_of(reinterpret_cast<const void*>(cb)) +
                     " faulted: it is not called again");
        }
    };
}

void net_refused(const std::string& mod, const char* what, const std::string& why) {
    log_once(std::string(what) + " refused (" + mod + "): " + why);
}

bool api_net_channel(const EmfNetChannel* def) {
    const std::string mod = module_name_of(_ReturnAddress());
    if (!g_state || !g_state->net) return false;
    if (!def || def->size < sizeof(EmfNetChannel) || !def->name) {
        net_refused(mod, "net_channel", "no definition, its size field too small, or no name");
        return false;
    }
    net::ChannelDef d;
    d.name = def->name;
    d.version = def->version;
    d.flags = def->flags;
    d.max_size = def->max_size;
    d.per_10s = def->per_10s;
    d.on_server = net_handler(def->on_server, def->user, d.name);
    d.on_player = net_handler(def->on_player, def->user, d.name);
    std::string why;
    if (!g_state->net->add_channel(mod, d, &why)) {
        net_refused(mod, "net_channel", why);
        return false;
    }
    return true;
}

bool net_data(const void* data, uint32_t size, std::string* out) {
    if (size && !data) return false;
    if (size > net::kMaxMessage) return false;
    out->assign(static_cast<const char*>(data ? data : ""), size);
    return true;
}

bool api_net_to_server(const char* channel, const void* data, uint32_t size) {
    const std::string mod = module_name_of(_ReturnAddress());
    std::string d, why;
    if (!g_state || !g_state->net || !channel || !net_data(data, size, &d)) return false;
    if (!g_state->net->to_server(mod, channel, d, &why)) {
        net_refused(mod, "net_to_server", why);
        return false;
    }
    return true;
}

bool api_net_to_player(EmfWorld*, uint32_t player, const char* channel, const void* data, uint32_t size) {
    const std::string mod = module_name_of(_ReturnAddress());
    std::string d, why;
    if (!g_state || !g_state->net || !channel || !net_data(data, size, &d)) return false;
    return g_state->net->to_player(mod, player, channel, d, &why);  // not a peer: false without a log (normal)
}

uint32_t api_net_to_players(EmfWorld*, const char* channel, const void* data, uint32_t size) {
    const std::string mod = module_name_of(_ReturnAddress());
    std::string d;
    if (!g_state || !g_state->net || !channel || !net_data(data, size, &d)) return 0;
    return g_state->net->to_players(mod, channel, d);
}

uint32_t api_net_peer(EmfWorld*, uint32_t player, const char* channel) {
    if (!g_state || !g_state->net || !channel) return 0;
    return g_state->net->peer(player, channel);
}

// ---- v26 pictures in windows ----

// The folder of the module that holds `address` (the mod calling the API), without a trailing slash.
std::wstring module_folder_of(const void* address) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &m))
        return L".";
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool portrait_key_ok(const std::string& k) {
    if (k.empty() || k.size() > 64 || k == "." || k == "..") return false;
    for (const char c : k)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-'))
            return false;
    if (k.find("..") != std::string::npos || k.back() == '.') return false;  // as the portrait cache's own keys
    std::string base = k.substr(0, k.find('.'));  // Windows device names, with or without an extension
    for (char& c : base) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
    const bool port = base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0;
    if (base.size() == 4 && port && base[3] >= '0' && base[3] <= '9')
        return false;
    return true;
}

std::mutex g_image_refusals_m;
std::set<std::string> g_image_refusals;  // keys refused once (logged once)

uint32_t api_image_load(const char* path) {
    if (!g_state || !path || !*path) return 0;
    const std::string mod = module_name_of(_ReturnAddress());
    const std::string p(path);
    std::wstring file;
    std::string key;
    const std::string kPortrait = "portrait:";
    if (p.compare(0, kPortrait.size(), kPortrait) == 0) {
        const std::string k = p.substr(kPortrait.size());
        if (!portrait_key_ok(k)) {
            core_log("image_load('" + p + "') refused (" + mod + "): a portrait key is 1-64 of A-Z a-z 0-9 _ . -");
            return 0;
        }
        file = g_state->portrait_dir + L"\\" + widen_utf8(k);
        key = p;
    } else {
        const std::wstring w = widen_utf8(p);
        const bool absolute = (w.size() > 2 && w[1] == L':') || (w.size() > 1 && (w[0] == L'\\' || w[0] == L'/'));
        file = absolute ? w : module_folder_of(_ReturnAddress()) + L"\\" + w;
        std::wstring lower = file;
        for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
        key = narrow_utf8(lower);
    }
    const auto refused = [&](const std::string& why) -> uint32_t {
        const std::lock_guard<std::mutex> g(g_image_refusals_m);
        if (g_image_refusals.insert(key).second) core_log("image_load('" + p + "') refused (" + mod + "): " + why);
        return 0;
    };
    if (!g_state->load_image) return refused("no in-game overlay (a dedicated server, or [overlay] enabled=0)");
    std::string why;
    uint32_t h = 0;
    try {
        h = g_state->load_image(file, key, &why);
    } catch (...) {  // never into the mod (an allocation that failed while decoding)
        h = 0;
        why = "it could not be loaded (out of memory?)";
    }
    return h ? h : refused(why.empty() ? "refused" : why);
}

bool guarded_teleport_call(EmfTeleportCallback cb, EmfWorld* w, const EmfTeleportResult* r, void* user) {
    __try {
        cb(w, r, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// v23: a component added to an entity, or removed (component_service.h).
bool api_change_component(EmfWorld* w, const EmfComponentChange* c) {
    if (!g_state) return false;
    if (!c || c->size < kComponentChangeV23Size) {
        log_once("change_component refused: no request, or its size field is too small");
        return false;
    }
    if (!g_state->components) {
        log_once("change_component is off: " +
                 (g_state->components_off.empty() ? std::string("not set up (yet)") : g_state->components_off));
        return false;
    }
    if (c->op != EMF_COMPONENT_ADD && c->op != EMF_COMPONENT_REMOVE) {
        refuse_limited("change_component refused: op must be EMF_COMPONENT_ADD or EMF_COMPONENT_REMOVE");
        return false;
    }
    ComponentChangeRequest r;
    r.world = w;
    r.entity = c->entity;
    r.hash = c->hash;
    r.op = c->op == EMF_COMPONENT_ADD ? ComponentOp::Add : ComponentOp::Remove;
    if (r.op == ComponentOp::Add && c->data) {
        if (!c->data_size || c->data_size > 4096) {
            refuse_limited("change_component refused: data given, so data_size must be the component's size");
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(c->data);
        r.data.assign(p, p + c->data_size);
    }
    r.flags = c->flags;
    r.mod = module_name_of(_ReturnAddress());  // a mod may remove what it added
    if (c->cb) {
        const EmfComponentCallback cb = c->cb;
        void* const user = c->user;
        const uint32_t entity = c->entity, hash = c->hash;
        r.done = [cb, user, entity, hash](EmfWorld* world, uint32_t status, const std::string& why) {
            if (!guarded_component_call(cb, world, entity, hash, status, why.c_str(), user))
                core_log("a change_component callback in " + module_name_of(reinterpret_cast<const void*>(cb)) +
                         " faulted");
        };
    }
    std::string why;
    return g_state->components->request(r, &why);
}

// v25: E prompts (offers.h). The caller's module owns the offer (a Lua script's offers are the Lua host's to track).
bool offer_spec_from(const EmfOffer* o, OfferSpec& s, const char* what) {
    if (!o || o->size < kOfferV25Size) {
        refuse_limited(std::string(what) + " refused: no offer, or its size field is too small");
        return false;
    }
    s.verb = o->verb;
    if (o->text) {
        const size_t n = strnlen(o->text, 64);
        if (n == 0 || n > 63) {
            refuse_limited(std::string(what) + " refused: text must be 1 to 63 bytes");
            return false;
        }
        s.text.assign(o->text, n);
        register_verb_text(s.text);
    }
    if ((o->flags & ~(kOfferLowPriority | kOfferBuildMode)) || o->reserved) {
        refuse_limited(std::string(what) + " refused: unknown flags (this EMF knows EMF_OFFER_LOW_PRIORITY and "
                                           "EMF_OFFER_BUILD_MODE), or reserved is not 0");
        return false;
    }
    s.sequence = o->sequence;
    s.range_m = o->range_m;
    s.flags = o->flags;
    return true;
}

bool offers_on(EmfWorld* w, const char* what) {
    if (!g_state) return false;
    if (!g_state->offers) {
        log_once(std::string(what) + " is off: " +
                 (g_state->components_off.empty() ? std::string("not set up (yet)") : g_state->components_off));
        return false;
    }
    if (!world_call_allowed(w, what)) return false;
    if (WorldView(raw(w)).kind() != EMF_WORLD_SERVER) {
        refuse_limited(std::string(what) + " refused: the server world only (a world this game hosts)");
        return false;
    }
    return true;
}

OfferCallback offer_callback(EmfOfferCallback cb, void* user) {
    if (!cb) return nullptr;
    auto broken = std::make_shared<std::atomic<bool>>(false);  // a callback that faulted is not called again
    return [cb, user, broken](EmfWorld* w, uint32_t player, uint32_t entity) {
        if (broken->load()) return;
        if (!guarded_offer_call(cb, w, player, entity, user)) {
            broken->store(true);
            core_log("an offer callback in " + module_name_of(reinterpret_cast<const void*>(cb)) +
                     " faulted: it is not called again (the prompt stays until the mod removes it)");
        }
    };
}

uint32_t api_spawn_offer(EmfWorld* w, const uint8_t guid[16], double x, double y, double z, const EmfOffer* o,
                         EmfOfferCallback cb, void* user) {
    if (!offers_on(w, "spawn_offer") || !guid) return 0;
    OfferSpec s;
    if (!offer_spec_from(o, s, "spawn_offer")) return 0;
    std::string why;
    const uint32_t id = g_state->offers->spawn_offer(w, guid, x, y, z, s, module_name_of(_ReturnAddress()),
                                                     offer_callback(cb, user), &why);
    if (!id) core_log("spawn_offer refused: " + why);
    return id;
}

bool api_offer(EmfWorld* w, uint32_t entity, const EmfOffer* o, EmfOfferCallback cb, void* user) {
    if (!offers_on(w, "offer")) return false;
    OfferSpec s;
    if (!offer_spec_from(o, s, "offer")) return false;
    std::string why;
    if (g_state->offers->offer(w, entity, s, module_name_of(_ReturnAddress()), offer_callback(cb, user), &why))
        return true;
    refuse_limited("offer(" + std::to_string(entity) + ") refused: " + why);
    return false;
}

bool api_offer_remove(EmfWorld* w, uint32_t entity) {
    if (!offers_on(w, "offer_remove")) return false;
    std::string why;
    if (g_state->offers->remove(w, entity, module_name_of(_ReturnAddress()), &why)) return true;
    refuse_limited("offer_remove(" + std::to_string(entity) + ") refused: " + why);
    return false;
}

bool api_teleport(EmfWorld* w, const EmfTeleport* req) {
    if (!g_state) return false;
    if (!req || req->size < kTeleportV20Size) {
        log_once("teleport refused: no request, or its size field is too small");
        return false;
    }
    if (!g_state->teleport) {
        log_once("teleport is off: " + (g_state->teleport_off.empty() ? std::string("not set up (yet)") : g_state->teleport_off));
        return false;
    }
    TeleportRequest r;
    r.world = w;
    r.entity = req->entity;
    r.x = req->x;
    r.y = req->y;
    r.z = req->z;
    r.yaw_deg = req->yaw_deg;
    r.spawn_point = req->spawn_point;
    r.flags = req->flags;
    if (req->cb) {
        const EmfTeleportCallback cb = req->cb;
        void* const user = req->user;
        const bool notify_ready = (req->flags & EMF_TELEPORT_NOTIFY_READY) != 0;
        r.done = [cb, user, notify_ready](EmfWorld* world, const TeleportOutcome& o) {
            EmfTeleportResult res{};
            res.size = sizeof res;
            res.entity = o.entity;
            res.status = o.status;
            res.flags = o.flags;
            if (o.status != kTpMoved || !notify_ready || o.session_ended) res.flags |= EMF_TELEPORT_LAST;
            if (o.session_ended) res.flags |= EMF_TELEPORT_SESSION_ENDED;
            res.x = o.pos[0];
            res.y = o.pos[1];
            res.z = o.pos[2];
            res.distance = o.distance;
            res.reason = o.reason.c_str();
            if (!guarded_teleport_call(cb, world, &res, user))
                core_log("a teleport callback in " + module_name_of(reinterpret_cast<const void*>(cb)) + " faulted");
        };
    }
    return g_state->teleport->request(r, w != nullptr && w == dispatching_world(), nullptr);
}

// ---- v21: world settings (docs/modding/world_settings.md; the work is ws::Service's, core/world_settings_service.h) ----
ws::Service* world_settings_or_log(const char* what) {
    if (!g_state || !g_state->world_settings) {
        log_once(std::string(what) + ": world settings are not set up on this core");
        return nullptr;
    }
    return g_state->world_settings;
}

bool api_world_setting_get(const char* key, double* value) {
    if (!key || !value || !g_state || !g_state->world_settings) return false;
    return g_state->world_settings->get(key, value, nullptr);
}

bool api_world_setting_set(const char* key, double value, uint32_t flags) {
    ws::Service* svc = world_settings_or_log("world_setting_set");
    if (!svc) return false;
    if (!key || (flags & ~EMF_WS_PERSIST)) {
        core_log("world_setting_set refused: " + std::string(key ? "unknown flag bits" : "no key"));
        return false;
    }
    std::string why;
    if (svc->set(key, value, (flags & EMF_WS_PERSIST) != 0, &why)) return true;
    core_log("world_setting_set " + std::string(key) + " refused: " + why);
    return false;
}

bool api_world_setting_reset(const char* key, uint32_t flags) {
    ws::Service* svc = world_settings_or_log("world_setting_reset");
    if (!svc) return false;
    if (flags & ~EMF_WS_PERSIST) {
        core_log("world_setting_reset refused: unknown flag bits");
        return false;
    }
    std::string why;
    if (svc->reset(key ? key : "", (flags & EMF_WS_PERSIST) != 0, &why)) return true;
    core_log("world_setting_reset " + std::string(key ? key : "all") + " refused: " + why);
    return false;
}

bool guarded_world_setting_call(EmfWorldSettingCallback cb, const char* key, double value, void* user) {
    __try {
        cb(key, value, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool api_on_world_setting_changed(EmfWorldSettingCallback cb, void* user) {
    if (!cb) return false;
    ws::Service* svc = world_settings_or_log("on_world_setting_changed");
    if (!svc) return false;
    svc->on_changed([cb, user](const std::string& key, double value) {
        if (!guarded_world_setting_call(cb, key.c_str(), value, user))
            core_log("a world setting callback in " + module_name_of(reinterpret_cast<const void*>(cb)) + " faulted");
    });
    return true;
}

uint32_t world_setting_type(ws::Type t) {
    switch (t) {
    case ws::Type::Bool: return EMF_WS_BOOL;
    case ws::Type::Enum: return EMF_WS_ENUM;
    case ws::Type::Minutes: return EMF_WS_MINUTES;
    case ws::Type::Byte: return EMF_WS_WHOLE;
    case ws::Type::Float: break;
    }
    return EMF_WS_FLOAT;
}

bool api_world_setting_info(uint32_t index, EmfWorldSettingInfo* out) {
    if (!out || out->size < sizeof(EmfWorldSettingInfo) || !g_state || !g_state->world_settings) return false;
    const auto rows = g_state->world_settings->rows();  // empty without a hosted world
    if (index >= rows.size()) return false;
    const auto& r = rows[index];
    out->type = world_setting_type(r.field->type);
    out->key = r.field->key;
    out->alias = r.field->alias ? r.field->alias : "";
    out->group = r.field->group ? r.field->group : "";
    out->min = r.field->min;
    out->max = r.field->max;
    out->value = r.value;
    out->world = r.world;
    out->flags = (r.changed ? EMF_WS_CHANGED : 0u) | (r.field->needs_client ? EMF_WS_NEEDS_CLIENT : 0u);
    out->pad = 0;
    return true;
}

bool guarded_setting_call(EmfSettingCallback cb, EmfWorld* w, const char* mod, const char* key, void* user) {
    __try {
        cb(w, mod, key, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const EmfApi kApi = {
    EMF_API_VERSION, sizeof(EmfApi),  api_log,        api_server_world, api_client_world,
    api_world_kind,  api_world_tick,  api_ecs_time_ns, api_singleton,   api_local_player,
    api_get_component, api_for_each_entity, api_on_tick, api_run_on_world, api_resolve,
    api_hook,        api_hook_program, api_register_program, api_spawn, api_destroy,
    api_on_save,     api_on_load,
    api_register_program_def, api_emit_event, api_read_events,
    api_on_event,
    api_for_each_item, api_find_item, api_item_info, api_give_item, api_spawn_ex,
    api_register_command, api_command_reply,
    api_get_template_component, api_item_data, api_set_item_max_stack, api_write_memory,
    api_program_singleton, api_program_read_other, api_put_items,
    api_server_session,
    api_wheel_open, api_notify,
    api_put_items_ex, api_take_items,
    api_hud_set,
    api_setting_declare, api_setting_int, api_setting_float, api_setting_text, api_setting_set,
    api_on_setting_changed, api_mod_info, api_mod_info_at,
    api_find_pattern, api_hook_pattern, api_hook_address, api_game_build,
    api_command_sender,
    api_program_write_other,
    api_register_command_ex,
    api_attribute_get, api_attribute_add, api_damage,
    api_chat_tell, api_chat_broadcast, api_player_name, api_on_chat, api_request_save,
    api_teleport,
    api_world_setting_get, api_world_setting_set, api_world_setting_reset, api_on_world_setting_changed,
    api_world_setting_info,
    api_player_xp_required, api_player_level_cap, api_player_level_set,
    api_change_component,
    api_window_open,
    api_window_update,
    api_window_close,
    api_spawn_offer,
    api_offer,
    api_offer_remove,
    api_image_load,
    api_offer_active,
    api_net_channel,
    api_net_to_server,
    api_net_to_player,
    api_net_to_players,
    api_net_peer,
    api_on_player,
    api_item_name,
    api_game_language,
    api_player_character_name,
    api_chat_tell_ex,
    api_chat_broadcast_ex,
    api_on_hit,
    api_on_hit_before,
    api_hit_rule_add,
    api_hit_rule_remove,
    api_buff_types,
    api_buff_type,
    api_buff_text,
    api_buffs_of,
    api_buff_apply,
    api_buff_remove,
    api_on_buff,
    api_marker_set,
    api_marker_remove,
    api_marker_clear,
    api_marker_types,
    api_world_to_screen,
    api_marker_send,
    api_marker_unsend,
    api_map_cursor,
    api_on_map_click,
    api_map_action,
    api_ground_at,
    // v35 a minimap's parts (api_minimap.cpp)
    api_player_view,
    api_fog_at,
    api_map_markers,
    api_canvas_set,
    api_canvas_draw,
    api_remove_building,  // v36
    api_path_find,        // v37
    api_path_cancel,
    api_door_state,  // v38
    api_door_set,
    api_door_lock,
    api_enemy_order,  // v39
    api_enemy_order_cancel,
    api_enemy_target,
    api_enemy_move,  // v40
    api_enemy_move_cancel,
    api_enemy_moving,
    api_player_permissions,
};

bool guarded_chat_call(EmfChatCallback cb, EmfWorld* w, uint64_t account, uint32_t entity, const char* text, void* user) {
    __try {
        cb(w, account, entity, text, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void set_core_state(CoreState* state) { g_state = state; }
CoreState* core_state() { return g_state; }
const EmfApi* api_table() { return &kApi; }

uint32_t api_drop_items(EmfWorld* w, const uint8_t item_guid[16], uint32_t count, const float pos[3]) {
    auto fn = reinterpret_cast<SpawnWrapperFn>(api_resolve("SpawnWrapper"));
    const uint32_t item = item_id_from_guid(item_guid);
    const auto rec = fn && item ? item_by_id(item_registry(w), item) : std::nullopt;
    std::string why = !fn ? "the SpawnWrapper signature is not usable on this game build" : "unknown item";
    uint32_t dropped = 0;
    for (uint32_t left = count; rec && left > 0;) {  // a pickup holds at most 65535
        const uint32_t n = (std::min)(left, 65535u);
        if (!give_pickup(fn, raw(w), *rec, n, pos[0], pos[1] + 0.5, pos[2], &why)) break;
        dropped += n;
        left -= n;
    }
    if (dropped < count)
        core_log("breaking blocks: " + std::to_string(count - dropped) + " of item " + std::to_string(item) +
                 " could not be dropped (" + why + ")");
    return dropped;
}

bool install_emf_hook(const char* name, uintptr_t target, void* detour, void** original) {
    if (g_state && g_state->hooks) {
        std::string why;
        if (g_state->hooks->install("emf", name, target, detour, original, &why) == HookTable::Result::Ok) return true;
        core_log(std::string("EMF's hook '") + name + "' failed: " + why);
        return false;
    }
    return hook_install(target, detour, original);
}

void damage_pre_conclusion(void* program_context) {
    hit_hook_new_step();  // first: this thread's once-per-hit tracker starts over for this run
    if (!g_state) return;
    uint8_t* world = program_context_world(program_context);
    if (!world) return;
    const WorldView view(world);
    if (view.kind() != EMF_WORLD_SERVER) return;  // the client world runs the same function
    const std::string order = g_state->hit_order.on_conclusion(view.tick());
    if (!order.empty()) core_log(order);
    if (!g_state->damage_queue.waiting()) return;
    void* system = view.singleton(kImpactSystem);
    int64_t period = 0;
    int64_t time_ns = 0;
    if (g_state->tick_offsets_ok && seh_read(world + world_offsets().tick_period_ns, &period, 8))
        time_ns = period * static_cast<int64_t>(view.tick());
    const size_t n = g_state->damage_queue.flush(world, g_state->server_session.load(), system,
                                                 g_state->alloc_pending_hit, time_ns, view.tick(), core_log);
    if (!system) log_once("damage: the server world has no impact::ImpactSystem singleton: hits wait, then are dropped");
    (void)n;
}

// ---- v31 hits: what the detour reads (the impact thread inside the server world's step; the conclusion writes Health
// and reads CombatFlags and Destructible, so these reads do not race it) ----
namespace {

EmfWorld* env_world(void* ctx) { return static_cast<EmfWorld*>(ctx); }

const void* destructible_of(EmfWorld* w, uint32_t id) {
    const ComponentRef r = get_component(g_state->get_component, raw(w), ecs::Destructible::kHash, id);
    if (r.ptr && r.size >= sizeof(ecs::Destructible)) return r.ptr;
    if (!g_state->get_template_component) return nullptr;
    return get_template_component(g_state->get_template_component, raw(w), ecs::Destructible::kHash, id);
}

HitClass hit_env_classify(uint32_t id, void* ctx) {
    if (!id) return HitClass::None;
    if (id <= 0x3FE) return HitClass::Player;  // the game's own player test (ids 1..0x3FE)
    EmfWorld* w = env_world(ctx);
    if (destructible_of(w, id)) return HitClass::Object;
    if (get_component(g_state->get_component, raw(w), ecs::Health::kHash, id).ptr) return HitClass::Creature;
    return HitClass::None;
}

bool hit_env_applies(uint32_t id, void* ctx) {
    const ComponentRef r = get_component(g_state->get_component, raw(env_world(ctx)), ecs::CombatFlags::kHash, id);
    ecs::CombatFlags cf{};
    if (!r.ptr || r.size < sizeof cf || !seh_read(r.ptr, &cf, sizeof cf)) return true;  // no CombatFlags: applied
    return cf.handleHits;
}

int32_t hit_env_health(uint32_t id, void* ctx) {
    EmfWorld* w = env_world(ctx);
    if (g_state->attr_find) {
        AttrHome home;
        std::string why;
        int32_t v = 0;
        if (attribute_home(w, id, kAttrHealth, &home, &why) == AttrStatus::Ok &&
            attr_read(g_state->attr_find, home.comp, home.size, home.registry, kAttrHealth, &v) == AttrStatus::Ok)
            return v;
    }
    uint16_t max = 0;  // a placed object not hit yet: the game gives it Health = Destructible.maxHealth on this hit
    if (const void* d = destructible_of(w, id); d && seh_read(d, &max, sizeof max)) return max;
    return -1;
}

uint32_t hit_env_weapon_item(uint32_t pide, void* ctx) {
    if (!pide) return 0;
    const ComponentRef r = get_component(g_state->get_component, raw(env_world(ctx)), ecs::ItemState::kHash, pide);
    uint32_t item = 0;
    if (!r.ptr || r.size < sizeof(ecs::ItemState) ||
        !seh_read(static_cast<const uint8_t*>(r.ptr) + offsetof(ecs::ItemState, itemId), &item, sizeof item))
        return 0;
    return item;
}

// v31 on_hit: one HitEvent of the server world, after its step on its thread
void hit_event_delivered(EmfWorld* w, const void* event, uint32_t size, void*) {
    if (!g_state || !g_state->hit_callbacks.after_count()) return;
    EmfHit h{};
    if (!hit_from_event(static_cast<const uint8_t*>(event), size, &h)) return;
    uint32_t pide = 0;
    std::memcpy(&pide, static_cast<const uint8_t*>(event) + offsetof(ecs::HitEvent, weaponPideId), sizeof pide);
    h.weapon_item = hit_env_weapon_item(pide, w);
    h.health_after = hit_env_health(h.part, w);  // the entity the hit was applied to, else its actor
    if (h.health_after < 0 && h.target != h.part) h.health_after = hit_env_health(h.target, w);
    g_state->hit_callbacks.call_after(w, h);
}

// rules and on_hit_before need the detour (logged once while it is off)
bool hit_before_ready() {
    if (g_state->hit_hook_off_why.empty()) return true;
    log_once("hits: before the hit is off (" + g_state->hit_hook_off_why + "): hit rules and on_hit_before are refused");
    return false;
}

}  // namespace

// ---- fall, shroud, cold and drowning (the env programs' worker thread; their Health is theirs alone meanwhile) ----
namespace {

HitClass env_classify(uint32_t id, void*) {
    if (!id) return HitClass::None;
    return id <= 0x3FE ? HitClass::Player : HitClass::Creature;  // no Destructible read here: it may race
}

int32_t env_max_health(uint32_t id, void* ctx) {
    AttrHome home;
    std::string why;
    int32_t v = 0;
    if (!g_state->attr_find || attribute_home(env_world(ctx), id, kAttrMaxHealth, &home, &why) != AttrStatus::Ok ||
        attr_read(g_state->attr_find, home.comp, home.size, home.registry, kAttrMaxHealth, &v) != AttrStatus::Ok)
        return -1;
    return v;
}

bool env_add_health(uint32_t id, int32_t delta, void* ctx, int32_t* now) {
    if (!g_state->attr_find || !g_state->attr_add) return false;
    AttrHome home;
    std::string why;
    if (attribute_home(env_world(ctx), id, kAttrHealth, &home, &why) != AttrStatus::Ok) return false;
    const int32_t max = env_max_health(id, ctx);
    int32_t applied = 0;
    return attr_add(g_state->attr_find, g_state->attr_add, home.comp, home.size, home.registry, kAttrHealth, delta,
                    max > 0 ? std::optional<int32_t>(max) : std::nullopt, &applied, now) == AttrStatus::Ok;
}

// Health inside the program: the attribute only (a real value may be below 0); no Destructible read here: it may race
int32_t env_health(uint32_t id, void* ctx) {
    if (!g_state->attr_find) return kEnvNoHealth;
    AttrHome home;
    std::string why;
    int32_t v = 0;
    if (attribute_home(env_world(ctx), id, kAttrHealth, &home, &why) != AttrStatus::Ok ||
        attr_read(g_state->attr_find, home.comp, home.size, home.registry, kAttrHealth, &v) != AttrStatus::Ok)
        return kEnvNoHealth;
    return v;
}

// EnvironmentSensor (+0 fogType: 0 none, 1 none for now, 2 dangerous, 3 deadly); fog_resistance_update reads it too
constexpr uint32_t kEnvironmentSensorHash = 0x556C976Eu;
bool env_deadly_fog(uint32_t id, void* ctx) {
    const ComponentRef r = get_component(g_state->get_component, raw(env_world(ctx)), kEnvironmentSensorHash, id);
    uint8_t fog = 0;
    return r.ptr && r.size >= 1 && seh_read(r.ptr, &fog, sizeof fog) && fog == 3;
}

// The slice of the program's job: J = *ctx, its entity list and count at the offsets read from code.
bool read_env_slice(void* ctx, EnvSlice* s) {
    uint8_t* job = nullptr;
    if (!ctx || !seh_read(ctx, &job, sizeof job) || !job) return false;
    const uint8_t* list = nullptr;
    uint64_t count = 0;
    if (!seh_read(job + g_state->env_list_off, &list, sizeof list) ||
        !seh_read(job + g_state->env_count_off, &count, sizeof count) || (count && !list) || count > 65536)
        return false;
    s->list = list;
    s->count = count;
    return true;
}

thread_local EnvSnapshot t_env;

bool env_wanted() {
    // a v31 rule alone (entity hits only) wants no env hits
    return g_state && g_state->env_off_why.empty() &&
           (g_state->hit_rules.any_env() || g_state->hit_callbacks.before_count() || g_state->hit_callbacks.after_count());
}

uint8_t* env_server_world(void* ctx) {
    uint8_t* world = program_context_world(ctx);
    return world && WorldView(world).kind() == EMF_WORLD_SERVER ? world : nullptr;
}

void env_pre_work(void* ctx, uint8_t) {
    t_env.clear();  // only this run's snapshot is ever used
    uint8_t* world = env_server_world(ctx);
    EnvSlice s;
    if (!world || !read_env_slice(ctx, &s)) return;
    t_env.pre(ctx, s, &env_health, world);
}

void env_post_work(void* ctx, uint8_t kind) {
    uint8_t* world = env_server_world(ctx);
    EnvSlice s;
    if (!world || !read_env_slice(ctx, &s)) {
        t_env.clear();
        return;
    }
    const std::vector<EnvDrop> drops = t_env.post(ctx, s, &env_health, world);
    if (drops.empty()) return;
    EnvEnv env;
    env.world = reinterpret_cast<EmfWorld*>(world);
    env.ctx = world;
    env.classify = &env_classify;
    env.add_health = &env_add_health;
    env.deadly_fog = &env_deadly_fog;
    for (const EnvDrop& d : drops) {
        EmfHit out{};
        if (env_apply(kind, d, env, g_state->hit_rules, g_state->hit_callbacks, g_state->hit_stats, g_state->hit_carry,
                      &out))
            g_state->env_queue.push(out);
    }
}

bool guarded_env(void (*work)(void*, uint8_t), void* ctx, uint8_t kind) {
    __try {
        work(ctx, kind);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void env_pre(void* program_context, uint8_t kind) {
    if (!env_wanted()) {
        t_env.clear();  // a post later in this run finds no snapshot (review I2)
        return;
    }
    if (!guarded_env(&env_pre_work, program_context, kind)) ++g_state->hit_stats.faults;
}

// by the snapshot alone: a rule added or removed between pre and post neither skips nor invents a drop
void env_post(void* program_context, uint8_t kind) {
    if (!g_state || !t_env.holds(program_context)) return;
    if (!guarded_env(&env_post_work, program_context, kind)) {
        ++g_state->hit_stats.faults;
        t_env.clear();
    }
}

void env_new_session() {
    if (!g_state) return;
    g_state->hit_carry.clear();
    (void)g_state->env_queue.take();  // the old world's entities: never delivered to the new one
}

void deliver_env_hits(EmfWorld* w) {
    if (!g_state || !w) return;
    for (EmfHit& h : g_state->env_queue.take()) {
        double at[3] = {};
        if (entity_position(w, h.target, at, nullptr))
            for (int i = 0; i < 3; ++i) h.position[i] = static_cast<float>(at[i]);
        g_state->hit_callbacks.call_after(w, h);
    }
}

bool subscribe_hit_events() {
    if (!g_state) return false;
    std::string why;
    const bool ok = g_state->relay.subscribe(EMF_WORLD_SERVER, "HitEvent", sizeof(ecs::HitEvent), hit_event_delivered,
                                             nullptr, &why);
    g_state->hit_events = ok;
    if (!ok) g_state->hit_events_off_why = why;
    return ok;
}

bool api_on_hit(EmfHitCallback cb, void* user) {
    if (!g_state || !cb) return false;
    if (!g_state->hit_events.load()) {
        log_once("on_hit refused: the server world's HitEvents are not available (" + g_state->hit_events_off_why + ")");
        return false;
    }
    const std::string module = module_name_of(reinterpret_cast<const void*>(cb));
    if (g_state->hit_callbacks.add_after(cb, user, module)) return true;
    refuse_limited("on_hit (" + module + ") refused: 16 callbacks already");
    return false;
}

bool api_on_hit_before(EmfHitBeforeCallback cb, void* user) {
    if (!g_state || !cb || !hit_before_ready()) return false;
    const std::string module = module_name_of(reinterpret_cast<const void*>(cb));
    if (g_state->hit_callbacks.add_before(cb, user, module)) return true;
    refuse_limited("on_hit_before (" + module + ") refused: 16 callbacks already");
    return false;
}

uint32_t api_hit_rule_add(const EmfHitRule* rule) {
    if (!g_state || !hit_before_ready()) return 0;
    if (!rule) {
        refuse_limited("hit_rule_add refused: no rule");
        return 0;
    }
    std::string why;
    const uint32_t id = g_state->hit_rules.add(*rule, &why);
    if (!id) refuse_limited("hit_rule_add refused: " + why);
    return id;
}

bool api_hit_rule_remove(uint32_t id) { return g_state && g_state->hit_rules.remove(id); }

void fill_pre_hit_env(EmfWorld* w, PreHitEnv* env) {
    env->world = w;
    env->ctx = w;
    if (!g_state || !w) return;
    env->classify = &hit_env_classify;
    env->applies = &hit_env_applies;
    env->health = &hit_env_health;
    env->weapon_item = &hit_env_weapon_item;
}

void note_program_hook(const std::string& program, bool installed) {
    if (!g_state) return;
    if (!installed) {
        core_log("EMF's program hook on '" + program + "' could not be installed" +
                 (program == "impact_hit_conclusion" ? ": damage stays off" : ""));
        return;
    }
    if (program == "impact_hit_conclusion") g_state->damage_hooked = true;
}

void damage_pre_clear(void* program_context) {
    if (!g_state) return;
    uint8_t* world = program_context_world(program_context);
    if (!world || WorldView(world).kind() != EMF_WORLD_SERVER) return;
    g_state->hit_order.on_clear(WorldView(world).tick());
}

void pump_setting_changes(EmfWorld* world) {
    if (!g_state || !g_state->settings) return;
    const auto changes = g_state->settings->take_changes();
    if (changes.empty()) return;
    std::vector<CoreState::SettingWatcher> watchers;
    {
        std::lock_guard<std::mutex> l(g_state->setting_watchers_lock);
        watchers = g_state->setting_watchers;
    }
    for (const auto& [mod, key] : changes) {
        for (size_t i = 0; i < watchers.size(); ++i) {
            auto& w = watchers[i];
            if (!w.enabled || (!w.mod.empty() && w.mod != mod)) continue;
            if (guarded_setting_call(w.cb, world, mod.c_str(), key.c_str(), w.user)) continue;
            w.enabled = false;
            {
                std::lock_guard<std::mutex> l(g_state->setting_watchers_lock);
                g_state->setting_watchers[i].enabled = false;
            }
            core_log("an on_setting_changed callback in " + module_name_of(reinterpret_cast<const void*>(w.cb)) +
                     " (told about a change of " + mod + "." + key + ") faulted and was switched off");
        }
    }
}

void note_chat_line(ChatLine line) {
    if (!g_state || !g_state->has_chat_watchers.load()) return;
    if (!g_state->chat_inbox.push(std::move(line)))
        refuse_limited("on_chat: more than 256 players' chat lines waited for the server world; the rest are dropped");
}

void pump_chat_lines(EmfWorld* world) {
    if (!g_state || !g_state->has_chat_watchers.load()) return;
    auto lines = g_state->chat_inbox.take();
    if (lines.empty()) return;
    std::vector<CoreState::ChatWatcher> watchers;
    {
        std::lock_guard<std::mutex> l(g_state->chat_watchers_lock);
        watchers = g_state->chat_watchers;
    }
    for (const auto& line : lines) {
        const uint32_t entity = g_state->entity_by_account ? g_state->entity_by_account(line.account) : 0u;
        for (size_t i = 0; i < watchers.size(); ++i) {
            auto& w = watchers[i];
            if (!w.enabled) continue;
            if (guarded_chat_call(w.cb, world, line.account, entity, line.text.c_str(), w.user)) continue;
            w.enabled = false;
            {
                std::lock_guard<std::mutex> l(g_state->chat_watchers_lock);
                g_state->chat_watchers[i].enabled = false;
            }
            core_log("an on_chat callback in " + module_name_of(reinterpret_cast<const void*>(w.cb)) +
                     " faulted and was switched off");
        }
    }
}

namespace {
struct WindowJob {
    EmfUiCallback cb;
    void* user;
    std::string id, key;
    uint32_t kind, reason;
    std::shared_ptr<std::atomic<bool>> open;  // the window's token: a click is delivered only while it is set
};
bool click_for_closed_window(const WindowJob& j) { return j.kind != EMF_UI_CLOSED && j.open && !j.open->load(); }
void deliver_window_event(EmfWorld* w, const WindowJob& j, uint32_t flags) {
    EmfUiEvent e{};
    e.size = sizeof e;
    e.kind = j.kind;
    e.window = j.id.c_str();
    e.key = j.key.c_str();
    e.from_key = "";
    e.reason = j.reason;
    e.flags = flags;
    if (!guarded_window_call(j.cb, w, &e, j.user))
        core_log("a window callback in " + module_name_of(reinterpret_cast<const void*>(j.cb)) + " faulted");
}
}  // namespace

void post_window_event(const WindowRequest& request, uint32_t kind, const std::string& key, uint32_t reason) {
    if (!g_state || !request.cb) return;
    WindowJob job{request.cb, request.user, request.id, key, kind, reason, request.open};
    if (click_for_closed_window(job)) return;  // its CLOSED is posted or delivered already: `user` may be freed
    if (!g_state->tracker.latest(request.world_kind, now())) {  // no step would come: CLOSED now, a click is dropped
        if (kind == EMF_UI_CLOSED) deliver_window_event(nullptr, job, EMF_UI_NO_WORLD);
        else log_once("a mod window's click was dropped: no world of its kind runs");
        return;
    }
    g_state->dispatcher.post(request.world_kind, [](EmfWorld* w, void* u) {
        std::unique_ptr<WindowJob> j(static_cast<WindowJob*>(u));
        if (click_for_closed_window(*j)) return;  // the window closed after this click was queued
        deliver_window_event(w, *j, 0);
    }, new WindowJob(std::move(job)));
}

void post_wheel_choice(const WheelRequest& request, int choice) {
    if (!g_state || !request.cb) return;
    struct Job {
        EmfWheelCallback cb;
        void* user;
        int choice;
    };
    auto* job = new Job{request.cb, request.user, choice};
    g_state->dispatcher.post(request.world_kind, [](EmfWorld* w, void* u) {
        auto* j = static_cast<Job*>(u);
        const Job copy = *j;
        delete j;  // before the call: a faulting callback must not leak it
        copy.cb(w, copy.choice, copy.user);
    }, job);
}

PlayerEvents::Deps player_events_deps() {
    return PlayerEvents::Deps{
        [](EmfWorld* w) { return read_players(api_table(), w); },
        [] { return static_cast<uint64_t>(GetTickCount64()); },
        [](const std::string& s) { core_log(s); },
        [](EmfPlayerCallback cb, EmfWorld* w, const EmfPlayerEvent* e, void* u) { return guarded_player_call(cb, w, e, u); },
        [](const void* addr) { return module_name_of(addr); }};
}

net::NetDeps net_deps(CoreState* s) {
    net::NetDeps d;
    d.hosted = [](uint32_t* id) { return hosted_with_local_player(id); };
    d.post = [s](uint8_t kind, std::function<void(EmfWorld*)> fn) {
        using Fn = std::function<void(EmfWorld*)>;
        // a message for this session's world: when a new world of that kind began meanwhile, it is not delivered there
        const auto session = [s, kind] {
            return kind == EMF_WORLD_SERVER ? s->server_session.load() : s->client_session.load();
        };
        const uint32_t then = session();
        s->dispatcher.post(kind, [](EmfWorld* w, void* u) {
            const std::unique_ptr<Fn> f(static_cast<Fn*>(u));  // freed even when the handler faults (it is guarded)
            (*f)(w);
        }, new Fn([fn = std::move(fn), session, then](EmfWorld* w) {
            if (session() == then) fn(w);
        }));
    };
    d.now_ms = [s] { return s->now_ms ? s->now_ms() : GetTickCount64(); };
    d.log = [](const std::string& line) { core_log(line); };
    d.connection_of = [s](uint32_t entity) -> std::optional<std::pair<uint32_t, uint64_t>> {
        return s->net_connection_of ? s->net_connection_of(entity) : std::nullopt;
    };
    d.entity_of = [s](uint64_t account) { return s->net_entity_of ? s->net_entity_of(account) : 0u; };
    d.is_admin = [s](uint64_t account) { return s->net_is_admin && s->net_is_admin(account); };
    return d;
}

}  // namespace emf::core
