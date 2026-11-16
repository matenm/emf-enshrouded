#pragma once
// Refusing a join: the game's own admin kick, server side (private research kick_player_2026-11-09.md). An admin's
// kick reaches handleAccountAction(GameServer*, const AccountActionEvent*) after the world tick; EMF calls that handler
// with its own Kick event, so the kick is exactly the admin's: the account is looked up when it runs, players with
// CanKickBan (admins, the host) are skipped, nothing is stored (the player can rejoin) and the target's game shows the
// game's "kicked" popup. Only Kick (0) is ever passed, never Ban (1) or UnBan (2): both write the ban lists.
#include <array>
#include <cstdint>
#include <optional>

namespace emf::core {

// AccountActionEvent (24 bytes): +0 timeStamp, +8 accountId (u64), +0x10 userGroupId (u32), +0x14 actionType (u8).
constexpr size_t kAccountActionEventSize = 24;
constexpr uint8_t kAccountActionKick = 0;

std::array<uint8_t, kAccountActionEventSize> kick_event(uint64_t account);

using HandleAccountActionFn = void(__fastcall*)(uint8_t* game_server, const uint8_t* event);
// Calls the handler for a Kick of `account` on `game_server` (the server object the receive loop runs for). Only on the
// server world's thread (the game calls it there), never while the world unloads. False without a handler, a server or
// an account, or when the call faulted.
bool kick_account(HandleAccountActionFn fn, uint8_t* game_server, uint64_t account);

// Whether the player on connection `handle` with `account` has the game's CanKickBan (an admin; the handler skips
// them), from a copy of the server's connection records (server_layout::kConnectionCount records from
// Server+kConnections). The record of `handle` when it holds that connection; else (handle 0: not known, or the record
// holds another connection now) the records that hold the account, whose handle names their own record. A player who
// reconnected can have two such records for a moment: when their bits disagree the answer is nullopt, never the first
// record's bit. nullopt also when no record holds that account.
std::optional<bool> connection_is_admin(const uint8_t* records, uint32_t handle, uint64_t account);

}  // namespace emf::core
