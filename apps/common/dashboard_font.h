/** @file
 * @brief Platform-matched native dashboard font loading. */
#ifndef APP_DASHBOARD_FONT_H
#define APP_DASHBOARD_FONT_H

#include "raylib.h"

#include <stdbool.h>

/** Exact-size native font atlases used by one dashboard. */
typedef struct {
    Font regular12;
    Font regular15;
    Font regular18;
    Font bold12;
    Font bold14;
    Font bold16;
    Font bold22;
    Font bold28;
    bool owned[8];
} AppDashboardFontSet_t;

/** Load the same exact-size UI faces selected by the Python dashboard.
 *
 * @param fonts Receives all regular and bold pixel-size atlases.
 * @param fallbackRegular Bundled regular font used if the platform face is unavailable.
 * @param fallbackBold Bundled bold font used if the platform face is unavailable.
 */
void AppDashboardFont_Load(AppDashboardFontSet_t *fonts,
                           const char *fallbackRegular, const char *fallbackBold);

/** Unload every owned atlas in a font set. */
void AppDashboardFont_Unload(AppDashboardFontSet_t *fonts);

/** Select an exact-size regular atlas, falling back to the 15-pixel face. */
Font AppDashboardFont_Regular(const AppDashboardFontSet_t *fonts, int size);

/** Select an exact-size bold atlas, falling back to the 16-pixel face. */
Font AppDashboardFont_Bold(const AppDashboardFontSet_t *fonts, int size);

/** Convert a Tk em-pixel size to the equivalent Raylib line-box pixel size. */
int AppDashboardFont_RenderSize(int pythonPixels);

/** Draw one text label centered by its visible glyph bounds.
 *
 * @param font Loaded font atlas.
 * @param text Single-line UTF-8 label.
 * @param bounds Rectangle in which the visible glyphs are centered.
 * @param fontSize Raylib rendering size in pixels.
 * @param color Text color.
 */
void AppDashboardFont_DrawCentered(Font font, const char *text,
                                   Rectangle bounds, float fontSize,
                                   Color color);

#endif
