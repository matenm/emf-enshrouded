#pragma once
// Holds the game's main thread at the exe's entry point until EMF has started. Used on the dedicated server only: it
// sets up its world within a second of starting, and EMF must hook that setup and load the mods first.
//
// hold_entry() replaces the first 12 bytes of the entry point with "mov rax, <stub>; jmp rax" (rax is free at an
// entry point; rcx, the argument, is kept). The stub runs on the main thread, waits, calls release_entry() to put the
// original bytes back and then calls the entry point. An entry point runs once, so no trampoline is needed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace emf::loader {

constexpr size_t kEntryPatchSize = 12;

struct EntryHold {
    uint8_t* entry = nullptr;
    uint8_t saved[kEntryPatchSize] = {};
    bool held = false;
};

inline bool write_code(uint8_t* at, const uint8_t* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, n);
    DWORD ignored = 0;
    VirtualProtect(at, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

inline bool hold_entry(EntryHold& h, uint8_t* entry, void* stub) {
    if (h.held || !entry || !stub) return false;
    uint8_t patch[kEntryPatchSize] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};  // mov rax, imm64; jmp rax
    const uint64_t target = reinterpret_cast<uint64_t>(stub);
    std::memcpy(patch + 2, &target, sizeof target);
    std::memcpy(h.saved, entry, kEntryPatchSize);
    if (!write_code(entry, patch, kEntryPatchSize)) return false;
    h.entry = entry;
    h.held = true;
    return true;
}

inline bool release_entry(EntryHold& h) {
    if (!h.held) return false;
    if (!write_code(h.entry, h.saved, kEntryPatchSize)) return false;
    h.held = false;
    return true;
}

// True for MSVC's standard exe start-up (mainCRTStartup / WinMainCRTStartup): "sub rsp,28h; call rel32; add rsp,28h;
// jmp rel32", 18 bytes. Only that is held: the 12-byte patch then stays inside it (the server of 1273043 has it).
inline bool entry_is_msvc_startup(const uint8_t* e) {
    static const uint8_t head[5] = {0x48, 0x83, 0xEC, 0x28, 0xE8};
    static const uint8_t mid[5] = {0x48, 0x83, 0xC4, 0x28, 0xE9};
    return e && std::memcmp(e, head, 5) == 0 && std::memcmp(e + 9, mid, 5) == 0;
}

// Where a jump at the entry point leads: "jmp rel32", "jmp [rip+disp32]" (the address read with `read8(address, &out)`,
// which must read memory safely), "mov rax, imm64; jmp rax" (EMF's own hold) or "mov r11, imm64; jmp r11"; 0 for
// anything else. Another tool that holds the start (ShroudForge's winmm.dll) leaves such a jump.
template <class Read8>
uint64_t entry_jump_target(const uint8_t* e, Read8 read8) {
    if (!e) return 0;
    const uint64_t at = reinterpret_cast<uint64_t>(e);
    int32_t rel = 0;
    uint64_t abs = 0;
    if (e[0] == 0xE9) {
        std::memcpy(&rel, e + 1, 4);
        return at + 5 + static_cast<int64_t>(rel);
    }
    if (e[0] == 0xFF && e[1] == 0x25) {
        std::memcpy(&rel, e + 2, 4);
        return read8(at + 6 + static_cast<int64_t>(rel), &abs) ? abs : 0;
    }
    if (e[0] == 0x48 && e[1] == 0xB8 && e[10] == 0xFF && e[11] == 0xE0) return std::memcpy(&abs, e + 2, 8), abs;
    if (e[0] == 0x49 && e[1] == 0xBB && e[10] == 0x41 && e[11] == 0xFF && e[12] == 0xE3) return std::memcpy(&abs, e + 2, 8), abs;
    return 0;
}

// The loader's line when the server's entry point is not MSVC's start-up code, with why: another tool's jump (into
// `module` when it is known, else to `target`), or else its first bytes.
inline std::string entry_not_held_message(const uint8_t* e, uint64_t target, const std::string& module) {
    std::string s = "the server's entry point is not the usual start-up code: ";
    if (target) {
        char where[32];
        std::snprintf(where, sizeof where, "0x%llx", static_cast<unsigned long long>(target));
        s += "another tool patched it: it jumps " + (module.empty() ? std::string("to ") + where : "into " + module) +
             " (ShroudForge's winmm.dll holds the start this way)";
    } else {
        s += "its first bytes are";
        for (int i = 0; e && i < 8; ++i) {
            char b[4];
            std::snprintf(b, sizeof b, " %02x", e[i]);
            s += b;
        }
        s += " (a game build EMF does not know, or another tool's code)";
    }
    return s + "; not held, so mods may load after its world setup";
}

inline uint8_t* exe_entry_point() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.AddressOfEntryPoint;
}

inline bool is_server_exe(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    return _wcsicmp(name.c_str(), L"enshrouded_server.exe") == 0;
}

// Starts EMF's thread first and holds the entry point only once that thread exists: a hold whose thread never started
// would keep the server waiting the whole time for nothing. `start_thread` and `hold` return false on failure.
enum class HoldStart { NotWanted, Held, ThreadFailed, HoldFailed };
template <class StartThread, class Hold>
HoldStart start_core(bool want_hold, StartThread start_thread, Hold hold) {
    if (!start_thread()) return HoldStart::ThreadFailed;
    if (!want_hold) return HoldStart::NotWanted;
    return hold() ? HoldStart::Held : HoldStart::HoldFailed;
}

// How EMF's start-up thread ended: still running, emf_core_start returned, or it never ran.
enum class CoreOutcome : long { Running = 0, Started, Missing, NoExport };

// The loader's line once the held server goes on after `ms` milliseconds; `ended`: the thread ended in time.
inline std::string hold_message(bool ended, CoreOutcome outcome, unsigned long long ms) {
    std::string s = "held the server's start " + std::to_string(ms) + " ms";
    if (!ended) return s + "; emf_core is still starting, the server goes on";
    switch (outcome) {
        case CoreOutcome::Started:
            return s + " until emf_core started";
        case CoreOutcome::Missing:
            return s + "; emf_core.dll was not found or failed to load: the server runs without mods";
        case CoreOutcome::NoExport:
            return s + "; emf_core.dll has no emf_core_start export: the server runs without mods";
        default:
            return s + " until EMF's start-up thread ended";
    }
}

}  // namespace emf::loader
