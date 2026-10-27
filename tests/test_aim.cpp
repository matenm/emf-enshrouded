// emf/aim.hpp: what the player looks at (the camera ray from PlayerInput) and which candidate is under the crosshair.
#include <doctest.h>

#include <cmath>
#include <vector>

#include "emf/aim.hpp"

using emf::aim::Quat;
using emf::aim::Ray;
using emf::aim::Vec3;

namespace {
emf::gen::ecs::PlayerInput g_input{};
void* aim_get_component(EmfWorld*, uint32_t hash, uint32_t entity, uint64_t* size) {
    if (hash == emf::gen::ecs::PlayerInput::kHash && entity == 1) {
        if (size) *size = sizeof(g_input);
        return &g_input;
    }
    if (size) *size = 0;
    return nullptr;
}
}  // namespace

TEST_CASE("aim: the camera looks along its orientation applied to +z (values seen in game)") {
    const Vec3 west = emf::aim::forward(Quat{0.092, -0.701, 0.092, 0.701});  // looked toward -x
    CHECK(west.x < -0.9);
    const Vec3 north = emf::aim::forward(Quat{0.131, 0.004, -0.001, 0.991});  // toward +z, slightly down
    CHECK(north.z > 0.9);
    CHECK(north.y < 0.0);
}

TEST_CASE("aim: pick takes the point closest to the ray, in front of it, within range of the player") {
    const Ray ray{{0, 0, 0}, {0, 0, 1}};
    const Vec3 player{0, 0, 2};
    std::vector<Vec3> pts = {{0.9, 0, 6}, {0.2, 0, 7}, {0, 0, -3}};
    auto i = emf::aim::pick(ray, player, pts);
    REQUIRE(i);
    CHECK(*i == 1);  // 0.2 m from the ray beats 0.9 m
    pts = {{0, 0, -3}};  // behind the camera
    CHECK_FALSE(emf::aim::pick(ray, player, pts));
    pts = {{0.1, 0, 30}};  // in line but too far from the player (default range 8 m)
    CHECK_FALSE(emf::aim::pick(ray, player, pts));
    CHECK(emf::aim::pick(ray, player, pts, {40.0, 1.2, 0.25}));  // a longer range finds it
    pts = {{0.3, 0, 9}, {0.1, 0, 5}};  // both near the ray (within the 0.25 m tie): the nearer one along the ray
    i = emf::aim::pick(ray, player, pts);
    REQUIRE(i);
    CHECK(*i == 1);
    pts = {{2.0, 0, 5}};  // 2 m off the ray
    CHECK_FALSE(emf::aim::pick(ray, player, pts));
}

TEST_CASE("aim: camera_ray reads the player's camera from PlayerInput (fixed-point position, unit direction)") {
    static EmfApi api{};
    api.version = EMF_API_VERSION;
    api.size = sizeof(EmfApi);
    api.get_component = aim_get_component;
    REQUIRE(emf::init(&api));
    g_input = {};
    auto& cam = g_input.fromClient.camera;
    cam.position.x = static_cast<int64_t>(10.5 * 4294967296.0);
    cam.position.y = static_cast<int64_t>(905.0 * 4294967296.0);
    cam.position.z = static_cast<int64_t>(-2.0 * 4294967296.0);
    cam.orientation.x = 0.0f;
    cam.orientation.y = 0.0f;
    cam.orientation.z = 0.0f;
    cam.orientation.w = 1.0f;
    const auto r = emf::aim::camera_ray(emf::World(reinterpret_cast<EmfWorld*>(0x10)), 1);
    REQUIRE(r);
    CHECK(r->origin.x == doctest::Approx(10.5));
    CHECK(r->origin.y == doctest::Approx(905.0));
    CHECK(r->origin.z == doctest::Approx(-2.0));
    CHECK(r->dir.z == doctest::Approx(1.0));
    CHECK_FALSE(emf::aim::camera_ray(emf::World(reinterpret_cast<EmfWorld*>(0x10)), 2));  // no PlayerInput
}
