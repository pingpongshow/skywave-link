// Modern look for the ImGui front end: embedded fonts, dark/light themes and
// a few semantic colours shared by the windows.
#pragma once

#include "imgui.h"
#include "window_base.hpp"

namespace skylink::ui {

// Semantic colours for the active theme. Windows use these instead of
// hard-coded RGB so both themes stay readable.
struct Palette {
    ImVec4 app_bg;        // main window background (floating windows sit above it)
    ImVec4 accent;        // selection, primary buttons, focus
    ImVec4 success;       // RX, OK, connected
    ImVec4 danger;        // TX, broadcast, errors
    ImVec4 warning;       // pending, attention
    ImVec4 info;          // secondary highlight (gateways, outgoing calls)
    ImVec4 muted;         // secondary text
    ImVec4 heading;       // section headers
};

// Loads the embedded UI and monospace fonts into the current context's
// atlas. Call once after ImGui::CreateContext(). Falls back to ImGui's
// built-in font when the binary was built without the TTFs.
void loadFonts();

// Font for fixed-width columns (SNR rows, logs). Never null after
// loadFonts(); it is the UI font when no monospace font is embedded.
ImFont* monoFont();

// Replaces the current style with the modern dark or light theme.
// font_size is the base text size in points (display-scale independent;
// ImGui rasterises at the framebuffer scale, so text stays sharp on HiDPI).
void applyModernStyle(bool dark, float font_size);

const Palette& palette();

// Text colour that keeps contrast on the given background colour.
ImVec4 readableTextOn(const ImVec4& bg);

// Button tinted with `base` (hover/active derived). Pair with PopButtonColor().
void PushButtonColor(const ImVec4& base);
void PopButtonColor();

// Small uppercase-style section label followed by a separator.
void SectionHeader(const char* label);

// Height reserved for the main window's status bar.
float statusBarHeight();

// Palette colour for a status kind (Info uses the normal text colour).
ImVec4 statusColor(StatusKind kind);

// A window's status line: the last result in its colour, with a dismiss
// button. Draws nothing while the status is empty.
void StatusLine(WindowStatus& status);

// Wrapped text in the warning colour, for conditions the operator must
// notice (dry-run radio, no voice media, unsaved settings).
void WarningText(const char* text);

} // namespace skylink::ui
