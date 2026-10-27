#pragma once
// Starting the chained DLLs (chain_list.h) and reporting what happened: one log line per DLL and a small status file
// (emf\chain_status.txt) that EMF's manager reads. A DLL that is missing or fails is skipped; the game always goes on.
#include <windows.h>

#include <string>
#include <vector>

namespace emf::loader {

struct ChainResult {
    enum Kind { Loaded, Missing, Failed };
    std::wstring name;
    Kind kind = Loaded;
    unsigned long error = 0;  // Windows error when Failed
};

// `exists(name)`: the file is there; `load(name, &error)`: loads it, false (and the Windows error) on failure.
template <class Exists, class Load>
std::vector<ChainResult> load_chain(const std::vector<std::wstring>& names, Exists exists, Load load) {
    std::vector<ChainResult> out;
    for (const auto& name : names) {
        ChainResult r{name, ChainResult::Loaded, 0};
        if (!exists(name)) r.kind = ChainResult::Missing;
        else if (!load(name, &r.error)) r.kind = ChainResult::Failed;
        out.push_back(r);
    }
    return out;
}

inline std::string utf8_name(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string chain_line(const ChainResult& r) {
    const std::string n = utf8_name(r.name);
    switch (r.kind) {
        case ChainResult::Loaded:
            return "chain: started " + n;
        case ChainResult::Missing:
            return "chain: " + n + " is listed but not in the game folder; skipped";
        default:
            return "chain: " + n + " did not load (Windows error " + std::to_string(r.error) + "); skipped";
    }
}

inline std::string chain_status_text(const std::vector<ChainResult>& results) {
    std::string s;
    for (const auto& r : results) {
        s += utf8_name(r.name) + "\t";
        if (r.kind == ChainResult::Loaded) s += "started";
        else if (r.kind == ChainResult::Missing) s += "missing";
        else s += "failed " + std::to_string(r.error);
        s += "\r\n";
    }
    return s;
}

}  // namespace emf::loader
