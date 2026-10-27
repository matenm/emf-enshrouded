#pragma once
// The debug bridge's line protocol (docs/modding/debug_bridge.md): one JSON object per line, UTF-8.
//   request: {"id": <number or string>, "cmd": "<name>", ...}
//   reply:   {"id": ..., "ok": true, ...}  or  {"id": ..., "ok": false, "error": "<plain words>"}
//   event:   {"event": "log" | "game" | ..., ...}  (pushed to clients that asked for them)
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/json.h"

namespace emf::core::bridge {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxLineIn = 64 * 1024;     // a request line, without its newline
constexpr size_t kMaxLineOut = 1024 * 1024;  // a reply or event line
constexpr size_t kMaxIdBytes = 200;          // a string id
constexpr int kMaxClients = 8;
constexpr int kBadLinesToDisconnect = 5;     // bad lines in a row before a client is disconnected

struct Request {
    std::string id;  // the id as JSON text, echoed in the reply
    std::string cmd;
    json::Value body;  // the whole request object
};

struct Parsed {
    std::optional<Request> request;  // empty on an error
    std::string id = "null";         // the id as JSON text when it could be read (for the error reply)
    std::string error;
};

// Reads one request line (without its newline).
Parsed parse_request(std::string_view line);

// {"id":<id>,"ok":true[,<members>]}; members are JSON object members without braces ("" for none).
std::string ok_reply(const std::string& id, const std::string& members);
// {"id":<id>,"ok":false,"code":"<code>","error":"..."[,<members>]}: code is one of timeout, busy, unavailable,
// bad_request, unknown_command, refused; members (optional) carry what there is anyway (a timed-out console's output).
std::string error_reply(const std::string& id, std::string_view code, std::string_view error,
                        const std::string& members = std::string());

// "\\.\pipe\emf-bridge-<pid>"
std::wstring bridge_pipe_name(uint32_t pid);
// emf\bridge.txt's text, for clients: "pipe=<name>", "pid=<pid>", "protocol=1" lines.
std::string bridge_info_text(const std::wstring& pipe, uint32_t pid);

// Optional request fields. Each returns false and sets *error when the field is there but wrong.
bool read_ms(const json::Value& body, const char* key, uint32_t def, uint32_t lo, uint32_t hi, uint32_t* out,
             std::string* error);
bool read_text(const json::Value& body, const char* key, std::optional<std::string>* out, std::string* error);
bool read_bool(const json::Value& body, const char* key, std::optional<bool>* out, std::string* error);
bool read_seq(const json::Value& body, const char* key, std::optional<uint64_t>* out, std::string* error);

}  // namespace emf::core::bridge
