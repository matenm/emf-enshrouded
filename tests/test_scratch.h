#pragma once
// Scratch names that two emf_tests runs at the same time on one PC never share: runs from other worktrees, which share
// %TEMP%, and two copies of one build, which also share the build folder. A name carries the process id and a random
// number drawn once per run; the pid alone is unique only among running processes, the random part also keeps a run
// away from what an earlier run with the same pid left behind.
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace test_scratch {

// Folders to remove when it is destroyed. A test that makes a folder in %TEMP% under a name another test of the same
// run may open again (a simulated restart with saved state, two tests sharing a millisecond) cannot remove it when it
// ends; it gives the folder to the run's ExitCleanup (remove_at_exit), which removes it when emf_tests exits.
class ExitCleanup {
public:
    ExitCleanup() = default;
    ExitCleanup(const ExitCleanup&) = delete;
    ExitCleanup& operator=(const ExitCleanup&) = delete;
    ~ExitCleanup() {
        for (const auto& p : paths_) {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    }
    void add(const std::wstring& path) {
        const std::lock_guard<std::mutex> l(m_);
        if (std::find(paths_.begin(), paths_.end(), path) == paths_.end()) paths_.push_back(path);
    }
    bool holds(const std::wstring& path) {
        const std::lock_guard<std::mutex> l(m_);
        return std::find(paths_.begin(), paths_.end(), path) != paths_.end();
    }

private:
    std::mutex m_;
    std::vector<std::wstring> paths_;
};

// The run's ExitCleanup: one for the whole emf_tests process, destroyed when it exits.
inline ExitCleanup& exit_cleanup() {
    static ExitCleanup cleanup;
    return cleanup;
}

// Removes `path` (a folder this run made) when emf_tests exits.
inline void remove_at_exit(const std::wstring& path) { exit_cleanup().add(path); }

// "<pid>_<8 hex digits>", the same for the whole run.
inline const std::wstring& run_tag() {
    static const std::wstring tag = [] {
        std::random_device rd;
        wchar_t buf[40];
        swprintf_s(buf, L"%lu_%08x", GetCurrentProcessId(), static_cast<unsigned>(rd()));
        return std::wstring(buf);
    }();
    return tag;
}

// %TEMP%\<stem>_<run tag><ext>
inline std::wstring temp_path(const std::wstring& stem, const std::wstring& ext = L"") {
    wchar_t base[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, base);
    return std::wstring(base, n > 0 && n <= MAX_PATH ? n : 0) + stem + L"_" + run_tag() + ext;
}

// True while the process runs, or when Windows will not say (then its folder is left alone).
inline bool process_alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() != ERROR_INVALID_PARAMETER;  // no such process
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// <root>\<run tag>, new for this run. Folders that earlier runs left under <root> are removed first, but only once
// their process has ended: a run going on at the same time keeps its folder (it may have DLLs loaded from it).
inline std::wstring run_folder(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    std::vector<std::filesystem::path> old;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) old.push_back(it->path());
    for (const auto& p : old) {
        const std::wstring name = p.filename().wstring();
        if (name == run_tag()) continue;  // this run's own
        const DWORD pid = static_cast<DWORD>(wcstoul(name.c_str(), nullptr, 10));  // 0: not a run's folder
        const bool same_pid_earlier_run = pid == GetCurrentProcessId();      // an earlier run that had this pid
        if (pid != 0 && !same_pid_earlier_run && process_alive(pid)) continue;
        std::error_code ignored;
        std::filesystem::remove_all(p, ignored);
    }
    const std::wstring dir = root + L"\\" + run_tag();
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// For one test: %TEMP% (GetTempPathW reads TMP first) is a new folder of this run's own, so the test can count what the
// code under test leaves in a fixed-name folder there (%TEMP%\emf-install\<n>) without seeing other runs' folders.
// TMP is put back, and the folder removed, at the end of the scope.
class PrivateTemp {
public:
    explicit PrivateTemp(const std::wstring& name) : dir_(temp_path(name)) {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        const DWORD n = GetEnvironmentVariableW(L"TMP", nullptr, 0);
        if (n > 0) {
            std::wstring old(n, L'\0');
            const DWORD got = GetEnvironmentVariableW(L"TMP", old.data(), n);
            had_ = got > 0 && got < n;
            if (had_) old_ = old.substr(0, got);
        }
        SetEnvironmentVariableW(L"TMP", dir_.c_str());
    }
    ~PrivateTemp() {
        SetEnvironmentVariableW(L"TMP", had_ ? old_.c_str() : nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    PrivateTemp(const PrivateTemp&) = delete;
    PrivateTemp& operator=(const PrivateTemp&) = delete;
    const std::wstring& dir() const { return dir_; }

private:
    std::wstring dir_, old_;
    bool had_ = false;
};

// Holds a lock shared by every emf_tests run from the same build folder, for a test that writes a file there which
// other code reads by a fixed name (<exe folder>\emf\config\<mod>.ini for the mods' own DLLs): a second copy of the
// build waits instead of finding the file changed or deleted under it.
class BuildFolderLock {
public:
    BuildFolderLock() {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        for (wchar_t& c : dir) c = static_cast<wchar_t>(towlower(c));
        const std::wstring name = L"Local\\emf-tests-build-" + std::to_wstring(std::hash<std::wstring>{}(dir));
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (mutex_) {
            const DWORD w = WaitForSingleObject(mutex_, 60000);
            owned_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
        }
    }
    ~BuildFolderLock() {
        if (owned_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    BuildFolderLock(const BuildFolderLock&) = delete;
    BuildFolderLock& operator=(const BuildFolderLock&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};

}  // namespace test_scratch
