/** @file
 * @brief Implementation of the shared retained dashboard log. */

#include "dashboard_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void AppDashboardLog_Init(AppDashboardLog_t *log) {
    if (log != NULL) {
        memset(log, 0, sizeof(*log));
    }
}

void AppDashboardLog_Append(AppDashboardLog_t *log, const char *format, ...) {
    if (log == NULL || format == NULL) {
        return;
    }
    const bool following_tail = log->count <= 9U ||
        log->scroll_offset >= log->count - 9U;
    if (log->count == APP_DASHBOARD_LOG_CAPACITY) {
        memmove(log->lines, &log->lines[1],
                (APP_DASHBOARD_LOG_CAPACITY - 1U) * sizeof(log->lines[0]));
        log->count--;
        if (log->scroll_offset != 0U) {
            log->scroll_offset--;
        }
    }
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(log->lines[log->count], APP_DASHBOARD_LOG_LINE_SIZE,
                    format, arguments);
    va_end(arguments);
    log->count++;
    if (following_tail) {
        log->scroll_offset = log->count > 9U ? log->count - 9U : 0U;
    }
}

void AppDashboardLog_Draw(AppDashboardLog_t *log, Rectangle bounds, Font font,
                          float font_size, float line_height,
                          AppDashboardLogColor_t color_for_line) {
    if (log == NULL || line_height <= 0.0f) {
        return;
    }
    const size_t visible = (size_t)(bounds.height / line_height);
    const size_t maximum = log->count > visible ? log->count - visible : 0U;
    if (log->scroll_offset > maximum) {
        log->scroll_offset = maximum;
    }
    const Vector2 mouse = GetMousePosition();
    if (CheckCollisionPointRec(mouse, bounds)) {
        const float wheel = GetMouseWheelMove();
        if (wheel > 0.0f) {
            const size_t amount = (size_t)(wheel * 3.0f);
            log->scroll_offset = amount > log->scroll_offset
                ? 0U : log->scroll_offset - amount;
        } else if (wheel < 0.0f) {
            const size_t amount = (size_t)(-wheel * 3.0f);
            log->scroll_offset = log->scroll_offset + amount > maximum
                ? maximum : log->scroll_offset + amount;
        }
    }

    const Rectangle track = {bounds.x + bounds.width - 10.0f, bounds.y,
                             8.0f, bounds.height};
    if (maximum != 0U && CheckCollisionPointRec(mouse, track) &&
        (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
         IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
        const float position = (mouse.y - track.y) / track.height;
        const float clamped = position < 0.0f ? 0.0f : position > 1.0f ? 1.0f : position;
        log->scroll_offset = (size_t)(clamped * (float)maximum + 0.5f);
    }

    for (size_t row = 0U; row < visible; row++) {
        const size_t index = log->scroll_offset + row;
        if (index >= log->count) {
            break;
        }
        const Color color = color_for_line != NULL
            ? color_for_line(log->lines[index]) : (Color){190, 198, 210, 255};
        DrawTextEx(font, log->lines[index],
                   (Vector2){bounds.x, bounds.y + (float)row * line_height},
                   font_size, 0.0f, color);
    }

    DrawRectangleRounded(track, 0.5f, 6, (Color){31, 42, 60, 255});
    if (maximum != 0U) {
        const float fraction = (float)visible / (float)log->count;
        const float thumb_height = track.height * fraction < 24.0f
            ? 24.0f : track.height * fraction;
        const float travel = track.height - thumb_height;
        const float top = track.y + travel * (float)log->scroll_offset / (float)maximum;
        DrawRectangleRounded((Rectangle){track.x, top, track.width, thumb_height},
                             0.5f, 6, (Color){76, 148, 255, 255});
    }
}
