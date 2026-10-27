#pragma once
// Archives for EMF.exe's install tests, made in the test: zip files with Windows' own tar.exe (bsdtar), and ustar
// files written byte by byte (tar.exe refuses to make the unsafe entries the tests need: "..", links).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app_test_files.h"

namespace app_test {

// Runs `exe args` (no window, waits up to 60 s) and returns its exit code (-1 when it did not start).
inline int run_quiet(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd = L"") {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

inline std::wstring tar_exe() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return std::wstring(sys) + L"\\tar.exe";
}

// A zip of everything in `src` (relative paths as they are there), made by tar.exe.
inline bool make_zip(const std::wstring& src, const std::wstring& zip) {
    std::wstring names;
    for (const auto& e : std::filesystem::directory_iterator(src)) names += L" \"" + e.path().filename().wstring() + L"\"";
    return run_quiet(tar_exe(), L"-a -c -f \"" + zip + L"\"" + names, src) == 0 && std::filesystem::exists(zip);
}

// One entry of a hand-made ustar archive: type '0' file, '5' folder, '1' hard link, '2' symbolic link.
struct TarEntry {
    std::string name;
    char type = '0';
    std::string data;  // a file's bytes
    std::string link;  // a link's target
};

inline std::string make_ustar(const std::vector<TarEntry>& entries) {
    std::string out;
    for (const auto& e : entries) {
        char h[512] = {};
        std::memcpy(h, e.name.data(), (std::min)(e.name.size(), size_t(99)));
        std::snprintf(h + 100, 8, "%07o", e.type == '5' ? 0755 : 0644);
        std::snprintf(h + 108, 8, "%07o", 0);
        std::snprintf(h + 116, 8, "%07o", 0);
        std::snprintf(h + 124, 12, "%011o", static_cast<unsigned>(e.type == '0' ? e.data.size() : 0));
        std::snprintf(h + 136, 12, "%011o", 1700000000u);
        h[156] = e.type;
        std::memcpy(h + 157, e.link.data(), (std::min)(e.link.size(), size_t(99)));
        std::memcpy(h + 257, "ustar", 6);
        std::memcpy(h + 263, "00", 2);
        std::memset(h + 148, ' ', 8);
        unsigned sum = 0;
        for (unsigned char c : h) sum += c;
        std::snprintf(h + 148, 8, "%06o", sum);
        h[155] = ' ';
        out.append(h, 512);
        if (e.type == '0') {
            out += e.data;
            out.append((512 - e.data.size() % 512) % 512, '\0');
        }
    }
    out.append(1024, '\0');
    return out;
}

}  // namespace app_test
