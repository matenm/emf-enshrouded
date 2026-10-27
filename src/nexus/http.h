#pragma once
// A small HTTPS client on WinHTTP: one synchronous request with timeouts. HTTPS only, no redirects; gzip and deflate
// replies are unpacked by WinHTTP. Used by client.h and v1.h; call it from a worker thread, never from a UI thread.
// http_stream (below) downloads a file to a sink, for download.h.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emf::nexus {

struct HttpRequest {
    std::string method = "POST";
    std::string url;  // https://host[:port]/path
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;  // no CR or LF in names or values
    std::string user_agent;                                    // no CR or LF
    int timeout_ms = 15000;                                    // for each of resolve, connect, send and receive
    size_t max_body = 4 * 1024 * 1024;                         // a longer reply is an error
    // Checked before connecting and between reads. A blocking WinHTTP call still runs to its own timeout, which is
    // never longer than the time left before the deadline.
    const std::atomic<bool>* cancel = nullptr;
    std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)();
    // Tests only: also http://127.0.0.1 (a test server). The API client and the downloader never set it.
    bool allow_loopback_http = false;
};

enum class HttpFailure { None, Network, Cancelled, TimedOut, TooLarge, Stopped };

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    int status = 0;  // HTTP status; 0 when no reply arrived
    std::string body;
    std::string error;  // why no reply arrived; exactly "cancelled" or "timed out" for those two
    HttpFailure failure = HttpFailure::None;
    HttpHeaders headers;  // the reply's headers, as sent
};

HttpResponse http_request(const HttpRequest& request);

// The value of header `name` (case ignored), or "" when there is none.
std::string header_value(const HttpHeaders& headers, std::string_view name);
// "Name: value" lines (CRLF between them, the status line first) as a header list.
HttpHeaders parse_raw_headers(std::string_view raw);

// ---- streaming a download (a file from Nexus's CDN) ----
// One GET whose body goes to a sink as it arrives, never all in memory. Redirects are followed by hand, at most
// `max_redirects`, and only to https:// addresses (redirect_target); the request's headers go along. Nothing is
// unpacked (no gzip): the bytes are the file's.
struct StreamRequest {
    std::string url;  // https:// only
    HttpHeaders headers;  // no CR or LF in names or values
    std::string user_agent;
    int timeout_ms = 30000;           // for each step; a transfer that stalls this long ends as "timed out"
    uint64_t max_bytes = 2ull << 30;  // a longer body is refused (a Content-Length above it before any byte)
    int max_redirects = 5;
    // Checked between reads; when it is set, a watcher closes the request, so a read that waits on a stalled
    // connection returns at once instead of after timeout_ms.
    const std::atomic<bool>* cancel = nullptr;
    bool allow_loopback_http = false;  // tests only: the first address may be http://127.0.0.1 (redirects stay https)
};

struct StreamResponse {
    int status = 0;          // the last reply's status; 0 when none arrived
    uint64_t bytes = 0;      // body bytes given to the sink
    std::string final_url;   // after redirects
    std::string error;       // why it stopped ("" when the whole body arrived with status 200)
    HttpFailure failure = HttpFailure::None;
};

struct StreamSink {
    // A 200 reply arrived: its length (`known` when it sent a Content-Length). False stops before the body (Stopped).
    std::function<bool(uint64_t length, bool known)> start;
    // A piece of the body. False stops the transfer (Stopped).
    std::function<bool(const char* data, size_t size)> write;
};

StreamResponse http_stream(const StreamRequest& request, const StreamSink& sink);

// Where a redirect from `from` to `location` goes (absolute, "//host/...", "/path" or a relative path). nullopt, with
// the reason in `why`, unless the result is an https:// address with a host and no user name.
std::optional<std::string> redirect_target(std::string_view from, std::string_view location, std::string* why = nullptr);

}  // namespace emf::nexus
