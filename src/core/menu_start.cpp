#include "menu_start.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "api.h"
#include "common/seh.h"

namespace emf::core {

namespace L = menu_layout;

namespace {
std::string slot_text(uint16_t slot) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%04x", slot);
    return b;
}
bool record_exists(uint8_t state) {
    return state == L::kRecordExists || state == L::kRecordLoaded || state == L::kRecordChanged;
}
}  // namespace

MenuStart::MenuStart(Deps deps) : deps_(std::move(deps)) {
    if (!deps_.now_ms) deps_.now_ms = [] { return GetTickCount64(); };
    if (!deps_.log) deps_.log = [](const std::string&) {};
}

bool MenuStart::read_u8(uintptr_t a, uint8_t* v) const { return a && deps_.read && deps_.read(a, v, 1); }
bool MenuStart::read_u16(uintptr_t a, uint16_t* v) const { return a && deps_.read && deps_.read(a, v, 2); }
bool MenuStart::read_u32(uintptr_t a, uint32_t* v) const { return a && deps_.read && deps_.read(a, v, 4); }
bool MenuStart::read_ptr(uintptr_t a, uintptr_t* v) const { return a && deps_.read && deps_.read(a, v, 8); }

std::optional<std::string> MenuStart::read_name(uintptr_t field) const {
    uintptr_t data = 0;
    uint64_t len = 0;
    if (!read_ptr(field, &data) || !deps_.read(field + 8, &len, 8) || !data || len == 0 || len > 256) return std::nullopt;
    std::string s(static_cast<size_t>(len), '\0');
    if (!deps_.read(data, s.data(), s.size())) return std::nullopt;
    for (char c : s)
        if (c == '\0') return std::nullopt;  // not a name: the layout is not what EMF expects
    return s;
}

bool MenuStart::busy() const {
    const std::lock_guard<std::mutex> l(lock_);
    return step_ != Step::Idle;
}

uintptr_t MenuStart::game_app() const { return app_.load(); }

std::optional<std::string> MenuStart::session_world_name() const {
    const uintptr_t app = game_app();
    if (!app) return std::nullopt;
    return read_name(app + L::kSessionName);
}

void MenuStart::note_character(void* pregame) {
    if (!pregame) return;
    CharacterSelection c;  // unknown unless every read works and the values are ones the layout knows
    uintptr_t save2 = 0;
    uint16_t sel = 0;
    uint8_t valid = 0;
    if (read_ptr(reinterpret_cast<uintptr_t>(pregame) + L::kPregameSave2, &save2) && save2 &&
        read_u16(save2 + L::kSelectedCharacter, &sel) && read_u8(save2 + L::kSelectedCharacterValid, &valid) && valid) {
        const uint32_t storage = sel >> 8, index = sel & 0xFF;  // slot id: low byte index + 1, high byte the storage
        if (storage <= 1 && index >= 1 && index <= L::kCharacterSlots) {
            c.storage = storage == 0 ? "local" : "steam";
            c.slot = index;
            c.name = read_name(save2 + L::kCharacterRecords + storage * L::kCharacterStorageStride +
                               (index - 1) * L::kCharacterRecordSize + L::kCharacterRecordName);
        }
    }
    const std::lock_guard<std::mutex> l(character_lock_);
    character_ = std::move(c);
}

CharacterSelection MenuStart::character() const {
    const std::lock_guard<std::mutex> l(character_lock_);
    return character_;
}

bool MenuStart::request(const LoadWorldRequest& r, Done done, LoadWorldResult* refused) {
    auto no = [refused](const char* code, const std::string& why) {
        if (refused) {
            *refused = LoadWorldResult{};
            refused->code = code;
            refused->error = why;
        }
        return false;
    };
    if (r.world < 1 || r.world > L::kWorldSlots)
        return no("bad_request", "slot must be a LOCAL world slot from 1 to " + std::to_string(L::kWorldSlots));
    if (r.character < 1 || r.character > L::kCharacterSlots)
        return no("bad_request", "character must be a LOCAL character slot from 1 to " + std::to_string(L::kCharacterSlots));
    const std::lock_guard<std::mutex> l(lock_);
    if (step_ != Step::Idle) return no("busy", "a load_world is already in progress");
    lingering_ = false;  // the new request owns the guard from here
    req_ = r;
    done_ = std::move(done);
    requested_ms_ = deps_.now_ms();
    deadline_ms_ = requested_ms_ + r.timeout_ms;
    selected_ms_ = 0;
    start_posted_ = area_posted_ = left_menu_ = false;
    start_posted_ms_ = 0;
    test_refuse_ = r.test_refuse;
    waiting_for_ = "the main menu (it did not come up)";
    world_name_.clear();
    character_name_.clear();
    guard_fired_ = false;
    guard_name_ = r.expect_name;
    step_ = Step::Waiting;
    deps_.log("load_world: LOCAL world slot " + std::to_string(r.world) + ", LOCAL character " +
              std::to_string(r.character) + (r.expect_name.empty() ? "" : " ('" + r.expect_name + "')") +
              ": waiting for the main menu's UI frame");
    return true;
}

MenuStart::Pending MenuStart::finish_locked(bool ok, const char* code, const std::string& error) {
    Pending p;
    p.done = std::move(done_);
    p.result.ok = ok;
    if (!ok) {
        p.result.code = code;
        p.result.error = error;
    }
    p.result.world = req_.world;
    p.result.character = req_.character;
    p.result.world_name = world_name_;
    p.result.character_name = character_name_;
    const uint64_t now = deps_.now_ms();
    p.result.ms = now - requested_ms_;
    step_ = Step::Idle;
    done_ = nullptr;
    const bool back_in_menu = left_menu_ && menu_active_.load();
    if (!ok && start_posted_ && !back_in_menu && armed_.load()) {
        // the start may still come out of the game's queue: the guard stays on until the game is back in the main menu
        // or the world runs, at the latest kGuardLingerMs after the start
        lingering_ = true;
        linger_until_ms_ = start_posted_ms_ + kGuardLingerMs;
        if (now >= linger_until_ms_) disarm_locked("60 s after the start");
        else deps_.log("load_world: the guard stays on until the game is back in the main menu or the world runs");
    } else {
        disarm_locked(ok ? "the world runs" : "the request ended");
    }
    return p;
}

void MenuStart::disarm_locked(const std::string& why) {
    lingering_ = false;
    if (armed_.exchange(false)) deps_.log("load_world: the guard is off again (" + why + ")");
}

void MenuStart::linger_check_locked(uint64_t now) {
    if (!lingering_) return;
    if (deps_.world_ready && deps_.world_ready()) disarm_locked("the world runs");
    else if (now >= linger_until_ms_) disarm_locked("60 s after the start");
}

void MenuStart::deliver(Pending p) {
    if (p.result.ok)
        deps_.log("load_world: '" + p.result.world_name + "' runs with '" + p.result.character_name + "' (" +
                  std::to_string(p.result.ms) + " ms)");
    else
        deps_.log("load_world: " + p.result.code + ": " + p.result.error);
    if (p.done) p.done(p.result);
}

void MenuStart::note_app(void* pregame) {
    uintptr_t app = 0;
    if (pregame && read_ptr(reinterpret_cast<uintptr_t>(pregame) + L::kPregameGameApp, &app) && app) app_ = app;
}

void MenuStart::on_other_state(void* pregame, uint8_t state, uint8_t phase) {
    note_app(pregame);  // the game app outlives the pregame: keep it
    if (phase != 0) return;
    menu_active_ = false;  // entered another state (Start Play, ...)
    pregame_ = 0;          // a successful Start Play frees the pregame: never read it after this
    {
        const std::lock_guard<std::mutex> l(lock_);
        if (start_posted_) left_menu_ = true;
    }
    deps_.log("load_world: the pregame entered state " + std::to_string(state));
}

void MenuStart::on_menu_state(void* pregame, uint8_t phase) {
    note_app(pregame);
    if (pregame) pregame_ = reinterpret_cast<uintptr_t>(pregame);
    if (phase == 0) deps_.log("load_world: the main menu is up");
    if (phase == 2) {
        menu_active_ = false;
        const std::lock_guard<std::mutex> l(lock_);
        if (start_posted_) left_menu_ = true;
        return;
    }
    menu_active_ = true;  // entered (0), or a command for it (1)
    if (phase == 0) {     // back in the main menu after a start: a lingering guard turns off
        const std::lock_guard<std::mutex> l(lock_);
        if (lingering_ && left_menu_) disarm_locked("the game is back in the main menu");
    }
}

std::optional<MenuStart::Pending> MenuStart::frame_a_locked(void* ui, uint64_t now) {
    if (deps_.any_world_running && deps_.any_world_running())
        return finish_locked(false, "unavailable", "not at the main menu: a world is running (quit to the menu first)");
    if (!menu_active_.load()) {
        waiting_for_ = "the main menu (it did not come up)";
        return std::nullopt;
    }
    const uintptr_t pg = pregame_.load();
    uintptr_t app = 0, save2 = 0;
    if (!read_ptr(pg + L::kPregameGameApp, &app) || !read_ptr(pg + L::kPregameSave2, &save2) || !app || !save2) {
        waiting_for_ = "the main menu's game objects";
        return std::nullopt;
    }
    uint32_t mode = 0;
    if (!read_u32(app + L::kMode, &mode) || mode != 0) {
        waiting_for_ = "the menu's session mode to be Lobby (it is " + std::to_string(mode) + ")";
        return std::nullopt;
    }
    uint8_t world_job = 1, world_list = 0, char_job = 1;
    if (!read_u8(save2 + L::kWorldListJob, &world_job) || !read_u8(save2 + L::kWorldListState, &world_list) ||
        !read_u8(save2 + L::kCharacterListJob, &char_job) || world_job == 1 || world_list != 3 || char_job == 1) {
        waiting_for_ = "the save lists to load";
        return std::nullopt;
    }
    // LOCAL storage only: the storage byte of both slot ids is 0
    const uintptr_t world_rec = save2 + L::kWorldRecords + (req_.world - 1) * L::kWorldRecordSize;
    const uintptr_t char_rec = save2 + L::kCharacterRecords + (req_.character - 1) * L::kCharacterRecordSize;
    uint8_t world_state = 0, char_state = 0;
    if (!read_u8(world_rec, &world_state) || !record_exists(world_state))
        return finish_locked(false, "refused", "LOCAL world slot " + std::to_string(req_.world) + " does not exist");
    if (!read_u8(char_rec, &char_state) || !record_exists(char_state))
        return finish_locked(false, "refused", "LOCAL character slot " + std::to_string(req_.character) + " does not exist");
    world_name_ = read_name(world_rec + L::kWorldRecordName).value_or("");
    character_name_ = read_name(char_rec + L::kCharacterRecordName).value_or("");
    if (!req_.expect_name.empty() && character_name_ != req_.expect_name)
        return finish_locked(false, "refused",
                             "LOCAL character slot " + std::to_string(req_.character) + " is '" + character_name_ +
                                 "', not '" + req_.expect_name + "'");
    armed_ = true;  // from the first post on, a Steam slot can never start
    if (!deps_.select_character(ui, local_slot_id(req_.character)) || !deps_.select_world(ui, local_slot_id(req_.world))) {
        waiting_for_ = "room in the menu's command queue";
        return std::nullopt;  // tried again next frame
    }
    deps_.log("load_world: selected LOCAL character " + std::to_string(req_.character) + " ('" + character_name_ +
              "') and LOCAL world slot " + std::to_string(req_.world) + " ('" + world_name_ + "')");
    step_ = Step::Selected;
    selected_ms_ = now;
    waiting_for_ = "the game to take the selection";
    return std::nullopt;
}

std::optional<MenuStart::Pending> MenuStart::frame_b_locked(void* ui, uint64_t now) {
    const uintptr_t pg = pregame_.load();
    uintptr_t app = 0, save2 = 0;
    read_ptr(pg + L::kPregameGameApp, &app);
    read_ptr(pg + L::kPregameSave2, &save2);
    uint16_t sel_char = 0, sel_world = 0, play = 0;
    uint8_t char_valid = 0, world_valid = 0, play_set = 0, adventure = 1;
    const bool read = app && save2 && read_u16(save2 + L::kSelectedCharacter, &sel_char) &&
                      read_u8(save2 + L::kSelectedCharacterValid, &char_valid) &&
                      read_u16(save2 + L::kSelectedWorld, &sel_world) &&
                      read_u8(save2 + L::kSelectedWorldValid, &world_valid) && read_u16(app + L::kPlaySlot, &play) &&
                      read_u8(app + L::kPlaySlotSet, &play_set) && read_u8(app + L::kAdventure, &adventure);
    const uint16_t want_char = local_slot_id(req_.character), want_world = local_slot_id(req_.world);
    const bool taken = read && sel_char == want_char && char_valid && sel_world == want_world && world_valid &&
                       play == want_world && play_set == 1 && adventure == 0;
    if (!taken) {
        if (now - selected_ms_ <= kSelectMs) return std::nullopt;
        return finish_locked(false, "unavailable",
                             "the game did not take the selection (selected character " + slot_text(sel_char) +
                                 ", world " + slot_text(sel_world) + ", to load " + slot_text(play) + ")");
    }
    // still the main menu of a game that runs no world, in Lobby mode
    if (deps_.any_world_running && deps_.any_world_running())
        return finish_locked(false, "unavailable", "not at the main menu: a world is running");
    if (!menu_active_.load()) return finish_locked(false, "unavailable", "the main menu was left before the start");
    uint32_t mode = 0;
    if (!read_u32(app + L::kMode, &mode) || mode != 0)
        return finish_locked(false, "unavailable",
                             "the menu's session mode is " + std::to_string(mode) + ", not Lobby: not started");
    if (!area_posted_) {
        // only into an empty queue: nothing the menu queued itself (a click) runs between the selection and the start
        if (deps_.queued_commands && deps_.queued_commands(ui) != 0) {
            waiting_for_ = "the menu's command queue to empty";
            return std::nullopt;
        }
        // both in the same frame, so nothing of ours is apart
        if (!deps_.session_area(ui, 0)) return std::nullopt;  // 0 = Offline; tried again next frame
        area_posted_ = true;
    }
    if (!deps_.start(ui)) return std::nullopt;
    start_posted_ = true;
    start_posted_ms_ = now;
    if (const auto name = read_name(app + L::kSessionName)) world_name_ = *name;
    deps_.log("load_world: offline start posted");
    step_ = Step::Started;
    waiting_for_ = "the world to run";
    return std::nullopt;
}

void MenuStart::on_ui_frame(void* ui) {
    std::optional<Pending> done;
    {
        const std::lock_guard<std::mutex> l(lock_);
        const uint64_t now = deps_.now_ms();
        if (step_ == Step::Idle) {
            linger_check_locked(now);
            return;
        }
        if (!ui) return;
        if (now > deadline_ms_) {
            done = finish_locked(false, "timeout", "timed out waiting for " + waiting_for_);
        } else if (step_ == Step::Waiting) {
            done = frame_a_locked(ui, now);
        } else if (step_ == Step::Selected) {
            done = frame_b_locked(ui, now);
        } else if (step_ == Step::Started) {
            if (deps_.world_ready && deps_.world_ready()) {
                if (const auto name = session_world_name()) world_name_ = *name;
                done = finish_locked(true, "", "");
            } else if (left_menu_ && menu_active_.load()) {  // Start Play gave up: the game is back at the main menu
                done = guard_fired_.load()
                           ? finish_locked(false, "refused",
                                           "the guard stopped the start (a Steam character or world, or another name)")
                           : finish_locked(false, "unavailable",
                                           "the game went back to the main menu instead of starting (see the game's log)");
            }
        }
    }
    if (done) deliver(std::move(*done));
}

void MenuStart::poll() {
    std::optional<Pending> done;
    {
        const std::lock_guard<std::mutex> l(lock_);
        const uint64_t now = deps_.now_ms();
        if (step_ == Step::Idle) {
            linger_check_locked(now);
            return;
        }
        if (now <= deadline_ms_) return;
        done = finish_locked(false, "timeout", "timed out waiting for " + waiting_for_);
    }
    deliver(std::move(*done));
}

bool MenuStart::guard(uint8_t* result, uintptr_t save2) {
    if (!armed_.load() || !result || !save2) return false;  // outside an EMF load: never anything
    std::string why;
    uint8_t char_storage = 0, world_storage = 0;
    if (!read_u8(save2 + L::kSelectedCharacter + 1, &char_storage) || char_storage != 0)
        why = "the selected character is not LOCAL storage";
    const uintptr_t app = game_app();
    if (why.empty() && (!app || !read_u8(app + L::kPlaySlot + 1, &world_storage) || world_storage != 0))
        why = "the world to load is not LOCAL storage";
    std::string expect;
    bool test_refuse = false;
    {
        const std::lock_guard<std::mutex> l(lock_);
        expect = guard_name_;
        test_refuse = test_refuse_;
    }
    if (why.empty() && test_refuse) why = "test_refuse: the request asked for a refusal (dev check)";
    if (why.empty()) {
        if (!expect.empty()) {
            uint16_t sel = 0;
            std::optional<std::string> name;
            if (read_u16(save2 + L::kSelectedCharacter, &sel) && (sel & 0xFF) >= 1 && (sel & 0xFF) <= L::kCharacterSlots)
                name = read_name(save2 + L::kCharacterRecords + ((sel & 0xFF) - 1) * L::kCharacterRecordSize +
                                 L::kCharacterRecordName);
            if (!name || *name != expect) why = "the selected character is not '" + expect + "'";
        }
    }
    if (why.empty()) return false;
    result[0] = L::kNoCharacterSelected;
    guard_fired_ = true;
    deps_.log("load_world: the guard stopped the start: " + why);
    return true;
}

// ---- the process's instance and its hooks ----
namespace {
std::unique_ptr<MenuStart> g_menu;
uintptr_t g_ui_global = 0;  // the UI context global (valid only inside the UI frame)

using UiFrameFn = void (*)();
UiFrameFn o_ui_frame = nullptr;
// pregameState(Pregame*, u8 state, u8 phase, Cmd*): a thunk (0x140827a10) passes a 6th byte on the stack, so two
// stack arguments are passed through too.
using PregameStateFn = uint64_t (*)(void*, uint64_t, uint64_t, void*, uint64_t, uint64_t);
PregameStateFn o_pregame_state = nullptr;
std::atomic<bool> g_ui_frame_seen{false};
using CharacterSelectedFn = uint8_t* (*)(uint8_t*, void*);
CharacterSelectedFn o_character_selected = nullptr;
using PostSlotFn = bool (*)(void*, uint16_t);
using PostCharacterFn = bool (*)(void*, uint32_t, uint16_t);
using PostAreaFn = bool (*)(void*, uint8_t);
using PostStartFn = bool (*)(void*);

void hk_ui_frame() {
    o_ui_frame();
    try {
        void* ui = nullptr;
        if (g_menu && g_ui_global && emf::seh_read(reinterpret_cast<const void*>(g_ui_global), &ui, 8) && ui) {
            if (!g_ui_frame_seen.exchange(true)) {
                char line[96];
                std::snprintf(line, sizeof line, "load_world: the UI frame runs (UI context %p)", ui);
                g_menu->log_line(line);
            }
            g_menu->on_ui_frame(ui);
        }
    } catch (...) {
    }
}

uint64_t hk_pregame_state(void* pregame, uint64_t state, uint64_t phase, void* cmd, uint64_t a5, uint64_t a6) {
    try {
        if (g_menu) {
            const auto s = static_cast<uint8_t>(state & 0xFF), p = static_cast<uint8_t>(phase & 0xFF);
            if (s == menu_layout::kMainMenuState)
                g_menu->on_menu_state(pregame, p);
            else
                g_menu->on_other_state(pregame, s, p);
            // the selected character, on the thread that changes it, while the pregame is surely alive (Start Play
            // can free it inside the game's handler): the last note before a world is the character Start Play took
            g_menu->note_character(pregame);
        }
    } catch (...) {
    }
    return o_pregame_state(pregame, state, phase, cmd, a5, a6);
}

uint8_t* hk_character_selected(uint8_t* out, void* save2) {
    uint8_t* r = o_character_selected(out, save2);
    try {
        if (g_menu && g_menu->armed()) g_menu->guard(r ? r : out, reinterpret_cast<uintptr_t>(save2));
    } catch (...) {
    }
    return r;
}

template <class Fn, class... A>
bool guarded_post(Fn fn, A... a) {
    __try {
        return fn(a...);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool post_character(PostCharacterFn fn, void* ui, uint16_t slot) { return guarded_post(fn, ui, 0u, slot); }
bool post_slot(PostSlotFn fn, void* ui, uint16_t slot) { return guarded_post(fn, ui, slot); }
bool post_area(PostAreaFn fn, void* ui, uint8_t area) { return guarded_post(fn, ui, area); }
bool post_start(PostStartFn fn, void* ui) { return guarded_post(fn, ui); }
}  // namespace

MenuStart* menu_start() { return g_menu.get(); }

std::optional<std::string> session_world_name() {
    return g_menu ? g_menu->session_world_name() : std::nullopt;
}

CharacterSelection current_character() { return g_menu ? g_menu->character() : CharacterSelection{}; }

bool install_menu_start(const std::array<Resolved, kSigCount>& sigs, bool known_build, int build_index,
                        MenuStart::Deps deps, std::string* why) {
    auto fail = [why](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (g_menu) return fail("already installed");
    if (build_index != known_build_index(1273043u))
        return fail("the menu's offsets are known for build 1273043's game exe only");
    auto addr = [&](Sig s) -> uintptr_t {
        const auto& r = sigs[static_cast<size_t>(s)];
        return is_usable(r, known_build) ? r.addr : 0;
    };
    const uintptr_t ui_frame = addr(Sig::UiFrame), menu = addr(Sig::PregameState), check = addr(Sig::CharacterSelected);
    const uintptr_t p_char = addr(Sig::PostSelectCharacter), p_world = addr(Sig::PostSelectWorld);
    const uintptr_t p_area = addr(Sig::PostSessionArea), p_start = addr(Sig::PostStart);
    if (!ui_frame || !menu || !check || !p_char || !p_world || !p_area || !p_start)
        return fail("a main-menu signature is not usable on this game build");
    // the UI frame's first instruction `mov rcx, [rip+disp32]` (at +0xA, 7 bytes) names the UI context global
    int32_t disp = 0;
    if (!emf::seh_read(reinterpret_cast<const void*>(ui_frame + 0xA + 3), &disp, 4))
        return fail("the UI frame's code could not be read");
    g_ui_global = static_cast<uintptr_t>(static_cast<intptr_t>(ui_frame + 0x11) + disp);
    deps.read = [](uintptr_t a, void* out, size_t n) { return emf::seh_read(reinterpret_cast<const void*>(a), out, n); };
    deps.select_character = [p_char](void* ui, uint16_t slot) {
        return post_character(reinterpret_cast<PostCharacterFn>(p_char), ui, slot);
    };
    deps.select_world = [p_world](void* ui, uint16_t slot) {
        return post_slot(reinterpret_cast<PostSlotFn>(p_world), ui, slot);
    };
    deps.session_area = [p_area](void* ui, uint8_t area) {
        return post_area(reinterpret_cast<PostAreaFn>(p_area), ui, area);
    };
    deps.start = [p_start](void* ui) { return post_start(reinterpret_cast<PostStartFn>(p_start), ui); };
    deps.queued_commands = [](void* ui) -> size_t {
        uintptr_t data = 0, queue = 0;
        uint32_t count = 0;
        const auto rd = [](uintptr_t a, void* out, size_t n) {
            return a && emf::seh_read(reinterpret_cast<const void*>(a), out, n);
        };
        if (!rd(reinterpret_cast<uintptr_t>(ui) + L::kUiData, &data, 8) || !rd(data + L::kUiCommandQueue, &queue, 8) ||
            !rd(queue + L::kQueueCount, &count, 4))
            return SIZE_MAX;
        return count;
    };
    g_menu = std::make_unique<MenuStart>(std::move(deps));
    // the guard first: it must be in place before anything can be started
    if (!install_emf_hook("characterSelected (load_world guard)", check, reinterpret_cast<void*>(&hk_character_selected),
                          reinterpret_cast<void**>(&o_character_selected))) {
        g_menu.reset();
        return fail("the character check could not be hooked");
    }
    if (!install_emf_hook("pregameState (load_world)", menu, reinterpret_cast<void*>(&hk_pregame_state),
                          reinterpret_cast<void**>(&o_pregame_state)) ||
        !install_emf_hook("uiFrame (load_world)", ui_frame, reinterpret_cast<void*>(&hk_ui_frame),
                          reinterpret_cast<void**>(&o_ui_frame))) {
        // the guard hook stays (it does nothing unarmed); nothing can be requested without the instance
        g_menu.reset();
        return fail("the main menu's state or UI frame could not be hooked");
    }
    return true;
}

}  // namespace emf::core
