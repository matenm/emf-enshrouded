#include "core/map_input.h"
#include "input_hook.h"

#include "core/api.h"

#include <windows.h>

#include <string>

#include <intrin.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "common/hooks.h"
#include "common/log.h"
#include "core/game_screen.h"
#include "injected_keys.h"

namespace emf::core {
namespace {

InputRouter* g_router = nullptr;
InputCallbacks g_cb;
WNDPROC g_original = nullptr;
std::atomic<bool> g_window_hooked{false};
std::atomic<bool> g_raw_keyboard{false};  // the game gets keys as raw input: characters come from there too
std::atomic<bool> g_raw_mouse{false};
std::atomic<bool> g_absolute_mouse{false};  // raw mouse input with MOUSE_MOVE_ABSOLUTE (remote desktop, a VM)
std::atomic<bool> g_legacy_keys{false};   // the window also gets WM_KEYDOWN/WM_CHAR: characters come from WM_CHAR
HeldKeys g_keys;  // key state as seen through raw input (window thread only), for ToUnicodeEx
KeyDebugLog g_keylog;  // [overlay] debug_keys=1 (window thread only)
void key_log(const std::string& line) {
    if (!line.empty()) emf::logf("%s", line.c_str());
}
CharSource g_chars;  // where the console's characters come from (window thread only)

SRWLOCK g_ranges_lock = SRWLOCK_INIT;
std::vector<std::pair<uintptr_t, uintptr_t>> g_ranges;

using KeyStateFn = SHORT(WINAPI*)(int);
KeyStateFn o_get_async_key_state = nullptr;
KeyStateFn o_get_key_state = nullptr;
std::atomic<bool> g_key_hooks{false};  // GetAsyncKeyState / GetKeyState are hooked

// The debug bridge's keys for mods (mod_key); null while the bridge is off.
std::atomic<InjectedKeys*> g_injected{nullptr};
using ForegroundFn = HWND(WINAPI*)();
ForegroundFn o_get_foreground_window = nullptr;
std::atomic<HWND> g_game_window{nullptr};

bool from_mod(void* return_address) {
    AcquireSRWLockShared(&g_ranges_lock);
    const bool yes = caller_is_mod(reinterpret_cast<uintptr_t>(return_address), g_ranges);
    ReleaseSRWLockShared(&g_ranges_lock);
    return yes;
}

// The game's window ("localWindowClass" of this process), or null.
HWND game_window() {
    if (HWND w = g_game_window.load()) return w;
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"localWindowClass", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) {
            g_game_window = w;
            return w;
        }
    }
    return nullptr;
}

// While the bridge holds a key for mods right now, their emf::keys::game_in_front() sees the game window in front.
HWND WINAPI hk_get_foreground_window() {
    InjectedKeys* inj = g_injected.load(std::memory_order_acquire);
    if (inj && inj->any() && inj->any_held(GetTickCount64()) && from_mod(_ReturnAddress()))
        if (HWND w = game_window()) return w;
    return o_get_foreground_window();
}

// Whether the game's window really is in front (not as mods are told while the bridge holds a key).
bool game_really_in_front() {
    const HWND fg = o_get_foreground_window ? o_get_foreground_window() : GetForegroundWindow();
    DWORD pid = 0;
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

SHORT WINAPI hk_get_async_key_state(int vk) {
    if (InjectedKeys* inj = g_injected.load(std::memory_order_acquire); inj && inj->any())
        if (from_mod(_ReturnAddress()))
            if (const auto s = injected_key_state(inj, vk, true, GetTickCount64(), true, game_really_in_front())) return *s;
    // the player types in the game's chat or rename box (game_screen.h): mods read every key as up, and a key held
    // then stays up for them until released
    if (const bool typing = game_typing_now(); (typing || typing_gate().any()) && from_mod(_ReturnAddress())) {
        const SHORT s = o_get_async_key_state(vk);
        if (typing_gate().hide(vk, (s & 0x8000) != 0, typing)) return 0;
    }
    if (g_router && (g_router->capturing() || g_router->hiding()) && from_mod(_ReturnAddress())) {
        if (g_router->capturing()) return 0;
        const SHORT s = o_get_async_key_state(vk);
        return g_router->hidden_from_mods(vk, (s & 0x8000) != 0) ? 0 : s;
    }
    return o_get_async_key_state(vk);
}

SHORT WINAPI hk_get_key_state(int vk) {
    if (InjectedKeys* inj = g_injected.load(std::memory_order_acquire); inj && inj->any())
        if (from_mod(_ReturnAddress()))
            if (const auto s = injected_key_state(inj, vk, true, GetTickCount64(), false, game_really_in_front())) return *s;
    // the player types in the game's chat or rename box (game_screen.h): mods read every key as up, and a key held
    // then stays up for them until released
    if (const bool typing = game_typing_now(); (typing || typing_gate().any()) && from_mod(_ReturnAddress())) {
        const SHORT s = o_get_key_state(vk);
        if (typing_gate().hide(vk, (s & 0x8000) != 0, typing)) return 0;
    }
    if (g_router && (g_router->capturing() || g_router->hiding()) && from_mod(_ReturnAddress())) {
        if (g_router->capturing()) return 0;
        const SHORT s = o_get_key_state(vk);
        return g_router->hidden_from_mods(vk, (s & 0x8000) != 0) ? 0 : s;
    }
    return o_get_key_state(vk);
}

void track_key(const KeyEvent& e) {
    UINT vk = e.vkey;
    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    g_keys.track(vk, e);
}

// Types characters into the console (those the router gives to it).
void type_chars(const std::wstring& chars) {
    for (wchar_t ch : chars)
        if (ch >= 0x20 && g_router->character(ch) == Route::Console && g_cb.console_char) g_cb.console_char(ch);
}

// A raw key's release: under Wine it may decide that characters come from raw input (CharSource).
void raw_release(const KeyEvent& e) {
    if (e.down) return;
    const bool was = g_chars.from_raw();
    type_chars(g_chars.raw_up(e.scan));
    if (!was && g_chars.from_raw())
        emf::logf("overlay: the keys came without character messages (as under Wine): the console's characters come "
                  "from raw input now");
}

// Characters for a raw key press: typed now when they come from raw input, else held until the window's own character
// message shows up (CharSource). repeat: the key was down already (auto-repeat: not logged).
void raw_characters(const KeyEvent& e, bool repeat) {
    if (!e.down || !g_cb.console_char) return;
    UINT vk = e.vkey;
    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    BYTE state[256];
    memcpy(state, g_keys.state(), sizeof(state));
    state[VK_CAPITAL] = (o_get_key_state ? o_get_key_state(VK_CAPITAL) : GetKeyState(VK_CAPITAL)) & 1;
    RawKeyInfo info;
    info.scan = e.scan;
    info.vk = vk;
    info.shift = g_keys.down(VK_SHIFT);
    info.ctrl = g_keys.down(VK_CONTROL);
    info.alt = g_keys.down(VK_MENU);
    info.caps = (state[VK_CAPITAL] & 1) != 0;
    if (info.ctrl && !info.alt) {  // Ctrl+key is a shortcut (AltGr = Ctrl+Alt is text)
        info.shortcut = true;
        if (!repeat) key_log(g_keylog.raw_key(info));
        return;
    }
    wchar_t buf[8] = {};
    const HKL layout = GetKeyboardLayout(0);
    // the plain scan code: its bit 15 would tell ToUnicodeEx the key is up (an extended key then gave nothing)
    info.n = ToUnicodeEx(vk, e.scan, state, buf, 8, 0x4 /* keep dead-key state */, layout);
    if (info.n > 0) info.proton.assign(buf, static_cast<size_t>(info.n));
    if (info.n == 0)  // nothing (< 0 is a dead key, which composes with the next key): the letter printed on the key
        info.fallback = fallback_chars(MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, layout), info.shift, info.caps, info.alt);
    const std::wstring& held = info.proton.empty() ? info.fallback : info.proton;
    info.typed = g_chars.raw_down(e.scan, held);
    type_chars(info.typed);
    bool printable = false;
    for (wchar_t ch : held) printable = printable || ch >= 0x20;
    info.waits = info.typed.empty() && printable && !g_chars.from_raw();
    if (!repeat) key_log(g_keylog.raw_key(info));
}
bool g_raw_repeat = false;  // the raw key being handled was down already (window thread only)

// Applies a key route. Returns true if the message is handled here (not passed to the game).
bool apply_key(const KeyEvent& e, Route r, bool raw) {
    switch (r) {
        case Route::Game: return false;
        case Route::Toggle:
            emf::logf("overlay: console %s", g_router->open() ? "opened" : "closed");
            if (g_router->open()) key_log(g_keylog.opened());
            if (!g_router->open()) g_chars.closed();  // what was held while deciding goes
            if (g_cb.toggled) g_cb.toggled(g_router->open());
            return true;
        case Route::Console:
            if (g_cb.console_key) g_cb.console_key(e);
            if (raw && g_router->open()) raw_characters(e, g_raw_repeat);  // typed now, or held until the WM_CHAR
            return true;
        case Route::Both:
            if (g_cb.console_key) g_cb.console_key(e);
            return false;
        case Route::Swallow: return true;
        case Route::CloseWindow:
            if (g_cb.close_window) g_cb.close_window();
            return true;
    }
    return false;
}

// The map gesture (map_input.h) for one mouse message on the game window.
MapInputResult map_click_result(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return map_input().message(msg, wp, lp,
                               map_input_env(static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top),
                                             GetTickCount64()));
}
bool map_click_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return map_click_result(hwnd, msg, wp, lp).swallow; }

LRESULT CALLBACK hook_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INPUT: {
            alignas(8) BYTE buf[256];
            UINT size = sizeof(buf);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) ==
                UINT(-1))
                break;
            const auto* ri = reinterpret_cast<const RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                if (!g_raw_keyboard.exchange(true)) emf::logf("overlay: the keyboard arrives as raw input");
                KeyEvent e;
                if (!decode_raw_keyboard(ri->data.keyboard, &e)) break;
                {
                    UINT vk = e.vkey;
                    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
                    g_raw_repeat = e.down && g_keys.down(vk);
                }
                track_key(e);
                raw_release(e);
                if (apply_key(e, g_router->key(e), true)) return DefWindowProcW(hwnd, msg, wp, lp);
            } else if (ri->header.dwType == RIM_TYPEMOUSE && g_router->capturing()) {
                g_raw_mouse = true;
                const RAWMOUSE& m = ri->data.mouse;
                const USHORT f = m.usButtonFlags;
                const float notches = (f & RI_MOUSE_WHEEL) ? static_cast<SHORT>(m.usButtonData) / float(WHEEL_DELTA) : 0.0f;
                const bool relative = (m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;
                if (!relative) g_absolute_mouse = true;
                const float dx = relative ? static_cast<float>(m.lLastX) : 0.0f;
                const float dy = relative ? static_cast<float>(m.lLastY) : 0.0f;
                if (!g_router->open() && g_router->wheel()) {  // a wheel: movement steers it
                    if (g_cb.overlay_mouse) g_cb.overlay_mouse(dx, dy, f, notches);
                } else {  // the console: the wheel scrolls, movement and buttons drive its cursor
                    if ((f & RI_MOUSE_WHEEL) && g_cb.console_wheel) g_cb.console_wheel(notches);
                    if (g_cb.overlay_mouse && (dx != 0.0f || dy != 0.0f || (f & ~(RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL))))
                        g_cb.overlay_mouse(dx, dy, f, 0.0f);
                }
                const USHORT ups = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                                   RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
                if (!(f & ups)) return DefWindowProcW(hwnd, msg, wp, lp);  // releases pass, everything else stays here
            } else if (ri->header.dwType == RIM_TYPEMOUSE) {
                g_raw_mouse = true;
                if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) g_absolute_mouse = true;
            }
            break;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            const KeyEvent e{static_cast<uint16_t>((lp >> 16) & 0xFF), static_cast<uint16_t>(wp),
                             msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, ((lp >> 24) & 1) != 0};
            if (!g_legacy_keys.exchange(true)) emf::logf("overlay: the game window also gets key messages (WM_KEYDOWN)");
            g_chars.legacy_seen();
            if (e.down && g_router->open()) key_log(g_keylog.key_message(e.scan, e.vkey, !g_raw_keyboard));
            // Raw input already decided this key; its key message copies that decision (no deciding twice).
            const Route r = g_raw_keyboard ? g_router->legacy_key(e) : g_router->key(e);
            // A dead toggle key (German ^) was stored by the game's TranslateMessage just now: clear it.
            if (e.down && e.scan == g_router->toggle_scan() && !e.extended && r != Route::Game)
                flush_dead_key(&ToUnicodeEx, static_cast<UINT>(wp), e.scan, GetKeyboardLayout(0));
            if (g_raw_keyboard) {
                if (r == Route::Swallow) return 0;
                break;
            }
            if (apply_key(e, r, false)) return 0;
            break;
        }
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR: {
            if (!g_chars.from_raw()) g_chars.wm_char();  // the window makes characters (whoever this one is for)
            if (g_raw_keyboard && g_chars.from_raw()) {
                if (g_router->capturing()) return 0;
                break;
            }
            // The toggle's own dead key, or a dead key typed into the console (it composes with the next WM_CHAR).
            if (g_router->character(static_cast<wchar_t>(wp)) != Route::Game) return 0;
            break;
        }
        case WM_ACTIVATEAPP:
            if (!wp) map_input().cancel();  // another program came to the front: a map hold ends
            break;
        case WM_KILLFOCUS:
            map_input().cancel();  // a map hold ends (its release stays EMF's)
            g_keys.clear();  // keys held now are released in another window: none may stay "held" here
            g_router->focus_lost();
            if (g_cb.focus_lost) g_cb.focus_lost();
            break;
        case WM_CHAR:
        case WM_SYSCHAR: {
            if (!g_chars.from_raw()) g_chars.wm_char();  // the window makes characters (whoever this one is for)
            if (g_raw_keyboard && g_chars.from_raw()) {  // characters come from raw input
                if (g_router->open()) key_log(g_keylog.character(static_cast<wchar_t>(wp), false));
                if (g_router->capturing()) return 0;
                break;
            }
            const Route r = g_router->character(static_cast<wchar_t>(wp));
            if (g_router->open()) key_log(g_keylog.character(static_cast<wchar_t>(wp), r == Route::Console && wp >= 0x20));
            if (r == Route::Console) {
                if (g_cb.console_char && wp >= 0x20) g_cb.console_char(static_cast<wchar_t>(wp));
                return 0;
            }
            if (r == Route::Swallow) return 0;
            break;
        }
        case WM_MOUSEWHEEL:
            if (!g_router->capturing() && map_click_message(hwnd, msg, wp, lp)) return 0;
            if (g_router->capturing()) {
                const float notches = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
                if (!g_raw_mouse && g_router->open() && g_cb.console_wheel) g_cb.console_wheel(notches);
                if (!g_raw_mouse && !g_router->open() && g_cb.overlay_mouse) g_cb.overlay_mouse(0.0f, 0.0f, 0, notches);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
            if (g_router->capturing()) {
                if (msg != WM_LBUTTONUP && msg != WM_RBUTTONUP) return 0;
                break;
            }
            // the map gesture of map actions (API v34): EMF's clicks never reach the game
            if (const MapInputResult r = map_click_result(hwnd, msg, wp, lp); r.swallow) {
                return 0;
            } else if (r.rewrite_down) {
                return CallWindowProcW(g_original, hwnd, WM_LBUTTONDOWN, wp, lp);
            } else if (r.rewrite_right_down) {
                return CallWindowProcW(g_original, hwnd, WM_RBUTTONDOWN, wp, lp);
            }
            break;
        case WM_MOUSEMOVE:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (g_router->capturing()) return 0;
            break;
        default: break;
    }
    return CallWindowProcW(g_original, hwnd, msg, wp, lp);
}

DWORD WINAPI find_window_thread(void*) {
    for (int tries = 0; tries < 20 * 60 * 10; ++tries) {  // up to 10 minutes
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, L"localWindowClass", nullptr)) != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId()) continue;
            g_original = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hook_proc)));
            g_window_hooked = g_original != nullptr;
            g_game_window = w;
            emf::logf("overlay: game window %p hooked (%s)", static_cast<void*>(w), g_original ? "ok" : "FAILED");
            return 0;
        }
        Sleep(50);
    }
    emf::logf("overlay: the game window never appeared; the console cannot take keyboard input");
    return 0;
}

// Hooks GetAsyncKeyState and GetKeyState once (the overlay and the debug bridge both need them). A hook is never
// installed twice: a second hook through the table would chain onto our own detour.
bool ensure_key_state_hooks() {
    static std::mutex m;
    static bool async_ok = false, state_ok = false;
    const std::lock_guard<std::mutex> l(m);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!async_ok) {
        const auto async = reinterpret_cast<uintptr_t>(GetProcAddress(user32, "GetAsyncKeyState"));
        async_ok = async && install_emf_hook("GetAsyncKeyState (overlay)", async,
                                             reinterpret_cast<void*>(&hk_get_async_key_state),
                                             reinterpret_cast<void**>(&o_get_async_key_state));
    }
    if (!state_ok) {
        const auto state = reinterpret_cast<uintptr_t>(GetProcAddress(user32, "GetKeyState"));
        state_ok = state && install_emf_hook("GetKeyState (overlay)", state, reinterpret_cast<void*>(&hk_get_key_state),
                                             reinterpret_cast<void**>(&o_get_key_state));
    }
    g_key_hooks = async_ok && state_ok;
    return g_key_hooks;
}

}  // namespace

void set_key_debug(bool on) { g_keylog.enable(on); }

bool window_hooked() { return g_window_hooked.load(); }

WindowsPointer read_windows_pointer() {
    WindowsPointer out;
    const HWND w = game_window();
    if (!w) return out;
    out.in_front = game_really_in_front();
    CURSORINFO ci{};
    ci.cbSize = sizeof ci;
    out.showing = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
    POINT p{};
    RECT r{};
    if (GetCursorPos(&p) && ScreenToClient(w, &p) && GetClientRect(w, &r)) {
        out.x = p.x;
        out.y = p.y;
        out.client_w = r.right - r.left;
        out.client_h = r.bottom - r.top;
    }
    return out;
}

bool absolute_mouse_seen() { return g_absolute_mouse.load(); }

InjectedKeyHooks install_injected_keys(InjectedKeys* keys) {
    if (!keys) return InjectedKeyHooks::None;
    if (!ensure_key_state_hooks()) return InjectedKeyHooks::None;
    static std::mutex m;
    static bool fg_ok = false;
    const std::lock_guard<std::mutex> l(m);
    if (!fg_ok) {
        const auto fg = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow"));
        fg_ok = fg && install_emf_hook("GetForegroundWindow (debug bridge)", fg,
                                       reinterpret_cast<void*>(&hk_get_foreground_window),
                                       reinterpret_cast<void**>(&o_get_foreground_window));
    }
    g_injected.store(keys, std::memory_order_release);  // the hooks read the keys from now on
    return fg_ok ? InjectedKeyHooks::Full : InjectedKeyHooks::NeedFocus;
}

void set_mod_ranges(std::vector<std::pair<uintptr_t, uintptr_t>> ranges) {
    AcquireSRWLockExclusive(&g_ranges_lock);
    g_ranges = std::move(ranges);
    ReleaseSRWLockExclusive(&g_ranges_lock);
}

bool install_input_hooks(InputRouter* router, InputCallbacks callbacks) {
    if (!router || g_router) return false;
    g_router = router;
    g_cb = std::move(callbacks);
    const bool hooked = ensure_key_state_hooks();
    if (!hooked) emf::logf("overlay: could not hook GetAsyncKeyState/GetKeyState; mods' hotkeys may fire while typing");
    if (HANDLE t = CreateThread(nullptr, 0, find_window_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return hooked;
}

HWND input_game_window() { return game_window(); }
bool input_capturing() { return g_router && g_router->capturing(); }

}  // namespace emf::core
