// The real WinHTTP code of src/nexus/http.cpp against a small HTTP server on 127.0.0.1 (tests only reach it with
// allow_loopback_http, which the API client and the downloader never set): API requests never follow a redirect,
// downloads follow none by themselves (only to https, by hand), and cancelling a download that hangs ends it at once
// instead of after the 30-second timeout.
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "local_http_server.h"
#include "nexus/http.h"
#include "nexus/v1.h"

using namespace emf::nexus;
using namespace std::chrono_literals;

namespace {
std::string redirect_to(const std::string& location) {
    return "HTTP/1.1 302 Found\r\nLocation: " + location + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}
std::string ok_body(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
}  // namespace

TEST_CASE("http (loopback): an API request never follows a redirect") {
    http_test::LocalHttpServer server;
    server.respond = [&](int index, const std::string&) {
        return index == 0 ? redirect_to(server.url("/elsewhere")) : ok_body("{\"followed\":true}");
    };
    server.start();
    // the request the API client builds, sent to the test server instead of api.nexusmods.com
    HttpRequest r = V1Client("0.3.0").request_for("users/validate.json", "my-api-key-123");
    CHECK_FALSE(r.allow_loopback_http);  // the API client never sets it
    r.url = server.url("/v1/users/validate.json");
    r.allow_loopback_http = true;
    const HttpResponse resp = http_request(r);
    CHECK(resp.status == 302);
    CHECK(resp.body.find("followed") == std::string::npos);
    CHECK(server.requests().size() == 1);
}

TEST_CASE("http (loopback): a download follows no redirect by itself, and only to https by hand") {
    http_test::LocalHttpServer server;
    server.respond = [&](int index, const std::string&) {
        return index == 0 ? redirect_to(server.url("/second")) : ok_body("the file");
    };
    server.start();
    StreamRequest r;
    r.url = server.url("/first");
    r.allow_loopback_http = true;
    std::string got;
    StreamSink sink;
    sink.write = [&](const char* d, size_t n) {
        got.append(d, n);
        return true;
    };
    const StreamResponse resp = http_stream(r, sink);
    CHECK(resp.failure != HttpFailure::None);
    CHECK(resp.error.find("not https") != std::string::npos);
    CHECK(got.empty());
    CHECK(server.requests().size() == 1);
}

TEST_CASE("http (loopback): a download streams its body, and loopback http needs the test switch") {
    http_test::LocalHttpServer server;
    server.respond = [&](int, const std::string&) { return ok_body(std::string(100000, 'Z')); };
    server.start();
    StreamRequest r;
    r.url = server.url("/file.zip");
    std::string got;
    StreamSink sink;
    sink.write = [&](const char* d, size_t n) {
        got.append(d, n);
        return true;
    };
    StreamResponse resp = http_stream(r, sink);  // without allow_loopback_http: refused before connecting
    CHECK(resp.error.find("https") != std::string::npos);
    CHECK(server.requests().empty());
    r.allow_loopback_http = true;
    resp = http_stream(r, sink);
    CHECK(resp.status == 200);
    CHECK(resp.failure == HttpFailure::None);
    CHECK(got.size() == 100000);
}

TEST_CASE("http (loopback): cancelling a download that hangs ends it at once, not after the timeout") {
    http_test::LocalHttpServer server;
    server.stall = true;  // a Content-Length of a megabyte, 100 bytes, then nothing
    server.respond = [&](int, const std::string&) {
        return "HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\n" + std::string(100, 'Z');
    };
    server.start();
    std::atomic<bool> cancel{false};
    StreamRequest r;
    r.url = server.url("/hangs.zip");
    r.allow_loopback_http = true;
    r.timeout_ms = 30000;
    r.cancel = &cancel;
    StreamSink sink;
    sink.write = [](const char*, size_t) { return true; };
    std::thread stopper([&] {
        std::this_thread::sleep_for(400ms);
        cancel = true;
    });
    const auto t0 = std::chrono::steady_clock::now();
    const StreamResponse resp = http_stream(r, sink);
    const auto took = std::chrono::steady_clock::now() - t0;
    stopper.join();
    CHECK(resp.failure == HttpFailure::Cancelled);
    CHECK(took < 5s);
}
