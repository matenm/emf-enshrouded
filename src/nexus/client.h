#pragma once
// Nexus Mods' public API over HTTPS, for the app. Each call is one request (an archive listing longer than a page
// takes a few, at most 4), made only when the player asks for something: no background fetching, no caching of whole
// lists. Every request names EMF as Nexus asks apps to: Application-Name: EMF, Application-Version: <EMF's version>,
// and a User-Agent of EMF/<version>. Synchronous: call it from a worker thread.
//
// Every call takes an optional cancel flag (set it from another thread) and an overall deadline; both are checked
// before each request and between reads, and end the call with the error "cancelled" or "timed out".
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "nexus/api.h"
#include "nexus/http.h"
#include "nexus/updates.h"

namespace emf::nexus {

constexpr std::chrono::milliseconds kDefaultDeadline{30000};

class Client {
public:
    using Transport = std::function<HttpResponse(const HttpRequest&)>;

    // `app_version`: EMF's VERSION ("0.2.0"); never blank. `transport` replaces WinHTTP in tests.
    explicit Client(std::string app_version, Transport transport = nullptr, int timeout_ms = 15000);

    Result<SearchPage> search(const SearchQuery& q, const std::atomic<bool>* cancel = nullptr,
                              std::chrono::milliseconds deadline = kDefaultDeadline) const;
    Result<NexusMod> mod(int64_t mod_id, const std::atomic<bool>* cancel = nullptr,
                         std::chrono::milliseconds deadline = kDefaultDeadline) const;
    Result<std::vector<NexusFile>> files(int64_t mod_id, const std::atomic<bool>* cancel = nullptr,
                                         std::chrono::milliseconds deadline = kDefaultDeadline) const;
    // The paths in one file's archive, in pages of 500, up to `max_entries` (at most 2000, so at most 4 requests).
    Result<ContentsPage> contents(int64_t mod_id, int64_t file_id, size_t max_entries = 2000,
                                  const std::atomic<bool>* cancel = nullptr,
                                  std::chrono::milliseconds deadline = kDefaultDeadline) const;
    // The update check (updates.h): the mods, their status and files, in requests of kUpdatesPerRequest mods, at most
    // kUpdateRequestsMax of them (ids past that are `skipped`). A failed request ends the check with its error and
    // keeps what the requests before it brought.
    Result<UpdatesCheck> updates(const std::vector<int64_t>& mod_ids, const std::atomic<bool>* cancel = nullptr,
                                 std::chrono::milliseconds deadline = kDefaultDeadline) const;
    // The changelogs of the files to take (updates.h): one request for the first kChangelogsPerRequest uids; none for
    // no uids. uid -> lines.
    Result<std::map<std::string, std::vector<std::string>>> changelogs(const std::vector<std::string>& uids,
                                                                       const std::atomic<bool>* cancel = nullptr,
                                                                       std::chrono::milliseconds deadline = kDefaultDeadline) const;

    // The request a call sends (for tests and the command-line tool).
    HttpRequest request_for(const std::string& body) const;

private:
    HttpResponse send(const std::string& body, const std::atomic<bool>* cancel,
                      std::chrono::steady_clock::time_point deadline) const;

    std::string version_;
    Transport transport_;
    int timeout_ms_;
};

}  // namespace emf::nexus
