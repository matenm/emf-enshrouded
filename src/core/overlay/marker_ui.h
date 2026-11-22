#pragma once
// Markers (API v33) on screen: where each world marker goes (the projection of the newest UI frame's camera, the fade,
// the edge arrow, the text of the 64 nearest), whether markers show at all, and their drawing in the game's style (the
// HUD font and its shadow, the marker's colour for its shape).
#include <cstdint>
#include <string>
#include <vector>

#include "core/marker_capture.h"
#include "core/markers.h"
#include "core/overlay/look_layout.h"
#include "core/overlay/window_ui.h"
#include "imgui.h"

namespace emf::core {

constexpr size_t kMarkerLabels = 64;      // the nearest this many markers show their text
constexpr float kMarkerSize1080 = 26.0f;  // a marker's shape at 1080 p, pixels
constexpr float kMarkerEdgeInset = 48.0f;  // the edge arrows' distance from the display's edges at 1080 p
constexpr float kMarkerEdgeBottom = 130.0f;  // ... and from the bottom, above the game's hotbar

struct MarkerLayout {
    float x = 0, y = 0;  // display pixels: the shape's centre
    float size = kMarkerSize1080;
    float alpha = 1;
    bool edge = false;  // off screen or behind: at the edge, with an arrow pointing out at `angle`
    float angle = 0;
    bool label = false;  // its text and distance are drawn
    double distance = 0;
    std::string text, dist;
    uint32_t rgba = 0;  // 0 = the HUD's accent colour
    uint8_t shape = EMF_MARKER_DIAMOND;
    uint32_t icon = 0;
    float bar = -1;  // EMF_MARKER_BAR: the filled share of the bar under the text; negative: no bar
};

// The markers with EMF_MARKER_WORLD that show, far ones first. player: the local player's position (null: the
// distance from the camera).
std::vector<MarkerLayout> layout_markers(const std::vector<Marker>& markers, const CameraSnap& cam, const double* player,
                                         ImVec2 display);
// Markers show while the console is closed, the game showed its world-space UI within kMarkerFreshMs, the map is not
// open and the camera is from a UI frame at most kMarkerFreshMs old.
bool show_markers(bool console_open, const MarkerSight& sight, const CameraSnap& cam, uint64_t now_ms);
// The markers with EMF_MARKER_GAME_MAP on the game's map screen: the map's rectangle (mapScaled, UI units) gives the
// place (the main scene is kMapWorldSize metres square, north up), the display's size over the UI's the pixels;
// outside MapCenteringFrame they are left out. Every one has its text, no distance; one with a game_icon is the text
// alone (the game draws its icon from its own list).
constexpr double kMapWorldSize = 10240.0;
constexpr float kMapMarkerSize1080 = 24.0f;
std::vector<MarkerLayout> layout_map_markers(const std::vector<Marker>& markers, const MapView& view, ImVec2 display);
// While the map is open (seen within kMarkerFreshMs), its view is that fresh and has a size, and the console is closed.
bool show_map_markers(bool console_open, const MarkerSight& sight, const MapView& view, uint64_t now_ms);
// Draws them (the render thread, inside an ImGui frame).
void draw_markers(ImDrawList* dl, const std::vector<MarkerLayout>& layouts, const TextLook& look, const ImageLookup* pictures);

}  // namespace emf::core
