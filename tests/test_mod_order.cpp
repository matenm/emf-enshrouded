#include <doctest.h>

#include <map>
#include <string>
#include <vector>

#include "core/mod_order.h"

using namespace emf::core;

namespace {
struct Dep {
    std::string id, range;
    bool optional = false;
};
ModCandidate mod(const char* id, const char* version, std::vector<Dep> deps = {}, uint32_t emf = 0,
                 const char* sort_name = nullptr) {
    ModCandidate c;
    c.id = id;
    c.version = *parse_version(version);
    for (auto& d : deps) c.depends.push_back(Dependency{d.id, *parse_constraint(d.range), d.optional});
    c.emf = emf;
    c.sort_name = sort_name ? sort_name : id;
    return c;
}
std::vector<std::string> ids(const std::vector<ModCandidate>& all, const OrderResult& r) {
    std::vector<std::string> out;
    for (size_t i : r.order) out.push_back(all[i].id);
    return out;
}
std::string why(const OrderResult& r, size_t index) {
    for (const auto& [i, reason] : r.rejected)
        if (i == index) return reason;
    return "";
}
std::string note(const OrderResult& r, size_t index) {
    for (const auto& [i, text] : r.notes)
        if (i == index) return text;
    return "";
}
}  // namespace

TEST_CASE("mod order: independent mods load in name order, ignoring case") {
    std::vector<ModCandidate> all{mod("zeta", "1"), mod("alpha", "1", {}, 0, "Alpha"), mod("beta", "1")};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"alpha", "beta", "zeta"});
    CHECK(r.rejected.empty());
}

TEST_CASE("mod order: a dependency loads before the mods that need it; chains") {
    std::vector<ModCandidate> all{mod("a_user", "1", {{"z_lib", ">=1.0"}}), mod("m_mid", "1", {{"z_lib", "*"}}),
                                  mod("z_lib", "1.2"), mod("b_top", "1", {{"a_user", "*"}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"z_lib", "a_user", "b_top", "m_mid"});
}

TEST_CASE("mod order: missing, too old and rejected dependencies reject the mod, with a reason") {
    std::vector<ModCandidate> all{mod("needs_missing", "1", {{"ghost", ">=1.2"}}),
                                  mod("needs_newer", "1", {{"lib", ">=1.2"}}), mod("lib", "1.1"),
                                  mod("needs_needs_missing", "1", {{"needs_missing", "*"}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"lib"});
    CHECK(why(r, 0) == "needs ghost >=1.2, which is not installed");
    CHECK(why(r, 1) == "needs lib >=1.2 (1.2.0 or newer), found 1.1.0: too old (update lib)");
    CHECK(why(r, 3) == "needs needs_missing, which was not loaded");
}

TEST_CASE("mod order: version ranges: too new, a gap between two ranges, and versions that fit") {
    std::vector<ModCandidate> all{mod("lib", "1.4.0"), mod("old_user", "1", {{"lib", "~1.2"}}),
                                  mod("gap_user", "1", {{"lib", "1.0 - 1.2 || ^2.0"}}),
                                  mod("caret_user", "1", {{"lib", "^1.2.0"}}), mod("exact_user", "1", {{"lib", "=1.4.0"}}),
                                  mod("both_user", "1", {{"lib", ">=1.0.0, <2.0.0"}}), mod("bare_user", "1", {{"lib", "1.3"}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"lib", "bare_user", "both_user", "caret_user", "exact_user"});
    CHECK(why(r, 1) == "needs lib ~1.2 (1.2.0 or newer, below 1.3.0), found 1.4.0: too new (look for an update of this mod)");
    CHECK(why(r, 2) == "needs lib 1.0 - 1.2 || ^2.0 (1.0.0 or newer, below 1.3.0; or 2.0.0 or newer, below 3.0.0), "
                       "found 1.4.0: not a version it works with");
}

TEST_CASE("mod order: an optional dependency that is missing does not stop the mod") {
    std::vector<ModCandidate> all{mod("a_mod", "1", {{"extra", "^2.0", true}}), mod("b_mod", "1")};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"a_mod", "b_mod"});
    CHECK(r.rejected.empty());
}

TEST_CASE("mod order: an installed optional dependency loads first; its version counts only once it runs") {
    std::vector<ModCandidate> all{mod("a_user", "1", {{"z_extra", "^1.0", true}}), mod("z_extra", "1.5"),
                                  mod("b_picky", "1", {{"z_extra", "^2.0", true}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"z_extra", "a_user", "b_picky"});
    CHECK(r.rejected.empty());  // whether z_extra runs is settled at load time (unmet_dependency)
    const auto fate = [](const std::string& id) -> std::optional<ModFate> {
        return id == "z_extra" ? std::optional<ModFate>(ModFate::Runs) : std::nullopt;
    };
    const auto version = [](const std::string& id) -> std::optional<Version> {
        return id == "z_extra" ? parse_version("1.5") : std::nullopt;
    };
    CHECK(unmet_dependency(all[2].depends, "", fate, version) ==
          "works with z_extra ^2.0 (2.0.0 or newer, below 3.0.0) when it runs, found 1.5.0: too old (update z_extra, "
          "or switch it off)");
    CHECK(unmet_dependency(all[0].depends, "", fate, version).empty());
    const auto off = [](const std::string& id) -> std::optional<ModFate> {
        return id == "z_extra" ? std::optional<ModFate>(ModFate::Off) : std::nullopt;
    };
    CHECK(unmet_dependency(all[2].depends, "", off, version).empty());  // switched off: no reason at all
}

TEST_CASE("mod order: an optional dependency that cannot load (rejected itself) does not stop the mod") {
    std::vector<ModCandidate> all{mod("a_user", "1", {{"z_extra", "*", true}}), mod("z_extra", "1", {{"ghost", "*"}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"a_user"});
    CHECK(why(r, 1) == "needs ghost, which is not installed");
    CHECK(why(r, 0).empty());
}

TEST_CASE("mod order: two mods that use each other optionally both load (the cycle is broken, with a note)") {
    std::vector<ModCandidate> all{mod("b_mod", "1", {{"a_mod", "*", true}}), mod("a_mod", "1", {{"b_mod", "*", true}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"a_mod", "b_mod"});  // by name: a_mod goes first
    CHECK(r.rejected.empty());
    CHECK(note(r, 1) == "loads before b_mod: they need each other, and a_mod needs b_mod only optionally");
}

TEST_CASE("mod order: a required one way and optional the other: the required order wins") {
    std::vector<ModCandidate> all{mod("a_mod", "1", {{"b_mod", "*"}}), mod("b_mod", "1", {{"a_mod", "*", true}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"b_mod", "a_mod"});
    CHECK(r.rejected.empty());
    CHECK(note(r, 1).find("loads before a_mod") == 0);
}

TEST_CASE("mod order: only an optional wait that closes a cycle is cut; a mod outside the cycle still waits") {
    // a uses b optionally; b needs c; c uses b optionally: the cycle is b <-> c, and a is not on it
    std::vector<ModCandidate> all{mod("a", "1", {{"b", "*", true}}), mod("b", "1", {{"c", "*"}}),
                                  mod("c", "1", {{"b", "*", true}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"c", "b", "a"});  // a's optional dependency b still loads first
    CHECK(r.rejected.empty());
    CHECK(note(r, 0).empty());
    CHECK(note(r, 2) == "loads before b: they need each other, and c needs b only optionally");
}

TEST_CASE("mod order: a mod waiting on its optional dependency and on another mod in the cycle keeps the other wait") {
    // x uses y optionally and needs z; y needs x; z is free: x waits for z, then the cycle x <-> y is cut at x -> y
    std::vector<ModCandidate> all{mod("x", "1", {{"y", "*", true}, {"z", "*"}}), mod("y", "1", {{"x", "*"}}),
                                  mod("z", "1")};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"z", "x", "y"});
    CHECK(note(r, 0) == "loads before y: they need each other, and x needs y only optionally");
}

TEST_CASE("mod order: a cycle rejects every mod in it") {
    std::vector<ModCandidate> all{mod("a", "1", {{"b", "*"}}), mod("b", "1", {{"a", "*"}}), mod("c", "1"),
                                  mod("d", "1", {{"a", "*"}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"c"});
    CHECK(why(r, 0).find("dependency cycle") != std::string::npos);
    CHECK(why(r, 1).find("dependency cycle") != std::string::npos);
    CHECK(why(r, 3) == "needs a, which was not loaded");
}

TEST_CASE("mod order: a mod that optionally uses a mod in a required cycle still loads, after the cycle is rejected") {
    std::vector<ModCandidate> all{mod("a", "1", {{"b", "*"}}), mod("b", "1", {{"a", "*"}}),
                                  mod("user", "1", {{"a", "*", true}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"user"});
    CHECK(why(r, 0).find("dependency cycle") == 0);
    CHECK(why(r, 2).empty());
}

TEST_CASE("mod order: a mod needing a newer EMF API is rejected; duplicate ids keep the first") {
    std::vector<ModCandidate> all{mod("future", "1", {}, 14), mod("dup", "1", {}, 0, "dup_a"),
                                  mod("dup", "2", {}, 0, "dup_b"), mod("uses_future", "1", {{"future", "*"}})};
    auto r = order_mods(all, 13);
    CHECK(ids(all, r) == std::vector<std::string>{"dup"});
    CHECK(r.order[0] == 1);
    CHECK(why(r, 0) == "needs EMF API 14, this EMF has 13");
    CHECK(why(r, 2) == "another mod already has the id \"dup\" (dup_a)");
    CHECK(why(r, 3) == "needs future, which was not loaded");
}

TEST_CASE("mod order: at run time a switched-off or failed dependency stops a mod, an optional one does not") {
    const std::vector<Dependency> deps{{"lua", Constraint{}, false}, {"extra", Constraint{}, true}, {"lib", Constraint{}, false}};
    std::map<std::string, ModFate> fates;
    const auto fate = [&](const std::string& id) -> std::optional<ModFate> {
        auto it = fates.find(id);
        return it == fates.end() ? std::nullopt : std::optional<ModFate>(it->second);
    };
    const auto no_version = [](const std::string&) -> std::optional<Version> { return std::nullopt; };
    CHECK(unmet_dependency(deps, "lua", fate, no_version).empty());  // nothing decided yet (or not installed)
    fates["extra"] = ModFate::Off;
    fates["lua"] = ModFate::Runs;
    fates["lib"] = ModFate::Runs;
    CHECK(unmet_dependency(deps, "lua", fate, no_version).empty());
    fates["lua"] = ModFate::Off;
    CHECK(unmet_dependency(deps, "lua", fate, no_version) == "needs the script host lua, which is switched off");
    fates["lua"] = ModFate::Runs;
    fates["lib"] = ModFate::NotLoaded;
    CHECK(unmet_dependency(deps, "lua", fate, no_version) == "needs lib, which is not loaded");
}

TEST_CASE("mod order: a script mod's host is added as a required dependency, also when listed as optional") {
    const auto opt = with_script_host({{"lua", *parse_constraint("^1.3"), true}}, "lua");
    REQUIRE(opt.size() == 1);
    CHECK_FALSE(opt[0].optional);
    CHECK(opt[0].version.text() == "^1.3");
    const auto added = with_script_host({}, "lua");
    REQUIRE(added.size() == 1);
    CHECK(added[0].id == "lua");
    CHECK(added[0].version.any());
    CHECK(with_script_host({}, "").empty());
    CHECK(dependency_text({"helper", *parse_constraint("^2.0"), true}) == "helper ^2.0 (optional)");
    CHECK(dependency_text({"lua", Constraint{}, false}) == "lua");
}
