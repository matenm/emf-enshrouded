// Save backups (src/saves/backup.h) against a fake PC in %TEMP%: which files, the manifest, copy into a temporary folder
// then rename, hashes, a locked or changing file, the starter and automatic backups with retention, restore (before-
// restore first, a subset, hash checks, refused while the game runs), delete, and that nothing is ever written into a
// save folder except by restore.
#include <windows.h>

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "common/sha256.h"
#include "save_test_files.h"
#include "saves/backup.h"

using namespace emf::saves;
using save_test::FakePc;

namespace {

const SaveFile* find(const std::vector<SaveFile>& v, const std::string& path) {
    for (const auto& f : v)
        if (f.path == path) return &f;
    return nullptr;
}
const BackupFile* find(const Backup& b, const std::string& path) {
    for (const auto& f : b.files)
        if (f.path == path) return &f;
    return nullptr;
}

// A clock the test moves on.
struct Clock {
    int64_t t = 1800000000;  // 2027-01-15
    std::function<int64_t()> fn() {
        return [this] { return t; };
    }
};

BackupOptions opts(const char* kind, Clock& c, const char* label = "") {
    BackupOptions o;
    o.kind = kind;
    o.label = label;
    o.made_by = "test";
    o.emf_version = "9.9.9";
    o.now = c.fn();
    o.retry_ms = 1;
    return o;
}

}  // namespace

TEST_CASE("saves: find_saves takes both storages' save files and nothing else") {
    FakePc pc(L"find");
    pc.usual();
    FakePc::write(pc.remote() + L"\\notes.txt", "not a save");
    FakePc::write(pc.remote(L"222") + L"\\character3.ksd", "second Steam user");
    FakePc::write(pc.roots.steam + L"\\userdata\\333\\1203620\\other.txt", "no remote folder");
    const auto files = find_saves(pc.roots);
    std::vector<std::string> paths;
    for (const auto& f : files) paths.push_back(f.path);
    CHECK(paths == std::vector<std::string>{
                       "steam/111/character0.ksd", "steam/111/world0.ksd", "steam/111/global.ksd",
                       "steam/111/user_settings.json", "steam/222/character3.ksd", "local/default/character0.ksd",
                       "local/default/character1.ksd", "local/default/world0.ksd", "local/local_settings.json"});
    const SaveFile* w = find(files, "steam/111/world0.ksd");
    REQUIRE(w);
    CHECK(w->storage == "steam");
    CHECK(w->steam_id == "111");
    CHECK(w->type == "world");
    CHECK(w->slot == 0);
    CHECK(w->size == std::string("steam world 0, a bigger file").size());
    CHECK(w->modified > 0);
    const SaveFile* c = find(files, "local/default/character1.ksd");
    REQUIRE(c);
    CHECK(c->storage == "local");
    CHECK(c->type == "character");
    CHECK(c->slot == 1);
    CHECK(find(files, "local/local_settings.json")->type == "local_settings");
    CHECK(find(files, "steam/111/global.ksd")->slot == -1);
    CHECK(steam_users_with_saves(pc.roots) == std::vector<std::string>{"111", "222"});
}

TEST_CASE("saves: no roots, no saves") {
    SaveRoots none;
    CHECK(find_saves(none).empty());
    FakePc pc(L"empty");
    CHECK(find_saves(pc.roots).empty());
    Clock c;
    const auto r = make_backup(pc.roots, opts("manual", c));
    CHECK_FALSE(r.ok);
    CHECK(r.nothing);
    CHECK(pc.store_entries().empty());
}

TEST_CASE("saves: the manifest round trip") {
    Backup b;
    b.kind = "before-restore";
    b.label = "Vor dem Zurückspielen \"quoted\" \\ line\nbreak";
    b.emf_version = "0.3.0";
    b.made_by = "EMF.exe";
    b.time = 1800000123;
    b.time_local = "2027-01-15 09:48:43";
    BackupFile f;
    f.storage = "steam";
    f.steam_id = "76561198000000000";
    f.type = "character";
    f.slot = 3;
    f.name = "character3.ksd";
    f.path = "steam/76561198000000000/character3.ksd";
    f.size = 31475;
    f.sha256 = std::string(64, 'a');
    f.modified = 1799999000;
    b.files.push_back(f);
    SlotInfo i;
    i.storage = "steam";
    i.type = "character";
    i.slot = 3;
    i.name = "Björn";
    i.level = 8;
    i.seen = 1799990000;
    i.last_loaded = 1799991000;
    b.info.push_back(i);
    SlotInfo w;
    w.storage = "local";
    w.type = "world";
    w.slot = 0;
    w.name = "My World 1";
    b.info.push_back(w);
    b.problems.push_back("world1.ksd (Local): it is in use by another program");

    std::string error;
    const auto back = parse_manifest(manifest_json(b), &error);
    REQUIRE_MESSAGE(back, error);
    CHECK(back->format == kManifestFormat);
    CHECK(back->kind == b.kind);
    CHECK(back->label == b.label);
    CHECK(back->emf_version == "0.3.0");
    CHECK(back->made_by == "EMF.exe");
    CHECK(back->time == b.time);
    CHECK(back->time_local == b.time_local);
    REQUIRE(back->files.size() == 1);
    const auto& g = back->files[0];
    CHECK(g.storage == "steam");
    CHECK(g.steam_id == f.steam_id);
    CHECK(g.type == "character");
    CHECK(g.slot == 3);
    CHECK(g.name == f.name);
    CHECK(g.path == f.path);
    CHECK(g.size == 31475);
    CHECK(g.sha256 == f.sha256);
    CHECK(g.modified == f.modified);
    REQUIRE(back->info.size() == 2);
    CHECK(back->info[0].name == "Björn");
    CHECK(back->info[0].level == 8);
    CHECK(back->info[0].seen == 1799990000);
    CHECK(back->info[0].last_loaded == 1799991000);
    CHECK_FALSE(back->info[1].level.has_value());
    CHECK(back->problems == b.problems);
    CHECK(back->bytes() == 31475);

    CHECK_FALSE(parse_manifest("{\"format\":1}", &error));  // no kind, no files
    CHECK_FALSE(parse_manifest("not json", &error));
    CHECK_FALSE(parse_manifest(R"({"format":1,"kind":"weird","time":1,"files":[]})", &error));
    // a file path that leaves the backup folder is refused
    CHECK_FALSE(parse_manifest(
        R"({"format":1,"kind":"manual","time":1,"files":[{"storage":"local","type":"world","slot":0,"name":"world0.ksd","path":"../world0.ksd","size":1,"sha256":"aa"}]})",
        &error));
}

TEST_CASE("saves: a backup is copied into a temporary folder, checked, then renamed into place") {
    FakePc pc(L"make");
    pc.usual();
    Clock c;
    const auto before = pc.snapshot();
    auto o = opts("manual", c, "my label");
    int seen_tmp = 0, listed_meanwhile = 0;
    o.hooks.opened = [&](const std::wstring&, int) {
        for (const auto& e : pc.store_entries())
            if (e.rfind(L".tmp-", 0) == 0) ++seen_tmp;
        listed_meanwhile += static_cast<int>(list_backups(pc.roots.store).size());
    };
    const auto r = make_backup(pc.roots, o);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(seen_tmp == 8);          // every file was copied while only the temporary folder existed
    CHECK(listed_meanwhile == 0);  // and the list showed nothing half done
    REQUIRE(pc.store_entries().size() == 1);  // named after the local time and the kind
    CHECK(pc.store_entries()[0].size() == 26);
    CHECK(pc.store_entries()[0].substr(19) == L"_manual");
    const auto& b = r.backup;
    CHECK(b.kind == "manual");
    CHECK(b.label == "my label");
    CHECK(b.made_by == "test");
    CHECK(b.emf_version == "9.9.9");
    CHECK(b.time == c.t);
    CHECK(b.files.size() == 8);
    CHECK(b.problems.empty());
    // each copy is the file, and the manifest has its hash
    const std::wstring dir = backup_folder(pc.roots.store, b.id);
    REQUIRE_FALSE(dir.empty());
    for (const auto& f : b.files) {
        std::wstring rel(f.path.begin(), f.path.end());
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const std::wstring copy = dir + L"\\" + rel;
        CHECK(FakePc::has(copy));
        CHECK(emf::sha256_file(copy) == f.sha256);
        CHECK(FakePc::read(copy).size() == f.size);
    }
    CHECK(find(b, "steam/111/world0.ksd")->sha256 == emf::sha256_file(pc.remote() + L"\\world0.ksd"));
    CHECK(FakePc::read(dir + L"\\local\\local_settings.json") == "{\"windowMode\":\"Fullscreen\"}");
    CHECK_FALSE(FakePc::has(dir + L"\\steam\\111\\backup"));  // never the game's own backups
    // the manifest on disk reads back the same
    const auto again = read_backup(pc.roots.store, b.id);
    REQUIRE(again);
    CHECK(again->files.size() == 8);
    CHECK(again->label == "my label");
    // nothing was written into the save folders
    CHECK(pc.snapshot() == before);
    // listed, newest first
    c.t += 5;
    REQUIRE(make_backup(pc.roots, opts("manual", c, "second")).ok);
    const auto list = list_backups(pc.roots.store);
    REQUIRE(list.size() == 2);
    CHECK(list[0].label == "second");
    CHECK(list[1].label == "my label");
    // two in the same second get different folders
    REQUIRE(make_backup(pc.roots, opts("manual", c, "third")).ok);
    CHECK(list_backups(pc.roots.store).size() == 3);
}

TEST_CASE("saves: a locked file is tried once more, then named as a problem") {
    FakePc pc(L"locked");
    pc.usual();
    Clock c;
    const std::wstring target = pc.local_default() + L"\\world0.ksd";
    HANDLE lock = INVALID_HANDLE_VALUE;
    SUBCASE("locked for good: the backup has the other files and names this one") {
        lock = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(lock != INVALID_HANDLE_VALUE);
        int tries = 0;
        auto o = opts("manual", c);
        o.hooks.before_open = [&](const std::wstring& s, int) {
            if (s == target) ++tries;
        };
        const auto r = make_backup(pc.roots, o);
        CloseHandle(lock);
        REQUIRE(r.ok);
        CHECK(tries == 2);
        CHECK(r.backup.files.size() == 7);
        REQUIRE(r.backup.problems.size() == 1);
        CHECK(r.backup.problems[0].find("world0.ksd") != std::string::npos);
        CHECK(r.backup.problems[0].find("Local") != std::string::npos);
        const auto back = read_backup(pc.roots.store, r.backup.id);
        REQUIRE(back);
        CHECK(back->problems.size() == 1);
    }
    SUBCASE("locked only for the first try: copied") {
        lock = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(lock != INVALID_HANDLE_VALUE);
        auto o = opts("manual", c);
        o.hooks.before_open = [&](const std::wstring& s, int attempt) {
            if (s == target && attempt == 1 && lock != INVALID_HANDLE_VALUE) {
                CloseHandle(lock);
                lock = INVALID_HANDLE_VALUE;
            }
        };
        const auto r = make_backup(pc.roots, o);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        REQUIRE(r.ok);
        CHECK(r.backup.files.size() == 8);
        CHECK(r.backup.problems.empty());
    }
}

TEST_CASE("saves: a file that changes while it is copied is copied again, or named") {
    FakePc pc(L"changing");
    pc.usual();
    Clock c;
    const std::wstring target = pc.remote() + L"\\character0.ksd";
    // the game writes the file while EMF reads it (it opens with every share flag, so the game's write never fails)
    auto change = [&](int n) {
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        REQUIRE(h != INVALID_HANDLE_VALUE);
        SetFilePointer(h, 0, nullptr, FILE_END);
        const std::string more = " written by the game " + std::to_string(n);
        DWORD put = 0;
        WriteFile(h, more.data(), static_cast<DWORD>(more.size()), &put, nullptr);
        CloseHandle(h);
    };
    SUBCASE("changed during the first try only") {
        auto o = opts("manual", c);
        o.hooks.opened = [&](const std::wstring& s, int attempt) {
            if (s == target && attempt == 0) change(0);
        };
        const auto r = make_backup(pc.roots, o);
        REQUIRE(r.ok);
        CHECK(r.backup.problems.empty());
        const BackupFile* f = find(r.backup, "steam/111/character0.ksd");
        REQUIRE(f);
        CHECK(f->sha256 == emf::sha256_file(target));  // the second copy, of the file as it is now
        CHECK(f->size == FakePc::read(target).size());
    }
    SUBCASE("changing every time") {
        auto o = opts("manual", c);
        int n = 0;
        o.hooks.opened = [&](const std::wstring& s, int) {
            if (s == target) change(++n);
        };
        const auto r = make_backup(pc.roots, o);
        REQUIRE(r.ok);
        CHECK(n == 2);
        CHECK_FALSE(find(r.backup, "steam/111/character0.ksd"));
        REQUIRE(r.backup.problems.size() == 1);
        CHECK(r.backup.problems[0].find("character0.ksd") != std::string::npos);
        CHECK(r.backup.problems[0].find("changed") != std::string::npos);
    }
}

TEST_CASE("saves: the starter backup is made once, labelled Before EMF") {
    FakePc pc(L"starter");
    Clock c;
    auto r = ensure_starter(pc.roots, opts("manual", c));
    CHECK(r.ok);
    CHECK(r.nothing);  // no saves yet: nothing to do, no backup
    CHECK_FALSE(has_starter(pc.roots.store));
    pc.usual();
    r = ensure_starter(pc.roots, opts("manual", c));
    REQUIRE(r.ok);
    CHECK_FALSE(r.nothing);
    CHECK(r.backup.kind == "starter");
    CHECK(r.backup.label == "Before EMF");
    CHECK(has_starter(pc.roots.store));
    c.t += 10;
    r = ensure_starter(pc.roots, opts("manual", c));
    CHECK(r.ok);
    CHECK(r.nothing);  // there is one already
    CHECK(list_backups(pc.roots.store).size() == 1);
}

TEST_CASE("saves: a starter backup with problems still counts: no second starter is made") {
    FakePc pc(L"starter-incomplete");
    pc.usual();
    Clock c;
    const std::wstring target = pc.remote() + L"\\world0.ksd";
    HANDLE lock = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(lock != INVALID_HANDLE_VALUE);
    auto r = ensure_starter(pc.roots, opts("manual", c));
    CloseHandle(lock);
    REQUIRE(r.ok);
    CHECK(r.backup.problems.size() == 1);
    CHECK(has_starter(pc.roots.store));
    c.t += 10;
    r = ensure_starter(pc.roots, opts("manual", c));
    CHECK(r.ok);
    CHECK(r.nothing);  // later attempts are automatic backups (test_save_fixes.cpp)
    CHECK(list_backups(pc.roots.store).size() == 1);
}

TEST_CASE("saves: retention deletes only the oldest automatic backups") {
    FakePc pc(L"prune");
    pc.usual();
    Clock c;
    REQUIRE(make_backup(pc.roots, opts("starter", c, "Before EMF")).ok);
    c.t += 60;
    REQUIRE(make_backup(pc.roots, opts("manual", c, "mine")).ok);
    for (int i = 0; i < 5; ++i) {
        c.t += 3600;
        REQUIRE(make_backup(pc.roots, opts("auto", c, ("auto " + std::to_string(i)).c_str())).ok);
    }
    c.t += 60;
    REQUIRE(make_backup(pc.roots, opts("before-restore", c)).ok);
    CHECK(prune_auto(pc.roots.store, 2) == 3);
    std::vector<std::string> left;
    for (const auto& b : list_backups(pc.roots.store)) left.push_back(b.kind + ":" + b.label);
    CHECK(left == std::vector<std::string>{"before-restore:", "auto:auto 4", "auto:auto 3", "manual:mine", "starter:Before EMF"});
    CHECK(prune_auto(pc.roots.store, 0) == 1);  // at least one automatic backup stays
    CHECK(prune_auto(pc.roots.store, 1) == 0);
}

TEST_CASE("saves: automatic backups at game start") {
    FakePc pc(L"auto");
    Clock c;
    AutoSettings s;
    s.keep = 2;
    s.min_minutes = 60;
    auto base = opts("auto", c);
    // no saves yet
    auto a = auto_backup(pc.roots, s, base);
    CHECK(a.what == AutoResult::What::Skipped);
    pc.usual();
    // the first one is the starter
    a = auto_backup(pc.roots, s, base);
    REQUIRE(a.what == AutoResult::What::Made);
    CHECK(a.made.backup.kind == "starter");
    // nothing changed since the newest backup (of any kind): skipped, and said so
    c.t += 7200;
    a = auto_backup(pc.roots, s, base);
    CHECK(a.what == AutoResult::What::Skipped);
    CHECK(a.text.find("saves unchanged since") != std::string::npos);
    CHECK(a.text.find("no new backup") != std::string::npos);
    CHECK(list_backups(pc.roots.store).size() == 1);
    // a save changed: an automatic backup
    FakePc::write(pc.local_default() + L"\\world0.ksd", "local world 0, played");
    a = auto_backup(pc.roots, s, base);
    REQUIRE(a.what == AutoResult::What::Made);
    CHECK(a.made.backup.kind == "auto");
    // changed again, but the last automatic one is younger than min_minutes: skipped
    c.t += 600;
    FakePc::write(pc.local_default() + L"\\world0.ksd", "local world 0, played more");
    a = auto_backup(pc.roots, s, base);
    CHECK(a.what == AutoResult::What::Skipped);
    // later: made, and only `keep` automatic backups stay; the starter is never pruned
    for (int i = 0; i < 3; ++i) {
        c.t += 3601;
        FakePc::write(pc.local_default() + L"\\world0.ksd", "local world 0, round " + std::to_string(i));
        a = auto_backup(pc.roots, s, base);
        REQUIRE(a.what == AutoResult::What::Made);
    }
    size_t autos = 0, starters = 0;
    for (const auto& b : list_backups(pc.roots.store)) {
        autos += b.kind == "auto";
        starters += b.kind == "starter";
    }
    CHECK(autos == 2);
    CHECK(starters == 1);
    // switched off: nothing at all
    s.on = false;
    c.t += 7200;
    FakePc::write(pc.local_default() + L"\\world0.ksd", "changed while off");
    a = auto_backup(pc.roots, s, base);
    CHECK(a.what == AutoResult::What::Skipped);
    CHECK(a.text.find("off") != std::string::npos);
}

TEST_CASE("saves: a manual backup is made even when nothing changed") {
    FakePc pc(L"manual-same");
    pc.usual();
    Clock c;
    REQUIRE(make_backup(pc.roots, opts("manual", c)).ok);
    c.t += 1;
    REQUIRE(make_backup(pc.roots, opts("manual", c)).ok);
    CHECK(list_backups(pc.roots.store).size() == 2);
}

TEST_CASE("saves: the automatic backup settings come from emf_core.ini [backup]") {
    FakePc pc(L"ini");
    const std::wstring ini = pc.root + L"\\emf_core.ini";
    auto s = read_auto_settings(ini);  // no file: the defaults
    CHECK(s.on);
    CHECK(s.keep == 10);
    CHECK(s.min_minutes == 60);
    FakePc::write(ini, "[backup]\r\non_start=0\r\nkeep=3\r\nmin_minutes=5\r\n");
    s = read_auto_settings(ini);
    CHECK_FALSE(s.on);
    CHECK(s.keep == 3);
    CHECK(s.min_minutes == 5);
    FakePc::write(ini, "[backup]\r\nkeep=0\r\nmin_minutes=-4\r\n");  // out of range: clamped
    s = read_auto_settings(ini);
    CHECK(s.keep == 1);
    CHECK(s.min_minutes == 0);
    FakePc::write(ini, "[backup]\r\nkeep=5000\r\n");
    CHECK(read_auto_settings(ini).keep == 100);
}

TEST_CASE("saves: restore puts chosen files back after a before-restore backup") {
    FakePc pc(L"restore");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c, "good state"));
    REQUIRE(made.ok);
    // the player goes on playing: the Local world and character change, a Steam file too
    FakePc::write(pc.local_default() + L"\\world0.ksd", "local world 0, broken by a mod");
    FakePc::write(pc.local_default() + L"\\character0.ksd", "local character 0, later");
    FakePc::write(pc.remote() + L"\\world0.ksd", "steam world later");
    c.t += 120;
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.paths = {"local/default/world0.ksd"};
    ro.busy = [] { return std::string(); };
    ro.made_by = "test";
    ro.now = c.fn();
    const auto r = restore_backup(pc.roots, ro);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(FakePc::read(pc.local_default() + L"\\world0.ksd") == "local world 0");
    // only that file
    CHECK(FakePc::read(pc.local_default() + L"\\character0.ksd") == "local character 0, later");
    CHECK(FakePc::read(pc.remote() + L"\\world0.ksd") == "steam world later");
    CHECK_FALSE(r.steam);
    REQUIRE(r.restored.size() == 1);
    // the before-restore backup holds the replaced file as it was
    REQUIRE_FALSE(r.before_id.empty());
    const auto before = read_backup(pc.roots.store, r.before_id);
    REQUIRE(before);
    CHECK(before->kind == "before-restore");
    REQUIRE(before->files.size() == 1);
    CHECK(before->files[0].path == "local/default/world0.ksd");
    CHECK(FakePc::read(backup_folder(pc.roots.store, r.before_id) + L"\\local\\default\\world0.ksd") ==
          "local world 0, broken by a mod");
    // the backup restored from stays
    CHECK(read_backup(pc.roots.store, made.backup.id));
    // no temporary file was left next to the saves
    for (const auto& [rel, v] : pc.snapshot()) CHECK(rel.find(L"emf-restore") == std::wstring::npos);
}

TEST_CASE("saves: restoring the whole backup, Steam files included") {
    FakePc pc(L"restore-all");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    FakePc::write(pc.remote() + L"\\character0.ksd", "steam character later");
    std::filesystem::remove(pc.roots.local + L"\\local_settings.json");
    c.t += 60;
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.busy = [] { return std::string(); };
    ro.now = c.fn();
    const auto r = restore_backup(pc.roots, ro);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(r.steam);
    CHECK(r.restored.size() == 8);
    CHECK(FakePc::read(pc.remote() + L"\\character0.ksd") == "steam character 0");
    CHECK(FakePc::read(pc.roots.local + L"\\local_settings.json") == "{\"windowMode\":\"Fullscreen\"}");
    // the restored file is new on disk: Steam sees it as the newest change
    WIN32_FILE_ATTRIBUTE_DATA a{};
    REQUIRE(GetFileAttributesExW((pc.remote() + L"\\character0.ksd").c_str(), GetFileExInfoStandard, &a));
    const auto before = read_backup(pc.roots.store, r.before_id);
    REQUIRE(before);
    CHECK(before->files.size() == 7);  // local_settings.json was not there: nothing of it to keep
}

TEST_CASE("saves: restore is refused while the game runs, and for a damaged copy") {
    FakePc pc(L"restore-refused");
    pc.usual();
    Clock c;
    const auto made = make_backup(pc.roots, opts("manual", c));
    REQUIRE(made.ok);
    FakePc::write(pc.local_default() + L"\\world0.ksd", "later");
    const auto snap = pc.snapshot();
    const size_t backups = list_backups(pc.roots.store).size();
    RestoreOptions ro;
    ro.id = made.backup.id;
    ro.paths = {"local/default/world0.ksd"};
    ro.now = c.fn();
    SUBCASE("the game runs") {
        ro.busy = [] { return std::string("enshrouded.exe is running"); };
        const auto r = restore_backup(pc.roots, ro);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("enshrouded.exe is running") != std::string::npos);
    }
    SUBCASE("the game starts while the before-restore backup is made") {
        int calls = 0;
        ro.busy = [&] { return ++calls > 1 ? std::string("enshrouded.exe started") : std::string(); };
        const auto r = restore_backup(pc.roots, ro);
        CHECK_FALSE(r.ok);
        CHECK(calls >= 2);
        CHECK(list_backups(pc.roots.store).size() == backups + 1);  // the before-restore backup stays: it is harmless
        CHECK(pc.snapshot() == snap);
        return;
    }
    SUBCASE("the backup's copy was changed") {
        ro.busy = [] { return std::string(); };
        FakePc::write(backup_folder(pc.roots.store, made.backup.id) + L"\\local\\default\\world0.ksd", "tampered");
        const auto r = restore_backup(pc.roots, ro);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("world0.ksd") != std::string::npos);
    }
    SUBCASE("a file the backup does not have") {
        ro.busy = [] { return std::string(); };
        ro.paths = {"local/default/world7.ksd"};
        CHECK_FALSE(restore_backup(pc.roots, ro).ok);
    }
    SUBCASE("a Steam user that is not on this PC") {
        ro.busy = [] { return std::string(); };
        ro.paths = {"steam/111/world0.ksd"};
        std::filesystem::remove_all(pc.roots.steam + L"\\userdata\\111");
        const auto r = restore_backup(pc.roots, ro);
        CHECK_FALSE(r.ok);
        return;  // the snapshot differs by the removed folder
    }
    SUBCASE("an unknown backup") {
        ro.busy = [] { return std::string(); };
        ro.id = "..\\..\\x";
        CHECK_FALSE(restore_backup(pc.roots, ro).ok);
    }
    CHECK(pc.snapshot() == snap);                               // nothing changed in the saves
    CHECK(list_backups(pc.roots.store).size() == backups);      // and no before-restore backup was made
}

TEST_CASE("saves: restore targets") {
    FakePc pc(L"targets");
    pc.usual();
    BackupFile f;
    f.storage = "local";
    f.type = "world";
    f.name = "world0.ksd";
    f.path = "local/default/world0.ksd";
    CHECK(restore_target(pc.roots, f) == pc.local_default() + L"\\world0.ksd");
    f.type = "local_settings";
    f.name = "local_settings.json";
    f.path = "local/local_settings.json";
    CHECK(restore_target(pc.roots, f) == pc.roots.local + L"\\local_settings.json");
    f.storage = "steam";
    f.steam_id = "111";
    f.type = "character";
    f.name = "character0.ksd";
    f.path = "steam/111/character0.ksd";
    CHECK(restore_target(pc.roots, f) == pc.remote() + L"\\character0.ksd");
    f.steam_id = "999";  // no such Steam user here
    f.path = "steam/999/character0.ksd";
    CHECK(restore_target(pc.roots, f).empty());
    f.steam_id = "..";
    f.path = "steam/../character0.ksd";
    CHECK(restore_target(pc.roots, f).empty());
    f.steam_id = "111";
    f.name = "evil.dll";  // only save file names go back
    f.path = "steam/111/evil.dll";
    CHECK(restore_target(pc.roots, f).empty());
    SaveRoots no_steam = pc.roots;
    no_steam.steam.clear();
    f.name = "character0.ksd";
    f.path = "steam/111/character0.ksd";
    CHECK(restore_target(no_steam, f).empty());
}

TEST_CASE("saves: delete removes one backup and refuses anything else") {
    FakePc pc(L"delete");
    pc.usual();
    Clock c;
    const auto a = make_backup(pc.roots, opts("starter", c, "Before EMF"));
    c.t += 1;
    const auto b = make_backup(pc.roots, opts("manual", c));
    REQUIRE(a.ok);
    REQUIRE(b.ok);
    const auto snap = pc.snapshot();
    CHECK_FALSE(delete_backup(pc.roots.store, "").ok);
    CHECK_FALSE(delete_backup(pc.roots.store, "..").ok);
    CHECK_FALSE(delete_backup(pc.roots.store, "..\\AppData").ok);
    CHECK_FALSE(delete_backup(pc.roots.store, "x/y").ok);
    CHECK_FALSE(delete_backup(pc.roots.store, "no such backup").ok);
    CHECK(delete_backup(pc.roots.store, b.backup.id).ok);
    CHECK_FALSE(FakePc::has(pc.roots.store + L"\\" + std::wstring(b.backup.id.begin(), b.backup.id.end())));
    CHECK(list_backups(pc.roots.store).size() == 1);
    CHECK(delete_backup(pc.roots.store, a.backup.id).ok);  // the player may delete the starter too (after the box)
    CHECK(list_backups(pc.roots.store).empty());
    CHECK(pc.snapshot() == snap);
    // a damaged backup (unreadable backup.json) is listed and can be deleted
    std::filesystem::create_directories(pc.roots.store + L"\\2027-01-01_00-00-00_manual");
    FakePc::write(pc.roots.store + L"\\2027-01-01_00-00-00_manual\\backup.json", "{broken");
    const auto list = list_backups(pc.roots.store);
    REQUIRE(list.size() == 1);
    CHECK(list[0].damaged);
    CHECK(delete_backup(pc.roots.store, "2027-01-01_00-00-00_manual").ok);
    // a folder that is not a backup's (no backup.json, another name) is never deleted
    std::filesystem::create_directories(pc.roots.store + L"\\my own folder");
    CHECK_FALSE(delete_backup(pc.roots.store, "my own folder").ok);
    CHECK(FakePc::has(pc.roots.store + L"\\my own folder"));
}

TEST_CASE("saves: nothing but restore writes into the save folders") {
    FakePc pc(L"hands-off");
    pc.usual();
    Clock c;
    const auto snap = pc.snapshot();
    AutoSettings s;
    s.keep = 1;
    s.min_minutes = 0;
    REQUIRE(ensure_starter(pc.roots, opts("manual", c)).ok);
    c.t += 10;
    REQUIRE(make_backup(pc.roots, opts("manual", c)).ok);
    for (int i = 0; i < 3; ++i) {
        c.t += 10;
        auto_backup(pc.roots, s, opts("auto", c));
    }
    list_backups(pc.roots.store);
    for (const auto& b : list_backups(pc.roots.store)) backup_summary(b);
    prune_auto(pc.roots.store, 1);
    clean_stale_temp(pc.roots.store, 0);
    const auto list = list_backups(pc.roots.store);
    REQUIRE_FALSE(list.empty());
    CHECK(delete_backup(pc.roots.store, list.front().id).ok);
    CHECK(pc.snapshot() == snap);
    // and a restore while busy writes nothing either
    RestoreOptions ro;
    ro.id = list.back().id;
    ro.busy = [] { return std::string("busy"); };
    restore_backup(pc.roots, ro);
    CHECK(pc.snapshot() == snap);
}

TEST_CASE("saves: stale temporary folders are cleaned, fresh ones kept") {
    FakePc pc(L"stale");
    std::filesystem::create_directories(pc.roots.store + L"\\.tmp-old");
    std::filesystem::create_directories(pc.roots.store + L"\\.tmp-fresh");
    FakePc::write(pc.roots.store + L"\\.tmp-old\\world0.ksd", "half");
    // make .tmp-old two hours old
    HANDLE h = CreateFileW((pc.roots.store + L"\\.tmp-old").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart -= 2ull * 3600 * 10000000;
    ft = {u.LowPart, u.HighPart};
    SetFileTime(h, &ft, nullptr, &ft);
    CloseHandle(h);
    clean_stale_temp(pc.roots.store, 3600);
    CHECK_FALSE(FakePc::has(pc.roots.store + L"\\.tmp-old"));
    CHECK(FakePc::has(pc.roots.store + L"\\.tmp-fresh"));
    CHECK(list_backups(pc.roots.store).empty());
}

TEST_CASE("saves: summaries name characters and worlds, with names and levels when known") {
    Backup b;
    auto add = [&](const char* storage, const char* id, const char* type, int slot, const char* name) {
        BackupFile f;
        f.storage = storage;
        f.steam_id = id;
        f.type = type;
        f.slot = slot;
        f.name = name;
        f.path = std::string(storage) + "/" + (std::string(storage) == "steam" ? std::string(id) : "default") + "/" + name;
        b.files.push_back(f);
    };
    add("steam", "111", "character", 0, "character0.ksd");
    add("steam", "111", "world", 0, "world0.ksd");
    add("steam", "111", "global", -1, "global.ksd");
    add("local", "", "character", 1, "character1.ksd");
    add("local", "", "world", 0, "world0.ksd");
    add("local", "", "world", 2, "world2.ksd");
    CHECK(backup_summary(b) ==
          "Steam: character slot 1, world slot 1 · Local: character slot 2, world slot 1, world slot 3");
    auto info = [&](const char* storage, const char* type, int slot, const char* name, std::optional<int> level) {
        SlotInfo i;
        i.storage = storage;
        i.type = type;
        i.slot = slot;
        i.name = name;
        i.level = level;
        b.info.push_back(i);
    };
    info("steam", "character", 0, "Astrid", 8);
    info("steam", "world", 0, "Embervale", std::nullopt);
    info("local", "character", 1, "Character 2", 1);
    info("local", "world", 0, "My World 1", std::nullopt);
    CHECK(backup_summary(b) ==
          "Steam: Astrid (level 8), world Embervale · Local: Character 2 (level 1), world My World 1, world slot 3");
    CHECK(file_label(b, b.files[2]) == "global.ksd");
    CHECK(file_label(b, b.files[0]) == "Astrid (level 8)");
    CHECK(storage_label("steam") == "Steam");
    CHECK(storage_label("local") == "Local");
    Backup settings_only;
    settings_only.files.push_back(b.files[2]);
    CHECK(backup_summary(settings_only).empty());
    CHECK(size_text(0) == "0 KB");
    CHECK(size_text(1536) == "1.5 KB");
    CHECK(size_text(300 * 1024) == "300 KB");
    CHECK(size_text(3 * 1024 * 1024 + 300 * 1024) == "3.3 MB");
}

TEST_CASE("saves: the roots come from test variables, and a test run never uses a real place") {
    std::map<std::wstring, std::wstring> vars;
    auto env = [&](const wchar_t* name) {
        const auto it = vars.find(name);
        return it == vars.end() ? std::wstring() : it->second;
    };
    RealPlaces real;
    real.steam = [] { return std::wstring(L"R:\\RealSteam"); };
    real.saved_games = [] { return std::wstring(L"R:\\Users\\me\\Saved Games"); };
    auto r = roots_from(env, false, real);
    CHECK(r.steam == L"R:\\RealSteam");
    CHECK(r.local == L"R:\\Users\\me\\Saved Games\\keen games\\Enshrouded");
    CHECK(r.store.empty());
    r = roots_from(env, true, real);  // a test run without variables: nothing real
    CHECK(r.steam.empty());
    CHECK(r.local.empty());
    vars[L"EMF_TEST_NO_STEAM"] = L"1";  // the test scripts' marker counts as a test run too
    r = roots_from(env, false, real);
    CHECK(r.steam.empty());
    CHECK(r.local.empty());
    vars[L"EMF_TEST_STEAM_ROOT"] = L"T:\\fake steam";
    vars[L"EMF_TEST_SAVED_GAMES"] = L"T:\\fake saved games";
    r = roots_from(env, true, real);
    CHECK(r.steam == L"T:\\fake steam");
    CHECK(r.local == L"T:\\fake saved games\\keen games\\Enshrouded");
    r = with_app_data(r, L"T:\\appdata\\EMF");
    CHECK(r.store == L"T:\\appdata\\EMF\\save-backups");
    CHECK(r.info == L"T:\\appdata\\EMF\\save-info.json");
    CHECK(with_app_data(r, L"").store.empty());
}

TEST_CASE("saves: the slot info for a backup's slots is copied into its manifest") {
    FakePc pc(L"info");
    pc.usual();
    Clock c;
    auto o = opts("manual", c);
    SlotInfo a;
    a.storage = "local";
    a.type = "character";
    a.slot = 1;
    a.name = "Character 2";
    a.level = 1;
    SlotInfo gone;  // a slot without a file in the backup is left out
    gone.storage = "local";
    gone.type = "world";
    gone.slot = 5;
    gone.name = "Old World";
    SlotInfo steam;
    steam.storage = "steam";
    steam.type = "character";
    steam.slot = 0;
    steam.name = "Astrid";
    o.info = {a, gone, steam};
    const auto r = make_backup(pc.roots, o);
    REQUIRE(r.ok);
    std::vector<std::string> names;
    for (const auto& i : r.backup.info) names.push_back(i.name);
    CHECK(names == std::vector<std::string>{"Character 2", "Astrid"});
    // with two Steam users EMF cannot tell whose the Steam names are: left out
    FakePc::write(pc.remote(L"222") + L"\\character0.ksd", "another Steam user");
    c.t += 1;
    const auto r2 = make_backup(pc.roots, o);
    REQUIRE(r2.ok);
    names.clear();
    for (const auto& i : r2.backup.info) names.push_back(i.name);
    CHECK(names == std::vector<std::string>{"Character 2"});
}

TEST_CASE("saves: a backup the installer's PowerShell made reads and restores the same") {
    FakePc pc(L"from-install");
    FakePc::write(pc.remote(L"4242") + L"\\character0.ksd", "played on");
    FakePc::write(pc.local_default() + L"\\world0.ksd", "played on");
    const auto fixture = std::filesystem::path(EMF_SOURCE_DIR) / "tests" / "fixtures" / "saves" / "backup_from_install.json";
    const std::wstring dir = pc.roots.store + L"\\2026-10-27_13-18-53_starter";
    FakePc::write(dir + L"\\backup.json", FakePc::read(fixture.wstring()));
    FakePc::write(dir + L"\\steam\\4242\\character0.ksd", "steam char");
    FakePc::write(dir + L"\\local\\default\\world0.ksd", "local world");
    const auto list = list_backups(pc.roots.store);
    REQUIRE(list.size() == 1);
    const Backup& b = list[0];
    CHECK_FALSE(b.damaged);
    CHECK(b.kind == "starter");
    CHECK(b.label == "Before EMF");
    CHECK(b.made_by == "install");
    REQUIRE(b.files.size() == 2);
    REQUIRE(b.info.size() == 2);
    CHECK(b.info[0].name == "Bj\xC3\xB6rn <&>");
    CHECK(b.info[1].played == 1793000000);
    CHECK(backup_summary(b) == "Steam: Bj\xC3\xB6rn <&> (level 8) \xC2\xB7 Local: world My World 1");
    CHECK(has_starter(pc.roots.store));
    RestoreOptions ro;
    ro.id = b.id;
    ro.busy = [] { return std::string(); };
    const auto r = restore_backup(pc.roots, ro);
    REQUIRE_MESSAGE(r.ok, r.error);
    CHECK(FakePc::read(pc.remote(L"4242") + L"\\character0.ksd") == "steam char");
    CHECK(FakePc::read(pc.local_default() + L"\\world0.ksd") == "local world");
}
