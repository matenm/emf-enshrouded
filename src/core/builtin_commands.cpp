#include "builtin_commands.h"

#include "mod_registry.h"
#include "settings.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "common/offsets.h"
#include "common/pattern_find.h"
#include "common/seh.h"
#include "api.h"
#include "daytime.h"
#include "inventory.h"
#include "emf/emf.hpp"
#include "emf/generated/components.hpp"

namespace emf::core {
namespace {

constexpr double kFixed = 4294967296.0;

BuiltinContext& ctx_of(void* user) { return *static_cast<BuiltinContext*>(user); }

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

bool parse_uint(const char* s, uint32_t max, uint32_t* out) {
    if (!s || !*s) return false;
    uint64_t v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + uint64_t(*p - '0');
        if (v > max) return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

bool parse_hours(const char* s, double* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (!end || *end || !(v >= 0.0 && v <= 24.0)) return false;
    *out = v;
    return true;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool player_pos(BuiltinContext& c, EmfWorld* w, uint32_t player, double out[3]) {
    uint64_t size = 0;
    const auto* p = static_cast<const int64_t*>(c.api->get_component(w, off::kHashCurrentTransform, player, &size));
    if (!p || size < 24) return false;
    for (int i = 0; i < 3; ++i) out[i] = p[i] / kFixed;
    return true;
}

// A component of `id` when its size is the generated struct's (else null).
template <class T>
const T* comp_of(BuiltinContext& c, EmfWorld* w, uint32_t id) {
    uint64_t size = 0;
    const void* p = c.api->get_component(w, T::kHash, id, &size);
    return p && size == sizeof(T) ? static_cast<const T*>(p) : nullptr;
}

// near [radius]: the entities around the player, nearest first, tagged by what they have.
void cmd_near(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t radius = 10;
    if (argc > 2 || (argc == 2 && (!parse_uint(argv[1], 200, &radius) || radius == 0))) {
        command_reply_line("usage: near [radius 1-200]");
        return;
    }
    double p[3];
    if (!player_pos(c, w, c.player ? c.player() : 0, p)) {
        command_reply_line("near: player position unknown");
        return;
    }
    struct Hit {
        uint32_t id;
        double d, pos[3];
    };
    struct Scan {
        BuiltinContext* c;
        EmfWorld* w;
        double p[3], r;
        std::vector<Hit> hits;
    } s{&c, w, {p[0], p[1], p[2]}, static_cast<double>(radius), {}};
    c.api->for_each_entity(w, [](uint32_t id, void* user) {
        auto& s = *static_cast<Scan*>(user);
        double q[3];
        if (!player_pos(*s.c, s.w, id, q)) return;
        const double dx = q[0] - s.p[0], dy = q[1] - s.p[1], dz = q[2] - s.p[2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= s.r) s.hits.push_back({id, d, {q[0], q[1], q[2]}});
    }, &s);
    std::sort(s.hits.begin(), s.hits.end(), [](const Hit& a, const Hit& b) { return a.d < b.d; });
    const size_t shown = (std::min)(s.hits.size(), size_t{30});
    for (size_t i = 0; i < shown; ++i) {
        const Hit& h = s.hits[i];
        std::string tags;
        if (comp_of<emf::gen::ecs::PlayerState>(c, w, h.id)) tags += " player";
        if (c.api->get_template_component && c.api->get_template_component(w, off::kHashActiveNpcState, h.id, nullptr))
            tags += " villager";
        if (comp_of<emf::gen::ecs::InventorySetup>(c, w, h.id)) tags += " inventory";
        if (comp_of<emf::gen::ecs::AnimalSaved>(c, w, h.id)) tags += " animal";
        if (comp_of<emf::gen::ecs::DynamicGrowth>(c, w, h.id)) {
            const auto* lv = comp_of<emf::gen::ecs::Level>(c, w, h.id);
            tags += lv ? fmt(" plant (level %u)", lv->dataStorage[0]) : std::string(" plant");
        }
        if (comp_of<emf::gen::ecs::Health>(c, w, h.id)) tags += " health";
        command_reply_line(fmt("  %u: %.1f m (%.1f, %.1f, %.1f)%s", h.id, h.d, h.pos[0], h.pos[1], h.pos[2], tags.c_str()));
    }
    command_reply_line(fmt("%zu entities within %u m%s", s.hits.size(), radius,
                           s.hits.size() > shown ? " (the nearest 30 shown)" : ""));
}

// inv <entity>: every stack in the entity's own and linked inventories.
void cmd_inv(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t id = 0;
    if (argc != 2 || !parse_uint(argv[1], 0xFFFFFFFFu, &id) || !id) {
        command_reply_line("usage: inv <entity>");
        return;
    }
    const auto* setup = comp_of<emf::gen::ecs::InventorySetup>(c, w, id);
    std::vector<uint32_t> invs{id};
    if (setup)
        for (const auto& link : setup->linksEntities)
            if (link.id && link.id != id) invs.push_back(link.id);
    size_t n = 0;
    for (uint32_t inv_id : invs) {
        const auto* inv = comp_of<emf::gen::ecs::Inventory>(c, w, inv_id);
        if (!inv) continue;
        for (uint32_t slot = 0; slot < 8; ++slot) {
            const auto& st = inv->slots[slot];
            if (!st.id.value) continue;
            ++n;
            EmfItemInfo info{};
            info.size = sizeof(info);
            const std::string name = c.api->item_info(w, st.id.value, &info) && info.name[0]
                                         ? std::string(info.name)
                                         : fmt("item %08x", st.id.value);
            std::string extra;
            if (st.data.pide.id) {
                extra = fmt(", data entity %u", st.data.pide.id);
                if (const auto* is = comp_of<emf::gen::ecs::ItemState>(c, w, st.data.pide.id))
                    extra += fmt(", level %u, rarity %u", static_cast<unsigned>(is->itemLevelUi),
                                 static_cast<unsigned>(is->itemRarityUi));
            }
            const unsigned long long slot_id = (static_cast<unsigned long long>(slot) << 32) | inv_id;
            command_reply_line(fmt("  inventory %u slot %u (slot id 0x%llx): %s x%u%s", inv_id, slot, slot_id,
                                   name.c_str(), st.data.count, extra.c_str()));
        }
    }
    if (!setup && !n)
        command_reply_line(fmt("entity %u has no inventory", id));
    else
        command_reply_line(fmt("entity %u: %zu stack(s)", id, n));
}

// hud <id> [place 0-2] [text]: a line kept on screen (API v12), to try a layout; no text removes it.
void cmd_hud(EmfWorld*, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    if (argc < 2) {
        command_reply_line("usage: hud <id> [place 0-2] [text]: no text removes the line");
        return;
    }
    uint32_t place = 0;
    int first = 2;
    if (argc > 2 && parse_uint(argv[2], 2, &place)) first = 3;
    std::string text;
    for (int i = first; i < argc; ++i) text += (text.empty() ? "" : " ") + std::string(argv[i]);
    if (!c.api->hud_set || !c.api->hud_set(argv[1], text.c_str(), place, 0)) {
        command_reply_line("hud: refused (see the log)");
        return;
    }
    command_reply_line(text.empty() ? fmt("hud: %s removed", argv[1]) : fmt("hud: %s shown (place %u)", argv[1], place));
}

// Every command; for a player in chat who is no admin only the ones they may run (marked for players, v17). In the
// chat (10 lines per command reach it) in pages of 8: "help 2" and so on.
void cmd_help(EmfWorld*, int argc, const char* const* argv, void* u) {
    const CommandSender* s = current_sender();
    const bool restricted = s && s->restricted;
    const bool chat = s && s->from_chat;
    std::vector<std::string> lines;
    for (const auto& c : ctx_of(u).center->entries()) {
        if (restricted && !c.players) continue;
        std::string line = c.help.empty() ? c.name : c.name + " " + c.help;
        if (c.players && !restricted && c.name != "help") line += " (any player)";
        lines.push_back(line);
    }
    if (!chat) {
        for (const auto& l : lines) command_reply_line(l);
        return;
    }
    constexpr size_t kPage = 8;
    const size_t pages = lines.empty() ? 1 : (lines.size() + kPage - 1) / kPage;
    size_t page = argc > 1 ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 1;
    page = page < 1 ? 1 : page > pages ? pages : page;
    std::string head = std::string(restricted ? "commands you may run here" : "commands") + ", page " +
                       std::to_string(page) + " of " + std::to_string(pages);
    if (page < pages) head += " (/help " + std::to_string(page + 1) + " for more)";
    command_reply_line(head);
    for (size_t i = (page - 1) * kPage; i < lines.size() && i < page * kPage; ++i) command_reply_line(lines[i]);
}

void cmd_mods(EmfWorld*, int, const char* const*, void* u) {
    auto& c = ctx_of(u);
    const auto mods = c.registry ? c.registry->list() : std::vector<ModRecord>{};
    size_t loaded = 0, off = 0, not_loaded = 0;
    for (const auto& m : mods) {
        std::string line = m.id;
        if (!m.version.empty()) line += " " + m.version;
        line += " (" + (m.has_manifest ? m.name : m.source) + "): " + mod_status_name(m.status);
        if (!m.reason.empty()) line += " (" + m.reason + ")";
        command_reply_line(line);
        if (m.status == ModStatus::Loaded || m.status == ModStatus::Script) ++loaded;
        else if (m.status == ModStatus::Disabled) ++off;
        else ++not_loaded;
    }
    command_reply_line(fmt("%zu mod(s): %zu loaded, %zu switched off, %zu not loaded", mods.size(), loaded, off,
                           not_loaded));
}

// "exe+0x6c0fc0" for an address in the game exe, else the module and offset, else the address
std::string where(uintptr_t a) {
    const CoreState* st = core_state();
    if (st && a >= st->exe.base && a < st->exe.base + st->exe.size)
        return fmt("exe+0x%llx", static_cast<unsigned long long>(a - st->exe.base));
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &m)) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        std::string s(path);
        s = s.substr(s.find_last_of("\\/") + 1);
        return fmt("%s+0x%llx", s.c_str(), static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)));
    }
    return fmt("0x%llx", static_cast<unsigned long long>(a));
}

void cmd_hooks(EmfWorld*, int, const char* const*, void*) {
    const CoreState* st = core_state();
    if (!st || !st->hooks) {
        command_reply_line("hooks are not available");
        return;
    }
    // The list is in install order. On one function the newest hook runs first and its original calls the one
    // installed before it; the oldest calls the game.
    const auto list = st->hooks->list();
    for (size_t i = 0; i < list.size(); ++i) {
        size_t before = 0, after = 0;
        for (size_t j = 0; j < list.size(); ++j)
            if (j != i && list[j].target == list[i].target) ++(j < i ? before : after);
        std::string line = list[i].owner + ": " + list[i].name + " at " + where(list[i].target);
        if (before + after) {
            const size_t pos = after + 1, n = before + after + 1;  // 1 = runs first
            const char* th = (pos % 100 >= 11 && pos % 100 <= 13) ? "th"
                             : pos % 10 == 1                      ? "st"
                             : pos % 10 == 2                      ? "nd"
                             : pos % 10 == 3                      ? "rd"
                                                                  : "th";
            line += fmt(" (chained: runs %zu%s of %zu)", pos, th, n);
        }
        command_reply_line(line);
    }
    command_reply_line(fmt("%zu hook(s)", list.size()));
}

void cmd_pattern(EmfWorld*, int argc, const char* const* argv, void*) {
    const CoreState* st = core_state();
    if (argc < 2 || !st) {
        command_reply_line("usage: pattern <bytes, e.g. 48 8B 05 ?? ?? ?? ??> [.section]");
        return;
    }
    std::string section = ".text", bytes;
    int last = argc - 1;
    if (argv[last][0] == '.') {
        section = argv[last];
        --last;
    }
    for (int i = 1; i <= last; ++i) bytes += (i > 1 ? " " : "") + std::string(argv[i]);
    const emf::Section* sec = nullptr;
    for (const auto& s : st->exe.sections)
        if (s.name == section) sec = &s;
    if (!sec) {
        command_reply_line("the game exe has no section " + section);
        return;
    }
    emf::PatternQuery q;
    q.bytes = bytes;
    const auto r = emf::find_unique({st->exe.scan_bytes(*sec), sec->size}, sec->start, st->exe.base, st->exe.size, q);
    if (r.address)
        command_reply_line("found once in " + section + " at " + where(r.address));
    else
        command_reply_line(r.error + (r.matches == 0 && r.error == "not found" ? " in " + section : ""));
}

std::string range_note(const SettingDef& d) {
    char b[160] = "";
    if (d.has_min && d.has_max) std::snprintf(b, sizeof b, ", %g to %g", d.min, d.max);
    else if (d.has_min) std::snprintf(b, sizeof b, ", at least %g", d.min);
    else if (d.has_max) std::snprintf(b, sizeof b, ", at most %g", d.max);
    std::string out = b;
    if (d.step > 0) {
        std::snprintf(b, sizeof b, ", steps of %g", d.step);
        out += b;
    }
    if (d.slider) out += ", slider";
    if (d.max_length) out += ", at most " + std::to_string(d.max_length) + " characters";
    return out;
}

// "1 (bottom right)" for a choice, "tr (Top right)" for a select, the value otherwise
std::string shown_value(const SettingDef& d, const std::string& v) {
    if (d.type != SettingType::Choice && d.type != SettingType::Select) return v;
    const std::string label = setting_shown_value(d, v);
    return label != v ? v + " (" + label + ")" : v;
}

void cmd_settings(EmfWorld*, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    if (argc < 2 || !c.settings) {
        command_reply_line("usage: settings <mod>");
        return;
    }
    const std::string id = argv[1];
    const auto rec = c.registry ? c.registry->find(id) : std::nullopt;
    const auto defs = c.settings->defs(id);
    if (!rec && defs.empty()) {
        command_reply_line(fmt("no mod \"%s\" (mods lists them)", id.c_str()));
        return;
    }
    if (rec)
        command_reply_line(fmt("enabled = %d (the mod's switch; applies at the next game start)",
                               c.settings->enabled(id, rec->enabled_default) ? 1 : 0));
    if (defs.empty()) {
        command_reply_line(id + " has no settings");
        return;
    }
    for (const auto& d : defs) {
        std::string type = setting_type_name(d.type);
        if (d.type == SettingType::Choice) {
            type += ":";
            for (size_t i = 0; i < d.choices.size(); ++i) type += (i ? ", " : " ") + std::to_string(i) + " " + d.choices[i];
        }
        if (d.type == SettingType::Select) {
            type += ":";
            for (size_t i = 0; i < d.options.size(); ++i)
                type += (i ? ", " : " ") + d.options[i].value +
                        (d.options[i].label != d.options[i].value ? " (" + d.options[i].label + ")" : "");
        }
        std::string line = d.key + " = " + shown_value(d, c.settings->get(id, d.key)) + "  [" + type + range_note(d) +
                           "; default " + d.default_value + "; " + (d.live ? "applies at once" : "next game start") + "]";
        if (d.label != d.key) line += "  " + d.label;
        if (!d.group.empty()) line += "  (" + d.group + ")";
        command_reply_line(line);
        if (!d.help.empty()) command_reply_line("    " + d.help);
        if (!d.when_key.empty()) {
            const bool on = setting_active(d, [&](const std::string& k) { return c.settings->get(id, k); });
            command_reply_line("    " + setting_condition_text(d, defs) + (on ? "" : " (greyed out in the Mods tab now)"));
        }
    }
}

void cmd_set(EmfWorld*, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    if (argc < 4 || !c.settings) {
        command_reply_line("usage: set <mod> <key> <value>   (settings <mod> lists the keys)");
        return;
    }
    const std::string id = argv[1], key = argv[2];
    std::string value = argv[3];
    for (int i = 4; i < argc; ++i) value += std::string(" ") + argv[i];
    if (key == "enabled") {
        const std::string l = lower(value);
        const bool on = l == "1" || l == "true" || l == "on" || l == "yes";
        const bool off = l == "0" || l == "false" || l == "off" || l == "no";
        if ((!on && !off) || !(c.registry && c.registry->find(id))) {
            command_reply_line(!(c.registry && c.registry->find(id)) ? fmt("no mod \"%s\"", id.c_str())
                                                                     : "use 1 or 0 for enabled");
            return;
        }
        command_reply_line(c.settings->set_enabled(id, on)
                               ? fmt("%s is switched %s from the next game start", id.c_str(), on ? "on" : "off")
                               : fmt("could not write emf\\config\\%s.ini", id.c_str()));
        return;
    }
    std::string why;
    const auto r = c.settings->set(id, key, value, &why);
    if (r == SettingsStore::SetResult::Ok || r == SettingsStore::SetResult::Unchanged) {
        for (const auto& d : c.settings->defs(id))
            if (d.key == key) {
                command_reply_line(id + "." + key + " = " + shown_value(d, c.settings->get(id, key)) +
                                   (d.live ? "" : " (applies at the next game start)"));
                break;
            }
        return;
    }
    command_reply_line("refused: " + why);
}

void cmd_items(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    const std::string filter = argc > 1 ? lower(argv[1]) : std::string();
    struct Acc {
        std::string filter;
        size_t total = 0, shown = 0, matched = 0;
    } acc{filter};
    c.api->for_each_item(w, [](const EmfItemInfo* i, void* p) {
        auto* a = static_cast<Acc*>(p);
        ++a->total;
        if (!a->filter.empty() && lower(i->name).find(a->filter) == std::string::npos) return;
        ++a->matched;
        if (a->shown++ < 50) command_reply_line(fmt("%s (0x%08x, stack %u)", i->name, i->item_id, i->max_stack));
    }, &acc);
    command_reply_line(fmt("%zu of %zu items%s", acc.matched, acc.total, acc.matched > 50 ? " (first 50 shown)" : ""));
}

void cmd_give(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t count = 1;
    if (argc < 2 || argc > 3 || (argc == 3 && (!parse_uint(argv[2], 65535, &count) || count == 0))) {
        command_reply_line("usage: give <item name> [count]");
        return;
    }
    const uint32_t id = c.api->find_item(w, argv[1]);
    if (!id) {
        command_reply_line(fmt("unknown item '%s' (items <part of a name> lists names)", argv[1]));
        return;
    }
    EmfItemInfo info{};
    info.size = sizeof(info);
    c.api->item_info(w, id, &info);
    const uint32_t player = c.player ? c.player() : 0;
    const uint32_t pickup = c.api->give_item(w, player, id, count);
    command_reply_line(pickup ? fmt("gave %u x %s to player %u", count, info.name, player)
                              : std::string("give failed (see the log)"));
}

// put's and take's result arrives after the next server step, when the command has returned: it goes to the reply the
// command had (the console, or the chat player who typed it), kept in this per-request record.
struct LateItems {
    BuiltinContext* c;
    ReplyFn reply;
};

void items_done(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t n, void* user, bool take) {
    const std::unique_ptr<LateItems> late(static_cast<LateItems*>(user));  // each accepted request ends exactly once
    if (!late->reply) return;
    EmfItemInfo info{};
    info.size = sizeof(info);
    const bool named = late->c->api->item_info(w, item, &info) && info.name[0];
    const std::string why = InventoryService::delivering_reason();
    late->reply(fmt(take ? "take %u x %s from entity %u: %u taken%s" : "put %u x %s into entity %u: %u added%s",
                    requested, named ? info.name : fmt("0x%08x", item).c_str(), entity, n,
                    why.empty() ? "" : (" (" + why + ")").c_str()));
}
void put_done(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t added, void* user) {
    items_done(w, entity, item, requested, added, user, false);
}
void take_done(EmfWorld* w, uint32_t entity, uint32_t item, uint32_t requested, uint32_t taken, void* user) {
    items_done(w, entity, item, requested, taken, user, true);
}

// put <item> [count] [entity] [level] [rarity] / take <item> [count] [entity]
void cmd_items(EmfWorld* w, int argc, const char* const* argv, void* u, bool take) {
    auto& c = ctx_of(u);
    uint32_t count = 1, entity = 0, level = 0, rarity = 0xFF;
    const int max_args = take ? 4 : 6;
    bool ok = argc >= 2 && argc <= max_args;
    if (ok && argc >= 3) ok = parse_uint(argv[2], 65535, &count) && count > 0;
    if (ok && argc >= 4) ok = parse_uint(argv[3], 0xffffffffu, &entity) && entity > 0;
    if (ok && argc >= 5) ok = parse_uint(argv[4], 1000, &level);
    if (ok && argc >= 6) ok = parse_uint(argv[5], 4, &rarity);
    if (!ok) {
        command_reply_line(take ? "usage: take <item name> [count 1-65535] [entity]"
                                : "usage: put <item name> [count 1-65535] [entity] [level] [rarity 0-4]");
        return;
    }
    const uint32_t id = c.api->find_item(w, argv[1]);
    if (!id) {
        command_reply_line(fmt("unknown item '%s' (items <part of a name> lists names)", argv[1]));
        return;
    }
    if (!entity) entity = c.player ? c.player() : 0;
    auto* late = new LateItems{&c, current_reply()};  // freed by the result callback, or below when refused
    bool queued = false;
    if (!take && argc < 5) {
        queued = c.api->put_items(w, entity, id, count, put_done, late);
    } else {
        EmfItemsRequest r{};
        r.size = sizeof(r);
        r.entity = entity;
        r.item = id;
        r.count = count;
        r.level = level;
        if (rarity != 0xFF) {
            r.flags = EMF_ITEMS_RARITY;
            r.rarity = static_cast<uint8_t>(rarity);
        }
        r.cb = take ? take_done : put_done;
        r.user = late;
        queued = take ? c.api->take_items(w, &r) : c.api->put_items_ex(w, &r);
    }
    if (!queued) {
        delete late;  // a refused request never calls back
        command_reply_line(take ? "take refused (see the log)" : "put refused (see the log)");
    }
}
void cmd_put(EmfWorld* w, int argc, const char* const* argv, void* u) { cmd_items(w, argc, argv, u, false); }
void cmd_take(EmfWorld* w, int argc, const char* const* argv, void* u) { cmd_items(w, argc, argv, u, true); }

void cmd_spawn(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint8_t guid[16];
    uint32_t count = 1;
    if (argc < 2 || argc > 3 || !emf::parse_guid(argv[1], guid) ||
        (argc == 3 && (!parse_uint(argv[2], 20, &count) || count == 0))) {
        command_reply_line("usage: spawn <template guid> [count 1-20]");
        return;
    }
    const uint32_t player = c.player ? c.player() : 0;
    double p[3];
    if (!player_pos(c, w, player, p)) {
        command_reply_line("spawn: player position unknown");
        return;
    }
    std::string ids;
    uint32_t made = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = c.api->spawn(w, guid, p[0] + 2.0 + 1.5 * i, p[1], p[2]);
        if (!id) continue;
        ++made;
        ids += (ids.empty() ? "" : " ") + std::to_string(id);
    }
    command_reply_line(fmt("spawned %u: %s", made, ids.empty() ? "(none, see the log)" : ids.c_str()));
}

void cmd_time(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    double hours = 0;
    if (argc != 2 || !parse_hours(argv[1], &hours)) {
        command_reply_line("usage: time <hours 0-24>");
        return;
    }
    auto* daytime = static_cast<uint8_t*>(c.api->singleton(w, "g38_daytime::Daytime"));
    auto* weather = static_cast<uint8_t*>(c.api->singleton(w, "WeatherSystem"));
    auto fn = reinterpret_cast<SetTimeOfDayFn>(c.api->resolve("SetTimeOfDay"));
    if (!daytime || !fn) {
        command_reply_line("time: the daytime system or setTimeOfDay is not available here");
        return;
    }
    const CoreState* st = core_state();
    if (st && !st->tick_offsets_ok) {  // setTimeOfDay needs the world's time
        command_reply_line("time: the world's tick fields are not confirmed on this game build");
        return;
    }
    const bool known = st && st->known_build;
    const auto r = set_time_of_day(daytime, weather, fn, c.api->ecs_time_ns(w), hours, known);
    if (!r.ok) {
        command_reply_line("time: setTimeOfDay faulted");
        return;
    }
    command_reply_line(fmt("time of day is now %.2f h (was %.2f h)%s", r.new_tod / 3600e9, r.old_tod / 3600e9,
                           known ? "" : "; weather not reset (a game build EMF does not know)"));
}

void cmd_pos(EmfWorld* w, int, const char* const*, void* u) {
    auto& c = ctx_of(u);
    const uint32_t player = c.player ? c.player() : 0;
    double p[3];
    if (!player_pos(c, w, player, p)) {
        command_reply_line("pos: player position unknown");
        return;
    }
    command_reply_line(fmt("player %u at (%.1f, %.1f, %.1f) m", player, p[0], p[1], p[2]));
}

// goto [#k] <x> <y> <z> [ground] | goto [#k] <entity> [ground]: teleports you (the chat sender, else the local player;
// #k = the k-th player, lowest entity id first) to a position, or 1.5 m beside an entity (API v20 teleport). The result
// comes one or more server steps later, to the same reply.
struct LateGoto {
    ReplyFn reply;
};
void goto_done(EmfWorld*, const EmfTeleportResult* r, void* user) {
    auto* late = static_cast<LateGoto*>(user);
    if (late->reply) {
        if (r->status == EMF_TELEPORT_FAILED)
            late->reply(fmt("goto: entity %u not moved: %s", r->entity, r->reason ? r->reason : "?"));
        else
            late->reply(fmt("goto: entity %u now at (%.1f, %.1f, %.1f), %.1f m from the target%s", r->entity, r->x, r->y,
                            r->z, r->distance,
                            (r->flags & EMF_TELEPORT_RAW_TARGET)   ? " (no ground found near it: exactly there)"
                            : (r->flags & EMF_TELEPORT_OFF_TARGET) ? " (the ground search moved it)"
                                                                   : ""));
    }
    if (r->flags & EMF_TELEPORT_LAST) delete late;
}

bool parse_coord(const char* s, double* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (!end || *end || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

void cmd_goto(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    std::vector<std::string> words(argv + 1, argv + argc);
    uint32_t k = 0;
    if (!words.empty() && words[0].size() > 1 && words[0][0] == '#') {
        if (!parse_uint(words[0].c_str() + 1, 1023, &k) || k == 0) words.clear();  // a bad #k: usage
        else words.erase(words.begin());
    }
    bool ground = false;
    if (!words.empty() && lower(words.back()) == "ground") {
        ground = true;
        words.pop_back();
    }
    double v[3] = {};
    bool ok = words.size() == 1 || words.size() == 3;
    for (size_t i = 0; ok && i < words.size(); ++i) ok = parse_coord(words[i].c_str(), &v[i]);
    if (!ok) {
        command_reply_line("usage: goto [#k] <x> <y> <z> [ground] | goto [#k] <entity> [ground]");
        return;
    }
    if (c.api->size < offsetof(EmfApi, teleport) + sizeof(c.api->teleport) || !c.api->teleport) {
        command_reply_line("goto: this core has no teleport");
        return;
    }
    uint32_t player = 0;
    if (k) {
        uint32_t seen = 0;
        for (uint32_t id = 1; id <= 1023 && !player; ++id)
            if (comp_of<emf::gen::ecs::PlayerState>(c, w, id) && ++seen == k) player = id;
        if (!player) {
            command_reply_line(fmt("goto: there is no player #%u", k));
            return;
        }
    } else {
        player = c.player ? c.player() : 0;
        if (!player) {
            command_reply_line("goto: no player to move (on a dedicated server's console name one: goto #1 ...)");
            return;
        }
    }
    double to[3] = {v[0], v[1], v[2]};
    if (words.size() == 1) {
        const double e = v[0];
        if (e < 1 || e > 4294967295.0 || e != std::floor(e)) {
            command_reply_line("goto: an entity is a whole number");
            return;
        }
        if (!player_pos(c, w, static_cast<uint32_t>(e), to)) {
            command_reply_line(fmt("goto: entity %u has no position", static_cast<uint32_t>(e)));
            return;
        }
        to[0] += 1.5;  // beside it, not inside it
    }
    EmfTeleport t{};
    t.size = sizeof t;
    t.entity = player;
    t.x = to[0];
    t.y = to[1];
    t.z = to[2];
    t.flags = ground ? EMF_TELEPORT_FIND_GROUND : 0u;
    t.cb = goto_done;
    t.user = new LateGoto{current_reply()};
    if (!c.api->teleport(w, &t)) {
        delete static_cast<LateGoto*>(t.user);  // a refused request never calls back
        command_reply_line("goto: refused (see the log)");
        return;
    }
    command_reply_line(fmt("goto: moving player %u to (%.1f, %.1f, %.1f)%s", player, to[0], to[1], to[2],
                           ground ? " with the ground search" : ""));
}

// "keen::ecs::ActiveNpcState" -> "ActiveNpcState"
const char* short_name(const char* qualified) {
    const char* p = std::strrchr(qualified, ':');
    return p ? p + 1 : qualified;
}

void cmd_inspect(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t id = 0;
    if (argc != 2 || !parse_uint(argv[1], 0xffffffffu, &id)) {
        command_reply_line("usage: inspect <entity>");
        return;
    }
    size_t total = 0, templates = 0;
    auto dynamic = [&](const emf::gen::ComponentInfo& ci) {
        uint64_t size = 0;
        if (!c.api->get_component(w, ci.hash, id, &size)) return;
        ++total;
        command_reply_line(fmt("%s (%llu B)", short_name(ci.name), static_cast<unsigned long long>(size)));
    };
    for (const auto& ci : emf::gen::kComponents) {
        dynamic(ci);
        uint64_t tsize = 0;
        if (c.api->get_template_component && c.api->get_template_component(w, ci.hash, id, &tsize)) {
            ++total;
            ++templates;
            command_reply_line(tsize ? fmt("%s (template, %llu B)", short_name(ci.name), static_cast<unsigned long long>(tsize))
                                     : fmt("%s (template, size unknown)", short_name(ci.name)));
        }
    }
    for (const auto& ci : emf::gen::kDynamicComponents) dynamic(ci);
    command_reply_line(total ? fmt("entity %u: %zu components (%zu template)", id, total, templates)
                             : fmt("entity %u: no components (no such entity?)", id));
}

const emf::gen::ComponentInfo* find_component(const char* name) {
    for (const auto& ci : emf::gen::kComponents)
        if (lower(short_name(ci.name)) == lower(name)) return &ci;
    return nullptr;
}

bool parse_offset(const char* s, uint32_t* out) {
    if (!s || !*s || *s == '-') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 0);  // 0x.. or decimal
    if (!end || *end || v > 0xffffu) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

// The entity's template data for a component, with the reply for each way it can be missing. Null if missing.
const emf::gen::ComponentInfo* template_target(BuiltinContext& c, EmfWorld* w, uint32_t id, const char* name,
                                               uint8_t** data) {
    const emf::gen::ComponentInfo* info = find_component(name);
    if (!info) {
        command_reply_line(fmt("unknown component '%s'", name));
        return nullptr;
    }
    uint64_t game_size = 0;
    *data = c.api->get_template_component
                ? static_cast<uint8_t*>(c.api->get_template_component(w, info->hash, id, &game_size))
                : nullptr;
    if (!*data) {
        command_reply_line(fmt("entity %u has no template data for %s", id, short_name(info->name)));
        return nullptr;
    }
    if (game_size && game_size != info->size) {
        command_reply_line(fmt("%s is %llu B in this game but %u B in EMF's headers (a game update changed it): refused",
                               short_name(info->name), static_cast<unsigned long long>(game_size), info->size));
        return nullptr;
    }
    return info;
}

void cmd_tpl(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t id = 0, bytes = 32, offset = 0;
    if (argc < 3 || argc > 5 || !parse_uint(argv[1], 0xffffffffu, &id) ||
        (argc >= 4 && (!parse_uint(argv[3], 256, &bytes) || bytes == 0)) || (argc == 5 && !parse_offset(argv[4], &offset))) {
        command_reply_line("usage: tpl <entity> <component> [bytes 1-256] [offset]");
        return;
    }
    uint8_t* p = nullptr;
    const auto* info = template_target(c, w, id, argv[2], &p);
    if (!info) return;
    if (offset >= info->size) {
        command_reply_line(fmt("offset out of range (%s is %u B)", short_name(info->name), info->size));
        return;
    }
    if (offset + bytes > info->size) bytes = info->size - offset;
    command_reply_line(fmt("%s of entity %u at %p (%u of %u B from +0x%03x):", short_name(info->name), id,
                           static_cast<const void*>(p), bytes, info->size, offset));
    for (uint32_t row = offset; row < offset + bytes; row += 16) {
        std::string line = fmt("+0x%03x:", row);
        for (uint32_t i = row; i < row + 16 && i < offset + bytes; ++i) {
            uint8_t b = 0;
            line += emf::seh_read(p + i, &b, 1) ? fmt(" %02x", b) : std::string(" ??");
        }
        command_reply_line(line);
    }
}

// One value of a tplset type, as raw bytes and as text.
struct Scalar {
    uint8_t bytes[4] = {};
    uint32_t size = 0;
};
bool parse_scalar(const std::string& type, const char* text, Scalar* out) {
    char* end = nullptr;
    if (type == "f32") {
        const float f = std::strtof(text, &end);
        if (!end || *end || !std::isfinite(f)) return false;
        std::memcpy(out->bytes, &f, 4);
        out->size = 4;
        return true;
    }
    if (type == "i32") {
        const long long v = std::strtoll(text, &end, 0);
        if (!end || *end || v < INT32_MIN || v > INT32_MAX) return false;
        const int32_t x = static_cast<int32_t>(v);
        std::memcpy(out->bytes, &x, 4);
        out->size = 4;
        return true;
    }
    const uint64_t limit = type == "u32" ? 0xffffffffull : type == "u16" ? 0xffffull : type == "u8" ? 0xffull : 0;
    if (!limit || *text == '-') return false;
    const unsigned long long v = std::strtoull(text, &end, 0);
    if (!end || *end || v > limit) return false;
    const uint32_t x = static_cast<uint32_t>(v);
    out->size = type == "u32" ? 4 : type == "u16" ? 2 : 1;
    std::memcpy(out->bytes, &x, out->size);
    return true;
}
std::string scalar_text(const std::string& type, const uint8_t* b) {
    if (type == "f32") {
        float f;
        std::memcpy(&f, b, 4);
        return fmt("%g", f);
    }
    if (type == "i32") {
        int32_t v;
        std::memcpy(&v, b, 4);
        return fmt("%d", v);
    }
    uint32_t v = 0;
    std::memcpy(&v, b, type == "u32" ? 4 : type == "u16" ? 2 : 1);
    return fmt("%u", v);
}

void cmd_tplset(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t id = 0, offset = 0;
    const std::string type = argc == 6 ? lower(argv[4]) : std::string();
    const bool type_ok = type == "f32" || type == "i32" || type == "u32" || type == "u16" || type == "u8";
    if (argc != 6 || !parse_uint(argv[1], 0xffffffffu, &id) || !parse_offset(argv[3], &offset) || !type_ok) {
        command_reply_line("usage: tplset <entity> <component> <offset> <f32|i32|u32|u16|u8> <value>");
        return;
    }
    Scalar value;
    if (!parse_scalar(type, argv[5], &value)) {
        command_reply_line(fmt("bad value '%s' for %s", argv[5], type.c_str()));
        return;
    }
    uint8_t* p = nullptr;
    const auto* info = template_target(c, w, id, argv[2], &p);
    if (!info) return;
    if (offset + value.size > info->size) {
        command_reply_line(fmt("offset out of range (%s is %u B)", short_name(info->name), info->size));
        return;
    }
    uint8_t before[4] = {}, after[4] = {};
    emf::seh_read(p + offset, before, value.size);
    if (!c.api->write_memory || !c.api->write_memory(p + offset, value.bytes, value.size)) {
        command_reply_line("could not write the template data (see the log)");
        return;
    }
    emf::seh_read(p + offset, after, value.size);
    command_reply_line(fmt("%s+0x%03x of entity %u: %s %s -> %s (every entity of this template, until the game restarts)",
                           short_name(info->name), offset, id, type.c_str(), scalar_text(type, before).c_str(),
                           scalar_text(type, after).c_str()));
}

void cmd_stack(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t max = 0;
    if (argc != 3 || !parse_uint(argv[2], 65535, &max) || max == 0) {
        command_reply_line("usage: stack <item name> <max 1-65535>");
        return;
    }
    const uint32_t id = c.api->find_item(w, argv[1]);
    if (!id) {
        command_reply_line(fmt("unknown item '%s' (items <part of a name> lists names)", argv[1]));
        return;
    }
    EmfItemInfo info{};
    info.size = sizeof(info);
    c.api->item_info(w, id, &info);
    const unsigned before = info.max_stack;
    if (!c.api->set_item_max_stack || !c.api->set_item_max_stack(id, static_cast<uint16_t>(max))) {
        command_reply_line(fmt("%s: could not change the stack size (see the log)", info.name));
        return;
    }
    command_reply_line(fmt("%s: stack %u -> %u (until the game restarts)", info.name, before, max));
}

void cmd_entities(EmfWorld* w, int, const char* const*, void* u) {
    size_t n = 0;
    ctx_of(u).api->for_each_entity(w, [](uint32_t, void* p) { ++*static_cast<size_t*>(p); }, &n);
    command_reply_line(fmt("%zu entities in this world", n));
}

// Removes one entity (host). Refuses players (the local one, or anything with PlayerState) and ids that are not in the
// world, so a typo cannot delete a player or poke the game with a stale id.
void cmd_destroy(EmfWorld* w, int argc, const char* const* argv, void* u) {
    auto& c = ctx_of(u);
    uint32_t id = 0;
    if (argc != 2 || !parse_uint(argv[1], 0xffffffffu, &id) || id == 0) {
        command_reply_line("usage: destroy <entity>");
        return;
    }
    struct Find {
        uint32_t id;
        bool found;
    } find{id, false};
    c.api->for_each_entity(w, [](uint32_t e, void* p) {
        auto* f = static_cast<Find*>(p);
        f->found |= e == f->id;
    }, &find);
    if (!find.found) {
        command_reply_line(fmt("no entity %u in this world", id));
        return;
    }
    uint64_t size = 0;
    const uint32_t player = c.player ? c.player() : 0;
    if (id == player || c.api->get_component(w, emf::gen::ecs::PlayerState::kHash, id, &size)) {
        command_reply_line(fmt("entity %u is a player: not destroyed", id));
        return;
    }
    command_reply_line(c.api->destroy(w, id) ? fmt("destroyed entity %u", id)
                                             : fmt("destroy %u failed (see the log)", id));
}

}  // namespace

bool add_builtin_commands(BuiltinContext& ctx) {
    if (!ctx.api || !ctx.center) return false;
    struct Def {
        const char* name;
        const char* help;
        CommandFn fn;
    };
    const Def defs[] = {
        {"help", "[page]: list the commands (in the chat 8 per page)", cmd_help},
        {"mods", ": list the mods and what became of them", cmd_mods},
        {"settings", "<mod>: list a mod's settings", cmd_settings},
        {"hooks", ": every inline hook, its owner and target (EMF's and mods')", cmd_hooks},
        {"pattern", "<bytes> [.section]: test a byte pattern against the game exe (found once?)", cmd_pattern},
        {"set", "<mod> <key> <value>: change a mod's setting (saved in emf/config/<mod>.ini)", cmd_set},
        {"items", "[part of a name]: list the game's items (debug names)", cmd_items},
        {"give", "<item name> [count]: drop items at your feet (host)", cmd_give},
        {"put", "<item name> [count 1-65535] [entity] [level] [rarity 0-4]: put items straight into an inventory, yours by "
                "default (host)",
         cmd_put},
        {"take", "<item name> [count 1-65535] [entity]: take items out of an inventory, yours by default (host)", cmd_take},
        {"spawn", "<template guid> [count 1-20]: spawn templates next to you (host)", cmd_spawn},
        {"time", "<hours 0-24>: set the time of day (host)", cmd_time},
        {"pos", ": your position", cmd_pos},
        {"goto", "[#k] <x> <y> <z> | <entity> [ground] (host)",
         cmd_goto},
        {"near", "[radius 1-200]: the entities around you, nearest first, and what they are (host)", cmd_near},
        {"hud", "<id> [place 0-2] [text]: keep a line on screen (0 top right, 1 bottom right, 2 bottom left); no text removes it", cmd_hud},
        {"inv", "<entity>: every stack in the entity's inventories, with slot ids and gear levels (host)", cmd_inv},
        {"entities", ": count the entities in the server world", cmd_entities},
        {"inspect", "<entity>: list the entity's components (template ones marked)", cmd_inspect},
        {"tpl", "<entity> <component> [bytes 1-256] [offset]: hex dump of the entity's template data", cmd_tpl},
        {"tplset", "<entity> <component> <offset> <f32|i32|u32|u16|u8> <value>: change one template value (host)",
         cmd_tplset},
        {"stack", "<item name> <max 1-65535>: change an item's stack size until the game restarts (host)", cmd_stack},
        {"destroy", "<entity>: remove an entity from the world, never a player (host)", cmd_destroy},
    };
    bool ok = true;
    for (const auto& d : defs) {
        const bool anywhere = std::strcmp(d.name, "help") == 0 || std::strcmp(d.name, "mods") == 0 ||
                              std::strcmp(d.name, "settings") == 0 || std::strcmp(d.name, "set") == 0 ||
                              std::strcmp(d.name, "hooks") == 0 || std::strcmp(d.name, "pattern") == 0;  // need no world
        // help is for every player (it lists only what they may run)
        const uint32_t flags = std::strcmp(d.name, "help") == 0 ? kCommandForPlayers : 0u;
        ok &= ctx.center->add(d.name, d.help, d.fn, &ctx, nullptr, anywhere, flags);
    }
    return ok;
}

}  // namespace emf::core
