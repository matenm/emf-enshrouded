// villager_jobs: players who joined a dedicated server or someone else's game (pure logic, no game access). Their own
// Villager Jobs reads the jobs key and sends "key" over the mod channel villager_jobs/ui (EMF API v27); the server picks
// what that player looks at with the player's camera from its own world, runs the assignment flow for that player and
// answers with notes and wheels; the player's game opens a wheel and sends back the pick. A message is text, its fields
// split by line breaks:
//   player -> server: key <key name> | pick <number> <entry, -1 = cancelled>
//   server -> player: note <text> | wheel <number> <title> <entry>... (1 to 64 entries)
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "emf/emf_api.h"
#include "jobs_logic.h"

namespace jobs::remote {

constexpr const char* kChannel = "villager_jobs/ui";
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxMessage = 16384;  // bytes: a recipe wheel of 64 entries fits
constexpr size_t kMaxEntries = 64;       // a wheel's entries (EmfWheel.count)
constexpr size_t kMaxKeyName = 24;
constexpr size_t kMaxText = 512;         // a note, a title or an entry

struct Msg {
    enum Kind { Key, Pick, Note, Wheel } kind = Note;
    uint32_t seq = 0;
    int choice = -1;
    std::string text;                 // key: the key's name; note: the text; wheel: the title
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

inline std::string encode_key(const std::string& key_name) { return "key\n" + detail::one_line(key_name).substr(0, kMaxKeyName); }
inline std::string encode_pick(uint32_t seq, int choice) {
    return "pick\n" + std::to_string(seq) + "\n" + std::to_string(choice < 0 ? -1 : choice);
}
inline std::string encode_note(const std::string& text) { return "note\n" + detail::one_line(text); }
inline std::string encode_wheel(uint32_t seq, const std::string& title, const std::vector<std::string>& labels) {
    std::string out = "wheel\n" + std::to_string(seq) + "\n" + detail::one_line(title);
    for (size_t i = 0; i < labels.size() && i < kMaxEntries; ++i) out += "\n" + detail::one_line(labels[i]);
    return out;
}

// The message in `text`, or nothing when it is broken (an unknown kind, a bad number, too long, a wheel without entries).
inline std::optional<Msg> decode(const std::string& text) {
    if (text.empty() || text.size() > kMaxMessage) return std::nullopt;
    const auto f = detail::split(text);
    Msg m;
    if (f[0] == "key" && f.size() == 2 && !f[1].empty() && f[1].size() <= kMaxKeyName) {
        m.kind = Msg::Key;
        m.text = f[1];
        return m;
    }
    if (f[0] == "pick" && f.size() == 3 && detail::parse_u32(f[1], &m.seq)) {
        m.kind = Msg::Pick;
        if (f[2] == "-1") {
            m.choice = -1;
            return m;
        }
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

// What the server does with a joined player's pick (`choice`, -1 = cancelled) of wheel `seq`: Run its action (put in
// `*action`), say the wheel has Closed (kept open past kExpireMs, or a newer wheel came), Refuse it (`admin` is the
// sender's rights now: a player who lost them while the wheel was open picks nothing; the check at the key press is not
// enough), or nothing (Quiet: a cancel). The wheel is taken in every case.
enum class PickAnswer { Run, Closed, Refused, Quiet };
template <class Action>
PickAnswer take_pick(WheelDesk<Action>& desk, uint64_t account, uint32_t seq, int choice, bool admin, uint64_t now_ms,
                     std::optional<Action>* action) {
    auto pick = desk.take(account, seq, now_ms);
    if (!pick) return choice >= 0 ? PickAnswer::Closed : PickAnswer::Quiet;
    if (!admin) return choice >= 0 ? PickAnswer::Refused : PickAnswer::Quiet;
    *action = std::move(pick);
    return PickAnswer::Run;
}

// The player's game: a wheel from the server opens only within kWaitMs of this game's own key press or pick, and only
// once (a late answer would open a wheel the player no longer expects; Late: the game says so and cancels it).
class ClientGate {
public:
    static constexpr uint64_t kWaitMs = 2000;
    enum class Answer { Open, Late, NotAsked };
    void asked(uint64_t now_ms) { at_ = now_ms ? now_ms : 1; }
    void picked(uint64_t now_ms) { at_ = now_ms ? now_ms : 1; }
    Answer take_wheel(uint64_t now_ms) {
        const uint64_t at = at_;
        at_ = 0;
        if (!at) return Answer::NotAsked;
        return now_ms >= at && now_ms - at <= kWaitMs ? Answer::Open : Answer::Late;
    }

private:
    uint64_t at_ = 0;
};

// The server: a joined player's key presses count at most once per kMinGapMs (a held key or a macro would make the
// server search the world and answer for each one).
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
    size_t size() const { return last_.size(); }

private:
    std::map<uint64_t, uint64_t> last_;
};

// Who gives, changes and stops jobs: EMF's admins (`emf_admin`: the host, [chat] admins) and the game's own admins
// (`game_perms` has EMF_PERM_KICK_BAN: a dedicated server's Admin group, the players a host made admins; API v43).
inline bool may_give_jobs(bool emf_admin, uint32_t game_perms) { return emf_admin || (game_perms & EMF_PERM_KICK_BAN) != 0; }

// What a press does for a player who may not give jobs: a working villager's job, and that only admins give them.
inline std::string refused_press(const std::optional<Candidate>& target, const std::string* job_text) {
    if (target && target->kind == Kind::Villager && job_text)
        return "Villager " + std::to_string(target->id) + " " + *job_text + " (only the server's admins give and stop jobs)";
    return "Only the server's admins give villagers jobs";
}

}  // namespace jobs::remote
