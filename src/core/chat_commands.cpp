#include "chat_commands.h"

#include "api.h"

#include <windows.h>

#include <cstring>

#include "common/hooks.h"

namespace emf::core {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Copies the game's text; false if it is empty, too long or cannot be read.
bool copy_text(const ChatText* t, char* out, size_t cap, size_t* n) {
    __try {
        if (!t->data || t->size == 0 || t->size > cap) return false;
        std::memcpy(out, t->data, static_cast<size_t>(t->size));
        *n = static_cast<size_t>(t->size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// queueChatMessage(uiContext, text, kind, locaTag1, locaTag2) -> sent
using QueueChatFn = bool(__fastcall*)(void* ui, const ChatText* text, uint8_t kind, uint32_t tag1, uint32_t tag2);
QueueChatFn o_queue_chat = nullptr;
ChatDeps g_deps;

bool __fastcall hk_queue_chat(void* ui, const ChatText* text, uint8_t kind, uint32_t tag1, uint32_t tag2) {
    bool taken = false;
    try {
        taken = take_chat_message(text, kind, g_deps);
    } catch (...) {  // never let an exception into the game's frames
    }
    return taken ? true : o_queue_chat(ui, text, kind, tag1, tag2);
}

}  // namespace

bool chat_line_command(std::string_view text, const std::function<bool(std::string_view)>& is_command,
                       std::string* line) {
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    if (text.size() < 2 || text[0] != '/' || is_space(text[1]) || text[1] == '/') return false;
    const std::string_view rest = text.substr(1);
    size_t end = 0;
    while (end < rest.size() && !is_space(rest[end])) ++end;
    std::string word(rest.substr(0, end));
    for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!is_command || !is_command(word)) return false;
    *line = std::string(rest);
    return true;
}

bool take_chat_message(const ChatText* text, uint8_t kind, const ChatDeps& deps) {
    if (!text || kind != kChatKindTyped || !deps.run || !deps.is_command) return false;
    char buf[kChatMaxBytes];
    size_t n = 0;
    if (!copy_text(text, buf, sizeof(buf), &n)) return false;
    std::string line;
    if (!chat_line_command(std::string_view(buf, n), deps.is_command, &line)) return false;
    if (deps.ours && !deps.ours()) return false;  // someone else's game: the server may run EMF and answer
    deps.run(line);
    return true;
}

bool install_chat_commands(uintptr_t target, ChatDeps deps) {
    if (!target || o_queue_chat) return false;
    g_deps = std::move(deps);
    return install_emf_hook("QueueChatMessage (chat commands)", target, reinterpret_cast<void*>(&hk_queue_chat),
                            reinterpret_cast<void**>(&o_queue_chat));
}

}  // namespace emf::core
