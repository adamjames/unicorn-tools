# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Cosmic Unicorn LED streaming system - streams screen content from various sources (desktop, 3DS) to a Pimoroni Cosmic Unicorn 32x32 RGB LED panel powered by a Pico 2 W (RP2350).

## Development Environments

Uses Nix flakes for reproducible dev environments. Enter with `nix develop`:

- `nix develop` - Full environment (Pico firmware + Python streaming tools)
- `nix develop .#pico` - Pico firmware only
- `nix develop .#3ds` - 3DS homebrew plugin (Docker-based devkitARM)
- `nix develop .#tools` - Python streaming tools only

## Build Commands

### Pico Firmware
```bash
firmware/cosmic/deploy.sh lite    # Build and deploy streaming-only firmware
firmware/cosmic/deploy.sh full    # Build and deploy full firmware with Lua shaders
```
Requires pico-sdk submodule with WiFi support (`git submodule update --init --recursive`).

### 3DS Plugin
```bash
cosmic-3gx-build      # Build 3GX plugin (uses Docker)
cosmic-3ds-deploy     # Deploy via FTP to 10.0.0.222:5000
cosmic-3ds-build      # Build sysmodule (deprecated, kept for future use)
```

### Host Tools
```bash
python tools/cosmic_cast.py       # Stream desktop screen to LED panel
python tools/cosmic_mock.py       # Mock LED panel for testing without hardware
```

## Architecture

### Firmware (`firmware/cosmic/`)
Two firmware variants built from same codebase via CMake:
- **cosmic-lite**: HTTP streaming receiver only (~790KB). Receives RGB frames over WiFi.
- **cosmic-full**: Adds Lua shader engine for standalone effects.

Source in `src/`, Pimoroni display drivers in `thirdparty/pimoroni/`, embedded Lua in `thirdparty/lua/`.

### 3DS Plugin (`firmware/3ds/plugin/`)
Luma3DS 3GX plugin using CTRPluginFramework. Captures 3DS screen, downscales to 32x32, streams to LED panel over WiFi. Built inside Docker (devkitpro/devkitarm) with cached 3gxtool and libctrpf.

### Streaming Protocol
HTTP POST of raw RGB888 frames (32x32x3 = 3072 bytes) to panel's WiFi endpoint. Supports delta compression for bandwidth optimization.

## Key Files

- `flake.nix` - Main Nix flake, imports modules from `nix/`
- `nix/pico.nix` - Pico SDK environment, requires WiFi-capable pico-sdk submodule
- `nix/3ds.nix` - 3DS Docker builds, includes 3gxtool Nix derivation
- `nix/tools.nix` - Python environment with pygame, pyserial, websockets, GStreamer
- `firmware/cosmic/CMakeLists.txt` - Pico firmware build (full and lite targets)
