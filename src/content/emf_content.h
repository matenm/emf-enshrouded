/* emf_content.h: the C ABI of emf_content.dll, which builds the content of a game folder's EML mods, and draws
   portraits of game models from the game's own files.
   See src/content/README.md for the rules: load it only at the held start before any mod, never in EMF.exe, and
   unload it after the build. */
#ifndef EMF_CONTENT_H
#define EMF_CONTENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMF_CONTENT_ABI 1
enum { EMF_CONTENT_OK = 0, EMF_CONTENT_MODS_FAILED = 1, EMF_CONTENT_RUN_FAILED = 2, EMF_CONTENT_BAD_REQUEST = 3,
       EMF_CONTENT_REPLY_TOO_SMALL = 4, EMF_CONTENT_PANIC = 5 };
/* emf_content_portraits: some items failed, the others are drawn or current (the same number as MODS_FAILED) */
enum { EMF_CONTENT_ITEMS_FAILED = 1 };
/* level: 0 error, 1 warning, 2 info, 3 debug; line: one UTF-8 log line without a newline */
typedef void (*emf_content_log_fn)(void* ctx, int level, const char* line);
uint32_t emf_content_abi(void);
int32_t emf_content_build(const char* request_json, emf_content_log_fn log, void* log_ctx,
                          char* reply, size_t reply_cap, size_t* reply_len);
int32_t emf_content_portraits(const char* request_json, emf_content_log_fn log, void* log_ctx,
                              char* reply, size_t reply_cap, size_t* reply_len);
/* The exports as GetProcAddress gives them. emf_content_portraits came after ABI 1 without changing it: a DLL
   without it is an older one (GetProcAddress gives NULL). */
typedef uint32_t (*emf_content_abi_fn)(void);
typedef int32_t (*emf_content_build_fn)(const char* request_json, emf_content_log_fn log, void* log_ctx,
                                        char* reply, size_t reply_cap, size_t* reply_len);
typedef int32_t (*emf_content_portraits_fn)(const char* request_json, emf_content_log_fn log, void* log_ctx,
                                            char* reply, size_t reply_cap, size_t* reply_len);

/* request_json (UTF-8):
     {"game_dir": "<folder holding enshrouded.kfc and mods\\>", "file_name": "enshrouded", "force": false,
      "server": null}
   file_name is optional (enshrouded or enshrouded_server when its .kfc or .exe is there, else the first .kfc or .exe);
   force rebuilds even when nothing changed; server is optional (null: EML decides from the file name). The run
   always patches, never exports and never registers runtime DLLs. For tests only, "test_panic": "<message>" makes
   the call panic with that message (it gives EMF_CONTENT_PANIC).

   log may be NULL. It is called only on the calling thread, during the call. It must return normally: no C++
   exception and no longjmp may leave it.

   One call at a time, over both calls: a call made while another runs (from another thread, or from inside the log
   callback) gives EMF_CONTENT_BAD_REQUEST at once. Call emf_content_build, emf_content_portraits and FreeLibrary on
   the same thread.

   reply: a NUL-terminated UTF-8 JSON
     {"code": n, "message": "...", "mods": [{"id": "...", "version": "..."}],
      "mod_errors": [{"id": "..." or null, "path": "...", "error": "..."}], "dlls": ["..."], "ms": n}
   and, only when a mod's Lua gave any while the build ran (EML's warn, or an error in its log span),
     "mod_warnings": [{"id": "...", "warning": "..."}]  (at most 20, each cut at 300 characters; the log has all)
   A build with warnings still returns its code (EMF_CONTENT_OK when it worked): the mod says part of it is missing.
   *reply_len is always set to the full reply's byte length with the NUL (reply_len must not be NULL: BAD_REQUEST).
   When reply_cap < *reply_len, the reply is cut to fit: it keeps the real code and ms, adds "truncated": true, has
   empty lists and as much of the message as fits, and the call still returns the real code. Only when not even that
   fits (about 100 bytes), or reply is NULL, does the call return EMF_CONTENT_REPLY_TOO_SMALL and write nothing; the
   build has run then and its result is unknown. Pass a big buffer (1 MB). */

/* emf_content_portraits: frames of game models turning, drawn from the game's own files (read-only) into a cache
   folder. Same conventions as emf_content_build: log, one call at a time, reply_len, a cut reply, no panic out.

   request_json (UTF-8):
     {"game_dir": "<folder holding enshrouded.kfc and enshrouded.exe>", "file_name": "enshrouded",
      "cache_dir": "<game>\\emf\\cache\\portraits",
      "items": [{"key": "wolf", "template": "<template GUID>", "frames": 24, "size": 256},
                {"key": "boar", "model": "<render model GUID>"}]}
   game_dir and cache_dir are needed (cache_dir a full path); file_name as for emf_content_build. Per item: key (1 to
   64 of A-Z a-z 0-9 _ . -, not ending in a dot, not a Windows device name such as CON or COM1, unique ignoring case);
   model (a keen::RenderModel) or template (a keen::ecs::TemplateResource, drawn with its ModelResource's model), a
   GUID as text, exactly one of them; frames 1 to 64 (default 24); size 8 to 512 (default 256). A bad item fails
   alone, and so does one whose drawing panics (the panic goes to the log). For tests only, "test_panic":
   "<message>" makes the call panic with that message.

   For each item: <cache_dir>\<key>\00.png, 01.png, ... (size x size RGBA8 PNG, sRGB, straight alpha, transparent
   background; frame i is the model turned by 360 * i / frames degrees, frame 0 a three-quarter view) and index.json
   (frames, size, the renderer's version, the template, and the inputs: the render model, its debugName, the content
   hashes of its render data and albedo textures, a hash of its records). An item whose index.json matches and whose
   frames are all there is left as it is ("current"); otherwise it is drawn again into a staging folder beside it,
   <key>~new-<pid>-<time> (time in seconds since 1970), which then replaces <cache_dir>\<key> whole: the old folder is
   moved aside to <key>~old-<pid>-<time> and removed. So the folder never mixes two drawings or keeps frames past the
   new count. A failed item's folder is as it was before the call, but for one case: when the new folder could not be
   moved in and the old one could not be moved back, the old one is left as <key>~old-<pid>-<time>, which the item's
   error names. A <cache_dir>\<key> that is not a folder (a file, a link) is never moved: that item fails. Staging and
   old folders left by earlier calls are removed when a call has a good item: this process's, and other processes'
   after an hour. The game's archive is opened once, and only when an item is good. No game file is written.

   reply: a NUL-terminated UTF-8 JSON
     {"code": n, "message": "...", "items": [{"key": "...", "status": "drawn" | "current" | "failed",
      "model": "<render model GUID>", "name": "<its debugName>", "frames": n, "size": n, "error": "...",
      "notes": ["..."]}], "ms": n}
   items are in the request's order; key is null when the item had no key as text; model, name, frames and size are
   left out of a failed item, error out of a good one, notes (what was left out of the drawing) when empty. code:
   EMF_CONTENT_OK every item drawn or current (or no items), EMF_CONTENT_ITEMS_FAILED some items failed,
   EMF_CONTENT_RUN_FAILED the game's archive or types could not be read (every good item failed),
   EMF_CONTENT_BAD_REQUEST not JSON, no game_dir, or no full cache_dir. A cut reply has "items": []. */

#ifdef __cplusplus
}
#endif

#endif
