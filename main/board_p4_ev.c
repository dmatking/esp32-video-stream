// Copyright 2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board support for ESP32-P4-Function-EV-Board (1024x600 EK79007 MIPI-DSI)
// with hardware JPEG decoder.

#include "board_interface.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOARD_NAME "ESP32-P4 Function EV Board"

// EK79007 1024x600 @ 60Hz
// Buffer height padded to 608 (16-aligned) for JPEG hardware decoder.
// The extra 8 rows are allocated but not visible on the 600-line panel.
#define LCD_H_RES       1024
#define LCD_V_RES       600
#define LCD_V_RES_ALIGN 608
#define LCD_HSYNC       10
#define LCD_HBP         160
#define LCD_HFP         160
#define LCD_VSYNC       1
#define LCD_VBP         23
#define LCD_VFP         12
#define LCD_DPI_CLK_MHZ 52
#define LCD_LANE_NUM    2
#define LCD_LANE_MBPS   1000

// LDO for MIPI PHY (VDD_MIPI_DPHY = 2.5V)
#define LCD_PHY_LDO_CHAN 3
#define LCD_PHY_LDO_MV   2500

// Board GPIOs
#define PIN_LCD_BL  26
#define PIN_LCD_RST 27

// RGB888: 3 bytes per pixel
#define BPP     3
#define FB_SIZE (LCD_H_RES * LCD_V_RES_ALIGN * BPP)

static const char *TAG = "BOARD_P4_EV";

static esp_lcd_panel_handle_t s_panel = NULL;
static uint8_t *s_fb[2] = { NULL, NULL };
static int s_back = 0;  // index of the back buffer (the one we draw into)
static jpeg_decoder_handle_t s_jpeg_decoder = NULL;

static void init_display(void)
{
    // 1. Power on MIPI DSI PHY via internal LDO
    esp_ldo_channel_handle_t ldo_mipi = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = LCD_PHY_LDO_CHAN,
        .voltage_mv = LCD_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi));

    // 2. Create DSI bus
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = LCD_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = LCD_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    // 3. Create DBI (command) IO
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    // 4. Create DPI (data) panel
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .num_fbs = 2,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .video_timing = {
            .h_size = LCD_H_RES,
            .v_size = LCD_V_RES_ALIGN,
            .hsync_back_porch = LCD_HBP,
            .hsync_pulse_width = LCD_HSYNC,
            .hsync_front_porch = LCD_HFP,
            .vsync_back_porch = LCD_VBP,
            .vsync_pulse_width = LCD_VSYNC,
            .vsync_front_porch = LCD_VFP,
        },
    };

    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &dev_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // Display On
    esp_lcd_panel_io_tx_param(dbi_io, 0x29, NULL, 0);

    // 5. Backlight on
    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    gpio_set_level(PIN_LCD_BL, 1);

    // 6. Get the DPI driver's two framebuffers (live in PSRAM)
    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1));
    s_fb[0] = (uint8_t *)fb0;
    s_fb[1] = (uint8_t *)fb1;
    s_back = 0;

    ESP_LOGI(TAG, "MIPI-DSI %dx%d (buf %dx%d) double-buffered, fb0=%p fb1=%p (%d bytes each)",
             LCD_H_RES, LCD_V_RES, LCD_H_RES, LCD_V_RES_ALIGN, s_fb[0], s_fb[1], FB_SIZE);
}

static void init_jpeg_decoder(void)
{
    jpeg_decode_engine_cfg_t cfg = {
        .timeout_ms = 40,
    };
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&cfg, &s_jpeg_decoder));
    ESP_LOGI(TAG, "Hardware JPEG decoder initialized");
}

void board_init(void)
{
    ESP_LOGI(TAG, "%s init", BOARD_NAME);
    init_display();
    init_jpeg_decoder();
    ESP_LOGI(TAG, "%s init done", BOARD_NAME);
}

const char *board_get_name(void) { return BOARD_NAME; }
bool board_has_lcd(void) { return s_panel != NULL; }

void board_lcd_sanity_test(void)
{
    if (!s_fb[0]) return;
    uint8_t *fb = s_fb[s_back];

    // Red
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        fb[i * BPP + 0] = 0x00; fb[i * BPP + 1] = 0x00; fb[i * BPP + 2] = 0xFF;
    }
    board_lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(500));

    fb = s_fb[s_back];
    // Green
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        fb[i * BPP + 0] = 0x00; fb[i * BPP + 1] = 0xFF; fb[i * BPP + 2] = 0x00;
    }
    board_lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(500));

    fb = s_fb[s_back];
    // Blue
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        fb[i * BPP + 0] = 0xFF; fb[i * BPP + 1] = 0x00; fb[i * BPP + 2] = 0x00;
    }
    board_lcd_flush();
    vTaskDelay(pdMS_TO_TICKS(500));

    board_lcd_clear();
    board_lcd_flush();
}

// --- Display drawing API ---

int board_lcd_width(void) { return LCD_H_RES; }
int board_lcd_height(void) { return LCD_V_RES; }

void board_lcd_flush(void)
{
    if (!s_panel || !s_fb[0]) return;
    uint8_t *fb = s_fb[s_back];
    esp_cache_msync(fb, FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES_ALIGN, fb);
    // Swap: the buffer we just presented becomes front, the other becomes back
    s_back ^= 1;
}

void board_lcd_clear(void)
{
    if (s_fb[s_back]) memset(s_fb[s_back], 0, FB_SIZE);
}

void board_lcd_fill(uint16_t color)
{
    // Convert RGB565 to RGB888 for this display
    uint8_t r = ((color >> 11) & 0x1F) << 3;
    uint8_t g = ((color >> 5) & 0x3F) << 2;
    uint8_t b = (color & 0x1F) << 3;
    uint8_t *fb = s_fb[s_back];
    if (!fb) return;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        fb[i * BPP + 0] = b;
        fb[i * BPP + 1] = g;
        fb[i * BPP + 2] = r;
    }
}

void board_lcd_set_pixel_raw(int x, int y, uint16_t color)
{
    // Raw pixel is RGB565 byte-swapped — unpack and store as RGB888
    color = (color >> 8) | (color << 8);
    uint8_t r = ((color >> 11) & 0x1F) << 3;
    uint8_t g = ((color >> 5) & 0x3F) << 2;
    uint8_t b = (color & 0x1F) << 3;
    uint8_t *fb = s_fb[s_back];
    if (fb) {
        int off = (y * LCD_H_RES + x) * BPP;
        fb[off + 0] = b;
        fb[off + 1] = g;
        fb[off + 2] = r;
    }
}

void board_lcd_set_pixel_rgb(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *fb = s_fb[s_back];
    if (fb) {
        int off = (y * LCD_H_RES + x) * BPP;
        fb[off + 0] = b;
        fb[off + 1] = g;
        fb[off + 2] = r;
    }
}

uint16_t board_lcd_pack_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

uint16_t board_lcd_get_pixel_raw(int x, int y)
{
    uint8_t *fb = s_fb[s_back];
    if (!fb) return 0;
    int off = (y * LCD_H_RES + x) * BPP;
    uint8_t b = fb[off + 0], g = fb[off + 1], r = fb[off + 2];
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

void board_lcd_unpack_rgb(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    color = (color >> 8) | (color << 8);
    *r = ((color >> 11) & 0x1F) << 3;
    *g = ((color >> 5) & 0x3F) << 2;
    *b = (color & 0x1F) << 3;
}

uint16_t *board_lcd_framebuffer(void)
{
    // P4 uses RGB888, not RGB565 — this returns NULL to signal that
    // callers should use the hardware JPEG decoder path instead.
    return NULL;
}

// --- Hardware JPEG decoder ---

uint8_t *board_lcd_framebuffer_rgb888(void) { return s_fb[s_back]; }
int board_lcd_fb_height(void) { return LCD_V_RES_ALIGN; }

jpeg_decoder_handle_t board_jpeg_decoder(void) { return s_jpeg_decoder; }
