// A stand-in for Windows' tar.exe, for EMF.exe's archive tests only; never shipped. It plays an archive whose header
// gives no unpacked size (a RAR5 file can do that): its listing says one file of 1 byte, and unpacking writes that
// file slowly (1 MB every 20 ms, up to 100 MB) into the -C folder, keeping it open the whole time, as libarchive does.
// After each MB it writes how many MB it wrote to the file named by EMF_STANDIN_TAR_LOG (when set), so a test can tell
// how far it got before EMF ended it. It ends by itself after a minute, so a broken test leaves nothing running.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {
// Three digits written over the last three, never truncated first: ended at any moment, the file holds a whole number.
void note_progress(const std::wstring& log, int mb) {
    if (log.empty()) return;
    HANDLE h = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char text[8];
    std::snprintf(text, sizeof text, "%03d", mb);
    DWORD wrote = 0;
    WriteFile(h, text, 3, &wrote, nullptr);
    CloseHandle(h);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    if (!args.empty() && args[0] == L"-tvf") {
        std::printf("-rw-rw-r--  0 0      0           1 Oct 26 16:40 big.bin\n");
        return 0;
    }
    std::wstring dest;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == L"-C") dest = args[i + 1];
    if (dest.empty()) return 2;
    wchar_t log_buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"EMF_STANDIN_TAR_LOG", log_buf, MAX_PATH);
    const std::wstring log = n && n < MAX_PATH ? std::wstring(log_buf, n) : std::wstring();
    HANDLE f = CreateFileW((dest + L"\\big.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 3;
    const std::vector<char> chunk(1 << 20, 0);
    const ULONGLONG start = GetTickCount64();
    for (int mb = 1; mb <= 100 && GetTickCount64() - start < 60000; ++mb) {
        DWORD wrote = 0;
        if (!WriteFile(f, chunk.data(), static_cast<DWORD>(chunk.size()), &wrote, nullptr)) break;
        note_progress(log, mb);
        Sleep(20);
    }
    CloseHandle(f);
    return 0;
}
