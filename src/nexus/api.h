#pragma once
// Nexus Mods' public GraphQL API (https://api.nexusmods.com/v2/graphql, no key needed): the request bodies EMF sends
// and the parsers for the replies. Pure (no network; client.h does the requests), so it is unit-tested with saved
// replies (tests/fixtures/nexus/). User text only ever goes into GraphQL variables, never into the query text.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emf::nexus {

constexpr const char* kHost = "api.nexusmods.com";
constexpr const char* kPath = "/v2/graphql";
constexpr const char* kGameDomain = "enshrouded";
constexpr int64_t kGameId = 5811;

enum class SortBy { Downloads, Endorsements, Updated, Created, Name };
// "downloads", "endorsements", "updated", "created", "name"; nullopt for anything else.
std::optional<SortBy> sort_from_name(std::string_view name);

struct SearchQuery {
    std::string text;      // matched against mod names (Nexus's WILDCARD: "loot" finds "Auto Loot Plus"); "" = all
    std::string category;  // Nexus category name ("Miscellaneous"); "" = all
    SortBy sort = SortBy::Downloads;
    bool descending = true;
    int count = 20;   // 1..100
    int offset = 0;   // >= 0
};

// Request bodies: {"query": "...", "variables": {...}}.
std::string search_request(const SearchQuery& q);
std::string mod_request(int64_t mod_id);  // one mod with its description
std::string files_request(int64_t mod_id);
std::string contents_request(int64_t mod_id, int64_t file_id, int count = 500, int offset = 0);  // count 1..1000

struct NexusMod {
    int64_t id = 0;
    std::string name, summary, version;
    std::string author;    // the author named on the page (may differ from the uploader)
    std::string uploader;  // the Nexus account that uploaded it
    int64_t downloads = 0, endorsements = 0;
    std::string updated_at, created_at;  // ISO 8601 as Nexus sends it ("2026-10-23T21:30:04Z")
    std::string category;
    std::string picture_url, thumbnail_url;
    bool adult = false;
    std::string description;  // BBCode; only filled by mod_request (see bbcode.h)
};

struct NexusFile {
    int64_t file_id = 0;
    std::string name, version;
    std::string category;  // MAIN, UPDATE, OPTIONAL, OLD_VERSION, MISCELLANEOUS, REMOVED, ARCHIVED
    uint64_t size_bytes = 0;
    int64_t date = 0;  // upload time, Unix seconds
    bool primary = false;
    std::vector<std::string> changelog;  // the file's changelog lines; only the update check asks for them (updates.h)
    std::string uid;                     // the file's GraphQL uid, for its changelog (updates.h)
};

struct NexusArchiveEntry {
    std::string path;  // as Nexus lists it, '/' between folders
    uint64_t size = 0;
};

struct SearchPage {
    int64_t total = 0;  // matches in all pages
    std::vector<NexusMod> mods;
};

struct ContentsPage {
    int64_t total = 0;  // entries in all pages; 0 also when Nexus has not listed the archive (yet)
    std::vector<NexusArchiveEntry> entries;
};

// A parsed reply, or why there is none (HTTP status, a reply that is not JSON, GraphQL errors). Never throws.
template <class T>
struct Result {
    T value{};
    std::string error;
    bool ok() const { return error.empty(); }
};

Result<SearchPage> parse_search(std::string_view body, int http_status = 200);
Result<NexusMod> parse_mod(std::string_view body, int http_status = 200);
Result<std::vector<NexusFile>> parse_files(std::string_view body, int http_status = 200);
Result<ContentsPage> parse_contents(std::string_view body, int http_status = 200);

// JSON string literal with quotes; invalid UTF-8 bytes become U+FFFD (Nexus refuses invalid JSON).
std::string json_quote(std::string_view s);

}  // namespace emf::nexus
