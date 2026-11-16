#include "game_kick.h"

#include <windows.h>

#include <cstring>

#include "server_chat.h"

namespace emf::core {
namespace {
// No C++ objects here: __try cannot share a frame with them.
bool call_guarded(HandleAccountActionFn fn, uint8_t* server, const uint8_t* ev) {
    __try {
        fn(server, ev);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

std::array<uint8_t, kAccountActionEventSize> kick_event(uint64_t account) {
    std::array<uint8_t, kAccountActionEventSize> ev{};
    std::memcpy(ev.data() + 8, &account, sizeof account);
    ev[0x14] = kAccountActionKick;
    return ev;
}

bool kick_account(HandleAccountActionFn fn, uint8_t* game_server, uint64_t account) {
    if (!fn || !game_server || !account) return false;
    const auto ev = kick_event(account);
    return call_guarded(fn, game_server, ev.data());
}

std::optional<bool> connection_is_admin(const uint8_t* records, uint32_t handle, uint64_t account) {
    using namespace server_layout;
    if (!records || !account) return std::nullopt;
    // record i is a live connection (its handle names record i) of `account`; *h its handle
    const auto holds = [records, account](uint32_t i, uint32_t* h) {
        const uint8_t* r = records + static_cast<size_t>(i) * kConnectionSize;
        uint64_t acct = 0;
        std::memcpy(h, r, sizeof *h);
        std::memcpy(&acct, r + kConnectionAccount, sizeof acct);
        return *h != 0 && (*h & 0x3F) == i && acct == account;
    };
    const auto bit = [records](uint32_t i) {
        return (records[static_cast<size_t>(i) * kConnectionSize + kConnectionPerms] & 1) != 0;
    };
    uint32_t h = 0;
    if (handle != 0 && (handle & 0x3F) < kConnectionCount && holds(handle & 0x3F, &h) && h == handle)
        return bit(handle & 0x3F);  // the connection the caller knows: its own record
    // By the account: a player who reconnected may have two records for a moment (the old one not cleared yet); when
    // they disagree, the answer is not known (never the first record's bit).
    std::optional<bool> found;
    for (uint32_t i = 0; i < kConnectionCount; ++i) {
        if (!holds(i, &h)) continue;
        if (found && *found != bit(i)) return std::nullopt;
        found = bit(i);
    }
    return found;
}

}  // namespace emf::core
