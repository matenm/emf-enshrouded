// waypoints: an EMF example mod in C++. Name the places you find and let a line on screen guide you back:
//   wp.set <name>   remember where you stand          wp.list         every waypoint and how far it is
//   wp.go <name>    guide me there (wp.go: stop)      wp.del <name>   forget one
// It teaches commands with arguments and a per-world save (emf::WorldFile): each world has its own waypoints, kept in
// <game>\emf\config\waypoints\<world slot>.txt. The book itself (names, the file's text, the guiding line) is in
// waypoint_book.h, which has no game in it and is unit-tested.
#include <windows.h>  // GetTickCount64

#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "emf/aim.hpp"
#include "emf/emf.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "waypoint_book.h"

namespace {
constexpr const char* kMod = "waypoints";
constexpr const char* kLine = "waypoints.guide";  // the HUD line's id (ids start with the mod's id)

// Used on the server world's thread only: its on_tick, and the commands, which run there after its step.
emf::WorldFile g_file(kMod);  // this world's file: read once per world load, saved only into that world's file
waypoints::Book g_book;       // the waypoints of the world being played
std::string g_target;         // the waypoint wp.go guides to ("" = none)
std::string g_shown;          // the HUD line now: sent again only when it changes

// Touched from other threads too.
std::atomic<bool> g_restyle{false};     // a setting changed: send the line again in its new corner and colour
std::atomic<uint64_t> g_server_ms{0};   // when the server world last stepped
std::atomic<bool> g_line_on{false};     // the line is on screen

waypoints::Vec vec(const emf::aim::Vec3& v) { return {v.x, v.y, v.z}; }

// Where the local player (the host) stands.
std::optional<waypoints::Vec> my_position(emf::World w) {
    const auto p = emf::position(w, emf::local_player());
    if (!p) return std::nullopt;
    return vec(*p);
}

// The waypoints are the host's: the book uses the host's position and the line is on the host's screen. The console
// and the host's own chat report from_chat 0; another player typing in the chat (an admin may run any command) 1.
bool host_only() {
    EmfCommandSender who{};
    who.size = sizeof who;
    if (!emf::has_v15() || !emf::command_sender(&who) || !who.from_chat) return false;
    EMF_REPLY("waypoints are the host's own: their position, their screen");
    return true;
}

uint32_t corner() {  // the "corner" setting is a select: its option's value
    const std::string c = emf::setting_text(kMod, "corner", "top_right");
    return c == "bottom_right" ? EMF_HUD_BOTTOM_RIGHT : c == "bottom_left" ? EMF_HUD_BOTTOM_LEFT : EMF_HUD_TOP_RIGHT;
}

void show(const std::string& text) {
    if (g_restyle.exchange(false)) g_shown = "\n";  // something no line is: send it again
    if (text == g_shown) return;
    g_shown = text;
    g_line_on = !text.empty();
    emf::hud(kLine, text, corner(), emf::setting_rgba(kMod, "color", 0x80FFC0FFu));  // "" removes the line
}

// Ten times a second: how far the target is and which way, from where the camera looks.
void guide(emf::World w) {
    const waypoints::Waypoint* target = g_target.empty() ? nullptr : g_book.find(g_target);
    const auto me = my_position(w);
    if (!target || !me) {
        show("");
        return;
    }
    if (waypoints::arrived(*me, target->pos)) {
        EMF_NOTIFY("Arrived at %s", target->name.c_str());  // a name is plain text: never the format itself
        g_target.clear();
        show("");
        return;
    }
    const auto ray = emf::aim::camera_ray(w, emf::local_player());  // the host's camera, from the server world
    const waypoints::Vec forward = ray ? vec(ray->dir) : waypoints::Vec{};
    show(waypoints::guide_text(target->name, *me, ray ? &forward : nullptr, target->pos));
}

void save_book() {
    if (!g_file.save(g_book.to_text())) EMF_REPLY("(not saved: this world's file is not read yet)");
}

double flat_distance(const waypoints::Vec& a, const waypoints::Vec& b) { return std::hypot(a.x - b.x, a.z - b.z); }
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;  // the core is too old: the log says why
    if (!emf::has_v13()) {
        EMF_LOG(kMod, "needs EMF API v13 or newer");
        return false;
    }
    g_file.attach();  // listens for the host's world loads

    emf::on_tick([](emf::World w) {
        if (w.is_client()) {  // in someone else's game no server world steps: take the line off our screen
            if (g_line_on && GetTickCount64() - g_server_ms > 3000) {
                emf::hud(kLine, "");
                g_line_on = false;
                g_restyle = true;  // sent again if we host again
            }
            return;
        }
        if (!w.is_server()) return;
        g_server_ms = GetTickCount64();
        // Once per world load: that world's waypoints ("" the first time). A new world forgets the old target.
        g_file.poll([](const std::string& text) {
            g_book = waypoints::Book::from_text(text);
            g_target.clear();
        });
        if (w.tick() % 6 == 0) guide(w);
    });
    emf::on_setting_changed(kMod, [](emf::World, const std::string&) { g_restyle = true; });

    // args[0] is the command's name, args[1] the first word after it.
    emf::add_command("wp.set", "<name>: remember where you stand", [](emf::World w, const std::vector<std::string>& args) {
        if (host_only()) return;
        const auto me = my_position(w);
        if (args.size() != 2) {
            EMF_REPLY("usage: wp.set <name>   (one word: letters, digits, _ or -)");
        } else if (!me) {
            EMF_REPLY("your position is not known yet");
        } else {
            const auto result = g_book.set(args[1], *me);
            if (result == waypoints::Book::Set::BadName) {
                EMF_REPLY("a name is 1-24 letters, digits, _ or -");
            } else if (result == waypoints::Book::Set::Full) {
                EMF_REPLY("this world has %zu waypoints already: wp.del one first", waypoints::Book::kMax);
            } else {
                EMF_REPLY("%s %s %.0f, %.0f, %.0f", args[1].c_str(),
                          result == waypoints::Book::Set::Moved ? "moved to" : "set at", me->x, me->y, me->z);
                save_book();
            }
        }
    });

    emf::add_command("wp.go", "[name]: a line on screen guides you there (no name: stop)",
                     [](emf::World w, const std::vector<std::string>& args) {
                         if (host_only()) return;
                         const waypoints::Waypoint* target = args.size() >= 2 ? g_book.find(args[1]) : nullptr;
                         const auto me = my_position(w);
                         if (args.size() < 2) {
                             g_target.clear();
                             EMF_REPLY("guide off");
                         } else if (!target) {
                             EMF_REPLY("no waypoint %s (wp.list shows them)", args[1].c_str());
                         } else {
                             g_target = target->name;
                             EMF_REPLY("guiding you to %s: %.0f m", target->name.c_str(),
                                       me ? flat_distance(*me, target->pos) : 0.0);
                         }
                     });

    emf::add_command("wp.list", ": every waypoint of this world and how far it is",
                     [](emf::World w, const std::vector<std::string>&) {
                         if (host_only()) return;
                         const auto me = my_position(w);
                         if (g_book.all().empty()) EMF_REPLY("no waypoints in this world yet (wp.set <name>)");
                         else EMF_REPLY("%zu waypoint(s):", g_book.all().size());
                         for (const auto& wp : g_book.all()) {
                             const char* guiding = waypoints::same_name(wp.name, g_target) ? " (guiding)" : "";
                             if (me) EMF_REPLY("  %s: %.0f m%s", wp.name.c_str(), flat_distance(*me, wp.pos), guiding);
                             else EMF_REPLY("  %s at %.0f, %.0f, %.0f%s", wp.name.c_str(), wp.pos.x, wp.pos.y, wp.pos.z, guiding);
                         }
                     });

    emf::add_command("wp.del", "<name>: forget a waypoint", [](emf::World, const std::vector<std::string>& args) {
        if (host_only()) return;
        if (args.size() != 2) {
            EMF_REPLY("usage: wp.del <name>");
        } else if (!g_book.erase(args[1])) {
            EMF_REPLY("no waypoint %s", args[1].c_str());
        } else {
            if (waypoints::same_name(g_target, args[1])) g_target.clear();
            EMF_REPLY("%s deleted", args[1].c_str());
            save_book();
        }
    });

    EMF_LOG(kMod, "loaded");
    return true;
}
