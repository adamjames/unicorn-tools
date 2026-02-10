# Pico SDK development environment for Cosmic Unicorn firmware
{ pkgs }:

{
  # Build inputs for Pico development
  buildInputs = with pkgs; [
    gcc-arm-embedded
    pico-sdk
    picotool
    cmake
    ninja
    openocd-rp2040
    (python3.withPackages (ps: [ ps.setuptools ]))
    micropython
    mpy-utils
    mpremote
    thonny
    minicom
    gdb
    pkg-config
  ];

  # Shell hook for Pico SDK setup (requires WiFi-capable pico-sdk submodule)
  shellHook = ''
    # Find pico-sdk submodule by searching upward from current directory
    SEARCH_DIR="$PWD"
    PICO_SDK_FOUND=""
    while [ "$SEARCH_DIR" != "/" ]; do
      if [ -d "$SEARCH_DIR/pico-sdk/lib/cyw43-driver" ]; then
        PICO_SDK_FOUND="$SEARCH_DIR/pico-sdk"
        break
      fi
      SEARCH_DIR="$(dirname "$SEARCH_DIR")"
    done

    if [ -n "$PICO_SDK_FOUND" ]; then
      export PICO_SDK_PATH="$PICO_SDK_FOUND"
      echo "Using pico-sdk with WiFi support: $PICO_SDK_PATH"
    else
      echo "ERROR: pico-sdk submodule not found or WiFi libs missing"
      echo "Run: git submodule update --init --recursive"
      return 1
    fi
  '';

  # udev rules for Pico USB access
  udevRules = ''
    SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000c", MODE="0666", GROUP="users"
    SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="0003", MODE="0666", GROUP="users"
    SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000a", MODE="0666", GROUP="users"
  '';
}
