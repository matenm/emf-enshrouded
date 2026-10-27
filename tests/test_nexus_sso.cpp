// Nexus single sign-on, as Nexus's demo does it, against a fake socket (EMF has no application slug yet, so the real
// service is never contacted): the hello, the connection token, the browser page, the key, reconnecting with the
// token, Nexus's errors, cancel and the time limit.
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "nexus/sso.h"

using namespace emf::nexus;
using namespace std::chrono_literals;

namespace {

const std::string kToken = R"({"success":true,"data":{"connection_token":"X5SjO3P4i8tiBgMdYVTCh3z57ZnWVK5z"},"error":null})";
const std::string kKey = R"({"success":true,"data":{"api_key":"VX2PMnc4T5Q3dUFmjE45WyRyZ3VkKzIvYVJiMUdick5XQU9QWHdUbFo4"},"error":null})";

// What one connection does: the messages it receives in order (nullopt: the connection drops there), then either it
// closes or it waits until aborted.
struct Script {
    bool connect_ok = true;
    std::string connect_error = "could not connect";
    std::vector<std::optional<std::string>> incoming;
    bool wait_at_end = false;
};

struct Shared {
    std::vector<std::string> sent;
    std::atomic<int> aborts{0};
    std::atomic<int> connections{0};
};

class FakeSocket : public SsoSocket {
public:
    FakeSocket(Script s, Shared& shared) : s_(std::move(s)), shared_(shared) {}
    bool connect(std::string& error) override {
        ++shared_.connections;
        if (!s_.connect_ok) error = s_.connect_error;
        return s_.connect_ok;
    }
    bool send_text(const std::string& text) override {
        shared_.sent.push_back(text);
        return true;
    }
    std::optional<std::string> receive_text() override {
        if (at_ < s_.incoming.size()) return s_.incoming[at_++];
        if (!s_.wait_at_end) return std::nullopt;
        std::unique_lock lock(m_);
        cv_.wait(lock, [&] { return aborted_; });
        return std::nullopt;
    }
    std::string error() const override { return "closed"; }
    void abort() override {
        {
            std::lock_guard lock(m_);
            aborted_ = true;
        }
        ++shared_.aborts;
        cv_.notify_all();
    }

private:
    Script s_;
    Shared& shared_;
    size_t at_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
    bool aborted_ = false;
};

SsoSocketFactory factory(std::vector<Script> scripts, Shared& shared) {
    auto list = std::make_shared<std::vector<Script>>(std::move(scripts));
    auto next = std::make_shared<size_t>(0);
    return [list, next, &shared]() -> std::unique_ptr<SsoSocket> {
        Script s;
        if (*next < list->size()) s = (*list)[(*next)++];
        else s.connect_ok = false;
        return std::make_unique<FakeSocket>(s, shared);
    };
}

SsoOptions options(std::vector<std::string>* opened) {
    SsoOptions o;
    o.id = "0f8fad5b-d9cb-469f-a165-70867728950e";
    o.slug = "emf";
    o.open_browser = [opened](const std::string& url) { opened->push_back(url); };
    o.reconnect_wait = 1ms;
    return o;
}

}  // namespace

TEST_CASE("sso: the hello names the id, the token (null at first) and protocol 2") {
    auto doc = emf::core::json::parse(sso_hello("abc", ""));
    REQUIRE(doc.value);
    CHECK(doc.value->get("id")->s == "abc");
    CHECK(doc.value->get("token")->is(emf::core::json::Value::Type::Null));
    CHECK(doc.value->get("protocol")->n == 2);
    doc = emf::core::json::parse(sso_hello("abc", "tok\"en"));
    REQUIRE(doc.value);
    CHECK(doc.value->get("token")->s == "tok\"en");
}

TEST_CASE("sso: Nexus's messages are read as a connection token, a key, an error or nothing EMF knows") {
    SsoMessage m = parse_sso_message(kToken);
    CHECK(m.kind == SsoMessage::Kind::ConnectionToken);
    CHECK(m.value == "X5SjO3P4i8tiBgMdYVTCh3z57ZnWVK5z");
    m = parse_sso_message(kKey);
    CHECK(m.kind == SsoMessage::Kind::ApiKey);
    CHECK(m.value.rfind("VX2PMnc4", 0) == 0);
    m = parse_sso_message(R"({"success":false,"data":null,"error":"Invalid application"})");
    CHECK(m.kind == SsoMessage::Kind::Error);
    CHECK(m.value == "Invalid application");
    CHECK(parse_sso_message(R"({"success":false})").kind == SsoMessage::Kind::Error);
    CHECK(parse_sso_message("not json").kind == SsoMessage::Kind::Other);
    CHECK(parse_sso_message(R"({"success":true,"data":{}})").kind == SsoMessage::Kind::Other);
    CHECK(parse_sso_message(R"({"success":true,"data":{"api_key":"has space"}})").kind == SsoMessage::Kind::Error);
}

TEST_CASE("sso: the browser page names the id and the application; ids are random UUIDs") {
    CHECK(sso_browser_url("0f8fad5b-d9cb-469f-a165-70867728950e", "emf") ==
          "https://www.nexusmods.com/sso?id=0f8fad5b-d9cb-469f-a165-70867728950e&application=emf");
    CHECK(sso_browser_url("id", "a b&c") == "https://www.nexusmods.com/sso?id=id&application=a%20b%26c");
    std::set<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        const std::string id = new_sso_id();
        CHECK(sso_id_ok(id));
        CHECK(id.size() == 36);
        CHECK(id[14] == '4');  // version 4
        CHECK(std::string("89ab").find(id[19]) != std::string::npos);
        ids.insert(id);
    }
    CHECK(ids.size() == 20);
    CHECK_FALSE(sso_id_ok("not-a-uuid"));
}

TEST_CASE("sso: hello, token, the browser opens once, then the key arrives") {
    Shared shared;
    std::vector<std::string> opened;
    const SsoResult r = run_sso(factory({Script{true, "", {kToken, kKey}}}, shared), options(&opened));
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.api_key == "VX2PMnc4T5Q3dUFmjE45WyRyZ3VkKzIvYVJiMUdick5XQU9QWHdUbFo4");
    REQUIRE(shared.sent.size() == 1);
    CHECK(shared.sent[0] == sso_hello("0f8fad5b-d9cb-469f-a165-70867728950e", ""));
    REQUIRE(opened.size() == 1);
    CHECK(opened[0] == "https://www.nexusmods.com/sso?id=0f8fad5b-d9cb-469f-a165-70867728950e&application=emf");
}

TEST_CASE("sso: without a slug nothing connects") {
    Shared shared;
    std::vector<std::string> opened;
    SsoOptions o = options(&opened);
    o.slug.clear();
    const SsoResult r = run_sso(factory({Script{true, "", {kToken, kKey}}}, shared), o);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("personal API key") != std::string::npos);
    CHECK(shared.connections == 0);
    CHECK(opened.empty());
    CHECK(std::string(kSsoSlug).empty());  // the release build has no application slug
}

TEST_CASE("sso: a dropped connection is made again with the same id and the connection token") {
    Shared shared;
    std::vector<std::string> opened;
    const SsoResult r = run_sso(factory({Script{true, "", {kToken, std::nullopt}}, Script{true, "", {kKey}}}, shared),
                                options(&opened));
    REQUIRE_MESSAGE(r.ok, r.error);
    REQUIRE(shared.sent.size() == 2);
    CHECK(shared.sent[1] == sso_hello("0f8fad5b-d9cb-469f-a165-70867728950e", "X5SjO3P4i8tiBgMdYVTCh3z57ZnWVK5z"));
    CHECK(opened.size() == 1);  // the browser page is not opened twice
}

TEST_CASE("sso: Nexus's error, a failed connection and too many drops end it with the reason") {
    Shared shared;
    std::vector<std::string> opened;
    SsoResult r = run_sso(factory({Script{true, "", {R"({"success":false,"data":null,"error":"Unknown application"})"}}}, shared),
                          options(&opened));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("Unknown application") != std::string::npos);

    Shared s2;
    r = run_sso(factory({Script{false, "the server name could not be resolved", {}}}, s2), options(&opened));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("could not be resolved") != std::string::npos);

    Shared s3;
    std::vector<Script> drops(6, Script{true, "", {std::nullopt}});
    drops[0].incoming = {kToken, std::nullopt};
    SsoOptions o = options(&opened);
    o.max_reconnects = 2;
    r = run_sso(factory(drops, s3), o);
    CHECK_FALSE(r.ok);
    CHECK(s3.connections == 3);
    CHECK(r.error.find("dropped") != std::string::npos);
}

TEST_CASE("sso: cancel and the time limit stop a wait for the player") {
    Shared shared;
    std::vector<std::string> opened;
    std::atomic<bool> cancel{false};
    SsoOptions o = options(&opened);
    o.cancel = &cancel;
    std::thread stopper([&] {
        std::this_thread::sleep_for(80ms);
        cancel = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    SsoResult r = run_sso(factory({Script{true, "", {kToken}, true}}, shared), o);
    stopper.join();
    CHECK_FALSE(r.ok);
    CHECK(r.cancelled);
    CHECK(shared.aborts >= 1);
    CHECK(std::chrono::steady_clock::now() - t0 < 3s);

    Shared s2;
    o = options(&opened);
    o.timeout = 120ms;
    r = run_sso(factory({Script{true, "", {kToken}, true}}, s2), o);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.cancelled);
    CHECK(r.error.find("timed out") != std::string::npos);
}
