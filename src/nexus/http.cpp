#include "nexus/http.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace emf::nexus {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// A WinHTTP error as text ("connect: could not connect (12029)").
std::string error_text(const char* step, DWORD code) {
    const char* what = nullptr;
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: what = "timed out"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: what = "the server name could not be resolved (no internet?)"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: what = "could not connect"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: what = "the connection was reset"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: what = "the secure connection failed (certificate or TLS)"; break;
        default: break;
    }
    std::string text;
    if (what) {
        text = what;
    } else {
        wchar_t* buf = nullptr;
        const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                           FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<LPWSTR>(&buf), 0,
                                       nullptr);
        if (n && buf) {
            text = narrow(buf);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) text.pop_back();
        }
        if (buf) LocalFree(buf);
        if (text.empty()) text = "error";
    }
    return std::string(step) + ": " + text + " (" + std::to_string(code) + ")";
}

bool has_crlf(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

HttpResponse& fail(HttpResponse& out, HttpFailure f, std::string text) {
    out.status = 0;
    out.body.clear();
    out.failure = f;
    out.error = std::move(text);
    return out;
}

// The last WinHTTP error: a timeout is "timed out", the rest a network failure with its text.
HttpResponse& net_fail(HttpResponse& out, const char* step) {
    const DWORD code = GetLastError();
    if (code == ERROR_WINHTTP_TIMEOUT) return fail(out, HttpFailure::TimedOut, "timed out");
    return fail(out, HttpFailure::Network, error_text(step, code));
}

// Cancelled or past the deadline: the failure to report, or None.
HttpFailure stop_now(const HttpRequest& r) {
    if (r.cancel && r.cancel->load()) return HttpFailure::Cancelled;
    if (std::chrono::steady_clock::now() >= r.deadline) return HttpFailure::TimedOut;
    return HttpFailure::None;
}

bool stopped(const HttpRequest& r, HttpResponse& out) {
    const HttpFailure f = stop_now(r);
    if (f == HttpFailure::None) return false;
    fail(out, f, f == HttpFailure::Cancelled ? "cancelled" : "timed out");
    return true;
}

// The reply's raw headers ("" when they cannot be read).
std::string raw_headers(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > (1u << 20)) return {};
    std::wstring buf(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &size,
                             WINHTTP_NO_HEADER_INDEX))
        return {};
    return narrow(buf.c_str());
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool istarts(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (lower(s[i]) != lower(prefix[i])) return false;
    return true;
}

// The address may be used: https, or http to 127.0.0.1 when a test allows it.
bool scheme_ok(const URL_COMPONENTS& parts, const std::wstring& host, bool allow_loopback_http) {
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) return true;
    return allow_loopback_http && parts.nScheme == INTERNET_SCHEME_HTTP && host == L"127.0.0.1";
}

// Closes the request it is armed with (once) when the cancel flag is set, so a WinHTTP call waiting on that request
// returns at once. disarm() says whether it closed it (then the caller must not close it again).
class CancelWatch {
public:
    explicit CancelWatch(const std::atomic<bool>* cancel) : cancel_(cancel) {
        if (cancel_) thread_ = std::thread([this] { run(); });
    }
    ~CancelWatch() {
        {
            std::lock_guard lock(m_);
            done_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    CancelWatch(const CancelWatch&) = delete;
    CancelWatch& operator=(const CancelWatch&) = delete;
    void arm(HINTERNET h) {
        std::lock_guard lock(m_);
        handle_ = h;
        closed_ = false;
    }
    bool disarm() {
        std::lock_guard lock(m_);
        handle_ = nullptr;
        return closed_;
    }

private:
    void run() {
        std::unique_lock lock(m_);
        while (!done_) {
            if (cancel_->load() && handle_ && !closed_) {
                WinHttpCloseHandle(handle_);
                closed_ = true;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(25));
        }
    }
    const std::atomic<bool>* cancel_;
    std::mutex m_;
    std::condition_variable cv_;
    HINTERNET handle_ = nullptr;
    bool closed_ = false, done_ = false;
    std::thread thread_;
};

// https://host[:port] of an https:// address ("" when it is not one).
std::string origin_of(std::string_view url) {
    if (!istarts(url, "https://")) return {};
    const size_t end = url.find_first_of("/?#", 8);
    return std::string(url.substr(0, end));
}

}  // namespace

std::string header_value(const HttpHeaders& headers, std::string_view name) {
    for (const auto& [k, v] : headers) {
        if (k.size() != name.size()) continue;
        bool same = true;
        for (size_t i = 0; i < k.size() && same; ++i) same = lower(k[i]) == lower(name[i]);
        if (same) return v;
    }
    return {};
}

HttpHeaders parse_raw_headers(std::string_view raw) {
    HttpHeaders out;
    size_t from = 0;
    bool first = true;
    while (from < raw.size()) {
        size_t end = raw.find('\n', from);
        if (end == std::string_view::npos) end = raw.size();
        std::string_view line = raw.substr(from, end - from);
        from = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (first) {  // the status line
            first = false;
            if (istarts(line, "HTTP/")) continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        out.emplace_back(std::string(line.substr(0, colon)), std::string(value));
    }
    return out;
}

std::optional<std::string> redirect_target(std::string_view from, std::string_view location, std::string* why) {
    const auto refuse = [&](const char* reason) -> std::optional<std::string> {
        if (why) *why = reason;
        return std::nullopt;
    };
    while (!location.empty() && (location.front() == ' ' || location.front() == '\t')) location.remove_prefix(1);
    while (!location.empty() && (location.back() == ' ' || location.back() == '\t')) location.remove_suffix(1);
    if (location.empty()) return refuse("the server sent a redirect without an address");
    for (unsigned char c : location)
        if (c < 0x21 || c >= 0x7F || c == '\\') return refuse("the server sent a redirect to an address with odd characters");
    std::string target;
    if (istarts(location, "https://")) {
        target = std::string(location);
    } else if (location.size() >= 2 && location[0] == '/' && location[1] == '/') {
        target = "https:" + std::string(location);
    } else {
        // a scheme of its own (http:, ftp:, javascript:...) before any '/', '?' or '#': not https
        const size_t colon = location.find(':');
        const size_t first_sep = location.find_first_of("/?#");
        if (colon != std::string_view::npos && (first_sep == std::string_view::npos || colon < first_sep))
            return refuse("the download was redirected to an address that is not https://");
        const std::string origin = origin_of(from);
        if (origin.empty()) return refuse("the redirect came from an address that is not https://");
        if (location[0] == '/') {
            target = origin + std::string(location);
        } else {  // relative to the current path's folder
            std::string_view path = from.substr(origin.size());
            path = path.substr(0, path.find_first_of("?#"));
            const size_t slash = path.rfind('/');
            target = origin + (slash == std::string_view::npos ? std::string("/") : std::string(path.substr(0, slash + 1))) +
                     std::string(location);
        }
    }
    // the host: present, without a user name ('@' before the path)
    const size_t host_end = target.find_first_of("/?#", 8);
    const std::string authority = target.substr(8, host_end == std::string::npos ? std::string::npos : host_end - 8);
    if (authority.empty() || authority[0] == ':') return refuse("the redirect has no host");
    if (authority.find('@') != std::string::npos) return refuse("the redirect carries a user name");
    return target;
}

HttpResponse http_request(const HttpRequest& r) {
    HttpResponse out;
    const std::wstring url = wide(r.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        return fail(out, HttpFailure::Network, "not a valid address: " + r.url);
    if (!scheme_ok(parts, std::wstring(parts.lpszHostName, parts.dwHostNameLength), r.allow_loopback_http))
        return fail(out, HttpFailure::Network, "only https:// addresses are allowed: " + r.url);
    for (const auto& [name, value] : r.headers)
        if (name.empty() || has_crlf(name) || has_crlf(value))
            return fail(out, HttpFailure::Network, "a header name or value is empty or has a line break");
    if (has_crlf(r.user_agent)) return fail(out, HttpFailure::Network, "the User-Agent has a line break");
    if (stopped(r, out)) return out;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    Handle session(WinHttpOpen(wide(r.user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return net_fail(out, "WinHttpOpen");
    // Each step's timeout, never longer than the time left before the deadline.
    long long t = r.timeout_ms > 0 ? r.timeout_ms : 15000;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          r.deadline - std::chrono::steady_clock::now()).count();
    if (r.deadline != (std::chrono::steady_clock::time_point::max)() && left < t) t = left < 1 ? 1 : left;
    const int ti = static_cast<int>(t);
    WinHttpSetTimeouts(session.h, ti, ti, ti, ti);
    // Ask for gzip/deflate and let WinHTTP unpack it (Windows 8.1 and later; without it the reply is plain).
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
    // Never follow a redirect: the address is the API's, and nothing else is fetched.
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);

    Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (!connection) return net_fail(out, "connect");
    Handle request(WinHttpOpenRequest(connection.h, wide(r.method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return net_fail(out, "request");
    std::wstring headers;
    for (const auto& [name, value] : r.headers) headers += wide(name) + L": " + wide(value) + L"\r\n";
    if (!headers.empty() &&
        !WinHttpAddRequestHeaders(request.h, headers.c_str(), static_cast<DWORD>(headers.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        return net_fail(out, "headers");
    const DWORD body_size = static_cast<DWORD>(r.body.size());
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body_size ? const_cast<char*>(r.body.data()) : WINHTTP_NO_REQUEST_DATA, body_size,
                            body_size, 0))
        return net_fail(out, "send");
    if (stopped(r, out)) return out;
    if (!WinHttpReceiveResponse(request.h, nullptr)) return net_fail(out, "receive");
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return net_fail(out, "status");
    HttpHeaders reply_headers = parse_raw_headers(raw_headers(request.h));
    for (;;) {
        if (stopped(r, out)) return out;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return net_fail(out, "receive");
        if (available == 0) break;
        if (out.body.size() + available > r.max_body)
            return fail(out, HttpFailure::TooLarge, "the reply is longer than " + std::to_string(r.max_body) + " bytes");
        const size_t at = out.body.size();
        out.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, out.body.data() + at, available, &read)) return net_fail(out, "receive");
        out.body.resize(at + read);
    }
    out.status = static_cast<int>(status);
    out.headers = std::move(reply_headers);
    return out;
}

StreamResponse http_stream(const StreamRequest& r, const StreamSink& sink) {
    StreamResponse out;
    const auto fail_with = [&](HttpFailure f, std::string text) {
        out.failure = f;
        out.error = std::move(text);
        return out;
    };
    const auto cancelled = [&] { return r.cancel && r.cancel->load(); };
    const auto net = [&](const char* step) {
        const DWORD code = GetLastError();
        if (cancelled()) return fail_with(HttpFailure::Cancelled, "cancelled");
        if (code == ERROR_WINHTTP_TIMEOUT) return fail_with(HttpFailure::TimedOut, "timed out");
        return fail_with(HttpFailure::Network, error_text(step, code));
    };
    for (const auto& [name, value] : r.headers)
        if (name.empty() || has_crlf(name) || has_crlf(value))
            return fail_with(HttpFailure::Network, "a header name or value is empty or has a line break");
    if (has_crlf(r.user_agent)) return fail_with(HttpFailure::Network, "the User-Agent has a line break");

    Handle session(WinHttpOpen(wide(r.user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return net("WinHttpOpen");
    const int t = r.timeout_ms > 0 ? r.timeout_ms : 30000;
    WinHttpSetTimeouts(session.h, t, t, t, t);
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;  // redirects are followed by hand, to https:// only
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
    std::wstring headers;
    for (const auto& [name, value] : r.headers) headers += wide(name) + L": " + wide(value) + L"\r\n";

    CancelWatch watch(r.cancel);  // a cancel closes the request that waits
    std::string url = r.url;
    for (int hop = 0;; ++hop) {
        if (cancelled()) return fail_with(HttpFailure::Cancelled, "cancelled");
        out.final_url = url;
        const std::wstring wurl = wide(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (wurl.empty() || !WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts))
            return fail_with(HttpFailure::Network, "not a valid address");
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        // only the first address may be a test's loopback http; a redirect is https (redirect_target)
        if (!scheme_ok(parts, host, r.allow_loopback_http && hop == 0))
            return fail_with(HttpFailure::Network, "only https:// addresses are allowed");
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";

        Handle connection(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
        if (!connection) return net("connect");
        Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        if (!request) return net("request");
        // armed for this hop; when the watcher closed the request on a cancel, it is not closed a second time
        struct Armed {
            CancelWatch& watch;
            Handle& request;
            ~Armed() {
                if (watch.disarm()) request.h = nullptr;
            }
        } armed{watch, request};
        watch.arm(request.h);
        if (!headers.empty() &&
            !WinHttpAddRequestHeaders(request.h, headers.c_str(), static_cast<DWORD>(headers.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
            return net("headers");
        if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            return net("send");
        if (!WinHttpReceiveResponse(request.h, nullptr)) return net("receive");
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            return net("status");
        out.status = static_cast<int>(status);
        const HttpHeaders reply = parse_raw_headers(raw_headers(request.h));

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (hop >= r.max_redirects) return fail_with(HttpFailure::Network, "the download was redirected too many times");
            std::string why;
            const auto next = redirect_target(url, header_value(reply, "Location"), &why);
            if (!next) return fail_with(HttpFailure::Network, why);
            url = *next;
            continue;
        }
        if (status != 200) return fail_with(HttpFailure::Network, "the download server answered " + std::to_string(status));

        const std::string length_text = header_value(reply, "Content-Length");
        const bool known = !length_text.empty() && length_text.size() <= 19 &&
                           length_text.find_first_not_of("0123456789") == std::string::npos;
        const uint64_t length = known ? std::stoull(length_text) : 0;
        if (known && length > r.max_bytes)
            return fail_with(HttpFailure::TooLarge, "the file is larger than " + std::to_string(r.max_bytes) + " bytes");
        if (sink.start && !sink.start(length, known)) return fail_with(HttpFailure::Stopped, "stopped");

        std::string buf(64 * 1024, '\0');
        for (;;) {
            if (cancelled()) return fail_with(HttpFailure::Cancelled, "cancelled");
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.h, &available)) return net("receive");
            if (available == 0) break;
            while (available > 0) {
                const DWORD want = available < buf.size() ? available : static_cast<DWORD>(buf.size());
                DWORD read = 0;
                if (!WinHttpReadData(request.h, buf.data(), want, &read)) return net("receive");
                if (read == 0) break;
                available -= read < available ? read : available;
                if (out.bytes + read > r.max_bytes)
                    return fail_with(HttpFailure::TooLarge, "the file is larger than " + std::to_string(r.max_bytes) + " bytes");
                out.bytes += read;
                if (sink.write && !sink.write(buf.data(), read)) return fail_with(HttpFailure::Stopped, "stopped");
                if (cancelled()) return fail_with(HttpFailure::Cancelled, "cancelled");
            }
        }
        return out;
    }
}

}  // namespace emf::nexus
