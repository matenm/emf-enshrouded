#pragma once
// The page <-> host bridge of EMF.exe: JSON messages from the page (window.chrome.webview.postMessage) become actions,
// and the state goes back as JSON (PostWebMessageAsJson). Everything that touches the desktop (the folder picker, the
// file dialog, the browser, Explorer, the process list, worker threads) goes through BridgeHost, so the bridge is tested
// without a window.
//
// Page -> host: {cmd: "state" | "set_enabled" {source,id,on} | "set_setting" {id,key,value} | "play" | "pick_folder"
//                | "open_url" {url, external} | "open_folder" {path} | "logs" | "copy" {text} | "rendered" {count,titles}
//                | "nexus_search" {text, sort, offset, seq} | "nexus_mod" {id, seq} | "nexus_file" {mod_id, file_id, seq}
//                | "install_pick" {nexus_id, nexus_name} | "install_path" {path, ...} (tests only)
//                | "install_apply" {plan_id} | "install_cancel" {plan_id} | "uninstall_mod" {key}
//                | "account" | "account_key" {key} | "account_check" | "account_logout" | "sso_start" | "sso_cancel"
//                | "nxm_take" | "nxm_give_back" | "nexus_download" {mod_id, file_id, name} | "download_cancel"
//                | "download_plan" {id} | "open_downloads"
//                | "updates_check" {seq} | "nexus_links" | "nexus_link" {source, id, link} (bridge_updates.cpp)
//                | "folder_*" and "server_*": the folder switcher and dedicated servers (server_bridge.h)}
// Host -> page: {type: "state", ...}, {type: "result", cmd, ok, error, text, ref}, {type: "logs", core, loader},
//               {type: "nexus_search" | "nexus_mod" | "nexus_file", seq, ...} (browse.h),
//               {type: "install_busy", archive}, {type: "install_plan", ok, error, plan} (install.h plan_json),
//               {type: "install_done" | "uninstall_done", ok, error, lines, key},
//               {type: "account", mode, key_set, checking, user, error, note, sso, personal_key, nxm, rate, waiting,
//                downloads} (account_json; mode: nexus_mode_name, as the state's "nexus"),
//               {type: "download", id, state, mod_id, file_id, name, file, bytes, total, error, page}
//                 (state: "running", "done", "failed", "cancelled", "need_key"),
//               {type: "download_ready", id, name, file, count}: a finished download whose plan waits (a plan was open)
//               {type: "updates", ...} (updates.h updates_reply), {type: "nexus_links", game, links}
// install_busy and install_plan carry from_download: the page does not let such a plan replace what is on screen.
// (a command's optional "ref" comes back in its result, so the page knows which control it was about)
//
// Nexus requests, unpacking, installing and uninstalling run on worker threads (BridgeHost::run_async); their answers
// reach the page when they are done. A newer search (or mod page) cancels the one before it, and an answer to an older
// request is dropped.
//
// The Nexus account: the player's API key is checked with Nexus (users/validate.json) and stored with
// DPAPI (account_store.h); the page only ever hears whether a key is set, the account's name and Premium. Downloads
// (nexus/download.h) come from an nxm:// link (handle_nxm: Windows hands it to EMF.exe, which hands it to the bridge)
// or, for Premium members, from a file's "Download with EMF" button. One runs at a time, with progress and cancel,
// into <app data>\downloads\, and the downloaded archive then goes through the same plan as "Install a downloaded
// file". A link that arrives before a key is set waits for it.
//
// Whether EMF.exe talks to Nexus at all is nexus_mode (below; the state's "nexus"): the release build (no
// application slug, no tester switch) makes no Nexus request (NexusMode::Off). Then the host refuses every command
// that would ask Nexus or use the account: nexus_search, nexus_mod, nexus_file, updates_check, account_key,
// account_check, account_logout, sso_start, nxm_take, nexus_download (a page script cannot get round the hidden tab and
// cards); its API clients get a transport that refuses too; it ignores a key a testing build stored (never read, never
// deleted) and answers an nxm:// link with how to download the file by hand. Local commands stay: nxm_give_back (links
// a testing build took), nexus_links and nexus_link (a file in the game folder), installing a downloaded file.
//
// Mod profiles (bridge_profiles.cpp; docs/app.md "Profiles"): page -> host "profile_save" {name, settings,
// replace} | "profile_switch" {slug} | "profile_rename" {slug, name} | "profile_delete" {slug} | "profile_export" {slug}
// | "profile_import" | "profile_import_path" {path} (tests only) | "profile_import_save" {name, replace} |
// "profile_import_cancel"; host -> page {type: "profile_exists", name, existing_name, settings} (Save as asks before it
// replaces another profile), {type: "profile_report", ...} (what a Switch did), {type: "profile_import", ok, error,
// name, slug, mods, exists, existing_name, missing}. The state carries "profiles": {current, current_name, changed,
// list}. Switch is refused while the game runs from the folder, like a switch.
//
// Save backups (bridge_saves.cpp; docs/app.md "Saves"): page -> host "saves_list" | "saves_backup" {label}
// | "saves_restore" {id, paths} | "saves_delete" {id} | "saves_open" {id} | "saves_auto" {on, keep}; host -> page
// {type: "saves", ...} (the list, the total size, the automatic backup settings), {type: "saves_restored", ok, error,
// restored, before_id, steam}, {type: "saves_note", ok, text} (the starter backup EMF.exe made at its first start).
// The state carries "saves_busy": why saves must not change now (an enshrouded.exe runs, or this folder's server).
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "account_store.h"
#include "core/json.h"
#include "core/profiles.h"
#include "game_folder.h"
#include "install.h"
#include "nexus/client.h"
#include "nexus/download.h"
#include "nexus/sso.h"
#include "nexus/v1.h"
#include "nxm_handler.h"
#include "saves/backup.h"
#include "server_process.h"
#include "servers.h"

namespace emf::core::json {
struct Value;
}

namespace emf::app {

class ServerDesk;  // server_bridge.h: the folder switcher and the Server tab

// Whether EMF.exe uses Nexus Mods' API, and how (docs/app.md "Nexus Mods"). One mode for every Nexus feature: Browse,
// Check for updates, the account, Mod Manager Download links and downloads.
// - Sso: a build with Nexus's application slug for a registered app (kSsoSlug): all of it, with "Log in with
//   Nexus". The tester switch adds the personal-key field next to it.
// - PersonalKey: no slug, but the tester switch is on ([nexus] personal_key=1 in EMF.exe's app.ini): all of it, with a
//   pasted personal API key. Nexus allows that for testing only; Nexus reviews an app in this mode.
// - Off: no slug and no switch, the release build: no Nexus request at all (Nexus's API policy: a public app must be
//   registered). No Browse tab, no Check for updates, no account, no nxm:// setting.
//   Everything local stays, with "Install a downloaded file" for a file the player downloaded from the website.
enum class NexusMode { Off, PersonalKey, Sso };
NexusMode nexus_mode(const std::string& slug, bool personal_key_switch);
const char* nexus_mode_name(NexusMode mode);  // "off", "personal_key", "sso" (the state's "nexus")

class BridgeHost {
public:
    virtual ~BridgeHost() = default;
    virtual std::wstring game() const = 0;                  // the folder shown ("" when none was found)
    virtual void choose_game(const std::wstring& dir) = 0;  // the player picked it: show and remember it
    virtual std::optional<std::wstring> pick_folder() = 0;  // the folder picker; nullopt when cancelled
    enum class Open { Url, Folder, SelectFile };
    virtual bool open(const std::wstring& target, Open how) = 0;  // the browser or Explorer
    virtual std::string running_reason() = 0;                    // why the game folder is busy ("" when not)
    virtual void rendered(int count, const std::vector<std::string>& titles) = 0;  // the page drew the mod list
    virtual std::wstring steam_game() = 0;                    // the game folder Steam knows ("" when none)
    virtual bool copy_text(const std::string& text) = 0;      // the clipboard (logs, errors for a bug report)
    // The file dialog for a downloaded mod archive (.zip, .7z, .rar); nullopt when cancelled.
    virtual std::optional<std::wstring> pick_archive() = 0;
    // Import and Export of a mod profile: the file dialogs for a .json file (Save offers `suggested` as its name);
    // nullopt when cancelled.
    virtual std::optional<std::wstring> pick_profile_file() { return std::nullopt; }
    virtual std::optional<std::wstring> pick_profile_save(const std::wstring& suggested) {
        (void)suggested;
        return std::nullopt;
    }
    // Runs `work` away from the UI thread. The closure it returns runs on the UI thread afterwards, and the messages
    // that closure returns go to the page.
    using Done = std::function<std::vector<std::string>()>;
    virtual void run_async(std::function<Done()> work) = 0;
    // How Nexus is reached: nullptr is WinHTTP; tests and --nexus-fixture give recorded replies.
    virtual nexus::Client::Transport nexus_transport() { return nullptr; }
    // EMF_TEST=1: "install_path" (an archive named by the page, for the smoke test) is allowed.
    virtual bool test_mode() const { return false; }

    // ---- the Nexus account and downloads ----
    // EMF's own folder for nexus.key and downloads\ (%LOCALAPPDATA%\EMF; a temporary one in smoke runs); "" = none.
    virtual std::wstring app_data() const { return {}; }
    // The registry for the nxm:// handler setting (a memory one in smoke runs); nullptr hides the setting.
    virtual RegistryLayer* registry() { return nullptr; }
    // This EMF.exe's full path, for the nxm:// command.
    virtual std::wstring exe_path() const { return {}; }
    // Runs `done` on the UI thread soon and posts the messages it returns. Call it on the UI thread only.
    virtual void run_on_ui(Done done) { (void)done; }
    // For worker threads (download progress, the SSO browser page): a poster that may be called from any thread at any
    // time, also after the window and the host are gone (then nothing happens). The default forwards to run_on_ui.
    virtual std::function<void(Done)> ui_poster() {
        return [this](Done done) { run_on_ui(std::move(done)); };
    }
    // The tester switch ([nexus] personal_key=1 in app.ini, read once at start; nexus_mode): the Nexus features and the
    // personal-key field, also next to "Log in with Nexus". Without it and without a slug: NexusMode::Off.
    virtual bool personal_key_allowed() const { return false; }
    // How the API v1 and the CDN are reached: nullptr is WinHTTP; tests and --nexus-fixture give recorded replies.
    virtual nexus::V1Client::Transport nexus_v1_transport() { return nullptr; }
    virtual nexus::Downloader::Fetch nexus_fetch() { return nullptr; }
    // Single sign-on: EMF's application slug ("" hides "Log in with Nexus") and its sockets (nullptr = WinHTTP).
    virtual std::string sso_slug() const { return nexus::kSsoSlug; }
    virtual nexus::SsoSocketFactory sso_sockets() { return nullptr; }
    // Unix seconds now (an nxm:// link's expiry).
    virtual int64_t now_unix() const;

    // ---- dedicated servers (server_bridge.h; the defaults do nothing or use the real functions) ----
    // The server folders Steam has (Enshrouded Dedicated Server, app 2278520).
    virtual std::vector<std::wstring> steam_servers() { return {}; }
    // The running enshrouded.exe and enshrouded_server.exe processes.
    virtual std::vector<ProcessInfo> processes() { return {}; }
    // Starts <folder>\enshrouded_server.exe in a job of this EMF.exe (ServerRunner).
    virtual ServerStart start_server(const std::wstring& folder, bool stop_on_close);
    virtual bool set_server_stop_on_close(const std::wstring& folder, bool on);
    virtual bool server_started_here(unsigned long pid);
    // Called on a worker thread.
    virtual StopOutcome stop_server(unsigned long pid, const StopOptions& o);
    virtual bool kill_server(unsigned long pid);
    // The EMF that Install EMF puts into a server: the package this EMF.exe came with.
    virtual EmfPackage emf_package();
    // Called on a worker thread.
    virtual ScriptRun install_emf(const EmfPackage& package, const std::wstring& server);
    // EMF's own mod on Nexus for "Check for updates" (0: not published yet, nothing is asked). Tests set another.
    virtual int64_t emf_nexus_id() const { return nexus::kEmfNexusModId; }

    // ---- save backups (bridge_saves.cpp) ----
    // Where the saves and the backups are; a smoke run uses only what EMF_TEST_STEAM_ROOT, EMF_TEST_SAVED_GAMES and
    // its own app data name, never a real place. Empty roots: nothing to back up.
    virtual saves::SaveRoots save_roots() { return {}; }
    // Why the saves must not change now ("" = go on), with `game` as the folder shown: from processes()
    // (saves_busy_reason). Called on the UI thread and on the restore's worker, so it must not read the host's state.
    virtual std::string saves_busy_for(const std::wstring& game);
    std::string saves_busy() { return saves_busy_for(game()); }
};

// Why saves must not be backed up or restored now, or "": an enshrouded.exe runs (from any folder: the saves belong to
// the Windows user, not to a game folder), or a dedicated server runs from `game`.
std::string saves_busy_reason(const std::vector<ProcessInfo>& processes, const std::wstring& game);

// A JSON string literal: quotes, backslashes and control characters escaped; bytes that are not UTF-8 become U+FFFD.
std::string json_quote(const std::string& s);

// The state message for the page.
std::string state_json(const FolderState& state, const std::string& running_reason);
// Its "profiles": the folder's profiles, the current one and whether the mods changed since (bridge_profiles.cpp).
std::string profiles_json(const FolderState& state);

class Bridge {
public:
    explicit Bridge(BridgeHost& host);
    ~Bridge();  // cancels Nexus requests; removes the unpack folder of a plan that was not applied

    // One message from the page; returns the messages to post back, in order (a fresh state after each change).
    std::vector<std::string> handle(const std::string& message);
    // The current state message when it differs from the last one sent (always with `force`).
    std::optional<std::string> state_if_changed(bool force);
    // The last state did not reach the page (the post failed): the next state_if_changed sends it again.
    void forget_state() { last_state_.clear(); }
    // Files in the game folder are being installed or uninstalled right now (the window should not close).
    bool changing() const;  // also: EMF is being installed into a server folder
    // A Stop from the Server tab is under way.
    bool server_stopping() const;
    // Tests: the Server tab's snapshot counts as read `ms` ago (server_bridge.h).
    void snapshot_age_for_test(unsigned ms);

    // An nxm:// link from Windows (the command line or a second EMF.exe): checked, then downloaded when a key is set
    // (else it waits for one). Returns the messages for the page.
    std::vector<std::string> handle_nxm(const std::string& url);
    // A link from EMF.exe's own command line (or one that arrived before the page): handled when the page asks for the
    // state, in order (at most 16 wait; none is dropped silently: each gets an answer).
    void queue_nxm(const std::string& url) {
        if (startup_nxm_.size() < 16) startup_nxm_.push_back(url);
    }
    // The page has asked for the state at least once (it listens).
    bool page_ready() const { return page_ready_; }
    // The account message for the page (never the key).
    std::string account_json();

private:
    std::string result(const std::string& cmd, bool ok, const std::string& error, const std::string& text = {});
    std::vector<std::string> start_plan(const std::wstring& archive, const std::string& nexus_id, const std::string& nexus_name,
                                        const std::string& nexus_file_id = {}, bool from_download = false);
    void drop_pending();
    std::string state_sig() const;  // the folder state a cached mod page was classified against
    // ---- mod profiles (bridge_profiles.cpp) ----
    // The profile commands; nullopt for any other command.
    std::optional<std::vector<std::string>> handle_profiles(const std::string& cmd, const core::json::Value& m);
    std::optional<core::Profile> profile_import_;  // a file picked for Import, waiting for the player's yes

    struct Request {
        int id = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        void renew() {
            if (cancel) *cancel = true;
            cancel = std::make_shared<std::atomic<bool>>(false);
            ++id;
        }
    };
    struct Pending {
        std::string id;
        InstallPlan plan;
    };

    BridgeHost& host_;
    nexus::Client client_;
    std::string last_state_;
    std::string ref_;     // the page's "ref" of the message being handled, echoed in its result
    FolderState folder_;  // the last scan
    Request search_, details_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::optional<Pending> pending_;  // the plan shown to the player
    std::map<int64_t, std::pair<std::string, std::string>> mod_cache_;  // mod id -> (state signature, mod_reply) for the session
    int plans_ = 0;
    bool busy_ = false;      // unpacking, installing or uninstalling
    bool changing_ = false;  // installing or uninstalling

    // ---- the Nexus account and downloads ----
    struct DownloadAsk {
        int64_t mod_id = 0, file_id = 0;
        std::string nxm_key;  // "" for a Premium member's download
        int64_t expires = 0, user_id = 0;
        std::string name;  // the mod's name when known (the Browse page)
        bool from_link = false;
    };
    std::wstring key_path() const;
    std::wstring downloads_dir() const;
    void ensure_account();  // reads nexus.key once
    std::vector<std::string> check_key(const std::string& key, const std::string& cmd);
    std::vector<std::string> start_download(const DownloadAsk& ask);
    std::string download_json(const char* state, const std::string& error = {}, const std::string& page = {}) const;
    std::vector<std::string> sso_start();
    // Nexus's used-up allowance still applies (false once its reset time has passed, which clears it).
    bool rate_blocked();
    void note_limits(const nexus::RateLimits& limits);

    nexus::V1Client v1_;
    bool account_loaded_ = false;
    std::optional<StoredAccount> account_;
    std::string account_error_;  // the last check's error, shown in the account section
    std::string account_note_;   // e.g. the key could not be saved on this PC
    bool checking_ = false;
    nexus::RateLimits limits_;  // from the last API v1 reply
    int64_t limits_until_ = 0;  // when a used-up allowance opens again (Unix seconds)
    int account_gen_ = 0;       // bumped by log out: a check that was under way then is dropped
    struct Download {
        bool running = false;
        int id = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        int64_t mod_id = 0, file_id = 0;
        std::string name, file;
        uint64_t bytes = 0, total = 0;
    } dl_;
    struct Sso {
        bool running = false;
        std::shared_ptr<std::atomic<bool>> cancel;
    } sso_;
    std::optional<DownloadAsk> waiting_;  // an nxm:// link that waits for an API key
    std::vector<std::string> startup_nxm_;
    // Finished downloads whose plan waits because a plan was open (or EMF was busy): "download_plan" opens one.
    struct Ready {
        int id = 0;
        std::wstring path;
        int64_t mod_id = 0, file_id = 0;
        std::string name, file;
    };
    std::vector<Ready> ready_;
    int ready_ids_ = 0;
    std::string ready_json() const;
    NexusMode mode() const;
    bool personal_key() const;  // the key field shows (the tester switch)
    bool page_ready_ = false;

    std::unique_ptr<ServerDesk> desk_;  // folders and dedicated servers
    // ---- Check for updates (bridge_updates.cpp) ----
    // The commands of "Check for updates" and the Nexus links; nullopt for any other command.
    std::optional<std::vector<std::string>> handle_updates(const std::string& cmd, const core::json::Value& m);
    // For a plan of a Nexus mod: installs the player linked to that mod, so the plan updates them (updates.h).
    std::map<std::string, UpdateKey> plan_update_keys() const;
    // A check under way is dropped (the folder changed, a mod was installed or removed): its answer would be out of date.
    // Returns the message that tells the page why ("" says nothing), or nothing when no check runs.
    std::vector<std::string> updates_stop(const std::string& why);
    Request updates_;              // the check running now (a newer click cancels it)
    bool updates_running_ = false;
    int updates_seq_ = 0;          // the page's seq of the check running now
    int64_t updates_retry_at_ = 0;  // after a failed check: no new one before this (Unix seconds)
    // ---- save backups (bridge_saves.cpp) ----
    // The save commands; nullopt for any other command.
    std::optional<std::vector<std::string>> handle_saves(const std::string& cmd, const core::json::Value& m);
    std::string saves_json();  // the "saves" message: the list, sizes, settings
    // The page listens for the first time: the starter backup, when there is none yet (on a worker).
    void saves_first_start();
    bool saves_working_ = false;  // a backup or a restore runs on a worker
    bool starter_tried_ = false;
};

}  // namespace emf::app
