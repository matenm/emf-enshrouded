#pragma once
// Mods' settings: declared by manifests or the API, stored in emf/config/<mod>.ini section [<mod>] (the files mods
// already read with GetPrivateProfile*), changed from the Mods tab, console commands or the API. Values are kept as
// normalised text; bad values in the ini are clamped or replaced by the default, bad values from set() are refused.
// The mod's on/off switch is its "enabled" key. Thread-safe.
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "manifest.h"

namespace emf::core {

class IniBackend {
public:
    virtual ~IniBackend() = default;
    virtual std::optional<std::string> read(const std::string& mod, const std::string& key) = 0;
    virtual bool write(const std::string& mod, const std::string& key, const std::string& value) = 0;
};

// <config_dir>\<mod>.ini, section [<mod>] (Win32 profile functions: only the key written changes).
class FileIniBackend : public IniBackend {
public:
    explicit FileIniBackend(std::wstring config_dir) : dir_(std::move(config_dir)) {}
    std::optional<std::string> read(const std::string& mod, const std::string& key) override;
    bool write(const std::string& mod, const std::string& key, const std::string& value) override;

private:
    std::wstring dir_;
};

class SettingsStore {
public:
    explicit SettingsStore(IniBackend* backend) : backend_(backend) {}

    // How a declaration treats the stored value:
    // - Start (loading mods, the API): the ini's value, or the current one when redeclared; a value that does not fit
    //   is clamped or replaced and written back, so mods reading their ini see what the Mods tab shows.
    // - Live (mod.json edited while the game runs): always the ini's value, fitted to the new definition in memory only
    //   (an edit saved half-way never overwrites what the player saved); a value that moves is a change (callbacks run).
    enum class Declare { Start, Live };
    // Declares (or redeclares) a setting and reads its stored value. False (and *why) for a bad definition; true with
    // *why set to something worth a log line (SettingDef::warning), or cleared.
    bool declare(const std::string& mod, SettingDef def, std::string* why = nullptr, Declare mode = Declare::Start);
    std::vector<SettingDef> defs(const std::string& mod) const;
    std::vector<std::string> mods() const;  // mods with declared settings, by id
    bool has(const std::string& mod, const std::string& key) const;
    std::string get(const std::string& mod, const std::string& key) const;  // effective value; "" if undeclared
    int64_t get_int(const std::string& mod, const std::string& key, int64_t fallback) const;
    double get_float(const std::string& mod, const std::string& key, double fallback) const;

    enum class SetResult { Ok, Unchanged, UnknownSetting, Invalid, WriteFailed };
    SetResult set(const std::string& mod, const std::string& key, const std::string& value, std::string* why = nullptr);

    // The on/off switch ("enabled" in the mod's ini; `fallback` when it holds nothing readable).
    bool enabled(const std::string& mod, bool fallback) const;
    bool set_enabled(const std::string& mod, bool on);

    // (mod, key) of every change since the last call, in order.
    std::vector<std::pair<std::string, std::string>> take_changes();
    uint64_t version() const;  // changes with every declare and change

private:
    struct Entry {
        SettingDef def;
        std::string value;
    };
    const Entry* find(const std::string& mod, const std::string& key) const;
    IniBackend* backend_;
    // io_m_ puts declarations, changes and file writes in one order (taken before m_, held across a file write); m_
    // guards the values and is never held across file I/O, so readers (the Mods tab, mods) never wait for the disk.
    mutable std::mutex io_m_;
    mutable std::mutex m_;
    std::map<std::string, std::vector<Entry>> mods_;  // in declaration order per mod
    std::vector<std::pair<std::string, std::string>> changes_;
    uint64_t version_ = 0;
};

// setting_value (manifest.h) checks and normalises a value for a definition.

}  // namespace emf::core
