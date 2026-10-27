#pragma once
// A radial menu ("wheel") for mods, drawn by the overlay: which entry the mouse direction highlights, pages of 8, and
// how a pick or a cancel happens. Pure logic (unit-tested); the overlay feeds it input and draws it.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "input_router.h"

namespace emf::core {

class WheelModel {
public:
    static constexpr size_t kPerPage = 8;
    static constexpr float kDeadZone = 40.0f;    // mouse counts from the centre before anything is highlighted
    static constexpr float kMaxRadius = 200.0f;  // the accumulated direction is clamped to this length
    static constexpr uint64_t kTapMs = 300;      // a hold-key release sooner than this is a tap...
    static constexpr float kTapPickRadius = 120.0f;  // ...that picks only after a flick at least this long

    // hold_mode: the wheel stays open while the key `hold_vk` is held and its release picks; otherwise a left click
    // picks. A quick tap (the release within kTapMs of opening, without a flick of kTapPickRadius) switches to the
    // click mode.
    // Esc, a right click or cancel() cancel. Resets the direction and the page. Times are in milliseconds.
    void open(std::string title, std::vector<std::string> items, bool hold_mode, uint16_t hold_vk, uint64_t now_ms);
    bool is_open() const { return open_; }

    // Raw mouse movement (screen axes: x right, y down).
    void mouse(float dx, float dy);
    // Mouse wheel steps: negative = next page, positive = previous page (wraps); fractions add up to whole notches.
    void scroll(float steps);

    // Index of the highlighted entry (in items()), or -1 inside the dead zone.
    int highlighted() const;
    size_t page() const { return page_; }
    size_t pages() const;
    // The entries shown on the current page: [first, first + count).
    size_t page_first() const { return page_ * kPerPage; }
    size_t page_count() const;

    // Each returns the result when the wheel closes (an entry index, or -1 = cancelled), else nothing.
    std::optional<int> key(const KeyEvent& e, uint64_t now_ms);
    std::optional<int> button(int which);  // 0 = left (down), 1 = right
    std::optional<int> cancel();

    const std::string& title() const { return title_; }
    const std::vector<std::string>& items() const { return items_; }
    std::pair<float, float> direction() const { return {x_, y_}; }
    bool hold_mode() const { return hold_mode_; }

private:
    std::optional<int> finish(int result);
    bool open_ = false;
    bool hold_mode_ = false;
    uint16_t hold_vk_ = 0;
    uint64_t opened_ms_ = 0;
    std::string title_;
    std::vector<std::string> items_;
    size_t page_ = 0;
    float x_ = 0.0f, y_ = 0.0f;
    float scroll_ = 0.0f;  // part of a notch not turned yet
};

}  // namespace emf::core
