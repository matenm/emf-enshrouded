// A shipped Lua mod run by the Lua host from a copy of its folder, with fake engine events, components and commands
// (the round 2 mods' tests: test_lua_round2.cpp for Grave Keeper, test_bounty.cpp for Bounty Board).
#pragma once
#include <windows.h>

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/manifest.h"
#include "emf/generated/components.hpp"
#include "lua_host/host.h"
#include "lua_test_fake.h"

using emf::lua::Host;
namespace ecs = emf::gen::ecs;

namespace {
uint32_t player_one() { return 1; }

std::string slurp(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void deliver(Fake& f, const char* event, const std::vector<uint8_t>& bytes) {
    for (const auto& s : f.subs)
        if (s.name == event) s.cb(kServer, bytes.data(), static_cast<uint32_t>(bytes.size()), s.user);
}

std::vector<uint8_t> died_event(uint32_t entity) {
    std::vector<uint8_t> b(ecs::EntityDiedEvent::kSize, 0);
    std::memcpy(&b[offsetof(ecs::EntityDiedEvent, deadEntityId)], &entity, 4);
    return b;
}
std::vector<uint8_t> hit_event(uint32_t source, uint32_t target) {
    std::vector<uint8_t> b(ecs::HitEvent::kSize, 0);
    std::memcpy(&b[offsetof(ecs::HitEvent, rootSourceId)], &source, 4);
    std::memcpy(&b[offsetof(ecs::HitEvent, targetId)], &target, 4);
    return b;
}
std::vector<uint8_t> pickup_event(uint32_t by, uint32_t from, uint32_t item, uint32_t count) {
    std::vector<uint8_t> b(ecs::PickupEvent::kSize, 0);
    std::memcpy(&b[offsetof(ecs::PickupEvent, itemId)], &item, 4);
    std::memcpy(&b[offsetof(ecs::PickupEvent, itemCount)], &count, 4);
    std::memcpy(&b[offsetof(ecs::PickupEvent, pickedUpByEntityId)], &by, 4);
    std::memcpy(&b[offsetof(ecs::PickupEvent, pickedUpFromEntityId)], &from, 4);
    return b;
}

template <class T>
T& component(Fake& f, uint32_t id) {
    auto& v = f.components[{T::kHash, id}];
    v.assign(sizeof(T), 0);
    return *reinterpret_cast<T*>(v.data());
}
void place(Fake& f, uint32_t id, double x, double y, double z) {
    auto& t = component<ecs::CurrentTransform>(f, id);
    t.transform.position.x = static_cast<int64_t>(x * 4294967296.0);
    t.transform.position.y = static_cast<int64_t>(y * 4294967296.0);
    t.transform.position.z = static_cast<int64_t>(z * 4294967296.0);
}
void add_entity(Fake& f, uint32_t id) {
    if (std::find(f.entities.begin(), f.entities.end(), id) == f.entities.end()) f.entities.push_back(id);
}
// A player: PlayerState, a position, an account in BasicPlayerInfos slot id - 1.
void add_player(Fake& f, uint32_t id, uint64_t account, double x, double y, double z) {
    component<ecs::PlayerState>(f, id);
    f.player_info(id - 1, id, account);
    place(f, id, x, y, z);
}
// A tombstone as the game leaves one (seen in game): Inventory with stacks, CreateItemLoot, DestroyOnLoot, no Health.
// `fresh`: as the game leaves one right after a death (seen in game, entity 6902): no Inventory of its own, its stacks
// in an inventory entity (id + 1) its InventorySetup links; else as one loaded from a save (entity 1039: its own
// Inventory).
void add_tombstone(Fake& f, uint32_t id, double x, double y, double z, const std::vector<std::pair<uint32_t, uint32_t>>& stacks,
                   bool fresh = false) {
    place(f, id, x, y, z);
    if (fresh) {
        auto& setup = component<ecs::InventorySetup>(f, id);
        setup.isInitialized = true;
        setup.linksEntities[0].id = id + 1;
        setup.linksCategories[0] = emf::gen::InventoryCategory::Generic;
    }
    auto& inv = component<ecs::Inventory>(f, fresh ? id + 1 : id);
    for (size_t i = 0; i < stacks.size() && i < 8; ++i) {
        inv.slots[i].id.value = stacks[i].first;
        inv.slots[i].data.count = stacks[i].second;
    }
    component<ecs::CreateItemLoot>(f, id);
    component<ecs::DestroyOnLoot>(f, id);
    component<ecs::LifeTime>(f, id);
    add_entity(f, id);
    if (fresh) add_entity(f, id + 1);
}
void remove_entity(Fake& f, uint32_t id) {
    for (auto it = f.components.begin(); it != f.components.end();)
        it = it->first.second == id ? f.components.erase(it) : std::next(it);
    f.entities.erase(std::remove(f.entities.begin(), f.entities.end(), id), f.entities.end());
}
void set_daytime(Fake& f, double hours) {
    std::vector<uint8_t> d(0x70, 0);
    const auto ns = static_cast<int64_t>(hours * 3600e9);
    std::memcpy(d.data() + 0x48, &ns, 8);
    f.singletons["g38_daytime::Daytime"] = d;
}

// Writes `text` with a modification time `step` (100 ns units << 32) later, so the host sees a change at once.
void write_later(const std::wstring& path, const std::string& text, int step) {
    write(path, text);
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ft.dwHighDateTime += static_cast<DWORD>(step);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

// A shipped Lua mod copied into the rig's mods folder (its world data stays in the test's folder), its settings at
// mod.json's defaults (plus `settings`), loaded, a world loaded (slot 1) and one second played.
struct ModRig {
    Rig r;
    std::unique_ptr<Host> host;
    std::wstring folder, data;
    std::string id;
    // `also`: more shipped mods loaded beside it, at their default settings (e.g. two class mods sharing a wheel).
    ModRig(const char* mod, const wchar_t* name, const std::map<std::string, std::string>& settings,
           const std::function<void(Fake&)>& world = nullptr, const std::vector<std::string>& also = {})
        : r(name), id(mod) {
        r.api.local_player = player_one;
        const std::wstring wid(id.begin(), id.end());
        folder = r.dir + L"\\" + wid;
        add_mod(id);
        for (const auto& [k, v] : settings) r.fake.settings[id + "." + k] = v;
        for (const auto& other : also) add_mod(other);
        if (world) world(r.fake);
        data = r.dir + L"\\..\\config\\" + wid + L"\\0001.txt";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(data).parent_path().lexically_normal(), ec);
        DeleteFileW(data.c_str());
        host = std::make_unique<Host>(&r.api, r.dir);
        host->load_all();
        REQUIRE_MESSAGE(host->status(id) == "running", host->status(id));
        fire_load(1);
        seconds(1);
    }
    ~ModRig() { DeleteFileW(data.c_str()); }
    // Copies the shipped mod `mid` into the mods folder and lists it as a script mod, its settings at their defaults.
    void add_mod(const std::string& mid) {
        const std::wstring w(mid.begin(), mid.end());
        const std::wstring to = r.dir + L"\\" + w;
        CreateDirectoryW(to.c_str(), nullptr);
        const std::filesystem::path src = std::filesystem::path(EMF_SOURCE_DIR) / "mods" / mid;
        for (const auto& e : std::filesystem::directory_iterator(src))
            if (e.is_regular_file()) std::filesystem::copy_file(e.path(), std::filesystem::path(to) / e.path().filename(),
                                                                 std::filesystem::copy_options::overwrite_existing);
        const auto m = emf::core::parse_manifest(slurp((src / "mod.json").wstring()));
        REQUIRE_MESSAGE(m.manifest, m.error);
        for (const auto& d : m.manifest->settings) r.fake.settings[mid + "." + d.key] = d.default_value;
        EmfModInfo info{};
        info.size = sizeof info;
        info.status = EMF_MOD_SCRIPT;
        std::snprintf(info.id, sizeof info.id, "%s", mid.c_str());
        std::snprintf(info.source, sizeof info.source, "%s\\", mid.c_str());
        std::snprintf(info.main, sizeof info.main, "main.lua");
        r.fake.mods.push_back(info);
    }
    void seconds(int n) {
        for (int i = 0; i < n; ++i) {
            r.fake.tick += 60;
            host->tick(kServer);
        }
    }
    // Runs a command as the host (console) or, with `chat_player`, as that player from the game's chat.
    std::string reply(const std::string& command, uint32_t chat_player = 0, uint64_t account = 0) {
        r.fake.replies.clear();
        r.fake.sender = chat_player ? EmfCommandSender{sizeof(EmfCommandSender), chat_player, account, 1, {}} : EmfCommandSender{};
        r.run_command(command);
        r.fake.sender = EmfCommandSender{};
        std::string all;
        for (const auto& l : r.fake.replies) all += (all.empty() ? "" : "\n") + l;
        return all;
    }
    // Inserts `insert` after the first `after` in the copied main.lua and lets the host reload it (the mod's tables
    // at the top of main.lua are meant to be edited while the game runs).
    void edit_main(const std::string& after, const std::string& insert) {
        std::string lua = slurp(folder + L"\\main.lua");
        const size_t at = lua.find(after);
        REQUIRE(at != std::string::npos);
        lua.insert(at + after.size(), insert);
        write_later(folder + L"\\main.lua", lua, 1);
        host->check_reload_now();
        REQUIRE_MESSAGE(host->status(id) == "running", host->status(id));
        seconds(1);
    }
    void send_events() {
        for (auto& p : r.fake.programs) p.fn(nullptr, p.user);
    }
    std::string hud(const std::string& line) {
        auto it = r.fake.hud.find(id + "." + line);
        return it == r.fake.hud.end() ? "" : it->second;
    }
    bool noted(const std::string& needle) {
        for (const auto& n : r.fake.notes)
            if (n.find(needle) != std::string::npos) return true;
        return false;
    }
};

// The loot events the mod sent (guest, host), with lootAll and bypassInteraction checked.
std::vector<std::pair<uint32_t, uint32_t>> loot_events(Fake& f) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto& [name, b] : f.emitted) {
        if (name != "LootInteractionEvent") continue;
        const auto* e = reinterpret_cast<const ecs::LootInteractionEvent*>(b.data());
        CHECK(e->lootAll);
        CHECK(e->bypassInteraction);
        out.push_back({e->guestId.id, e->hostId.id});
    }
    return out;
}
}  // namespace
