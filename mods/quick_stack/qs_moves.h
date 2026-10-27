#pragma once
// Quick Stack and Chest Machines: the item moves (take, then put, then give back what did not fit), kept apart from
// quick_stack.cpp so tests run them over a fake API. The rules every move keeps:
//   - nothing taken is ever lost: what does not fit goes back where it came from; a put that is refused, or an entity
//     that is gone (a give needs its transform), falls back to the other end of the move, then to the nearest player,
//     and a line in the log says so when even that fails;
//   - a result that arrives after the hosted session changed (a reload, another world) touches nothing: its entity ids
//     belong to other entities now;
//   - the game's item queue (64 waiting requests, 8 handled a step) is never flooded by this mod: a move reserves its
//     room before its take (Requests), and a recycling stays busy until every one of its puts has answered.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "emf/emf.hpp"
#include "emf/query.hpp"
#include "quickstack.h"

namespace quickstack {

// This mod's requests in the core's item queue. A move holds one place from its take to its last answer (its take, its
// put and a give back come one after another); a recycling holds one per material put. kMax leaves room for other mods.
class Requests {
public:
    static constexpr int kMax = 40;
    int free() const { return kMax - used_; }
    int used() const { return used_; }
    bool reserve(int n) {
        if (n <= 0 || used_ + n > kMax) return false;
        used_ += n;
        return true;
    }
    void release(int n = 1) { used_ = (std::max)(0, used_ - n); }
    void clear() { used_ = 0; }

private:
    int used_ = 0;
};

// Drops `n` of `item` at `first`, else at `second`, else at the nearest player who is there; the entity it was dropped
// at, or 0 (nowhere: the caller logs it as lost).
inline uint32_t drop_somewhere(emf::World w, uint32_t first, uint32_t second, uint32_t item, uint32_t n) {
    const EmfApi* a = emf::api();
    if (!a || !a->give_item || !n) return 0;
    if (first && a->give_item(w.raw(), first, item, n)) return first;
    if (second && a->give_item(w.raw(), second, item, n)) return second;
    const auto close_to = emf::position(w, second ? second : first);
    std::vector<std::pair<double, uint32_t>> players;
    for (uint32_t id : emf::players(w)) {
        const auto p = emf::position(w, id);
        players.push_back({close_to && p ? emf::distance(*p, *close_to) : 0.0, id});
    }
    std::sort(players.begin(), players.end());
    for (const auto& [d, id] : players)
        if (id != first && id != second && a->give_item(w.raw(), id, item, n)) return id;
    return 0;
}

inline void log_drop(const char* mod, uint32_t where, uint32_t intended, uint32_t item, uint32_t n) {
    if (!where) EMF_LOG(mod, "LOST %u of item %08x: %u and everyone else were gone (please report)", n, item, intended);
    else if (where != intended) EMF_LOG(mod, "%u of item %08x dropped at %u (%u was gone)", n, item, where, intended);
    else EMF_LOG(mod, "%u of item %08x had no room in %u: dropped beside it", n, item, intended);
}

// A result of an earlier session: it must not touch the entities that now have its ids.
inline bool same_session(uint32_t session, const char* mod, const char* what) {
    if (emf::server_session() == session) return true;
    EMF_LOG(mod, "%s answered after the world changed: nothing done with it (the old world's save decides)", what);
    return false;
}

// Gives `n` of `item` back into `owner` (a put); what does not fit, or a refused put, is dropped at owner, else at
// `other`, else at the nearest player. `then` (optional) runs once at the end.
inline void give_back(emf::World w, uint32_t owner, uint32_t other, uint32_t item, uint32_t n, uint32_t session,
                      const char* mod, std::function<void()> then = nullptr) {
    auto finish = std::make_shared<std::function<void()>>(std::move(then));
    const bool q = emf::put_items_ex(w, owner, item, n, 0, 0xFF, [owner, other, item, n, session, mod, finish](emf::World w2, uint32_t back) {
        if (same_session(session, mod, "a give back") && back < n)
            log_drop(mod, drop_somewhere(w2, owner, other, item, n - back), owner, item, n - back);
        if (*finish) (*finish)();
    });
    if (!q) {
        log_drop(mod, drop_somewhere(w, owner, other, item, n), owner, item, n);
        if (*finish) (*finish)();
    }
}

// One move: take m.count of m.item from exactly m.slot of `from`, put it into m.chest, and give back into `from` what
// does not fit. It holds one place in `req` from its take to its end (false, nothing done, when there is none).
// done(world, added, taken) runs once at the end, also when the take found nothing, was refused, or the world changed.
using MoveDone = std::function<void(emf::World, uint32_t added, uint32_t taken)>;
inline bool move_stack(emf::World w, uint32_t from, const Move& m, uint32_t session, Requests* req, const char* mod,
                       MoveDone done) {
    if (!req->reserve(1)) return false;
    auto end = std::make_shared<MoveDone>(std::move(done));
    auto finish = [req, end](emf::World wx, uint32_t added, uint32_t taken) {
        req->release(1);
        if (*end) (*end)(wx, added, taken);
    };
    const bool queued = emf::take_items(w, from, m.item, m.count, m.slot, [from, m, session, mod, finish](emf::World w2, uint32_t taken) {
        if (!taken || !same_session(session, mod, "a take")) {
            finish(w2, 0, taken);
            return;
        }
        const bool q = emf::put_items_ex(w2, m.chest, m.item, taken, 0, 0xFF, [from, m, taken, session, mod, finish](emf::World w3, uint32_t added) {
            if (!same_session(session, mod, "a put")) {
                finish(w3, 0, taken);
                return;
            }
            if (added < taken)  // no room for the rest: it goes back where it came from
                give_back(w3, from, m.chest, m.item, taken - added, session, mod, [finish, w3, added, taken] { finish(w3, added, taken); });
            else
                finish(w3, added, taken);
        });
        if (!q) give_back(w2, from, m.chest, m.item, taken, session, mod, [finish, w2, taken] { finish(w2, 0, taken); });
    }, true);  // exactly that slot
    if (!queued) {
        req->release(1);
        return false;
    }
    return true;
}

// One recycling: take plan.take of `item` from exactly `slot` of the recycler (all or nothing), then put each of
// plan.refund into `target`; what does not fit there is dropped at the target, else the recycler, else the nearest
// player. It holds one place in `req` per material put (false, nothing done, when there are not that many).
// done(world, recycled) runs once, after every put has answered (or the take failed, or the world changed).
using RecycleDone = std::function<void(emf::World, bool recycled)>;
inline bool recycle_stack(emf::World w, uint32_t recycler, uint32_t target, uint32_t item, uint64_t slot, const Recycle& plan,
                          uint32_t session, Requests* req, const char* mod, RecycleDone done) {
    const int places = (std::max)(1, static_cast<int>(plan.refund.size()));
    if (!req->reserve(places)) return false;
    auto end = std::make_shared<RecycleDone>(std::move(done));
    const bool queued = emf::take_items(w, recycler, item, plan.take, slot, [recycler, target, item, plan, session, mod, req, places, end](emf::World w2, uint32_t taken) {
        if (!same_session(session, mod, "a recycler's take")) {
            req->release(places);
            if (*end) (*end)(w2, false);
            return;
        }
        if (taken < plan.take) {  // all or nothing: busy, or the stack changed; tried again next time
            req->release(places);
            if (taken) give_back(w2, recycler, target, item, taken, session, mod);
            if (*end) (*end)(w2, false);
            return;
        }
        if (plan.refund.empty()) {
            req->release(places);
            if (*end) (*end)(w2, true);
            return;
        }
        auto pending = std::make_shared<size_t>(plan.refund.size());
        auto one_done = [req, end, pending](emf::World wx) {
            req->release(1);
            if (--*pending == 0 && *end) (*end)(wx, true);
        };
        for (const auto& s : plan.refund) {
            const uint32_t mat = s.item, n = s.count;
            const bool q = emf::put_items_ex(w2, target, mat, n, 0, 0xFF, [target, recycler, mat, n, session, mod, one_done](emf::World w3, uint32_t added) {
                if (same_session(session, mod, "a recycler's put") && added < n)
                    log_drop(mod, drop_somewhere(w3, target, recycler, mat, n - added), target, mat, n - added);
                one_done(w3);
            });
            if (!q) {
                log_drop(mod, drop_somewhere(w2, target, recycler, mat, n), target, mat, n);
                one_done(w2);
            }
        }
    }, true);
    if (!queued) {
        req->release(places);
        return false;
    }
    return true;
}

}  // namespace quickstack
