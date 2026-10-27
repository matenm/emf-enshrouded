#include "cursor_model.h"

#include <algorithm>

namespace emf::core {

void CursorModel::set_display(ImVec2 display) {
    display_ = display;
    if (!placed_) {
        placed_ = true;
        pos_ = ImVec2(display.x * 0.5f, display.y * 0.5f);
    }
    clamp();
}

void CursorModel::move(float dx, float dy, float speed) {
    pos_.x += dx * speed;
    pos_.y += dy * speed;
    clamp();
}

void CursorModel::clamp() {
    pos_.x = std::clamp(pos_.x, 0.0f, (std::max)(display_.x - 1.0f, 0.0f));
    pos_.y = std::clamp(pos_.y, 0.0f, (std::max)(display_.y - 1.0f, 0.0f));
}

}  // namespace emf::core
