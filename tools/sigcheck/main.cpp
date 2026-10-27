// Offline check: maps enshrouded.exe as an image (no code runs) and verifies every engine signature.
// The exe's sha256 picks the known build whose RVAs are checked; on an unknown build each signature only has to match
// exactly once (what the core accepts there), and the RVAs it found are printed for adding the build to the table.
// Exit: 0 = a known build and everything OK; 1 = something does not match; 2 = an unknown build (add it); 3 = the tool
// itself failed (usage, the file could not be read or mapped).
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "common/code_offsets.h"
#include "common/pattern_file.h"
#include "common/pe_image.h"
#include "common/sha256.h"
#include "common/signatures.h"

// emf_sigcheck --patterns <file> <exe>: checks a mod's pattern file (common/pattern_file.h) against an exe.
// Exit: 0 = every pattern found exactly once; 1 = a pattern failed or a line is bad; 3 = usage, file or exe unreadable.
int check_patterns(const wchar_t* file, const wchar_t* exe) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "could not read %ls\n", file);
        return 3;
    }
    std::stringstream text;
    text << in.rdbuf();
    const emf::PatternFile pf = emf::parse_pattern_file(text.str());
    HMODULE h = LoadLibraryExW(exe, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!h) {
        std::fprintf(stderr, "could not map %ls (error %lu)\n", exe, GetLastError());
        return 3;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(h) & ~uintptr_t(3));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const size_t image_size = nt->OptionalHeader.SizeOfImage;
    const auto secs = emf::sections(base);
    size_t ok = 0;
    for (const auto& e : pf.entries) {
        const emf::Section* sec = nullptr;
        for (const auto& s : secs)
            if (s.name == e.section) sec = &s;
        if (!sec) {
            std::printf("FAIL %-24s line %d: the exe has no section %s\n", e.name.c_str(), e.line, e.section.c_str());
            continue;
        }
        const auto r = emf::find_unique({reinterpret_cast<const uint8_t*>(sec->start), sec->size}, sec->start,
                                        reinterpret_cast<uintptr_t>(base), image_size, e.query);
        if (r.address) {
            ++ok;
            std::printf("OK   %-24s rva=0x%llx\n", e.name.c_str(),
                        static_cast<unsigned long long>(r.address - reinterpret_cast<uintptr_t>(base)));
        } else {
            std::printf("FAIL %-24s line %d: %s\n", e.name.c_str(), e.line, r.error.c_str());
        }
    }
    for (const auto& err : pf.errors) std::printf("BAD  %s\n", err.c_str());
    FreeLibrary(h);
    const bool all = ok == pf.entries.size() && pf.errors.empty();
    std::printf("%s: %zu of %zu patterns found once%s\n", all ? "all patterns OK" : "FAILED", ok, pf.entries.size(),
                pf.errors.empty() ? "" : " (and bad lines)");
    return all ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring(argv[1]) == L"--patterns") {
        if (argc < 4) {
            std::fprintf(stderr, "usage: emf_sigcheck --patterns <mod.patterns> <path to enshrouded.exe>\n");
            return 3;
        }
        return check_patterns(argv[2], argv[3]);
    }
    if (argc < 2) {
        std::fprintf(stderr, "usage: emf_sigcheck <path to enshrouded.exe>\n       emf_sigcheck --patterns <file> <exe>\n");
        return 3;
    }
    const std::string sha = emf::sha256_file(argv[1]);
    if (sha.empty()) {
        std::fprintf(stderr, "could not read %ls\n", argv[1]);
        return 3;
    }
    const int build = emf::find_known_build(sha);
    const bool known = build >= 0;
    const unsigned number = known ? emf::known_builds()[static_cast<size_t>(build)].number : 0;
    if (known)
        std::printf("sha256 %s: known build %u\n", sha.c_str(), number);
    else
        std::printf("sha256 %s: UNKNOWN build (signatures must match once; RVAs are not checked)\n", sha.c_str());

    HMODULE h = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!h) {
        std::fprintf(stderr, "could not map %ls (error %lu)\n", argv[1], GetLastError());
        return 3;
    }
    auto base = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(h) & ~uintptr_t(3));
    auto res = emf::resolve_all(base, build);
    auto defs = emf::all_signatures();
    size_t ok = 0;
    for (size_t i = 0; i < emf::kSigCount; ++i) {
        const auto& r = res[i];
        if (emf::absent_on(defs[i], build) && r.count == 0) {  // client-only code on the server exe: expected
            ++ok;
            std::printf("--   %-20s absent in this exe (as recorded)\n", defs[i].name);
            continue;
        }
        const bool good = emf::is_usable(r, known);
        ok += good;
        const unsigned long long rva = r.count ? (unsigned long long)(r.addr - reinterpret_cast<uintptr_t>(base)) : 0ull;
        if (known && defs[i].rva[static_cast<size_t>(build)] == emf::kRvaUnchecked)  // not recorded for this build
            std::printf("%-4s %-20s matches=%zu rva=0x%llx expected=unchecked\n", good ? "OK" : "FAIL", defs[i].name,
                        r.count, rva);
        else if (known)
            std::printf("%-4s %-20s matches=%zu rva=0x%llx expected=0x%x\n", good ? "OK" : "FAIL", defs[i].name, r.count,
                        rva, defs[i].rva[static_cast<size_t>(build)]);
        else
            std::printf("%-4s %-20s matches=%zu rva=0x%llx\n", good ? "OK" : "FAIL", defs[i].name, r.count, rva);
    }
    // Struct offsets read from instructions inside those functions (common/code_offsets.h).
    const auto offs = emf::resolve_code_offsets(base, res, build);
    const auto odefs = emf::all_code_offsets();
    size_t ook = 0;
    for (size_t i = 0; i < emf::kCodeOffsetCount; ++i) {
        // the scan itself must confirm the table (at runtime a known build would fall back to its record)
        const bool good = emf::offset_check_ok(offs, i, known);
        ook += good;
        const char* in = defs[static_cast<size_t>(odefs[i].function)].name;
        if (known)
            std::printf("%-4s %-20s matches=%zu offset=0x%x expected=0x%x (in %s)\n", good ? "OK" : "FAIL",
                        odefs[i].name, offs[i].count, offs[i].scanned, odefs[i].value[static_cast<size_t>(build)], in);
        else
            std::printf("%-4s %-20s matches=%zu offset=0x%x (in %s)\n", good ? "OK" : "FAIL", odefs[i].name,
                        offs[i].count, offs[i].scanned, in);
    }
    const bool all = ok == emf::kSigCount && ook == emf::kCodeOffsetCount;
    if (known)
        std::printf("%s: %zu of %zu signatures and %zu of %zu code offsets OK on build %u\n",
                    all ? "all signatures OK" : "FAILED", ok, emf::kSigCount, ook, emf::kCodeOffsetCount, number);
    else
        std::printf("%s: %zu of %zu signatures and %zu of %zu code offsets match once on an UNKNOWN build "
                    "(add it to known_builds)\n",
                    all ? "unknown build" : "FAILED", ok, emf::kSigCount, ook, emf::kCodeOffsetCount);
    FreeLibrary(h);
    if (!all) return 1;
    return known ? 0 : 2;  // an unknown build: add it before trusting the table
}
