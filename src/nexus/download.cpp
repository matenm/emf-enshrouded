#include "nexus/download.h"

#include <windows.h>

#include <cctype>
#include <filesystem>

#include "nexus/classify.h"

namespace emf::nexus {

namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), ' ');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string mb(uint64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return b;
}

int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A file being written: removed again unless keep() is called.
class PartFile {
public:
    explicit PartFile(std::wstring path) : path_(std::move(path)) {
        h_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~PartFile() {
        close();
        if (!kept_) DeleteFileW(path_.c_str());
    }
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;
    bool open() const { return h_ != INVALID_HANDLE_VALUE; }
    bool write(const char* data, size_t n) {
        while (n > 0) {
            const DWORD chunk = n > (1u << 20) ? (1u << 20) : static_cast<DWORD>(n);
            DWORD put = 0;
            if (!WriteFile(h_, data, chunk, &put, nullptr) || put != chunk) return false;
            data += chunk;
            n -= chunk;
        }
        return true;
    }
    void close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
    // Renames it to `final_path` (replacing an older download of that name) and keeps it.
    bool keep_as(const std::wstring& final_path) {
        close();
        if (!MoveFileExW(path_.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        kept_ = true;
        return true;
    }

private:
    std::wstring path_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    bool kept_ = false;
};

}  // namespace

std::string uri_file_name(std::string_view uri) {
    std::string_view path = uri.substr(0, uri.find_first_of("?#"));
    const size_t scheme = path.find("://");
    if (scheme != std::string_view::npos) {
        const size_t slash = path.find('/', scheme + 3);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash);
    }
    const size_t last = path.rfind('/');
    const std::string_view seg = last == std::string_view::npos ? path : path.substr(last + 1);
    std::string out;
    for (size_t i = 0; i < seg.size(); ++i) {
        if (seg[i] == '%' && i + 2 < seg.size() && hexv(seg[i + 1]) >= 0 && hexv(seg[i + 2]) >= 0) {
            out += static_cast<char>(hexv(seg[i + 1]) * 16 + hexv(seg[i + 2]));
            i += 2;
        } else {
            out += seg[i];
        }
    }
    return out;
}

std::string safe_download_name(std::string_view nexus_name) {
    std::string name(nexus_name);
    const size_t sep = name.find_last_of("/\\");
    if (sep != std::string::npos) name = name.substr(sep + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return {};
    const std::string ext = name.substr(dot);
    const std::string low = lower(ext);
    if (low != ".zip" && low != ".7z" && low != ".rar") return {};
    std::string stem = name.substr(0, dot);
    for (char& c : stem) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || std::string("<>:\"|?*").find(c) != std::string::npos) c = '_';
    }
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) stem.pop_back();
    while (!stem.empty() && stem.front() == ' ') stem.erase(stem.begin());
    if (stem.empty()) stem = "nexus-download";
    // at most 160 characters in all, not cut inside a UTF-8 sequence
    const size_t room = 160 - ext.size();
    if (stem.size() > room) {
        size_t cut = room;
        while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) --cut;
        stem.resize(cut);
        while (!stem.empty() && (stem.back() == '.' || stem.back() == ' ')) stem.pop_back();
    }
    std::string out = stem + ext;
    if (unsafe_archive_path(out)) out = "nexus-" + out;  // a device name (con.zip)
    if (unsafe_archive_path(out)) return {};
    return out;
}

Downloader::Downloader(std::string app_version, V1Client::Transport api, Fetch fetch)
    : v1_(app_version, std::move(api)), fetch_(std::move(fetch)), version_(v1_.version()) {}

DownloadOutcome Downloader::run(const DownloadJob& job) const {
    DownloadOutcome out;
    const auto cancelled = [&] { return job.cancel && job.cancel->load(); };
    const auto stop = [&](std::string why) {
        out.ok = false;
        out.error = std::move(why);
        return out;
    };
    if (job.api_key.empty()) return stop("Set your Nexus API key first (the EMF tab, Nexus account).");
    if (job.mod_id <= 0 || job.file_id <= 0) return stop("That is not a Nexus file.");
    if (cancelled()) {
        out.cancelled = true;
        return stop("cancelled");
    }

    // 1. the file's details: its archive name and size
    const auto file = v1_.file(job.api_key, job.mod_id, job.file_id, job.cancel);
    out.limits = file.limits;
    out.api_status = file.status;
    if (!file.ok()) {
        out.cancelled = file.error == "cancelled";
        return stop(out.cancelled ? file.error : "Could not get the file's details from Nexus: " + file.error);
    }
    out.title = file.value.name;
    out.version = file.value.version;
    const std::string name = safe_download_name(file.value.file_name);
    if (name.empty())
        return stop("EMF installs .zip, .7z and .rar files, and this file is \"" + file.value.file_name +
                    "\". Download it on Nexus and install it by hand.");
    if (file.value.size_known && file.value.size_bytes > job.max_bytes)
        return stop("The file is " + mb(file.value.size_bytes) + "; EMF downloads and installs files up to 2 GB.");
    if (out.limits.exhausted()) return stop(rate_limit_message(out.limits));

    // 2. where to download it
    const auto links = v1_.download_links(job.api_key, job.mod_id, job.file_id, job.nxm_key, job.nxm_expires, job.cancel);
    if (links.limits.known) out.limits = links.limits;
    out.api_status = links.status;
    if (!links.ok()) {
        out.cancelled = links.error == "cancelled";
        if (out.cancelled) return stop(links.error);
        if ((links.status == 403 || links.status == 401) && job.nxm_key.empty())
            return stop("Nexus gives download links through its API to Premium members only. With a free account, click "
                        "Mod Manager Download on the file's page on Nexus, and EMF downloads it from there. (" +
                        links.error + ")");
        if (links.status == 403 || links.status == 410)
            return stop("Nexus refused this download link: it may have expired, or be for another account. Click Mod "
                        "Manager Download on Nexus again. (" + links.error + ")");
        return stop("Could not get the download link from Nexus: " + links.error);
    }
    const std::string uri = links.value.front().uri;

    // 3. the file, from the CDN (never with the API key)
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(job.dir), ec);
    const std::wstring final_path = job.dir + L"\\" + wide(name);
    PartFile part(final_path + L".part");
    if (!part.open()) return stop("Could not write to the downloads folder " + narrow(job.dir) + ".");
    out.file_name = name;

    const uint64_t expected = file.value.size_known ? file.value.size_bytes : 0;
    uint64_t total = expected, done = 0, reported = 0;
    uint64_t announced = 0;  // the CDN's Content-Length
    bool announced_known = false;
    std::string sink_error;
    StreamSink sink;
    sink.start = [&](uint64_t length, bool known) {
        if (known && length > job.max_bytes) {
            sink_error = "The file is " + mb(length) + "; EMF downloads and installs files up to 2 GB.";
            return false;
        }
        if (known && expected && length != expected) {
            sink_error = "Nexus's download server sends " + std::to_string(length) + " bytes, but the file's size on Nexus is " +
                         std::to_string(expected) + " bytes. EMF did not keep it.";
            return false;
        }
        if (known && !total) total = length;
        announced = length;
        announced_known = known;
        if (job.progress) job.progress(0, total);
        return true;
    };
    sink.write = [&](const char* data, size_t n) {
        if (expected && done + n > expected) {
            sink_error = "Nexus's download server sent more than the file's size on Nexus (" + std::to_string(expected) +
                         " bytes). EMF did not keep it.";
            return false;
        }
        if (announced_known && done + n > announced) {
            sink_error = "Nexus's download server sent more than it announced (" + std::to_string(announced) +
                         " bytes). EMF did not keep it.";
            return false;
        }
        if (done + n > job.max_bytes) {
            sink_error = "The file is larger than 2 GB; EMF downloads and installs files up to 2 GB.";
            return false;
        }
        if (!part.write(data, n)) {
            sink_error = "Could not write the download (is the disk full?).";
            return false;
        }
        done += n;
        if (job.progress && done - reported >= 256 * 1024) {
            reported = done;
            job.progress(done, total);
        }
        return true;
    };
    StreamRequest req;
    req.url = uri;
    req.user_agent = "EMF/" + version_ + " (Enshrouded Modding Framework; Windows)";
    req.headers = {{"Application-Name", "EMF"}, {"Application-Version", version_}};
    req.max_bytes = job.max_bytes;
    req.cancel = job.cancel;
    const StreamResponse got = fetch_ ? fetch_(req, sink) : http_stream(req, sink);
    if (got.failure == HttpFailure::Cancelled || cancelled()) {
        out.cancelled = true;
        return stop("cancelled");
    }
    if (!sink_error.empty()) return stop(sink_error);
    if (got.failure == HttpFailure::TooLarge) return stop("The file is larger than 2 GB; EMF downloads and installs files up to 2 GB.");
    if (got.failure != HttpFailure::None || got.status != 200)
        return stop("The download from Nexus failed: " + (got.error.empty() ? "answer " + std::to_string(got.status) : got.error) + ".");
    if (expected && done != expected)
        return stop("The download has " + std::to_string(done) + " bytes, but the file's size on Nexus is " +
                    std::to_string(expected) + " bytes. EMF did not keep it.");
    if (!expected && announced_known && done != announced)
        return stop("The download has " + std::to_string(done) + " bytes, but Nexus's download server announced " +
                    std::to_string(announced) + " bytes (the connection ended early?). EMF did not keep it.");
    if (job.progress) job.progress(done, total ? total : done);
    if (!part.keep_as(final_path)) return stop("Could not finish the download in the downloads folder (" + std::to_string(GetLastError()) + ").");
    out.ok = true;
    out.path = final_path;
    out.bytes = done;
    return out;
}

}  // namespace emf::nexus
