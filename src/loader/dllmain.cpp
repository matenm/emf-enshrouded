// emf_loader: installed as dinput8.dll next to enshrouded.exe. Loads <game>\emf\emf_core.dll from a
// thread (never under the loader lock) and calls emf_core_start(game_dir). In the dedicated server it also holds the
// main thread at the exe's entry point until emf_core_start has returned (see entry_hold.h).
//
// First it starts the DLL mods listed in dinput8-chain.ini (chain_list.h): other mods that want the name dinput8.dll
// are renamed and listed there by EMF's installer. They are loaded right here in DllMain, the way the community's
// chain loaders do it and the moment they would have started as dinput8.dll themselves. The entry point is not
// touched for them: ShroudForge patches it too.
#include <windows.h>

#include <share.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "chain_list.h"
#include "chain_load.h"
#include "entry_hold.h"
#include "layer_env.h"
#include "loader_id.h"

// Marks this DLL as EMF's loader for the installer and the core (loader_id.h); exported, so it is kept.
extern "C" __declspec(dllexport) const char emf_loader_identity[] = EMF_LOADER_MARKER;

namespace {
void loader_log(const std::wstring& dir, const char* msg) {
    // tiny separate log so a missing/broken core is visible
    // shared: another thread (or the manager) may have it open at the same time
    if (FILE* f = _wfsopen((dir + L"\\emf\\emf_loader.log").c_str(), L"ab", _SH_DENYNO)) {
        fprintf(f, "emf_loader: %s\n", msg);
        fclose(f);
    }
    OutputDebugStringA("emf_loader: ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}

HANDLE g_core_done = nullptr;  // set once emf_core_start returned or could not run (server only)
std::atomic<emf::loader::CoreOutcome> g_core_outcome{emf::loader::CoreOutcome::Running};  // how init_thread ended
emf::loader::EntryHold g_hold;
constexpr DWORD kHoldMs = 20000;

std::wstring game_dir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

DWORD WINAPI init_thread(LPVOID) {
    struct Done {
        ~Done() {
            if (g_core_done) SetEvent(g_core_done);
        }
    } done;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    HMODULE core = LoadLibraryW((dir + L"\\emf\\emf_core.dll").c_str());
    if (!core) {
        loader_log(dir, "emf\\emf_core.dll not found or failed to load; running the game without mods");
        g_core_outcome = emf::loader::CoreOutcome::Missing;
        return 0;
    }
    using StartFn = void (*)(const wchar_t*);
    auto start = reinterpret_cast<StartFn>(GetProcAddress(core, "emf_core_start"));
    if (!start) {
        loader_log(dir, "emf_core.dll has no emf_core_start export");
        g_core_outcome = emf::loader::CoreOutcome::NoExport;
        return 0;
    }
    loader_log(dir, "starting emf_core");
    start(dir.c_str());
    g_core_outcome = emf::loader::CoreOutcome::Started;
    return 0;
}

// The dedicated server's entry point lands here first (main thread, loader lock free): it waits until EMF has hooked
// the world setup and loaded the mods, puts the entry point's bytes back and runs it.
DWORD WINAPI held_entry(void* arg) {
    const ULONGLONG start = GetTickCount64();
    const DWORD w = WaitForSingleObject(g_core_done, kHoldMs);
    loader_log(game_dir(),
               emf::loader::hold_message(w == WAIT_OBJECT_0, g_core_outcome.load(), GetTickCount64() - start).c_str());
    if (!emf::loader::release_entry(g_hold)) {
        loader_log(game_dir(), "could not restore the server's entry point; stopping");
        ExitProcess(1);
    }
    using EntryFn = DWORD(WINAPI*)(void*);
    return reinterpret_cast<EntryFn>(g_hold.entry)(arg);
}

std::string read_file(const std::wstring& path) {
    std::string text;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (1u << 20)) text.append(buf, n);
    fclose(f);
    return text;
}


// Starts the DLLs listed in dinput8-chain.ini, logs each and writes emf\chain_status.txt (read by EMF's core, which
// starts after this, and by the manager).
void run_chain(const std::wstring& dir) {
    const auto names = emf::loader::parse_chain_list(read_file(dir + L"\\dinput8-chain.ini"));
    const auto results = emf::loader::load_chain(
        names,
        [&](const std::wstring& name) {
            return GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
        },
        [&](const std::wstring& name, unsigned long* error) {
            // A crash in the DLL's own DllMain comes back as error 1114 (Windows unloads it); one that hangs or ends
            // the process stops the game as it would have without EMF.
            if (LoadLibraryExW((dir + L"\\" + name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return true;
            *error = GetLastError();
            return false;
        });
    for (const auto& r : results) loader_log(dir, emf::loader::chain_line(r).c_str());
    const std::string text = emf::loader::chain_status_text(results);
    if (FILE* f = _wfsopen((dir + L"\\emf\\chain_status.txt").c_str(), L"wb", _SH_DENYNO)) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

void put_first(const wchar_t* var, const std::wstring& item) {
    std::wstring current;
    const DWORD n = GetEnvironmentVariableW(var, nullptr, 0);
    if (n) {
        current.resize(n);
        current.resize(GetEnvironmentVariableW(var, current.data(), n));
    }
    SetEnvironmentVariableW(var, emf::loader::list_with_first(current, item).c_str());
}

// The in-game console draws through EMF's Vulkan layer (emf\vk\VkLayer_EMF_console.json -> emf\emf_core.dll). The
// Vulkan loader reads these variables when the game creates its instance, so they are set here, before any game code
// runs, and only for this process (nothing in the registry).
void enable_vulkan_layer() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    const std::wstring vk = dir + L"\\emf\\vk";
    if (GetFileAttributesW((vk + L"\\VkLayer_EMF_console.json").c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (GetPrivateProfileIntW(L"overlay", L"enabled", 1, (dir + L"\\emf\\emf_core.ini").c_str()) == 0) return;
    put_first(L"VK_ADD_LAYER_PATH", vk);
    put_first(L"VK_INSTANCE_LAYERS", L"VK_LAYER_EMF_console");
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // One EMF loader per process: a copy of it listed in the chain file under another name (a player following
        // another tool's chain instructions) only forwards DirectInput and does nothing else.
        const std::wstring guard = L"Local\\EMF_loader_" + std::to_wstring(GetCurrentProcessId());
        CreateMutexW(nullptr, FALSE, guard.c_str());  // kept for the life of the process
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(inst, self, MAX_PATH);
            loader_log(game_dir(), ("another EMF loader already runs in this game; this copy (" +
                                    emf::loader::utf8_name(self) + ") does nothing").c_str());
            return TRUE;
        }
        run_chain(game_dir());
        enable_vulkan_layer();  // after the chained DLLs: it puts EMF's layer first even if one of them set the list
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        uint8_t* entry = nullptr;
        bool want_hold = false;
        if (emf::loader::is_server_exe(exe)) {
            entry = emf::loader::exe_entry_point();
            if (!emf::loader::entry_is_msvc_startup(entry)) {
                // say why: another tool's jump (and into which DLL), or code EMF does not know
                const uint64_t target = emf::loader::entry_jump_target(entry, [](uint64_t at, uint64_t* out) {
                    SIZE_T got = 0;
                    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, sizeof *out, &got) &&
                           got == sizeof *out;
                });
                std::string module;
                HMODULE owner = nullptr;
                wchar_t path[MAX_PATH];
                if (target && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                 reinterpret_cast<LPCWSTR>(target), &owner) &&
                    owner && GetModuleFileNameW(owner, path, MAX_PATH)) {
                    const std::wstring p(path);
                    module = emf::loader::utf8_name(p.substr(p.find_last_of(L"\\/") + 1));
                }
                loader_log(game_dir(), emf::loader::entry_not_held_message(entry, target, module).c_str());
            } else {
                g_core_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                want_hold = g_core_done != nullptr;
                if (!want_hold)
                    loader_log(game_dir(), "could not create an event: the server's start is not held (mods may load "
                                           "after its world setup)");
            }
        }
        // The thread first (it runs once DllMain has returned: the loader lock), the hold only if the thread exists.
        DWORD thread_error = 0;
        const auto started = emf::loader::start_core(
            want_hold,
            [&thread_error] {
                HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
                if (!t) {
                    thread_error = GetLastError();
                    return false;
                }
                CloseHandle(t);
                return true;
            },
            [entry] { return emf::loader::hold_entry(g_hold, entry, reinterpret_cast<void*>(&held_entry)); });
        if (started == emf::loader::HoldStart::ThreadFailed) {
            char msg[128];
            std::snprintf(msg, sizeof msg, "could not start emf_core's thread (error %lu); running the game without mods",
                          thread_error);
            loader_log(game_dir(), msg);
        } else if (started == emf::loader::HoldStart::HoldFailed) {
            loader_log(game_dir(), "could not hold the server's entry point; mods may load after its world setup");
        }
    }
    return TRUE;
}
