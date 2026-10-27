#include "browse.h"

#include <algorithm>
#include <cctype>

#include "core/json.h"
#include "json_out.h"
#include "nexus/bbcode.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

int category_rank(const std::string& c) {
    static const char* const order[] = {"MAIN", "OPTIONAL", "UPDATE", "MISCELLANEOUS", "OLD_VERSION"};
    for (int i = 0; i < 5; ++i)
        if (c == order[i]) return i;
    if (c == "ARCHIVED") return 7;
    if (c == "REMOVED") return 8;
    return 6;
}

std::string mod_url(int64_t id) { return "https://www.nexusmods.com/enshrouded/mods/" + std::to_string(id); }

std::string picture(const std::string& url) { return image_url_allowed(url) ? url : std::string(); }

std::string contents_name(int64_t mod, int64_t file) {
    return "contents_" + std::to_string(mod) + "_" + std::to_string(file) + ".json";
}

// A number from a GraphQL variable or filter: {"modId": 170} or {"modId": [{"value": 170, "op": "EQUALS"}]}.
int64_t var_id(const Value* v) {
    if (!v) return 0;
    if (v->is(VT::Array) && !v->arr.empty()) v = v->arr[0].get("value");
    if (!v) return 0;
    if (v->is(VT::Number)) return static_cast<int64_t>(v->n);
    if (v->is(VT::String)) return _atoi64(v->s.c_str());
    return 0;
}

}  // namespace

const std::vector<std::string>& image_hosts() {
    static const std::vector<std::string> hosts = {"staticdelivery.nexusmods.com", "raw.githubusercontent.com",
                                                   "user-images.githubusercontent.com", "i.imgur.com"};
    return hosts;
}

bool image_url_allowed(const std::string& url) {
    const std::string scheme = "https://";
    if (url.size() <= scheme.size() || url.size() > 2048 || url.compare(0, scheme.size(), scheme) != 0) return false;
    for (unsigned char ch : url)
        if (ch <= 0x20 || ch >= 0x7F || std::string("\"<>\\^`{|}").find(static_cast<char>(ch)) != std::string::npos) return false;
    const size_t slash = url.find('/', scheme.size());
    if (slash == std::string::npos) return false;  // a picture has a path
    std::string host = url.substr(scheme.size(), slash - scheme.size());
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return std::find(image_hosts().begin(), image_hosts().end(), host) != image_hosts().end();
}

nexus::GameState game_state_of(const FolderState& s) {
    nexus::GameState g;
    g.known = s.game_found;
    g.proxies = s.proxies;
    g.chained = s.chained_tools;
    return g;
}

std::vector<nexus::NexusFile> order_files(std::vector<nexus::NexusFile> files) {
    std::stable_sort(files.begin(), files.end(), [](const nexus::NexusFile& a, const nexus::NexusFile& b) {
        const int ra = category_rank(a.category), rb = category_rank(b.category);
        if (ra != rb) return ra < rb;
        return a.date > b.date;
    });
    return files;
}

bool contents_at_once(const std::string& category) { return category == "MAIN"; }

std::string search_reply(int seq, int offset, const nexus::Result<nexus::SearchPage>& r) {
    std::vector<std::string> mods;
    for (const auto& m : r.value.mods)
        mods.push_back(Obj()
                           .num("id", static_cast<double>(m.id)).str("name", m.name).str("summary", m.summary)
                           .str("version", m.version).str("author", m.author.empty() ? m.uploader : m.author)
                           .num("downloads", static_cast<double>(m.downloads))
                           .num("endorsements", static_cast<double>(m.endorsements)).str("updated", m.updated_at)
                           .str("thumbnail", picture(m.thumbnail_url)).boolean("adult", m.adult).str("url", mod_url(m.id))
                           .text());
    return Obj()
        .str("type", "nexus_search").num("seq", seq).boolean("ok", r.ok()).str("error", r.error).num("offset", offset)
        .num("total", static_cast<double>(r.value.total)).raw("mods", json_list(mods))
        .text();
}

std::string file_json(const FileFacts& f, const nexus::GameState& g) {
    Obj o;
    o.num("file_id", static_cast<double>(f.file.file_id)).str("name", f.file.name).str("version", f.file.version);
    o.str("category", f.file.category).num("size", static_cast<double>(f.file.size_bytes)).num("date", static_cast<double>(f.file.date));
    o.boolean("primary", f.file.primary).boolean("checked", f.checked);
    const bool listed = f.checked && f.contents.ok() && !f.contents.value.entries.empty();
    o.boolean("listed", listed).str("error", f.checked ? f.contents.error : std::string());
    o.num("entries", static_cast<double>(f.contents.value.entries.size()));
    if (listed) {
        const nexus::Classification c = nexus::classify_archive(f.contents.value.entries, g);
        std::vector<std::string> also;
        for (auto k : c.also) also.push_back(nexus::kind_label(k));
        o.str("kind", nexus::kind_id(c.kind)).str("kind_label", nexus::kind_label(c.kind)).strs("also", also).strs("notes", c.notes);
    } else {
        o.str("kind", "").str("kind_label", "").strs("also", {}).strs("notes", {});
    }
    return o.text();
}

std::string mod_reply(int seq, const nexus::Result<nexus::NexusMod>& mod, const nexus::Result<std::vector<nexus::NexusFile>>& files,
                      const std::vector<FileFacts>& facts, const nexus::GameState& g) {
    const nexus::NexusMod& m = mod.value;
    std::string mod_obj = "null";
    if (mod.ok())
        mod_obj = Obj()
                      .num("id", static_cast<double>(m.id)).str("name", m.name).str("summary", m.summary).str("version", m.version)
                      .str("author", m.author.empty() ? m.uploader : m.author).str("uploader", m.uploader)
                      .num("downloads", static_cast<double>(m.downloads)).num("endorsements", static_cast<double>(m.endorsements))
                      .str("updated", m.updated_at).str("created", m.created_at).str("category", m.category)
                      .str("picture", picture(m.picture_url)).str("thumbnail", picture(m.thumbnail_url)).boolean("adult", m.adult)
                      .str("description_html", nexus::bbcode_to_html(m.description, image_hosts())).str("url", mod_url(m.id))
                      .text();
    std::vector<std::string> list;
    for (const auto& f : facts) list.push_back(file_json(f, g));
    return Obj()
        .str("type", "nexus_mod").num("seq", seq).boolean("ok", mod.ok()).str("error", mod.error).raw("mod", mod_obj)
        .raw("files", json_list(list)).str("files_error", files.error)
        .text();
}

std::string file_reply(int seq, int64_t mod_id, const FileFacts& f, const nexus::GameState& g) {
    return Obj().str("type", "nexus_file").num("seq", seq).num("mod_id", static_cast<double>(mod_id)).raw("file", file_json(f, g)).text();
}

nexus::Client::Transport fixture_transport(const std::wstring& dir) {
    return [dir](const nexus::HttpRequest& req) {
        nexus::HttpResponse resp;
        const auto serve = [&](const std::string& name) {
            if (!read_file(dir + L"\\" + wide(name), resp.body, 16u << 20)) return false;
            resp.status = 200;
            return true;
        };
        const auto parsed = core::json::parse(req.body);
        const Value* vars = parsed.value ? parsed.value->get("variables") : nullptr;
        const Value* query = parsed.value ? parsed.value->get("query") : nullptr;
        const std::string q = query && query->is(VT::String) ? query->s : "";
        const Value empty;
        const Value& v = vars ? *vars : empty;
        if (q.find("EmfSearch") != std::string::npos) {
            const int64_t offset = var_id(v.get("offset"));
            if ((offset > 0 && serve("search_" + std::to_string(offset) + ".json")) || serve("search.json")) return resp;
        } else if (q.find("EmfMod(") != std::string::npos) {
            if (serve("mod_" + std::to_string(var_id(v.get("modId"))) + ".json")) return resp;
        } else if (q.find("EmfFiles") != std::string::npos) {
            if (serve("files_" + std::to_string(var_id(v.get("modId"))) + ".json")) return resp;
        } else if (q.find("EmfContents") != std::string::npos) {
            const Value* filter = v.get("filter");
            const int64_t mod = filter ? var_id(filter->get("modId")) : 0, file = filter ? var_id(filter->get("fileId")) : 0;
            if (var_id(v.get("offset")) == 0 && serve(contents_name(mod, file))) return resp;
            resp.status = 200;  // no listing (yet), as Nexus answers for new uploads
            resp.body = R"({"data":{"modFileContents":{"totalCount":0,"nodes":[]}}})";
            return resp;
        } else if (q.find("EmfUpdates") != std::string::npos) {
            if (serve("updates.json")) return resp;  // "Check for updates": one recorded reply for the mods it knows
        } else if (q.find("EmfChangelogs") != std::string::npos) {
            if (serve("changelogs.json")) return resp;  // and the changelogs of the files to take
        }
        resp.status = 404;
        resp.body = R"({"errors":[{"message":"no recorded reply for this request"}]})";
        return resp;
    };
}

nexus::V1Client::Transport fixture_v1_transport(const std::wstring& dir) {
    return [dir](const nexus::HttpRequest& req) {
        nexus::HttpResponse resp;
        std::string daily = "19999";
        std::string d;
        if (read_file(dir + L"\\v1_daily.txt", d, 64)) {
            while (!d.empty() && (d.back() == '\n' || d.back() == '\r' || d.back() == ' ')) d.pop_back();
            if (!d.empty() && d.size() < 10 && d.find_first_not_of("0123456789") == std::string::npos) daily = d;
        }
        resp.headers = {{"X-RL-Daily-Remaining", daily}, {"X-RL-Hourly-Remaining", daily == "0" ? "0" : "499"},
                        {"X-RL-Hourly-Reset", "2026-10-26T20:00:00+00:00"}, {"X-RL-Daily-Reset", "2026-10-27T00:00:00+00:00"}};
        const auto serve = [&](const std::string& name) {
            if (!read_file(dir + L"\\" + wide(name), resp.body, 1u << 20)) return false;
            resp.status = 200;
            return true;
        };
        std::string key;
        for (const auto& [k, v] : req.headers)
            if (k == "apikey") key = v;
        const std::string base = nexus::kV1Base;
        const std::string path = req.url.rfind(base, 0) == 0 ? req.url.substr(base.size()) : std::string();
        const std::string game = std::string("games/") + nexus::kGameDomain + "/mods/";
        if (path == "users/validate.json") {
            if (key.rfind("bad", 0) == 0) {
                resp.status = 401;
                resp.body = R"({"message":"Please provide a valid API Key"})";
                return resp;
            }
            if (serve("v1_validate.json")) return resp;
        } else if (path.rfind(game, 0) == 0) {
            // <mod>/files/<file>.json or <mod>/files/<file>/download_link.json[?...]
            const std::string rest = path.substr(game.size(), path.find('?') == std::string::npos ? std::string::npos : path.find('?') - game.size());
            const size_t files = rest.find("/files/");
            if (files != std::string::npos) {
                const std::string mod = rest.substr(0, files);
                std::string tail = rest.substr(files + 7);
                const bool link = tail.size() > 19 && tail.compare(tail.size() - 19, 19, "/download_link.json") == 0;
                const std::string file = link ? tail.substr(0, tail.size() - 19) : tail.substr(0, tail.size() - 5);
                const bool digits = !mod.empty() && !file.empty() && mod.find_first_not_of("0123456789") == std::string::npos &&
                                    file.find_first_not_of("0123456789") == std::string::npos;
                if (digits && serve(std::string(link ? "v1_link_" : "v1_file_") + mod + "_" + file + ".json")) return resp;
            }
        }
        resp.status = 404;
        resp.body = R"({"message":"no recorded reply for this request"})";
        return resp;
    };
}

nexus::Downloader::Fetch fixture_fetch(const std::wstring& dir) {
    return [dir](const nexus::StreamRequest& req, const nexus::StreamSink& sink) {
        nexus::StreamResponse out;
        out.final_url = req.url;
        const std::string name = nexus::uri_file_name(req.url);
        std::string bytes;
        if (name.empty() || name.find_first_of("\\/:") != std::string::npos ||
            !read_file(dir + L"\\downloads\\" + wide(name), bytes, 64u << 20)) {
            out.status = 404;
            out.failure = nexus::HttpFailure::Network;
            out.error = "the download server answered 404";
            return out;
        }
        out.status = 200;
        if (sink.start && !sink.start(bytes.size(), true)) {
            out.failure = nexus::HttpFailure::Stopped;
            out.error = "stopped";
            return out;
        }
        for (size_t at = 0; at < bytes.size(); at += 65536) {
            if (req.cancel && req.cancel->load()) {
                out.failure = nexus::HttpFailure::Cancelled;
                out.error = "cancelled";
                return out;
            }
            const size_t n = (std::min<size_t>)(65536, bytes.size() - at);
            out.bytes += n;
            if (sink.write && !sink.write(bytes.data() + at, n)) {
                out.failure = nexus::HttpFailure::Stopped;
                out.error = "stopped";
                return out;
            }
        }
        return out;
    };
}

nexus::V1Client::Transport offline_v1_transport() {
    return [](const nexus::HttpRequest&) {
        nexus::HttpResponse r;
        r.failure = nexus::HttpFailure::Network;
        r.error = "this test run makes no Nexus requests";
        return r;
    };
}

nexus::Downloader::Fetch offline_fetch() {
    return [](const nexus::StreamRequest&, const nexus::StreamSink&) {
        nexus::StreamResponse r;
        r.failure = nexus::HttpFailure::Network;
        r.error = "this test run makes no Nexus requests";
        return r;
    };
}

std::wstring fixture_image(const std::wstring& dir, const std::string& url) {
    if (!image_url_allowed(url)) return {};
    std::string path = url.substr(0, url.find_first_of("?#"));
    const std::string name = path.substr(path.find_last_of('/') + 1);
    if (name.empty() || name == "." || name == ".." || name.find_first_of("\\:") != std::string::npos) return {};
    const std::wstring file = dir + L"\\images\\" + wide(name);
    return exists(file) && !is_dir(file) ? file : std::wstring();
}

}  // namespace emf::app
