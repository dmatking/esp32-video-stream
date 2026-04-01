// Copyright 2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// MJPEG-over-TCP video stream receiver. The server sends each frame as
// a 4-byte little-endian length prefix followed by JPEG data. Frames
// are decoded and pushed to the display.
//
// On ESP32-S3: software tjpgd ROM decoder → RGB565 SPI display.
// On ESP32-P4: hardware JPEG decoder → RGB888 MIPI-DSI display.

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "board_interface.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "esp32s3/rom/tjpgd.h"
#elif CONFIG_IDF_TARGET_ESP32P4
#include "driver/jpeg_decode.h"
#include "esp_cache.h"
#include "esp_hosted.h"
#endif

static const char *TAG = "VIDEO";

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;

// JPEG receive buffer — 64KB is plenty for 320x240 MJPEG frames,
// but 1024x600 at best quality can produce 100KB+ frames.
#define JPEG_BUF_SIZE (256 * 1024)

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi...");
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

// Read exactly `len` bytes from socket, handling partial reads.
static int recv_exact(int sock, uint8_t *buf, int len)
{
    int received = 0;
    while (received < len) {
        int n = recv(sock, buf + received, len - received, 0);
        if (n <= 0) return n;
        received += n;
    }
    return received;
}

// -----------------------------------------------------------------------
// ESP32-S3: software JPEG decode (tjpgd ROM) → RGB565 framebuffer
// -----------------------------------------------------------------------
#if CONFIG_IDF_TARGET_ESP32S3

#define TJPGD_WORK_SIZE (4096)

typedef struct {
    const uint8_t *jpeg_data;
    int jpeg_len;
    int jpeg_pos;
    uint16_t *fb;
    int fb_width;
} decode_ctx_t;

static UINT tjpgd_input(JDEC *jdec, BYTE *buf, UINT ndata)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jdec->device;
    int remaining = ctx->jpeg_len - ctx->jpeg_pos;
    if ((int)ndata > remaining) ndata = remaining;
    if (buf) {
        memcpy(buf, ctx->jpeg_data + ctx->jpeg_pos, ndata);
    }
    ctx->jpeg_pos += ndata;
    return ndata;
}

static UINT tjpgd_output(JDEC *jdec, void *bitmap, JRECT *rect)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jdec->device;
    uint8_t *rgb = (uint8_t *)bitmap;

    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = *rgb++;
            uint8_t g = *rgb++;
            uint8_t b = *rgb++;
            uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            ctx->fb[y * ctx->fb_width + x] = (c >> 8) | (c << 8);
        }
    }
    return 1;
}

static bool decode_frame(uint8_t *jpeg_buf, uint32_t frame_len,
                         void *work, void *ctx_ptr)
{
    decode_ctx_t *ctx = (decode_ctx_t *)ctx_ptr;
    ctx->jpeg_data = jpeg_buf;
    ctx->jpeg_len = frame_len;
    ctx->jpeg_pos = 0;

    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, tjpgd_input, work, TJPGD_WORK_SIZE, ctx);
    if (res == JDR_OK) {
        jd_decomp(&jdec, tjpgd_output, 0);
        return true;
    }
    ESP_LOGW(TAG, "JPEG prepare failed: %d (len=%lu)", res, (unsigned long)frame_len);
    return false;
}

#endif // CONFIG_IDF_TARGET_ESP32S3

// -----------------------------------------------------------------------
// ESP32-P4: hardware JPEG decode → RGB888 framebuffer
// -----------------------------------------------------------------------
#if CONFIG_IDF_TARGET_ESP32P4

// Declared in board_p4_ev.c
extern jpeg_decoder_handle_t board_jpeg_decoder(void);

// Decode directly into framebuffer (no intermediate copy).
// The framebuffer must be large enough for 16-aligned dimensions.
static bool decode_frame_hw(uint8_t *jpeg_buf, uint32_t frame_len,
                            uint8_t *fb, int fb_size)
{
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(board_jpeg_decoder(), &dec_cfg,
                                         jpeg_buf, frame_len,
                                         fb, fb_size, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HW JPEG decode failed: %s (len=%lu)",
                 esp_err_to_name(err), (unsigned long)frame_len);
        return false;
    }
    return true;
}

#endif // CONFIG_IDF_TARGET_ESP32P4

// -----------------------------------------------------------------------
// Stream task (shared structure, target-specific decode)
// -----------------------------------------------------------------------
static void stream_task(void *arg)
{
    const int w = board_lcd_width();
    const int h = board_lcd_height();
    uint32_t frame_count = 0;
    int64_t fps_start = 0;

#if CONFIG_IDF_TARGET_ESP32S3
    uint16_t *fb = board_lcd_framebuffer();
    assert(fb);

    void *tjpgd_work = malloc(TJPGD_WORK_SIZE);
    assert(tjpgd_work);

    decode_ctx_t ctx = {
        .fb = fb,
        .fb_width = w,
    };
#elif CONFIG_IDF_TARGET_ESP32P4
    // DPI framebuffer is 16-pixel-aligned in height, so JPEG decoder can
    // write directly into it without an intermediate buffer.
    int fb_size = w * board_lcd_fb_height() * 3;
    ESP_LOGI(TAG, "Direct decode into DPI fb (%dx%d, %d bytes)",
             w, board_lcd_fb_height(), fb_size);
#endif

    // Allocate JPEG receive buffer
#if CONFIG_IDF_TARGET_ESP32S3
    uint8_t *jpeg_buf = heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#elif CONFIG_IDF_TARGET_ESP32P4
    // Hardware JPEG decoder needs DMA-aligned input buffer
    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    size_t jpeg_buf_size = 0;
    uint8_t *jpeg_buf = (uint8_t *)jpeg_alloc_decoder_mem(JPEG_BUF_SIZE, &tx_mem_cfg, &jpeg_buf_size);
#endif
    assert(jpeg_buf);

    ESP_LOGI(TAG, "Display: %dx%d, MJPEG mode, JPEG buf: %dKB", w, h, JPEG_BUF_SIZE / 1024);

    while (1) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            ESP_LOGE(TAG, "Socket creation failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        struct sockaddr_in server = {
            .sin_family = AF_INET,
            .sin_port = htons(CONFIG_STREAM_SERVER_PORT),
        };
        inet_aton(CONFIG_STREAM_SERVER_IP, &server.sin_addr);

        int rcvbuf = 64 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        ESP_LOGI(TAG, "Connecting to %s:%d...",
                 CONFIG_STREAM_SERVER_IP, CONFIG_STREAM_SERVER_PORT);

        if (connect(sock, (struct sockaddr *)&server, sizeof(server)) != 0) {
            ESP_LOGW(TAG, "Connect failed, retrying in 2s...");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "Connected! Receiving MJPEG frames...");
        fps_start = esp_timer_get_time();
        frame_count = 0;

        while (1) {
            int64_t t_start = esp_timer_get_time();

            uint32_t frame_len = 0;
            int n = recv_exact(sock, (uint8_t *)&frame_len, 4);
            if (n <= 0) break;

            if (frame_len > JPEG_BUF_SIZE) {
                ESP_LOGW(TAG, "Frame too large: %lu bytes, skipping", (unsigned long)frame_len);
                uint32_t remaining = frame_len;
                while (remaining > 0) {
                    uint32_t chunk = remaining > JPEG_BUF_SIZE ? JPEG_BUF_SIZE : remaining;
                    n = recv_exact(sock, jpeg_buf, chunk);
                    if (n <= 0) break;
                    remaining -= chunk;
                }
                if (n <= 0) break;
                continue;
            }

            n = recv_exact(sock, jpeg_buf, frame_len);
            if (n <= 0) break;

            int64_t t_recv = esp_timer_get_time();

            bool ok = false;
#if CONFIG_IDF_TARGET_ESP32S3
            ok = decode_frame(jpeg_buf, frame_len, tjpgd_work, &ctx);
#elif CONFIG_IDF_TARGET_ESP32P4
            ok = decode_frame_hw(jpeg_buf, frame_len,
                                board_lcd_framebuffer_rgb888(), fb_size);
#endif

            int64_t t_decode = esp_timer_get_time();

            if (ok) {
                board_lcd_flush();
                frame_count++;
            }

            int64_t t_flush = esp_timer_get_time();

            if (frame_count % 30 == 0) {
                int64_t now = t_flush;
                float fps = 30.0f * 1000000.0f / (float)(now - fps_start);
                ESP_LOGI(TAG, "Frame %lu, %.1f fps, %luB | recv=%llums dec=%llums flush=%llums",
                         (unsigned long)frame_count, fps, (unsigned long)frame_len,
                         (long long)(t_recv - t_start) / 1000,
                         (long long)(t_decode - t_recv) / 1000,
                         (long long)(t_flush - t_decode) / 1000);
                fps_start = now;
            }
        }

        ESP_LOGW(TAG, "Stream ended after %lu frames", (unsigned long)frame_count);
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MJPEG video stream receiver starting");

    ESP_ERROR_CHECK(nvs_flash_init());
    board_init();
    board_lcd_clear();
    board_lcd_flush();

#if CONFIG_IDF_TARGET_ESP32P4
    // Connect to ESP32-C6 co-processor for WiFi
    ESP_LOGI(TAG, "Connecting to co-processor...");
    ESP_ERROR_CHECK(esp_hosted_init());
    ESP_ERROR_CHECK(esp_hosted_connect_to_slave());
    ESP_LOGI(TAG, "Co-processor connected");
#endif

    wifi_init();

    xTaskCreate(stream_task, "stream", 16384, NULL, 5, NULL);
}
