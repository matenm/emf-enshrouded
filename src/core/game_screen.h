#pragma once
// The game's current HUD screen (keen::HudScreenType, one byte in UiPlayData; its offset from the HudScreenRead
// signature). While it is one of the game's text boxes (the chat, the social screen's chat pane, the rename box) mods'
// key reads (GetAsyncKeyState / GetKeyState from a mod) say every key is up, and the console key goes to the game, so
// typing a letter never fires a mod's hotkey. Read once per client tick (markers_client_tick); thread-safe.
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace emf::core {

constexpr uint8_t kScreenInGame = 1;     // playing, no menu
constexpr uint8_t kScreenTextChat = 43;  // the chat box (opened with Enter)
constexpr uint8_t kScreenSocial = 44;    // the social screen, which holds a second chat pane
constexpr uint8_t kScreenRename = 47;    // the rename box
constexpr uint64_t kGameScreenStaleMs = 500;  // a reading older than this says nothing (main menu, loading, a server)

// The screens whose text box takes the keys.
bool screen_takes_text(uint8_t screen);

class GameScreen {
public:
    void set_offset(uint32_t offset) { offset_.store(offset, std::memory_order_relaxed); }  // 0: unknown, no check
    uint32_t offset() const { return offset_.load(std::memory_order_relaxed); }
    void seen(uint8_t screen, uint64_t now_ms);
    // The byte at UiPlayData + offset (guarded): nothing when the offset is 0, `uipd` is null or the read faults.
    void read(const uint8_t* uipd, uint64_t now_ms);
    // The screen read within the last 500 ms, else nothing.
    std::optional<uint8_t> screen(uint64_t now_ms) const;
    // A text screen read within the last 500 ms, once a reading said InGame (a build whose byte means something
    // else never pauses every key).
    bool typing(uint64_t now_ms) const;

private:
    std::atomic<uint32_t> offset_{0};
    std::atomic<uint32_t> screen_{0};
    std::atomic<bool> armed_{false};  // a reading said InGame once: only then do text screens pause keys
    std::atomic<uint64_t> seen_ms_{0};
};

// Keys a mod asks about while the player types read as up; one that was down then stays up for mods until it is
// released, so the key that closes the chat (or a letter still held) is not a new press. Lock-free.
class TypingKeyGate {
public:
    // True when a mod asking about `vk` (physically `down`) is to be told it is up.
    bool hide(int vk, bool down, bool typing);
    bool any() const;  // some key is held up for mods since the player typed
    // The window saw vk go up (raw or key message): it counts again, whether or not a mod asks about it. Shift, Ctrl and
    // Alt clear their generic code and both sides.
    void released(int vk);

private:
    std::atomic<uint64_t> held_[4] = {};
};

// The HUD screen byte's offset in UiPlayData from the HudScreenRead match (the u32 at +4), or 0 when the match's compare
// (+11) is not with TextChat or the offset is not plausible.
uint32_t hud_screen_offset_at(const uint8_t* match);

// The console's `screen` line: the screen with the game's name for it, and whether mods' keys read as up.
std::string game_screen_line(std::optional<uint8_t> screen, uint32_t offset, bool server, bool key_hooks);

GameScreen& game_screen();
// One gate per API: GetAsyncKeyState (async) and GetKeyState (the thread's own view) can disagree on a held key.
TypingKeyGate& typing_gate(bool async);
// The window saw vk go up: both gates forget it.
void typing_key_released(int vk);
// The player types in one of the game's text boxes right now (GetTickCount64 clock).
bool game_typing_now();

}  // namespace emf::core
