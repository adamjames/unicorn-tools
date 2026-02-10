// Simple HTTP server for UnicornLEDStream
// Based on lwIP TCP server example
// Supports HTTP/1.1 keep-alive for high-throughput frame streaming

#include "http_server.hpp"
#include "shader_lua.hpp"
#include "shader_editor_html.hpp"
#include "builtin_shaders.hpp"
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
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
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
    // For large response streaming (e.g., editor HTML)
    const char* send_buffer;
    size_t send_offset;
    size_t send_total;
};

struct tcp_pcb* server_pcb = nullptr;
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
volatile bool g_pending_frame = false;
static uint8_t g_frame_buffer[32 * 32 * 3];      // Incoming frame
static uint8_t g_displayed_frame[32 * 32 * 3];   // Last frame applied to display
static bool g_displayed_frame_valid = false;       // True after first frame displayed

// HTTP response templates - with keep-alive support and CORS
const char* HTTP_200_KEEPALIVE = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n%s";
const char* HTTP_200_CLOSE = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n%s";
const char* HTTP_400_BAD_REQUEST = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 11\r\n\r\nBad Request";
const char* HTTP_404_NOT_FOUND = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 9\r\n\r\nNot Found";
const char* HTTP_500_ERROR = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 21\r\n\r\nInternal Server Error";
const char* HTTP_OPTIONS_CORS = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nAccess-Control-Max-Age: 86400\r\nContent-Length: 0\r\n\r\n";
const char* HTTP_200_HTML = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: %d\r\n\r\n";

// Case-insensitive string search (since strcasestr isn't standard)
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
    // HTTP/1.1 defaults to keep-alive unless Connection: close is specified
    size_t len = 0;
    const char* conn = find_header(request, "Connection", &len);
    if (conn) {
        // Check for "close"
        if (len >= 5 && ci_strstr(conn, "close")) {
            return false;
        }
        // Check for "keep-alive"
        if (ci_strstr(conn, "keep-alive")) {
            return true;
        }
    }
    // Check HTTP version - 1.1 defaults to keep-alive
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
    state->send_buffer = nullptr;
    state->send_offset = 0;
    state->send_total = 0;
    // keep_alive and request_buffer stay as-is
}

// Continue sending large buffer (called when TCP has sent data)
static err_t tcp_sent_callback(void* arg, struct tcp_pcb* pcb, u16_t len) {
    ClientState* state = (ClientState*)arg;
    if (!state || !state->send_buffer) return ERR_OK;
    
    // Try to send more data
    while (state->send_offset < state->send_total) {
        size_t remaining = state->send_total - state->send_offset;
        size_t sndbuf = tcp_sndbuf(pcb);
        if (sndbuf == 0) break;  // Buffer full, wait for next callback
        
        size_t chunk = remaining < sndbuf ? remaining : sndbuf;
        if (chunk > 2048) chunk = 2048;
        
        err_t err = tcp_write(pcb, state->send_buffer + state->send_offset, chunk, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) break;
        
        state->send_offset += chunk;
    }
    tcp_output(pcb);
    
    // If done sending, close connection
    if (state->send_offset >= state->send_total) {
        state->send_buffer = nullptr;
        close_client(state);
        return ERR_ABRT;  // Tell lwIP we closed it
    }
    
    return ERR_OK;
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
    
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/editor") == 0) {
        // Serve the shader editor HTML using streaming
        char header[128];
        int header_len = snprintf(header, sizeof(header), HTTP_200_HTML, (int)SHADER_EDITOR_HTML_LEN);
        
        // Setup streaming state
        state->send_buffer = SHADER_EDITOR_HTML;
        state->send_offset = 0;
        state->send_total = SHADER_EDITOR_HTML_LEN;
        state->keep_alive = false;
        
        // Register sent callback
        tcp_sent(state->pcb, tcp_sent_callback);
        
        // Send header and start sending body
        cyw43_arch_lwip_begin();
        tcp_write(state->pcb, header, header_len, TCP_WRITE_FLAG_COPY);
        
        // Send as much of the body as we can
        while (state->send_offset < state->send_total) {
            size_t remaining = state->send_total - state->send_offset;
            size_t sndbuf = tcp_sndbuf(state->pcb);
            if (sndbuf == 0) break;
            
            size_t chunk = remaining < sndbuf ? remaining : sndbuf;
            if (chunk > 2048) chunk = 2048;
            
            err_t err = tcp_write(state->pcb, state->send_buffer + state->send_offset, chunk, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) break;
            
            state->send_offset += chunk;
        }
        tcp_output(state->pcb);
        cyw43_arch_lwip_end();
        
        // If we sent everything, close now; otherwise tcp_sent_callback will continue
        if (state->send_offset >= state->send_total) {
            state->send_buffer = nullptr;
            close_client(state);
        }
        return;
    }
    else if (strcmp(uri, "/api/status") == 0) {
        // Status endpoint
        json_response = "{\"status\":\"running\",\"version\":\"1.0\"}";
        handled = true;
    }
    else if (strcmp(uri, "/api/brightness") == 0) {
        // Get/set brightness
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
        if (body && body_len > 0) {
            int width = pimoroni::CosmicUnicorn::WIDTH;
            int height = pimoroni::CosmicUnicorn::HEIGHT;
            size_t expected = width * height * 3;
            if (body_len >= expected && expected <= sizeof(g_frame_buffer)) {
                memcpy(g_frame_buffer, body, expected);
                g_pending_frame = true;
            }
            json_response = "{\"status\":\"ok\"}";
        } else {
            json_response = "{\"status\":\"error\",\"message\":\"no data\"}";
        }
        handled = true;
    }
    else if (strcmp(uri, "/api/shader") == 0 && strcmp(method, "POST") == 0) {
        // Upload Lua shader source code
        if (body && body_len > 0) {
            if (shader_lua::load_shader(body, body_len)) {
                json_response = "{\"status\":\"ok\",\"message\":\"shader loaded\"}";
            } else {
                // Return error message
                char error_json[256];
                snprintf(error_json, sizeof(error_json), 
                    "{\"status\":\"error\",\"message\":\"%s\"}", 
                    shader_lua::get_error());
                int len = snprintf(response, sizeof(response), response_template, 
                    (int)strlen(error_json), error_json);
                send_response(state->pcb, response, len);
                if (state->keep_alive) {
                    reset_client_state(state);
                } else {
                    close_client(state);
                }
                return;
            }
        } else {
            json_response = "{\"status\":\"error\",\"message\":\"no shader code\"}";
        }
        handled = true;
    }
    else if (strcmp(uri, "/api/shader") == 0 && strcmp(method, "DELETE") == 0) {
        // Unload current shader
        shader_lua::unload();
        json_response = "{\"status\":\"ok\",\"message\":\"shader unloaded\"}";
        handled = true;
    }
    else if (strcmp(uri, "/api/shader/status") == 0) {
        // Check shader status
        if (shader_lua::is_loaded()) {
            json_response = "{\"status\":\"ok\",\"loaded\":true}";
        } else {
            json_response = "{\"status\":\"ok\",\"loaded\":false}";
        }
        handled = true;
    }
    else if (strcmp(uri, "/api/shaders") == 0 && strcmp(method, "GET") == 0) {
        // Return list of built-in shaders
        static char shaders_json[1024];
        char* p = shaders_json;
        p += sprintf(p, "{\"shaders\":[");
        for (int i = 0; i < BUILTIN_SHADER_COUNT; i++) {
            if (i > 0) p += sprintf(p, ",");
            p += sprintf(p, "{\"index\":%d,\"name\":\"%s\"}", i, BUILTIN_SHADERS[i].name);
        }
        p += sprintf(p, "]}");

        int len = snprintf(response, sizeof(response), response_template,
            (int)strlen(shaders_json), shaders_json);
        send_response(state->pcb, response, len);
        if (state->keep_alive) {
            reset_client_state(state);
        } else {
            close_client(state);
        }
        return;
    }
    else if (strncmp(uri, "/api/shader/", 12) == 0 && strcmp(method, "GET") == 0) {
        // Return shader code by index: /api/shader/0, /api/shader/1, etc.
        int idx = atoi(uri + 12);
        if (idx >= 0 && idx < BUILTIN_SHADER_COUNT) {
            const char* code = BUILTIN_SHADERS[idx].code;
            size_t code_len = strlen(code);

            // Send plain text response with shader code using streaming
            char header[128];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %d\r\n\r\n",
                (int)code_len);

            // Setup streaming state
            state->send_buffer = code;
            state->send_offset = 0;
            state->send_total = code_len;
            state->keep_alive = false;

            // Register sent callback
            tcp_sent(state->pcb, tcp_sent_callback);

            // Send header and start sending body
            cyw43_arch_lwip_begin();
            tcp_write(state->pcb, header, header_len, TCP_WRITE_FLAG_COPY);

            // Send as much of the body as we can
            while (state->send_offset < state->send_total) {
                size_t remaining = state->send_total - state->send_offset;
                size_t sndbuf = tcp_sndbuf(state->pcb);
                if (sndbuf == 0) break;

                size_t chunk = remaining < sndbuf ? remaining : sndbuf;
                if (chunk > 2048) chunk = 2048;

                err_t err = tcp_write(state->pcb, state->send_buffer + state->send_offset, chunk, TCP_WRITE_FLAG_COPY);
                if (err != ERR_OK) break;

                state->send_offset += chunk;
            }
            tcp_output(state->pcb);
            cyw43_arch_lwip_end();

            // If we sent everything, close now; otherwise tcp_sent_callback will continue
            if (state->send_offset >= state->send_total) {
                close_client(state);
            }
            return;
        } else {
            json_response = "{\"error\":\"shader not found\"}";
            handled = true;
        }
    }
    else if (strcmp(uri, "/api/reboot") == 0 && strcmp(method, "POST") == 0) {
        // Normal reboot - restart the Pico
        shader_lua::unload();
        g_reboot_requested = true;
        g_reboot_to_bootloader = false;
        json_response = "{\"status\":\"rebooting\"}";
        handled = true;
    }
    else if (strcmp(uri, "/api/reboot-bootloader") == 0 && strcmp(method, "POST") == 0) {
        // Reboot into USB bootloader for firmware update
        // Requires: USB data connected AND client IP in whitelist
        uint32_t client_ip = ip4_addr_get_u32(&state->pcb->remote_ip);
        if (!tud_mounted()) {
            json_response = "{\"status\":\"error\",\"message\":\"USB not connected\"}";
        } else if (!is_bootloader_allowed(client_ip)) {
            json_response = "{\"status\":\"error\",\"message\":\"IP not authorized\"}";
        } else {
            shader_lua::unload();
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
        state->keep_alive = false;  // Close on 404
    }
    
    if (state->keep_alive) {
        // Reset state for next request on this connection
        reset_client_state(state);
    } else {
        close_client(state);
    }
}

// TCP receive callback
static err_t tcp_recv_callback(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    ClientState* state = (ClientState*)arg;
    
    if (!p) {
        // Connection closed by client
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
    state->request_buffer[state->request_len] = '\0';  // Null terminate
    
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
            // Request complete, process it
            process_request(state);
        }
    }
    
    return ERR_OK;
}

// TCP error callback
static void tcp_err_callback(void* arg, err_t err) {
    ClientState* state = (ClientState*)arg;
    if (state) {
        state->pcb = nullptr;  // PCB is already freed by lwIP
        close_client(state);
    }
}

// TCP poll callback (timeout for keep-alive)
static err_t tcp_poll_callback(void* arg, struct tcp_pcb* pcb) {
    ClientState* state = (ClientState*)arg;
    if (state) {
        state->idle_polls++;
        if (state->idle_polls > KEEPALIVE_TIMEOUT_POLLS) {
            // Connection idle too long, close it
            close_client(state);
            return ERR_ABRT;
        }
    }
    return ERR_OK;
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
    state->keep_alive = true;  // Default to keep-alive for HTTP/1.1
    
    // Set TCP options for lower latency
    tcp_nagle_disable(newpcb);
    
    tcp_arg(newpcb, state);
    tcp_recv(newpcb, tcp_recv_callback);
    tcp_err(newpcb, tcp_err_callback);
    tcp_poll(newpcb, tcp_poll_callback, 2);  // Poll every ~1 second
    
    active_connections++;
    
    return ERR_OK;
}

} // anonymous namespace

bool init(uint16_t port) {
    printf("Starting HTTP server on port %d (keep-alive enabled)...\n", port);
    
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
    
    cyw43_arch_lwip_end();
    
    printf("HTTP server started on port %d\n", port);
    return true;
}

void stop() {
    if (server_pcb) {
        cyw43_arch_lwip_begin();
        tcp_close(server_pcb);
        server_pcb = nullptr;
        cyw43_arch_lwip_end();
    }
}

void poll() {
    // Nothing needed here - lwIP handles it via cyw43_arch_poll()
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
    // Touch all resources to ensure they're paged in / cached
    // This prevents lag on first request

    uint32_t start = to_ms_since_boot(get_absolute_time());
    constexpr uint32_t WARMUP_DURATION_MS = 1400;  // Cover full animation (600+400+300=1300ms)

    // Touch the HTML data
    volatile size_t html_size = SHADER_EDITOR_HTML_LEN;
    volatile char html_first = SHADER_EDITOR_HTML[0];
    volatile char html_last = SHADER_EDITOR_HTML[html_size - 1];
    (void)html_first;
    (void)html_last;

    // Touch all shader data
    for (int i = 0; i < BUILTIN_SHADER_COUNT; i++) {
        volatile const char* name = BUILTIN_SHADERS[i].name;
        volatile const char* code = BUILTIN_SHADERS[i].code;
        while (*name) { volatile char c = *name++; (void)c; }
        while (*code) { volatile char c = *code++; (void)c; }
    }

    // Animate for the full warmup duration
    while (to_ms_since_boot(get_absolute_time()) - start < WARMUP_DURATION_MS) {
        cyw43_arch_poll();
        if (animate_callback) animate_callback();
        sleep_ms(16);  // ~60fps
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
    return g_frame_buffer;
}

void clear_pending_frame() {
    // Copy incoming frame to displayed frame before clearing
    memcpy(g_displayed_frame, g_frame_buffer, sizeof(g_displayed_frame));
    g_displayed_frame_valid = true;
    g_pending_frame = false;
}

uint8_t* get_displayed_frame_buffer() {
    return g_displayed_frame;
}

bool has_displayed_frame() {
    return g_displayed_frame_valid;
}

void resolve_allowed_hosts() {
    const char* hosts[] = BOOTLOADER_ALLOWED_HOSTS;
    g_allowed_ip_count = 0;

    for (int i = 0; i < BOOTLOADER_ALLOWED_COUNT; i++) {
        ip_addr_t addr;
        err_t err = dns_gethostbyname(hosts[i], &addr, nullptr, nullptr);

        if (err == ERR_OK) {
            // Cached result available immediately
            g_allowed_ips[g_allowed_ip_count++] = ip4_addr_get_u32(&addr);
            printf("Resolved %s -> %s\n", hosts[i], ip4addr_ntoa(&addr));
        } else if (err == ERR_INPROGRESS) {
            // Need to wait for DNS - do blocking resolve
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
