#pragma once
// The player's Nexus Mods API key for EMF.exe, stored encrypted for the current Windows user with DPAPI
// (CryptProtectData) in %LOCALAPPDATA%\EMF\nexus.key, together with what the last check said (the account's name and
// whether it is Premium), so EMF.exe asks Nexus nothing when it starts. Another Windows user, or a copy of the file on
// another PC, cannot read it. The key is never logged and never sent to the page (only "set" or "not set"); it goes
// only to api.nexusmods.com. The account's email is never stored.
#include <cstdint>
#include <optional>
#include <string>

namespace emf::app {

struct StoredAccount {
    std::string key;         // the API key
    int64_t user_id = 0;     // from the last check
    std::string name;        // the account's name
    bool premium = false;
    bool supporter = false;
    std::string checked_at;  // when the key was last checked ("2026-10-26 20:00")
};

// DPAPI for the current user, with EMF's own entropy. nullopt when Windows refuses (or the bytes are not EMF's blob).
std::optional<std::string> dpapi_protect(const std::string& plain);
std::optional<std::string> dpapi_unprotect(const std::string& blob);

// Writes the account encrypted (a temporary file, flushed to disk, then renamed over the old one; the folder is made
// when missing).
bool save_account(const std::wstring& path, const StoredAccount& account);

// The key file as it is. A left-over <path>.tmp (EMF ended while saving) is removed first.
struct AccountFile {
    enum class Status { None, Ok, Unreadable, OtherUser };  // OtherUser: DPAPI says another Windows user saved it
    Status status = Status::None;
    std::optional<StoredAccount> account;  // when Ok
};
AccountFile read_account_file(const std::wstring& path);
// The stored account; nullopt when there is none or it cannot be read.
std::optional<StoredAccount> load_account(const std::wstring& path);
// Removes the file (and a left-over <path>.tmp); true when it is gone (also when it was never there).
bool delete_account(const std::wstring& path);

}  // namespace emf::app
