// The C API table handed to mods. Every function is safe to call with a null/stale world:
// reads are fault-guarded and return 0/NULL.
#include "api.h"
#include "api_sizes.h"
#include "common/pattern_find.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <set>
#include <vector>

#include "attributes.h"
#include "common/log_limiter.h"
#include "common/hooks.h"
#include "common/offsets.h"
#include "common/seh.h"
#include "events.h"
#include "inventory.h"
#include "items.h"
#include "program_access.h"
#include "spawn.h"
#include "teleport.h"
#include "emf/generated/components.hpp"

namespace emf::core {
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
    if (!seh_read(raw(w) + off::kTickPeriodNs, &period, 8) || !seh_read(raw(w) + off::kTick, &tick, 4)) return 0;
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
    return ok;
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
    if (dispatching_world() == w) return attribute_add_now(w, entity, id, delta);
    defer_to_server({w, g_state->server_session.load(), entity, id, 0, delta},
                    [](EmfWorld* world, const DeferredCall& c) { attribute_add_now(world, c.a, c.b, c.value); },
                    "attribute_add");
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
    const ComponentRef flags = get_component(g_state->get_component, raw(w), ecs::CombatFlags::kHash, target);
    ecs::CombatFlags cf{};
    if (!flags.ptr || flags.size != sizeof cf || !seh_read(flags.ptr, &cf, sizeof cf) || !cf.handleHits) {
        refuse_limited(what + " refused: the target takes no hits (no CombatFlags.handleHits)");
        return false;
    }
    if (cf.immune) {
        refuse_limited(what + " refused: the target is immune (CombatFlags.immune)");
        return false;
    }
    if (!get_component(g_state->get_component, raw(w), ecs::Health::kHash, target).ptr) {
        refuse_limited(what + " refused: the target has no Health");
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
bool chat_tell_line(uint32_t handle, uint64_t account, const std::string& line) {
    return g_state->chat_tell_line ? g_state->chat_tell_line(handle, account, line) : queue_chat_tell(handle, account, line);
}
bool chat_everyone_line(const std::string& line) {
    return g_state->chat_everyone_line ? g_state->chat_everyone_line(line) : queue_chat_everyone(line);
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

// chat_tell on the server world's thread: the host's notify, or the outbox for the receive loop. How many lines went.
size_t chat_tell_now(EmfWorld* w, uint32_t player, const std::vector<std::string>& lines) {
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
        if (!chat_tell_line(info->handle, info->account, l)) {
            refuse_limited(what + " refused: too many chat lines are waiting for the server");
            return sent;
        }
        core_log("chat to player " + std::to_string(player) + ": " + l);
        ++sent;
    }
    return sent;
}

bool api_chat_tell(EmfWorld* w, uint32_t player, const char* text) {
    if (!g_state) return false;
    const std::string mod = module_name_of(_ReturnAddress());
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
        const size_t sent = chat_tell_now(w, player, lines);
        chat_budget_return(mod, lines.size(), sent);
        return sent == lines.size();
    }
    struct Job {
        EmfWorld* world;
        uint32_t session, player;
        std::vector<std::string> lines;
        std::string mod;
    };
    auto* job = new Job{w, g_state->server_session.load(), player, lines, mod};
    g_state->dispatcher.post(EMF_WORLD_SERVER, [](EmfWorld* world, void* u) {
        std::unique_ptr<Job> j(static_cast<Job*>(u));
        if (!g_state) return;
        if (world != j->world || g_state->server_session.load() != j->session) {
            refuse_limited("chat_tell(" + std::to_string(j->player) + "): not sent (the world it was meant for is gone)");
            chat_budget_return(j->mod, j->lines.size(), 0);
            return;
        }
        chat_budget_return(j->mod, j->lines.size(), chat_tell_now(world, j->player, j->lines));
    }, job);
    return true;
}

bool api_chat_broadcast(const char* text) {
    if (!g_state) return false;
    const std::string mod = module_name_of(_ReturnAddress());
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
        if (!chat_everyone_line(l)) {
            refuse_limited("chat_broadcast refused: too many chat lines are waiting for the server");
            if (!host) chat_budget_return(mod, lines.size(), sent);  // the host's notes were shown: they count
            return host;
        }
        core_log("chat to everyone: " + l);
        ++sent;
    }
    return true;
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

bool guarded_teleport_call(EmfTeleportCallback cb, EmfWorld* w, const EmfTeleportResult* r, void* user) {
    __try {
        cb(w, r, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
    if (g_state->tick_offsets_ok && seh_read(world + off::kTickPeriodNs, &period, 8))
        time_ns = period * static_cast<int64_t>(view.tick());
    const size_t n = g_state->damage_queue.flush(world, g_state->server_session.load(), system,
                                                 g_state->alloc_pending_hit, time_ns, view.tick(), core_log);
    if (!system) log_once("damage: the server world has no impact::ImpactSystem singleton: hits wait, then are dropped");
    (void)n;
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

}  // namespace emf::core
