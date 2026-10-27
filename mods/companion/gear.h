#pragma once
// companion: the gear the player gave the companion, without the game (unit-tested). Gear lives in the mod (the body
// has no inventory or equipment components): a piece is taken from the player's inventory through the framework's
// take_items, worn, and put back (a new item with the same id, level and rarity) when taken back. The level bonus it
// gives is added to the player's level. Crash rules (design "Rulings after the builder check"): a take that was never
// confirmed is dropped on load; a piece on its way back stays in the file for 120 s and counts as worn again on load
// (a crash may duplicate an item, never lose one).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace companion {

enum class Slot : uint8_t { Weapon, Head, Chest, Legs, Hands, Feet };
constexpr int kSlotCount = 6;
enum class PieceState : uint8_t { Taking, Held, Returning };

struct Piece {
    uint32_t item = 0;
    uint32_t level = 0;
    uint8_t rarity = 0;
    Slot slot = Slot::Weapon;
    PieceState state = PieceState::Taking;
    bool put_confirmed = false;  // Returning: the game confirmed the put
    bool put_failed = false;     // Returning: the put found no room and the slot is worn by a newer piece (it waits)
    uint64_t since_ms = 0;       // Returning + put_confirmed: when
    uint32_t token = 0;          // this piece, for the answer to its put (identical items are told apart); not saved
};

// What became of a returning piece after the game's answer to putting it back.
enum class PutOutcome : uint8_t {
    Returned,   // it is in the player's inventory
    WornAgain,  // no room: the companion wears it again
    Waiting,    // no room and a newer piece wears its slot: it goes back with the next take_back_all (or load)
    Unknown,    // no returning piece of that item was waiting for an answer
};

constexpr uint64_t kReturnKeepMs = 120000;  // a returned piece stays in the file this long (the character save lags)

// "weapon", "head", ... for messages ("?" for a value outside the enum).
inline const char* slot_name(Slot s) {
    static const char* const kNames[kSlotCount] = {"weapon", "head", "chest", "legs", "hands", "feet"};
    const int i = static_cast<int>(s);
    return i >= 0 && i < kSlotCount ? kNames[i] : "?";
}

// The gear slot of an item from its debug name (export/items.csv): "Weapon_..." is a weapon; "Armor_..." with
// _Helmet / _Chest / _Arms / _Legs / _Boots is armour for that slot. Anything else (rings, shields, NPC-only items,
// materials) is not companion gear.
inline std::optional<Slot> slot_from_name(const std::string& name) {
    if (name.find("NPC_ONLY") != std::string::npos) return std::nullopt;
    if (name.rfind("Weapon_", 0) == 0) return Slot::Weapon;
    if (name.rfind("Armor_", 0) != 0) return std::nullopt;
    const struct {
        const char* token;
        Slot slot;
    } kSlots[] = {{"_Helmet", Slot::Head}, {"_Chest", Slot::Chest}, {"_Arms", Slot::Hands}, {"_Legs", Slot::Legs},
                  {"_Boots", Slot::Feet}};
    for (const auto& s : kSlots) {
        const size_t at = name.find(s.token);
        if (at == std::string::npos) continue;
        const char after = at + std::strlen(s.token) < name.size() ? name[at + std::strlen(s.token)] : '\0';
        if (after == '\0' || after == '_') return s.slot;  // a whole word ("_Chest", not "_Chestplate")
    }
    return std::nullopt;
}

// The companion's level: the player's level plus the gear bonus, from 1 to 100.
inline uint32_t companion_level(uint32_t player_level, uint32_t bonus) {
    const uint32_t l = player_level + bonus;
    return l < 1 ? 1 : (l > 100 ? 100 : l);
}

class GearBook {
public:
    struct GiveResult {
        bool ok = false;
    };
    // The player hands over `p` (state Taking) for its slot. Refused while another give for that slot waits.
    GiveResult give(Piece p, uint64_t /*now_ms*/) {
        if (find(p.slot, PieceState::Taking)) return {};
        p.state = PieceState::Taking;
        p.put_confirmed = false;
        p.token = ++next_token_;
        pieces_.push_back(p);
        return {true};
    }
    // The game's answer to the take for `slot`. Taken: the piece is worn and the one it replaces (if any) is returned
    // (state Returning; the caller puts it back). Not taken: the give is undone; the old piece stays worn.
    std::vector<Piece> on_take_result(Slot slot, bool taken, uint64_t /*now_ms*/) {
        std::vector<Piece> back;
        Piece* incoming = find(slot, PieceState::Taking);
        if (!incoming) return back;
        if (!taken) {
            erase(incoming);
            return back;
        }
        if (Piece* old = find(slot, PieceState::Held)) {
            old->state = PieceState::Returning;
            back.push_back(*old);
        }
        incoming = find(slot, PieceState::Taking);
        incoming->state = PieceState::Held;
        return back;
    }
    // Everything worn goes back to the player, and pieces that found no room before try again (the caller puts each
    // returned piece back). Pieces already on their way are not sent twice.
    std::vector<Piece> take_back_all(uint64_t /*now_ms*/) {
        std::vector<Piece> back;
        for (auto& p : pieces_)
            if (p.state == PieceState::Held || (p.state == PieceState::Returning && p.put_failed)) {
                p.state = PieceState::Returning;
                p.put_confirmed = false;
                p.put_failed = false;
                back.push_back(p);
            }
        return back;
    }
    // The game's answer to putting a returning piece back: ok = it is in the player's inventory (kept in the file for
    // kReturnKeepMs, then forgotten); not ok = worn again (nothing is lost; the player can try again).
    PutOutcome on_put_result(uint32_t token, bool ok, uint64_t now_ms) {
        for (auto& p : pieces_)
            if (p.token == token && p.state == PieceState::Returning && !p.put_confirmed && !p.put_failed) {
                if (ok) {
                    p.put_confirmed = true;
                    p.since_ms = now_ms;
                    return PutOutcome::Returned;
                }
                if (!find(p.slot, PieceState::Held)) {
                    p.state = PieceState::Held;
                    return PutOutcome::WornAgain;
                }
                p.put_failed = true;  // a newer piece wears the slot
                return PutOutcome::Waiting;
            }
        return PutOutcome::Unknown;
    }
    void tick(uint64_t now_ms) {
        for (size_t i = 0; i < pieces_.size();)
            if (pieces_[i].state == PieceState::Returning && pieces_[i].put_confirmed &&
                now_ms - pieces_[i].since_ms >= kReturnKeepMs)
                pieces_.erase(pieces_.begin() + static_cast<std::ptrdiff_t>(i));
            else
                ++i;
    }
    // Pieces worn now (one per slot).
    std::vector<Piece> held() const {
        std::vector<Piece> out;
        for (const auto& p : pieces_)
            if (p.state == PieceState::Held) out.push_back(p);
        return out;
    }
    // Returning pieces whose put was not confirmed yet (to put back, e.g. after a reload).
    std::vector<Piece> unreturned() const {
        std::vector<Piece> out;
        for (const auto& p : pieces_)
            if (p.state == PieceState::Returning && !p.put_confirmed) out.push_back(p);
        return out;
    }
    // Level bonus: weapon max(1, (weapon level − player level) / 2 + 1), +1 per armour piece, at most `cap`.
    uint32_t bonus(uint32_t player_level, uint32_t cap) const {
        uint32_t b = 0;
        for (const auto& p : pieces_) {
            if (p.state != PieceState::Held) continue;
            if (p.slot == Slot::Weapon) {
                const int d = (static_cast<int>(p.level) - static_cast<int>(player_level)) / 2 + 1;
                b += static_cast<uint32_t>(d < 1 ? 1 : d);
            } else {
                b += 1;
            }
        }
        return b > cap ? cap : b;
    }
    bool empty() const { return pieces_.empty(); }

    // One line per piece: "piece <slot> <item hex> <level> <rarity> <held|taking|returning>".
    std::string serialize() const {
        std::string out;
        char b[96];
        for (const auto& p : pieces_) {
            snprintf(b, sizeof(b), "piece %d %08x %u %u %s\n", static_cast<int>(p.slot), p.item, p.level,
                     static_cast<unsigned>(p.rarity),
                     p.state == PieceState::Held ? "held" : (p.state == PieceState::Taking ? "taking" : "returning"));
            out += b;
        }
        return out;
    }
    // Reads a file written by serialize() after a restart or crash: worn pieces are worn; a returning piece is worn
    // again when its slot is free, else it stays returning (put it back: unreturned()); unconfirmed takes are dropped.
    // Other lines are ignored.
    void load(const std::string& text) {
        pieces_.clear();
        std::vector<Piece> returning;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            int slot = -1;
            Piece p;
            unsigned rarity = 0;
            char state[16] = {};
            if (sscanf_s(line.c_str(), "piece %d %x %u %u %15s", &slot, &p.item, &p.level, &rarity, state,
                         static_cast<unsigned>(sizeof(state))) != 5 ||
                slot < 0 || slot >= kSlotCount || !p.item)
                continue;
            const std::string st = state;
            if (st != "held" && st != "returning") continue;  // "taking": never confirmed, the item is still the player's
            p.slot = static_cast<Slot>(slot);
            p.rarity = static_cast<uint8_t>(rarity);
            if (st == "returning") {
                returning.push_back(p);
                continue;
            }
            p.state = PieceState::Held;
            if (find(p.slot, PieceState::Held)) {  // two worn pieces for one slot (should not happen): return the extra
                p.state = PieceState::Returning;
                returning.push_back(p);
                continue;
            }
            p.token = ++next_token_;
            pieces_.push_back(p);
        }
        for (auto& p : returning) {
            p.put_confirmed = false;
            p.state = find(p.slot, PieceState::Held) ? PieceState::Returning : PieceState::Held;
            p.token = ++next_token_;
            pieces_.push_back(p);
        }
    }

private:
    Piece* find(Slot slot, PieceState state) {
        for (auto& p : pieces_)
            if (p.slot == slot && p.state == state) return &p;
        return nullptr;
    }
    void erase(Piece* p) { pieces_.erase(pieces_.begin() + (p - pieces_.data())); }
    std::vector<Piece> pieces_;
    uint32_t next_token_ = 0;
};

// The gear wheel: at most 63 pieces, so "Take all gear back" (the last entry) always fits the wheel's 64.
constexpr size_t kGearWheelPieces = 63;
inline size_t gear_wheel_offered(size_t candidates) {
    return candidates < kGearWheelPieces ? candidates : kGearWheelPieces;
}
inline std::vector<std::string> gear_wheel_labels(const std::vector<std::string>& pieces) {
    std::vector<std::string> out(pieces.begin(), pieces.begin() + static_cast<std::ptrdiff_t>(gear_wheel_offered(pieces.size())));
    out.push_back("Take all gear back");
    return out;
}

}  // namespace companion
