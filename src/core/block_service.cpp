// Breaking blocks: the queue and the step (block_service.h).
#include "core/block_service.h"

#include <algorithm>
#include <cstring>

namespace emf::core {

uint32_t BlockService::request(const EmfBreakBlocks& r, EmfBreakCallback cb, void* user, std::string* why) {
    const std::string bad = break_check(r);
    if (!bad.empty()) {
        if (why) *why = bad;
        return 0;
    }
    Request q;
    q.spec = break_spec(r);
    q.spheres = hole_spheres(r.x, r.y, r.z, request_radius(r), r.height);
    q.drops = (r.flags & EMF_BREAK_DROPS) != 0;
    q.mid[0] = r.x;
    q.mid[1] = r.y + r.height / 2.0f;
    q.mid[2] = r.z;
    q.cb = cb;
    q.user = user;
    AcquireSRWLockExclusive(&lock_);
    uint32_t id = 0;
    if (waiting_.size() < kBlockRequestsMax) {
        id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;
        q.id = id;
        waiting_.push_back(std::move(q));
    }
    ReleaseSRWLockExclusive(&lock_);
    if (!id && why) *why = "64 break requests are already waiting";
    return id;
}

BlockDone BlockService::answer(const Request& r, uint32_t status, const KernelCount* k, uint32_t dropped) {
    BlockDone d;
    d.id = r.id;
    d.cb = r.cb;
    d.user = r.user;
    d.result.size = sizeof d.result;
    d.result.status = status;
    if (k) {
        d.result.damaged = k->damaged;
        d.result.removed = k->removed;
    }
    d.result.dropped = dropped;
    return d;
}

std::vector<BlockDone> BlockService::step(const BlockEngine& e) {
    std::vector<Request> run;
    AcquireSRWLockExclusive(&lock_);
    size_t spheres = 0, n = 0;
    while (n < waiting_.size() && spheres + waiting_[n].spheres.size() <= kBlockMaxSpheres) spheres += waiting_[n++].spheres.size();
    run.assign(std::make_move_iterator(waiting_.begin()), std::make_move_iterator(waiting_.begin() + n));
    waiting_.erase(waiting_.begin(), waiting_.begin() + n);
    ReleaseSRWLockExclusive(&lock_);
    std::vector<BlockDone> done;
    if (run.empty()) return done;

    MaterialFacts facts[128] = {};
    if (!e.facts || !e.facts(facts)) {
        for (const Request& r : run) done.push_back(answer(r, EMF_BREAK_FAILED));
        return done;
    }
    const float factor = e.mining_factor ? e.mining_factor() : 1.0f;
    for (size_t i = 0; i < run.size(); ++i) {
        const Request& r = run[i];
        float table[256];
        build_break_table(facts, r.spec, factor, table);
        KernelCount k;
        bool ok = true;
        for (const Sphere& whole : r.spheres) {
            Sphere s = whole;
            if (!r.spec.world && e.shrink) {
                s.r = e.shrink(whole, table);
                if (!(s.r > 0)) continue;  // right next to the world's own blocks: skipped
            }
            if (!e.kernel || !e.kernel(s, table, r.spec.instant, &k)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            done.push_back(answer(r, EMF_BREAK_FAILED, &k));
            // the rest of this step waits, in front of what came in meanwhile
            AcquireSRWLockExclusive(&lock_);
            waiting_.insert(waiting_.begin(), std::make_move_iterator(run.begin() + static_cast<std::ptrdiff_t>(i) + 1),
                            std::make_move_iterator(run.end()));
            ReleaseSRWLockExclusive(&lock_);
            return done;
        }
        uint32_t dropped = 0;
        if (r.drops && e.drop)
            for (const DropStack& d : break_drops(k.removed_per_material, facts, carry_))
                dropped += e.drop(facts[d.building_id].item_guid, d.count, r.mid);
        done.push_back(answer(r, EMF_BREAK_DONE, &k, dropped));
    }
    return done;
}

std::vector<BlockDone> BlockService::cancel_all() {
    std::vector<Request> all;
    AcquireSRWLockExclusive(&lock_);
    all.swap(waiting_);
    ReleaseSRWLockExclusive(&lock_);
    std::vector<BlockDone> done;
    for (const Request& r : all) done.push_back(answer(r, EMF_BREAK_CANCELLED));
    return done;
}

std::vector<BlockDone> BlockService::world_reset() {
    std::vector<BlockDone> done = cancel_all();
    std::memset(carry_, 0, sizeof carry_);
    return done;
}

size_t BlockService::waiting() const {
    AcquireSRWLockShared(&lock_);
    const size_t n = waiting_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

}  // namespace emf::core
