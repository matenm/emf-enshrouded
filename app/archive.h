#pragma once
// Unpacking a mod archive the player downloaded (.zip, .7z, .rar) with Windows' own tar.exe (libarchive), for
// EMF.exe's "Install a downloaded file". Nothing from the archive is ever run: tar.exe unpacks it into a new folder in
// %TEMP%\emf-install\, started from System32 (never from the archive's folder), and the result is checked twice:
// - before: tar's listing (`tar -tvf`) must have no path with "..", no absolute path or drive, no link or device, and
//   stay within the limits (entries, bytes);
// - after: the unpacked folder is walked without following links; a reparse point (symbolic link, junction) or a hard
//   link refuses the archive, and so does a total above the limit (also watched while tar runs: what tar writes is
//   counted as it writes, so an archive whose header gives no size is stopped soon after the limit).
#include <cstdint>
#include <string>
#include <vector>

namespace emf::app {

struct ArchiveLimits {
    uint64_t max_bytes = 2ull << 30;  // unpacked, 2 GB
    size_t max_entries = 50000;
    unsigned timeout_ms = 5 * 60 * 1000;
};

// One line of `tar -tvf`: the type ('-' file, 'd' folder, 'l' symbolic link, 'h' hard link, 'c' 'b' 'p' 's' devices
// and pipes), the size and the path as listed.
struct ListedEntry {
    char type = '?';
    uint64_t size = 0;
    std::string path;
};
std::vector<ListedEntry> parse_tar_listing(const std::string& text);

// What makes a listing unsafe ("" when nothing does), in words for the player. Paths are checked with
// nexus::unsafe_archive_path, the one path check the classifier and the installer share.
std::string check_listing(const std::vector<ListedEntry>& entries, const ArchiveLimits& limits);

// %SystemRoot%\System32\tar.exe, or "" when Windows has none (Windows 10 before version 1803).
std::wstring system_tar();

struct Unpacked {
    bool ok = false;
    std::string error;               // why not, for the player
    std::vector<std::string> files;  // relative to the folder, '/' between folders, UTF-8, sorted
    uint64_t bytes = 0;
};
// Unpacks `archive` into `dest` (an existing empty folder) with `tar` after checking its listing, then walks `dest`.
Unpacked unpack_archive(const std::wstring& tar, const std::wstring& archive, const std::wstring& dest,
                        const ArchiveLimits& limits = {});

// The files below `dir` (relative paths with '/', UTF-8, sorted), without following links. False when a reparse point
// or a hard link is found, or there are more than `max_entries` (`why` says which).
bool list_tree(const std::wstring& dir, std::vector<std::string>& files, uint64_t& bytes, std::string& why,
               size_t max_entries = 50000);

// Deletes a folder EMF made (an unpack folder), never following a link inside it: a link itself is removed, not what
// it points to. True when the folder is gone.
bool delete_tree(const std::wstring& dir);

// A new empty folder %TEMP%\emf-install\<n> ("" when it cannot be made).
std::wstring new_unpack_folder();
// Removes %TEMP%\emf-install\<n> folders older than a day (left behind when EMF.exe was ended mid-way).
void clean_old_unpack_folders();

// The SHA-256 of a file as 64 lower-case hex digits ("" when it cannot be read).
std::string file_sha256(const std::wstring& path);

}  // namespace emf::app
