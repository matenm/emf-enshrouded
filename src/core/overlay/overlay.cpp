#include "overlay.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <memory>
#include <optional>

#include "console_ui.h"
#include "cursor_model.h"
#include "emf/generated/components.hpp"
#include "input_hook.h"
#include "mods_ui.h"
#include "submit.h"
#include "vk_overlay.h"
#include "wheel_gate.h"
#include "wheel_model.h"
#include "wheel_ui.h"

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
float g_mouse_speed = 1.0f;
// Mods' radial wheel: requested from any thread, opened and driven on the render thread.
WheelModel g_wheel;  // render thread
// The wheel's request state (any thread); input capture follows it.
WheelGate g_wheel_gate([](bool on) {
    if (g_router) g_router->set_wheel(on);
});

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

void setup_fonts(ImGuiIO& io, float height) {
    const float size = std::clamp(height / 54.0f, 13.0f, 40.0f);
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const std::wstring font = std::wstring(windir) + L"\\Fonts\\consola.ttf";
    if (GetFileAttributesW(font.c_str()) != INVALID_FILE_ATTRIBUTES &&
        io.Fonts->AddFontFromFileTTF(utf8(font).c_str(), size))
        return;
    ImFontConfig cfg;
    cfg.SizePixels = size;
    io.Fonts->AddFontDefault(&cfg);
}

void before_frame(ImGuiIO& io) {
    take_wheel_request();
    if (g_wheel_gate.take_cancel() && g_wheel.is_open()) {  // alt-tab while it was open: its keys' releases are gone
        g_wheel.cancel();
        finish_wheel(-1);
    }
    std::vector<InputEvent> events;
    AcquireSRWLockExclusive(&g_queue_lock);
    events.swap(g_queue);
    ReleaseSRWLockExclusive(&g_queue_lock);
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
                if (e.open) {
                    g_ui.focus_input = true;
                    g_cursor.set_display(io.DisplaySize);
                    io.AddMousePosEvent(g_cursor.pos().x, g_cursor.pos().y);
                } else {  // no hover or held button stays behind
                    io.AddMouseButtonEvent(0, false);
                    io.AddMouseButtonEvent(1, false);
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                }
                break;
            case InputEvent::Key:
                feed_key_event(g_mods_ui, e.key, io, ImGui::GetFrameCount());
                break;
            case InputEvent::FocusLost:  // alt-tab: the releases go to another window
                io.AddFocusEvent(false);  // ImGui drops every held key and button (once, in order with the keys)
                break;
            case InputEvent::Char: io.AddInputCharacterUTF16(static_cast<ImWchar16>(e.ch)); break;
            case InputEvent::Wheel: io.AddMouseWheelEvent(0.0f, e.wheel); break;
            case InputEvent::Mouse:  // the console's cursor (a wheel's input is handled above)
                if (!g_router->open()) break;
                g_cursor.set_display(io.DisplaySize);
                g_cursor.move(e.dx, e.dy, g_mouse_speed);
                io.AddMousePosEvent(g_cursor.pos().x, g_cursor.pos().y);
                if (e.buttons & RI_MOUSE_LEFT_BUTTON_DOWN) io.AddMouseButtonEvent(0, true);
                if (e.buttons & RI_MOUSE_LEFT_BUTTON_UP) io.AddMouseButtonEvent(0, false);
                if (e.buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) io.AddMouseButtonEvent(1, true);
                if (e.buttons & RI_MOUSE_RIGHT_BUTTON_UP) io.AddMouseButtonEvent(1, false);
                break;
        }
    }
    io.MouseDrawCursor = g_router->open();  // the game hides the system cursor
    // Slide: ~0.15 s down or up.
    const float target = g_router->open() ? 1.0f : 0.0f;
    const float step = io.DeltaTime * 7.0f;
    g_ui.open_fraction = g_ui.open_fraction < target ? (std::min)(target, g_ui.open_fraction + step)
                                                     : (std::max)(target, g_ui.open_fraction - step);
}

void refresh_sources() {
    if (g_sources.components.empty())
        for (const auto& c : emf::gen::kComponents) {
            const char* p = std::strrchr(c.name, ':');
            g_sources.components.emplace_back(p ? p + 1 : c.name);
        }
    g_sources.commands.clear();
    for (const auto& [name, help] : g_deps.center->list()) g_sources.commands.push_back(name);
    g_sources.mod_settings.clear();
    if (g_deps.mods)
        for (const auto& m : g_deps.mods->list()) {
            std::vector<std::string> keys;
            if (g_deps.settings)
                for (const auto& d : g_deps.settings->defs(m.id)) keys.push_back(d.key);
            g_sources.mod_settings.emplace_back(m.id, std::move(keys));
        }
    const uint64_t now = GetTickCount64();
    if (g_deps.item_names && (g_sources.items.empty() || now - g_items_refreshed > 10000)) {
        g_items_refreshed = now;
        auto items = g_deps.item_names();
        if (!items.empty()) g_sources.items = std::move(items);
    }
}

// A world is running (the HUD lines are current); on the main menu and loading screens none ticks.
bool world_live() {
    const uint64_t now = GetTickCount64();
    return g_deps.tracker &&
           (g_deps.tracker->latest(EMF_WORLD_CLIENT, now) || g_deps.tracker->latest(EMF_WORLD_SERVER, now));
}

void build(ImVec2 display) {
    if (g_router->open() || g_ui.open_fraction > 0.0f) refresh_sources();  // Tab completion: only for the console
    std::function<void()> mods_tab;
    if (g_deps.mods) mods_tab = [] { draw_mods_tab(g_deps.mods->list(), g_deps.settings, g_mods_ui); };
    AcquireSRWLockShared(&g_status_lock);
    g_ui.status = g_status;
    ReleaseSRWLockShared(&g_status_lock);
    draw_console(*g_model, g_ui, g_sources, display, [](const std::string& line) {
        submit_line(*g_deps.center, host_state(*g_deps.tracker, GetTickCount64()), line, g_deps.reply);
        g_model->save_history(g_history_path);
    }, mods_tab);
    if (g_ui.open_fraction <= 0.0f) {
        draw_toast(toast_lines(*g_model, ToastState{g_toast_from.load(), g_toast_until.load()}, GetTickCount64()),
                   display);
        if (g_deps.hud && show_hud(false, !g_deps.hud->empty(), world_live())) draw_hud(g_deps.hud->lines(), display);
    }
    draw_wheel(g_wheel, display);
}

void log_line(const char* s) {
    if (g_deps.log) g_deps.log(s);
}

}  // namespace

OverlayConfig read_overlay_config(const std::wstring& ini_path) {
    OverlayConfig c;
    c.enabled = GetPrivateProfileIntW(L"overlay", L"enabled", 1, ini_path.c_str()) != 0;
    c.chat_commands = GetPrivateProfileIntW(L"overlay", L"chat", 1, ini_path.c_str()) != 0;
    wchar_t speed[32] = {};
    GetPrivateProfileStringW(L"overlay", L"mouse_speed", L"", speed, 32, ini_path.c_str());
    if (speed[0]) {
        wchar_t* end = nullptr;
        const double v = std::wcstod(speed, &end);
        if (end && !*end && v >= 0.1 && v <= 10.0) c.mouse_speed = static_cast<float>(v);
    }
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"overlay", L"key", L"", text, 32, ini_path.c_str());
    if (text[0]) {
        wchar_t* end = nullptr;
        const unsigned long v = std::wcstoul(text, &end, 0);
        if (end && !*end && v >= 1 && v <= 0x7F) c.toggle_scan = static_cast<uint16_t>(v);
    }
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
    g_mouse_speed = config.mouse_speed;
    g_model = std::make_unique<ConsoleModel>();
    g_router = std::make_unique<InputRouter>(config.toggle_scan);
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
    const bool input = install_input_hooks(g_router.get(), std::move(cb));
    VkOverlayHooks hooks;
    hooks.visible = [] {
        return g_router->open() || g_ui.open_fraction > 0.0f || GetTickCount64() < g_toast_until.load() ||
               g_wheel_gate.busy() ||
               (g_deps.hud && show_hud(g_router->open(), !g_deps.hud->empty(), world_live()));
    };
    hooks.fonts = setup_fonts;
    hooks.before_frame = before_frame;
    hooks.build = build;
    hooks.turned_off = [] {  // never leave an invisible console or wheel holding the input
        g_router->set_open(false);
        g_wheel.cancel();
        hand_back(g_wheel_gate.turned_off(hide_held_key), -1);
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
    g_toast_until = GetTickCount64() + ms;
}

bool overlay_open_wheel(const WheelRequest& request) {
    if (!g_model || !g_router) return false;
    const std::function<bool()> ready = [] { return vulkan_overlay_ready(); };  // judged under the gate's lock
    const auto why = g_wheel_gate.request(request, ready, window_hooked(), g_router->open());
    if (why == WheelGate::Refusal::None) return true;
    static const char* const kWhy[] = {"", "the overlay cannot draw yet", "the game window is not hooked (no input)",
                                       "the console is open", "no entries", "another wheel is open"};
    log_line((std::string("wheel refused: ") + kWhy[static_cast<int>(why)]).c_str());
    return false;
}

void overlay_notify(const std::string& text) {
    if (!g_model) return;
    if (GetTickCount64() >= g_toast_until.load()) g_toast_from = g_model->version();  // a new toast, else join it
    g_toast_until = GetTickCount64() + 6000;
    g_model->add_line(text);
}

OverlayStatus overlay_status() {
    OverlayStatus s;
    if (!g_model || !g_router) return s;
    s.running = vulkan_overlay_ready();
    s.console_open = g_router->open();
    s.wheel_open = g_wheel_gate.busy();
    return s;
}

void overlay_set_status(const std::string& text) {
    AcquireSRWLockExclusive(&g_status_lock);
    g_status = text;
    ReleaseSRWLockExclusive(&g_status_lock);
}

}  // namespace emf::core
