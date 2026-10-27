// emf_bridge_fake: serves the debug bridge's protocol with made-up game data, for testing bridge clients without the
// game (scripts/bridge_clients_test.ps1 runs it).
//   emf_bridge_fake.exe <folder> [seconds] [pipe-pid]
// It listens on \\.\pipe\emf-bridge-<pid> (the same security as the game's bridge), writes <folder>\emf\bridge.txt and
// ends after <seconds> (default 60) or when <folder>\emf\bridge.stop appears. The world "runs" 2 s after the start and
// the player exists 1 s later; a log line comes every 300 ms ("[life_skills] fake level up" every tenth), a pickup
// event every second; screenshots are a small picture written into <folder>\emf\screens; load_world answers like the
// game for LOCAL world slot 1 "My World 1" with LOCAL character slot 1 "Character 2". The state's character is that
// LOCAL character, unless <folder>\emf\fake_character.txt says "steam" (Steam slot 1, "Steam Hero") or "unknown" (a
// test writes it, read at every state request).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "common/log_ring.h"
#include "core/bridge/pipe_server.h"
#include "core/bridge/protocol.h"

#include "core/bridge/screens.h"
#include "core/bridge/service.h"
#include "core/bridge/state.h"

using namespace emf::core;
using namespace emf::core::bridge;

namespace {

std::string utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The state's character: LOCAL slot 1 "Character 2", or what <emf>\fake_character.txt names (steam, unknown).
StateSnapshot::Character fake_character(const std::wstring& emf_dir) {
    char text[32] = {};
    HANDLE f = CreateFileW((emf_dir + L"\\fake_character.txt").c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        ReadFile(f, text, sizeof text - 1, &n, nullptr);
        CloseHandle(f);
    }
    const std::string kind(text);
    if (kind.rfind("unknown", 0) == 0) return StateSnapshot::Character{};
    if (kind.rfind("steam", 0) == 0) return StateSnapshot::Character{"steam", 1u, std::string("Steam Hero")};
    return StateSnapshot::Character{"local", 1u, std::string("Character 2")};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: emf_bridge_fake <folder> [seconds]\n");
        return 2;
    }
    const std::wstring emf_dir = std::wstring(argv[1]) + L"\\emf";
    CreateDirectoryW(argv[1], nullptr);
    CreateDirectoryW(emf_dir.c_str(), nullptr);
    const int seconds = argc > 2 ? _wtoi(argv[2]) : 60;
    const uint64_t start = GetTickCount64();
    const uint32_t pid = GetCurrentProcessId();
    emf::LogRing ring(5000);
    PipeServer pipe;
    std::unique_ptr<Service> service;

    Sources s;
    s.hello = HelloInfo{"fake", 16, 1273043, false, pid};
    s.now_ms = [] { return GetTickCount64(); };
    s.console = [](const std::string& text, std::function<void(const std::string&)> line,
                   std::function<void(const std::string&)> done) {
        line("> " + text);
        if (text.rfind("put ", 0) == 0) {  // like the real put: its result comes one server step later
            line("put: queued");
            done("ran");
            std::thread([line] {
                Sleep(20);
                line("put 1 x Material_T1_Wood_Logs into entity 1: 1 added");
            }).detach();
            return;
        }
        if (text == "nosuch") {
            line("unknown command 'nosuch' (type help)");
            done("unknown");
            return;
        }
        if (text == "near 10") {
            line("  1: 0.0 m (100.5, 50.0, 200.3) player inventory health");  // the format of the real "near"
            line("  12: 2.1 m (102.0, 50.0, 201.0) inventory");
            line("  15: 6.8 m (106.0, 51.0, 204.0) villager inventory health");
            line("3 entities within 10 m");
        } else {
            line("fake: ran '" + text + "'");
        }
        done("ran");
    };
    s.state = [start, emf_dir](std::function<void(std::string)> done) {
        StateSnapshot st;
        st.character = fake_character(emf_dir);
        if (GetTickCount64() - start > 2000) {
            st.world_status = "running";
            st.world_kind = "server";
            st.world_tick = static_cast<uint32_t>((GetTickCount64() - start) / 16);
            st.time_of_day_h = 13.5;
            st.player_id = 1;
            st.position = std::array<double, 3>{100.5, 50.0, 200.25};
            // a world it hosts: the attributes (API v18) with the max
            st.health = pick_health(std::nullopt, std::make_pair(180, 250), 180);
            st.world_slot = 1;
            st.world_name = "My World 1";
        }
        st.overlay_running = true;
        st.hud.push_back(HudLine{"clock", "13:30", HudPlace::BottomRight, 0});
        st.presents = (GetTickCount64() - start) / 16;
        st.frame_ms = 16.6;
        done(state_json(st));
    };
    s.mods = [] {
        ModRecord a;
        a.id = "clock";
        a.name = "Clock";
        a.version = "1.1.0";
        a.source = "clock\\";
        ModRecord b;
        b.id = "quick_stack";
        b.name = "Quick Stack";
        b.version = "1.0.0";
        b.status = ModStatus::Disabled;
        b.reason = "switched off (emf\\config\\quick_stack.ini)";
        b.source = "quick_stack\\";
        return mods_json({a, b});
    };
    s.log = &ring;
    s.world_running = [start] { return GetTickCount64() - start > 2000; };
    s.player_exists = [start] { return GetTickCount64() - start > 3000; };
    s.shot_start = [emf_dir](const ShotOptions& opt, std::function<void(const ShotResult&)> done) -> Refusal {
        std::thread([emf_dir, done, opt] {
            const uint32_t w = opt.max_width && opt.max_width < 64 ? opt.max_width : 64;
            const uint32_t h = w * 36 / 64;
            std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* p = &bgr[(static_cast<size_t>(y) * w + x) * 3];
                    p[0] = static_cast<uint8_t>(x * 4);
                    p[1] = static_cast<uint8_t>(y * 7);
                    p[2] = 128;
                }
            SYSTEMTIME t;
            GetLocalTime(&t);
            const std::wstring path = emf_dir + L"\\screens\\" + screenshot_name(t, 1);
            ShotResult r;
            std::string why;
            const auto png = encode_png_bgr(bgr.data(), w, h, &why);
            if (png.empty() || !write_screenshot(emf_dir, path, png, &why)) {
                r.error = why;
            } else {
                r.ok = true;
                r.path = utf8(path);
                r.width = w;
                r.height = h;
                r.hdr = !opt.overlay;  // (made up: lets a client test show the field)
            }
            Sleep(50);  // as if waiting for the next frame
            done(r);
        }).detach();
        return {};
    };
    s.shot_cancel = [] { return false; };
    s.mod_key = [](uint32_t, KeyAction a, uint32_t hold, std::function<void(const KeyResult&)> done) -> Refusal {
        KeyResult r;
        r.ok = true;
        if (a != KeyAction::Press) {
            done(r);
            return {};
        }
        std::thread([hold, done, r]() mutable {
            Sleep(hold);
            r.mod_reads = hold / 16;  // a mod polling every server step
            done(r);
        }).detach();
        return {};
    };
    // load_world, made up like the game's answers: LOCAL world slot 1 is "My World 1" and LOCAL character slot 1 is
    // "Character 2"; other slots do not exist. The reply comes 300 ms later, as if the world had loaded. (The fake's
    // world runs anyway, so it does not refuse a request "not at the main menu" as the game does.)
    s.load_world = [](const LoadWorldParams& p, std::function<void(const LoadWorldReply&)> done) -> Refusal {
        if (p.slot > 10) return Refusal{code::kBadRequest, "slot must be a LOCAL world slot from 1 to 10"};
        if (p.character > 16) return Refusal{code::kBadRequest, "character must be a LOCAL character slot from 1 to 16"};
        std::thread([p, done] {
            Sleep(300);
            LoadWorldReply r;
            r.code = code::kRefused;
            if (p.slot != 1) {
                r.error = "LOCAL world slot " + std::to_string(p.slot) + " does not exist";
            } else if (p.character != 1) {
                r.error = "LOCAL character slot " + std::to_string(p.character) + " does not exist";
            } else if (!p.expect_name.empty() && p.expect_name != "Character 2") {
                r.error = "LOCAL character slot 1 is 'Character 2', not '" + p.expect_name + "'";
            } else if (p.test_refuse) {
                r.error = "the guard stopped the start (a Steam character or world, or another name)";
            } else {
                r.ok = true;
                r.code.clear();
                r.slot = 1;
                r.character = 1;
                r.world = "My World 1";
                r.character_name = "Character 2";
                r.ms = 300;
            }
            done(r);
        }).detach();
        return {};
    };
    std::atomic<bool> quit{false};
    s.quit = [&quit]() -> Refusal {
        quit = true;  // the fake ends a moment later, as the game would after saving
        return {};
    };
    s.event_names = game_event_names();
    Output o;
    o.send = [&pipe](ClientId c, std::string line, bool droppable) { pipe.send(c, std::move(line), droppable); };
    o.disconnect = [&pipe](ClientId c) { pipe.disconnect(c); };
    service = std::make_unique<Service>(s, o);

    PipeHandler h;
    h.connected = [&](ClientId c) { service->on_connect(c); };
    h.line = [&](ClientId c, std::string_view line) { service->on_line(c, line); };
    h.too_long = [&](ClientId c) { service->on_too_long(c); };
    h.disconnected = [&](ClientId c) { service->on_disconnect(c); };
    h.poll = [&] { service->poll(); };
    // [pipe-pid]: name the pipe after another process (the clients must refuse a pipe its named game does not serve)
    const std::wstring name = bridge_pipe_name(argc > 3 ? static_cast<uint32_t>(_wtoi(argv[3])) : pid);
    std::string why;
    if (!pipe.start(name, pipe_sddl(current_user_sid(), current_integrity_sid()), h, kMaxLineIn, kMaxClients, &why)) {
        std::fprintf(stderr, "emf_bridge_fake: %s\n", why.c_str());
        return 1;
    }
    ring.set_notify(pipe.wake_event());
    const std::wstring info = emf_dir + L"\\bridge.txt";
    const std::wstring stop = emf_dir + L"\\bridge.stop";
    DeleteFileW(stop.c_str());
    {
        const std::string text = bridge_info_text(name, pid);
        HANDLE f = CreateFileW(info.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n = 0;
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
            CloseHandle(f);
        }
    }
    std::printf("emf_bridge_fake: serving %s for %d s\n", utf8(name).c_str(), seconds);
    std::fflush(stdout);
    for (int tick = 1; GetTickCount64() - start < static_cast<uint64_t>(seconds) * 1000; ++tick) {
        Sleep(300);
        if (GetFileAttributesW(stop.c_str()) != INVALID_FILE_ATTRIBUTES) break;
        if (quit) {
            Sleep(300);
            break;
        }
        char line[96];
        std::snprintf(line, sizeof line, "[fake] tick %d", tick);
        ring.push(line);
        if (tick % 10 == 0) ring.push("[life_skills] fake level up");
        if (tick % 3 == 0) service->publish("pickup", pickup_fields(0x12345678u, "Material_T1_Wood_Logs", 5, 1, 0, {1, 2, 3}));
    }
    pipe.stop(2000);
    DeleteFileW(info.c_str());
    DeleteFileW(stop.c_str());
    return 0;
}
