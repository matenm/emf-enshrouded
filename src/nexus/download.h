#pragma once
// Downloading one Nexus file for EMF.exe: three requests, made only when the player clicks.
//   1. the file's details (API v1, with the key): its archive name and exact size;
//   2. its download links (API v1, with the key, plus an nxm:// link's key and expiry for a free account);
//   3. the file itself from the first CDN address, over HTTPS (redirects only to https://), streamed into
//      <dir>\<name>.part and renamed to <dir>\<name> when every byte is there.
// The API key goes to api.nexusmods.com only, never to the CDN. A file that is not a .zip, .7z or .rar, or is larger
// than the install pipeline's 2 GB cap, is refused before it is fetched; a download whose size differs from Nexus's
// size_in_bytes, or from the CDN's Content-Length, is stopped and deleted. A failure or a cancel leaves no file behind.
// Synchronous: run it on a worker thread.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "nexus/http.h"
#include "nexus/v1.h"

namespace emf::nexus {

constexpr uint64_t kMaxDownloadBytes = 2ull << 30;  // the install pipeline's cap (app/archive.h, ArchiveLimits)

struct DownloadJob {
    int64_t mod_id = 0, file_id = 0;
    std::string api_key;
    std::string nxm_key;      // from an nxm:// link; "" for a Premium member's download
    int64_t nxm_expires = 0;  // from an nxm:// link
    std::wstring dir;         // the downloads folder (made when missing)
    uint64_t max_bytes = kMaxDownloadBytes;
    const std::atomic<bool>* cancel = nullptr;
    // Bytes so far and the total (0 when unknown), from the worker thread, at most every 256 KiB and at the end.
    std::function<void(uint64_t done, uint64_t total)> progress;
};

struct DownloadOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string error;           // why not, in words
    std::wstring path;           // the downloaded file
    std::string file_name;       // its name in the downloads folder
    std::string title, version;  // the file's name and version on Nexus
    uint64_t bytes = 0;
    RateLimits limits;   // from the last API reply
    int api_status = 0;  // the last API reply's status (403: refused)
};

class Downloader {
public:
    using Fetch = std::function<StreamResponse(const StreamRequest&, const StreamSink&)>;
    // `api` replaces WinHTTP for the API calls and `fetch` for the CDN, in tests.
    explicit Downloader(std::string app_version, V1Client::Transport api = nullptr, Fetch fetch = nullptr);
    DownloadOutcome run(const DownloadJob& job) const;

private:
    V1Client v1_;
    Fetch fetch_;
    std::string version_;
};

// The name a download gets in the downloads folder: the last part of Nexus's name, characters Windows refuses
// replaced by '_', at most 160 characters, never a device name. "" when it is not a .zip, .7z or .rar.
std::string safe_download_name(std::string_view nexus_name);
// The decoded last part of a CDN address's path ("" when it has none).
std::string uri_file_name(std::string_view uri);

}  // namespace emf::nexus
