#!//usr/bin/env python3.13
"""
Stream a Wayland screen to Cosmic Unicorn 32x32 LED display.

Uses the XDG Desktop Portal ScreenCast API to get a PipeWire video
stream, then reads frames via GStreamer, downscales to 32x32 RGB,
and POSTs them to the device.

On first run, GNOME will ask you to pick a window/monitor to share.
After that, frames stream continuously with no further interaction.

Usage:
  python3.13 pcsx2_to_unicorn.py [--host cosmic.lan] [--fps N] [--wifi]

Automatically uses USB serial (~100 FPS) if the Pico is connected,
otherwise WiFi (~30 FPS). Use --wifi to force WiFi mode.
"""

import argparse
import glob
import http.client
import os
import queue
import shutil
import signal
import socket
import struct
import sys
import zlib
import termios
import threading
import time
import tty
import urllib.request

import serial

import dbus
from dbus.mainloop.glib import DBusGMainLoop
import gi
gi.require_version("Gst", "1.0")
gi.require_version("GstApp", "1.0")
from gi.repository import GLib, Gst, GstApp


WIDTH = 32
HEIGHT = 32
FRAME_SIZE = WIDTH * HEIGHT * 3  # 3072 bytes RGB
PIXEL_RANGE = range(0, FRAME_SIZE, 3)  # Pre-computed for hot path

PORTAL_BUS = "org.freedesktop.portal.Desktop"


class InstantFPS:
    """Rolling window FPS tracker for instantaneous performance measurement."""

    def __init__(self, window_size=30):
        from collections import deque
        self.times = deque(maxlen=window_size)

    def tick(self):
        """Record a frame timestamp."""
        self.times.append(time.monotonic())

    @property
    def fps(self):
        """Calculate FPS from the rolling window."""
        if len(self.times) < 2:
            return 0.0
        window = self.times[-1] - self.times[0]
        return (len(self.times) - 1) / window if window > 0 else 0.0

    def reset(self):
        """Clear the window."""
        self.times.clear()


class StatusUI:
    """Terminal UI with 2D minimap, status line, and log area."""

    MAP_W = 32  # Width of minimap in chars
    MAP_H = 10  # Height of minimap
    LOG_LINES = 6  # Number of log lines to show

    def __init__(self):
        self.frames_sent = 0
        self.fps = 0.0
        self.send_ms = 0
        self.errors = 0
        self.stalls = 0  # WiFi timeouts
        self.crop = [0, 0, 0, 0]  # L, R, T, B
        self.source_size = [0, 0]  # Full source dimensions
        self.content_size = [0, 0]  # Cropped content dimensions
        self.action = ""
        self.mode = "auto"
        self.delta_pct = 0.0  # Percentage of frames sent as delta
        self._started = False
        self._term_size = (80, 24)
        self._needs_clear = True
        self._log = []  # Log messages

    def log(self, msg):
        """Add a message to the log and update display."""
        # Strip leading whitespace for cleaner display
        msg = msg.lstrip()
        self._log.append(msg)
        # Keep only recent messages
        if len(self._log) > 50:
            self._log = self._log[-50:]
        # Update display if started
        if self._started:
            self._render()

    def start(self):
        """Initialize display area."""
        self._update_term_size()
        # Set up SIGWINCH handler for terminal resize
        signal.signal(signal.SIGWINCH, self._on_resize)
        # Hide cursor, clear screen
        sys.stdout.write("\033[?25l\033[2J")
        sys.stdout.flush()
        self._started = True
        self._render()

    def _on_resize(self, signum, frame):
        """Handle terminal resize."""
        self._update_term_size()
        self._needs_clear = True

    def _update_term_size(self):
        """Get current terminal size."""
        size = shutil.get_terminal_size((80, 24))
        self._term_size = (size.columns, size.lines)

    def update(self, **kwargs):
        """Update status fields and redraw."""
        for k, v in kwargs.items():
            if hasattr(self, k):
                setattr(self, k, v)
        self._render()

    def _render(self):
        """Render minimap and status centered in terminal."""
        if not self._started:
            return

        term_w, term_h = self._term_size

        # Clear if needed (resize, etc)
        if self._needs_clear:
            sys.stdout.write("\033[2J")
            self._needs_clear = False

        l, r, t, b = self.crop
        sw, sh = self.source_size

        # Calculate viewport rect as fractions
        if sw > 0 and sh > 0:
            vp_left = l / sw
            vp_right = 1.0 - (r / sw)
            vp_top = t / sh
            vp_bot = 1.0 - (b / sh)
        else:
            vp_left, vp_top = 0, 0
            vp_right, vp_bot = 1, 1

        # Convert to character positions
        x1 = int(vp_left * self.MAP_W)
        x2 = int(vp_right * self.MAP_W)
        y1 = int(vp_top * self.MAP_H)
        y2 = int(vp_bot * self.MAP_H)
        x1 = max(0, min(x1, self.MAP_W - 1))
        x2 = max(x1 + 1, min(x2, self.MAP_W))
        y1 = max(0, min(y1, self.MAP_H - 1))
        y2 = max(y1 + 1, min(y2, self.MAP_H))

        # Build minimap lines
        map_lines = []
        map_lines.append("┌" + "─" * self.MAP_W + "┐")
        for row in range(self.MAP_H):
            line = "│"
            for col in range(self.MAP_W):
                if y1 <= row < y2 and x1 <= col < x2:
                    line += "\033[42m \033[0m"  # Green bg for viewport
                else:
                    line += "\033[100m \033[0m"  # Gray bg for outside
            line += "│"
            map_lines.append(line)
        map_lines.append("└" + "─" * self.MAP_W + "┘")

        # Status line
        src_str = f"{sw}x{sh}" if sw > 0 else "?"
        crop_str = f"{self.content_size[0]}x{self.content_size[1]}" if self.content_size[0] > 0 else "?"
        mode_color = "\033[32m" if self.mode == "delta" else ("\033[33m" if self.mode == "full" else "\033[36m")
        status = (
            f"\033[36mFRM\033[0m {self.frames_sent:4d}  "
            f"\033[36mFPS\033[0m {self.fps:4.1f}  "
            f"{mode_color}{self.mode.upper()}\033[0m {self.delta_pct:4.0f}%Δ  "
            f"\033[33mSRC\033[0m {src_str}  "
            f"\033[32mOUT\033[0m {crop_str}"
        )
        if self.action:
            status += f"  \033[35m{self.action}\033[0m"
        if self.stalls:
            status += f"  \033[31mWIFI:{self.stalls}\033[0m"
        if self.errors:
            status += f"  \033[31mERR:{self.errors}\033[0m"

        # Help line
        help_line = "\033[90m[4]4:3 [+/-]zoom [←↑↓→]pan [wasd]crop [r]eset [^C]quit\033[0m"

        # Log lines (most recent)
        log_display = self._log[-self.LOG_LINES:] if self._log else []

        # Calculate layout
        content_height = self.MAP_H + 4 + self.LOG_LINES  # box + status + help + log
        content_width = self.MAP_W + 2

        start_row = max(1, (term_h - content_height) // 2)
        pad_left = max(0, (term_w - content_width) // 2)
        padding = " " * pad_left

        # Build output with cursor positioning
        output = ""
        row = start_row

        # Log area first (above minimap)
        log_width = min(term_w - 4, 70)
        log_pad = max(0, (term_w - log_width) // 2)
        for i in range(self.LOG_LINES):
            if i < len(log_display):
                msg = log_display[i][:log_width]  # Truncate to fit
                output += f"\033[{row};1H\033[K{' ' * log_pad}\033[90m{msg}\033[0m"
            else:
                output += f"\033[{row};1H\033[K"
            row += 1

        # Minimap
        for line in map_lines:
            output += f"\033[{row};1H\033[K{padding}{line}"
            row += 1

        # Status (centered separately since it has different width)
        status_pad = max(0, (term_w - 60) // 2)
        output += f"\033[{row};1H\033[K{' ' * status_pad}{status}"
        row += 1

        # Help line
        help_pad = max(0, (term_w - 55) // 2)
        output += f"\033[{row};1H\033[K{' ' * help_pad}{help_line}"

        sys.stdout.write(output)
        sys.stdout.flush()

    def message(self, msg):
        """Add to log and show via action."""
        self.log(msg)
        self.action = msg

    def clear(self):
        """Clear and restore terminal."""
        # Show cursor, clear screen
        sys.stdout.write("\033[?25h\033[2J\033[H")
        sys.stdout.flush()
PORTAL_PATH = "/org/freedesktop/portal/desktop"
SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast"
REQUEST_IFACE = "org.freedesktop.portal.Request"


def find_pico_serial_port():
    """Auto-detect Pico USB CDC port. Returns None if not connected."""
    patterns = [
        '/dev/ttyACM*',      # Linux
        '/dev/cu.usbmodem*', # macOS
    ]
    for pattern in patterns:
        ports = glob.glob(pattern)
        if ports:
            return sorted(ports)[0]
    return None


# Serial protocol constants
SERIAL_CMD_FRAME = 0xFE
SERIAL_CMD_DELTA = 0xFD
SERIAL_CMD_BRIGHTNESS = 0xFC
SERIAL_RESP_OK = 0x01
SERIAL_RESP_BUSY = 0x02
SERIAL_RESP_ERROR = 0x03


class SerialFrameSender:
    """Send frames over USB CDC serial - lower latency than WiFi."""

    DELTA_THRESHOLD = 150

    def __init__(self, port):
        self.port = port
        # USB CDC ignores baud rate - runs at USB full speed (12 Mbps)
        # Set high value anyway for documentation
        self.ser = serial.Serial(port, 921600, timeout=0.1)
        # Disable output buffering for lower latency
        self.ser.write_timeout = 0.05
        self.prev_frame = None
        self.prev_crc = None
        self._pending_crc = None
        self.mode = "auto"
        # Stats
        self.stats_delta = 0
        self.stats_full = 0
        self.stats_skipped = 0
        self.stats_stalls = 0
        self.stats_bytes = 0
        # Pre-allocated buffers to avoid allocations in hot path
        self._frame_buf = bytearray(1 + FRAME_SIZE)  # cmd + frame
        self._frame_buf[0] = SERIAL_CMD_FRAME
        self._delta_buf = bytearray(3 + 1024 * 5)  # cmd + count + entries
        self._delta_buf[0] = SERIAL_CMD_DELTA
        self._stats = {
            "delta": 0, "full": 0, "skipped": 0, "stalls": 0,
            "total": 0, "bytes": 0, "delta_pct": 0.0
        }

    def send(self, rgb_data):
        """Send frame, using delta encoding when beneficial."""
        if self.mode == "full":
            return self._send_full(rgb_data)

        # Check for identical frame
        frame_crc = zlib.crc32(rgb_data)
        self._pending_crc = frame_crc
        if self.prev_frame is not None and frame_crc == self.prev_crc:
            self.stats_skipped += 1
            return True

        # Try delta
        if self.prev_frame is not None and self.mode != "full":
            delta = self._build_delta(rgb_data)
            if delta is not None and len(delta) < FRAME_SIZE:
                return self._send_delta(rgb_data, delta)

        return self._send_full(rgb_data)

    def _send_full(self, rgb_data):
        """Send full frame over serial."""
        try:
            # Copy frame data into pre-allocated buffer (avoids allocation)
            self._frame_buf[1:] = rgb_data
            self.ser.write(self._frame_buf)
            resp = self.ser.read(1)
            if resp and resp[0] == SERIAL_RESP_OK:
                self.prev_frame = rgb_data
                self.prev_crc = self._pending_crc or zlib.crc32(rgb_data)
                self.stats_full += 1
                self.stats_bytes += FRAME_SIZE + 1
                return True
            elif resp and resp[0] == SERIAL_RESP_BUSY:
                self.stats_skipped += 1
                return True
            else:
                self.stats_stalls += 1
                return False
        except Exception:
            self.stats_stalls += 1
            return False

    def _build_delta(self, rgb_data):
        """Build delta packet in pre-allocated buffer."""
        buf = self._delta_buf
        count = 0
        offset = 3  # 1 cmd + 2 count (cmd already set in __init__)

        prev = self.prev_frame
        threshold = self.DELTA_THRESHOLD
        for i in range(0, FRAME_SIZE, 3):
            if (rgb_data[i] != prev[i] or
                rgb_data[i+1] != prev[i+1] or
                rgb_data[i+2] != prev[i+2]):
                struct.pack_into('<HBBB', buf, offset, i // 3,
                                 rgb_data[i], rgb_data[i+1], rgb_data[i+2])
                offset += 5
                count += 1
                if count > threshold:
                    return None  # Too many changes

        if count == 0:
            return None  # No changes - will be caught by CRC check

        struct.pack_into('<H', buf, 1, count)
        # Return memoryview to avoid copy
        return memoryview(buf)[:offset]

    def _send_delta(self, rgb_data, delta):
        """Send delta frame over serial."""
        try:
            delta_len = len(delta)
            self.ser.write(delta)
            resp = self.ser.read(1)
            if resp and resp[0] == SERIAL_RESP_OK:
                self.prev_frame = rgb_data
                self.prev_crc = self._pending_crc or zlib.crc32(rgb_data)
                self.stats_delta += 1
                self.stats_bytes += delta_len
                return True
            elif resp and resp[0] == SERIAL_RESP_BUSY:
                self.stats_skipped += 1
                return True
            else:
                self.stats_stalls += 1
                return False
        except Exception:
            self.stats_stalls += 1
            return False

    def get_stats(self):
        """Return stats dict."""
        total = self.stats_delta + self.stats_full + self.stats_skipped
        self._stats["delta"] = self.stats_delta
        self._stats["full"] = self.stats_full
        self._stats["skipped"] = self.stats_skipped
        self._stats["stalls"] = self.stats_stalls
        self._stats["total"] = total
        self._stats["bytes"] = self.stats_bytes
        self._stats["delta_pct"] = (self.stats_delta / total * 100) if total > 0 else 0
        return self._stats

    def close(self):
        self.ser.close()


class FrameSender:
    """Persistent HTTP connection to the Cosmic Unicorn."""

    # Threshold: if more than this many pixels changed, send full frame
    # Delta format: 2 bytes count + 5 bytes per pixel (2 index + 3 RGB)
    # Full frame: 3072 bytes. Break-even at (3072 - 2) / 5 = 614 pixels
    # But perf testing shows delta only faster when ~100+ pixels change,
    # so we use a lower threshold to prefer full frames for small changes
    DELTA_THRESHOLD = 150

    def __init__(self, host, mode="auto"):
        self.host = host
        self.conn = None
        self.prev_frame = None
        self.prev_crc = None  # Fast identical-frame detection
        self._pending_crc = None  # CRC computed in _build_delta, reused in send
        self.mode = mode  # "auto", "delta", "full"
        # Stats
        self.stats_delta = 0
        self.stats_full = 0
        self.stats_skipped = 0
        self.stats_stalls = 0  # Timeout/connection failures
        self.stats_bytes = 0
        # Pre-allocated buffers to avoid allocations in hot path
        self._delta_buf = bytearray(2 + 1024 * 5)  # Max delta size
        self._headers = {"Content-Type": "application/octet-stream"}
        # Reusable stats dict to avoid allocations in tight loop
        self._stats = {
            "delta": 0, "full": 0, "skipped": 0, "stalls": 0,
            "total": 0, "bytes": 0, "delta_pct": 0.0
        }

    def _connect(self):
        self.conn = http.client.HTTPConnection(self.host, timeout=2.0)  # 2s for initial connect
        self.conn.connect()
        # Prioritize packets for low-latency streaming
        sock = self.conn.sock
        sock.settimeout(0.1)  # 100ms timeout for requests - fail fast
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_PRIORITY, 6)   # 0-7, higher = more priority
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0x10)     # IPTOS_LOWDELAY
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # Send immediately, don't batch
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1) # Disable delayed ACKs
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)  # Smaller send buffer for lower latency
        # Aggressive keepalive to detect dead connections faster
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 1)   # Start keepalive after 1s idle
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 1)  # 1s between probes
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 2)    # 2 failed probes = dead

    def _build_delta(self, rgb_data):
        """Compare with previous frame, return packed delta bytes or None if full frame needed."""
        if self.prev_frame is None or len(self.prev_frame) != FRAME_SIZE:
            self._pending_crc = None
            return None  # No previous frame or size mismatch, send full

        # Fast path: CRC check for identical frames (common with static content)
        frame_crc = zlib.crc32(rgb_data)
        self._pending_crc = frame_crc  # Cache for reuse in send()
        if frame_crc == self.prev_crc:
            return b''  # Identical frame, skip

        # Reuse pre-allocated buffer
        buf = self._delta_buf
        count = 0
        offset = 2

        prev = self.prev_frame
        for i in PIXEL_RANGE:
            if (rgb_data[i] != prev[i] or
                rgb_data[i+1] != prev[i+1] or
                rgb_data[i+2] != prev[i+2]):
                struct.pack_into('<HBBB', buf, offset, i // 3,
                                 rgb_data[i], rgb_data[i+1], rgb_data[i+2])
                offset += 5
                count += 1
                if count > self.DELTA_THRESHOLD:
                    return None  # Too many changes, send full frame

        # Write count at start, return view of buffer (no copy)
        # Skip if 5 or fewer pixels changed (likely noise/artifacts)
        struct.pack_into('<H', buf, 0, count)
        return memoryview(buf)[:offset] if count > 5 else b''

    def send(self, rgb_data):
        """POST frame data. Uses delta encoding when beneficial."""
        # Determine what to send based on mode
        if self.mode == "full":
            # Force full frame
            endpoint = "/api/frame"
            body = rgb_data
            is_delta = False
        else:
            # Delta or auto mode: try to build delta
            delta = self._build_delta(rgb_data)
            if delta == b'':
                # No/trivial changes, skip
                self.stats_skipped += 1
                return True
            if delta is not None and (self.mode == "delta" or len(delta) < FRAME_SIZE):
                # Use delta (forced in delta mode, or smaller in auto mode)
                endpoint = "/api/delta"
                body = delta
                is_delta = True
            else:
                # No previous frame or delta too large
                endpoint = "/api/frame"
                body = rgb_data
                is_delta = False

        for attempt in range(2):
            try:
                if self.conn is None:
                    self._connect()
                self.conn.request("POST", endpoint, body=body,
                                  headers=self._headers)
                resp = self.conn.getresponse()
                resp_body = resp.read()
                if resp.status == 200:
                    # Check for busy response (device still drawing previous frame)
                    if b'"busy"' in resp_body:
                        self.stats_skipped += 1
                        return True  # Not an error, just skip
                    self.prev_frame = rgb_data
                    # Reuse CRC from _build_delta if available, else compute
                    self.prev_crc = self._pending_crc or zlib.crc32(rgb_data)
                    self.stats_bytes += len(body)
                    if is_delta:
                        self.stats_delta += 1
                    else:
                        self.stats_full += 1
                    return True
                return False
            except Exception:
                self.conn = None  # reconnect on next attempt
                self.stats_stalls += 1
        return False

    def get_stats(self):
        """Return stats dict (reuses internal dict to avoid allocations)."""
        total = self.stats_delta + self.stats_full + self.stats_skipped
        self._stats["delta"] = self.stats_delta
        self._stats["full"] = self.stats_full
        self._stats["skipped"] = self.stats_skipped
        self._stats["stalls"] = self.stats_stalls
        self._stats["total"] = total
        self._stats["bytes"] = self.stats_bytes
        self._stats["delta_pct"] = (self.stats_delta / total * 100) if total > 0 else 0
        return self._stats


class PortalScreenCast:
    """Manages an XDG Desktop Portal ScreenCast session."""

    def __init__(self, status=None):
        DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SessionBus()
        self.portal = self.bus.get_object(PORTAL_BUS, PORTAL_PATH)
        self.screencast = dbus.Interface(self.portal, SCREENCAST_IFACE)
        self.loop = GLib.MainLoop()
        self.session_path = None
        self.pipewire_node = None
        self.pipewire_fd = None
        self._request_count = 0
        self._sender = self.bus.get_unique_name().replace(".", "_").lstrip(":")
        self._status = status

    def _log(self, msg):
        if self._status:
            self._status.log(msg)
        else:
            print(msg)

    def _next_token(self):
        self._request_count += 1
        return f"u{self._request_count}"

    def _request_path(self, token):
        return f"/org/freedesktop/portal/desktop/request/{self._sender}/{token}"

    def _call_with_response(self, method, *args, options=None):
        """Call a portal method and wait for the Response signal."""
        if options is None:
            options = {}
        token = self._next_token()
        options["handle_token"] = token
        request_path = self._request_path(token)

        result = [None]

        def on_response(response_code, response_result):
            result[0] = (response_code, response_result)
            self.loop.quit()

        self.bus.add_signal_receiver(
            on_response,
            signal_name="Response",
            dbus_interface=REQUEST_IFACE,
            path=request_path,
        )

        method(*args, options)

        self.loop.run()
        return result[0]

    def start(self):
        """Create session, select sources, and start the screencast."""
        # Create session
        self._log("Creating portal session...")
        code, result = self._call_with_response(
            self.screencast.CreateSession,
            options={"session_handle_token": "unicorn"},
        )
        if code != 0:
            print(f"ERROR: CreateSession failed (code {code})", file=sys.stderr)
            sys.exit(1)
        self.session_path = result["session_handle"]

        # Select sources (user picks window/monitor)
        self._log("Select a window or monitor to share...")
        code, result = self._call_with_response(
            self.screencast.SelectSources,
            self.session_path,
            options={
                "types": dbus.UInt32(2),  # windows only
                "persist_mode": dbus.UInt32(2),  # persist until revoked
            },
        )
        if code != 0:
            print(f"ERROR: SelectSources failed (code {code})", file=sys.stderr)
            sys.exit(1)

        # Start (returns PipeWire node ID)
        self._log("Starting screencast...")
        code, result = self._call_with_response(
            self.screencast.Start,
            self.session_path,
            "",  # parent window
        )
        if code != 0:
            print(f"ERROR: Start failed (code {code})", file=sys.stderr)
            sys.exit(1)

        streams = result.get("streams", [])
        if not streams:
            print("ERROR: No streams returned", file=sys.stderr)
            sys.exit(1)

        self.pipewire_node = int(streams[0][0])
        props = streams[0][1]
        size = props.get("size", (0, 0))
        self._log(f"Stream: node={self.pipewire_node} size={size[0]}x{size[1]}")

        # Get PipeWire remote fd from portal
        pw_fd = self.screencast.OpenPipeWireRemote(
            self.session_path, dbus.Dictionary(signature="sv")
        )
        self.pipewire_fd = pw_fd.take()
        return self.pipewire_node, self.pipewire_fd


def main():
    parser = argparse.ArgumentParser(description="Screen capture to Cosmic Unicorn streamer")
    parser.add_argument("--host", default="cosmic.lan", help="Device hostname")
    parser.add_argument("--mode", choices=["auto", "delta", "full"], default="auto",
                        help="Frame mode: auto (delta when beneficial), delta (force), full (force)")
    parser.add_argument("--fps", type=int, default=None,
                        help="Target frame rate (default: 100 for serial, 30 for WiFi)")
    parser.add_argument("--wifi", action="store_true",
                        help="Force WiFi mode even if serial is available")
    args = parser.parse_args()

    # Create and show status UI immediately
    status = StatusUI()
    status.start()

    Gst.init(None)

    # Test device connectivity
    status.log(f"Connecting to {args.host}...")
    try:
        with urllib.request.urlopen(f"http://{args.host}/api/status", timeout=3) as resp:
            status.log(f"Device online: {resp.read().decode()[:60]}")
    except Exception as e:
        status.log(f"WARNING: Can't reach {args.host}: {e}")

    # Set up portal screencast
    status.log("Setting up screen capture portal...")
    portal = PortalScreenCast(status)
    node_id, pw_fd = portal.start()

    # Pipeline: crop at native res (cheap, no format conversion yet),
    # then videoconvert + scale only on the cropped region
    pipeline_str = (
        f"pipewiresrc fd={pw_fd} path={node_id} ! "
        f"video/x-raw,pixel-aspect-ratio=1/1 ! "
        f"videocrop name=crop ! "
        f"videoconvert ! "
        f"videoscale method=bilinear add-borders=false ! "
        f"video/x-raw,format=RGB,width={WIDTH},height={HEIGHT},pixel-aspect-ratio=1/1 ! "
        f"appsink name=sink emit-signals=true drop=true max-buffers=1"
    )
    status.log(f"Pipeline ready, starting stream...")

    pipeline = Gst.parse_launch(pipeline_str)
    appsink = pipeline.get_by_name("sink")
    crop_elem = pipeline.get_by_name("crop")

    # Stream dimensions (filled in by first frame)
    stream_size = [0, 0]

    # Monitor pipeline bus for errors
    bus = pipeline.get_bus()
    bus.add_signal_watch()
    def on_bus_message(bus, msg):
        t = msg.type
        if t == Gst.MessageType.ERROR:
            err, dbg = msg.parse_error()
            status.log(f"GStreamer ERROR: {err.message}")
        elif t == Gst.MessageType.WARNING:
            err, dbg = msg.parse_warning()
            status.log(f"GStreamer WARNING: {err.message}")
    bus.connect("message", on_bus_message)

    # Frame buffer shared between GStreamer callback and send loop
    frame_buf = [None]
    frame_id = [0]
    frame_cond = threading.Condition()
    crop_state = {"last_time": 0, "first": True, "locked": False}

    def on_new_sample(sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        buf = sample.get_buffer()
        ok, mapinfo = buf.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK
        data = bytes(mapinfo.data[:FRAME_SIZE])
        buf.unmap(mapinfo)

        if crop_state["first"]:
            # Get native stream size from pipewiresrc's src pad
            pw_src = pipeline.get_by_name("crop")
            pad = pw_src.get_static_pad("sink")
            caps = pad.get_current_caps()
            if caps:
                s = caps.get_structure(0)
                stream_size[0] = s.get_int("width").value
                stream_size[1] = s.get_int("height").value
                status.log(f"Native stream: {stream_size[0]}x{stream_size[1]}")
            crop_state["first"] = False

        # Auto-crop disabled - use manual controls (wasd/arrows) or press 'r' to reset
        # The auto-detection was unreliable on 32x32 downscaled frames

        with frame_cond:
            frame_buf[0] = data
            frame_id[0] += 1
            frame_cond.notify()
        return Gst.FlowReturn.OK

    def _update_crop(data, native_w, native_h):
        """Detect black borders + decorations on 32x32 frame, scale to native crop."""
        threshold = 30
        stride = WIDTH * 3

        # Scan from left
        left = 0
        for col in range(WIDTH):
            for row in range(HEIGHT):
                off = row * stride + col * 3
                if data[off] + data[off+1] + data[off+2] > threshold:
                    left = col
                    break
            else:
                continue
            break

        # Scan from right
        right = 0
        for col in range(WIDTH - 1, left, -1):
            for row in range(HEIGHT):
                off = row * stride + col * 3
                if data[off] + data[off+1] + data[off+2] > threshold:
                    right = WIDTH - 1 - col
                    break
            else:
                continue
            break

        # Scan from top
        top = 0
        for row in range(HEIGHT):
            off = row * stride
            if any(data[off+i] + data[off+i+1] + data[off+i+2] > threshold
                   for i in range(0, stride, 3)):
                top = row
                break

        # Scan from bottom
        bottom = 0
        for row in range(HEIGHT - 1, top, -1):
            off = row * stride
            if any(data[off+i] + data[off+i+1] + data[off+i+2] > threshold
                   for i in range(0, stride, 3)):
                bottom = HEIGHT - 1 - row
                break

        # Trim low-saturation decoration rows (title bar, status bar)
        content_h = HEIGHT - top - bottom
        max_dec = max(1, content_h // 8)

        for row in range(top, top + max_dec):
            off = row * stride
            sat_count = sum(1 for x in range(0, stride, 3)
                           if max(data[off+x], data[off+x+1], data[off+x+2])
                            - min(data[off+x], data[off+x+1], data[off+x+2]) > 40)
            if sat_count / (WIDTH) > 0.2:
                break
            top = row + 1

        for row in range(HEIGHT - 1 - bottom, HEIGHT - 1 - bottom - max_dec, -1):
            off = row * stride
            sat_count = sum(1 for x in range(0, stride, 3)
                           if max(data[off+x], data[off+x+1], data[off+x+2])
                            - min(data[off+x], data[off+x+1], data[off+x+2]) > 40)
            if sat_count / (WIDTH) > 0.2:
                break
            bottom = HEIGHT - row

        # Scale 32x32 crop values to native resolution
        # Account for current crop already applied
        cur_l = crop_elem.get_property("left")
        cur_r = crop_elem.get_property("right")
        cur_t = crop_elem.get_property("top")
        cur_b = crop_elem.get_property("bottom")
        cur_w = native_w - cur_l - cur_r
        cur_h = native_h - cur_t - cur_b

        n_left = cur_l + int(left * cur_w / WIDTH)
        n_right = cur_r + int(right * cur_w / WIDTH)
        n_top = cur_t + int(top * cur_h / HEIGHT)
        n_bottom = cur_b + int(bottom * cur_h / HEIGHT)

        new = (n_left, n_right, n_top, n_bottom)
        old = (cur_l, cur_r, cur_t, cur_b)
        content_w = native_w - n_left - n_right
        content_h = native_h - n_top - n_bottom
        if new != old and content_w > 64 and content_h > 64:
            print(f"  Auto-crop: left={n_left} right={n_right} top={n_top} bottom={n_bottom} "
                  f"(content: {content_w}x{content_h})")
            crop_elem.set_property("left", n_left)
            crop_elem.set_property("right", n_right)
            crop_elem.set_property("top", n_top)
            crop_elem.set_property("bottom", n_bottom)
            return True
        return new != (0, 0, 0, 0)  # lock even if unchanged, as long as crop is non-zero

    appsink.connect("new-sample", on_new_sample)

    # Run GLib main loop in a thread (drives PipeWire + GStreamer bus signals)
    glib_loop = GLib.MainLoop()
    glib_thread = threading.Thread(target=glib_loop.run, daemon=True)
    glib_thread.start()

    # Crop adjustment helpers
    NUDGE = 20  # pixels per keypress at native resolution

    def nudge_crop(side, delta):
        """Adjust one side of the crop. Clamps to valid range."""
        val = crop_elem.get_property(side) + delta
        cur_l = crop_elem.get_property("left")
        cur_r = crop_elem.get_property("right")
        cur_t = crop_elem.get_property("top")
        cur_b = crop_elem.get_property("bottom")
        nw, nh = stream_size
        if side == "left":
            val = max(0, min(val, nw - cur_r - 64))
        elif side == "right":
            val = max(0, min(val, nw - cur_l - 64))
        elif side == "top":
            val = max(0, min(val, nh - cur_b - 64))
        elif side == "bottom":
            val = max(0, min(val, nh - cur_t - 64))
        crop_elem.set_property(side, val)

    def move_crop(dx, dy):
        """Shift the crop region. Moves each axis independently. Returns action string."""
        nw, nh = stream_size
        if nw <= 0 or nh <= 0:
            return "wait"

        cur_l = crop_elem.get_property("left")
        cur_r = crop_elem.get_property("right")
        cur_t = crop_elem.get_property("top")
        cur_b = crop_elem.get_property("bottom")

        msg = []
        # Move horizontally if dx != 0
        if dx != 0:
            new_l = cur_l + dx
            new_r = cur_r - dx
            if new_l < 0 or new_r < 0:
                msg.append("H-edge")
            elif nw - new_l - new_r < 64:
                msg.append("H-min")
            else:
                crop_elem.set_property("left", new_l)
                crop_elem.set_property("right", new_r)

        # Move vertically if dy != 0
        if dy != 0:
            new_t = cur_t + dy
            new_b = cur_b - dy
            if new_t < 0 or new_b < 0:
                msg.append("V-edge")
            elif nh - new_t - new_b < 64:
                msg.append("V-min")
            else:
                crop_elem.set_property("top", new_t)
                crop_elem.set_property("bottom", new_b)

        return "pan" + (f" ({', '.join(msg)})" if msg else "")

    def do_recrop():
        """Reset crop to zero (full frame)."""
        crop_elem.set_property("left", 0)
        crop_elem.set_property("right", 0)
        crop_elem.set_property("top", 0)
        crop_elem.set_property("bottom", 0)

    def scroll_crop(dx, dy):
        """Scroll the view by adjusting crop asymmetrically. Always works."""
        nw, nh = stream_size
        cur_l = crop_elem.get_property("left")
        cur_r = crop_elem.get_property("right")
        cur_t = crop_elem.get_property("top")
        cur_b = crop_elem.get_property("bottom")

        # Scroll horizontally
        if dx > 0:  # scroll right - show content from right side
            new_l = cur_l + NUDGE
            new_r = max(0, cur_r - NUDGE)
            if nw - new_l - new_r >= 64:
                crop_elem.set_property("left", new_l)
                crop_elem.set_property("right", new_r)
        elif dx < 0:  # scroll left - show content from left side
            new_l = max(0, cur_l - NUDGE)
            new_r = cur_r + NUDGE
            if nw - new_l - new_r >= 64:
                crop_elem.set_property("left", new_l)
                crop_elem.set_property("right", new_r)

        # Scroll vertically
        if dy > 0:  # scroll down - show content from bottom
            new_t = cur_t + NUDGE
            new_b = max(0, cur_b - NUDGE)
            if nh - new_t - new_b >= 64:
                crop_elem.set_property("top", new_t)
                crop_elem.set_property("bottom", new_b)
        elif dy < 0:  # scroll up - show content from top
            new_t = max(0, cur_t - NUDGE)
            new_b = cur_b + NUDGE
            if nh - new_t - new_b >= 64:
                crop_elem.set_property("top", new_t)
                crop_elem.set_property("bottom", new_b)

    def zoom(delta):
        """Zoom in (positive delta) or out (negative delta) from center."""
        nw, nh = stream_size
        if nw <= 0 or nh <= 0:
            return
        cur_l = crop_elem.get_property("left")
        cur_r = crop_elem.get_property("right")
        cur_t = crop_elem.get_property("top")
        cur_b = crop_elem.get_property("bottom")
        # Calculate aspect-correct zoom (maintain current aspect ratio)
        cur_w = nw - cur_l - cur_r
        cur_h = nh - cur_t - cur_b
        if cur_w <= 0 or cur_h <= 0:
            return
        aspect = cur_w / cur_h
        # Horizontal delta, vertical delta scaled by aspect
        dh = delta
        dv = int(delta / aspect) if aspect > 0 else delta
        new_l = max(0, cur_l + dh)
        new_r = max(0, cur_r + dh)
        new_t = max(0, cur_t + dv)
        new_b = max(0, cur_b + dv)
        # Ensure minimum size
        if nw - new_l - new_r >= 64 and nh - new_t - new_b >= 64:
            crop_elem.set_property("left", new_l)
            crop_elem.set_property("right", new_r)
            crop_elem.set_property("top", new_t)
            crop_elem.set_property("bottom", new_b)

    def crop_4_3():
        """Crop to 4:3 content centered in the window (removes pillarboxing)."""
        nw, nh = stream_size
        if nw <= 0 or nh <= 0:
            return
        # Calculate 4:3 region centered in window
        target_w = int(nh * 4 / 3)
        if target_w > nw:
            # Window is narrower than 4:3 - crop top/bottom instead
            target_h = int(nw * 3 / 4)
            side = (nh - target_h) // 2
            crop_elem.set_property("left", 0)
            crop_elem.set_property("right", 0)
            crop_elem.set_property("top", side)
            crop_elem.set_property("bottom", side)
        else:
            # Normal case: pillarboxing (black bars on sides)
            side = (nw - target_w) // 2
            crop_elem.set_property("left", side)
            crop_elem.set_property("right", side)
            crop_elem.set_property("top", 0)
            crop_elem.set_property("bottom", 0)

    # Auto-detect serial connection (preferred) or fall back to WiFi
    serial_port = find_pico_serial_port()
    using_serial = False
    if serial_port and not args.wifi:
        try:
            sender = SerialFrameSender(serial_port)
            sender.mode = args.mode
            using_serial = True
        except Exception as e:
            status.log(f"Serial failed ({e}), falling back to WiFi")
            sender = FrameSender(args.host, mode=args.mode)
    else:
        sender = FrameSender(args.host, mode=args.mode)

    # Frame rate: use appropriate default based on connection type
    # Serial can handle ~100 FPS, WiFi tops out around 30 FPS
    if args.fps is not None:
        target_fps = args.fps
    else:
        target_fps = 100 if using_serial else 30

    if using_serial:
        status.log(f"Serial: {serial_port}, mode: {args.mode}, target {target_fps} FPS")
    else:
        status.log(f"WiFi: {args.host}, mode: {args.mode}, target {target_fps} FPS")
    min_frame_interval = 1.0 / target_fps if target_fps > 0 else 0
    last_send_time = 0.0

    # Start pipeline
    pipeline.set_state(Gst.State.PLAYING)
    status.log("Streaming started")

    # Wait for first frame
    time.sleep(0.5)

    frames_sent = 0
    send_errors = 0
    t_start = time.monotonic()
    last_sent_id = 0

    # Instantaneous FPS tracking (always on, rolling window)
    fps_tracker = InstantFPS(window_size=30)

    # Display throttling - decouple UI updates from frame rate
    last_display_time = 0.0
    DISPLAY_INTERVAL = 0.1  # Max 10 UI updates/sec

    def update_status_crop(action=""):
        """Update status with current crop values."""
        nw, nh = stream_size
        l = crop_elem.get_property("left")
        r = crop_elem.get_property("right")
        t = crop_elem.get_property("top")
        b = crop_elem.get_property("bottom")
        cw = nw - l - r if nw > 0 else 0
        ch = nh - t - b if nh > 0 else 0
        status.update(crop=[l, r, t, b], source_size=[nw, nh], content_size=[cw, ch], action=action)

    # Input handling with dedicated thread for reliable escape sequence parsing
    key_queue = queue.Queue()
    input_running = [True]

    # Save terminal settings BEFORE any changes
    fd = sys.stdin.fileno()
    original_term_settings = termios.tcgetattr(fd)

    def input_thread_func():
        """Read keys in blocking mode and put parsed keys into queue."""
        tty.setcbreak(fd)  # cbreak mode: char-at-a-time but keeps output processing
        try:
            while input_running[0]:
                ch = sys.stdin.read(1)
                if not ch or ch == '\x03':  # EOF or Ctrl+C
                    break
                if ch == '\x1b':  # Start of escape sequence
                    # Read next two chars (blocking) - escape sequences are '[' + letter
                    ch2 = sys.stdin.read(1)
                    if ch2 == '[':
                        ch3 = sys.stdin.read(1)
                        key_queue.put(('arrow', ch3))
                    else:
                        # Not an arrow, put both chars
                        key_queue.put(('key', ch2))
                else:
                    key_queue.put(('key', ch))
        except Exception:
            pass

    input_thread = threading.Thread(target=input_thread_func, daemon=True)
    input_thread.start()

    try:
        while True:
            # Check for key input from queue (non-blocking)
            action = ""
            try:
                key_type, key_val = key_queue.get_nowait()
                if key_type == 'arrow':
                    if key_val == 'A':    # up
                        action = move_crop(0, -NUDGE)
                    elif key_val == 'B':  # down
                        action = move_crop(0, NUDGE)
                    elif key_val == 'C':  # right
                        action = move_crop(NUDGE, 0)
                    elif key_val == 'D':  # left
                        action = move_crop(-NUDGE, 0)
                elif key_type == 'key':
                    ch = key_val
                    if ch == 'r':
                        do_recrop()
                        action = "reset"
                    elif ch == '4':
                        crop_4_3()
                        action = "4:3"
                    elif ch in ('+', '='):
                        zoom(NUDGE)
                        action = "zoom+"
                    elif ch == '-':
                        zoom(-NUDGE)
                        action = "zoom-"
                    elif ch == 'a':
                        nudge_crop("left", NUDGE)
                        action = "crop L+"
                    elif ch == 'A':
                        nudge_crop("left", -NUDGE)
                        action = "crop L-"
                    elif ch == 'd':
                        nudge_crop("right", NUDGE)
                        action = "crop R+"
                    elif ch == 'D':
                        nudge_crop("right", -NUDGE)
                        action = "crop R-"
                    elif ch == 'w':
                        nudge_crop("top", NUDGE)
                        action = "crop T+"
                    elif ch == 'W':
                        nudge_crop("top", -NUDGE)
                        action = "crop T-"
                    elif ch == 's':
                        nudge_crop("bottom", NUDGE)
                        action = "crop B+"
                    elif ch == 'S':
                        nudge_crop("bottom", -NUDGE)
                        action = "crop B-"
                if action:
                    update_status_crop(action)
            except queue.Empty:
                pass

            with frame_cond:
                # Wait for new frame (with timeout to allow keyboard handling)
                while frame_id[0] == last_sent_id:
                    frame_cond.wait(timeout=0.01)
                current_id = frame_id[0]
                frame = frame_buf[0]

            # Frame rate limiting
            if min_frame_interval > 0:
                elapsed = time.monotonic() - last_send_time
                if elapsed < min_frame_interval:
                    time.sleep(min_frame_interval - elapsed)
            last_send_time = time.monotonic()

            t_send = time.monotonic()
            ok = sender.send(frame)
            send_ms = (time.monotonic() - t_send) * 1000

            last_sent_id = current_id
            frames_sent += 1
            if not ok:
                send_errors += 1

            # Record frame for instantaneous FPS calculation
            fps_tracker.tick()

            # Time-based display update (decoupled from frame rate)
            now = time.monotonic()
            if now - last_display_time >= DISPLAY_INTERVAL:
                last_display_time = now
                stats = sender.get_stats()
                status.update(
                    frames_sent=frames_sent, fps=fps_tracker.fps, send_ms=send_ms,
                    errors=send_errors, stalls=stats['stalls'],
                    mode=args.mode, delta_pct=stats['delta_pct']
                )

    except KeyboardInterrupt:
        pass
    finally:
        input_running[0] = False
        # Restore terminal settings
        termios.tcsetattr(fd, termios.TCSADRAIN, original_term_settings)
        status.clear()

        elapsed = time.monotonic() - t_start
        avg_fps = frames_sent / elapsed if elapsed > 0 else 0
        stats = sender.get_stats()
        print(f"Stopped. {frames_sent} frames in {elapsed:.1f}s (avg {avg_fps:.1f} FPS, final {fps_tracker.fps:.1f} FPS, {send_errors} errors)")
        print(f"  Mode: {args.mode} | Delta: {stats['delta']} | Full: {stats['full']} | "
              f"Skipped: {stats['skipped']} | Delta%: {stats['delta_pct']:.1f}%")
        print(f"  Bytes sent: {stats['bytes']:,} ({stats['bytes']/1024:.1f} KB)")

    pipeline.set_state(Gst.State.NULL)
    glib_loop.quit()


if __name__ == "__main__":
    main()
