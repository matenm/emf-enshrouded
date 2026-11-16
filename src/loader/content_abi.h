#pragma once
// The loader's own declaration of emf_content.dll's C interface (ABI 1): only what the loader calls. The DLL builds
// the content of a folder's EML mods; the loader loads it at the held start, calls it once and unloads it
// (content_build.h).
//
// The two exports:
//   uint32_t emf_content_abi(void)                    the interface's version: 1
//   int32_t  emf_content_build(request_json, log, log_ctx, reply, reply_cap, reply_len)
//
// request_json, UTF-8: {"game_dir": "<folder>", "file_name": "enshrouded", "force": true, "server": false}
//   game_dir: the folder with <file_name>.kfc, <file_name>.exe and mods\ (for EMF: the shadow copy, never the game's
//   own folder); force: build even when nothing changed; server: the dedicated server's data.
//
// log: called on the calling thread during the call, level 0 error, 1 warning, 2 info, 3 debug, one UTF-8 line
//   without a newline. It must return normally (no C++ exception may leave it).
//
// reply: a NUL-terminated UTF-8 JSON object in the caller's buffer:
//   {"code": n, "message": "...", "mods": [{"id": "...", "version": "..."}],
//    "mod_errors": [{"id": "..." or null, "path": "...", "error": "..."}], "dlls": [...], "ms": n}
//   plus "mod_warnings": [{"id": "...", "warning": "..."}] when a mod's Lua warned while the build ran (at most 30, 3 per mod,
//   each cut at 300 characters); the code stays what it was (OK when the build worked).
//   *reply_len is set to the full reply's length with its NUL. When it does not fit, the reply is cut: the real code
//   and ms, "truncated": true, empty lists, as much of the message as fits; the call returns the real code. Only when
//   not even that fits (or reply is null) does it return EMF_CONTENT_REPLY_TOO_SMALL and write nothing: the build has
//   run then and its result is unknown.
//
// One call at a time (a second one while one runs gives EMF_CONTENT_BAD_REQUEST), and the build and FreeLibrary on
// the same thread.
#include <cstddef>
#include <cstdint>

namespace emf::loader::content_abi {

constexpr uint32_t kAbiVersion = 1;

enum : int32_t {
    EMF_CONTENT_OK = 0,
    EMF_CONTENT_MODS_FAILED = 1,  // a mod's folder or mod.json could not be read (mod_errors says which)
    EMF_CONTENT_RUN_FAILED = 2,
    EMF_CONTENT_BAD_REQUEST = 3,
    EMF_CONTENT_REPLY_TOO_SMALL = 4,
    EMF_CONTENT_PANIC = 5,
};

typedef void (*emf_content_log_fn)(void* ctx, int level, const char* line);
typedef uint32_t (*emf_content_abi_fn)(void);
typedef int32_t (*emf_content_build_fn)(const char* request_json, emf_content_log_fn log, void* log_ctx, char* reply,
                                        size_t reply_cap, size_t* reply_len);
// emf_content_portraits (src/content/emf_content.h): came after ABI 1 without changing it; an older DLL has none
typedef int32_t (*emf_content_portraits_fn)(const char* request_json, emf_content_log_fn log, void* log_ctx,
                                            char* reply, size_t reply_cap, size_t* reply_len);

}  // namespace emf::loader::content_abi
