#include <zephyr/kernel.h>

#include "chart.h"
#include "../assets/bongo_frames.h"

/* sleep.c is only compiled for non-split/central builds (see CMakeLists.txt),
 * but chart.c is compiled unconditionally, so mirror that guard here. */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include "sleep.h"
#else
static inline bool is_sleep_screen_active(void) { return false; }
#endif

/*
 * Bongo cat, driven by WPM.
 *
 * ZMK raises zmk_wpm_state_changed only about once a second, far too coarse to
 * animate from, so that event just selects a band and an LVGL timer steps the
 * frames.
 *
 * Threading: screen.c's activity handler calls lv_task_handler() directly from
 * the ZMK event thread on the sleep transition, so a tick can run there rather
 * than on the display work queue. Pre-existing behaviour, benign here only
 * because WPM has decayed to 0 and the timer is paused long before the 60
 * minute sleep timeout fires.
 *
 * Cost: LVGL's lv_canvas_draw_* helpers invalidate the whole canvas object, so
 * each tick flushes the full 144x168 buffer to the panel (~24ms of SPI at
 * 1MHz), not just the frame rect. Keep the periods modest.
 */

/* Singleton: sized for the one screen widget custom_status_screen.c creates. */
static lv_obj_t *bongo_canvas;
static lv_timer_t *bongo_timer;
static uint8_t frame_idx;
static uint8_t last_wpm;

static const lv_img_dsc_t *const idle_frames[] = {&idle_img1, &idle_img2, &idle_img3, &idle_img4,
                                                  &idle_img5};
static const lv_img_dsc_t *const slow_frames[] = {&slow_img};
static const lv_img_dsc_t *const fast_frames[] = {&fast_img1, &fast_img2};

/*
 * Ascending WPM bands, last row is the catch-all. period_ms 0 holds frames[0]
 * with no timer running at all.
 *
 * Thresholds follow the upstream widget (idle < 30 WPM, slow < 60, fast above)
 * and the slow band holds one frame because upstream only ships one slow_img.
 * The rest band is ours: ZMK's WPM decays gradually rather than snapping to 0,
 * so keying off `wpm == 0` left the timer ticking for seconds after the user
 * stopped typing, animating for nobody.
 */
static const struct bongo_band {
    uint8_t max_wpm;
    const lv_img_dsc_t *const *frames;
    uint8_t n_frames;
    uint16_t period_ms;
} bands[] = {
    {5, idle_frames, 1, 0},
    {30, idle_frames, ARRAY_SIZE(idle_frames), 200},
    {60, slow_frames, ARRAY_SIZE(slow_frames), 0},
    {0, fast_frames, ARRAY_SIZE(fast_frames), 120}, /* catch-all, max_wpm unused */
};

static const struct bongo_band *cur_band = &bands[0];

/* Centred horizontally; y 60 is the free band between the battery rings, which
 * end at y 56, and the BT profile dots, which start at y 105. */
#define BONGO_X ((SCREEN_WIDTH - BONGO_FRAME_W) / 2)
#define BONGO_Y 60

static const struct bongo_band *band_for_wpm(uint8_t wpm) {
    for (size_t i = 0; i < ARRAY_SIZE(bands) - 1; i++) {
        if (wpm < bands[i].max_wpm) {
            return &bands[i];
        }
    }
    return &bands[ARRAY_SIZE(bands) - 1];
}

/* Both palette entries of every frame are opaque and INDEXED_1BIT assigns an
 * index to all 128x40 pixels, so the blit fully covers the rect -- no need to
 * clear the background first. */
static void draw_frame(lv_obj_t *canvas, const lv_img_dsc_t *frame) {
    lv_draw_img_dsc_t img_dsc;
    lv_draw_img_dsc_init(&img_dsc);
    lv_canvas_draw_img(canvas, BONGO_X, BONGO_Y, frame, &img_dsc);
}

static void bongo_tick(lv_timer_t *timer) {
    /* draw_top() short-circuits to the sleep screen, so never paint over it. */
    if (is_sleep_screen_active()) {
        lv_timer_pause(timer);
        return;
    }

    frame_idx = (frame_idx + 1) % cur_band->n_frames;
    draw_frame(bongo_canvas, cur_band->frames[frame_idx]);
}

static void select_band(const struct bongo_band *band) {
    if (band != cur_band) {
        cur_band = band;
        frame_idx = 0;
    }

    if (band->period_ms == 0) {
        if (bongo_timer != NULL) {
            lv_timer_pause(bongo_timer);
        }
    } else if (bongo_timer == NULL) {
        bongo_timer = lv_timer_create(bongo_tick, band->period_ms, NULL); /* starts running */
    } else {
        lv_timer_set_period(bongo_timer, band->period_ms);
        lv_timer_resume(bongo_timer); /* also recovers from a sleep-time pause */
    }
}

void draw_chart_status(lv_obj_t *canvas, const struct status_state *state) {
    bongo_canvas = canvas;

    /* draw_top() fans in from five listeners but only WPM can move the band, so
     * skip the timer work on the other four. The blit stays unconditional --
     * draw_top() calls fill_background() first, so the cat must be repainted
     * every time regardless of whether the frame changed. */
    if (state->wpm != last_wpm) {
        last_wpm = state->wpm;
        select_band(band_for_wpm(state->wpm));
    }

    draw_frame(canvas, cur_band->frames[frame_idx]);
}
