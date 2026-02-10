#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# Find flake.nix by walking up from script directory
find_flake() {
    local dir="$1"
    while [[ "$dir" != "/" ]]; do
        if [[ -f "$dir/flake.nix" ]]; then
            echo "$dir"
            return 0
        fi
        dir="$(dirname "$dir")"
    done
    return 1
}

usage() {
    echo "Usage: $0 [full|lite] [hostname]"
    echo ""
    echo "Variants:"
    echo "  full  - Full build with Lua shader support (default)"
    echo "  lite  - Lightweight streaming-only build"
    echo ""
    echo "Examples:"
    echo "  $0              # Build and deploy 'full' to cosmic.lan"
    echo "  $0 lite         # Build and deploy 'lite' to cosmic.lan"
    echo "  $0 full 192.168.1.50  # Deploy to specific IP"
    exit 1
}

# Parse arguments
VARIANT="${1:-full}"
DEVICE_HOST="${2:-cosmic.lan}"

case "$VARIANT" in
    full)
        FIRMWARE="cosmic-full"
        ;;
    lite)
        FIRMWARE="cosmic-lite"
        ;;
    -h|--help)
        usage
        ;;
    *)
        echo "Unknown variant: $VARIANT"
        usage
        ;;
esac

FLAKE_DIR="$(find_flake "$SCRIPT_DIR")" || { echo "ERROR: No flake.nix found"; exit 1; }
UF2_FILE="$BUILD_DIR/$FIRMWARE.uf2"

echo "=== Cosmic Unicorn Build & Deploy ==="
echo "Variant: $VARIANT ($FIRMWARE)"
echo "Target:  $DEVICE_HOST"
echo ""

# Ensure build directory exists and is configured
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "[0/4] Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

if [[ ! -f "$BUILD_DIR/Makefile" ]]; then
    echo "[0/4] Configuring cmake..."
    cd "$BUILD_DIR"
    nix develop "$FLAKE_DIR" --command cmake ..
fi

# Build
echo "[1/4] Building firmware..."
cd "$BUILD_DIR"
nix develop "$FLAKE_DIR" --command make "$FIRMWARE" -j"$(nproc)"

if [[ ! -f "$UF2_FILE" ]]; then
    echo "ERROR: Build failed - $UF2_FILE not found"
    exit 1
fi

echo "Built: $UF2_FILE ($(stat -c%s "$UF2_FILE") bytes)"

# Reboot to bootloader
echo ""
echo "[2/4] Rebooting device to bootloader..."
if ! curl -sf -X POST "http://$DEVICE_HOST/api/reboot-bootloader" -o /dev/null; then
    echo "WARNING: Could not contact device - is it powered on?"
    echo "         Waiting for manual bootloader entry (hold BOOTSEL and power on)..."
fi

# Wait for USB mount
echo ""
echo "[3/4] Waiting for bootloader USB mount..."
MOUNT_POINT=""
for i in {1..30}; do
    MOUNT_POINT=$(find /run/media /media -maxdepth 2 -name "RP235*" -type d 2>/dev/null | head -1)
    if [[ -n "$MOUNT_POINT" && -f "$MOUNT_POINT/INFO_UF2.TXT" ]]; then
        break
    fi
    sleep 0.5
done

if [[ -z "$MOUNT_POINT" ]]; then
    echo "ERROR: Bootloader USB not found after 15 seconds"
    echo "       Try holding BOOTSEL while powering on the device"
    exit 1
fi

echo "Found bootloader at: $MOUNT_POINT"

# Flash
echo ""
echo "[4/4] Flashing firmware..."
cp "$UF2_FILE" "$MOUNT_POINT/"
sync

echo ""
echo "=== Deploy complete! ==="
echo "Device will reboot automatically. Waiting for it to come online..."

# Wait for device to come back
sleep 5
for i in {1..20}; do
    if curl -sf --connect-timeout 2 "http://$DEVICE_HOST/api/status" 2>/dev/null; then
        echo ""
        echo "Device online!"
        exit 0
    fi
    sleep 1
done

echo ""
echo "WARNING: Device not responding yet - may still be connecting to WiFi"
