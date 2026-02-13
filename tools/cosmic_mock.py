#!/usr/bin/env python3
"""
Mock Cosmic Unicorn server for testing.

Accepts the same HTTP API as the real device and displays frames in a window.
Run with: python cosmic_mock.py [--port 8080] [--scale 16]
"""

import argparse
import socket
import struct
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

# Try pygame first, fall back to tkinter
try:
    import pygame
    USE_PYGAME = True
except ImportError:
    USE_PYGAME = False
    import tkinter as tk
    from PIL import Image, ImageTk

WIDTH = 32
HEIGHT = 32
FRAME_SIZE = WIDTH * HEIGHT * 3

# Shared frame buffer
frame_lock = threading.Lock()
frame_data = bytearray(FRAME_SIZE)
frame_updated = threading.Event()


class CosmicHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Quieter logging
        pass

    def _send_ok(self, body=b"OK"):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", len(body))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/status":
            self._send_ok(b'{"mock":true,"width":32,"height":32}')
        else:
            self.send_error(404)

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length)

        if self.path == "/api/frame":
            if len(body) == FRAME_SIZE:
                with frame_lock:
                    frame_data[:] = body
                frame_updated.set()
                self._send_ok()
            else:
                self.send_error(400, f"Expected {FRAME_SIZE} bytes, got {len(body)}")

        elif self.path == "/api/delta":
            if len(body) >= 2:
                count = struct.unpack("<H", body[:2])[0]
                offset = 2
                with frame_lock:
                    for _ in range(count):
                        if offset + 5 > len(body):
                            break
                        idx, r, g, b = struct.unpack("<HBBB", body[offset:offset+5])
                        offset += 5
                        if idx < WIDTH * HEIGHT:
                            base = idx * 3
                            frame_data[base] = r
                            frame_data[base + 1] = g
                            frame_data[base + 2] = b
                frame_updated.set()
                self._send_ok()
            else:
                self.send_error(400, "Invalid delta")
        else:
            self.send_error(404)


def run_server(port):
    server = HTTPServer(("0.0.0.0", port), CosmicHandler)
    server.serve_forever()


def run_udp_server(port):
    """UDP server for low-latency frame streaming"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"UDP server listening on port {port}")

    pkt_count = 0
    while True:
        data, addr = sock.recvfrom(FRAME_SIZE + 64)  # Allow some overhead
        pkt_count += 1
        if pkt_count <= 5 or pkt_count % 100 == 0:
            print(f"UDP packet #{pkt_count} from {addr}: {len(data)} bytes")
        if len(data) == FRAME_SIZE:
            with frame_lock:
                frame_data[:] = data
            frame_updated.set()
        else:
            print(f"  WARNING: Expected {FRAME_SIZE} bytes, got {len(data)}")


def run_pygame_display(scale):
    import time
    pygame.init()
    screen = pygame.display.set_mode((WIDTH * scale, HEIGHT * scale))
    pygame.display.set_caption("Cosmic Mock - 0 frames")
    clock = pygame.time.Clock()

    surface = pygame.Surface((WIDTH, HEIGHT))
    running = True
    frames_received = 0
    last_fps_time = time.time()
    fps_frame_count = 0

    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False

        # Update display when new frame arrives
        if frame_updated.wait(timeout=0.05):
            frame_updated.clear()
            with frame_lock:
                # Convert RGB bytes to pygame surface
                for y in range(HEIGHT):
                    for x in range(WIDTH):
                        idx = (y * WIDTH + x) * 3
                        r, g, b = frame_data[idx], frame_data[idx+1], frame_data[idx+2]
                        surface.set_at((x, y), (r, g, b))
            frames_received += 1
            fps_frame_count += 1

        # Scale and blit
        scaled = pygame.transform.scale(surface, (WIDTH * scale, HEIGHT * scale))
        screen.blit(scaled, (0, 0))
        pygame.display.flip()

        # Calculate actual received FPS every second
        now = time.time()
        elapsed = now - last_fps_time
        if elapsed >= 1.0:
            actual_fps = fps_frame_count / elapsed
            pygame.display.set_caption(f"Cosmic Mock - {frames_received} frames ({actual_fps:.1f} fps)")
            fps_frame_count = 0
            last_fps_time = now

        clock.tick(60)

    pygame.quit()


def run_tkinter_display(scale):
    root = tk.Tk()
    root.title("Cosmic Mock")

    canvas = tk.Canvas(root, width=WIDTH * scale, height=HEIGHT * scale, bg="black")
    canvas.pack()

    # Pre-create rectangles for each pixel
    rects = []
    for y in range(HEIGHT):
        row = []
        for x in range(WIDTH):
            r = canvas.create_rectangle(
                x * scale, y * scale,
                (x + 1) * scale, (y + 1) * scale,
                fill="black", outline=""
            )
            row.append(r)
        rects.append(row)

    def update():
        if frame_updated.is_set():
            frame_updated.clear()
            with frame_lock:
                for y in range(HEIGHT):
                    for x in range(WIDTH):
                        idx = (y * WIDTH + x) * 3
                        r, g, b = frame_data[idx], frame_data[idx+1], frame_data[idx+2]
                        color = f"#{r:02x}{g:02x}{b:02x}"
                        canvas.itemconfig(rects[y][x], fill=color)
        root.after(16, update)  # ~60 FPS

    root.after(16, update)
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description="Mock Cosmic Unicorn server")
    parser.add_argument("--port", "-p", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--scale", "-s", type=int, default=16, help="Pixel scale (default: 16)")
    args = parser.parse_args()

    print(f"Starting mock Cosmic Unicorn server on port {args.port}")
    print(f"Display scale: {args.scale}x ({WIDTH * args.scale}x{HEIGHT * args.scale} window)")
    print(f"Using: {'pygame' if USE_PYGAME else 'tkinter'}")
    print()
    print("Endpoints:")
    print(f"  UDP  udp://0.0.0.0:{args.port}              - Raw frame (3072 bytes RGB)")
    print(f"  POST http://localhost:{args.port}/api/frame  - Full frame (3072 bytes RGB)")
    print(f"  POST http://localhost:{args.port}/api/delta  - Delta update")
    print(f"  GET  http://localhost:{args.port}/api/status - Status check")
    print()
    print("Press Ctrl+C or close window to quit")

    # Start UDP server in background thread (primary, low-latency)
    udp_thread = threading.Thread(target=run_udp_server, args=(args.port,), daemon=True)
    udp_thread.start()

    # Start HTTP server in background thread (fallback)
    server_thread = threading.Thread(target=run_server, args=(args.port,), daemon=True)
    server_thread.start()

    # Run display in main thread
    if USE_PYGAME:
        run_pygame_display(args.scale)
    else:
        run_tkinter_display(args.scale)


if __name__ == "__main__":
    main()
