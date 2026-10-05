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
#define REFRESH_MS      50
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

#define DISC_SIZE       140
#define DOT_SIZE        12

static const char *TAG = "display";

/* Written by the audio task, read by the LVGL task */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_bpm;
static bool s_have_music;

static lv_obj_t *s_arc;
static lv_obj_t *s_disc;
static lv_obj_t *s_dot;
static lv_obj_t *s_title_label;
static lv_obj_t *s_bpm_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_link_label;

void display_show_music(const mood_t *mood, const music_features_t *f)
{
    portENTER_CRITICAL(&s_lock);
    s_bpm = mood->silent ? 0 : f->bpm;
    s_have_music = !mood->silent;
    portEXIT_CRITICAL(&s_lock);
}

static float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
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

static void refresh_cb(lv_timer_t *timer)
{
    light_status_t st;
    light_get_status(&st);
    portENTER_CRITICAL(&s_lock);
    float bpm = s_bpm;
    bool have_music = s_have_music;
    portEXIT_CRITICAL(&s_lock);

    float level;
    lv_color_t color = screen_color(&st.out, &level);
    lv_obj_set_style_bg_color(s_disc, color, 0);
    lv_obj_set_style_shadow_color(s_disc, color, 0);
    lv_obj_set_style_bg_opa(s_disc, (lv_opa_t)(40 + 215 * level), 0);
    lv_obj_set_style_shadow_opa(s_disc, (lv_opa_t)(200 * level), 0);
    lv_obj_set_style_arc_color(s_arc, color, LV_PART_INDICATOR);

    /* Valence left to right, energy bottom to top, inside the disc */
    const int span = DISC_SIZE / 2 - DOT_SIZE;
    lv_obj_align(s_dot, LV_ALIGN_CENTER, (int)lroundf((st.mood.valence - 0.5f) * 2 * span),
                 (int)lroundf((0.5f - st.mood.energy) * 2 * span));
    if (st.mode == LIGHT_MODE_MUSIC && !st.mood.silent) {
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_title_label, title(&st));
    if (st.mode == LIGHT_MODE_MUSIC && have_music) {
        lv_label_set_text_fmt(s_bpm_label, "%s, %d BPM", s_mood_names[mood_quadrant(&st.mood)], (int)lroundf(bpm));
        /* LVGL's own formatter has no float support */
        lv_label_set_text_fmt(s_detail_label, "valence %d%%  energy %d%%", (int)lroundf(st.mood.valence * 100),
                              (int)lroundf(st.mood.energy * 100));
    } else {
        lv_label_set_text(s_bpm_label, "");
        lv_label_set_text(s_detail_label, "");
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
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x262626), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 2, LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_RELEASED, NULL);

    /* Glowing disc mirrors the light; the dot inside shows valence and energy */
    s_disc = lv_obj_create(scr);
    lv_obj_set_size(s_disc, DISC_SIZE, DISC_SIZE);
    lv_obj_center(s_disc);
    lv_obj_set_style_radius(s_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_disc, 0, 0);
    lv_obj_set_style_shadow_width(s_disc, 40, 0);
    lv_obj_set_style_shadow_spread(s_disc, 4, 0);
    lv_obj_remove_flag(s_disc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_dot = lv_obj_create(s_disc);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_dot, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_dot, 2, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);

    s_link_label = lv_label_create(scr);
    lv_label_set_text(s_link_label, LV_SYMBOL_WIFI);
    lv_obj_align(s_link_label, LV_ALIGN_CENTER, 0, -140);

    s_title_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_28, 0);
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -105);

    s_bpm_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_bpm_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_bpm_label, LV_ALIGN_CENTER, 0, 92);

    s_detail_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_detail_label, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, 116);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 104, 36);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
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

#endif
