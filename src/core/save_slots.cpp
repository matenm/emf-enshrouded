#include "core/save_slots.h"

#include <windows.h>

#include <memory>

#include "api.h"
#include "common/seh.h"
#include "core/menu_start.h"

namespace emf::core {

namespace L = menu_layout;
namespace S = slot_layout;

namespace {

constexpr int64_t kEarliest = 1577836800;  // 2020-01-01: no Enshrouded world was played before
constexpr int64_t kLatest = 4102444800;    // 2100-01-01
constexpr int64_t kMaxPlayNs = 100ll * 365 * 24 * 3600 * 1000000000ll;

template <class T>
bool rd(const SlotMemoryRead& read, uintptr_t a, T* v) {
    return a && read && read(a, v, sizeof(T));
}

// {char* data; u64 length} at `field`: the name, or "" when it is not one (unreadable, empty, too long, a NUL inside).
std::string read_name(const SlotMemoryRead& read, uintptr_t field) {
    uintptr_t data = 0;
    uint64_t len = 0;
    if (!rd(read, field, &data) || !rd(read, field + 8, &len) || !data || len == 0 || len > 256) return {};
    std::string s(static_cast<size_t>(len), '\0');
    if (!read(data, s.data(), s.size())) return {};
    for (char c : s)
        if (c == '\0') return {};
    return s;
}

}  // namespace

std::optional<std::vector<saves::SlotSeen>> read_menu_slots(uintptr_t save2, const SlotMemoryRead& read) {
    uint8_t world_job = 1, world_list = 0, char_job = 1;
    if (!rd(read, save2 + L::kWorldListJob, &world_job) || !rd(read, save2 + L::kWorldListState, &world_list) ||
        !rd(read, save2 + L::kCharacterListJob, &char_job) || world_job == 1 || world_list != 3 || char_job == 1)
        return std::nullopt;
    std::vector<saves::SlotSeen> out;
    for (uint32_t st = 0; st < 2; ++st) {
        const char* storage = st == 0 ? "local" : "steam";
        for (uint32_t i = 0; i < L::kCharacterSlots; ++i) {
            const uintptr_t rec = save2 + L::kCharacterRecords + st * L::kCharacterStorageStride + i * L::kCharacterRecordSize;
            saves::SlotSeen s;
            s.storage = storage;
            s.type = "character";
            s.slot = static_cast<int>(i);
            if (!rd(read, rec, &s.state)) return std::nullopt;
            if (s.state == L::kRecordExists || s.state == L::kRecordChanged) {
                s.name = read_name(read, rec + L::kCharacterRecordName);
                uint32_t level = 0;
                if (rd(read, rec + S::kCharacterLevel, &level) && level >= 1 && level <= static_cast<uint32_t>(S::kMaxLevel))
                    s.level = static_cast<int>(level);
            }
            out.push_back(std::move(s));
        }
        for (uint32_t i = 0; i < L::kWorldSlots; ++i) {
            const uintptr_t rec = save2 + L::kWorldRecords + st * L::kWorldStorageStride + i * L::kWorldRecordSize;
            saves::SlotSeen s;
            s.storage = storage;
            s.type = "world";
            s.slot = static_cast<int>(i);
            if (!rd(read, rec, &s.state)) return std::nullopt;
            if (s.state == L::kRecordExists || s.state == L::kRecordChanged) {
                s.name = read_name(read, rec + L::kWorldRecordName);
                int64_t played = 0, ns = 0;
                if (rd(read, rec + S::kWorldLastPlayed, &played) && played >= kEarliest && played <= kLatest) s.played = played;
                if (rd(read, rec + S::kWorldPlayTimeNs, &ns) && ns > 0 && ns < kMaxPlayNs) s.play_seconds = ns / 1000000000ll;
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::optional<uint16_t> selected_character_slot(uintptr_t save2, const SlotMemoryRead& read) {
    uint16_t id = 0;
    uint8_t valid = 0;
    if (!rd(read, save2 + L::kSelectedCharacter, &id) || !rd(read, save2 + L::kSelectedCharacterValid, &valid) || !valid)
        return std::nullopt;
    const uint32_t storage = id >> 8, index = id & 0xFF;
    if (storage > 1 || index < 1 || index > L::kCharacterSlots) return std::nullopt;
    return id;
}

SaveInfoRecorder::SaveInfoRecorder(Deps deps) : deps_(std::move(deps)) {
    if (!deps_.now) deps_.now = [] { return static_cast<int64_t>(time(nullptr)); };
    if (!deps_.log) deps_.log = [](const std::string&) {};
    if (!deps_.sleep) deps_.sleep = [](unsigned ms) { Sleep(ms); };
}

void SaveInfoRecorder::save(const saves::SaveInfo& info) {
    if (saves::write_save_info(deps_.path, info)) ++writes_;
    else deps_.log("save info: could not write save-info.json in %LOCALAPPDATA%\\EMF");
}

bool SaveInfoRecorder::look_once() {
    auto seen = read_menu_slots(save2_.load(), deps_.read);
    if (!seen) return false;
    const std::string account = deps_.steam_user ? deps_.steam_user() : std::string();
    for (auto& s : *seen)
        if (s.storage == "steam") s.steam_id = account;  // names never cross Steam accounts
    const std::lock_guard<std::mutex> l(file_lock_);
    bool changed = false;
    saves::SaveInfo info = saves::merge_seen(saves::read_save_info(deps_.path), *seen, deps_.now(), &changed);
    if (changed) {
        info.updated = deps_.now();
        save(info);
        size_t named = 0;
        for (const auto& s : info.slots) named += !s.name.empty();
        deps_.log("save info: " + std::to_string(named) + " slot name(s) from the main menu written to save-info.json");
    }
    return true;
}

void SaveInfoRecorder::on_pregame(void* pregame, uint8_t state, uint8_t phase) {
    if (!pregame) return;
    uintptr_t save2 = 0;
    if (!rd(deps_.read, reinterpret_cast<uintptr_t>(pregame) + L::kPregameSave2, &save2) || !save2) return;
    if (state == L::kMainMenuState) {
        if (phase == 2) {
            menu_ = false;
            return;
        }
        if (phase != 0) return;  // entered: one round of looks per visit
        save2_ = save2;
        menu_ = true;
        const unsigned visit = ++visit_;
        auto work = [this, visit] {
            for (unsigned i = 0; i < deps_.tries; ++i) {
                if (visit != visit_.load() || !menu_.load()) return;
                if (look_once()) return;
                deps_.sleep(1000);
            }
        };
        if (deps_.run) deps_.run(work);
        else work();
        return;
    }
    if (phase != 0) return;
    menu_ = false;  // another state (Start Play, ...): the pregame may be freed after this call
    if (state == 3) {  // Start Play: the character it takes is the selected one
        if (const auto id = selected_character_slot(save2, deps_.read)) pending_character_ = *id;
    }
}

void SaveInfoRecorder::on_world_loaded(uint16_t slot_id) {
    const uint16_t character = pending_character_.exchange(0);
    auto work = [this, slot_id, character] {
        const std::lock_guard<std::mutex> l(file_lock_);
        saves::SaveInfo info = saves::read_save_info(deps_.path);
        bool any = false, changed = false;
        const int64_t now = deps_.now();
        const std::string account = deps_.steam_user ? deps_.steam_user() : std::string();
        const auto mark = [&](uint16_t id, const char* type) {
            if ((id & 0xFF) == 0 || (id >> 8) > 1) return;
            info = saves::note_loaded(info, (id >> 8) ? "steam" : "local", type, (id & 0xFF) - 1, now, &changed, account);
            any |= changed;
        };
        mark(slot_id, "world");
        mark(character, "character");
        if (any) {
            info.updated = now;
            save(info);
        }
    };
    if (deps_.run) deps_.run(work);
    else work();
}

// ---- the process's recorder and its hook ----
namespace {
std::unique_ptr<SaveInfoRecorder> g_recorder;
using PregameStateFn = uint64_t (*)(void*, uint64_t, uint64_t, void*, uint64_t, uint64_t);
PregameStateFn o_pregame_state = nullptr;

uint64_t hk_pregame_state(void* pregame, uint64_t state, uint64_t phase, void* cmd, uint64_t a5, uint64_t a6) {
    try {
        if (g_recorder) g_recorder->on_pregame(pregame, static_cast<uint8_t>(state & 0xFF), static_cast<uint8_t>(phase & 0xFF));
    } catch (...) {
    }
    return o_pregame_state(pregame, state, phase, cmd, a5, a6);
}
}  // namespace

SaveInfoRecorder* save_info_recorder() { return g_recorder.get(); }

bool install_save_info(const std::array<Resolved, kSigCount>& sigs, bool known_build, int build_index,
                       SaveInfoRecorder::Deps deps, std::string* why) {
    auto fail = [why](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (g_recorder) return fail("already installed");
    if (build_index != known_build_index(1273043u)) return fail("the slot records' offsets are known for build 1273043's game exe only");
    const auto& r = sigs[static_cast<size_t>(Sig::PregameState)];
    if (!is_usable(r, known_build)) return fail("the pregame state signature is not usable on this game build");
    deps.read = [](uintptr_t a, void* out, size_t n) { return emf::seh_read(reinterpret_cast<const void*>(a), out, n); };
    g_recorder = std::make_unique<SaveInfoRecorder>(std::move(deps));
    if (!install_emf_hook("pregameState (save info)", r.addr, reinterpret_cast<void*>(&hk_pregame_state),
                          reinterpret_cast<void**>(&o_pregame_state))) {
        g_recorder.reset();
        return fail("the pregame state handler could not be hooked");
    }
    return true;
}

}  // namespace emf::core
