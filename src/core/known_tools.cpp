#include "core/known_tools.h"

#include <algorithm>
#include <cctype>

#include "loader/loader_id.h"

namespace emf::core {

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const KnownTool* find_tool(const std::vector<KnownTool>& tools, const std::string& word) {
    for (const auto& t : tools)
        if (t.word == word) return &t;
    return nullptr;
}

std::string display(const std::vector<KnownTool>& tools, const std::string& word) {
    if (word == "emf") return "EMF";
    if (word.empty()) return "unknown tool";
    const KnownTool* t = find_tool(tools, word);
    return t ? t->name : word;
}

std::vector<std::pair<std::string, std::string>> status_lines(const std::string& chain_status) {
    std::vector<std::pair<std::string, std::string>> out;  // name, how
    size_t from = 0;
    while (from < chain_status.size()) {
        size_t to = chain_status.find('\n', from);
        if (to == std::string::npos) to = chain_status.size();
        const std::string line = trim(chain_status.substr(from, to - from));
        from = to + 1;
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) out.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    return out;
}
}  // namespace

std::vector<KnownTool> parse_known_tools(const std::string& text) {
    std::vector<KnownTool> out;
    size_t from = 0;
    while (from < text.size()) {
        size_t to = text.find('\n', from);
        if (to == std::string::npos) to = text.size();
        const std::string line = trim(text.substr(from, to - from));
        from = to + 1;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> parts;
        size_t a = 0;
        while (true) {
            const size_t bar = line.find('|', a);
            parts.push_back(trim(line.substr(a, bar == std::string::npos ? std::string::npos : bar - a)));
            if (bar == std::string::npos) break;
            a = bar + 1;
        }
        if (parts.size() < 3 || parts[0].empty() || parts[1].empty()) continue;
        KnownTool t{lower(parts[0]), parts[1], {}, {}, {}, {}};
        for (size_t i = 2; i < parts.size(); ++i) {
            const std::string& p = parts[i];
            if (p.rfind("text:", 0) == 0 && p.size() > 5) t.texts.push_back(lower(p.substr(5)));
            else if (p.rfind("beside:", 0) == 0 && p.size() > 7) t.beside.push_back(p.substr(7));
            else if (p.rfind("same-loader:", 0) == 0 && p.size() > 12) t.same_loader.push_back(lower(p.substr(12)));
            else if (p.rfind("helper:", 0) == 0 && p.size() > 7) t.helpers.push_back(lower(p.substr(7)));
        }
        if (t.texts.empty() && t.beside.empty()) continue;
        out.push_back(std::move(t));
    }
    return out;
}

std::string tool_for_dll_text(const std::vector<KnownTool>& tools, const std::string& dll_text) {
    if (dll_text.find(EMF_LOADER_MARKER) != std::string::npos) return "emf";
    if (tools.empty()) return {};
    const std::string low = lower(dll_text);  // once per file: markers are stored in lower case
    for (const auto& t : tools)
        for (const auto& s : t.texts)
            if (low.find(s) != std::string::npos) return t.word;
    return {};
}

std::vector<std::string> started_chain_names(const std::string& chain_status) {
    std::vector<std::string> out;
    for (const auto& [name, how] : status_lines(chain_status))
        if (how == "started") out.push_back(name);
    return out;
}

std::vector<std::string> installed_report(const std::vector<KnownTool>& tools, const GameFolderView& view) {
    std::vector<std::string> lines;
    std::vector<std::string> active = view.chained_tools;  // tools with a DLL that runs
    const auto helper = [&](const KnownTool& t, const std::string& name) {
        return std::find(t.helpers.begin(), t.helpers.end(), lower(name)) != t.helpers.end();
    };
    std::string files;
    for (const auto& [name, word] : view.proxies) {
        const KnownTool* t = find_tool(tools, word);
        if (!t || !helper(*t, name)) active.push_back(word);  // a helper file does not start the tool alone
        files += (files.empty() ? "" : ", ") + name + " = " + display(tools, word);
    }
    if (!files.empty()) lines.push_back("next to the game: " + files);

    std::string started, skipped;
    for (const auto& [name, how] : status_lines(view.chain_status)) {
        if (how == "started") started += (started.empty() ? "" : ", ") + name;
        else skipped += (skipped.empty() ? "" : ", ") + name + " (" + how + ")";
    }
    if (!started.empty() || !skipped.empty()) {
        std::string s = "started by EMF (dinput8-chain.ini): " + (started.empty() ? std::string("none") : started);
        if (!skipped.empty()) s += "; listed but not started: " + skipped;
        lines.push_back(s);
    }

    for (const auto& t : tools) {
        if (t.texts.empty()) continue;  // its DLLs cannot be told apart: nothing to compare
        std::string here;
        for (const auto& b : t.beside)
            if (std::find(view.beside.begin(), view.beside.end(), b) != view.beside.end()) here = b;
        if (here.empty() || std::find(active.begin(), active.end(), t.word) != active.end()) continue;
        // which known tool holds a name its loader wants ("EML and ShroudForge both want dbghelp.dll")
        std::string why = "another tool has its file name";
        for (const auto& want : t.same_loader)
            for (const auto& [name, word] : view.proxies)
                if (lower(name) == want && !word.empty() && word != t.word)
                    why = display(tools, word) + " has " + name + ", which both want";
        std::string helper_note;
        for (const auto& [name, word] : view.proxies)
            if (word == t.word && helper(t, name)) helper_note += "; its " + name + " alone does not start it";
        lines.push_back(t.name + " is installed (" + here + ") but none of its DLLs is active: " + why + helper_note);
    }
    return lines;
}

const std::vector<std::string>& proxy_dll_names() {
    // umpdc.dll and cfgmgr32.dll: the Greeneye loader's other names
    static const std::vector<std::string> names = {"dinput8.dll", "dbghelp.dll",  "dbgcore.dll",
                                                   "winmm.dll",   "powrprof.dll", "version.dll",
                                                   "xinput1_4.dll", "dxgi.dll",   "umpdc.dll",
                                                   "cfgmgr32.dll"};
    return names;
}

}  // namespace emf::core
