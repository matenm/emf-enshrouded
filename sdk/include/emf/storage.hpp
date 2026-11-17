#pragma once
// emf/storage.hpp — a per-world text file for your mod (header-only, on top of emf.hpp).
//
// The game knows nothing about your mod's data, so keep it next to EMF: <game>\emf\config\<mod>\<slot>.txt, one file
// per world save slot. WorldFile does the bookkeeping every mod otherwise gets wrong:
//   - it learns the slot when the host's world save loads (on_load), per hosted session (server_session),
//   - it hands you the file once per session, after that session's save loaded (one another program holds open is
//     read on a later poll: never handed over as "" and never saved over meanwhile),
//   - it saves only while the file it read belongs to the running session (never into another world's file after a
//     reload or a world switch) and while that world runs (poll called within the last 2 s: not after you quit to the
//     menu or joined someone else's game), atomically (a crash leaves the old file or the new one, never an empty one).
//
//   emf::WorldFile g_file("mymod");                       // global or static: it must outlive the game session
//   ... in emf_mod_load:  g_file.attach();
//   ... in on_tick:       if (w.is_server()) g_file.poll([](const std::string& text) { /* parse your state */ });
//   ... when it changed:  g_file.save(my_state_as_text());  // cheap when unchanged
//
// poll and save run on the server world's thread (from on_tick, a command, a put/take result...).
//
// A text per player (by account) in the same world goes beside it, in <slot>.players.txt, read by the same poll and
// saved under the same rules (save_player; at most 16 KiB per player and 2 MiB per file):
//   ... when a player's data changed:  g_file.save_player(account, text);   // "" removes the player
//   ... to read it:                    if (auto t = g_file.player(account)) { /* *t: "" when none */ }
// player and save_player run on the server world's thread too (or under your own lock): poll rebuilds the texts.
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>

#include "emf/emf.hpp"

namespace emf {

class WorldFile {
public:
    explicit WorldFile(std::string mod) : mod_(std::move(mod)) {}
    WorldFile(const WorldFile&) = delete;
    WorldFile& operator=(const WorldFile&) = delete;

    // The clock (milliseconds) save() checks the server world's liveness with; tests pass their own.
    void set_clock(uint64_t (*clock)()) { clock_ = clock; }
    // The folder for the files (default <game>\emf\config\<mod>). Call before attach, e.g. in tests.
    void set_dir(std::wstring dir) { dir_ = std::move(dir); }

    // Call once from emf_mod_load, after emf::init: listens for the host's world loads. `with` = the EmfApi to use
    // (default: the one emf::init was given; a script host passes its own).
    bool attach(const EmfApi* with = nullptr) {
        api_ = with ? with : api();
        if (!api_ || !api_->on_load || !api_->server_session) return false;
        if (dir_.empty()) dir_ = default_dir();
        api_->on_load([](uint16_t slot, void* self) {
            auto* f = static_cast<WorldFile*>(self);
            f->slot_ = slot;
            f->loaded_session_ = f->api_->server_session();
            ++f->loads_;  // every load is read again, even a second one in the same session
        }, this);
        return true;
    }

    // From your server tick: once per hosted session, after its save loaded, on_loaded(text) gets the file ("" when
    // there is none yet). Returns true on the call that read it.
    template <class F>
    bool poll(F&& on_loaded) {
        if (!api_) return false;
        last_poll_ms_ = clock_();  // the server world runs
        const uint32_t session = api_->server_session();
        const uint32_t loads = loads_;
        if (players_state_ == PlayersState::Unreadable && file_session_ == session && read_loads_ == loads && file_slot_)
            load_players();  // another program had it open: read it again
        if (loaded_session_ != session || (file_session_ == session && read_loads_ == loads) || !slot_) return false;
        file_slot_ = slot_;
        std::string text;
        if (!read_into(path(), text)) {  // there, but another program holds it: never handed over as "" (no file)
            say_once(unreadable_said_, "could not read %ls (another program has it open?): the mod's world data waits until it can",
                     path().c_str());
            return false;  // not read: not ready, so nothing is saved over it; the next poll tries again
        }
        unreadable_said_ = false;
        file_session_ = session;
        read_loads_ = loads;
        written_ = text;
        load_players();
        on_loaded(text);
        return true;
    }

    // True while the running session's file has been read (save works only then).
    bool ready() const {
        return api_ && file_slot_ && file_slot_ == slot_ && file_session_ && file_session_ == api_->server_session() &&
               read_loads_ == loads_ && clock_() - last_poll_ms_ <= kLiveMs;
    }
    // What the file holds: the text read by the last poll, or saved since.
    const std::string& text() const { return written_; }
    // The world slot whose file was read (save writes only there); 0 before the first read.
    uint16_t slot() const { return file_slot_; }
    std::wstring path() const {
        if (!file_slot_) return {};
        wchar_t name[16];
        swprintf(name, 16, L"\\%04x.txt", static_cast<unsigned>(file_slot_));
        return dir_ + name;
    }

    // Writes `text` when it differs from what the file holds. False when not ready() or on a write error (logged).
    bool save(const std::string& text) {
        if (!ready()) return false;
        if (text == written_) return true;
        if (!write_whole(path(), text, failed_path_)) return false;
        written_ = text;
        return true;
    }

    // ---- a text per player in the same world ----
    static constexpr size_t kMaxPlayerText = 16384;     // bytes per player
    static constexpr size_t kMaxPlayersFile = 2097152;  // bytes of the whole file
    std::wstring players_path() const {
        if (!file_slot_) return {};
        wchar_t name[24];
        swprintf(name, 24, L"\\%04x.players.txt", static_cast<unsigned>(file_slot_));
        return dir_ + name;
    }
    // `account`'s text in the running world ("" when none); nothing while not ready() (no world's data yet, another
    // world, someone else's game) or while the world's players file cannot be read (not EMF's format, or another
    // program has it open: poll reads it again).
    std::optional<std::string> player(uint64_t account) const {
        if (!ready() || players_state_ != PlayersState::Ok) return std::nullopt;
        const auto it = players_.find(account);
        return it == players_.end() ? std::string() : it->second;
    }
    // Saves `account`'s text ("" removes the player); the whole file is written, only when something changed. False
    // when not ready(), for account 0, over kMaxPlayerText or kMaxPlayersFile, into a file that did not parse (it is
    // never overwritten), or on a write error (each logged once).
    bool save_player(uint64_t account, const std::string& text) {
        if (!ready() || !account) return false;
        if (players_state_ != PlayersState::Ok) return false;  // logged when it was read
        if (text.size() > kMaxPlayerText) {
            say_once(big_said_, "a player's data over %u bytes was not saved", static_cast<unsigned>(kMaxPlayerText));
            return false;
        }
        const auto have = players_.find(account);
        if (text.empty() ? have == players_.end() : have != players_.end() && have->second == text) return true;
        std::map<uint64_t, std::string> next = players_;
        if (text.empty()) next.erase(account);
        else next[account] = text;
        if (next == players_) return true;
        const std::string all = players_text(next);
        if (all.size() > kMaxPlayersFile) {
            say_once(full_said_, "player data was not saved: %ls would pass %u bytes", players_path().c_str(),
                     static_cast<unsigned>(kMaxPlayersFile));
            return false;
        }
        if (!write_whole(players_path(), all, failed_players_path_)) return false;
        players_ = std::move(next);
        return true;
    }

private:
    // `text` into `file` whole: a temporary file renamed over it (a crash leaves the old file or the new one).
    // `failed`: the file of this kind whose write failure was logged (each kind logs once, not on every save).
    bool write_whole(const std::wstring& file, const std::string& text, std::wstring& failed) {
        CreateDirectoryW(parent(dir_).c_str(), nullptr);
        CreateDirectoryW(dir_.c_str(), nullptr);
        const std::wstring tmp = file + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            ok = fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = fclose(f) == 0 && ok;
        }
        ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) {
            DeleteFileW(tmp.c_str());
            if (failed != file) {  // once per file, not on every save
                failed = file;
                if (api_->log)
                    api_->log(mod_.c_str(), "could not write %ls (further failures for it are not logged)", file.c_str());
            }
            return false;
        }
        failed.clear();
        return true;
    }
    // Reads the world's players file: none is no players; one another program holds is read again by poll; one that is
    // not EMF's format is never overwritten (until it is fixed or removed and the world loaded again).
    void load_players() {
        const std::wstring file = players_path();
        std::string text;
        FILE* f = nullptr;
        const errno_t e = _wfopen_s(&f, file.c_str(), L"rb");
        bool unreadable = e != 0 || !f;
        if (unreadable && e == ENOENT) {
            players_.clear();
            players_state_ = PlayersState::Ok;
            return;
        }
        if (!unreadable) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            unreadable = ferror(f) != 0;
            fclose(f);
        }
        if (unreadable) {
            players_.clear();
            if (players_state_ != PlayersState::Unreadable && api_->log)
                api_->log(mod_.c_str(), "could not read %ls (another program has it open?): player data waits until it can",
                          file.c_str());
            players_state_ = PlayersState::Unreadable;
            return;
        }
        if (!parse_players(text, players_)) {
            players_state_ = PlayersState::Bad;
            if (api_->log)
                api_->log(mod_.c_str(),
                          "%ls is not EMF's player data format: it is not read or saved over until it is fixed or removed "
                          "and the world loaded again",
                          file.c_str());
            return;
        }
        players_state_ = PlayersState::Ok;
    }
    template <class... A>
    void say_once(bool& said, const char* fmt, A... a) {
        if (said) return;
        said = true;
        if (api_->log) api_->log(mod_.c_str(), fmt, a...);
    }
    // "emf players 1\n", then per player "0x<16 hex> <bytes>\n<text>\n" (any bytes fit).
    static std::string players_text(const std::map<uint64_t, std::string>& m) {
        std::string s = "emf players 1\n";
        for (const auto& [account, text] : m) {
            char head[48];
            std::snprintf(head, sizeof head, "0x%016llx %zu\n", static_cast<unsigned long long>(account), text.size());
            s += head;
            s += text;
            s += '\n';
        }
        return s;
    }
    // False (and `out` empty) when `t` is not that format; an empty or missing file is no players.
    static bool parse_players(const std::string& t, std::map<uint64_t, std::string>& out) {
        out.clear();
        if (t.empty()) return true;
        const std::string head = "emf players 1\n";
        if (t.compare(0, head.size(), head) != 0) return false;
        size_t i = head.size();
        while (i < t.size()) {
            const size_t nl = t.find('\n', i);
            if (nl == std::string::npos || nl - i < 20 || nl - i > 28 || t[i] != '0' || t[i + 1] != 'x' || t[i + 18] != ' ')
                return out.clear(), false;
            uint64_t account = 0;
            for (size_t k = i + 2; k < i + 18; ++k) {
                const char c = t[k];
                const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0) return out.clear(), false;
                account = account * 16 + static_cast<uint64_t>(v);
            }
            size_t len = 0;
            for (size_t k = i + 19; k < nl; ++k) {
                if (t[k] < '0' || t[k] > '9') return out.clear(), false;
                len = len * 10 + static_cast<size_t>(t[k] - '0');
            }
            if (i + 19 == nl || len > kMaxPlayerText || nl + 1 + len >= t.size() || t[nl + 1 + len] != '\n')
                return out.clear(), false;
            if (!out.emplace(account, t.substr(nl + 1, len)).second) return out.clear(), false;  // an account twice
            i = nl + 1 + len + 1;
        }
        return true;
    }

    std::wstring default_dir() const {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring d(exe);
        return d.substr(0, d.find_last_of(L"\\/")) + L"\\emf\\config\\" + std::wstring(mod_.begin(), mod_.end());
    }
    static std::wstring parent(const std::wstring& d) { return d.substr(0, d.find_last_of(L"\\/")); }
    // The file's text into `text` ("" when there is no file); false when it is there but cannot be read.
    static bool read_into(const std::wstring& file, std::string& text) {
        text.clear();
        FILE* f = nullptr;
        const errno_t e = _wfopen_s(&f, file.c_str(), L"rb");
        if (e == ENOENT) return true;
        if (e != 0 || !f) return false;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        const bool bad = ferror(f) != 0;
        fclose(f);
        return !bad;
    }
    std::string mod_;
    std::wstring dir_;
    const EmfApi* api_ = nullptr;
    uint64_t (*clock_)() = &GetTickCount64;
    static constexpr uint64_t kLiveMs = 2000;  // poll from the server world's tick runs 60 times a second
    uint64_t last_poll_ms_ = 0;
    std::atomic<uint16_t> slot_{0};
    std::atomic<uint32_t> loaded_session_{0};
    std::atomic<uint32_t> loads_{0};
    uint16_t file_slot_ = 0;
    uint32_t file_session_ = 0, read_loads_ = 0;
    std::string written_;
    std::wstring failed_path_;          // the world file whose write failure was logged
    bool unreadable_said_ = false;      // the log said the world file could not be read (until it could)
    std::wstring failed_players_path_;  // the players file whose write failure was logged
    std::map<uint64_t, std::string> players_;  // the running world's player texts (read by poll)
    enum class PlayersState { Ok, Unreadable, Bad };  // Unreadable: poll reads it again; Bad: never overwritten
    PlayersState players_state_ = PlayersState::Ok;
    bool big_said_ = false, full_said_ = false;
};

}  // namespace emf
