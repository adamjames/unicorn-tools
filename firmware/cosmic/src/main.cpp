#include "board_config.hpp"
#include "http_server.hpp"
#include "shader_lua.hpp"
#include "builtin_shaders.hpp"
#include "secrets.h"

#include "cosmic_unicorn.hpp"
#include "pico_graphics.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"

using namespace pimoroni;

// Global display objects (extern'd by http_server.cpp)
CosmicUnicorn cosmic_unicorn;
PicoGraphics_PenRGB888 graphics(CosmicUnicorn::WIDTH, CosmicUnicorn::HEIGHT, nullptr);

static volatile bool wifi_running = true;
static volatile bool wifi_connected = false;

// Scan result storage
static volatile bool scan_complete = false;
static volatile bool network_found = false;
static volatile uint32_t detected_auth = CYW43_AUTH_WPA2_AES_PSK;  // Default fallback

// Callback for WiFi scan results
static int scan_callback(void* env, const cyw43_ev_scan_result_t* result) {
    if (result && result->ssid_len > 0) {
        // Check if this is our target network
        if (result->ssid_len == strlen(WIFI_SSID) &&
            memcmp(result->ssid, WIFI_SSID, result->ssid_len) == 0) {
            
            network_found = true;
            
            // Decode auth_mode to CYW43_AUTH_* constant
            // auth_mode from scan is a bitmask:
            //   0x01 = WEP
            //   0x02 = WPA
            //   0x04 = WPA2 (also covers WPA3 transition networks)
            uint8_t auth = result->auth_mode;
            
            printf("Found '%s' (RSSI: %d, Channel: %d, Auth: 0x%02x)\n", 
                   WIFI_SSID, result->rssi, result->channel, auth);
            
            if (auth == 0) {
                detected_auth = CYW43_AUTH_OPEN;
                printf("  -> Open network (no encryption)\n");
            } else if (auth & 0x04) {
                // WPA2 detected - use WPA3/WPA2 mixed mode for compatibility
                detected_auth = CYW43_AUTH_WPA3_WPA2_AES_PSK;
                printf("  -> WPA2/WPA3 detected, using mixed mode\n");
            } else if (auth & 0x02) {
                // WPA only (legacy)
                detected_auth = CYW43_AUTH_WPA_TKIP_PSK;
                printf("  -> WPA (legacy) detected\n");
            } else if (auth & 0x01) {
                // WEP - not supported well, try anyway
                detected_auth = CYW43_AUTH_OPEN;
                printf("  -> WEP detected (limited support)\n");
            } else {
                // Unknown, try WPA3/WPA2 mixed
                detected_auth = CYW43_AUTH_WPA3_WPA2_AES_PSK;
                printf("  -> Unknown auth (0x%02x), trying WPA3/WPA2 mixed\n", auth);
            }
            
            return 1;  // Stop scanning
        }
    }
    return 0;  // Continue scanning
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

static float get_boot_radius_target() {
    switch (boot_stage) {
        case BOOT_STAGE_INIT:         return 0.1f;
        case BOOT_STAGE_WIFI_SCAN:    return 0.3f;
        case BOOT_STAGE_WIFI_CONNECT: return 0.6f;
        case BOOT_STAGE_HTTP_READY:   return 1.0f;
        default:                      return 1.0f;
    }
}

// Forward declaration
static void show_test_pattern(float radius);

// Warmup animation state
static uint32_t warmup_start_time = 0;
static float warmup_current_radius = 0.6f;
static volatile bool warmup_complete = false;
static constexpr float WARMUP_SMALL_SIZE = 0.7f;    // Size during cycling/filling
static constexpr float WARMUP_FINAL_SIZE = 1.0f;    // Final scaled-up size
static constexpr uint32_t WARMUP_CYCLE_MS = 600;    // Time to cycle through faces
static constexpr uint32_t WARMUP_FILL_MS = 400;     // Time to fill faces sequentially
static constexpr uint32_t WARMUP_SCALE_MS = 300;    // Time to scale up

// Warmup face animation state (visible to show_test_pattern)
static int warmup_cycle_face = -1;    // Which single face to show (-1 = none)
static int warmup_filled_faces = 0;   // How many faces are filled (0-6)

// Animation callback for warmup - cycle faces, fill sequentially, scale up
static void warmup_animate() {
    // Don't animate if warmup already complete (Core 0 takes over)
    if (warmup_complete) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    if (warmup_start_time == 0) {
        warmup_start_time = now;
    }

    uint32_t elapsed = now - warmup_start_time;

    if (elapsed < WARMUP_CYCLE_MS) {
        // Phase 1: Cycle through faces one at a time (spinner)
        warmup_current_radius = WARMUP_SMALL_SIZE;
        float t = (float)elapsed / WARMUP_CYCLE_MS;
        warmup_cycle_face = (int)(t * 12) % 6;  // Cycle through twice
        warmup_filled_faces = 0;
    } else if (elapsed < WARMUP_CYCLE_MS + WARMUP_FILL_MS) {
        // Phase 2: Fill faces one by one
        warmup_current_radius = WARMUP_SMALL_SIZE;
        warmup_cycle_face = -1;
        float t = (float)(elapsed - WARMUP_CYCLE_MS) / WARMUP_FILL_MS;
        warmup_filled_faces = 1 + (int)(t * 5.99f);  // 1 to 6
        if (warmup_filled_faces > 6) warmup_filled_faces = 6;
    } else {
        // Phase 3: Scale up with all faces filled
        warmup_cycle_face = -1;
        warmup_filled_faces = 6;
        float t = (float)(elapsed - WARMUP_CYCLE_MS - WARMUP_FILL_MS) / WARMUP_SCALE_MS;
        if (t >= 1.0f) {
            t = 1.0f;
            warmup_complete = true;
        }
        // Ease-out for scale up
        float ease = 1.0f - (1.0f - t) * (1.0f - t);
        warmup_current_radius = WARMUP_SMALL_SIZE + (WARMUP_FINAL_SIZE - WARMUP_SMALL_SIZE) * ease;
    }

    show_test_pattern(warmup_current_radius);
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

// Draw line with rainbow color based on position
static void draw_rainbow_line(float x0, float y0, float x1, float y1, uint16_t frame) {
    int ix0 = (int)(x0 + 0.5f), iy0 = (int)(y0 + 0.5f);
    int ix1 = (int)(x1 + 0.5f), iy1 = (int)(y1 + 0.5f);
    int dx = abs(ix1 - ix0), dy = -abs(iy1 - iy0);
    int sx = ix0 < ix1 ? 1 : -1, sy = iy0 < iy1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if (ix0 >= 0 && ix0 < 32 && iy0 >= 0 && iy0 < 32) {
            int hue_val = (ix0 * 8 + iy0 * 8 + frame * 4) % 256;
            if (hue_val < 0) hue_val += 256;
            uint8_t r, g, b;
            hue_to_rgb(hue_val, r, g, b);
            graphics.set_pen(r, g, b);
            graphics.pixel(Point(ix0, iy0));
        }
        if (ix0 == ix1 && iy0 == iy1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ix0 += sx; }
        if (e2 <= dx) { err += dx; iy0 += sy; }
    }
}

// Simple test pattern: rotating square (2D) or cube (3D when WiFi connected)
static void show_test_pattern(float size) {
    static uint16_t frame = 0;
    static float rotation_offset = 0.0f;  // Angle when warmup completed
    static uint16_t frame_at_warmup = 0;  // Frame when warmup completed
    static bool captured_warmup = false;

    int width = CosmicUnicorn::WIDTH;
    int height = CosmicUnicorn::HEIGHT;
    float cx = (width - 1) / 2.0f;
    float cy = (height - 1) / 2.0f;

    // Clear to black
    graphics.set_pen(0, 0, 0);
    graphics.clear();

    // Rotation angle - capture offset when warmup completes for smooth transition
    float rotation;
    if (warmup_complete) {
        if (!captured_warmup) {
            rotation_offset = frame * 0.02f;  // Current angle at fast speed
            frame_at_warmup = frame;
            captured_warmup = true;
        }
        rotation = rotation_offset + (frame - frame_at_warmup) * 0.01f;
    } else {
        rotation = frame * 0.02f;
    }
    float cos_rot = cosf(rotation);
    float sin_rot = sinf(rotation);

    // Half-size of shape (smaller cube)
    float s = size * 7.0f;

    // Check if WiFi is connected (show 3D cube) or not (show 2D square)
    bool show_cube = (boot_stage >= BOOT_STAGE_WIFI_CONNECT);

    if (show_cube) {
        // 3D cube with perspective projection
        float persp = 2.5f;

        // 8 vertices of a cube centered at origin
        float verts[8][3] = {
            {-s, -s, -s}, { s, -s, -s}, { s,  s, -s}, {-s,  s, -s},
            {-s, -s,  s}, { s, -s,  s}, { s,  s,  s}, {-s,  s,  s}
        };

        // Rotate and project vertices, snap to integers to avoid sub-pixel jitter
        int proj[8][2];
        float rotated[8][3];
        for (int i = 0; i < 8; i++) {
            float x = verts[i][0] * cos_rot - verts[i][2] * sin_rot;
            float z = verts[i][0] * sin_rot + verts[i][2] * cos_rot;
            float y = verts[i][1];
            float y2 = y * cosf(rotation * 0.7f) - z * sinf(rotation * 0.7f);
            float z2 = y * sinf(rotation * 0.7f) + z * cosf(rotation * 0.7f);
            rotated[i][0] = x; rotated[i][1] = y2; rotated[i][2] = z2;
            float scale = persp / (persp + z2 * 0.03f);
            proj[i][0] = (int)(cx + x * scale + 0.5f);
            proj[i][1] = (int)(cy + y2 * scale + 0.5f);
        }

        // 6 faces of cube (vertex indices, ordered for backface culling)
        int faces[6][4] = {
            {0, 1, 2, 3}, {4, 7, 6, 5},  // back, front
            {0, 4, 5, 1}, {2, 6, 7, 3},  // bottom, top
            {0, 3, 7, 4}, {1, 5, 6, 2}   // left, right
        };

        // Fill faces based on warmup state
        // Before HTTP ready: wireframe only
        // During warmup cycle phase: show one visible face at a time (spinner)
        // During warmup fill phase: fill faces sequentially
        // After warmup: all faces filled
        int faces_to_fill = 0;
        int cycle_index = -1;  // Which visible face to show (0-2, since max 3 visible)
        if (boot_stage >= BOOT_STAGE_HTTP_READY) {
            if (warmup_complete) {
                faces_to_fill = 6;
            } else if (warmup_cycle_face >= 0) {
                cycle_index = warmup_cycle_face % 3;  // Cycle through 3 visible faces
            } else {
                faces_to_fill = warmup_filled_faces;
            }
        }
        int visible_count = 0;
        int filled = 0;
        for (int f = 0; f < 6; f++) {
            // Calculate face normal for backface culling
            int ax = proj[faces[f][1]][0] - proj[faces[f][0]][0];
            int ay = proj[faces[f][1]][1] - proj[faces[f][0]][1];
            int bx = proj[faces[f][2]][0] - proj[faces[f][0]][0];
            int by = proj[faces[f][2]][1] - proj[faces[f][0]][1];
            if (ax * by - ay * bx <= 0) continue;  // backface

            // Skip faces we're not filling
            if (cycle_index >= 0) {
                // Cycling mode: only draw one visible face at a time
                if (visible_count != cycle_index) {
                    visible_count++;
                    continue;
                }
                visible_count++;
            } else if (filled >= faces_to_fill) {
                // Sequential fill mode: stop when we've filled enough
                break;
            }

            filled++;

            // Get bounding box of face
            int minX = 32, maxX = 0, minY = 32, maxY = 0;
            for (int v = 0; v < 4; v++) {
                if (proj[faces[f][v]][0] < minX) minX = proj[faces[f][v]][0];
                if (proj[faces[f][v]][0] > maxX) maxX = proj[faces[f][v]][0];
                if (proj[faces[f][v]][1] < minY) minY = proj[faces[f][v]][1];
                if (proj[faces[f][v]][1] > maxY) maxY = proj[faces[f][v]][1];
            }

            // Get original (unrotated) vertex positions for this face
            float v0x = verts[faces[f][0]][0], v0y = verts[faces[f][0]][1], v0z = verts[faces[f][0]][2];
            float v1x = verts[faces[f][1]][0], v1y = verts[faces[f][1]][1], v1z = verts[faces[f][1]][2];
            float v3x = verts[faces[f][3]][0], v3y = verts[faces[f][3]][1], v3z = verts[faces[f][3]][2];

            // Projected corners for interpolation
            int p0x = proj[faces[f][0]][0], p0y = proj[faces[f][0]][1];
            int p1x = proj[faces[f][1]][0], p1y = proj[faces[f][1]][1];
            int p3x = proj[faces[f][3]][0], p3y = proj[faces[f][3]][1];

            // Fill pixels inside face
            for (int py = minY; py <= maxY && py < 32; py++) {
                if (py < 0) continue;
                for (int px = minX; px <= maxX && px < 32; px++) {
                    if (px < 0) continue;
                    // Point-in-quad test using cross products
                    bool inside = true;
                    for (int e = 0; e < 4 && inside; e++) {
                        int n = (e + 1) % 4;
                        int ex = proj[faces[f][n]][0] - proj[faces[f][e]][0];
                        int ey = proj[faces[f][n]][1] - proj[faces[f][e]][1];
                        int px2 = px - proj[faces[f][e]][0];
                        int py2 = py - proj[faces[f][e]][1];
                        if (ex * py2 - ey * px2 < 0) inside = false;
                    }
                    if (inside) {
                        // Bilinear interpolation to get local 3D coords
                        int dx1 = p1x - p0x, dy1 = p1y - p0y;
                        int dx3 = p3x - p0x, dy3 = p3y - p0y;
                        int dpx = px - p0x, dpy = py - p0y;
                        int det = dx1 * dy3 - dx3 * dy1;
                        if (det == 0) continue;  // Degenerate face
                        float u = (float)(dpx * dy3 - dx3 * dpy) / det;
                        float v = (float)(dx1 * dpy - dpx * dy1) / det;
                        u = u < 0 ? 0 : (u > 1 ? 1 : u);
                        v = v < 0 ? 0 : (v > 1 ? 1 : v);

                        // Interpolate original 3D position
                        float lx = v0x + u * (v1x - v0x) + v * (v3x - v0x);
                        float ly = v0y + u * (v1y - v0y) + v * (v3y - v0y);
                        float lz = v0z + u * (v1z - v0z) + v * (v3z - v0z);

                        // Use local 3D coords for hue (net wrapped around cube)
                        int hue_val = (int)((lx + ly + lz) * 12 + 128) % 256;
                        if (hue_val < 0) hue_val += 256;
                        uint8_t r, g, b;
                        hue_to_rgb(hue_val, r, g, b);
                        // Darken faces (20% brightness)
                        graphics.set_pen(r * 0.2f, g * 0.2f, b * 0.2f);
                        graphics.pixel(Point(px, py));
                    }
                }
            }
        }

        // Draw edges (static rainbow when warmup complete)
        int edges[12][2] = {
            {0,1}, {1,2}, {2,3}, {3,0},
            {4,5}, {5,6}, {6,7}, {7,4},
            {0,4}, {1,5}, {2,6}, {3,7}
        };
        uint16_t edge_frame = warmup_complete ? 0 : frame;
        for (int i = 0; i < 12; i++) {
            draw_rainbow_line((float)proj[edges[i][0]][0], (float)proj[edges[i][0]][1],
                              (float)proj[edges[i][1]][0], (float)proj[edges[i][1]][1], edge_frame);
        }
    } else {
        // 2D rotating square (filled with rainbow)
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float dx = x - cx;
                float dy = y - cy;

                // Rotate point to check against axis-aligned square
                float rx = dx * cos_rot - dy * sin_rot;
                float ry = dx * sin_rot + dy * cos_rot;

                // Check if inside rotated square
                if (fabsf(rx) <= s && fabsf(ry) <= s) {
                    int hue_anim = warmup_complete ? 0 : frame * 4;
                    int hue_val = (int)(rx * 8 + ry * 8 + hue_anim) % 256;
                    if (hue_val < 0) hue_val += 256;
                    uint8_t r, g, b;
                    hue_to_rgb(hue_val, r, g, b);
                    graphics.set_pen(r, g, b);
                    graphics.pixel(Point(x, y));
                }
            }
        }
    }

    cosmic_unicorn.update(&graphics);
    frame++;
}

void core1_wifi_task() {
    // Initialize the CYW43 WiFi chip
    if (cyw43_arch_init()) {
        printf("WiFi init failed!\n");
        return;
    }
    
    printf("WiFi chip initialized\n");
    
    // Enable station mode
    cyw43_arch_enable_sta_mode();
    
    // Scan for networks until we find our target
    printf("Scanning for WiFi network '%s'...\n", WIFI_SSID);
    boot_stage = BOOT_STAGE_WIFI_SCAN;
    
    while (!network_found && wifi_running) {
        cyw43_wifi_scan_options_t scan_options = {0};
        int scan_err = cyw43_wifi_scan(&cyw43_state, &scan_options, nullptr, scan_callback);
        
        if (scan_err == 0) {
            // Wait for scan to complete
            while (cyw43_wifi_scan_active(&cyw43_state) && !network_found) {
                cyw43_arch_poll();
                sleep_ms(10);
            }
        } else {
            printf("Failed to start scan: %d\n", scan_err);
        }
        
        if (!network_found) {
            printf("Network '%s' not found, retrying in 3 seconds...\n", WIFI_SSID);
            // Blink LED while waiting
            for (int i = 0; i < 6 && wifi_running; i++) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i % 2);
                sleep_ms(500);
            }
        }
    }
    
    if (!wifi_running) {
        return;
    }
    
    printf("Connecting to '%s' with auth type 0x%08x...\n", WIFI_SSID, (unsigned int)detected_auth);
    boot_stage = BOOT_STAGE_WIFI_CONNECT;

    // Connect to WiFi with detected auth type
    int result = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID, 
        WIFI_PASSWORD, 
        detected_auth,
        30000  // 30 second timeout
    );
    
    if (result != 0) {
        printf("WiFi connection failed! Error: %d\n", result);
        // Blink LED rapidly to indicate error
        while (wifi_running) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
            sleep_ms(100);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
            sleep_ms(100);
        }
        return;
    }
    
    wifi_connected = true;
    printf("WiFi connected!\n");

    // Disable WiFi power saving for consistent latency
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
    printf("WiFi power management: performance mode\n");

    // Resolve allowed hosts for bootloader reboot
    http_server::resolve_allowed_hosts();
    
    // Print IP address
    const ip4_addr_t* ip = netif_ip4_addr(netif_default);
    printf("IP Address: %s\n", ip4addr_ntoa(ip));
    
    // LED solid on when connected
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    
    // Start HTTP server
    if (http_server::init(80)) {
        printf("HTTP server started on port 80\n");
        printf("Access the device at: http://%s/\n", ip4addr_ntoa(ip));

        // Set boot stage before warmup so face animation works
        boot_stage = BOOT_STAGE_HTTP_READY;

        // Warm up the HTTP server while animating the display
        // This ensures the server is fully responsive before showing ready
        printf("Warming up HTTP server...\n");
        http_server::warmup(warmup_animate);
        printf("HTTP server ready\n");

        http_server_ready = true;
    } else {
        printf("Failed to start HTTP server!\n");
    }
    
    // Keep the WiFi stack running
    bool was_connected = false;
    uint32_t last_led_update = 0;
    uint32_t last_link_check = 0;
    while (wifi_running) {
        // Poll the WiFi stack frequently for low latency
        cyw43_arch_poll();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Periodically check WiFi link status and keep it alive
        if (now - last_link_check >= 5000) {
            last_link_check = now;
            int link_status = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (link_status != CYW43_LINK_JOIN) {
                printf("WiFi link lost (status=%d), reconnecting...\n", link_status);
                cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                    detected_auth, 10000);
            }
        }

        // Update LED less frequently
        if (now - last_led_update >= 100) {
            last_led_update = now;
            int connections = http_server::get_active_connections();
            if (connections > 0) {
                static bool led_state = false;
                led_state = !led_state;
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
                was_connected = true;
            } else if (was_connected) {
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                was_connected = false;
            }
        }
        
        // Minimal sleep - just yield to allow other tasks
        sleep_us(100);
    }

    // Stop HTTP server on exit
    http_server::stop();
}

int main() {
    // Set fixed clock speed for consistent performance
    // RP2350 default is 150MHz, we'll use 150MHz fixed (safe without voltage change)
    set_sys_clock_khz(150000, true);
    
    stdio_init_all();
    
    // Brief delay for USB serial to connect
    sleep_ms(2000);

    printf("UnicornLEDStream starting...\n");
    printf("System clock: %lu MHz\n", clock_get_hz(clk_sys) / 1000000);

    const BoardInfo& board = detect_board();
    printf("Detected board: %s (%dx%d)\n", board.name, board.width, board.height);
    
    cosmic_unicorn.init();
    cosmic_unicorn.set_brightness(0.5f);
    printf("Display initialized\n");

    // Initialize Lua shader engine
    shader_lua::init();
    printf("Lua shader engine initialized\n");

    // Show initial boot animation immediately
    show_test_pattern(0.1f);
    
    // Launch WiFi task on core 1
    multicore_launch_core1(core1_wifi_task);

    printf("Running boot animation...\n");
    
    // Allocate shader frame buffer
    int width = CosmicUnicorn::WIDTH;
    int height = CosmicUnicorn::HEIGHT;
    uint8_t* shader_buffer = new uint8_t[width * height * 3];
    uint32_t shader_start_time = 0;
    int shader_frame = 0;

    // Boot animation radius (smoothly animated)
    float current_radius = 0.1f;

    // Enable watchdog with 2 second timeout
    watchdog_enable(2000, true);
    printf("Watchdog enabled (2s timeout)\n");
    
    // Main loop - run shader if loaded, show rainbow when idle, otherwise wait
    uint32_t last_frame_time = 0;
    int idle_frames = 0;
    bool shader_was_running = false;
    bool external_frame_mode = false;  // True after /api/frame received, suppresses idle anim
    constexpr uint32_t FRAME_TIMEOUT_MS = 500;  // Kill shader if frame takes >500ms

    // Button state for shader cycling
    int current_shader_idx = -1;  // -1 = no builtin shader loaded
    uint32_t last_button_time = 0;
    constexpr uint32_t BUTTON_DEBOUNCE_MS = 200;

    // Activity tracking for brightness dimming
    uint32_t last_activity_time = to_ms_since_boot(get_absolute_time());
    bool is_dimmed = false;
    constexpr uint32_t DIM_TIMEOUT_MS = 30000;
    constexpr float BRIGHTNESS_STEP = 0.05f;
    constexpr float BRIGHTNESS_MIN = 0.05f;
    constexpr float BRIGHTNESS_MAX = 1.0f;
    float current_brightness = 0.5f;
    bool display_sleeping = false;

    while (wifi_running) {
        // Feed the watchdog
        watchdog_update();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Check for reboot request (must be handled from Core 0 for reset_usb_boot)
        if (http_server::reboot_requested()) {
            bool to_bootloader = http_server::reboot_to_bootloader();
            printf("Reboot requested from Core 0, waiting for Core 1 to flush...\n");

            // Signal Core 1 to stop and let it flush TCP responses
            wifi_running = false;

            // Wait for Core 1 to process pending TCP and shut down
            sleep_ms(500);

            // Disable watchdog to prevent normal reset during bootloader transition
            watchdog_disable();

            // Stop Core 1 completely before reset_usb_boot
            multicore_reset_core1();

            if (to_bootloader) {
                printf("Rebooting into USB bootloader...\n");
                reset_usb_boot(0, 0);
            } else {
                printf("Rebooting...\n");
                watchdog_reboot(0, 0, 0);
            }
            // Never returns
        }

        // Check buttons for shader cycling (with debounce)
        if (now - last_button_time > BUTTON_DEBOUNCE_MS) {
            if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_A)) {
                current_shader_idx = (current_shader_idx + 1) % BUILTIN_SHADER_COUNT;
                printf("Loading shader: %s\n", BUILTIN_SHADERS[current_shader_idx].name);
                const char* code = BUILTIN_SHADERS[current_shader_idx].code;
                shader_lua::load_shader(code, strlen(code));
                external_frame_mode = false;
                last_button_time = now;
                last_activity_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_B)) {
                current_shader_idx = (current_shader_idx - 1 + BUILTIN_SHADER_COUNT) % BUILTIN_SHADER_COUNT;
                printf("Loading shader: %s\n", BUILTIN_SHADERS[current_shader_idx].name);
                const char* code = BUILTIN_SHADERS[current_shader_idx].code;
                shader_lua::load_shader(code, strlen(code));
                external_frame_mode = false;
                last_button_time = now;
                last_activity_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_C)) {
                if (shader_lua::is_loaded()) {
                    printf("Stopping shader\n");
                    shader_lua::unload();
                    current_shader_idx = -1;
                }
                last_button_time = now;
                last_activity_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_BRIGHTNESS_UP)) {
                current_brightness = fminf(current_brightness + BRIGHTNESS_STEP, BRIGHTNESS_MAX);
                cosmic_unicorn.set_brightness(current_brightness);
                printf("Brightness: %.0f%%\n", current_brightness * 100);
                last_button_time = now;
                last_activity_time = now;
                is_dimmed = false;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_BRIGHTNESS_DOWN)) {
                current_brightness = fmaxf(current_brightness - BRIGHTNESS_STEP, BRIGHTNESS_MIN);
                cosmic_unicorn.set_brightness(current_brightness);
                printf("Brightness: %.0f%%\n", current_brightness * 100);
                last_button_time = now;
                last_activity_time = now;
                is_dimmed = false;
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
                last_activity_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_VOLUME_UP)) {
                cosmic_unicorn.adjust_volume(+5);
                printf("Volume up\n");
                last_button_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_VOLUME_DOWN)) {
                cosmic_unicorn.adjust_volume(-5);
                printf("Volume down\n");
                last_button_time = now;
            }
            else if (cosmic_unicorn.is_pressed(CosmicUnicorn::SWITCH_D)) {
                // Cycle rotation mode for hypercube shader (0-4)
                int mode = shader_lua::get_global_int("rotation_mode", 0);
                mode = (mode + 1) % 5;
                shader_lua::set_global_int("rotation_mode", mode);
                const char* mode_names[] = {"XZ+ZW+YZ", "XZ+YZ", "ZW only", "XW+YW", "XY+ZW"};
                printf("Rotation mode: %d (%s)\n", mode, mode_names[mode]);
                last_button_time = now;
                last_activity_time = now;
            }
        }

        // Process pending HTTP display operations (from core 1)
        if (http_server::has_pending_brightness()) {
            float new_brightness = http_server::get_pending_brightness();
            cosmic_unicorn.set_brightness(new_brightness);
            last_activity_time = now;
            is_dimmed = false;
        }

        if (http_server::has_pending_frame()) {
            // Stop any running shader so external frames take over
            if (shader_lua::is_loaded()) {
                printf("External frame received, stopping shader\n");
                shader_lua::unload();
                current_shader_idx = -1;
            }
            uint8_t* frame_data = http_server::get_pending_frame_buffer();
            uint8_t* prev_data = http_server::get_displayed_frame_buffer();
            bool have_prev = http_server::has_displayed_frame();

            if (have_prev) {
                // Delta update: only set pixels that changed
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        size_t idx = (y * width + x) * 3;
                        if (frame_data[idx]   != prev_data[idx]   ||
                            frame_data[idx+1] != prev_data[idx+1] ||
                            frame_data[idx+2] != prev_data[idx+2]) {
                            graphics.set_pen(frame_data[idx], frame_data[idx+1], frame_data[idx+2]);
                            graphics.pixel(Point(x, y));
                        }
                    }
                }
            } else {
                // First frame: set all pixels
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        size_t idx = (y * width + x) * 3;
                        graphics.set_pen(frame_data[idx], frame_data[idx+1], frame_data[idx+2]);
                        graphics.pixel(Point(x, y));
                    }
                }
            }
            cosmic_unicorn.update(&graphics);
            http_server::clear_pending_frame();
            last_activity_time = now;
            external_frame_mode = true;
        }

        // Brightness dimming based on activity - DISABLED
        // if (!display_sleeping) {
        //     bool has_activity = http_server::get_active_connections() > 0;
        //     if (has_activity) {
        //         last_activity_time = now;
        //     }
        //
        //     bool should_dim = (now - last_activity_time) > DIM_TIMEOUT_MS;
        //     if (should_dim && !is_dimmed) {
        //         cosmic_unicorn.set_brightness(current_brightness * 0.5f);
        //         is_dimmed = true;
        //     } else if (!should_dim && is_dimmed) {
        //         cosmic_unicorn.set_brightness(current_brightness);
        //         is_dimmed = false;
        //     }
        // }
        
        // If a shader is loaded, run it
        if (shader_lua::is_loaded()) {
            if (shader_start_time == 0) {
                shader_start_time = now;
                shader_frame = 0;
                printf("Starting Lua shader execution\n");
            }
            shader_was_running = true;
            
            // Run shader at ~30fps
            if (now - last_frame_time >= 33) {
                float t = (now - shader_start_time) / 1000.0f;
                float dt = (now - last_frame_time) / 1000.0f;
                
                uint32_t frame_start = now;
                bool success = shader_lua::render_frame(shader_buffer, width, height, t, shader_frame, dt);
                uint32_t frame_time = to_ms_since_boot(get_absolute_time()) - frame_start;
                
                if (frame_time > FRAME_TIMEOUT_MS) {
                    printf("Shader too slow (%lu ms), unloading\n", frame_time);
                    shader_lua::unload();
                } else if (success) {
                    // Copy shader buffer to graphics
                    for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                            size_t idx = (y * width + x) * 3;
                            graphics.set_pen(shader_buffer[idx], shader_buffer[idx+1], shader_buffer[idx+2]);
                            graphics.pixel(Point(x, y));
                        }
                    }
                    cosmic_unicorn.update(&graphics);
                    shader_frame++;
                } else {
                    printf("Shader error: %s\n", shader_lua::get_error());
                    shader_lua::unload();
                }
                last_frame_time = to_ms_since_boot(get_absolute_time());
            }
            sleep_ms(1);
        }
        // Shader was just stopped - clear the display
        else if (shader_was_running) {
            shader_was_running = false;
            shader_start_time = 0;
            printf("Shader stopped, clearing display\n");
            // Clear to black
            graphics.set_pen(0, 0, 0);
            graphics.clear();
            cosmic_unicorn.update(&graphics);
            idle_frames = 0;
        }
        // Boot animation: before warmup starts, animate based on boot stage
        else if (warmup_start_time == 0) {
            if (now - last_frame_time >= 33) {
                float target_radius = get_boot_radius_target();
                float diff = target_radius - current_radius;
                if (diff > 0.01f) {
                    current_radius += diff * 0.1f;
                } else {
                    current_radius = target_radius;
                }
                show_test_pattern(current_radius);
                last_frame_time = now;
            }
            sleep_ms(10);
        }
        // Idle animation: after warmup complete (skip if receiving external frames)
        else if (warmup_complete && !external_frame_mode) {
            if (now - last_frame_time >= 33) {
                show_test_pattern(1.0f);
                last_frame_time = now;
            }
            sleep_ms(10);
        } else {
            // Between warmup start and completion - just wait
            sleep_us(100);
        }
    }
    
    delete[] shader_buffer;

    wifi_running = false;

    return 0;
}
