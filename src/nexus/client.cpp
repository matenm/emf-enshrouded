#include "nexus/client.h"

#include <algorithm>

namespace emf::nexus {

namespace {
using Clock = std::chrono::steady_clock;

constexpr size_t kContentsPage = 500;  // Nexus refuses queries above complexity 10000; a page of 500 paths is 2000
constexpr size_t kMaxContents = 2000;  // so at most 4 requests per file

template <class T>
Result<T> failed(const HttpResponse& r) {
    Result<T> out;
    switch (r.failure) {
        case HttpFailure::Cancelled: out.error = "cancelled"; break;
        case HttpFailure::TimedOut: out.error = "timed out"; break;
        case HttpFailure::TooLarge: out.error = "the reply was too large"; break;
        default: out.error = "could not reach Nexus: " + r.error; break;
    }
    return out;
}

// Why the call must stop before its next request ("cancelled", "timed out"), or "".
template <class T>
bool stopped(const std::atomic<bool>* cancel, Clock::time_point deadline, Result<T>& out) {
    if (cancel && cancel->load()) out.error = "cancelled";
    else if (Clock::now() >= deadline) out.error = "timed out";
    return !out.error.empty();
}
}  // namespace

Client::Client(std::string app_version, Transport transport, int timeout_ms)
    : version_(app_version.empty() ? std::string("unknown") : std::move(app_version)),
      transport_(std::move(transport)),
      timeout_ms_(timeout_ms) {}

HttpRequest Client::request_for(const std::string& body) const {
    HttpRequest r;
    r.method = "POST";
    r.url = std::string("https://") + kHost + kPath;
    r.body = body;
    r.user_agent = "EMF/" + version_ + " (Enshrouded Modding Framework; Windows)";
    r.headers = {{"Content-Type", "application/json"},
                 {"Accept", "application/json"},
                 {"Application-Name", "EMF"},
                 {"Application-Version", version_}};
    r.timeout_ms = timeout_ms_;
    return r;
}

HttpResponse Client::send(const std::string& body, const std::atomic<bool>* cancel, Clock::time_point deadline) const {
    HttpRequest r = request_for(body);
    r.cancel = cancel;
    r.deadline = deadline;
    return transport_ ? transport_(r) : http_request(r);
}

Result<SearchPage> Client::search(const SearchQuery& q, const std::atomic<bool>* cancel,
                                  std::chrono::milliseconds deadline) const {
    const auto until = Clock::now() + deadline;
    Result<SearchPage> out;
    if (stopped(cancel, until, out)) return out;
    const HttpResponse r = send(search_request(q), cancel, until);
    if (r.status == 0) return failed<SearchPage>(r);
    return parse_search(r.body, r.status);
}

Result<NexusMod> Client::mod(int64_t mod_id, const std::atomic<bool>* cancel, std::chrono::milliseconds deadline) const {
    const auto until = Clock::now() + deadline;
    Result<NexusMod> out;
    if (stopped(cancel, until, out)) return out;
    const HttpResponse r = send(mod_request(mod_id), cancel, until);
    if (r.status == 0) return failed<NexusMod>(r);
    return parse_mod(r.body, r.status);
}

Result<std::vector<NexusFile>> Client::files(int64_t mod_id, const std::atomic<bool>* cancel,
                                             std::chrono::milliseconds deadline) const {
    const auto until = Clock::now() + deadline;
    Result<std::vector<NexusFile>> out;
    if (stopped(cancel, until, out)) return out;
    const HttpResponse r = send(files_request(mod_id), cancel, until);
    if (r.status == 0) return failed<std::vector<NexusFile>>(r);
    return parse_files(r.body, r.status);
}

Result<ContentsPage> Client::contents(int64_t mod_id, int64_t file_id, size_t max_entries,
                                      const std::atomic<bool>* cancel, std::chrono::milliseconds deadline) const {
    const auto until = Clock::now() + deadline;
    const size_t want = std::min(max_entries, kMaxContents);
    const size_t max_requests = (want + kContentsPage - 1) / kContentsPage;
    Result<ContentsPage> all;
    size_t offset = 0;
    for (size_t n = 0; n < max_requests && all.value.entries.size() < want; ++n) {
        Result<ContentsPage> stop;
        if (stopped(cancel, until, stop)) return stop;
        const HttpResponse r = send(
            contents_request(mod_id, file_id, static_cast<int>(kContentsPage), static_cast<int>(offset)), cancel,
            until);
        if (r.status == 0) return failed<ContentsPage>(r);
        auto page = parse_contents(r.body, r.status);
        if (!page.ok()) return page;
        if (n == 0) all.value.total = page.value.total;
        const size_t got = page.value.entries.size();
        for (auto& e : page.value.entries) {
            if (all.value.entries.size() == want) break;
            all.value.entries.push_back(std::move(e));
        }
        offset += got;
        // a short page is the last one, whatever totalCount says
        if (got < kContentsPage || offset >= static_cast<size_t>(std::max<int64_t>(all.value.total, 0))) break;
    }
    return all;
}

Result<UpdatesCheck> Client::updates(const std::vector<int64_t>& mod_ids, const std::atomic<bool>* cancel,
                                     std::chrono::milliseconds deadline) const {
    const auto until = Clock::now() + deadline;
    Result<UpdatesCheck> out;
    std::vector<int64_t> ids;  // each valid id once, in order
    for (int64_t id : mod_ids)
        if (id > 0 && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    const size_t cap = kUpdatesPerRequest * kUpdateRequestsMax;
    if (ids.size() > cap) {
        out.value.skipped.assign(ids.begin() + static_cast<std::ptrdiff_t>(cap), ids.end());
        ids.resize(cap);
    }
    for (size_t at = 0; at < ids.size(); at += kUpdatesPerRequest) {
        if (stopped(cancel, until, out)) return out;
        const std::vector<int64_t> batch(ids.begin() + static_cast<std::ptrdiff_t>(at),
                                         ids.begin() + static_cast<std::ptrdiff_t>(std::min(ids.size(), at + kUpdatesPerRequest)));
        const HttpResponse r = send(updates_request(batch), cancel, until);
        ++out.value.requests;
        if (r.status == 0) {
            out.error = failed<UpdatesCheck>(r).error;
            return out;
        }
        auto page = parse_updates(r.body, batch, r.status);
        if (!page.ok()) {
            out.error = page.error;
            return out;
        }
        for (auto& [id, m] : page.value) out.value.mods[id] = std::move(m);
    }
    return out;
}

Result<std::map<std::string, std::vector<std::string>>> Client::changelogs(const std::vector<std::string>& uids,
                                                                           const std::atomic<bool>* cancel,
                                                                           std::chrono::milliseconds deadline) const {
    using Logs = std::map<std::string, std::vector<std::string>>;
    const auto until = Clock::now() + deadline;
    Result<Logs> out;
    const std::vector<std::string> asked = changelogs_request_uids(uids);
    if (asked.empty() || stopped(cancel, until, out)) return out;
    const HttpResponse r = send(changelogs_request(asked), cancel, until);
    if (r.status == 0) return failed<Logs>(r);
    return parse_changelogs(r.body, r.status);
}

}  // namespace emf::nexus
