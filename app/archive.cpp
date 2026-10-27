#include "archive.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <thread>

#include "nexus/classify.h"
#include "text.h"

namespace emf::app {

namespace {

constexpr size_t kMaxListing = 32u << 20;  // tar's listing kept in memory (50000 entries fit easily)
constexpr size_t kMaxErrors = 64u << 10;

std::string size_text(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) snprintf(buf, sizeof buf, "%.1f GB", b / double(1ull << 30));
    else snprintf(buf, sizeof buf, "%.0f MB", b / double(1ull << 20));
    return buf;
}

// A path from a tar listing shown to the player: control characters become '?', and it is cut to a readable length, so
// a crafted name (newlines, a forged listing line) cannot spill into the message.
std::string show_path(const std::string& p) {
    std::string out;
    for (size_t i = 0; i < p.size() && out.size() < 80; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        out += c < 0x20 || c == 0x7F ? '?' : p[i];
    }
    if (p.size() > out.size()) out += "...";
    return out;
}

// Reads a pipe to its end; keeps at most `cap` bytes but keeps reading, so the program never blocks on a full pipe.
void drain(HANDLE pipe, std::string& out, size_t cap) {
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got > 0)
        if (out.size() < cap) out.append(buf, (std::min)(static_cast<size_t>(got), cap - out.size()));
}

struct Run {
    bool started = false, timed_out = false, stopped = false;
    bool wrote_too_much = false;  // the program wrote more than max_write bytes (it was ended)
    DWORD exit_code = 0;
    DWORD start_error = 0;
    std::string out, err;
};

// The bytes the processes of `job` have written so far (files and pipes), from Windows' own count; 0 without a job.
uint64_t job_bytes_written(HANDLE job) {
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION info{};
    if (!job || !QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation, &info, sizeof info, nullptr))
        return 0;
    return info.IoInfo.WriteTransferCount;
}

// Runs `exe args` with no window, its working folder System32 (never a folder with the archive's files), stdin from
// NUL and both outputs read. The process is in a job that is killed if EMF.exe dies, so tar never outlives the app.
// `keep_going` is asked about once a second; false ends the program. `max_write` (0: none): once the program has
// written more bytes than that (Windows counts them for the job, every 200 ms), it is ended. This counts a file that is
// still being written, which a folder listing shows as 0 bytes until it is closed.
Run run(const std::wstring& exe, const std::wstring& args, unsigned timeout_ms, const std::function<bool()>& keep_going,
        uint64_t max_write = 0) {
    Run r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return r;
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
        CloseHandle(out_r);
        CloseHandle(out_w);
        return r;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    wchar_t sys[MAX_PATH + 1] = {};
    GetSystemDirectoryW(sys, MAX_PATH + 1);
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   nullptr, sys, &si, &pi);
    r.start_error = ok ? 0 : GetLastError();
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (job) CloseHandle(job);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return r;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);  // before it runs, so a child cannot escape it
    ResumeThread(pi.hThread);
    r.started = true;
    std::thread reader_out([&] { drain(out_r, r.out, kMaxListing); });
    std::thread reader_err([&] { drain(err_r, r.err, kMaxErrors); });
    const ULONGLONG start = GetTickCount64();
    ULONGLONG asked = start;
    for (;;) {
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
        const ULONGLONG now = GetTickCount64();
        if (max_write && job_bytes_written(job) > max_write) r.wrote_too_much = true;
        else if (now - start > timeout_ms) r.timed_out = true;
        else if (keep_going && now - asked >= 1000) {
            asked = now;
            if (!keep_going()) r.stopped = true;
        }
        if (r.timed_out || r.stopped || r.wrote_too_much) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 10000);
            break;
        }
    }
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    if (max_write && job_bytes_written(job) > max_write) r.wrote_too_much = true;  // also when it ended by itself
    reader_out.join();
    reader_err.join();
    CloseHandle(out_r);
    CloseHandle(err_r);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);  // tar is already gone; the job just enforced death-with-parent
    return r;
}

// tar's own words, first line only and short, for an error message.
std::string tar_words(const std::string& err) {
    std::string t = to_utf8_text(err);
    const size_t nl = t.find_first_of("\r\n");
    if (nl != std::string::npos) t.resize(nl);
    if (t.rfind("tar.exe: ", 0) == 0) t.erase(0, 9);
    else if (t.rfind("tar: ", 0) == 0) t.erase(0, 5);
    if (t.size() > 300) t = t.substr(0, 300) + "...";
    return show_path(t);  // never let control characters through
}

bool walk(const std::wstring& root, const std::wstring& rel, std::vector<std::string>& files, uint64_t& bytes,
          std::string& why, size_t max_entries, size_t& seen) {
    WIN32_FIND_DATAW fd;
    const std::wstring here = rel.empty() ? root : root + L"\\" + rel;
    HANDLE h = FindFirstFileExW(lp(here + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {  // a real folder with "\\*" always yields "." and ".."; INVALID means it failed
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return true;  // genuinely empty (rare)
        why = "a folder in it could not be read (" + utf8(rel) + ": " + windows_error_text(e) + ")";
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring sub = rel.empty() ? name : rel + L"\\" + name;
        if (++seen > max_entries) {
            why = "it has more than " + std::to_string(max_entries) + " files";
            ok = false;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            why = "it contains a link (" + show_path(utf8(sub)) + ")";
            ok = false;
            break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!walk(root, sub, files, bytes, why, max_entries, seen)) {
                ok = false;
                break;
            }
            continue;
        }
        HANDLE f = CreateFileW(lp(root + L"\\" + sub).c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        const bool got = f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &info);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (got && info.nNumberOfLinks > 1) {
            why = "it contains a hard link (" + show_path(utf8(sub)) + ")";
            ok = false;
            break;
        }
        bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        std::string p = utf8(sub);
        std::replace(p.begin(), p.end(), '\\', '/');
        files.push_back(std::move(p));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// Total size of the files below `dir` (links not followed), stopping once it passes `stop_at`.
uint64_t tree_bytes(const std::wstring& dir, uint64_t stop_at) {
    uint64_t total = 0;
    std::vector<std::wstring> todo{dir};
    while (!todo.empty() && total <= stop_at) {
        const std::wstring d = todo.back();
        todo.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(lp(d + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) todo.push_back(d + L"\\" + name);
            else total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return total;
}

std::wstring temp_root() {
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0 || n > MAX_PATH) return {};
    return std::wstring(tmp, n) + L"emf-install";
}

std::string lower_ext(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// The first bytes of `path` match the archive type its name claims (.zip is PK, .7z is 7z, .rar is Rar!). A name with
// any other extension (a .tar in a test) is not checked here, so libarchive still reads it. This stops a file that is a
// tar, an mtree spec or anything else wearing a .zip name (which libarchive would otherwise read, mtree even pulling
// file contents from disk paths).
bool magic_matches_extension(const std::wstring& path, const std::string& name) {
    const std::string ext = lower_ext(name);
    if (ext != "zip" && ext != "7z" && ext != "rar") return true;
    std::string head;
    read_file(path, head, 8);
    const auto starts = [&](const char* sig, size_t n) { return head.size() >= n && memcmp(head.data(), sig, n) == 0; };
    if (ext == "zip") return starts("PK\x03\x04", 4) || starts("PK\x05\x06", 4) || starts("PK\x07\x08", 4);
    if (ext == "7z") return starts("7z\xBC\xAF\x27\x1C", 6);
    return starts("Rar!\x1A\x07", 6);  // rar4 (…\x00) and rar5 (…\x01\x00)
}

}  // namespace

std::vector<ListedEntry> parse_tar_listing(const std::string& text) {
    // "-rw-rw-r--  0 0      0          25 Oct 26 16:40 Wrapper/dinput8.dll": mode, links, owner, group, size, three
    // date fields, then the path (with " -> target" or " link to target" for links)
    std::vector<ListedEntry> out;
    size_t from = 0;
    while (from < text.size()) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        std::string line = text.substr(from, to - from);
        from = to + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t at = 0;
        std::string fields[8];
        bool whole = true;
        for (auto& f : fields) {
            while (at < line.size() && line[at] == ' ') ++at;
            const size_t end = line.find(' ', at);
            if (end == std::string::npos) {
                whole = false;
                break;
            }
            f = line.substr(at, end - at);
            at = end;
        }
        ListedEntry e;
        e.type = fields[0].empty() ? '?' : fields[0][0];
        if (!whole || at >= line.size()) {  // not a listing line: keep it as unknown, so it is refused
            e.type = '?';
            e.path = line;
            out.push_back(std::move(e));
            continue;
        }
        e.path = line.substr(at + 1);
        uint64_t size = 0;
        bool digits = !fields[4].empty();
        for (char c : fields[4]) {
            if (c < '0' || c > '9') digits = false;
            else size = size * 10 + static_cast<uint64_t>(c - '0');
        }
        e.size = digits ? size : 0;
        if (e.type == 'l') {  // the link's target is not part of its name
            const size_t arrow = e.path.rfind(" -> ");
            if (arrow != std::string::npos) e.path.resize(arrow);
        } else if (e.type == 'h') {
            const size_t link = e.path.rfind(" link to ");
            if (link != std::string::npos) e.path.resize(link);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// A name part that ends in a dot or a space: Windows drops those, so two entries ("notes." and "notes") would land in
// one folder. unsafe_archive_path allows them (the trimmed name is not empty); the listing refuses them outright.
static bool trailing_dot_or_space(const std::string& path) {
    size_t from = 0;
    while (from <= path.size()) {
        size_t to = path.find_first_of("/\\", from);
        if (to == std::string::npos) to = path.size();
        if (to > from) {
            const char last = path[to - 1];
            if ((last == '.' || last == ' ') && path.compare(from, to - from, ".") != 0 && path.compare(from, to - from, "..") != 0)
                return true;
        }
        from = to + 1;
    }
    return false;
}

std::string check_listing(const std::vector<ListedEntry>& entries, const ArchiveLimits& limits) {
    if (entries.size() > limits.max_entries)
        return "it has " + std::to_string(entries.size()) + " entries (EMF unpacks at most " + std::to_string(limits.max_entries) + ")";
    uint64_t total = 0;
    for (const auto& e : entries) {
        std::string_view p = e.path;  // "./x" from tar -C dir . is x (libarchive unpacks it there)
        while (p.size() >= 2 && p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p.remove_prefix(2);
        if (p.empty() && e.type == 'd') continue;
        if (nexus::unsafe_archive_path(p) || trailing_dot_or_space(std::string(p)))
            return "a path in it is not safe to unpack (" + show_path(e.path) + ")";
        if (e.type == 'l') return "it contains a symbolic link (" + show_path(e.path) + ")";
        if (e.type == 'h') return "it contains a hard link (" + show_path(e.path) + ")";
        if (e.type != '-' && e.type != 'd') return "it contains something that is not a file or folder (" + show_path(e.path) + ")";
        total += e.size;
        if (total > limits.max_bytes) return "it unpacks to more than " + size_text(limits.max_bytes);
    }
    return "";
}

std::wstring system_tar() {
    wchar_t sys[MAX_PATH + 1] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH + 1);
    if (n == 0 || n > MAX_PATH) return {};
    const std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
    return exists(tar) && !is_dir(tar) ? tar : std::wstring();
}

bool list_tree(const std::wstring& dir, std::vector<std::string>& files, uint64_t& bytes, std::string& why, size_t max_entries) {
    files.clear();
    bytes = 0;
    size_t seen = 0;
    const bool ok = walk(dir, L"", files, bytes, why, max_entries, seen);
    std::sort(files.begin(), files.end());
    return ok;
}

Unpacked unpack_archive(const std::wstring& tar, const std::wstring& archive, const std::wstring& dest, const ArchiveLimits& limits) {
    Unpacked u;
    const auto refuse = [&](const std::string& why) {
        u.ok = false;
        u.error = why;
        u.files.clear();
        delete_tree(dest);  // leave no half-unpacked files behind
        return u;
    };
    if (tar.empty() || !exists(tar))
        return refuse("Windows' tar.exe (System32\\tar.exe) is missing, and EMF unpacks archives with it. It comes with "
                      "Windows 10 version 1803 and later.");
    if (!exists(archive) || is_dir(archive)) return refuse(utf8(archive) + " is not there any more.");
    const std::string name = utf8(archive.substr(archive.find_last_of(L"\\/") + 1));
    if (!magic_matches_extension(archive, name))
        return refuse(name + " does not look like a .zip, .7z or .rar file (its contents do not match its name). EMF "
                              "does not unpack it.");

    // 1. the listing
    const Run list = run(tar, L"-tvf \"" + archive + L"\"", limits.timeout_ms, nullptr);
    if (!list.started) return refuse("Could not start tar.exe: " + windows_error_text(list.start_error));
    if (list.timed_out) return refuse("tar.exe took too long to read " + name + ".");
    if (to_utf8_text(list.err).find("unreadable filename") != std::string::npos)
        return refuse("Windows' tar cannot read the file names in " + name + " (non-Latin characters); unpack it "
                      "yourself and install the folder, or ask the author to repack it.");
    if (list.exit_code != 0)
        return refuse("tar.exe could not read " + name + (list.err.empty() ? "." : ": " + tar_words(list.err)) +
                      " Is it a .zip, .7z or .rar file, and not password-protected?");
    const auto entries = parse_tar_listing(list.out);
    if (entries.empty()) return refuse(name + " is empty.");
    if (const std::string why = check_listing(entries, limits); !why.empty())
        return refuse("EMF does not install " + name + ": " + why + ".");

    // 2. unpacking, watched: a total above the limit ends tar. What tar wrote is counted as it writes (an archive whose
    // header gives no size, such as some RAR5 files, passes the listing check); the folder is walked too, in case
    // Windows gave no count. 1 MB more is allowed for tar's own messages.
    bool too_big = false;
    const Run x = run(tar, L"-x -f \"" + archive + L"\" -C \"" + dest + L"\"", limits.timeout_ms, [&] {
        too_big = tree_bytes(dest, limits.max_bytes) > limits.max_bytes;
        return !too_big;
    }, limits.max_bytes + (1u << 20));
    if (!x.started) return refuse("Could not start tar.exe: " + windows_error_text(x.start_error));
    if (too_big || x.wrote_too_much) return refuse("EMF does not install " + name + ": it unpacks to more than " + size_text(limits.max_bytes) + ".");
    if (x.timed_out) return refuse("tar.exe took too long to unpack " + name + ".");
    if (x.exit_code != 0)
        return refuse("tar.exe could not unpack " + name + (x.err.empty() ? "." : ": " + tar_words(x.err)));

    // 3. what is really there
    std::string why;
    if (!list_tree(dest, u.files, u.bytes, why, limits.max_entries)) return refuse("EMF does not install " + name + ": " + why + ".");
    if (u.bytes > limits.max_bytes) return refuse("EMF does not install " + name + ": it unpacks to more than " + size_text(limits.max_bytes) + ".");
    if (u.files.empty()) return refuse(name + " has no files in it.");
    u.ok = true;
    return u;
}

bool delete_tree(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(lp(dir).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(lp(dir).c_str()) || DeleteFileW(lp(dir).c_str());
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(lp(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(lp(dir).c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(lp(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                // the link itself, never what it points to
                if (!((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(lp(p).c_str()) : DeleteFileW(lp(p).c_str()))) {
                    SetFileAttributesW(lp(p).c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(lp(p).c_str());
                }
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(p);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(lp(p).c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(lp(p).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(lp(dir).c_str()) != 0 || !exists(dir);
}

std::wstring new_unpack_folder() {
    const std::wstring root = temp_root();
    if (root.empty()) return {};
    CreateDirectoryW(root.c_str(), nullptr);
    for (unsigned n = 1; n < 100000; ++n) {
        const std::wstring d = root + L"\\" + std::to_wstring(n);
        if (CreateDirectoryW(d.c_str(), nullptr)) return d;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
    }
    return {};
}

void clean_old_unpack_folders() {
    const std::wstring root = temp_root();
    if (root.empty()) return;
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    const ULONGLONG now = (static_cast<ULONGLONG>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    const ULONGLONG day = 24ull * 3600 * 10000000;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (name.find_first_not_of(L"0123456789") != std::wstring::npos) continue;  // only EMF's numbered folders
        const ULONGLONG made = (static_cast<ULONGLONG>(fd.ftCreationTime.dwHighDateTime) << 32) | fd.ftCreationTime.dwLowDateTime;
        if (now > made && now - made > day) delete_tree(root + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string file_sha256(const std::wstring& path) {
    HANDLE f = CreateFileW(lp(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0));
        unsigned char digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof digest, 0))) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char b : digest) {
                out += hex[b >> 4];
                out += hex[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

}  // namespace emf::app
