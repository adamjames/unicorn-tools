// Lite HTTP server for UnicornLEDStreamLite (streaming-only firmware)
// Stripped version of http_server.cpp - no shader/editor/audio routes
// Supports HTTP/1.1 keep-alive for high-throughput frame streaming

#include "http_server.hpp"
#include "cosmic_unicorn.hpp"
#include "secrets.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

#include "pico/stdlib.h"
#include "tusb.h"
#include "pico/cyw43_arch.h"
#include "pico/bootrom.h"
#include "hardware/sync.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"

namespace http_server {

namespace {

constexpr size_t MAX_REQUEST_SIZE = 16384;  // 16KB max request
constexpr size_t MAX_RESPONSE_SIZE = 4096;
constexpr int KEEPALIVE_TIMEOUT_POLLS = 10;  // ~5 seconds at 2 polls/sec

struct ClientState {
    struct tcp_pcb* pcb;
    uint8_t* request_buffer;
    size_t request_len;
    size_t content_length;
    bool headers_complete;
    bool keep_alive;
    int idle_polls;
};

struct tcp_pcb* server_pcb = nullptr;
struct udp_pcb* udp_server_pcb = nullptr;
volatile int active_connections = 0;
volatile bool g_reboot_requested = false;
volatile bool g_reboot_to_bootloader = false;

// Resolved IPs for allowed bootloader hosts (+ localhost)
uint32_t g_allowed_ips[BOOTLOADER_ALLOWED_COUNT + 1] = {0};
int g_allowed_ip_count = 0;

bool is_bootloader_allowed(uint32_t client_ip) {
    uint8_t a = client_ip & 0xFF;
    uint8_t b = (client_ip >> 8) & 0xFF;

    // Deny gateway/router (10.0.0.1)
    if (client_ip == PP_HTONL(0x0A000001)) return false;

    // Always allow localhost (127.x.x.x)
    if (a == 127) return true;

    // Allow local subnet (10.0.0.x) as fallback if DNS resolution failed
    if (a == 10 && b == 0 && g_allowed_ip_count == 0) return true;

    // Check against resolved hosts
    for (int i = 0; i < g_allowed_ip_count; i++) {
        if (client_ip == g_allowed_ips[i]) return true;
    }
    return false;
}

// Pending display operations (set by HTTP callbacks, processed by core 0)
volatile bool g_pending_brightness = false;
volatile float g_pending_brightness_value = 0.5f;

// Frame buffer with spinlock protection for cross-core access
// Core 1 (HTTP) writes to g_frame_buffer, Core 0 (display) reads from it
static spin_lock_t* g_frame_lock = nullptr;
static uint8_t g_frame_buffer[32 * 32 * 3];      // Incoming frame (written by Core 1)
static uint8_t g_ready_frame[32 * 32 * 3];       // Ready for Core 0 to read
static volatile bool g_pending_frame = false;
static volatile uint32_t g_frame_sequence = 0;   // Increments on each new frame

// Delta frame support: store indices of changed pixels
static uint16_t g_delta_indices[1024];           // Indices of changed pixels
static volatile uint16_t g_delta_count = 0;      // 0 = full frame, >0 = delta with this many changes

// HTTP response templates - with keep-alive support and CORS
const char* HTTP_200_KEEPALIVE = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n%s";
const char* HTTP_200_CLOSE = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n%s";
const char* HTTP_400_BAD_REQUEST = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 11\r\n\r\nBad Request";
const char* HTTP_404_NOT_FOUND = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 9\r\n\r\nNot Found";
const char* HTTP_OPTIONS_CORS = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nAccess-Control-Max-Age: 86400\r\nContent-Length: 0\r\n\r\n";

// Case-insensitive string search
static const char* ci_strstr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return nullptr;
}

// Find header value in request
static const char* find_header(const char* request, const char* header_name, size_t* value_len) {
    const char* pos = ci_strstr(request, header_name);
    if (!pos) return nullptr;

    pos += strlen(header_name);
    while (*pos == ' ' || *pos == ':') pos++;

    const char* end = strstr(pos, "\r\n");
    if (end && value_len) {
        *value_len = end - pos;
    }
    return pos;
}

// Parse Content-Length header
static size_t get_content_length(const char* request) {
    size_t len = 0;
    const char* cl = find_header(request, "Content-Length", &len);
    if (cl) {
        return strtoul(cl, nullptr, 10);
    }
    return 0;
}

// Check if request wants keep-alive
static bool wants_keep_alive(const char* request) {
    size_t len = 0;
    const char* conn = find_header(request, "Connection", &len);
    if (conn) {
        if (len >= 5 && ci_strstr(conn, "close")) {
            return false;
        }
        if (ci_strstr(conn, "keep-alive")) {
            return true;
        }
    }
    if (strstr(request, "HTTP/1.1")) {
        return true;
    }
    return false;
}

// Find end of headers
static const char* find_body(const char* request) {
    const char* body = strstr(request, "\r\n\r\n");
    if (body) return body + 4;
    return nullptr;
}

// Send HTTP response
static void send_response(struct tcp_pcb* pcb, const char* response, size_t len) {
    if (pcb && response) {
        cyw43_arch_lwip_begin();
        tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
        tcp_output(pcb);
        cyw43_arch_lwip_end();
    }
}

static void close_client(ClientState* state) {
    if (state) {
        if (state->pcb) {
            cyw43_arch_lwip_begin();
            tcp_arg(state->pcb, nullptr);
            tcp_recv(state->pcb, nullptr);
            tcp_err(state->pcb, nullptr);
            tcp_poll(state->pcb, nullptr, 0);
            tcp_close(state->pcb);
            cyw43_arch_lwip_end();
        }
        if (state->request_buffer) {
            free(state->request_buffer);
        }
        free(state);
        if (active_connections > 0) {
            active_connections--;
        }
    }
}

// Reset client state for next request (keep-alive)
static void reset_client_state(ClientState* state) {
    state->request_len = 0;
    state->content_length = 0;
    state->headers_complete = false;
    state->idle_polls = 0;
}

// Forward declaration
static err_t tcp_recv_callback(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err);

// Process complete HTTP request
static void process_request(ClientState* state) {
    char response[MAX_RESPONSE_SIZE];
    const char* request = (const char*)state->request_buffer;

    // Check if client wants keep-alive
    state->keep_alive = wants_keep_alive(request);

    // Parse method and URI
    char method[8] = {0};
    char uri[128] = {0};

    if (sscanf(request, "%7s %127s", method, uri) != 2) {
        send_response(state->pcb, HTTP_400_BAD_REQUEST, strlen(HTTP_400_BAD_REQUEST));
        close_client(state);
        return;
    }

    // Handle CORS preflight
    if (strcmp(method, "OPTIONS") == 0) {
        send_response(state->pcb, HTTP_OPTIONS_CORS, strlen(HTTP_OPTIONS_CORS));
        if (state->keep_alive) {
            reset_client_state(state);
        } else {
            close_client(state);
        }
        return;
    }

    const char* body = find_body(request);
    size_t body_len = 0;
    if (body && state->content_length > 0) {
        body_len = state->request_len - (body - request);
    }

    // Select response template based on keep-alive
    const char* response_template = state->keep_alive ? HTTP_200_KEEPALIVE : HTTP_200_CLOSE;

    // Route handling
    bool handled = false;
    const char* json_response = "{\"status\":\"ok\"}";

    if (strcmp(uri, "/api/status") == 0) {
        json_response = "{\"status\":\"running\",\"version\":\"1.0-lite\"}";
        handled = true;
    }
    else if (strcmp(uri, "/api/brightness") == 0) {
        if (strcmp(method, "POST") == 0 && body && body_len > 0) {
            float value = 0.5f;
            const char* val_pos = strstr(body, "\"value\"");
            if (val_pos) {
                val_pos = strchr(val_pos, ':');
                if (val_pos) {
                    value = strtof(val_pos + 1, nullptr);
                    g_pending_brightness_value = value;
                    g_pending_brightness = true;
                }
            }
            json_response = "{\"status\":\"ok\"}";
        } else {
            float b = g_pending_brightness_value;
            char brightness_json[64];
            snprintf(brightness_json, sizeof(brightness_json), "{\"brightness\":%.2f}", b);
            int len = snprintf(response, sizeof(response), response_template,
                (int)strlen(brightness_json), brightness_json);
            send_response(state->pcb, response, len);

            if (state->keep_alive) {
                reset_client_state(state);
            } else {
                close_client(state);
            }
            return;
        }
        handled = true;
    }
    else if (strcmp(uri, "/api/frame") == 0 && strcmp(method, "POST") == 0) {
        if (g_pending_frame) {
            // Previous frame not yet drawn - reject to avoid dropping frames
            json_response = "{\"status\":\"busy\"}";
        } else if (body && body_len > 0) {
            int width = pimoroni::CosmicUnicorn::WIDTH;
            int height = pimoroni::CosmicUnicorn::HEIGHT;
            size_t expected = width * height * 3;
            if (body_len >= expected && expected <= sizeof(g_frame_buffer)) {
                // Copy to staging buffer first (outside lock)
                memcpy(g_frame_buffer, body, expected);

                // Then atomically swap to ready buffer under lock
                uint32_t irq = spin_lock_blocking(g_frame_lock);
                memcpy(g_ready_frame, g_frame_buffer, expected);
                g_delta_count = 0;  // Full frame, not delta
                g_frame_sequence++;
                g_pending_frame = true;
                spin_unlock(g_frame_lock, irq);
            }
            json_response = "{\"status\":\"ok\"}";
        } else {
            json_response = "{\"status\":\"error\",\"message\":\"no data\"}";
        }
        handled = true;
    }
    else if (strcmp(uri, "/api/delta") == 0 && strcmp(method, "POST") == 0) {
        if (g_pending_frame) {
            // Previous frame not yet drawn - reject to avoid dropping frames
            json_response = "{\"status\":\"busy\"}";
        } else if (body && body_len >= 2) {
            // Delta frame: u16 count + (u16 index, u8 r, u8 g, u8 b) per pixel
            const uint8_t* p = (const uint8_t*)body;
            uint16_t count = p[0] | (p[1] << 8);
            size_t expected = 2 + count * 5;

            if (body_len >= expected && count <= 1024) {
                uint32_t irq = spin_lock_blocking(g_frame_lock);

                // Apply delta updates to the ready frame and record indices
                const uint8_t* entry = p + 2;
                uint16_t valid_count = 0;
                for (uint16_t i = 0; i < count; i++) {
                    uint16_t idx = entry[0] | (entry[1] << 8);
                    if (idx < 1024) {
                        size_t offset = idx * 3;
                        g_ready_frame[offset]     = entry[2];  // R
                        g_ready_frame[offset + 1] = entry[3];  // G
                        g_ready_frame[offset + 2] = entry[4];  // B
                        g_delta_indices[valid_count++] = idx;
                    }
                    entry += 5;
                }
                g_delta_count = valid_count;
                g_frame_sequence++;
                g_pending_frame = true;

                spin_unlock(g_frame_lock, irq);
                json_response = "{\"status\":\"ok\"}";
            } else {
                json_response = "{\"status\":\"error\",\"message\":\"invalid delta\"}";
            }
        } else {
            json_response = "{\"status\":\"error\",\"message\":\"no data\"}";
        }
        handled = true;
    }
    else if (strcmp(uri, "/api/reboot") == 0 && strcmp(method, "POST") == 0) {
        g_reboot_requested = true;
        g_reboot_to_bootloader = false;
        json_response = "{\"status\":\"rebooting\"}";
        handled = true;
    }
    else if (strcmp(uri, "/api/reboot-bootloader") == 0 && strcmp(method, "POST") == 0) {
        uint32_t client_ip = ip4_addr_get_u32(&state->pcb->remote_ip);
        if (!tud_mounted()) {
            json_response = "{\"status\":\"error\",\"message\":\"USB not connected\"}";
        } else if (!is_bootloader_allowed(client_ip)) {
            json_response = "{\"status\":\"error\",\"message\":\"IP not authorized\"}";
        } else {
            g_reboot_requested = true;
            g_reboot_to_bootloader = true;
            json_response = "{\"status\":\"rebooting to bootloader\"}";
        }
        handled = true;
    }

    if (handled) {
        int len = snprintf(response, sizeof(response), response_template,
            (int)strlen(json_response), json_response);
        send_response(state->pcb, response, len);
    } else {
        send_response(state->pcb, HTTP_404_NOT_FOUND, strlen(HTTP_404_NOT_FOUND));
        state->keep_alive = false;
    }

    if (state->keep_alive) {
        reset_client_state(state);
    } else {
        close_client(state);
    }
}

// TCP receive callback
static err_t tcp_recv_callback(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    ClientState* state = (ClientState*)arg;

    if (!p) {
        close_client(state);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        close_client(state);
        return err;
    }

    // Reset idle counter on activity
    state->idle_polls = 0;

    // Allocate buffer if needed
    if (!state->request_buffer) {
        state->request_buffer = (uint8_t*)malloc(MAX_REQUEST_SIZE);
        if (!state->request_buffer) {
            pbuf_free(p);
            close_client(state);
            return ERR_MEM;
        }
        state->request_len = 0;
    }

    // Copy data to buffer
    size_t copy_len = p->tot_len;
    if (state->request_len + copy_len > MAX_REQUEST_SIZE) {
        copy_len = MAX_REQUEST_SIZE - state->request_len;
    }

    pbuf_copy_partial(p, state->request_buffer + state->request_len, copy_len, 0);
    state->request_len += copy_len;
    state->request_buffer[state->request_len] = '\0';

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    // Check if we have complete headers
    if (!state->headers_complete) {
        const char* body = find_body((const char*)state->request_buffer);
        if (body) {
            state->headers_complete = true;
            state->content_length = get_content_length((const char*)state->request_buffer);
        }
    }

    // Check if request is complete
    if (state->headers_complete) {
        const char* body = find_body((const char*)state->request_buffer);
        size_t body_received = state->request_len - (body - (const char*)state->request_buffer);

        if (body_received >= state->content_length) {
            process_request(state);
        }
    }

    return ERR_OK;
}

// TCP error callback
static void tcp_err_callback(void* arg, err_t err) {
    ClientState* state = (ClientState*)arg;
    if (state) {
        state->pcb = nullptr;
        close_client(state);
    }
}

// TCP poll callback (timeout for keep-alive)
static err_t tcp_poll_callback(void* arg, struct tcp_pcb* pcb) {
    ClientState* state = (ClientState*)arg;
    if (state) {
        state->idle_polls++;
        if (state->idle_polls > KEEPALIVE_TIMEOUT_POLLS) {
            close_client(state);
            return ERR_ABRT;
        }
    }
    return ERR_OK;
}

// UDP receive callback - for low-latency frame streaming
static void udp_recv_callback(void* arg, struct udp_pcb* pcb, struct pbuf* p,
                               const ip_addr_t* addr, u16_t port) {
    if (!p) return;

    // Check for full frame (3072 bytes = 32x32x3 RGB)
    constexpr size_t FRAME_SIZE = 32 * 32 * 3;
    if (p->tot_len == FRAME_SIZE && !g_pending_frame) {
        // Copy frame data
        pbuf_copy_partial(p, g_frame_buffer, FRAME_SIZE, 0);

        // Atomically update ready frame
        uint32_t irq = spin_lock_blocking(g_frame_lock);
        memcpy(g_ready_frame, g_frame_buffer, FRAME_SIZE);
        g_delta_count = 0;  // Full frame
        g_frame_sequence++;
        g_pending_frame = true;
        spin_unlock(g_frame_lock, irq);
    }

    pbuf_free(p);
}

// TCP accept callback
static err_t tcp_accept_callback(void* arg, struct tcp_pcb* newpcb, err_t err) {
    if (err != ERR_OK || !newpcb) {
        return ERR_VAL;
    }

    ClientState* state = (ClientState*)calloc(1, sizeof(ClientState));
    if (!state) {
        tcp_abort(newpcb);
        return ERR_MEM;
    }

    state->pcb = newpcb;
    state->keep_alive = true;

    tcp_nagle_disable(newpcb);

    tcp_arg(newpcb, state);
    tcp_recv(newpcb, tcp_recv_callback);
    tcp_err(newpcb, tcp_err_callback);
    tcp_poll(newpcb, tcp_poll_callback, 2);

    active_connections++;

    return ERR_OK;
}

} // anonymous namespace

bool init(uint16_t port) {
    printf("Starting HTTP server on port %d (lite, keep-alive enabled)...\n", port);

    // Initialize frame buffer spinlock
    int lock_num = spin_lock_claim_unused(true);
    g_frame_lock = spin_lock_init(lock_num);
    printf("Frame buffer spinlock initialized (lock %d)\n", lock_num);

    cyw43_arch_lwip_begin();

    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        printf("Failed to create PCB\n");
        cyw43_arch_lwip_end();
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ADDR_ANY, port);
    if (err != ERR_OK) {
        printf("Failed to bind to port %d: %d\n", port, err);
        tcp_close(pcb);
        cyw43_arch_lwip_end();
        return false;
    }

    server_pcb = tcp_listen_with_backlog(pcb, 4);
    if (!server_pcb) {
        printf("Failed to listen\n");
        tcp_close(pcb);
        cyw43_arch_lwip_end();
        return false;
    }

    tcp_accept(server_pcb, tcp_accept_callback);

    // Also set up UDP server on same port for low-latency streaming
    udp_server_pcb = udp_new();
    if (udp_server_pcb) {
        err = udp_bind(udp_server_pcb, IP_ADDR_ANY, port);
        if (err == ERR_OK) {
            udp_recv(udp_server_pcb, udp_recv_callback, nullptr);
            printf("UDP server started on port %d\n", port);
        } else {
            printf("Failed to bind UDP: %d\n", err);
            udp_remove(udp_server_pcb);
            udp_server_pcb = nullptr;
        }
    }

    cyw43_arch_lwip_end();

    printf("HTTP server started on port %d\n", port);
    return true;
}

void stop() {
    cyw43_arch_lwip_begin();
    if (server_pcb) {
        tcp_close(server_pcb);
        server_pcb = nullptr;
    }
    if (udp_server_pcb) {
        udp_remove(udp_server_pcb);
        udp_server_pcb = nullptr;
    }
    cyw43_arch_lwip_end();
}

void poll() {
    // Nothing needed - lwIP handles it via cyw43_arch_poll()
}

int get_active_connections() {
    return active_connections;
}

bool reboot_requested() {
    return g_reboot_requested;
}

bool reboot_to_bootloader() {
    return g_reboot_to_bootloader;
}

void warmup(void (*animate_callback)()) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    constexpr uint32_t WARMUP_DURATION_MS = 800;  // Shorter warmup for lite

    // Animate for the warmup duration
    while (to_ms_since_boot(get_absolute_time()) - start < WARMUP_DURATION_MS) {
        cyw43_arch_poll();
        if (animate_callback) animate_callback();
        sleep_ms(16);
    }
}

bool has_pending_brightness() {
    return g_pending_brightness;
}

float get_pending_brightness() {
    g_pending_brightness = false;
    return g_pending_brightness_value;
}

bool has_pending_frame() {
    return g_pending_frame;
}

uint8_t* get_pending_frame_buffer() {
    // Returns the ready frame buffer - caller must hold lock or call after has_pending_frame()
    return g_ready_frame;
}

void clear_pending_frame() {
    // Atomically clear the pending flag
    uint32_t irq = spin_lock_blocking(g_frame_lock);
    g_pending_frame = false;
    spin_unlock(g_frame_lock, irq);
}

uint32_t get_frame_sequence() {
    return g_frame_sequence;
}

uint16_t get_delta_count() {
    return g_delta_count;
}

uint16_t* get_delta_indices() {
    return g_delta_indices;
}

void acquire_frame_lock() {
    // For external callers that need to hold the lock while reading frame data
    spin_lock_unsafe_blocking(g_frame_lock);
}

void release_frame_lock() {
    spin_unlock_unsafe(g_frame_lock);
}

void resolve_allowed_hosts() {
    const char* hosts[] = BOOTLOADER_ALLOWED_HOSTS;
    g_allowed_ip_count = 0;

    for (int i = 0; i < BOOTLOADER_ALLOWED_COUNT; i++) {
        ip_addr_t addr;
        err_t err = dns_gethostbyname(hosts[i], &addr, nullptr, nullptr);

        if (err == ERR_OK) {
            g_allowed_ips[g_allowed_ip_count++] = ip4_addr_get_u32(&addr);
            printf("Resolved %s -> %s\n", hosts[i], ip4addr_ntoa(&addr));
        } else if (err == ERR_INPROGRESS) {
            printf("Resolving %s...\n", hosts[i]);
            for (int tries = 0; tries < 50; tries++) {
                cyw43_arch_poll();
                sleep_ms(100);
                err = dns_gethostbyname(hosts[i], &addr, nullptr, nullptr);
                if (err == ERR_OK) {
                    g_allowed_ips[g_allowed_ip_count++] = ip4_addr_get_u32(&addr);
                    printf("Resolved %s -> %s\n", hosts[i], ip4addr_ntoa(&addr));
                    break;
                }
            }
            if (err != ERR_OK) {
                printf("Failed to resolve %s\n", hosts[i]);
            }
        } else {
            printf("DNS error for %s: %d\n", hosts[i], err);
        }
    }
    printf("Bootloader allowed from %d hosts + localhost\n", g_allowed_ip_count);
}

} // namespace http_server
