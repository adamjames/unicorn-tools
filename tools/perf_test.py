#!/usr/bin/env python3
"""
Performance test for UnicornLEDStreamLite firmware.

Tests:
1. Latency - round-trip time for frame delivery
2. Throughput - max frames/second at various rates
3. Busy rejection - behavior when sending faster than device can draw
4. Delta vs Full frame performance comparison

Usage:
  python3 perf_test.py [--host cosmic.lan]
"""

import argparse
import http.client
import os
import socket
import statistics
import struct
import time


FRAME_SIZE = 32 * 32 * 3  # 3072 bytes


def create_connection(host, request_timeout=0.1):
    """Create optimized HTTP connection with aggressive timeouts."""
    conn = http.client.HTTPConnection(host, timeout=2.0)  # 2s for initial connect
    conn.connect()
    sock = conn.sock
    sock.settimeout(request_timeout)  # 100ms for requests
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0x10)     # IPTOS_LOWDELAY
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_PRIORITY, 6)   # Higher priority
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
    # Aggressive keepalive
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 1)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 2)
    return conn


def generate_frame(pattern: int) -> bytes:
    """Generate a test frame with a specific pattern."""
    data = bytearray(FRAME_SIZE)
    for i in range(0, FRAME_SIZE, 3):
        pixel = (i // 3 + pattern) % 256
        data[i] = pixel  # R
        data[i + 1] = (pixel * 2) % 256  # G
        data[i + 2] = (pixel * 3) % 256  # B
    return bytes(data)


def generate_delta(num_pixels: int, pattern: int) -> bytes:
    """Generate a delta frame with specified number of changed pixels."""
    buf = bytearray(2 + num_pixels * 5)
    struct.pack_into('<H', buf, 0, num_pixels)
    for i in range(num_pixels):
        idx = (i * 7) % 1024  # Spread pixels across display
        r = (pattern + i) % 256
        g = (pattern + i * 2) % 256
        b = (pattern + i * 3) % 256
        struct.pack_into('<HBBB', buf, 2 + i * 5, idx, r, g, b)
    return bytes(buf)


def test_latency(conn, num_samples=100):
    """Measure round-trip latency for frame delivery."""
    print(f"\n=== Latency Test ({num_samples} samples) ===")

    latencies = []
    timeouts = 0
    headers = {"Content-Type": "application/octet-stream"}
    host = conn.host

    for i in range(num_samples):
        frame = generate_frame(i)

        start = time.perf_counter()
        try:
            conn.request("POST", "/api/frame", body=frame, headers=headers)
            resp = conn.getresponse()
            resp.read()
            elapsed = (time.perf_counter() - start) * 1000

            if resp.status == 200:
                latencies.append(elapsed)
        except Exception:
            timeouts += 1
            conn = create_connection(host)

        # Small delay to let device draw
        time.sleep(0.02)

    if latencies:
        print(f"  Samples:  {len(latencies)}")
        print(f"  Timeouts: {timeouts}")
        print(f"  Min:      {min(latencies):.2f} ms")
        print(f"  Max:      {max(latencies):.2f} ms")
        print(f"  Mean:     {statistics.mean(latencies):.2f} ms")
        print(f"  Median:   {statistics.median(latencies):.2f} ms")
        print(f"  Stdev:    {statistics.stdev(latencies):.2f} ms" if len(latencies) > 1 else "")
        print(f"  P95:      {sorted(latencies)[int(len(latencies)*0.95)]:.2f} ms")

    return latencies, timeouts


def test_throughput(conn, target_fps_list=[30, 50, 60, 100, 0]):
    """Test throughput at various frame rates. 0 = unlimited."""
    print("\n=== Throughput Test ===")

    headers = {"Content-Type": "application/octet-stream"}
    results = []

    for target_fps in target_fps_list:
        fps_label = f"{target_fps} FPS" if target_fps > 0 else "unlimited"
        frame_interval = 1.0 / target_fps if target_fps > 0 else 0

        sent = 0
        busy = 0
        errors = 0
        start_time = time.perf_counter()
        duration = 3.0  # Test duration in seconds

        last_send = 0
        pattern = 0

        while time.perf_counter() - start_time < duration:
            # Rate limiting
            if frame_interval > 0:
                now = time.perf_counter()
                elapsed = now - last_send
                if elapsed < frame_interval:
                    time.sleep(frame_interval - elapsed)
                last_send = time.perf_counter()

            frame = generate_frame(pattern)
            pattern = (pattern + 1) % 256

            try:
                conn.request("POST", "/api/frame", body=frame, headers=headers)
                resp = conn.getresponse()
                body = resp.read()

                if resp.status == 200:
                    if b'"busy"' in body:
                        busy += 1
                    else:
                        sent += 1
                else:
                    errors += 1
            except Exception as e:
                errors += 1
                conn = create_connection(conn.host)

        actual_time = time.perf_counter() - start_time
        actual_fps = sent / actual_time if actual_time > 0 else 0
        total_attempted = sent + busy + errors

        print(f"  Target {fps_label:>12}: {actual_fps:5.1f} FPS actual, "
              f"{sent:4d} sent, {busy:3d} busy, {errors:2d} errors "
              f"({total_attempted} attempted)")

        results.append({
            'target': target_fps,
            'actual': actual_fps,
            'sent': sent,
            'busy': busy,
            'errors': errors
        })

    return results


def test_busy_behavior(conn, burst_size=20):
    """Test busy rejection by sending frames as fast as possible."""
    print(f"\n=== Busy Rejection Test (burst of {burst_size}) ===")

    headers = {"Content-Type": "application/octet-stream"}

    results = {'ok': 0, 'busy': 0, 'error': 0}
    latencies = []

    for i in range(burst_size):
        frame = generate_frame(i)

        start = time.perf_counter()
        try:
            conn.request("POST", "/api/frame", body=frame, headers=headers)
            resp = conn.getresponse()
            body = resp.read()
            elapsed = (time.perf_counter() - start) * 1000

            if resp.status == 200:
                if b'"busy"' in body:
                    results['busy'] += 1
                else:
                    results['ok'] += 1
                    latencies.append(elapsed)
            else:
                results['error'] += 1
        except Exception:
            results['error'] += 1

    print(f"  OK:     {results['ok']:3d}")
    print(f"  Busy:   {results['busy']:3d}")
    print(f"  Errors: {results['error']:3d}")
    if latencies:
        print(f"  OK latency: {statistics.mean(latencies):.2f} ms avg")

    return results


def test_delta_performance(conn, pixel_counts=[10, 50, 100, 300, 600]):
    """Compare delta vs full frame performance."""
    print("\n=== Delta vs Full Frame Performance ===")

    headers = {"Content-Type": "application/octet-stream"}

    # First test full frame
    full_latencies = []
    for i in range(50):
        frame = generate_frame(i)
        start = time.perf_counter()
        conn.request("POST", "/api/frame", body=frame, headers=headers)
        resp = conn.getresponse()
        resp.read()
        if resp.status == 200:
            full_latencies.append((time.perf_counter() - start) * 1000)
        time.sleep(0.02)

    full_avg = statistics.mean(full_latencies) if full_latencies else 0
    print(f"  Full frame (3072 bytes): {full_avg:.2f} ms avg")

    # Test delta with various pixel counts
    for count in pixel_counts:
        delta_latencies = []
        for i in range(50):
            delta = generate_delta(count, i)
            start = time.perf_counter()
            conn.request("POST", "/api/delta", body=delta, headers=headers)
            resp = conn.getresponse()
            resp.read()
            if resp.status == 200:
                delta_latencies.append((time.perf_counter() - start) * 1000)
            time.sleep(0.02)

        if delta_latencies:
            avg = statistics.mean(delta_latencies)
            size = 2 + count * 5
            savings = (1 - size / FRAME_SIZE) * 100
            print(f"  Delta {count:3d} px ({size:4d} bytes, {savings:4.1f}% smaller): {avg:.2f} ms avg")


def test_sustained(conn, duration=10, target_fps=30):
    """Test sustained streaming for a longer period."""
    print(f"\n=== Sustained Streaming Test ({duration}s at {target_fps} FPS) ===")

    headers = {"Content-Type": "application/octet-stream"}
    frame_interval = 1.0 / target_fps

    sent = 0
    busy = 0
    errors = 0
    latencies = []

    start_time = time.perf_counter()
    last_send = 0
    pattern = 0

    while time.perf_counter() - start_time < duration:
        now = time.perf_counter()
        elapsed = now - last_send
        if elapsed < frame_interval:
            time.sleep(frame_interval - elapsed)
        last_send = time.perf_counter()

        frame = generate_frame(pattern)
        pattern = (pattern + 1) % 256

        req_start = time.perf_counter()
        try:
            conn.request("POST", "/api/frame", body=frame, headers=headers)
            resp = conn.getresponse()
            body = resp.read()
            req_time = (time.perf_counter() - req_start) * 1000

            if resp.status == 200:
                if b'"busy"' in body:
                    busy += 1
                else:
                    sent += 1
                    latencies.append(req_time)
            else:
                errors += 1
        except Exception:
            errors += 1
            conn = create_connection(conn.host)

    actual_time = time.perf_counter() - start_time
    actual_fps = sent / actual_time

    print(f"  Duration:   {actual_time:.1f}s")
    print(f"  Sent:       {sent} frames")
    print(f"  Busy:       {busy}")
    print(f"  Errors:     {errors}")
    print(f"  Actual FPS: {actual_fps:.1f}")
    if latencies:
        print(f"  Latency:    {statistics.mean(latencies):.2f} ms avg, "
              f"{max(latencies):.2f} ms max")


def test_wifi_vs_gc(conn, duration=30):
    """
    Diagnose whether latency spikes are WiFi hiccups or Python GC.

    WiFi hiccups:
    - Affect both small and large payloads equally
    - Correlate with retransmissions
    - Often come in bursts (interference)

    Python GC pauses:
    - Only affect the Python side (client)
    - Don't correlate with payload size
    - Happen periodically based on allocation patterns

    This test:
    1. Sends minimal status requests (tiny payload, minimal Python work)
    2. Sends full frames (large payload, more Python work)
    3. Compares spike patterns
    """
    print(f"\n=== WiFi vs GC Diagnosis ({duration}s) ===")

    # Test 1: Minimal requests (GET /api/status) - isolates WiFi
    print("\n  Phase 1: Minimal requests (GET /api/status)...")
    minimal_latencies = []
    minimal_spikes = []
    start = time.perf_counter()

    while time.perf_counter() - start < duration / 2:
        req_start = time.perf_counter()
        try:
            conn.request("GET", "/api/status")
            resp = conn.getresponse()
            resp.read()
            latency = (time.perf_counter() - req_start) * 1000
            minimal_latencies.append(latency)
            if latency > 50:  # Spike threshold
                minimal_spikes.append((time.perf_counter() - start, latency))
        except Exception:
            pass
        time.sleep(0.01)  # 100 req/s max

    # Test 2: Full frames with pre-allocated buffer - isolates WiFi + device processing
    print("  Phase 2: Full frames (pre-allocated buffer)...")
    headers = {"Content-Type": "application/octet-stream"}
    # Pre-allocate a single frame buffer to minimize Python allocations
    static_frame = bytes([i % 256 for i in range(FRAME_SIZE)])

    frame_latencies = []
    frame_spikes = []
    phase2_start = time.perf_counter()

    while time.perf_counter() - phase2_start < duration / 2:
        req_start = time.perf_counter()
        try:
            conn.request("POST", "/api/frame", body=static_frame, headers=headers)
            resp = conn.getresponse()
            resp.read()
            latency = (time.perf_counter() - req_start) * 1000
            frame_latencies.append(latency)
            if latency > 50:
                frame_spikes.append((time.perf_counter() - phase2_start, latency))
        except Exception:
            pass
        time.sleep(0.02)  # 50 req/s max

    # Analysis
    print("\n  Results:")
    print(f"    Minimal requests: {len(minimal_latencies)} samples")
    if minimal_latencies:
        print(f"      Mean: {statistics.mean(minimal_latencies):.2f} ms")
        print(f"      P95:  {sorted(minimal_latencies)[int(len(minimal_latencies)*0.95)]:.2f} ms")
        print(f"      Max:  {max(minimal_latencies):.2f} ms")
        print(f"      Spikes (>50ms): {len(minimal_spikes)}")

    print(f"    Frame requests: {len(frame_latencies)} samples")
    if frame_latencies:
        print(f"      Mean: {statistics.mean(frame_latencies):.2f} ms")
        print(f"      P95:  {sorted(frame_latencies)[int(len(frame_latencies)*0.95)]:.2f} ms")
        print(f"      Max:  {max(frame_latencies):.2f} ms")
        print(f"      Spikes (>50ms): {len(frame_spikes)}")

    # Diagnosis
    print("\n  Diagnosis:")
    minimal_spike_rate = len(minimal_spikes) / (duration / 2) if duration > 0 else 0
    frame_spike_rate = len(frame_spikes) / (duration / 2) if duration > 0 else 0

    if minimal_spike_rate > 0.1 and frame_spike_rate > 0.1:
        if abs(minimal_spike_rate - frame_spike_rate) / max(minimal_spike_rate, frame_spike_rate) < 0.3:
            print("    -> Similar spike rates suggest WIFI INTERFERENCE")
            print("       (Both large and small payloads affected equally)")
        else:
            print("    -> Different spike rates - may be mixed causes")
    elif frame_spike_rate > minimal_spike_rate * 2:
        print("    -> Frame spikes >> minimal spikes suggests DEVICE PROCESSING")
        print("       (Only large payloads cause spikes)")
    elif minimal_spike_rate > 0.1:
        print("    -> Minimal request spikes suggest WIFI INTERFERENCE")
    else:
        print("    -> Low spike rates - connection is stable")

    return {
        'minimal': {'latencies': minimal_latencies, 'spikes': minimal_spikes},
        'frames': {'latencies': frame_latencies, 'spikes': frame_spikes}
    }


def test_gc_isolation(conn, duration=20):
    """
    Test to isolate Python GC by comparing:
    1. Tight loop with allocations (triggers GC)
    2. Tight loop without allocations (no GC)
    """
    print(f"\n=== GC Isolation Test ({duration}s) ===")

    headers = {"Content-Type": "application/octet-stream"}

    # Phase 1: With allocations (new frame each time)
    print("\n  Phase 1: With allocations (new bytes() each request)...")
    alloc_latencies = []
    alloc_spikes = []
    start = time.perf_counter()

    while time.perf_counter() - start < duration / 2:
        # Allocate new frame each time - triggers GC eventually
        frame = bytes([((i + int(time.perf_counter() * 1000)) % 256) for i in range(FRAME_SIZE)])

        req_start = time.perf_counter()
        try:
            conn.request("POST", "/api/frame", body=frame, headers=headers)
            resp = conn.getresponse()
            resp.read()
            latency = (time.perf_counter() - req_start) * 1000
            alloc_latencies.append(latency)
            if latency > 50:
                alloc_spikes.append(latency)
        except Exception:
            pass
        time.sleep(0.02)

    # Phase 2: Without allocations (reuse buffer)
    print("  Phase 2: Without allocations (reuse buffer)...")
    static_frame = bytearray(FRAME_SIZE)
    noalloc_latencies = []
    noalloc_spikes = []
    start = time.perf_counter()

    while time.perf_counter() - start < duration / 2:
        # Modify in place - no allocations
        for i in range(0, min(100, FRAME_SIZE)):  # Just touch a few bytes
            static_frame[i] = (static_frame[i] + 1) % 256

        req_start = time.perf_counter()
        try:
            conn.request("POST", "/api/frame", body=static_frame, headers=headers)
            resp = conn.getresponse()
            resp.read()
            latency = (time.perf_counter() - req_start) * 1000
            noalloc_latencies.append(latency)
            if latency > 50:
                noalloc_spikes.append(latency)
        except Exception:
            pass
        time.sleep(0.02)

    # Results
    print("\n  Results:")
    print(f"    With allocations: {len(alloc_latencies)} samples, {len(alloc_spikes)} spikes")
    if alloc_latencies:
        print(f"      Mean: {statistics.mean(alloc_latencies):.2f} ms, Max: {max(alloc_latencies):.2f} ms")

    print(f"    Without allocations: {len(noalloc_latencies)} samples, {len(noalloc_spikes)} spikes")
    if noalloc_latencies:
        print(f"      Mean: {statistics.mean(noalloc_latencies):.2f} ms, Max: {max(noalloc_latencies):.2f} ms")

    print("\n  Diagnosis:")
    if len(alloc_spikes) > len(noalloc_spikes) * 2:
        print("    -> More spikes with allocations suggests PYTHON GC")
    elif len(alloc_spikes) > 0 or len(noalloc_spikes) > 0:
        print("    -> Similar spike counts suggests WIFI (not GC)")
    else:
        print("    -> No significant spikes detected")


def test_timeout_sweep(host, samples_per_timeout=200):
    """Test various timeout values to find optimal setting."""
    print(f"\n=== Timeout Sweep Test ({samples_per_timeout} samples each) ===")
    print(f"{'Timeout':>10} {'Success':>8} {'Timeouts':>9} {'Mean':>8} {'P95':>8} {'Max':>8}")
    print("-" * 60)

    headers = {"Content-Type": "application/octet-stream"}
    frame = bytes([i % 256 for i in range(FRAME_SIZE)])

    def make_conn(timeout_s):
        # Use longer timeout for connection, shorter for requests
        conn = http.client.HTTPConnection(host, timeout=2.0)
        conn.connect()
        conn.sock.settimeout(timeout_s)  # Apply request timeout after connect
        conn.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
        return conn

    for timeout_ms in [30, 40, 50, 75, 100, 150, 200, 300]:
        timeout_s = timeout_ms / 1000.0

        try:
            conn = make_conn(timeout_s)
        except Exception as e:
            print(f"{timeout_ms:>8}ms  (connection failed: {e})")
            continue

        latencies = []
        timeouts = 0

        for i in range(samples_per_timeout):
            start = time.perf_counter()
            try:
                conn.request("POST", "/api/frame", body=frame, headers=headers)
                resp = conn.getresponse()
                resp.read()
                elapsed = (time.perf_counter() - start) * 1000
                latencies.append(elapsed)
            except Exception:
                timeouts += 1
                try:
                    conn.close()
                except:
                    pass
                try:
                    conn = make_conn(timeout_s)
                except:
                    pass  # Will fail on next iteration

            time.sleep(0.02)

        try:
            conn.close()
        except:
            pass

        if latencies:
            p95_idx = int(len(latencies) * 0.95)
            print(f"{timeout_ms:>8}ms {len(latencies):>8} {timeouts:>9} "
                  f"{statistics.mean(latencies):>7.1f}ms "
                  f"{sorted(latencies)[p95_idx]:>7.1f}ms "
                  f"{max(latencies):>7.1f}ms")
        else:
            print(f"{timeout_ms:>8}ms {0:>8} {timeouts:>9}   (all failed)")

    print("\n  Recommendation: Use timeout ~3-5x your P95 latency")
    print("  Lower = faster stall recovery, but more false-positive reconnects")


def main():
    parser = argparse.ArgumentParser(description="Performance test for UnicornLEDStreamLite")
    parser.add_argument("--host", default="cosmic.lan", help="Device hostname")
    parser.add_argument("--test", choices=["all", "latency", "throughput", "busy", "delta", "sustained", "wifi", "gc", "timeout"],
                        default="all", help="Which test to run")
    args = parser.parse_args()

    print(f"Connecting to {args.host}...")
    conn = create_connection(args.host)

    # Verify connection
    conn.request("GET", "/api/status")
    resp = conn.getresponse()
    status = resp.read().decode()
    print(f"Device status: {status}")

    tests = {
        'latency': lambda: test_latency(create_connection(args.host)),
        'throughput': lambda: test_throughput(create_connection(args.host)),
        'busy': lambda: test_busy_behavior(create_connection(args.host)),
        'delta': lambda: test_delta_performance(create_connection(args.host)),
        'sustained': lambda: test_sustained(create_connection(args.host)),
        'wifi': lambda: test_wifi_vs_gc(create_connection(args.host)),
        'gc': lambda: test_gc_isolation(create_connection(args.host)),
        'timeout': lambda: test_timeout_sweep(args.host),
    }

    if args.test == "all":
        for name, test_fn in tests.items():
            try:
                test_fn()
            except Exception as e:
                print(f"\n  Test failed: {e}")
    else:
        tests[args.test]()

    print("\n=== Tests Complete ===")
    conn.close()


if __name__ == "__main__":
    main()
