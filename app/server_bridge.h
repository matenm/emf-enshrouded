#pragma once
// The page's commands for folders and dedicated servers (docs/app.md "Servers"). The Bridge hands it every
// "folder_*" and "server_*" command and adds its fields to the state message.
//
// Page -> host: {cmd: "folder_switch" {path} | "folder_add" {path} | "folder_pick_server" | "folder_forget" {path}
//                | "server_detect" | "server_chat_save" {everyone, admins, players} | "server_admin_add" {account}
//                | "server_start" | "server_stop" {shared_ok} | "server_kill" | "server_on_close" {stop} | "server_logs"
//                | "server_install_emf"}
// Host -> page: the state's "folders" [{kind, path, label, current, missing}] and "server_tab" (null for a game folder:
//               {config, chat, refused [{account, time, command}], process {running, pid, started_here}, stopping,
//               stop_failed, stop_text, stop_on_close, can_start, package {ok, version, error, mods}, installing,
//               emf_here, log_path, core_log_path}),
//               {type: "server_detect", found: [{path, name, added}]},
//               {type: "server_stop", state: "progress" | "done" | "failed" | "ask", text} ("ask": other programs share
//               the server's console; the page asks, then sends server_stop {shared_ok: true}),
//               {type: "server_install", ok, error, lines}, {type: "server_logs", server, core, server_path, core_path}
//
// A folder the page names is added only when Steam has it (server_detect), or in a test run (EMF_TEST=1); any other
// server folder comes through the folder dialog. Passwords from enshrouded_server.json never reach the page: the
// config goes out as server_config_json, the logs through redact_log with every password seen this session.
//
// The Server tab's files (the config, [chat], the refused accounts of emf_core.log, the folders' labels, the process) are
// read on a worker every few seconds and kept (a snapshot): the window never waits for a slow drive, except once when a
// folder is first shown or after a change made here. The logs are read on a worker too.
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bridge.h"
#include "servers.h"

namespace emf::app {

class ServerDesk {
public:
    explicit ServerDesk(BridgeHost& host) : host_(host) {}
    ~ServerDesk();  // a stop or an install still running answers no one
    // How a worker's answer gets a fresh state message (the Bridge's state_if_changed).
    void set_state_source(std::function<std::optional<std::string>()> f) { fresh_state_ = std::move(f); }
    // Whether the Bridge is installing, uninstalling or unpacking a mod in this folder (one busy gate both ways).
    void set_busy_source(std::function<bool()> f) { bridge_busy_ = std::move(f); }

    static bool handles(const std::string& cmd);
    struct Reply {
        std::vector<std::string> messages;
        std::optional<std::wstring> switch_to;  // show this folder (the Bridge drops a plan built for the old one)
        bool state = false;                     // a fresh state after the messages
    };
    Reply handle(const std::string& cmd, const core::json::Value& m, const FolderState& folder, const std::string& ref);
    // ,"folders":[...],"server_tab":{...}|null for the state message; also remembers the folder shown (a server
    // shown at start, or picked with Change, joins the list).
    std::string state_fields(const FolderState& folder);
    // EMF is being installed into a server folder (the window should not close; no mod install meanwhile).
    bool installing() const { return installing_; }
    // A Stop runs (the window waits for it before it closes).
    bool stopping() const { return stopping_; }
    // A log of the server folder `folder` as the app shows it (the EMF tab's logs): its end, every password seen this
    // session taken out.
    std::string server_log_text(const std::wstring& folder, const std::wstring& log);
    // Tests: the snapshot counts as read `ms` ago.
    void age_snapshot(unsigned ms);

private:
    struct Snapshot {
        std::wstring folder;  // the folder shown when it was read
        bool server = false;
        ServerConfig cfg;
        ChatSettings chat;
        std::vector<RefusedAccount> refused;
        std::string log_key;  // emf_core.log's size and time when `refused` was read
        bool exe = false;     // enshrouded_server.exe is there
        unsigned long pid = 0;
        std::vector<std::pair<std::wstring, std::string>> labels;  // each listed folder: its label
        std::vector<std::wstring> missing;                         // listed folders without their exe
        unsigned long long at = 0;                                 // when it was read (GetTickCount64)
    };
    static Snapshot read_snapshot(const std::wstring& folder, bool server, const std::vector<FolderEntry>& entries,
                                  const std::vector<ProcessInfo>& processes, const Snapshot* before,
                                  std::vector<std::string>* secrets);
    void refresh_snapshot(const std::wstring& folder, bool server);
    void remember_secrets(const std::vector<std::string>& secrets);
    FolderList& list();
    void save_list();
    std::string result(const std::string& cmd, bool ok, const std::string& error, const std::string& text = {}) const;
    std::string folder_label(const FolderEntry& f) const;  // from the snapshot (no file read)
    std::wstring list_file() const;

    BridgeHost& host_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::function<std::optional<std::string>()> fresh_state_;
    std::function<bool()> bridge_busy_;
    std::optional<FolderList> list_;
    std::string ref_;
    std::optional<Snapshot> snap_;
    bool snap_busy_ = false;  // a worker reads the next snapshot
    bool logs_busy_ = false;  // a worker reads the logs
    std::vector<std::string> secrets_;  // every password seen this session (only to take them out of logs; never sent)
    // the last Stop: one at a time, and what it left belongs to its server only (its pid and folder)
    bool stopping_ = false, stop_failed_ = false, installing_ = false;
    std::string stop_text_;
    unsigned long stop_pid_ = 0;
    std::wstring stop_folder_;
};

}  // namespace emf::app
