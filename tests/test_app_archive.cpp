// EMF.exe's archive unpacking (app/archive.h): tar's listing read and checked, unsafe paths refused before and after
// unpacking, links and size limits, deleting an unpack folder without following links, SHA-256.
#include <doctest.h>

#include <algorithm>
#include <filesystem>

#include "app/archive.h"
#include "app/text.h"
#include "app_archive_files.h"

using namespace emf::app;
using app_test::TempGame;

TEST_CASE("app archive: tar's verbose listing gives each entry's type, size and path") {
    const std::string listing =
        "-rw-rw-r--  0 0      0          25 Oct 26 16:40 Wrapper/dinput8.dll\r\n"
        "-rw-rw-r--  0 0      0          24 Oct 26 16:40 Wrapper/My Mod.ini\r\n"
        "drwxrwxr-x  0 0      0           0 Oct 26 16:40 Wrapper/sub dir/\r\n"
        "lrwxrwxrwx  0 user   group       0 Jan  1  2024 link -> /etc/passwd\r\n"
        "hrw-r--r--  0 user   group       0 Jan  1  2024 hard link to Wrapper/dinput8.dll\r\n";
    const auto e = parse_tar_listing(listing);
    REQUIRE(e.size() == 5);
    CHECK(e[0].type == '-');
    CHECK(e[0].size == 25);
    CHECK(e[0].path == "Wrapper/dinput8.dll");
    CHECK(e[1].path == "Wrapper/My Mod.ini");
    CHECK(e[2].type == 'd');
    CHECK(e[2].path == "Wrapper/sub dir/");
    CHECK(e[3].type == 'l');
    CHECK(e[3].path == "link");
    CHECK(e[4].type == 'h');
    CHECK(e[4].path == "hard");
}

TEST_CASE("app archive: the listing check uses the classifier's path check (streams, devices, control characters)") {
    ArchiveLimits lim;
    for (const char* bad : {"../evil.txt", "a/../../b", "/abs.txt", "\\abs.txt", "C:/drive.txt", "c:x", "file.txt:stream",
                            "CON", "con.txt", "a/NUL", "aux.dll", "COM1", "lpt9.log", "...", "tab\there"})
        CHECK_MESSAGE(!check_listing({{'-', 1, bad}}, lim).empty(), bad);
    CHECK_FALSE(check_listing({{'-', 1, std::string("nul\0byte", 8)}}, lim).empty());
    for (const char* ok : {"dinput8.dll", "mods/foo/mod.json", "Wrapper/sub dir/readme.txt", "console.txt", "com10.txt",
                           "a.b.c", "\xC3\x9Cnicode.txt", ".hidden/x"})
        CHECK_MESSAGE(check_listing({{'-', 1, ok}}, lim).empty(), ok);
}

TEST_CASE("app archive: a listing with '..', absolute paths, links, devices or too much is refused") {
    ArchiveLimits lim;
    const auto file = [](const char* p, uint64_t size = 1) { return ListedEntry{'-', size, p}; };
    CHECK(check_listing({file("a.txt"), {'d', 0, "dir/"}}, lim).empty());
    CHECK(check_listing({file("../evil.txt")}, lim).find("../evil.txt") != std::string::npos);
    CHECK_FALSE(check_listing({file("/abs.txt")}, lim).empty());
    CHECK_FALSE(check_listing({file("C:/drive.txt")}, lim).empty());
    CHECK(check_listing({{'l', 0, "link"}}, lim).find("symbolic link") != std::string::npos);
    CHECK(check_listing({{'h', 0, "hard"}}, lim).find("hard link") != std::string::npos);
    CHECK_FALSE(check_listing({{'c', 0, "dev"}}, lim).empty());
    CHECK_FALSE(check_listing({{'?', 0, "garbage line"}}, lim).empty());
    lim.max_bytes = 100;
    CHECK(check_listing({file("a", 60), file("b", 60)}, lim).find("more than") != std::string::npos);
    lim = ArchiveLimits{};
    lim.max_entries = 2;
    CHECK_FALSE(check_listing({file("a"), file("b"), file("c")}, lim).empty());
}

TEST_CASE("app archive: Windows' tar.exe is found in System32") {
    const std::wstring tar = system_tar();
    REQUIRE_FALSE(tar.empty());
    CHECK(utf8(tar).find("tar.exe") != std::string::npos);
}

TEST_CASE("app archive: a zip unpacks into the folder and lists its files") {
    TempGame src(L"arc_src", false), out(L"arc_out", false), keep(L"arc_zip", false);
    src.write(L"Wrapper\\dinput8.dll", "dll bytes");
    src.write(L"Wrapper\\sub dir\\readme.txt", "hello");
    src.write(L"Wrapper\\\x00DCnicode.txt", "u");
    const std::wstring zip = keep.at(L"mod.zip");
    REQUIRE(app_test::make_zip(src.dir, zip));
    const Unpacked u = unpack_archive(system_tar(), zip, out.dir);
    REQUIRE_MESSAGE(u.ok, u.error);
    CHECK(u.files == std::vector<std::string>{"Wrapper/dinput8.dll", "Wrapper/sub dir/readme.txt", "Wrapper/\xC3\x9Cnicode.txt"});
    CHECK(u.bytes == 9 + 5 + 1);
    CHECK(out.read(L"Wrapper\\sub dir\\readme.txt") == "hello");
}

TEST_CASE("app archive: '..', links and archives over the limit are refused, nothing is written outside") {
    TempGame keep(L"arc_bad", false);
    const auto try_tar = [&](const std::vector<app_test::TarEntry>& entries, const ArchiveLimits& lim = {}) {
        static int n = 0;
        const std::wstring file = keep.at(L"bad" + std::to_wstring(++n) + L".tar");
        keep.write(file.substr(keep.dir.size() + 1), app_test::make_ustar(entries));
        TempGame out(L"arc_bad_out", false);
        return unpack_archive(system_tar(), file, out.dir, lim);
    };
    Unpacked u = try_tar({{"ok.txt", '0', "x"}, {"../evil.txt", '0', "evil"}});
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("../evil.txt") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(keep.dir).parent_path() / "evil.txt"));
    u = try_tar({{"link", '2', "", "C:\\Windows\\System32"}});
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("link") != std::string::npos);
    u = try_tar({{"a.txt", '0', "aaaa"}, {"b.txt", '1', "", "a.txt"}});
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("link") != std::string::npos);
    ArchiveLimits tiny;
    tiny.max_bytes = 10;
    u = try_tar({{"big.bin", '0', std::string(64, 'x')}}, tiny);
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("more than") != std::string::npos);
    // not an archive at all
    keep.write(L"junk.zip", "this is not a zip file");
    TempGame out(L"arc_junk_out", false);
    u = unpack_archive(system_tar(), keep.at(L"junk.zip"), out.dir);
    CHECK_FALSE(u.ok);
    CHECK_FALSE(u.error.empty());
    CHECK_FALSE(unpack_archive(L"", keep.at(L"junk.zip"), out.dir).ok);  // no tar.exe: said, not crashed
}

TEST_CASE("app archive: an archive with no size in its header is stopped while it unpacks once it passes the limit") {
    // tools/standin_tar plays tar for such an archive (a RAR5 can be one): a listing of 1 byte, then one file that grows
    // 1 MB every 20 ms up to 100 MB, kept open (Windows lists an open file's size as 0 until it is closed)
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring tar(exe);
    tar = tar.substr(0, tar.find_last_of(L"\\/")) + L"\\standin_tar\\tar.exe";
    REQUIRE(std::filesystem::exists(tar));
    TempGame keep(L"arc_nosize", false), out(L"arc_nosize_out", false);
    keep.write(L"bomb.rar", std::string("Rar!\x1A\x07\x01\x00", 8) + "no size in its header");
    const std::wstring log = keep.at(L"progress.txt");
    SetEnvironmentVariableW(L"EMF_STANDIN_TAR_LOG", log.c_str());
    ArchiveLimits lim;
    lim.max_bytes = 8ull << 20;  // 8 MB
    const Unpacked u = unpack_archive(tar, keep.at(L"bomb.rar"), out.dir, lim);
    SetEnvironmentVariableW(L"EMF_STANDIN_TAR_LOG", nullptr);
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("more than 8 MB") != std::string::npos);
    const int written_mb = std::atoi(keep.read(L"progress.txt").c_str());
    CHECK(written_mb >= 8);
    CHECK(written_mb < 50);  // ended soon after the limit, not when it was done (100 MB) or out of time
    CHECK_FALSE(std::filesystem::exists(out.dir + L"\\big.bin"));  // nothing half-unpacked stays
}

TEST_CASE("app archive: an unpack folder is deleted without following a junction inside it") {
    TempGame target(L"arc_target", false), folder(L"arc_folder", false);
    target.write(L"precious.txt", "keep me");
    folder.write(L"a\\b.txt", "x");
    REQUIRE(app_test::run_quiet(L"C:\\Windows\\System32\\cmd.exe",
                                L"/c mklink /J \"" + folder.at(L"jn") + L"\" \"" + target.dir + L"\"") == 0);
    std::vector<std::string> files;
    uint64_t bytes = 0;
    std::string why;
    CHECK_FALSE(list_tree(folder.dir, files, bytes, why));  // the walk refuses the link
    CHECK(why.find("link") != std::string::npos);
    CHECK(delete_tree(folder.dir));
    CHECK_FALSE(std::filesystem::exists(folder.dir));
    CHECK(target.read(L"precious.txt") == "keep me");
}

TEST_CASE("app archive: SHA-256 of a file; unpack folders are new and numbered") {
    TempGame g(L"arc_sha", false);
    g.write(L"abc.txt", "abc");
    CHECK(file_sha256(g.at(L"abc.txt")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(file_sha256(g.at(L"missing.txt")).empty());
    const std::wstring a = new_unpack_folder(), b = new_unpack_folder();
    REQUIRE_FALSE(a.empty());
    CHECK(a != b);
    CHECK(utf8(a).find("emf-install") != std::string::npos);
    CHECK(delete_tree(a));
    CHECK(delete_tree(b));
}


// ---- edge cases ----

TEST_CASE("app archive: a .zip whose bytes are not a zip is refused (a tar or mtree wearing a .zip name)") {
    TempGame keep(L"arc_magic", false);
    // a real tar, written byte for byte, named .zip
    keep.write(L"fake.zip", app_test::make_ustar({{"a.txt", '0', "hi"}}));
    TempGame out(L"arc_magic_out", false);
    Unpacked u = unpack_archive(system_tar(), keep.at(L"fake.zip"), out.dir);
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("does not look like a .zip") != std::string::npos);
    // an mtree spec named .zip (libarchive would read file contents from disk paths)
    keep.write(L"spec.zip", "#mtree\na.txt type=file\n");
    TempGame out2(L"arc_magic_out2", false);
    u = unpack_archive(system_tar(), keep.at(L"spec.zip"), out2.dir);
    CHECK_FALSE(u.ok);
    CHECK(u.error.find("does not look like a .zip") != std::string::npos);
    // a real zip is accepted
    TempGame src(L"arc_magic_src", false), zipkeep(L"arc_magic_zip", false);
    src.write(L"ok.txt", "x");
    const std::wstring zip = zipkeep.at(L"real.zip");
    REQUIRE(app_test::make_zip(src.dir, zip));
    TempGame out3(L"arc_magic_out3", false);
    CHECK(unpack_archive(system_tar(), zip, out3.dir).ok);
}

TEST_CASE("app archive: a listing with a trailing dot or space in a name is refused") {
    ArchiveLimits lim;
    CHECK_FALSE(check_listing({{'-', 1, "notes./a.txt"}}, lim).empty());
    CHECK_FALSE(check_listing({{'-', 1, "dinput8-chain.ini "}}, lim).empty());
    CHECK_FALSE(check_listing({{'-', 1, "mods/foo /mod.json"}}, lim).empty());
    CHECK(check_listing({{'-', 1, "mods/foo/a.b.txt"}}, lim).empty());  // a dot inside a name is fine
}

TEST_CASE("app archive: long paths and names with a trailing dot are deleted (no leak)") {
    TempGame keep(L"arc_long", false);
    // a path longer than MAX_PATH, built under a numbered unpack folder
    std::wstring deep = keep.dir + L"\\root";
    for (int i = 0; i < 12; ++i) deep += L"\\dddddddddddddddddddddddddddddddd";  // 12 * 33 > 260
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(L"\\\\?\\" + deep), ec);
    std::ofstream(std::filesystem::path(L"\\\\?\\" + deep + L"\\x.txt"), std::ios::binary) << "deep";
    CHECK(delete_tree(keep.dir + L"\\root"));
    CHECK_FALSE(exists(keep.dir + L"\\root"));
}
