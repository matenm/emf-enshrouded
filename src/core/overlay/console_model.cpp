#include "console_model.h"

#include "common/srw.h"

#include <algorithm>
#include <fstream>

namespace emf::core {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool contains(std::string_view s, std::string_view p) { return s.find(p) != std::string_view::npos; }

std::vector<std::string_view> words_of(std::string_view s) {
    std::vector<std::string_view> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') ++j;
        w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}

// Candidates for one word: prefix matches (sorted) first, then (if `substrings`) substring matches (sorted).
std::vector<std::string> match_word(std::string_view word, const std::vector<std::string>& pool, bool substrings) {
    const std::string w = lower(word);
    std::vector<std::string> prefix, inside;
    for (const auto& p : pool) {
        const std::string l = lower(p);
        if (starts_with(l, w))
            prefix.push_back(p);
        else if (substrings && !w.empty() && contains(l, w))
            inside.push_back(p);
    }
    std::sort(prefix.begin(), prefix.end());
    std::sort(inside.begin(), inside.end());
    prefix.insert(prefix.end(), inside.begin(), inside.end());
    return prefix;
}

}  // namespace

LineKind classify_reply(std::string_view line) {
    if (starts_with(line, "> ")) return LineKind::Command;
    if (starts_with(line, "usage:") || starts_with(line, "unknown ") || starts_with(line, "not run") ||
        starts_with(line, "could not") || contains(line, " refused") || contains(line, " failed") ||
        contains(line, "crashed") || (starts_with(line, "command '") && contains(line, "disabled")))
        return LineKind::Error;
    if (starts_with(line, "gave ") || starts_with(line, "spawned ") || starts_with(line, "raised ") ||
        contains(line, " -> ") || contains(line, "is now "))
        return LineKind::Success;
    return LineKind::Normal;
}

bool is_console_clear(std::string_view line) {
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
    while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
    const std::string l = lower(line);
    return l == "clear" || l == "cls";
}

void ConsoleModel::add_line(std::string text) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    add_line(std::move(text), static_cast<uint32_t>(t.wHour) * 3600u + t.wMinute * 60u + t.wSecond);
}

void ConsoleModel::add_line(std::string text, uint32_t seconds) {
    const LineKind kind = classify_reply(text);
    const SrwExclusive lock(lock_);
    lines_.push_back(ConsoleLine{kind, std::move(text), seconds % 86400u});
    while (lines_.size() > max_lines_) lines_.pop_front();
    ++version_;
}

void ConsoleModel::clear() {
    const SrwExclusive lock(lock_);
    lines_.clear();
    ++version_;
    cleared_version_ = version_;
}

uint64_t ConsoleModel::cleared_version() const {
    const SrwShared lock(lock_);
    return cleared_version_;
}

std::string ConsoleModel::stamp_text(uint32_t seconds) {
    seconds %= 86400u;
    char b[16];
    snprintf(b, sizeof(b), "%02u:%02u:%02u", seconds / 3600u, seconds / 60u % 60u, seconds % 60u);
    return b;
}

std::vector<ConsoleLine> ConsoleModel::lines(size_t first, size_t count) const {
    std::vector<ConsoleLine> out;
    const SrwShared lock(lock_);
    for (size_t i = first; i < lines_.size() && i < first + count; ++i) out.push_back(lines_[i]);
    return out;
}

std::string ConsoleModel::text(size_t first, size_t count, bool times) const {
    std::string out;
    const SrwShared lock(lock_);
    for (size_t i = first; i < lines_.size() && i < first + count; ++i) {
        if (i > first) out += "\r\n";
        if (times) out += stamp_text(lines_[i].seconds) + " ";
        out += lines_[i].text;
    }
    return out;
}

std::vector<size_t> ConsoleModel::find_all(std::string_view needle) const {
    std::vector<size_t> out;
    if (needle.empty()) return out;
    const std::string n = lower(needle);
    const auto low = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    const auto holds = [&](const std::string& text) {  // any case, without a copy of each line
        if (text.size() < n.size()) return false;
        for (size_t at = 0; at + n.size() <= text.size(); ++at) {
            size_t k = 0;
            while (k < n.size() && low(text[at + k]) == n[k]) ++k;
            if (k == n.size()) return true;
        }
        return false;
    };
    const SrwShared lock(lock_);
    for (size_t i = 0; i < lines_.size(); ++i)
        if (holds(lines_[i].text)) out.push_back(i);
    return out;
}

size_t ConsoleModel::size() const {
    const SrwShared lock(lock_);
    return lines_.size();
}

std::vector<std::pair<LineKind, std::string>> ConsoleModel::snapshot(size_t first, size_t count) const {
    std::vector<std::pair<LineKind, std::string>> out;
    const SrwShared lock(lock_);  // a bad_alloc while copying must not leave the lock held (add_line would block)
    for (size_t i = first; i < lines_.size() && i < first + count; ++i) out.emplace_back(lines_[i].kind, lines_[i].text);
    return out;
}

uint64_t ConsoleModel::version() const {
    const SrwShared lock(lock_);
    return version_;
}

void ConsoleModel::push_history(const std::string& line) {
    if (!line.empty() && (history_.empty() || history_.back() != line)) {
        history_.push_back(line);
        if (history_.size() > kMaxHistory) history_.erase(history_.begin(), history_.end() - kMaxHistory);
    }
    history_pos_ = history_.size();
}

std::string ConsoleModel::history_prev() {
    if (history_.empty()) return {};
    if (history_pos_ > 0) --history_pos_;
    return history_[history_pos_];
}

std::string ConsoleModel::history_next() {
    if (history_pos_ + 1 < history_.size()) return history_[++history_pos_];
    history_pos_ = history_.size();
    return {};
}

bool ConsoleModel::load_history(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        push_history(line);
    }
    return true;
}

bool ConsoleModel::save_history(const std::wstring& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& h : history_) f << h << "\r\n";
    return static_cast<bool>(f);
}

std::vector<std::string> complete(std::string_view input, const CompletionSources& src, size_t max) {
    std::vector<std::string> out;
    const auto words = words_of(input);
    if (words.empty()) return out;
    const bool trailing_space = input.back() == ' ';
    const std::string cmd = lower(words[0]);
    std::vector<std::string> picks;
    std::string head;  // the line up to the word being completed
    if (words.size() == 1 && !trailing_space) {
        picks = match_word(words[0], src.commands, false);
    } else {
        const size_t index = trailing_space ? words.size() : words.size() - 1;  // the word being typed
        const std::string_view partial = trailing_space ? std::string_view() : words.back();
        for (size_t i = 0; i < index; ++i) head += std::string(words[i]) + " ";
        if ((cmd == "give" || cmd == "put" || cmd == "take" || cmd == "stack") && index == 1)
            picks = match_word(partial, src.items, true);
        else if ((cmd == "tpl" || cmd == "tplset") && index == 2)
            picks = match_word(partial, src.components, true);
        else if ((cmd == "settings" || cmd == "set") && index == 1) {
            std::vector<std::string> ids;
            for (const auto& m : src.mod_settings) ids.push_back(m.id);
            picks = match_word(partial, ids, false);
        } else if (cmd == "set" && index == 2) {
            for (const auto& m : src.mod_settings)
                if (m.id == words[1]) {
                    std::vector<std::string> all;
                    for (const auto& s : m.settings) all.push_back(s.key);
                    all.push_back("enabled");
                    picks = match_word(partial, all, false);
                }
        }
    }
    for (const auto& p : picks) {
        std::string line = head + p;
        if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(std::move(line));
        if (out.size() >= max) break;
    }
    return out;
}

}  // namespace emf::core
