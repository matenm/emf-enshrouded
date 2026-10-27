// EMF.exe's Nexus account pieces without a window: the key stored with DPAPI for the current Windows user, the nxm://
// handler setting against a fake registry (no real HKCU writes), the command line (--nxm) and the hand-off of a link to
// an EMF.exe that already runs.
#include <doctest.h>

#include <windows.h>
#include <dpapi.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/account_store.h"
#include "app/args.h"
#include "app/nxm_handler.h"
#include "app_test_files.h"

using namespace emf::app;
using app_test::TempGame;

namespace {
const std::wstring kCommandKey = L"Software\\Classes\\nxm\\shell\\open\\command";
const std::wstring kNxmKey = L"Software\\Classes\\nxm";
const std::wstring kExe = L"C:\\Games\\Enshrouded\\emf\\EMF.exe";
const std::wstring kVortex = L"\"C:\\Program Files\\Black Tree Gaming Ltd\\Vortex\\Vortex.exe\" -d \"%1\"";

// The arguments Windows gives EMF.exe for a command line (without the program itself).
std::vector<std::wstring> argv_of(const std::wstring& command) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command.c_str(), &argc);
    std::vector<std::wstring> out;
    for (int i = 1; argv && i < argc; ++i) out.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    return out;
}

void install_vortex(MemoryRegistry& reg) {
    reg.set(kNxmKey, L"", L"URL:NXM Protocol");
    reg.set(kNxmKey, L"URL Protocol", L"");
    reg.set(kNxmKey + L"\\DefaultIcon", L"", L"C:\\Program Files\\Black Tree Gaming Ltd\\Vortex\\Vortex.exe,0");
    reg.set(kCommandKey, L"", kVortex);
}
}  // namespace

// ---- the key, encrypted for this Windows user ----

TEST_CASE("app account: DPAPI turns a secret into bytes that hold none of it, and back") {
    const std::string secret = "aGVsbG8=--d29ybGQ=--Zm9vYmFy";
    const auto blob = dpapi_protect(secret);
    REQUIRE(blob);
    CHECK(blob->size() > secret.size());
    CHECK(blob->find(secret) == std::string::npos);
    const auto back = dpapi_unprotect(*blob);
    REQUIRE(back);
    CHECK(*back == secret);
    CHECK_FALSE(dpapi_unprotect("not a DPAPI blob"));
    std::string damaged = *blob;
    damaged[damaged.size() / 2] ^= 0x5A;
    CHECK_FALSE(dpapi_unprotect(damaged));
    // without EMF's entropy (another program of the same user asking DPAPI plainly) the blob does not open
    DATA_BLOB in{static_cast<DWORD>(blob->size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob->data()))};
    DATA_BLOB out{};
    CHECK_FALSE(CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out));
    if (out.pbData) LocalFree(out.pbData);
}

TEST_CASE("app account: a stale temporary key file goes, and a damaged key file says to paste the key again") {
    TempGame g(L"account_stale", false);
    const std::wstring file = g.at(L"EMF\\nexus.key");
    g.write(L"EMF\\nexus.key.tmp", "left from a crash");
    CHECK(read_account_file(file).status == AccountFile::Status::None);
    CHECK_FALSE(g.has(L"EMF\\nexus.key.tmp"));  // removed on load
    g.write(L"EMF\\nexus.key", "damaged");
    const AccountFile f = read_account_file(file);
    CHECK(f.status == AccountFile::Status::Unreadable);
    CHECK_FALSE(f.account);
    StoredAccount a;
    a.key = "aGVsbG8=--d29ybGQ=--Zm9vYmFy";
    a.name = "EmberTester";
    REQUIRE(save_account(file, a));
    CHECK(read_account_file(file).status == AccountFile::Status::Ok);
    g.write(L"EMF\\nexus.key.tmp", "another left-over");
    CHECK(delete_account(file));
    CHECK_FALSE(g.has(L"EMF\\nexus.key.tmp"));  // removed on log out too
}

TEST_CASE("app account: the stored account comes back as it was, and the file shows neither key nor name") {
    TempGame g(L"account_store", false);
    const std::wstring file = g.at(L"EMF\\nexus.key");
    CHECK_FALSE(load_account(file));  // nothing stored yet
    StoredAccount a;
    a.key = "aGVsbG8=--d29ybGQ=--Zm9vYmFy";
    a.user_id = 12345;
    a.name = "EmberTester";
    a.premium = true;
    a.supporter = false;
    a.checked_at = "2026-10-26 20:00";
    REQUIRE(save_account(file, a));
    const std::string bytes = g.read(L"EMF\\nexus.key");
    CHECK_FALSE(bytes.empty());
    CHECK(bytes.find("aGVsbG8") == std::string::npos);
    CHECK(bytes.find("EmberTester") == std::string::npos);
    const auto back = load_account(file);
    REQUIRE(back);
    CHECK(back->key == a.key);
    CHECK(back->user_id == 12345);
    CHECK(back->name == "EmberTester");
    CHECK(back->premium);
    CHECK_FALSE(back->supporter);
    CHECK(back->checked_at == "2026-10-26 20:00");

    g.write(L"EMF\\nexus.key", "damaged");
    CHECK_FALSE(load_account(file));
    REQUIRE(save_account(file, a));
    CHECK(delete_account(file));
    CHECK_FALSE(g.has(L"EMF\\nexus.key"));
    CHECK(delete_account(file));  // already gone is fine
}

// ---- the nxm:// handler setting ----

TEST_CASE("app nxm handler: who opens nxm links, read from the command") {
    CHECK(command_program(kVortex) == L"C:\\Program Files\\Black Tree Gaming Ltd\\Vortex\\Vortex.exe");
    CHECK(command_program(L"C:\\Program Files\\MO2\\nxmhandler.exe \"%1\"") == L"C:\\Program Files\\MO2\\nxmhandler.exe");
    CHECK(command_program(L"C:\\Tools\\handler.bat %1") == L"C:\\Tools\\handler.bat");
    CHECK(command_program(L"").empty());
    CHECK(nxm_command_for(kExe) == L"\"C:\\Games\\Enshrouded\\emf\\EMF.exe\" --nxm \"%1\"");

    MemoryRegistry reg;
    NxmOwner o = nxm_owner(reg, kExe);
    CHECK(o.command.empty());
    CHECK(o.label == "nothing");
    CHECK_FALSE(o.ours);

    install_vortex(reg);
    o = nxm_owner(reg, kExe);
    CHECK(o.label == "Vortex");
    CHECK(o.program == L"C:\\Program Files\\Black Tree Gaming Ltd\\Vortex\\Vortex.exe");
    CHECK_FALSE(o.ours);
    CHECK_FALSE(o.machine);

    reg.set(kCommandKey, L"", L"\"D:\\MO2\\nxmhandler.exe\" \"%1\"");
    CHECK(nxm_owner(reg, kExe).label == "Mod Organizer 2");
    reg.set(kCommandKey, L"", L"\"D:\\Other\\EMF.exe\" --nxm \"%1\"");
    o = nxm_owner(reg, kExe);
    CHECK(o.label == "another copy of EMF (D:\\Other\\EMF.exe)");
    CHECK(o.emf);
    CHECK_FALSE(o.ours);
    reg.set(kCommandKey, L"", L"\"c:\\games\\enshrouded\\EMF\\emf.exe\" --nxm \"%1\"");  // the same file, other case
    CHECK(nxm_owner(reg, kExe).ours);
}

TEST_CASE("app nxm handler: taking nxm links over from Vortex keeps Vortex's command, and giving them back restores it") {
    MemoryRegistry reg;
    install_vortex(reg);
    NxmChange c = nxm_take(reg, kExe);
    REQUIRE_MESSAGE(c.ok, c.error);
    CHECK(c.text.find("Vortex") != std::string::npos);
    CHECK(reg.get(RegistryLayer::Hive::User, kCommandKey, L"").value_or(L"") == nxm_command_for(kExe));
    CHECK(reg.get(RegistryLayer::Hive::User, kNxmKey, L"URL Protocol").has_value());
    CHECK(reg.notified > 0);
    NxmOwner o = nxm_owner(reg, kExe);
    CHECK(o.ours);
    CHECK(o.label == "EMF");
    CHECK(nxm_previous_label(reg) == "Vortex");

    c = nxm_take(reg, kExe);  // again: the backup still names Vortex
    REQUIRE(c.ok);
    CHECK(nxm_previous_label(reg) == "Vortex");

    c = nxm_give_back(reg, kExe);
    REQUIRE_MESSAGE(c.ok, c.error);
    CHECK(c.text.find("Vortex") != std::string::npos);
    CHECK(reg.get(RegistryLayer::Hive::User, kCommandKey, L"").value_or(L"") == kVortex);
    CHECK(reg.get(RegistryLayer::Hive::User, kNxmKey, L"").value_or(L"") == L"URL:NXM Protocol");
    CHECK(reg.get(RegistryLayer::Hive::User, kNxmKey + L"\\DefaultIcon", L"").has_value());  // Vortex's other values stay
    CHECK(nxm_owner(reg, kExe).label == "Vortex");
    CHECK(nxm_previous_label(reg).empty());  // the backup is gone
}

TEST_CASE("app nxm handler: when nothing had nxm links, giving them back removes EMF's key") {
    MemoryRegistry reg;
    NxmChange c = nxm_take(reg, kExe);
    REQUIRE(c.ok);
    CHECK(reg.get(RegistryLayer::Hive::User, kNxmKey, L"").value_or(L"") == L"URL:NXM Protocol");
    CHECK(nxm_previous_label(reg) == "nothing");
    c = nxm_give_back(reg, kExe);
    REQUIRE(c.ok);
    CHECK_FALSE(reg.exists(RegistryLayer::Hive::User, kNxmKey));
    CHECK(nxm_owner(reg, kExe).label == "nothing");
}

TEST_CASE("app nxm handler: an all-users handler (HKLM) is shown, overridden for this user, and shows again after") {
    MemoryRegistry reg;
    reg.set_machine(kCommandKey, L"", L"\"C:\\NMM\\NexusClient.exe\" \"%1\"");
    NxmOwner o = nxm_owner(reg, kExe);
    CHECK(o.machine);
    CHECK(o.label == "Nexus Mod Manager");
    REQUIRE(nxm_take(reg, kExe).ok);
    CHECK(nxm_owner(reg, kExe).ours);
    CHECK(reg.get(RegistryLayer::Hive::Machine, kCommandKey, L"").has_value());  // HKLM is never written
    REQUIRE(nxm_give_back(reg, kExe).ok);
    CHECK_FALSE(reg.exists(RegistryLayer::Hive::User, kNxmKey));
    CHECK(nxm_owner(reg, kExe).label == "Nexus Mod Manager");
}

TEST_CASE("app nxm handler: a program that took nxm links since is left alone") {
    MemoryRegistry reg;
    install_vortex(reg);
    REQUIRE(nxm_take(reg, kExe).ok);
    reg.set(kCommandKey, L"", L"\"D:\\MO2\\nxmhandler.exe\" \"%1\"");  // MO2 registered itself afterwards
    const NxmChange c = nxm_give_back(reg, kExe);
    CHECK(c.ok);
    CHECK(c.text.find("Mod Organizer 2") != std::string::npos);
    CHECK(reg.get(RegistryLayer::Hive::User, kCommandKey, L"").value_or(L"") == L"\"D:\\MO2\\nxmhandler.exe\" \"%1\"");
    CHECK(nxm_previous_label(reg).empty());

    MemoryRegistry plain;  // never taken: nothing to give back
    const NxmChange none = nxm_give_back(plain, kExe);
    CHECK_FALSE(none.ok);
}

TEST_CASE("app nxm handler: a failed registry write is reported, and EMF's own path must be known") {
    MemoryRegistry reg;
    install_vortex(reg);
    reg.fail_writes = true;
    NxmChange c = nxm_take(reg, kExe);
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.error.empty());
    reg.fail_writes = false;
    CHECK(nxm_owner(reg, kExe).label == "Vortex");
    CHECK_FALSE(nxm_take(reg, L"").ok);
}

// ---- the command line and the hand-off ----

TEST_CASE("app args: --nxm and a bare nxm:// link, next to the other switches") {
    AppArgs a = parse_app_args({L"--nxm", L"nxm://enshrouded/mods/170/files/1197?key=k&expires=1"});
    CHECK(a.nxm == L"nxm://enshrouded/mods/170/files/1197?key=k&expires=1");
    a = parse_app_args({L"NXM://enshrouded/mods/1/files/2"});
    CHECK(a.nxm == L"NXM://enshrouded/mods/1/files/2");
    a = parse_app_args({L"--game", L"C:\\G", L"--smoke", L"out.json", L"--capture", L"p.png", L"--select", L"emf:clock",
                        L"--page", L"browse", L"--eval", L"1+1", L"--devtools", L"--nexus-fixture", L"F", L"--probe-wait",
                        L"99999", L"--unknown", L"--nxm"});
    CHECK(a.game == L"C:\\G");
    CHECK(a.smoke == L"out.json");
    CHECK(a.capture == L"p.png");
    CHECK(a.select == L"emf:clock");
    CHECK(a.page == L"browse");
    CHECK(a.eval == L"1+1");
    CHECK(a.devtools);
    CHECK(a.nexus_fixture == L"F");
    CHECK(a.probe_wait == 10000);
    CHECK(a.nxm.empty());  // --nxm without a link
    CHECK_FALSE(a.release_nxm);
    CHECK(parse_app_args({L"--release-nxm"}).release_nxm);
    CHECK(parse_app_args({}).probe_wait == 1000);
}

TEST_CASE("app args: a hand-off carries one nxm link and nothing else") {
    const std::wstring url = L"nxm://enshrouded/mods/170/files/1197?key=k&expires=1";
    auto got = decode_handoff(kHandoffTag, url.data(), url.size() * sizeof(wchar_t));
    REQUIRE(got);
    CHECK(*got == url);
    const std::wstring with_nul = url + L'\0';
    got = decode_handoff(kHandoffTag, with_nul.data(), with_nul.size() * sizeof(wchar_t));  // a trailing NUL is fine
    REQUIRE(got);
    CHECK(*got == url);
    CHECK_FALSE(decode_handoff(kHandoffTag + 1, url.data(), url.size() * sizeof(wchar_t)));
    CHECK_FALSE(decode_handoff(kHandoffTag, url.data(), url.size() * sizeof(wchar_t) - 1));  // half a character
    CHECK_FALSE(decode_handoff(kHandoffTag, nullptr, 0));
    const std::wstring other = L"https://example.com";
    CHECK_FALSE(decode_handoff(kHandoffTag, other.data(), other.size() * sizeof(wchar_t)));
    const std::wstring inner(L"nxm://a\0b", 9);
    CHECK_FALSE(decode_handoff(kHandoffTag, inner.data(), inner.size() * sizeof(wchar_t)));
    const std::wstring huge = L"nxm://" + std::wstring(5000, L'a');
    CHECK_FALSE(decode_handoff(kHandoffTag, huge.data(), huge.size() * sizeof(wchar_t)));
}

TEST_CASE("app nxm handler: taking the links again after another program took them saves that program's setting") {
    MemoryRegistry reg;
    REQUIRE(nxm_take(reg, kExe).ok);  // nothing had them
    reg.set(kNxmKey, L"", L"URL:NXM Protocol");
    reg.set(kCommandKey, L"", L"\"D:\\MO2\\nxmhandler.exe\" \"%1\"");  // MO2 took them since
    REQUIRE(nxm_take(reg, kExe).ok);  // and the player gives them to EMF again
    CHECK(nxm_previous_label(reg) == "Mod Organizer 2");
    REQUIRE(nxm_give_back(reg, kExe).ok);
    CHECK(reg.get(RegistryLayer::Hive::User, kCommandKey, L"").value_or(L"") == L"\"D:\\MO2\\nxmhandler.exe\" \"%1\"");
}

TEST_CASE("app nxm handler: a command stored as REG_EXPAND_SZ comes back as one") {
    MemoryRegistry reg;
    const std::wstring cmd = L"\"%LOCALAPPDATA%\\Programs\\Vortex\\Vortex.exe\" -d \"%1\"";
    reg.set(kCommandKey, L"", cmd, true);
    CHECK(reg.is_expand(RegistryLayer::Hive::User, kCommandKey, L""));
    REQUIRE(nxm_take(reg, kExe).ok);
    CHECK_FALSE(reg.is_expand(RegistryLayer::Hive::User, kCommandKey, L""));  // EMF's own command is a plain string
    REQUIRE(nxm_give_back(reg, kExe).ok);
    CHECK(reg.get(RegistryLayer::Hive::User, kCommandKey, L"").value_or(L"") == cmd);
    CHECK(reg.is_expand(RegistryLayer::Hive::User, kCommandKey, L""));
}

TEST_CASE("app args: when Windows starts EMF.exe for a link, only the link counts") {
    // a link with a raw quote could smuggle switches into "EMF.exe" --nxm "%1"
    AppArgs a = parse_app_args({L"--nxm", L"nxm://enshrouded/mods/1/files/2?key=x", L"--smoke", L"C:\\file.txt", L"--game",
                                L"C:\\Other", L"--capture", L"C:\\p.png", L"--release-nxm", L"--devtools"});
    keep_only_link(a);
    CHECK(a.nxm == L"nxm://enshrouded/mods/1/files/2?key=x");
    CHECK(a.smoke.empty());
    CHECK(a.game.empty());
    CHECK(a.capture.empty());
    CHECK_FALSE(a.release_nxm);
    CHECK_FALSE(a.devtools);
    AppArgs b = parse_app_args({L"--game", L"C:\\G"});
    keep_only_link(b);  // no link: nothing changes
    CHECK(b.game == L"C:\\G");
}

TEST_CASE("app args: a link with raw quotes cannot bring switches along, whatever comes after it") {
    // what Windows runs for "EMF.exe" --nxm "%1" when %1 holds raw quotes
    const std::wstring link = L"nxm://a\" --release-nxm --smoke C:\\Users\\Public\\victim.txt --nxm \"";
    AppArgs a = parse_app_args(argv_of(L"\"C:\\G\\emf\\EMF.exe\" --nxm \"" + link + L"\""));
    CHECK(a.link_seen);
    finalize_args(a, false);
    CHECK(a.nxm == L"nxm://a");
    CHECK_FALSE(a.release_nxm);
    CHECK(a.smoke.empty());

    // a line ending in --nxm "": an empty link still counts as one, and is not taken as the link
    a = parse_app_args(argv_of(L"\"C:\\G\\emf\\EMF.exe\" --release-nxm --smoke C:\\x.txt --capture C:\\y.png --nxm \"\""));
    CHECK(a.link_seen);
    CHECK(a.nxm.empty());
    finalize_args(a, false);
    CHECK_FALSE(a.release_nxm);
    CHECK(a.smoke.empty());
    CHECK(a.capture.empty());

    a = parse_app_args({L"--nxm", L"nxm://enshrouded/mods/1/files/2", L"--nxm", L""});  // a later empty one is ignored
    CHECK(a.nxm == L"nxm://enshrouded/mods/1/files/2");
    a = parse_app_args({L"--release-nxm", L"NXM:junk"});  // any bare nxm: token counts
    CHECK(a.link_seen);
    finalize_args(a, false);
    CHECK_FALSE(a.release_nxm);
}

TEST_CASE("app args: --smoke, --capture and the other test switches need EMF_TEST=1") {
    const AppArgs parsed = parse_app_args({L"--smoke", L"out.json", L"--capture", L"p.png", L"--eval", L"1+1", L"--devtools",
                                           L"--nexus-fixture", L"F", L"--probe-wait", L"500", L"--game", L"C:\\G",
                                           L"--page", L"browse", L"--select", L"emf:clock"});
    AppArgs player = parsed;
    finalize_args(player, false);
    CHECK(player.smoke.empty());  // a shortcut cannot make EMF.exe overwrite a file
    CHECK(player.capture.empty());
    CHECK(player.eval.empty());
    CHECK_FALSE(player.devtools);
    CHECK(player.nexus_fixture.empty());
    CHECK(player.probe_wait == 1000);
    CHECK(player.game == L"C:\\G");  // showing a folder or a tab writes nothing
    CHECK(player.page == L"browse");
    CHECK(player.select == L"emf:clock");
    AppArgs test = parsed;
    finalize_args(test, true);
    CHECK(test.smoke == L"out.json");
    CHECK(test.capture == L"p.png");
    CHECK(test.eval == L"1+1");
    CHECK(test.devtools);
    CHECK(test.nexus_fixture == L"F");
    CHECK(test.probe_wait == 500);
    AppArgs both = parse_app_args({L"--smoke", L"out.json", L"--nxm", L"nxm://enshrouded/mods/1/files/2"});
    finalize_args(both, true);  // the smoke test combines a link with its own switches
    CHECK(both.smoke == L"out.json");
    CHECK(both.nxm == L"nxm://enshrouded/mods/1/files/2");
}
