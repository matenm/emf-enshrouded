#pragma once
// Nexus Mods' REST API v1 (https://api.nexusmods.com/v1/): the calls that need the player's API key.
//   GET users/validate.json                                    who the key belongs to, Premium or not
//   GET games/enshrouded/mods/<mod>/files/<file>.json          a file's archive name and exact size
//   GET games/enshrouded/mods/<mod>/files/<file>/download_link.json[?key=<k>&expires=<e>]
//                                                              where to download it (CDN addresses)
// Premium members ask for download links without parameters; free accounts only with the key and expiry of an nxm://
// link from the website's "Mod Manager Download" button (Nexus's rule). Every request carries the key in its `apikey`
// header (never in the address), EMF's Application-Name and Application-Version, and goes to api.nexusmods.com only.
// Every reply's X-RL-* headers say how many requests the account has left; when both allowances are used up EMF stops
// asking (rate_limit_message). The email the validate reply carries is never read.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "nexus/client.h"
#include "nexus/http.h"

namespace emf::nexus {

constexpr const char* kV1Base = "https://api.nexusmods.com/v1/";

struct NexusUser {
    int64_t user_id = 0;
    std::string name;
    bool premium = false;
    bool supporter = false;
    std::string profile_url;  // the avatar picture
};

struct V1File {
    int64_t file_id = 0;
    std::string name, version, category;
    std::string file_name;    // the archive's name ("AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip")
    uint64_t size_bytes = 0;  // exact; only when size_known
    bool size_known = false;
};

struct CdnLink {
    std::string name, short_name;
    std::string uri;  // https:// only
};

// The X-RL-* headers of a reply. -1 when a header was not there.
struct RateLimits {
    bool known = false;  // at least one remaining count arrived
    int64_t daily_limit = -1, daily_remaining = -1, hourly_limit = -1, hourly_remaining = -1;
    std::string daily_reset, hourly_reset;  // as Nexus sends them (ISO 8601)
    // Both allowances are used up: Nexus refuses requests until a reset (the hourly allowance works once the daily one
    // is gone).
    bool exhausted() const { return known && daily_remaining == 0 && hourly_remaining == 0; }
};
RateLimits parse_rate_limits(const HttpHeaders& headers);
// "Nexus's request limit for your account is used up ..." with when it resets.
std::string rate_limit_message(const RateLimits& limits);
// "2026-10-26T20:00:00+00:00" (also "Z", fractions, a space for the T, no zone = UTC) as Unix seconds; 0 when it is not
// such a time.
int64_t iso8601_to_unix(std::string_view text);
// A time in the PC's own time zone: "21:00" today, "2026-10-27 01:00" on another day.
std::string local_clock(int64_t unix_seconds);

template <class T>
struct V1Result {
    T value{};
    std::string error;  // "" when it worked; otherwise why, in words
    int status = 0;     // the HTTP status (0 when no reply arrived)
    RateLimits limits;  // from the reply's headers (filled by V1Client)
    bool ok() const { return error.empty(); }
};

V1Result<NexusUser> parse_validate(std::string_view body, int http_status);
V1Result<V1File> parse_file_info(std::string_view body, int http_status);
V1Result<std::vector<CdnLink>> parse_download_links(std::string_view body, int http_status);

// A key EMF will send: 8 to 1024 printable ASCII characters, no spaces.
bool api_key_text_ok(std::string_view key);
// Percent-escapes everything but letters, digits and -_.~ (for a query parameter).
std::string url_query_escape(std::string_view s);

class V1Client {
public:
    using Transport = std::function<HttpResponse(const HttpRequest&)>;
    // `app_version`: EMF's VERSION; `transport` replaces WinHTTP in tests.
    explicit V1Client(std::string app_version, Transport transport = nullptr, int timeout_ms = 15000);

    V1Result<NexusUser> validate(const std::string& api_key, const std::atomic<bool>* cancel = nullptr,
                                 std::chrono::milliseconds deadline = kDefaultDeadline) const;
    V1Result<V1File> file(const std::string& api_key, int64_t mod_id, int64_t file_id,
                          const std::atomic<bool>* cancel = nullptr,
                          std::chrono::milliseconds deadline = kDefaultDeadline) const;
    // `nxm_key`/`expires` from an nxm:// link; "" and 0 for a Premium member's download.
    V1Result<std::vector<CdnLink>> download_links(const std::string& api_key, int64_t mod_id, int64_t file_id,
                                                  const std::string& nxm_key, int64_t expires,
                                                  const std::atomic<bool>* cancel = nullptr,
                                                  std::chrono::milliseconds deadline = kDefaultDeadline) const;

    // The request for `path` (after /v1/) with the key in its header (for tests).
    HttpRequest request_for(const std::string& path, const std::string& api_key) const;
    const std::string& version() const { return version_; }

private:
    template <class T, class Parse>
    V1Result<T> get(const std::string& path, const std::string& api_key, const std::atomic<bool>* cancel,
                    std::chrono::milliseconds deadline, Parse parse) const;

    std::string version_;
    Transport transport_;
    int timeout_ms_;
};

}  // namespace emf::nexus
