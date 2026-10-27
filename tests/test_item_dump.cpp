#include <doctest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/sample_mod/item_dump.h"

namespace {
EmfItemInfo make(uint32_t id, const char* name, uint16_t max_stack) {
    EmfItemInfo i{};
    i.size = sizeof(i);
    i.item_id = id;
    for (int k = 0; k < 16; ++k) i.guid[k] = static_cast<uint8_t>(k);
    i.max_stack = max_stack;
    std::memcpy(i.name, name, std::min(std::strlen(name), sizeof(i.name) - 1));
    return i;
}
}  // namespace

TEST_CASE("item dump: one CSV line per item, sorted by name, with id, GUID text and stack size") {
    const std::vector<EmfItemInfo> items{make(0x22, "Torch", 1), make(0x11, "Wood_Log", 20), make(0x33, "", 5)};
    const std::string csv = sample::items_csv(items);
    CHECK(csv.rfind("name,item_id,guid,max_stack\n", 0) == 0);
    const auto torch = csv.find("Torch,0x00000022,03020100-0504-0706-0809-0a0b0c0d0e0f,1\n");
    const auto wood = csv.find("Wood_Log,0x00000011,");
    CHECK(torch != std::string::npos);
    CHECK(wood != std::string::npos);
    CHECK(torch < wood);
    CHECK(csv.find(",0x00000033,") != std::string::npos);  // nameless items are kept
}

TEST_CASE("item dump: names with commas or quotes are quoted") {
    const std::vector<EmfItemInfo> items{make(1, "a,b", 1), make(2, "say \"hi\"", 1)};
    const std::string csv = sample::items_csv(items);
    CHECK(csv.find("\"a,b\",0x00000001") != std::string::npos);
    CHECK(csv.find("\"say \"\"hi\"\"\",0x00000002") != std::string::npos);
}
