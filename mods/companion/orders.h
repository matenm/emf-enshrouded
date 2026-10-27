#pragma once
// companion: the player's orders (follow / stay / attack), without the game (unit-tested). The server tick turns the
// actions into game events: guard the player or a marker entity (StartGuardingEvent), spawn/destroy the marker, raise
// the companion's threat toward a target (AddThreatTargetEvent). docs/engine_parts/re_companion_orders.md.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "emf/aim.hpp"

namespace companion {

using Vec3 = emf::aim::Vec3;

enum class Mode : uint8_t { Follow, Stay, Attack };

struct OrderAction {
    enum Kind : uint8_t { GuardPlayer, GuardEntity, SpawnMarker, DestroyMarker, Threat, Notice } kind;
    uint32_t id = 0;   // GuardEntity / DestroyMarker / Threat
    Vec3 pos;          // SpawnMarker
    std::string text;  // Notice
};

struct EntityState {
    Vec3 pos;
    bool alive = true;
};
// An entity's position and whether it is alive; nothing if it does not exist.
using OrderLookup = std::function<std::optional<EntityState>(uint32_t id)>;

class Orders {
public:
    static constexpr double kAttackRange = 40.0;     // metres from the player
    static constexpr uint64_t kThreatEveryMs = 2000;
    static constexpr uint64_t kAttackMaxMs = 30000;
    static constexpr uint64_t kMarkerRetryMs = 5000;  // a marker that vanishes again is put back at most this often
    static constexpr int kMarkerTries = 3;            // vanishing this many times in a row: the companion follows

    Mode mode() const { return mode_; }
    // The entity the companion should guard: 0 = the player, else the stay marker.
    uint32_t guard_target() const { return mode_ == Mode::Stay ? marker_ : 0; }

    void follow() {
        leave_stay();
        mode_ = Mode::Follow;
        guard_player_ = true;
    }
    void stay(const Vec3& where) {
        leave_stay();
        mode_ = Mode::Stay;
        stay_pos_ = where;
        need_marker_ = true;
    }
    void attack(uint32_t target, uint64_t now_ms) {
        leave_stay();
        mode_ = Mode::Attack;
        target_ = target;
        attack_since_ms_ = now_ms;
        next_threat_ms_ = now_ms;
        guard_player_ = true;  // it attacks while guarding the player
    }
    // The marker asked for by SpawnMarker exists now.
    void on_marker_spawned(uint32_t id) {
        if (mode_ != Mode::Stay || !waiting_marker_) return;
        waiting_marker_ = false;
        marker_ = id;
        guard_marker_ = true;
    }
    // A new hosted session: entity ids of the old world mean nothing now (they are reused).
    void on_session(uint32_t session) {
        if (session == session_) return;
        session_ = session;
        marker_ = 0;
        old_marker_ = 0;
        waiting_marker_ = false;
        if (mode_ == Mode::Stay) need_marker_ = true;
        if (mode_ == Mode::Attack) follow();
    }
    // The companion leaves (dismissed): back to follow, and the stay marker (if any) is handed to the caller to destroy
    // now, while its id is still the marker's.
    uint32_t release_marker() {
        follow();
        const uint32_t m = old_marker_;
        old_marker_ = 0;
        return m;
    }
    // A (new) companion entity: tell it the current order again.
    void on_companion_spawned() {
        if (mode_ != Mode::Stay)
            guard_player_ = true;
        else if (marker_)
            guard_marker_ = true;
        else if (!waiting_marker_)
            need_marker_ = true;
        if (mode_ == Mode::Attack) next_threat_ms_ = 0;
    }

    std::vector<OrderAction> tick(uint64_t now_ms, const Vec3& player, const OrderLookup& lookup) {
        std::vector<OrderAction> out;
        if (old_marker_) {
            out.push_back({OrderAction::DestroyMarker, old_marker_, {}, ""});
            old_marker_ = 0;
        }
        if (mode_ == Mode::Stay) {
            if (marker_ && !waiting_marker_) {
                if (lookup(marker_)) {
                    marker_fails_ = 0;
                } else {  // the marker is gone: put a new one (again and again: slower, then give up)
                    marker_ = 0;
                    need_marker_ = true;
                    if (++marker_fails_ > 1) next_marker_ms_ = now_ms + kMarkerRetryMs;
                }
            }
            if (need_marker_ && marker_fails_ >= kMarkerTries) {
                out.push_back({OrderAction::Notice, 0, {}, "The companion cannot keep its stay spot; it follows you"});
                follow();
                guard_player_ = false;
                out.push_back({OrderAction::GuardPlayer, 0, {}, ""});
                return out;
            }
            if (need_marker_ && now_ms >= next_marker_ms_) {
                need_marker_ = false;
                waiting_marker_ = true;
                out.push_back({OrderAction::SpawnMarker, 0, stay_pos_, ""});
            }
            if (guard_marker_ && marker_) {
                guard_marker_ = false;
                out.push_back({OrderAction::GuardEntity, marker_, {}, ""});
            }
            return out;
        }
        if (guard_player_) {
            guard_player_ = false;
            out.push_back({OrderAction::GuardPlayer, 0, {}, ""});
        }
        if (mode_ == Mode::Attack) {
            const auto t = lookup(target_);
            std::string why;
            if (!t || !t->alive)
                why = "The target is gone";
            else if (emf::aim::length(emf::aim::sub(t->pos, player)) > kAttackRange)
                why = "The target got away";
            else if (now_ms - attack_since_ms_ >= kAttackMaxMs)
                why = "The companion gave up the attack";
            if (!why.empty()) {
                out.push_back({OrderAction::Notice, 0, {}, why});
                follow();
                guard_player_ = false;
                out.push_back({OrderAction::GuardPlayer, 0, {}, ""});
                return out;
            }
            if (now_ms >= next_threat_ms_) {
                next_threat_ms_ = now_ms + kThreatEveryMs;
                out.push_back({OrderAction::Threat, target_, {}, ""});
            }
        }
        return out;
    }

    // "follow" or "stay x y z" (an attack is saved as follow).
    std::string serialize() const {
        if (mode_ != Mode::Stay) return "follow";
        char b[96];
        snprintf(b, sizeof(b), "stay %.2f %.2f %.2f", stay_pos_.x, stay_pos_.y, stay_pos_.z);
        return b;
    }
    bool parse(const std::string& line) {
        if (line == "follow") {
            follow();
            return true;
        }
        Vec3 p;
        char extra = 0;
        if (sscanf_s(line.c_str(), "stay %lf %lf %lf %c", &p.x, &p.y, &p.z, &extra, 1u) == 3) {
            stay(p);
            return true;
        }
        return false;
    }

private:
    void leave_stay() {
        if (marker_) old_marker_ = marker_;
        marker_ = 0;
        need_marker_ = waiting_marker_ = guard_marker_ = false;
        marker_fails_ = 0;
        next_marker_ms_ = 0;
    }
    Mode mode_ = Mode::Follow;
    uint32_t session_ = 0;
    Vec3 stay_pos_;
    uint32_t marker_ = 0, old_marker_ = 0;
    bool need_marker_ = false, waiting_marker_ = false, guard_marker_ = false, guard_player_ = false;
    uint32_t target_ = 0;
    uint64_t attack_since_ms_ = 0, next_threat_ms_ = 0;
    int marker_fails_ = 0;
    uint64_t next_marker_ms_ = 0;
};

// Threat targets handed from the server tick to the companion's program, tagged with the companion and session they
// are meant for; the program takes only its own (ids are reused after a reload or by a new companion). Not locked.
class ThreatBox {
public:
    void push(uint32_t companion, uint32_t session, uint32_t target) {
        if (items_.size() < 64) items_.push_back({companion, session, target});
    }
    // The targets for this companion and session; everything else is dropped.
    std::vector<uint32_t> take(uint32_t companion, uint32_t session) {
        std::vector<uint32_t> out;
        for (const auto& t : items_)
            if (t.companion == companion && t.session == session) out.push_back(t.target);
        items_.clear();
        return out;
    }

private:
    struct Item {
        uint32_t companion, session, target;
    };
    std::vector<Item> items_;
};

// Entities whose death the program saw, per session (an id reused in a later session is alive). Keeps the newest 256.
class DeathLog {
public:
    static constexpr size_t kMax = 256;
    void record(uint32_t session, uint32_t id) {
        if (ids_.size() >= kMax) ids_.pop_front();
        ids_.push_back({session, id});
    }
    bool dead(uint32_t session, uint32_t id) const {
        for (const auto& d : ids_)
            if (d.session == session && d.id == id) return true;
        return false;
    }
    size_t size() const { return ids_.size(); }

private:
    struct Dead {
        uint32_t session, id;
    };
    std::deque<Dead> ids_;
};

}  // namespace companion
