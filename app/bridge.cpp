#include "bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>

#include "archive.h"
#include "browse.h"
#include "core/json.h"
#include "json_out.h"
#include "locate.h"
#include "nexus/nxm.h"
#include "server_bridge.h"
#include "text.h"

#ifndef EMF_VERSION
#define EMF_VERSION "dev"
#endif

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

// Length of the UTF-8 sequence at s[i], or 0 when the bytes there are not valid UTF-8.
size_t utf8_length(const std::string& s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if ((b(i + k) & 0xC0) != 0x80) return 0;
    if (n == 2 && c < 0xC2) return 0;                                   // overlong
    if (n == 3 && c == 0xE0 && b(i + 1) < 0xA0) return 0;               // overlong
    if (n == 3 && c == 0xED && b(i + 1) >= 0xA0) return 0;              // UTF-16 surrogates
    if (n == 4 && (c > 0xF4 || (c == 0xF0 && b(i + 1) < 0x90) || (c == 0xF4 && b(i + 1) >= 0x90))) return 0;
    return n;
}

std::string setting_json(const ModSetting& s) {
    Obj o;
    o.str("key", s.def.key).str("type", core::setting_type_name(s.def.type)).str("label", s.def.label).str("help", s.def.help);
    o.str("value", s.value).str("default", s.def.default_value).boolean("live", s.def.live);
    if (s.def.has_min) o.num("min", s.def.min);
    if (s.def.has_max) o.num("max", s.def.max);
    if (s.def.step > 0) o.num("step", s.def.step);
    if (s.def.slider) o.str("control", "slider");
    if (s.def.max_length) o.num("max_length", s.def.max_length);
    o.strs("choices", s.def.choices);
    std::string options = "[";
    for (size_t i = 0; i < s.def.options.size(); ++i)
        options += (i ? "," : "") + Obj().str("value", s.def.options[i].value).str("label", s.def.options[i].label).text();
    o.raw("options", options + "]");
    o.str("group", s.def.group).boolean("active", s.active).str("when", s.when).boolean("controls", s.controls);
    return o.text();
}

std::string mod_json(const ModEntry& m) {
    Obj o;
    o.str("key", m.key).str("source", m.source).str("id", m.id).str("name", m.name).str("version", m.version).strs("authors", m.authors);
    o.str("description", m.description).str("loaded_by", m.loaded_by).boolean("enabled", m.enabled);
    o.boolean("can_toggle", m.can_toggle).str("toggle_note", m.toggle_note).str("path", utf8(m.path));
    o.str("readme", m.readme).str("error", m.error).str("status", m.status).str("note", m.note);
    o.str("nexus_id", m.nexus_id).str("game_build", m.game_build).strs("depends", m.depends).boolean("is_loader", m.is_loader);
    o.str("install_key", m.install_key);
    o.str("side", m.side).boolean("side_declared", m.side_declared).str("server_note", m.server_note).str("server_level", m.server_level);
    std::string settings = "[";
    for (size_t i = 0; i < m.settings.size(); ++i) settings += (i ? "," : "") + setting_json(m.settings[i]);
    o.raw("settings", settings + "]");
    return o.text();
}

std::string installed_json(const InstalledMod& m) {
    return Obj()
        .str("key", m.key).str("name", m.name).str("version", m.version).str("nexus_id", m.nexus_id)
        .str("archive", m.archive).str("installed_at", m.installed_at).num("files", static_cast<double>(m.files))
        .strs("roots", m.roots).strs("chain", m.chain).strs("outside", m.outside)
        .text();
}

// The end of a log: its last 64 KB, from a line start. Read while the game writes it (shared, from the end).
std::string log_tail(const std::wstring& path) {
    const size_t keep = 64 * 1024;
    std::string text;
    if (!read_file_tail(path, text, keep + 1)) return {};
    if (text.size() > keep) {  // started inside the file: drop the cut first line
        const size_t nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? text.size() - keep : nl + 1);
    }
    return to_utf8_text(text);
}

// A setting value from the page: text as it is, true/false as 1/0, numbers as short text.
std::string value_text(const Value* v) {
    if (!v) return {};
    if (v->is(VT::String)) return v->s;
    if (v->is(VT::Bool)) return v->b ? "1" : "0";
    if (v->is(VT::Number)) return json_number(v->n);
    return {};
}

std::string text_of(const Value& v, const char* key) {
    const Value* x = v.get(key);
    return x && x->is(VT::String) ? x->s : std::string();
}

// A cached mod reply (built for one seq) re-stamped with the seq the page now waits for.
std::string restamp_seq(std::string reply, int seq) {
    const std::string key = "\"seq\":";
    const size_t at = reply.find(key);
    if (at == std::string::npos) return reply;
    size_t end = at + key.size();
    while (end < reply.size() && (reply[end] == '-' || (reply[end] >= '0' && reply[end] <= '9'))) ++end;
    return reply.substr(0, at + key.size()) + std::to_string(seq) + reply.substr(end);
}

int64_t int_of(const Value& v, const char* key, int64_t fallback = 0) {
    const Value* x = v.get(key);
    if (x && x->is(VT::Number) && std::isfinite(x->n) && std::fabs(x->n) < 1e15) return static_cast<int64_t>(x->n);
    if (x && x->is(VT::String) && !x->s.empty() && x->s.size() < 16 && x->s.find_first_not_of("0123456789") == std::string::npos)
        return _atoi64(x->s.c_str());
    return fallback;
}

// A Nexus id from the page (a number or its digits), as digits; "" for anything else.
std::string nexus_id_of(const Value& v, const char* key) {
    const int64_t n = int_of(v, key);
    return n > 0 && n < 1000000000000LL ? std::to_string(n) : std::string();
}

std::string done_json(const char* type, const InstallResult& r, const std::string& key) {
    return Obj().str("type", type).boolean("ok", r.ok).str("error", r.error).strs("lines", r.done).str("key", key).text();
}

constexpr int64_t kExpiryGrace = 300;  // seconds a link's expiry may lie behind this PC's clock before EMF refuses it

// NexusMode::Off: what the host answers a command that would ask Nexus or use the account (the page hides them all).
constexpr const char* kNoNexus =
    "This version of EMF doesn't connect to Nexus Mods. Download mods on nexusmods.com (Manual Download), then "
    "install them with Install a downloaded file on EMF's Mods tab.";
// ... and an nxm:// link (EMF still opens them when a testing build took them).
constexpr const char* kNoLinkDownloads =
    "This version of EMF doesn't download from Nexus. On the file's page on Nexus, click Manual Download, then "
    "install the file with Install a downloaded file on EMF's Mods tab.";

// NexusMode::Off: the API clients' transport. The commands are refused before they get here; this is the second
// lock, so no code path can reach Nexus.
nexus::Client::Transport no_nexus_requests() {
    return [](const nexus::HttpRequest&) {
        nexus::HttpResponse r;
        r.failure = nexus::HttpFailure::Network;
        r.error = kNoNexus;
        return r;
    };
}

bool nexus_off(const BridgeHost& host) {
    return nexus_mode(host.sso_slug(), host.personal_key_allowed()) == NexusMode::Off;
}

// Local time as "2026-10-26 20:00".
std::string now_minute() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

std::string download_msg(int id, const char* state, int64_t mod_id, int64_t file_id, const std::string& name,
                         const std::string& file, uint64_t bytes, uint64_t total, const std::string& error,
                         const std::string& page) {
    return Obj()
        .str("type", "download").num("id", id).str("state", state).num("mod_id", static_cast<double>(mod_id))
        .num("file_id", static_cast<double>(file_id)).str("name", name).str("file", file)
        .num("bytes", static_cast<double>(bytes)).num("total", static_cast<double>(total)).str("error", error)
        .str("page", page)
        .text();
}

// *.part files in the downloads folder older than a day: left by an EMF.exe that was closed in the middle of a download.
// A fresh one may belong to a second EMF.exe downloading right now, so it stays.
void remove_stale_parts(const std::wstring& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || it->path().extension() != L".part") continue;
        const auto t = it->last_write_time(e2);
        if (!e2 && now - t > std::chrono::hours(24)) fs::remove(it->path(), e2);
    }
}

bool archive_name_ok(const std::wstring& path) {
    for (const wchar_t* ext : {L".zip", L".7z", L".rar"}) {
        const size_t n = wcslen(ext);
        if (path.size() > n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0) return true;
    }
    return false;
}

}  // namespace

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const size_t n = utf8_length(s, i);
            if (n == 0) {
                out += "\xEF\xBF\xBD";  // U+FFFD
                ++i;
            } else {
                out.append(s, i, n);
                i += n;
            }
            continue;
        }
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
        ++i;
    }
    return out + "\"";
}

std::string state_json(const FolderState& s, const std::string& running_reason) {
    Obj o;
    o.str("type", "state").str("app_version", EMF_VERSION).str("game", utf8(s.game)).boolean("game_found", s.game_found);
    o.boolean("server", s.server).boolean("emf_installed", s.emf_installed).str("emf_version", s.emf_version);
    o.boolean("shroudforge_active", s.shroudforge_active);
    o.boolean("game_running", !running_reason.empty()).str("running_reason", running_reason);
    std::vector<std::string> mods, warnings, installed;
    for (const auto& m : s.mods) mods.push_back(mod_json(m));
    o.raw("mods", json_list(mods));
    for (const auto& w : s.warnings) warnings.push_back(Obj().str("level", w.level).str("text", w.text).text());
    o.raw("warnings", json_list(warnings));
    o.strs("report", s.report);
    for (const auto& m : s.installed) installed.push_back(installed_json(m));
    o.raw("installed", json_list(installed));
    o.raw("profiles", profiles_json(s));
    return o.text();
}

int64_t BridgeHost::now_unix() const { return static_cast<int64_t>(std::time(nullptr)); }


bool Bridge::changing() const { return changing_ || desk_->installing() || saves_working_; }

bool Bridge::server_stopping() const { return desk_->stopping(); }

void Bridge::snapshot_age_for_test(unsigned ms) { desk_->age_snapshot(ms); }

Bridge::Bridge(BridgeHost& host)
    : host_(host), client_(EMF_VERSION, nexus_off(host) ? no_nexus_requests() : host.nexus_transport()),
      v1_(EMF_VERSION, nexus_off(host) ? no_nexus_requests() : host.nexus_v1_transport()),
      desk_(std::make_unique<ServerDesk>(host)) {
    desk_->set_state_source([this] { return state_if_changed(true); });
    desk_->set_busy_source([this] { return busy_; });  // one busy gate with Install EMF into a server
}

Bridge::~Bridge() {
    *alive_ = false;
    if (search_.cancel) *search_.cancel = true;
    if (details_.cancel) *details_.cancel = true;
    if (updates_.cancel) *updates_.cancel = true;
    if (dl_.cancel) *dl_.cancel = true;
    if (sso_.cancel) *sso_.cancel = true;
    drop_pending();
}

void Bridge::drop_pending() {
    if (pending_ && !pending_->plan.folder.empty()) {
        const std::wstring folder = pending_->plan.folder;  // deleting a tree can be slow: off the UI thread
        std::thread([folder] { delete_tree(folder); }).detach();
    }
    pending_.reset();
}

std::string Bridge::result(const std::string& cmd, bool ok, const std::string& error, const std::string& text) {
    return Obj().str("type", "result").str("cmd", cmd).boolean("ok", ok).str("error", error).str("text", text).str("ref", ref_).text();
}

std::string Bridge::state_sig() const {
    std::string s;
    for (const auto& [file, word] : folder_.proxies) s += file + "=" + word + ";";
    s += "chain:";
    for (const auto& w : folder_.chained_tools) s += w + ",";
    s += folder_.emf_installed ? "emf" : "";
    return s;
}

std::optional<std::string> Bridge::state_if_changed(bool force) {
    folder_ = scan_game_folder(host_.game());
    std::string s = state_json(folder_, host_.running_reason());
    s.insert(s.size() - 1, desk_->state_fields(folder_));  // the folder switcher and the Server tab
    s.insert(s.size() - 1, std::string(",\"nexus\":\"") + nexus_mode_name(mode()) + "\"");  // the Nexus features
    s.insert(s.size() - 1, ",\"saves_busy\":" + json_quote(host_.saves_busy()));  // the Saves tab (bridge_saves.cpp)
    if (!force && s == last_state_) return std::nullopt;
    last_state_ = s;
    return s;
}

std::vector<std::string> Bridge::start_plan(const std::wstring& archive, const std::string& nexus_id, const std::string& nexus_name,
                                            const std::string& nexus_file_id, bool from_download) {
    const std::wstring game = host_.game();
    if (!folder_.game_found) return {result("install", false, "Choose the game folder first.")};
    if (!archive_name_ok(archive)) return {result("install", false, "EMF installs .zip, .7z and .rar files.")};
    if (busy_) return {result("install", false, "EMF is still busy with the last install. Wait a moment.")};
    if (desk_->installing()) return {result("install", false, "EMF is being installed into this server. Wait a moment.")};
    busy_ = true;
    PlanInput in;
    in.game = game;
    in.archive = utf8(archive.substr(archive.find_last_of(L"\\/") + 1));
    in.nexus_id = nexus_id;
    in.nexus_name = nexus_name;
    in.nexus_file_id = nexus_file_id;
    in.nexus_certain = from_download;
    in.update_keys = plan_update_keys();  // an install the player linked to this Nexus mod is updated, not doubled
    in.state = game_state_of(folder_);
    in.tools_table = folder_.tools_table;
    in.emf_installed = folder_.emf_installed;
    auto alive = alive_;
    host_.run_async([this, alive, in, archive, from_download]() mutable -> BridgeHost::Done {
        clean_old_unpack_folders();
        PlanInput input = std::move(in);
        std::optional<InstallPlan> plan;
        std::string error;
        input.folder = new_unpack_folder();
        if (input.folder.empty()) {
            error = "Could not make a folder for unpacking in %TEMP%.";
        } else {
            try {  // whatever happens, the page hears back (and busy_ is reset)
                const Unpacked u = unpack_archive(system_tar(), archive, input.folder);
                if (!u.ok) {
                    error = u.error;
                } else {
                    input.files = u.files;
                    plan = build_plan(input);
                }
            } catch (const std::exception& e) {
                plan.reset();
                error = std::string("EMF could not read the archive: ") + e.what();
            }
            if (!plan) delete_tree(input.folder);
        }
        return [this, alive, plan = std::move(plan), error, from_download]() mutable -> std::vector<std::string> {
            if (!*alive) {
                if (plan) delete_tree(plan->folder);
                return {};
            }
            busy_ = false;
            if (!plan)
                return {Obj().str("type", "install_plan").boolean("ok", false).str("error", error).raw("plan", "null")
                            .boolean("from_download", from_download).text()};
            drop_pending();
            const std::string id = "p" + std::to_string(++plans_);
            const std::string json = plan_json(*plan, id, host_.running_reason());
            pending_ = Pending{id, std::move(*plan)};
            return {Obj().str("type", "install_plan").boolean("ok", true).str("error", "").raw("plan", json)
                        .boolean("from_download", from_download).text()};
        };
    });
    return {Obj().str("type", "install_busy").str("archive", in.archive).boolean("from_download", from_download).text()};
}

// ---- the Nexus account and downloads ----

std::wstring Bridge::key_path() const {
    const std::wstring d = host_.app_data();
    return d.empty() ? std::wstring() : d + L"\\nexus.key";
}

std::wstring Bridge::downloads_dir() const {
    const std::wstring d = host_.app_data();
    return d.empty() ? std::wstring() : d + L"\\downloads";
}

void Bridge::ensure_account() {
    if (mode() == NexusMode::Off) return;  // a key a testing build stored is neither read nor used (nor deleted)
    if (account_loaded_) return;
    account_loaded_ = true;
    const std::wstring path = key_path();
    if (path.empty()) return;
    const AccountFile f = read_account_file(path);
    account_ = f.account;
    if (f.status == AccountFile::Status::OtherUser)
        account_note_ = "The saved key belongs to another Windows user, so EMF cannot read it. Paste your own key.";
    else if (f.status == AccountFile::Status::Unreadable)
        account_note_ = "The saved key could not be read. Paste it again.";
}

bool Bridge::rate_blocked() {
    if (!limits_.exhausted()) return false;
    if (host_.now_unix() < limits_until_) return true;
    limits_ = {};  // the reset time has passed: EMF may ask again (the next reply says how many are left)
    return false;
}

void Bridge::note_limits(const nexus::RateLimits& l) {
    if (!l.known) return;
    limits_ = l;
    if (l.exhausted()) {
        int64_t at = nexus::iso8601_to_unix(l.hourly_reset);
        if (at <= 0) at = nexus::iso8601_to_unix(l.daily_reset);
        const int64_t now = host_.now_unix();
        limits_until_ = at > now ? at : now + 300;  // no reset time, or one already past: try again in five minutes
    }
}

NexusMode nexus_mode(const std::string& slug, bool personal_key_switch) {
    if (!slug.empty()) return NexusMode::Sso;
    return personal_key_switch ? NexusMode::PersonalKey : NexusMode::Off;
}

const char* nexus_mode_name(NexusMode mode) {
    switch (mode) {
    case NexusMode::PersonalKey: return "personal_key";
    case NexusMode::Sso: return "sso";
    case NexusMode::Off: break;
    }
    return "off";
}

NexusMode Bridge::mode() const { return nexus_mode(host_.sso_slug(), host_.personal_key_allowed()); }

bool Bridge::personal_key() const { return host_.personal_key_allowed(); }

std::string Bridge::ready_json() const {
    const Ready* r = ready_.empty() ? nullptr : &ready_.front();
    return Obj().str("type", "download_ready").num("id", r ? r->id : 0).str("name", r ? r->name : "").str("file", r ? r->file : "")
        .num("count", static_cast<double>(ready_.size())).text();
}

std::string Bridge::account_json() {
    ensure_account();
    rate_blocked();  // a used-up allowance whose reset time has passed is shown as cleared
    const NexusMode nm = mode();
    const bool off = nm == NexusMode::Off;
    const StoredAccount* acct = !off && account_ ? &*account_ : nullptr;  // off: never a key, whatever is stored
    Obj o;
    o.str("type", "account").str("mode", nexus_mode_name(nm)).boolean("key_set", acct != nullptr)
        .boolean("checking", checking_);
    if (acct && !acct->name.empty())
        o.raw("user", Obj().str("name", acct->name).boolean("premium", acct->premium)
                          .boolean("supporter", acct->supporter).str("checked_at", acct->checked_at).text());
    else
        o.raw("user", "null");
    o.str("error", account_error_).str("note", account_note_);
    o.raw("sso", Obj().boolean("available", !host_.sso_slug().empty()).boolean("running", sso_.running).text());
    o.boolean("personal_key", personal_key());
    if (RegistryLayer* reg = host_.registry()) {
        // without an account the setting shows only while this EMF.exe has the links (a testing build took them), so
        // the player can give them back
        const NxmOwner w = nxm_owner(*reg, host_.exe_path());
        o.raw("nxm", Obj().boolean("available", !off || w.ours).str("owner", w.label).str("program", utf8(w.program))
                         .boolean("ours", w.ours).boolean("emf", w.emf).boolean("machine", w.machine)
                         .str("previous", nxm_previous_label(*reg)).text());
    } else {
        o.raw("nxm", Obj().boolean("available", false).text());
    }
    o.raw("rate", Obj().boolean("known", limits_.known).num("daily", static_cast<double>(limits_.daily_remaining))
                      .num("hourly", static_cast<double>(limits_.hourly_remaining))
                      .num("daily_limit", static_cast<double>(limits_.daily_limit))
                      .num("hourly_limit", static_cast<double>(limits_.hourly_limit))
                      .str("daily_reset", limits_.daily_reset).str("hourly_reset", limits_.hourly_reset)
                      .boolean("exhausted", limits_.exhausted())
                      .str("message", limits_.exhausted() ? nexus::rate_limit_message(limits_) : std::string())
                      .text());
    if (waiting_)
        o.raw("waiting", Obj().num("mod_id", static_cast<double>(waiting_->mod_id))
                             .num("file_id", static_cast<double>(waiting_->file_id)).text());
    else
        o.raw("waiting", "null");
    o.str("downloads", utf8(downloads_dir()));
    return o.text();
}

std::string Bridge::download_json(const char* state, const std::string& error, const std::string& page) const {
    return download_msg(dl_.id, state, dl_.mod_id, dl_.file_id, dl_.name, dl_.file, dl_.bytes, dl_.total, error, page);
}

std::vector<std::string> Bridge::check_key(const std::string& key, const std::string& cmd) {
    if (checking_) return {result(cmd, false, "EMF is already checking a key with Nexus. Wait a moment.")};
    if (!nexus::api_key_text_ok(key))
        return {result(cmd, false, "That does not look like a Nexus API key: copy the whole key from "
                                   "nexusmods.com/settings/api-keys, under \"Personal API Key\".")};
    if (rate_blocked()) return {result(cmd, false, nexus::rate_limit_message(limits_))};
    checking_ = true;
    account_error_.clear();
    auto alive = alive_;
    const nexus::V1Client v1 = v1_;
    const int gen = account_gen_;
    host_.run_async([this, alive, key, cmd, v1, gen]() -> BridgeHost::Done {
        const auto r = v1.validate(key);
        return [this, alive, key, cmd, r, gen]() -> std::vector<std::string> {
            if (!*alive || gen != account_gen_) return {};  // the player logged out meanwhile: the key stays out
            checking_ = false;
            note_limits(r.limits);
            std::vector<std::string> out;
            if (!r.ok()) {
                account_error_ = r.error;
                out.push_back(result(cmd, false, r.error));
                out.push_back(account_json());
                return out;
            }
            StoredAccount a;
            a.key = key;
            a.user_id = r.value.user_id;
            a.name = r.value.name;
            a.premium = r.value.premium;
            a.supporter = r.value.supporter;
            a.checked_at = now_minute();
            account_note_.clear();
            const std::wstring path = key_path();
            if (path.empty() || !save_account(path, a))
                account_note_ = "EMF could not save the key on this PC, so it is used only until EMF closes.";
            account_ = a;
            account_loaded_ = true;
            const std::optional<DownloadAsk> resume = waiting_;  // a Mod Manager Download link that waited for the key
            waiting_.reset();
            out.push_back(result(cmd, true, "", "Logged in to Nexus as " + a.name + (a.premium ? " (Premium)." : ".")));
            out.push_back(account_json());
            if (resume)
                for (auto& m : start_download(*resume)) out.push_back(std::move(m));
            return out;
        };
    });
    return {account_json()};
}

std::vector<std::string> Bridge::sso_start() {
    const std::string slug = host_.sso_slug();
    if (slug.empty())
        return {result("sso_start", false, mode() == NexusMode::Off
                                               ? kNoNexus
                                               : "Log in with Nexus needs a build of EMF with a Nexus application slug. Use a personal API key.")};
    if (sso_.running) return {account_json()};
    sso_.running = true;
    sso_.cancel = std::make_shared<std::atomic<bool>>(false);
    account_error_.clear();
    auto alive = alive_;
    auto cancel = sso_.cancel;
    nexus::SsoSocketFactory sockets = host_.sso_sockets();
    if (!sockets) sockets = nexus::winhttp_sso_sockets(std::string("EMF/") + EMF_VERSION + " (Enshrouded Modding Framework; Windows)");
    BridgeHost* host = &host_;
    const int gen = account_gen_;
    const auto poster = host_.ui_poster();  // safe from the worker even after the window is gone
    host_.run_async([this, alive, cancel, slug, sockets, host, gen, poster]() -> BridgeHost::Done {
        nexus::SsoOptions o;
        o.id = nexus::new_sso_id();
        o.slug = slug;
        o.cancel = cancel.get();
        o.open_browser = [host, alive, poster](const std::string& url) {  // from this worker: the browser opens on the UI
            poster([host, alive, url]() -> std::vector<std::string> {        // thread, and only while the bridge lives
                if (*alive && nexus_url_allowed(url)) host->open(wide(url), BridgeHost::Open::Url);
                return {};
            });
        };
        const nexus::SsoResult r = nexus::run_sso(sockets, o);
        return [this, alive, r, gen]() -> std::vector<std::string> {
            if (!*alive) return {};
            sso_.running = false;
            if (gen != account_gen_) return {account_json()};  // logged out meanwhile
            if (!r.ok) {
                if (!r.cancelled) account_error_ = r.error;
                return {account_json()};
            }
            return check_key(r.api_key, "sso_start");
        };
    });
    return {account_json()};
}

std::vector<std::string> Bridge::start_download(const DownloadAsk& ask) {
    const auto fail = [&](const std::string& why, const std::string& page = {}) {
        return std::vector<std::string>{download_msg(0, "failed", ask.mod_id, ask.file_id, ask.name, "", 0, 0, why, page)};
    };
    if (mode() == NexusMode::Off) return fail(kNoNexus);
    if (dl_.running)
        return fail("EMF is downloading " + (dl_.file.empty() ? (dl_.name.empty() ? std::string("a file") : dl_.name) : dl_.file) +
                    " now. Wait for it to finish, or cancel it, then try again.");
    ensure_account();
    if (!account_) {
        if (ask.from_link && waiting_)
            return fail("A Mod Manager Download link is already waiting for your key. Set the key, then click Mod Manager "
                        "Download on this file again.");
        if (ask.from_link) {
            waiting_ = ask;
            return {download_msg(0, "need_key", ask.mod_id, ask.file_id, ask.name, "", 0, 0,
                                 "Set your Nexus API key first (the EMF tab, Nexus account): EMF downloads the file as soon as the key works.", ""),
                    account_json()};
        }
        return fail("Set your Nexus API key first: the EMF tab, Nexus account.");
    }
    if (ask.user_id > 0 && account_->user_id > 0 && ask.user_id != account_->user_id)
        return fail("This download link was made for another Nexus account than the one EMF uses (" + account_->name +
                    "). Click Mod Manager Download again while logged in to Nexus as " + account_->name +
                    ", or set the other account's key in EMF.");
    // the PC's clock may be off by a few minutes: within that, Nexus's own reply decides
    if (ask.expires > 0 && ask.expires + kExpiryGrace <= host_.now_unix())
        return fail("This download link has expired. Click Mod Manager Download on Nexus again.",
                    nexus::nexus_file_page(ask.mod_id, ask.file_id));
    if (ask.nxm_key.empty() && !account_->premium)
        return fail("Nexus gives downloads through its API to Premium members only. On the file's page on Nexus, click "
                    "Mod Manager Download, and EMF downloads it from there.",
                    nexus::nexus_file_page(ask.mod_id, ask.file_id));
    if (rate_blocked()) return fail(nexus::rate_limit_message(limits_));
    const std::wstring dir = downloads_dir();
    if (dir.empty()) return fail("EMF has no folder to download into.");

    dl_.running = true;
    ++dl_.id;
    dl_.cancel = std::make_shared<std::atomic<bool>>(false);
    dl_.mod_id = ask.mod_id;
    dl_.file_id = ask.file_id;
    dl_.name = ask.name;
    dl_.file.clear();
    dl_.bytes = dl_.total = 0;

    nexus::DownloadJob job;
    job.mod_id = ask.mod_id;
    job.file_id = ask.file_id;
    job.api_key = account_->key;
    job.nxm_key = ask.nxm_key;
    job.nxm_expires = ask.expires;
    job.dir = dir;
    job.cancel = dl_.cancel.get();
    auto alive = alive_;
    const int id = dl_.id;
    const auto poster = host_.ui_poster();  // safe from the worker even after the window is gone
    auto last = std::make_shared<std::atomic<unsigned long long>>(0);  // when the last progress message went out
    job.progress = [this, alive, id, poster, last](uint64_t done, uint64_t total) {
        const unsigned long long now = GetTickCount64();
        if (done != total && now - last->load() < 200) return;  // at most five a second
        last->store(now);
        poster([this, alive, id, done, total]() -> std::vector<std::string> {  // runs on the UI thread
            if (!*alive || id != dl_.id || !dl_.running) return {};
            dl_.bytes = done;
            dl_.total = total;
            return {download_json("running")};
        });
    };
    const nexus::Downloader downloader(EMF_VERSION, host_.nexus_v1_transport(), host_.nexus_fetch());
    auto cancel = dl_.cancel;  // keeps the flag alive while the worker runs
    host_.run_async([this, alive, id, job, downloader, cancel, ask]() -> BridgeHost::Done {
        remove_stale_parts(job.dir);
        const nexus::DownloadOutcome r = downloader.run(job);
        return [this, alive, id, r, ask]() -> std::vector<std::string> {
            if (!*alive || id != dl_.id) return {};
            dl_.running = false;
            note_limits(r.limits);
            dl_.file = r.file_name;
            std::vector<std::string> out;
            if (r.cancelled) {
                out.push_back(download_json("cancelled"));
                out.push_back(account_json());
                return out;
            }
            if (!r.ok) {
                const bool page = (r.api_status == 403 && ask.nxm_key.empty()) || ask.from_link;
                out.push_back(download_json("failed", r.error, page ? nexus::nexus_file_page(ask.mod_id, ask.file_id) : ""));
                out.push_back(account_json());
                return out;
            }
            dl_.bytes = dl_.total = r.bytes;
            if (dl_.name.empty()) dl_.name = r.title;
            out.push_back(download_json("done"));
            out.push_back(account_json());
            if (pending_ || busy_) {  // a plan is open (or EMF is busy): this one waits, the page offers it
                ready_.push_back(Ready{++ready_ids_, r.path, ask.mod_id, ask.file_id, dl_.name, r.file_name});
                out.push_back(ready_json());
                return out;
            }
            // the same plan as "Install a downloaded file", knowing the Nexus mod and file for certain
            for (auto& m : start_plan(r.path, std::to_string(ask.mod_id), dl_.name, std::to_string(ask.file_id), true))
                out.push_back(std::move(m));
            return out;
        };
    });
    return {download_json("running")};
}

std::vector<std::string> Bridge::handle_nxm(const std::string& url) {
    const nexus::NxmParse p = nexus::parse_nxm(url);
    if (!p.link) {
        std::string why = p.error;
        if (p.other_game)
            if (RegistryLayer* reg = host_.registry()) {
                const std::string previous = nxm_previous_label(*reg);
                if (!previous.empty() && previous != "nothing")
                    why += " " + previous + " opened these links before EMF: give them back to it on the EMF tab (Mod Manager "
                           "Download links), then click Mod Manager Download again.";
            }
        return {download_msg(0, "failed", 0, 0, "", "", 0, 0, why, "")};
    }
    if (mode() == NexusMode::Off) {  // EMF still opens the links when a testing build took them
        std::string why = kNoLinkDownloads;
        if (RegistryLayer* reg = host_.registry())
            if (nxm_owner(*reg, host_.exe_path()).ours) {
                const std::string previous = nxm_previous_label(*reg);
                why += previous.empty() || previous == "nothing"
                           ? " EMF opens these links because a testing version took them: to stop that, click Stop "
                             "opening them on the EMF tab (Mod Manager Download links)."
                           : " To make the button open " + previous + " again, give them back to " + previous +
                                 " on the EMF tab (Mod Manager Download links).";
            }
        return {download_msg(0, "failed", p.link->mod_id, p.link->file_id, "", "", 0, 0, why,
                             nexus::nexus_file_page(p.link->mod_id, p.link->file_id))};
    }
    DownloadAsk ask;
    ask.mod_id = p.link->mod_id;
    ask.file_id = p.link->file_id;
    ask.nxm_key = p.link->key;
    ask.expires = p.link->expires;
    ask.user_id = p.link->user_id;
    ask.from_link = true;
    return start_download(ask);
}

std::vector<std::string> Bridge::handle(const std::string& message) {
    auto parsed = core::json::parse(message);
    ref_.clear();
    if (!parsed.value || !parsed.value->is(VT::Object)) return {result("", false, "The app could not read a message from its page.")};
    const Value& m = *parsed.value;
    ref_ = text_of(m, "ref");
    const std::string cmd = text_of(m, "cmd");
    const std::wstring game = host_.game();
    // after a change: the result, then the fresh state
    auto with_state = [&](std::string r) {
        std::vector<std::string> out{std::move(r)};
        if (auto s = state_if_changed(true)) out.push_back(*s);
        return out;
    };

    if (cmd == "state") {
        page_ready_ = true;
        saves_first_start();  // the starter backup at EMF.exe's first start (bridge_saves.cpp)
        std::vector<std::string> out{*state_if_changed(true)};
        const std::vector<std::string> links = std::move(startup_nxm_);  // the links that came before the page listened
        startup_nxm_.clear();
        for (const auto& url : links)
            for (auto& x : handle_nxm(url)) out.push_back(std::move(x));
        return out;
    }
    if (cmd == "set_enabled") {
        const std::string reason = host_.running_reason();
        if (!reason.empty())
            return with_state(result(cmd, false, "Mods are switched while the game is closed: close the game first (" + reason + ")."));
        const Value* on = m.get("on");
        const Change c = set_mod_enabled(game, text_of(m, "source"), text_of(m, "id"), on && on->is(VT::Bool) && on->b);
        return with_state(result(cmd, c.ok, c.error, c.ok ? "Takes effect at the next game start." : ""));
    }
    if (cmd == "set_setting") {
        const Change c = set_mod_setting(game, text_of(m, "id"), text_of(m, "key"), value_text(m.get("value")));
        std::string text = "Saved.";
        if (!host_.running_reason().empty())
            text += " The running game uses it from its next start (the in-game Mods tab changes settings at once).";
        return with_state(result(cmd, c.ok, c.error, c.ok ? text : ""));
    }
    if (auto out = handle_profiles(cmd, m)) return *out;  // mod profiles (bridge_profiles.cpp)
    if (cmd == "play") {
        if (!game.empty() && !exists(game + L"\\enshrouded.exe") && exists(game + L"\\enshrouded_server.exe"))
            return {result(cmd, false, "This is a dedicated server folder: start the server with enshrouded_server.exe.")};
        const bool ok = host_.open(L"steam://rungameid/1203620", BridgeHost::Open::Url);
        std::string text = ok ? "Starting Enshrouded through Steam..." : "";
        // Steam starts its own copy of the game: say so when that is not the folder shown
        const std::wstring steam = host_.steam_game();
        if (ok && !steam.empty() && !game.empty() && !(path_inside(steam, game) && path_inside(game, steam)))
            text += " Play starts the game in " + utf8(steam) + "; this folder is not it.";
        return {result(cmd, ok, ok ? "" : "Steam did not start the game. Is Steam installed?", text)};
    }
    if (cmd == "copy") {
        const std::string text = text_of(m, "text");
        if (text.size() > (1u << 20)) return {result(cmd, false, "That is too much text to copy.")};
        const bool ok = host_.copy_text(text);
        return {result(cmd, ok, ok ? "" : "Could not put the text on the clipboard.", ok ? "Copied." : "")};
    }
    if (cmd == "pick_folder") {
        const auto dir = host_.pick_folder();
        if (!dir) return {result(cmd, true, "")};
        if (!is_game_folder(*dir))
            return {result(cmd, false, "There is no enshrouded.exe or enshrouded_server.exe in " + utf8(*dir) +
                                           ". Choose the folder the game (or a dedicated server) is installed in.")};
        drop_pending();  // a plan built for the old folder must not be applied to the new one
        updates_stop("");  // and a check of the old folder is no answer for the new one (the page drops it too)
        host_.choose_game(*dir);
        return with_state(result(cmd, true, "", "Showing " + utf8(*dir)));
    }
    if (cmd == "open_url") {
        // the app's own buttons: nexusmods.com only; a link the player clicked in a mod's description: any https link
        std::string url = text_of(m, "url");
        const Value* external = m.get("external");
        const bool from_description = external && external->is(VT::Bool) && external->b;
        if (from_description && url.rfind("http://", 0) == 0) url = "https://" + url.substr(7);  // open it securely
        if (!(from_description ? web_url_allowed(url) : nexus_url_allowed(url)))
            return {result(cmd, false, from_description ? "Only https:// links open from here." : "Only links to nexusmods.com open from here.")};
        const bool ok = host_.open(wide(url), BridgeHost::Open::Url);
        return {result(cmd, ok, ok ? "" : "The browser did not open.")};
    }
    if (cmd == "open_folder") {
        const std::wstring path = wide(text_of(m, "path"));
        if (game.empty() || !path_inside(game, path)) return {result(cmd, false, "Only folders inside the game folder open from here.")};
        if (!exists(path)) return {result(cmd, false, utf8(path) + " is not there any more.")};
        const bool ok = host_.open(path, is_dir(path) ? BridgeHost::Open::Folder : BridgeHost::Open::SelectFile);
        return {result(cmd, ok, ok ? "" : "Explorer did not open.")};
    }
    if (cmd == "logs") {
        std::string core, loader;
        if (folder_.server) {  // a dedicated server's: every password seen this session is stripped (server_bridge.h)
            core = desk_->server_log_text(game, game + L"\\emf\\emf_core.log");
            loader = desk_->server_log_text(game, game + L"\\emf\\emf_loader.log");
        } else {
            core = log_tail(game + L"\\emf\\emf_core.log");
            loader = log_tail(game + L"\\emf\\emf_loader.log");
        }
        return {Obj().str("type", "logs").str("core", core).str("loader", loader).text()};
    }
    if (cmd == "rendered") {
        const Value* count = m.get("count");
        std::vector<std::string> titles;
        if (const Value* t = m.get("titles"); t && t->is(VT::Array))
            for (const Value& x : t->arr)
                if (x.is(VT::String)) titles.push_back(x.s);
        host_.rendered(count && count->is(VT::Number) ? static_cast<int>(count->n) : 0, titles);
        return {};
    }
    // ---- the folder switcher and dedicated servers (server_bridge.h) ----
    if (ServerDesk::handles(cmd)) {
        ServerDesk::Reply r = desk_->handle(cmd, m, folder_, ref_);
        if (r.switch_to) {
            drop_pending();    // a plan built for the old folder must not be applied to the new one
            updates_stop("");  // and a check of the old folder is no answer for the new one
            host_.choose_game(*r.switch_to);
        }
        std::vector<std::string> out = std::move(r.messages);
        if (r.state)
            if (auto s = state_if_changed(true)) out.push_back(*s);
        return out;
    }

    // ---- no Nexus in this build (NexusMode::Off): the page hides these, and a page script gets no further ----
    if (cmd == "nexus_search" || cmd == "nexus_mod" || cmd == "nexus_file" || cmd == "updates_check" ||
        cmd == "account_key" || cmd == "account_check" || cmd == "account_logout" || cmd == "nxm_take") {
        // (a stored key stays; nexus_download is refused by start_download, with the download message the page shows;
        // sso_start by sso_start)
        if (mode() == NexusMode::Off) return {result(cmd, false, kNoNexus)};
    }

    // ---- Browse: Nexus requests on a worker thread ----
    if (cmd == "nexus_search") {
        nexus::SearchQuery q;
        q.text = text_of(m, "text");
        q.sort = nexus::sort_from_name(text_of(m, "sort")).value_or(nexus::SortBy::Downloads);
        if (q.sort != nexus::SortBy::Endorsements && q.sort != nexus::SortBy::Updated) q.sort = nexus::SortBy::Downloads;
        q.count = 20;
        const int64_t offset = int_of(m, "offset");
        q.offset = static_cast<int>(offset < 0 ? 0 : offset > 100000 ? 100000 : offset);
        const int seq = static_cast<int>(int_of(m, "seq"));
        search_.renew();
        const int id = search_.id;
        auto alive = alive_;
        auto cancel = search_.cancel;
        const nexus::Client client = client_;
        host_.run_async([this, alive, cancel, id, seq, q, client]() -> BridgeHost::Done {
            std::string reply;
            if (!*cancel) reply = search_reply(seq, q.offset, client.search(q, cancel.get()));
            return [this, alive, id, reply]() -> std::vector<std::string> {
                if (!*alive || id != search_.id || reply.empty()) return {};  // a newer search replaced it
                return {reply};
            };
        });
        return {};
    }
    if (cmd == "nexus_mod") {
        const int64_t mod_id = int_of(m, "id");
        const int seq = static_cast<int>(int_of(m, "seq"));
        if (mod_id <= 0) return {result(cmd, false, "That is not a Nexus mod.")};
        details_.renew();
        const std::string sig = state_sig();
        if (auto it = mod_cache_.find(mod_id); it != mod_cache_.end() && it->second.first == sig)
            return {restamp_seq(it->second.second, seq)};  // seen this session, folder unchanged: no network
        const int id = details_.id;
        auto alive = alive_;
        auto cancel = details_.cancel;
        const nexus::Client client = client_;
        const nexus::GameState g = game_state_of(folder_);
        host_.run_async([this, alive, cancel, id, seq, mod_id, client, g, sig]() -> BridgeHost::Done {
            std::string reply;
            bool cacheable = false;
            if (!*cancel) {
                const auto mod = client.mod(mod_id, cancel.get());
                nexus::Result<std::vector<nexus::NexusFile>> files;
                std::vector<FileFacts> facts;
                if (mod.ok() && !*cancel) {
                    files = client.files(mod_id, cancel.get());
                    size_t asked = 0;
                    for (const auto& f : order_files(files.value)) {
                        FileFacts ff;
                        ff.file = f;
                        if (contents_at_once(f.category) && asked < kMaxContentsAtOnce && !*cancel) {
                            ff.checked = true;
                            ff.contents = client.contents(mod_id, f.file_id, 2000, cancel.get());
                            ++asked;
                        }
                        facts.push_back(std::move(ff));
                    }
                }
                if (!*cancel) {
                    reply = mod_reply(seq, mod, files, facts, g);
                    cacheable = mod.ok() && files.ok();  // a full page, worth keeping for the session
                }
            }
            return [this, alive, id, mod_id, sig, reply, cacheable]() -> std::vector<std::string> {
                if (!*alive || id != details_.id || reply.empty()) return {};  // the player opened another mod
                if (cacheable) mod_cache_[mod_id] = {sig, reply};
                return {reply};
            };
        });
        return {};
    }
    if (cmd == "nexus_file") {
        const int64_t mod_id = int_of(m, "mod_id"), file_id = int_of(m, "file_id");
        const int seq = static_cast<int>(int_of(m, "seq"));
        if (mod_id <= 0 || file_id <= 0) return {result(cmd, false, "That is not a Nexus file.")};
        auto alive = alive_;
        const nexus::Client client = client_;
        const nexus::GameState g = game_state_of(folder_);
        host_.run_async([alive, seq, mod_id, file_id, client, g]() -> BridgeHost::Done {
            FileFacts ff;
            ff.file.file_id = file_id;
            ff.checked = true;
            ff.contents = client.contents(mod_id, file_id);
            const std::string reply = file_reply(seq, mod_id, ff, g);
            return [alive, reply]() -> std::vector<std::string> {
                if (!*alive) return {};
                return {reply};
            };
        });
        return {};
    }

    // ---- installing a downloaded archive ----
    if (cmd == "install_pick" || cmd == "install_path") {
        std::optional<std::wstring> path;
        if (cmd == "install_path") {
            if (!host_.test_mode()) return {result(cmd, false, "The app does not know the command \"" + cmd + "\".")};
            path = wide(text_of(m, "path"));
        } else {
            if (busy_ || desk_->installing()) return {result(cmd, false, "EMF is still busy with the last install. Wait a moment.")};
            path = host_.pick_archive();
            if (!path) return {};  // cancelled
        }
        return start_plan(*path, nexus_id_of(m, "nexus_id"), text_of(m, "nexus_name"));
    }
    if (cmd == "install_cancel") {
        if (pending_ && pending_->id == text_of(m, "plan_id")) drop_pending();
        return {};
    }
    if (cmd == "install_apply" || cmd == "uninstall_mod") {
        const bool install = cmd == "install_apply";
        const char* type = install ? "install_done" : "uninstall_done";
        const std::string key = install ? (pending_ ? pending_->plan.key : "") : text_of(m, "key");
        const auto refuse = [&](const std::string& why) {
            InstallResult r;
            r.error = why;
            return std::vector<std::string>{done_json(type, r, key)};
        };
        if (install && (!pending_ || pending_->id != text_of(m, "plan_id")))
            return refuse("That plan is gone. Choose the file again.");
        if (!install && !valid_install_key(key)) return refuse("EMF has no record of that install.");
        if (busy_ || desk_->installing()) return refuse("EMF is still busy with the last install. Wait a moment.");
        const std::string reason = host_.running_reason();
        if (!reason.empty())
            return refuse(std::string(install ? "Mods are installed" : "Mods are uninstalled") +
                          " while the game is closed: close the game first (" + reason + ").");
        if (game.empty() || !folder_.game_found) return refuse("Choose the game folder first.");
        busy_ = true;
        changing_ = true;
        std::optional<InstallPlan> plan;
        if (install) {
            plan = std::move(pending_->plan);
            pending_.reset();
        }
        auto alive = alive_;
        host_.run_async([this, alive, install, type, key, game, plan]() -> BridgeHost::Done {
            InstallResult r;
            try {  // whatever happens, the page hears back (and busy_ is reset)
                r = install ? apply_plan(game, *plan) : uninstall_mod(game, key);
            } catch (const std::exception& e) {
                r.ok = false;
                r.error = std::string("EMF stopped with an error: ") + e.what();
            }
            if (install) delete_tree(plan->folder);
            const std::string reply = done_json(type, r, key);
            return [this, alive, reply]() -> std::vector<std::string> {
                if (!*alive) return {};
                busy_ = false;
                changing_ = false;
                std::vector<std::string> out{reply};
                for (auto& u : updates_stop("You installed or uninstalled a mod while EMF was checking, so the answer would be out of date. Check again."))
                    out.push_back(std::move(u));
                if (auto s = state_if_changed(true)) out.push_back(*s);
                return out;
            };
        });
        return {};
    }

    // ---- the Nexus account and downloads ----
    if (cmd == "account") return {account_json()};
    if (cmd == "account_key") {
        if (!personal_key())
            return {result(cmd, false, "EMF is registered with Nexus: use Log in with Nexus. Personal API keys are for "
                                       "testing only.")};
        std::string key = text_of(m, "key");
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r' || key.back() == '\n')) key.pop_back();
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
        return check_key(key, cmd);
    }
    if (cmd == "account_check") {
        ensure_account();
        if (!account_) return {result(cmd, false, "There is no key to check: set one first.")};
        return check_key(account_->key, cmd);
    }
    if (cmd == "account_logout") {
        if (sso_.cancel) *sso_.cancel = true;
        const std::wstring path = key_path();
        const bool removed = path.empty() || delete_account(path);
        if (dl_.running && dl_.cancel) *dl_.cancel = true;  // a download that uses the key stops too
        ++account_gen_;  // a key check under way now is dropped when it answers
        checking_ = false;
        account_.reset();
        account_loaded_ = true;
        account_error_.clear();
        account_note_.clear();
        waiting_.reset();
        return {result(cmd, removed, removed ? "" : "EMF could not remove " + utf8(path) + ": delete it by hand.",
                       removed ? "Logged out. EMF removed your key from this PC." : ""),
                account_json()};
    }
    if (cmd == "sso_start") return sso_start();  // refused without a slug
    if (cmd == "sso_cancel") {
        if (sso_.cancel) *sso_.cancel = true;
        return {};
    }
    if (cmd == "nxm_take" || cmd == "nxm_give_back") {
        RegistryLayer* reg = host_.registry();
        if (!reg) return {result(cmd, false, "This EMF.exe cannot change which program opens nxm:// links.")};
        const NxmChange ch = cmd == "nxm_take" ? nxm_take(*reg, host_.exe_path()) : nxm_give_back(*reg, host_.exe_path());
        return {result(cmd, ch.ok, ch.error, ch.text), account_json()};
    }
    if (cmd == "nexus_download") {
        DownloadAsk ask;
        ask.mod_id = int_of(m, "mod_id");
        ask.file_id = int_of(m, "file_id");
        ask.name = text_of(m, "name");
        if (ask.mod_id <= 0 || ask.file_id <= 0 || ask.mod_id > 999999999999LL || ask.file_id > 999999999999LL)
            return {result(cmd, false, "That is not a Nexus file.")};
        return start_download(ask);
    }
    if (cmd == "download_plan") {
        const int64_t id = int_of(m, "id");
        const auto it = std::find_if(ready_.begin(), ready_.end(), [&](const Ready& r) { return r.id == id; });
        if (it == ready_.end()) return {result(cmd, false, "That download is not waiting any more."), ready_json()};
        if (pending_) return {result(cmd, false, "Install or cancel the plan that is open first.")};
        if (busy_) return {result(cmd, false, "EMF is still busy with the last install. Wait a moment.")};
        const Ready r = *it;
        ready_.erase(it);
        std::vector<std::string> out = start_plan(r.path, std::to_string(r.mod_id), r.name, std::to_string(r.file_id), true);
        out.push_back(ready_json());
        return out;
    }
    if (cmd == "download_cancel") {
        if (dl_.running && dl_.cancel) *dl_.cancel = true;
        return {};
    }
    if (cmd == "open_downloads") {
        const std::wstring dir = downloads_dir();
        if (dir.empty()) return {result(cmd, false, "EMF has no downloads folder.")};
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        const bool ok = host_.open(dir, BridgeHost::Open::Folder);
        return {result(cmd, ok, ok ? "" : "Explorer did not open.")};
    }
    if (auto out = handle_updates(cmd, m)) return *out;
    if (auto out = handle_saves(cmd, m)) return *out;  // save backups (bridge_saves.cpp)
    return {result(cmd, false, "The app does not know the command \"" + cmd + "\".")};
}

}  // namespace emf::app
