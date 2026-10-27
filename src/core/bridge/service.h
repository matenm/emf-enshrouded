#pragma once
// The debug bridge's commands (docs/modding/debug_bridge.md), apart from any transport: the pipe server hands it each
// client's lines and sends what it answers. Everything it reads from the game comes through Sources, so tests run it
// against fakes. Lines may arrive on one thread while answers come back on others (the world's tick, the screenshot
// writer); it is thread-safe, and it never calls a source while holding its own lock.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/log_ring.h"

namespace emf::core::bridge {

using ClientId = uint64_t;

// The machine-readable "code" of an error reply.
namespace code {
constexpr const char* kTimeout = "timeout";
constexpr const char* kBusy = "busy";
constexpr const char* kUnavailable = "unavailable";
constexpr const char* kBadRequest = "bad_request";
constexpr const char* kUnknownCommand = "unknown_command";
constexpr const char* kRefused = "refused";
}  // namespace code

// A source's "no": an error code and plain words. Empty error = accepted.
struct Refusal {
    std::string code;
    std::string error;
    bool refused() const { return !error.empty(); }
};

struct HelloInfo {
    std::string emf_version;
    uint32_t api_version = 0;
    uint32_t game_build = 0;  // 0 = not a known build
    bool dedicated = false;   // enshrouded_server.exe
    uint32_t pid = 0;
};

struct ShotOptions {
    bool overlay = true;     // false: the frame before EMF's overlay draws on it
    uint32_t max_width = 0;  // 0 = full size; else scaled down to at most this width
};

struct ShotResult {
    bool ok = false;
    std::string code;  // when not ok
    std::string error;
    std::string path;  // the PNG, UTF-8
    uint32_t width = 0, height = 0;
    bool hdr = false;  // an HDR10 frame, tone-mapped to sRGB
};

enum class KeyAction { Press, Down, Up };

// load_world: a LOCAL save started offline from the main menu (src/core/menu_start.h).
struct LoadWorldParams {
    uint32_t slot = 0;         // LOCAL world slot, 1-based
    uint32_t character = 0;    // LOCAL character slot, 1-based
    std::string expect_name;   // "" = any; else the character's name must be this
    uint32_t timeout_ms = 180000;
    bool test_refuse = false;  // dev check: the guard refuses even the LOCAL pair (the game stays in the menu)
};
struct LoadWorldReply {
    bool ok = false;
    std::string code;  // when not ok
    std::string error;
    uint32_t slot = 0, character = 0;
    std::string world, character_name;
    uint64_t ms = 0;
};

struct KeyResult {
    bool ok = false;
    std::string error;
    uint32_t mod_reads = 0;  // how often mods read the key while it was held
};

struct Sources {
    HelloInfo hello;
    std::function<uint64_t()> now_ms;
    // Runs `text` as if typed into the console: line() for each line of output, then done(outcome) once, outcome being
    // command_outcome_name()'s words. Any thread, now or later (a queued command answers when the world runs it).
    std::function<void(const std::string& text, std::function<void(const std::string&)> line,
                       std::function<void(const std::string& outcome)> done)>
        console;
    // One state snapshot: done(<JSON object>) once, now or later; never called back means "timed out".
    std::function<void(std::function<void(std::string)> done)> state;
    std::function<std::string()> mods;  // the mod list as a JSON array
    emf::LogRing* log = nullptr;        // the log ring (null: log and wait for log are refused)
    std::function<bool()> world_running;
    std::function<bool()> player_exists;
    // Starts a screenshot: accepted (done then comes once, from any thread), or why not.
    std::function<Refusal(const ShotOptions& options, std::function<void(const ShotResult&)> done)> shot_start;
    // Cancels the started screenshot if no frame took it yet (true); false once a frame took it.
    std::function<bool()> shot_cancel;
    // A key for mods' key reads: accepted (done then comes once: at once for down/up, after the release for a press),
    // or why not.
    std::function<Refusal(uint32_t vk, KeyAction action, uint32_t hold_ms, std::function<void(const KeyResult&)> done)>
        mod_key;
    // Asks the game to save and close (after this request's reply went out), or why not.
    std::function<Refusal()> quit;
    // Starts a LOCAL save offline from the main menu: accepted (done then comes once, from any thread, when the world
    // runs or with why not), or why not.
    std::function<Refusal(const LoadWorldParams& params, std::function<void(const LoadWorldReply&)> done)> load_world;
    std::vector<std::string> event_names;  // the game events `events` can push
};

struct Output {
    // A finished JSON line for one client (no newline). Droppable lines (pushed events) may be dropped when the client
    // reads too slowly; replies are not.
    std::function<void(ClientId, std::string line, bool droppable)> send;
    std::function<void(ClientId)> disconnect;
};

constexpr uint32_t kEventsPerSecond = 200;  // per client
constexpr size_t kMaxPendingPerClient = 8;  // requests waiting for an answer; console lines the game has not run
constexpr uint32_t kSettleDefaultMs = 300;  // console: how long it waits for late lines after the command ran
constexpr uint32_t kSettleMaxMs = 5000;

class Service {
public:
    Service(Sources sources, Output output);
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void on_connect(ClientId c);
    void on_line(ClientId c, std::string_view line);  // one line from the client, without its newline
    void on_too_long(ClientId c);                     // the client sent a line over kMaxLineIn (dropped)
    void on_disconnect(ClientId c);
    // Timeouts, settling console replies, waits and log following. Call often (the pipe thread: every 50 ms and on
    // wake-ups).
    void poll();
    // A game event (any thread): pushed to the clients that asked for `name`, at most kEventsPerSecond per client.
    void publish(const std::string& name, const std::string& fields_json);
    // Whether any client asked for game event `name` (so the core can skip decoding it).
    bool wants(const std::string& name) const;
    size_t clients() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;  // answers that come back late hold it weakly
};

// The commands, for hello and the error for an unknown one.
const std::vector<std::string>& command_names();

}  // namespace emf::core::bridge
