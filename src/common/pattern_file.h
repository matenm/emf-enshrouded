#pragma once
// A mod's pattern file (<mod>.patterns): the engine functions it finds by bytes, so emf_sigcheck --patterns can check
// them against a game exe without starting the game. One per line:
//   name = 48 8B 05 ?? ?? ?? ?? ; offset=3 ; rel32 ; tail=1 ; section=.rdata
// '#' starts a comment line. Numbers are decimal (a leading 0 is not octal) or 0x hex and must fit an int32; tabs work
// like spaces, a trailing ';' and a UTF-8 BOM are fine; a file without any pattern is an error. Pure parsing
// (unit-tested).
#include <string>
#include <string_view>
#include <vector>

#include "pattern_find.h"

namespace emf {

struct PatternEntry {
    std::string name;
    PatternQuery query;
    std::string section = ".text";
    int line = 0;
};

struct PatternFile {
    std::vector<PatternEntry> entries;
    std::vector<std::string> errors;  // "line 3: ..." for each line that was skipped
};

PatternFile parse_pattern_file(std::string_view text);

}  // namespace emf
