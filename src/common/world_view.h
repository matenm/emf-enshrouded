#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace emf {

enum class WorldKind : uint8_t { Server = 0, Prediction = 1, Client = 2 };

// Game function: getComponent(out {ptr,size}, world, qualifiedHash, entityId) -> out
using GetComponentFn = void* (*)(void* out, void* world, uint32_t hash, uint32_t entity);

// Game function: getTemplateComponent(world, qualifiedHash, entityId) -> template data pointer or null
using GetTemplateComponentFn = void* (*)(uint8_t* world, uint32_t hash, uint32_t entity);

struct ComponentRef {
    void* ptr = nullptr;
    uint64_t size = 0;
};

// Safe, read-only accessors on an ECS world pointer. Every read is fault-guarded:
// a bad pointer or a half-built world yields 0 / nullptr / nothing, never a crash.
class WorldView {
public:
    explicit WorldView(uint8_t* world) : world_(world) {}
    static WorldView from_storage(void* storage);

    uint8_t* raw() const { return world_; }
    uint8_t state() const;
    uint8_t kind() const;
    uint32_t tick() const;
    int64_t tick_length_ns() const;
    uint32_t entity_count() const;
    uint64_t singleton_count() const;
    void* singleton(std::string_view name) const;
    // u16 value stored under `key` in the Keen map at world+map_offset (plain keys, no mixing), or nullopt.
    std::optional<uint16_t> map_u16(uint32_t map_offset, uint32_t key) const;
    // The game's own size of a template component (its reflection descriptor, checked against the hash), or nullopt.
    std::optional<uint32_t> template_component_size(uint32_t hash) const;
    // Whether `hash` is a per-entity ("dynamic") component of this world (the registry's dynamic map, where the
    // engine resolves read/write/other-entity names); nullopt when the registry cannot be read.
    std::optional<bool> has_dynamic_component(uint32_t hash) const;
    // EntityIds currently in the entity map (snapshot).
    std::vector<uint32_t> entity_ids() const;
    template <class F>
    void for_each_entity(F f) const {
        for (uint32_t id : entity_ids()) f(id);
    }

private:
    uint8_t* world_;
};

// Calls the game's getComponent under a fault guard.
ComponentRef get_component(GetComponentFn fn, uint8_t* world, uint32_t hash, uint32_t entity);
// Calls the game's getTemplateComponent under a fault guard; null for a null fn/world, a miss or a fault.
void* get_template_component(GetTemplateComponentFn fn, uint8_t* world, uint32_t hash, uint32_t entity);

}  // namespace emf
