#!/usr/bin/env python3
"""
Serial streaming client for UnicornLEDStreamLite.

Much lower latency than WiFi - direct USB CDC connection.

Protocol:
  0xFE + 3072 bytes = full frame
  0xFD + u16 count + (u16 idx, u8 r, u8 g, u8 b) * count = delta
  0xFC + u8 brightness (0-255) = set brightness

Response: 0x01 (ok), 0x02 (busy), 0x03 (error)

Usage:
  python3 serial_stream.py [--port /dev/ttyACM0]
"""

import argparse
import glob
import serial
import struct
import statistics
import time


FRAME_SIZE = 32 * 32 * 3  # 3072 bytes

CMD_FRAME = 0xFE
CMD_DELTA = 0xFD
CMD_BRIGHTNESS = 0xFC

RESP_OK = 0x01
RESP_BUSY = 0x02
RESP_ERROR = 0x03


def find_pico_port():
    """Auto-detect Pico USB CDC port."""
    # Common patterns for Pico
    patterns = [
        '/dev/ttyACM*',      # Linux
        '/dev/cu.usbmodem*', # macOS
        '/dev/ttyUSB*',      # Linux USB-UART
    ]
    for pattern in patterns:
        ports = glob.glob(pattern)
        if ports:
            return sorted(ports)[0]
    return None


class SerialSender:
    """Send frames over serial with optimized buffering."""

    def __init__(self, port, baudrate=921600):
        # USB CDC ignores baud rate, but set high for documentation
        self.ser = serial.Serial(port, baudrate, timeout=0.1)
        self.ser.write_timeout = 0.05
        self.prev_frame = None
        self.stats_full = 0
        self.stats_delta = 0
        self.stats_busy = 0
        self.stats_error = 0
        # Pre-allocated buffer to avoid allocations
        self._frame_buf = bytearray(1 + FRAME_SIZE)
        self._frame_buf[0] = CMD_FRAME

    def send_frame(self, rgb_data: bytes) -> bool:
        """Send full frame using pre-allocated buffer."""
        self._frame_buf[1:] = rgb_data
        self.ser.write(self._frame_buf)
        resp = self.ser.read(1)
        if resp and resp[0] == RESP_OK:
            self.prev_frame = rgb_data
            self.stats_full += 1
            return True
        elif resp and resp[0] == RESP_BUSY:
            self.stats_busy += 1
            return True  # Not an error
        else:
            self.stats_error += 1
            return False

    def send_delta(self, rgb_data: bytes) -> bool:
        """Send delta frame if beneficial."""
        if self.prev_frame is None:
            return self.send_frame(rgb_data)

        # Find changed pixels
        changes = []
        for i in range(0, FRAME_SIZE, 3):
            if (rgb_data[i] != self.prev_frame[i] or
                rgb_data[i+1] != self.prev_frame[i+1] or
                rgb_data[i+2] != self.prev_frame[i+2]):
                idx = i // 3
                changes.append((idx, rgb_data[i], rgb_data[i+1], rgb_data[i+2]))

        if len(changes) == 0:
            return True  # No changes

        # Delta only beneficial if < ~600 pixels changed
        # Delta size: 1 + 2 + 5*n, Full size: 1 + 3072
        # Break-even at n = (3072 - 2) / 5 = 614
        if len(changes) > 600:
            return self.send_frame(rgb_data)

        # Build delta packet
        buf = bytearray(1 + 2 + len(changes) * 5)
        buf[0] = CMD_DELTA
        struct.pack_into('<H', buf, 1, len(changes))
        for i, (idx, r, g, b) in enumerate(changes):
            struct.pack_into('<HBBB', buf, 3 + i * 5, idx, r, g, b)

        self.ser.write(buf)
        resp = self.ser.read(1)
        if resp == bytes([RESP_OK]):
            self.prev_frame = rgb_data
            self.stats_delta += 1
            return True
        elif resp == bytes([RESP_BUSY]):
            self.stats_busy += 1
            return True
        else:
            self.stats_error += 1
            return False

    def set_brightness(self, value: float) -> bool:
        """Set brightness (0.0 - 1.0)."""
        val = int(max(0, min(255, value * 255)))
        self.ser.write(bytes([CMD_BRIGHTNESS, val]))
        resp = self.ser.read(1)
        return resp == bytes([RESP_OK])

    def close(self):
        self.ser.close()


def test_latency(sender, samples=100):
    """Measure round-trip latency."""
    print(f"\n=== Serial Latency Test ({samples} samples) ===")

    latencies = []
    frame = bytes([(i + 128) % 256 for i in range(FRAME_SIZE)])

    for i in range(samples):
        # Modify frame slightly
        frame = bytes([(b + 1) % 256 for b in frame[:100]]) + frame[100:]

        start = time.perf_counter()
        ok = sender.send_frame(frame)
        elapsed = (time.perf_counter() - start) * 1000

        if ok:
            latencies.append(elapsed)

        time.sleep(0.01)  # Small delay between frames

    if latencies:
        print(f"  Samples:  {len(latencies)}")
        print(f"  Min:      {min(latencies):.2f} ms")
        print(f"  Max:      {max(latencies):.2f} ms")
        print(f"  Mean:     {statistics.mean(latencies):.2f} ms")
        print(f"  Median:   {statistics.median(latencies):.2f} ms")
        if len(latencies) > 1:
            print(f"  Stdev:    {statistics.stdev(latencies):.2f} ms")
        print(f"  Busy:     {sender.stats_busy}")
        print(f"  Errors:   {sender.stats_error}")

    return latencies


def test_throughput(sender, duration=5):
    """Test maximum throughput."""
    print(f"\n=== Serial Throughput Test ({duration}s) ===")

    frame = bytes([i % 256 for i in range(FRAME_SIZE)])
    sent = 0
    start = time.perf_counter()

    while time.perf_counter() - start < duration:
        frame = bytes([(b + 1) % 256 for b in frame])
        if sender.send_frame(frame):
            sent += 1

    elapsed = time.perf_counter() - start
    fps = sent / elapsed

    print(f"  Duration: {elapsed:.1f}s")
    print(f"  Sent:     {sent} frames")
    print(f"  FPS:      {fps:.1f}")
    print(f"  Busy:     {sender.stats_busy}")
    print(f"  Errors:   {sender.stats_error}")

    return fps


def test_animation(sender, duration=5):
    """Show a test animation."""
    print(f"\n=== Animation Test ({duration}s) ===")

    start = time.perf_counter()
    frame_num = 0

    while time.perf_counter() - start < duration:
        # Rainbow sweep
        frame = bytearray(FRAME_SIZE)
        t = time.perf_counter() - start
        for y in range(32):
            for x in range(32):
                hue = (x + y + int(t * 30)) % 256
                r, g, b = hue_to_rgb(hue)
                idx = (y * 32 + x) * 3
                frame[idx] = r
                frame[idx + 1] = g
                frame[idx + 2] = b

        sender.send_frame(bytes(frame))
        frame_num += 1

    elapsed = time.perf_counter() - start
    print(f"  Frames: {frame_num}")
    print(f"  FPS:    {frame_num / elapsed:.1f}")


def hue_to_rgb(hue):
    """Convert hue (0-255) to RGB."""
    region = hue // 43
    remainder = (hue - region * 43) * 6
    if region == 0:
        return 255, remainder, 0
    elif region == 1:
        return 255 - remainder, 255, 0
    elif region == 2:
        return 0, 255, remainder
    elif region == 3:
        return 0, 255 - remainder, 255
    elif region == 4:
        return remainder, 0, 255
    else:
        return 255, 0, 255 - remainder


def main():
    parser = argparse.ArgumentParser(description="Serial streaming test")
    parser.add_argument("--port", help="Serial port (auto-detect if not specified)")
    parser.add_argument("--test", choices=["latency", "throughput", "animation", "all"],
                        default="all", help="Which test to run")
    args = parser.parse_args()

    port = args.port or find_pico_port()
    if not port:
        print("ERROR: No Pico found. Specify --port or connect device.")
        return 1

    print(f"Connecting to {port}...")
    sender = SerialSender(port)

    # Set initial brightness
    sender.set_brightness(0.5)

    tests = {
        'latency': lambda: test_latency(sender),
        'throughput': lambda: test_throughput(sender),
        'animation': lambda: test_animation(sender),
    }

    if args.test == "all":
        for name, fn in tests.items():
            fn()
    else:
        tests[args.test]()

    print("\n=== Tests Complete ===")
    sender.close()
    return 0


if __name__ == "__main__":
    exit(main())
