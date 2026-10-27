#include "pattern_file.h"

#include <cstdint>

#include "sigscan.h"

namespace emf {

namespace {
std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}
// A number as decimal ("10", "010" = ten, not octal) or 0x hex ("0x1F"), with an optional sign; it must fit an int32.
enum class Num { Ok, Bad, Range };
Num to_int(const std::string& s, int32_t& out) {
    size_t i = 0;
    const bool negative = !s.empty() && s[0] == '-';
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) ++i;
    int base = 10;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i == s.size()) return Num::Bad;
    constexpr int64_t kLimit = int64_t{1} << 31;  // |INT32_MIN|
    int64_t v = 0;
    bool range = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) return Num::Bad;
        if (!range) v = v * base + d;
        if (v > kLimit) range = true;  // keep reading: a bad character later still makes it "not a number"
    }
    if (range || v > (negative ? kLimit : kLimit - 1)) return Num::Range;
    out = static_cast<int32_t>(negative ? -v : v);
    return Num::Ok;
}
}  // namespace

PatternFile parse_pattern_file(std::string_view text) {
    PatternFile f;
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // a UTF-8 BOM (Notepad writes one)
    int number = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = text.find('\n', pos);
        std::string line(text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
        for (char& c : line)
            if (c == '\t') c = ' ';  // tabs separate like spaces (also between the bytes)
        line = trim(line);
        ++number;
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        if (line.empty() || line[0] == '#') continue;
        auto fail = [&](const std::string& why) { f.errors.push_back("line " + std::to_string(number) + ": " + why); };
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            fail("expected name = bytes");
            continue;
        }
        PatternEntry e;
        e.line = number;
        e.name = trim(std::string_view(line).substr(0, eq));
        if (e.name.empty()) {
            fail("the pattern has no name");
            continue;
        }
        // bytes ; option ; option
        std::vector<std::string> parts;
        std::string rest = line.substr(eq + 1);
        size_t start = 0;
        while (true) {
            const size_t semi = rest.find(';', start);
            parts.push_back(trim(std::string_view(rest).substr(start, semi == std::string::npos ? std::string::npos
                                                                                                 : semi - start)));
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
        e.query.bytes = parts[0];
        if (parse_pattern(e.query.bytes).bytes.empty()) {
            fail("\"" + e.query.bytes + "\" is not a pattern");
            continue;
        }
        bool ok = true;
        for (size_t i = 1; i < parts.size() && ok; ++i) {
            const std::string& o = parts[i];
            if (o.empty()) continue;  // a trailing ';' (or ";;")
            const size_t oeq = o.find('=');
            const std::string key = trim(std::string_view(o).substr(0, oeq));
            const std::string value = oeq == std::string::npos ? "" : trim(std::string_view(o).substr(oeq + 1));
            if (key == "rel32" && oeq == std::string::npos) {
                e.query.follow_rel32 = true;
            } else if (key == "offset" || key == "tail") {
                const Num n = to_int(value, key == "offset" ? e.query.offset : e.query.rel32_tail);
                ok = n == Num::Ok;
                if (n == Num::Bad) fail(key + " must be a decimal or 0x hex number");
                if (n == Num::Range) fail(key + " is out of range (-2147483648..2147483647)");
            } else if (key == "section" && !value.empty()) {
                e.section = value;
            } else {
                ok = false;
                fail("unknown option \"" + o + "\" (offset=, rel32, tail=, section=)");
            }
        }
        if (ok) f.entries.push_back(std::move(e));
    }
    if (f.entries.empty() && f.errors.empty())  // an empty file must not pass emf_sigcheck --patterns
        f.errors.push_back("no patterns in the file (one per line: name = bytes [; offset=N] [; rel32] [; tail=N] "
                           "[; section=.rdata])");
    return f;
}

}  // namespace emf
