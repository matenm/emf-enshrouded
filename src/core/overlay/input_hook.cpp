#include "input_hook.h"

#include "core/api.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "common/hooks.h"
#include "common/log.h"
#include "injected_keys.h"

namespace emf::core {
namespace {

InputRouter* g_router = nullptr;
InputCallbacks g_cb;
WNDPROC g_original = nullptr;
std::atomic<bool> g_window_hooked{false};
std::atomic<bool> g_raw_keyboard{false};  // the game gets keys as raw input: characters come from there too
std::atomic<bool> g_raw_mouse{false};
std::atomic<bool> g_legacy_keys{false};   // the window also gets WM_KEYDOWN/WM_CHAR: characters come from WM_CHAR
HeldKeys g_keys;  // key state as seen through raw input (window thread only), for ToUnicodeEx

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

// Characters for a raw key press (keyboards whose legacy WM_CHAR messages are suppressed by the game's raw input).
void raw_characters(const KeyEvent& e) {
    if (!e.down || !g_cb.console_char) return;
    UINT vk = e.vkey;
    if (!vk) vk = MapVirtualKeyW(e.scan | (e.extended ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    if (g_keys.down(VK_CONTROL) && !g_keys.down(VK_MENU)) return;  // Ctrl+key is a shortcut (AltGr = Ctrl+Alt is text)
    BYTE state[256];
    memcpy(state, g_keys.state(), sizeof(state));
    state[VK_CAPITAL] = (o_get_key_state ? o_get_key_state(VK_CAPITAL) : GetKeyState(VK_CAPITAL)) & 1;
    wchar_t buf[8] = {};
    const int n = ToUnicodeEx(vk, e.scan | (e.extended ? 0xE000 : 0), state, buf, 8, 0x4 /* keep dead-key state */,
                              GetKeyboardLayout(0));
    for (int i = 0; i < n; ++i)
        if (buf[i] >= 0x20 && g_router->character(buf[i]) == Route::Console) g_cb.console_char(buf[i]);
}

// Applies a key route. Returns true if the message is handled here (not passed to the game).
bool apply_key(const KeyEvent& e, Route r, bool raw) {
    switch (r) {
        case Route::Game: return false;
        case Route::Toggle:
            emf::logf("overlay: console %s", g_router->open() ? "opened" : "closed");
            if (g_cb.toggled) g_cb.toggled(g_router->open());
            return true;
        case Route::Console:
            if (g_cb.console_key) g_cb.console_key(e);
            if (raw && !g_legacy_keys) raw_characters(e);  // only when no WM_CHAR will follow
            return true;
        case Route::Both:
            if (g_cb.console_key) g_cb.console_key(e);
            return false;
        case Route::Swallow: return true;
    }
    return false;
}

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
                track_key(e);
                if (apply_key(e, g_router->key(e), true)) return DefWindowProcW(hwnd, msg, wp, lp);
            } else if (ri->header.dwType == RIM_TYPEMOUSE && g_router->capturing()) {
                g_raw_mouse = true;
                const RAWMOUSE& m = ri->data.mouse;
                const USHORT f = m.usButtonFlags;
                const float notches = (f & RI_MOUSE_WHEEL) ? static_cast<SHORT>(m.usButtonData) / float(WHEEL_DELTA) : 0.0f;
                const bool relative = (m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;
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
            if (g_raw_keyboard && !g_legacy_keys) {
                if (g_router->capturing()) return 0;
                break;
            }
            // The toggle's own dead key, or a dead key typed into the console (it composes with the next WM_CHAR).
            if (g_router->character(static_cast<wchar_t>(wp)) != Route::Game) return 0;
            break;
        }
        case WM_KILLFOCUS:
            g_keys.clear();  // keys held now are released in another window: none may stay "held" here
            g_router->focus_lost();
            if (g_cb.focus_lost) g_cb.focus_lost();
            break;
        case WM_CHAR:
        case WM_SYSCHAR: {
            if (g_raw_keyboard && !g_legacy_keys) {  // characters come from raw input
                if (g_router->capturing()) return 0;
                break;
            }
            const Route r = g_router->character(static_cast<wchar_t>(wp));
            if (r == Route::Console) {
                if (g_cb.console_char && wp >= 0x20) g_cb.console_char(static_cast<wchar_t>(wp));
                return 0;
            }
            if (r == Route::Swallow) return 0;
            break;
        }
        case WM_MOUSEWHEEL:
            if (g_router->capturing()) {
                const float notches = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
                if (!g_raw_mouse && g_router->open() && g_cb.console_wheel) g_cb.console_wheel(notches);
                if (!g_raw_mouse && !g_router->open() && g_cb.overlay_mouse) g_cb.overlay_mouse(0.0f, 0.0f, 0, notches);
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
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

bool window_hooked() { return g_window_hooked.load(); }

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

}  // namespace emf::core
