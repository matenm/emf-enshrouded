// Save backups, edge cases: one starter (later attempts are automatic backups), retention that counts only
// complete backups, a before-restore backup that must hold every file it replaces, listing errors, sharing with the
// game's own writes, leftover restore files, a failure part-way through a restore, the store lock's timeout, Steam names
// per account, a junction inside a backup, and paths in what EMF says.
#include <windows.h>

#include <doctest.h>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "save_test_files.h"
#include "saves/backup.h"
#include "saves/save_info.h"

using namespace emf::saves;
using save_test::FakePc;

namespace {

struct Clock {
    int64_t t = 1800000000;
    std::function<int64_t()> fn() {
        return [this] { return t; };
    }
};

BackupOptions opts(const char* kind, Clock& c, const char* label = "") {
    BackupOptions o;
    o.kind = kind;
    o.label = label;
    o.made_by = "test";
    o.now = c.fn();
    o.retry_ms = 1;
    return o;
}

// Holds a save file the way another program would: no sharing at all (share 0), or sharing reads only.
struct Hold {
    HANDLE h = INVALID_HANDLE_VALUE;
    Hold(const std::wstring& path, DWORD share) {
        h = CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
    }
    ~Hold() { release(); }
    void release() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
};

size_t count_kind(const std::wstring& store, const char* kind) {
    size_t n = 0;
    for (const auto& b : list_backups(store)) n += b.kind == kind;
    return n;
}

}  // namespace

TEST_CASE("saves fix: once a starter exists, even one with problems, later attempts are automatic backups") {
    FakePc pc(L"fix-starter");
    pc.usual();
    Clock c;
    {
        Hold lock(pc.remote() + L"\\world0.ksd", 0);
        const auto r = ensure_starter(pc.roots, opts("manual", c));
        REQUIRE(r.ok);
        CHECK(r.backup.kind == "starter");
        CHECK(r.backup.problems.size() == 1);
    }
    CHECK(has_starter(pc.roots.store));  // it counts: EMF never makes a second "Before EMF"
    c.t += 10;
    auto again = ensure_starter(pc.roots, opts("manual", c));
    CHECK(again.ok);
    CHECK(again.nothing);
    AutoSettings s;
    s.keep = 10;
    s.min_minutes = 60;
    // the game's start, and EMF.exe's: an automatic backup now holds the whole set
    auto a = auto_backup(pc.roots, s, opts("auto", c));
    REQUIRE(a.what == AutoResult::What::Made);
    CHECK(a.made.backup.kind == "auto");
    CHECK(a.made.backup.label != kStarterLabel);
    CHECK(a.made.backup.problems.empty());
    CHECK(a.made.backup.files.size() == 8);
    // and the next starts follow the automatic rules: the interval, then unchanged saves
    c.t += 60;
    a = auto_backup(pc.roots, s, opts("auto", c));
    CHECK(a.what == AutoResult::What::Skipped);
    c.t += 7200;
    a = auto_backup(pc.roots, s, opts("auto", c));
    CHECK(a.what == AutoResult::What::Skipped);
    CHECK(a.text.find("unchanged") != std::string::npos);
    CHECK(count_kind(pc.roots.store, "starter") == 1);
    CHECK(count_kind(pc.roots.store, "auto") == 1);
}

TEST_CASE("saves fix: retention counts only complete automatic backups, and a new incomplete one prunes nothing") {
    FakePc pc(L"fix-retention");
    pc.usual();
    Clock c;
    REQUIRE(make_backup(pc.roots, opts("starter", c, "Before EMF")).ok);
    auto make_auto = [&](bool locked, const char* label) {
        c.t += 3600;
        FakePc::write(pc.local_default() + L"\\world0.ksd", std::string("world at ") + label);
        std::unique_ptr<Hold> lock;
        if (locked) lock = std::make_unique<Hold>(pc.remote() + L"\\character0.ksd", 0);
        const auto r = make_backup(pc.roots, opts("auto", c, label));
        REQUIRE(r.ok);
        CHECK(r.backup.problems.empty() == !locked);
    };
    make_auto(false, "A1");
    make_auto(false, "A2");
    make_auto(true, "I3");
    make_auto(true, "I4");
    CHECK(prune_auto(pc.roots.store, 2) == 0);  // two complete ones: nothing goes, the incomplete ones are newer
    make_auto(false, "A5");
    CHECK(prune_auto(pc.roots.store, 2) == 1);  // A1, past the second complete one
    std::vector<std::string> labels;
    for (const auto& b : list_backups(pc.roots.store))
        if (b.kind == "auto") labels.push_back(b.label);
    CHECK(labels == std::vector<std::string>{"A5", "I4", "I3", "A2"});
    make_auto(false, "A6");
    CHECK(prune_auto(pc.roots.store, 2) == 3);  // I4, I3 (older than the second complete one) and A2
    labels.clear();
    for (const auto& b : list_backups(pc.roots.store))
        if (b.kind == "auto") labels.push_back(b.label);
    CHECK(labels == std::vector<std::string>{"A6", "A5"});

    // through auto_backup: a new backup with problems never prunes
    AutoSettings s;
    s.keep = 1;
    s.min_minutes = 0;
    c.t += 3600;
    FakePc::write(pc.local_default() + L"\\world0.ksd", "world, later");
    Hold lock(pc.remote() + L"\\character0.ksd", 0);
    const auto a = auto_backup(pc.roots, s, opts("auto", c));
    REQUIRE(a.what == AutoResult::What::Made);
    CHECK_FALSE(a.made.backup.problems.empty());
    CHECK(a.pruned == 0);
    CHECK(count_kind(pc.roots.store, "auto") == 3);
}

TEST_CASE("saves fix: restore is refused when the before-restore backup misses a file it would replace") {
    FakePc pc(L"fix-before");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    FakePc::write(pc.local_default() + L"\\world0.ksd", "later");
    // something that is not a file now stands where character1.ksd was: EMF cannot keep a copy of it
    std::filesystem::remove(pc.local_default() + L"\\character1.ksd");
    std::filesystem::create_directories(pc.local_default() + L"\\character1.ksd");
    const auto snap = pc.snapshot();
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.paths = {"local/default/world0.ksd", "local/default/character1.ksd"};
    ro.busy = [] { return std::string(); };
    ro.now = c.fn();
    const auto r = restore_backup(pc.roots, ro);
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("character1.ksd") != std::string::npos);
    CHECK(r.error.find("restored nothing") != std::string::npos);
    CHECK(pc.snapshot() == snap);
}

TEST_CASE("saves fix: a save folder that cannot be listed to the end is an error, not a shorter backup") {
    FakePc pc(L"fix-list");
    pc.usual();
    Clock c;
    testing::set_list_fault([](const std::wstring& dir, size_t index) {
        return dir.find(L"\\default") != std::wstring::npos && index == 1;
    });
    std::string error;
    const auto files = find_saves(pc.roots, &error);
    const auto r = make_backup(pc.roots, opts("manual", c));
    testing::set_list_fault(nullptr);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("could not list") != std::string::npos);
    CHECK(list_backups(pc.roots.store).empty());
    // without the fault: the whole set, no error
    error = "unchanged";
    CHECK(find_saves(pc.roots, &error).size() == 8);
    CHECK(error.empty());
}

TEST_CASE("saves fix: the game can rewrite or replace a save while EMF reads it") {
    FakePc pc(L"fix-share");
    pc.usual();
    Clock c;
    const std::wstring target = pc.local_default() + L"\\world0.ksd";
    const std::wstring tmp = pc.local_default() + L"\\world0.ksd.game-tmp";
    BOOL replaced = FALSE, rewritten = FALSE;
    auto o = opts("manual", c);
    // while EMF's handle is open (one read): the game or Steam can still open the file to write it
    o.hooks.opened = [&](const std::wstring& s, int attempt) {
        if (s != target || attempt != 0) return;
        HANDLE w = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        rewritten = w != INVALID_HANDLE_VALUE;
        if (rewritten) CloseHandle(w);
    };
    // Windows refuses to rename a file over one that any program holds open, so EMF closes its handle right after the
    // read: from then on a save written to a new file and renamed over this one works, and EMF sees the change
    o.hooks.read_done = [&](const std::wstring& s, int attempt) {
        if (s != target || attempt != 0) return;
        FakePc::write(tmp, "world saved by the game");
        replaced = MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    };
    const auto r = make_backup(pc.roots, o);
    CHECK(rewritten);
    CHECK(replaced);
    REQUIRE(r.ok);
    CHECK(r.backup.problems.empty());  // the change was seen and the file copied again
    for (const auto& f : r.backup.files)
        if (f.path == "local/default/world0.ksd") CHECK(f.size == std::string("world saved by the game").size());
}

TEST_CASE("saves fix: a restore removes EMF's own leftover temporary files, and nothing else") {
    FakePc pc(L"fix-leftover");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    const std::wstring dir = pc.local_default();
    FakePc::write(dir + L"\\world0.ksd.emf-restore-in-progress-4242", "left by a restore that was cut short");
    FakePc::write(dir + L"\\character1.ksd.emf-restore-in-progress-17", "another");
    FakePc::write(dir + L"\\world0.ksd.emf-restore-in-progress-x", "not EMF's pattern");
    FakePc::write(dir + L"\\notes.emf-restore-in-progress-9", "not a save name");
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.paths = {"local/default/world0.ksd"};
    ro.busy = [] { return std::string(); };
    ro.now = c.fn();
    REQUIRE(restore_backup(pc.roots, ro).ok);
    CHECK_FALSE(FakePc::has(dir + L"\\world0.ksd.emf-restore-in-progress-4242"));
    CHECK_FALSE(FakePc::has(dir + L"\\character1.ksd.emf-restore-in-progress-17"));
    CHECK(FakePc::has(dir + L"\\world0.ksd.emf-restore-in-progress-x"));
    CHECK(FakePc::has(dir + L"\\notes.emf-restore-in-progress-9"));
}

TEST_CASE("saves fix: a restore that fails part-way says what it did and leaves no temporary file") {
    FakePc pc(L"fix-partway");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    FakePc::write(pc.local_default() + L"\\world0.ksd", "world later");
    FakePc::write(pc.local_default() + L"\\character0.ksd", "character later");
    // another program holds character0.ksd open without sharing delete: EMF can read it, but not replace it
    Hold hold(pc.local_default() + L"\\character0.ksd", FILE_SHARE_READ);
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.paths = {"local/default/world0.ksd", "local/default/character0.ksd"};
    ro.busy = [] { return std::string(); };
    ro.now = c.fn();
    const auto r = restore_backup(pc.roots, ro);
    hold.release();
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("character0.ksd") != std::string::npos);
    CHECK(r.error.find("Restored before that") != std::string::npos);
    CHECK_FALSE(r.before_id.empty());
    CHECK(r.error.find(r.before_id) != std::string::npos);
    CHECK(FakePc::read(pc.local_default() + L"\\world0.ksd") == "local world 0");
    CHECK(FakePc::read(pc.local_default() + L"\\character0.ksd") == "character later");
    for (const auto& [rel, v] : pc.snapshot()) CHECK(rel.find(L"emf-restore") == std::wstring::npos);
}

TEST_CASE("saves fix: when another EMF program holds the store, EMF waits, then refuses plainly") {
    FakePc pc(L"fix-lock");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    testing::set_store_lock_timeout_ms(50);
    // another EMF program (here a thread that stays alive) holds the store's lock
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr), done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread holder([&] {
        HANDLE m = CreateMutexW(nullptr, TRUE, store_lock_name(pc.roots.store).c_str());
        SetEvent(ready);
        WaitForSingleObject(done, 10000);
        ReleaseMutex(m);
        CloseHandle(m);
    });
    WaitForSingleObject(ready, 10000);
    const auto snap_store = pc.store_entries();
    const auto mk = make_backup(pc.roots, opts("manual", c));
    const auto del = delete_backup(pc.roots.store, made.backup.id);
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.busy = [] { return std::string(); };
    const auto rs = restore_backup(pc.roots, ro);
    const size_t pruned = prune_auto(pc.roots.store, 1);
    SetEvent(done);
    holder.join();
    CloseHandle(ready);
    CloseHandle(done);
    testing::set_store_lock_timeout_ms(0);
    CHECK_FALSE(mk.ok);
    CHECK(mk.error.find("Another EMF program") != std::string::npos);
    CHECK_FALSE(del.ok);
    CHECK(del.error.find("Another EMF program") != std::string::npos);
    CHECK_FALSE(rs.ok);
    CHECK(rs.error.find("Another EMF program") != std::string::npos);
    CHECK(pruned == 0);
    CHECK(pc.store_entries() == snap_store);
    // free again: it works
    CHECK(delete_backup(pc.roots.store, made.backup.id).ok);
}

TEST_CASE("saves fix: the store lock's name is the same in C++ and in the installer's PowerShell") {
    // package\emf_common.ps1 (Get-SaveStoreMutexName) gives the same name for this path; test_package checks it too
    CHECK(store_lock_name(L"D:\\Data\\EMF\\save-backups") == L"Local\\EMF.SaveBackups.72b0c584");
    CHECK(store_lock_name(L"d:\\data\\emf\\save-backups") ==
          store_lock_name(L"D:\\DATA\\EMF\\SAVE-BACKUPS"));
}

TEST_CASE("saves fix: Steam names belong to one Steam account") {
    FakePc pc(L"fix-steam-names");
    pc.usual();
    FakePc::write(pc.remote(L"222") + L"\\character0.ksd", "another account's character");
    Clock c;
    auto o = opts("manual", c);
    SlotInfo mine;
    mine.storage = "steam";
    mine.steam_id = "111";
    mine.type = "character";
    mine.slot = 0;
    mine.name = "Astrid";
    mine.level = 8;
    o.info = {mine};
    const auto r = make_backup(pc.roots, o);  // two Steam users: the name is used for 111's file only
    REQUIRE(r.ok);
    REQUIRE(r.backup.info.size() == 1);
    CHECK(r.backup.info[0].steam_id == "111");
    for (const auto& f : r.backup.files) {
        if (f.path == "steam/111/character0.ksd") CHECK(file_label(r.backup, f) == "Astrid (level 8)");
        if (f.path == "steam/222/character0.ksd") CHECK(file_label(r.backup, f) == "character slot 1");
    }
    // in save-info.json, two accounts' slot 0 are two entries
    SlotSeen a;
    a.storage = "steam";
    a.steam_id = "111";
    a.type = "character";
    a.slot = 0;
    a.state = 2;
    a.name = "Astrid";
    SlotSeen b = a;
    b.steam_id = "222";
    b.name = "Brynja";
    SaveInfo info = merge_seen({}, {a}, 100, nullptr);
    info = merge_seen(info, {b}, 200, nullptr);
    REQUIRE(info.slots.size() == 2);
    const auto back = parse_save_info(save_info_json(info));
    REQUIRE(back);
    CHECK(back->slots[0].steam_id != back->slots[1].steam_id);
    // a manifest's Steam info names its account
    const auto parsed = parse_manifest(manifest_json(r.backup));
    REQUIRE(parsed);
    CHECK(parsed->info[0].steam_id == "111");
}

TEST_CASE("saves fix: deleting a backup never follows a junction inside it") {
    FakePc pc(L"fix-junction");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    const std::wstring outside = pc.root + L"\\outside";
    FakePc::write(outside + L"\\precious.txt", "keep me");
    const std::wstring link = backup_folder(pc.roots.store, made.backup.id) + L"\\jn";
    const std::wstring cmd = L"cmd /c mklink /J \"" + link + L"\" \"" + outside + L"\" >nul";
    REQUIRE(_wsystem(cmd.c_str()) == 0);
    REQUIRE(delete_backup(pc.roots.store, made.backup.id).ok);
    CHECK(list_backups(pc.roots.store).empty());
    CHECK(FakePc::read(outside + L"\\precious.txt") == "keep me");
}

TEST_CASE("saves fix: paths in what EMF says use %LOCALAPPDATA% and %USERPROFILE%") {
    wchar_t local[MAX_PATH] = {}, profile[MAX_PATH] = {};
    REQUIRE(GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0);
    REQUIRE(GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0);
    CHECK(display_path(std::wstring(local) + L"\\EMF\\save-backups") == "%LOCALAPPDATA%\\EMF\\save-backups");
    CHECK(display_path(std::wstring(profile) + L"\\Saved Games\\x") == "%USERPROFILE%\\Saved Games\\x");
    CHECK(display_path(L"D:\\Elsewhere\\save-backups") == "D:\\Elsewhere\\save-backups");
}

TEST_CASE("saves fix: a Steam name recorded with its account replaces the one recorded before accounts were") {
    SlotSeen old;  // as an earlier EMF wrote it: no account
    old.storage = "steam";
    old.type = "character";
    old.slot = 0;
    old.state = 2;
    old.name = "Astrid";
    old.level = 7;
    SaveInfo info = merge_seen({}, {old}, 100, nullptr);
    REQUIRE(info.slots.size() == 1);
    SlotSeen now = old;
    now.steam_id = "111";
    now.level = 8;
    bool changed = false;
    info = merge_seen(info, {now}, 200, &changed);
    CHECK(changed);
    REQUIRE(info.slots.size() == 1);  // no second, account-less entry for the same slot
    CHECK(info.slots[0].steam_id == "111");
    CHECK(info.slots[0].level == 8);
}
