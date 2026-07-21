#pragma once

/*
 * Heartbeat LED driver for TheWave32 modules.
 *
 * Drives the WS2812B / SK6812 RGB LED on GPIO 48 (every common
 * ESP32-S3 dev kit, including the v0rtex board, exposes one).
 *
 * Behaviour:
 *
 *   1. Boot rainbow — 3 s hue sweep on first boot. Confirms the LED
 *      is wired and the colour pipeline works.
 *
 *   2. Discrete colour cycle — afterwards the LED steps through a
 *      13-colour palette (red, orange, yellow, chartreuse, green,
 *      spring-green, cyan, azure, blue, violet, magenta, pink,
 *      white). Each colour: 1 s ON at full brightness, then 2 s
 *      fully OFF, then the next colour. One full loop = 39 s.
 *
 *   3. Per-module starting offset — `tw32_led_init` hashes the
 *      module slug to pick the slot the cycle starts in, so two
 *      boards running different modules side-by-side never beat
 *      the same hue at the same moment.
 *
 * The cadence is strictly time-driven and does NOT speed up while
 * a module is doing its primary work. An earlier revision pulsed
 * the LED on every JSON event, which made it flicker fast under
 * heavy traffic and hid the per-colour beat — that path has been
 * removed.
 *
 * Boards without GPIO 48 RGB log a warning and skip the timer —
 * no panic, no functional impact.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire up the LED + start the heartbeat. ``module_name`` is folded
 * into the colour palette so every module has a distinct look on the
 * RGB LED. Idempotent: only the first call configures hardware. */
esp_err_t tw32_led_init(const char *module_name);

/* Kept for ABI / source compatibility with earlier revisions —
 * currently a no-op. The activity-pulse design caused fast flicker
 * during heavy event traffic; the cycle is now strictly time-driven. */
void tw32_led_pulse(void);

/* Switch the LED into a faster cadence (≈ 200 ms ON / 200 ms OFF,
 * still cycling through the palette) so the user can see at a
 * glance that the module is doing its primary action. ``true``
 * during ``start`` / ``scan`` / ``attack`` / ``connect`` /
 * ``replay``; ``false`` during ``stop`` / ``disconnect`` / ``clear``.
 * `tw32_cli_ack_ok` calls this automatically so individual modules
 * don't need to invoke it themselves. */
void tw32_led_set_running(bool on);

#ifdef __cplusplus
}
#endif
