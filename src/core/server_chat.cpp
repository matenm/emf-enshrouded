#include "server_chat.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "chat_commands.h"

namespace emf::core {

bool ChatPolicy::allows(uint64_t account) const {
    if (everyone) return true;
    for (uint64_t a : admins)
        if (a == account) return true;
    return false;
}

ChatPolicy parse_chat_policy(std::string_view admins, bool everyone, std::vector<std::string>* skipped) {
    ChatPolicy p;
    p.everyone = everyone;
    size_t at = 0;
    while (at <= admins.size()) {
        size_t end = admins.find(',', at);
        if (end == std::string_view::npos) end = admins.size();
        std::string_view item = admins.substr(at, end - at);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
        const std::string_view entry = item;
        if (item.size() > 2 && item[0] == '0' && (item[1] == 'x' || item[1] == 'X')) item.remove_prefix(2);
        bool ok = !item.empty() && item.size() <= 16;
        uint64_t v = 0;
        for (char c : item) {
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) {
                ok = false;
                break;
            }
            v = (v << 4) | static_cast<uint64_t>(d);
        }
        if (ok) p.admins.push_back(v);
        else if (skipped && !entry.empty()) skipped->emplace_back(entry);
        at = end + 1;
    }
    return p;
}

std::string skipped_admin_warning(std::string_view entry) {
    return "[chat] admins: '" + printable(entry) +
           "' is not an account hash and is skipped (an account hash is up to 16 hex digits, e.g. 0x00ab00cd00ef0011; "
           "a Steam64 id is not one: the log line \"chat command refused from account 0x...\" shows the value to add)";
}

std::string account_text(uint64_t account) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(account));
    return buf;
}

ChatAction judge_chat(std::string_view text, uint64_t account, const ChatPolicy& policy,
                      const std::function<bool(std::string_view)>& is_command, std::string* line,
                      const std::function<bool(std::string_view)>& for_players) {
    std::string cmd;
    if (!chat_line_command(text, is_command, &cmd)) return ChatAction::Pass;
    if (!policy.allows(account)) {
        std::string word = cmd.substr(0, cmd.find_first_of(" \t"));
        for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!for_players || !for_players(word)) return ChatAction::Refuse;
    }
    if (line) *line = std::move(cmd);
    return ChatAction::Run;
}

ChatRateLimit::Verdict ChatRateLimit::check(uint64_t account, uint64_t now_ms) {
    std::lock_guard<std::mutex> l(m_);
    if (windows_.size() > 1024) windows_.clear();  // accounts come and go; forgetting them only resets their windows
    Window& w = windows_[account];
    if (w.count == 0 || now_ms - w.start > window_ms_) {
        w.start = now_ms;
        w.count = 0;
    }
    ++w.count;
    if (w.count <= max_) return Verdict::Ok;
    return w.count == max_ + 1 ? Verdict::Warn : Verdict::Drop;
}

bool ChatOutbox::add(uint32_t handle, uint64_t account, std::string text, bool raw) {
    std::lock_guard<std::mutex> l(m_);
    if (items_.size() >= kMaxChatOutbox) return false;
    ChatReply r;
    r.handle = handle;
    r.account = account;
    r.text = std::move(text);
    r.raw = raw;
    items_.push_back(std::move(r));
    return true;
}

bool ChatOutbox::add_everyone(std::string text) {
    std::lock_guard<std::mutex> l(m_);
    if (items_.size() >= kMaxChatOutbox) return false;
    ChatReply r;
    r.text = std::move(text);
    r.raw = true;
    r.everyone = true;
    items_.push_back(std::move(r));
    return true;
}

std::vector<ChatReply> ChatOutbox::take() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<ChatReply> out;
    out.swap(items_);
    return out;
}

void ChatOutbox::put_back(std::vector<ChatReply> lines) {
    if (lines.empty()) return;
    std::lock_guard<std::mutex> l(m_);
    lines.insert(lines.end(), std::make_move_iterator(items_.begin()), std::make_move_iterator(items_.end()));
    items_.swap(lines);
}

size_t ChatOutbox::size() {
    std::lock_guard<std::mutex> l(m_);
    return items_.size();
}

namespace {
// `s` cut to at most `n` bytes, never inside a UTF-8 character (continuation bytes 10xxxxxx are not cut from their lead).
std::string cut_utf8(std::string s, size_t n) {
    if (s.size() <= n) return s;
    size_t end = n;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    s.resize(end);
    return s;
}
}  // namespace

std::vector<std::string> chat_lines(std::string_view text, size_t max_lines, size_t* dropped) {
    std::vector<std::string> out;
    size_t extra = 0;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view part = text.substr(at, end - at);
        if (!part.empty() && part.back() == '\r') part.remove_suffix(1);
        at = end + 1;
        if (part.empty()) continue;  // empty lines (and a last empty one) are skipped
        if (out.size() < max_lines) out.push_back(cut_utf8(printable(part), static_cast<size_t>(kChatMaxBytes)));
        else ++extra;
    }
    if (extra) {
        out.push_back("...");
        --extra;
    }
    if (dropped) *dropped = extra;
    return out;
}

int chat_line_limit(std::string_view module) {
    if (module.size() == 3 && std::tolower(static_cast<unsigned char>(module[0])) == 'l' &&
        std::tolower(static_cast<unsigned char>(module[1])) == 'u' && std::tolower(static_cast<unsigned char>(module[2])) == 'a')
        return 120;
    return 30;
}

bool ChatLineBudget::take(const std::string& mod, int lines, uint64_t now_ms, int max) {
    std::lock_guard<std::mutex> l(m_);
    if (windows_.size() > 256) windows_.clear();  // forgetting only resets windows
    Window& w = windows_[mod];
    if (w.lines == 0 || now_ms - w.start >= window_ms_) {
        w.start = now_ms;
        w.lines = 0;
    }
    if (w.lines + lines > (max < 0 ? max_ : max)) return false;
    w.lines += lines;
    return true;
}

void ChatLineBudget::give_back(const std::string& mod, int lines) {
    if (lines <= 0) return;
    std::lock_guard<std::mutex> l(m_);
    auto it = windows_.find(mod);
    if (it != windows_.end()) it->second.lines = it->second.lines > lines ? it->second.lines - lines : 0;
}

bool ChatInbox::push(ChatLine line) {
    std::lock_guard<std::mutex> l(m_);
    if (items_.size() >= kMaxChatInbox) {
        ++dropped_;
        return false;
    }
    items_.push_back(std::move(line));
    return true;
}

std::vector<ChatLine> ChatInbox::take() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<ChatLine> out;
    out.swap(items_);
    return out;
}

std::vector<ChatConnection> connected_players(const uint8_t* records) {
    using namespace server_layout;
    std::vector<ChatConnection> out;
    if (!records) return out;
    for (uint32_t i = 0; i < kConnectionCount; ++i) {
        const uint8_t* rec = records + static_cast<size_t>(i) * kConnectionSize;
        uint32_t handle = 0;
        uint64_t account = 0;
        std::memcpy(&handle, rec, 4);
        std::memcpy(&account, rec + kConnectionAccount, 8);
        if (handle == 0 || (handle & 0x3F) != i || account == 0) continue;
        if (const auto reader = reader_from_connection(rec, handle, account)) out.push_back({handle, account, *reader});
    }
    return out;
}

std::optional<ChatConnection> connection_of(const uint8_t* records, uint64_t account) {
    if (account == 0) return std::nullopt;
    for (const auto& c : connected_players(records))
        if (c.account == account) return c;
    return std::nullopt;
}

namespace {
bool looks_like_pointer(uint64_t v) { return v >= 0x10000ull && v < 0x7FFFFFFF0000ull && (v & 7) == 0; }
}  // namespace

std::optional<std::string> connection_name(const uint8_t* records, uint32_t handle, uint64_t account,
                                           const MemoryRead& read) {
    using namespace server_layout;
    if (!records || !read || account == 0) return std::nullopt;
    const uint8_t* rec = nullptr;
    if (const auto at = connection_record_offset(handle)) {
        const uint8_t* r = records + (*at - kConnections);
        if (reader_from_connection(r, handle, account)) rec = r;
    }
    if (!rec)
        if (const auto c = connection_of(records, account)) rec = records + (c->handle & 0x3F) * kConnectionSize;
    if (!rec) return std::nullopt;
    uint64_t text = 0, length = 0;
    std::memcpy(&text, rec + kConnectionName, 8);
    std::memcpy(&length, rec + kConnectionNameLength, 8);
    if (!looks_like_pointer(text) || length == 0 || length > 64) return std::nullopt;
    char buf[64];
    if (!read(reinterpret_cast<const void*>(static_cast<uintptr_t>(text)), buf, static_cast<size_t>(length)))
        return std::nullopt;
    for (size_t i = 0; i < length; ++i) {
        const auto u = static_cast<unsigned char>(buf[i]);
        if (u < 0x20 || u == 0x7f) return std::nullopt;
    }
    return std::string(buf, static_cast<size_t>(length));
}

std::optional<PlayerInfo> read_player_info(const uint8_t* entry, uint32_t entity) {
    if (!entry || entity == 0) return std::nullopt;
    PlayerInfo p;
    std::memcpy(&p.entity, entry, 4);
    std::memcpy(&p.handle, entry + kPlayerInfoHandle, 4);
    std::memcpy(&p.account, entry + kPlayerInfoAccount, 8);
    if (p.entity != entity || p.account == 0) return std::nullopt;
    return p;
}

std::optional<size_t> connection_record_offset(uint32_t handle) {
    using namespace server_layout;
    const uint32_t index = handle & 0x3F;
    if (index >= kConnectionCount) return std::nullopt;
    return kConnections + kConnectionSize * index;
}

std::optional<uint32_t> reader_from_connection(const uint8_t* record, uint32_t handle, uint64_t account) {
    if (!record || handle == 0) return std::nullopt;  // 0 is no connection (a cleared record holds 0)
    uint32_t held = 0, peer = 0;
    uint64_t holder = 0;
    std::memcpy(&held, record, 4);
    std::memcpy(&peer, record + server_layout::kConnectionPeer, 4);
    std::memcpy(&holder, record + server_layout::kConnectionAccount, 8);
    if (held != handle || holder != account || peer == 0) return std::nullopt;
    const uint32_t reader = peer & 0x7F;
    if (reader >= static_cast<uint32_t>(chat_layout::kReaderCount)) return std::nullopt;
    return reader;
}

uint32_t entity_by_account(const uint8_t* infos, size_t count, uint64_t account,
                           const std::function<bool(uint32_t entity)>& alive) {
    if (!infos || account == 0) return 0;
    uint32_t found = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = infos + i * kPlayerInfoSize;
        uint32_t id = 0;
        std::memcpy(&id, e, 4);
        if (id != i + 1) continue;  // no player entity at this index
        bool holds = false;
        for (size_t at = 8; at + 8 <= kPlayerInfoSize && !holds; at += 8) {
            uint64_t v = 0;
            std::memcpy(&v, e + at, 8);
            holds = v == account;
        }
        if (!holds || (alive && !alive(id))) continue;
        if (found) return 0;  // two live players with this value: not an account field after all
        found = id;
    }
    return found;
}

std::string printable(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = '?';
    }
    return out;
}

std::optional<std::string> ReplyLimiter::next(const std::string& line) {
    if (sent_ < max_) {
        ++sent_;
        return line;
    }
    if (sent_ == max_) {
        ++sent_;
        return std::string("... (more in the server log)");
    }
    return std::nullopt;
}

std::string describe_chat_system(const uint8_t* sys) {
    using namespace chat_layout;
    if (!sys) return "no chat system";
    std::string out;
    char buf[160];
    const uint8_t* node = nullptr;
    std::memcpy(&node, sys + kTail, sizeof node);
    if (node) {
        uint32_t len = 0, handle = 0, mask = 0;
        uint64_t account = 0;
        std::memcpy(&len, node + kNodeLen, 4);
        std::memcpy(&handle, node + kNodeHandle, 4);
        std::memcpy(&account, node + kNodeAccount, 8);
        std::memcpy(&mask, node + kNodeMask, 4);
        std::snprintf(buf, sizeof buf, "newest: len %u handle 0x%x account %s mask 0x%x;", len, handle,
                      account_text(account).c_str(), mask);
        out += buf;
    } else {
        out += "newest: none;";
    }
    out += " readers:";
    for (int i = 0; i < kReaderCount; ++i) {
        const uint8_t* r = sys + kReaders + kReaderStride * static_cast<size_t>(i);
        int32_t pending = 0;
        std::memcpy(&pending, r + kReaderPending, 4);
        if (r[0] == 0 && pending == 0) continue;
        std::snprintf(buf, sizeof buf, " r%d %s %d", i, r[0] ? "on" : "off", pending);
        out += buf;
    }
    return out;
}

bool newest_line_is(const uint8_t* sys, std::string_view text) {
    using namespace chat_layout;
    if (!sys) return false;
    const uint8_t* node = nullptr;
    std::memcpy(&node, sys + kTail, sizeof node);
    if (!node) return false;
    uint32_t len = 0;
    std::memcpy(&len, node + kNodeLen, 4);
    return len == text.size() && std::memcmp(node + kNodeText, text.data(), text.size()) == 0;
}

bool restrict_line(uint8_t* sys, uint32_t reader) {
    using namespace chat_layout;
    if (!sys || reader >= static_cast<uint32_t>(kReaderCount)) return false;
    uint8_t* node = nullptr;
    std::memcpy(&node, sys + kTail, sizeof node);
    if (!node) return false;
    uint32_t len = 0, mask = 0;
    std::memcpy(&len, node + kNodeLen, 4);
    std::memcpy(&mask, node + kNodeMask, 4);
    uint32_t size = len + 0x17;  // what chatLogAdd adds to each active reader's pending count
    if (size & 3) size = size - (size & 3) + 4;
    for (int i = 0; i < kReaderCount; ++i) {
        if (static_cast<uint32_t>(i) == reader || (mask & (1u << i))) continue;
        uint8_t* r = sys + kReaders + kReaderStride * static_cast<size_t>(i);
        if (r[0] == 0) continue;  // not a player: chatLogAdd set its bit already
        int32_t pending = 0;
        std::memcpy(&pending, r + kReaderPending, 4);
        pending -= static_cast<int32_t>(size);
        std::memcpy(r + kReaderPending, &pending, 4);
        mask |= 1u << i;
    }
    std::memcpy(node + kNodeMask, &mask, 4);
    return true;
}

}  // namespace emf::core
