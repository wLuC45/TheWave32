#include "tw32_led.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define LED_GPIO          48
#define TICK_MS           50            /* 20 Hz frame rate */
#define BOOT_RAINBOW_MS   3000
#define BOOT_TICKS        (BOOT_RAINBOW_MS / TICK_MS)     /* 60 */

/* Idle cadence: 1 s ON + 2 s OFF per palette slot. */
#define IDLE_ON_TICKS     20
#define IDLE_OFF_TICKS    40
#define IDLE_SLOT         (IDLE_ON_TICKS + IDLE_OFF_TICKS)   /* 60 → 3 s/colour */

/* Running cadence: 200 ms ON + 200 ms OFF per slot. The colour still
 * advances every slot so the user sees both "fast" *and* "alternating
 * between colours". */
#define RUN_ON_TICKS      4
#define RUN_OFF_TICKS     4
#define RUN_SLOT          (RUN_ON_TICKS + RUN_OFF_TICKS)     /* 8 → 400 ms/colour */

static const char *TAG = "tw32_led";

typedef struct { uint8_t r, g, b; } rgb_t;

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_timer;
static volatile bool      s_inited;
static volatile bool      s_running;          /* false → idle, true → fast cycle */
static volatile uint32_t  s_tick;
static uint32_t           s_module_offset;   /* slot offset derived from slug */

typedef struct { uint16_t hue; uint8_t sat; } palette_t;

/* 13-colour palette: 12 hues spread across the wheel + white. */
static const palette_t s_palette[] = {
    {   0, 255 },  /* red          */
    {  25, 255 },  /* orange       */
    {  55, 255 },  /* yellow       */
    {  90, 230 },  /* chartreuse   */
    { 120, 255 },  /* green        */
    { 160, 230 },  /* spring green */
    { 180, 255 },  /* cyan         */
    { 210, 230 },  /* azure        */
    { 240, 255 },  /* blue         */
    { 270, 230 },  /* violet       */
    { 300, 255 },  /* magenta      */
    { 335, 220 },  /* pink         */
    {   0,   0 },  /* white        */
};
#define N_PALETTE (sizeof(s_palette) / sizeof(s_palette[0]))

static rgb_t hsv(uint16_t h, uint8_t s, uint8_t v)
{
    h %= 360;
    uint16_t sec  = h / 60;
    uint16_t off  = h - sec * 60;
    uint8_t hi    = v;
    uint8_t lo    = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t down  = (uint8_t)((uint16_t)v * (255u - (uint16_t)s * off / 60) / 255);
    uint8_t up    = (uint8_t)((uint16_t)v * (255u - (uint16_t)s * (60 - off) / 60) / 255);
    rgb_t c;
    switch (sec) {
        case 0: c = (rgb_t){hi,   up,   lo}; break;
        case 1: c = (rgb_t){down, hi,   lo}; break;
        case 2: c = (rgb_t){lo,   hi,   up}; break;
        case 3: c = (rgb_t){lo,   down, hi}; break;
        case 4: c = (rgb_t){up,   lo,   hi}; break;
        default:c = (rgb_t){hi,   lo,   down}; break;
    }
    return c;
}

static void emit(rgb_t c)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, c.r, c.g, c.b);
    led_strip_refresh(s_strip);
}

static uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    if (s) {
        for (const char *p = s; *p; ++p) h = ((h << 5) + h) + (uint8_t)*p;
    }
    return h;
}

static void tick_cb(void *arg)
{
    (void)arg;
    s_tick++;

    /* Phase 1 (first 3 s): boot rainbow. */
    if (s_tick < BOOT_TICKS) {
        uint16_t hue = (uint16_t)((s_tick * 360) / BOOT_TICKS);
        emit(hsv(hue, 255, 220));
        return;
    }

    /* Phase 2: ON-OFF colour cycle. Cadence depends on whether the
     * module is currently in its primary action. */
    uint32_t slot_len = s_running ? RUN_SLOT     : IDLE_SLOT;
    uint32_t on_len   = s_running ? RUN_ON_TICKS : IDLE_ON_TICKS;

    uint32_t local = s_tick - BOOT_TICKS + s_module_offset;
    uint32_t slot  = (local / slot_len) % N_PALETTE;
    uint32_t pos   = local % slot_len;

    rgb_t c;
    if (pos < on_len) {
        c = hsv(s_palette[slot].hue, s_palette[slot].sat, 255);
    } else {
        c = (rgb_t){0, 0, 0};
    }

    emit(c);
}

esp_err_t tw32_led_init(const char *module_name)
{
    if (s_inited) return ESP_OK;

    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "led init failed: 0x%x — heartbeat disabled", err);
        return err;
    }

    /* Hash the slug to offset the palette starting slot — each module
     * reads as a different colour at boot time. */
    uint32_t hash = djb2(module_name);
    s_module_offset = (hash % N_PALETTE) * IDLE_SLOT;

    emit((rgb_t){0, 0, 0});

    const esp_timer_create_args_t targs = {
        .callback = tick_cb,
        .name = "tw32-led",
    };
    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timer create failed: 0x%x", err);
        return err;
    }
    esp_timer_start_periodic(s_timer, TICK_MS * 1000ULL);

    s_inited = true;
    return ESP_OK;
}

void tw32_led_pulse(void)
{
    /* Intentionally empty — see header. */
}

void tw32_led_set_running(bool on)
{
    if (s_running != on) {
        s_running = on;
        /* Re-anchor the cycle so the visual change is immediate. */
        s_tick = BOOT_TICKS;
    }
}
