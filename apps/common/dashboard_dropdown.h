#ifndef APP_DASHBOARD_DROPDOWN_H
#define APP_DASHBOARD_DROPDOWN_H

/** @file
 * @brief High-contrast dropdown control shared by the native dashboards. */

#include <stdbool.h>
#include <stddef.h>

#include "raylib.h"

/** Draw a dropdown whose options open above, or below when top space is insufficient.
 *
 * This placement keeps selectors near the bottom of a fixed dashboard inside
 * the drawable window at every supported window size.
 *
 * @param bounds Collapsed control bounds.
 * @param font Font used for the selected value and menu items.
 * @param items Array of human-readable option labels.
 * @param item_count Number of labels in @p items.
 * @param selected Selected item index, updated after a choice.
 * @param expanded Whether the option menu is currently visible.
 * @return `true` when the selected item changed.
 */
bool AppDashboard_UpwardDropdown(Rectangle bounds, Font font,
                                 const char *const items[], size_t item_count,
                                 int *selected, bool *expanded);

#endif
