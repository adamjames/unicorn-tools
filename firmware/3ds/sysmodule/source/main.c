#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 0x1005
#endif
#ifndef SO_RCVTIMEO
#define SO_RCVTIMEO 0x1006
#endif

#define LED_W 32
#define LED_H 32
#define FRAME_SIZE (LED_W * LED_H * 3)
#define DELTA_THRESHOLD 600

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240

#define VRAM_PHYS_BASE 0x18000000
#define VRAM_VIRT_BASE 0x1F000000

#define SOC_BUFFERSIZE 0x80000

#define CONFIG_PATH "/3ds/cosmic_stream.cfg"
#define CONFIG_PATH_ALT "/luma/sysmodules/cosmic_stream.cfg"

// Runtime config (loaded from file or defaults)
static char cfg_host[128] = "cosmic.lan";
static int cfg_port = 80;
static int cfg_fps = 20;
static u64 cfg_frame_ns = 1000000000ULL / 20;

static u32 *soc_buffer;
static int http_sock = -1;
static bool running = true;

static u8 frame_rgb[FRAME_SIZE];
static u8 prev_frame[FRAME_SIZE];
static bool has_prev;
static u32 prev_crc;

static u8 delta_buf[2 + 1024 * 5];

// Trim whitespace from string
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return s;
}

// Load config from file
static void load_config(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) f = fopen(CONFIG_PATH_ALT, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(trimmed);
        char *val = trim(eq + 1);

        if (strcmp(key, "host") == 0) {
            strncpy(cfg_host, val, sizeof(cfg_host) - 1);
            cfg_host[sizeof(cfg_host) - 1] = '\0';
        } else if (strcmp(key, "port") == 0) {
            cfg_port = atoi(val);
            if (cfg_port <= 0 || cfg_port > 65535) cfg_port = 80;
        } else if (strcmp(key, "fps") == 0) {
            cfg_fps = atoi(val);
            if (cfg_fps < 1) cfg_fps = 1;
            if (cfg_fps > 60) cfg_fps = 60;
            cfg_frame_ns = 1000000000ULL / cfg_fps;
        }
    }
    fclose(f);
}

static u32 crc32_compute(const u8 *data, size_t len) {
    u32 crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static bool http_connect(void) {
    if (http_sock >= 0) return true;

    struct hostent *he = gethostbyname(cfg_host);
    if (!he) return false;

    http_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (http_sock < 0) return false;

    struct timeval tv = { .tv_sec = 3 };
    setsockopt(http_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(http_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int one = 1;
    setsockopt(http_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg_port),
    };
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(http_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(http_sock);
        http_sock = -1;
        return false;
    }

    return true;
}

static void http_disconnect(void) {
    if (http_sock >= 0) { close(http_sock); http_sock = -1; }
}

static bool http_post(const char *path, const u8 *data, size_t len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!http_connect()) continue;

        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %u\r\n"
            "Connection: keep-alive\r\n"
            "\r\n", path, cfg_host, (unsigned)len);

        if (send(http_sock, hdr, hlen, 0) != hlen) { http_disconnect(); continue; }

        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(http_sock, data + sent, len - sent, 0);
            if (n <= 0) { http_disconnect(); goto retry; }
            sent += n;
        }

        char resp[512];
        ssize_t rlen = recv(http_sock, resp, sizeof(resp) - 1, 0);
        if (rlen <= 0) { http_disconnect(); continue; }
        resp[rlen] = '\0';

        if (strstr(resp, "200")) return true;
        http_disconnect();
retry:;
    }
    return false;
}

static void capture_top_screen(void) {
    GSPGPU_CaptureInfo capture;
    if (R_FAILED(GSPGPU_ImportDisplayCaptureInfo(&capture)))
        return;

    GSPGPU_CaptureInfoEntry *top = &capture.screencapture[0];
    u32 phys = (u32)top->framebuf0_vaddr;

    if (phys < VRAM_PHYS_BASE || phys >= VRAM_PHYS_BASE + 0x600000)
        return;

    u8 *fb = (u8 *)(phys - VRAM_PHYS_BASE + VRAM_VIRT_BASE);
    u32 format = top->format & 0x7;
    u32 stride = top->framebuf_widthbytesize;

    if (stride == 0) stride = TOP_H * 3;

    u32 bpp;
    switch (format) {
        case 0: bpp = 4; break;
        case 1: bpp = 3; break;
        case 2: bpp = 2; break;
        case 3: bpp = 2; break;
        case 4: bpp = 2; break;
        default: bpp = 3; break;
    }

    for (int oy = 0; oy < LED_H; oy++) {
        for (int ox = 0; ox < LED_W; ox++) {
            int sx = ox * TOP_W / LED_W;
            int sy = oy * TOP_H / LED_H;

            u32 si = sx * stride + (TOP_H - 1 - sy) * bpp;
            int di = (oy * LED_W + ox) * 3;

            u8 r, g, b;
            switch (format) {
                case 0:
                    r = fb[si + 3];
                    g = fb[si + 2];
                    b = fb[si + 1];
                    break;
                case 1:
                    b = fb[si + 0];
                    g = fb[si + 1];
                    r = fb[si + 2];
                    break;
                case 2: {
                    u16 px = fb[si] | (fb[si + 1] << 8);
                    r = (px >> 11) << 3;
                    g = ((px >> 5) & 0x3F) << 2;
                    b = (px & 0x1F) << 3;
                    break;
                }
                default:
                    r = fb[si + 2];
                    g = fb[si + 1];
                    b = fb[si + 0];
                    break;
            }

            frame_rgb[di + 0] = r;
            frame_rgb[di + 1] = g;
            frame_rgb[di + 2] = b;
        }
    }
}

static bool send_frame(void) {
    u32 crc = crc32_compute(frame_rgb, FRAME_SIZE);

    if (has_prev && crc == prev_crc)
        return true;

    if (has_prev) {
        u16 count = 0;
        int off = 2;

        for (int i = 0; i < FRAME_SIZE; i += 3) {
            if (frame_rgb[i] != prev_frame[i] ||
                frame_rgb[i+1] != prev_frame[i+1] ||
                frame_rgb[i+2] != prev_frame[i+2]) {
                u16 px = i / 3;
                delta_buf[off++] = px & 0xFF;
                delta_buf[off++] = (px >> 8) & 0xFF;
                delta_buf[off++] = frame_rgb[i];
                delta_buf[off++] = frame_rgb[i+1];
                delta_buf[off++] = frame_rgb[i+2];
                count++;
                if (count > DELTA_THRESHOLD) break;
            }
        }

        if (count <= 5) goto save;

        if (count <= DELTA_THRESHOLD) {
            delta_buf[0] = count & 0xFF;
            delta_buf[1] = (count >> 8) & 0xFF;
            if (http_post("/api/delta", delta_buf, off)) goto save;
            return false;
        }
    }

    if (!http_post("/api/frame", frame_rgb, FRAME_SIZE))
        return false;

save:
    memcpy(prev_frame, frame_rgb, FRAME_SIZE);
    prev_crc = crc;
    has_prev = true;
    return true;
}

int main(void) {
    srvInit();
    gspInit();
    fsInit();

    // Load config from SD card
    load_config();

    fsExit();

    soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFERSIZE);

    // Wait for WiFi before initializing sockets
    svcSleepThread(10000000000ULL);  // 10 seconds

    bool soc_ok = false;
    if (soc_buffer && R_SUCCEEDED(socInit(soc_buffer, SOC_BUFFERSIZE))) {
        soc_ok = true;
    }

    while (running) {
        if (soc_ok) {
            capture_top_screen();
            send_frame();
        }
        svcSleepThread(cfg_frame_ns);
    }

    http_disconnect();
    socExit();
    free(soc_buffer);
    gspExit();
    srvExit();
    return 0;
}
