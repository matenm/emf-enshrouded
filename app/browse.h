#pragma once
// The Browse page's data: Nexus replies (nexus/client.h) turned into the page's JSON, the hosts pictures may come from,
// the classifier's view of the game folder, and recorded replies for tests and screenshots (--nexus-fixture).
#include <cstdint>
#include <string>
#include <vector>

#include "game_folder.h"
#include "nexus/classify.h"
#include "nexus/client.h"
#include "nexus/download.h"
#include "nexus/v1.h"

namespace emf::app {

// The hosts the page shows pictures from (thumbnails, description images). index.html's CSP img-src and the host's
// request filter (main.cpp) allow the same, and nothing else from outside.
const std::vector<std::string>& image_hosts();
// https://<one of image_hosts()>/<path>, without a user name, a port or odd characters.
bool image_url_allowed(const std::string& url);

// What the classifier needs to know about the folder: its proxy DLLs and their tools (from the app's scan).
nexus::GameState game_state_of(const FolderState& s);

// Files in the order the page lists them: MAIN, OPTIONAL, UPDATE, MISCELLANEOUS, OLD_VERSION, ARCHIVED, REMOVED (others
// before ARCHIVED); newest first within each.
std::vector<nexus::NexusFile> order_files(std::vector<nexus::NexusFile> files);
// MAIN files only: the details look inside their archives at once; every other file when the player asks. Keeps one mod
// page to about six Nexus requests (the mod, its files, and a couple of listings).
bool contents_at_once(const std::string& category);
constexpr size_t kMaxContentsAtOnce = 2;  // archive listings fetched for one mod page, at most

struct FileFacts {
    nexus::NexusFile file;
    bool checked = false;  // its archive listing was asked for
    nexus::Result<nexus::ContentsPage> contents;
};

// {type:"nexus_search", seq, ok, error, offset, total, mods:[{id, name, summary, version, author, downloads,
// endorsements, updated, thumbnail, adult}]}
std::string search_reply(int seq, int offset, const nexus::Result<nexus::SearchPage>& r);
// {file_id, name, version, category, size, date, primary, checked, listed, error, entries, kind, kind_label, also,
// notes}: "listed" false with "checked" true means Nexus has no listing for the file (new uploads).
std::string file_json(const FileFacts& f, const nexus::GameState& g);
// {type:"nexus_mod", seq, ok, error, mod:{..., description_html, url}, files:[file_json], files_error}
std::string mod_reply(int seq, const nexus::Result<nexus::NexusMod>& mod,
                      const nexus::Result<std::vector<nexus::NexusFile>>& files, const std::vector<FileFacts>& facts,
                      const nexus::GameState& g);
// {type:"nexus_file", seq, mod_id, file:file_json}: one file looked at on request.
std::string file_reply(int seq, int64_t mod_id, const FileFacts& f, const nexus::GameState& g);

// Recorded replies instead of Nexus, from `dir`: search_<offset>.json or search.json, mod_<id>.json, files_<id>.json,
// contents_<mod>_<file>.json (a missing listing is an empty one, as Nexus answers for new uploads), updates.json (the
// update check: one reply; a mod it does not mention is one Nexus does not have), changelogs.json (its changelogs). Anything else is a 404.
nexus::Client::Transport fixture_transport(const std::wstring& dir);
// The fixture's picture for an image URL: <dir>\images\<last part of the URL's path> ("" when there is none).
std::wstring fixture_image(const std::wstring& dir, const std::string& url);
// Recorded API v1 replies from `dir`: v1_validate.json (a key starting with "bad" gets a 401),
// v1_file_<mod>_<file>.json, v1_link_<mod>_<file>.json; anything else is a 404. Every reply has rate-limit headers
// (X-RL-Daily-Remaining from v1_daily.txt when it is there, else 19999).
nexus::V1Client::Transport fixture_v1_transport(const std::wstring& dir);
// The CDN from `dir`: <dir>\downloads\<the address's file name>, or a 404.
nexus::Downloader::Fetch fixture_fetch(const std::wstring& dir);
// No Nexus at all (a smoke run without --nexus-fixture): every request fails at once.
nexus::V1Client::Transport offline_v1_transport();
nexus::Downloader::Fetch offline_fetch();

}  // namespace emf::app
