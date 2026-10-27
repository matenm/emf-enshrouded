#include "args.h"

#include <cwchar>
#include <cwctype>

namespace emf::app {

namespace {
bool is_nxm(const std::wstring& s) { return s.size() > 6 && _wcsnicmp(s.c_str(), L"nxm://", 6) == 0; }
bool nxm_token(const std::wstring& s) { return s.size() >= 4 && _wcsnicmp(s.c_str(), L"nxm:", 4) == 0; }
}  // namespace

AppArgs parse_app_args(const std::vector<std::wstring>& args) {
    AppArgs a;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& s = args[i];
        const bool more = i + 1 < args.size();
        if (s == L"--game" && more) a.game = args[++i];
        else if (s == L"--nxm") {
            a.link_seen = true;
            if (more && !args[i + 1].empty()) a.nxm = args[++i];
            else if (more) ++i;  // --nxm "": nothing to download, but a link came all the same
        }
        else if (s == L"--smoke" && more) a.smoke = args[++i];
        else if (s == L"--capture" && more) a.capture = args[++i];
        else if (s == L"--select" && more) a.select = args[++i];
        else if (s == L"--page" && more) a.page = args[++i];
        else if (s == L"--eval" && more) a.eval = args[++i];
        else if (s == L"--devtools") a.devtools = true;
        else if (s == L"--release-nxm") a.release_nxm = true;
        else if ((s == L"--stop-server" || s == L"--stop-server-break") && more) {
            a.stop_server = true;
            a.stop_break = s == L"--stop-server-break";
            const std::wstring& v = args[++i];
            if (!v.empty() && v.size() <= 10 && v.find_first_not_of(L"0123456789") == std::wstring::npos)
                a.stop_pid = std::wcstoul(v.c_str(), nullptr, 10);
        }
        else if (s == L"--shared-ok") a.stop_shared_ok = true;
        else if (s == L"--nexus-fixture" && more) a.nexus_fixture = args[++i];
        else if (s == L"--probe-wait" && more) {
            const long v = std::wcstol(args[++i].c_str(), nullptr, 10);
            a.probe_wait = static_cast<unsigned>(v < 100 ? 100 : v > 10000 ? 10000 : v);
        } else if (nxm_token(s)) {
            a.link_seen = true;
            a.nxm = s;
        }
    }
    return a;
}

void keep_only_link(AppArgs& a) {
    if (!a.link_seen) return;
    AppArgs only;
    only.nxm = a.nxm;
    only.link_seen = true;
    a = only;
}

void finalize_args(AppArgs& a, bool testing) {
    if (testing) return;
    keep_only_link(a);
    a.smoke.clear();
    a.capture.clear();
    a.eval.clear();
    a.devtools = false;
    a.nexus_fixture.clear();
    a.probe_wait = 1000;
}

std::optional<std::wstring> decode_handoff(uintptr_t tag, const void* data, size_t bytes) {
    if (tag != kHandoffTag || !data || bytes == 0 || bytes % sizeof(wchar_t) != 0) return std::nullopt;
    const size_t n = bytes / sizeof(wchar_t);
    if (n > kMaxHandoffChars + 1) return std::nullopt;
    std::wstring s(static_cast<const wchar_t*>(data), n);
    if (!s.empty() && s.back() == L'\0') s.pop_back();
    if (s.size() > kMaxHandoffChars || s.find(L'\0') != std::wstring::npos || !is_nxm(s)) return std::nullopt;
    return s;
}

}  // namespace emf::app
