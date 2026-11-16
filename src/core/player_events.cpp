#include "core/player_events.h"

#include <cstdio>
#include <cstring>

#include "common/seh.h"
#include "core/manifest.h"
#include "emf/generated/components.hpp"

namespace emf::core {

namespace gen = emf::gen::ecs;

std::vector<RosterSeen> read_players(const EmfApi* api, EmfWorld* w) {
    std::vector<RosterSeen> out;
    if (!api || !w || !api->get_component) return out;
    const auto* infos = api->singleton ? static_cast<const uint8_t*>(api->singleton(w, "BasicPlayerInfos")) : nullptr;
    const uint32_t host = api->local_player ? api->local_player() : 0;
    for (uint32_t id = 1; id <= 1023; ++id) {
        uint64_t size = 0;
        if (!api->get_component(w, gen::PlayerState::kHash, id, &size)) continue;
        if (!api->get_component(w, gen::CurrentTransform::kHash, id, &size)) continue;
        RosterSeen s;
        s.entity = id;
        s.host = host != 0 && id == host;
        uint8_t head[16];
        if (infos && emf::seh_read(infos + static_cast<size_t>(id - 1) * 0xA0, head, sizeof head)) {
            uint32_t held = 0;
            std::memcpy(&held, head, 4);
            if (held == id) std::memcpy(&s.account, head + 8, 8);
        }
        s.ready = true;
        uint64_t in_size = 0;
        const auto* in = static_cast<const uint8_t*>(api->get_component(w, gen::PlayerInput::kHash, id, &in_size));
        constexpr size_t kReady = offsetof(gen::PlayerInput, fromClientIsReadyForGameplay);
        uint8_t ready = 1;
        if (!s.host && in && in_size > kReady && emf::seh_read(in + kReady, &ready, 1)) s.ready = ready != 0;
        char name[65] = {};  // a connection's name is 1-64 bytes: read whole
        if (api->player_name && api->player_name(w, id, name, sizeof name)) s.name = name;
        out.push_back(std::move(s));
    }
    return out;
}

bool PlayerEvents::watch(EmfPlayerCallback cb, void* user) {
    if (!cb) return false;
    std::lock_guard<std::mutex> l(m_);
    watchers_.push_back({cb, user, true});
    watched_ = true;
    return true;
}

void PlayerEvents::world_loaded() { arm_pending_ = true; }

void PlayerEvents::hit(uint32_t target, uint32_t source) {
    if (watched_ && target >= 1 && target <= 1023) roster_.hit(target, source, d_.now_ms ? d_.now_ms() : 0);
}

void PlayerEvents::died(uint32_t entity) {
    if (watched_ && entity >= 1 && entity <= 1023) deaths_.push_back(entity);
}

void PlayerEvents::server_step(EmfWorld* w, uint32_t session) {
    if (!watched_) return;
    const uint64_t now = d_.now_ms ? d_.now_ms() : 0;
    if (session != session_) {  // another world: nobody is known until its load
        session_ = session;
        roster_.reset(UINT64_MAX);
        armed_ = false;
        deaths_.clear();
    }
    if (arm_pending_) {  // 1 s from this first step after the load (the mods' world data first)
        arm_pending_ = false;
        arm_at_ = now + kPlayerArmDelayMs;
        roster_.reset(arm_at_);
        armed_ = true;
        read_once_ = false;
        deaths_.clear();
    }
    introduce(w);
    std::vector<RosterEvent> events;
    for (uint32_t entity : deaths_)  // after the relay handed this step's hits too
        if (auto e = roster_.died(entity, now)) events.push_back(std::move(*e));
    deaths_.clear();
    if (armed_ && now >= arm_at_ && (!read_once_ || now - last_read_ >= kPlayerReadMs || now < last_read_)) {
        last_read_ = now;
        read_once_ = true;
        auto changes = roster_.update(d_.read ? d_.read(w) : std::vector<RosterSeen>{}, now);
        events.insert(events.end(), changes.begin(), changes.end());
    }
    if (!events.empty()) deliver(w, events);
}

void PlayerEvents::introduce(EmfWorld* w) {
    std::vector<Watcher> late;
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& x : watchers_)
            if (x.intro) {
                x.intro = false;
                if (x.on) late.push_back(x);
            }
    }
    if (late.empty() || !armed_ || !read_once_) return;  // nobody announced yet: the arming's joins reach them
    const auto present = roster_.present();
    for (const auto& x : late)
        for (const auto& ev : present)
            if (!call(w, x, ev)) break;
}

bool PlayerEvents::call(EmfWorld* w, const Watcher& x, const RosterEvent& ev) {
    EmfPlayerEvent e{};
    e.size = sizeof e;
    e.kind = ev.kind;
    e.account = ev.account;
    e.player = ev.entity;
    e.killer = ev.killer;
    e.host = ev.host ? 1 : 0;
    const size_t name_bytes = utf8_fit(ev.name, sizeof e.name - 1);  // cut between characters, never inside one
    std::memcpy(e.name, ev.name.data(), name_bytes);
    e.name[name_bytes] = 0;
    if (d_.call && d_.call(x.cb, w, &e, x.user)) return true;
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& y : watchers_)
            if (y.cb == x.cb && y.user == x.user) y.on = false;
    }
    if (d_.log)
        d_.log("an on_player callback in " +
               (d_.module_of ? d_.module_of(reinterpret_cast<const void*>(x.cb)) : std::string("a mod")) +
               " faulted and was switched off");
    return false;
}

void PlayerEvents::deliver(EmfWorld* w, const std::vector<RosterEvent>& events) {
    std::vector<Watcher> list;
    {
        std::lock_guard<std::mutex> l(m_);
        list = watchers_;
    }
    for (const auto& ev : events)
        for (auto& x : list)
            if (x.on && !call(w, x, ev)) x.on = false;
}

}  // namespace emf::core
