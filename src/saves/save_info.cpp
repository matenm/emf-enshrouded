#include "saves/save_info.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

#include "core/json.h"
#include "saves/detail.h"

namespace emf::saves {

namespace {

using core::json::Value;
using VT = Value::Type;

int order_key(const SlotInfo& s) { return (s.storage == "steam" ? 0 : 2) + (s.type == "character" ? 0 : 1); }

void sort_slots(std::vector<SlotInfo>& v) {
    std::sort(v.begin(), v.end(), [](const SlotInfo& a, const SlotInfo& b) {
        if (a.storage != b.storage) return a.storage == "steam";
        if (a.steam_id != b.steam_id) return a.steam_id < b.steam_id;
        if (order_key(a) != order_key(b)) return order_key(a) < order_key(b);
        return a.slot < b.slot;
    });
}

// A Steam slot belongs to one Steam account: two accounts' slot 0 are two entries.
SlotInfo* find(SaveInfo& info, const std::string& storage, const std::string& type, int slot, const std::string& steam_id) {
    for (auto& s : info.slots)
        if (s.storage == storage && s.type == type && s.slot == slot && (storage != "steam" || s.steam_id == steam_id)) return &s;
    return nullptr;
}

}  // namespace

std::string save_info_json(const SaveInfo& info) {
    std::string s = "{\n  \"format\": " + std::to_string(info.format > 0 ? info.format : 1) +
                    ",\n  \"updated\": " + std::to_string(info.updated) + ",\n  \"slots\": [";
    for (size_t i = 0; i < info.slots.size(); ++i) s += std::string(i ? "," : "") + "\n" + detail::slot_info_json(info.slots[i], "    ");
    s += info.slots.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return s;
}

std::optional<SaveInfo> parse_save_info(std::string_view text, std::string* error) {
    const auto parsed = core::json::parse(text);
    if (!parsed.value || !parsed.value->is(VT::Object)) {
        if (error) *error = parsed.value ? "save-info.json is not a JSON object" : "save-info.json is not valid JSON: " + parsed.error;
        return std::nullopt;
    }
    const Value& v = *parsed.value;
    SaveInfo info;
    if (const Value* f = v.get("format"); f && f->is(VT::Number)) info.format = static_cast<int>(f->n);
    if (const Value* u = v.get("updated"); u && u->is(VT::Number)) info.updated = static_cast<int64_t>(u->n);
    if (const Value* slots = v.get("slots"); slots && slots->is(VT::Array))
        for (const Value& x : slots->arr)
            if (auto s = detail::slot_info_from(x)) info.slots.push_back(std::move(*s));
    sort_slots(info.slots);
    return info;
}

SaveInfo read_save_info(const std::wstring& path) {
    std::string text;
    if (path.empty() || !detail::read_small(path, text, 1u << 20)) return {};
    return parse_save_info(text).value_or(SaveInfo{});
}

bool write_save_info(const std::wstring& path, const SaveInfo& info) {
    if (path.empty()) return false;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        // EMF's own folder (%LOCALAPPDATA%\EMF), made if missing
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path.substr(0, slash)), ec);
    }
    const std::wstring tmp = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    if (!detail::write_whole(tmp, save_info_json(info))) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

SaveInfo merge_seen(SaveInfo info, const std::vector<SlotSeen>& seen, int64_t now, bool* changed) {
    bool any = false;
    for (const auto& s : seen) {
        if ((s.storage != "steam" && s.storage != "local") || (s.type != "character" && s.type != "world") || s.slot < 0) continue;
        if (s.storage == "steam" && !s.steam_id.empty()) {  // an entry from before accounts were recorded goes
            const auto old = std::remove_if(info.slots.begin(), info.slots.end(), [&](const SlotInfo& x) {
                return x.storage == "steam" && x.steam_id.empty() && x.type == s.type && x.slot == s.slot;
            });
            if (old != info.slots.end()) {
                info.slots.erase(old, info.slots.end());
                any = true;
            }
        }
        SlotInfo* known = find(info, s.storage, s.type, s.slot, s.storage == "steam" ? s.steam_id : std::string());
        if (s.state == 1 || s.state == 6) {  // empty, or deleted
            if (known) {
                info.slots.erase(info.slots.begin() + (known - info.slots.data()));
                any = true;
            }
            continue;
        }
        if ((s.state != 2 && s.state != 4) || s.name.empty()) continue;  // nothing certain about this slot
        if (!known) {
            SlotInfo n;
            n.storage = s.storage;
            if (s.storage == "steam") n.steam_id = s.steam_id;
            n.type = s.type;
            n.slot = s.slot;
            n.name = s.name;
            n.level = s.level;
            n.played = s.played;
            n.play_seconds = s.play_seconds;
            n.seen = now;
            info.slots.push_back(std::move(n));
            any = true;
            continue;
        }
        if (known->name != s.name) {  // another character or world in this slot
            known->name = s.name;
            known->level = s.level;
            known->last_loaded = 0;
            known->played = s.played;
            known->play_seconds = s.play_seconds;
            any = true;
        } else {
            if (s.level && known->level != s.level) {
                known->level = s.level;
                any = true;
            }
            if (s.played && known->played != s.played) {
                known->played = s.played;
                any = true;
            }
            if (s.play_seconds && known->play_seconds != s.play_seconds) {
                known->play_seconds = s.play_seconds;
                any = true;
            }
        }
        known->seen = now;
    }
    sort_slots(info.slots);
    if (changed) *changed = any;
    return info;
}

SaveInfo note_loaded(SaveInfo info, const std::string& storage, const std::string& type, int slot, int64_t now, bool* changed,
                     const std::string& steam_id) {
    SlotInfo* known = find(info, storage, type, slot, storage == "steam" ? steam_id : std::string());
    if (known) known->last_loaded = now;
    if (changed) *changed = known != nullptr;
    return info;
}

}  // namespace emf::saves
