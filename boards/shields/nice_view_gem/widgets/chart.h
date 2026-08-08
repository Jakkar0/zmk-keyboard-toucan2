#pragma once

#include <lvgl.h>
#include "util.h"

/* Despite the name, this widget draws the WPM-driven bongo cat, not a chart.
 * The file kept its name so screen.c and CMakeLists.txt stay untouched; see
 * chart.c for the animation model. */

struct chart_status_state {
    uint8_t wpm;
};

void draw_chart_status(lv_obj_t *canvas, const struct status_state *state);
