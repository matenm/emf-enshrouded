#include "hud_model.h"

#include <algorithm>

namespace emf::core {

namespace {
bool good_id(const std::string& id) {
    if (id.empty() || id.size() > HudModel::kMaxId) return false;
    for (char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-'))
            return false;
    return true;
}
}  // namespace

HudModel::Result HudModel::set(const std::string& id, const std::string& text, uint32_t place, uint32_t rgba) {
    if (!good_id(id)) return Result::BadId;
    if (place > static_cast<uint32_t>(HudPlace::BottomLeft)) return Result::BadPlace;
    std::lock_guard<std::mutex> l(m_);
    if (text.empty()) {
        if (lines_.erase(id)) ++version_;
        return Result::Removed;
    }
    auto it = lines_.find(id);
    if (it == lines_.end() && lines_.size() >= kMaxLines) return Result::Full;
    size_t cut = (std::min)(text.size(), kMaxText);
    if (cut < text.size())
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;  // not inside a character
    HudLine line{id, text.substr(0, cut), static_cast<HudPlace>(place), rgba};
    if (it != lines_.end() && it->second.text == line.text && it->second.place == line.place &&
        it->second.rgba == line.rgba)
        return Result::Ok;  // unchanged
    lines_[id] = std::move(line);
    ++version_;
    return Result::Ok;
}

std::vector<HudLine> HudModel::lines() const {
    std::vector<HudLine> out;
    {
        std::lock_guard<std::mutex> l(m_);
        for (const auto& [id, line] : lines_) out.push_back(line);  // map order: by id
    }
    std::stable_sort(out.begin(), out.end(), [](const HudLine& a, const HudLine& b) { return a.place < b.place; });
    return out;
}

bool HudModel::empty() const {
    std::lock_guard<std::mutex> l(m_);
    return lines_.empty();
}

uint64_t HudModel::version() const {
    std::lock_guard<std::mutex> l(m_);
    return version_;
}

}  // namespace emf::core
