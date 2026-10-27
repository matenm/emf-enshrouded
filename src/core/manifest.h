#pragma once
// Mod manifests (emf/mods/<folder>/mod.json): who a mod is, what it needs, and the settings it offers. Pure parsing
// (unit-tested); the loader (mods.cpp) and the settings store (settings.h) use the result.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace emf::core {

struct Version {
    int major = 0, minor = 0, patch = 0;
};
// "1", "1.2" or "1.2.3" (missing parts are 0); nothing else.
std::optional<Version> parse_version(std::string_view text);
int compare(Version a, Version b);  // <0, 0, >0
std::string version_text(Version v);  // "1.2.0"

// One span of versions: from lo (if any) to hi (if any), each end in or out.
struct VersionSpan {
    bool has_lo = false, lo_in = true;
    Version lo;
    bool has_hi = false, hi_in = false;
    Version hi;
};

// A version range as mod.json's "depends" gives it (SemVer, as npm and Cargo read it):
//   "*" / "x"            any version
//   "^1.2.3"             1.2.3 up to below 2.0.0 (^0.2.3: below 0.3.0; ^0.0.3: only 0.0.3)
//   "~1.2.3", "~1.2"     1.2.3 / 1.2.0 up to below 1.3.0 ("~1": below 2.0.0)
//   ">=1.0", ">1.2", "<2", "<=1.2"   comparisons (">1.2" is newer than every 1.2.x, "<=1.2" up to every 1.2.x)
//   "=1.2.3"             exactly that version ("=1.2" is exactly 1.2.0, as EMF always read it)
//   "1.2.3", "1.2"       a bare version: as ^ (that version or a compatible newer one)
//   "1.2.x", "1.*"       any number in place of the x
//   "1.2.3 - 2.0.0"      both ends included
//   ">=1.0, <2.0"        all must hold (a comma or a space between them)
//   "^1.0 || ^2.0"       either side
// Missing parts of a version are 0 (">=1.1" is ">=1.1.0"). Versions are numbers only: no "v", no pre-release tags.
struct Constraint {
    std::vector<VersionSpan> spans;  // a version fits one of them; none = any version
    std::string written = "*";       // as the mod wrote it (trimmed)
    bool any() const { return spans.empty(); }
    std::string text() const { return written; }
    std::string describe() const;  // "1.2.0 or newer, below 2.0.0", "exactly 1.2.3", "any version"
};
std::optional<Constraint> parse_constraint(std::string_view text, std::string* error = nullptr);
bool satisfies(Version v, const Constraint& c);
enum class Fit { Ok, TooOld, TooNew, Between };  // Between: in a gap of a "||" range
Fit fit(Version v, const Constraint& c);

// One entry of "depends".
struct Dependency {
    std::string id;
    Constraint version;     // "*" when none is given
    bool optional = false;  // the mod loads without it; when it is installed, its version must still fit
};

// The values 0-5 are the C API's EMF_SETTING_BOOL..TEXT (api.cpp casts them): append new types only.
enum class SettingType { Bool, Int, Float, Choice, Key, Text, Select, Color };
const char* setting_type_name(SettingType t);

struct SettingOption {
    std::string value;  // what is stored and what the mod reads
    std::string label;  // what players see (default: the value)
};

struct SettingDef {
    std::string key;
    SettingType type = SettingType::Text;
    std::string label;          // shown in the Mods tab (default: the key)
    std::string help;
    std::string default_value;  // as text: bool "1"/"0", numbers without trailing zeros, choice = index, select = value
    double min = 0.0, max = 0.0;
    bool has_min = false, has_max = false;
    double step = 0.0;                   // Int/Float: values are min (or 0) plus whole steps; 0 = any value
    bool slider = false;                 // Int/Float shown as a slider ("type": "slider", which needs min and max)
    std::vector<std::string> choices;    // Choice only (stored as the index)
    std::vector<SettingOption> options;  // Select only (stored as the option's value)
    uint32_t max_length = 0;             // Text: at most this many characters (0: only the 256-byte limit)
    std::string group;                   // the heading it is shown under ("" = none)
    std::string when_key, when_value;    // "enabledWhen": usable only while setting when_key has value when_value
    bool live = false;                   // the mod applies a change at once (else: at the next game start)
    // Set by the manifest:
    bool strict = false;   // a newer kind (slider, select, keybind, color): a mistake refuses the mod
    std::string problem;   // a mistake in a display field of an old kind: SettingsStore::declare refuses this setting
    std::string warning;   // declared, and this is reported (an enabledWhen dropped because its setting is refused)
};

struct Manifest {
    std::string id, name, version_text, description;
    Version version;
    std::vector<std::string> authors;
    std::string dll;   // DLL file inside the mod's folder (default "<id>.dll"; empty for a script mod)
    std::string main;  // script entry ("main.lua") for mods run by a script host
    uint32_t emf = 0;  // the lowest EMF API version the mod needs (0 = any)
    std::vector<Dependency> depends;
    bool enabled_default = true;  // the switch's value while the mod's ini has no "enabled" key
    std::vector<SettingDef> settings;
};

// mod.json is refused above this size (the loader reads no more than this plus one byte).
constexpr size_t kMaxManifestBytes = 256 * 1024;
// Whole-number settings stay within +-this: the ini text and the API's int64/double keep them exactly.
constexpr double kMaxWholeSetting = 999999999999999.0;

struct ManifestResult {
    std::optional<Manifest> manifest;
    std::string error;  // what is wrong, naming the field (or the JSON error with line and column)
};
ManifestResult parse_manifest(std::string_view json_text);

// 1-48 of a-z 0-9 _ . -
bool valid_mod_id(std::string_view id);
// 1-48 of A-Z a-z 0-9 _ . -
bool valid_setting_key(std::string_view key);
// Builds and checks one setting (shared by manifests and the API's setting_declare). `default_text` may be empty
// (then: false / min or 0 / first choice or option / "" / #ffffff). Returns "" or what is wrong. A key setting's
// default is checked when it is declared (SettingsStore::declare refuses that one setting), not here.
std::string normalise_setting(SettingDef& def);

// The value `text` would have for `def`: normalised, or nullopt (and *why) when it is not valid. With `clamp`,
// numbers outside min/max are pulled in instead of refused (used for values read from the ini). Numbers with a step
// are moved to the nearest step.
std::optional<std::string> setting_value(const SettingDef& def, const std::string& text, bool clamp, std::string* why);

// The order settings are shown in (the Mods tab and EMF.exe): those without a group first, then each group in the
// order its first setting appears; declaration order inside each. Indexes into `defs`.
std::vector<size_t> setting_display_order(const std::vector<SettingDef>& defs);
// Whether `def` can be changed now: it has no "enabledWhen", or the setting it names has the value it asks for.
// `value_of(key)` gives a setting's current value.
bool setting_active(const SettingDef& def, const std::function<std::string(const std::string&)>& value_of);
// Why `def` is greyed out, for players: "only while Show the line is on" ("" without "enabledWhen").
std::string setting_condition_text(const SettingDef& def, const std::vector<SettingDef>& defs);
// A value as players see it: "on"/"off", a choice's or option's label, else the value.
std::string setting_shown_value(const SettingDef& def, const std::string& value);
// A number as settings store it: whole numbers exactly ("1234567"), others with 15 digits ("0.1", "2.5").
std::string setting_number_text(double v);
// Characters (UTF-8 code points) in `text`.
size_t utf8_chars(const std::string& text);
// `text` is well-formed UTF-8 (no stray, overlong or cut-off sequences, no surrogates, nothing above U+10FFFF).
bool valid_utf8(const std::string& text);

}  // namespace emf::core
