// emf_probe: loaded by enshrouded.exe as dinput8.dll. Only starts a thread in DllMain.
#include <windows.h>

#include <string>

#include "common/log.h"
#include "common/sha256.h"
#include "common/signatures.h"
#include "probe.h"

namespace {
constexpr const char* kVersion = "emf_probe 0.1 (step A)";

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

DWORD WINAPI init_thread(LPVOID) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    emf::log_open(dir + L"\\emf_probe.log");

    const void* base = GetModuleHandleW(nullptr);
    emf::logf("%s", kVersion);
    emf::logf("exe %s base %p", utf8(exe).c_str(), base);
    const std::string sha = emf::sha256_file(exe);
    const int build = emf::find_known_build(sha);
    const bool known = build >= 0;
    if (known)
        emf::logf("sha256 %s (known build %u)", sha.c_str(), emf::known_builds()[static_cast<size_t>(build)].number);
    else
        emf::logf("sha256 %s (UNKNOWN build: signatures only)", sha.c_str());

    auto res = emf::resolve_all(base, build);
    auto defs = emf::all_signatures();
    for (size_t i = 0; i < emf::kSigCount; ++i) {
        emf::logf("sig %-20s %-4s matches=%zu addr=%p", defs[i].name,
                  emf::is_usable(res[i], known) ? "OK" : "FAIL", res[i].count, reinterpret_cast<void*>(res[i].addr));
    }
    if (!emf::required_signatures_ok(res, known)) {
        emf::logf("a required signature is missing: probe stays idle (no hooks installed)");
        return 0;
    }
    emf::probe::start(res, known);
    return 0;
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // FreeLibrary only. At process exit other threads are already gone: touch nothing.
        emf::probe::stop();
        emf::logf("unloaded");
        emf::log_close();
    }
    return TRUE;
}
