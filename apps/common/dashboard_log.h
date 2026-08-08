#ifndef APP_DASHBOARD_LOG_H
#define APP_DASHBOARD_LOG_H

/** @file
 * @brief Scrollable retained log shared by the native dashboards. */

#include <stddef.h>

#include "raylib.h"

#define APP_DASHBOARD_LOG_CAPACITY 256U
#define APP_DASHBOARD_LOG_LINE_SIZE 220U

/** Retained log text and its current scroll position. */
typedef struct {
    /** Oldest-to-newest retained lines. */
    char lines[APP_DASHBOARD_LOG_CAPACITY][APP_DASHBOARD_LOG_LINE_SIZE];
    /** Number of populated lines. */
    size_t count;
    /** Index of the first visible line. */
    size_t scroll_offset;
} AppDashboardLog_t;

/** Callback that selects a color for one retained log line. */
typedef Color (*AppDashboardLogColor_t)(const char *line);

/** Clear a retained dashboard log. */
void AppDashboardLog_Init(AppDashboardLog_t *log);
/** Append formatted text while preserving a user's scrolled position. */
void AppDashboardLog_Append(AppDashboardLog_t *log, const char *format, ...);
/** Draw retained lines with mouse-wheel and clickable-scrollbar navigation. */
void AppDashboardLog_Draw(AppDashboardLog_t *log, Rectangle bounds, Font font,
                          float font_size, float line_height,
                          AppDashboardLogColor_t color_for_line);

#endif
