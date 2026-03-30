// Copyright 2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// MJPEG-over-TCP video stream receiver. The server sends each frame as
// a 4-byte little-endian length prefix followed by JPEG data. Frames
// are decoded with the tjpgd ROM decoder and pushed to the display.

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
#include "esp32s3/rom/tjpgd.h"

#include "board_interface.h"

static const char *TAG = "VIDEO";

#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_events;

// JPEG receive buffer — 64KB should be plenty for 320x240 MJPEG frames
#define JPEG_BUF_SIZE (64 * 1024)

// tjpgd work area — needs ~3.5KB for baseline JPEG
#define TJPGD_WORK_SIZE (4096)

// Context passed through tjpgd callbacks
typedef struct {
    const uint8_t *jpeg_data;
    int jpeg_len;
    int jpeg_pos;
    uint16_t *fb;
    int fb_width;
} decode_ctx_t;

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

// tjpgd input callback — feed JPEG data from memory buffer
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

// tjpgd output callback — write decoded RGB888 blocks to framebuffer as RGB565
static UINT tjpgd_output(JDEC *jdec, void *bitmap, JRECT *rect)
{
    decode_ctx_t *ctx = (decode_ctx_t *)jdec->device;
    uint8_t *rgb = (uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;

    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = *rgb++;
            uint8_t g = *rgb++;
            uint8_t b = *rgb++;
            // Pack to RGB565 with byte swap for SPI
            uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            ctx->fb[y * ctx->fb_width + x] = (c >> 8) | (c << 8);
        }
    }
    (void)bw;
    return 1;  // continue decoding
}

static void stream_task(void *arg)
{
    const int w = board_lcd_width();
    const int h = board_lcd_height();
    uint32_t frame_count = 0;
    int64_t fps_start = 0;

    uint16_t *fb = board_lcd_framebuffer();
    assert(fb);

    // Allocate JPEG receive buffer in PSRAM
    uint8_t *jpeg_buf = heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(jpeg_buf);

    // tjpgd work area in internal RAM for speed
    void *tjpgd_work = malloc(TJPGD_WORK_SIZE);
    assert(tjpgd_work);

    decode_ctx_t ctx = {
        .fb = fb,
        .fb_width = w,
    };

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

        int rcvbuf = 32 * 1024;
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
            // Read 4-byte length prefix
            uint32_t frame_len = 0;
            int n = recv_exact(sock, (uint8_t *)&frame_len, 4);
            if (n <= 0) break;

            if (frame_len > JPEG_BUF_SIZE) {
                ESP_LOGW(TAG, "Frame too large: %lu bytes, skipping", (unsigned long)frame_len);
                // Drain the oversized frame
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

            // Read JPEG data
            n = recv_exact(sock, jpeg_buf, frame_len);
            if (n <= 0) break;

            // Decode JPEG to framebuffer
            ctx.jpeg_data = jpeg_buf;
            ctx.jpeg_len = frame_len;
            ctx.jpeg_pos = 0;

            JDEC jdec;
            JRESULT res = jd_prepare(&jdec, tjpgd_input, tjpgd_work, TJPGD_WORK_SIZE, &ctx);
            if (res == JDR_OK) {
                jd_decomp(&jdec, tjpgd_output, 0);
            } else {
                ESP_LOGW(TAG, "JPEG prepare failed: %d (len=%lu)", res, (unsigned long)frame_len);
                continue;
            }

            board_lcd_flush();
            frame_count++;

            if (frame_count % 30 == 0) {
                int64_t now = esp_timer_get_time();
                float fps = 30.0f * 1000000.0f / (float)(now - fps_start);
                ESP_LOGI(TAG, "Frame %lu, %.1f fps, last JPEG %lu bytes",
                         (unsigned long)frame_count, fps, (unsigned long)frame_len);
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

    wifi_init();

    xTaskCreate(stream_task, "stream", 8192, NULL, 5, NULL);
}
