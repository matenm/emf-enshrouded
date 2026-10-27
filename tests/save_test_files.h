#pragma once
// A fake PC for the save backup tests: a Steam folder with Steam Cloud saves, a Saved Games folder with Local saves and
// EMF's data folder, all in one temp folder of this run's own, removed again at the end of the test. Never a real one.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>

#include "saves/backup.h"
#include "test_scratch.h"

namespace save_test {

struct FakePc {
    std::wstring root;
    emf::saves::SaveRoots roots;

    explicit FakePc(const std::wstring& name) {
        static int n = 0;
        root = test_scratch::temp_path(L"emf_saves_" + name + L"_" + std::to_wstring(++n));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);
        roots.steam = root + L"\\Steam";
        roots.local = root + L"\\Saved Games\\keen games\\Enshrouded";
        roots.store = root + L"\\AppData\\EMF\\save-backups";
        roots.info = root + L"\\AppData\\EMF\\save-info.json";
    }
    ~FakePc() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    FakePc(const FakePc&) = delete;
    FakePc& operator=(const FakePc&) = delete;

    std::wstring remote(const std::wstring& id = L"111") const { return roots.steam + L"\\userdata\\" + id + L"\\1203620\\remote"; }
    std::wstring local_default() const { return roots.local + L"\\default"; }

    static void write(const std::wstring& path, const std::string& bytes) {
        const std::filesystem::path p(path);
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << bytes;
    }
    static std::string read(const std::wstring& path) {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }
    static bool has(const std::wstring& path) { return std::filesystem::exists(std::filesystem::path(path)); }

    // The usual saves: Steam user 111 with a character, a world, global.ksd and user_settings.json (plus the game's own
    // backup\ and migrated\ folders, which are not backed up); Local with two characters, a world and local_settings.json.
    void usual() const {
        write(remote() + L"\\character0.ksd", "steam character 0");
        write(remote() + L"\\world0.ksd", "steam world 0, a bigger file");
        write(remote() + L"\\global.ksd", "steam global");
        write(remote() + L"\\user_settings.json", "{\"steam\":1}");
        write(remote() + L"\\backup\\character0.ksd.backup-9", "the game's own backup");
        write(remote() + L"\\migrated\\characters", "old format");
        write(local_default() + L"\\character0.ksd", "local character 0");
        write(local_default() + L"\\character1.ksd", "local character 1");
        write(local_default() + L"\\world0.ksd", "local world 0");
        write(local_default() + L"\\backup\\world0.ksd.backup-9", "the game's own local backup");
        write(roots.local + L"\\local_settings.json", "{\"windowMode\":\"Fullscreen\"}");
    }

    // Every file under the two save folders: relative path -> (size, last write time). Equal snapshots mean nothing
    // was written there.
    using Snapshot = std::map<std::wstring, std::tuple<uintmax_t, long long>>;
    Snapshot snapshot() const {
        Snapshot s;
        for (const auto& top : {roots.steam, roots.local}) {
            std::error_code ec;
            if (!std::filesystem::exists(top, ec)) continue;
            for (std::filesystem::recursive_directory_iterator it(top, ec), end; !ec && it != end; it.increment(ec)) {
                const auto rel = it->path().wstring().substr(root.size());
                if (it->is_directory()) {
                    s[rel + L"\\"] = {0, 0};
                    continue;
                }
                s[rel] = {it->file_size(), static_cast<long long>(it->last_write_time().time_since_epoch().count())};
            }
        }
        return s;
    }

    // The folders in the store (backups and anything else), by name.
    std::vector<std::wstring> store_entries() const {
        std::vector<std::wstring> v;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(roots.store, ec), end; !ec && it != end; it.increment(ec))
            v.push_back(it->path().filename().wstring());
        return v;
    }
};

}  // namespace save_test
