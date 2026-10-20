/*
 * voice_chat — push-to-talk voice client for the Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *
 * Touch and hold the screen to talk. On release the recording is sent as ONE binary
 * WebSocket frame (WAV, 16 kHz, mono, 16-bit PCM) to your voice server. The server's
 * JSON text frames (TRANSCRIBED_REQUEST / RESPONSE / ERROR) are shown on screen and its
 * binary reply audio (MP3, WAV or raw 16-bit PCM, detected automatically) is played
 * through the speaker. MP3 is decoded on the device with minimp3 (CC0).
 *
 * Audio uses Waveshare's bsp_extra helper on top of the 1.75C BSP (the same calls as the
 * working 05_Spec_Analyzer example). Server URL, API key and Wi-Fi live in secrets.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <ctype.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"     // xTaskCreatePinnedToCoreWithCaps

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"

#include "secrets.h"

/* minimp3: public-domain (CC0) MP3 decoder, implementation compiled in minimp3_impl.c */
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define TAG "voice_chat"

/* The server's certificate chain, embedded from main/server_ca.pem (see README, "TLS certificate").
 * Using the exact chain works even when the server doesn't send its intermediate certificate,
 * or uses a CA that isn't in ESP-IDF's built-in bundle. */
extern const char server_ca_pem_start[] asm("_binary_server_ca_pem_start");

/* ---------------------------------------------------------------- settings */
#define REC_RATE            16000           // server expects 16 kHz
#define I2S_CHANNELS        2               // bsp_extra runs the codec in stereo
#define READ_FRAMES         512             // frames per I2S read (32 ms)
#define REC_MAX_SECONDS     10              // recording limit
#define REC_MIN_MS          300             // ignore taps shorter than this
#define WAV_HEADER_BYTES    44
#define RX_MAX_BYTES        (3 * 1024 * 1024)   // largest single server message we accept
#define PLAY_FRAMES         512
#define SPEAKER_VOLUME      75              // 0-100
#define TURN_TIMEOUT_US     (45LL * 1000 * 1000)
#define TEXT_ONLY_IDLE_US   (4LL * 1000 * 1000)  // reply text but no audio: return to ready after this
#define REPLY_MAX_CHARS     320             // what fits on the round screen
#define REPLY_BIG_FONT_CHARS 90             // longer replies switch to the 16 px font
#define AUTO_OFF_MS         (2 * 60 * 1000) // switch off after this long idle; 0 = never
#define SWIPE_MIN_PX        60              // vertical travel that counts as a swipe
#define SWIPE_MAX_MS        800             // a swipe must happen this soon after touching

#ifndef WS_PORT
#define WS_PORT 443
#endif

/* ------------------------------------------------------------------- state */
typedef enum {
    ST_BOOT, ST_WIFI, ST_CONNECTING, ST_OFF, ST_READY, ST_LISTENING, ST_THINKING, ST_SPEAKING, ST_ERROR
} app_state_t;

typedef struct {
    uint8_t *data;
    size_t   len;
} play_item_t;

static volatile app_state_t s_state = ST_BOOT;
static volatile bool s_recording = false;
static volatile bool s_rec_cancel = false;     // set by a swipe: discard the recording
static volatile bool s_ws_connected = false;
static volatile bool s_got_text = false;
static volatile bool s_enabled = false;          // on/off toggle; starts off
static volatile int64_t s_last_activity_us = 0;  // for auto-off
static volatile int64_t s_turn_start_us = 0;
static volatile int64_t s_last_rx_us = 0;

static TaskHandle_t s_rec_task = NULL;
static QueueHandle_t s_play_q = NULL;
static esp_websocket_client_handle_t s_ws = NULL;
static EventGroupHandle_t s_wifi_eg = NULL;
#define WIFI_CONNECTED_BIT BIT0

static char s_url[512];
static char s_reply[REPLY_MAX_CHARS + 8];

/* receive reassembly (filled from the websocket task) */
static uint8_t *s_rx_buf = NULL;
static size_t   s_rx_len = 0;
static size_t   s_rx_cap = 0;
static int      s_rx_op  = 0;

/* -------------------------------------------------------------------- UI */
static lv_obj_t *s_ring;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_you;
static lv_obj_t *s_lbl_reply;
static lv_obj_t *s_btn_power;

static void ui_label(lv_obj_t *lbl, const char *fmt, ...)
{
    char buf[REPLY_MAX_CHARS + 64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    bsp_display_lock(-1);
    lv_label_set_text(lbl, buf);
    bsp_display_unlock();
}

/* ---- "thinking" animation: three dots bouncing in a wave -------------------------------
 * Each dot is a small object, so every animation frame repaints only a few hundred pixels:
 * no full-screen redraws, which keeps the display's DMA memory needs tiny. */
#define DOT_COUNT     3
#define DOT_SIZE      18
#define DOT_GAP       16
#define DOT_BOUNCE    22     // px
#define DOT_PERIOD_MS 1000   // one full wave
#define DOT_STAGGER   140    // ms between dots

static lv_obj_t *s_dots;
static lv_obj_t *s_dot[DOT_COUNT];
static bool      s_dots_running = false;

/* v runs 0..1000 over one period; the dot hops during the first half, rests the second half */
static void dot_anim_cb(void *var, int32_t v)
{
    float phase = v / 1000.0f;
    float hop = phase < 0.5f ? sinf(phase * 2.0f * (float)M_PI) : 0.0f;
    /* squash slightly near the floor so the hop reads as "bouncy"; keep the bottom on the floor */
    int32_t h = (phase > 0.02f && hop < 0.12f) ? DOT_SIZE - 4 : DOT_SIZE;
    lv_obj_set_height((lv_obj_t *)var, h);
    lv_obj_set_y((lv_obj_t *)var, DOT_BOUNCE - (int32_t)(hop * DOT_BOUNCE) + (DOT_SIZE - h));
}

static void dots_build(lv_obj_t *scr)
{
    static const uint32_t colors[DOT_COUNT] = { 0x3498DB, 0x9B59B6, 0x1ABC9C };
    const int32_t width = DOT_COUNT * DOT_SIZE + (DOT_COUNT - 1) * DOT_GAP + 4;

    s_dots = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, width, DOT_SIZE + DOT_BOUNCE + 4);
    lv_obj_align(s_dots, LV_ALIGN_CENTER, 0, 10);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < DOT_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(s_dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(d, i * (DOT_SIZE + DOT_GAP), DOT_BOUNCE);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        s_dot[i] = d;
    }
}

/* Call with the display lock held. */
static void dots_start_locked(void)
{
    if (s_dots_running) return;
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < DOT_COUNT; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dot[i]);
        lv_anim_set_exec_cb(&a, dot_anim_cb);
        lv_anim_set_values(&a, 0, 1000);
        lv_anim_set_duration(&a, DOT_PERIOD_MS);
        lv_anim_set_delay(&a, i * DOT_STAGGER);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    s_dots_running = true;
}

/* Call with the display lock held. */
static void dots_stop_locked(void)
{
    if (!s_dots_running) return;
    for (int i = 0; i < DOT_COUNT; i++) {
        lv_anim_delete(s_dot[i], dot_anim_cb);
        lv_obj_set_pos(s_dot[i], i * (DOT_SIZE + DOT_GAP), DOT_BOUNCE);
        lv_obj_set_size(s_dot[i], DOT_SIZE, DOT_SIZE);
    }
    lv_obj_add_flag(s_dots, LV_OBJ_FLAG_HIDDEN);
    s_dots_running = false;
}

static void dots_stop(void)
{
    bsp_display_lock(-1);
    dots_stop_locked();
    bsp_display_unlock();
}

/* Applies a state to the UI. Caller must hold the display lock (or be in the LVGL task).
 * While the app is switched off, "ready" shows as "paused". */
static void ui_state_apply(app_state_t st)
{
    if (!s_enabled && st == ST_READY) st = ST_OFF;
    if (st == ST_READY) s_last_activity_us = esp_timer_get_time();
    s_state = st;

    const char *text = "";
    uint32_t color = 0x444444;
    switch (st) {
    case ST_BOOT:       text = "Starting...";            color = 0x444444; break;
    case ST_WIFI:       text = "Connecting to Wi-Fi...";  color = 0xF5A623; break;
    case ST_CONNECTING: text = "Connecting to server..."; color = 0xF5A623; break;
    case ST_OFF:        text = "Swipe up to start";      color = 0x444444; break;
    case ST_READY:      text = "Hold to talk";           color = 0x2ECC71; break;
    case ST_LISTENING:  text = "Listening...";           color = 0xE74C3C; break;
    case ST_THINKING:   text = "Thinking...";            color = 0x3498DB; break;
    case ST_SPEAKING:   text = "Speaking...";            color = 0x9B59B6; break;
    case ST_ERROR:      text = "Error";                  color = 0xE74C3C; break;
    }

    lv_label_set_text(s_lbl_status, text);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(color), 0);
    if (st == ST_THINKING) dots_start_locked(); else dots_stop_locked();

    /* power button: green when on, gray when off */
    uint32_t pc = s_enabled ? 0x2ECC71 : 0x666666;
    lv_obj_set_style_border_color(s_btn_power, lv_color_hex(pc), 0);
    lv_obj_set_style_text_color(s_btn_power, lv_color_hex(pc), 0);
}

static void ui_state(app_state_t st)
{
    bsp_display_lock(-1);
    ui_state_apply(st);
    bsp_display_unlock();
}

/* Switch the app on or off (runs in the LVGL task, so no display locking here). */
static void set_enabled(bool on)
{
    if (s_enabled == on) return;
    s_enabled = on;
    ESP_LOGI(TAG, "Switched %s", on ? "on" : "off");

    if (on && s_state == ST_OFF) {
        ui_state_apply(s_ws_connected ? ST_READY : ST_CONNECTING);
    } else if (!on && s_state == ST_READY) {
        ui_state_apply(ST_OFF);
    } else {
        /* mid-turn (listening/thinking/speaking) or still connecting: just update the button;
         * the state switches to paused/ready when the turn finishes */
        ui_state_apply(s_state);
    }
}

/* Power button tap: toggles on/off. */
static void power_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_recording) return;
    set_enabled(!s_enabled);
}

/* Full-screen touch area (runs in the LVGL task: only set flags here, no display locking).
 *   touch and hold      -> record while held (only when on)
 *   swipe up / down     -> switch on / off; a recording the touch started is discarded
 * Recording starts the moment the finger lands, so the first word isn't lost; if the finger
 * then moves SWIPE_MIN_PX vertically within SWIPE_MAX_MS, it was a swipe, not talking. */
static lv_point_t s_touch_start;
static int64_t    s_touch_start_us;
static bool       s_swiped;

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED) {
        if (indev) lv_indev_get_point(indev, &s_touch_start);
        s_touch_start_us = esp_timer_get_time();
        s_swiped = false;
        if (s_state == ST_READY && s_ws_connected && !s_recording) {
            s_last_activity_us = s_touch_start_us;
            s_rec_cancel = false;
            s_recording = true;
            xTaskNotifyGive(s_rec_task);
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (s_swiped || !indev) return;
        if (esp_timer_get_time() - s_touch_start_us > (int64_t)SWIPE_MAX_MS * 1000) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int dx = p.x - s_touch_start.x;
        int dy = p.y - s_touch_start.y;
        if (LV_ABS(dy) < SWIPE_MIN_PX || LV_ABS(dy) < 2 * LV_ABS(dx)) return;

        s_swiped = true;
        if (s_recording) {            // the touch started a recording: throw it away
            s_rec_cancel = true;
            s_recording = false;
        }
        set_enabled(dy < 0);          // screen y grows downward: negative = swipe up
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_recording = false;
    }
}

static void ui_build(void)
{
    bsp_display_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* colored ring around the round display shows the state */
    s_ring = lv_obj_create(scr);
    lv_obj_remove_style_all(s_ring);
    lv_obj_set_size(s_ring, BSP_LCD_H_RES - 6, BSP_LCD_H_RES - 6);
    lv_obj_center(s_ring);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring, 8, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_lbl_status, "Starting...");
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 70);

    /* Transcript and reply share one column, so a long reply pushes nothing on top of the
     * transcript. The column has a fixed box inside the round screen: content is centered
     * vertically and anything that still doesn't fit is clipped, not drawn over the ring. */
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 330, 230);
    lv_obj_align(col, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_you = lv_label_create(col);
    lv_obj_set_width(s_lbl_you, LV_PCT(100));
    lv_label_set_long_mode(s_lbl_you, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl_you, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lbl_you, lv_color_hex(0x9AA0A6), 0);
    lv_obj_set_style_text_font(s_lbl_you, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_lbl_you, "");

    s_lbl_reply = lv_label_create(col);
    lv_obj_set_width(s_lbl_reply, LV_PCT(100));
    lv_label_set_long_mode(s_lbl_reply, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_lbl_reply, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lbl_reply, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_reply, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_lbl_reply, "");

    dots_build(scr);

    /* invisible full-screen touch area on top of everything */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, LV_PCT(100), LV_PCT(100));
    lv_obj_center(touch);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(touch, touch_event_cb, LV_EVENT_ALL, NULL);

    /* power indicator, also tappable: created last so it sits above the touch area */
    s_btn_power = lv_obj_create(scr);
    lv_obj_remove_style_all(s_btn_power);
    lv_obj_set_size(s_btn_power, 60, 60);
    lv_obj_align(s_btn_power, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_radius(s_btn_power, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_btn_power, 3, 0);
    lv_obj_set_style_border_color(s_btn_power, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_color(s_btn_power, lv_color_hex(0x666666), 0);
    lv_obj_set_style_bg_color(s_btn_power, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(s_btn_power, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_btn_power, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_btn_power, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_btn_power, power_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(s_btn_power);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_center(icon);

    bsp_display_unlock();
}

/* ------------------------------------------------------------- helpers */
static void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }
static uint16_t get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void write_wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate, uint16_t channels, uint16_t bits)
{
    uint16_t block_align = channels * bits / 8;
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);                 // fmt chunk size
    put_le16(h + 20, 1);                  // PCM
    put_le16(h + 22, channels);
    put_le32(h + 24, rate);
    put_le32(h + 28, rate * block_align); // byte rate
    put_le16(h + 32, block_align);
    put_le16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
}

/* Finds fmt + data chunks. Returns false if not a usable PCM WAV. */
static bool parse_wav(const uint8_t *buf, size_t len, const uint8_t **pcm, size_t *pcm_len,
                      uint32_t *rate, uint16_t *channels, uint16_t *bits)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }
    bool have_fmt = false;
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t size = get_le32(buf + pos + 4);
        size_t body = pos + 8;

        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= len) {
            uint16_t fmt = get_le16(buf + body);
            *channels = get_le16(buf + body + 2);
            *rate     = get_le32(buf + body + 4);
            *bits     = get_le16(buf + body + 14);
            if (fmt != 1 && fmt != 0xFFFE) {
                ESP_LOGW(TAG, "WAV format %u is not PCM", fmt);
                return false;
            }
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) return false;
            size_t avail = len - body;
            *pcm = buf + body;
            /* streaming WAVs often carry 0 or 0xFFFFFFFF as the data size */
            *pcm_len = (size == 0 || size == 0xFFFFFFFF || size > avail) ? avail : size;
            return true;
        }
        pos = body + size + (size & 1);
        if (size == 0xFFFFFFFF) break;
    }
    return false;
}

static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

static void make_uuid(char *out /* 37 bytes */)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void reply_clear(void)
{
    s_reply[0] = '\0';
    s_got_text = false;
    ui_label(s_lbl_you, "");
    ui_label(s_lbl_reply, "");
}

static void reply_append(const char *text)
{
    size_t cur = strlen(s_reply);
    if (cur > 0 && cur < REPLY_MAX_CHARS) {
        strncat(s_reply, "\n", REPLY_MAX_CHARS - cur);
        cur++;
    }
    if (cur < REPLY_MAX_CHARS) {
        strncat(s_reply, text, REPLY_MAX_CHARS - cur);
    }
    if (strlen(text) + cur > REPLY_MAX_CHARS) {
        strcpy(s_reply + REPLY_MAX_CHARS - 3, "...");
    }
    /* long replies get the smaller font so they fit the round screen */
    const lv_font_t *font = strlen(s_reply) > REPLY_BIG_FONT_CHARS ? &lv_font_montserrat_16 : &lv_font_montserrat_20;
    bsp_display_lock(-1);
    lv_obj_set_style_text_font(s_lbl_reply, font, 0);
    lv_label_set_text(s_lbl_reply, s_reply);
    bsp_display_unlock();
}

/* ---------------------------------------------------------- display text
 * The built-in Montserrat fonts only contain ASCII, so "smart" punctuation and accented
 * letters from the LLM show up as empty boxes. Convert them to ASCII look-alikes for display
 * (the spoken reply is unaffected). Emoji are dropped; anything else unknown becomes '?'. */
static const char *const s_latin1_hi[64] = {   /* U+00C0 .. U+00FF */
    "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
    "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
    "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y",
};

static const char *ascii_for(uint32_t cp)
{
    if (cp >= 0xC0 && cp <= 0xFF) return s_latin1_hi[cp - 0xC0];
    switch (cp) {
    case 0x00A0: case 0x2002: case 0x2003: case 0x2007: case 0x2009: case 0x200A: case 0x202F:
        return " ";
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x00B4: case 0x0060:
        return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB:
        return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00B7: case 0x25CF: return "-";
    case 0x00B0: return " deg";
    case 0x00A9: return "(c)";
    case 0x00AE: return "(R)";
    case 0x2122: return "(TM)";
    case 0x00B1: return "+/-";
    case 0x00A3: return "GBP";
    case 0x20AC: return "EUR";
    case 0x00A5: return "JPY";
    case 0x00BF: return "?";
    case 0x00A1: return "!";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x200B: case 0x200C: case 0x200D: case 0xFE0F: case 0xFEFF: return "";   /* invisible */
    default: break;
    }
    if (cp >= 0x1F000) return "";   /* emoji and pictographs */
    return "?";
}

static void to_display_ascii(const char *in, char *out, size_t out_len)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    while (*p && o + 1 < out_len) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80)                                  { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); n = 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); n = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            n = 4;
        } else { cp = '?'; n = 1; }                       /* invalid byte */
        p += n;

        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp == 0x2014 || cp == 0x2015) {
            /* em dash: "word—word" reads better as "word - word" (no doubled spaces) */
            if (o > 0 && out[o - 1] != ' ' && o + 1 < out_len) out[o++] = ' ';
            if (o + 1 < out_len) out[o++] = '-';
            if (*p && *p != ' ' && o + 1 < out_len) out[o++] = ' ';
        } else {
            for (const char *r = ascii_for(cp); *r && o + 1 < out_len; r++) out[o++] = *r;
        }
    }
    out[o] = '\0';
}

/* -------------------------------------------------------- server messages */
static void handle_event_obj(cJSON *obj)
{
    static char m[1024];   /* display-safe copy of "message" (called from the WebSocket task only) */
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    cJSON *msg  = cJSON_GetObjectItem(obj, "message");
    to_display_ascii(cJSON_IsString(msg) ? msg->valuestring : "", m, sizeof(m));

    if (!cJSON_IsString(type)) {
        /* object without a type: show it compactly */
        char *s = cJSON_PrintUnformatted(obj);
        if (s) { to_display_ascii(s, m, sizeof(m)); reply_append(m); cJSON_free(s); }
        s_got_text = true;
        return;
    }

    if (strcmp(type->valuestring, "TRANSCRIBED_REQUEST") == 0) {
        ESP_LOGI(TAG, "Heard: %s", m);
        ui_label(s_lbl_you, "\"%s\"", m);
    } else if (strcmp(type->valuestring, "RESPONSE") == 0) {
        ESP_LOGI(TAG, "Reply: %s", m);
        dots_stop();
        reply_append(m);
        s_got_text = true;
    } else if (strcmp(type->valuestring, "ERROR") == 0) {
        ESP_LOGE(TAG, "Server error: %s", m);
        dots_stop();
        ui_label(s_lbl_reply, "Server error: %s", m);
        s_got_text = true;
    } else {
        ESP_LOGW(TAG, "Ignoring message type %s", type->valuestring);
    }
}

static void handle_text(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) {
        /* plain text reply */
        static char t[1024];
        ESP_LOGI(TAG, "Text: %s", text);
        dots_stop();
        to_display_ascii(text, t, sizeof(t));
        reply_append(t);
        s_got_text = true;
        return;
    }
    if (cJSON_IsArray(root)) {
        cJSON *item;
        cJSON_ArrayForEach(item, root) {
            if (cJSON_IsObject(item)) handle_event_obj(item);
        }
    } else if (cJSON_IsObject(root)) {
        handle_event_obj(root);
    }
    cJSON_Delete(root);
}

static bool rx_reserve(size_t need)
{
    if (need > RX_MAX_BYTES) return false;
    if (need <= s_rx_cap) return true;
    size_t cap = s_rx_cap ? s_rx_cap : 64 * 1024;
    while (cap < need) cap *= 2;
    if (cap > RX_MAX_BYTES) cap = RX_MAX_BYTES;
    uint8_t *p = heap_caps_realloc(s_rx_buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return false;
    s_rx_buf = p;
    s_rx_cap = cap;
    return true;
}

static void handle_ws_data(const esp_websocket_event_data_t *d)
{
    if (d->op_code == 0x08) {   // close frame: first two bytes are the close code
        int code = (d->data_len >= 2) ? (((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1]) : 0;
        ESP_LOGW(TAG, "Server closed the connection, code %d", code);
        return;
    }
    if (d->op_code == 0x09 || d->op_code == 0x0A) return;   // ping/pong

    if (d->op_code == 0x01 || d->op_code == 0x02) {
        if (d->payload_offset == 0) {
            s_rx_len = 0;
            s_rx_op = d->op_code;
        }
    } else if (d->op_code != 0x00) {
        return;   // unknown opcode
    }

    if (d->data_len > 0) {
        if (!rx_reserve(s_rx_len + d->data_len + 1)) {
            ESP_LOGE(TAG, "Incoming message too large or out of memory; dropping it");
            s_rx_len = 0;
            return;
        }
        memcpy(s_rx_buf + s_rx_len, d->data_ptr, d->data_len);
        s_rx_len += d->data_len;
    }

    bool frame_complete = (d->payload_offset + d->data_len >= d->payload_len);
    if (!frame_complete) return;

    s_last_rx_us = esp_timer_get_time();

    if (s_rx_op == 0x01) {
        if (!rx_reserve(s_rx_len + 1)) { s_rx_len = 0; return; }   // also covers empty frames
        s_rx_buf[s_rx_len] = '\0';
        handle_text((const char *)s_rx_buf);
    } else if (s_rx_op == 0x02 && s_rx_len > 0) {
        play_item_t item = {
            .data = heap_caps_malloc(s_rx_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
            .len = s_rx_len,
        };
        if (item.data) {
            memcpy(item.data, s_rx_buf, s_rx_len);
            ESP_LOGI(TAG, "Received %u bytes of reply audio", (unsigned)s_rx_len);
            if (xQueueSend(s_play_q, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Playback queue full; dropping audio");
                heap_caps_free(item.data);
            }
        }
    }
    s_rx_len = 0;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        s_ws_connected = true;
        if (s_state == ST_CONNECTING || s_state == ST_ERROR || s_state == ST_BOOT) {
            ui_state(ST_READY);
            ui_label(s_lbl_reply, "");
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (will retry)");
        s_ws_connected = false;
        if (s_state != ST_SPEAKING) ui_state(ST_CONNECTING);
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WebSocket closed");
        s_ws_connected = false;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error (check host, port, path, API key, TLS)");
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_ws_data(d);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ recording */
static void rec_task(void *arg)
{
    const size_t io_bytes = READ_FRAMES * I2S_CHANNELS * sizeof(int16_t);
    int16_t *io = heap_caps_malloc(io_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t max_samples = (size_t)REC_MAX_SECONDS * REC_RATE;
    uint8_t *wav = heap_caps_malloc(WAV_HEADER_BYTES + max_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!io || !wav) {
        ESP_LOGE(TAG, "Out of memory for recording buffers");
        ui_state(ST_ERROR);
        vTaskDelete(NULL);
    }
    int16_t *pcm = (int16_t *)(wav + WAV_HEADER_BYTES);

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ui_state(ST_LISTENING);
        reply_clear();

        size_t got = 0;
        bsp_extra_i2s_read(io, io_bytes, &got, 200);   // drop stale buffered audio

        size_t n = 0;
        while (s_recording && n < max_samples) {
            got = 0;
            if (bsp_extra_i2s_read(io, io_bytes, &got, 1000) != ESP_OK || got == 0) {
                continue;
            }
            size_t frames = got / (I2S_CHANNELS * sizeof(int16_t));
            for (size_t i = 0; i < frames && n < max_samples; i++) {
                int32_t mixed = ((int32_t)io[2 * i] + (int32_t)io[2 * i + 1]) / 2;
                pcm[n++] = (int16_t)mixed;
            }
        }
        s_recording = false;

        uint32_t ms = (uint32_t)(n * 1000 / REC_RATE);
        if (s_rec_cancel) {           // the touch was a swipe, not talking
            s_rec_cancel = false;
            ESP_LOGI(TAG, "Swipe: discarded %u ms of audio", (unsigned)ms);
            ui_state(ST_READY);       // shows as paused if the swipe switched the app off
            continue;
        }
        ESP_LOGI(TAG, "Recorded %u ms", (unsigned)ms);

        if (ms < REC_MIN_MS) {
            ui_state(ST_READY);
            ui_label(s_lbl_reply, "Hold while you talk");
            continue;
        }
        if (!s_ws_connected) {
            ui_state(ST_CONNECTING);
            ui_label(s_lbl_reply, "Not connected to the server");
            continue;
        }

        write_wav_header(wav, (uint32_t)(n * sizeof(int16_t)), REC_RATE, 1, 16);
        size_t total = WAV_HEADER_BYTES + n * sizeof(int16_t);

        s_turn_start_us = esp_timer_get_time();
        s_last_rx_us = s_turn_start_us;
        ui_state(ST_THINKING);

        int sent = esp_websocket_client_send_bin(s_ws, (const char *)wav, (int)total, pdMS_TO_TICKS(15000));
        if (sent < 0) {
            ESP_LOGE(TAG, "Send failed");
            ui_state(ST_READY);
            ui_label(s_lbl_reply, "Send failed - try again");
        } else {
            ESP_LOGI(TAG, "Sent %d bytes (%u ms of audio)", sent, (unsigned)ms);
        }
    }
}

/* ------------------------------------------------------------- playback */
typedef enum { AUD_WAV, AUD_MP3, AUD_OGG, AUD_RAW } audio_kind_t;

static audio_kind_t classify_audio(const uint8_t *d, size_t len)
{
    if (len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0) return AUD_WAV;
    if (len >= 3 && memcmp(d, "ID3", 3) == 0) return AUD_MP3;
    if (len >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return AUD_MP3;   // MPEG frame sync
    if (len >= 4 && memcmp(d, "OggS", 4) == 0) return AUD_OGG;
    return AUD_RAW;
}

static const char *kind_name(audio_kind_t k)
{
    switch (k) {
    case AUD_WAV: return "WAV";
    case AUD_MP3: return "MP3";
    case AUD_OGG: return "OGG (Opus/Vorbis)";
    default:      return "raw/unknown";
    }
}

static inline int16_t mono_at(const uint8_t *pcm, size_t i, int channels, size_t frame_bytes)
{
    int16_t l, r;
    memcpy(&l, pcm + i * frame_bytes, 2);
    if (channels < 2) return l;
    memcpy(&r, pcm + i * frame_bytes + 2, 2);
    return (int16_t)(((int32_t)l + r) / 2);
}

/* All playback goes out at REC_RATE (16 kHz), the rate the codec is set to at boot.
 * Replies at other rates (your server's MP3 is 22.05 kHz) are converted here with linear
 * interpolation instead of re-clocking the codec. Re-clocking tears down and rebuilds the I2S
 * driver, which briefly takes internal DMA memory from the display ("Draw bitmap failed").
 * For speech, linear interpolation is indistinguishable from re-clocking. */
typedef struct {
    double  step;      // input samples per output sample
    double  pos;       // read position in [prev, x0, x1, ...]
    int16_t prev;      // last input sample of the previous chunk
    size_t  out_n;     // stereo frames waiting in the output buffer
} resampler_t;

static resampler_t s_rs;

static void resampler_reset(void)
{
    s_rs.step = 1.0;
    s_rs.pos = 1.0;
    s_rs.prev = 0;
    s_rs.out_n = 0;
}

static void out_flush(int16_t *out)
{
    if (s_rs.out_n == 0) return;
    size_t written = 0;
    bsp_extra_i2s_write(out, s_rs.out_n * 2 * sizeof(int16_t), &written, 1000);
    s_rs.out_n = 0;
}

/* Plays interleaved 16-bit samples (1 or 2 channels) at in_rate through the 16 kHz codec. */
static void play_pcm16(const uint8_t *pcm, size_t frames, int channels, uint32_t in_rate, int16_t *out)
{
    if (frames == 0 || channels <= 0 || in_rate == 0) return;
    s_rs.step = (double)in_rate / (double)REC_RATE;

    const size_t frame_bytes = (size_t)channels * 2;
    /* mono sample i of this chunk (stereo is averaged); index 0 is the previous chunk's last */
    #define SAMPLE_AT(i) ((i) == 0 ? s_rs.prev : mono_at(pcm, (i) - 1, channels, frame_bytes))

    while (s_rs.pos + 1.0 <= (double)frames) {
        size_t i = (size_t)s_rs.pos;
        double f = s_rs.pos - (double)i;
        int32_t a = SAMPLE_AT(i);
        int32_t b2 = SAMPLE_AT(i + 1);
        int16_t y = (int16_t)(a + (int32_t)((b2 - a) * f));

        out[2 * s_rs.out_n] = y;
        out[2 * s_rs.out_n + 1] = y;
        if (++s_rs.out_n == PLAY_FRAMES) out_flush(out);
        s_rs.pos += s_rs.step;
    }
    s_rs.pos -= (double)frames;
    s_rs.prev = mono_at(pcm, frames - 1, channels, frame_bytes);
    #undef SAMPLE_AT
}

/* MP3 decoder state lives across queue items, so a stream split over several
 * WebSocket messages decodes as one. Reset at the end of each reply. */
/* Decoder state and frame buffer live in PSRAM: internal RAM is reserved for the display's
 * DMA transfers (running out of it shows up as "Draw bitmap failed: ESP_ERR_NO_MEM"). */
static mp3dec_t *s_mp3 = NULL;
static bool     s_mp3_active = false;
static uint8_t *s_mp3_carry = NULL;          // incomplete frame left from the previous message
static size_t   s_mp3_carry_len = 0;
static int16_t *s_mp3_pcm = NULL;
#define MP3_CARRY_MAX 8192

static void play_task(void *arg)
{
    int16_t *out = heap_caps_malloc(PLAY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_mp3_carry = heap_caps_malloc(MP3_CARRY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3 = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mp3_pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out || !s_mp3_carry || !s_mp3 || !s_mp3_pcm) {
        ESP_LOGE(TAG, "Out of memory for playback buffers");
        vTaskDelete(NULL);
    }
    resampler_reset();
    uint32_t fmt_rate = REC_RATE;            // format of the current raw/WAV reply stream
    uint16_t fmt_ch = 1, fmt_bits = 16;

    for (;;) {
        play_item_t item;
        xQueueReceive(s_play_q, &item, portMAX_DELAY);

        audio_kind_t kind = classify_audio(item.data, item.len);
        if (kind == AUD_RAW && s_mp3_active) kind = AUD_MP3;   // continuation of an MP3 stream

        const uint8_t *b = item.data;
        ESP_LOGI(TAG, "Reply audio: %s, %u bytes, first bytes %02X %02X %02X %02X %02X %02X %02X %02X",
                 kind_name(kind), (unsigned)item.len,
                 item.len > 0 ? b[0] : 0, item.len > 1 ? b[1] : 0, item.len > 2 ? b[2] : 0, item.len > 3 ? b[3] : 0,
                 item.len > 4 ? b[4] : 0, item.len > 5 ? b[5] : 0, item.len > 6 ? b[6] : 0, item.len > 7 ? b[7] : 0);

        if (kind == AUD_OGG) {
            ESP_LOGW(TAG, "OGG/Opus reply audio isn't supported yet; ask the server for MP3 or WAV");
            heap_caps_free(item.data);
            continue;
        }

        ui_state(ST_SPEAKING);

        if (kind == AUD_MP3) {
            /* prepend any partial frame left over from the previous message */
            const uint8_t *in = item.data;
            size_t in_len = item.len;
            uint8_t *joined = NULL;
            if (s_mp3_active && s_mp3_carry_len > 0) {
                joined = heap_caps_malloc(s_mp3_carry_len + item.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (joined) {
                    memcpy(joined, s_mp3_carry, s_mp3_carry_len);
                    memcpy(joined + s_mp3_carry_len, item.data, item.len);
                    in = joined;
                    in_len = s_mp3_carry_len + item.len;
                }
            }
            s_mp3_carry_len = 0;
            if (!s_mp3_active) {
                mp3dec_init(s_mp3);
                s_mp3_active = true;
            }

            /* If more of this reply is still arriving, keep the tail for later: minimp3 would
             * skip a frame that is cut off at the end of the buffer. */
            vTaskDelay(pdMS_TO_TICKS(150));
            bool final_chunk = (uxQueueMessagesWaiting(s_play_q) == 0);

            size_t off = 0;
            int frames_decoded = 0;
            while (off < in_len) {
                if (!final_chunk && in_len - off < 4096) break;   // wait for the next message
                mp3dec_frame_info_t info;
                int samples = mp3dec_decode_frame(s_mp3, in + off, (int)(in_len - off), s_mp3_pcm, &info);
                if (info.frame_bytes == 0) break;            // need more data (or nothing usable)
                off += info.frame_bytes;
                if (samples <= 0) continue;                  // skipped ID3 tag / junk
                if (frames_decoded++ == 0) {
                    ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
                }
                play_pcm16((const uint8_t *)s_mp3_pcm, (size_t)samples, info.channels, (uint32_t)info.hz, out);
            }
            size_t left = in_len - off;
            if (left > 0 && left <= MP3_CARRY_MAX && s_mp3_carry) {
                memcpy(s_mp3_carry, in + off, left);
                s_mp3_carry_len = left;
            }
            if (joined) heap_caps_free(joined);
        } else {
            const uint8_t *pcm = item.data;
            size_t pcm_len = item.len;
            if (kind == AUD_WAV) {
                uint32_t r; uint16_t c, bits;
                if (!parse_wav(item.data, item.len, &pcm, &pcm_len, &r, &c, &bits)) {
                    ESP_LOGW(TAG, "Unreadable WAV header; skipping");
                    heap_caps_free(item.data);
                    continue;
                }
                fmt_rate = r; fmt_ch = c; fmt_bits = bits;
                ESP_LOGI(TAG, "WAV: %u Hz, %u ch, %u bit", (unsigned)r, c, bits);
            }
            /* AUD_RAW: raw PCM continuing the last known format */
            if (fmt_bits != 16 || fmt_ch == 0) {
                ESP_LOGW(TAG, "Unsupported audio (%u ch, %u bit); only 16-bit PCM plays", fmt_ch, fmt_bits);
                heap_caps_free(item.data);
                continue;
            }
            play_pcm16(pcm, pcm_len / ((size_t)fmt_ch * 2), fmt_ch, fmt_rate, out);
        }

        heap_caps_free(item.data);

        vTaskDelay(pdMS_TO_TICKS(150));
        if (uxQueueMessagesWaiting(s_play_q) == 0) {
            /* end of the reply: flush, add a little silence, reset decoder + resampler */
            out_flush(out);
            memset(out, 0, PLAY_FRAMES * 2 * sizeof(int16_t));
            size_t written = 0;
            bsp_extra_i2s_write(out, PLAY_FRAMES * 2 * sizeof(int16_t), &written, 1000);
            resampler_reset();

            s_mp3_active = false;
            s_mp3_carry_len = 0;
            fmt_rate = REC_RATE; fmt_ch = 1; fmt_bits = 16;
            ui_state(s_ws_connected ? ST_READY : ST_CONNECTING);
        }
    }
}

/* ----------------------------------------------------------------- Wi-Fi */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying", e ? e->reason : -1);
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\"", WIFI_SSID);
}

/* ------------------------------------------------------------- WebSocket */
static void ws_start(void)
{
    char key_enc[256], conv[37];
    url_encode(WS_API_KEY, key_enc, sizeof(key_enc));
    make_uuid(conv);

    snprintf(s_url, sizeof(s_url), "wss://%s:%d%s?x-api-key=%s&conversationId=%s",
             WS_HOST, WS_PORT, WS_PATH, key_enc, conv);
    /* never log the key */
    ESP_LOGI(TAG, "Connecting to wss://%s:%d%s (conversationId=%s)", WS_HOST, WS_PORT, WS_PATH, conv);

    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .cert_pem = server_ca_pem_start,
        .buffer_size = 4096,             // small: these buffers come from internal RAM
        .task_stack = 8192,
        .network_timeout_ms = 15000,
        .reconnect_timeout_ms = 5000,
        .ping_interval_sec = 20,
    };

    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);
}

/* ----------------------------------------------------------------- main */
void app_main(void)
{
    ESP_LOGI(TAG, "Voice chat for ESP32-S3-Touch-AMOLED-1.75C");

    lv_display_t *disp = bsp_display_start();
    if (disp) {
        bsp_display_backlight_on();
    }
    ui_build();
    ui_state(ST_BOOT);

    if (bsp_extra_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio codec init failed");
        ui_state(ST_ERROR);
        ui_label(s_lbl_reply, "Audio init failed");
        return;
    }
    int vol_set = 0;
    bsp_extra_codec_volume_set(SPEAKER_VOLUME, &vol_set);

    s_play_q = xQueueCreate(16, sizeof(play_item_t));
    xTaskCreatePinnedToCore(rec_task, "rec", 6 * 1024, NULL, 6, &s_rec_task, 1);
    /* the MP3 decoder needs ~16 KB of stack scratch: put this task's stack in PSRAM */
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "play", 32 * 1024, NULL, 6, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the playback task");
    }

    ui_state(ST_WIFI);
    wifi_start();
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    ui_state(ST_CONNECTING);
    ws_start();

    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Free memory: internal %u KB (largest DMA block %u KB), PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* supervisor: recover from replies that never come or text-only replies */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t now = esp_timer_get_time();

        if (s_state == ST_THINKING) {
            if (s_got_text && uxQueueMessagesWaiting(s_play_q) == 0 && now - s_last_rx_us > TEXT_ONLY_IDLE_US) {
                ui_state(ST_READY);
            } else if (now - s_turn_start_us > TURN_TIMEOUT_US) {
                ESP_LOGW(TAG, "No reply within %lld s", TURN_TIMEOUT_US / 1000000);
                ui_state(ST_READY);
                ui_label(s_lbl_reply, "No reply from the server");
            }
        }

        /* auto-off: idle and ready for too long -> paused */
        if (AUTO_OFF_MS > 0 && s_enabled && s_state == ST_READY &&
            now - s_last_activity_us > (int64_t)AUTO_OFF_MS * 1000) {
            ESP_LOGI(TAG, "Auto-off after %d s idle", AUTO_OFF_MS / 1000);
            s_enabled = false;
            ui_state(ST_READY);          /* shows as paused because s_enabled is false */
        }
    }
}
