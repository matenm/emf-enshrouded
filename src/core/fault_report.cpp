#include "fault_report.h"
#include "module_name.h"

#include <windows.h>

namespace emf::core {
namespace {
std::function<void(const std::string&)> g_reporter;
SRWLOCK g_lock = SRWLOCK_INIT;
}  // namespace

std::string module_name_of(const void* addr) {
    HMODULE m = nullptr;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     static_cast<LPCWSTR>(addr), &m) || !m)
        return "unknown module";
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "unknown module";
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    char out[MAX_PATH * 3];
    const int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof(out), nullptr, nullptr);
    return len > 1 ? std::string(out, len - 1) : "unknown module";
}

std::string module_stem_of(const void* addr) {
    std::string s = module_name_of(addr);  // UTF-8, so a mod named with any letters keeps its name
    if (s == "unknown module") return "unknown";
    const size_t dot = s.find_last_of('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

void set_fault_reporter(std::function<void(const std::string&)> reporter) {
    AcquireSRWLockExclusive(&g_lock);
    g_reporter = std::move(reporter);
    ReleaseSRWLockExclusive(&g_lock);
}

void report_fault(const std::string& what, const void* callback) {
    AcquireSRWLockShared(&g_lock);
    auto reporter = g_reporter;
    ReleaseSRWLockShared(&g_lock);
    if (reporter) reporter(what + " in " + module_name_of(callback) + " faulted and was disabled");
}

}  // namespace emf::core
