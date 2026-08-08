/** @file
 * @brief Implementation of the shared upward-opening dashboard dropdown. */

#include "dashboard_dropdown.h"

/** Draw centered text inside one dropdown row. */
static void drawCentered(Font font, const char *text, Rectangle bounds,
                         float font_size, Color color) {
    const Vector2 size = MeasureTextEx(font, text, font_size, 0.0f);
    DrawTextEx(font, text,
               (Vector2){bounds.x + (bounds.width - size.x) * 0.5f,
                         bounds.y + (bounds.height - size.y) * 0.5f - 1.0f},
               font_size, 0.0f, color);
}

bool AppDashboard_UpwardDropdown(Rectangle bounds, Font font,
                                 const char *const items[], size_t item_count,
                                 int *selected, bool *expanded) {
    if (items == NULL || item_count == 0U || selected == NULL || expanded == NULL) {
        return false;
    }
    if (*selected < 0 || (size_t)*selected >= item_count) {
        *selected = 0;
    }

    const Color field = {27, 37, 59, 255};
    const Color field_hover = {35, 50, 76, 255};
    const Color popup = {8, 14, 27, 255};
    const Color row = {20, 28, 46, 255};
    const Color hover = {103, 232, 249, 255};
    const Color chosen = {76, 148, 255, 255};
    const Color text = {241, 246, 255, 255};
    const Color dark_text = {7, 22, 28, 255};
    const Color border = {94, 234, 212, 255};
    const Vector2 mouse = GetMousePosition();
    const bool field_hovered = CheckCollisionPointRec(mouse, bounds);
    bool changed = false;

    const float row_height = 34.0f;
    const float gap = 6.0f;
    const Rectangle popup_bounds = {
        bounds.x,
        bounds.y - (float)item_count * row_height - gap,
        bounds.width,
        (float)item_count * row_height,
    };

    if (*expanded) {
        DrawRectangleRounded(popup_bounds, 0.06f, 8, popup);
        DrawRectangleRoundedLinesEx(popup_bounds, 0.06f, 8, 1.0f, border);
        for (size_t index = 0U; index < item_count; index++) {
            const Rectangle item_bounds = {
                popup_bounds.x + 3.0f,
                popup_bounds.y + 2.0f + (float)index * row_height,
                popup_bounds.width - 6.0f,
                row_height - 4.0f,
            };
            const bool hovered = CheckCollisionPointRec(mouse, item_bounds);
            const bool is_selected = index == (size_t)*selected;
            DrawRectangleRounded(item_bounds, 0.18f, 6,
                                 hovered ? hover : is_selected ? chosen : row);
            drawCentered(font, items[index], item_bounds, 16.0f,
                         hovered ? dark_text : text);
            if (hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                changed = *selected != (int)index;
                *selected = (int)index;
                *expanded = false;
            }
        }
    }

    DrawRectangleRounded(bounds, 0.22f, 8,
                         field_hovered ? field_hover : field);
    DrawRectangleRoundedLinesEx(bounds, 0.22f, 8, 1.0f,
                                field_hovered || *expanded ? border
                                                          : (Color){64, 85, 116, 255});
    drawCentered(font, items[*selected],
                 (Rectangle){bounds.x, bounds.y, bounds.width - 24.0f, bounds.height},
                 16.0f, text);
    const float arrow_x = bounds.x + bounds.width - 15.0f;
    const float arrow_y = bounds.y + bounds.height * 0.5f;
    DrawTriangle((Vector2){arrow_x - 5.0f, arrow_y + 3.0f},
                 (Vector2){arrow_x + 5.0f, arrow_y + 3.0f},
                 (Vector2){arrow_x, arrow_y - 4.0f}, text);

    if (field_hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        *expanded = !*expanded;
    } else if (*expanded && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
               !CheckCollisionPointRec(mouse, popup_bounds) && !field_hovered) {
        *expanded = false;
    }
    return changed;
}
