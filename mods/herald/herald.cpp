// herald (Server Herald): an EMF mod for dedicated servers and games you host. A message of the day for each player who
// joins, join / leave / death lines for everyone, announcements from a file in turn, a restart countdown (herald.restart,
// also from emf\commands.txt), a chat log per day, playtime per account and world, /players /playtime /motd for every
// player, and an opt-in Discord webhook. Off by default: it changes what players see. The rules (tested without the
// game) are herald_logic.h; chat lines go through EMF API v19 (chat_tell, chat_broadcast, player_name, on_chat,
// request_save).
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "emf/emf.hpp"
#include "emf/generated/components.hpp"
#include "emf/query.hpp"
#include "emf/storage.hpp"
#include "herald_logic.h"
#include "webhook_winhttp.h"

namespace gen = emf::gen::ecs;

namespace {
constexpr const char* kMod = "herald";

// ---- settings (the change callback may run on any world's thread) ----
struct Config {
    std::string motd;
    uint64_t motd_delay_ms = 5000;
    bool joins = true, leaves = true, deaths = false;
    std::string prefix = "[Server]";
    uint64_t announce_ms = 30 * 60 * 1000;
    bool restart_save = true;
    bool chat_log = true;
    bool playtime = true;
    bool webhook = false;
    herald::HookEvents hook_events = herald::HookEvents::Joins;
    bool any_host = false;
};
std::mutex g_config_lock;
Config g_config;
std::atomic<bool> g_switched_off{false};  // switched off in the Mods tab (the DLL stays until the next start)

std::wstring game_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d(exe);
    return d.substr(0, d.find_last_of(L"\\/"));
}
std::wstring config_dir() { return game_dir() + L"\\emf\\config\\herald"; }

bool switch_on() {
    const std::wstring ini = game_dir() + L"\\emf\\config\\herald.ini";
    return GetPrivateProfileIntW(L"herald", L"enabled", 1, ini.c_str()) != 0;
}

Config config() {
    std::lock_guard<std::mutex> l(g_config_lock);
    return g_config;
}
void read_settings() {
    Config c;
    c.motd = emf::setting_text(kMod, "motd", "");
    c.motd_delay_ms = static_cast<uint64_t>(emf::setting_float(kMod, "motd_delay_seconds", 5.0) * 1000.0);
    c.joins = emf::setting_bool(kMod, "announce_joins", true);
    c.leaves = emf::setting_bool(kMod, "announce_leaves", true);
    c.deaths = emf::setting_bool(kMod, "announce_deaths", false);
    c.prefix = herald::printable(emf::setting_text(kMod, "prefix", "[Server]"));
    c.announce_ms = static_cast<uint64_t>(emf::setting_float(kMod, "announce_every_minutes", 30.0) * 60000.0);
    c.restart_save = emf::setting_bool(kMod, "restart_save", true);
    c.chat_log = emf::setting_bool(kMod, "chat_log", true);
    c.playtime = emf::setting_bool(kMod, "playtime", true);
    c.webhook = emf::setting_bool(kMod, "webhook", false);
    c.hook_events = herald::hook_events(emf::setting_text(kMod, "webhook_events", "joins"));
    c.any_host = emf::setting_bool(kMod, "webhook_any_host", false);
    std::lock_guard<std::mutex> l(g_config_lock);
    g_config = c;
}

std::string read_file(const std::wstring& path, bool* found = nullptr) {
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        if (found) *found = false;
        return text;
    }
    if (found) *found = true;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1u << 20)) text.append(buf, n);
    fclose(f);
    return text;
}

// ---- the Discord webhook: a worker thread posts while it is on and ends when it goes off (herald_logic.h
// WebhookWorker); the URL never leaves the worker (never logged or printed) ----
// Each post's answer, from the worker thread (one at a time): the first success, and at most one failure a minute.
void hook_status(int status, uint64_t now) {
    static bool said_ok = false;
    static uint64_t said_error_ms = 0;
    if (status >= 200 && status < 300) {
        if (!said_ok) EMF_LOG(kMod, "webhook: the first message was posted (HTTP %d)", status);
        said_ok = true;
    } else if (!said_error_ms || now - said_error_ms > 60000) {  // never the URL
        said_error_ms = now;
        if (status > 0) EMF_LOG(kMod, "webhook: a post was refused (HTTP %d); the lines in it are dropped", status);
        else EMF_LOG(kMod, "webhook: a post got no answer (Windows error %d); the lines in it are dropped", -status);
    }
}
herald::WebhookWorker g_hook(
    [](const herald::UrlParts& url) { return std::unique_ptr<herald::Transport>(std::make_unique<herald::WinHttpTransport>(url)); },
    hook_status, [] { return GetTickCount64(); });

// Reads webhook.txt again (the setting or the file changed, the herald switched on or off). Logs whether it is on,
// never the URL. Off (or the herald switched off), the worker thread ends.
void reload_webhook() {
    const Config c = config();
    std::optional<herald::UrlParts> url;
    if (c.webhook && !g_switched_off) {
        bool found = false;
        const std::string text = read_file(config_dir() + L"\\webhook.txt", &found);
        std::string line;
        for (size_t at = 0; at < text.size() && line.empty();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            line = text.substr(at, end - at);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // a BOM
            at = end + 1;
        }
        std::string why;
        if (line.empty())
            EMF_LOG(kMod, "webhook: on in the settings, but emf\\config\\herald\\webhook.txt %s: no messages go out",
                    found ? "holds no URL" : "does not exist");
        else if (!herald::webhook_url_allowed(line, c.any_host, &why))
            EMF_LOG(kMod, "webhook: off: the URL in emf\\config\\herald\\webhook.txt is not allowed: %s", why.c_str());
        else
            url = herald::split_https_url(line);
    }
    const bool was = g_hook.on();
    const bool discord = url && (herald::host_is(url->host, "discord.com") || herald::host_is(url->host, "discordapp.com"));
    g_hook.set_url(url);
    if (url && !was) EMF_LOG(kMod, "webhook: on (%s); at most one post every 2 s", discord ? "Discord" : "another https host");
    if (!url && was) EMF_LOG(kMod, "webhook: off%s", g_switched_off ? " (Server Herald is switched off)" : "");
}

void hook_line(herald::Hook what, const std::string& line) {
    if (!g_hook.on() || !herald::hook_wants(config().hook_events, what)) return;
    g_hook.queue().push(herald::discord_escape(line));  // names and chat lines are players' text: no markdown
    g_hook.wake();
}

// ---- the chat log: emf\logs\chat-YYYY-MM-DD.log (local time). Lines wait in g_chat and are written once per server
// step (write_chat_log), at once when 64 wait, when the herald is switched off, and at the game's exit. ----
herald::ChatLogBuffer g_chat;

// Appends each day's waiting text to its file. Windows' own file calls, no C runtime: at the game's exit (g_chat_exit)
// a thread that was ended may still hold a C runtime lock.
void append_chat_files(const std::vector<std::pair<std::string, std::string>>& batches) {
    if (batches.empty()) return;
    const std::wstring dir = game_dir() + L"\\emf\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const auto& [name, text] : batches) {
        HANDLE f = CreateFileW((dir + L"\\" + std::wstring(name.begin(), name.end())).c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD wrote = 0;
        WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
        CloseHandle(f);
    }
}
void write_chat_log() { append_chat_files(g_chat.take()); }
// At the game's exit (EMF has no closing call for mods): what the last step left, if no one holds the buffer.
struct ChatLogAtExit {
    ~ChatLogAtExit() { append_chat_files(g_chat.try_take()); }
} g_chat_exit;

void chat_log(const std::string& line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (g_chat.add(herald::chat_log_name(t.wYear, t.wMonth, t.wDay), line)) write_chat_log();  // a flood: now
}
void chat_log_event(const std::string& what) {
    if (!config().chat_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    chat_log(herald::chat_log_event(t.wHour, t.wMinute, t.wSecond, what));
}

// ---- server state (the server world's thread: on_tick, commands, events, on_chat) ----
herald::Roster g_roster;
herald::Announcer g_announcer;
herald::Countdown g_countdown;
herald::Playtime g_playtime;
emf::WorldFile g_file(kMod);
uint32_t g_session = 0;
bool g_file_read = false;  // this session's file was read once (a second read replaces instead of adding)
uint64_t g_last_second = 0, g_last_accrue = 0, g_last_save = 0;
bool g_no_input_said = false;

void reload_announcements() {
    bool found = false;
    const std::string text = read_file(config_dir() + L"\\announcements.txt", &found);
    g_announcer.set_lines(herald::parse_announcements(text));
    if (found) EMF_LOG(kMod, "announcements: %zu line(s) from emf\\config\\herald\\announcements.txt", g_announcer.count());
}

// A line for everyone (with the prefix), in the chat log and, when chosen, the webhook.
void say(const std::string& text, herald::Hook what) {
    const Config c = config();
    emf::broadcast(herald::with_prefix(c.prefix, text));
    chat_log_event(herald::with_prefix(c.prefix, text));
    hook_line(what, text);
}

std::string name_of(emf::World w, uint32_t entity) { return herald::player_label(emf::player_name(w, entity), entity); }

int64_t unix_now() { return static_cast<int64_t>(std::time(nullptr)); }

void save_playtime() {
    if (config().playtime && g_file.ready()) g_file.save(g_playtime.text());
}

void each_second(emf::World w, uint64_t now) {
    const Config c = config();
    // who is there
    std::vector<herald::Seen> seen;
    const uint32_t host = emf::local_player();
    for (uint32_t id : emf::players(w)) {
        herald::Seen s;
        s.entity = id;
        s.account = emf::player_account(w, id);
        const auto* input = w.get<gen::PlayerInput>(id);
        if (!input && !g_no_input_said) {
            g_no_input_said = true;
            EMF_LOG(kMod, "player %u has no PlayerInput this game build EMF knows: players count as joined when they appear", id);
        }
        s.ready = !input || input->fromClientIsReadyForGameplay || id == host;  // the host's own game: always ready
        s.name = name_of(w, id);
        seen.push_back(std::move(s));
    }
    // playtime of those online since the last second
    const uint64_t delta = g_last_accrue ? now - g_last_accrue : 0;
    g_last_accrue = now;
    if (c.playtime && delta)
        for (const auto& o : g_roster.online()) g_playtime.add(o.account, delta, unix_now(), o.name);
    for (const auto& ch : g_roster.update(seen, now, c.motd_delay_ms)) {
        const bool joined = ch.kind == herald::Change::Joined;
        const std::string line = herald::printable(ch.name) + (joined ? " joined (" : " left (") +
                                 std::to_string(ch.online) + " online)";
        EMF_LOG(kMod, "%s, entity %u, account %s", line.c_str(), ch.entity, herald::account_text(ch.account).c_str());
        if (joined ? c.joins : c.leaves) emf::broadcast(herald::with_prefix(c.prefix, line));
        chat_log_event(line);
        hook_line(joined ? herald::Hook::Join : herald::Hook::Leave, line);
        if (joined && c.playtime) g_playtime.add(ch.account, 0, unix_now(), ch.name);  // first / last seen
        if (!joined) save_playtime();
    }
    for (const auto& o : g_roster.motd_due(now)) {
        if (c.motd.empty()) {
            g_roster.motd_result(o.entity, true, now);  // nothing to say
            continue;
        }
        const bool sent = emf::tell(w, o.entity, herald::with_prefix(c.prefix, herald::expand_motd(c.motd, herald::printable(o.name),
                                                                                                  g_roster.online().size())));
        if (g_roster.motd_result(o.entity, sent, now))
            EMF_LOG(kMod, "the MOTD for %s could not be sent after %d tries (see the lines above)",
                    herald::printable(o.name).c_str(), herald::kMotdTries);
    }
    if (auto line = g_announcer.tick(now, c.announce_ms, !g_roster.online().empty())) say(*line, herald::Hook::Say);
    for (const auto& e : g_countdown.tick(now)) {
        say(e.text, herald::Hook::Restart);
        if (e.now && c.restart_save)
            EMF_LOG(kMod, "restart countdown at 0: %s", emf::request_save() ? "save asked" : "the save could not be asked (see the log)");
    }
    if (now - g_last_save >= 60000) {
        g_last_save = now;
        save_playtime();
    }
}

void on_tick(emf::World w) {
    if (!w.is_server()) return;
    write_chat_log();  // the lines of the last step, once per file (also the last ones after a switch off)
    if (g_switched_off) return;
    const uint64_t now = GetTickCount64();
    const uint32_t session = emf::server_session();
    if (session != g_session) {  // a new hosted session: the old one's players and file are gone
        g_session = session;
        g_roster.clear();
        g_countdown.cancel();
        g_playtime.clear();
        g_file_read = false;
        g_last_accrue = 0;
        g_last_save = now;
    }
    g_file.poll([](const std::string& text) {
        if (g_file_read) g_playtime.clear();  // read again in the same session: the file is the truth
        g_playtime.merge_text(text);          // first read: time counted before it arrived is added
        g_file_read = true;
    });
    if (now - g_last_second < 1000) return;
    g_last_second = now;
    each_second(w, now);
}

// ---- commands ----
std::string join_words(const std::vector<std::string>& a, size_t from) {
    std::string out;
    for (size_t i = from; i < a.size(); ++i) out += (out.empty() ? "" : " ") + a[i];
    return out;
}

struct Sender {
    uint32_t entity = 0;
    uint64_t account = 0;
};
Sender sender(emf::World w) {
    EmfCommandSender s{};
    if (emf::command_sender(&s) && s.from_chat) return {s.entity, s.account};
    const uint32_t host = emf::local_player();
    return {host, host ? emf::player_account(w, host) : 0};
}

void cmd_say(emf::World, const std::vector<std::string>& a) {
    const std::string text = herald::printable(join_words(a, 1));
    if (text.empty()) {
        EMF_REPLY("usage: herald.say <text>");
        return;
    }
    say(text, herald::Hook::Say);
    EMF_REPLY("said to %zu player(s) online: %s", g_roster.online().size(), text.c_str());
}

void cmd_tell(emf::World w, const std::vector<std::string>& a) {
    const std::vector<std::string> words(a.begin() + (a.empty() ? 0 : 1), a.end());
    const auto t = herald::find_target(g_roster.online(), words);
    if (!t.entity) {
        EMF_REPLY("%s", t.error.c_str());
        return;
    }
    const std::string text = herald::printable(join_words(words, t.words));
    if (text.empty()) {
        EMF_REPLY("usage: herald.tell <name|#k> <text>");
        return;
    }
    const bool ok = emf::tell(w, t.entity, herald::with_prefix(config().prefix, text));
    EMF_REPLY("%s %s", ok ? "told" : "could not tell (see the log)", herald::printable(t.name).c_str());
}

void cmd_restart(emf::World, const std::vector<std::string>& a) {
    const auto seconds = a.size() > 1 ? herald::parse_minutes(a[1]) : std::nullopt;
    if (!seconds) {
        EMF_REPLY("usage: herald.restart <minutes> [reason] (e.g. herald.restart 10 nightly; 0.5 = 30 s)");
        return;
    }
    const std::string first = g_countdown.start(*seconds, join_words(a, 2), GetTickCount64());
    say(first, herald::Hook::Restart);
    EMF_REPLY("restart countdown started (%s); the server itself is stopped outside EMF (EMF.exe Stop, a scheduled task)%s",
              herald::duration_text(*seconds).c_str(), config().restart_save ? "; it saves at 0" : "");
}

void cmd_cancel(emf::World, const std::vector<std::string>&) {
    if (!g_countdown.cancel()) {
        EMF_REPLY("no restart countdown is running");
        return;
    }
    say("Server restart cancelled", herald::Hook::Restart);
    EMF_REPLY("restart countdown cancelled");
}

void cmd_save(emf::World, const std::vector<std::string>&) {
    if (emf::request_save())
        EMF_REPLY("save asked: the game writes the world in a moment (the log says \"world saved\")");
    else
        EMF_REPLY("the save could not be asked (see the log)");
}

std::string when_text(int64_t unix_time) {
    if (!unix_time) return "unknown";
    const std::time_t t = static_cast<std::time_t>(unix_time);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

void cmd_seen(emf::World, const std::vector<std::string>& a) {
    const std::string name = join_words(a, 1);
    if (name.empty()) {
        EMF_REPLY("usage: herald.seen <name>");
        return;
    }
    std::vector<std::string> words(a.begin() + 1, a.end());
    const auto t = herald::find_target(g_roster.online(), words);
    uint64_t account = 0;
    if (t.entity && t.words == words.size()) {
        for (const auto& o : g_roster.online())
            if (o.entity == t.entity) {
                account = o.account;
                EMF_REPLY("%s is online now (this session %s)", herald::printable(o.name).c_str(),
                          herald::duration_text((GetTickCount64() - o.joined_ms) / 1000).c_str());
            }
    }
    if (!account) account = g_playtime.by_name(name);
    const herald::PlayRecord* r = account ? g_playtime.find(account) : nullptr;
    if (!r) {
        if (!t.entity) EMF_REPLY("nobody named '%s' was seen on this world (by their last name)", herald::printable(name).c_str());
        return;
    }
    EMF_REPLY("%s (%s): last seen %s, first seen %s, playtime %s", herald::printable(r->name).c_str(),
              herald::account_text(account).c_str(), when_text(r->last_seen).c_str(), when_text(r->first_seen).c_str(),
              herald::duration_text(r->total_ms / 1000).c_str());
}

void cmd_reload(emf::World, const std::vector<std::string>&) {
    reload_announcements();
    reload_webhook();
    EMF_REPLY("announcements: %zu line(s); webhook %s", g_announcer.count(), g_hook.on() ? "on" : "off");
}

void cmd_players(emf::World, const std::vector<std::string>&) {
    for (const auto& line : herald::players_lines(g_roster.online(), GetTickCount64())) EMF_REPLY("%s", line.c_str());
}

void cmd_playtime(emf::World w, const std::vector<std::string>&) {
    const Sender s = sender(w);
    const herald::PlayRecord* r = s.account ? g_playtime.find(s.account) : nullptr;
    if (!r) {
        EMF_REPLY("%s", s.account ? "no playtime is recorded for you on this world yet"
                                  : "EMF could not tell which player you are; try again in a moment");
        return;
    }
    EMF_REPLY("your playtime on this world: %s (rank %zu of %zu)", herald::duration_text(r->total_ms / 1000).c_str(),
              g_playtime.rank(s.account), g_playtime.size());
}

void cmd_motd(emf::World w, const std::vector<std::string>&) {
    const Config c = config();
    if (c.motd.empty()) {
        EMF_REPLY("there is no message of the day");
        return;
    }
    const Sender s = sender(w);
    const std::string name = s.entity ? herald::printable(name_of(w, s.entity)) : "you";
    EMF_REPLY("%s", herald::with_prefix(c.prefix, herald::expand_motd(c.motd, name, g_roster.online().size())).c_str());
}

// Every herald command checks the Mods tab's switch first: switched off, it says so and does nothing (the DLL stays
// loaded until the next start, so its commands are still there).
void herald_command(const char* name, const char* help, void (*f)(emf::World, const std::vector<std::string>&),
                    uint32_t flags = 0) {
    const auto checked = [f](emf::World w, const std::vector<std::string>& a) {
        if (g_switched_off) {
            EMF_REPLY("Server Herald is switched off (Mods tab); switch it on to use its commands");
            return;
        }
        f(w, a);
    };
    if (!emf::add_command(name, help, checked, flags)) EMF_LOG(kMod, "the command %s could not be added (see the core log)", name);
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* api) {
    if (!emf::init(api)) return false;
    if (!emf::has_v19()) {
        EMF_LOG(kMod, "needs EMF API v19 or newer");
        return false;
    }
    read_settings();
    emf::on_setting_changed(kMod, [](emf::World, const std::string& key) {
        if (key == "enabled") g_switched_off = !switch_on();
        read_settings();
        if (key == "enabled" || key.rfind("webhook", 0) == 0) reload_webhook();  // switched off: the worker ends
    });
    g_file.attach();
    emf::on_event<gen::EntityDiedEvent>(EMF_WORLD_SERVER, [](emf::World w, const gen::EntityDiedEvent& e) {
        const uint32_t id = e.deadEntityId.id;
        if (g_switched_off || id < 1 || id > emf::kMaxPlayerEntity || !w.get<gen::PlayerState>(id)) return;
        const std::string line = herald::printable(name_of(w, id)) + " fell";
        const Config c = config();
        if (c.deaths) emf::broadcast(herald::with_prefix(c.prefix, line));
        chat_log_event(line);
        hook_line(herald::Hook::Death, line);
    });
    const bool chat = emf::on_chat(kMod, [](emf::World w, uint64_t account, uint32_t entity, const std::string& text) {
        if (g_switched_off) return;
        std::string name;
        if (entity) name = name_of(w, entity);
        else if (const auto* o = g_roster.by_account(account)) name = o->name;
        else name = "player";
        if (config().chat_log) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            chat_log(herald::chat_log_line(t.wHour, t.wMinute, t.wSecond, account, name, text));
        }
        hook_line(herald::Hook::Chat, herald::printable(name) + ": " + herald::printable(text));
    });
    if (!chat)
        EMF_LOG(kMod, "players' chat lines do not reach mods here (players' chat is off: [chat] players=1 in "
                      "emf\\emf_core.ini of a game you host): the chat log has joins, leaves and the herald's lines only");
    herald_command("herald.say", "<text>: a line for everyone", cmd_say);
    herald_command("herald.tell", "<name|#k> <text>: a line for one player (#k as /players numbers them)", cmd_tell);
    herald_command("herald.restart", "<minutes> [reason]: count down to a restart (10, 5, 2, 1 min, 30 s); the stop is outside EMF",
        cmd_restart);
    herald_command("herald.cancel", ": stop the restart countdown", cmd_cancel);
    herald_command("herald.save", ": save the world now", cmd_save);
    herald_command("herald.seen", "<name>: when a player was last on this world, and their playtime", cmd_seen);
    herald_command("herald.reload", ": read announcements.txt and webhook.txt again", cmd_reload);
    herald_command("players", ": who is online, and for how long", cmd_players, EMF_COMMAND_PLAYERS);
    herald_command("playtime", ": your playtime on this world, and your rank", cmd_playtime, EMF_COMMAND_PLAYERS);
    herald_command("motd", ": the message of the day", cmd_motd, EMF_COMMAND_PLAYERS);
    reload_announcements();
    reload_webhook();
    emf::on_tick(on_tick);
    const Config c = config();
    EMF_LOG(kMod, "ready: MOTD %s, join/leave lines %s/%s, announcements every %llu min (%zu line(s)), chat log %s, "
                  "playtime %s, webhook %s",
            c.motd.empty() ? "none" : "set", c.joins ? "on" : "off", c.leaves ? "on" : "off",
            static_cast<unsigned long long>(c.announce_ms / 60000), g_announcer.count(),
            c.chat_log && chat ? "on" : (c.chat_log ? "joins only" : "off"), c.playtime ? "on" : "off",
            g_hook.on() ? "on" : "off");
    return true;
}
