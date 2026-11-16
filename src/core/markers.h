#pragma once
// Markers (API v33): the game-free part. The store of every marker this game draws (a mod's own, and those a server
// sent), the projection of a world point the way the game's worldToUi does it (private research markers_2026-11-11
// 1.1), the arrow at the screen's edge, the fade toward a marker's max distance, the distance text, and the wire format
// of the core channel emf/markers.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "emf/emf_api.h"

namespace emf::core {

constexpr size_t kMarkersPerOwner = 256, kMarkersTotal = 1024, kMarkerTextBytes = 64, kMarkerIdMax = 48;
constexpr size_t kMarkersPerMessage = 256;
constexpr double kMarkerCoordMax = 100000.0;  // metres from the origin (the main scene is 10 240 m square)

struct Marker {
    std::string owner;  // the mod (module name; "lua" for the Lua host, whose ids start "<script id>:")
    std::string id;
    bool received = false;  // a server sent it (emf/markers); kept apart from the game's own mods' markers
    double x = 0, y = 0, z = 0;
    uint32_t entity = 0;  // followed in the client world; 0 = fixed position
    float height = 0;
    std::string text;
    uint32_t icon = 0;  // a v26 picture
    uint32_t rgba = 0;  // 0 = the HUD's accent colour
    uint8_t shape = EMF_MARKER_DIAMOND;
    uint32_t flags = 0;
    float max_distance = 0;
    uint32_t game_icon = 0;
};

// A valid marker id: 1-48 of A-Z a-z 0-9 _ . - :
bool marker_id_ok(const char* id);
// The store's number checks: v33's size, finite coordinates within kMarkerCoordMax, a sane height, max_distance and shape.
bool marker_numbers_ok(const EmfMarker& m);
// `s` cut to at most n bytes, never inside a UTF-8 character.
std::string marker_cut_utf8(std::string s, size_t n);

class MarkerStore {
public:
    enum class SetResult { Ok, Replaced, BadId, BadNumbers, OwnerFull, Full };
    // Sets (or replaces) the marker (owner, id) among the local or the received ones. Refused: a bad id, a struct
    // smaller than v33's EmfMarker, coordinates beyond kMarkerCoordMax or not finite, a bad height, max_distance or
    // shape; the owner's 257th or the store's 1025th marker. The text is cut at 64 bytes.
    SetResult set(const std::string& owner, const EmfMarker& m, bool received);
    bool remove(const std::string& owner, const std::string& id, bool received);
    size_t clear(const std::string& owner);  // that owner's local markers
    size_t clear_received();                 // every marker a server sent
    // A followed marker's position, by id (received ones; the server's refresh): false when it is not there.
    bool move(const std::string& owner, const std::string& id, double x, double y, double z);
    std::vector<Marker> snapshot() const;
    size_t size() const;
    // Every followed marker takes its entity's position where pos_of knows it; the others keep theirs.
    void follow(const std::function<bool(uint32_t entity, double out[3])>& pos_of);

private:
    using Key = std::tuple<bool, std::string, std::string>;  // received, owner, id
    mutable std::mutex m_;
    std::map<Key, Marker> markers_;
};

// The camera of one UI frame: the game's view (affine, row vectors: rows 0-2 the axes, row 3 the translation) and
// projection (elements 0, 1, 4, 5, 8, 9, 12, 13) from UiPlayData, when the frame was built, and whether it is real.
struct CameraSnap {
    float v[16] = {};
    float p[16] = {};
    uint64_t frame_ms = 0;
    bool valid = false;
};

struct ScreenPoint {
    float x = 0, y = 0;  // display pixels
    float depth = 0;     // metres in front of the camera
};
// The world point on a w x h display, or nothing when it is behind (or at) the camera, the camera is not valid or
// has no projection, or the numbers are not finite.
std::optional<ScreenPoint> project(const CameraSnap& cam, double x, double y, double z, float w, float h);

struct EdgePoint {
    bool valid = false;
    bool behind = false;
    float x = 0, y = 0;  // on the rectangle `inset` pixels inside the display's edges
    float angle = 0;     // the arrow's direction, radians (0 = right, pi/2 = down)
};
// Where the edge arrow of a point off screen (or behind) sits: on the inset rectangle, along the line from the
// centre toward the point; a point behind is mirrored to the side it is on (straight behind: the bottom).
EdgePoint edge_point(const CameraSnap& cam, double x, double y, double z, float w, float h, float inset);

// 1 within 80 % of max_distance, 0 at it and beyond, linear between; max_distance 0 = 1 everywhere.
float fade(double dist, float max_distance);
// "42 m", or "1.2 km" from 1000 m; "" for a negative or not finite distance.
std::string distance_text(double metres);

// ---- the wire format of emf/markers (JSON): {"o": owner, "s": [markers], "r": [ids], "p": [[id, x, y, z]]} ----
struct DecodedMarker {
    std::string id, text;
    EmfMarker marker{};  // id and text are null here: as_marker() points them at the strings above
    EmfMarker as_marker() const {
        EmfMarker m = marker;
        m.id = id.c_str();
        m.text = text.empty() ? nullptr : text.c_str();
        return m;
    }
};
struct MarkerPosition {
    std::string id;
    double x = 0, y = 0, z = 0;
};
struct DecodedMarkers {
    std::string owner;
    std::vector<DecodedMarker> markers;
    std::vector<std::string> removes;
    std::vector<MarkerPosition> positions;
};
std::string encode_markers(const std::string& owner, const std::vector<EmfMarker>& markers,
                           const std::vector<std::string>& removes);
std::string encode_positions(const std::string& owner, const std::vector<MarkerPosition>& positions);
// False for text that is not such an object, an owner that is not 1-48 characters, more than kMarkersPerMessage
// markers, removes or positions, or a position that is not [text, number, number, number]. Fields of a marker that are
// missing take their defaults; a marker's own checks are the store's.
bool decode_markers(const std::string& text, DecodedMarkers* out);

}  // namespace emf::core
