#pragma once
// Draws a mod's radial wheel with Dear ImGui: a ring of up to 8 slices in the middle of the screen, the highlighted one
// lit, the title and the page in the centre, a hint below. Renderer-agnostic.
#include <functional>
#include <string>
#include <utility>

#include "imgui.h"
#include "wheel_model.h"

namespace emf::core {

// "Mace (6 Wood Logs, 1 Stone)" -> {"Mace", "(6 Wood Logs, 1 Stone)"}; a label without " (" stays on one line.
std::pair<std::string, std::string> wheel_label_lines(const std::string& label);

// A title wider than `width` breaks at the last space that keeps the first line inside it; the second line may still
// be too wide (the drawing cuts it). A title that fits, or has no such space, stays on one line.
std::pair<std::string, std::string> wheel_title_lines(const std::string& title, float width,
                                                      const std::function<float(const std::string&)>& measure);

void draw_wheel(const WheelModel& wheel, ImVec2 display);

}  // namespace emf::core
