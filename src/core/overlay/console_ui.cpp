#include "console_ui.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

namespace emf::core {
namespace {

constexpr size_t kInputCap = 1024;

ImVec4 colour(LineKind k) {
    switch (k) {
        case LineKind::Command: return ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
        case LineKind::Error: return ImVec4(1.00f, 0.48f, 0.45f, 1.0f);
        case LineKind::Success: return ImVec4(0.49f, 0.91f, 0.53f, 1.0f);
        default: return ImVec4(0.70f, 0.73f, 0.76f, 1.0f);
    }
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

void set_buffer(ImGuiInputTextCallbackData* d, const std::string& text) {
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, text.c_str());
}

int input_callback(ImGuiInputTextCallbackData* d) {
    auto* c = static_cast<CallbackCtx*>(d->UserData);
    ConsoleUiState& ui = *c->ui;
    switch (d->EventFlag) {
        case ImGuiInputTextFlags_CallbackCompletion: {
            const std::string current(d->Buf, d->BufTextLen);
            const bool cycling = !ui.completions.empty() && ui.completion_index < ui.completions.size() &&
                                 current == ui.completions[ui.completion_index];
            if (cycling) {
                ui.completion_index = (ui.completion_index + 1) % ui.completions.size();
                set_buffer(d, ui.completions[ui.completion_index]);
                break;
            }
            ui.completions = complete(current, *c->sources);
            ui.completion_index = 0;
            if (ui.completions.empty()) break;
            if (ui.completions.size() == 1) {
                set_buffer(d, ui.completions[0] + " ");
                ui.completions.clear();
                break;
            }
            const std::string prefix = common_prefix(ui.completions);
            if (prefix.size() > current.size()) {
                set_buffer(d, prefix);  // extend to what all candidates share; the list stays shown and the next
                                        // Tab (prefix != completions[0]) starts cycling at the first candidate
            } else {
                set_buffer(d, ui.completions[0]);
            }
            break;
        }
        case ImGuiInputTextFlags_CallbackHistory: {
            const std::string line =
                d->EventKey == ImGuiKey_UpArrow ? c->model->history_prev() : c->model->history_next();
            set_buffer(d, line);
            ui.completions.clear();
            break;
        }
        case ImGuiInputTextFlags_CallbackEdit:
            ui.completions.clear();  // typing ends a Tab cycle
            break;
        default: break;
    }
    return 0;
}

}  // namespace

void draw_console(ConsoleModel& model, ConsoleUiState& ui, const CompletionSources& sources, ImVec2 display,
                  const std::function<void(const std::string&)>& submit, const std::function<void()>& mods_tab) {
    if (ui.open_fraction <= 0.0f) return;
    const float height = (std::max)(display.y * 0.5f * (std::min)(ui.open_fraction, 1.0f), 1.0f);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(display.x, height));
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.055f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::Begin("##emf_console", nullptr, flags);

    if (mods_tab) {
        ImGuiIO& tio = ImGui::GetIO();
        if (tio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) ui.switch_tab = true;
        const int want = ui.switch_tab ? 1 - ui.tab : -1;
        ui.switch_tab = false;
        if (ImGui::BeginTabBar("##emf_tabs")) {
            if (ImGui::BeginTabItem("Console", nullptr, want == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ui.tab != 0) ui.focus_input = true;
                ui.tab = 0;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Mods", nullptr, want == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ui.tab = 1;
                ImGui::EndTabItem();
            }
            if (!ui.status.empty())  // a label on the right, not a tab (a click does nothing)
                ImGui::TabItemButton(ui.status.c_str(), ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
            ImGui::EndTabBar();
        }
        if (ui.tab == 1) {
            mods_tab();
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }
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
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(count), line_h);
    while (clipper.Step()) {
        const auto lines = model.snapshot(static_cast<size_t>(clipper.DisplayStart),
                                          static_cast<size_t>(clipper.DisplayEnd - clipper.DisplayStart));
        for (const auto& [kind, text] : lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, colour(kind));
            ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
            ImGui::PopStyleColor();
        }
    }
    const uint64_t version = model.version();
    if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - line_h) {
        if (io.MouseWheel > 0.0f || ImGui::IsKeyPressed(ImGuiKey_PageUp)) ui.follow = false;
    } else {
        ui.follow = true;
    }
    const float view_h = ImGui::GetWindowHeight();  // a resize moves the bottom too
    if (ui.follow && (version != ui.seen_version || view_h != ui.seen_height)) ImGui::SetScrollHereY(1.0f);
    ui.seen_version = version;
    ui.seen_height = view_h;
    ImGui::EndChild();

    // Input line.
    char buf[kInputCap];
    const size_t n = (std::min)(ui.input.size(), kInputCap - 1);
    std::memcpy(buf, ui.input.data(), n);
    buf[n] = '\0';
    ImGui::PushItemWidth(-1.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    if (ui.focus_input) {
        ImGui::SetKeyboardFocusHere();
        ui.focus_input = false;
    }
    CallbackCtx ctx{&model, &ui, &sources};
    const ImGuiInputTextFlags in_flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                                         ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit;
    const bool entered = ImGui::InputText("##input", buf, kInputCap, in_flags, input_callback, &ctx);
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();
    ui.input = buf;
    if (entered) {
        std::string line = ui.input;
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) {
            model.push_history(line);
            submit(line);
        }
        ui.input.clear();
        ui.completions.clear();
        ui.follow = true;
        ui.focus_input = true;
    }
    if (!entered && !ImGui::IsAnyItemActive())
        ui.focus_input = true;  // keep typing going to the input line while the console is open (also after a click)
    const ImVec2 under = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // Completion list under the panel.
    if (ui.completions.size() > 1) {
        ImGui::SetNextWindowPos(under);
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
    if (version <= toast.from_version) return {};
    const uint64_t fresh = version - toast.from_version;
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

void draw_toast(const ConsoleLines& lines, ImVec2 display) {
    if (lines.empty()) return;
    const float pad = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(pad, pad));
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.055f, 1.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::Begin("##emf_toast", nullptr, flags);
    ImGui::PushTextWrapPos(display.x * 0.6f);
    for (const auto& [kind, text] : lines) {
        ImGui::PushStyleColor(ImGuiCol_Text, colour(kind));
        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleColor();
}

void draw_hud(const std::vector<HudLine>& lines, ImVec2 display) {
    if (lines.empty()) return;
    const float pad = ImGui::GetFontSize();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    static const char* const kNames[] = {"##emf_hud_top_right", "##emf_hud_bottom_right", "##emf_hud_bottom_left"};
    for (int place = 0; place < 3; ++place) {
        bool any = false;
        for (const auto& l : lines) any |= static_cast<int>(l.place) == place;
        if (!any) continue;
        // the game's own HUD keeps the top left, the top centre and the bottom centre; its quest tracker sits in the
        // top right (seen in game), so top-right lines start below it
        const ImVec2 pos = place == 0 ? ImVec2(display.x - pad, display.y * 0.24f)
                         : place == 1 ? ImVec2(display.x - pad, display.y - pad)
                                      : ImVec2(pad, display.y - pad);
        const ImVec2 pivot = place == 0 ? ImVec2(1.0f, 0.0f) : place == 1 ? ImVec2(1.0f, 1.0f) : ImVec2(0.0f, 1.0f);
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.055f, 1.0f));
        ImGui::Begin(kNames[place], nullptr, flags);
        for (const auto& l : lines) {
            if (static_cast<int>(l.place) != place) continue;
            const ImVec4 c = l.rgba ? ImVec4(((l.rgba >> 24) & 0xFF) / 255.0f, ((l.rgba >> 16) & 0xFF) / 255.0f,
                                             ((l.rgba >> 8) & 0xFF) / 255.0f, (l.rgba & 0xFF) / 255.0f)
                                    : ImVec4(0.86f, 0.86f, 0.80f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::TextUnformatted(l.text.c_str(), l.text.c_str() + l.text.size());
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }
}

}  // namespace emf::core
