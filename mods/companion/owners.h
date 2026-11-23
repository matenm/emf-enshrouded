// One companion per player (GitHub #51): what was one set per world (the companion, its summon policy, orders, gear,
// catch-up and the checks of its body) is one set per player, by the player's account. The host's set keeps the old
// file name, so an old save loads as before. Pure: companion.cpp feeds it the world and does what it asks.
// Threads: the server tick is the only one that adds or removes owners; the ECS program reads them under lock().
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "companion_logic.h"
#include "gear.h"
#include "orders.h"

namespace companion {

struct Owner {
    Owner(uint64_t account_, bool host_, const SummonPolicy& policy_, const CatchUp& catch_up_)
        : account(account_), host(host_), policy(policy_), catch_up(catch_up_) {}
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;

    const uint64_t account;
    const bool host;        // the player whose game hosts the world (its file is the old <slot>.txt)
    uint32_t player = 0;    // their entity in the server world now (0: not in it)
    Companion companion;
    SummonPolicy policy;
    Orders orders;
    GearBook gear;
    CatchUp catch_up;
    ThreatBox threats;  // targets for its program (each player's own: take() drops what it does not hand out)
    std::atomic<bool> died{false};  // the program saw its companion die; taken by the server tick
    bool away_out = false;          // it was out when they left: it comes back when they return (and the file says out)
    bool loaded = false;            // their file was read this session
    int missed = 0;                 // checks in a row that did not find them in the world
    std::string saved_text;         // what their file holds now
    bool return_after_load = false;  // pieces left "returning" by a crash go back once they are in the world
    std::atomic<uint32_t> taken_pide{0};
    uint64_t taken_check_ms = 0;
    uintptr_t body_tpl = 0;         // the companion's template data at its spawn (who it is: ids are reused)
    int body_faction = -1;
    uint32_t follow_applied = 0;
    uint32_t no_faction_said = 0;
    uint32_t client_checked = 0;
    uint64_t last_level_ms = 0;
    uint64_t last_report_ms = 0;

    // First seen this session (an owner lives for one session): auto_summon wants one from now on, and so does
    // `was_out` (their companion was out when the last session ended: it comes back as after a reload, also when no load
    // notice comes and their file is never read).
    void begin(uint32_t session, uint64_t now_ms, bool was_out = false) {
        if (was_out) policy.on_summoned();
        policy.on_session(session, now_ms);
        orders.on_session(session);
    }
    // Their file, read once this session after the world's save loaded: gear, order, and whether one comes back.
    void load(const SavedCompanion& s, uint64_t now_ms) {
        gear.load(s.gear);
        if (!orders.parse(s.order)) orders.follow();
        if (s.out) policy.want_on_load();
        policy.on_loaded(now_ms);
        return_after_load = !gear.unreturned().empty();
        loaded = true;
    }
    // What their file should say: out while it is out, coming back, or away with them.
    SavedCompanion saved() const {
        SavedCompanion s;
        s.out = companion.phase() == Companion::Phase::Active || policy.coming() || away_out;
        s.order = orders.serialize();
        s.gear = gear.serialize();
        return s;
    }
    // Once a second, they are in the world.
    void seen() { missed = 0; }
    // Once a second, they are not: true from the second check in a row (one check may miss them for a moment).
    bool missing() { return ++missed >= 2; }
    // They left the world: a companion out is dismissed and comes back when they return, its order kept. The markers
    // to destroy now.
    std::vector<uint32_t> on_left() {
        player = 0;
        if (companion.phase() != Companion::Phase::Active) return {};
        away_out = true;
        companion.request_stop();
        return orders.release_markers_keep_order();
    }
    // They are in the world again (a new entity): one that was out when they left comes back to them (as a lost one:
    // once they stand still).
    void on_back(uint32_t entity, uint64_t now_ms) {
        player = entity;
        if (!away_out) return;
        away_out = false;
        policy.on_summoned(true);  // coming back with them is no loss: the count of losses starts over
        policy.on_lost(now_ms);
    }
};

// companion.as <player entity> <word>: the arguments as the console gives them (args[0] is the command's own name).
// nullopt when a part is missing or the entity is not a number above 0. The word in lower case.
struct AsOrder {
    uint32_t player = 0;
    std::string word;
};
inline std::optional<AsOrder> parse_as(const std::vector<std::string>& args) {
    if (args.size() < 3 || args[1].empty()) return std::nullopt;
    char* end = nullptr;
    const unsigned long player = std::strtoul(args[1].c_str(), &end, 10);
    if (*end || player == 0 || player > 0xFFFFFFFFul) return std::nullopt;
    AsOrder a;
    a.player = static_cast<uint32_t>(player);
    for (char c : args[2]) a.word += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return a;
}

class Owners {
public:
    // The file (in emf/config/companion) of `account`'s companion in world slot `slot`: the host's is <slot>.txt, as
    // before; another player's <slot>-<account as 16 hex digits>.txt.
    static std::wstring file_name(uint16_t slot, uint64_t account, bool host) {
        wchar_t name[48];
        if (host)
            swprintf(name, 48, L"%04x.txt", slot);
        else
            swprintf(name, 48, L"%04x-%016llx.txt", slot, static_cast<unsigned long long>(account));
        return name;
    }

    // Server tick: the owner of `account`, made with these defaults when there is none.
    Owner& get(uint64_t account, bool host, const SummonPolicy& policy, const CatchUp& catch_up) {
        auto it = owners_.find(account);
        if (it != owners_.end()) return *it->second;
        std::lock_guard<std::mutex> l(m_);
        auto& o = owners_[account];
        o = std::make_unique<Owner>(account, host, policy, catch_up);
        return *o;
    }
    // Server tick: the owner of `account`, or nullptr.
    Owner* find(uint64_t account) {
        auto it = owners_.find(account);
        return it == owners_.end() ? nullptr : it->second.get();
    }
    // A new session (another world, a reload): nobody's set carries over. The accounts whose companion was out (or away
    // with them), for begin()'s was_out.
    std::set<uint64_t> clear() {
        std::set<uint64_t> out;
        for (const auto& [account, o] : owners_)
            if (o->companion.phase() == Companion::Phase::Active || o->away_out) out.insert(account);
        std::lock_guard<std::mutex> l(m_);
        owners_.clear();
        return out;
    }
    // Every owner: `fn(Owner&)`. The server tick calls it without the lock (it alone changes the map); the program
    // inside lock().
    template <class Fn>
    void each(Fn fn) {
        for (auto& [account, o] : owners_) fn(*o);
    }
    std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(m_); }
    // The owner whose companion is `entity`, or nullptr.
    Owner* owner_of_companion(uint32_t entity) {
        if (!entity) return nullptr;
        for (auto& [account, o] : owners_)
            if (o->companion.entity() == entity) return o.get();
        return nullptr;
    }
    // Companions out or coming back, of every player in the world (one who left holds no place: theirs asks for one
    // again when they return).
    size_t out_count() const {
        size_t n = 0;
        for (const auto& [account, o] : owners_)
            n += o->player && (o->companion.phase() == Companion::Phase::Active || o->policy.coming());
        return n;
    }
    // Whether one more may be summoned with the server's limit `max` (0: no limit). `coming`: this one is counted
    // already (a summon that was armed or wanted).
    bool may_summon(uint32_t max, bool coming) const {
        if (max == 0) return true;
        size_t n = out_count();
        if (coming && n > 0) --n;
        return n < max;
    }
    size_t size() const { return owners_.size(); }

private:
    std::mutex m_;
    std::map<uint64_t, std::unique_ptr<Owner>> owners_;
};

}  // namespace companion
