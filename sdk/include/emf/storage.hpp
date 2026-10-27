#pragma once
// emf/storage.hpp — a per-world text file for your mod (header-only, on top of emf.hpp).
//
// The game knows nothing about your mod's data, so keep it next to EMF: <game>\emf\config\<mod>\<slot>.txt, one file
// per world save slot. WorldFile does the bookkeeping every mod otherwise gets wrong:
//   - it learns the slot when the host's world save loads (on_load), per hosted session (server_session),
//   - it hands you the file once per session, after that session's save loaded,
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
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
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
        if (loaded_session_ != session || (file_session_ == session && read_loads_ == loads) || !slot_) return false;
        file_slot_ = slot_;
        const std::string text = read(path());
        file_session_ = session;
        read_loads_ = loads;
        written_ = text;
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
        CreateDirectoryW(parent(dir_).c_str(), nullptr);
        CreateDirectoryW(dir_.c_str(), nullptr);
        const std::wstring file = path(), tmp = file + L".tmp";
        FILE* f = nullptr;
        bool ok = _wfopen_s(&f, tmp.c_str(), L"wb") == 0 && f;
        if (ok) {
            ok = fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = fclose(f) == 0 && ok;
        }
        ok = ok && MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) {
            DeleteFileW(tmp.c_str());
            if (failed_path_ != file) {  // once per file, not on every save
                failed_path_ = file;
                if (api_->log)
                    api_->log(mod_.c_str(), "could not write %ls (further failures for it are not logged)", file.c_str());
            }
            return false;
        }
        failed_path_.clear();
        written_ = text;
        return true;
    }

private:
    std::wstring default_dir() const {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring d(exe);
        return d.substr(0, d.find_last_of(L"\\/")) + L"\\emf\\config\\" + std::wstring(mod_.begin(), mod_.end());
    }
    static std::wstring parent(const std::wstring& d) { return d.substr(0, d.find_last_of(L"\\/")); }
    static std::string read(const std::wstring& file) {
        std::string text;
        FILE* f = nullptr;
        if (_wfopen_s(&f, file.c_str(), L"rb") == 0 && f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            fclose(f);
        }
        return text;
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
    std::wstring failed_path_;  // the file whose write failure was logged
};

}  // namespace emf
