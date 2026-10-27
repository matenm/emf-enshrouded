#include "protocol.h"

#include <cmath>

#include "json_out.h"

namespace emf::core::bridge {

Parsed parse_request(std::string_view line) {
    Parsed p;
    auto r = json::parse(line);
    if (!r.value) {
        p.error = "not JSON: " + r.error;
        return p;
    }
    json::Value& v = *r.value;
    if (!v.is(json::Value::Type::Object)) {
        p.error = "a request is one JSON object";
        return p;
    }
    const json::Value* id = v.get("id");
    if (!id) {
        p.error = "the request has no id";
        return p;
    }
    if (id->is(json::Value::Type::Number)) {
        if (!std::isfinite(id->n)) {
            p.error = "id must be a number or a string";
            return p;
        }
    } else if (id->is(json::Value::Type::String)) {
        if (id->s.size() > kMaxIdBytes) {
            p.error = "id is longer than 200 bytes";
            return p;
        }
    } else {
        p.error = "id must be a number or a string";
        return p;
    }
    p.id = to_json(*id);
    const json::Value* cmd = v.get("cmd");
    if (!cmd || !cmd->is(json::Value::Type::String) || cmd->s.empty()) {
        p.error = "the request has no cmd";
        return p;
    }
    Request req;
    req.id = p.id;
    req.cmd = cmd->s;
    req.body = std::move(v);
    p.request = std::move(req);
    return p;
}

std::wstring bridge_pipe_name(uint32_t pid) { return L"\\\\.\\pipe\\emf-bridge-" + std::to_wstring(pid); }

std::string bridge_info_text(const std::wstring& pipe, uint32_t pid) {
    std::string name;
    for (wchar_t c : pipe) name += c < 0x80 ? static_cast<char>(c) : '?';  // the name is ASCII
    return "pipe=" + name + "\r\npid=" + std::to_string(pid) + "\r\nprotocol=" + std::to_string(kProtocolVersion) +
           "\r\n";
}

std::string ok_reply(const std::string& id, const std::string& members) {
    std::string s = "{\"id\":" + id + ",\"ok\":true";
    if (!members.empty()) s += "," + members;
    return s + "}";
}

std::string error_reply(const std::string& id, std::string_view code, std::string_view error,
                        const std::string& members) {
    std::string s = "{\"id\":" + id + ",\"ok\":false,\"code\":" + json_quote(code) + ",\"error\":" + json_quote(error);
    if (!members.empty()) s += "," + members;
    return s + "}";
}

bool read_ms(const json::Value& body, const char* key, uint32_t def, uint32_t lo, uint32_t hi, uint32_t* out,
             std::string* error) {
    const json::Value* v = body.get(key);
    if (!v) {
        *out = def;
        return true;
    }
    if (!v->is(json::Value::Type::Number) || v->n != std::floor(v->n) || v->n < lo || v->n > hi) {
        *error = std::string(key) + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi);
        return false;
    }
    *out = static_cast<uint32_t>(v->n);
    return true;
}

bool read_text(const json::Value& body, const char* key, std::optional<std::string>* out, std::string* error) {
    out->reset();
    const json::Value* v = body.get(key);
    if (!v) return true;
    if (!v->is(json::Value::Type::String)) {
        *error = std::string(key) + " must be text";
        return false;
    }
    *out = v->s;
    return true;
}

bool read_bool(const json::Value& body, const char* key, std::optional<bool>* out, std::string* error) {
    out->reset();
    const json::Value* v = body.get(key);
    if (!v) return true;
    if (!v->is(json::Value::Type::Bool)) {
        *error = std::string(key) + " must be true or false";
        return false;
    }
    *out = v->b;
    return true;
}

bool read_seq(const json::Value& body, const char* key, std::optional<uint64_t>* out, std::string* error) {
    out->reset();
    const json::Value* v = body.get(key);
    if (!v) return true;
    if (!v->is(json::Value::Type::Number) || v->n != std::floor(v->n) || v->n < 0 || v->n > 9.0e15) {
        *error = std::string(key) + " must be a line number (0 or more)";
        return false;
    }
    *out = static_cast<uint64_t>(v->n);
    return true;
}

}  // namespace emf::core::bridge
