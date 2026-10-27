#pragma once
// Server Herald's rules without the game (tests: tests/test_herald.cpp): who joined and left, the announcements in turn,
// the restart countdown, playtime per account, finding a player by "#k" or name, the chat log's lines, and the Discord
// webhook's URL filter, message body, rate-limited queue and worker thread. Nothing here touches the game, files or the
// network (the worker posts through a Transport it is given).
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace herald {

// ---- text helpers ----
inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// Control characters (CR, LF, ESC...) become '?': a player's text can never start a new log line or a chat line.
inline std::string printable(std::string s) {
    for (auto& c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = '?';
    }
    return s;
}
// "<prefix> <text>", or the text alone when the prefix is empty.
inline std::string with_prefix(const std::string& prefix, const std::string& text) {
    return prefix.empty() ? text : prefix + " " + text;
}
// "45 s", "12 min", "3 h 05 min".
inline std::string duration_text(uint64_t seconds) {
    char b[48];
    if (seconds < 60) std::snprintf(b, sizeof b, "%llu s", static_cast<unsigned long long>(seconds));
    else if (seconds < 3600) std::snprintf(b, sizeof b, "%llu min", static_cast<unsigned long long>(seconds / 60));
    else
        std::snprintf(b, sizeof b, "%llu h %02llu min", static_cast<unsigned long long>(seconds / 3600),
                      static_cast<unsigned long long>((seconds % 3600) / 60));
    return b;
}
inline std::string account_text(uint64_t account) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%016llx", static_cast<unsigned long long>(account));
    return b;
}
// The MOTD with {name} and {online} filled in.
inline std::string expand_motd(std::string motd, const std::string& name, size_t online) {
    auto replace = [&motd](const std::string& key, const std::string& value) {
        for (size_t at = motd.find(key); at != std::string::npos; at = motd.find(key, at + value.size()))
            motd.replace(at, key.size(), value);
    };
    replace("{name}", name);
    replace("{online}", std::to_string(online));
    return motd;
}
inline std::string player_label(const std::optional<std::string>& name, uint32_t entity) {
    return name && !name->empty() ? printable(*name) : "player #" + std::to_string(entity);
}

// ---- who is online ----
// One player entity of the server world as the herald saw it this second.
struct Seen {
    uint32_t entity = 0;
    uint64_t account = 0;  // 0: not known yet
    bool ready = false;    // PlayerInput.fromClientIsReadyForGameplay
    std::string name;      // as the game knows it, or "player #<entity>"
};
struct Online {
    uint32_t entity = 0;
    uint64_t account = 0;
    std::string name;
    uint64_t joined_ms = 0;
    uint64_t motd_at_ms = 0;  // when the MOTD is due (again, after a failed tell)
    bool motd_sent = false;   // sent, or given up after kMotdTries failed tells
    int motd_tries = 0;
};
constexpr int kMotdTries = 10;  // failed tells (the chat limit when many join at once) before a MOTD is given up
struct Change {
    enum Kind { Joined, Left } kind = Joined;
    uint32_t entity = 0;
    uint64_t account = 0;
    std::string name;
    uint64_t session_ms = 0;  // Left: how long they were online
    size_t online = 0;        // players online after the change
};

// A player joins once their entity is there with an account and says it is ready for gameplay; they leave when that
// entity is gone or holds another account (a reconnect is a leave and a join: entity ids change). Lowest entity first.
class Roster {
public:
    std::vector<Change> update(const std::vector<Seen>& seen, uint64_t now_ms, uint64_t motd_delay_ms) {
        std::vector<Change> out;
        for (size_t i = 0; i < online_.size();) {
            const Online& o = online_[i];
            const bool still = std::any_of(seen.begin(), seen.end(), [&](const Seen& s) {
                return s.entity == o.entity && s.account == o.account;
            });
            if (still) {
                ++i;
                continue;
            }
            Change c{Change::Left, o.entity, o.account, o.name, now_ms - o.joined_ms, 0};
            online_.erase(online_.begin() + static_cast<std::ptrdiff_t>(i));
            c.online = online_.size();
            out.push_back(c);
        }
        for (const Seen& s : seen) {
            if (!s.ready || !s.account || !s.entity) continue;
            auto it = std::find_if(online_.begin(), online_.end(), [&](const Online& o) { return o.entity == s.entity; });
            if (it != online_.end()) {
                if (!s.name.empty()) it->name = s.name;  // a name the game knows later replaces "player #k"
                continue;
            }
            Online o{s.entity, s.account, s.name, now_ms, now_ms + motd_delay_ms, false, 0};
            online_.push_back(o);
            std::sort(online_.begin(), online_.end(), [](const Online& a, const Online& b) { return a.entity < b.entity; });
            out.push_back({Change::Joined, s.entity, s.account, s.name, 0, online_.size()});
        }
        return out;
    }
    // Players whose MOTD is due now. Tell motd_result how each went: a MOTD counts as sent only when its tell was.
    std::vector<Online> motd_due(uint64_t now_ms) const {
        std::vector<Online> due;
        for (const auto& o : online_)
            if (!o.motd_sent && now_ms >= o.motd_at_ms) due.push_back(o);
        return due;
    }
    // The tell of `entity`'s MOTD went (sent) or not: a failed one is due again a second later, up to kMotdTries times.
    // True when it was given up just now (the caller logs it).
    bool motd_result(uint32_t entity, bool sent, uint64_t now_ms) {
        for (auto& o : online_) {
            if (o.entity != entity || o.motd_sent) continue;
            if (sent) {
                o.motd_sent = true;
                return false;
            }
            if (++o.motd_tries >= kMotdTries) {
                o.motd_sent = true;
                return true;
            }
            o.motd_at_ms = now_ms + 1000;
            return false;
        }
        return false;
    }
    const std::vector<Online>& online() const { return online_; }
    const Online* by_account(uint64_t account) const {
        for (const auto& o : online_)
            if (o.account == account) return &o;
        return nullptr;
    }
    void clear() { online_.clear(); }

private:
    std::vector<Online> online_;
};

// "#1 Alice 25 min, #2 Bob 3 min" in lines of at most `width` characters, after "N online:".
inline std::vector<std::string> players_lines(const std::vector<Online>& online, uint64_t now_ms, size_t width = 160) {
    std::vector<std::string> lines;
    if (online.empty()) {
        lines.push_back("nobody is online");
        return lines;
    }
    lines.push_back(std::to_string(online.size()) + " online:");
    std::string line;
    for (size_t i = 0; i < online.size(); ++i) {
        const Online& o = online[i];
        const std::string item = "#" + std::to_string(i + 1) + " " + printable(o.name) + " (" +
                                 duration_text((now_ms - o.joined_ms) / 1000) + ")";
        if (!line.empty() && line.size() + 2 + item.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : ", ") + item;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// ---- a player by "#k" or by name ----
struct Target {
    uint32_t entity = 0;
    std::string name;
    size_t words = 0;   // how many of the words named the player (the rest is the message)
    std::string error;  // why none was found
};
// words: what follows the command ("#2 hello", "Player Two hello", "pla hello"). "#k" counts in /players order; else
// the longest run of words that is exactly a name (case ignored; names may hold spaces), else a unique name start.
inline Target find_target(const std::vector<Online>& online, const std::vector<std::string>& words) {
    Target t;
    if (words.empty() || words[0].empty()) {
        t.error = "say who: #k (as /players numbers them) or a name";
        return t;
    }
    if (words[0][0] == '#') {
        char* end = nullptr;
        const long k = std::strtol(words[0].c_str() + 1, &end, 10);
        if (end && *end == 0 && k >= 1 && static_cast<size_t>(k) <= online.size()) {
            t.entity = online[static_cast<size_t>(k - 1)].entity;
            t.name = online[static_cast<size_t>(k - 1)].name;
            t.words = 1;
            return t;
        }
        t.error = "no player " + printable(words[0]) + " (" + std::to_string(online.size()) + " online; /players lists them)";
        return t;
    }
    for (size_t n = words.size(); n >= 1; --n) {
        std::string joined;
        for (size_t i = 0; i < n; ++i) joined += (i ? " " : "") + words[i];
        const std::string want = lower(joined);
        for (const auto& o : online)
            if (lower(o.name) == want) {
                t.entity = o.entity;
                t.name = o.name;
                t.words = n;
                return t;
            }
    }
    const std::string start = lower(words[0]);
    std::vector<const Online*> fits;
    for (const auto& o : online)
        if (lower(o.name).rfind(start, 0) == 0) fits.push_back(&o);
    if (fits.size() == 1) {
        t.entity = fits[0]->entity;
        t.name = fits[0]->name;
        t.words = 1;
        return t;
    }
    if (fits.empty()) {
        t.error = "no player named '" + printable(words[0]) + "' is online (/players lists them)";
        return t;
    }
    t.error = "'" + printable(words[0]) + "' fits several players:";
    for (size_t i = 0; i < fits.size(); ++i) t.error += (i ? ", " : " ") + printable(fits[i]->name);
    t.error += " (use #k from /players)";
    return t;
}

// ---- announcements ----
// The lines of announcements.txt: one per line, empty lines and lines starting with '#' skipped, CR and spaces trimmed.
inline std::vector<std::string> parse_announcements(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (out.empty() && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);  // a UTF-8 BOM (Notepad)
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        line.erase(0, lead);
        if (!line.empty() && line[0] != '#') out.push_back(printable(line));
        at = end + 1;
    }
    return out;
}

// Sends the lines in turn, one every interval while at least one player is online. The clock starts when someone is
// online (a full interval first); while nobody is, it waits.
class Announcer {
public:
    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        if (next_ >= lines_.size()) next_ = 0;
    }
    std::optional<std::string> tick(uint64_t now_ms, uint64_t interval_ms, bool anyone_online) {
        if (!interval_ms || lines_.empty() || !anyone_online) {
            started_ = false;
            return std::nullopt;
        }
        if (!started_) {
            started_ = true;
            since_ms_ = now_ms;
            return std::nullopt;
        }
        if (now_ms - since_ms_ < interval_ms) return std::nullopt;
        since_ms_ = now_ms;
        const std::string line = lines_[next_];
        next_ = (next_ + 1) % lines_.size();
        return line;
    }
    size_t count() const { return lines_.size(); }

private:
    std::vector<std::string> lines_;
    size_t next_ = 0;
    bool started_ = false;  // false: the clock waits for someone online
    uint64_t since_ms_ = 0;
};

// ---- the restart countdown ----
inline std::string restart_text(uint64_t remaining_s, const std::string& reason) {
    std::string when;
    if (remaining_s == 0) when = "Server restarting now";
    else if (remaining_s % 60 == 0)
        when = "Server restart in " + std::to_string(remaining_s / 60) + (remaining_s == 60 ? " minute" : " minutes");
    else if (remaining_s > 60)
        when = "Server restart in " + std::to_string(remaining_s / 60) + " min " + std::to_string(remaining_s % 60) + " s";
    else
        when = "Server restart in " + std::to_string(remaining_s) + (remaining_s == 1 ? " second" : " seconds");
    return reason.empty() ? when : when + ": " + reason;
}

// herald.restart: a line at once, then at 10, 5, 2 and 1 minutes and 30 seconds left (only marks below the start), then
// "restarting now" (and the save) at 0. The real stop is outside EMF.
class Countdown {
public:
    struct Event {
        std::string text;
        bool now = false;  // the countdown reached 0 (save, if set)
    };
    static constexpr uint64_t kMarks[] = {600, 300, 120, 60, 30};
    // Starts (or restarts) the countdown; the line to send at once.
    std::string start(uint64_t total_s, std::string reason, uint64_t now_ms) {
        running_ = true;
        end_ms_ = now_ms + total_s * 1000;
        reason_ = printable(std::move(reason));
        marks_.clear();
        for (uint64_t m : kMarks)
            if (m < total_s) marks_.push_back(m);
        return restart_text(total_s, reason_);
    }
    std::vector<Event> tick(uint64_t now_ms) {
        std::vector<Event> out;
        if (!running_) return out;
        const uint64_t left_ms = now_ms >= end_ms_ ? 0 : end_ms_ - now_ms;
        std::optional<uint64_t> passed;  // after a stall several marks may pass at once: only the last is said
        while (!marks_.empty() && left_ms <= marks_.front() * 1000) {
            passed = marks_.front();
            marks_.erase(marks_.begin());
        }
        if (passed && left_ms > 0) out.push_back({restart_text(*passed, reason_), false});
        if (left_ms == 0) {
            running_ = false;
            out.push_back({restart_text(0, reason_), true});
        }
        return out;
    }
    bool cancel() {
        const bool was = running_;
        running_ = false;
        marks_.clear();
        return was;
    }
    bool running() const { return running_; }
    uint64_t remaining_s(uint64_t now_ms) const { return !running_ || now_ms >= end_ms_ ? 0 : (end_ms_ - now_ms + 999) / 1000; }
    const std::string& reason() const { return reason_; }

private:
    bool running_ = false;
    uint64_t end_ms_ = 0;
    std::string reason_;
    std::vector<uint64_t> marks_;
};
// "10", "2.5", "0.75" minutes -> seconds (1 s .. 24 h); nothing for anything else.
inline std::optional<uint64_t> parse_minutes(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const double m = std::strtod(text.c_str(), &end);
    if (!end || *end != 0 || !(m > 0) || m > 24 * 60) return std::nullopt;
    const auto s = static_cast<uint64_t>(m * 60 + 0.5);
    if (s == 0) return std::nullopt;
    return s;
}

// ---- playtime ----
struct PlayRecord {
    uint64_t total_ms = 0;
    int64_t first_seen = 0;  // Unix time (s)
    int64_t last_seen = 0;
    std::string name;
};

// Name text in the save line: ';', '%', control characters and DEL become %XX, so a name can never break a line.
inline std::string escape_field(const std::string& s) {
    std::string out;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == ';' || c == '%') {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", u);
            out += b;
        } else {
            out += c;
        }
    }
    return out;
}
inline std::string unescape_field(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 3 <= s.size()) {
            const std::string hex = s.substr(i + 1, 2);
            char* end = nullptr;
            const bool digits = std::isxdigit(static_cast<unsigned char>(hex[0])) && std::isxdigit(static_cast<unsigned char>(hex[1]));
            const long v = digits ? std::strtol(hex.c_str(), &end, 16) : -1;
            if (v >= 0 && end && *end == 0) {
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Seconds online per account on one world, with first and last seen and the last name. Saved as one line per account:
// "0x<account>;<total s>;<first>;<last>;<name>".
class Playtime {
public:
    void add(uint64_t account, uint64_t ms, int64_t now_unix, const std::string& name) {
        if (!account) return;
        PlayRecord& r = records_[account];
        r.total_ms += ms;
        if (!r.first_seen) r.first_seen = now_unix;
        r.last_seen = now_unix;
        if (!name.empty()) r.name = name;
    }
    const PlayRecord* find(uint64_t account) const {
        auto it = records_.find(account);
        return it == records_.end() ? nullptr : &it->second;
    }
    // 1-based place by total time (ties: the lower account first), 0 when unknown.
    size_t rank(uint64_t account) const {
        const PlayRecord* me = find(account);
        if (!me) return 0;
        size_t place = 1;
        for (const auto& [a, r] : records_)
            if (r.total_ms / 1000 > me->total_ms / 1000 || (r.total_ms / 1000 == me->total_ms / 1000 && a < account)) ++place;
        return place;
    }
    size_t size() const { return records_.size(); }
    // The account whose last name is `name` (case ignored), else the only one whose name starts with it; 0 if none or
    // several.
    uint64_t by_name(const std::string& name) const {
        const std::string want = lower(name);
        if (want.empty()) return 0;
        for (const auto& [a, r] : records_)
            if (lower(r.name) == want) return a;
        uint64_t found = 0;
        for (const auto& [a, r] : records_)
            if (lower(r.name).rfind(want, 0) == 0) {
                if (found) return 0;
                found = a;
            }
        return found;
    }
    std::string text() const {
        std::string out;
        char b[96];
        for (const auto& [a, r] : records_) {
            std::snprintf(b, sizeof b, "0x%016llx;%llu;%lld;%lld;", static_cast<unsigned long long>(a),
                          static_cast<unsigned long long>(r.total_ms / 1000), static_cast<long long>(r.first_seen),
                          static_cast<long long>(r.last_seen));
            out += b;
            out += escape_field(r.name);
            out += '\n';
        }
        return out;
    }
    // Adds the records in `text` to these (totals summed, the earliest first seen, the latest last seen and its name).
    // Lines that do not parse are skipped.
    void merge_text(const std::string& text) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;
            size_t p = 0;
            for (int i = 0; i < 4; ++i) {
                const size_t semi = line.find(';', p);
                if (semi == std::string::npos) break;
                f.push_back(line.substr(p, semi - p));
                p = semi + 1;
            }
            if (f.size() != 4) continue;
            f.push_back(line.substr(p));
            char* e0 = nullptr;
            const unsigned long long account = std::strtoull(f[0].c_str(), &e0, 16);
            if (!account || !e0 || *e0) continue;
            PlayRecord r;
            r.total_ms = std::strtoull(f[1].c_str(), nullptr, 10) * 1000;
            r.first_seen = std::strtoll(f[2].c_str(), nullptr, 10);
            r.last_seen = std::strtoll(f[3].c_str(), nullptr, 10);
            r.name = unescape_field(f[4]);
            PlayRecord& mine = records_[account];
            mine.total_ms += r.total_ms;
            if (r.first_seen && (!mine.first_seen || r.first_seen < mine.first_seen)) mine.first_seen = r.first_seen;
            if (r.last_seen > mine.last_seen) {
                mine.last_seen = r.last_seen;
                if (!r.name.empty()) mine.name = r.name;
            }
            if (mine.name.empty()) mine.name = r.name;
        }
    }
    void clear() { records_.clear(); }

private:
    std::map<uint64_t, PlayRecord> records_;
};

// ---- the chat log ----
inline std::string chat_log_name(int year, int month, int day) {
    char b[40];
    std::snprintf(b, sizeof b, "chat-%04d-%02d-%02d.log", year, month, day);
    return b;
}
// "HH:MM:SS 0x<account> <name>: <text>"
inline std::string chat_log_line(int h, int m, int s, uint64_t account, const std::string& name, const std::string& text) {
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d ", h, m, s);
    return b + account_text(account) + " " + printable(name) + ": " + printable(text);
}
// "HH:MM:SS -- <what>" (joins, leaves, deaths, the herald's own lines)
inline std::string chat_log_event(int h, int m, int s, const std::string& what) {
    char b[32];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d -- ", h, m, s);
    return b + printable(what);
}

// The chat log's lines wait here and are written together: once per server step (one open of each day's file), at
// once when kMaxLines wait, and at the game's exit. A flood of chat lines then costs one write per step, not an open,
// write and close of the file for each line on the server world's thread.
class ChatLogBuffer {
public:
    static constexpr size_t kMaxLines = 64;
    // A line for `file` (the day's file name); true when kMaxLines wait: write them now.
    bool add(const std::string& file, const std::string& line) {
        std::lock_guard<std::mutex> l(m_);
        if (batches_.empty() || batches_.back().first != file) batches_.push_back({file, std::string()});
        batches_.back().second += line + "\r\n";
        return ++lines_ >= kMaxLines;
    }
    // What waits, as (file, text with CRLF line ends) in the order the lines came; empty after.
    std::vector<std::pair<std::string, std::string>> take() {
        std::lock_guard<std::mutex> l(m_);
        return take_locked();
    }
    // The same, but only if no one holds the buffer (at the game's exit a thread may have been ended holding it).
    std::vector<std::pair<std::string, std::string>> try_take() {
        std::unique_lock<std::mutex> l(m_, std::try_to_lock);
        if (!l.owns_lock()) return {};
        return take_locked();
    }

private:
    std::vector<std::pair<std::string, std::string>> take_locked() {
        lines_ = 0;
        std::vector<std::pair<std::string, std::string>> out;
        out.swap(batches_);
        return out;
    }
    std::mutex m_;
    std::vector<std::pair<std::string, std::string>> batches_;
    size_t lines_ = 0;
};

// ---- the Discord webhook ----
enum class Hook { Join, Leave, Death, Restart, Say, Chat };
enum class HookEvents { Joins, JoinsDeaths, AllWithChat };
inline HookEvents hook_events(const std::string& setting) {
    if (setting == "joins_deaths") return HookEvents::JoinsDeaths;
    if (setting == "all_with_chat") return HookEvents::AllWithChat;
    return HookEvents::Joins;
}
// joins: joins, leaves and restart notices; joins_deaths: and deaths; all_with_chat: and players' chat and herald.say.
inline bool hook_wants(HookEvents e, Hook what) {
    switch (what) {
        case Hook::Join:
        case Hook::Leave:
        case Hook::Restart: return true;
        case Hook::Death: return e != HookEvents::Joins;
        case Hook::Say:
        case Hook::Chat: return e == HookEvents::AllWithChat;
    }
    return false;
}

struct UrlParts {
    std::string host;  // lower case
    unsigned port = 443;
    std::string path;  // with the query, starting with '/'
};
// An https URL split for WinHTTP; nothing when it is not a plain https URL (userinfo, backslashes, spaces, control
// characters, an odd port or host are refused).
inline std::optional<UrlParts> split_https_url(const std::string& url) {
    const std::string scheme = "https://";
    if (url.size() <= scheme.size() || lower(url.substr(0, scheme.size())) != scheme) return std::nullopt;
    for (char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7f || c == '\\') return std::nullopt;
    }
    const size_t start = scheme.size();
    const size_t end = url.find_first_of("/?#", start);
    const std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return std::nullopt;
    UrlParts p;
    const size_t colon = authority.find(':');
    p.host = lower(authority.substr(0, colon));
    if (colon != std::string::npos) {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        p.port = static_cast<unsigned>(std::strtoul(port.c_str(), nullptr, 10));
        if (p.port == 0 || p.port > 65535) return std::nullopt;
    }
    if (p.host.empty() || p.host.front() == '.' || p.host.back() == '.' || p.host.find("..") != std::string::npos ||
        p.host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos)
        return std::nullopt;
    p.path = end == std::string::npos ? "/" : url.substr(end);
    if (p.path[0] != '/') p.path = "/" + p.path;  // "https://host?x" -> "/?x"
    if (p.path.find('#') != std::string::npos) p.path.erase(p.path.find('#'));
    return p;
}
inline bool host_is(const std::string& host, const std::string& domain) {
    return host == domain || (host.size() > domain.size() + 1 && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
                              host[host.size() - domain.size() - 1] == '.');
}
// The webhook URL may be used: https, and (unless any_host) discord.com or discordapp.com (or a subdomain such as
// ptb.discord.com) with a path under /api/webhooks/. `why` says what is wrong without repeating the URL (it is a secret).
inline bool webhook_url_allowed(const std::string& url, bool any_host, std::string* why = nullptr) {
    auto no = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    const auto p = split_https_url(url);
    if (!p) return no("it is not a plain https:// URL");
    if (any_host) return true;
    if (!host_is(p->host, "discord.com") && !host_is(p->host, "discordapp.com"))
        return no("its host is not discord.com or discordapp.com (webhook_any_host allows other https hosts)");
    if (p->path.rfind("/api/webhooks/", 0) != 0) return no("its path does not start with /api/webhooks/");
    return true;
}

inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20 || u == 0x7f) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", u);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    return out;
}
// A line for Discord with its markdown escaped (a backslash before each of \ [ ] ( ) * _ ~ | > and `), so what players
// typed, or a player's name, cannot render as a masked link ("[gift](https://...)"), bold or a spoiler.
inline std::string discord_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': case '[': case ']': case '(': case ')': case '*': case '_': case '~': case '|': case '>': case '`':
                out += '\\';
                break;
            default:
                break;
        }
        out += c;
    }
    return out;
}
// The POST body: the text, and no pings (allowed_mentions with nothing to parse: "@everyone" in a chat line stays text).
inline std::string webhook_body(const std::string& content) {
    return "{\"content\":\"" + json_escape(content) + "\",\"allowed_mentions\":{\"parse\":[]}}";
}

// Where a batch goes (the WinHTTP POST in the mod; a fake in tests). The HTTP status, or a negative value when the
// request did not get an answer.
struct Transport {
    virtual ~Transport() = default;
    virtual int post(const std::string& body) = 0;
};

// Lines for the webhook, batched: at most one POST every 2 s (Discord's limit is about 5 per 2 s), at most kBatch
// characters each (Discord takes 2000), at most kMax lines waiting (the oldest are dropped and counted). After a 429
// (too many requests) it waits 10 s. Thread-safe: the game thread pushes, a worker pumps.
class WebhookQueue {
public:
    static constexpr size_t kBatch = 1900;
    static constexpr size_t kMax = 200;
    static constexpr uint64_t kGapMs = 2000;
    static constexpr uint64_t kBackoffMs = 10000;
    void push(const std::string& line) {
        std::lock_guard<std::mutex> l(m_);
        std::string t = printable(line);
        if (t.size() > kBatch) t.resize(kBatch);
        lines_.push_back(std::move(t));
        while (lines_.size() > kMax) {
            lines_.pop_front();
            ++dropped_;
        }
    }
    // The next batch when one may be sent now (and it is taken out), else nothing.
    std::optional<std::string> next(uint64_t now_ms) {
        std::lock_guard<std::mutex> l(m_);
        if (lines_.empty() || (sent_once_ && now_ms < ready_ms_)) return std::nullopt;
        std::string batch;
        if (dropped_) {
            batch = "(" + std::to_string(dropped_) + " older line(s) dropped)";
            dropped_ = 0;
        }
        while (!lines_.empty() && (batch.empty() || batch.size() + 1 + lines_.front().size() <= kBatch)) {
            batch += (batch.empty() ? "" : "\n") + lines_.front();
            lines_.pop_front();
        }
        sent_once_ = true;
        ready_ms_ = now_ms + kGapMs;
        return batch;
    }
    void too_many(uint64_t now_ms) {
        std::lock_guard<std::mutex> l(m_);
        ready_ms_ = now_ms + kBackoffMs;
    }
    size_t waiting() const {
        std::lock_guard<std::mutex> l(m_);
        return lines_.size();
    }
    uint64_t dropped() const {
        std::lock_guard<std::mutex> l(m_);
        return dropped_;
    }

private:
    mutable std::mutex m_;
    std::deque<std::string> lines_;
    uint64_t dropped_ = 0;
    uint64_t ready_ms_ = 0;
    bool sent_once_ = false;
};
// Sends at most one batch through `t`: its HTTP status, or 0 when nothing was due.
inline int pump_webhook(WebhookQueue& q, Transport& t, uint64_t now_ms) {
    const auto batch = q.next(now_ms);
    if (!batch) return 0;
    const int status = t.post(webhook_body(*batch));
    if (status == 429) q.too_many(now_ms);
    return status;
}

// The webhook's worker thread: it posts the queue while a URL is set, and ends by itself soon after the URL is gone
// (the webhook or the herald switched off), so no thread is left behind; a new URL starts a new one. The thread shares
// the state, so the worker may go first; nothing ever waits for the thread on a game thread (a post can take its
// time-outs). EMF has no "the game is closing" call for mods: at the game's exit Windows ends the thread.
class WebhookWorker {
public:
    using MakeTransport = std::function<std::unique_ptr<Transport>(const UrlParts&)>;
    using Status = std::function<void(int status, uint64_t now_ms)>;  // each post's answer (the mod logs it)
    using Clock = std::function<uint64_t()>;
    WebhookWorker(MakeTransport make, Status status, Clock clock, uint64_t wake_ms = 500) : s_(std::make_shared<State>()) {
        s_->make = std::move(make);
        s_->status = std::move(status);
        s_->clock = std::move(clock);
        s_->wake_ms = wake_ms;
    }
    // No lock here: at the game's exit the thread may have been ended while it held one.
    ~WebhookWorker() {
        s_->stop = true;
        s_->cv.notify_all();
    }
    WebhookWorker(const WebhookWorker&) = delete;
    WebhookWorker& operator=(const WebhookWorker&) = delete;

    WebhookQueue& queue() { return s_->queue; }
    // A new URL (the thread makes a new connection), or none: the thread ends. Starts a thread when one is needed.
    void set_url(std::optional<UrlParts> url) {
        bool start = false;
        {
            std::lock_guard<std::mutex> l(s_->m);
            s_->url = std::move(url);
            ++s_->generation;
            if (s_->url && !s_->running) start = s_->running = true;
        }
        s_->cv.notify_all();
        if (!start) return;
        try {
            std::thread(run, s_).detach();
        } catch (...) {  // no thread (out of resources): the webhook stays quiet
            std::lock_guard<std::mutex> l(s_->m);
            s_->running = false;
        }
    }
    void wake() { s_->cv.notify_all(); }
    bool on() const {
        std::lock_guard<std::mutex> l(s_->m);
        return s_->url.has_value();
    }
    bool running() const {
        std::lock_guard<std::mutex> l(s_->m);
        return s_->running;
    }
    // Waits up to `ms` for the thread to end (tests).
    bool wait_stopped(uint64_t ms) const {
        std::unique_lock<std::mutex> l(s_->m);
        return s_->cv.wait_for(l, std::chrono::milliseconds(ms), [this] { return !s_->running; });
    }

private:
    struct State {
        mutable std::mutex m;
        std::condition_variable cv;
        std::optional<UrlParts> url;
        uint64_t generation = 0;
        bool running = false;
        std::atomic<bool> stop{false};
        WebhookQueue queue;
        MakeTransport make;
        Status status;
        Clock clock;
        uint64_t wake_ms = 500;
    };
    static void run(std::shared_ptr<State> s) {
        std::unique_ptr<Transport> transport;
        uint64_t generation = ~0ull;
        for (;;) {
            std::optional<UrlParts> url;
            {
                std::unique_lock<std::mutex> l(s->m);
                if (!s->url || s->stop) {
                    s->running = false;
                    s->cv.notify_all();
                    return;
                }
                if (s->generation != generation) {
                    generation = s->generation;
                    transport.reset();
                }
                url = s->url;
            }
            if (!transport) transport = s->make(*url);
            const uint64_t now = s->clock();
            const int status = transport ? pump_webhook(s->queue, *transport, now) : 0;
            if (status && s->status) s->status(status, now);
            std::unique_lock<std::mutex> l(s->m);
            if (s->url && !s->stop) s->cv.wait_for(l, std::chrono::milliseconds(s->wake_ms));
        }
    }
    std::shared_ptr<State> s_;
};

}  // namespace herald
