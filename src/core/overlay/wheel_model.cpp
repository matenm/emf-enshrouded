#include "wheel_model.h"

#include <cmath>

namespace emf::core {

void WheelModel::open(std::string title, std::vector<std::string> items, bool hold_mode, uint16_t hold_vk,
                      uint64_t now_ms) {
    title_ = std::move(title);
    items_ = std::move(items);
    hold_mode_ = hold_mode;
    hold_vk_ = hold_vk;
    opened_ms_ = now_ms;
    page_ = 0;
    x_ = y_ = 0.0f;
    scroll_ = 0.0f;
    open_ = !items_.empty();
}

void WheelModel::mouse(float dx, float dy) {
    if (!open_) return;
    x_ += dx;
    y_ += dy;
    const float len = std::sqrt(x_ * x_ + y_ * y_);
    if (len > kMaxRadius) {
        x_ *= kMaxRadius / len;
        y_ *= kMaxRadius / len;
    }
}

size_t WheelModel::pages() const { return items_.empty() ? 0 : (items_.size() + kPerPage - 1) / kPerPage; }

size_t WheelModel::page_count() const {
    if (items_.empty()) return 0;
    const size_t first = page_first();
    return items_.size() - first < kPerPage ? items_.size() - first : kPerPage;
}

void WheelModel::scroll(float steps) {
    if (!open_ || steps == 0.0f) return;
    const size_t n = pages();
    if (n <= 1) return;
    scroll_ += steps;
    while (scroll_ <= -1.0f) {
        page_ = (page_ + 1) % n;
        scroll_ += 1.0f;
    }
    while (scroll_ >= 1.0f) {
        page_ = (page_ + n - 1) % n;
        scroll_ -= 1.0f;
    }
}

int WheelModel::highlighted() const {
    if (!open_ || std::sqrt(x_ * x_ + y_ * y_) < kDeadZone) return -1;
    const size_t n = page_count();
    if (n == 0) return -1;
    // 0 degrees = up, clockwise (screen y grows downwards).
    double angle = std::atan2(static_cast<double>(x_), static_cast<double>(-y_)) * 180.0 / 3.14159265358979323846;
    if (angle < 0.0) angle += 360.0;
    const double width = 360.0 / static_cast<double>(n);
    const size_t slice = static_cast<size_t>(std::floor((angle + width / 2.0) / width)) % n;
    return static_cast<int>(page_first() + slice);
}

std::optional<int> WheelModel::finish(int result) {
    open_ = false;
    return result;
}

std::optional<int> WheelModel::key(const KeyEvent& e, uint64_t now_ms) {
    if (!open_) return std::nullopt;
    if (e.down && e.scan == InputRouter::kEscScan && !e.extended) return finish(-1);
    if (hold_mode_ && !e.down && e.vkey == hold_vk_) {
        const int h = highlighted();
        const bool flick = std::sqrt(x_ * x_ + y_ * y_) >= kTapPickRadius;
        if (now_ms - opened_ms_ < kTapMs && !flick) {  // tapped, not held: stay open for a click
            hold_mode_ = false;
            return std::nullopt;
        }
        return finish(h);
    }
    return std::nullopt;
}

std::optional<int> WheelModel::button(int which) {
    if (!open_) return std::nullopt;
    if (which == 1) return finish(-1);
    if (which == 0) {
        const int h = highlighted();
        if (h >= 0) return finish(h);
    }
    return std::nullopt;
}

std::optional<int> WheelModel::cancel() {
    if (!open_) return std::nullopt;
    return finish(-1);
}

}  // namespace emf::core
