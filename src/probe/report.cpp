#include "report.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

#include "common/offsets.h"
#include "common/seh.h"

namespace emf::probe {
namespace {

template <class T>
bool rd(const void* addr, T& out) {
    return seh_read(addr, &out, sizeof(T));
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

char kind_letter(uint8_t kind) { return kind == 0 ? 's' : kind == 1 ? 'p' : kind == 2 ? 'c' : '?'; }

constexpr uint64_t kNoServerWarnAfterMs = 10000;
constexpr size_t kMaxNpcLines = 10;

}  // namespace

double fixed_to_meters(int64_t v) { return static_cast<double>(v) / 4294967296.0; }

bool npc_ctx_info(const void* ctx, uint32_t* entity_id, uint8_t* current_state) {
    if (!ctx) return false;
    const uint8_t* c = static_cast<const uint8_t*>(ctx);
    const uint8_t* pc = nullptr;
    const uint8_t* q = nullptr;
    const uint8_t* list = nullptr;
    const uint8_t* dyn = nullptr;
    uint32_t index = 0;
    if (!rd(c, pc) || !pc || !rd(pc, q) || !q || !rd(pc + 8, index) || index == 0) return false;
    if (!rd(q + 0x828, list) || !list || !rd(list + uint64_t(index - 1) * 16, *entity_id)) return false;
    if (!rd(c + 0x1B0, dyn) || !dyn || !rd(dyn, *current_state)) return false;
    return true;
}

void Reporter::on_tick(const WorldView& w, uint64_t now_ms, const LineSink& sink) {
    if (w.state() != 2) return;  // not running yet
    const uint8_t kind = w.kind();
    if (kind > 2) return;
    AcquireSRWLockExclusive(&lock_);
    if (kind == 0) server_seen_ = true;
    if (kind == 2 && first_client_ms_ == 0) first_client_ms_ = now_ms;
    const bool due = !reported_once_[kind] || now_ms - last_report_ms_[kind] >= interval_ms_;
    if (due) {
        reported_once_[kind] = true;
        last_report_ms_[kind] = now_ms;
    }
    const bool warn = kind == 2 && !server_seen_ && !warned_no_server_ &&
                      now_ms - first_client_ms_ >= kNoServerWarnAfterMs;
    if (warn) warned_no_server_ = true;
    ReleaseSRWLockExclusive(&lock_);

    if (warn) sink("no server world ticking after 10 s (client of someone else's game?)");
    if (!due) return;
    sink(fmt("world kind=%c tick=%u entities=%u singletons=%llu", kind_letter(kind), w.tick(), w.entity_count(),
             (unsigned long long)w.singleton_count()));
    if (kind == 2) report_client(w, sink);
    if (kind == 0) report_server(w, sink);
}

void Reporter::report_client(const WorldView& w, const LineSink& sink) {
    const uint8_t* lpd = static_cast<const uint8_t*>(w.singleton("LocalPlayerData"));
    if (!lpd) {
        sink("client: LocalPlayerData not registered yet");
        return;
    }
    uint32_t id = 0;
    if (!rd(lpd + off::kLocalPlayerId, id) || id == 0) {
        sink("client: no local player entity yet");
        return;
    }
    ComponentRef t = get_component(get_component_, w.raw(), off::kHashCurrentTransform, id);
    int64_t pos[3];
    if (!t.ptr || !seh_read(t.ptr, pos, sizeof(pos))) {
        sink(fmt("player id=%u: no CurrentTransform", id));
        return;
    }
    sink(fmt("player id=%u pos=(%.2f, %.2f, %.2f) m", id, fixed_to_meters(pos[0]), fixed_to_meters(pos[1]),
             fixed_to_meters(pos[2])));
}

void Reporter::report_server(const WorldView& w, const LineSink& sink) {
    size_t villagers = 0;
    for (uint32_t id : w.entity_ids()) {
        ComponentRef c = get_component(get_component_, w.raw(), off::kHashActiveNpcState, id);
        if (!c.ptr) continue;
        ++villagers;
        if (villagers > kMaxNpcLines) continue;
        uint8_t s[3] = {0, 0, 0};
        seh_read(c.ptr, s, sizeof(s));
        sink(fmt("npc id=%u size=%llu state=%u prev=%u decision=%u", id, (unsigned long long)c.size, s[0], s[1],
                 s[2]));
    }
    sink(fmt("villagers: %zu", villagers));
}

}  // namespace emf::probe
