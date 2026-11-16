#pragma once
// Markers (API v33): what EMF copies from the game for drawing, lock-free for the UI and render threads. In the UI
// frame (the HUD editor's UiScopeOpen detour calls markers_ui_frame for every scope): the camera block of UiPlayData
// (the view at +0x1d20, read from the UiCameraBlock guard, and the projection 0x40 after it) when doPlayStateUi() opens;
// when the game last opened doCachedEntityHealthWidgets (its world-space health bars: the world UI shows) and mapScaled
// (the map screen); the rectangles of mapScaled, MapCenteringFrame and doPlayStateUi(). On the client world's thread:
// the G38UiPlayData pointer and the local player's position. Private research markers_2026-11-11 1.1-1.5 and 3.1-3.2,
// the spike in plans/2026-11-11-markers-step1.ledger.md.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "common/fnv.h"
#include "core/markers.h"

namespace emf::core {

constexpr uint64_t kMarkerUiSpaceHash = 0xf5139eefe5b90316ull;  // "doPlayStateUi()": a new UI frame
constexpr uint64_t kMarkerWorldUiHash = emf::fnv1a64("doCachedEntityHealthWidgets");
constexpr uint64_t kMarkerMapScaledHash = emf::fnv1a64("mapScaled");
constexpr uint64_t kMarkerMapFrameHash = emf::fnv1a64("MapCenteringFrame");
constexpr uint64_t kMarkerFreshMs = 250;      // a scope seen within this long counts as shown
constexpr uint64_t kMarkerWorldStaleMs = 2000;  // the client world's tick older than this: its UiPlayData is not read
constexpr size_t kMarkerNodeRect = 0x114;       // a UI node's rect (x, y, w, h in UI units), as the HUD editor reads it

// A seqlock for a small trivially copyable value: one writer, any number of readers; a reader that meets a write in
// progress (or a changed sequence) retries up to 4 times, then gives up.
template <class T>
class SeqBox {
    static_assert(std::is_trivially_copyable_v<T>);

public:
    void store(const T& v) {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&data_, &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }
    // false: never stored, or torn every try
    bool load(T* out) const {
        for (int i = 0; i < 4; ++i) {
            const uint32_t a = seq_.load(std::memory_order_acquire);
            if (a == 0) return false;
            if (a & 1) continue;
            std::memcpy(out, &data_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == a) return true;
        }
        return false;
    }
    void reset() { seq_.store(0, std::memory_order_release); }
    void begin_write_for_tests() { seq_.store(seq_.load() | 1u); }

private:
    std::atomic<uint32_t> seq_{0};
    T data_{};
};

struct MarkerSight {
    bool world_ui = false;  // the game showed its world-space UI within kMarkerFreshMs
    bool map_open = false;  // the map screen (mapScaled) within kMarkerFreshMs
};
// When the UI thread last saw those scopes (its clock; 0: never), for the console's `markers`.
struct MarkerSeen {
    uint64_t world_ui_ms = 0;  // doCachedEntityHealthWidgets
    uint64_t map_ms = 0;       // mapScaled
};

struct MapView {
    float rect[4] = {};  // mapScaled: the whole map (x, y, w, h), UI units, last frame's layout
    float clip[4] = {};  // MapCenteringFrame: where the map shows
    float ui_w = 0, ui_h = 0;  // doPlayStateUi()'s size: UI units -> display pixels
    uint64_t ms = 0;
};

// The view's offset in UiPlayData, from the UiCameraBlock guard (0 = capture off).
void marker_capture_set_layout(uint32_t view_off);
bool marker_capture_ready();
// The client world's thread, every tick: its G38UiPlayData (null when there is none) and now.
void marker_capture_set_uipd(uint8_t* uipd, uint64_t now_ms);
void marker_capture_set_player(bool known, double x, double y, double z);
// The local player's rotation (CurrentTransform +0x18, a quaternion), from the same client tick.
void marker_capture_set_player_rotation(float x, float y, float z, float w);
bool marker_player_rotation(float out[4]);
// The UI thread: every scope the game opens (node: the scope's node, may be null).
void markers_ui_frame(uint64_t hash, const uint8_t* node, uint64_t now_ms);

// The display's size in pixels (the overlay sets it every frame); before that the UI's size from doPlayStateUi().
void marker_capture_set_display(float w, float h);
bool marker_display(float* w, float* h);
// doPlayStateUi()'s size in UI units as the game laid it out (kept every frame, also with the HUD editor off): false
// before it was seen. The UI is 1080 units high; its width follows the screen's shape.
bool marker_ui_size(float* w, float* h);

bool marker_camera(CameraSnap* out);
bool marker_player_pos(double out[3]);
MarkerSight marker_sight(uint64_t now_ms);
MarkerSeen marker_seen();
bool marker_map_view(MapView* out);
uint32_t marker_capture_faults();  // reads that faulted (a bad UiPlayData pointer)
void marker_capture_reset_for_tests();

}  // namespace emf::core
