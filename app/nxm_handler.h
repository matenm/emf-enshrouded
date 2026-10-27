#pragma once
// Who opens nxm:// links (Nexus's "Mod Manager Download" button) for the current Windows user, and EMF's opt-in to be
// that program. Vortex and Mod Organizer 2 use the same links, so EMF never takes them silently:
// - nxm_owner reads the command Windows runs now: HKCU\Software\Classes\nxm\shell\open\command, else the all-users
//   one under HKLM (read only), and names the program ("Vortex", "Mod Organizer 2", "EMF", ...).
// - nxm_take (the player's click) first saves what was there in HKCU\Software\EMF\NxmBackup: whether the keys nxm,
//   nxm\shell, nxm\shell\open and nxm\shell\open\command existed, and the raw type and bytes of the key's default value,
//   its "URL Protocol" and the command (kept when EMF already had the links, renewed when another program took them
//   since). A value EMF could not put back exactly (a kind it does not know, or above 64 KiB) refuses the take. Then
//   it writes HKCU\Software\Classes\nxm: "URL Protocol", a default "URL:NXM Protocol" when there was none, and the
//   command "<EMF.exe>" --nxm "%1". Other values and keys (another tool's icon) stay.
// - nxm_give_back puts the saved values back with their types and bytes, removes the keys EMF made (an empty HKCU key
//   would hide an all-users handler in HKEY_CLASSES_ROOT), and forgets the backup. When another program took the links
//   since, EMF leaves them alone.
// HKLM is never written. All registry work goes through RegistryLayer, so the logic is tested against MemoryRegistry
// (and the real API on a remapped scratch key), and smoke runs never touch the real registry.
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace emf::app {

constexpr size_t kMaxRegValueBytes = 64 * 1024;  // a value above this is not kept by the backup (the take is refused)

// A registry value as Windows stores it: its type (REG_SZ, REG_EXPAND_SZ, REG_BINARY, ...) and its bytes (a string's
// UTF-16 with its NUL). too_large: above kMaxRegValueBytes, bytes not read.
struct RegValue {
    unsigned long type = 0;
    std::string bytes;
    bool too_large = false;
};

class RegistryLayer {
public:
    enum class Hive { User, Machine };  // HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE (read only)
    virtual ~RegistryLayer() = default;
    // A value as it is ("" name = the key's default value); nullopt when the key or value is missing.
    virtual std::optional<RegValue> get_raw(Hive hive, const std::wstring& key, const std::wstring& name) = 0;
    virtual bool exists(Hive hive, const std::wstring& key) = 0;
    // HKCU only. The key is made when missing.
    virtual bool set_raw(const std::wstring& key, const std::wstring& name, const RegValue& value) = 0;
    virtual bool remove_value(const std::wstring& key, const std::wstring& name) = 0;  // true when it is gone
    virtual bool remove_tree(const std::wstring& key) = 0;                             // true when it is gone
    virtual void notify() {}  // tells Explorer that link associations changed
    // Windows' status (LSTATUS) of the last call that failed.
    virtual long last_status() const = 0;

    // A REG_SZ or REG_EXPAND_SZ value as text (nullopt for other types or when missing).
    std::optional<std::wstring> get(Hive hive, const std::wstring& key, const std::wstring& name);
    bool is_expand(Hive hive, const std::wstring& key, const std::wstring& name);  // a REG_EXPAND_SZ
    bool set(const std::wstring& key, const std::wstring& name, const std::wstring& value, bool expand = false);
};

// The registry in memory, for tests and smoke runs. Keys and names ignore case.
class MemoryRegistry final : public RegistryLayer {
public:
    std::optional<RegValue> get_raw(Hive hive, const std::wstring& key, const std::wstring& name) override;
    bool exists(Hive hive, const std::wstring& key) override;
    bool set_raw(const std::wstring& key, const std::wstring& name, const RegValue& value) override;
    bool remove_value(const std::wstring& key, const std::wstring& name) override;
    bool remove_tree(const std::wstring& key) override;
    void notify() override { ++notified; }
    long last_status() const override { return last_; }
    void set_machine(const std::wstring& key, const std::wstring& name, const std::wstring& value);

    bool fail_writes = false;  // every write fails with ERROR_ACCESS_DENIED (a locked-down PC)
    int notified = 0;

private:
    using Keys = std::map<std::wstring, std::map<std::wstring, RegValue>>;
    Keys& hive(Hive h) { return h == Hive::User ? user_ : machine_; }
    bool refuse();
    Keys user_, machine_;
    long last_ = 0;
};

// The real registry (HKCU for writes, HKLM read only). `tell_explorer`: notify() tells Explorer (tests pass false).
std::unique_ptr<RegistryLayer> windows_registry(bool tell_explorer = true);

struct NxmOwner {
    std::wstring command;  // what Windows runs for an nxm:// link ("" when nothing is set)
    std::wstring program;  // the program in it
    std::string label;     // "Vortex", "Mod Organizer 2", "Nexus Mod Manager", "EMF", "another copy of EMF (<path>)",
                           // the program's file name, or "nothing"
    bool ours = false;     // this EMF.exe
    bool emf = false;      // any EMF.exe
    bool machine = false;  // set for all users (HKLM), not by this user
};
NxmOwner nxm_owner(RegistryLayer& reg, const std::wstring& exe);

// "<exe>" --nxm "%1"
std::wstring nxm_command_for(const std::wstring& exe);
// The program a command runs: the quoted part, else up to ".exe", else up to the first space.
std::wstring command_program(const std::wstring& command);

struct NxmChange {
    bool ok = false;
    std::string error;  // why not, for the player
    std::string text;   // what happened, for the player
};
NxmChange nxm_take(RegistryLayer& reg, const std::wstring& exe);
NxmChange nxm_give_back(RegistryLayer& reg, const std::wstring& exe);
// Who had nxm links before EMF took them ("nothing" when nobody), or "" when EMF has not taken them.
std::string nxm_previous_label(RegistryLayer& reg);

}  // namespace emf::app
