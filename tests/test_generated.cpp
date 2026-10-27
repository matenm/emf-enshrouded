// The generated header is itself the test: every struct carries static_asserts for its size and
// every field offset (from types.json). These cases check a few known values on top of that.
// windows.h first, so a generated name that clashes with a Windows macro breaks the build here.
#include <windows.h>

#include <doctest.h>

#include <cstddef>
#include <cstring>

#include "emf/generated/components.hpp"

namespace g = emf::gen;

TEST_CASE("generated: ActiveNpcState hash and size") {
    CHECK(g::ecs::ActiveNpcState::kHash == 0x1a8b5909u);
    CHECK(sizeof(g::ecs::ActiveNpcState) == 456);
    CHECK(offsetof(g::ecs::ActiveNpcState, decisionSetup) == 56);
}

TEST_CASE("generated: DynamicActiveNpcState layout") {
    CHECK(sizeof(g::ecs::DynamicActiveNpcState) == 136);
    CHECK(offsetof(g::ecs::DynamicActiveNpcState, decision) == 2);
    CHECK(offsetof(g::ecs::DynamicActiveNpcState, debugStateHistory) == 0x7A);
    g::ecs::DynamicActiveNpcState s{};
    s.state = g::active_npc::State::Roam;
    CHECK(static_cast<uint8_t>(s.state) == 7);
}

TEST_CASE("generated: CurrentTransform and WorldPosition") {
    CHECK(g::ecs::CurrentTransform::kHash == 0x027ac564u);
    CHECK(sizeof(g::ecs::CurrentTransform) == 56);
    CHECK(sizeof(g::WorldPosition) == 24);
    CHECK(offsetof(g::WorldTransform, orientation) == 0x18);
}

TEST_CASE("generated: Inventory is 8 item stacks") {
    CHECK(sizeof(g::ecs::Inventory) == 96);
    CHECK(sizeof(g::ItemStack) == 12);
}

TEST_CASE("generated: component registry lists every component once") {
    CHECK(g::kComponentCount > 650);  // direct and indirect subclasses of keen::ecs::Component, no abstract bases
    bool found = false, health = false, base_actor = false;
    for (const auto& c : g::kComponents) {
        found |= (c.hash == 0x1a8b5909u && c.size == 456);
        health |= std::strcmp(c.name, "keen::ecs::Health") == 0;
        base_actor |= std::strcmp(c.name, "keen::ecs::BaseActor") == 0;
    }
    CHECK(found);
    CHECK(health);
    CHECK_FALSE(base_actor);  // abstract
}

TEST_CASE("generated: components that derive from another component (Actor, Health) keep their base fields") {
    CHECK(g::ecs::Actor::kHash == 0x009e8b34u);
    CHECK(sizeof(g::ecs::Actor) == 3832);
    CHECK(offsetof(g::ecs::Actor, currentState) == 0xbf8);  // from BaseActor; bit 50 = IsReadyForGameplay
    CHECK(offsetof(g::ecs::Actor, currentServerAction) == 0xc20);
    CHECK(sizeof(g::ecs::Health) > 0);
}

TEST_CASE("generated: StartGuardingEvent layout, hash and short name") {
    using E = g::ecs::StartGuardingEvent;
    CHECK(sizeof(E) == 16);
    CHECK(E::kSize == 16);
    CHECK(E::kHash == 0xb096ec86u);
    CHECK(std::strcmp(E::kName, "StartGuardingEvent") == 0);
    CHECK(offsetof(E, timeStamp) == 0);
    CHECK(offsetof(E, guardingEntityId) == 8);
    CHECK(offsetof(E, guardedEntityId) == 12);
}

TEST_CASE("generated: other events keep their base fields") {
    CHECK(offsetof(g::ecs::EntityDiedEvent, deadEntityId) == 8);
    CHECK(sizeof(g::ecs::HitEvent) == 248);
    CHECK(offsetof(g::ecs::HitEvent, healthChange) == 188);
    CHECK(offsetof(g::ecs::HitEvent, targetId) == 148);  // from BaseHitEvent
    CHECK(std::strcmp(g::ecs::NpcCommandEvent::kName, "NpcCommandEvent") == 0);
}

TEST_CASE("generated: event registry lists the events with their sizes") {
    CHECK(g::kEventCount >= 127);
    bool found = false;
    for (const auto& e : g::kEvents) found |= (e.hash == 0xb096ec86u && e.size == 16 && std::strcmp(e.name, "StartGuardingEvent") == 0);
    CHECK(found);
}

TEST_CASE("generated: the dynamic halves have their own table (name, hash, size)") {
    bool found = false;
    for (size_t i = 0; i < g::kDynamicComponentCount; ++i)
        if (std::strcmp(g::kDynamicComponents[i].name, "keen::ecs::DynamicActiveNpcState") == 0) {
            found = true;
            CHECK(g::kDynamicComponents[i].hash == 0x9e65507au);
            CHECK(g::kDynamicComponents[i].size == 136);
        }
    CHECK(found);
    CHECK(g::kDynamicComponentCount >= 60);
}
