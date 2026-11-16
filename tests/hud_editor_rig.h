#pragma once
// A rig for the HUD editor's tests (test_hud_editor.cpp and later the commands and the page): EMF's settings in a map, a
// layout folder of this run's own (removed at the end), and fakes for everything the editor reaches into the game for.
#include <doctest.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/emf_settings.h"
#include "core/hud_editor/editor.h"
#include "core/settings.h"
#include "test_scratch.h"

namespace hud_test {

struct MapIni : emf::core::IniBackend {
    std::map<std::string, std::string> values;
    std::optional<std::string> read(const std::string& mod, const std::string& key) override {
        auto it = values.find(mod + "." + key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const std::string& mod, const std::string& key, const std::string& value) override {
        values[mod + "." + key] = value;
        return true;
    }
};

struct HudRig {
    MapIni ini;
    emf::core::SettingsStore store{&ini};
    std::wstring root, dir;

    uint64_t now = 100000;
    int installs = 0;
    bool install_ok = true;
    std::string install_why;
    std::string other_mod;
    uint32_t fault = 0;
    std::set<uint32_t> down;
    int toggles = 0, writes = 0, other_calls = 0;
    std::vector<std::string> logs;
    std::array<emf::core::hud::LiveInfo, emf::core::hud::kHudElementCount> live{};
    emf::core::hud::ScaleState scale{true, false};
    bool bars = true;  // the bar detour is installed
    // what the build's signatures allow, as deps() hands it over: scale and the bars follow `scale` and `bars` as they
    // are then (a rig without them stands for a build without them), the panel this
    bool build_panel = true;
    emf::core::hud::GateInput gate{false, true, "1287369"};
    std::unique_ptr<emf::core::hud::HudEditor> editor;

    HudRig() {
        static int n = 0;
        root = test_scratch::temp_path(L"emf_hud_rig_" + std::to_wstring(++n));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);  // the folder above emf\hud exists in a game folder
        dir = root + L"\\hud";
        for (emf::core::SettingDef d : emf::core::emf_setting_defs({}))
            REQUIRE(store.declare(emf::core::kEmfSettingsId, std::move(d), nullptr, emf::core::SettingsStore::Declare::Keep));
    }
    ~HudRig() {
        editor.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    emf::core::hud::HudEditorDeps deps() {
        emf::core::hud::HudEditorDeps d;
        d.settings = &store;
        d.dir = dir;
        d.build_number = 1287369;
        d.gate = gate;
        d.install_hooks = [this](std::string* why) {
            ++installs;
            if (!install_ok && why) *why = install_why;
            return install_ok;
        };
        d.other_hud_mod = [this] {
            ++other_calls;
            return other_mod;
        };
        d.fault_code = [this] { return fault; };
        d.live = [this](size_t i) { return live[i]; };
        d.scale_state = [this] { return scale; };
        d.bars_ready = [this] { return bars; };
        d.build_scale = scale.available;
        d.build_bars = d.build_scale && bars;
        d.build_panel = d.build_bars && build_panel;
        d.key_down = [this](uint32_t vk) { return down.count(vk) > 0; };
        d.toggle_page = [this] { ++toggles; };
        d.run = [this](std::function<void()> f) {
            ++writes;
            f();
        };
        d.now_ms = [this] { return now; };
        d.log = [this](const std::string& s) { logs.push_back(s); };
        return d;
    }

    emf::core::hud::HudEditor& make() {
        editor = std::make_unique<emf::core::hud::HudEditor>(deps());
        editor->start();
        return *editor;
    }
    std::vector<emf::core::hud::LayoutFile> files() const { return emf::core::hud::read_layout_dir(dir); }
    void set(const std::string& key, const std::string& value) {
        store.set(emf::core::kEmfSettingsId, "hud_editor." + key, value);
    }
    bool logged(const std::string& part) const {
        for (const auto& l : logs)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
    int count_logged(const std::string& part) const {
        int n = 0;
        for (const auto& l : logs)
            if (l.find(part) != std::string::npos) ++n;
        return n;
    }
};

}  // namespace hud_test
