/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "gateway.h"

#if CONFIG_DISPLAY_ENABLE

#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#define LCD_HOST        SPI2_HOST
#define LCD_RES         360
#define DRAW_LINES      36
#define REFRESH_MS      40
#define TOUCH_I2C_PORT  I2C_NUM_0

#if CONFIG_DISPLAY_BACKLIGHT_ACTIVE_HIGH
#define BL_ON           1
#else
#define BL_ON           0
#endif
#if CONFIG_DISPLAY_MIRROR_X
#define TOUCH_MIRROR_X  1
#else
#define TOUCH_MIRROR_X  0
#endif
#if CONFIG_DISPLAY_MIRROR_Y
#define TOUCH_MIRROR_Y  1
#else
#define TOUCH_MIRROR_Y  0
#endif


static const char *TAG = "display";

/* Written by the audio task, read by the LVGL task */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_bpm;
static bool s_have_music;
static float s_bass, s_high;        /* latest hop band shares */
static bool s_beat;                 /* a beat arrived since the last refresh */

static lv_obj_t *s_arc;
static lv_obj_t *s_corona;
static lv_obj_t *s_title_label;
static lv_obj_t *s_bpm_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_link_label;
static uint32_t s_theme_shown_at;   /* tick when the theme was last switched, 0 = never */
#define THEME_SHOW_MS   2000

/*
 * Corona: a black moon with streamers of light around it, like a solar eclipse.
 * The streamers take the light's current color, grow with the music's level,
 * flare on beats, swell in broad lobes with bass and flicker with treble.
 */
#define MOON_R          72
#define RAY_MAX         72          /* streamer length at full level, px */
#define RAY_MIN         6
#define RAY_COUNT       72
#define CORONA_SIZE     (2 * (MOON_R + RAY_MAX + 6))

static float s_ray_len[RAY_COUNT];
static float s_ray_noise[RAY_COUNT];
static lv_color_t s_corona_color;
static float s_corona_glow;         /* 0..1, ring and streamer brightness */
static float s_phase;               /* slow drift of the streamer pattern, rad */
static float s_kick;                /* beat flare, decays */
static float s_bass_s, s_high_s;    /* smoothed band shares */
static uint32_t s_rand = 1;

void display_show_music(const mood_t *mood, const music_features_t *f)
{
    portENTER_CRITICAL(&s_lock);
    s_bpm = mood->silent ? 0 : f->bpm;
    s_have_music = !mood->silent;
    portEXIT_CRITICAL(&s_lock);
}

void display_on_frame(const music_frame_t *frame)
{
    portENTER_CRITICAL(&s_lock);
    s_bass = frame->bass;
    s_high = frame->high;
    s_beat |= frame->beat;
    portEXIT_CRITICAL(&s_lock);
}

static float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static float frand(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return (s_rand >> 8) / 16777216.0f;
}

/* Approximate the RGBW mix with RGB: white LEDs add a warm white */
static lv_color_t screen_color(const rgbw_t *c, float *level)
{
    float r = c->r + c->w;
    float g = c->g + 0.8f * c->w;
    float b = c->b + 0.55f * c->w;
    float peak = fmaxf(fmaxf(r, g), b);
    *level = clamp01(fmaxf(fmaxf(c->r, c->g), fmaxf(c->b, c->w)));
    if (peak < 1e-3f) {
        return lv_color_make(60, 60, 60);
    }
    return lv_color_make((uint8_t)(255 * r / peak), (uint8_t)(255 * g / peak), (uint8_t)(255 * b / peak));
}

static const char *const s_mood_names[MOOD_MAX] = {
    [MOOD_SILENCE] = "Quiet",
    [MOOD_CALM] = "Calm",
    [MOOD_HAPPY] = "Happy",
    [MOOD_TENSE] = "Tense",
    [MOOD_SAD] = "Sad",
};

static const char *title(const light_status_t *st)
{
    static const char *const genres[MUSIC_GENRE_MAX] = {
        [MUSIC_GENRE_SILENCE] = "Quiet",
        [MUSIC_GENRE_AMBIENT] = "Ambient",
        [MUSIC_GENRE_CLASSICAL] = "Classical",
        [MUSIC_GENRE_POP] = "Pop",
        [MUSIC_GENRE_ROCK] = "Rock",
        [MUSIC_GENRE_ELECTRONIC] = "Electronic",
        [MUSIC_GENRE_HIPHOP] = "Hip-hop",
    };
    switch (st->mode) {
    case LIGHT_MODE_STATIC:
        return "Static";
    case LIGHT_MODE_OFF:
        return "Off";
    default:
        return st->mood.silent || st->genre >= MUSIC_GENRE_MAX ? "Quiet" : genres[st->genre];
    }
}

/* Advance the streamers by one refresh */
static void update_corona(const light_status_t *st, float level, bool beat, float bass, float high)
{
    const float dt = REFRESH_MS / 1000.0f;
    /* Size follows the music, not the user's brightness setting */
    float e = st->brightness > 0.01f ? clamp01(level / st->brightness) : 0;

    if (st->mode == LIGHT_MODE_STATIC) {
        e = 0.45f + 0.05f * sinf(s_phase * 2.0f);
        bass = 0.3f;
        high = 0;
        beat = false;
    }
    s_kick = beat ? 1.0f : s_kick * expf(-dt / 0.15f);
    s_bass_s += (bass - s_bass_s) * 0.15f;
    s_high_s += (high - s_high_s) * 0.3f;
    s_phase += dt * (0.25f + 0.5f * e);

    for (int i = 0; i < RAY_COUNT; i++) {
        float a = 6.2832f * i / RAY_COUNT;
        s_ray_noise[i] = 0.7f * s_ray_noise[i] + 0.3f * frand();
        float shape = 0.5f + 0.22f * sinf(3 * a + s_phase) + 0.14f * sinf(5 * a - 1.7f * s_phase + 1.3f) +
                      0.3f * s_bass_s * sinf(2 * a + 0.5f * s_phase) + 1.2f * s_high_s * (s_ray_noise[i] - 0.5f) +
                      0.35f * s_kick;
        float target = st->mode == LIGHT_MODE_OFF ? 0 : RAY_MIN + RAY_MAX * clamp01(shape * (0.3f + 0.8f * e));
        /* Fast rise, slower fall, like the light itself */
        s_ray_len[i] += (target - s_ray_len[i]) * (target > s_ray_len[i] ? 0.6f : 0.2f);
    }
    s_corona_glow = st->mode == LIGHT_MODE_OFF ? 0.15f : 0.45f + 0.55f * fmaxf(e, s_kick);
}

static void corona_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const int cx = (area.x1 + area.x2) / 2, cy = (area.y1 + area.y2) / 2;
    const float rot = s_phase * 0.15f;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = s_corona_color;
    line.round_end = 1;
    for (int pass = 0; pass < 2; pass++) {
        /* A wide faint streamer, then a thin bright core over its inner part */
        line.width = pass ? 2 : 7;
        line.opa = (lv_opa_t)((pass ? 230 : 70) * s_corona_glow);
        for (int i = 0; i < RAY_COUNT; i++) {
            float len = s_ray_len[i] * (pass ? 0.6f : 1.0f);
            if (len < 1) {
                continue;
            }
            float a = 6.2832f * i / RAY_COUNT + rot, c = cosf(a), s = sinf(a);
            line.p1.x = cx + c * (MOON_R + 2);
            line.p1.y = cy + s * (MOON_R + 2);
            line.p2.x = cx + c * (MOON_R + 2 + len);
            line.p2.y = cy + s * (MOON_R + 2 + len);
            lv_draw_line(layer, &line);
        }
    }

    /* Bright rim around the moon, with a softer halo outside it */
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = cx;
    arc.center.y = cy;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.color = s_corona_color;
    arc.radius = MOON_R + 12;
    arc.width = 12;
    arc.opa = (lv_opa_t)(60 * s_corona_glow);
    lv_draw_arc(layer, &arc);
    arc.radius = MOON_R + 4;
    arc.width = 5;
    arc.opa = (lv_opa_t)(255 * s_corona_glow);
    lv_draw_arc(layer, &arc);

    lv_draw_rect_dsc_t moon;
    lv_draw_rect_dsc_init(&moon);
    moon.bg_color = lv_color_black();
    moon.radius = LV_RADIUS_CIRCLE;
    lv_area_t moon_area = {cx - MOON_R, cy - MOON_R, cx + MOON_R, cy + MOON_R};
    lv_draw_rect(layer, &moon, &moon_area);
}

static void refresh_cb(lv_timer_t *timer)
{
    light_status_t st;
    light_get_status(&st);
    portENTER_CRITICAL(&s_lock);
    float bpm = s_bpm, bass = s_bass, high = s_high;
    bool have_music = s_have_music, beat = s_beat;
    s_beat = false;
    portEXIT_CRITICAL(&s_lock);

    float level;
    s_corona_color = screen_color(&st.out, &level);
    update_corona(&st, level, beat, bass, high);
    lv_obj_invalidate(s_corona);
    lv_obj_set_style_arc_color(s_arc, s_corona_color, LV_PART_INDICATOR);

    if (s_theme_shown_at && lv_tick_elaps(s_theme_shown_at) < THEME_SHOW_MS) {
        lv_label_set_text(s_title_label, st.theme == LIGHT_THEME_SONG ? "Song colors" : "Vivid colors");
    } else {
        lv_label_set_text(s_title_label, title(&st));
    }
    if (st.mode == LIGHT_MODE_MUSIC && have_music) {
        lv_label_set_text_fmt(s_bpm_label, "%s, %d BPM", s_mood_names[mood_quadrant(&st.mood)], (int)lroundf(bpm));
    } else {
        lv_label_set_text(s_bpm_label, "");
    }
    lv_label_set_text(s_mode_label, light_mode_name(st.mode));

    /* Follow changes made over MQTT or the button, unless the user is dragging */
    if (!lv_obj_has_state(s_arc, LV_STATE_PRESSED)) {
        lv_arc_set_value(s_arc, (int32_t)lroundf(st.brightness * 100));
    }
    lv_obj_set_style_text_color(s_link_label, gateway_mqtt_is_connected() ?
                                lv_palette_main(LV_PALETTE_GREEN) : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
}

static void arc_event_cb(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        light_set_brightness(lv_arc_get_value(arc) / 100.0f);
    } else {
        gateway_mqtt_publish_light_state();
    }
}

static void corona_click_cb(lv_event_t *e)
{
    light_next_theme();
    s_theme_shown_at = lv_tick_get() | 1;
    gateway_mqtt_publish_light_state();
}

static void mode_event_cb(lv_event_t *e)
{
    light_next_mode();
    gateway_mqtt_publish_light_state();
}

static void build_ui(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Brightness ring with a gap at the bottom for the mode button */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, LCD_RES - 16, LCD_RES - 16);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x1c1c1c), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_RELEASED, NULL);

    /* The corona is drawn by hand; tap it to switch between the vivid and the Song colors */
    s_corona = lv_obj_create(scr);
    lv_obj_remove_style_all(s_corona);
    lv_obj_set_size(s_corona, CORONA_SIZE, CORONA_SIZE);
    lv_obj_center(s_corona);
    lv_obj_add_flag(s_corona, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_corona, corona_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_corona, corona_click_cb, LV_EVENT_CLICKED, NULL);

    s_link_label = lv_label_create(scr);
    lv_label_set_text(s_link_label, LV_SYMBOL_WIFI);
    lv_obj_align(s_link_label, LV_ALIGN_CENTER, 0, -150);

    /* Genre and mood sit inside the moon */
    s_title_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -8);
    lv_obj_remove_flag(s_title_label, LV_OBJ_FLAG_CLICKABLE);

    s_bpm_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_bpm_label, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(s_bpm_label, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 96, 32);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 152);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1c1c1c), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, mode_event_cb, LV_EVENT_CLICKED, NULL);
    s_mode_label = lv_label_create(btn);
    lv_obj_center(s_mode_label);

    refresh_cb(NULL);
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *tp)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = CONFIG_DISPLAY_TOUCH_SDA_GPIO,
        .scl_io_num = CONFIG_DISPLAY_TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "touch I2C bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch IO");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_RES,
        .y_max = LCD_RES,
        .rst_gpio_num = CONFIG_DISPLAY_TOUCH_RST_GPIO,
        .int_gpio_num = CONFIG_DISPLAY_TOUCH_INT_GPIO,
        .flags = {
            .mirror_x = TOUCH_MIRROR_X,
            .mirror_y = TOUCH_MIRROR_Y,
        },
    };
    return esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, tp);
}

esp_err_t display_start(void)
{
#if CONFIG_DISPLAY_BACKLIGHT_GPIO >= 0
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_DISPLAY_BACKLIGHT_GPIO,
                             .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), TAG, "backlight GPIO");
    gpio_set_level(CONFIG_DISPLAY_BACKLIGHT_GPIO, !BL_ON);
#endif

    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(CONFIG_DISPLAY_QSPI_CLK_GPIO,
                                                                   CONFIG_DISPLAY_QSPI_D0_GPIO, CONFIG_DISPLAY_QSPI_D1_GPIO,
                                                                   CONFIG_DISPLAY_QSPI_D2_GPIO, CONFIG_DISPLAY_QSPI_D3_GPIO,
                                                                   LCD_RES * DRAW_LINES * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "QSPI bus");

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(CONFIG_DISPLAY_QSPI_CS_GPIO, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io), TAG, "panel IO");

    const st77916_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DISPLAY_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *) &vendor_cfg,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(io, &panel_cfg, &panel), TAG, "ST77916 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");

    /* Below the audio and light tasks so drawing never delays the analysis */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_RES * DRAW_LINES,
        .double_buffer = true,
        .hres = LCD_RES,
        .vres = LCD_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "LVGL display");

    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGW(TAG, "Touch controller not found, display only");
    }

    lvgl_port_lock(0);
    build_ui(disp);
    lvgl_port_unlock();

#if CONFIG_DISPLAY_BACKLIGHT_GPIO >= 0
    gpio_set_level(CONFIG_DISPLAY_BACKLIGHT_GPIO, BL_ON);
#endif
    ESP_LOGI(TAG, "ST77916 %dx%d on QSPI, touch %s", LCD_RES, LCD_RES, tp ? "on" : "off");
    return ESP_OK;
}

#else

esp_err_t display_start(void)
{
    return ESP_OK;
}

void display_show_music(const mood_t *mood, const music_features_t *f)
{
}

void display_on_frame(const music_frame_t *frame)
{
}

#endif
