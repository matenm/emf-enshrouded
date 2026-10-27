// A stand-in for the Enshrouded dedicated server (enshrouded_server.exe), for EMF.exe's server tests only; never
// shipped. Like the real server (build 1273043: its handler at 0x140575fb0) it registers a console control handler
// that turns Ctrl+C, Ctrl+Break and closing its window into a clean shutdown: it logs "[app] Trigger gameflow shutdown,
// exit: <reason>" to logs\enshrouded_server.log, writes stopped.txt with the reason and exits 0. It writes its start
// lines to that log too.
//
// standin.txt next to it changes how it behaves (one word per line):
//   ignore_ctrl        the control handler swallows the signal and the server keeps running (a server EMF cannot stop)
//   ignore_ctrl_c      Ctrl+C does nothing (as for a server started with Ctrl+C switched off); Ctrl+Break still works
//   autosave_ms=<n>    every n ms it appends "world saved (slot 0x0001)" to emf\emf_core.log, as EMF's core does
//   slow_shutdown_ms=<n>  after the shutdown line it takes n ms more to exit (a long final save); a second signal
//                     meanwhile is written to second_signal.txt
//   spawn_twin         it starts a second copy of itself on its own console (enshrouded_server.exe --twin), as a
//                     command window shares its console with the server it runs; the twin writes twin_stopped.txt
// Like the real server, its shutdown writes a last "world saved" line to emf\emf_core.log (the save at shutdown).
// It ends by itself after two minutes, so a broken test leaves nothing running.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::wstring g_dir;
std::atomic<int> g_reason{-1};
bool g_ignore = false;
bool g_ignore_c = false;
unsigned g_slow_ms = 0;

const char* reason_name(int r) {
    switch (r) {
        case CTRL_C_EVENT: return "Ctrl_C";
        case CTRL_BREAK_EVENT: return "Ctrl_Break";
        case CTRL_CLOSE_EVENT: return "Close";
        case CTRL_LOGOFF_EVENT: return "Logoff";
        case CTRL_SHUTDOWN_EVENT: return "Shutdown";
        default: return "Unknown";
    }
}

void append(const std::wstring& rel, const std::string& line) {
    const std::wstring path = g_dir + L"\\" + rel;
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string text = line + "\r\n";
    DWORD put = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
    CloseHandle(h);
}

std::string stamp() {
    static const ULONGLONG start = GetTickCount64();
    const ULONGLONG ms = GetTickCount64() - start;
    char b[48];
    std::snprintf(b, sizeof b, "[I %02llu:%02llu:%02llu,%03llu] ", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
    return b;
}

BOOL WINAPI on_ctrl(DWORD type) {
    if (g_ignore || (g_ignore_c && type == CTRL_C_EVENT)) return TRUE;
    int expected = -1;
    if (!g_reason.compare_exchange_strong(expected, static_cast<int>(type))) {
        HANDLE h = CreateFileW((g_dir + L"\\second_signal.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    // like the real server: the handler waits while the main thread shuts down (Windows ends a closed console's
    // process a few seconds after this returns)
    for (int i = 0; i < 100; ++i) Sleep(50);
    return TRUE;
}

}  // namespace

// The twin: shares the server's console and stops on its signals too.
std::atomic<int> g_twin_reason{-1};
BOOL WINAPI on_twin_ctrl(DWORD type) {
    int expected = -1;
    g_twin_reason.compare_exchange_strong(expected, static_cast<int>(type));
    for (int i = 0; i < 100; ++i) Sleep(50);
    return TRUE;
}

int run_twin() {
    SetConsoleCtrlHandler(on_twin_ctrl, TRUE);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 120000) {
        const int r = g_twin_reason.load();
        if (r >= 0) {
            HANDLE h = CreateFileW((g_dir + L"\\twin_stopped.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD put = 0;
                const std::string text = reason_name(r);
                WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
                CloseHandle(h);
            }
            return 0;
        }
        Sleep(50);
    }
    return 3;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::wstring exe(buf, n);
    g_dir = exe.substr(0, exe.find_last_of(L'\\'));
    if (argc > 1 && std::wstring(argv[1]) == L"--twin") return run_twin();
    unsigned autosave_ms = 0;
    bool twin = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (g_dir + L"\\standin.txt").c_str(), L"rb") == 0 && f) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l == "ignore_ctrl") g_ignore = true;
            if (l == "ignore_ctrl_c") g_ignore_c = true;
            if (l.rfind("autosave_ms=", 0) == 0) autosave_ms = static_cast<unsigned>(std::atoi(l.c_str() + 12));
            if (l.rfind("slow_shutdown_ms=", 0) == 0) g_slow_ms = static_cast<unsigned>(std::atoi(l.c_str() + 17));
            if (l == "spawn_twin") twin = true;
        }
        std::fclose(f);
    }
    CreateDirectoryW((g_dir + L"\\logs").c_str(), nullptr);
    CreateDirectoryW((g_dir + L"\\emf").c_str(), nullptr);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (twin) {  // on this console, as a command window that started the server shares it
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\" --twin";
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    append(L"logs\\enshrouded_server.log", stamp() + "stand-in enshrouded_server (pid " + std::to_string(GetCurrentProcessId()) + ")");
    append(L"logs\\enshrouded_server.log", stamp() + "[game_server] Switching state from LoadEcsScene to Run after 1.00 ms");
    std::printf("stand-in server running\n");
    std::fflush(stdout);

    const ULONGLONG start = GetTickCount64();
    ULONGLONG last_save = start;
    while (GetTickCount64() - start < 120000) {
        const int r = g_reason.load();
        if (r >= 0) {
            append(L"logs\\enshrouded_server.log", stamp() + "[app] Trigger gameflow shutdown, exit: " + reason_name(r));
            if (g_slow_ms) Sleep(g_slow_ms / 2);  // a long final save: its line comes half-way, then the rest
            append(L"emf\\emf_core.log", "[00:00:00.000 t1] world saved (slot 0x0001)");  // the save at shutdown
            if (g_slow_ms) Sleep(g_slow_ms / 2);
            append(L"logs\\enshrouded_server.log", stamp() + "[server] stand-in saved the world and shut down");
            HANDLE h = CreateFileW((g_dir + L"\\stopped.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD put = 0;
                const std::string text = reason_name(r);
                WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
                CloseHandle(h);
            }
            return 0;
        }
        if (autosave_ms && GetTickCount64() - last_save >= autosave_ms) {
            last_save = GetTickCount64();
            append(L"emf\\emf_core.log", "[00:00:00.000 t1] world saved (slot 0x0001)");
        }
        Sleep(50);
    }
    append(L"logs\\enshrouded_server.log", stamp() + "stand-in ran two minutes and ends by itself");
    return 3;
}
