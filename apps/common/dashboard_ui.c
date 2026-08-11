/** @file
 * @brief Shared native dashboard palette, controls, text, and clipboard fields. */
#include "dashboard_ui.h"

#include "dashboard_font.h"
#include "raygui.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const Color COLOR_CANVAS = {10, 15, 28, 255};
const Color COLOR_PANEL = {20, 28, 46, 255};
const Color COLOR_PANEL_RAISED = {27, 37, 59, 255};
const Color COLOR_BORDER = {49, 65, 92, 255};
const Color COLOR_TEXT = {232, 239, 249, 255};
const Color COLOR_MUTED = {143, 158, 181, 255};
const Color COLOR_CYAN = {45, 212, 191, 255};
const Color COLOR_BLUE = {76, 148, 255, 255};
const Color COLOR_PURPLE = {167, 139, 250, 255};

static AppDashboardFontSet_t fonts;

void AppDashboardUi_Init(const char *fallbackRegularFile,
                         const char *fallbackBoldFile) {
    const char *regularPath = TextFormat("%s%s", GetApplicationDirectory(),
                                         fallbackRegularFile);
    const char *boldPath = TextFormat("%s%s", GetApplicationDirectory(),
                                      fallbackBoldFile);
    AppDashboardFont_Load(&fonts, regularPath, boldPath);
    GuiSetFont(AppDashboardUi_Font());
    GuiSetStyle(DEFAULT, TEXT_SIZE, AppDashboardFont_RenderSize(15));
    GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
    GuiSetStyle(DEFAULT, TEXT_PADDING, 10);
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(COLOR_BORDER));
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt(COLOR_PANEL_RAISED));
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(COLOR_TEXT));
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(COLOR_CYAN));
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED, ColorToInt((Color){35, 50, 76, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(COLOR_BLUE));
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt((Color){42, 63, 96, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
    GuiSetStyle(DEFAULT, BORDER_COLOR_DISABLED, ColorToInt((Color){43, 52, 70, 255}));
    GuiSetStyle(DEFAULT, BASE_COLOR_DISABLED, ColorToInt((Color){24, 30, 43, 255}));
    GuiSetStyle(DEFAULT, TEXT_COLOR_DISABLED, ColorToInt((Color){91, 104, 124, 255}));
    GuiSetStyle(DROPDOWNBOX, BASE_COLOR_NORMAL, ColorToInt((Color){14, 22, 38, 255}));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_NORMAL, ColorToInt((Color){241, 246, 255, 255}));
    GuiSetStyle(DROPDOWNBOX, BASE_COLOR_FOCUSED, ColorToInt((Color){103, 232, 249, 255}));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_FOCUSED, ColorToInt((Color){7, 22, 28, 255}));
    GuiSetStyle(DROPDOWNBOX, BASE_COLOR_PRESSED, ColorToInt(COLOR_BLUE));
    GuiSetStyle(DROPDOWNBOX, TEXT_COLOR_PRESSED, ColorToInt(WHITE));
    GuiSetStyle(TEXTBOX, BORDER_WIDTH, 0);
    GuiSetStyle(TEXTBOX, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiSetStyle(TEXTBOX, TEXT_PADDING, 10);
    GuiSetStyle(TEXTBOX, BASE_COLOR_NORMAL, ColorToInt(BLANK));
    GuiSetStyle(TEXTBOX, BASE_COLOR_FOCUSED, ColorToInt(BLANK));
    GuiSetStyle(TEXTBOX, BASE_COLOR_PRESSED, ColorToInt(BLANK));
    GuiSetStyle(STATUSBAR, BORDER_WIDTH, 0);
    GuiSetStyle(STATUSBAR, TEXT_ALIGNMENT, TEXT_ALIGN_LEFT);
    GuiSetStyle(STATUSBAR, TEXT_PADDING, 10);
    GuiSetStyle(STATUSBAR, BASE_COLOR_NORMAL, ColorToInt(BLANK));
}

void AppDashboardUi_Shutdown(void) {
    AppDashboardFont_Unload(&fonts);
}

Font AppDashboardUi_Font(void) {
    return AppDashboardFont_Regular(&fonts, 15);
}

Font AppDashboardUi_RegularFont(int logicalSize) {
    return AppDashboardFont_Regular(&fonts, logicalSize);
}

int AppDashboardUi_RenderSize(int logicalSize) {
    return AppDashboardFont_RenderSize(logicalSize);
}

void AppDashboardUi_DrawText(const char *text, int x, int y,
                             int logicalSize, Color color) {
    const Font font = AppDashboardFont_Regular(&fonts, logicalSize);
    DrawTextEx(font, text, (Vector2){(float)x, (float)y},
               (float)AppDashboardFont_RenderSize(logicalSize), 0.0f, color);
}

void AppDashboardUi_DrawDisplayText(const char *text, int x, int y,
                                    int logicalSize, Color color) {
    const Font font = AppDashboardFont_Bold(&fonts, logicalSize);
    DrawTextEx(font, text, (Vector2){(float)x, (float)y},
               (float)AppDashboardFont_RenderSize(logicalSize), 0.5f, color);
}

void AppDashboardUi_DrawPanel(Rectangle bounds, const char *title) {
    DrawRectangleRounded(bounds, 0.08f, 10, COLOR_PANEL);
    DrawRectangleRoundedLinesEx(bounds, 0.08f, 10, 1.0f, COLOR_BORDER);
    if (title != NULL) {
        AppDashboardUi_DrawDisplayText(title, (int)bounds.x + 20,
                                       (int)bounds.y + 15, 14, COLOR_MUTED);
    }
}

bool AppDashboardUi_MetricCard(Rectangle bounds, const char *label,
                               const char *value, Color accent) {
    const bool hovered = CheckCollisionPointRec(GetMousePosition(), bounds);
    DrawRectangleRounded(bounds, 0.30f, 12, COLOR_PANEL_RAISED);
    if (hovered) {
        DrawRectangleRoundedLinesEx(bounds, 0.30f, 12, 1.0f, accent);
    }
    DrawRectangleRounded((Rectangle){bounds.x + 18.0f, bounds.y + 14.0f,
                                     4.0f, 55.0f}, 1.0f, 8, accent);
    AppDashboardUi_DrawDisplayText(label, (int)bounds.x + 34,
                                   (int)bounds.y + 14, 12, COLOR_MUTED);
    AppDashboardUi_DrawDisplayText(value, (int)bounds.x + 34,
                                   (int)bounds.y + 42, 22, COLOR_TEXT);
    return hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

bool AppDashboardUi_Button(Rectangle bounds, const char *label, bool enabled) {
    const bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    const bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    const Color fill = !enabled ? (Color){39, 49, 67, 255}
        : pressed ? (Color){34, 197, 181, 255}
        : hovered ? (Color){103, 232, 249, 255} : COLOR_CYAN;
    DrawRectangleRounded(bounds, 16.0f / bounds.height, 12, fill);
    const int fontSize = AppDashboardFont_RenderSize(16);
    AppDashboardFont_DrawCentered(
        AppDashboardFont_Bold(&fonts, 16), label, bounds, (float)fontSize,
        enabled ? (Color){7, 22, 28, 255} : (Color){94, 107, 127, 255});
    return enabled && hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

/** Draw the common field surface. */
static void drawFieldSurface(Rectangle bounds, bool focused) {
    DrawRectangleRounded(bounds, 16.0f / bounds.height, 12, COLOR_PANEL_RAISED);
    DrawRectangleRoundedLinesEx(bounds, 16.0f / bounds.height, 12, 1.0f,
                                focused ? COLOR_CYAN : COLOR_BORDER);
}

void AppDashboardUi_StatusField(Rectangle bounds, const char *text) {
    drawFieldSurface(bounds, false);
    GuiStatusBar((Rectangle){bounds.x + 1.0f, bounds.y + 1.0f,
                             bounds.width - 2.0f, bounds.height - 2.0f}, text);
}

bool AppDashboardUi_Toggle(Rectangle bounds, const char *label,
                           bool *value, bool enabled) {
    const bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    const Rectangle track = {bounds.x, bounds.y + 2.0f, 38.0f, 20.0f};
    const Color trackColor = *value && enabled ? COLOR_CYAN
        : hovered ? (Color){50, 72, 111, 255} : (Color){37, 52, 81, 255};
    DrawRectangleRounded(track, 1.0f, 12, trackColor);
    DrawCircle((int)(track.x + (*value ? 29.0f : 9.0f)),
               (int)(track.y + 10.0f), 8.0f,
               *value && enabled ? (Color){7, 22, 28, 255} : COLOR_MUTED);
    AppDashboardUi_DrawText(label, (int)bounds.x + 46, (int)bounds.y + 2, 15,
                            enabled ? COLOR_TEXT : (Color){94, 107, 127, 255});
    if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        *value = !*value;
        return true;
    }
    return false;
}

/** Return whether the platform clipboard modifier is held. */
static bool clipboardModifierDown(void) {
#if defined(__APPLE__)
    return IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
}

/** Replace a field with printable clipboard text. */
static void pasteClipboardText(char *text, size_t capacity) {
    const char *clipboard = GetClipboardText();
    if (clipboard == NULL || capacity == 0U) return;
    size_t output = 0U;
    for (size_t input = 0U; clipboard[input] != '\0' && output + 1U < capacity;
         ++input) {
        const unsigned char value = (unsigned char)clipboard[input];
        if (value >= 0x20U && value != 0x7FU) text[output++] = (char)value;
    }
    text[output] = '\0';
}

void AppDashboardUi_TextBox(Rectangle bounds, char *text, size_t capacity,
                            bool *editing) {
    const bool modifier = *editing && clipboardModifierDown();
    const bool copy = modifier && IsKeyPressed(KEY_C);
    const bool cut = modifier && IsKeyPressed(KEY_X);
    const bool paste = modifier && IsKeyPressed(KEY_V);
    const bool shortcut = copy || cut || paste;
    if (copy || cut) SetClipboardText(text);
    if (cut) text[0] = '\0';
    else if (paste) pasteClipboardText(text, capacity);
    drawFieldSurface(bounds, *editing);
    const Rectangle content = {bounds.x + 1.0f, bounds.y + 1.0f,
                               bounds.width - 2.0f, bounds.height - 2.0f};
    if (GuiTextBox(content, text, (int)capacity, shortcut ? false : *editing)) {
        *editing = !*editing;
    }
}

bool AppDashboardUi_NumberField(Rectangle bounds, char *text, size_t capacity,
                                bool *editing,
                                AppDashboardNumberDrag_t *drag,
                                uint32_t step, uint32_t maximum) {
    AppDashboardUi_TextBox(bounds, text, capacity, editing);
    const Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(mouse, bounds)) {
        char *end = NULL;
        const unsigned long parsed = strtoul(text, &end, 10);
        drag->startY = mouse.y;
        drag->startValue = end != text && *end == '\0' && parsed <= maximum
            ? (uint32_t)parsed : 0U;
        drag->active = true;
        drag->moved = false;
    }
    if (drag->active && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const int pixels = (int)(drag->startY - mouse.y);
        if (pixels >= 3 || pixels <= -3) {
            const int64_t increments = pixels / 3;
            int64_t value = (int64_t)drag->startValue +
                increments * (int64_t)step;
            if (value < 0) value = 0;
            if ((uint64_t)value > maximum) value = maximum;
            (void)snprintf(text, capacity, "%u", (uint32_t)value);
            *editing = false;
            drag->moved = true;
        }
    }
    if (drag->active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        drag->active = false;
        if (drag->moved) *editing = false;
    }
    return drag->moved;
}

void AppDashboardUi_TabFocus(bool **editingFields, size_t fieldCount) {
    if (fieldCount == 0U || !IsKeyPressed(KEY_TAB)) return;
    size_t current = fieldCount;
    for (size_t index = 0U; index < fieldCount; index++) {
        if (*editingFields[index]) {
            current = index;
            break;
        }
    }
    const bool reverse = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const size_t target = current == fieldCount
        ? (reverse ? fieldCount - 1U : 0U)
        : reverse ? (current + fieldCount - 1U) % fieldCount
                  : (current + 1U) % fieldCount;
    for (size_t index = 0U; index < fieldCount; index++) {
        *editingFields[index] = index == target;
    }
}
