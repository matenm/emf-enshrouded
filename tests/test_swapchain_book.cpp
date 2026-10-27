#include <doctest.h>

#include "core/overlay/swapchain_book.h"

using namespace emf::core;

namespace {
template <class H>
H handle(uintptr_t v) {
    return reinterpret_cast<H>(v);
}
}  // namespace

TEST_CASE("SwapchainBook: a swapchain is found until destroyed; a replacement keeps the old one until it is destroyed") {
    SwapchainBook b;
    const auto a = handle<VkSwapchainKHR>(0x100), c = handle<VkSwapchainKHR>(0x200);
    SwapchainRecord& ra = b.on_create(a, VK_FORMAT_B8G8R8A8_UNORM, VkExtent2D{1920, 1080}, 3, VK_NULL_HANDLE);
    CHECK(ra.extent.width == 1920);
    REQUIRE(b.find(a) != nullptr);
    CHECK_FALSE(b.find(a)->retired);
    b.on_create(c, VK_FORMAT_B8G8R8A8_SRGB, VkExtent2D{3840, 2160}, 3, a);  // resize: a is the oldSwapchain
    REQUIRE(b.find(a) != nullptr);
    CHECK(b.find(a)->retired);
    CHECK(b.size() == 2);
    auto gone = b.take(a);
    REQUIRE(gone.has_value());
    CHECK(gone->handle == a);
    CHECK(b.find(a) == nullptr);
    REQUIRE(b.find(c) != nullptr);
    CHECK(b.find(c)->extent.height == 2160);
    CHECK_FALSE(b.take(a).has_value());  // destroyed twice: nothing
    CHECK(b.take(c).has_value());
    CHECK(b.size() == 0);
}
