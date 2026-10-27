#include "commands.h"

#include <algorithm>

#include "fault_report.h"

namespace emf::core {
namespace {

thread_local const ReplyFn* t_reply = nullptr;
thread_local const CommandSender* t_sender = nullptr;

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    for (char c : n)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-'))
            return false;
    return true;
}

bool call_guarded(CommandFn fn, EmfWorld* world, int argc, const char* const* argv, void* user) {
    __try {
        fn(world, argc, argv, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ReplyScope {
    const ReplyFn* previous;
    explicit ReplyScope(const ReplyFn* r) : previous(t_reply) { t_reply = r; }
    ~ReplyScope() { t_reply = previous; }
};

// Calls a queued line's done callback, with its outcome, on every way out of its handling.
struct DoneCall {
    const DoneFn& done;
    CommandOutcome outcome = CommandOutcome::Empty;
    ~DoneCall() {
        if (done) done(outcome);
    }
};

}  // namespace

const char* command_outcome_name(CommandOutcome o) {
    switch (o) {
        case CommandOutcome::Ran: return "ran";
        case CommandOutcome::Unknown: return "unknown";
        case CommandOutcome::Refused: return "refused";
        case CommandOutcome::NeedsWorld: return "needs_world";
        case CommandOutcome::TooOld: return "too_old";
        case CommandOutcome::Crashed: return "crashed";
        default: return "empty";
    }
}

std::vector<std::string> split_command(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) ++i;
        if (i >= line.size()) break;
        std::string word;
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '"') ++i;
                word += line[i++];
            }
            ++i;  // closing quote (or end)
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n')
                word += line[i++];
        }
        out.push_back(std::move(word));
    }
    return out;
}

void command_reply_line(const std::string& text) {
    if (t_reply && *t_reply) (*t_reply)(text);
}

ReplyFn current_reply() { return t_reply && *t_reply ? *t_reply : ReplyFn(); }

namespace {
thread_local int t_private = 0;  // PrivateReplies alive on this thread
}  // namespace

bool reply_is_private() { return t_private > 0; }
PrivateReplies::PrivateReplies() { ++t_private; }
PrivateReplies::~PrivateReplies() { --t_private; }
ReplyFn private_reply(ReplyFn reply) {
    if (!reply) return reply;
    return [reply = std::move(reply)](const std::string& line) {
        const PrivateReplies mark;
        reply(line);
    };
}

const CommandSender* current_sender() { return t_sender; }

uint32_t command_player(uint32_t local_player) {
    return t_sender && t_sender->from_chat ? t_sender->entity : local_player;
}

bool CommandCenter::add(const std::string& name, const std::string& help, CommandFn fn, void* user, std::string* why,
                        bool anywhere, uint32_t flags, bool update_same_owner) {
    auto fail = [why](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (!fn) return fail("null callback");
    if (!valid_name(name)) return fail("command names are 1-32 letters, digits, _ . or -");
    if (flags & ~kCommandKnownFlags) return fail("unknown flags (a newer mod?)");
    const std::string key = lower(name);
    const bool players = (flags & kCommandForPlayers) != 0;
    AcquireSRWLockExclusive(&lock_);
    bool dup = false, updated = false;
    for (auto& c : cmds_) {
        if (c.name != key) continue;
        if (update_same_owner && c.fn == fn && c.user == user) {  // the same owner declares it again
            c.help = help;
            c.players = players;
            updated = true;
        } else {
            dup = true;
        }
    }
    if (!dup && !updated) cmds_.push_back(Cmd{key, help, fn, user, true, anywhere, players});
    ReleaseSRWLockExclusive(&lock_);
    return dup ? fail("a command with that name already exists") : true;
}

bool CommandCenter::for_players(std::string_view lower_name) const {
    bool yes = false;
    AcquireSRWLockShared(&lock_);
    for (const auto& c : cmds_)
        if (c.name == lower_name) yes = c.players;
    for (const auto& d : denied_)
        if (d == lower_name) yes = false;
    ReleaseSRWLockShared(&lock_);
    return yes;
}

void CommandCenter::set_denied_for_players(std::vector<std::string> names) {
    AcquireSRWLockExclusive(&lock_);
    denied_ = std::move(names);
    ReleaseSRWLockExclusive(&lock_);
}

std::vector<std::string> parse_command_names(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        if (c == ',' || c == ' ' || c == '\t') {
            if (!word.empty()) out.push_back(lower(word));
            word.clear();
        } else {
            word += c;
        }
    }
    return out;
}

CommandRoute command_route(uint8_t world_kind, bool server_world_exists) {
    if (world_kind == EMF_WORLD_SERVER) return CommandRoute::Run;
    if (world_kind == EMF_WORLD_CLIENT && !server_world_exists) return CommandRoute::Reject;
    return CommandRoute::None;
}

size_t CommandCenter::run_without_world(const std::string& line, const ReplyFn& reply, const std::string& reason,
                                        CommandOutcome* outcome) {
    CommandOutcome scratch = CommandOutcome::Empty;
    CommandOutcome& result = outcome ? *outcome : scratch;
    result = CommandOutcome::Empty;
    const auto words = split_command(line);
    if (words.empty()) return 0;
    if (reply) reply("> " + line);
    const std::string key = lower(words[0]);
    Cmd cmd{};
    size_t index = 0;
    bool found = false;
    AcquireSRWLockShared(&lock_);
    for (size_t i = 0; i < cmds_.size(); ++i)
        if (cmds_[i].name == key) {
            cmd = cmds_[i];
            index = i;
            found = true;
        }
    ReleaseSRWLockShared(&lock_);
    if (!found) {
        result = CommandOutcome::Unknown;
        if (reply) reply("unknown command '" + words[0] + "' (type help)");
        return 0;
    }
    if (!cmd.anywhere || !cmd.enabled) {
        result = cmd.enabled ? CommandOutcome::NeedsWorld : CommandOutcome::Refused;
        if (reply) reply(cmd.enabled ? reason : "command '" + key + "' is disabled (it crashed earlier)");
        return 0;
    }
    std::vector<const char*> argv;
    for (const auto& w : words) argv.push_back(w.c_str());
    const ReplyScope scope(&reply);
    if (!call_guarded(cmd.fn, nullptr, static_cast<int>(argv.size()), argv.data(), cmd.user)) {
        AcquireSRWLockExclusive(&lock_);
        if (index < cmds_.size()) cmds_[index].enabled = false;
        ReleaseSRWLockExclusive(&lock_);
        report_fault("command '" + key + "'", reinterpret_cast<const void*>(cmd.fn));
        result = CommandOutcome::Crashed;
        if (reply) reply("command '" + key + "' crashed and was disabled");
        return 0;
    }
    result = CommandOutcome::Ran;
    return 1;
}

size_t CommandCenter::reject_pending(const std::string& reason, const ReplyFn& reply) {
    std::vector<Line> queued;
    AcquireSRWLockExclusive(&lock_);
    queued.swap(queue_);
    ReleaseSRWLockExclusive(&lock_);
    size_t answered = 0;
    for (const auto& q : queued) {
        DoneCall done_call{q.done};
        if (split_command(q.text).empty()) continue;
        done_call.outcome = CommandOutcome::NeedsWorld;
        ++answered;
        for (const ReplyFn* r : {&reply, &q.reply})
            if (*r) {
                (*r)("> " + q.text);
                (*r)(reason);
            }
    }
    return answered;
}

void CommandCenter::enqueue(std::string line, uint64_t now_ms, CommandSender sender, ReplyFn reply, DoneFn done) {
    AcquireSRWLockExclusive(&lock_);
    size_t chat = 0;
    if (sender.from_chat)
        for (const auto& q : queue_) chat += q.sender.from_chat ? 1 : 0;
    const bool full = sender.from_chat && chat >= kMaxChatLines;
    if (!full) queue_.push_back(Line{std::move(line), now_ms, sender, reply, done});
    ReleaseSRWLockExclusive(&lock_);
    if (!full) return;
    if (reply) reply("not run: too many commands are waiting; try again in a moment");
    if (done) done(CommandOutcome::Refused);
}

size_t CommandCenter::pending() const {
    AcquireSRWLockShared(&lock_);
    const size_t n = queue_.size();
    ReleaseSRWLockShared(&lock_);
    return n;
}

size_t CommandCenter::run_pending(EmfWorld* world, const ReplyFn& reply, uint64_t now_ms, uint64_t max_age_ms) {
    std::vector<Line> queued;
    AcquireSRWLockExclusive(&lock_);
    queued.swap(queue_);
    const auto resolve = resolve_;
    ReleaseSRWLockExclusive(&lock_);
    size_t ran = 0;
    for (const auto& q : queued) {
        DoneCall done_call{q.done};  // destroyed last: after the reply and sender scopes below end
        // the line's own reply (a chat sender) gets everything the global reply gets for this line; copies, so that a
        // late line (current_reply) still reaches both after run_pending returned
        const ReplyFn both = q.reply ? ReplyFn([global = reply, own = q.reply](const std::string& s) {
            if (global) global(s);
            own(s);
        })
                                     : ReplyFn();
        const ReplyFn& out = q.reply ? both : reply;
        const ReplyScope scope(&out);
        CommandSender sender = q.sender;
        if (sender.from_chat && resolve) resolve(sender);  // world reads happen here, on the world's thread
        const CommandSender* const previous_sender = t_sender;
        t_sender = &sender;
        struct SenderReset {
            const CommandSender* prev;
            ~SenderReset() { t_sender = prev; }
        } sender_reset{previous_sender};
        const std::string& line = q.text;
        const auto words = split_command(line);
        if (words.empty()) continue;
        if (out) out("> " + line);
        if (now_ms > q.queued_ms && now_ms - q.queued_ms > max_age_ms) {
            done_call.outcome = CommandOutcome::TooOld;
            if (out)
                out("not run: typed " + std::to_string((now_ms - q.queued_ms) / 1000) +
                      " s ago, while no world you host was running");
            continue;
        }
        const std::string key = lower(words[0]);
        Cmd cmd{};
        size_t index = 0;
        bool found = false;
        AcquireSRWLockShared(&lock_);
        for (size_t i = 0; i < cmds_.size(); ++i)
            if (cmds_[i].name == key) {
                cmd = cmds_[i];
                index = i;
                found = true;
            }
        ReleaseSRWLockShared(&lock_);
        if (!found) {
            done_call.outcome = CommandOutcome::Unknown;
            if (out) out("unknown command '" + words[0] + "' (type help)");
            continue;
        }
        if (!cmd.enabled) {
            done_call.outcome = CommandOutcome::Refused;
            if (out) out("command '" + key + "' is disabled (it crashed earlier)");
            continue;
        }
        bool denied = false;
        AcquireSRWLockShared(&lock_);
        for (const auto& d : denied_) denied = denied || d == key;
        ReleaseSRWLockShared(&lock_);
        if (sender.restricted && (!cmd.players || denied)) {  // judged when it was queued too; it may have changed since
            done_call.outcome = CommandOutcome::Refused;
            if (out) out("not run: only admins may run " + key + " here (/help lists the commands you may run)");
            continue;
        }
        std::vector<const char*> argv;
        for (const auto& w : words) argv.push_back(w.c_str());
        ++ran;
        done_call.outcome = CommandOutcome::Ran;
        if (!call_guarded(cmd.fn, world, static_cast<int>(argv.size()), argv.data(), cmd.user)) {
            done_call.outcome = CommandOutcome::Crashed;
            AcquireSRWLockExclusive(&lock_);
            cmds_[index].enabled = false;
            ReleaseSRWLockExclusive(&lock_);
            report_fault("command '" + key + "'", reinterpret_cast<const void*>(cmd.fn));
            if (out) out("command '" + key + "' crashed and was disabled");
        }
    }
    return ran;
}

void CommandCenter::set_sender_resolver(std::function<void(CommandSender&)> resolve) {
    AcquireSRWLockExclusive(&lock_);
    resolve_ = std::move(resolve);
    ReleaseSRWLockExclusive(&lock_);
}

std::vector<CommandEntry> CommandCenter::entries() const {
    std::vector<CommandEntry> out;
    AcquireSRWLockShared(&lock_);
    for (const auto& c : cmds_) {
        bool players = c.players;
        for (const auto& d : denied_) players = players && d != c.name;
        out.push_back({c.name, c.help, players});
    }
    ReleaseSRWLockShared(&lock_);
    std::sort(out.begin(), out.end(), [](const CommandEntry& a, const CommandEntry& b) { return a.name < b.name; });
    return out;
}

std::vector<std::pair<std::string, std::string>> CommandCenter::list() const {
    std::vector<std::pair<std::string, std::string>> out;
    AcquireSRWLockShared(&lock_);
    for (const auto& c : cmds_) out.emplace_back(c.name, c.help);
    ReleaseSRWLockShared(&lock_);
    std::sort(out.begin(), out.end());
    return out;
}

void CommandCenter::reset() {
    AcquireSRWLockExclusive(&lock_);
    cmds_.clear();
    queue_.clear();
    ReleaseSRWLockExclusive(&lock_);
}

}  // namespace emf::core
