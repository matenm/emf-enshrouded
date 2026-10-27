#pragma once
// Temp folders for EMF.exe's tests: a fake game folder that is removed again at the end of the test.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace app_test {

struct TempGame {
    std::wstring dir;
    explicit TempGame(const wchar_t* name, bool with_exe = true) {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        dir = std::wstring(base) + L"emf_app_test_" + name + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
              std::to_wstring(GetTickCount64());
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        if (with_exe) write(L"enshrouded.exe", "fake exe");
    }
    ~TempGame() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::wstring at(const std::wstring& rel) const { return dir + L"\\" + rel; }
    void write(const std::wstring& rel, const std::string& bytes) const {
        const std::filesystem::path p(at(rel));
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        f << bytes;
    }
    std::string read(const std::wstring& rel) const {
        std::ifstream f(std::filesystem::path(at(rel)), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }
    bool has(const std::wstring& rel) const { return std::filesystem::exists(std::filesystem::path(at(rel))); }
    void mkdir(const std::wstring& rel) const { std::filesystem::create_directories(at(rel)); }
};

}  // namespace app_test
