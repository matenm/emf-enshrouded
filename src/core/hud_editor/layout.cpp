#include "core/hud_editor/layout.h"

#include <cctype>
#include <cmath>
#include <cstdio>

#include "core/bridge/json_out.h"
#include "core/json.h"
#include "core/profiles.h"

namespace emf::core::hud {

namespace {

float round_hundredths(double v) { return static_cast<float>(std::round(v * 100.0) / 100.0); }

// Part and bar names in a file are read in any case, as the console commands take them ("Health" is health).
bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string raw_text(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    return buf;
}

// Reads a number field into *out through `clamp`; notes what moved or was wrong.
template <class Clamp>
void read_number(const json::Value& obj, const char* field, const std::string& key, double lo, double hi, double dflt,
                 Clamp clamp, float* out, std::vector<std::string>& notes) {
    const json::Value* v = obj.get(field);
    if (!v) return;
    const std::string where = key + "." + field;
    if (!v->is(json::Value::Type::Number)) {
        notes.push_back(where + " is not a number, left at " + number_text(static_cast<float>(dflt)));
        return;
    }
    *out = clamp(v->n);
    if (!std::isfinite(v->n) || v->n < lo || v->n > hi)
        notes.push_back(where + ": " + raw_text(v->n) + " is outside " + number_text(static_cast<float>(lo)) + " to " +
                        number_text(static_cast<float>(hi)) + ", kept at " + number_text(*out));
}

// A colour from a file into *out: a colour, or "game" (the game's: left unset); anything else is a note.
void read_colour(const json::Value& v, const std::string& where, Colour* out, std::vector<std::string>& notes) {
    if (v.is(json::Value::Type::String)) {
        const ColourText t = parse_colour(v.s);
        if (t.colour) {
            *out = *t.colour;
            return;
        }
        if (t.game) {  // the game's own colour, also over an earlier spelling of the same name in this file
            *out = Colour{};
            return;
        }
    }
    std::string shown = "that value";
    if (v.is(json::Value::Type::String))
        shown = bridge::json_quote(v.s);
    else if (v.is(json::Value::Type::Number))
        shown = raw_text(v.n);
    else if (v.is(json::Value::Type::Bool))
        shown = v.b ? "true" : "false";
    *out = Colour{};  // the game's, as the note says (also over an earlier spelling of the same name)
    notes.push_back(where + ": " + shown + " is not a colour (#rrggbb or #rrggbbaa), left at the game's");
}

}  // namespace

float clamp_offset(double percent) {
    if (!std::isfinite(percent)) return 0.0f;
    if (percent < kOffsetMin) return kOffsetMin;
    if (percent > kOffsetMax) return kOffsetMax;
    return round_hundredths(percent);
}

float clamp_scale(double s) {
    if (!std::isfinite(s)) return 1.0f;
    if (s < kScaleMin) return kScaleMin;
    if (s > kScaleMax) return kScaleMax;
    return round_hundredths(s);
}

int percent_to_pixels(float percent, float extent) {
    return static_cast<int>(std::lround(static_cast<double>(percent) / 100.0 * extent));
}

float pixels_to_percent(float pixels, float extent) {
    if (!(extent > 0.0f)) return 0.0f;
    return clamp_offset(static_cast<double>(pixels) / extent * 100.0);
}

float percent_to_units(float percent, float extent) { return static_cast<float>(percent / 100.0 * extent); }

std::string number_text(float v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v));
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string layout_to_json(const HudLayout& l, uint32_t build) {
    std::string out = "{\n  \"format\": " + std::to_string(kLayoutFormat) + ",\n  \"name\": " + bridge::json_quote(l.name) +
                      ",\n  \"build\": " + std::to_string(build) + ",\n";
    std::string body;
    const auto parts = hud_elements();
    for (size_t i = 0; i < kHudElementCount; ++i) {
        const ElementLayout& e = l.elements[i];
        if (e.is_default()) continue;
        if (!body.empty()) body += ",\n";
        body += "    " + bridge::json_quote(parts[i].scope) + ": {\"x\": " + number_text(e.x) + ", \"y\": " + number_text(e.y) +
                ", \"scale\": " + number_text(e.scale) + ", \"hidden\": " + (e.hidden ? "true" : "false") + "}";
    }
    if (l.order != kDefaultOrder) {
        out += "  \"vitals_order\": [";
        for (size_t i = 0; i < kBarCount; ++i) out += (i ? ", " : "") + bridge::json_quote(bar_id(l.order[i]));
        out += "],\n";
    }
    if (l.buffs_up) out += "  \"buffs_up\": true,\n";
    std::string colours;
    for (int b = 0; b < static_cast<int>(kBarCount); ++b) {
        const BarColours& bc = l.colours[static_cast<size_t>(b)];
        if (!bc.any()) continue;
        std::string fields;
        for (size_t i = 0; i < kBarPartCount; ++i) {
            if (!bc.part[i].set) continue;
            if (!fields.empty()) fields += ", ";
            fields += bridge::json_quote(bar_part_id(static_cast<BarPart>(i))) + ": " + bridge::json_quote(colour_text(bc.part[i]));
        }
        if (!colours.empty()) colours += ", ";
        colours += bridge::json_quote(bar_id(b)) + ": {" + fields + "}";
    }
    if (l.panel.set) colours += (colours.empty() ? "" : ", ") + std::string("\"panel\": ") + bridge::json_quote(colour_text(l.panel));
    if (!colours.empty()) out += "  \"colours\": {" + colours + "},\n";
    if (body.empty())
        out += "  \"elements\": {}\n";
    else
        out += "  \"elements\": {\n" + body + "\n  }\n";
    out += "}\n";
    return out;
}

LayoutRead layout_from_json(std::string_view text) {
    LayoutRead r;
    if (text.size() > kLayoutFileMax) {
        r.error = "larger than 64 KB";
        return r;
    }
    json::ParseResult pr = json::parse(text);
    if (!pr.value) {
        r.error = "not JSON: " + pr.error + " (line " + std::to_string(pr.line) + ", column " + std::to_string(pr.column) + ")";
        return r;
    }
    const json::Value& root = *pr.value;
    if (!root.is(json::Value::Type::Object)) {
        r.error = "not a HUD layout (the file is not a JSON object)";
        return r;
    }
    const json::Value* fmt = root.get("format");
    if (!fmt) {
        r.error = "no \"format\"";
        return r;
    }
    if (!fmt->is(json::Value::Type::Number)) {
        r.error = "\"format\" is not a number";
        return r;
    }
    if (fmt->n > kLayoutFormat) {
        r.error = "made by a newer EMF (format " + raw_text(fmt->n) + "): update EMF";
        return r;
    }
    if (fmt->n != kLayoutFormat) {
        r.error = "format " + raw_text(fmt->n) + " is not a HUD layout format";
        return r;
    }
    HudLayout l;
    if (const json::Value* name = root.get("name"); name && name->is(json::Value::Type::String)) {
        const std::string trimmed = trim_profile_name(name->s);
        const std::string problem = profile_name_problem(trimmed);
        if (problem.empty())
            l.name = trimmed;
        else
            r.notes.push_back("name: " + problem);
    }
    if (const json::Value* els = root.get("elements")) {
        if (!els->is(json::Value::Type::Object)) {
            r.error = "\"elements\" is not an object";
            return r;
        }
        const auto parts = hud_elements();
        for (const auto& [key, val] : els->obj) {
            size_t idx = kHudElementCount;
            for (size_t i = 0; i < kHudElementCount; ++i)
                if (same_name(key, parts[i].scope)) idx = i;
            if (idx == kHudElementCount) {
                r.notes.push_back("unknown part \"" + key + "\" skipped");
                continue;
            }
            if (!val.is(json::Value::Type::Object)) {
                r.notes.push_back(key + ": not an object, skipped");
                continue;
            }
            ElementLayout& e = l.elements[idx];
            read_number(val, "x", key, kOffsetMin, kOffsetMax, 0, clamp_offset, &e.x, r.notes);
            read_number(val, "y", key, kOffsetMin, kOffsetMax, 0, clamp_offset, &e.y, r.notes);
            if (parts[idx].bar < 0) {
                read_number(val, "scale", key, kScaleMin, kScaleMax, 1, clamp_scale, &e.scale, r.notes);
            } else if (const json::Value* s = val.get("scale"); s && !(s->is(json::Value::Type::Number) && s->n == 1)) {
                // a bar has no scale of its own (the vitals part scales the three): a hand-edited one would be kept and
                // shown, and scale nothing
                r.notes.push_back(key + ".scale: health, mana and stamina have no scale of their own (scale " +
                                  parts[static_cast<size_t>(find_element("vitals"))].scope + "), left at 1");
            }
            if (const json::Value* h = val.get("hidden")) {
                if (h->is(json::Value::Type::Bool))
                    e.hidden = h->b;
                else
                    r.notes.push_back(key + ".hidden is not true or false, left at false");
            }
        }
    }
    if (const json::Value* ord = root.get("vitals_order")) {
        BarOrder o{};
        bool ok = ord->is(json::Value::Type::Array) && ord->arr.size() == kBarCount;
        for (size_t i = 0; ok && i < kBarCount; ++i) {
            const json::Value& v = ord->arr[i];
            ok = false;
            if (!v.is(json::Value::Type::String)) break;
            for (int b = 0; b < static_cast<int>(kBarCount); ++b)
                if (same_name(v.s, bar_id(b))) {
                    o[i] = static_cast<uint8_t>(b);
                    ok = true;
                }
        }
        if (ok && valid_order(o))
            l.order = o;
        else
            r.notes.push_back("vitals_order is not health, mana and stamina once each, left at health, mana, stamina");
    }
    if (const json::Value* up = root.get("buffs_up")) {
        if (up->is(json::Value::Type::Bool))
            l.buffs_up = up->b;
        else
            r.notes.push_back("buffs_up is not true or false, left at false");
    }
    if (const json::Value* cv = root.get("colours")) {
        if (!cv->is(json::Value::Type::Object)) {
            r.notes.push_back("colours is not an object, skipped");
        } else {
            for (const auto& [key, val] : cv->obj) {
                if (same_name(key, "panel")) {
                    read_colour(val, "colours.panel", &l.panel, r.notes);
                    continue;
                }
                int bar = -1;
                for (int b = 0; b < static_cast<int>(kBarCount); ++b)
                    if (same_name(key, bar_id(b))) bar = b;
                if (bar < 0) {
                    r.notes.push_back("colours: unknown bar \"" + key + "\" skipped");
                    continue;
                }
                if (!val.is(json::Value::Type::Object)) {
                    r.notes.push_back("colours." + key + ": not an object, skipped");
                    continue;
                }
                for (const auto& [pk, pv] : val.obj) {
                    const std::optional<BarPart> part = find_bar_part(pk);
                    if (!part) {
                        r.notes.push_back("colours." + key + ": unknown part \"" + pk + "\" skipped");
                        continue;
                    }
                    read_colour(pv, "colours." + key + "." + pk,
                                &l.colours[static_cast<size_t>(bar)].part[static_cast<size_t>(*part)], r.notes);
                }
            }
        }
    }
    r.layout = std::move(l);
    return r;
}

}  // namespace emf::core::hud
