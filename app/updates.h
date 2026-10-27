#pragma once
// "Check for updates" on EMF.exe's Mods tab: which installed mods EMF can tie to a Nexus mod, the links the
// player sets by hand, and the page's message with what Nexus said (nexus/updates.h decides). Plain file work without a
// window, tested against temp folders; the bridge (bridge_updates.cpp) runs the check on a worker thread, only when the
// player clicks.
//
// A mod is checked when EMF knows its Nexus mod:
// - an install EMF recorded (emf\installed_mods\<key>.json: nexus_id, and nexus_file_id when EMF downloaded the file);
// - a link the player set on the mod's details ("Link to a Nexus mod..."), kept in emf\config\nexus_links.json;
// - nexus_id in the mod's mod.json (EMF and EML mods).
// In that order: a record wins over a link, a link over mod.json. EMF guesses nothing else (no matching by name).
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "game_folder.h"
#include "install.h"
#include "nexus/client.h"
#include "nexus/updates.h"

namespace emf::app {

// ---- the links the player set by hand: emf\config\nexus_links.json ----
// {"links": {"chain:creative.dll": 182, "eml:foo": 195}}: a mod row ("source:id", as the page names rows) -> Nexus mod.
std::wstring nexus_links_path(const std::wstring& game);  // <game>\emf\config\nexus_links.json
// The links; entries that are not a known source with an id and a Nexus id of 1 to 12 digits are left out.
std::map<std::string, int64_t> read_nexus_links(const std::wstring& game);
// Sets (or, with mod_id 0, removes) one link and writes the file whole (a new file next to it, then a rename).
Change write_nexus_link(const std::wstring& game, const std::string& row, int64_t mod_id);
// A row's name for the links: "source:id"; "" when the source is not one EMF shows or the id is empty or too long.
std::string link_row(const std::string& source, const std::string& id);

// What the player typed as the Nexus mod: its id ("170") or its page on nexusmods.com, for Enshrouded
// ("https://www.nexusmods.com/enshrouded/mods/170?tab=files"). Anything else says why not.
struct NexusLinkParse {
    int64_t mod_id = 0;
    std::string error;
};
NexusLinkParse parse_nexus_mod_link(const std::string& text);

// ---- what is checked ----
struct UpdateTarget {
    int64_t nexus_id = 0;
    std::string via;                // "record", "link" or "mod.json"
    std::string install_key;        // the install record it comes from or belongs to ("" none)
    std::vector<std::string> rows;  // the mod rows it shows on ("source:id")
    std::string name;
    nexus::Installed installed;     // version, Nexus file id, archive, name
    std::string note;               // something the player should know about how EMF compared
    bool whole_install = true;      // its rows are all the rows of its install (a link may cover only one of them)
};
// Every installed mod EMF can tie to a Nexus mod. A record's file id (and its archive's upload time) is not used when
// none of its mods says the version EMF installed any more (the files were changed by hand since): then the versions
// are compared.
std::vector<UpdateTarget> update_targets(const FolderState& s, const std::vector<InstallRecord>& records,
                                         const std::map<std::string, int64_t>& links);

// For installs of a Nexus mod that should update an install recorded under another key (an install the player linked
// to that mod by hand, all of its rows; or a record that learned its Nexus id that way): Nexus mod id (digits) -> that
// install's key and why. A record named nexus-<id> wins (the usual key); a mod tied to two installs is left out, and so
// is a link on only some rows of an install (an update must not replace the parts the player did not link).
std::map<std::string, UpdateKey> update_keys(const std::vector<UpdateTarget>& targets,
                                             const std::vector<InstallRecord>& records);

// The uids of the files to take (or look at) that the check names, for their changelogs (Client::changelogs), and the
// changelogs put on those files.
std::vector<std::string> changelog_uids(const std::vector<UpdateTarget>& targets, const nexus::UpdatesCheck& check);
void attach_changelogs(nexus::UpdatesCheck& check, const std::map<std::string, std::vector<std::string>>& logs);

constexpr int kUpdatesRetrySeconds = 60;  // after a failed check, EMF asks Nexus again only after this

// ---- the page's message ----
// EMF itself on Nexus (kEmfNexusModId; never updated by EMF, only reported).
struct EmfSelf {
    int64_t nexus_id = 0;
    std::string installed;  // EMF's version here
};
// {type:"updates", seq, ok, error, retry_after, asked, requests, skipped,
//  summary:{update, maybe, current, unavailable, gone, failed},
//  items:[{nexus_id, state, reason, via, rows, install_key, name, note, installs_as, separate,
//          installed:{version, file_id, file_name}, nexus:{name, version, updated, status, url, files_url}, file,
//          choices}], emf, note}
// file and choices: {file_id, name, version, category, date, size, changelog, page}. A target whose mod was not
// asked or answered (a failed request, past the cap) is "failed". installs_as: the key Update with EMF installs under;
// separate: a reason when that is not the target's own install. retry_after: seconds to wait after a failed check.
std::string updates_reply(int seq, const std::vector<UpdateTarget>& targets,
                          const nexus::Result<nexus::UpdatesCheck>& check, const EmfSelf& emf,
                          const std::map<std::string, UpdateKey>& keys = {});
// The links as the page gets them: {type:"nexus_links", game, links:{"source:id": id}}.
std::string nexus_links_json(const std::wstring& game, const std::map<std::string, int64_t>& links);

}  // namespace emf::app
