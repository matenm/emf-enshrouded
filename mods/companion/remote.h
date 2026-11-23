// companion: players who joined a dedicated server or someone else's game (pure logic, no game access; GitHub #52).
// Their own Companion reads their keys and sends them over the mod channel companion/ui (EMF API v27); the server acts
// for that player on their own companion (owners.h) and answers with notes and wheels; the player's game opens a wheel
// and sends back the pick. The pattern is Villager Jobs' (mods/villager_jobs/remote_logic.h). A message is text, its
// fields split by line breaks:
//   player -> server: key <wheel|follow|stay|attack|dismiss> | pick <number> <entry, -1 = cancelled>
//   server -> player: note <text> | wheel <number> <title> <entry>... (1 to 64 entries)
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace companion::remote {

constexpr const char* kChannel = "companion/ui";
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxMessage = 8192;  // bytes: a gear wheel of 64 entries fits
constexpr size_t kMaxEntries = 64;      // a wheel's entries (EmfWheel.count)
constexpr size_t kMaxText = 160;        // a note, a title or an entry

// The keys a player's game sends: the order wheel key and the single order keys.
enum class Key { Wheel, Follow, Stay, Attack, Dismiss };
inline const char* key_name(Key k) {
    switch (k) {
        case Key::Wheel: return "wheel";
        case Key::Follow: return "follow";
        case Key::Stay: return "stay";
        case Key::Attack: return "attack";
        case Key::Dismiss: return "dismiss";
    }
    return "wheel";
}
inline std::optional<Key> key_from(const std::string& s) {
    for (Key k : {Key::Wheel, Key::Follow, Key::Stay, Key::Attack, Key::Dismiss})
        if (s == key_name(k)) return k;
    return std::nullopt;
}

struct Msg {
    enum Kind { KeyPress, Pick, Note, Wheel } kind = Note;
    Key key = Key::Wheel;
    uint32_t seq = 0;
    int choice = -1;
    std::string text;                 // note: the text; wheel: the title
    std::vector<std::string> labels;  // wheel: the entries
};

namespace detail {
inline std::string one_line(const std::string& s) {
    std::string out = s.substr(0, kMaxText);
    for (char& c : out)
        if (c == '\n' || c == '\r') c = ' ';
    return out;
}
inline std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t nl = s.find('\n', from);
        out.push_back(s.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (nl == std::string::npos) return out;
        from = nl + 1;
    }
}
inline bool parse_u32(const std::string& s, uint32_t* out) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}
}  // namespace detail

inline std::string encode_key(Key k) { return std::string("key\n") + key_name(k); }
inline std::string encode_pick(uint32_t seq, int choice) {
    return "pick\n" + std::to_string(seq) + "\n" + std::to_string(choice < 0 ? -1 : choice);
}
inline std::string encode_note(const std::string& text) { return "note\n" + detail::one_line(text); }
inline std::string encode_wheel(uint32_t seq, const std::string& title, const std::vector<std::string>& labels) {
    std::string out = "wheel\n" + std::to_string(seq) + "\n" + detail::one_line(title);
    for (size_t i = 0; i < labels.size() && i < kMaxEntries; ++i) out += "\n" + detail::one_line(labels[i]);
    return out;
}

// The message in `text`, or nothing when it is broken (an unknown kind or key, a bad number, too long, a wheel without
// entries).
inline std::optional<Msg> decode(const std::string& text) {
    if (text.empty() || text.size() > kMaxMessage) return std::nullopt;
    const auto f = detail::split(text);
    Msg m;
    if (f[0] == "key" && f.size() == 2) {
        const auto k = key_from(f[1]);
        if (!k) return std::nullopt;
        m.kind = Msg::KeyPress;
        m.key = *k;
        return m;
    }
    if (f[0] == "pick" && f.size() == 3 && detail::parse_u32(f[1], &m.seq)) {
        m.kind = Msg::Pick;
        if (f[2] == "-1") return m;
        uint32_t c = 0;
        if (!detail::parse_u32(f[2], &c) || c >= kMaxEntries) return std::nullopt;
        m.choice = static_cast<int>(c);
        return m;
    }
    if (f[0] == "note" && f.size() == 2 && f[1].size() <= kMaxText) {
        m.kind = Msg::Note;
        m.text = f[1];
        return m;
    }
    if (f[0] == "wheel" && f.size() >= 4 && f.size() - 3 <= kMaxEntries && detail::parse_u32(f[1], &m.seq) &&
        f[2].size() <= kMaxText) {
        m.kind = Msg::Wheel;
        m.text = f[2];
        for (size_t i = 3; i < f.size(); ++i) {
            if (f[i].size() > kMaxText) return std::nullopt;
            m.labels.push_back(f[i]);
        }
        return m;
    }
    return std::nullopt;
}

// The server's open wheels of players who joined: one per player (a new one replaces it), each with its number, taken
// by that player's pick of that number within kExpireMs.
template <class Action>
class WheelDesk {
public:
    static constexpr uint64_t kExpireMs = 30000;
    uint32_t open(uint64_t account, Action a, uint64_t now_ms) {
        const uint32_t seq = ++next_;
        open_[account] = Open{seq, now_ms, std::move(a)};
        return seq;
    }
    // The action of `account`'s wheel `seq` when it is still open (nothing otherwise); the wheel is taken either way.
    std::optional<Action> take(uint64_t account, uint32_t seq, uint64_t now_ms) {
        const auto it = open_.find(account);
        if (it == open_.end() || it->second.seq != seq) return std::nullopt;
        std::optional<Action> out;
        if (now_ms >= it->second.at && now_ms - it->second.at <= kExpireMs) out = std::move(it->second.action);
        open_.erase(it);
        return out;
    }
    void forget(uint64_t account) { open_.erase(account); }
    void clear() { open_.clear(); }
    size_t size() const { return open_.size(); }

private:
    struct Open {
        uint32_t seq;
        uint64_t at;
        Action action;
    };
    uint32_t next_ = 0;
    std::map<uint64_t, Open> open_;
};

// The player's game: a wheel from the server opens only within kWaitMs of this game's own key press or pick, and only
// once (a late answer would open a wheel the player no longer expects; Late: the game says so and cancels it).
class ClientGate {
public:
    static constexpr uint64_t kWaitMs = 2000;
    enum class Answer { Open, Late, NotAsked };
    void asked(uint64_t now_ms) { at_ = now_ms ? now_ms : 1; }
    Answer take_wheel(uint64_t now_ms) {
        const uint64_t at = at_;
        at_ = 0;
        if (!at) return Answer::NotAsked;
        return now_ms >= at && now_ms - at <= kWaitMs ? Answer::Open : Answer::Late;
    }

private:
    uint64_t at_ = 0;
};

// The server: a joined player's key presses count at most once per kMinGapMs (a held key or a macro).
class PressGate {
public:
    static constexpr uint64_t kMinGapMs = 300;
    bool allow(uint64_t account, uint64_t now_ms) {
        const auto it = last_.find(account);
        if (it != last_.end() && now_ms >= it->second && now_ms - it->second < kMinGapMs) return false;
        last_[account] = now_ms;
        return true;
    }
    void forget(uint64_t account) { last_.erase(account); }
    void clear() { last_.clear(); }

private:
    std::map<uint64_t, uint64_t> last_;
};

}  // namespace companion::remote
