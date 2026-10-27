#include <doctest.h>
#include <windows.h>
#include "common/pe_image.h"
#include "common/seh.h"

TEST_CASE("section finds .text of the running test exe") {
    const void* base = GetModuleHandleW(nullptr);
    auto text = emf::section(base, ".text");
    REQUIRE(text.has_value());
    CHECK(text->size > 0);
    CHECK(text->start > reinterpret_cast<uintptr_t>(base));
    CHECK_FALSE(emf::section(base, ".nope").has_value());
}

TEST_CASE("sections lists .rdata too") {
    bool found = false;
    for (auto& s : emf::sections(GetModuleHandleW(nullptr))) found |= (s.name == ".rdata");
    CHECK(found);
}

TEST_CASE("seh_read survives a bad pointer") {
    int out = 0;
    CHECK_FALSE(emf::seh_read(nullptr, &out, sizeof(out)));
    int in = 1234;
    CHECK(emf::seh_read(&in, &out, sizeof(out)));
    CHECK(out == 1234);
}

TEST_CASE("pe image: map_image_file maps a fresh copy of the running exe, apart from the live module") {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const uintptr_t copy = emf::map_image_file(self);
    REQUIRE(copy != 0);
    const uintptr_t live = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    CHECK(copy != live);  // a separate copy: hooks in the live code do not show in it
    const auto live_text = emf::section(reinterpret_cast<const void*>(live), ".text");
    const auto copy_text = emf::section(reinterpret_cast<const void*>(copy), ".text");
    REQUIRE(live_text);
    REQUIRE(copy_text);
    CHECK(copy_text->start - copy == live_text->start - live);  // the same layout (RVAs)
    CHECK(copy_text->size == live_text->size);
    CHECK(emf::map_image_file(L"C:\\no\\such\\file.exe") == 0);
}
