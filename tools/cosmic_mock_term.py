#!/usr/bin/env python3
"""
Terminal-based mock Cosmic Unicorn server for SSH sessions.

Displays 32x32 frames using ANSI 24-bit color in the terminal.
"""

import argparse
import socket
import struct
import sys
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

WIDTH = 32
HEIGHT = 32
FRAME_SIZE = WIDTH * HEIGHT * 3

frame_lock = threading.Lock()
frame_data = bytearray(FRAME_SIZE)
frame_updated = threading.Event()
frame_count = [0]


class CosmicHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Show connection info on line 4
        sys.stdout.write(f"\033[4;1H\033[K{self.client_address[0]}: {format % args}\033[0m")
        sys.stdout.flush()

    def _send_ok(self, body=b"OK"):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", len(body))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/api/status":
            self._send_ok(b'{"mock":true,"width":32,"height":32,"term":true}')
        else:
            self.send_error(404)

    def do_POST(self):
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length)

        if self.path == "/api/frame":
            if len(body) == FRAME_SIZE:
                with frame_lock:
                    frame_data[:] = body
                    frame_count[0] += 1
                frame_updated.set()
                self._send_ok()
            else:
                self.send_error(400)

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
                    frame_count[0] += 1
                frame_updated.set()
                self._send_ok()
            else:
                self.send_error(400)
        else:
            self.send_error(404)


def run_server(port):
    server = HTTPServer(("0.0.0.0", port), CosmicHandler)
    server.serve_forever()


def run_udp_server(port):
    """UDP server for low-latency frame streaming"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))

    while True:
        data, addr = sock.recvfrom(FRAME_SIZE + 64)
        if len(data) == FRAME_SIZE:
            with frame_lock:
                frame_data[:] = data
                frame_count[0] += 1
            frame_updated.set()


def render_frame_half():
    """Render using half-block chars (▀) - 2 pixels per char vertically."""
    lines = []
    with frame_lock:
        for y in range(0, HEIGHT, 2):
            line = ""
            for x in range(WIDTH):
                # Top pixel (foreground)
                idx_top = (y * WIDTH + x) * 3
                r1, g1, b1 = frame_data[idx_top], frame_data[idx_top+1], frame_data[idx_top+2]
                # Bottom pixel (background)
                idx_bot = ((y+1) * WIDTH + x) * 3
                r2, g2, b2 = frame_data[idx_bot], frame_data[idx_bot+1], frame_data[idx_bot+2]
                # Upper half block with fg=top, bg=bottom
                line += f"\033[38;2;{r1};{g1};{b1}m\033[48;2;{r2};{g2};{b2}m▀"
            line += "\033[0m"
            lines.append(line)
    return lines


def render_frame_full():
    """Render using full block chars (██) - 1 pixel per 2 chars."""
    lines = []
    with frame_lock:
        for y in range(HEIGHT):
            line = ""
            for x in range(WIDTH):
                idx = (y * WIDTH + x) * 3
                r, g, b = frame_data[idx], frame_data[idx+1], frame_data[idx+2]
                line += f"\033[48;2;{r};{g};{b}m  "
            line += "\033[0m"
            lines.append(line)
    return lines


def main():
    parser = argparse.ArgumentParser(description="Terminal mock Cosmic Unicorn")
    parser.add_argument("--port", "-p", type=int, default=8080, help="HTTP port (default: 8080)")
    parser.add_argument("--full", "-f", action="store_true", help="Use full blocks (wider but clearer)")
    args = parser.parse_args()

    print(f"\033[2J\033[H", end="")  # Clear screen
    print(f"Cosmic Mock (terminal) - port {args.port}")
    print(f"UDP or POST /api/frame")
    print(f"Ctrl+C to quit\n")

    # Start UDP server (primary, low-latency)
    udp_thread = threading.Thread(target=run_udp_server, args=(args.port,), daemon=True)
    udp_thread.start()

    # Start HTTP server (fallback)
    server_thread = threading.Thread(target=run_server, args=(args.port,), daemon=True)
    server_thread.start()

    render = render_frame_full if args.full else render_frame_half
    last_count = 0

    # Hide cursor
    print("\033[?25l", end="")

    try:
        while True:
            if frame_updated.wait(timeout=0.1):
                frame_updated.clear()
                if frame_count[0] != last_count:
                    last_count = frame_count[0]
                    lines = render()
                    # Move cursor to line 5, render frame
                    output = f"\033[5;1H"
                    output += "\n".join(lines)
                    output += f"\n\033[0m frame {last_count}"
                    sys.stdout.write(output)
                    sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        print("\033[?25h\033[0m")  # Show cursor, reset colors


if __name__ == "__main__":
    main()
