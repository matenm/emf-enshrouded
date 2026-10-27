// Downloading a Nexus file: the file's details and its download link from the API v1, then the file from the CDN into
// the downloads folder, against a fake API transport and a fake CDN (no network). The key goes to the API only; sizes
// are checked against Nexus's size_in_bytes and the 2 GB cap; a failure or a cancel leaves no file behind.
#include <doctest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nexus/download.h"

using namespace emf::nexus;
namespace fs = std::filesystem;

namespace {
std::string fixture(const char* name) {
    std::ifstream f(std::string(EMF_SOURCE_DIR) + "/tests/fixtures/nexus/" + name, std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string header(const HttpHeaders& h, const std::string& name) {
    for (const auto& [k, v] : h)
        if (k == name) return v;
    return "<missing>";
}

struct TempDir {
    fs::path dir;
    TempDir() {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        dir = fs::path(base) / (L"emf_dl_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        fs::remove_all(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::vector<std::string> files() const {
        std::vector<std::string> out;
        std::error_code ec;
        if (fs::exists(dir, ec))
            for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path().filename().string());
        return out;
    }
};

// A fake Nexus: the API answers from fixtures; the CDN sends `body` (or what `cdn` says).
struct FakeNexus {
    std::vector<HttpRequest> api;
    std::vector<StreamRequest> cdn_requests;
    std::string file_reply = fixture("v1_file_170_1197.json");
    int file_status = 200;
    std::string link_reply = fixture("v1_download_link_170_1197.json");
    int link_status = 200;
    HttpHeaders limits = {{"X-RL-Daily-Remaining", "100"}, {"X-RL-Hourly-Remaining", "50"}};
    std::string body = std::string(735506, 'Z');
    bool send_length = true;
    uint64_t claimed_length = 0;  // a Content-Length that differs from the body (0: the body's)
    std::atomic<bool>* cancel_after_first_piece = nullptr;
    StreamResponse fail;  // when failure is set, the CDN fails this way

    V1Client::Transport transport() {
        return [this](const HttpRequest& r) {
            api.push_back(r);
            HttpResponse resp;
            resp.headers = limits;
            if (r.url.find("download_link.json") != std::string::npos) {
                resp.status = link_status;
                resp.body = link_reply;
            } else {
                resp.status = file_status;
                resp.body = file_reply;
            }
            return resp;
        };
    }
    Downloader::Fetch fetch() {
        return [this](const StreamRequest& r, const StreamSink& sink) {
            cdn_requests.push_back(r);
            StreamResponse out;
            if (fail.failure != HttpFailure::None) return fail;
            out.status = 200;
            out.final_url = r.url;
            const uint64_t length = claimed_length ? claimed_length : body.size();
            if (sink.start && !sink.start(length, send_length)) {
                out.failure = HttpFailure::Stopped;
                out.error = "stopped";
                return out;
            }
            for (size_t at = 0; at < body.size(); at += 65536) {
                if (r.cancel && r.cancel->load()) {
                    out.failure = HttpFailure::Cancelled;
                    out.error = "cancelled";
                    return out;
                }
                const size_t n = (std::min<size_t>)(65536, body.size() - at);
                out.bytes += n;
                if (sink.write && !sink.write(body.data() + at, n)) {
                    out.failure = HttpFailure::Stopped;
                    out.error = "stopped";
                    return out;
                }
                if (cancel_after_first_piece) *cancel_after_first_piece = true;
            }
            return out;
        };
    }
};

DownloadJob job(const TempDir& t) {
    DownloadJob j;
    j.mod_id = 170;
    j.file_id = 1197;
    j.api_key = "my-api-key-123";
    j.dir = t.dir.wstring();
    return j;
}
}  // namespace

TEST_CASE("nexus download: an nxm link's file lands in the downloads folder under Nexus's name") {
    TempDir t;
    FakeNexus nexus;
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    DownloadJob j = job(t);
    j.nxm_key = "one+time/key=";
    j.nxm_expires = 1792790875;
    std::vector<std::pair<uint64_t, uint64_t>> progress;
    j.progress = [&](uint64_t done, uint64_t total) { progress.emplace_back(done, total); };
    const DownloadOutcome r = d.run(j);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.file_name == "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip");
    CHECK(r.title == "AutoLootPlus 1.1.8");
    CHECK(r.version == "1.1.8");
    CHECK(r.bytes == 735506);
    CHECK(r.path == (t.dir / L"AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip").wstring());
    CHECK(fs::file_size(r.path) == 735506);
    CHECK(t.files() == std::vector<std::string>{"AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip"});  // no .part left
    CHECK(r.limits.daily_remaining == 100);
    // the API: the file's details, then its link with the link's key and expiry
    REQUIRE(nexus.api.size() == 2);
    CHECK(nexus.api[0].url == "https://api.nexusmods.com/v1/games/enshrouded/mods/170/files/1197.json");
    CHECK(nexus.api[1].url.find("download_link.json?key=one%2Btime%2Fkey%3D&expires=1792790875") != std::string::npos);
    for (const auto& a : nexus.api) CHECK(header(a.headers, "apikey") == "my-api-key-123");
    // the CDN: Nexus's first https link, without the API key
    REQUIRE(nexus.cdn_requests.size() == 1);
    CHECK(nexus.cdn_requests[0].url.rfind("https://cf-files.nexusmods.com/cdn/5811/170/", 0) == 0);
    CHECK(header(nexus.cdn_requests[0].headers, "apikey") == "<missing>");
    CHECK(header(nexus.cdn_requests[0].headers, "Application-Name") == "EMF");
    CHECK(nexus.cdn_requests[0].max_bytes == kMaxDownloadBytes);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == std::make_pair<uint64_t, uint64_t>(735506, 735506));
}

TEST_CASE("nexus download: a Premium member's download asks for the link without parameters") {
    TempDir t;
    FakeNexus nexus;
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    const DownloadOutcome r = d.run(job(t));
    REQUIRE_MESSAGE(r.ok, r.error);
    REQUIRE(nexus.api.size() == 2);
    CHECK(nexus.api[1].url == "https://api.nexusmods.com/v1/games/enshrouded/mods/170/files/1197/download_link.json");
}

TEST_CASE("nexus download: a file of another size than Nexus says is not kept") {
    TempDir t;
    FakeNexus nexus;
    nexus.body = std::string(1000, 'Z');
    nexus.send_length = false;  // no Content-Length: the bytes are counted
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    DownloadOutcome r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("735506") != std::string::npos);
    CHECK(t.files().empty());

    nexus.body = std::string(735506, 'Z');
    nexus.send_length = true;
    nexus.claimed_length = 999;  // the CDN announces another size: stopped before the body
    r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("size") != std::string::npos);
    CHECK(t.files().empty());

    nexus.claimed_length = 0;
    nexus.body = std::string(735600, 'Z');  // more bytes than Nexus says: stopped as soon as it passes the size
    nexus.send_length = false;
    r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(t.files().empty());
}

TEST_CASE("nexus download: nothing above the 2 GB cap is fetched") {
    TempDir t;
    FakeNexus nexus;
    nexus.file_reply = R"({"file_id":1197,"name":"Huge","version":"1","file_name":"huge.zip","size_in_bytes":3000000000})";
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    DownloadOutcome r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("2 GB") != std::string::npos);
    CHECK(nexus.api.size() == 1);  // no link asked for
    CHECK(nexus.cdn_requests.empty());

    // unknown size, but the CDN announces more than the cap (a smaller cap here)
    FakeNexus unsized;
    unsized.file_reply = fixture("v1_file_no_size.json");
    Downloader d2("0.3.0", unsized.transport(), unsized.fetch());
    DownloadJob j = job(t);
    j.max_bytes = 100000;
    r = d2.run(j);
    CHECK_FALSE(r.ok);
    CHECK(t.files().empty());
}

TEST_CASE("nexus download: a file that is not a .zip, .7z or .rar is not downloaded") {
    TempDir t;
    FakeNexus nexus;
    nexus.file_reply = R"({"file_id":1197,"name":"Tool","version":"1","file_name":"Installer.exe","size_in_bytes":10})";
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    const DownloadOutcome r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find(".zip, .7z and .rar") != std::string::npos);
    CHECK(nexus.api.size() == 1);
    CHECK(nexus.cdn_requests.empty());
}

TEST_CASE("nexus download: Nexus's refusals are explained for free accounts and for old links") {
    TempDir t;
    FakeNexus nexus;
    nexus.link_reply = fixture("v1_download_link_403.json");
    nexus.link_status = 403;
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    DownloadOutcome r = d.run(job(t));  // no nxm key: a "Premium" download for a free account
    CHECK_FALSE(r.ok);
    CHECK(r.api_status == 403);
    CHECK(r.error.find("Premium") != std::string::npos);
    CHECK(r.error.find("Mod Manager Download") != std::string::npos);
    CHECK(nexus.cdn_requests.empty());

    nexus.link_reply = fixture("v1_download_link_410.json");
    nexus.link_status = 410;
    DownloadJob j = job(t);
    j.nxm_key = "k";
    j.nxm_expires = 5;
    r = d.run(j);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("Mod Manager Download on Nexus again") != std::string::npos);
    CHECK(t.files().empty());
}

TEST_CASE("nexus download: when both request allowances are used up EMF stops before the next request") {
    TempDir t;
    FakeNexus nexus;
    nexus.limits = {{"X-RL-Daily-Remaining", "0"}, {"X-RL-Hourly-Remaining", "0"}, {"X-RL-Hourly-Reset", "2026-10-26T20:00:00+00:00"}};
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    const DownloadOutcome r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("used up") != std::string::npos);
    CHECK(r.limits.exhausted());
    CHECK(nexus.api.size() == 1);
}

TEST_CASE("nexus download: without a key nothing is asked; a cancel or a broken connection leaves nothing behind") {
    TempDir t;
    FakeNexus nexus;
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    DownloadJob j = job(t);
    j.api_key.clear();
    DownloadOutcome r = d.run(j);
    CHECK_FALSE(r.ok);
    CHECK(nexus.api.empty());

    std::atomic<bool> cancel{false};
    nexus.cancel_after_first_piece = &cancel;
    j = job(t);
    j.cancel = &cancel;
    r = d.run(j);
    CHECK_FALSE(r.ok);
    CHECK(r.cancelled);
    CHECK(t.files().empty());

    nexus.cancel_after_first_piece = nullptr;
    nexus.fail.failure = HttpFailure::Network;
    nexus.fail.error = "receive: the connection was reset (12030)";
    r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("connection was reset") != std::string::npos);
    CHECK(t.files().empty());
}

TEST_CASE("nexus download: a file of the same name downloaded before is replaced") {
    TempDir t;
    fs::create_directories(t.dir);
    std::ofstream(t.dir / L"AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip", std::ios::binary) << "old";
    FakeNexus nexus;
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    const DownloadOutcome r = d.run(job(t));
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(fs::file_size(r.path) == 735506);
}

TEST_CASE("nexus download: file names are made safe for the downloads folder") {
    CHECK(safe_download_name("AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip") == "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip");
    CHECK(safe_download_name("..\\..\\evil/thing.7z") == "thing.7z");
    CHECK(safe_download_name("a<b>c:d|e?f*g\".rar") == "a_b_c_d_e_f_g_.rar");
    CHECK(safe_download_name("Mod.ZIP") == "Mod.ZIP");
    CHECK(safe_download_name("con.zip") == "nexus-con.zip");
    CHECK(safe_download_name("tool.exe").empty());
    CHECK(safe_download_name("noext").empty());
    CHECK(safe_download_name(".zip") == "nexus-download.zip");
    CHECK(safe_download_name(std::string(300, 'x') + ".zip").size() <= 160);
    CHECK(uri_file_name("https://cf-files.nexusmods.com/cdn/5811/170/Auto%20Loot%201.zip?md5=x&expires=1") == "Auto Loot 1.zip");
    CHECK(uri_file_name("https://x.example/") .empty());
}

TEST_CASE("nexus download: without Nexus's size, a download shorter than the CDN's Content-Length is not kept") {
    TempDir t;
    FakeNexus nexus;
    nexus.file_reply = fixture("v1_file_no_size.json");
    nexus.body = std::string(1000, 'Z');
    nexus.claimed_length = 5000;  // the connection ended early
    Downloader d("0.3.0", nexus.transport(), nexus.fetch());
    const DownloadOutcome r = d.run(job(t));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("5000") != std::string::npos);
    CHECK(t.files().empty());
}
