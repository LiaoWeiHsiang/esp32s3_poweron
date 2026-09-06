/**
 * @file wireguard-platform-esp32.c
 * @brief ESP32 platform implementation for wireguard-lwip
 */

#include "wireguard-platform.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sys.h"
#include <string.h>

/* ============================================================================
 * Time Functions
 * ========================================================================== */

uint32_t wireguard_sys_now() {
    // Use lwIP's built-in time function
    return sys_now();
}

void wireguard_tai64n_now(uint8_t *output) {
    // TAI64N format: 8 bytes seconds + 4 bytes nanoseconds
    // For simplicity, use Unix epoch time
    uint64_t now_us = esp_timer_get_time();
    uint64_t seconds = now_us / 1000000ULL;
    uint32_t nanoseconds = (now_us % 1000000ULL) * 1000;

    // Log raw uptime before TAI offset (only every ~5s to avoid spam)
    static uint64_t last_log_s = 0;
    if (seconds - last_log_s >= 5) {
        printf("[TAI64N] uptime=%llu s, nano=%lu\n", (unsigned long long)seconds, (unsigned long)nanoseconds);
        last_log_s = seconds;
    }

    // TAI64 starts at 1970-01-01 00:00:10 TAI (Unix epoch + 10 seconds)
    // Add TAI offset: 2^62 + Unix time
    seconds += 0x400000000000000AULL;

    // Write in big-endian format
    output[0] = (seconds >> 56) & 0xFF;
    output[1] = (seconds >> 48) & 0xFF;
    output[2] = (seconds >> 40) & 0xFF;
    output[3] = (seconds >> 32) & 0xFF;
    output[4] = (seconds >> 24) & 0xFF;
    output[5] = (seconds >> 16) & 0xFF;
    output[6] = (seconds >> 8) & 0xFF;
    output[7] = seconds & 0xFF;

    output[8] = (nanoseconds >> 24) & 0xFF;
    output[9] = (nanoseconds >> 16) & 0xFF;
    output[10] = (nanoseconds >> 8) & 0xFF;
    output[11] = nanoseconds & 0xFF;
}

/* ============================================================================
 * Random Number Generation
 * ========================================================================== */

void wireguard_random_bytes(void *bytes, size_t size) {
    // Use ESP32 hardware RNG
    esp_fill_random(bytes, size);
}

/* ============================================================================
 * Load Management
 * ========================================================================== */

/* Below this much free internal heap, treat the device as under load so the
 * mac2/cookie handshake gate kicks in and cheap-to-send floods can't force
 * expensive DH/AEAD work. */
#define WG_LOAD_HEAP_FLOOR_BYTES (24 * 1024)
/* More than this many mac1-valid initiation/response messages within one
 * window is treated as a flood, even with heap to spare. */
#define WG_LOAD_INIT_WINDOW_MS   1000
#define WG_LOAD_INIT_THRESHOLD   20

bool wireguard_is_under_load() {
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < WG_LOAD_HEAP_FLOOR_BYTES) {
        return true;
    }

    static uint32_t window_start_ms = 0;
    static uint32_t window_count = 0;

    uint32_t now = wireguard_sys_now();
    if (now - window_start_ms >= WG_LOAD_INIT_WINDOW_MS) {
        window_start_ms = now;
        window_count = 0;
    }
    window_count++;
    return window_count > WG_LOAD_INIT_THRESHOLD;
}
