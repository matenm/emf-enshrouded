#pragma once
// The mouse cursor the console draws while it is open: the game keeps the system cursor hidden and centred, so the
// console moves its own cursor with the raw mouse movement. Pure logic (unit-tested).
#include "imgui.h"

namespace emf::core {

class CursorModel {
public:
    // The display size; the first call puts the cursor in the middle, later calls keep it inside.
    void set_display(ImVec2 display);
    // Raw movement in mouse counts, times `speed`.
    void move(float dx, float dy, float speed);
    ImVec2 pos() const { return pos_; }

private:
    void clamp();
    ImVec2 display_{0, 0};
    ImVec2 pos_{0, 0};
    bool placed_ = false;
};

}  // namespace emf::core
