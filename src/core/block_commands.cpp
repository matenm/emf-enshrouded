// Breaking blocks: the console commands (block_commands.h).
#include "core/block_commands.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace emf::core {

namespace {

BlockCommandDeps g_deps;

const char* hardness_name(uint32_t h) {
    static const char* const kNames[] = {"Soft", "SlightlyHard", "ModeratelyHard", "Hard", "VeryHard", "Unbreakable"};
    return h < 6 ? kNames[h] : "?";
}

std::string at_text(float x, float y, float z) {
    char b[64];
    std::snprintf(b, sizeof b, "at %.2f %.2f %.2f", x, y, z);
    return b;
}

// The first building block where the player looks, within kBlockLookRange; else why not (empty text: none there).
bool looked_at(EmfWorld* w, EmfBlockInfo* out, float ray_end[3], std::string* why) {
    float o[3], d[3];
    if (!g_deps.camera || !g_deps.camera(w, o, d)) {
        *why = "no camera: look from a player (from the chat: yours; on a dedicated server's console there is none)";
        return false;
    }
    for (int i = 0; i < 3; ++i) ray_end[i] = o[i] + d[i] * kBlockLookRange;
    *out = EmfBlockInfo{};
    out->size = sizeof *out;
    why->clear();
    return g_deps.find_wall && g_deps.find_wall(w, o, ray_end, out);
}

void cmd_blocks(EmfWorld* w, int, const char* const*, void*) {
    EmfBlockInfo b{};
    float end[3];
    std::string why;
    if (looked_at(w, &b, end, &why)) {
        command_reply_line(block_info_line(b));
        return;
    }
    if (!why.empty()) {
        command_reply_line(why);
        return;
    }
    EmfBlockInfo e{};
    e.size = sizeof e;
    std::string tail;
    if (g_deps.block_at && g_deps.block_at(w, end[0], end[1], end[2], &e)) tail = " (the ray's end: " + block_info_line(e) + ")";
    command_reply_line("no building block within 12 m where you look" + tail);
}

struct BreakReply {
    ReplyFn reply;
};
void on_break(EmfWorld*, uint32_t id, const EmfBreakResult* r, void* user) {
    const std::unique_ptr<BreakReply> br(static_cast<BreakReply*>(user));
    if (!br->reply || !r) return;
    char b[160];
    if (r->status == EMF_BREAK_DONE)
        std::snprintf(b, sizeof b, "break %u: damaged %u, removed %u, dropped %u", id, r->damaged, r->removed, r->dropped);
    else
        std::snprintf(b, sizeof b, "break %u: %s", id,
                      r->status == EMF_BREAK_CANCELLED ? "cancelled (the world ended first)" : "failed (breaking is off now)");
    br->reply(b);
}

bool parse_float(const char* s, float* out) {
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (!end || end == s || *end) return false;
    *out = v;
    return true;
}

void cmd_break(EmfWorld* w, int argc, const char* const* argv, void*) {
    EmfBreakBlocks r{};
    r.size = sizeof r;
    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        if (std::strcmp(argv[i], "instant") == 0)
            r.flags |= EMF_BREAK_INSTANT;
        else if (std::strcmp(argv[i], "drops") == 0)
            r.flags |= EMF_BREAK_DROPS;
        else if (std::strcmp(argv[i], "height") == 0)
            ok = i + 1 < argc && parse_float(argv[++i], &r.height);
        else
            ok = i == 1 && parse_float(argv[i], &r.radius);
    }
    if (!ok) {
        command_reply_line("usage: break [radius 0.25-3.5] [instant] [drops] [height <m>]");
        return;
    }
    EmfBlockInfo b{};
    float end[3];
    std::string why;
    if (!looked_at(w, &b, end, &why)) {
        command_reply_line(why.empty() ? "no building block within 12 m where you look" : why);
        return;
    }
    r.x = b.x;
    r.y = b.y;
    r.z = b.z;
    auto* br = new BreakReply{current_reply()};
    const uint32_t id = g_deps.break_blocks ? g_deps.break_blocks(w, &r, on_break, br) : 0;
    if (!id) {
        delete br;
        command_reply_line(std::string("break refused: ") + (r.why[0] ? r.why : "not available"));
        return;
    }
    char line[200];
    std::snprintf(line, sizeof line, "break %u queued %s (radius %.2f m%s%s%s); the answer follows after the step", id,
                  at_text(r.x, r.y, r.z).c_str(), r.radius == 0.0f ? 1.0f : r.radius,
                  r.height > 0 ? (", a column " + std::to_string(r.height).substr(0, 4) + " m high").c_str() : "",
                  (r.flags & EMF_BREAK_INSTANT) ? ", instant" : "", (r.flags & EMF_BREAK_DROPS) ? ", drops" : "");
    command_reply_line(line);
}

}  // namespace

std::string block_info_line(const EmfBlockInfo& b) {
    char t[160];
    const std::string at = at_text(b.x, b.y, b.z);
    if (b.kind == EMF_BLOCK_BUILDING) {
        char dmg[24] = "";
        if (b.damage >= 0) std::snprintf(dmg, sizeof dmg, ", damage %d/255", b.damage);
        if (b.health > 0)
            std::snprintf(t, sizeof t, "building %u (%s, %d health%s) %s", b.material, hardness_name(b.hardness), b.health,
                          dmg, at.c_str());
        else
            std::snprintf(t, sizeof t, "building %u (no config%s) %s", b.material, dmg, at.c_str());
    } else if (b.kind == EMF_BLOCK_TERRAIN) {
        std::snprintf(t, sizeof t, "terrain %u %s", b.material, at.c_str());
    } else {
        std::snprintf(t, sizeof t, "nothing %s", at.c_str());
    }
    return t;
}

bool add_block_commands(CommandCenter& commands, BlockCommandDeps deps) {
    g_deps = std::move(deps);
    bool ok = commands.add("blocks", ": the player-built block where you look (material, hardness, health, damage so far) "
                                     "(host or dedicated server)",
                           cmd_blocks, nullptr, nullptr);
    ok &= commands.add("break", "[radius 0.25-3.5] [instant] [drops] [height <m>]: break the player-built blocks where "
                                "you look (never the ground; the hole stays and is saved; host or dedicated server)",
                       cmd_break, nullptr, nullptr);
    return ok;
}

}  // namespace emf::core
