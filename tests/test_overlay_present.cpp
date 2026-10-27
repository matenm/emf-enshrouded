#include <doctest.h>

#include <atomic>
#include <vector>

#include "core/overlay/vk_overlay.h"

using namespace emf::core;

namespace {
struct PresentLog {
    int calls = 0;
    std::vector<VkSemaphore> waits;
} g_pl;
VkResult VKAPI_PTR fake_present(VkQueue, const VkPresentInfoKHR* info) {
    ++g_pl.calls;
    g_pl.waits.assign(info->pWaitSemaphores, info->pWaitSemaphores + info->waitSemaphoreCount);
    return VK_SUCCESS;
}
int g_renders = 0;
bool render_ok(VkQueue, const VkPresentInfoKHR*, VkSemaphore* signal, void*) {
    ++g_renders;
    *signal = reinterpret_cast<VkSemaphore>(uintptr_t(0x1234));
    return true;
}
bool render_faults(VkQueue, const VkPresentInfoKHR*, VkSemaphore*, void*) {
    ++g_renders;
    *static_cast<volatile int*>(nullptr) = 1;
    return true;
}
const VkSemaphore kGameSem = reinterpret_cast<VkSemaphore>(uintptr_t(0x77));
VkPresentInfoKHR game_info() {
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &kGameSem;
    return info;
}
}  // namespace

TEST_CASE("present_through: closed goes straight to the game's present; open renders and presents after our work") {
    std::atomic<bool> disabled{false};
    const VkPresentInfoKHR info = game_info();
    g_pl = {};
    g_renders = 0;
    CHECK(present_through(nullptr, &info, false, &disabled, render_ok, nullptr, fake_present) == VK_SUCCESS);
    CHECK(g_renders == 0);
    CHECK(g_pl.calls == 1);
    CHECK(g_pl.waits == std::vector<VkSemaphore>{kGameSem});
    CHECK(present_through(nullptr, &info, true, &disabled, render_ok, nullptr, fake_present) == VK_SUCCESS);
    CHECK(g_renders == 1);
    CHECK(g_pl.calls == 2);
    CHECK(g_pl.waits == std::vector<VkSemaphore>{reinterpret_cast<VkSemaphore>(uintptr_t(0x1234))});
}

TEST_CASE("present_through: a faulting draw still presents and disables the overlay") {
    std::atomic<bool> disabled{false};
    const VkPresentInfoKHR info = game_info();
    g_pl = {};
    g_renders = 0;
    CHECK(present_through(nullptr, &info, true, &disabled, render_faults, nullptr, fake_present) == VK_SUCCESS);
    CHECK(disabled);
    CHECK(g_pl.calls == 1);
    CHECK(g_pl.waits == std::vector<VkSemaphore>{kGameSem});
    CHECK(present_through(nullptr, &info, true, &disabled, render_faults, nullptr, fake_present) == VK_SUCCESS);
    CHECK(g_renders == 1);  // disabled: not tried again
    CHECK(g_pl.calls == 2);
}
