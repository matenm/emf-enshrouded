#include "runtime_parts.h"

namespace emf::core::bridge {

void SlotBook::new_session() { known_.store(false); }

void SlotBook::loaded(uint16_t slot) {
    slot_.store(slot);
    known_.store(true);
}

void SlotBook::saved(uint16_t slot) { loaded(slot); }

std::optional<uint32_t> SlotBook::current(const std::optional<std::string>& world_kind) const {
    if (!known_.load()) return std::nullopt;
    if (world_kind && *world_kind == "client") return std::nullopt;
    return slot_.load();
}

bool StateBatch::add(Done done, uint64_t now_ms) {
    const std::lock_guard<std::mutex> l(m_);
    waiting_.push_back(std::move(done));
    if (posted_ && now_ms - posted_ms_ < kRepostMs) return false;
    posted_ = true;
    posted_ms_ = now_ms;
    return true;
}

void StateBatch::fill(const WorldValues& values) {
    const std::lock_guard<std::mutex> l(m_);
    for (auto& d : waiting_) ready_.emplace_back(std::move(d), values);
    waiting_.clear();
    posted_ = false;
}

std::vector<std::pair<StateBatch::Done, WorldValues>> StateBatch::take_ready() {
    const std::lock_guard<std::mutex> l(m_);
    std::vector<std::pair<Done, WorldValues>> out;
    out.swap(ready_);
    return out;
}

size_t StateBatch::waiting() const {
    const std::lock_guard<std::mutex> l(m_);
    return waiting_.size();
}

void PressBook::add(uint32_t vk, Done done) {
    const std::lock_guard<std::mutex> l(m_);
    presses_.emplace_back(vk, std::move(done));
}

PressBook::Done PressBook::take(uint32_t vk) {
    const std::lock_guard<std::mutex> l(m_);
    for (size_t i = 0; i < presses_.size(); ++i)
        if (presses_[i].first == vk) {
            Done d = std::move(presses_[i].second);
            presses_.erase(presses_.begin() + static_cast<std::ptrdiff_t>(i));
            return d;
        }
    return {};
}

size_t PressBook::size() const {
    const std::lock_guard<std::mutex> l(m_);
    return presses_.size();
}

Refusal mod_key_action(InjectedKeys& keys, PressBook& presses, uint32_t vk, KeyAction action, uint32_t hold_ms,
                       PressBook::Done done, uint64_t now_ms) {
    KeyResult r;
    r.ok = true;
    switch (action) {
        case KeyAction::Press:
            if (!keys.press(vk, now_ms, hold_ms))
                return Refusal{code::kBusy, "that key is held already (mod_key up releases it)"};
            presses.add(vk, std::move(done));
            return {};
        case KeyAction::Down:
            if (!keys.down(vk, now_ms)) return Refusal{code::kBusy, "that key is held already (mod_key up releases it)"};
            if (done) done(r);
            return {};
        case KeyAction::Up: {
            r.mod_reads = keys.up(vk);
            if (auto press = presses.take(vk)) press(r);  // a press of the same key ends here too
            if (done) done(r);
            return {};
        }
    }
    return Refusal{code::kBadRequest, "unknown key action"};
}

void release_due(InjectedKeys& keys, PressBook& presses, uint64_t now_ms) {
    for (const auto& [vk, reads] : keys.expire(now_ms)) {
        PressBook::Done done = presses.take(vk);
        if (!done) continue;  // a "down" held for the full minute: released, nobody waits
        KeyResult r;
        r.ok = true;
        r.mod_reads = reads;
        done(r);
    }
}

size_t answer_lines_without_world(CommandCenter& center, HostState state, const ReplyFn& sink) {
    if (state == HostState::Hosting || !center.pending()) return 0;
    return center.reject_pending("not run: no world you host is running any more", sink);
}

namespace {
struct WindowSearch {
    HWND best = nullptr;
    bool best_is_game = false;
};
BOOL CALLBACK window_of_ours(HWND w, LPARAM p) {
    auto* s = reinterpret_cast<WindowSearch*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    const bool game = wcscmp(cls, L"localWindowClass") == 0;
    if (!s->best || (game && !s->best_is_game)) {
        s->best = w;
        s->best_is_game = game;
    }
    return TRUE;
}
}  // namespace

HWND game_main_window() {
    WindowSearch s;
    EnumWindows(window_of_ours, reinterpret_cast<LPARAM>(&s));
    return s.best;
}

}  // namespace emf::core::bridge
