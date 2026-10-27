#pragma once
// Starting a LOCAL save offline from the main menu without clicks (the debug bridge's load_world), the way the menu's
// own "Play -> Start a Session -> Start... -> Offline: Start" does it: commands posted to the pregame command queue from
// inside the UI frame (docs/engine_parts/re_attributes_menu.md, Part 2). Build 1273043's client only.
//
// The bridge thread only stores a request; every game call happens in the UI frame hook. While an EMF-started load runs,
// and only then, a post-hook on the game's "character selected?" check refuses a Steam character or world (and, when
// asked, a character with another name): the game then logs "No Character selected" and stays in the menu.
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "common/signatures.h"

namespace emf::core {

// Offsets inside the game's objects, read from build 1273043's code (re_attributes_menu.md: save2 is +0x10 from 1269579).
namespace menu_layout {
constexpr uint32_t kPregameGameApp = 0x28;   // Pregame -> gameApp*
constexpr uint32_t kPregameSave2 = 0x140;    // Pregame -> save2*
constexpr uint8_t kMainMenuState = 2;        // the pregame state handler's state id of the main menu (3 = Start Play)
// the UI's pregame command queue, as the posters reach it: ui+0xD0 -> UiData; *(UiData+0x33CC0) -> queue; +0x10 u32 count
constexpr uint32_t kUiData = 0xD0;
constexpr uint32_t kUiCommandQueue = 0x33CC0;
constexpr uint32_t kQueueCount = 0x10;
// save2
constexpr uint32_t kWorldListJob = 0x980;    // u8: 1 = the world list job is busy
constexpr uint32_t kWorldListState = 0x984;  // u8: 3 = the world list is loaded
constexpr uint32_t kWorldRecords = 0x990;    // world record = save2 + this + storage * 0xE60 + index * 0x170
constexpr uint32_t kWorldStorageStride = 0xE60;
constexpr uint32_t kWorldRecordSize = 0x170;
constexpr uint32_t kWorldSlots = 10;
constexpr uint32_t kWorldRecordName = 0x58;  // {char* data; u64 length}
constexpr uint32_t kCharacterListJob = 0x27E8;  // u8: 1 = the character list job is busy
constexpr uint32_t kCharacterRecords = 0x27F8;  // character record = save2 + this + storage * 0x57980 + index * 0x5798
constexpr uint32_t kCharacterStorageStride = 0x57980;
constexpr uint32_t kCharacterRecordSize = 0x5798;
constexpr uint32_t kCharacterSlots = 16;
constexpr uint32_t kCharacterRecordName = 0x10;  // {char* data; u64 length}
constexpr uint32_t kSelectedWorld = 0x1192BB;    // u16 slot id
constexpr uint32_t kSelectedWorldValid = 0x1192BD;
constexpr uint32_t kSelectedCharacter = 0x1192BE;  // u16 slot id (its high byte, +0x1192BF, is the storage)
constexpr uint32_t kSelectedCharacterValid = 0x1192C0;
// gameApp
constexpr uint32_t kMode = 0x34FA8;         // u32: 0 Lobby, 1 Host_Offline, 2 Host_Online, 3 Client_Online
constexpr uint32_t kSessionName = 0x34FE8;  // {char* data; u64 length}: the world's name once playWorld ran
constexpr uint32_t kPlaySlot = 0x35058;     // u16 slot id of the save to load (its high byte, +0x35059, the storage)
constexpr uint32_t kPlaySlotSet = 0x3505A;  // u8
constexpr uint32_t kAdventure = 0x35270;    // u8: an adventure instead of a save (playWorld clears it)
// record states (u8 at +0)
constexpr uint8_t kRecordExists = 2, kRecordLoaded = 3, kRecordChanged = 4;
constexpr uint8_t kNoCharacterSelected = 0x1F;  // the "character selected?" check's error: back to the menu
}  // namespace menu_layout

// A slot id: low byte = index + 1, high byte = storage (0 Local, 1 Steam).
constexpr uint16_t local_slot_id(uint32_t one_based) { return static_cast<uint16_t>(one_based & 0xFF); }

struct LoadWorldRequest {
    uint32_t world = 1;        // LOCAL world slot, 1-based (1..10)
    uint32_t character = 1;    // LOCAL character slot, 1-based (1..16)
    std::string expect_name;   // "" = any; else the character must have this name
    uint32_t timeout_ms = 180000;
    bool test_refuse = false;  // dev check of the refusal path: the guard refuses even the LOCAL pair
};

// The character the game has selected (save2+0x1192BE, the slot id the menu's character screen and load_world set, and
// Start Play's character check reads): "local" or "steam" with its 1-based slot and its record's name, or "unknown"
// (nothing selected, a value the layout does not know, unreadable memory, or the menu hooks are not installed).
struct CharacterSelection {
    std::string storage = "unknown";  // local | steam | unknown
    std::optional<uint32_t> slot;     // 1-based, in that storage's table
    std::optional<std::string> name;  // the character record's name
};

struct LoadWorldResult {
    bool ok = false;
    std::string code;   // when not ok: "unavailable", "refused", "timeout", "busy", "bad_request"
    std::string error;
    uint32_t world = 0, character = 0;
    std::string world_name, character_name;
    uint64_t ms = 0;    // from the request until the world ran
};

class MenuStart {
public:
    using Done = std::function<void(const LoadWorldResult&)>;
    struct Deps {
        std::function<bool(uintptr_t address, void* out, size_t n)> read;  // game memory, fault-guarded
        // the game's command posters (the UI context first): true = queued
        std::function<bool(void* ui, uint16_t slot)> select_character;
        std::function<bool(void* ui, uint16_t slot)> select_world;
        std::function<bool(void* ui, uint8_t area)> session_area;
        std::function<bool(void* ui)> start;
        // the started world is ready: a world this game hosts steps and the local player is in it (the server world
        // steps about a second after the start, long before the save has loaded and the player exists)
        std::function<bool()> world_ready;
        std::function<bool()> any_world_running;     // any world steps (hosted or someone else's)
        // commands waiting in the UI's pregame command queue (SIZE_MAX: unreadable); the start is posted only into an
        // empty queue
        std::function<size_t(void* ui)> queued_commands;
        std::function<uint64_t()> now_ms;
        std::function<void(const std::string&)> log;
    };
    static constexpr uint64_t kSelectMs = 5000;  // frame B: the selection must show within this
    // A request that ends (a timeout) after the start was posted leaves the guard on until the game is back in the main
    // menu or the world runs, at the latest this long after the start.
    static constexpr uint64_t kGuardLingerMs = 60000;

    explicit MenuStart(Deps deps);

    // Any thread (the bridge): checks the slots and stores the request for the UI frame. False with a result (code,
    // error) when refused at once; otherwise done comes once, later, from the UI frame or poll().
    bool request(const LoadWorldRequest& r, Done done, LoadWorldResult* refused);
    // The main-menu state's calls (the pregame state handler with state 2, before the game's handler runs): phase 0
    // enter, 1 a command, 2 exit.
    void on_menu_state(void* pregame, uint8_t phase);
    // The pregame state handler with another state (Start Play, ...): the main menu is not active.
    void on_other_state(void* pregame, uint8_t state, uint8_t phase);
    // The UI frame hook (the UI context is valid now): the only place commands are posted.
    void on_ui_frame(void* ui);
    // Any thread: ends a request whose time is up even when no UI frame runs.
    void poll();
    // The "character selected?" check's post-hook: while an EMF load runs (and only then) a Steam character or world,
    // or a character with another name than expected, becomes the error 0x1F. True when it forced the error.
    bool guard(uint8_t* result, uintptr_t save2);

    bool armed() const { return armed_.load(); }
    void log_line(const std::string& s) const { deps_.log(s); }
    bool busy() const;
    bool menu_active() const { return menu_active_.load(); }
    uintptr_t game_app() const;
    // The session's world name (gameApp+0x34FE8) once the game has seen the menu; nothing before or when unreadable.
    std::optional<std::string> session_world_name() const;
    // The pregame state handler's thread, before the game's handler runs (the thread that changes the selection, and
    // the pregame is alive then): reads the selected character from the pregame's save2 and keeps it. A world never
    // calls this, so while it runs the character Start Play took stays. A null pregame keeps what was seen.
    void note_character(void* pregame);
    // The character last noted ("unknown" before the first note). Any thread.
    CharacterSelection character() const;

private:
    enum class Step { Idle, Waiting, Selected, Started };
    struct Pending {
        Done done;
        LoadWorldResult result;
    };
    std::optional<std::string> read_name(uintptr_t field) const;  // {char*, u64} at field
    bool read_u8(uintptr_t a, uint8_t* v) const;
    bool read_u16(uintptr_t a, uint16_t* v) const;
    bool read_u32(uintptr_t a, uint32_t* v) const;
    bool read_ptr(uintptr_t a, uintptr_t* v) const;
    void note_app(void* pregame);  // keeps the game app from a pregame call
    // with lock_ held: ends the request (done is called by the caller after the lock is released). After the start
    // was posted, a failure leaves the guard on (lingering) until the game is back in the menu or the world runs.
    Pending finish_locked(bool ok, const char* code, const std::string& error);
    void disarm_locked(const std::string& why);
    // with lock_ held, no request: a lingering guard turns off when the world runs or its time is up
    void linger_check_locked(uint64_t now);
    void deliver(Pending p);
    // with lock_ held, Step::Waiting: checks the menu and the slots; posts frame A. Returns a finished request, if any.
    std::optional<Pending> frame_a_locked(void* ui, uint64_t now);
    std::optional<Pending> frame_b_locked(void* ui, uint64_t now);

    Deps deps_;
    mutable std::mutex lock_;
    Step step_ = Step::Idle;
    LoadWorldRequest req_;
    Done done_;
    uint64_t requested_ms_ = 0, deadline_ms_ = 0, selected_ms_ = 0;
    bool start_posted_ = false, area_posted_ = false;
    bool left_menu_ = false;  // the main menu was left (its exit, or another state entered) since start was posted
    uint64_t start_posted_ms_ = 0;
    bool lingering_ = false;  // no request, but the guard stays on (a request ended after its start was posted)
    uint64_t linger_until_ms_ = 0;
    bool test_refuse_ = false;  // the request's test_refuse (read by the guard under lock_)
    std::string waiting_for_;  // what the request still waits for (named in a timeout)
    std::string world_name_, character_name_;
    std::atomic<uintptr_t> pregame_{0};  // the main menu's Pregame; forgotten when Start Play begins (it is freed after)
    std::atomic<uintptr_t> app_{0};      // the game app (lives for the process)
    std::atomic<bool> menu_active_{false};
    std::atomic<bool> armed_{false};
    std::atomic<bool> guard_fired_{false};
    std::string guard_name_;  // the expected name while armed (under lock_)
    mutable std::mutex character_lock_;
    CharacterSelection character_;  // the last note_character (under character_lock_)
};

// The process's MenuStart (null until install_menu_start ran and succeeded).
MenuStart* menu_start();
// Installs the hooks (UI frame, main-menu state, character check) and the posters when every signature is usable, on
// build 1273043's client (the offsets are that build's); false (and the reason) otherwise. Deps other than the game's
// functions come from the caller.
bool install_menu_start(const std::array<Resolved, kSigCount>& sigs, bool known_build, int build_index,
                        MenuStart::Deps deps, std::string* why);
// The hosted world's display name for the bridge's state, when the menu hooks know the game app.
std::optional<std::string> session_world_name();
// The selected character for the bridge's state ("unknown" without the menu hooks).
CharacterSelection current_character();

}  // namespace emf::core
