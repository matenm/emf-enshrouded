#include "service.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "emf/keys.hpp"
#include "json_out.h"
#include "protocol.h"

namespace emf::core::bridge {

const std::vector<std::string>& command_names() {
    static const std::vector<std::string> k = {"hello",  "console",    "state",   "mods", "log",       "wait",
                                               "events", "screenshot", "mod_key", "quit", "load_world"};
    return k;
}

namespace {

constexpr uint32_t kStateTimeoutMs = 2000;
constexpr uint32_t kShotFrameMs = 5000;   // a frame must take the screenshot within this
constexpr uint32_t kShotWriteMs = 25000;  // then the copy and the PNG must be done within this
constexpr uint32_t kKeySlackMs = 10000;   // a press answers at most this long after its release time
constexpr size_t kConsoleTextMax = 2000;
constexpr size_t kConsoleBytesMax = 512 * 1024;
constexpr size_t kMatchMax = 1000;
constexpr size_t kLogReplyJsonMax = 960 * 1024;
constexpr size_t kLogReadBytes = 960 * 1024;  // log text copied out of the ring per read (under its lock)
constexpr size_t kFollowLinesPerPoll = 500;
constexpr size_t kFollowBytesPerPoll = 512 * 1024;
constexpr size_t kWaitScanLines = 5000;
constexpr uint32_t kLoadWorldSlackMs = 10000;  // the game's own timeout answers first; this is the backstop

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

std::string strings_json(const std::vector<std::string>& v) {
    std::vector<std::string> q;
    for (const auto& x : v) q.push_back(json_quote(x));
    return json_list(q);
}

bool blank(const std::string& s) { return s.find_first_not_of(" \t") == std::string::npos; }

}  // namespace

struct Service::Impl : std::enable_shared_from_this<Impl> {
    Sources src;
    Output out;
    mutable std::mutex m;

    struct Client {
        int bad = 0;  // bad lines in a row
        bool following = false;
        uint64_t cursor = 0;  // the last log line pushed to a follower
        std::set<std::string> events;
        uint64_t window_start = 0;  // events: the current second and how many went out in it
        uint32_t window_count = 0;
        uint64_t dropped = 0;
        std::set<uint32_t> held_keys;  // mod_key "down" without "up" yet
        size_t console_queued = 0;     // console lines the game has not finished (their answer may have timed out)
    };
    std::map<ClientId, Client> clients;

    enum class Kind { Console, State, Wait, Shot, Key, LoadWorld };
    enum class WaitFor { World, Player, Log };
    struct Pending {
        ClientId client = 0;
        std::string id;
        Kind kind = Kind::Console;
        uint64_t deadline = 0;
        // console
        std::string echo;
        bool first = true;
        std::vector<std::string> lines;
        size_t bytes = 0;
        bool truncated = false;
        uint32_t settle_ms = kSettleDefaultMs;
        std::string outcome;       // set once the command was handled ("" = pending)
        uint64_t settle_until = 0;  // after it ran: answer when no line came until this
        uint64_t settle_limit = 0;  // and at the latest at this
        // wait
        WaitFor wait_for = WaitFor::World;
        std::string match;
        uint64_t cursor = 0;
        // screenshot
        bool shot_taken = false;
        // mod_key
        std::string key_name;
        uint32_t vk = 0;
        KeyAction action = KeyAction::Press;
    };
    std::map<uint64_t, Pending> pending;
    uint64_t next_no = 1;

    Impl(Sources s, Output o) : src(std::move(s)), out(std::move(o)) {}

    uint64_t now() const { return src.now_ms ? src.now_ms() : GetTickCount64(); }
    uint64_t log_seq() const { return src.log ? src.log->last_seq() : 0; }

    // ---- sending (never under the lock) ----
    void send(ClientId c, std::string line, bool droppable) {
        if (out.send) out.send(c, std::move(line), droppable);
    }
    void reply_ok(ClientId c, const std::string& id, const std::string& members) {
        std::string line = ok_reply(id, members);
        if (line.size() > kMaxLineOut) line = error_reply(id, code::kRefused, "the reply would be larger than 1 MiB");
        send(c, std::move(line), false);
    }
    void reply_error(ClientId c, const std::string& id, std::string_view code, std::string_view error,
                     const std::string& members = std::string()) {
        std::string line = error_reply(id, code, error, members);
        if (line.size() > kMaxLineOut) line = error_reply(id, code, error);
        send(c, std::move(line), false);
    }
    void bad(ClientId c, const std::string& id, std::string_view error) { reply_error(c, id, code::kBadRequest, error); }
    void push_event(ClientId c, std::string line) {
        if (line.size() <= kMaxLineOut) send(c, std::move(line), true);
    }

    bool known(ClientId c) const {
        const std::lock_guard<std::mutex> l(m);
        return clients.count(c) != 0;
    }

    void bad_line(ClientId c, const std::string& id, const std::string& error) {
        bool cut = false;
        {
            const std::lock_guard<std::mutex> l(m);
            auto it = clients.find(c);
            if (it == clients.end()) return;
            cut = ++it->second.bad >= kBadLinesToDisconnect;
        }
        if (!cut) return bad(c, id, error);
        bad(c, id, error + " (" + std::to_string(kBadLinesToDisconnect) + " bad lines in a row: disconnected)");
        if (out.disconnect) out.disconnect(c);
    }

    // ---- requests waiting for an answer ----
    // Registers `p`; false (after answering the client) when it has too many waiting.
    bool add_pending(ClientId c, Pending p, uint64_t* no) {
        const std::string id = p.id;
        {
            const std::lock_guard<std::mutex> l(m);
            if (!clients.count(c)) return false;
            size_t n = 0;
            for (const auto& [k, q] : pending) n += q.client == c;
            if (n < kMaxPendingPerClient) {
                *no = next_no++;
                p.client = c;
                pending.emplace(*no, std::move(p));
                return true;
            }
        }
        reply_error(c, id, code::kBusy,
                    "too many requests waiting (" + std::to_string(kMaxPendingPerClient) + "): wait for some answers first");
        return false;
    }
    std::optional<Pending> take(uint64_t no) {
        const std::lock_guard<std::mutex> l(m);
        auto it = pending.find(no);
        if (it == pending.end()) return std::nullopt;
        Pending p = std::move(it->second);
        pending.erase(it);
        return p;
    }

    // ---- console ----
    void console_line(uint64_t no, const std::string& line) {
        const uint64_t t = now();
        const std::lock_guard<std::mutex> l(m);
        auto it = pending.find(no);
        if (it == pending.end() || it->second.kind != Kind::Console) return;
        Pending& p = it->second;
        const bool first = p.first;
        p.first = false;
        if (first && line == p.echo) return;  // "> text": the client knows what it typed
        if (!p.outcome.empty()) p.settle_until = t + p.settle_ms;  // a late line: wait a little longer
        if (p.bytes + line.size() > kConsoleBytesMax) {
            p.truncated = true;
            return;
        }
        p.bytes += line.size();
        p.lines.push_back(line);
    }
    std::string console_members(const Pending& p) const {
        JsonObj o;
        if (!p.outcome.empty()) o.str("status", p.outcome);
        o.raw("output", strings_json(p.lines));
        if (p.truncated) o.boolean("truncated", true);
        o.num("log_seq", static_cast<double>(log_seq()));
        return o.members();
    }
    void reply_console(const Pending& p) { reply_ok(p.client, p.id, console_members(p)); }
    void console_done(uint64_t no, const std::string& outcome) {
        std::optional<Pending> answer;
        const uint64_t t = now();
        {
            const std::lock_guard<std::mutex> l(m);
            auto it = pending.find(no);
            if (it == pending.end()) return;
            Pending& p = it->second;
            p.outcome = outcome.empty() ? "ran" : outcome;
            // Only a command that ran can still send lines (put's result one step later): it settles; others answer.
            if (p.outcome != "ran" || p.settle_ms == 0) {
                answer = std::move(p);
                pending.erase(it);
            } else {
                p.settle_until = t + p.settle_ms;
                p.settle_limit = t + kSettleMaxMs;
            }
        }
        if (answer) reply_console(*answer);
    }
    void console_finished(ClientId c) {
        const std::lock_guard<std::mutex> l(m);
        auto it = clients.find(c);
        if (it != clients.end() && it->second.console_queued) --it->second.console_queued;
    }

    void do_console(ClientId c, const Request& r) {
        std::optional<std::string> text;
        std::string err;
        uint32_t timeout = 0, settle = 0;
        if (!read_text(r.body, "text", &text, &err)) return bad(c, r.id, err);
        if (!text || blank(*text)) return bad(c, r.id, "console needs \"text\": the command line to run");
        if (text->find_first_of("\r\n") != std::string::npos)
            return bad(c, r.id, "one command per request: text has a line break");
        if (text->size() > kConsoleTextMax) return bad(c, r.id, "text is longer than 2000 bytes");
        if (!read_ms(r.body, "timeout_ms", 10000, 1, 120000, &timeout, &err)) return bad(c, r.id, err);
        if (!read_ms(r.body, "settle_ms", kSettleDefaultMs, 0, kSettleMaxMs, &settle, &err)) return bad(c, r.id, err);
        if (!src.console) return reply_error(c, r.id, code::kUnavailable, "console is not available");
        bool full = false;
        {  // a line whose answer timed out still waits in the game's queue: those count too
            const std::lock_guard<std::mutex> l(m);
            auto it = clients.find(c);
            if (it == clients.end()) return;
            full = it->second.console_queued >= kMaxPendingPerClient;
            if (!full) ++it->second.console_queued;
        }
        if (full)
            return reply_error(c, r.id, code::kBusy,
                               "8 console lines are still waiting for the world to run them: wait for some");
        Pending p;
        p.id = r.id;
        p.kind = Kind::Console;
        p.deadline = now() + timeout;
        p.echo = "> " + *text;
        p.settle_ms = settle;
        uint64_t no = 0;
        if (!add_pending(c, std::move(p), &no)) {
            console_finished(c);
            return;
        }
        std::weak_ptr<Impl> weak = weak_from_this();
        src.console(
            *text,
            [weak, no](const std::string& line) {
                if (auto s = weak.lock()) s->console_line(no, line);
            },
            [weak, no, c](const std::string& outcome) {
                if (auto s = weak.lock()) {
                    s->console_finished(c);
                    s->console_done(no, outcome);
                }
            });
    }

    // ---- state, mods, hello, quit ----
    void state_done(uint64_t no, const std::string& json) {
        auto p = take(no);
        if (!p) return;
        if (json.size() < 2 || json.front() != '{' || json.back() != '}')
            return reply_error(p->client, p->id, code::kUnavailable, "the state could not be read");
        std::string members = json.substr(1, json.size() - 2);
        members += (members.empty() ? "" : ",") + std::string("\"log_seq\":") + json_number(static_cast<double>(log_seq()));
        reply_ok(p->client, p->id, members);
    }
    void do_state(ClientId c, const Request& r) {
        if (!src.state) return reply_error(c, r.id, code::kUnavailable, "state is not available");
        Pending p;
        p.id = r.id;
        p.kind = Kind::State;
        p.deadline = now() + kStateTimeoutMs;
        uint64_t no = 0;
        if (!add_pending(c, std::move(p), &no)) return;
        std::weak_ptr<Impl> weak = weak_from_this();
        src.state([weak, no](std::string json) {
            if (auto s = weak.lock()) s->state_done(no, json);
        });
    }
    void do_mods(ClientId c, const Request& r) {
        reply_ok(c, r.id, "\"mods\":" + (src.mods ? src.mods() : std::string("[]")));
    }
    void do_hello(ClientId c, const Request& r) {
        const HelloInfo& h = src.hello;
        JsonObj o;
        o.str("emf", h.emf_version).num("api", h.api_version);
        if (h.game_build)
            o.num("game_build", h.game_build);
        else
            o.str("game_build", "unknown");
        o.str("process", h.dedicated ? "dedicated server" : "client").num("pid", h.pid);
        o.num("protocol", kProtocolVersion).raw("commands", strings_json(command_names()));
        o.raw("events", strings_json(src.event_names)).num("max_clients", kMaxClients);
        reply_ok(c, r.id, o.members());
    }
    void do_quit(ClientId c, const Request& r) {
        if (!src.quit) return reply_error(c, r.id, code::kUnavailable, "quit is not available");
        const Refusal no = src.quit();
        if (no.refused()) return reply_error(c, r.id, no.code.empty() ? code::kUnavailable : no.code, no.error);
        reply_ok(c, r.id, "\"quitting\":true");
    }

    // ---- load_world ----
    void load_done(uint64_t no, const LoadWorldReply& r) {
        auto p = take(no);
        if (!p) return;
        if (!r.ok)
            return reply_error(p->client, p->id, r.code.empty() ? code::kUnavailable : r.code,
                               r.error.empty() ? "the world did not start" : r.error);
        JsonObj o;
        o.num("slot", r.slot).num("character", r.character).str("world", r.world);
        o.str("character_name", r.character_name).num("ms", static_cast<double>(r.ms));
        reply_ok(p->client, p->id, o.members());
    }
    void do_load_world(ClientId c, const Request& r) {
        std::string err;
        LoadWorldParams lp;
        std::optional<std::string> name;
        const char* usage = "load_world needs \"slot\" and \"character\": LOCAL slots (1-based), e.g. "
                            "{\"cmd\":\"load_world\",\"slot\":1,\"character\":1}";
        if (!r.body.get("slot") || !r.body.get("character")) return bad(c, r.id, usage);
        if (!read_ms(r.body, "slot", 0, 1, 255, &lp.slot, &err)) return bad(c, r.id, err);
        if (!read_ms(r.body, "character", 0, 1, 255, &lp.character, &err)) return bad(c, r.id, err);
        if (!read_text(r.body, "expect_name", &name, &err)) return bad(c, r.id, err);
        if (name) lp.expect_name = *name;
        if (!read_ms(r.body, "timeout_ms", 180000, 1000, 600000, &lp.timeout_ms, &err)) return bad(c, r.id, err);
        std::optional<bool> test_refuse;
        if (!read_bool(r.body, "test_refuse", &test_refuse, &err)) return bad(c, r.id, err);
        lp.test_refuse = test_refuse.value_or(false);
        if (!src.load_world) return reply_error(c, r.id, code::kUnavailable, "load_world is not available");
        Pending p;
        p.id = r.id;
        p.kind = Kind::LoadWorld;
        p.deadline = now() + lp.timeout_ms + kLoadWorldSlackMs;
        uint64_t no = 0;
        if (!add_pending(c, std::move(p), &no)) return;
        std::weak_ptr<Impl> weak = weak_from_this();
        const Refusal why = src.load_world(lp, [weak, no](const LoadWorldReply& res) {
            if (auto s = weak.lock()) s->load_done(no, res);
        });
        if (why.refused() && take(no)) reply_error(c, r.id, why.code.empty() ? code::kUnavailable : why.code, why.error);
    }

    // ---- log ----
    static std::string line_json(const emf::LogRing::Line& l) {
        return JsonObj().num("seq", static_cast<double>(l.seq)).str("line", l.text).text();
    }
    void do_log(ClientId c, const Request& r) {
        if (!src.log) return reply_error(c, r.id, code::kUnavailable, "the log ring is off");
        std::string err;
        std::optional<uint64_t> since;
        std::optional<bool> follow;
        uint32_t max = 0;
        if (!read_seq(r.body, "since", &since, &err)) return bad(c, r.id, err);
        if (!read_ms(r.body, "max", since ? 1000 : 100, 1, 5000, &max, &err)) return bad(c, r.id, err);
        if (!read_bool(r.body, "follow", &follow, &err)) return bad(c, r.id, err);
        uint64_t from = 0;
        if (since) {
            from = *since;
        } else {
            const uint64_t last = src.log->last_seq();
            from = last > max ? last - max : 0;
        }
        const uint64_t first_kept = src.log->first_seq();
        const uint64_t missed = since && first_kept > *since + 1 ? first_kept - *since - 1 : 0;
        bool more = false;
        const auto lines = src.log->after(from, max, kLogReadBytes, &more);
        std::vector<std::string> items;
        size_t bytes = 0;
        uint64_t last = from;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string item = line_json(lines[i]);
            if (!items.empty() && bytes + item.size() > kLogReplyJsonMax) {
                more = true;
                break;
            }
            bytes += item.size() + 1;
            items.push_back(std::move(item));
            last = lines[i].seq;
        }
        bool following = false;
        {
            const std::lock_guard<std::mutex> l(m);
            auto it = clients.find(c);
            if (it == clients.end()) return;
            if (follow) {
                it->second.following = *follow;
                if (*follow) it->second.cursor = last;  // pushing goes on after what this reply holds
            }
            following = it->second.following;
        }
        JsonObj o;
        o.raw("lines", json_list(items)).num("last", static_cast<double>(last)).boolean("more", more);
        if (missed) o.num("missed", static_cast<double>(missed));
        o.boolean("following", following);
        reply_ok(c, r.id, o.members());
    }

    void pump_followers() {
        if (!src.log) return;
        std::vector<std::pair<ClientId, uint64_t>> followers;
        {
            const std::lock_guard<std::mutex> l(m);
            for (const auto& [id, cl] : clients)
                if (cl.following) followers.emplace_back(id, cl.cursor);
        }
        for (const auto& [id, cursor] : followers) {
            const auto lines = src.log->after(cursor, kFollowLinesPerPoll, kFollowBytesPerPoll);
            if (lines.empty()) continue;
            {
                const std::lock_guard<std::mutex> l(m);
                auto it = clients.find(id);
                if (it == clients.end() || !it->second.following || it->second.cursor != cursor) continue;
                it->second.cursor = lines.back().seq;
            }
            const uint64_t missed = lines[0].seq > cursor + 1 ? lines[0].seq - cursor - 1 : 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                JsonObj e;
                e.str("event", "log").num("seq", static_cast<double>(lines[i].seq)).str("line", lines[i].text);
                if (i == 0 && missed) e.num("missed", static_cast<double>(missed));
                push_event(id, e.text());
            }
        }
    }

    // ---- wait ----
    void do_wait(ClientId c, const Request& r) {
        std::string err;
        std::optional<std::string> what, match;
        std::optional<uint64_t> since;
        uint32_t timeout = 0;
        if (!read_text(r.body, "for", &what, &err)) return bad(c, r.id, err);
        Pending p;
        if (what == "world")
            p.wait_for = WaitFor::World;
        else if (what == "player")
            p.wait_for = WaitFor::Player;
        else if (what == "log")
            p.wait_for = WaitFor::Log;
        else
            return bad(c, r.id, "wait needs \"for\": world, player or log");
        if (!read_ms(r.body, "timeout_ms", 30000, 1, 600000, &timeout, &err)) return bad(c, r.id, err);
        if (p.wait_for == WaitFor::Log) {
            if (!read_text(r.body, "match", &match, &err)) return bad(c, r.id, err);
            if (!match || match->empty()) return bad(c, r.id, "wait for log needs \"match\": the text to look for");
            if (match->size() > kMatchMax) return bad(c, r.id, "match is longer than 1000 bytes");
            if (!read_seq(r.body, "since", &since, &err)) return bad(c, r.id, err);
            if (!src.log) return reply_error(c, r.id, code::kUnavailable, "the log ring is off");
            p.match = *match;
            p.cursor = since ? *since : src.log->last_seq();  // by default only lines from now on
        }
        p.id = r.id;
        p.kind = Kind::Wait;
        p.deadline = now() + timeout;
        uint64_t no = 0;
        if (!add_pending(c, std::move(p), &no)) return;
        check_waits();
    }

    void check_waits() {
        struct W {
            uint64_t no;
            WaitFor what;
            std::string match;
            uint64_t cursor;
        };
        std::vector<W> waits;
        {
            const std::lock_guard<std::mutex> l(m);
            for (const auto& [no, p] : pending)
                if (p.kind == Kind::Wait) waits.push_back({no, p.wait_for, p.match, p.cursor});
        }
        if (waits.empty()) return;
        std::optional<bool> world, player;  // asked once per check
        for (const auto& w : waits) {
            std::string members;
            bool hit = false;
            if (w.what == WaitFor::World) {
                if (!world) world = src.world_running && src.world_running();
                hit = *world;
            } else if (w.what == WaitFor::Player) {
                if (!player) player = src.player_exists && src.player_exists();
                hit = *player;
            } else if (src.log) {
                bool more = false;
                const auto lines = src.log->after(w.cursor, kWaitScanLines, kLogReadBytes, &more);
                uint64_t cursor = w.cursor;
                for (const auto& line : lines) {
                    cursor = line.seq;
                    if (line.text.find(w.match) != std::string::npos) {
                        hit = true;
                        members = JsonObj().num("seq", static_cast<double>(line.seq)).str("line", line.text).members();
                        break;
                    }
                }
                if (!hit && cursor != w.cursor) {
                    const std::lock_guard<std::mutex> l(m);
                    auto it = pending.find(w.no);
                    if (it != pending.end()) it->second.cursor = cursor;
                }
            }
            if (!hit) continue;
            if (auto p = take(w.no)) reply_ok(p->client, p->id, members);
        }
    }

    // ---- events ----
    void do_events(ClientId c, const Request& r) {
        const json::Value* names = r.body.get("names");
        const char* usage = "events needs \"names\": a list like [\"pickup\", \"kill\"] ([] stops them)";
        if (!names || !names->is(json::Value::Type::Array)) return bad(c, r.id, usage);
        std::set<std::string> chosen;
        for (const auto& n : names->arr) {
            if (!n.is(json::Value::Type::String)) return bad(c, r.id, usage);
            bool ok = false;
            for (const auto& e : src.event_names) ok |= e == n.s;
            if (!ok) return bad(c, r.id, "unknown event '" + n.s + "' (events: " + joined(src.event_names) + ")");
            chosen.insert(n.s);
        }
        {
            const std::lock_guard<std::mutex> l(m);
            auto it = clients.find(c);
            if (it == clients.end()) return;
            it->second.events = chosen;
        }
        reply_ok(c, r.id, "\"names\":" + strings_json(std::vector<std::string>(chosen.begin(), chosen.end())));
    }

    void publish(const std::string& name, const std::string& fields) {
        std::vector<std::pair<ClientId, std::string>> lines;
        const uint64_t t = now();
        {
            const std::lock_guard<std::mutex> l(m);
            for (auto& [id, cl] : clients) {
                if (!cl.events.count(name)) continue;
                if (t - cl.window_start >= 1000) {
                    cl.window_start = t;
                    cl.window_count = 0;
                }
                if (cl.window_count >= kEventsPerSecond) {
                    ++cl.dropped;
                    continue;
                }
                ++cl.window_count;
                JsonObj e;
                e.str("event", "game").str("name", name).raw("fields", fields);
                if (cl.dropped) {
                    e.num("dropped", static_cast<double>(cl.dropped));
                    cl.dropped = 0;
                }
                lines.emplace_back(id, e.text());
            }
        }
        for (auto& [id, line] : lines) push_event(id, std::move(line));
    }

    // ---- screenshot ----
    void shot_done(uint64_t no, const ShotResult& r) {
        auto p = take(no);
        if (!p) return;
        if (!r.ok)
            return reply_error(p->client, p->id, r.code.empty() ? code::kUnavailable : r.code,
                               r.error.empty() ? "the screenshot failed" : r.error);
        JsonObj o;
        o.str("path", r.path).num("width", r.width).num("height", r.height).boolean("hdr", r.hdr);
        reply_ok(p->client, p->id, o.members());
    }
    void do_screenshot(ClientId c, const Request& r) {
        if (r.body.get("path")) return bad(c, r.id, "screenshot takes no path: pictures always go to emf\\screens");
        std::string err;
        std::optional<bool> overlay;
        uint32_t max_width = 0;
        if (!read_bool(r.body, "overlay", &overlay, &err)) return bad(c, r.id, err);
        if (!read_ms(r.body, "max_width", 0, 16, 16384, &max_width, &err))
            return bad(c, r.id, "max_width must be a whole number from 16 to 16384");
        if (!src.shot_start) return reply_error(c, r.id, code::kUnavailable, "screenshots are not available");
        ShotOptions opt;
        opt.overlay = overlay.value_or(true);
        opt.max_width = max_width;
        Pending p;
        p.id = r.id;
        p.kind = Kind::Shot;
        p.deadline = now() + kShotFrameMs;
        uint64_t no = 0;
        if (!add_pending(c, std::move(p), &no)) return;
        std::weak_ptr<Impl> weak = weak_from_this();
        const Refusal why = src.shot_start(opt, [weak, no](const ShotResult& res) {
            if (auto s = weak.lock()) s->shot_done(no, res);
        });
        if (!why.refused()) return;
        if (take(no)) reply_error(c, r.id, why.code.empty() ? code::kUnavailable : why.code, why.error);
    }

    // ---- mod_key ----
    void key_done(uint64_t no, const KeyResult& k) {
        auto p = take(no);
        if (!p) return;
        if (!k.ok)
            return reply_error(p->client, p->id, code::kUnavailable, k.error.empty() ? "the key press failed" : k.error);
        static const char* const kAction[] = {"press", "down", "up"};
        JsonObj o;
        o.str("key", p->key_name).num("vk", p->vk).str("action", kAction[static_cast<int>(p->action)]);
        o.num("mod_reads", k.mod_reads);
        reply_ok(p->client, p->id, o.members());
    }
    void do_mod_key(ClientId c, const Request& r) {
        std::string err;
        std::optional<std::string> key, action;
        uint32_t hold = 0;
        if (!read_text(r.body, "key", &key, &err)) return bad(c, r.id, err);
        if (!key || key->empty()) return bad(c, r.id, "mod_key needs \"key\": a key name like L, F6 or Space");
        std::wstring wide;
        bool ascii = key->size() <= 16;
        for (char ch : *key) {
            ascii &= static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7F;
            wide += static_cast<wchar_t>(static_cast<unsigned char>(ch));
        }
        std::string name;
        const uint32_t vk = ascii ? emf::keys::parse_key(wide, &name) : 0;
        if (!vk)
            return bad(c, r.id, "unknown key '" + *key +
                                    "' (a letter, a digit, F1-F24, a name like Space or PageUp, or a number like 0x4C)");
        if (!read_text(r.body, "action", &action, &err)) return bad(c, r.id, err);
        KeyAction a = KeyAction::Press;
        if (!action || *action == "press")
            a = KeyAction::Press;
        else if (*action == "down")
            a = KeyAction::Down;
        else if (*action == "up")
            a = KeyAction::Up;
        else
            return bad(c, r.id, "action must be press, down or up");
        if (!read_ms(r.body, "hold_ms", 150, 20, 5000, &hold, &err)) return bad(c, r.id, err);
        if (!src.mod_key) return reply_error(c, r.id, code::kUnavailable, "mod_key is not available");
        Pending p;
        p.id = r.id;
        p.kind = Kind::Key;
        p.deadline = now() + (a == KeyAction::Press ? hold : 0) + kKeySlackMs;
        p.key_name = name;
        p.vk = vk;
        p.action = a;
        uint64_t no = 0;
        if (!add_pending(c, std::move(p), &no)) return;
        std::weak_ptr<Impl> weak = weak_from_this();
        const Refusal why = src.mod_key(vk, a, hold, [weak, no](const KeyResult& k) {
            if (auto s = weak.lock()) s->key_done(no, k);
        });
        if (why.refused()) {
            if (take(no)) reply_error(c, r.id, why.code.empty() ? code::kUnavailable : why.code, why.error);
            return;
        }
        const std::lock_guard<std::mutex> l(m);
        auto it = clients.find(c);
        if (it == clients.end()) return;
        if (a == KeyAction::Down) it->second.held_keys.insert(vk);
        if (a == KeyAction::Up) it->second.held_keys.erase(vk);
    }

    // ---- timeouts and settled console replies ----
    void expire() {
        const uint64_t t = now();
        std::vector<Pending> expired, settled;
        std::vector<uint64_t> untaken_shots;
        {
            const std::lock_guard<std::mutex> l(m);
            for (auto it = pending.begin(); it != pending.end();) {
                Pending& p = it->second;
                if (p.kind == Kind::Console && !p.outcome.empty()) {  // ran: answer once it settled
                    if (t >= p.settle_until || t >= p.settle_limit) {
                        settled.push_back(std::move(p));
                        it = pending.erase(it);
                    } else {
                        ++it;
                    }
                } else if (t < p.deadline) {
                    ++it;
                } else if (p.kind == Kind::Shot && !p.shot_taken) {
                    untaken_shots.push_back(it->first);
                    ++it;
                } else {
                    expired.push_back(std::move(p));
                    it = pending.erase(it);
                }
            }
        }
        for (const auto& p : settled) reply_console(p);
        for (const auto& p : expired) {
            switch (p.kind) {
                case Kind::Console:
                    reply_error(p.client, p.id, code::kTimeout,
                                "the world did not finish the command in time (later lines go to the log)",
                                console_members(p));
                    break;
                case Kind::State:
                    reply_error(p.client, p.id, code::kTimeout, "the world did not answer within 2 s (is it loading?)");
                    break;
                case Kind::Wait: reply_error(p.client, p.id, code::kTimeout, "timed out"); break;
                case Kind::Shot:
                    reply_error(p.client, p.id, code::kTimeout, "the screenshot did not finish within 30 s");
                    break;
                case Kind::Key: reply_error(p.client, p.id, code::kTimeout, "the key press did not finish"); break;
                case Kind::LoadWorld:
                    reply_error(p.client, p.id, code::kTimeout, "the world did not start in time (the game did not answer)");
                    break;
            }
        }
        for (uint64_t no : untaken_shots) {
            const bool cancelled = !src.shot_cancel || src.shot_cancel();
            if (cancelled) {
                if (auto p = take(no))
                    reply_error(p->client, p->id, code::kTimeout,
                                "no frame was presented within 5 s (is the game minimised?)");
                continue;
            }
            const std::lock_guard<std::mutex> l(m);  // a frame took it: the copy is being written
            auto it = pending.find(no);
            if (it == pending.end()) continue;
            it->second.shot_taken = true;
            it->second.deadline = t + kShotWriteMs;
        }
    }

    void dispatch(ClientId c, const Request& r) {
        const std::string& cmd = r.cmd;
        if (cmd == "hello") return do_hello(c, r);
        if (cmd == "console") return do_console(c, r);
        if (cmd == "state") return do_state(c, r);
        if (cmd == "mods") return do_mods(c, r);
        if (cmd == "log") return do_log(c, r);
        if (cmd == "wait") return do_wait(c, r);
        if (cmd == "events") return do_events(c, r);
        if (cmd == "screenshot") return do_screenshot(c, r);
        if (cmd == "mod_key") return do_mod_key(c, r);
        if (cmd == "quit") return do_quit(c, r);
        if (cmd == "load_world") return do_load_world(c, r);
        reply_error(c, r.id, code::kUnknownCommand,
                    "unknown command '" + cmd + "' (commands: " + joined(command_names()) + ")");
    }
};

Service::Service(Sources sources, Output output)
    : impl_(std::make_shared<Impl>(std::move(sources), std::move(output))) {}

Service::~Service() = default;

void Service::on_connect(ClientId c) {
    const std::lock_guard<std::mutex> l(impl_->m);
    impl_->clients[c] = Impl::Client{};
}

void Service::on_line(ClientId c, std::string_view line) {
    if (!impl_->known(c)) return;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty()) return;  // a blank line: nothing to answer
    Parsed p = parse_request(line);
    if (!p.request) return impl_->bad_line(c, p.id, p.error);
    {
        const std::lock_guard<std::mutex> l(impl_->m);
        auto it = impl_->clients.find(c);
        if (it == impl_->clients.end()) return;
        it->second.bad = 0;
    }
    impl_->dispatch(c, *p.request);
}

void Service::on_too_long(ClientId c) { impl_->bad_line(c, "null", "the line is longer than 64 KiB"); }

void Service::on_disconnect(ClientId c) {
    std::set<uint32_t> held;
    {
        const std::lock_guard<std::mutex> l(impl_->m);
        auto it = impl_->clients.find(c);
        if (it == impl_->clients.end()) return;
        held = std::move(it->second.held_keys);
        impl_->clients.erase(it);
        for (auto p = impl_->pending.begin(); p != impl_->pending.end();)
            p = p->second.client == c ? impl_->pending.erase(p) : std::next(p);
    }
    if (impl_->src.mod_key)
        for (uint32_t vk : held) impl_->src.mod_key(vk, KeyAction::Up, 0, [](const KeyResult&) {});
}

void Service::poll() {
    impl_->expire();
    impl_->check_waits();
    impl_->pump_followers();
}

void Service::publish(const std::string& name, const std::string& fields_json) { impl_->publish(name, fields_json); }

bool Service::wants(const std::string& name) const {
    const std::lock_guard<std::mutex> l(impl_->m);
    for (const auto& [id, cl] : impl_->clients)
        if (cl.events.count(name)) return true;
    return false;
}

size_t Service::clients() const {
    const std::lock_guard<std::mutex> l(impl_->m);
    return impl_->clients.size();
}

}  // namespace emf::core::bridge
