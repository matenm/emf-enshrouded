// A stand-in for closed-source DLL mods that install themselves as dinput8.dll (Creative Mode, Stat Sliders, Auto
// Loot Plus, XHL-Core). Built for EMF's chain-loading tests only; never shipped. It forwards dinput8's exports like
// those mods do and, like them, starts its work from a thread created in DllMain. That thread appends one line to
// <game>\emf_standin.log: the file name this copy was loaded under, whether the game's main window exists yet (a
// mod loaded before game code runs sees none) and whether EMF's core is in the process.
#include <windows.h>

#include <cstdio>
#include <string>

namespace {
HMODULE g_self = nullptr;

std::wstring module_path(HMODULE m) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

DWORD WINAPI work(LPVOID) {
    const std::wstring exe = module_path(nullptr);
    const std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    const std::wstring self = module_path(g_self);
    const std::wstring name = self.substr(self.find_last_of(L"\\/") + 1);
    const bool window = FindWindowW(nullptr, L"Enshrouded") != nullptr;
    Sleep(5000);  // EMF's core starts on its own thread; look again a little later
    const bool core = GetModuleHandleW(L"emf_core.dll") != nullptr;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\emf_standin.log").c_str(), L"ab") == 0 && f) {
        std::fprintf(f, "standin loaded as %s; game window at load: %s; emf_core present 5 s later: %s; pid %lu\n",
                     narrow(name).c_str(), window ? "yes" : "no", core ? "yes" : "no", GetCurrentProcessId());
        std::fclose(f);
    }
    return 0;
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        if (HANDLE t = CreateThread(nullptr, 0, work, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
