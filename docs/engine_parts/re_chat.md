# Text chat: where a sent message goes (build 1269579)

The goal is to catch `/give Material_T1_Wood_Logs 5` typed into the game's own text chat, run it as an EMF command, and drop the message so it is never sent.
The client-side sections are static analysis only, with REA/Ghidra and `tools/re`: **nothing in them was observed at runtime.** The server side (build 1273043, below) was confirmed in game. Each signature below was checked with REA `search_bytes` over all initialized memory: exactly 1 match each. Confidence is High unless noted.

## TL;DR
- **Hook `queueChatMessage` 0x140d5ce70** (UI side). It runs once per message the local player submits with Enter, from either chat window. It runs before any network code, so it behaves the same offline, on a host and on a client.
- Text: `rdx` points at a `StringView {const char* data; u64 size}`. These are 8-bit chars, the size is in bytes, there is no guaranteed NUL, and the maximum is 0x400. The text is almost certainly UTF-8. `r8b` = kind: **0 = typed text**. Other callers pass 2 or 3 with an empty text.
- Drop: **return `true` without calling the original.** The UI then acts exactly as after a successful send: "sent" sound, input box cleared, chat closed. The message is never queued, so it never reaches the GameClient, the server or other players. Nobody else waits for it.
- Optional local reply line: `chatLogAdd` 0x140e546e0 on the client chat system `*(GameClient+0xF8)`. Medium/Low confidence: how the line renders is not verified.

## Call chain (observed in code)
1. **Chat UI** (immediate mode). There are two windows: `FUN_140cc7780` (HUD text chat, called from 0x140bdbe70) and `FUN_140ca9e80` (second chat UI, called from 0x140bd7700). Both reference `"Send to chat"`.
   - Text input widget `FUN_140b12120(widget, &buf{ptr,len,cap=0x400}, persistentText, ...)`. The submit state comes from `FUN_140d155b0(widget, &state, &"Send to chat", 0)`.
   - `state == 1` with a non-empty text calls `queueChatMessage(DAT_14281b498 /*UI ctx*/, &StringView{buf.ptr, buf.len}, 0, 0, 0)`.
     - If it returns true: play the sound at `*(ui+200)+0x12C30`, clear the persistent input text (`*(text+8) = 0`), reset one widget field (`+0x1980 = 0`).
   - Any other non-zero state plays the cancel sound (`+0x12C40`).
   - In both cases `FUN_140d5e7e0(ui, 1)` follows. It is another UI command constructor (probably close/defocus chat).
   - `FUN_140ca9e80` also sets a byte (`+0x2FA = 1`) just before the call, whatever the result.
2. **`queueChatMessage` 0x140d5ce70** appends **UI command 0x74** (0x40C bytes) to the UI command queue (`*(*(ui+0xD0)+0x33CC0)`; a slice of pointers plus an arena). Record layout:
   - `+0x000` u8 = 0x74
   - `+0x001` u8 kind
   - `+0x002` char[0x400] text
   - `+0x402` u16 length
   - `+0x404` u32 locaTag1
   - `+0x408` u32 locaTag2

   It returns false (and queues nothing) when the queue is full. The text copy `FUN_140b1bf50` (a `SmallDynamicArray<char,0x400>`) **aborts the game** (fatal `SmallDynamicArray is out of elements`) if the length is over 0x400.
3. **App command dispatch** `FUN_140845600(gameApp)` (reached through a table; its caller 0x140845a70 has no direct callers).
   - It walks the queued commands (`gameApp+0x34F58`, via `FUN_1412032c0`) and handles a few app-level types itself.
   - It forwards the rest to the play state `FUN_1408534a0` (`gameApp+0x35280`), whose default case forwards to the **GameClient command handler `FUN_140851530`** (or to the pregame state machine at `gameApp+0x35278`, which has no chat case).
4. **GameClient case 0x74** (inside `FUN_140851530`, so it cannot be hooked on its own):
   ```c
   if (*(u8*)(gameClient + 0x4672A2) != 0 || cmd[1] == 4)
       chatClientSend(&res, *(void**)(gameClient + 0xF8), cmd[1], &StringView{cmd+2, *(u16*)(cmd+0x402)}, *(u32*)(cmd+0x404), *(u32*)(cmd+0x408));
   ```
   `gameClient+0x4672A0..A2` is a copy of the server's `CommunicationSetup {voiceChatMode, isVoiceChatEnabled, isTextChatEnabled}`. It is written from `ServerInfoMessage` in `FUN_1408451e0` and `FUN_1408695a0`. So **+0x4672A2 = isTextChatEnabled**. That is Medium: the byte offset (0xEA = third byte of the 3-byte struct at 0xE8) is observed, and the field order is inferred from the adjacent strings. If the server disables text chat, a kind-0 message is dropped here.
5. **`chatClientSend` 0x140e973b0** (only caller: case 0x74).
   - It allocates a node of len+0x40 from the pool at `sys+0x1D8`. Node layout: `+0` kind, `+4` relative offset 0x3C (text at node+0x40), `+8` len, `+0xC` tag1, `+0x10` tag2, `+0x14` the global `DAT_14281b4e8`, `+0x28/+0x30` prev/next.
   - It appends the node to the **outbox** list (`sys+0x208` head, `+0x210` tail, `+0x218` count).
   - Result `{u8 code; u32}`: 0 ok, 0x12 too long, 2 pool full. The caller ignores it.
6. **Outbox flush `FUN_1408664d0(gameClient)`**, called from GameClient update 0x14086e7b0.
   - For each node it builds a `ChatMessageClientMessage` (0x410 bytes: `+0` u32 type, `+4` string[0x400] + length, `+0x408` locaTag1, `+0x40C` locaTag2; descriptor 0x141fb99f8).
   - It serializes the message with `FUN_1411d81d0`, sends it on the connection at `gameClient+0x180`, and pops the node (`FUN_140e8dd80`).
   - Its log text says `"[server] Failed to open chat message message"`, but this is client code; the same string is copy-pasted.
7. **Server**: `FUN_140e47c20` receives it (`Received chat message from unknown Source...`) and adds it to the server's ChatMessagesSystem (set up in 0x140e3a350). `FUN_140e44a20` then sends a `ChatMessageServerMessage` (0x420 bytes: same fields plus `playerHandle` +0x410 and `accountIdHash` +0x418) to each of the 17 player slots, **including the sender**.
8. **Client receive**: in `FUN_1408695a0`, message id 0x13 = `ChatMessageServerMessage`, which calls `chatLogAdd(gameClient+0xF8 system, ...)`. The chat log UI reads that inbox.
   - So the local player's own line only appears through the server echo. Dropping the message at step 2 means it shows nowhere.

## Server side, confirmed in game (dedicated server, build 1273043)
- A player's chat line reaches `chatLogAdd` from the server's receive loop with `playerHandle` = the player's
  **connection handle** (0x40 for the first player: index `handle & 0x3F` into 16 connection records of 0x148 bytes at
  `Server+0x1E0`, generation in the bits above). Record `+0` = that handle, `+4` = the player's **peer handle** (0x81),
  `+0x48` = the account hash.
- The chat system's reader for that player is `peer & 0x7F` (1 here); the send loop marks lines by it, and the server's
  slot table at `Server+0x16A0` (17 x 8 bytes) holds the same peer handles. `connection handle & 0x7f` is NOT the
  reader (it gave 64).
- `chatLogAdd` sets the reader-mask bit of every inactive reader and adds the line's size to every active reader's
  pending count; the server sends a line to readers whose bit is clear.
- A line added with `playerHandle` 0 and `accountIdHash` 0 is sent to the players but their game does not show it. With
  the sender's handle and account it shows as "<their name>: <text>".

### Lines from mods (EMF API v19)
- A player's `BasicPlayerInfos` entry (the server world singleton, entry `entity - 1`, 0xA0 bytes) holds at `+0x04`
  the same connection handle their chat lines carry (0x40 for the first player) and at
  `+0x08` their account hash. `chat_tell` takes the handle from there and checks it against the connection record
  (`Server+0x1E0`, handle & 0x3F) before it adds a line spoken by that player and narrows it to their reader; when the
  record holds another connection, the record whose `+0x48` holds the account is used.
- `chat_broadcast` adds one line per connected record (handle not 0 and naming its own slot, an account, a peer whose
  reader the mask can name), each spoken by its player and narrowed to that player.
- Players' ordinary lines reach `on_chat` from the same `chatLogAdd` hook (kind 0, inside the receive loop), after the
  command check; EMF's own lines are added through the original function and never come back to it.
- **Confirmed in game:**
  - A dedicated server with a game on the same PC joined: `BasicPlayerInfos` +0x04 = 0x40 matched the connection record (peer
    0x81, reader 1); the joined game showed the broadcast, the tell outside a command and the command replies as
    "<name>: [Server] ...".
  - A game hosted with `[chat] players=1`: the host also has a connection record (handle 0x40, peer 0x80, reader 0) and
    the server sends it the lines (its pending count goes back to 0), but the offline session has no text chat
    (ENGINE_MAP: Enter opens nothing), so nothing shows; EMF keeps a notify for the host.
  - The connection record holds the player's name: `+0x20` a pointer to its text, `+0x28` its length (u64), `+0x30`
    its capacity. It is the platform name the chat shows as the speaker, not the character's name.
    `BasicPlayerInfos` holds no name (its pointers at +0x20, +0x40, +0x60, +0x80 are arrays; checked in game).
  - `requestServerSave` from a server world callback: `world saved` 3-43 ms later, on the dedicated server and in the
    hosted game; the callback runs on the same thread as the `loadSavegameStep` hook.

## Hook candidates

### A. `queueChatMessage` 0x140d5ce70 (recommended)
- Signature (struct offsets wildcarded): `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 41 8B D9 45 0F B6 F8 4C 8B E2 48 8B A8 ?? ?? ?? ??`. **1 match** (0x140d5ce70). The shorter 32-byte prefix (up to `41 8B D9`) is also unique.
- Prototype: `bool __fastcall queueChatMessage(void* uiCtx, const StringView* text, u8 kind, u32 locaTag1, u32 locaTag2)`.
  - `rcx` = UI context (`DAT_14281b498`), `rdx` = text, `r8b` = kind, `r9d` = tag1, `[rsp+0x28]` = tag2.
  - The callers only test `al`.
- Callers, all four checked:
  - 0x140cc7780 and 0x140ca9e80: kind 0, the typed text.
  - 0x140c78180: kind 2, empty text, a tag from the UI data at `+0xAFA0/+0xAF90`.
  - 0x140c1e630: kind 3, empty text, an entity/template tag.
  - Kinds 2 and 3 are probably ping or marker lines (Low). Filter on `kind == 0`.
- Drop: return true. **What skipping changes**: nothing outside the UI. The input box clears and the chat closes as usual, because the caller does that on `true`. If the hook returned false instead, the typed text would stay in the box with no sound, and the chat would still close.
  - No pending flag waits for the command, and nothing counts sent messages. The 0x140ca9e80 byte at `+0x2FA` is set before the call, so it is set either way.
- Covers offline, host and client alike (it runs before the network). It also catches messages even when `isTextChatEnabled` is 0, as long as the chat input can be opened at all (not checked).
- Thread: the UI build thread. It is very likely the main/game thread that also runs the app command dispatch and the GameClient update, but that is inferred, not shown. Copy the text out and hand it to EMF's command runner. Do not keep the pointer, because it points into a UI stack buffer.

### B. `chatClientSend` 0x140e973b0 (alternative, game side)
- Signature: `48 89 6C 24 20 56 41 54 41 57 48 83 EC 20 49 8B 41 08 4D 8B F9 45 0F B6 E0 48 8B EA 48 8B F1 48 3D 00 04 00 00 76 ?? 33 D2 C6 01 12`. **1 match**.
- Prototype: `Result* __fastcall chatClientSend(Result* out, ChatSystem* sys, u8 kind, const StringView* text, u32 tag1, u32 tag2)`. `rcx` = out, `rdx` = sys, `r8b` = kind, `r9` = text, `[rsp+0x28]`/`[rsp+0x30]` = tags.
- Drop: `((u8*)out)[0] = 0; *(u32*)((u8*)out+4) = 0; return out;`. The single caller ignores the result.
- Downsides:
  - It is reached only when `isTextChatEnabled` is set (or kind 4).
  - It runs one frame later than A.
  - The UI has already played the "sent" sound and cleared the box by then, so the end result looks the same.
- It is the narrowest network-side point if A ever stops working.

## Optional: add a local line to the chat log
- **`chatLogAdd` 0x140e546e0**. Signature: `40 55 56 41 54 41 55 41 57 48 83 EC 30 49 8B 41 08 33 ED 4D 8B F9 45 0F B6 E0 48 8B F2 4C 8B E9 48 3D 00 04 00 00 76 ??`. **1 match**.
- Prototype: `Result* chatLogAdd(Result* out, ChatSystem* sys, u8 type, const StringView* text, u32 locaTag1, u32 locaTag2, u32 playerHandle, u64 accountIdHash, const struct {u64 time; u8 has;}* time)`.
  - It appends a node (text ≤ 0x400) to the **inbox** list (`sys+0x1C0/0x1C8/0x1D0`, pool `sys+0x190`). When the pool is full it evicts the oldest node.
  - It then marks the node for each of the 32 reader slots (UI windows on a client, players on the server).
- It is shared by the server and the client. Callers: client receive `FUN_1408695a0` and `FUN_140840a90`; server `FUN_140e47c20`, `FUN_140e48dc0`, `FUN_140e384d0`; and the ECS program `FUN_1402de660`, which uses type 1 with an empty text and a loca tag (a localized system line).
- For a local-only reply: call it on the **client** system `*(GameClient+0xF8)` (GameClient = `playState+0x1B8`) with type 0, your text, tags 0, and the time from `FUN_140a3d9b0(*(GameClient+0x18))` as `{t, 1}` (the client receive path does this).
  - Adding to the inbox sends nothing; the outbox is a different list. Never call it on the server's system, because that would broadcast the line to every player.
- Unknowns (Low): which `playerHandle`/`accountIdHash` makes the UI show a sensible speaker (0 may show an empty or unknown name, the local player's handle would show "you"). The lists have no lock, so call it on the GameClient thread only (for example from inside hook A, if that is the same thread). Test at runtime before relying on it.

## Text encoding (Medium-High)
- Every step handles `char` bytes with a byte length: the widget buffer has capacity 0x400 bytes; `FUN_140b1bf50`, the node copies and the reflected `string` field (capacity 0x400) all work on bytes. No UTF-16 conversion was seen anywhere on the path.
- So it is a byte string, which is almost certainly UTF-8. ASCII commands such as `/give ...` are safe either way.

## EMF sketch
```cpp
struct StringView { const char* data; uint64_t size; };
using QueueChatFn = bool(__fastcall*)(void*, const StringView*, uint8_t, uint32_t, uint32_t);
QueueChatFn o_queue_chat;
bool __fastcall hk_queue_chat(void* ui, const StringView* t, uint8_t kind, uint32_t a, uint32_t b) {
    if (kind == 0 && t && t->data && t->size > 1 && t->data[0] == '/') {
        std::string cmd(t->data + 1, t->size - 1);   // copy; the buffer is the UI's
        emf_run_console_command(cmd);                // placeholder: whatever the overlay console calls
        return true;                                 // UI clears the box; nothing is sent
    }
    return o_queue_chat(ui, t, kind, a, b);
}
```

## Dead ends (do not repeat)
- `sendChatMessage`, `openChat`, `closeChat`, `textChatOpenInput`, `UiTextChat`, `textChatMessage`, `chatMessage`: no code references (UI data or hashed names).
- Finding the 0x74 consumer by the displacement 0x33CC0 does not work. The hits are the ~150 UI command constructors (0x140d55af0–0x140d5fe90, 0x140afe5d0–0x140b05b40) and UI readers, plus false positives `0x14086daa0` (a stack offset) and `0x14082b5b0` (the UI-state constructor). The consumer reaches the queue through `gameApp+0x34F58`. What worked was searching for `movzx r32, word [reg+0x402]`.
- `FUN_140867e50` (pregame command handler, reached through `FUN_1408670d0`/`FUN_140867d70`) and `FUN_140870740` ("[pregame] Start Play") have no chat case.
- `FUN_140e6d0f0` (outbox peek) has no direct callers (it is inlined or reached indirectly); use `FUN_1408664d0` instead.

## Open risks
- The client-side hooks are not tested at runtime. The thread for hook A is inferred. Whether the chat input opens at all when the server disables text chat is unknown.
- Kinds 1, 2, 3 and 4 are only partly named (0 = typed text, 1 = localized system line; 2 and 3 come from two UI callers; 4 skips the text-chat gate).
- The wildcarded struct offsets keep signature A valid across small updates. The UI command id 0x74 is not in the signature, but it would change if commands are added before it. Re-check with `python sig.py` after game updates.
