#pragma once
// Nexus Mods single sign-on ("Log in with Nexus"), as Nexus's demo does it (github.com/Nexus-Mods/sso-integration-demo):
//   1. connect to wss://sso.nexusmods.com and send {"id": <uuid>, "token": null, "protocol": 2};
//   2. Nexus answers {"success":true,"data":{"connection_token":"..."},"error":null};
//   3. EMF opens https://www.nexusmods.com/sso?id=<uuid>&application=<slug> in the browser, where the player
//      authorises EMF;
//   4. Nexus sends {"success":true,"data":{"api_key":"..."},"error":null} over the same connection.
// A dropped connection is made again with the same id and the connection token instead of null.
//
// The slug names EMF as a registered application; Nexus staff issue it to a registered app. In the release build
// kSsoSlug is empty, run_sso refuses, and EMF.exe makes no Nexus request at all unless the tester switch is on
// (app/bridge.h nexus_mode; with the switch the player pastes a personal API key, for testing only).
// The socket is an interface, so the protocol is unit-tested with a fake one; winhttp_sso_sockets is the real one
// (WinHTTP's WebSocket API).
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace emf::nexus {

inline constexpr const char* kSsoSlug = "";  // EMF's application slug from Nexus: set it when Nexus issues it
inline constexpr const char* kSsoUrl = "wss://sso.nexusmods.com";

class SsoSocket {
public:
    virtual ~SsoSocket() = default;
    virtual bool connect(std::string& error) = 0;  // blocking; abort() ends it
    virtual bool send_text(const std::string& text) = 0;
    // The next whole text message; nullopt when the connection closed, failed or was aborted.
    virtual std::optional<std::string> receive_text() = 0;
    virtual std::string error() const = 0;  // why the last call failed
    // From any thread, as often as needed: a connect or receive_text that is waiting returns.
    virtual void abort() = 0;
};
using SsoSocketFactory = std::function<std::unique_ptr<SsoSocket>()>;

// {"id":"<id>","token":null,"protocol":2}, or with "token":"<token>" when reconnecting.
std::string sso_hello(const std::string& id, const std::string& token);

struct SsoMessage {
    enum class Kind { ConnectionToken, ApiKey, Error, Other };
    Kind kind = Kind::Other;
    std::string value;  // the token, the key or Nexus's error text
};
SsoMessage parse_sso_message(std::string_view text);

// https://www.nexusmods.com/sso?id=<id>&application=<slug>, both escaped.
std::string sso_browser_url(const std::string& id, const std::string& slug);
// A random UUID (version 4), lower case.
std::string new_sso_id();
bool sso_id_ok(std::string_view id);

struct SsoOptions {
    std::string id;    // new_sso_id()
    std::string slug;  // kSsoSlug
    // Called once, from the calling (worker) thread, when Nexus has accepted the connection.
    std::function<void(const std::string& url)> open_browser;
    const std::atomic<bool>* cancel = nullptr;
    std::chrono::milliseconds timeout = std::chrono::minutes(5);  // for the whole sign-in
    int max_reconnects = 3;
    std::chrono::milliseconds reconnect_wait = std::chrono::seconds(1);
};

struct SsoResult {
    bool ok = false;
    bool cancelled = false;
    std::string api_key;  // when ok
    std::string error;    // why not, in words
};

// The whole sign-in. Synchronous: run it on a worker thread. Cancel and the time limit abort a waiting socket.
SsoResult run_sso(const SsoSocketFactory& make_socket, const SsoOptions& options);

// WinHTTP WebSocket connections to `url` (wss://; ws:// only to 127.0.0.1 or localhost, for a local test server).
SsoSocketFactory winhttp_sso_sockets(std::string user_agent, std::string url = kSsoUrl);

}  // namespace emf::nexus
