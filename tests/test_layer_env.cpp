// The loader enables EMF's Vulkan layer through ';'-separated environment lists without dropping what is there.
#include <doctest.h>

#include "loader/layer_env.h"

using emf::loader::list_with_first;

TEST_CASE("layer env: the item goes first in a list, the rest is kept") {
    CHECK(list_with_first(L"", L"VK_LAYER_EMF_console") == L"VK_LAYER_EMF_console");
    CHECK(list_with_first(L"VK_LAYER_A;VK_LAYER_B", L"VK_LAYER_EMF_console") ==
          L"VK_LAYER_EMF_console;VK_LAYER_A;VK_LAYER_B");
    CHECK(list_with_first(L"C:\\x", L"E:\\game\\emf\\vk") == L"E:\\game\\emf\\vk;C:\\x");
}

TEST_CASE("layer env: an item already in the list (any case) is not added twice") {
    CHECK(list_with_first(L"VK_LAYER_A;vk_layer_emf_console", L"VK_LAYER_EMF_console") ==
          L"VK_LAYER_A;vk_layer_emf_console");
    CHECK(list_with_first(L"VK_LAYER_EMF_console", L"VK_LAYER_EMF_console") == L"VK_LAYER_EMF_console");
    CHECK(list_with_first(L"VK_LAYER_EMF_console_old", L"VK_LAYER_EMF_console") ==
          L"VK_LAYER_EMF_console;VK_LAYER_EMF_console_old");  // whole entries only
}
