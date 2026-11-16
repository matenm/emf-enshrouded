// The chat key gate (game_screen.h).
#include "core/game_screen.h"

#include <windows.h>

#include <cstring>

#include "common/seh.h"

namespace emf::core {

bool screen_takes_text(uint8_t screen) {
    return screen == kScreenTextChat || screen == kScreenSocial || screen == kScreenRename;
}

void GameScreen::seen(uint8_t screen, uint64_t now_ms) {
    screen_.store(screen, std::memory_order_relaxed);
    seen_ms_.store(now_ms ? now_ms : 1, std::memory_order_release);
}

void GameScreen::read(const uint8_t* uipd, uint64_t now_ms) {
    const uint32_t off = offset();
    uint8_t screen = 0;
    if (!off || !uipd || !emf::seh_read(uipd + off, &screen, 1)) return;
    seen(screen, now_ms);
}

std::optional<uint8_t> GameScreen::screen(uint64_t now_ms) const {
    const uint64_t at = seen_ms_.load(std::memory_order_acquire);
    if (!at || now_ms < at || now_ms - at > kGameScreenStaleMs) return std::nullopt;
    return static_cast<uint8_t>(screen_.load(std::memory_order_relaxed));
}

bool GameScreen::typing(uint64_t now_ms) const {
    const auto s = screen(now_ms);
    return s && screen_takes_text(*s);
}

bool TypingKeyGate::hide(int vk, bool down, bool typing) {
    const bool marks = vk > 0 && vk < 256;
    std::atomic<uint64_t>* word = marks ? &held_[vk >> 6] : nullptr;
    const uint64_t bit = marks ? uint64_t{1} << (vk & 63) : 0;
    if (typing) {
        if (marks && down) word->fetch_or(bit, std::memory_order_relaxed);
        return true;
    }
    if (!marks || !(word->load(std::memory_order_relaxed) & bit)) return false;
    if (down) return true;               // held since the player typed: still not a press
    word->fetch_and(~bit, std::memory_order_relaxed);  // released: it counts again
    return false;
}

bool TypingKeyGate::any() const {
    for (const auto& w : held_)
        if (w.load(std::memory_order_relaxed)) return true;
    return false;
}

uint32_t hud_screen_offset_at(const uint8_t* match) {
    uint8_t code[12];
    if (!match || !emf::seh_read(match, code, sizeof code) || code[11] != kScreenTextChat) return 0;
    uint32_t off = 0;
    std::memcpy(&off, code + 4, sizeof off);
    return off > 0 && off < 0x10000 ? off : 0;  // UiPlayData is some KB, not more
}

GameScreen& game_screen() {
    static GameScreen g;
    return g;
}

TypingKeyGate& typing_gate() {
    static TypingKeyGate g;
    return g;
}

bool game_typing_now() { return game_screen().typing(GetTickCount64()); }

}  // namespace emf::core
