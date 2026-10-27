#include "nexus/sso.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cctype>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/json.h"
#include "nexus/api.h"
#include "nexus/v1.h"

namespace emf::nexus {

namespace {
using Clock = std::chrono::steady_clock;
using core::json::Value;
using VT = Value::Type;

std::string str(const Value* v) { return v && v->is(VT::String) ? v->s : std::string(); }

bool token_ok(const std::string& t) {
    if (t.empty() || t.size() > 512) return false;
    for (unsigned char c : t)
        if (c <= 0x20 || c >= 0x7F) return false;
    return true;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool istarts(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
    return true;
}

// ---- the real socket: WinHTTP's WebSocket API ----
class WinHttpSsoSocket final : public SsoSocket {
public:
    WinHttpSsoSocket(std::string user_agent, std::string url) : ua_(std::move(user_agent)), url_(std::move(url)) {}
    ~WinHttpSsoSocket() override { close_all(); }

    bool connect(std::string& error) override {
        std::string u = url_;
        bool secure = true;
        if (istarts(u, "wss://")) {
            u = "https://" + u.substr(6);
        } else if (istarts(u, "ws://")) {
            secure = false;
            u = "http://" + u.substr(5);
        } else {
            error = "not a WebSocket address";
            return false;
        }
        const std::wstring wurl = wide(u);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof parts;
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts)) {
            error = "not a valid address";
            return false;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        if (!secure && host != L"127.0.0.1" && host != L"localhost") {
            error = "ws:// is only allowed to this computer";
            return false;
        }
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";

        // an abort() from the watcher can come between any two steps: each new handle is checked against it, so a
        // handle made after abort() closed the others is closed at once
        const auto aborted = [&] {
            if (!aborted_) return false;
            close_all();
            error = "aborted";
            return true;
        };
        session_ = WinHttpOpen(wide(ua_).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (aborted()) return false;
        if (!session_.load()) return fail(error, "WinHttpOpen");
        // resolve, connect and send time out; receiving waits (the sign-in waits for the player), and run_sso's cancel
        // and time limit abort it
        WinHttpSetTimeouts(session_.load(), 15000, 15000, 15000, 0);
        connection_ = WinHttpConnect(session_.load(), host.c_str(), parts.nPort, 0);
        if (aborted()) return false;
        if (!connection_.load()) return fail(error, "connect");
        request_ = WinHttpOpenRequest(connection_.load(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (aborted()) return false;
        if (!request_.load()) return fail(error, "request");
        if (!WinHttpSetOption(request_.load(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return fail(error, "upgrade");
        if (!WinHttpSendRequest(request_.load(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) return fail(error, "send");
        if (!WinHttpReceiveResponse(request_.load(), nullptr)) return fail(error, "receive");
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request_.load(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 101) {
            error = "the login service answered " + std::to_string(status) + " instead of opening a WebSocket";
            return false;
        }
        ws_ = WinHttpWebSocketCompleteUpgrade(request_.load(), 0);
        if (aborted()) return false;
        if (!ws_.load()) return fail(error, "upgrade");
        if (HINTERNET r = request_.exchange(nullptr)) WinHttpCloseHandle(r);  // the WebSocket handle carries on
        return !aborted();
    }

    bool send_text(const std::string& text) override {
        HINTERNET ws = ws_.load();
        if (!ws) return false;
        const DWORD err = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                               const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
        if (err != NO_ERROR) error_ = "send failed (" + std::to_string(err) + ")";
        return err == NO_ERROR;
    }

    std::optional<std::string> receive_text() override {
        std::string msg;
        bool binary = false;
        for (;;) {
            HINTERNET ws = ws_.load();
            if (!ws) {
                error_ = "aborted";
                return std::nullopt;
            }
            char buf[8192];
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD err = WinHttpWebSocketReceive(ws, buf, sizeof buf, &read, &type);
            if (err != NO_ERROR) {
                error_ = aborted_ ? "aborted" : "the connection failed (" + std::to_string(err) + ")";
                return std::nullopt;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                error_ = "Nexus closed the connection";
                return std::nullopt;
            }
            msg.append(buf, read);
            if (msg.size() > 64 * 1024) {
                error_ = "Nexus sent a message too long for a sign-in";
                return std::nullopt;
            }
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) binary = true;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && !binary) return msg;
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                msg.clear();  // a binary message: not part of the protocol
                binary = false;
            }
        }
    }

    std::string error() const override { return error_; }

    void abort() override {
        aborted_ = true;
        close_all();
    }

private:
    bool fail(std::string& error, const char* step) {
        const DWORD code = GetLastError();
        if (aborted_) error = "aborted";
        else if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED) error = "the server name could not be resolved (no internet?)";
        else if (code == ERROR_WINHTTP_CANNOT_CONNECT) error = "could not connect";
        else if (code == ERROR_WINHTTP_TIMEOUT) error = "timed out";
        else if (code == ERROR_WINHTTP_SECURE_FAILURE) error = "the secure connection failed (certificate or TLS)";
        else error = std::string(step) + " failed (" + std::to_string(code) + ")";
        return false;
    }
    // Each handle is closed once, by whoever takes it first (the worker or abort() from another thread). Closing a
    // handle makes a call that waits on it return.
    void close_all() {
        for (std::atomic<HINTERNET>* h : {&ws_, &request_, &connection_, &session_})
            if (HINTERNET x = h->exchange(nullptr)) WinHttpCloseHandle(x);
    }

    std::string ua_, url_, error_;
    std::atomic<HINTERNET> session_{nullptr}, connection_{nullptr}, request_{nullptr}, ws_{nullptr};
    std::atomic<bool> aborted_{false};
};

}  // namespace

std::string sso_hello(const std::string& id, const std::string& token) {
    return "{\"id\":" + json_quote(id) + ",\"token\":" + (token.empty() ? std::string("null") : json_quote(token)) +
           ",\"protocol\":2}";
}

SsoMessage parse_sso_message(std::string_view text) {
    SsoMessage m;
    const auto doc = core::json::parse(text, core::json::ParseOptions{true});  // Nexus's text
    if (!doc.value || !doc.value->is(VT::Object)) return m;
    const Value& v = *doc.value;
    if (const Value* ok = v.get("success"); ok && ok->is(VT::Bool) && !ok->b) {
        m.kind = SsoMessage::Kind::Error;
        m.value = str(v.get("error"));
        if (m.value.size() > 300) m.value.resize(300);
        if (m.value.empty()) m.value = "Nexus refused the sign-in";
        return m;
    }
    const Value* data = v.get("data");
    if (!data || !data->is(VT::Object)) return m;
    if (const Value* t = data->get("connection_token"); t && t->is(VT::String)) {
        if (!token_ok(t->s)) {
            m.kind = SsoMessage::Kind::Error;
            m.value = "Nexus sent a connection token EMF cannot use";
            return m;
        }
        m.kind = SsoMessage::Kind::ConnectionToken;
        m.value = t->s;
        return m;
    }
    if (const Value* k = data->get("api_key"); k && k->is(VT::String)) {
        if (!api_key_text_ok(k->s)) {
            m.kind = SsoMessage::Kind::Error;
            m.value = "Nexus sent an API key EMF cannot use";
            return m;
        }
        m.kind = SsoMessage::Kind::ApiKey;
        m.value = k->s;
    }
    return m;
}

std::string sso_browser_url(const std::string& id, const std::string& slug) {
    return "https://www.nexusmods.com/sso?id=" + url_query_escape(id) + "&application=" + url_query_escape(slug);
}

std::string new_sso_id() {
    unsigned char b[16] = {};
    if (BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // never expected; fall back to the clock and the process id rather than a fixed id
        const auto t = static_cast<unsigned long long>(Clock::now().time_since_epoch().count());
        for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(t >> (8 * i));
        const DWORD pid = GetCurrentProcessId();
        for (int i = 0; i < 4; ++i) b[8 + i] = static_cast<unsigned char>(pid >> (8 * i));
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // RFC 4122 variant
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[b[i] >> 4];
        s += hex[b[i] & 15];
    }
    return s;
}

bool sso_id_ok(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

SsoResult run_sso(const SsoSocketFactory& make_socket, const SsoOptions& o) {
    SsoResult res;
    if (o.slug.empty()) {
        res.error = "This build of EMF has no Nexus application slug, so it cannot log in with Nexus: use a personal API key.";
        return res;
    }
    if (!sso_id_ok(o.id)) {
        res.error = "The sign-in id is not valid.";
        return res;
    }
    const auto deadline = Clock::now() + o.timeout;
    const auto stop_reason = [&]() -> int {
        if (o.cancel && o.cancel->load()) return 1;
        if (Clock::now() >= deadline) return 2;
        return 0;
    };

    // the watcher aborts the socket in use when the player cancels or the time is up, on every tick (abort() may be
    // repeated, and a socket still connecting may make a new handle after the first one)
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    SsoSocket* current = nullptr;
    std::thread watcher([&] {
        std::unique_lock lock(m);
        while (!finished) {
            if (stop_reason() && current) current->abort();
            cv.wait_for(lock, std::chrono::milliseconds(20));
        }
    });

    std::string token, last_error;
    bool opened = false;
    int reconnects = 0;
    for (;;) {
        if (stop_reason()) break;
        std::unique_ptr<SsoSocket> sock = make_socket ? make_socket() : nullptr;
        if (!sock) {
            res.error = "EMF could not open a connection to Nexus's login service.";
            break;
        }
        {
            std::lock_guard lock(m);
            current = sock.get();
        }
        bool done = false;
        std::string err;
        if (!sock->connect(err)) {
            last_error = err;
            if (!stop_reason() && !opened && reconnects == 0) {
                res.error = "Could not reach Nexus's login service: " + err + ".";
                done = true;
            }
        } else if (sock->send_text(sso_hello(o.id, token))) {
            for (;;) {
                const auto msg = sock->receive_text();
                if (!msg) {
                    last_error = sock->error();
                    break;
                }
                const SsoMessage pm = parse_sso_message(*msg);
                if (pm.kind == SsoMessage::Kind::ConnectionToken) {
                    token = pm.value;
                    if (!opened) {
                        opened = true;
                        if (o.open_browser) o.open_browser(sso_browser_url(o.id, o.slug));
                    }
                } else if (pm.kind == SsoMessage::Kind::ApiKey) {
                    res.ok = true;
                    res.api_key = pm.value;
                    done = true;
                    break;
                } else if (pm.kind == SsoMessage::Kind::Error) {
                    res.error = "Nexus's login service says: " + pm.value + ".";
                    done = true;
                    break;
                }
            }
        } else {
            last_error = sock->error();
        }
        {
            std::lock_guard lock(m);
            current = nullptr;
        }
        sock.reset();
        if (done || stop_reason()) break;
        if (++reconnects > o.max_reconnects) {
            res.error = "The connection to Nexus's login service dropped" + (last_error.empty() ? std::string() : " (" + last_error + ")") +
                        ". Try again.";
            break;
        }
        for (auto until = Clock::now() + o.reconnect_wait; Clock::now() < until && !stop_reason();)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!res.ok && res.error.empty()) {
        if (stop_reason() == 1) {
            res.cancelled = true;
            res.error = "cancelled";
        } else {
            res.error = "Logging in with Nexus timed out. Try again.";
        }
    }
    {
        std::lock_guard lock(m);
        finished = true;
    }
    cv.notify_all();
    watcher.join();
    return res;
}

SsoSocketFactory winhttp_sso_sockets(std::string user_agent, std::string url) {
    return [user_agent, url]() -> std::unique_ptr<SsoSocket> { return std::make_unique<WinHttpSsoSocket>(user_agent, url); };
}

}  // namespace emf::nexus
