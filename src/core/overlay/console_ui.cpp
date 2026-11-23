#include "console_ui.h"

#include "core/hud_editor/elements.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

#include "imgui_internal.h"  // the input line's sideways scroll (ImGuiInputTextState)

namespace emf::core {
namespace {

constexpr size_t kInputCap = 1024;

// The suggestions' colours: names light, argument hints a little dimmer, help grey, the argument being typed gold.
const ImVec4 kSuggestName(0.86f, 0.88f, 0.90f, 1.0f);
const ImVec4 kSuggestPicked(1.00f, 1.00f, 1.00f, 1.0f);
const ImVec4 kSuggestHint(0.66f, 0.71f, 0.77f, 1.0f);
const ImVec4 kSuggestHelp(0.52f, 0.55f, 0.59f, 1.0f);
const ImVec4 kSuggestCurrent(0.75f, 0.69f, 0.45f, 1.0f);
const ImVec4 kSuggestRowBack(0.75f, 0.69f, 0.45f, 0.22f);
const ImVec4 kGhost(0.52f, 0.55f, 0.59f, 1.0f);

ImVec4 piece_colour(HintPiece::Kind k) {
    switch (k) {
        case HintPiece::Kind::Command: return kSuggestPicked;
        case HintPiece::Kind::Current: return kSuggestCurrent;
        case HintPiece::Kind::Help: return kSuggestHelp;
        default: return kSuggestHint;
    }
}

void text(const std::string& s, const ImVec4& c) {
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextUnformatted(s.c_str(), s.c_str() + s.size());
    ImGui::PopStyleColor();
}

// The hint line and the matching rows in a box under the input line (no input of its own). Returns the box's
// bottom edge (`under.y` when nothing shows).
float draw_suggestions(const Suggester& g, ImVec2 under, ImVec2 display) {
    const Suggestions& s = g.current();
    const bool rows = g.showing();
    if (!rows && s.hint.empty()) return under.y;
    ImGui::SetNextWindowPos(under);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2((std::max)(display.x - 2.0f * under.x, 100.0f), FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##emf_suggest", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (!s.hint.empty()) {  // "give <item name> [count]: drop items at your feet (host)", the current argument gold
        for (size_t i = 0; i < s.hint.size(); ++i) {
            if (i) ImGui::SameLine(0.0f, 0.0f);
            const ImVec4 c = piece_colour(s.hint[i].kind);
            text(s.hint[i].text, c);
            if (s.hint[i].kind == HintPiece::Kind::Current) {  // underlined too (one font: no bold)
                const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                draw->AddLine(ImVec2(a.x, b.y), b, ImGui::GetColorU32(c));
            }
        }
        if (rows) ImGui::Separator();
    }
    if (rows) {  // three columns: the name or value, the command's arguments, the help in grey
        float name_w = 0.0f, hint_w = 0.0f;
        for (const auto& r : s.rows) {
            name_w = (std::max)(name_w, ImGui::CalcTextSize(r.label.c_str()).x);
            hint_w = (std::max)(hint_w, ImGui::CalcTextSize(r.hint.c_str()).x);
        }
        const float gap = ImGui::GetFontSize();
        const float x0 = ImGui::GetCursorPosX();
        const float x_hint = x0 + name_w + gap;
        const float x_help = hint_w > 0.0f ? x_hint + hint_w + gap : x_hint;
        for (size_t i = 0; i < s.rows.size(); ++i) {
            const SuggestRow& r = s.rows[i];
            const bool picked = i == g.selected();
            ImGui::SetCursorPosX(x0);
            if (picked) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float w = x_help - x0 + ImGui::CalcTextSize(r.help.c_str()).x;
                draw->AddRectFilled(ImVec2(p.x - 2.0f, p.y), ImVec2(p.x + w + 2.0f, p.y + ImGui::GetTextLineHeight()),
                                    ImGui::GetColorU32(kSuggestRowBack));
            }
            text(r.label, picked ? kSuggestPicked : kSuggestName);
            if (!r.hint.empty()) {
                ImGui::SameLine(x_hint);
                text(r.hint, kSuggestHint);
            }
            if (!r.help.empty()) {
                ImGui::SameLine(x_help);
                text(r.help, kSuggestHelp);
            }
        }
    }
    const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
    ImGui::End();
    return bottom;
}

std::string common_prefix(const std::vector<std::string>& v) {
    if (v.empty()) return {};
    std::string p = v[0];
    for (const auto& s : v) {
        size_t i = 0;
        while (i < p.size() && i < s.size() && std::tolower(static_cast<unsigned char>(p[i])) ==
                                                    std::tolower(static_cast<unsigned char>(s[i])))
            ++i;
        p.resize(i);
    }
    return p;
}

struct CallbackCtx {
    ConsoleModel* model;
    ConsoleUiState* ui;
    const CompletionSources* sources;
};

// The input line's room in bytes (its buffer also holds the closing '\0').
size_t room(const ImGuiInputTextCallbackData* d) { return d->BufSize > 0 ? static_cast<size_t>(d->BufSize - 1) : 0; }

// Puts `text` in the input line; false, the line unchanged, when it does not fit (ImGui's InsertChars would drop a
// text too long after the old line was deleted, and leave the line empty).
bool set_buffer(ImGuiInputTextCallbackData* d, const std::string& text) {
    if (text.size() > room(d)) return false;
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, text.c_str());
    return true;
}

void set_line(ImGuiInputTextCallbackData* d, const LineEdit& e) {
    if (!set_buffer(d, e.text)) return;
    d->CursorPos = d->SelectionStart = d->SelectionEnd =
        static_cast<int>((std::min)(e.caret, static_cast<size_t>(d->BufTextLen)));
}

// Called every frame the input line is active (CallbackAlways), or for the frame's one event (Tab, Up/Down, an edit).
int input_callback(ImGuiInputTextCallbackData* d) {
    auto* c = static_cast<CallbackCtx*>(d->UserData);
    ConsoleUiState& ui = *c->ui;
    if (ui.caret_to_end) {
        ui.caret_to_end = false;
        d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen;
    }
    if (ui.suggest)  // the suggestions follow the line and the caret (they recompute only when either changed)
        ui.suggester.update(std::string_view(d->Buf, static_cast<size_t>(d->BufTextLen)),
                            static_cast<size_t>(d->CursorPos), *c->sources);
    switch (d->EventFlag) {
        case ImGuiInputTextFlags_CallbackCompletion: {
            if (ui.suggest && ui.suggester.showing()) {  // Tab takes the highlighted suggestion
                if (const auto e = ui.suggester.accept(*c->sources, room(d))) set_line(d, *e);  // (not when it
                ui.completions.clear();                                                         // would not fit)
                break;
            }
            // no list shows (nothing suggested, hidden by Esc or a line from the history, or suggestions off):
            // complete and cycle through the matches
            const std::string current(d->Buf, d->BufTextLen);
            // a line from the cycle is not the player's edit: the suggestions stay hidden, so the cycle's list is the
            // only one and the next Tab goes on cycling
            const auto cycle_to = [&](const std::string& line) {
                if (set_buffer(d, line)) ui.suggester.replaced(line, line.size());
            };
            const bool cycling = !ui.completions.empty() && ui.completion_index < ui.completions.size() &&
                                 current == ui.completions[ui.completion_index];
            if (cycling) {
                ui.completion_index = (ui.completion_index + 1) % ui.completions.size();
                cycle_to(ui.completions[ui.completion_index]);
                break;
            }
            ui.completions = complete(current, *c->sources);
            ui.completion_index = 0;
            if (ui.completions.empty()) break;
            if (ui.completions.size() == 1) {  // done: the next argument's suggestions may show
                set_buffer(d, ui.completions[0] + " ");
                ui.completions.clear();
                break;
            }
            const std::string prefix = common_prefix(ui.completions);
            if (prefix.size() > current.size()) {
                cycle_to(prefix);  // extend to what all candidates share; the list stays shown and the next
                                   // Tab (prefix != completions[0]) starts cycling at the first candidate
            } else {
                cycle_to(ui.completions[0]);
            }
            break;
        }
        case ImGuiInputTextFlags_CallbackHistory: {
            const bool up = d->EventKey == ImGuiKey_UpArrow;
            if (ui.suggest && ui.suggester.move(up ? -1 : +1)) break;  // in the list while it shows
            const std::string line = up ? c->model->history_prev() : c->model->history_next();
            set_buffer(d, line);
            ui.completions.clear();
            ui.suggester.replaced(line, line.size());  // no list for a line from the history: Up/Down stay its
            break;
        }
        case ImGuiInputTextFlags_CallbackEdit:
            ui.completions.clear();  // typing ends a Tab cycle
            break;
        default:
            if (ui.suggest && ui.accept_right)  // Right at the end of the line takes the suggestion
                if (const auto e = ui.suggester.accept_right(*c->sources, room(d))) set_line(d, *e);
            break;
    }
    ui.caret = static_cast<size_t>(d->CursorPos);
    return 0;
}

}  // namespace

ImVec4 line_colour(const ConsoleUiState& ui, LineKind kind) {
    switch (kind) {
        case LineKind::Command: return ui.colour_command;
        case LineKind::Error: return ui.colour_error;
        case LineKind::Success: return ui.colour_success;
        default: return ui.colour_reply;
    }
}

int next_tab(int tab, bool has_mods, bool has_settings) {
    for (int step = 1; step <= 3; ++step) {
        const int t = (tab + step) % 3;
        if (t == 0 || (t == 1 && has_mods) || (t == 2 && has_settings)) return t;
    }
    return 0;
}

void draw_look_preview(const TextLook& look, float width) {
    ImFont* font = look.bold ? look.bold : look.font ? look.font : ImGui::GetFont();
    const float size = font->FontSize;
    const char* const hud = "12:30  A line on screen";
    const char* const note = "A note looks like this";
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = std::floor(size * 0.5f);
    const ImVec2 note_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, note);
    const float h = pad * 3.0f + size * 2.0f;
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + h), IM_COL32(40, 48, 40, 255));  // a stand-in for the world
    const ImVec2 note_at(at.x + (width - note_size.x) * 0.5f, at.y + pad);
    if (look.style == HudStyle::Game) {
        NoteBand band;
        band.min = ImVec2(note_at.x - size * 2.0f, note_at.y - pad * 0.5f);
        band.max = ImVec2(note_at.x + note_size.x + size * 2.0f, note_at.y + size + pad * 0.5f);
        band.fade_left = band.fade_right = size * 2.0f;
        band.text_min = note_at;
        band.text_max = ImVec2(note_at.x + note_size.x, note_at.y + size);
        draw_note_band(dl, band);
    }
    draw_look_text(dl, look, font, size, note_at, look.colour, note, note + std::strlen(note));
    const ImVec2 hud_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, hud);
    draw_look_text(dl, look, font, size, ImVec2(at.x + width - hud_size.x - pad, at.y + pad * 2.0f + size), look.colour,
                   hud, hud + std::strlen(hud));
    ImGui::Dummy(ImVec2(width, h));
}

void draw_console(ConsoleModel& model, ConsoleUiState& ui, const CompletionSources& sources, ImVec2 display,
                  const std::function<void(const std::string&)>& submit, const std::function<void()>& mods_tab,
                  const std::function<void()>& settings_tab) {
    ui.wants_escape = false;  // set again below while the suggestions show
    ui.ghost_shown.clear();   // and the ghost text while it draws
    if (ui.open_fraction <= 0.0f) {
        ui.escape = false;  // an Esc while closed is not the list's: it must not hide it at the next opening
        return;
    }
    const float height = (std::max)(display.y * ui.height * (std::min)(ui.open_fraction, 1.0f), 1.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(display.x, height));
    ImGui::SetNextWindowBgAlpha(ui.opacity);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui.background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##emf_console", nullptr, flags);

    if (mods_tab || settings_tab) {
        ImGuiIO& tio = ImGui::GetIO();
        if (tio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) ui.switch_tab = true;
        const int want = ui.open_tab >= 0 ? ui.open_tab
                                          : (ui.switch_tab ? next_tab(ui.tab, bool(mods_tab), bool(settings_tab)) : -1);
        ui.open_tab = -1;
        ui.switch_tab = false;
        if (ui.tabs_font) ImGui::PushFont(ui.tabs_font);  // the tab bar and the Mods tab ([overlay] tabs_font)
        if (ImGui::BeginTabBar("##emf_tabs")) {
            if (ImGui::BeginTabItem("Console", nullptr, want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ui.tab != 0) ui.focus_input = true;
                ui.tab = 0;
                ImGui::EndTabItem();
            }
            if (mods_tab && ImGui::BeginTabItem("Mods", nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ui.tab = 1;
                ImGui::EndTabItem();
            }
            if (settings_tab &&
                ImGui::BeginTabItem("Settings", nullptr, want == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ui.tab = 2;
                ImGui::EndTabItem();
            }
            if (!ui.status.empty())  // a label on the right, not a tab (a click does nothing)
                ImGui::TabItemButton(ui.status.c_str(), ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
            ImGui::EndTabBar();
        }
        if (ui.tab == 1 || ui.tab == 2) {
            ui.escape = false;
            if (ui.tab == 1 && mods_tab) mods_tab();
            if (ui.tab == 2 && settings_tab) settings_tab();
            if (ui.tabs_font) ImGui::PopFont();
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }
        if (ui.tabs_font) ImGui::PopFont();
    }

    // Scrollback.
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##scrollback", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    ImGuiIO& io = ImGui::GetIO();
    // the wheel scrolls the scrollback wherever the cursor is (ImGui scrolls it itself when the cursor is over it)
    if (io.MouseWheel != 0.0f && !ImGui::IsWindowHovered()) ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * line_h * 3.0f);
    const float page = ImGui::GetWindowHeight() * 0.9f;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(ImGui::GetScrollY() - page);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(ImGui::GetScrollY() + page);
    const size_t count = model.size();
    const uint64_t version = model.version();
    if (ui.find_open && (version != ui.find_seen_version || ui.find_text != ui.find_seen_text)) {
        const bool new_text = ui.find_text != ui.find_seen_text;
        ui.find_matches = model.find_all(ui.find_text);  // old lines drop off the front: indices move, so again
        ui.find_seen_version = version;
        ui.find_seen_text = ui.find_text;
        if (new_text || ui.find_current >= ui.find_matches.size()) {  // typing jumps to the newest match
            ui.find_current = ui.find_matches.empty() ? 0 : ui.find_matches.size() - 1;
            ui.find_jump = !ui.find_matches.empty();
        }
    }
    const bool marking = ui.find_open && !ui.find_matches.empty();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(count), line_h);
    while (clipper.Step()) {
        const auto lines = model.lines(static_cast<size_t>(clipper.DisplayStart),
                                       static_cast<size_t>(clipper.DisplayEnd - clipper.DisplayStart));
        size_t index = static_cast<size_t>(clipper.DisplayStart);
        for (const auto& line : lines) {
            if (marking && std::binary_search(ui.find_matches.begin(), ui.find_matches.end(), index)) {
                const bool current = ui.find_matches[ui.find_current] == index;  // a dim band, the current one brighter
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + line_h),
                    ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, current ? 0.16f : 0.07f)));
            }
            if (ui.timestamps) {
                const std::string stamp = ConsoleModel::stamp_text(line.seconds);
                ImVec4 dim = ui.colour_reply;
                dim.w *= 0.55f;
                ImGui::PushStyleColor(ImGuiCol_Text, dim);
                ImGui::TextUnformatted(stamp.c_str(), stamp.c_str() + stamp.size());
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, line_colour(ui, line.kind));
            ImGui::TextUnformatted(line.text.c_str(), line.text.c_str() + line.text.size());
            ImGui::PopStyleColor();
            ++index;
        }
    }
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - line_h) {
        if (io.MouseWheel > 0.0f || ImGui::IsKeyPressed(ImGuiKey_PageUp)) ui.follow = false;
    } else if (!ui.find_jump && !(ui.find_open && !ui.find_matches.empty() && !ui.follow)) {
        ui.follow = true;  // at the bottom (not while the view rests on a match found: it moves there next frame)
    }
    const float view_h = ImGui::GetWindowHeight();  // a resize moves the bottom too
    if (ui.find_jump && ui.find_current < ui.find_matches.size()) {
        // after the follow check, which still sees this frame's scroll (SetScrollY applies next frame): the jump wins
        ui.find_jump = false;
        ui.follow = false;
        ImGui::SetScrollY((std::max)(0.0f, static_cast<float>(ui.find_matches[ui.find_current]) * line_h -
                                               ImGui::GetWindowHeight() * 0.5f));
    }
    if (ui.follow && (version != ui.seen_version || view_h != ui.seen_height)) ImGui::SetScrollHereY(1.0f);
    ui.seen_version = version;
    ui.seen_height = view_h;
    ImGui::EndChild();

    // Input line.
    if (ui.escape) {  // Esc while the find line or the suggestions showed: it closes / they hide (the next Esc closes
        ui.escape = false;  // the console)
        if (ui.find_open) {
            ui.find_open = false;
            ui.find_focus = false;
            ui.focus_input = true;
        } else {
            ui.suggester.escape();
        }
    }
    // Ctrl+F opens the find line (or goes back to it); Ctrl+Shift+C copies, as the Copy button
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        ui.find_open = true;
        ui.find_focus = true;
        ui.find_typing = true;
    }
    const bool copy_key = io.KeyCtrl && io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C, false);
    char buf[kInputCap];
    const size_t n = (std::min)(ui.input.size(), kInputCap - 1);
    std::memcpy(buf, ui.input.data(), n);
    buf[n] = '\0';
    const ImGuiStyle& footer_style = ImGui::GetStyle();
    const float button_w = ImGui::CalcTextSize("Find").x + ImGui::CalcTextSize("Copy").x +
                           footer_style.FramePadding.x * 4.0f + footer_style.ItemSpacing.x * 2.0f;
    const float find_w = ui.find_open ? (std::max)(160.0f, ImGui::GetContentRegionAvail().x * 0.25f) : 0.0f;
    const float count_w = ui.find_open ? ImGui::CalcTextSize("00000 of 00000").x + footer_style.ItemSpacing.x * 3.0f : 0.0f;
    ImGui::PushItemWidth(-(button_w + find_w + count_w));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    if (ui.find_focus) ui.focus_input = false;  // the find line asked for the keys
    if (ui.focus_input) {
        if (ui.find_typing) ui.caret_to_end = true;  // back from the find line: keep what was typed
        ui.find_typing = false;
        ImGui::SetKeyboardFocusHere();
        ui.focus_input = false;
    }
    CallbackCtx ctx{&model, &ui, &sources};
    ImGuiInputTextFlags in_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                                   ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit;
    if (ui.suggest || ui.caret_to_end) in_flags |= ImGuiInputTextFlags_CallbackAlways;  // the caret, every frame the line
                                                                                         // is active
    // A new press of Right with the caret at the end of the line (last frame's caret: the press cannot move it
    // further). Not its auto-repeats: holding Right to walk to the end of the line must not take a row there.
    ui.accept_right = ui.suggest && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) && !io.KeyCtrl && !io.KeyShift &&
                      !io.KeyAlt && ui.caret == ui.input.size();
    const bool entered = ImGui::InputText("##input", buf, kInputCap, in_flags, input_callback, &ctx);
    const ImVec2 input_lo = ImGui::GetItemRectMin(), input_hi = ImGui::GetItemRectMax();
    const bool input_active = ImGui::IsItemActive();
    const ImGuiID input_id = ImGui::GetItemID();
    ui.accept_right = false;
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();
    ui.input = buf;
    // The find line: Enter goes to the next older match, Shift+Enter to the next newer one.
    if (ui.find_open) {
        ImGui::SameLine();
        char fbuf[256];
        const size_t fn = (std::min)(ui.find_text.size(), sizeof(fbuf) - 1);
        std::memcpy(fbuf, ui.find_text.data(), fn);
        fbuf[fn] = '\0';
        ImGui::PushItemWidth(find_w);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
        if (ui.find_focus) {
            ImGui::SetKeyboardFocusHere();
            ui.find_focus = false;
        }
        const bool find_entered =
            ImGui::InputTextWithHint("##find", "find", fbuf, sizeof(fbuf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemActive()) ui.find_typing = true;
        if (find_entered) {
            ui.find_focus = true;  // Enter leaves the line: back to it for the next one
            if (!ui.find_matches.empty()) {
                const size_t n_matches = ui.find_matches.size();
                ui.find_current =
                    io.KeyShift ? (ui.find_current + 1) % n_matches : (ui.find_current + n_matches - 1) % n_matches;
                ui.find_jump = true;
            }
        }
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();
        ui.find_text = fbuf;
        ImGui::SameLine();
        const std::string counter = ui.find_text.empty() ? std::string()
                                    : ui.find_matches.empty()
                                        ? std::string("no match")
                                        : std::to_string(ui.find_current + 1) + " of " + std::to_string(ui.find_matches.size());
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", counter.c_str());
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Dummy(ImVec2((std::max)(0.0f, count_w - ImGui::CalcTextSize(counter.c_str()).x - footer_style.ItemSpacing.x), 0));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Find")) {
        ui.find_open = !ui.find_open;
        ui.find_focus = ui.find_open;
        ui.find_typing = ui.find_open;
        if (!ui.find_open) ui.focus_input = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy") || copy_key) {  // the scrollback (the matching lines while the find line finds some)
        std::string text;
        if (ui.find_open && !ui.find_matches.empty()) {
            for (size_t i : ui.find_matches) {
                if (!text.empty()) text += "\r\n";
                text += model.text(i, 1, ui.timestamps);
            }
        } else {
            text = model.text(0, model.size(), ui.timestamps);
        }
        ImGui::SetClipboardText(text.c_str());
    }
    if (ui.suggest && !entered && input_active) {  // grey ghost text completes the top match after the caret
        const std::string ghost = ui.suggester.ghost();
        // not while the line is scrolled sideways (also after it got short again): the text is not where it would go
        const ImGuiInputTextState* st = ImGui::GetInputTextState(input_id);
        if (!ghost.empty() && st && st->Scroll.x == 0.0f) {
            const ImGuiStyle& style = ImGui::GetStyle();
            const ImVec2 lo = input_lo, hi = input_hi;
            const float x = lo.x + style.FramePadding.x + ImGui::CalcTextSize(ui.input.c_str()).x;
            if (x + ImGui::CalcTextSize(ghost.c_str()).x <= hi.x - style.FramePadding.x) {  // only while nothing scrolls
                ImGui::GetWindowDrawList()->AddText(ImVec2(x, lo.y + style.FramePadding.y), ImGui::GetColorU32(kGhost),
                                                    ghost.c_str());
                ui.ghost_shown = ghost;
            }
        }
    }
    if (entered) {
        std::string line = ui.input;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) {
            model.push_history(line);
            submit(line);
        }
        ui.input.clear();
        ui.completions.clear();
        ui.suggester.clear();
        ui.caret = 0;
        ui.follow = true;
        ui.focus_input = true;
    }
    if (!entered && !ImGui::IsAnyItemActive()) {  // keep typing going to the line that had the keys (also after a click)
        if (ui.find_open && ui.find_typing)
            ui.find_focus = true;
        else
            ui.focus_input = true;
    }
    const ImVec2 under = ImVec2(input_lo.x, input_hi.y + 2.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Suggestions as you type under the panel; the next Esc is theirs while rows show.
    float below = under.y;
    if (ui.suggest) {
        below = draw_suggestions(ui.suggester, under, display);
        ui.wants_escape = ui.suggester.showing();
        if (ui.wants_escape) ui.completions.clear();  // one list at a time: rows that show end a Tab cycle
    }
    if (ui.find_open) ui.wants_escape = true;  // the next Esc closes the find line
    // The Tab cycle's list under the panel (under the suggestions' hint line).
    if (ui.completions.size() > 1) {
        ImGui::SetNextWindowPos(ImVec2(under.x, below > under.y ? below + 2.0f : under.y));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::Begin("##emf_completions", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
        for (size_t i = 0; i < ui.completions.size(); ++i) {
            const bool current = i == ui.completion_index;
            ImGui::PushStyleColor(ImGuiCol_Text, current ? ImVec4(1, 1, 1, 1) : ImVec4(0.70f, 0.73f, 0.76f, 1));
            ImGui::TextUnformatted(ui.completions[i].c_str());
            ImGui::PopStyleColor();
        }
        ImGui::End();
    }
}

ConsoleLines toast_lines(const ConsoleModel& model, const ToastState& toast, uint64_t now_ms, size_t max) {
    if (now_ms >= toast.until_ms) return {};
    const uint64_t version = model.version();
    const uint64_t from = (std::max)(toast.from_version, model.cleared_version());  // lines from before a clear are gone
    if (version <= from) return {};
    const uint64_t fresh = version - from;
    const size_t size = model.size();
    if (fresh <= max && fresh <= size) return model.snapshot(size - static_cast<size_t>(fresh), static_cast<size_t>(fresh));
    if (max < 2) return model.snapshot(size - (std::min)(size, max), (std::min)(size, max));
    // More than fits: the newest lines, after a note saying how many are left out (e.g. a long /help).
    const size_t keep = (std::min)(max - 1, size);
    ConsoleLines out{{LineKind::Normal, "... " + std::to_string(fresh - keep) + " more lines in the console"}};
    auto tail = model.snapshot(size - keep, keep);
    out.insert(out.end(), std::make_move_iterator(tail.begin()), std::make_move_iterator(tail.end()));
    return out;
}

namespace {

// A note line's colour: errors red, successes green, the echoed command white, everything else the look's colour.
uint32_t note_rgba(LineKind k, uint32_t normal) {
    switch (k) {
        case LineKind::Command: return 0xFFFFFFFFu;
        case LineKind::Error: return 0xFF7A73FFu;
        case LineKind::Success: return 0x7DE887FFu;
        default: return normal;
    }
}

constexpr ImGuiWindowFlags kBoxFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
const ImVec4 kBoxBack(0.03f, 0.04f, 0.055f, 1.0f);  // EMF's earlier boxes

// style=boxed: the notes in a dark box (EMF's earlier look) at the notes' place.
void draw_notes_boxed(const ConsoleLines& lines, ImVec2 display, const TextLook& look) {
    if (look.font) ImGui::PushFont(look.font);
    const float pad = ImGui::GetFontSize();
    const NotesPlace place = look.notes_place;
    const ImVec2 pos = place == NotesPlace::TopLeft ? ImVec2(pad, pad)
                     : place == NotesPlace::Right   ? ImVec2(display.x - pad, display.y * 0.40f)
                                                    : ImVec2(display.x * 0.5f, display.y * 0.075f);
    const ImVec2 pivot = place == NotesPlace::TopLeft ? ImVec2(0, 0) : place == NotesPlace::Right ? ImVec2(1, 0) : ImVec2(0.5f, 0);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBoxBack);
    ImGui::Begin("##emf_toast", nullptr, kBoxFlags);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + note_wrap_width(place, display));
    for (const auto& [kind, text] : lines) {
        ImGui::PushStyleColor(ImGuiCol_Text, look_colour4(note_rgba(kind, look.colour)));
        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor();
    if (look.font) ImGui::PopFont();
}

// The label of the HUD part of the mods' lines at a place (the HUD page's sample line).
const char* lines_label(int place) {
    const int e = hud::lines_element(place);
    return e < 0 ? "" : hud::hud_elements()[static_cast<size_t>(e)].label;
}

// A colour at half its alpha (the samples).
uint32_t dimmed(uint32_t rgba) { return (rgba & 0xFFFFFF00u) | ((rgba & 0xFFu) / 2u); }

// style=boxed: one small window per place (EMF's earlier look).
void draw_hud_boxed(const std::vector<HudLine>& lines, ImVec2 display, const TextLook& look, const HudLinesView& view) {
    ImFont* font = look.bold ? look.bold : look.font;  // HUD lines are bold (with font=console: the console's font)
    if (font) ImGui::PushFont(font);
    const float pad = ImGui::GetFontSize();
    static const char* const kNames[] = {"##emf_hud_top_right", "##emf_hud_bottom_right", "##emf_hud_bottom_left"};
    for (int place = 0; place < 3; ++place) {
        const LinesEdit& ed = view.edits[static_cast<size_t>(place)];
        if (ed.hidden) continue;
        bool any = false;
        for (const auto& l : lines) any |= static_cast<int>(l.place) == place;
        if (!any && !view.samples) continue;
        // the game's own HUD keeps the top left, the top centre and the bottom centre; its quest tracker sits in the
        // top right (seen in game), so top-right lines start below it
        const ImVec2 pos = place == 0 ? ImVec2(display.x - pad, display.y * 0.24f)
                         : place == 1 ? ImVec2(display.x - pad, display.y - pad)
                                      : ImVec2(pad, display.y - pad);
        const ImVec2 pivot = place == 0 ? ImVec2(1.0f, 0.0f) : place == 1 ? ImVec2(1.0f, 1.0f) : ImVec2(0.0f, 1.0f);
        ImGui::SetNextWindowPos(ImVec2(pos.x + ed.offset.x, pos.y + ed.offset.y), ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, kBoxBack);
        // behind the console (the HUD page shows them): a new window is otherwise made on top
        ImGui::Begin(kNames[place], nullptr, kBoxFlags | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::SetWindowFontScale(ed.scale);
        if (!any) {
            ImGui::PushStyleColor(ImGuiCol_Text, look_colour4(dimmed(look.colour)));
            ImGui::TextUnformatted(lines_label(place));
            ImGui::PopStyleColor();
        }
        for (const auto& l : lines) {
            if (static_cast<int>(l.place) != place) continue;
            ImGui::PushStyleColor(ImGuiCol_Text, look_colour4(l.rgba ? l.rgba : look.colour));
            ImGui::TextUnformatted(l.text.c_str(), l.text.c_str() + l.text.size());
            ImGui::PopStyleColor();
        }
        if (view.rects) {
            const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
            (*view.rects)[static_cast<size_t>(place)] = ImVec4(p.x, p.y, s.x, s.y);
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
    if (font) ImGui::PopFont();
}

}  // namespace

void draw_notes(const ConsoleLines& lines, ImVec2 display, const TextLook& look) {
    if (lines.empty()) return;
    if (look.style == HudStyle::Boxed) {
        draw_notes_boxed(lines, display, look);
        return;
    }
    // like the game's banners: bold, on a band that fades at its ends (game style), under the compass by default; each
    // note wraps at half the screen's width into at most kNoteMaxRows rows, and the band holds at most kNoteBandRows
    // rows (the newest)
    ImFont* font = look.bold ? look.bold : look.font ? look.font : ImGui::GetFont();
    const float size = font->FontSize;
    const float wrap = note_wrap_width(look.notes_place, display);
    const float gap = std::floor(size * 0.15f);
    const auto measure = [font, size](const std::string& s) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str(), s.c_str() + s.size()).x;
    };
    constexpr size_t kNoteBandRows = 8;
    struct Row {
        std::string text;
        uint32_t rgba;
        float width;
    };
    std::vector<Row> rows;
    for (const auto& [kind, text] : lines)
        for (auto& r : wrap_rows(text, wrap, measure)) {
            const float w = measure(r);
            rows.push_back({std::move(r), note_rgba(kind, look.colour), w});
        }
    if (rows.empty()) return;
    if (rows.size() > kNoteBandRows) rows.erase(rows.begin(), rows.end() - kNoteBandRows);
    ImVec2 block(0.0f, size * static_cast<float>(rows.size()) + gap * static_cast<float>(rows.size() - 1));
    for (const Row& r : rows) block.x = (std::max)(block.x, r.width);
    const NoteBand band = note_band(look.notes_place, block, size, display);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (look.style == HudStyle::Game) draw_note_band(dl, band);
    float y = band.text_min.y;
    for (const Row& r : rows) {
        draw_look_text(dl, look, font, size, ImVec2(note_line_x(look.notes_place, band, r.width), y), r.rgba,
                       r.text.c_str(), r.text.c_str() + r.text.size());
        y += size + gap;
    }
}

void draw_hud(const std::vector<HudLine>& lines, ImVec2 display, const TextLook& look, const HudLinesView& view) {
    if (view.rects) view.rects->fill(std::nullopt);
    if (lines.empty() && !view.samples) return;
    if (look.style == HudStyle::Boxed) {
        draw_hud_boxed(lines, display, look, view);
        return;
    }
    // the game's own lines are bold
    ImFont* font = look.bold ? look.bold : look.font ? look.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const HudPlace place : {HudPlace::TopRight, HudPlace::BottomRight, HudPlace::BottomLeft}) {
        const LinesEdit& ed = view.edits[static_cast<size_t>(place)];
        if (ed.hidden) continue;
        const float size = font->FontSize * ed.scale;
        const HudLine* here[HudModel::kMaxLines];  // drawn every frame: no allocation
        size_t n = 0;
        for (const auto& l : lines)
            if (l.place == place && n < HudModel::kMaxLines) here[n++] = &l;
        ImVec2 lo(FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX);
        auto grow = [&lo, &hi](ImVec2 at, ImVec2 s) {
            lo = ImVec2((std::min)(lo.x, at.x), (std::min)(lo.y, at.y));
            hi = ImVec2((std::max)(hi.x, at.x + s.x), (std::max)(hi.y, at.y + s.y));
        };
        if (n == 0 && view.samples) {
            const char* label = lines_label(static_cast<int>(place));
            const ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
            ImVec2 at = hud_line_position(place, 0, 1, s, size, display);
            at = ImVec2(at.x + ed.offset.x, at.y + ed.offset.y);
            draw_look_text(dl, look, font, size, at, dimmed(look.colour), label);
            grow(at, s);
        }
        for (size_t i = 0; i < n; ++i) {
            char text[HudModel::kMaxText + 1];  // one line: a line break becomes a space
            const size_t len = (std::min)(here[i]->text.size(), HudModel::kMaxText);
            for (size_t k = 0; k < len; ++k) {
                const char c = here[i]->text[k];
                text[k] = c == '\n' || c == '\r' ? ' ' : c;
            }
            text[len] = '\0';
            const ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text, text + len);
            ImVec2 at = hud_line_position(place, i, n, s, size, display);
            at = ImVec2(at.x + ed.offset.x, at.y + ed.offset.y);
            draw_look_text(dl, look, font, size, at, here[i]->rgba ? here[i]->rgba : look.colour, text, text + len);
            grow(at, s);
        }
        if (view.rects && hi.x > lo.x)
            (*view.rects)[static_cast<size_t>(place)] = ImVec4(lo.x, lo.y, hi.x - lo.x, hi.y - lo.y);
    }
}

}  // namespace emf::core
