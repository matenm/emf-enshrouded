#include "mod_order.h"

#include <algorithm>
#include <map>
#include <set>

namespace emf::core {

namespace {
std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string range_note(const Dependency& d) {
    return d.version.any() ? "" : " " + d.version.text() + " (" + d.version.describe() + ")";
}

// Why a mod is refused because its dependency `d` is there in version `found`, which does not fit (an optional one:
// only when it runs).
std::string mismatch(const Dependency& d, Version found, Fit f) {
    const std::string head = (d.optional ? "works with " : "needs ") + d.id + range_note(d) +
                             (d.optional ? " when it runs" : "") + ", found " + version_text(found) + ": ";
    switch (f) {
        case Fit::TooOld: return head + "too old (update " + d.id + (d.optional ? ", or switch it off)" : ")");
        case Fit::TooNew:
            return head + "too new (look for an update of this mod" + (d.optional ? ", or switch " + d.id + " off)" : ")");
        default:
            return head + "not a version it works with" + (d.optional ? " (switch " + d.id + " off, or use one it works with)" : "");
    }
}
}  // namespace

OrderResult order_mods(const std::vector<ModCandidate>& mods, uint32_t api_version) {
    OrderResult r;
    const size_t n = mods.size();
    std::vector<std::string> keys(n);
    for (size_t i = 0; i < n; ++i) keys[i] = lower(mods[i].sort_name) + '\x01' + std::to_string(i);
    std::vector<size_t> by_name(n);
    for (size_t i = 0; i < n; ++i) by_name[i] = i;
    std::sort(by_name.begin(), by_name.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });

    std::vector<std::string> why(n);      // non-empty = rejected
    std::map<std::string, size_t> owner;  // id -> the candidate that has it
    for (size_t i : by_name) {
        auto it = owner.find(mods[i].id);
        if (it != owner.end()) {
            why[i] = "another mod already has the id \"" + mods[i].id + "\" (" + mods[it->second].sort_name + ")";
            continue;
        }
        owner[mods[i].id] = i;
        if (mods[i].emf > api_version)
            why[i] = "needs EMF API " + std::to_string(mods[i].emf) + ", this EMF has " + std::to_string(api_version);
    }
    const auto find = [&](const std::string& id) -> std::optional<size_t> {
        auto it = owner.find(id);
        return it == owner.end() ? std::nullopt : std::optional<size_t>(it->second);
    };
    // Missing dependencies and versions that do not fit. An optional one counts only when it runs, which is settled at
    // load time (unmet_dependency): switched off, it does not matter what version it is.
    for (size_t i : by_name) {
        if (!why[i].empty()) continue;
        for (const Dependency& d : mods[i].depends) {
            const auto at = find(d.id);
            if (d.optional) continue;
            if (!at) {
                why[i] = "needs " + d.id + (d.version.any() ? "" : " " + d.version.text()) + ", which is not installed";
                break;
            }
            const Fit f = fit(mods[*at].version, d.version);
            if (f != Fit::Ok) {
                why[i] = mismatch(d, mods[*at].version, f);
                break;
            }
        }
    }
    // A rejected required dependency rejects its users (until nothing changes); a rejected optional one does not
    auto propagate = [&] {
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i : by_name) {
                if (!why[i].empty()) continue;
                for (const Dependency& d : mods[i].depends) {
                    const auto at = find(d.id);
                    if (d.optional || !at || why[*at].empty()) continue;
                    why[i] = "needs " + d.id + ", which was not loaded";
                    changed = true;
                    break;
                }
            }
        }
    };
    propagate();

    // Kahn's algorithm over the mods still in, the ready set ordered by name. Stuck on a cycle that an optional
    // dependency closes, that optional wait is dropped (the first such mod by name, its dependencies in order) until a
    // mod is ready; a cycle of required dependencies rejects its mods, and the order is worked out again without them.
    for (size_t round = 0; round <= n; ++round) {
        r.order.clear();
        r.notes.clear();
        std::vector<bool> in(n), placed(n, false);
        std::set<std::pair<size_t, size_t>> dropped;  // (mod, dependency): an optional wait cut to break a cycle
        std::map<size_t, std::string> cut;             // mod -> the optional dependencies it now loads before
        for (size_t i = 0; i < n; ++i) in[i] = why[i].empty();
        std::vector<size_t> waiting(n, 0);
        std::vector<std::vector<size_t>> users(n);
        for (size_t i = 0; i < n; ++i) {
            if (!in[i]) continue;
            for (const Dependency& d : mods[i].depends) {
                const auto at = find(d.id);
                if (!at || !in[*at]) continue;  // an optional one that is missing or not loaded
                ++waiting[i];
                users[*at].push_back(i);
            }
        }
        std::set<std::pair<std::string, size_t>> ready;
        for (size_t i = 0; i < n; ++i)
            if (in[i] && waiting[i] == 0) ready.insert({keys[i], i});
        size_t left = 0;
        for (size_t i = 0; i < n; ++i) left += in[i] ? 1 : 0;
        while (true) {
            while (!ready.empty()) {
                const size_t i = ready.begin()->second;
                ready.erase(ready.begin());
                r.order.push_back(i);
                placed[i] = true;
                --left;
                for (size_t u : users[i])
                    if (!dropped.count({u, i}) && --waiting[u] == 0) ready.insert({keys[u], u});
            }
            if (left == 0) break;
            // Does `from` lead back to `to` through mods not placed yet (along the waits still standing)?
            const auto leads_to = [&](size_t from, size_t to) {
                std::vector<bool> seen(n, false);
                std::vector<size_t> stack{from};
                while (!stack.empty()) {
                    const size_t x = stack.back();
                    stack.pop_back();
                    if (x == to) return true;
                    if (seen[x]) continue;
                    seen[x] = true;
                    for (const Dependency& d : mods[x].depends) {
                        const auto at = find(d.id);
                        if (at && in[*at] && !placed[*at] && !dropped.count({x, *at})) stack.push_back(*at);
                    }
                }
                return false;
            };
            bool cut_one = false;
            for (size_t i : by_name) {
                if (!in[i] || placed[i]) continue;
                for (const Dependency& d : mods[i].depends) {
                    const auto at = find(d.id);
                    if (!d.optional || !at || !in[*at] || placed[*at] || dropped.count({i, *at}) || !leads_to(*at, i)) continue;
                    dropped.insert({i, *at});  // i loads before d, which (through others) needs i
                    cut[i] += (cut[i].empty() ? "" : ", ") + d.id;
                    if (--waiting[i] == 0) ready.insert({keys[i], i});
                    cut_one = true;
                    break;
                }
                if (cut_one) break;
            }
            if (!cut_one) break;
        }
        for (const auto& [i, deps] : cut)
            r.notes.emplace_back(i, "loads before " + deps + ": they need each other, and " + mods[i].id + " needs " +
                                        deps + " only optionally");
        if (left == 0) break;

        // Whatever is left waits on a cycle of required dependencies, or needs a mod in one
        std::vector<bool> stuck(n, false);
        for (size_t i = 0; i < n; ++i) stuck[i] = in[i] && !placed[i];
        bool found_cycle = false;
        for (size_t start : by_name) {
            if (!stuck[start] || !why[start].empty()) continue;
            // walk required dependencies among the stuck mods until a mod repeats
            std::vector<size_t> path;
            std::map<size_t, size_t> at;
            size_t cur = start;
            bool cycle = false;
            while (true) {
                at[cur] = path.size();
                path.push_back(cur);
                bool next = false;
                for (const Dependency& d : mods[cur].depends) {
                    const auto dep = find(d.id);
                    if (d.optional || !dep || !stuck[*dep] || !why[*dep].empty()) continue;
                    cur = *dep;
                    next = true;
                    break;
                }
                if (!next) break;  // a dead end: it needs a mod already explained
                if (at.count(cur)) {
                    cycle = true;
                    break;
                }
            }
            if (!cycle) continue;
            std::string text = "dependency cycle: ";
            for (size_t k = at[cur]; k < path.size(); ++k) text += mods[path[k]].id + " -> ";
            text += mods[cur].id;
            for (size_t k = at[cur]; k < path.size(); ++k) why[path[k]] = text;
            found_cycle = true;
        }
        propagate();
        if (!found_cycle || round == n) {  // cannot happen; never loop without progress
            for (size_t i = 0; i < n; ++i)
                if (stuck[i] && why[i].empty()) why[i] = "a dependency could not be loaded";
        }
    }

    for (size_t i : by_name)
        if (!why[i].empty()) r.rejected.emplace_back(i, why[i]);
    return r;
}

std::vector<Dependency> with_script_host(std::vector<Dependency> deps, const std::string& host) {
    if (host.empty()) return deps;
    for (auto& d : deps)
        if (d.id == host) {
            d.optional = false;
            return deps;
        }
    deps.push_back(Dependency{host, Constraint{}, false});
    return deps;
}

std::string dependency_text(const Dependency& d) {
    return d.id + (d.version.any() ? "" : " " + d.version.text()) + (d.optional ? " (optional)" : "");
}

std::string unmet_dependency(const std::vector<Dependency>& deps, const std::string& host,
                             const std::function<std::optional<ModFate>(const std::string&)>& fate,
                             const std::function<std::optional<Version>(const std::string&)>& version_of) {
    for (const Dependency& d : deps) {
        const auto f = fate(d.id);
        if (d.optional) {  // never needed; when it runs, the mod works with it, so its version must fit
            if (!f || *f != ModFate::Runs) continue;
            const auto v = version_of(d.id);
            if (!v) continue;
            const Fit ft = fit(*v, d.version);
            if (ft != Fit::Ok) return mismatch(d, *v, ft);
            continue;
        }
        if (!f || *f == ModFate::Runs) continue;
        return "needs " + std::string(d.id == host ? "the script host " : "") + d.id + ", which is " +
               (*f == ModFate::Off ? "switched off" : "not loaded");
    }
    return "";
}

}  // namespace emf::core
