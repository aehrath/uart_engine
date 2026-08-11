/** @file
 * @brief Shared native dashboard palette, controls, text, and clipboard fields. */
#ifndef APP_DASHBOARD_UI_H
#define APP_DASHBOARD_UI_H

#include "raylib.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Pointer-drag state retained by one numeric field. */
typedef struct {
    float startY;
    uint32_t startValue;
    bool active;
    bool moved;
} AppDashboardNumberDrag_t;

extern const Color COLOR_CANVAS;
extern const Color COLOR_PANEL;
extern const Color COLOR_PANEL_RAISED;
extern const Color COLOR_BORDER;
extern const Color COLOR_TEXT;
extern const Color COLOR_MUTED;
extern const Color COLOR_CYAN;
extern const Color COLOR_BLUE;
extern const Color COLOR_PURPLE;

/** Initialize fonts and the shared raygui style from bundled fallback names. */
void AppDashboardUi_Init(const char *fallbackRegularFile,
                         const char *fallbackBoldFile);

/** Release all dashboard font atlases. */
void AppDashboardUi_Shutdown(void);

/** Return the regular 15-pixel logical UI font. */
Font AppDashboardUi_Font(void);

/** Return an exact-size regular UI font. */
Font AppDashboardUi_RegularFont(int logicalSize);

/** Convert a logical Python/Tk font size into its Raylib render size. */
int AppDashboardUi_RenderSize(int logicalSize);

/** Draw regular dashboard text. */
void AppDashboardUi_DrawText(const char *text, int x, int y,
                             int logicalSize, Color color);

/** Draw bold dashboard text. */
void AppDashboardUi_DrawDisplayText(const char *text, int x, int y,
                                    int logicalSize, Color color);

/** Draw one rounded dashboard panel. */
void AppDashboardUi_DrawPanel(Rectangle bounds, const char *title);

/** Draw one clickable telemetry card. */
bool AppDashboardUi_MetricCard(Rectangle bounds, const char *label,
                               const char *value, Color accent);

/** Draw one shared action button. */
bool AppDashboardUi_Button(Rectangle bounds, const char *label, bool enabled);

/** Draw one non-editable, left-aligned status field. */
void AppDashboardUi_StatusField(Rectangle bounds, const char *text);

/** Draw one rounded toggle and update its value when clicked. */
bool AppDashboardUi_Toggle(Rectangle bounds, const char *label,
                           bool *value, bool enabled);

/** Draw one editable rounded field with whole-field clipboard shortcuts. */
void AppDashboardUi_TextBox(Rectangle bounds, char *text, size_t capacity,
                            bool *editing);

/** Draw an editable numeric field that also changes when dragged vertically. */
bool AppDashboardUi_NumberField(Rectangle bounds, char *text, size_t capacity,
                                bool *editing,
                                AppDashboardNumberDrag_t *drag,
                                uint32_t step, uint32_t maximum);

/** Move keyboard focus through a group of editable fields on Tab/Shift-Tab. */
void AppDashboardUi_TabFocus(bool **editingFields, size_t fieldCount);

#endif
