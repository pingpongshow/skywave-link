// Modern look for the ImGui front end: embedded fonts, dark/light themes and
// semantic colours.

#include "skywave_link/modern_style.hpp"

#include <cstdio>

// Generated at configure time from the TTFs (see CMakeLists.txt). A size of
// zero means the font was not available when the binary was built.
extern const unsigned char kEmbeddedFontUi[];
extern const unsigned int kEmbeddedFontUiSize;
extern const unsigned char kEmbeddedFontMono[];
extern const unsigned int kEmbeddedFontMonoSize;

namespace skylink::ui {

namespace {

ImFont* g_mono_font = nullptr;
Palette g_palette{};

constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

ImVec4 withAlpha(ImVec4 c, float a) {
    c.w = a;
    return c;
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImFont* addEmbeddedFont(const unsigned char* data, unsigned int size, const char* name) {
    if (size == 0) return nullptr;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // static data, the atlas must not free it
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    // ImGui 1.92 bakes glyphs on demand at the framebuffer scale of the
    // viewport, so one entry covers every size and Retina density.
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(data), static_cast<int>(size), 15.0f, &cfg);
}

void setDarkColors(ImGuiStyle& style) {
    ImVec4* c = style.Colors;
    const ImVec4 accent = rgb(77, 139, 245);
    const ImVec4 bg = rgb(23, 26, 32);
    const ImVec4 panel = rgb(26, 29, 35);
    const ImVec4 raised = rgb(34, 38, 46);
    const ImVec4 border = rgb(45, 50, 60);

    c[ImGuiCol_Text] = rgb(228, 231, 236);
    c[ImGuiCol_TextDisabled] = rgb(124, 131, 145);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = withAlpha(rgb(30, 33, 40), 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = raised;
    c[ImGuiCol_FrameBgHovered] = rgb(42, 47, 57);
    c[ImGuiCol_FrameBgActive] = rgb(50, 56, 68);
    c[ImGuiCol_TitleBg] = rgb(22, 24, 30);
    c[ImGuiCol_TitleBgActive] = rgb(30, 34, 42);
    c[ImGuiCol_TitleBgCollapsed] = rgb(22, 24, 30);
    c[ImGuiCol_MenuBarBg] = rgb(23, 26, 32);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(58, 64, 76);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(74, 81, 95);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = rgb(110, 162, 250);
    c[ImGuiCol_Button] = rgb(40, 52, 74);
    c[ImGuiCol_ButtonHovered] = rgb(56, 92, 158);
    c[ImGuiCol_ButtonActive] = rgb(62, 115, 210);
    c[ImGuiCol_Header] = withAlpha(accent, 0.24f);
    c[ImGuiCol_HeaderHovered] = withAlpha(accent, 0.34f);
    c[ImGuiCol_HeaderActive] = withAlpha(accent, 0.46f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = withAlpha(accent, 0.70f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = withAlpha(accent, 0.18f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accent, 0.55f);
    c[ImGuiCol_ResizeGripActive] = withAlpha(accent, 0.85f);
    c[ImGuiCol_InputTextCursor] = c[ImGuiCol_Text];
    c[ImGuiCol_Tab] = raised;
    c[ImGuiCol_TabHovered] = rgb(56, 92, 158);
    c[ImGuiCol_TabSelected] = rgb(40, 52, 74);
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = panel;
    c[ImGuiCol_TabDimmedSelected] = raised;
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.5f);
    c[ImGuiCol_PlotLines] = rgb(140, 180, 250);
    c[ImGuiCol_PlotLinesHovered] = rgb(250, 170, 90);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = rgb(110, 162, 250);
    c[ImGuiCol_TableHeaderBg] = raised;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = rgb(38, 42, 51);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextLink] = rgb(110, 162, 250);
    c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.35f);
    c[ImGuiCol_TreeLines] = border;
    c[ImGuiCol_DragDropTarget] = rgb(250, 190, 80);
    c[ImGuiCol_DragDropTargetBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_UnsavedMarker] = c[ImGuiCol_Text];
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1, 1, 1, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.45f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

    g_palette.app_bg = rgb(17, 19, 23);
    g_palette.accent = accent;
    g_palette.success = rgb(64, 186, 110);
    g_palette.danger = rgb(229, 83, 75);
    g_palette.warning = rgb(242, 178, 60);
    g_palette.info = rgb(86, 196, 230);
    g_palette.muted = rgb(140, 147, 160);
    g_palette.heading = rgb(150, 170, 205);
}

void setLightColors(ImGuiStyle& style) {
    ImVec4* c = style.Colors;
    const ImVec4 accent = rgb(37, 99, 235);
    const ImVec4 bg = rgb(250, 250, 252);
    const ImVec4 panel = rgb(255, 255, 255);
    const ImVec4 raised = rgb(234, 236, 240);
    const ImVec4 border = rgb(214, 218, 225);

    c[ImGuiCol_Text] = rgb(28, 31, 36);
    c[ImGuiCol_TextDisabled] = rgb(135, 141, 152);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = withAlpha(rgb(255, 255, 255), 0.99f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(227, 230, 235);
    c[ImGuiCol_FrameBgHovered] = rgb(218, 222, 229);
    c[ImGuiCol_FrameBgActive] = rgb(208, 214, 224);
    c[ImGuiCol_TitleBg] = rgb(232, 234, 238);
    c[ImGuiCol_TitleBgActive] = rgb(220, 225, 233);
    c[ImGuiCol_TitleBgCollapsed] = rgb(232, 234, 238);
    c[ImGuiCol_MenuBarBg] = rgb(236, 238, 242);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(196, 201, 210);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(170, 177, 189);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = rgb(29, 78, 216);
    c[ImGuiCol_Button] = rgb(222, 231, 248);
    c[ImGuiCol_ButtonHovered] = rgb(200, 216, 246);
    c[ImGuiCol_ButtonActive] = rgb(176, 199, 242);
    c[ImGuiCol_Header] = withAlpha(accent, 0.14f);
    c[ImGuiCol_HeaderHovered] = withAlpha(accent, 0.22f);
    c[ImGuiCol_HeaderActive] = withAlpha(accent, 0.32f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = withAlpha(accent, 0.70f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = withAlpha(accent, 0.15f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(accent, 0.45f);
    c[ImGuiCol_ResizeGripActive] = withAlpha(accent, 0.75f);
    c[ImGuiCol_InputTextCursor] = c[ImGuiCol_Text];
    c[ImGuiCol_Tab] = raised;
    c[ImGuiCol_TabHovered] = rgb(200, 216, 246);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = raised;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.5f);
    c[ImGuiCol_PlotLines] = rgb(37, 99, 235);
    c[ImGuiCol_PlotLinesHovered] = rgb(220, 110, 30);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = rgb(29, 78, 216);
    c[ImGuiCol_TableHeaderBg] = raised;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = rgb(228, 231, 236);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0.025f);
    c[ImGuiCol_TextLink] = accent;
    c[ImGuiCol_TextSelectedBg] = withAlpha(accent, 0.25f);
    c[ImGuiCol_TreeLines] = border;
    c[ImGuiCol_DragDropTarget] = rgb(220, 140, 20);
    c[ImGuiCol_DragDropTargetBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_UnsavedMarker] = c[ImGuiCol_Text];
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(accent, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);

    g_palette.app_bg = rgb(240, 242, 245);
    g_palette.accent = accent;
    g_palette.success = rgb(22, 140, 70);
    g_palette.danger = rgb(205, 50, 45);
    g_palette.warning = rgb(180, 110, 0);
    g_palette.info = rgb(0, 120, 170);
    g_palette.muted = rgb(105, 112, 125);
    g_palette.heading = rgb(70, 90, 130);
}

} // namespace

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFont* ui_font = addEmbeddedFont(kEmbeddedFontUi, kEmbeddedFontUiSize, "Roboto Medium");
    if (!ui_font) {
        ui_font = io.Fonts->AddFontDefault();
    }
    io.FontDefault = ui_font;
    g_mono_font = addEmbeddedFont(kEmbeddedFontMono, kEmbeddedFontMonoSize, "Cousine");
    if (!g_mono_font) {
        g_mono_font = ui_font;
    }
}

ImFont* monoFont() {
    return g_mono_font;
}

void applyModernStyle(bool dark, float font_size) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();  // start from known metrics so re-applying is idempotent

    if (font_size < 8.0f) font_size = 15.0f;
    style.FontSizeBase = font_size;
    style._NextFrameFontSizeBase = font_size;  // survives a mid-frame theme switch

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 12.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.SeparatorTextBorderSize = 1.0f;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;

    style.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(0.0f, 2.0f);
    style.DisplaySafeAreaPadding = ImVec2(4.0f, 4.0f);

    if (dark) {
        setDarkColors(style);
    } else {
        setLightColors(style);
    }
}

const Palette& palette() {
    return g_palette;
}

ImVec4 readableTextOn(const ImVec4& bg) {
    const float luminance = 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z;
    return luminance > 0.7f ? ImVec4(0.08f, 0.09f, 0.11f, 1.0f) : ImVec4(1, 1, 1, 1);
}

void PushButtonColor(const ImVec4& base) {
    const ImVec4 white(1, 1, 1, base.w);
    const ImVec4 black(0, 0, 0, base.w);
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(base, white, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(base, black, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_Text, readableTextOn(base));
}

void PopButtonColor() {
    ImGui::PopStyleColor(4);
}

void SectionHeader(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, g_palette.heading);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

float statusBarHeight() {
    return ImGui::GetTextLineHeight() + 2.0f * 6.0f + 2.0f;
}

ImVec4 statusColor(StatusKind kind) {
    switch (kind) {
        case StatusKind::Success: return g_palette.success;
        case StatusKind::Warning: return g_palette.warning;
        case StatusKind::Error: return g_palette.danger;
        case StatusKind::Info:
        default: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

void StatusLine(WindowStatus& status) {
    if (status.empty()) {
        return;
    }
    ImGui::PushID(&status);
    if (ImGui::SmallButton("x")) {
        status = WindowStatus{};
        ImGui::PopID();
        return;
    }
    ImGui::SetItemTooltip("Dismiss");
    ImGui::SameLine();
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - status.posted_at).count();
    ImGui::PushStyleColor(ImGuiCol_Text, statusColor(status.kind));
    ImGui::TextWrapped("%s", status.text.c_str());
    ImGui::PopStyleColor();
    if (age >= 5 && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%llds ago", static_cast<long long>(age));
    }
    ImGui::PopID();
}

void WarningText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, g_palette.warning);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

} // namespace skylink::ui
