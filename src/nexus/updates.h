#pragma once
// The update check of EMF.exe: which of the player's installed Nexus mods have a newer file. One request to
// Nexus's public GraphQL API asks for up to 50 mods at once, and one more asks for the changelogs of the files to take;
// the rules below then decide, honestly, between "update", "can't tell", "up to date", "not on Nexus now", "gone from
// Nexus" and "not checked". Pure (client.h sends the requests), unit-tested with recorded replies
// (tests/fixtures/nexus/updates_reply.json and changelogs_reply.json).
//
// What Nexus answers (docs/nexus.md):
// - legacyModsByDomain(ids: [{gameDomain, modId}], count) gives each mod's name, version, updatedAt and status
//   ("published", "hidden", "removed", "wastebinned", "not_published", "under_moderation"); an id Nexus does not have
//   is left out of the answer, without an error. Without `count` only 20 come back.
// - modFiles(modId, gameId) under an alias per mod (f170: modFiles(...)) gives the files; a mod Nexus does not have
//   gives [] (no error), a hidden or removed one still lists its files. 50 mods in one request passed Nexus's
//   complexity limit; without changelogs that is about 50 KB of JSON (with them it was 130 KB, two thirds changelogs).
// - modFilesByUid(uids, count) gives files by their uid (gameId << 32 | fileId, as Nexus sends it) with changelogText;
//   a uid it does not know is left out.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nexus/api.h"

namespace emf::nexus {

// EMF's own mod id on Nexus (0: the check does not ask about EMF). When it is set, a check also says when a newer
// EMF is out, with a link to its page. EMF never updates itself.
constexpr int64_t kEmfNexusModId = 0;

constexpr size_t kUpdatesPerRequest = 50;     // mods in one request
constexpr size_t kUpdateRequestsMax = 4;      // requests per check, so at most 200 mods per click
constexpr size_t kChangelogsPerRequest = 50;  // the changelogs of the files to take: one request, at most 50 files
constexpr size_t kChangelogLines = 30;        // lines kept of a file's changelog, each at most kChangelogLineBytes
constexpr size_t kChangelogLineBytes = 300;
constexpr int64_t kSiblingSeconds = 3600;  // a file uploaded within this of the installed one, at its version, is a variant

// The ids one request carries, in order: invalid (<= 0) and repeated ids are dropped, and at most kUpdatesPerRequest.
std::vector<int64_t> updates_request_ids(const std::vector<int64_t>& mod_ids);
// The request for those ids (updates_request_ids): {"query": ..., "variables": {"ids": [...], "count", "gameId",
// "f<id>": <id>, ...}}. The ids are variables; the query text only names them (f170).
std::string updates_request(const std::vector<int64_t>& mod_ids);

struct ModStatus {
    int64_t mod_id = 0;
    bool found = false;  // Nexus answered with this mod
    std::string name, version, updated_at;
    std::string status;            // as Nexus says it ("published", "hidden", ...)
    std::vector<NexusFile> files;  // their changelogs come from changelogs_request
    bool files_failed = false;     // Nexus sent an error for this mod's files (errors[].path names its alias)
};

// One entry per asked id (found=false when Nexus has no such mod). GraphQL errors next to data keep what arrived and
// mark the mods whose files failed; an HTTP error, a reply that is not JSON, errors without data, or a reply without
// the mods' part are an error. Never throws.
Result<std::map<int64_t, ModStatus>> parse_updates(std::string_view body, const std::vector<int64_t>& asked,
                                                   int http_status = 200);

// A whole check (Client::updates): the mods Nexus answered for, how many requests it took, and the ids it did not ask
// because of the cap (kUpdatesPerRequest x kUpdateRequestsMax).
struct UpdatesCheck {
    std::map<int64_t, ModStatus> mods;
    size_t requests = 0;
    std::vector<int64_t> skipped;
};

// The changelogs of some files, by uid: the uids one request carries (digits only, each once, at most
// kChangelogsPerRequest), the request, and the reply as uid -> lines (kChangelogLines of kChangelogLineBytes at most).
std::vector<std::string> changelogs_request_uids(const std::vector<std::string>& uids);
std::string changelogs_request(const std::vector<std::string>& uids);
Result<std::map<std::string, std::vector<std::string>>> parse_changelogs(std::string_view body, int http_status = 200);

// -1, 0 or 1 for two SemVer-ish versions: a leading "v" dropped, numeric parts compared (missing parts are 0, so
// "1" == "1.0.0"), "+build" ignored. After a dash only the known pre-release words count, before the release and in
// this order: dev, alpha, beta, pre / preview, rc (with a number: "beta2", "rc.1"). nullopt when either does not parse
// ("1.0b", "Beta 2", "1.0-hotfix", "1.2-3", "").
std::optional<int> compare_versions(std::string_view a, std::string_view b);

// The upload time in the name Nexus gives a download, "<name>-<mod id>-<version>-<unix time>.<zip|7z|rar>"
// ("AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip" -> 1792790875): the `date` of the file it was. 0 when the name has none.
int64_t archive_upload_time(std::string_view archive_name);

// The words of a file or archive name, without version numbers, the archive's extension and the numbers Nexus adds:
// "ShroudHud 1.7 - Standalone (GV 1.0.0.3)" -> "shroud hud standalone gv",
// "AutoLootPlus 1.1.8-170-1-1-8-1792790875.zip" -> "auto loot plus".
std::string file_name_key(std::string_view name);

// What EMF knows about an installed mod.
struct Installed {
    std::string version;  // what the installed mod says it is ("" unknown)
    int64_t file_id = 0;  // the Nexus file EMF downloaded it as (0 unknown)
    std::string archive;  // the archive it was installed from, to match file names ("" unknown)
    std::string name;     // the mod's name
    // The archive's upload time may name the installed file (archive_upload_time). False when the mod's files changed
    // since EMF installed that archive.
    bool archive_names_file = true;
};

enum class UpdateState { Update, Maybe, Current, Unavailable, Gone, Failed };
// "update", "maybe" (can't tell), "current", "unavailable" (not on Nexus now: hidden, unpublished, under moderation),
// "gone" (removed, or no such mod), "failed" (Nexus's answer about it was incomplete)
const char* update_state_name(UpdateState s);

struct UpdateDecision {
    UpdateState state = UpdateState::Maybe;
    std::string reason;  // plain words for the player
    // The file to take (Update), or Nexus's main file the player may look at ("can't tell"): not an update by itself.
    std::optional<NexusFile> file;
    std::vector<NexusFile> choices;           // several files could be it: the player chooses (newest first)
    std::optional<NexusFile> installed_file;  // the installed file as Nexus lists it now (its file id or upload time)
    std::string nexus_version;                // the newest version Nexus shows for the mod (its page or its main files)
};

// The rules (docs/app.md, "Checking for updates"):
// - failed: Nexus sent an error for the mod's files, or its files without the mod; unavailable: hidden, not published
//   or under moderation; gone: removed, or no such mod;
// - the installed file is known (the file id EMF downloaded, or the upload time in a Nexus archive's name): an update
//   when a newer upload of the same file (same name without numbers, with a newer version) is there, or the installed
//   file is now an old version, archived or removed - unless the file to take then is older than it (a rollback, an
//   archived beta), which is "can't tell". Newer files under other names (main or update files; optional ones for an
//   optional file) are "can't tell"; one uploaded within an hour of it at its version is a variant (Standalone /
//   ModLoader), and an older version uploaded later is ignored;
// - only the version is known: SemVer-ish compare with the newest version of the page and its main files; a version
//   that does not parse is "can't tell", never "update"; a main file with the version already installed is "can't
//   tell" too;
// - several main files: the one whose own words (those not all of them have) all appear in the installed file's or
//   archive's name; the mod's own name counts only when it has such a word (else it would always pick the plain
//   variant); otherwise the player chooses, and so when the newest upload of a file is not its highest version.
UpdateDecision decide_update(const Installed& in, const ModStatus& mod);

}  // namespace emf::nexus
