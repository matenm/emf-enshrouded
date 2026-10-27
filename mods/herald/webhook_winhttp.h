#pragma once
// Server Herald's Discord webhook transport: one HTTPS POST with WinHTTP, from the herald's worker thread only (never the
// game's threads). The URL is a secret: it is never logged, and redirects are refused (a redirect could send the
// messages to another host).
#include <windows.h>
#include <winhttp.h>

#include <string>

#include "herald_logic.h"

namespace herald {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

class WinHttpTransport : public Transport {
public:
    explicit WinHttpTransport(UrlParts url) : url_(std::move(url)) {}
    ~WinHttpTransport() override {
        if (connect_) WinHttpCloseHandle(connect_);
        if (session_) WinHttpCloseHandle(session_);
    }
    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    // The HTTP status, or -(Windows error) when there was no answer.
    int post(const std::string& body) override {
        if (!session_) {
            session_ = WinHttpOpen(L"EMF-ServerHerald/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session_) return -static_cast<int>(GetLastError());
            WinHttpSetTimeouts(session_, 5000, 5000, 5000, 5000);
            // TLS 1.2 or newer only (1.3 where Windows has it)
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // an older Windows without TLS 1.3 in WinHTTP
                if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                    const int err = static_cast<int>(GetLastError());
                    WinHttpCloseHandle(session_);
                    session_ = nullptr;
                    return -err;  // no post without TLS 1.2+
                }
            }
        }
        if (!connect_) {
            connect_ = WinHttpConnect(session_, widen(url_.host).c_str(), static_cast<INTERNET_PORT>(url_.port), 0);
            if (!connect_) return -static_cast<int>(GetLastError());
        }
        HINTERNET req = WinHttpOpenRequest(connect_, L"POST", widen(url_.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req) return -static_cast<int>(GetLastError());
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never)) {  // never post where it leads
            const int err = static_cast<int>(GetLastError());
            WinHttpCloseHandle(req);
            return -err;
        }
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        int status = 0;
        if (WinHttpSendRequest(req, headers, static_cast<DWORD>(-1L), const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, size = sizeof code;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &code, &size, WINHTTP_NO_HEADER_INDEX))
                status = static_cast<int>(code);
            // read and drop the answer, so the connection can be kept
            char sink[512];
            DWORD got = 0;
            while (WinHttpReadData(req, sink, sizeof sink, &got) && got) {
            }
        } else {
            status = -static_cast<int>(GetLastError());
        }
        WinHttpCloseHandle(req);
        if (status <= 0 && connect_) {  // start over next time (a dropped connection)
            WinHttpCloseHandle(connect_);
            connect_ = nullptr;
        }
        return status;
    }

private:
    UrlParts url_;
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
};

}  // namespace herald
