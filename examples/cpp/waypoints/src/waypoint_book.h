#pragma once
// waypoints: the part without the game (unit-tested): the waypoint book, its text in the world's file, and the line
// that guides you ("home: 120 m ahead left, 4 m up").
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace waypoints {

struct Vec {
    double x = 0, y = 0, z = 0;  // metres; y is up
};
struct Waypoint {
    std::string name;
    Vec pos;
};

// A name is one word (a command line splits at spaces): 1-24 letters, digits, _ or -.
inline bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 24) return false;
    for (const unsigned char c : name)
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

inline bool same_name(std::string_view a, std::string_view b) {  // ignoring case: "Home" is "home"
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

class Book {
public:
    static constexpr size_t kMax = 50;  // a list a person can read
    enum class Set { Added, Moved, BadName, Full };

    Set set(const std::string& name, Vec pos) {
        if (!valid_name(name)) return Set::BadName;
        for (auto& w : list_)
            if (same_name(w.name, name)) {
                w.pos = pos;  // keeps its first spelling
                return Set::Moved;
            }
        if (list_.size() >= kMax) return Set::Full;
        list_.push_back({name, pos});
        sort();
        return Set::Added;
    }
    const Waypoint* find(std::string_view name) const {
        for (const auto& w : list_)
            if (same_name(w.name, name)) return &w;
        return nullptr;
    }
    bool erase(std::string_view name) {
        for (auto it = list_.begin(); it != list_.end(); ++it)
            if (same_name(it->name, name)) {
                list_.erase(it);
                return true;
            }
        return false;
    }
    const std::vector<Waypoint>& all() const { return list_; }  // sorted by name, ignoring case

    // The world's file: one "name x y z" line per waypoint.
    std::string to_text() const {
        std::string out;
        char line[96];
        for (const auto& w : list_) {
            std::snprintf(line, sizeof line, "%s %.2f %.2f %.2f\n", w.name.c_str(), w.pos.x, w.pos.y, w.pos.z);
            out += line;
        }
        return out;
    }
    // Reads it back. A line it cannot read is skipped, so an edited or damaged file never stops the mod.
    static Book from_text(std::string_view text) {
        Book b;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            std::string line(text.substr(start, end - start));
            start = end + 1;
            char name[32] = {}, rest[2] = {};
            Vec p;
            // exactly a name and three numbers: a fourth field (rest) means the line is not ours
            if (sscanf_s(line.c_str(), "%31s %lf %lf %lf %1s", name, static_cast<unsigned>(sizeof name), &p.x, &p.y, &p.z,
                         rest, static_cast<unsigned>(sizeof rest)) == 4)
                b.set(name, p);
        }
        return b;
    }

private:
    void sort() {
        for (size_t i = 1; i < list_.size(); ++i)  // insertion sort: the list is short and nearly sorted
            for (size_t j = i; j > 0 && lower(list_[j].name) < lower(list_[j - 1].name); --j) std::swap(list_[j], list_[j - 1]);
    }
    static std::string lower(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    std::vector<Waypoint> list_;
};

// Which way is "right" for the camera. The camera looks along its +z; whether its +x is your right depends on the
// engine's handedness, which is not checked in game: a guess. If the line on screen says "left" for a waypoint on your
// right, set this to false and build again.
constexpr bool kCameraXIsRight = true;

// Which way `to` lies for someone at `from` looking along `forward` (the camera's view direction): "ahead",
// "ahead right", "right", "behind right", "behind", "behind left", "left" or "ahead left". Only the ground plane counts
// (x and z); "" when the camera looks straight up or down, or `to` is right above or below you.
inline std::string direction(Vec from, Vec forward, Vec to) {
    const double flen = std::hypot(forward.x, forward.z);
    const double dx = to.x - from.x, dz = to.z - from.z;
    if (flen < 1e-6 || std::hypot(dx, dz) < 0.5) return "";
    const double fx = forward.x / flen, fz = forward.z / flen;
    const double ahead = dx * fx + dz * fz;
    const double right = kCameraXIsRight ? dx * fz - dz * fx : dz * fx - dx * fz;
    static const char* const kNames[8] = {"ahead",  "ahead right", "right", "behind right",
                                          "behind", "behind left", "left",  "ahead left"};
    double deg = std::atan2(right, ahead) * 180.0 / 3.14159265358979323846;  // 0 ahead, 90 right, -90 left
    if (deg < 0) deg += 360.0;
    return kNames[static_cast<int>((deg + 22.5) / 45.0) % 8];
}

// The line on screen: "home: 120 m ahead left, 4 m up" (the distance on the ground, then the height when it is 3 m
// or more). Without a view direction (no camera known) only the distance.
inline std::string guide_text(const std::string& name, Vec from, const Vec* forward, Vec to) {
    const long metres = std::lround(std::hypot(to.x - from.x, to.z - from.z));
    std::string text = name + ": " + std::to_string(metres) + " m";
    const std::string way = forward ? direction(from, *forward, to) : "";
    if (!way.empty()) text += " " + way;
    const long up = std::lround(to.y - from.y);
    if (up >= 3) text += ", " + std::to_string(up) + " m up";
    if (up <= -3) text += ", " + std::to_string(-up) + " m down";
    return text;
}

// Close enough to call it arrived: within 4 m.
inline bool arrived(Vec from, Vec to) {
    const double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    return dx * dx + dy * dy + dz * dz < 16.0;
}

}  // namespace waypoints
