#include "overlay.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <optional>
#include <set>

#include "core/emf_settings.h"
#include "core/game_screen.h"
#include "core/game_text.h"
#include "core/hud_editor/lines.h"
#include "core/screen_glyphs.h"
#include "console_ui.h"
#include "image_store.h"
#include "marker_ui.h"
#include "core/canvas.h"
#include "core/fog_grid.h"
#include "core/map_input.h"
#include "core/minimap_draw.h"
#include "window_model.h"
#include "cursor_model.h"
#include "emf/generated/components.hpp"
#include "extra_font.h"
#include "font_plan.h"
#include "input_hook.h"
#include "mods_ui.h"
#include "settings_ui.h"
#include "hud_page.h"
#include "submit.h"
#include "vk_overlay.h"
#include "wheel_gate.h"
#include "wheel_model.h"
#include "wheel_ui.h"
#include "window_gate.h"
#include "window_ui.h"

namespace emf::core {
namespace {

struct InputEvent {
    enum Kind { Key, Char, Wheel, Toggle, Mouse, FocusLost } kind;
    KeyEvent key{};
    wchar_t ch = 0;
    float wheel = 0.0f;
    bool open = false;
    float dx = 0.0f, dy = 0.0f;  // Mouse: raw movement while a wheel captures
    uint16_t buttons = 0;        // Mouse: RI_MOUSE_* flags
};

std::unique_ptr<ConsoleModel> g_model;
std::unique_ptr<InputRouter> g_router;
OverlayDeps g_deps;
ConsoleUiState g_ui;  // render thread
SRWLOCK g_queue_lock = SRWLOCK_INIT;
std::vector<InputEvent> g_queue;
CompletionSources g_sources;  // render thread
uint64_t g_items_refreshed = 0;
std::atomic<uint64_t> g_toast_from{0};   // model version when the toast started
std::atomic<uint64_t> g_toast_until{0};  // GetTickCount64 when it ends
std::wstring g_history_path;
SRWLOCK g_status_lock = SRWLOCK_INIT;
std::string g_status;   // overlay_set_status: the tab bar's status text
CursorModel g_cursor;   // render thread: the console's mouse cursor
ModsUiState g_mods_ui;  // render thread
SettingsUiState g_settings_ui;  // render thread: the Settings tab
HudPageState g_hud_page;        // render thread: the HUD page
std::string g_pending_page;     // under g_queue_lock: a Settings page to show (overlay_toggle_settings_page)
std::atomic<bool> g_page_showing{false};  // the HUD page is on screen (the render thread writes)
float g_mouse_speed = 1.0f;
// Mods' radial wheel: requested from any thread, opened and driven on the render thread.
WheelModel g_wheel;  // render thread
// The wheel's request state (any thread); input capture follows it.
WheelGate g_wheel_gate([](bool on) {
    if (g_router) g_router->set_wheel(on);
});
// A mod's window (API v24): the gate takes it from any thread; the render thread draws its copy.
WindowGate g_window_gate([](bool on, uint16_t close_vk) {
    // the close key down now (the key that opened it): that press must not close it (InputRouter::set_window)
    const bool down = on && close_vk && (GetAsyncKeyState(close_vk) & 0x8000) != 0;
    if (g_router) g_router->set_window(on, close_vk, down);
});
std::optional<WindowRequest> g_window;          // render thread: the copy drawn
uint64_t g_window_seen = ~0ull;                 // render thread: the gate's version of that copy
WindowUiState g_window_ui;                      // render thread
bool g_window_was_open = false;                 // render thread: the pointer is placed on open, freed on close
std::atomic<bool> g_window_close_asked{false};  // Esc or the close key (the window thread)
LookSettings g_look;                // [hud] and the look keys of [overlay], read at start
OverlayFonts g_fonts;               // render thread: the atlas's fonts (new pointers after each rebuild)
FontRebuild g_font_rebuild;         // render thread
std::set<std::string> g_font_told;  // render thread: the font problems already in the log
uint64_t g_glyphs_built = 0;        // render thread: the ScreenGlyphs want the atlas was built for
uint32_t g_extra_language = 0;      // render thread: the language g_extra_file was picked for
std::wstring g_extra_file;          // render thread: its extra font ("" none)
std::vector<unsigned char> g_extra_bytes;  // render thread: its bytes; the atlas points into them until the next build

void log_line(const char* s);
bool world_live();

// A window handed back by the gate gets its CLOSED (exactly once per window).
void window_closed(const std::optional<WindowRequest>& r, uint32_t reason) {
    if (r && g_deps.window_event) g_deps.window_event(*r, EMF_UI_CLOSED, "", reason);
}

// Runs just before a wheel's capture ends: closed while its hold key is still down (Esc, a click), the mod that
// opened it must not take that key as a new press, so it stays hidden from mods until released.
void hide_held_key(const WheelRequest& r) {
    if (r.hold_vk && (GetAsyncKeyState(r.hold_vk) & 0x8000)) g_router->hide_until_released(r.hold_vk);
}

// Hands a closed wheel's request back with `choice` (exactly once per request).
void hand_back(const std::optional<WheelRequest>& r, int choice) {
    if (r && g_deps.wheel_done) g_deps.wheel_done(*r, choice);
}

// Render thread: the open wheel closed with `choice`.
void finish_wheel(int choice) { hand_back(g_wheel_gate.finish(hide_held_key), choice); }

// Opens a requested wheel (render thread, before the frame's input).
void take_wheel_request() {
    const auto r = g_wheel_gate.take();
    if (!r) return;
    const bool held = r->hold_vk && (GetAsyncKeyState(r->hold_vk) & 0x8000) != 0;
    g_wheel.open(r->title, r->items, held, r->hold_vk, GetTickCount64());
    if (!g_wheel.is_open()) finish_wheel(-1);
}

void push(InputEvent e) {
    AcquireSRWLockExclusive(&g_queue_lock);
    if (g_queue.size() < 4096) g_queue.push_back(e);
    ReleaseSRWLockExclusive(&g_queue_lock);
}

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Whether the fonts must be built again for a swapchain `height` pixels high (render thread, before a frame): the
// first time, and when a new size changed the faces' pixel sizes and held for a moment.
bool fonts_due(float height) {
    if (screen_glyphs().wanted_version() != g_glyphs_built) g_font_rebuild.request();  // new item names' characters
    return g_font_rebuild.due(font_sizes(g_look, static_cast<int>(height)), GetTickCount64());
}

// Plans and builds the fonts for a swapchain `height` pixels high (render thread, between frames). A font that cannot
// be used is said once in the log, however often the window changes size; the first few builds are logged.
void setup_fonts(ImGuiIO& io, float height) {
    const int h = static_cast<int>(height);
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const FontPaths paths{g_deps.emf_dir + L"\\fonts", std::wstring(windir) + L"\\Fonts\\consola.ttf"};
    const FontPlan plan = plan_fonts(g_look, h, paths, [](const std::wstring& f) { return font_file_ok(f); });
    std::vector<std::string> problems = plan.problems;
    // the characters the item names need (ScreenGlyphs), and for Japanese, Korean, Chinese and Thai the extra font: picked
    // once per language (the player's Noto file in emf\fonts, else Windows' own), its bytes kept for the atlas
    const GlyphWant want = screen_glyphs().wanted();
    const bool needs_font = !want.glyphs.empty() && language_needs_extra_font(want.language);
    const bool new_pick = needs_font && want.language != g_extra_language;
    std::vector<unsigned char> fresh;
    if (new_pick) {
        g_extra_language = want.language;
        g_extra_file = pick_extra_font(want.language, paths.fonts_dir, std::wstring(windir) + L"\\Fonts",
                                       [](const std::wstring& f) { return font_file_ok(f); });
        if (!g_extra_file.empty() && !read_font_file(g_extra_file, kMaxFontBytes, fresh)) g_extra_file.clear();
    }
    // the same file as last time: its kept bytes (the atlas lets go of them when it is cleared, then points into them
    // again); a new one: the fresh bytes, kept once the atlas is built
    const std::vector<unsigned char>& font = new_pick ? fresh : g_extra_bytes;
    ExtraGlyphs extra;
    extra.glyphs = want.glyphs;
    if (needs_font && !font.empty()) {
        extra.font = font.data();
        extra.font_size = font.size();
    }
    g_fonts = build_font_atlas(*io.Fonts, plan, &problems, &extra);
    if (new_pick) g_extra_bytes.swap(fresh);  // the old bytes go when this returns: the atlas no longer points into them
    if (!needs_font) {                        // no extra font now: nothing points into them
        g_extra_bytes.clear();
        g_extra_bytes.shrink_to_fit();
        g_extra_language = 0;
        g_extra_file.clear();
    }
    const std::vector<uint32_t> drawable = drawable_glyphs(g_fonts, want.glyphs);
    screen_glyphs().baked(want.version, drawable);
    g_glyphs_built = want.version;
    g_ui.tabs_font = g_fonts.tabs;  // the old fonts are gone
    g_font_rebuild.built(font_sizes(g_look, h));
    if (!want.glyphs.empty()) {
        const char* code = language_code(want.language);
        char line[700];
        if (needs_font && g_extra_file.empty())
            std::snprintf(line, sizeof line,
                          "item names: no font for %s on EMF's screens: put %s from %s into %s and restart the game; "
                          "until then they show in English",
                          code, utf8(extra_font_file_name(want.language)).c_str(), extra_font_download(want.language),
                          utf8(paths.fonts_dir).c_str());
        else if (needs_font)
            std::snprintf(line, sizeof line, "item names: %s on EMF's screens with %s (%zu of %zu characters, atlas %dx%d)",
                          code, utf8(g_extra_file).c_str(), drawable.size(), want.glyphs.size(), io.Fonts->TexWidth,
                          io.Fonts->TexHeight);
        else
            std::snprintf(line, sizeof line, "item names: %zu of %zu characters beyond EMF's base fonts for %s (atlas %dx%d)",
                          drawable.size(), want.glyphs.size(), code, io.Fonts->TexWidth, io.Fonts->TexHeight);
        problems.push_back(line);
    }
    for (const auto& p : problems) {
        if (!g_font_told.insert(p).second) continue;
        log_line((p.rfind("item names: ", 0) == 0 ? p : "overlay: " + p).c_str());
    }
    constexpr int kLoggedBuilds = 10;
    static int builds = 0;
    if (++builds <= kLoggedBuilds) {
        char msg[200];
        std::snprintf(msg, sizeof(msg), "overlay: text at %.0f px (console) and %.0f px (on-screen), %d fonts, atlas %dx%d%s",
                      plan.face(FontRole::Console).pixel_size, plan.face(FontRole::Hud).pixel_size, io.Fonts->Fonts.Size,
                      io.Fonts->TexWidth, io.Fonts->TexHeight,
                      builds == kLoggedBuilds ? " (later font builds are not logged)" : "");
        log_line(msg);
    }
}

// EMF's own settings changed (the Settings tab, `set emf ...`): the look, the console's colours, the mouse speed and the
// suggestions follow at once; the fonts are built again only when a font, the style or a size changed. Render thread,
// before a frame; one version compare while nothing changed.
void follow_settings() {
    static uint64_t seen = 0;
    if (!g_deps.settings) return;
    const uint64_t v = g_deps.settings->version();
    if (v == seen) return;
    seen = v;
    std::vector<std::string> problems;
    const LookSettings look = look_from_store(*g_deps.settings, &problems);
    if (fonts_differ(g_look, look)) g_font_rebuild.request();
    g_look = look;
    g_ui.height = static_cast<float>(g_look.height) / 100.0f;
    g_ui.opacity = static_cast<float>(g_look.opacity) / 100.0f;
    g_ui.background = look_colour4(g_look.background);
    g_ui.colour_command = look_colour4(g_look.colour_command);
    g_ui.colour_reply = look_colour4(g_look.colour_reply);
    g_ui.colour_error = look_colour4(g_look.colour_error);
    g_ui.colour_success = look_colour4(g_look.colour_success);
    apply_look_theme(ImGui::GetStyle(), g_look);
    const SettingsStore& s = *g_deps.settings;
    if (s.has(kEmfSettingsId, "overlay.mouse_speed"))
        g_mouse_speed = static_cast<float>(s.get_float(kEmfSettingsId, "overlay.mouse_speed", g_mouse_speed));
    if (s.has(kEmfSettingsId, "overlay.suggest")) g_ui.suggest = s.get(kEmfSettingsId, "overlay.suggest") == "1";
    if (s.has(kEmfSettingsId, "overlay.timestamps"))
        g_ui.timestamps = s.get(kEmfSettingsId, "overlay.timestamps") == "1";
    if (s.has(kEmfSettingsId, "overlay.suggest_rows"))
        g_ui.suggester.set_rows(static_cast<size_t>(s.get_int(kEmfSettingsId, "overlay.suggest_rows", 8)));
}

void before_frame(ImGuiIO& io) {
    follow_settings();
    take_wheel_request();
    std::string page;
    AcquireSRWLockExclusive(&g_queue_lock);
    page.swap(g_pending_page);
    ReleaseSRWLockExclusive(&g_queue_lock);
    if (!page.empty()) {
        g_ui.open_tab = 2;
        g_settings_ui.page = page;
    }
    if (g_wheel_gate.take_cancel() && g_wheel.is_open()) {  // alt-tab while it was open: its keys' releases are gone
        g_wheel.cancel();
        finish_wheel(-1);
    }
    // a mod's window: closed by Esc or its key, or by the world ending (the main menu); then the copy to draw
    if (g_window_close_asked.exchange(false)) window_closed(g_window_gate.finish(), EMF_UI_BY_PLAYER);
    if (g_window_gate.busy() && !world_live()) window_closed(g_window_gate.turned_off(), EMF_UI_BY_CORE);
    if (g_window_gate.version() != g_window_seen) {
        g_window_seen = g_window_gate.version();
        g_window = g_window_gate.current();
    }
    const bool window_open = g_window.has_value();
    std::vector<InputEvent> events;
    AcquireSRWLockExclusive(&g_queue_lock);
    events.swap(g_queue);
    ReleaseSRWLockExclusive(&g_queue_lock);
    // One pointer: in the game's menus the Windows pointer shows, and the console follows it (no arrow of its own);
    // in play it is hidden and the console's arrow moves with the raw mouse. Looked at only while the console or a mod's
    // window is open.
    PointerSource pointer = PointerSource::Own;
    if (g_router->open() || window_open) {
        const WindowsPointer wp = read_windows_pointer();
        pointer = pointer_source(wp.showing, wp.in_front, absolute_mouse_seen());
        if (pointer == PointerSource::Windows) {
            g_cursor.set_display(io.DisplaySize);
            if (const auto at = client_to_display(wp.x, wp.y, wp.client_w, wp.client_h, io.DisplaySize)) {
                g_cursor.place(*at);  // the arrow takes over from here when the game hides the pointer again
                io.AddMousePosEvent(at->x, at->y);
            }
        }
    }
    if (window_open != g_window_was_open && !g_router->open()) {
        if (window_open) {  // the pointer starts where the console's arrow was
            g_cursor.set_display(io.DisplaySize);
            if (pointer == PointerSource::Own) io.AddMousePosEvent(g_cursor.pos().x, g_cursor.pos().y);
        } else {  // no hover or held button stays behind
            io.AddMouseButtonEvent(0, false);
            io.AddMouseButtonEvent(1, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
    }
    g_window_was_open = window_open;
    for (const auto& e : events) {
        if (g_wheel.is_open() && (e.kind == InputEvent::Key || e.kind == InputEvent::Mouse)) {  // the wheel's input
            std::optional<int> result;
            if (e.kind == InputEvent::Key) {
                result = g_wheel.key(e.key, GetTickCount64());
            } else {
                g_wheel.mouse(e.dx, e.dy);
                if (e.wheel != 0.0f) g_wheel.scroll(e.wheel);
                if (e.buttons & RI_MOUSE_LEFT_BUTTON_DOWN) result = g_wheel.button(0);
                if (!result && (e.buttons & RI_MOUSE_RIGHT_BUTTON_DOWN)) result = g_wheel.button(1);
            }
            if (result) finish_wheel(*result);
            continue;
        }
        switch (e.kind) {
            case InputEvent::Toggle:
                io.ClearInputKeys();
                g_ui.escape = false;  // an Esc from before the console opened or closed is not the list's
                if (e.open) {
                    g_ui.focus_input = true;
                    g_cursor.set_display(io.DisplaySize);
                    if (pointer == PointerSource::Own) io.AddMousePosEvent(g_cursor.pos().x, g_cursor.pos().y);
                } else {  // no hover or held button stays behind
                    io.AddMouseButtonEvent(0, false);
                    io.AddMouseButtonEvent(1, false);
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                    if (g_deps.hud_editor) g_deps.hud_editor->flush();  // the HUD page's last edit is saved now
                }
                break;
            case InputEvent::Key:
                // Esc reaches the console only while its suggestions show (InputRouter): it hides them (only while
                // the console is open). ImGui never gets it (an active input line would throw away what was typed).
                if (e.key.scan == InputRouter::kEscScan && !e.key.extended) {
                    if (capturing_key(g_mods_ui)) {  // a key bind or the console key waits: Esc cancels it
                        feed_key_event(g_mods_ui, e.key, io, ImGui::GetFrameCount());
                        break;
                    }
                    if (e.key.down && g_router->open()) g_ui.escape = true;
                    break;
                }
                feed_key_event(g_mods_ui, e.key, io, ImGui::GetFrameCount());
                break;
            case InputEvent::FocusLost:  // alt-tab: the releases go to another window
                io.AddFocusEvent(false);  // ImGui drops every held key and button (once, in order with the keys)
                break;
            case InputEvent::Char: io.AddInputCharacterUTF16(static_cast<ImWchar16>(e.ch)); break;
            case InputEvent::Wheel: io.AddMouseWheelEvent(0.0f, e.wheel); break;
            case InputEvent::Mouse:  // the console's or a window's cursor (a wheel's input is handled above)
                if (!g_router->open() && !window_open) break;
                if (pointer == PointerSource::Own) {  // with the Windows pointer the position came from it above
                    g_cursor.set_display(io.DisplaySize);
                    g_cursor.move(e.dx, e.dy, g_mouse_speed);
                    io.AddMousePosEvent(g_cursor.pos().x, g_cursor.pos().y);
                }
                if (e.buttons & RI_MOUSE_LEFT_BUTTON_DOWN) io.AddMouseButtonEvent(0, true);
                if (e.buttons & RI_MOUSE_LEFT_BUTTON_UP) io.AddMouseButtonEvent(0, false);
                if (e.buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) io.AddMouseButtonEvent(1, true);
                if (e.buttons & RI_MOUSE_RIGHT_BUTTON_UP) io.AddMouseButtonEvent(1, false);
                break;
        }
    }
    // EMF's arrow only where the game hides the Windows pointer (play); in its menus that pointer is the one
    io.MouseDrawCursor = (g_router->open() || window_open) && pointer == PointerSource::Own;
    // Slide: ~0.15 s down or up.
    const float target = g_router->open() ? 1.0f : 0.0f;
    const float step = io.DeltaTime * 7.0f;
    g_ui.open_fraction = g_ui.open_fraction < target ? (std::min)(target, g_ui.open_fraction + step)
                                                     : (std::max)(target, g_ui.open_fraction - step);
}

// The names the console completes and suggests (render thread, every frame the console shows); the version changes
// only when something did, so the suggestions recompute only then.
void refresh_sources() {
    bool changed = false;
    if (g_sources.components.empty()) {
        for (const auto& c : emf::gen::kComponents) {
            const char* p = std::strrchr(c.name, ':');
            g_sources.components.emplace_back(p ? p + 1 : c.name);
        }
        changed = true;
    }
    std::vector<std::string> commands, help;
    for (auto& [name, text] : g_deps.center->list()) {
        commands.push_back(std::move(name));
        help.push_back(std::move(text));
    }
    for (const char* own : {"clear", "cls"}) {  // the console's own (draw_console's submit takes them)
        commands.emplace_back(own);
        help.emplace_back(": empty the console's scrollback");
    }
    if (commands != g_sources.commands || help != g_sources.command_help) {
        g_sources.commands = std::move(commands);
        g_sources.command_help = std::move(help);
        changed = true;
    }
    std::vector<ModSource> mods;
    if (g_deps.mods)
        for (const auto& m : g_deps.mods->list()) {
            ModSource mod;
            mod.id = m.id;
            mod.name = m.name;
            if (g_deps.settings)
                for (const auto& d : g_deps.settings->defs(m.id)) mod.settings.push_back(setting_source(d));
            mods.push_back(std::move(mod));
        }
    if (mods != g_sources.mod_settings) {
        g_sources.mod_settings = std::move(mods);
        changed = true;
    }
    const uint64_t now = GetTickCount64();
    if (g_deps.item_names && (g_sources.items.empty() || now - g_items_refreshed > 10000)) {
        g_items_refreshed = now;
        auto items = g_deps.item_names();
        if (!items.empty() && items != g_sources.items) {
            g_sources.items = std::move(items);
            changed = true;
        }
    }
    if (changed) ++g_sources.version;
}

// A world is running (the HUD lines are current); on the main menu and loading screens none ticks.
bool world_live() {
    const uint64_t now = GetTickCount64();
    return g_deps.tracker &&
           (g_deps.tracker->latest(EMF_WORLD_CLIENT, now) || g_deps.tracker->latest(EMF_WORLD_SERVER, now));
}

// How HUD lines, notes and wheels are drawn: the look's settings with this atlas's fonts.
TextLook text_look() {
    TextLook t;
    t.style = g_look.style;
    t.shadow = g_look.shadow;
    t.colour = g_look.hud_colour;
    t.accent = g_look.accent;
    t.notes_place = g_look.notes_place;
    t.font = g_fonts.hud;
    t.bold = g_fonts.hud_bold;
    return t;
}

// A mod window's pictures (API v26): the image store's frame, once the render thread uploaded the picture.
std::optional<ImageDraw> window_picture(uint32_t handle, double seconds, uint32_t fps) {
    const auto f = overlay_images().frame(handle, seconds, fps);
    if (!f) return std::nullopt;
    return ImageDraw{static_cast<ImTextureID>(f->texture), ImVec2(f->uv.u0, f->uv.v0), ImVec2(f->uv.u1, f->uv.v1),
                     f->aspect};
}

// Markers (API v33) over the 3D view or the game's map screen, under everything else EMF draws.
// The map gesture's ring (API v34): a circle filling over 1 s at the held spot on the map.
void draw_map_ring(ImVec2 display, uint64_t now) {
    MapInputEnv env = map_input_env(display.x, display.y, now);
    env.captured = input_capturing();
    map_input().frame(env);
    const MapRing r = map_input().ring(now);
    MapView v;
    if (!r.holding || !marker_map_view(&v) || !(v.rect[2] > 0) || !(v.ui_w > 0) || !(v.ui_h > 0)) return;
    const float sx = display.x / v.ui_w, sy = display.y / v.ui_h, k = display.y / 1080.0f;
    const ImVec2 c(static_cast<float>((v.rect[0] + r.x / kMapWorldSize * v.rect[2]) * sx),
                   static_cast<float>((v.rect[1] + (1.0 - r.z / kMapWorldSize) * v.rect[3]) * sy));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float radius = 22.0f * k;
    dl->AddCircle(c, radius, IM_COL32(0, 0, 0, 140), 40, 6.0f * k);
    const float a0 = -1.5707963f;
    dl->PathArcTo(c, radius, a0, a0 + 6.2831853f * r.progress, 40);
    dl->PathStroke(look_colour(text_look().accent), 0, 4.0f * k);
}

void build_markers(ImVec2 display) {
    marker_capture_set_display(display.x, display.y);  // world_to_screen's pixels
    const uint64_t now = GetTickCount64();
    draw_map_ring(display, now);
    if (!g_deps.markers) return;
    const bool console = g_router->open() || g_ui.open_fraction > 0.0f;
    const MarkerSight sight = marker_sight(now);
    static const ImageLookup pictures = window_picture;
    MapView map;
    if (sight.map_open) {
        if (!marker_map_view(&map) || !show_map_markers(console, sight, map, now)) return;
        const auto layouts = layout_map_markers(g_deps.markers->snapshot(), map, display);
        if (!layouts.empty()) draw_markers(ImGui::GetBackgroundDrawList(), layouts, text_look(), &pictures);
        return;
    }
    CameraSnap cam;
    if (!marker_camera(&cam) || !show_markers(console, sight, cam, now)) return;
    double player[3];
    const bool has_player = marker_player_pos(player);
    const auto layouts = layout_markers(g_deps.markers->snapshot(), cam, has_player ? player : nullptr, display);
    if (!layouts.empty()) draw_markers(ImGui::GetBackgroundDrawList(), layouts, text_look(), &pictures);
}

// Canvases (API v35): mods' drawing panels over the HUD, hidden where the markers hide (the map, menus, the console).
ImU32 rgba_col(uint32_t rgba) {
    return IM_COL32((rgba >> 24) & 0xff, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff);
}

void draw_canvas_markers(ImDrawList* dl, const std::vector<Marker>& markers, const float r[4], float units_w,
                         const float* a, bool circle) {
    const ImVec2 centre(r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f);
    const float radius = (std::min)(r[2], r[3]) * 0.5f, s = 7.0f * r[3] / 220.0f;  // a dot's size follows the canvas
    for (const Marker& m : markers) {
        if (!(m.flags & EMF_MARKER_MINIMAP)) continue;
        float px, py;
        canvas_world_to(r, units_w, a[0], a[1], a[2], m.x, m.z, &px, &py);
        const float dx = px - centre.x, dy = py - centre.y;
        if (circle ? dx * dx + dy * dy > radius * radius : (px < r[0] || py < r[1] || px > r[0] + r[2] || py > r[1] + r[3]))
            continue;
        const ImU32 fill = m.rgba ? rgba_col(m.rgba) : look_colour(text_look().accent), edge = IM_COL32(0, 0, 0, 180);
        const ImVec2 c(px, py);
        if (m.shape == EMF_MARKER_SQUARE) {
            dl->AddRectFilled(ImVec2(px - s, py - s), ImVec2(px + s, py + s), fill);
            dl->AddRect(ImVec2(px - s, py - s), ImVec2(px + s, py + s), edge, 0, 0, 1.5f);
        } else if (m.shape == EMF_MARKER_CIRCLE || m.shape == EMF_MARKER_NONE) {
            dl->AddCircleFilled(c, s, fill, 16);
            dl->AddCircle(c, s, edge, 16, 1.5f);
        } else {
            const ImVec2 q[4] = {ImVec2(px, py - s * 1.3f), ImVec2(px + s, py), ImVec2(px, py + s * 1.3f), ImVec2(px - s, py)};
            dl->AddConvexPolyFilled(q, 4, fill);
            dl->AddPolyline(q, 4, edge, ImDrawFlags_Closed, 1.5f);
        }
    }
}

void draw_canvases(ImVec2 display) {
    if (!g_deps.canvases) return;
    const auto view = g_deps.canvases->view();
    if (!view || view->empty()) return;
    const uint64_t now = GetTickCount64();
    const MarkerSight sight = marker_sight(now);
    const bool console = g_router->open() || g_ui.open_fraction > 0.0f;
    if (console || !sight.world_ui || sight.map_open || !world_live()) return;
    minimap_note_shown(now);  // the game's map (drawn by the game's UI) follows the overlay's panels
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // once per frame: the markers for MARKERS commands, the fog copy for FOG commands (no lock per cell)
    std::vector<Marker> markers;
    bool markers_read = false;
    const std::shared_ptr<const FogSnapshot> fog = g_deps.fog ? g_deps.fog->snapshot() : nullptr;
    for (const Canvas& c : *view) {
        if (c.spec.hidden) continue;
        float r[4];
        canvas_layout(c.spec, display.x, display.y, r);
        const float k = r[2] / c.spec.w;  // pixels per unit
        const bool circle = c.spec.shape == EMF_CANVAS_CIRCLE;
        const ImVec2 o(r[0], r[1]), centre(r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f);
        const float radius = (std::min)(r[2], r[3]) * 0.5f;
        auto at = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
        dl->PushClipRect(o, ImVec2(r[0] + r[2], r[1] + r[3]), true);
        // what the list puts before its MAP is under the map: drawn only when the game cannot draw the map
        const size_t first = canvas_first_drawn(c.cmds, minimap_draw_ready());
        for (size_t ci = first; ci < c.cmds.size(); ++ci) {
            const CanvasCmd& m = c.cmds[ci];
            const ImU32 col = rgba_col(m.rgba);
            const float* a = m.a;
            switch (m.kind) {
                case EMF_CANVAS_RECT_CMD:
                    if (a[4] > 0) dl->AddRect(at(a[0], a[1]), at(a[0] + a[2], a[1] + a[3]), col, 0, 0, a[4] * k);
                    else dl->AddRectFilled(at(a[0], a[1]), at(a[0] + a[2], a[1] + a[3]), col);
                    break;
                case EMF_CANVAS_CIRCLE_CMD:
                    if (a[3] > 0) dl->AddCircle(at(a[0], a[1]), a[2] * k, col, 48, a[3] * k);
                    else dl->AddCircleFilled(at(a[0], a[1]), a[2] * k, col, 48);
                    break;
                case EMF_CANVAS_LINE: dl->AddLine(at(a[0], a[1]), at(a[2], a[3]), col, (std::max)(1.0f, a[4] * k)); break;
                case EMF_CANVAS_TRI: dl->AddTriangleFilled(at(a[0], a[1]), at(a[2], a[3]), at(a[4], a[5]), col); break;
                case EMF_CANVAS_TEXT: {
                    ImFont* f = g_fonts.hud ? g_fonts.hud : ImGui::GetFont();
                    const float size = a[2] * k;
                    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, m.s.c_str());
                    const ImVec2 p = at(a[0], a[1]);
                    dl->AddText(f, size, ImVec2(p.x - ts.x * 0.5f + 1, p.y - ts.y * 0.5f + 1), IM_COL32(0, 0, 0, (m.rgba & 0xff) * 3 / 4),
                                m.s.c_str());
                    dl->AddText(f, size, ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), col, m.s.c_str());
                    break;
                }
                case EMF_CANVAS_IMAGE: {
                    const auto img = window_picture(m.image, 0, 0);
                    if (!img) break;
                    const ImVec2 p0 = at(a[0], a[1]), p1 = at(a[0] + a[2], a[1] + a[3]);
                    const bool whole = a[4] == 0 && a[5] == 0 && a[6] == 0 && a[7] == 0;
                    const ImVec2 u0 = whole ? img->uv0 : ImVec2(img->uv0.x + (img->uv1.x - img->uv0.x) * a[4], img->uv0.y + (img->uv1.y - img->uv0.y) * a[5]);
                    const ImVec2 u1 = whole ? img->uv1 : ImVec2(img->uv0.x + (img->uv1.x - img->uv0.x) * a[6], img->uv0.y + (img->uv1.y - img->uv0.y) * a[7]);
                    if (a[8] == 0) {
                        dl->AddImage(img->tex, p0, p1, u0, u1, col);
                    } else {  // turned about its centre
                        const ImVec2 cc((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                        const float hw = (p1.x - p0.x) * 0.5f, hh = (p1.y - p0.y) * 0.5f, t = a[8] * 0.017453293f;
                        const float cs = std::cos(t), sn = std::sin(t);
                        auto rot = [&](float x, float y) { return ImVec2(cc.x + x * cs - y * sn, cc.y + x * sn + y * cs); };
                        dl->AddImageQuad(img->tex, rot(-hw, -hh), rot(hw, -hh), rot(hw, hh), rot(-hw, hh), u0,
                                         ImVec2(u1.x, u0.y), u1, ImVec2(u0.x, u1.y), col);
                    }
                    break;
                }
                case EMF_CANVAS_FOG: {
                    if (!fog) break;
                    const FogHeader& fh = fog->header;
                    const auto cells = fog_cells(r, c.spec.w, a[0], a[1], a[2],
                                                 [&fh](double x, double z) { return fog_explored(fh, x, z); });
                    for (const FogCell& f : cells) {
                        if (circle) {
                            const float dx = f.x + f.w * 0.5f - centre.x, dy = f.y + f.h * 0.5f - centre.y;
                            if (dx * dx + dy * dy > (radius + f.w) * (radius + f.w)) continue;
                        }
                        const uint32_t alpha = static_cast<uint32_t>((m.rgba & 0xff) * f.alpha);
                        dl->AddRectFilled(ImVec2(f.x, f.y), ImVec2(f.x + f.w, f.y + f.h), rgba_col((m.rgba & 0xffffff00u) | alpha));
                    }
                    break;
                }
                case EMF_CANVAS_MARKERS:
                    if (!markers_read && g_deps.markers) {
                        markers = g_deps.markers->snapshot();
                        markers_read = true;
                    }
                    draw_canvas_markers(dl, markers, r, c.spec.w, a, circle);
                    break;
                default: break;  // MAP: the game draws it (part 3b)
            }
        }
        dl->PopClipRect();
    }
}

void build(ImVec2 display) {
    build_markers(display);
    draw_canvases(display);
    if (g_window) {  // under the console: drawn first, and without input while the console is open
        static const ImageLookup pictures = window_picture;
        const auto click = draw_window(*g_window, g_window_ui, !g_router->open(), display, text_look(), &pictures);
        if (click && g_deps.window_event)
            g_deps.window_event(*g_window, click->right ? EMF_UI_RIGHT_CLICK : EMF_UI_CLICK, click->key, 0);
        if (g_window_ui.close_clicked) {
            g_window_ui.close_clicked = false;
            window_closed(g_window_gate.finish(), EMF_UI_BY_PLAYER);
        }
    }
    if (g_router->open() || g_ui.open_fraction > 0.0f) refresh_sources();  // Tab completion: only for the console
    std::function<void()> mods_tab;
    if (g_deps.mods) mods_tab = [] { draw_mods_tab(g_deps.mods->list(), g_deps.settings, g_mods_ui); };
    std::function<void()> settings_tab;
    if (g_deps.settings)
        settings_tab = [] {
            draw_settings_tab(g_deps.settings, g_mods_ui, g_settings_ui,
                              [] { draw_look_preview(text_look(), ImGui::GetContentRegionAvail().x); },
                              g_deps.hud_editor ? std::function<void()>([] {
                                  draw_hud_page(*g_deps.hud_editor, g_hud_page, ImGui::GetIO().DisplaySize);
                              })
                                                : std::function<void()>());
        };
    AcquireSRWLockShared(&g_status_lock);
    g_ui.status = g_status;
    ReleaseSRWLockShared(&g_status_lock);
    draw_console(*g_model, g_ui, g_sources, display, [](const std::string& line) {
        if (is_console_clear(line))  // the console's own: also on the main menu, never to the chat or commands.txt
            g_model->clear();
        else
            submit_line(*g_deps.center, host_state(*g_deps.tracker, GetTickCount64()), line, g_deps.reply);
        g_model->save_history(g_history_path);
    }, mods_tab, settings_tab);
    // Esc hides the suggestions first, and cancels a key bind or console key that waits for a key
    g_page_showing = g_router->open() && g_ui.tab == 2 && g_settings_ui.page == kPageHud;
    g_router->set_esc_to_console(g_router->open() && (g_ui.wants_escape || capturing_key(g_mods_ui)));
    const TextLook look = text_look();
    if (g_ui.open_fraction <= 0.0f)
        draw_notes(toast_lines(*g_model, ToastState{g_toast_from.load(), g_toast_until.load()}, GetTickCount64()),
                   display, look);
    // Mods' lines: with the console closed, or behind the HUD page (with a sample line for each empty place) so they
    // can be picked and dragged there; the HUD editor's layout moves, scales and hides each place.
    if (g_deps.hud && (g_ui.open_fraction <= 0.0f || g_page_showing)) {
        const hud::HudSnapshot* snap = g_deps.hud_editor ? g_deps.hud_editor->snapshots().current() : nullptr;
        const bool samples = g_page_showing && snap && snap->active;
        if (show_hud(false, !g_deps.hud->empty() || samples, world_live())) {
            std::array<std::optional<ImVec4>, 3> rects{};
            HudLinesView view;
            view.samples = samples;
            view.rects = &rects;
            for (int p = 0; p < 3; ++p) view.edits[static_cast<size_t>(p)] = lines_edit(snap, static_cast<HudPlace>(p), display);
            draw_hud(g_deps.hud->lines(), display, look, view);
            const hud::UiSpace ui = hud::hud_ui_space();
            const uint64_t now = GetTickCount64();
            for (int p = 0; p < 3; ++p) {
                const auto& r = rects[static_cast<size_t>(p)];
                if (!r || !(display.x > 0) || !(display.y > 0)) {
                    hud::lines_not_drawn(p);
                    continue;
                }
                const float sx = ui.w / display.x, sy = ui.h / display.y;
                const float units[4] = {r->x * sx, r->y * sy, r->z * sx, r->w * sy};
                hud::set_lines_live(p, units, now);
            }
        }
    }
    draw_wheel(g_wheel, display, look);
}

void log_line(const char* s) {
    if (g_deps.log) g_deps.log(s);
}

}  // namespace

namespace {
// An [overlay] value as text, without a "; comment" after it on the same line (Windows keeps those in the value) and
// the spaces and tabs around it; "" when it is not set.
std::wstring overlay_value(const wchar_t* key, const std::wstring& ini_path) {
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"overlay", key, L"", buf, 256, ini_path.c_str());
    std::wstring v(buf);
    v = v.substr(0, v.find(L';'));
    const size_t first = v.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    return v.substr(first, v.find_last_not_of(L" \t") - first + 1);
}
}  // namespace

OverlayConfig read_overlay_config(const std::wstring& ini_path) {
    OverlayConfig c;
    c.enabled = GetPrivateProfileIntW(L"overlay", L"enabled", 1, ini_path.c_str()) != 0;
    c.chat_commands = GetPrivateProfileIntW(L"overlay", L"chat", 1, ini_path.c_str()) != 0;
    const std::wstring speed = overlay_value(L"mouse_speed", ini_path);
    if (!speed.empty()) {
        wchar_t* end = nullptr;
        const double v = std::wcstod(speed.c_str(), &end);
        if (end && !*end && v >= 0.1 && v <= 10.0) c.mouse_speed = static_cast<float>(v);
    }
    const std::wstring key = overlay_value(L"key", ini_path);
    if (!key.empty()) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(key.c_str(), &end, 0);
        if (end && !*end && v >= 1 && v <= 0x7F) c.toggle_scan = static_cast<uint16_t>(v);
    }
    c.suggest = GetPrivateProfileIntW(L"overlay", L"suggest", 1, ini_path.c_str()) != 0;
    c.timestamps = GetPrivateProfileIntW(L"overlay", L"timestamps", 0, ini_path.c_str()) != 0;
    const std::wstring scroll = overlay_value(L"scrollback", ini_path);
    if (!scroll.empty()) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(scroll.c_str(), &end, 10);
        if (end && !*end) c.scrollback = std::clamp<size_t>(v, kMinScrollback, kMaxScrollback);
    }
    c.debug_keys = GetPrivateProfileIntW(L"overlay", L"debug_keys", 0, ini_path.c_str()) != 0;
    const std::wstring rows = overlay_value(L"suggest_rows", ini_path);
    if (!rows.empty()) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(rows.c_str(), &end, 10);
        if (end && !*end) c.suggest_rows = std::clamp<size_t>(v, Suggester::kMinRows, Suggester::kMaxRows);
    }
    c.look = read_look_settings(
        [&ini_path](const char* section, const char* key) {
            wchar_t buf[512] = {};
            GetPrivateProfileStringW(std::wstring(section, section + std::strlen(section)).c_str(),
                                     std::wstring(key, key + std::strlen(key)).c_str(), L"", buf, 512, ini_path.c_str());
            return utf8(buf);
        },
        &c.look_problems);
    return c;
}

OverlayConfig overlay_config_with_store(OverlayConfig c, const SettingsStore& s) {
    if (!s.has(kEmfSettingsId, "hud.style")) return c;  // no EMF settings in this store
    c.look_problems.clear();
    c.look = look_from_store(s, &c.look_problems);
    c.mouse_speed = static_cast<float>(s.get_float(kEmfSettingsId, "overlay.mouse_speed", c.mouse_speed));
    c.chat_commands = s.get(kEmfSettingsId, "overlay.chat") == "1";
    c.suggest = s.get(kEmfSettingsId, "overlay.suggest") == "1";
    c.suggest_rows = static_cast<size_t>(s.get_int(kEmfSettingsId, "overlay.suggest_rows", 8));
    if (s.has(kEmfSettingsId, "overlay.timestamps")) c.timestamps = s.get(kEmfSettingsId, "overlay.timestamps") == "1";
    if (s.has(kEmfSettingsId, "overlay.scrollback"))
        c.scrollback = std::clamp<size_t>(
            static_cast<size_t>(s.get_int(kEmfSettingsId, "overlay.scrollback", static_cast<int64_t>(c.scrollback))),
            kMinScrollback, kMaxScrollback);
    const std::string key = s.get(kEmfSettingsId, "overlay.key");  // "0x29", checked by the store
    const unsigned long scan = std::strtoul(key.c_str(), nullptr, 16);
    if (scan >= 1 && scan <= 0x7F) c.toggle_scan = static_cast<uint16_t>(scan);
    return c;
}

void feed_key_event(ModsUiState& mods_ui, const KeyEvent& e, ImGuiIO& io, int frame) {
    if (capture_key_event(mods_ui, e, frame)) return;  // the Mods tab waits for a key to bind
    const ImGuiKey k = imgui_key(e);
    if (k == ImGuiKey_None) return;
    io.AddKeyEvent(k, e.down);
    if (k == ImGuiKey_LeftCtrl || k == ImGuiKey_RightCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, e.down);
    if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) io.AddKeyEvent(ImGuiMod_Shift, e.down);
    if (k == ImGuiKey_LeftAlt || k == ImGuiKey_RightAlt) io.AddKeyEvent(ImGuiMod_Alt, e.down);
}

ImGuiKey imgui_key(const KeyEvent& e) {
    UINT vk = e.vkey;
    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_RETURN: return e.extended ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_SHIFT: return e.scan == 0x36 ? ImGuiKey_RightShift : ImGuiKey_LeftShift;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_CONTROL: return e.extended ? ImGuiKey_RightCtrl : ImGuiKey_LeftCtrl;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_MENU: return e.extended ? ImGuiKey_RightAlt : ImGuiKey_LeftAlt;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RMENU: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

bool start_overlay(const OverlayConfig& config, OverlayDeps deps) {
    if (!config.enabled || g_model || !deps.center || !deps.tracker) return false;
    g_deps = std::move(deps);
    for (const auto& p : config.look_problems) log_line(("overlay: emf_core.ini " + p).c_str());
    g_look = config.look;
    g_ui.height = static_cast<float>(g_look.height) / 100.0f;
    g_ui.opacity = static_cast<float>(g_look.opacity) / 100.0f;
    g_ui.background = look_colour4(g_look.background);
    g_ui.colour_command = look_colour4(g_look.colour_command);
    g_ui.colour_reply = look_colour4(g_look.colour_reply);
    g_ui.colour_error = look_colour4(g_look.colour_error);
    g_ui.colour_success = look_colour4(g_look.colour_success);
    g_mouse_speed = config.mouse_speed;
    g_ui.suggest = config.suggest;
    g_ui.suggester.set_rows(config.suggest_rows);
    g_model = std::make_unique<ConsoleModel>(config.scrollback);
    g_ui.timestamps = config.timestamps;
    g_router = std::make_unique<InputRouter>(config.toggle_scan);
    g_router->set_game_text(&game_typing_now);  // the console key types into the game's chat while it is open
    g_history_path = g_deps.emf_dir + L"\\console_history.txt";
    g_model->load_history(g_history_path);
    InputCallbacks cb;
    cb.console_key = [](const KeyEvent& e) { push(InputEvent{InputEvent::Key, e}); };
    cb.console_char = [](wchar_t ch) {
        InputEvent e{InputEvent::Char};
        e.ch = ch;
        push(e);
    };
    cb.console_wheel = [](float d) {
        InputEvent e{InputEvent::Wheel};
        e.wheel = d;
        push(e);
    };
    cb.close_window = [] { g_window_close_asked = true; };
    cb.focus_lost = [] {
        g_wheel_gate.focus_lost();
        push(InputEvent{InputEvent::FocusLost});  // the console forgets held keys and buttons
    };
    cb.overlay_mouse = [](float dx, float dy, uint16_t buttons, float wheel) {
        InputEvent e{InputEvent::Mouse};
        e.dx = dx;
        e.dy = dy;
        e.buttons = buttons;
        e.wheel = wheel;
        push(e);
    };
    cb.toggled = [](bool open) {
        if (open && !vulkan_overlay_ready()) {  // it could not be seen: keep the game's input with the game
            g_router->set_open(false);
            static bool told = false;
            if (!told) {
                told = true;
                log_line(vulkan_overlay_ready_reason());
            }
            return;
        }
        InputEvent e{InputEvent::Toggle};
        e.open = open;
        push(e);
    };
    set_key_debug(config.debug_keys);  // [overlay] debug_keys=1: the first console keys go to the log
    const bool input = install_input_hooks(g_router.get(), std::move(cb));
    VkOverlayHooks hooks;
    hooks.visible = [] {
        OverlayDue d;
        const uint64_t now = GetTickCount64();
        d.console = g_router->open() || g_ui.open_fraction > 0.0f;
        d.toast = now < g_toast_until.load();
        d.busy = g_wheel_gate.busy() || g_window_gate.busy();
        if (d.console || d.toast || d.busy) return true;
        d.world_live = world_live();
        if (!d.world_live) return false;
        d.hud_lines = g_deps.hud && !g_deps.hud->empty();
        const MarkerSight sight = marker_sight(now);
        d.map_open = sight.map_open;
        d.world_ui = sight.world_ui;
        if (d.world_ui) {
            if (g_deps.canvases) {
                const auto view = g_deps.canvases->view();
                d.canvases = view && !view->empty();
            }
            d.markers = g_deps.markers && g_deps.markers->size() > 0;
        }
        return overlay_due(d);
    };
    hooks.fonts_due = fonts_due;
    hooks.fonts = setup_fonts;
    hooks.style = [](ImGuiStyle& s) { apply_look_theme(s, g_look); };
    hooks.before_frame = before_frame;
    hooks.build = build;
    hooks.turned_off = [] {  // never leave an invisible console or wheel holding the input
        g_router->set_open(false);
        g_wheel.cancel();
        hand_back(g_wheel_gate.turned_off(hide_held_key), -1);
        window_closed(g_window_gate.turned_off(), EMF_UI_BY_CORE);
    };
    set_vulkan_overlay(std::move(hooks));
    char msg[200];
    std::snprintf(msg, sizeof(msg), "overlay: console on the key with scan code 0x%02x (input hooks %s, Vulkan %s)",
                  config.toggle_scan, input ? "ok" : "PARTLY",
                  vulkan_overlay_ready() ? "ready" : "waiting for the game's frames");
    log_line(msg);
    return true;
}

ConsoleModel* overlay_console() { return g_model.get(); }

void overlay_show_replies(uint64_t ms) {
    if (!g_model) return;
    g_toast_from = g_model->version();
    g_toast_until = GetTickCount64() + (ms ? ms : static_cast<uint64_t>(g_look.notes_seconds) * 1000);
}

bool overlay_open_wheel(const WheelRequest& request) {
    if (!g_model || !g_router) return false;
    const std::function<bool()> ready = [] { return vulkan_overlay_ready(); };  // judged under the gate's lock
    const auto why = g_wheel_gate.request(request, ready, window_hooked(), g_router->open(), g_window_gate.busy());
    if (why == WheelGate::Refusal::None) return true;
    static const char* const kWhy[] = {"", "the overlay cannot draw yet", "the game window is not hooked (no input)",
                                       "the console is open", "no entries", "another wheel is open",
                                       "a mod's window is open"};
    log_line((std::string("wheel refused: ") + kWhy[static_cast<int>(why)]).c_str());
    return false;
}

bool overlay_toggle_settings_page(const char* page) {
    if (!g_model || !g_router || !page) return false;
    const PageToggle t = page_toggle(g_router->open(), g_page_showing.load(), g_wheel_gate.busy() || g_window_gate.busy());
    if (t == PageToggle::Refuse) return false;
    if (t == PageToggle::Close) {
        g_router->set_open(false);
        InputEvent e{InputEvent::Toggle};
        e.open = false;
        push(e);
        return true;
    }
    if (!vulkan_overlay_ready()) return false;  // it could not be seen: keep the game's input with the game
    AcquireSRWLockExclusive(&g_queue_lock);
    g_pending_page = page;
    ReleaseSRWLockExclusive(&g_queue_lock);
    if (t == PageToggle::Open) {
        g_router->set_open(true);
        InputEvent e{InputEvent::Toggle};
        e.open = true;
        push(e);
    }
    return true;
}

void overlay_notify(const std::string& text) {
    if (!g_model) return;
    if (GetTickCount64() >= g_toast_until.load()) g_toast_from = g_model->version();  // a new toast, else join it
    g_toast_until = GetTickCount64() + static_cast<uint64_t>(g_look.notes_seconds) * 1000;  // [hud] notes_seconds
    g_model->add_line(text);
}

ImageStore& overlay_images() {
    static ImageStore store;  // lives as long as the process: pictures stay until the game closes
    return store;
}

uint32_t overlay_load_image(const std::wstring& path, const std::string& key, std::string* why) {
    return load_image(overlay_images(), path, key, why);
}

OverlayStatus overlay_status() {
    OverlayStatus s;
    if (!g_model || !g_router) return s;
    s.running = vulkan_overlay_ready();
    s.console_open = g_router->open();
    s.wheel_open = g_wheel_gate.busy();
    if (auto w = g_window_gate.current()) s.window = w->id;
    return s;
}

bool overlay_open_window(WindowRequest r, std::optional<WindowRequest>* replaced, std::string* why) {
    if (!g_model || !g_router) {
        if (why) *why = "the overlay is not running";
        return false;
    }
    const std::function<bool()> ready = [] { return vulkan_overlay_ready(); };  // judged under the gate's lock
    const auto refusal = g_window_gate.open(std::move(r), ready, window_hooked(), g_wheel_gate.busy(), replaced);
    if (refusal == WindowGate::Refusal::None) return true;
    static const char* const kWhy[] = {"", "the overlay cannot draw yet", "the game window is not hooked (no input)",
                                       "a wheel is open", "another window is open"};
    if (why) *why = kWhy[static_cast<int>(refusal)];
    return false;
}

bool overlay_update_window(const WindowRequest& contents) { return g_window_gate.update(contents); }

bool overlay_close_window(const std::string& id, std::optional<WindowRequest>* closed) {
    return g_window_gate.close(id, closed);
}

std::optional<WindowRequest> overlay_window() { return g_window_gate.current(); }

bool overlay_click_window(const std::string& key, bool right, std::string* why) {
    const auto w = g_window_gate.current();
    if (!w) {
        if (why) *why = "no window is open";
        return false;
    }
    for (const auto& x : w->widgets) {
        if (x.key != key || !widget_takes_clicks(x.type)) continue;
        if (x.flags & EMF_UI_DISABLED) {
            if (why) *why = key + " is disabled";
            return false;
        }
        if (g_deps.window_event) g_deps.window_event(*w, right ? EMF_UI_RIGHT_CLICK : EMF_UI_CLICK, key, 0);
        return true;
    }
    if (why) *why = "no button, row, slot or picture with the key " + key;
    return false;
}

bool overlay_close_window_by_player() {
    const auto r = g_window_gate.finish();
    window_closed(r, EMF_UI_BY_PLAYER);
    return r.has_value();
}

void overlay_set_status(const std::string& text) {
    AcquireSRWLockExclusive(&g_status_lock);
    g_status = text;
    ReleaseSRWLockExclusive(&g_status_lock);
}

}  // namespace emf::core
