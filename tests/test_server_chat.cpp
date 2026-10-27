// Players' /commands on the server side: who may, what happens to the line, replies to the sender only.
#include <doctest.h>

#include <cstring>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "core/server_chat.h"

using namespace emf::core;

namespace {
bool known(std::string_view w) { return w == "give" || w == "help" || w == "pos"; }

// A fake chat system: 32 readers, a tail line with a mask, laid out as the game's (chat_layout).
struct FakeChat {
    std::vector<uint8_t> sys = std::vector<uint8_t>(0x200, 0);
    std::vector<uint8_t> node = std::vector<uint8_t>(0x80, 0);
    FakeChat(uint32_t text_len) {
        const auto node_addr = reinterpret_cast<uintptr_t>(node.data());
        std::memcpy(sys.data() + chat_layout::kTail, &node_addr, 8);
        std::memcpy(node.data() + chat_layout::kNodeLen, &text_len, 4);
    }
    void reader(int i, bool active, int32_t pending) {
        sys[chat_layout::kReaders + chat_layout::kReaderStride * i] = active ? 1 : 0;
        std::memcpy(sys.data() + chat_layout::kReaders + chat_layout::kReaderStride * i + chat_layout::kReaderPending,
                    &pending, 4);
    }
    int32_t pending(int i) const {
        int32_t v;
        std::memcpy(&v, sys.data() + chat_layout::kReaders + chat_layout::kReaderStride * i + chat_layout::kReaderPending, 4);
        return v;
    }
    uint32_t mask() const {
        uint32_t m;
        std::memcpy(&m, node.data() + chat_layout::kNodeMask, 4);
        return m;
    }
    void set_mask(uint32_t m) { std::memcpy(node.data() + chat_layout::kNodeMask, &m, 4); }
};
}  // namespace

TEST_CASE("server chat: the admin list reads hex account hashes; account_text prints them the same way") {
    const auto p = parse_chat_policy("0x00ab00cd00ef0011, 1122334455667788 ,junk,0x", false);
    REQUIRE(p.admins.size() == 2);
    CHECK(p.allows(0x00ab00cd00ef0011ull));
    CHECK(p.allows(0x1122334455667788ull));
    CHECK_FALSE(p.allows(0x99));
    CHECK(account_text(0x00ab00cd00ef0011ull) == "0x00ab00cd00ef0011");
    CHECK(parse_chat_policy(account_text(0xfedcba9876543210ull), false).allows(0xfedcba9876543210ull));
    CHECK(parse_chat_policy("", true).allows(0x5));  // everyone=1
    CHECK_FALSE(parse_chat_policy("", false).allows(0x5));
}

TEST_CASE("server chat: admin entries that are no account hash (a pasted Steam64 id) are handed back for a warning") {
    std::vector<std::string> skipped;
    const auto p = parse_chat_policy("0xaa, 76561198012345678 ,junk,, 0x", false, &skipped);
    CHECK(p.admins == std::vector<uint64_t>{0xaa});
    CHECK(skipped == std::vector<std::string>{"76561198012345678", "junk", "0x"});  // empty entries are no mistake
    const std::string w = skipped_admin_warning("76561198012345678");
    CHECK(w.find("[chat] admins: '76561198012345678' is not an account hash") == 0);
    CHECK(w.find("Steam64") != std::string::npos);
    CHECK(w.find("\"chat command refused from account 0x...\"") != std::string::npos);  // where the right value is
    CHECK(parse_chat_policy("0xaa", false).admins.size() == 1);  // the list is optional
}

TEST_CASE("server chat: an admin's /command runs, someone else's is refused, chat and unknown /words stay chat") {
    const auto p = parse_chat_policy("0xaa", false);
    std::string line;
    CHECK(judge_chat("/give Wood 5", 0xaa, p, known, &line) == ChatAction::Run);
    CHECK(line == "give Wood 5");
    CHECK(judge_chat("  /POS  ", 0xaa, p, known, &line) == ChatAction::Run);
    CHECK(judge_chat("/give Wood 5", 0xbb, p, known, &line) == ChatAction::Refuse);
    CHECK(judge_chat("hello all", 0xbb, p, known, &line) == ChatAction::Pass);
    CHECK(judge_chat("/dance", 0xaa, p, known, &line) == ChatAction::Pass);  // not an EMF command
    CHECK(judge_chat("/", 0xaa, p, known, &line) == ChatAction::Pass);
    CHECK(judge_chat("", 0xaa, p, known, &line) == ChatAction::Pass);
}

TEST_CASE("server chat: the outbox keeps replies in order across threads and empties on take") {
    ChatOutbox box;
    std::thread t([&] {
        for (int i = 0; i < 100; ++i) box.add(7, 0x77, "a" + std::to_string(i));
    });
    t.join();
    box.add(9, 0x99, "b");
    const auto items = box.take();
    REQUIRE(items.size() == 101);
    CHECK(items[0].handle == 7u);
    CHECK(items[0].account == 0x77u);
    CHECK(items[0].text == "a0");
    CHECK(items[99].text == "a99");
    CHECK(items[100].handle == 9u);
    CHECK(items[100].account == 0x99u);
    CHECK(box.take().empty());
}

TEST_CASE("server chat: a reply line reaches only its reader; the others are marked done and their counts lowered") {
    FakeChat c(10);  // size = align4(10 + 0x17) = 0x24
    c.reader(0, true, 0x24);
    c.reader(1, true, 0x24 + 8);
    c.reader(2, false, 0);   // no player there: the game already set its bit
    c.reader(5, true, 0x24);
    c.set_mask(1u << 2);
    REQUIRE(restrict_line(c.sys.data(), 5));
    CHECK(c.mask() == ((1u << 0) | (1u << 1) | (1u << 2)));  // reader 5 still wants it
    CHECK(c.pending(0) == 0);
    CHECK(c.pending(1) == 8);
    CHECK(c.pending(2) == 0);
    CHECK(c.pending(5) == 0x24);
}

TEST_CASE("server chat: restrict_line changes nothing for a reader the mask cannot name, or a bit already set") {
    FakeChat c(4);
    c.reader(0, true, 0x1c);
    CHECK_FALSE(restrict_line(c.sys.data(), 40));  // the line goes to everyone
    CHECK(c.mask() == 0u);
    CHECK(c.pending(0) == 0x1c);
    c.set_mask(1u << 0);  // reader 0 already done with it
    REQUIRE(restrict_line(c.sys.data(), 3));
    CHECK(c.pending(0) == 0x1c);  // not lowered twice
    CHECK_FALSE(restrict_line(nullptr, 3));
}

TEST_CASE("server chat: the sender's entity is the one player info entry that holds the account hash") {
    constexpr size_t kEntry = kPlayerInfoSize;
    std::vector<uint8_t> infos(kEntry * 6, 0);
    auto entry = [&](uint32_t index, uint32_t id, uint64_t account, size_t at) {
        std::memcpy(infos.data() + index * kEntry, &id, 4);
        std::memcpy(infos.data() + index * kEntry + at, &account, 8);
    };
    entry(0, 1, 0x1111222233334444ull, 0x58);
    entry(2, 3, 0xaaaabbbbccccddddull, 0x58);
    entry(4, 99, 0xeeeeffff00001111ull, 0x58);  // a stale entry: its id is not index + 1
    CHECK(entity_by_account(infos.data(), 6, 0xaaaabbbbccccddddull) == 3u);
    CHECK(entity_by_account(infos.data(), 6, 0x1111222233334444ull) == 1u);
    CHECK(entity_by_account(infos.data(), 6, 0xeeeeffff00001111ull) == 0u);
    CHECK(entity_by_account(infos.data(), 6, 0x5u) == 0u);
    CHECK(entity_by_account(infos.data(), 6, 0) == 0u);  // never match "no account"
    entry(5, 6, 0xaaaabbbbccccddddull, 0x60);  // two entries with the same value: ambiguous
    CHECK(entity_by_account(infos.data(), 6, 0xaaaabbbbccccddddull) == 0u);
}

TEST_CASE("server chat: an old entry of the same account (a reconnect) is skipped when its entity is gone") {
    constexpr size_t kEntry = kPlayerInfoSize;
    std::vector<uint8_t> infos(kEntry * 8, 0);
    auto entry = [&](uint32_t index, uint64_t account) {
        const uint32_t id = index + 1;
        std::memcpy(infos.data() + index * kEntry, &id, 4);
        std::memcpy(infos.data() + index * kEntry + 0x58, &account, 8);
    };
    entry(1, 0x77ull);  // entity 2: the player before reconnecting (gone)
    entry(6, 0x77ull);  // entity 7: the same player now
    auto alive = [](uint32_t e) { return e == 7; };
    CHECK(entity_by_account(infos.data(), 8, 0x77ull) == 0u);  // without knowing who is alive: ambiguous
    CHECK(entity_by_account(infos.data(), 8, 0x77ull, alive) == 7u);
    CHECK(entity_by_account(infos.data(), 8, 0x77ull, [](uint32_t) { return false; }) == 0u);
}

TEST_CASE("server chat: printable() keeps typed text but not control characters (log and console lines)") {
    CHECK(printable("give Wood 5") == "give Wood 5");
    CHECK(printable("a\r\nfake line\x1b[2J") == "a??fake line?[2J");
    CHECK(printable(std::string("x\0y", 3)) == "x?y");
    CHECK(printable("Hjärta") == "Hjärta");  // UTF-8 bytes stay
}

TEST_CASE("server chat: a command's replies to chat stop after 10 lines with one note") {
    ReplyLimiter lim(10);
    int sent = 0;
    for (int i = 0; i < 15; ++i) {
        auto out = lim.next("line " + std::to_string(i));
        if (!out) continue;
        ++sent;
        if (sent == 11) CHECK(out->find("more in the server log") != std::string::npos);
    }
    CHECK(sent == 11);  // 10 lines and the note
}

TEST_CASE("server chat: newest_line_is checks the line just added is the reply (text and length)") {
    FakeChat c(9);
    std::memcpy(c.node.data() + 0x40, "[EMF] hey", 9);
    CHECK(newest_line_is(c.sys.data(), "[EMF] hey"));
    CHECK_FALSE(newest_line_is(c.sys.data(), "[EMF] hex"));
    CHECK_FALSE(newest_line_is(c.sys.data(), "[EMF] hey!"));
    CHECK_FALSE(newest_line_is(nullptr, "x"));
}

TEST_CASE("server chat: describe_chat_system names the newest line and every reader with something to send") {
    FakeChat c(9);
    const uint32_t handle = 0x40;
    const uint64_t account = 0x1111222233334444ull;
    std::memcpy(c.node.data() + chat_layout::kNodeHandle, &handle, 4);
    std::memcpy(c.node.data() + chat_layout::kNodeAccount, &account, 8);
    c.set_mask(0x5);
    c.reader(1, true, 0x24);
    c.reader(3, false, 8);
    const std::string d = describe_chat_system(c.sys.data());
    CHECK(d.find("newest: len 9 handle 0x40 account 0x1111222233334444 mask 0x5") != std::string::npos);
    CHECK(d.find("r1 on 36") != std::string::npos);
    CHECK(d.find("r3 off 8") != std::string::npos);
    CHECK(d.find("r0 ") == std::string::npos);  // nothing there
    CHECK(describe_chat_system(nullptr) == "no chat system");
}

TEST_CASE("server chat: a sender's chat reader is their connection's peer handle & 0x7f (seen live: 0x40 -> 0x81 -> 1)") {
    std::vector<uint8_t> rec(server_layout::kConnectionSize, 0);
    const uint32_t handle = 0x40, peer = 0x81;
    const uint64_t account = 0x1111222233334444ull;
    std::memcpy(rec.data(), &handle, 4);
    std::memcpy(rec.data() + server_layout::kConnectionPeer, &peer, 4);
    std::memcpy(rec.data() + server_layout::kConnectionAccount, &account, 8);
    CHECK(reader_from_connection(rec.data(), 0x40, account) == std::optional<uint32_t>(1u));
    CHECK_FALSE(reader_from_connection(rec.data(), 0x80, account).has_value());  // the record is another connection's
    CHECK_FALSE(reader_from_connection(rec.data(), 0x40, 0x1234).has_value());  // another player's account
    const uint32_t far_peer = 0xA5;  // reader 37: the mask cannot name it
    std::memcpy(rec.data() + server_layout::kConnectionPeer, &far_peer, 4);
    CHECK_FALSE(reader_from_connection(rec.data(), 0x40, account).has_value());
    const uint32_t none = 0;
    std::memcpy(rec.data() + server_layout::kConnectionPeer, &none, 4);
    CHECK_FALSE(reader_from_connection(rec.data(), 0x40, account).has_value());  // no peer
    CHECK_FALSE(reader_from_connection(nullptr, 0x40, account).has_value());
    CHECK(connection_record_offset(0x40) == std::optional<size_t>(0x1E0));
    CHECK(connection_record_offset(0x45) == std::optional<size_t>(0x1E0 + 5 * 0x148));
    CHECK_FALSE(connection_record_offset(0x50).has_value());  // index 16: there are 16 records
}

TEST_CASE("server chat: a connection record whose handle field is 0 (cleared) names no reader, even for handle 0") {
    std::vector<uint8_t> rec(server_layout::kConnectionSize, 0);
    const uint32_t peer = 0x85;  // the player left: the handle is cleared, the old peer and account stay behind
    const uint64_t account = 0x1111222233334444ull;
    std::memcpy(rec.data() + server_layout::kConnectionPeer, &peer, 4);
    std::memcpy(rec.data() + server_layout::kConnectionAccount, &account, 8);
    CHECK_FALSE(reader_from_connection(rec.data(), 0x40, account).has_value());
    CHECK_FALSE(reader_from_connection(rec.data(), 0, account).has_value());  // 0 is no connection
}

TEST_CASE("server chat: a player who is no admin gets 5 commands per 10 s; the 6th is refused once, the rest dropped") {
    ChatRateLimit limit(5, 10000);
    for (int i = 0; i < 5; ++i) CHECK(limit.check(0xbb, 1000 + i) == ChatRateLimit::Verdict::Ok);
    CHECK(limit.check(0xbb, 1100) == ChatRateLimit::Verdict::Warn);
    CHECK(limit.check(0xbb, 1200) == ChatRateLimit::Verdict::Drop);
    CHECK(limit.check(0xbb, 5000) == ChatRateLimit::Verdict::Drop);
    CHECK(limit.check(0xcc, 1200) == ChatRateLimit::Verdict::Ok);  // every account counts alone
    CHECK(limit.check(0xbb, 11001) == ChatRateLimit::Verdict::Ok);  // a new window
}

// ---- API v19: lines from mods, players' lines for on_chat, connections and player infos ----

TEST_CASE("chat_lines: '\\n' splits, CR and empty lines go, control characters become '?', 10 lines then '...'") {
    CHECK(chat_lines("hello") == std::vector<std::string>{"hello"});
    CHECK(chat_lines("a\r\nb\n\nc\n") == std::vector<std::string>{"a", "b", "c"});
    CHECK(chat_lines("tab\there\x1b[2J") == std::vector<std::string>{"tab?here?[2J"});
    CHECK(chat_lines("").empty());
    CHECK(chat_lines("\n\n").empty());
    std::string many;
    for (int i = 0; i < 13; ++i) many += std::to_string(i) + "\n";
    size_t dropped = 99;
    const auto l = chat_lines(many, 10, &dropped);
    REQUIRE(l.size() == 11);
    CHECK(l[9] == "9");
    CHECK(l[10] == "...");
    CHECK(dropped == 2);  // "11" and "12"; "10" became the "..."
    chat_lines("x", 10, &dropped);
    CHECK(dropped == 0);
}

TEST_CASE("chat_lines: a line is cut at the game's 1024 bytes, never inside a UTF-8 character") {
    const std::string ascii(1500, 'a');
    CHECK(chat_lines(ascii)[0].size() == 1024);
    std::string utf8(1023, 'b');
    utf8 += "\xC3\xA9tail";  // an e-acute across the 1024th byte
    const auto l = chat_lines(utf8);
    CHECK(l[0].size() == 1023);  // the two-byte character is not split
    CHECK(l[0].back() == 'b');
}

TEST_CASE("ChatLineBudget: 30 lines per mod per 10 s; a call that does not fit counts nothing; mods count alone") {
    ChatLineBudget b(30, 10000);
    CHECK(b.take("herald", 10, 1000));
    CHECK(b.take("herald", 20, 2000));
    CHECK_FALSE(b.take("herald", 1, 3000));
    CHECK(b.take("lua", 30, 3000));
    CHECK_FALSE(b.take("herald", 1, 10999));
    CHECK(b.take("herald", 30, 11000));  // a new window from the first line's time
}

TEST_CASE("ChatInbox: lines in order, at most 256 waiting, the rest counted as dropped") {
    ChatInbox in;
    for (int i = 0; i < 300; ++i) in.push({0xA1, 0x40, std::to_string(i)});
    CHECK(in.dropped() == 44);
    const auto lines = in.take();
    REQUIRE(lines.size() == kMaxChatInbox);
    CHECK(lines.front().text == "0");
    CHECK(lines.back().text == "255");
    CHECK(in.take().empty());
    CHECK(in.push({0xA1, 0x40, "again"}));
}

TEST_CASE("ChatOutbox: replies, mods' raw lines and lines for everyone; at most 1024 wait") {
    ChatOutbox box;
    CHECK(box.add(0x40, 0xA1, "reply"));
    CHECK(box.add(0x41, 0xA2, "raw", true));
    CHECK(box.add_everyone("all"));
    auto items = box.take();
    REQUIRE(items.size() == 3);
    CHECK_FALSE(items[0].raw);
    CHECK(items[1].raw);
    CHECK(items[2].everyone);
    CHECK(items[2].raw);
    for (size_t i = 0; i < kMaxChatOutbox; ++i) REQUIRE(box.add(0x40, 0xA1, "x"));
    CHECK_FALSE(box.add(0x40, 0xA1, "full"));
    CHECK_FALSE(box.add_everyone("full"));
    CHECK(box.size() == kMaxChatOutbox);
}

namespace {
// The server's 16 connection records with the given (handle, peer, account) in their own slots.
std::vector<uint8_t> records(std::initializer_list<std::tuple<uint32_t, uint32_t, uint64_t>> conns) {
    std::vector<uint8_t> r(static_cast<size_t>(server_layout::kConnectionSize) * server_layout::kConnectionCount, 0);
    for (const auto& [handle, peer, account] : conns) {
        uint8_t* rec = r.data() + (handle & 0x3F) * server_layout::kConnectionSize;
        std::memcpy(rec, &handle, 4);
        std::memcpy(rec + server_layout::kConnectionPeer, &peer, 4);
        std::memcpy(rec + server_layout::kConnectionAccount, &account, 8);
    }
    return r;
}
}  // namespace

TEST_CASE("connected_players: every record with a handle in its own slot, an account and a reader; connection_of by account") {
    auto r = records({{0x40, 0x81, 0xA1}, {0x83, 0x85, 0xA2}, {0x45, 0x00, 0xA3}, {0x46, 0x86, 0}});
    // a stale record: a handle that names another slot (copied there) is no connection
    const uint32_t stray = 0x47;
    std::memcpy(r.data() + 9 * server_layout::kConnectionSize, &stray, 4);
    const uint64_t acct = 0xA9;
    std::memcpy(r.data() + 9 * server_layout::kConnectionSize + server_layout::kConnectionAccount, &acct, 8);
    const auto all = connected_players(r.data());
    REQUIRE(all.size() == 2);
    CHECK(all[0].handle == 0x40);
    CHECK(all[0].account == 0xA1);
    CHECK(all[0].reader == 1u);
    CHECK(all[1].handle == 0x83);  // generation bits above the index are kept
    CHECK(all[1].reader == 5u);
    CHECK(connection_of(r.data(), 0xA2)->handle == 0x83);
    CHECK_FALSE(connection_of(r.data(), 0xA3).has_value());  // no peer
    CHECK_FALSE(connection_of(r.data(), 0).has_value());
    CHECK(connected_players(nullptr).empty());
}

TEST_CASE("read_player_info: entity at +0, connection handle at +4 (seen live: 0x40), account at +8") {
    std::vector<uint8_t> e(kPlayerInfoSize, 0);
    const uint32_t id = 1, handle = 0x40;
    const uint64_t account = 0xa1b2c3d4e5f60718ull;  // an account hash (a made-up one)
    std::memcpy(e.data(), &id, 4);
    std::memcpy(e.data() + 4, &handle, 4);
    std::memcpy(e.data() + 8, &account, 8);
    const auto p = read_player_info(e.data(), 1);
    REQUIRE(p.has_value());
    CHECK(p->handle == 0x40u);
    CHECK(p->account == account);
    CHECK_FALSE(read_player_info(e.data(), 2).has_value());  // another entity's entry
    const uint64_t none = 0;
    std::memcpy(e.data() + 8, &none, 8);
    CHECK_FALSE(read_player_info(e.data(), 1).has_value());  // no account
    CHECK_FALSE(read_player_info(nullptr, 1).has_value());
}

TEST_CASE("connection_name: the text at record +0x20 with its length at +0x28 (seen live), found by handle or account") {
    auto r = records({{0x40, 0x80, 0xA1}, {0x83, 0x85, 0xA2}});
    alignas(8) static const char ember[16] = "Ember\0junk";
    alignas(8) static const char bad[16] = "Bad\x1b[2J";
    alignas(8) static char long_name[80];
    std::memset(long_name, 'x', sizeof long_name);
    auto set_name = [&](uint32_t slot, const char* text, uint64_t len) {
        uint8_t* rec = r.data() + slot * server_layout::kConnectionSize;
        const uint64_t p = reinterpret_cast<uint64_t>(text);
        std::memcpy(rec + server_layout::kConnectionName, &p, 8);
        std::memcpy(rec + server_layout::kConnectionNameLength, &len, 8);
    };
    const MemoryRead read = [](const void* src, void* dst, size_t n) {
        if (!src) return false;
        std::memcpy(dst, src, n);
        return true;
    };
    set_name(0, ember, 5);
    CHECK(connection_name(r.data(), 0x40, 0xA1, read) == std::optional<std::string>("Ember"));
    CHECK(connection_name(r.data(), 0x44 /* a stale hint */, 0xA1, read) == std::optional<std::string>("Ember"));
    CHECK_FALSE(connection_name(r.data(), 0x40, 0xCC, read).has_value());  // nobody holds that account
    set_name(3, bad, 8);
    CHECK_FALSE(connection_name(r.data(), 0x83, 0xA2, read).has_value());  // control characters: not a name
    set_name(3, long_name, 65);
    CHECK_FALSE(connection_name(r.data(), 0x83, 0xA2, read).has_value());  // longer than 64 bytes
    set_name(3, long_name, 0);
    CHECK_FALSE(connection_name(r.data(), 0x83, 0xA2, read).has_value());  // empty
    set_name(3, nullptr, 4);
    CHECK_FALSE(connection_name(r.data(), 0x83, 0xA2, read).has_value());  // nothing readable there
    CHECK_FALSE(connection_name(nullptr, 0x40, 0xA1, read).has_value());
}

TEST_CASE("chat_line_limit: 30 lines per mod in 10 s; the Lua host (every Lua mod, each held to 30 by the host) gets 120") {
    CHECK(chat_line_limit("herald") == 30);
    CHECK(chat_line_limit("lua") == 120);
    CHECK(chat_line_limit("LUA") == 120);
    ChatLineBudget b(30, 10000);
    CHECK(b.take("lua", 100, 1000, chat_line_limit("lua")));
    CHECK(b.take("lua", 20, 1000, chat_line_limit("lua")));
    CHECK_FALSE(b.take("lua", 1, 1000, chat_line_limit("lua")));
    CHECK_FALSE(b.take("herald", 31, 1000, chat_line_limit("herald")));
}
