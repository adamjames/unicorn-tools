#include "board_config.hpp"
#include "http_server.hpp"
#include "secrets.h"

#include "cosmic_unicorn.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "tusb.h"

using namespace pimoroni;

// Global display object - uses set_pixel() directly, no PicoGraphics buffer
CosmicUnicorn cosmic_unicorn;

// Serial protocol constants
constexpr uint8_t SERIAL_CMD_FRAME = 0xFE;       // Full frame: 0xFE + 3072 bytes RGB
constexpr uint8_t SERIAL_CMD_DELTA = 0xFD;       // Delta: 0xFD + u16 count + (u16 idx, u8 r, u8 g, u8 b) * count
constexpr uint8_t SERIAL_CMD_BRIGHTNESS = 0xFC; // Brightness: 0xFC + u8 (0-255 mapped to 0.0-1.0)
constexpr uint8_t SERIAL_RESP_OK = 0x01;
constexpr uint8_t SERIAL_RESP_BUSY = 0x02;
constexpr uint8_t SERIAL_RESP_ERROR = 0x03;
constexpr size_t FRAME_SIZE = 32 * 32 * 3;  // 3072 bytes

// Serial frame buffer (separate from HTTP to avoid conflicts)
static uint8_t g_serial_frame[FRAME_SIZE];
static volatile bool g_serial_frame_pending = false;

static volatile bool wifi_running = true;
static volatile bool wifi_connected = false;

// Scan result storage
static volatile bool network_found = false;
static volatile uint32_t detected_auth = CYW43_AUTH_WPA2_AES_PSK;

static int scan_callback(void* env, const cyw43_ev_scan_result_t* result) {
    if (result && result->ssid_len > 0) {
        if (result->ssid_len == strlen(WIFI_SSID) &&
            memcmp(result->ssid, WIFI_SSID, result->ssid_len) == 0) {

            network_found = true;

            uint8_t auth = result->auth_mode;
            printf("Found '%s' (RSSI: %d, Channel: %d, Auth: 0x%02x)\n",
                   WIFI_SSID, result->rssi, result->channel, auth);

            if (auth == 0) {
                detected_auth = CYW43_AUTH_OPEN;
            } else if (auth & 0x04) {
                detected_auth = CYW43_AUTH_WPA3_WPA2_AES_PSK;
            } else if (auth & 0x02) {
                detected_auth = CYW43_AUTH_WPA_TKIP_PSK;
            } else {
                detected_auth = CYW43_AUTH_WPA3_WPA2_AES_PSK;
            }

            return 1;
        }
    }
    return 0;
}

// Boot animation state
static volatile bool http_server_ready = false;

enum BootStage {
    BOOT_STAGE_INIT = 0,
    BOOT_STAGE_WIFI_SCAN,
    BOOT_STAGE_WIFI_CONNECT,
    BOOT_STAGE_HTTP_READY,
};
static volatile BootStage boot_stage = BOOT_STAGE_INIT;

// Read exact number of bytes from USB CDC with timeout
static bool serial_read_exact(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    size_t received = 0;
    while (received < len) {
        if (to_ms_since_boot(get_absolute_time()) - start > timeout_ms) {
            return false;  // Timeout
        }
        if (tud_cdc_available()) {
            uint32_t chunk = tud_cdc_read(buf + received, len - received);
            received += chunk;
        }
        tud_task();  // Keep USB stack running
    }
    return true;
}

// Process incoming serial commands - returns true if a frame was received
static bool process_serial_input(CosmicUnicorn& display, float& brightness) {
    if (!tud_cdc_connected() || !tud_cdc_available()) {
        return false;
    }

    uint8_t cmd;
    if (tud_cdc_read(&cmd, 1) != 1) {
        return false;
    }

    if (cmd == SERIAL_CMD_FRAME) {
        // Full frame: read 3072 bytes
        if (g_serial_frame_pending) {
            // Previous frame not yet drawn - reject
            tud_cdc_write_char(SERIAL_RESP_BUSY);
            tud_cdc_write_flush();
            // Drain incoming data to stay in sync
            serial_read_exact(g_serial_frame, FRAME_SIZE, 100);
            return false;
        }

        if (!serial_read_exact(g_serial_frame, FRAME_SIZE, 100)) {
            tud_cdc_write_char(SERIAL_RESP_ERROR);
            tud_cdc_write_flush();
            return false;
        }

        g_serial_frame_pending = true;
        tud_cdc_write_char(SERIAL_RESP_OK);
        tud_cdc_write_flush();
        return true;
    }
    else if (cmd == SERIAL_CMD_DELTA) {
        // Delta frame: u16 count + entries
        if (g_serial_frame_pending) {
            tud_cdc_write_char(SERIAL_RESP_BUSY);
            tud_cdc_write_flush();
            return false;
        }

        uint8_t count_buf[2];
        if (!serial_read_exact(count_buf, 2, 50)) {
            tud_cdc_write_char(SERIAL_RESP_ERROR);
            tud_cdc_write_flush();
            return false;
        }
        uint16_t count = count_buf[0] | (count_buf[1] << 8);

        if (count > 1024) {
            tud_cdc_write_char(SERIAL_RESP_ERROR);
            tud_cdc_write_flush();
            return false;
        }

        // Read delta entries directly and apply to frame buffer
        for (uint16_t i = 0; i < count; i++) {
            uint8_t entry[5];  // u16 index + RGB
            if (!serial_read_exact(entry, 5, 50)) {
                tud_cdc_write_char(SERIAL_RESP_ERROR);
                tud_cdc_write_flush();
                return false;
            }
            uint16_t idx = entry[0] | (entry[1] << 8);
            if (idx < 1024) {
                size_t offset = idx * 3;
                g_serial_frame[offset] = entry[2];      // R
                g_serial_frame[offset + 1] = entry[3];  // G
                g_serial_frame[offset + 2] = entry[4];  // B
            }
        }

        g_serial_frame_pending = true;
        tud_cdc_write_char(SERIAL_RESP_OK);
        tud_cdc_write_flush();
        return true;
    }
    else if (cmd == SERIAL_CMD_BRIGHTNESS) {
        uint8_t val;
        if (!serial_read_exact(&val, 1, 50)) {
            tud_cdc_write_char(SERIAL_RESP_ERROR);
            tud_cdc_write_flush();
            return false;
        }
        brightness = val / 255.0f;
        display.set_brightness(brightness);
        tud_cdc_write_char(SERIAL_RESP_OK);
        tud_cdc_write_flush();
        return false;  // Not a frame
    }

    // Unknown command
    tud_cdc_write_char(SERIAL_RESP_ERROR);
    tud_cdc_write_flush();
    return false;
}

// Draw serial frame to display
static void draw_serial_frame(CosmicUnicorn& display) {
    if (!g_serial_frame_pending) return;

    int width = CosmicUnicorn::WIDTH;
    int height = CosmicUnicorn::HEIGHT;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t idx = (y * width + x) * 3;
            display.set_pixel(x, y,
                g_serial_frame[idx],
                g_serial_frame[idx + 1],
                g_serial_frame[idx + 2]);
        }
    }

    g_serial_frame_pending = false;
}

// HSV to RGB helper (hue 0-255, full saturation/value)
static void hue_to_rgb(uint8_t hue, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t region = hue / 43;
    uint8_t remainder = (hue - (region * 43)) * 6;
    switch (region) {
        case 0: r = 255; g = remainder; b = 0; break;
        case 1: r = 255 - remainder; g = 255; b = 0; break;
        case 2: r = 0; g = 255; b = remainder; break;
        case 3: r = 0; g = 255 - remainder; b = 255; break;
        case 4: r = remainder; g = 0; b = 255; break;
        default: r = 255; g = 0; b = 255 - remainder; break;
    }
}

// Simplified boot animation: rainbow gradient that fills based on boot progress
// Uses set_pixel() directly - no PicoGraphics intermediate buffer
static void show_boot_pattern(float progress) {
    static uint16_t frame = 0;
    int width = CosmicUnicorn::WIDTH;
    int height = CosmicUnicorn::HEIGHT;

    // Fill columns left-to-right based on progress (0.0 to 1.0)
    int fill_cols = (int)(progress * width);
    if (fill_cols > width) fill_cols = width;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (x < fill_cols) {
                int hue_val = (x * 8 + y * 4 + frame * 2) % 256;
                uint8_t r, g, b;
                hue_to_rgb(hue_val, r, g, b);
                cosmic_unicorn.set_pixel(x, y, r / 3, g / 3, b / 3);
            } else {
                cosmic_unicorn.set_pixel(x, y, 0, 0, 0);
            }
        }
    }

    frame++;
}

static float get_boot_progress() {
    switch (boot_stage) {
        case BOOT_STAGE_INIT:         return 0.1f;
        case BOOT_STAGE_WIFI_SCAN:    return 0.3f;
        case BOOT_STAGE_WIFI_CONNECT: return 0.6f;
        case BOOT_STAGE_HTTP_READY:   return 1.0f;
        default:                      return 1.0f;
    }
}

// Warmup animation callback
static volatile bool warmup_complete = false;

static void warmup_animate() {
    if (!warmup_complete) {
        show_boot_pattern(1.0f);
    }
}

void core1_wifi_task() {
    if (cyw43_arch_init()) {
        printf("WiFi init failed!\n");
        return;
    }

    printf("WiFi chip initialized\n");
    cyw43_arch_enable_sta_mode();

    printf("Scanning for WiFi network '%s'...\n", WIFI_SSID);
    boot_stage = BOOT_STAGE_WIFI_SCAN;

    while (!network_found && wifi_running) {
        cyw43_wifi_scan_options_t scan_options = {0};
        int scan_err = cyw43_wifi_scan(&cyw43_state, &scan_options, nullptr, scan_callback);

        if (scan_err == 0) {
            while (cyw43_wifi_scan_active(&cyw43_state) && !network_found) {
                cyw43_arch_poll();
                sleep_ms(10);
            }
        } else {
            printf("Failed to start scan: %d\n", scan_err);
        }

        if (!network_found) {
            printf("Network '%s' not found, retrying in 3 seconds...\n", WIFI_SSID);
            for (int i = 0; i < 6 && wifi_running; i++) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i % 2);
                sleep_ms(500);
            }
        }
    }

    if (!wifi_running) return;

    boot_stage = BOOT_STAGE_WIFI_CONNECT;

    // Try WPA2 first (simpler handshake, better on weak signal), then detected auth
    uint32_t auth_types[] = {
        CYW43_AUTH_WPA2_AES_PSK,
        detected_auth,
    };
    int num_auth = (detected_auth != CYW43_AUTH_WPA2_AES_PSK) ? 2 : 1;

    // Retry connection with shorter timeout since we have retry logic
    int result = -1;
    int attempt = 0;
    while (result != 0 && wifi_running) {
        uint32_t auth = auth_types[attempt % num_auth];
        attempt++;
        printf("Connecting to '%s' (auth=0x%08x, attempt %d)...\n",
            WIFI_SSID, (unsigned int)auth, attempt);

        result = cyw43_arch_wifi_connect_timeout_ms(
            WIFI_SSID, WIFI_PASSWORD, auth, 15000);

        if (result != 0) {
            printf("WiFi connect failed (error %d), retrying in 3s...\n", result);
            for (int i = 0; i < 6 && wifi_running; i++) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i % 2);
                sleep_ms(500);
            }
        }
    }

    if (!wifi_running) return;

    wifi_connected = true;
    printf("WiFi connected!\n");

    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);

    http_server::resolve_allowed_hosts();

    const ip4_addr_t* ip = netif_ip4_addr(netif_default);
    printf("IP Address: %s\n", ip4addr_ntoa(ip));

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    if (http_server::init(80)) {
        printf("HTTP server started on port 80\n");
        printf("Access the device at: http://%s/\n", ip4addr_ntoa(ip));

        boot_stage = BOOT_STAGE_HTTP_READY;

        printf("Warming up HTTP server...\n");
        http_server::warmup(warmup_animate);
        printf("HTTP server ready\n");

        warmup_complete = true;
        http_server_ready = true;
    } else {
        printf("Failed to start HTTP server!\n");
    }

    // Keep WiFi stack running
    bool was_connected = false;
    uint32_t last_led_update = 0;
    uint32_t last_link_check = 0;
    while (wifi_running) {
        cyw43_arch_poll();

        int connections = http_server::get_active_connections();

        if (connections > 0) {
            // Active streaming - skip housekeeping for maximum responsiveness
            if (!was_connected) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                was_connected = true;
            }
            sleep_ms(2);
            continue;
        }

        // Idle - do housekeeping
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if (now - last_link_check >= 5000) {
            last_link_check = now;
            int link_status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link_status != CYW43_LINK_JOIN) {
                printf("WiFi link lost (status=%d), reconnecting...\n", link_status);
                cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                    detected_auth, 10000);
            }
        }

        if (now - last_led_update >= 100) {
            last_led_update = now;
            if (was_connected) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                was_connected = false;
            }
        }

        sleep_ms(2);
    }

    http_server::stop();
}

int main() {
    set_sys_clock_khz(150000, true);

    stdio_init_all();
    sleep_ms(2000);

    printf("UnicornLEDStreamLite starting...\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);

    const BoardInfo& board = detect_board();
    printf("Detected board: %s (%dx%d)\n", board.name, board.width, board.height);

    cosmic_unicorn.init();
    cosmic_unicorn.set_brightness(0.5f);
    printf("Display initialized\n");

    // Show initial boot pattern
    show_boot_pattern(0.1f);

    // Launch WiFi task on core 1
    multicore_launch_core1(core1_wifi_task);

    printf("Running boot animation...\n");

    int width = CosmicUnicorn::WIDTH;
    int height = CosmicUnicorn::HEIGHT;

    float current_progress = 0.1f;

    // Enable watchdog
    watchdog_enable(2000, true);
    printf("Watchdog enabled (2s timeout)\n");

    // Button state
    uint32_t last_button_time = 0;
    constexpr uint32_t BUTTON_DEBOUNCE_MS = 200;
    constexpr float BRIGHTNESS_STEP = 0.05f;
    constexpr float BRIGHTNESS_MIN = 0.05f;
    constexpr float BRIGHTNESS_MAX = 1.0f;
    float current_brightness = 0.5f;
    bool display_sleeping = false;
    bool external_frame_mode = false;
    uint32_t last_frame_time = 0;

    uint16_t housekeeping_counter = 0;
    constexpr uint16_t HOUSEKEEPING_INTERVAL = 250;  // Every 250 iterations (~500ms at 2ms sleep)

    while (wifi_running) {
        watchdog_update();

        // Fast path: active streaming - minimal housekeeping
        if (external_frame_mode) {
            // Check for serial frames first (lower latency than WiFi)
            tud_task();
            if (process_serial_input(cosmic_unicorn, current_brightness)) {
                draw_serial_frame(cosmic_unicorn);
            }

            if (http_server::has_pending_brightness()) {
                float new_brightness = http_server::get_pending_brightness();
                current_brightness = new_brightness;
                cosmic_unicorn.set_brightness(new_brightness);
            }

            if (http_server::has_pending_frame()) {
                http_server::acquire_frame_lock();
                uint8_t* frame_data = http_server::get_pending_frame_buffer();
                uint16_t delta_count = http_server::get_delta_count();

                if (delta_count > 0) {
                    uint16_t* indices = http_server::get_delta_indices();
                    for (uint16_t i = 0; i < delta_count; i++) {
                        uint16_t idx = indices[i];
                        int x = idx % width;
                        int y = idx / width;
                        size_t offset = idx * 3;
                        cosmic_unicorn.set_pixel(x, y,
                            frame_data[offset], frame_data[offset+1], frame_data[offset+2]);
                    }
                } else {
                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                            size_t idx = (y * width + x) * 3;
                            cosmic_unicorn.set_pixel(x, y,
                                frame_data[idx], frame_data[idx+1], frame_data[idx+2]);
                        }
                    }
                }

                http_server::release_frame_lock();
                http_server::clear_pending_frame();
            }

            // Periodic housekeeping during streaming
            if (++housekeeping_counter >= HOUSEKEEPING_INTERVAL) {
                housekeeping_counter = 0;

                if (http_server::reboot_requested()) {
                    bool to_bootloader = http_server::reboot_to_bootloader();
                    wifi_running = false;
                    sleep_ms(500);
                    watchdog_disable();
                    multicore_reset_core1();
                    if (to_bootloader) reset_usb_boot(0, 0);
                    else watchdog_reboot(0, 0, 0);
                }

                if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_BRIGHTNESS_UP)) {
                    current_brightness = fminf(current_brightness + BRIGHTNESS_STEP, BRIGHTNESS_MAX);
                    cosmic_unicorn.set_brightness(current_brightness);
                }
                else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_BRIGHTNESS_DOWN)) {
                    current_brightness = fmaxf(current_brightness - BRIGHTNESS_STEP, BRIGHTNESS_MIN);
                    cosmic_unicorn.set_brightness(current_brightness);
                }
                else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_SLEEP)) {
                    display_sleeping = !display_sleeping;
                    cosmic_unicorn.set_brightness(display_sleeping ? 0.0f : current_brightness);
                }
            }

            sleep_ms(2);
            continue;
        }

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Check for reboot request
        if (http_server::reboot_requested()) {
            bool to_bootloader = http_server::reboot_to_bootloader();
            printf("Reboot requested from Core 0, waiting for Core 1 to flush...\n");

            wifi_running = false;
            sleep_ms(500);
            watchdog_disable();
            multicore_reset_core1();

            if (to_bootloader) {
                printf("Rebooting into USB bootloader...\n");
                reset_usb_boot(0, 0);
            } else {
                printf("Rebooting...\n");
                watchdog_reboot(0, 0, 0);
            }
        }

        // Check buttons (with debounce)
        if (now - last_button_time > BUTTON_DEBOUNCE_MS) {
            if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_BRIGHTNESS_UP)) {
                current_brightness = fminf(current_brightness + BRIGHTNESS_STEP, BRIGHTNESS_MAX);
                cosmic_unicorn.set_brightness(current_brightness);
                printf("Brightness: %.0f%%\n", current_brightness * 100);
                last_button_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_BRIGHTNESS_DOWN)) {
                current_brightness = fmaxf(current_brightness - BRIGHTNESS_STEP, BRIGHTNESS_MIN);
                cosmic_unicorn.set_brightness(current_brightness);
                printf("Brightness: %.0f%%\n", current_brightness * 100);
                last_button_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_SLEEP)) {
                display_sleeping = !display_sleeping;
                if (display_sleeping) {
                    cosmic_unicorn.set_brightness(0.0f);
                    printf("Display sleeping\n");
                } else {
                    cosmic_unicorn.set_brightness(current_brightness);
                    printf("Display waking\n");
                }
                last_button_time = now;
            }
        }

        // Process serial input (can trigger streaming mode)
        tud_task();
        if (process_serial_input(cosmic_unicorn, current_brightness)) {
            draw_serial_frame(cosmic_unicorn);
            external_frame_mode = true;  // Switch to streaming mode
            printf("Serial streaming started\n");
        }

        // Process pending HTTP brightness
        if (http_server::has_pending_brightness()) {
            float new_brightness = http_server::get_pending_brightness();
            current_brightness = new_brightness;
            cosmic_unicorn.set_brightness(new_brightness);
        }

        // Process pending HTTP frame - write directly to bitstream via set_pixel()
        if (http_server::has_pending_frame()) {
            // Acquire lock to safely read frame data
            http_server::acquire_frame_lock();
            uint8_t* frame_data = http_server::get_pending_frame_buffer();
            uint16_t delta_count = http_server::get_delta_count();

            if (delta_count > 0) {
                // Delta update: only update changed pixels
                uint16_t* indices = http_server::get_delta_indices();
                for (uint16_t i = 0; i < delta_count; i++) {
                    uint16_t idx = indices[i];
                    int x = idx % width;
                    int y = idx / width;
                    size_t offset = idx * 3;
                    cosmic_unicorn.set_pixel(x, y,
                        frame_data[offset], frame_data[offset+1], frame_data[offset+2]);
                }
            } else {
                // Full frame: update all pixels
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        size_t idx = (y * width + x) * 3;
                        cosmic_unicorn.set_pixel(x, y,
                            frame_data[idx], frame_data[idx+1], frame_data[idx+2]);
                    }
                }
            }

            http_server::release_frame_lock();
            http_server::clear_pending_frame();
            external_frame_mode = true;
        }

        // Boot animation: before server is ready
        if (!warmup_complete) {
            if (now - last_frame_time >= 33) {
                float target = get_boot_progress();
                float diff = target - current_progress;
                if (diff > 0.01f) {
                    current_progress += diff * 0.1f;
                } else {
                    current_progress = target;
                }
                show_boot_pattern(current_progress);
                last_frame_time = now;
            }
            sleep_ms(10);
        }
        // Idle: no external frames being received
        else if (!external_frame_mode) {
            if (now - last_frame_time >= 33) {
                show_boot_pattern(1.0f);
                last_frame_time = now;
            }
            sleep_ms(10);
        }
        // Streaming mode: just yield
        else {
            sleep_ms(2);
        }
    }

    wifi_running = false;
    return 0;
}
